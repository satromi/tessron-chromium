/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	rtc_audio_tessronos.cc
 *	ブラウザ: the sound of WebRTC's peers (design 14.5.2): what a media
 *	element plays of a remote audio track.
 *
 *	WebRTC decodes the sound it receives only when it is asked for it:
 *	the audio device module of the window (blink::WebRtcAudioDeviceImpl)
 *	is pulled 10 ms at a time (RenderData), WebRTC's mixer takes those
 *	frames from every receiving stream of the window's connections, and
 *	the remote tracks' sinks are fed on the way. In a browser the puller
 *	is the renderer of WebRTC's sound (blink::WebRtcAudioRenderer), made
 *	when an element plays a remote stream, on an output stream of the
 *	audio service. With nothing pulling, the receiving streams' jitter
 *	buffers fill, nothing is heard, the tracks' sinks get nothing, and the
 *	echo canceller of the microphone has no reference of what is played
 *	(the module hands each pulled buffer to its playout sinks, the
 *	microphone's audio processors among them).
 *
 *	Here the renderer is a thread of this file for each window's module,
 *	on a stream of the browser's mix (br_mix.cc): it pulls whenever the
 *	stream and the ring hold less than a tenth of a second, telling the
 *	module how long what is queued will take to be heard (the playout
 *	delay the echo canceller aligns with). It keeps pulling while an
 *	element plays or pauses a remote stream, so that the receiving
 *	streams do not fill up, and hands silence to the mix when none plays.
 *
 *	The elements are its proxies, each with its playing state and volume:
 *	as in Chromium, the volume of each remote source is the sum of the
 *	volumes of the playing elements that have it (at most 10, none: 0),
 *	set on the source on WebRTC's signaling thread, and WebRTC scales that
 *	stream by it before mixing.
 */

#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "base/functional/bind.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_glitch_info.h"
#include "third_party/blink/public/platform/modules/mediastream/web_media_stream.h"
#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#include "third_party/blink/renderer/modules/peerconnection/peer_connection_dependency_factory.h"
#include "third_party/blink/renderer/modules/webrtc/webrtc_audio_device_impl.h"
#include "third_party/blink/renderer/platform/heap/persistent.h"
#include "third_party/blink/renderer/platform/mediastream/media_stream_audio_track.h"
#include "third_party/blink/renderer/platform/mediastream/media_stream_component.h"
#include "third_party/blink/renderer/platform/mediastream/media_stream_descriptor.h"
#include "third_party/blink/renderer/platform/webrtc/peer_connection_remote_audio_source.h"
#include "third_party/webrtc/api/media_stream_interface.h"

/* Whether the sound device is there (br_snd.cc) */
extern "C" int br_snd_start( void );
/* The browser's mix of its sounds (br_mix.cc) */
extern "C" int br_mix_open( void );
extern "C" void br_mix_close( int h );
extern "C" int br_mix_write( int h, const short *frames, int nframes );
extern "C" int br_mix_buffered( int h );

namespace {

constexpr int kRate = 48000;
constexpr int kFrames = 480;		/* a pull: 10 ms, as the module takes */
constexpr int kAhead = 4800;		/* frames kept queued: 100 ms */

class Renderer;

/* An element playing a remote stream (on the engine's thread) */
struct Proxy {
	Renderer				*renderer;
	blink::Persistent<blink::MediaStreamDescriptor>	stream;
	bool					playing = false;
	float					volume = 1.0f;
};

class Renderer {
public:
	Renderer( blink::WebRtcAudioDeviceImpl *device, scoped_refptr<base::SingleThreadTaskRunner> signaling )
		: device_(device), signaling_(std::move(signaling))
	{
		if ( br_snd_start() != 0 ) printf("browser: rtc: no sound device; the peers' sound is not heard\n");
		mix_ = br_mix_open();
		running_ = true;
		pthread_attr_t at;
		pthread_attr_init(&at);
		pthread_attr_setstacksize(&at, 256 * 1024);
		if ( pthread_create(&thread_, &at, &Renderer::run, this) != 0 ) {
			printf("browser: rtc: no thread for the peers' sound\n");
			running_ = false;
		}
		pthread_attr_destroy(&at);
		printf("browser: rtc: the peers' sound is played\n");
	}

	~Renderer()
	{
		if ( running_ ) {
			running_ = false;
			pthread_join(thread_, nullptr);
		}
		br_mix_close(mix_);
		printf("browser: rtc: the peers' sound stopped (%ld frames)\n", frames_.load());
	}

	blink::WebRtcAudioDeviceImpl *device() const { return device_.get(); }

	void Add( Proxy *p ) { proxies_.insert(p); }
	void Remove( Proxy *p )
	{
		proxies_.erase(p);
		Changed(p);
	}
	bool Empty() const { return proxies_.empty(); }

	/* A proxy's state or stream changed: the volumes of its remote sources set again */
	void Changed( Proxy *p )
	{
		bool any = false;
		for ( Proxy *q : proxies_ ) any |= q->playing;
		playing_ = any;
		if ( !p->stream ) return;
		for ( const auto &c : p->stream->AudioComponents() ) {
			webrtc::AudioSourceInterface *src = SourceOf(c.Get());
			if ( src != nullptr ) UpdateVolume(src);
		}
	}

	static webrtc::AudioSourceInterface *SourceOf( blink::MediaStreamComponent *c )
	{
		blink::PeerConnectionRemoteAudioTrack *t =
			blink::PeerConnectionRemoteAudioTrack::From(blink::MediaStreamAudioTrack::From(c));
		return t != nullptr && t->track_interface() != nullptr ? t->track_interface()->GetSource() : nullptr;
	}

private:
	void UpdateVolume( webrtc::AudioSourceInterface *src )
	{
		float volume = 0.0f;
		for ( Proxy *q : proxies_ ) {
			if ( !q->playing || !q->stream ) continue;
			for ( const auto &c : q->stream->AudioComponents() ) {
				if ( SourceOf(c.Get()) == src ) {
					volume += q->volume;
					break;
				}
			}
		}
		volume = std::min(volume, 10.0f);
		scoped_refptr<webrtc::AudioSourceInterface> keep(src);
		auto set = []( scoped_refptr<webrtc::AudioSourceInterface> s, float v ) { s->SetVolume(v); };
		if ( signaling_ && !signaling_->BelongsToCurrentThread() ) {
			signaling_->PostTask(FROM_HERE, base::BindOnce(set, std::move(keep), volume));
		} else {
			set(std::move(keep), volume);
		}
	}

	static void *run( void *self )
	{
		static_cast<Renderer *>(self)->loop();
		return nullptr;
	}

	void loop()
	{
		std::unique_ptr<media::AudioBus> bus = media::AudioBus::Create(2, kFrames);
		short out[kFrames * 2];
		while ( running_ ) {
			int queued = br_mix_buffered(mix_);
			if ( queued >= kAhead ) {
				usleep(3000);
				continue;
			}
			base::TimeDelta delay = base::Microseconds((int64_t)queued * 1000000 / kRate);
			base::TimeDelta current;
			/* pulled whether or not an element plays, or the receiving streams fill */
			device_->RenderData(bus.get(), kRate, delay, &current, media::AudioGlitchInfo());
			if ( !playing_ ) bus->Zero();
			const float *l = bus->channel(0).data(), *r = bus->channel(1).data();
			for ( int i = 0; i < kFrames; i++ ) {
				out[i * 2] = to16(l[i]);
				out[i * 2 + 1] = to16(r[i]);
			}
			int done = 0;
			while ( done < kFrames && running_ ) {
				int n = br_mix_write(mix_, out + done * 2, kFrames - done);
				if ( n <= 0 ) {
					usleep(3000);
					continue;
				}
				done += n;
			}
			frames_ += done;
		}
	}

	static short to16( float v )
	{
		v = v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v;
		return (short)( v * 32767.0f );
	}

	scoped_refptr<blink::WebRtcAudioDeviceImpl>	device_;
	scoped_refptr<base::SingleThreadTaskRunner>	signaling_;
	std::set<Proxy *>				proxies_;
	int						mix_ = -1;
	pthread_t					thread_;
	std::atomic<bool>				running_{false};
	std::atomic<bool>				playing_{false};
	std::atomic<long>				frames_{0};
};

/* The renderers, one for each window's module (on the engine's thread) */
std::map<blink::WebRtcAudioDeviceImpl *, std::unique_ptr<Renderer>> &Renderers()
{
	static auto *m = new std::map<blink::WebRtcAudioDeviceImpl *, std::unique_ptr<Renderer>>();
	return *m;
}

}  // namespace

/*
 * An element of the window of 'context' plays 'web': a proxy of the
 * renderer of WebRTC's sound when the stream's first audio track is a
 * remote peer's (as Chromium chooses a renderer by it), else null and the
 * element plays the track's sound itself
 */
void *br_rtc_audio_attach( blink::ExecutionContext *context, const blink::WebMediaStream &web )
{
	blink::MediaStreamDescriptor *d = web;
	if ( context == nullptr || d == nullptr || d->AudioComponents().empty() ) return nullptr;
	if ( Renderer::SourceOf(d->AudioComponents()[0].Get()) == nullptr ) return nullptr;
	blink::PeerConnectionDependencyFactory &factory = blink::PeerConnectionDependencyFactory::From(*context);
	blink::WebRtcAudioDeviceImpl *device = factory.GetWebRtcAudioDevice();
	if ( device == nullptr ) return nullptr;
	std::unique_ptr<Renderer> &r = Renderers()[device];
	if ( !r ) r = std::make_unique<Renderer>(device, factory.GetWebRtcSignalingTaskRunner());
	Proxy *p = new Proxy;
	p->renderer = r.get();
	p->stream = d;
	r->Add(p);
	r->Changed(p);
	return p;
}

void br_rtc_audio_play( void *proxy, bool playing, double volume )
{
	Proxy *p = static_cast<Proxy *>(proxy);
	if ( p == nullptr ) return;
	p->playing = playing;
	p->volume = (float)std::max(0.0, volume);
	p->renderer->Changed(p);
}

void br_rtc_audio_detach( void *proxy )
{
	Proxy *p = static_cast<Proxy *>(proxy);
	if ( p == nullptr ) return;
	Renderer *r = p->renderer;
	p->playing = false;
	r->Remove(p);
	delete p;
	if ( r->Empty() ) Renderers().erase(r->device());
}
