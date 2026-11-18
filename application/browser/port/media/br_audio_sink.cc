/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_audio_sink.cc
 *	ブラウザ: where the media pipeline's sound goes (design 14.6): the
 *	audio renderer's sink (media::AudioRendererSink), a stream of the
 *	browser's mix (br_mix.cc), which goes on the sound ring of br_snd.cc
 *	and through it on the sound device.
 *
 *	The device takes 48 kHz, 16-bit stereo; the renderer is told so
 *	(GetOutputDeviceInfo) and converts to it. A thread of the sink asks
 *	the renderer for a buffer of frames whenever its stream and the ring hold less than
 *	a tenth of a second, telling it how long what is already queued will
 *	take to be heard: the renderer keeps its clock, and with it the
 *	picture, in time with what is heard from that delay. Where nothing
 *	can be heard (no device, or one that takes nothing) the ring is
 *	drained by the clock (br_snd.cc), so the time goes on all the same.
 */

#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/unguessable_token.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_capturer_source.h"
#include "media/base/audio_glitch_info.h"
#include "media/base/audio_parameters.h"
#include "media/base/audio_renderer_sink.h"
#include "media/base/output_device_info.h"

/* The ring the sound device is fed from (br_snd.cc) */
extern "C" int br_snd_start( void );
/* The browser's mix of its sounds, a stream for each sink (br_mix.cc) */
extern "C" int br_mix_open( void );
extern "C" void br_mix_close( int h );
extern "C" int br_mix_write( int h, const short *frames, int nframes );
extern "C" int br_mix_buffered( int h );

namespace {

constexpr int kRate = 48000;
constexpr int kFrames = 480;		/* a buffer: 10 ms */
constexpr int kAhead = 4800;		/* frames kept queued: 100 ms */

class BrAudioSink final : public media::AudioRendererSink {
public:
	void Initialize( const media::AudioParameters &params, RenderCallback *callback ) override
	{
		params_ = params;
		callback_ = callback;
		bus_ = media::AudioBus::Create(params);
		out_.reset(new short[params.frames_per_buffer() * 2]);
	}

	/*
	 * Started and stopped from the media thread, and stopped once more
	 * when the last reference goes, from wherever that is: the thread is
	 * made and joined under a lock, and a stop on the thread itself lets
	 * it go instead of waiting for itself
	 */
	void Start() override
	{
		std::lock_guard<std::mutex> g(state_);
		if ( running_ || callback_ == nullptr ) return;
		if ( br_snd_start() != 0 ) printf("browser: media: no sound device; the sound is not heard, the time goes on by the clock\n");
		if ( mix_ < 0 ) mix_ = br_mix_open();
		running_ = true;
		pthread_attr_t at;
		pthread_attr_init(&at);
		pthread_attr_setstacksize(&at, 256 * 1024);
		if ( pthread_create(&thread_, &at, &BrAudioSink::run, this) != 0 ) {
			printf("browser: media: no thread for the sound\n");
			running_ = false;
		}
		pthread_attr_destroy(&at);
	}

	void Stop() override
	{
		std::lock_guard<std::mutex> g(state_);
		if ( !running_ ) return;
		running_ = false;
		if ( pthread_equal(pthread_self(), thread_) ) {
			pthread_detach(thread_);
		} else {
			pthread_join(thread_, nullptr);
		}
		br_mix_close(mix_);
		mix_ = -1;
	}

	void Pause() override { playing_ = false; }
	void Play() override { playing_ = true; }
	void Flush() override {}
	bool SetVolume( double volume ) override
	{
		volume_ = volume;
		return true;
	}

	media::OutputDeviceInfo GetOutputDeviceInfo() override
	{
		return media::OutputDeviceInfo(std::string(), media::OUTPUT_DEVICE_STATUS_OK,
					       media::AudioParameters(media::AudioParameters::AUDIO_PCM_LOW_LATENCY,
								      media::ChannelLayoutConfig::Stereo(), kRate,
								      kFrames));
	}
	void GetOutputDeviceInfoAsync( OutputDeviceInfoCB cb ) override
	{
		base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
			FROM_HERE, base::BindOnce(std::move(cb), GetOutputDeviceInfo()));
	}
	bool IsOptimizedForHardwareParameters() override { return true; }
	bool CurrentThreadIsRenderingThread() override
	{
		return running_ && pthread_equal(pthread_self(), thread_);
	}

	/* Frames of the media given to the ring since the program began */
	static std::atomic<long> frames_out;

private:
	~BrAudioSink() override { Stop(); }

	static void *run( void *self )
	{
		static_cast<BrAudioSink *>(self)->loop();
		return nullptr;
	}

	/* A buffer rendered whenever the ring runs below kAhead frames */
	void loop()
	{
		const int frames = params_.frames_per_buffer();
		while ( running_ ) {
			if ( !playing_ ) {
				usleep(10000);
				continue;
			}
			int queued = br_mix_buffered(mix_);
			if ( queued >= kAhead ) {
				usleep(5000);
				continue;
			}
			base::TimeDelta delay = base::Microseconds((int64_t)queued * 1000000 / kRate);
			int got = callback_->Render(delay, base::TimeTicks::Now(), media::AudioGlitchInfo(), bus_.get());
			if ( got <= 0 ) {
				usleep(5000);
				continue;
			}
			if ( got > frames ) got = frames;
			const float *l = bus_->channel(0).data();
			const float *r = bus_->channels() > 1 ? bus_->channel(1).data() : l;
			float v = (float)volume_;
			for ( int i = 0; i < got; i++ ) {
				out_[i * 2] = to16(l[i] * v);
				out_[i * 2 + 1] = to16(r[i] * v);
			}
			/* the ring takes what fits; the rest waits for its room */
			int done = 0;
			while ( done < got && running_ ) {
				int n = br_mix_write(mix_, out_.get() + done * 2, got - done);
				if ( n <= 0 ) {
					usleep(5000);
					continue;
				}
				done += n;
			}
			frames_out += done;
		}
	}

	static short to16( float v )
	{
		if ( v > 1.0f ) v = 1.0f;
		if ( v < -1.0f ) v = -1.0f;
		return (short)( v * 32767.0f );
	}

	media::AudioParameters		params_;
	RenderCallback			*callback_ = nullptr;
	std::unique_ptr<media::AudioBus> bus_;
	std::unique_ptr<short[]>	out_;
	pthread_t			thread_;
	std::mutex			state_;		/* over starting and stopping the thread */
	std::atomic<bool>		running_{false};
	std::atomic<bool>		playing_{false};
	std::atomic<double>		volume_{1.0};
	int				mix_ = -1;	/* its stream in the mix */
};

std::atomic<long> BrAudioSink::frames_out{0};

}  // namespace

/* A sink for a player's audio renderer */
scoped_refptr<media::AudioRendererSink> br_audio_sink()
{
	return base::MakeRefCounted<BrAudioSink>();
}

/* Frames the media elements have given the sound ring since the program began */
long br_media_sound_frames()
{
	return BrAudioSink::frames_out.load();
}

/* ---------------------------------------------------------------- the microphone */

/* Recording on the sound device (br_snd.cc) */
extern "C" int br_mic_open( void );
extern "C" int br_mic_read( short *frames, int max );
extern "C" void br_mic_close( void );
extern "C" int br_mon_open( int mode, int pid );
extern "C" int br_mon_read( int key, short *frames, int max );
extern "C" void br_mon_close( int key );

namespace {

/* The sessions of a share's sound, and what each takes */
struct MonSession {
	int	mode;
	int	pid;
};
std::mutex					g_mon_sessions_lock;
std::map<base::UnguessableToken, MonSession>	g_mon_sessions;

/*
 * A microphone's capture (getUserMedia's sound), for the local audio
 * source of a stream: a thread of its own reads what the device recorded
 * (48 kHz, 16-bit stereo) and hands it on in buffers of the size the
 * source asked for, as floats, with the time it was read
 */
class BrAudioCapturer final : public media::AudioCapturerSource {
public:
	BrAudioCapturer() = default;
	/* A share's sound: what the sound device's monitor takes in place of the microphone */
	explicit BrAudioCapturer( const MonSession &mon ) : monitor_(true), mon_(mon) {}

	void Initialize( const media::AudioParameters &params, CaptureCallback *callback ) override
	{
		params_ = params;
		callback_ = callback;
	}

	void Start() override
	{
		if ( running_ || callback_ == nullptr ) return;
		if ( monitor_ ) {
			mon_key_ = br_mon_open(mon_.mode, mon_.pid);
			if ( mon_key_ < 0 ) {
				callback_->OnCaptureError(ErrorCode::kDeviceRemoved, "The sound device gives no monitor.");
				return;
			}
		} else if ( br_mic_open() != 0 ) {
			callback_->OnCaptureError(ErrorCode::kDeviceRemoved, "The sound device does not record.");
			return;
		}
		running_ = true;
		AddRef();		/* the thread holds this until it ends */
		pthread_t th;
		pthread_attr_t at;
		pthread_attr_init(&at);
		pthread_attr_setstacksize(&at, 256 * 1024);
		pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
		if ( pthread_create(&th, &at, &BrAudioCapturer::Run, this) != 0 ) {
			running_ = false;
			Release();
			CloseSource();
		}
		pthread_attr_destroy(&at);
	}

	void Stop() override
	{
		running_ = false;
	}

	void SetVolume( double ) override {}
	void SetAutomaticGainControl( bool ) override {}
	void SetOutputDeviceForAec( const std::string & ) override {}

private:
	~BrAudioCapturer() override = default;

	void CloseSource()
	{
		if ( monitor_ ) {
			br_mon_close(mon_key_);
		} else {
			br_mic_close();
		}
	}

	static void *Run( void *p )
	{
		static_cast<BrAudioCapturer *>(p)->Loop();
		return nullptr;
	}

	void Loop()
	{
		int ch = params_.channels() > 0 ? params_.channels() : 2;
		int n = params_.frames_per_buffer() > 0 ? params_.frames_per_buffer() : kFrames;
		std::unique_ptr<media::AudioBus> bus = media::AudioBus::Create(ch, n);
		std::vector<short> got((size_t)n * 2);
		int have = 0;
		base::TimeDelta period = base::Seconds(1) * n / 48000;
		base::TimeTicks last = base::TimeTicks::Now();
		callback_->OnCaptureStarted();
		while ( running_ ) {
			int k = monitor_ ? br_mon_read(mon_key_, got.data() + have * 2, n - have)
					 : br_mic_read(got.data() + have * 2, n - have);
			if ( k < 0 ) {
				callback_->OnCaptureError(ErrorCode::kDeviceRemoved, "The sound device stopped recording.");
				break;
			}
			have += k;
			if ( monitor_ && have < n && base::TimeTicks::Now() - last >= period * 2 ) {
				/*
				 * The monitor has frames only while something is played:
				 * silence meanwhile, so that the track's time goes on as
				 * the page's other tracks' does
				 */
				std::fill(got.begin() + (ptrdiff_t)have * 2, got.end(), (short)0);
				have = n;
			}
			if ( have < n ) {
				usleep(5000);
				continue;
			}
			last = base::TimeTicks::Now();
			/* stereo in; one channel out is their mean */
			for ( int c = 0; c < ch; c++ ) {
				float *d = bus->channel(c).data();
				for ( int i = 0; i < n; i++ ) {
					float l = got[(size_t)i * 2] / 32768.0f, r = got[(size_t)i * 2 + 1] / 32768.0f;
					d[i] = ch == 1 ? ( l + r ) / 2 : ( c == 0 ? l : r );
				}
			}
			callback_->Capture(bus.get(), base::TimeTicks::Now(), media::AudioGlitchInfo(), 1.0);
			have = 0;
		}
		CloseSource();
		Release();
	}

	media::AudioParameters	params_;
	CaptureCallback		*callback_ = nullptr;
	std::atomic<bool>	running_{false};
	bool			monitor_ = false;
	MonSession		mon_{0, 0};
	int			mon_key_ = -1;
};

}  // namespace

/* A share's sound device opened (on the services' thread): its session's capture is the monitor's */
void br_share_audio_session( const base::UnguessableToken &session, int mode, int pid )
{
	std::lock_guard<std::mutex> g(g_mon_sessions_lock);
	g_mon_sessions[session] = MonSession{mode, pid};
}

/* The capture of a session, for a stream's local audio source (br_blink.cc's platform): the microphone's, or a share's sound */
scoped_refptr<media::AudioCapturerSource> br_audio_capturer( const base::UnguessableToken &session )
{
	std::lock_guard<std::mutex> g(g_mon_sessions_lock);
	auto it = g_mon_sessions.find(session);
	if ( it != g_mon_sessions.end() ) return base::MakeRefCounted<BrAudioCapturer>(it->second);
	return base::MakeRefCounted<BrAudioCapturer>();
}
