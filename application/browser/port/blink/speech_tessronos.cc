/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	speech_tessronos.cc
 *	ブラウザ: the Web Speech API (speechSynthesis, SpeechRecognition) on
 *	the synthesizers and the recognizer in the browser (design 14.4.15;
 *	port/speech/speech_glue.c, built by tools/browser/speech.sh).
 *
 *	  - speechSynthesis: two voices, Japanese (Open JTalk, the voice of
 *	    NIT) and English (Flite, cmu_us_slt). An utterance goes to the
 *	    voice it names, else to the one of its language, else to the
 *	    Japanese voice when the text holds kana or kanji. The utterances
 *	    are said one after another on a thread of their own: the samples
 *	    made at 48 kHz in stereo and written to the sound ring the media
 *	    elements use (br_snd.cc) as it has room, the page told when one
 *	    starts and ends. Pause holds the writing, cancel ends the one
 *	    said (interrupted) and drops those waiting (cancelled).
 *	  - SpeechRecognition: the microphone, asked for on a panel, is
 *	    recorded until the speech has been followed by a second of
 *	    silence (or for ten seconds, or until the page stops it); the
 *	    recording is then told whisper.cpp (the tiny model, in the
 *	    language asked for) and the page given what it heard, as one
 *	    final result.
 *
 *	The dictionary, the voice and the model are record 2 of the
 *	browser's program object, read the first time they are asked for
 *	(br_speech_rec.cc); the dictionary's files, kept compressed, are
 *	inflated then (br_speech_file).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "media/mojo/mojom/speech_recognition_error.mojom-blink.h"
#include "media/mojo/mojom/speech_recognition_error_code.mojom-blink.h"
#include "media/mojo/mojom/speech_recognition_result.mojom-blink.h"
#include "media/mojo/mojom/speech_recognizer.mojom-blink.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "third_party/blink/public/mojom/speech/speech_synthesis.mojom-blink.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "third_party/zlib/zlib.h"

#include "../../br.h"

/* The services' thread (blob_tessronos.cc) */
void br_on_service_thread( base::OnceClosure task );

/* The sound ring and the microphone (port/media/br_snd.cc) */
extern "C" int br_snd_start( void );
/* The browser's mix of its sounds, a stream for each utterance (port/media/br_mix.cc) */
extern "C" int br_mix_open( void );
extern "C" void br_mix_close( int h );
extern "C" int br_mix_write( int h, const short *frames, int nframes );
extern "C" int br_mix_buffered( int h );
extern "C" int br_mic_available( void );
extern "C" int br_mic_open( void );
extern "C" int br_mic_read( short *frames, int max );
extern "C" void br_mic_close( void );

/* The synthesizers and the recognizer (port/speech/speech_glue.c) */
extern "C" int br_tts_ja( const char *utf8, double speed, double pitch, double volume, short **pcm, int *n, int *rate );
extern "C" int br_tts_en( const char *utf8, double speed, double pitch, double volume, short **pcm, int *n, int *rate );
extern "C" int br_stt( const float *samples, int n, const char *lang, int threads, char *out, int cap );

/* The files of speech, from the browser's program object (port/speech/br_speech_rec.cc) */
extern "C" int br_speech_rec( const char *name, const unsigned char **data, unsigned long *len, int *zipped );

namespace {

namespace bm = blink::mojom::blink;
namespace med = media::mojom::blink;

std::mutex				g_files_lock;
std::map<std::string, std::string>	g_inflated;

/* The bytes zlib compressed, inflated */
bool inflate_all( const unsigned char *p, size_t n, std::string *out )
{
	z_stream z;
	memset(&z, 0, sizeof(z));
	if ( inflateInit(&z) != Z_OK ) return false;
	z.next_in = const_cast<Bytef *>(p);
	z.avail_in = (uInt)n;
	std::vector<unsigned char> buf(1 << 20);
	int r;
	do {
		z.next_out = buf.data();
		z.avail_out = (uInt)buf.size();
		r = inflate(&z, Z_NO_FLUSH);
		if ( r != Z_OK && r != Z_STREAM_END ) break;
		out->append((const char *)buf.data(), buf.size() - z.avail_out);
	} while ( r != Z_STREAM_END );
	inflateEnd(&z);
	return r == Z_STREAM_END;
}

}  // namespace

/*
 * A file the synthesizers and the recognizer read, by its name ("mem/"
 * before it is the dictionary's directory): its bytes, kept as long as
 * the browser runs; 0 for a name there is no file of
 */
extern "C" int br_speech_file( const char *name, const void **data, unsigned long *len )
{
	std::string n = name;
	if ( n.compare(0, 4, "mem/") == 0 ) n = n.substr(4);
	const unsigned char *p = nullptr;
	unsigned long size = 0;
	int zipped = 0;
	if ( !br_speech_rec(n.c_str(), &p, &size, &zipped) ) return 0;
	if ( !zipped ) {
		*data = p;
		*len = size;
		return 1;
	}
	std::lock_guard<std::mutex> hold(g_files_lock);
	auto it = g_inflated.find(n);
	if ( it == g_inflated.end() ) {
		std::string s;
		if ( !inflate_all(p, (size_t)size, &s) ) {
			printf("browser: speech: %s is not inflated\n", n.c_str());
			return 0;
		}
		printf("browser: speech: %s, %zu bytes\n", n.c_str(), s.size());
		it = g_inflated.emplace(n, std::move(s)).first;
	}
	*data = it->second.data();
	*len = (unsigned long)it->second.size();
	return 1;
}

namespace {

constexpr char kJaUri[] = "tessronos-open-jtalk-ja";
constexpr char kEnUri[] = "tessronos-flite-en";

/* Whether a text holds kana or kanji */
bool japanese( const std::string &t )
{
	std::u16string u = base::UTF8ToUTF16(t);
	for ( char16_t c : u ) {
		if ( ( c >= 0x3040 && c <= 0x30ff ) || ( c >= 0x4e00 && c <= 0x9fff ) ) return true;
	}
	return false;
}

/* Mono samples at 'rate' as 48 kHz stereo frames */
std::vector<short> to_48k_stereo( const short *s, int n, int rate )
{
	std::vector<short> out;
	if ( n <= 0 || rate <= 0 ) return out;
	long m = (long)n * 48000 / rate;
	out.resize((size_t)m * 2);
	for ( long i = 0; i < m; i++ ) {
		double x = (double)i * rate / 48000.0;
		long k = (long)x;
		double f = x - (double)k;
		int a = s[std::min<long>(k, n - 1)], b = s[std::min<long>(k + 1, n - 1)];
		short v = (short)( a + ( b - a ) * f );
		out[(size_t)i * 2] = v;
		out[(size_t)i * 2 + 1] = v;
	}
	return out;
}

struct Job {
	std::string				text, lang, voice;
	double					rate, pitch, volume;
	std::shared_ptr<mojo::Remote<bm::SpeechSynthesisClient>>	client;	/* the services' thread's */
};

void tell( std::shared_ptr<mojo::Remote<bm::SpeechSynthesisClient>> c, int what, bm::SpeechSynthesisErrorCode code )
{
	br_on_service_thread(base::BindOnce([]( std::shared_ptr<mojo::Remote<bm::SpeechSynthesisClient>> c, int what,
						bm::SpeechSynthesisErrorCode code ) {
		if ( !c->is_bound() ) return;
		switch ( what ) {
		case 0: ( *c )->OnStartedSpeaking(); break;
		case 1: ( *c )->OnFinishedSpeaking(code); break;
		case 2: ( *c )->OnPausedSpeaking(); break;
		case 3: ( *c )->OnResumedSpeaking(); break;
		default: ( *c )->OnEncounteredSpeakingError(); break;
		}
	}, c, what, code));
}

class Speech final : public bm::SpeechSynthesis {
public:
	Speech() { std::thread(&Speech::Run, this).detach(); }

	void Bind( mojo::PendingReceiver<bm::SpeechSynthesis> r ) { receivers_.Add(this, std::move(r)); }

	void AddVoiceListObserver( mojo::PendingRemote<bm::SpeechSynthesisVoiceListObserver> o ) override
	{
		mojo::Remote<bm::SpeechSynthesisVoiceListObserver> observer(std::move(o));
		blink::Vector<bm::SpeechSynthesisVoicePtr> list;
		list.push_back(bm::SpeechSynthesisVoice::New(blink::String(kJaUri), blink::String::FromUtf8("Open JTalk 日本語"),
							     blink::String("ja-JP"), true, true));
		list.push_back(bm::SpeechSynthesisVoice::New(blink::String(kEnUri), blink::String("Flite English"),
							     blink::String("en-US"), true, false));
		observer->OnSetVoiceList(std::move(list));
		observers_.push_back(std::move(observer));
	}

	void Speak( bm::SpeechSynthesisUtterancePtr u, mojo::PendingRemote<bm::SpeechSynthesisClient> c ) override
	{
		Job j;
		j.text = u->text.Utf8();
		j.lang = u->lang.Utf8();
		j.voice = u->voice.Utf8();
		j.rate = u->rate > 0 ? u->rate : 1.0;
		j.pitch = u->pitch > 0 ? u->pitch : 1.0;
		j.volume = u->volume >= 0 ? u->volume : 1.0;
		j.client = std::make_shared<mojo::Remote<bm::SpeechSynthesisClient>>(std::move(c));
		{
			std::lock_guard<std::mutex> hold(lock_);
			queue_.push_back(std::move(j));
		}
		wake_.notify_all();
	}

	void Pause() override
	{
		paused_ = true;
		std::shared_ptr<mojo::Remote<bm::SpeechSynthesisClient>> c = current();
		if ( c ) tell(c, 2, bm::SpeechSynthesisErrorCode::kNoError);
	}

	void Resume() override
	{
		paused_ = false;
		wake_.notify_all();
		std::shared_ptr<mojo::Remote<bm::SpeechSynthesisClient>> c = current();
		if ( c ) tell(c, 3, bm::SpeechSynthesisErrorCode::kNoError);
	}

	void Cancel() override
	{
		std::deque<Job> dropped;
		{
			std::lock_guard<std::mutex> hold(lock_);
			dropped.swap(queue_);
			stop_ = true;
		}
		paused_ = false;
		wake_.notify_all();
		for ( Job &j : dropped ) tell(j.client, 1, bm::SpeechSynthesisErrorCode::kCancelled);
	}

private:
	std::shared_ptr<mojo::Remote<bm::SpeechSynthesisClient>> current()
	{
		std::lock_guard<std::mutex> hold(lock_);
		return now_;
	}

	void Run()
	{
		for ( ;; ) {
			Job j;
			{
				std::unique_lock<std::mutex> hold(lock_);
				wake_.wait(hold, [&] { return !queue_.empty(); });
				j = std::move(queue_.front());
				queue_.pop_front();
				now_ = j.client;
				stop_ = false;
			}
			Say(j);
			std::lock_guard<std::mutex> hold(lock_);
			now_.reset();
		}
	}

	void Say( const Job &j )
	{
		bool ja = j.voice == kJaUri || ( j.voice != kEnUri && ( j.lang.compare(0, 2, "ja") == 0
				|| ( j.lang.empty() && japanese(j.text) ) ) );
		short *pcm = nullptr;
		int n = 0, rate = 0;
		auto t0 = std::chrono::steady_clock::now();
		int ok = ja ? br_tts_ja(j.text.c_str(), j.rate, j.pitch, j.volume, &pcm, &n, &rate)
			    : br_tts_en(j.text.c_str(), j.rate, j.pitch, j.volume, &pcm, &n, &rate);
		long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
		if ( !ok || n <= 0 ) {
			printf("browser: speech: nothing said (%s)\n", ja ? "ja" : "en");
			free(pcm);
			tell(j.client, 1, bm::SpeechSynthesisErrorCode::kErrorOccurred);
			return;
		}
		std::vector<short> f = to_48k_stereo(pcm, n, rate);
		free(pcm);
		printf("browser: speech: %s, %d samples at %d Hz (%.2f s) made in %ld ms\n", ja ? "ja" : "en", n, rate,
		       (double)n / rate, ms);
		tell(j.client, 0, bm::SpeechSynthesisErrorCode::kNoError);
		(void)br_snd_start();
		int mix = br_mix_open();
		size_t at = 0, frames = f.size() / 2;
		while ( at < frames ) {
			if ( stop_ ) break;
			if ( paused_ ) {
				std::unique_lock<std::mutex> hold(lock_);
				wake_.wait_for(hold, std::chrono::milliseconds(50), [&] { return !paused_ || stop_; });
				continue;
			}
			int w = br_mix_write(mix, &f[at * 2], (int)std::min<size_t>(frames - at, 4800));
			at += (size_t)w;
			if ( w == 0 ) std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		/* heard: its stream mixed, and what is in the ring after it played out */
		for ( int t = 0; !stop_ && t < 400 && br_mix_buffered(mix) > 0; t++ ) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		br_mix_close(mix);
		tell(j.client, 1, stop_ ? bm::SpeechSynthesisErrorCode::kInterrupted : bm::SpeechSynthesisErrorCode::kNoError);
	}

	mojo::ReceiverSet<bm::SpeechSynthesis>				receivers_;
	std::vector<mojo::Remote<bm::SpeechSynthesisVoiceListObserver>>	observers_;
	std::mutex							lock_;
	std::condition_variable						wake_;
	std::deque<Job>							queue_;
	std::shared_ptr<mojo::Remote<bm::SpeechSynthesisClient>>	now_;
	std::atomic<bool>						stop_{false}, paused_{false};
};

/* ---------------------------------------------------------------- recognition */

class Session final : public med::SpeechRecognitionSession {
public:
	explicit Session( std::shared_ptr<std::atomic<int>> stop ) : stop_(std::move(stop)) {}
	void Abort() override { *stop_ = 2; }
	void StopCapture() override { *stop_ = 1; }
	void UpdateRecognitionContext( med::SpeechRecognitionRecognitionContextPtr ) override {}

private:
	std::shared_ptr<std::atomic<int>>	stop_;
};

void client_do( std::shared_ptr<mojo::Remote<med::SpeechRecognitionSessionClient>> c,
		base::OnceCallback<void(med::SpeechRecognitionSessionClient *)> f )
{
	br_on_service_thread(base::BindOnce([]( std::shared_ptr<mojo::Remote<med::SpeechRecognitionSessionClient>> c,
						base::OnceCallback<void(med::SpeechRecognitionSessionClient *)> f ) {
		if ( c->is_bound() ) std::move(f).Run(c->get());
	}, c, std::move(f)));
}

void error_of( std::shared_ptr<mojo::Remote<med::SpeechRecognitionSessionClient>> c, med::SpeechRecognitionErrorCode code )
{
	client_do(c, base::BindOnce([]( med::SpeechRecognitionErrorCode code, med::SpeechRecognitionSessionClient *cl ) {
		cl->ErrorOccurred(med::SpeechRecognitionError::New(code, med::SpeechAudioErrorDetails::kNone));
		cl->Ended();
	}, code));
}

/* One recognition: the microphone recorded, then what was said heard (a thread of its own) */
void listen( std::string origin, std::string lang, std::shared_ptr<std::atomic<int>> stop,
	     std::shared_ptr<mojo::Remote<med::SpeechRecognitionSessionClient>> c )
{
	if ( !br_mic_available() ) {	/* no microphone: nothing to ask for */
		error_of(c, med::SpeechRecognitionErrorCode::kAudioCapture);
		return;
	}
	static std::mutex lock;
	static std::map<std::string, bool> answered;	/* the user's answers, by origin */
	bool yes;
	{
		std::lock_guard<std::mutex> hold(lock);
		auto it = answered.find(origin);
		yes = it != answered.end() && it->second;
		if ( it == answered.end() ) {
			yes = br_ask_permission(origin, "マイクの使用");
			answered[origin] = yes;
		}
	}
	if ( !yes ) {
		error_of(c, med::SpeechRecognitionErrorCode::kNotAllowed);
		return;
	}
	if ( br_mic_open() != 0 ) {
		error_of(c, med::SpeechRecognitionErrorCode::kAudioCapture);
		return;
	}
	client_do(c, base::BindOnce([]( med::SpeechRecognitionSessionClient *cl ) {
		cl->Started();
		cl->AudioStarted();
	}));
	std::vector<float> mono;		/* 16 kHz */
	std::vector<short> buf(4800 * 2);
	bool sound = false;
	int quiet_ms = 0, total_ms = 0;
	double acc[3] = { 0, 0, 0 };
	int phase = 0;
	while ( *stop == 0 && total_ms < 10000 ) {
		int n = br_mic_read(buf.data(), 4800);
		if ( n <= 0 ) {
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			total_ms += 20;
			continue;
		}
		double energy = 0;
		for ( int i = 0; i < n; i++ ) {
			double v = ( buf[(size_t)i * 2] + buf[(size_t)i * 2 + 1] ) / 65536.0;
			energy += v * v;
			acc[phase++] = v;
			if ( phase == 3 ) {	/* 48 kHz to 16 kHz: three averaged */
				mono.push_back((float)( ( acc[0] + acc[1] + acc[2] ) / 3.0 ));
				phase = 0;
			}
		}
		int ms = n / 48;
		total_ms += ms;
		bool loud = energy / n > 1e-4;
		if ( loud && !sound ) {
			sound = true;
			client_do(c, base::BindOnce([]( med::SpeechRecognitionSessionClient *cl ) { cl->SoundStarted(); }));
		}
		quiet_ms = loud ? 0 : quiet_ms + ms;
		if ( sound && quiet_ms >= 1000 ) break;
	}
	br_mic_close();
	int how = *stop;
	client_do(c, base::BindOnce([]( bool sound, med::SpeechRecognitionSessionClient *cl ) {
		if ( sound ) cl->SoundEnded();
		cl->AudioEnded();
	}, sound));
	if ( how == 2 ) {
		error_of(c, med::SpeechRecognitionErrorCode::kAborted);
		return;
	}
	if ( !sound ) {
		error_of(c, med::SpeechRecognitionErrorCode::kNoSpeech);
		return;
	}
	char text[2048];
	std::string l = lang.substr(0, lang.find('-'));
	auto t0 = std::chrono::steady_clock::now();
	int ok = br_stt(mono.data(), (int)mono.size(), l.c_str(), 4, text, sizeof(text));
	long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
	std::string heard = ok ? text : "";
	size_t s = heard.find_first_not_of(' ');
	heard = ( s == std::string::npos ) ? std::string() : heard.substr(s);
	printf("browser: speech: heard \"%s\" in %.2f s of sound, in %ld ms\n", heard.c_str(), mono.size() / 16000.0, ms);
	if ( heard.empty() ) {
		error_of(c, med::SpeechRecognitionErrorCode::kNoMatch);
		return;
	}
	client_do(c, base::BindOnce([]( std::string heard, double secs, med::SpeechRecognitionSessionClient *cl ) {
		blink::Vector<med::WebSpeechRecognitionResultPtr> results;
		auto r = med::WebSpeechRecognitionResult::New();
		r->hypotheses.push_back(med::SpeechRecognitionHypothesis::New(blink::String::FromUtf8(heard), 0.9));
		r->is_provisional = false;
		r->audio_start_time = base::TimeDelta();
		r->audio_end_time = base::Seconds(secs);
		results.push_back(std::move(r));
		cl->ResultRetrieved(std::move(results));
		cl->Ended();
	}, heard, mono.size() / 16000.0));
}

class Listening final : public med::SpeechRecognizer {
public:
	void Bind( mojo::PendingReceiver<med::SpeechRecognizer> r ) { receivers_.Add(this, std::move(r)); }

	void Start( med::StartSpeechRecognitionRequestParamsPtr params ) override
	{
		auto stop = std::make_shared<std::atomic<int>>(0);
		auto c = std::make_shared<mojo::Remote<med::SpeechRecognitionSessionClient>>(std::move(params->client));
		mojo::MakeSelfOwnedReceiver(std::make_unique<Session>(stop), std::move(params->session_receiver));
		std::thread(listen, br_page_origin(), params->language.Utf8(), stop, c).detach();
	}

private:
	mojo::ReceiverSet<med::SpeechRecognizer>	receivers_;
};

Speech		*g_speech;
Listening	*g_listening;

template <typename Interface, typename Service>
void Binder( Service **svc )
{
	blink::GetEmptyBrowserInterfaceBroker().SetBinderForTesting(
		Interface::Name_, base::BindRepeating([]( Service **svc, mojo::ScopedMessagePipeHandle h ) {
			br_on_service_thread(base::BindOnce([]( Service **svc, mojo::PendingReceiver<Interface> r ) {
				if ( *svc == nullptr ) *svc = new Service();
				( *svc )->Bind(std::move(r));
			}, svc, mojo::PendingReceiver<Interface>(std::move(h))));
		}, svc));
}

}  // namespace

/* The binders of the frames' broker, once when the engine is up */
void br_speech_init()
{
	Binder<bm::SpeechSynthesis, Speech>(&g_speech);
	Binder<med::SpeechRecognizer, Listening>(&g_listening);
}
