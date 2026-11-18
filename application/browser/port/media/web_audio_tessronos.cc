/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	web_audio_tessronos.cc
 *	The output of Web Audio (AudioContext) for the engine (design 17.19).
 *
 *	An AudioContext renders its graph when its destination is asked for
 *	frames; in a browser the audio service asks, from the device's clock.
 *	Here a thread of this file asks: it keeps its stream of the browser's
 *	mix (br_mix.cc) about a tenth of a second ahead of what the sound
 *	device plays, rendering a buffer of frames at a time and writing them
 *	as 16-bit stereo at 48 kHz. The context's own rate, when the page asks
 *	for another, is converted by the engine's destination.
 */

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
#include <memory>
#include <optional>

#include "base/time/time.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_glitch_info.h"
#include "media/base/audio_renderer_sink.h"
#include "media/base/output_device_info.h"
#include "third_party/blink/public/platform/web_audio_device.h"
#include "third_party/blink/public/platform/web_audio_latency_hint.h"
#include "third_party/blink/public/platform/web_audio_sink_descriptor.h"

/* The ring the sound device is fed from (br_snd.cc) */
extern "C" int br_snd_start( void );
/* The browser's mix of its sounds, a stream for each context (br_mix.cc) */
extern "C" int br_mix_open( void );
extern "C" void br_mix_close( int h );
extern "C" int br_mix_write( int h, const short *frames, int nframes );
extern "C" int br_mix_buffered( int h );

namespace {

constexpr int kRate = 48000;
constexpr int kFrames = 480;		/* a buffer: 10 ms */
constexpr int kAhead = 4800;		/* frames kept queued: 100 ms */

/* Frames of Web Audio written to the sound ring since the start */
std::atomic<long>	g_frames_out{0};

class BrWebAudioDevice final : public blink::WebAudioDevice {
public:
	BrWebAudioDevice( unsigned channels, bool silent, media::AudioRendererSink::RenderCallback *callback )
		: channels_(channels == 0 ? 2 : channels), silent_(silent), callback_(callback),
		  bus_(media::AudioBus::Create((int)channels_, kFrames)) {}

	~BrWebAudioDevice() override { Stop(); }

	void Start() override
	{
		if ( running_ ) return;
		if ( !silent_ && br_snd_start() != 0 ) {
			printf("browser: web audio: no sound device; the context runs silent\n");
		}
		if ( !silent_ && mix_ < 0 ) mix_ = br_mix_open();
		running_ = true;
		paused_ = false;
		pthread_attr_t at;
		pthread_attr_init(&at);
		pthread_attr_setstacksize(&at, 256 * 1024);
		if ( pthread_create(&thread_, &at, &BrWebAudioDevice::run, this) != 0 ) {
			printf("browser: web audio: no thread to render on\n");
			running_ = false;
		}
		pthread_attr_destroy(&at);
	}

	void Stop() override
	{
		if ( !running_ ) return;
		running_ = false;
		pthread_join(thread_, nullptr);
		br_mix_close(mix_);
		mix_ = -1;
	}

	void Pause() override { paused_ = true; }
	void Resume() override { paused_ = false; }
	double SampleRate() override { return kRate; }
	int FramesPerBuffer() override { return kFrames; }
	int MaxChannelCount() override { return 2; }
	void SetDetectSilence( bool ) override {}
	media::OutputDeviceStatus MaybeCreateSinkAndGetStatus() override
	{
		return media::OUTPUT_DEVICE_STATUS_OK;
	}

private:
	unsigned					channels_;
	bool						silent_;	/* rendered on the clock, not heard */
	media::AudioRendererSink::RenderCallback	*callback_;
	std::unique_ptr<media::AudioBus>		bus_;
	pthread_t					thread_;
	std::atomic<bool>				running_{false};
	std::atomic<bool>				paused_{false};
	short						out_[kFrames * 2];
	int						mix_ = -1;	/* its stream in the mix */

	static void *run( void *self )
	{
		static_cast<BrWebAudioDevice *>( self )->loop();
		return nullptr;
	}

	/* A buffer rendered whenever the ring runs below kAhead frames */
	void loop()
	{
		while ( running_ ) {
			if ( paused_ ) {
				usleep(10000);
				continue;
			}
			int queued = silent_ ? 0 : br_mix_buffered(mix_);
			if ( silent_ ) {
				/* a buffer's worth of time, then a buffer rendered and let go */
				usleep(kFrames * 1000000 / kRate);
			} else if ( queued >= kAhead ) {
				usleep(5000);
				continue;
			}
			base::TimeDelta delay = base::Microseconds((int64_t)queued * 1000000 / kRate);
			bus_->Zero();
			int got = callback_->Render(delay, base::TimeTicks::Now(), media::AudioGlitchInfo(),
						    bus_.get());
			if ( got <= 0 ) {
				usleep(5000);
				continue;
			}
			if ( got > kFrames ) got = kFrames;
			if ( silent_ ) continue;
			const float *l = bus_->channel(0).data();
			const float *r = bus_->channels() > 1 ? bus_->channel(1).data() : l;
			for ( int i = 0; i < got; i++ ) {
				out_[i * 2] = to16(l[i]);
				out_[i * 2 + 1] = to16(r[i]);
			}
			/* the ring takes what fits; the rest waits for its room */
			int done = 0;
			while ( done < got && running_ ) {
				int n = br_mix_write(mix_, out_ + done * 2, got - done);
				if ( n <= 0 ) {
					usleep(5000);
					continue;
				}
				done += n;
			}
			g_frames_out += done;
		}
	}

	static short to16( float v )
	{
		if ( v > 1.0f ) v = 1.0f;
		if ( v < -1.0f ) v = -1.0f;
		return (short)( v * 32767.0f );
	}
};

}  // namespace

/* The device of an AudioContext's destination (BrPlatform::CreateAudioDevice) */
std::unique_ptr<blink::WebAudioDevice> BrCreateAudioDevice( const blink::WebAudioSinkDescriptor &sink,
							   unsigned channels,
							   media::AudioRendererSink::RenderCallback *callback )
{
	/* a silent sink renders on the clock without being heard */
	return std::make_unique<BrWebAudioDevice>(
		channels, sink.Type() != blink::WebAudioSinkDescriptor::kAudible, callback);
}

/* Frames Web Audio has written for the sound device since the start */
long br_web_audio_frames()
{
	return g_frames_out.load();
}
