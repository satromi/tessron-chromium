/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	media_player.cc
 *	ブラウザ: the player behind <video> and <audio> (design 14.6).
 *
 *	Chromium's media pipeline plays the media: a demuxer (FFmpeg's for a
 *	resource given by its address, ChunkDemuxer for a MediaSource), the
 *	renderer with its decoders (FFmpeg, libvpx, libopus, dav1d; the
 *	decrypting ones when a CDM is attached) and its audio and video
 *	renderers, which keep the picture in time with the sound. What a
 *	browser's content layer would give the pipeline is here: the thread
 *	it runs on, the sink the sound goes to (br_audio_sink.cc, on the sound
 *	device), the sink the frames come to, and the link to the element
 *	(blink::WebMediaPlayer) and to the MediaSource and MediaKeys of the
 *	page.
 *
 *	The frames come to BrVideoSink: the application's loop asks the
 *	video renderer for the frame of the time now, and when it changed,
 *	the element is painted again; a paint draws the frame (PaintFrame).
 *	The element's controls are Blink's own (modules/media_controls). The
 *	frame is recorded as an image of its YUV planes: the raster on the
 *	GPU uploads the planes and makes them RGB in a shader, the raster on
 *	the processor makes RGB with libyuv.
 *
 *	A MediaStream given as the element's srcObject (a camera's, a
 *	canvas's or a WebRTC peer's) needs no pipeline: the frames of its
 *	video track come to the same BrVideoSink, and the sound of its audio
 *	track, made 48 kHz stereo, goes to the browser's mix as it comes. A
 *	peer's sound is pulled from WebRTC instead (rtc_audio_tessronos.cc).
 *
 *	A resource given by its address is fetched whole through the network
 *	stack and read from memory. The element's load algorithm must not be
 *	re-entered: the load, and what it reports, run from tasks of their
 *	own.
 *
 *	Each player says on the console what its pipeline does (start, seek,
 *	play, pause, the buffering states, the end) and, every kReportMs
 *	while it plays or waits to, where it stands: the time, the ranges
 *	held, the frame shown and its time, the frames decoded and dropped,
 *	and how the sound ring is drained (br_snd_state). A playback that
 *	stops on a board is followed from these lines.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_writer.h"
#include "base/memory/weak_ptr.h"
#include "base/synchronization/lock.h"
#include "base/task/bind_post_task.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "cc/paint/paint_canvas.h"
#include "cc/paint/skia_paint_canvas.h"
#include "components/viz/common/surfaces/surface_id.h"
#include "cc/paint/paint_flags.h"
#include "cc/paint/paint_image_builder.h"
#include "cc/paint/paint_image_generator.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_parameters.h"
#include "media/base/audio_renderer_sink.h"
#include "media/base/callback_registry.h"
#include "media/base/cdm_context.h"
#include "media/base/decryptor.h"
#include "media/base/decoder.h"
#include "media/base/media_log.h"
#include "media/base/media_log_record.h"
#include "media/base/media_track.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "media/base/pipeline_impl.h"
#include "media/base/video_frame.h"
#include "media/base/video_renderer_sink.h"
#include "media/base/video_transformation.h"
#include "media/filters/chunk_demuxer.h"
#include "media/filters/memory_data_source.h"
#include "media/filters/pipeline_controller.h"
#include "media/renderers/default_decoder_factory.h"
#include "media/renderers/paint_canvas_video_renderer.h"
#include "media/renderers/renderer_impl_factory.h"
#include "third_party/blink/public/platform/web_content_decryption_module.h"
#include "third_party/blink/public/platform/web_content_decryption_module_result.h"
#include "third_party/blink/public/platform/web_media_player.h"
#include "third_party/blink/public/platform/web_media_player_encrypted_media_client.h"
#include "third_party/blink/public/platform/web_media_player_source.h"
#include "third_party/blink/public/platform/web_time_range.h"
#include "third_party/blink/public/platform/modules/mediastream/web_media_stream.h"
#include "third_party/blink/public/platform/modules/mediastream/web_media_stream_audio_sink.h"
#include "third_party/blink/public/platform/modules/mediastream/web_media_stream_track.h"
#include "third_party/blink/public/web/modules/mediastream/media_stream_video_sink.h"
#include "third_party/blink/renderer/platform/mediastream/media_stream_component.h"
#include "third_party/blink/renderer/platform/mediastream/media_stream_descriptor.h"
#include "third_party/blink/renderer/core/html/media/html_media_element.h"
#include "third_party/blink/renderer/core/layout/layout_object.h"
#include "third_party/blink/renderer/modules/encryptedmedia/html_media_element_encrypted_media.h"
#include "third_party/blink/renderer/platform/media/media_player_client.h"
#include "third_party/blink/renderer/platform/media/web_content_decryption_module_impl.h"
#include "third_party/blink/renderer/platform/media/web_media_source_impl.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkPathBuilder.h"
#include "third_party/skia/include/core/SkYUVAInfo.h"
#include "third_party/skia/include/core/SkYUVAPixmaps.h"
#include "ui/gfx/geometry/skia_conversions.h"

#include "../net/br_net.h"

/* The sound device's sink (br_audio_sink.cc) */
scoped_refptr<media::AudioRendererSink> br_audio_sink();

/* The sound ring (br_snd.cc) */
extern "C" int br_snd_start( void );
extern "C" void br_snd_state( long *taken, long *dropped, int *ring, int *device, const char **how, long *stalls );
/* The browser's mix of its sounds, a stream for each track played (br_mix.cc) */
extern "C" int br_mix_open( void );
extern "C" void br_mix_close( int h );
extern "C" int br_mix_write( int h, const short *frames, int nframes );

/* The sound of WebRTC's peers, played through the window's module (rtc_audio_tessronos.cc) */
void *br_rtc_audio_attach( blink::ExecutionContext *context, const blink::WebMediaStream &web );
void br_rtc_audio_play( void *proxy, bool playing, double volume );
void br_rtc_audio_detach( void *proxy );

/* FFmpeg's demuxer (media_demuxer.cc) */
std::unique_ptr<media::Demuxer> br_ffmpeg_demuxer( scoped_refptr<base::SequencedTaskRunner> task_runner,
						   media::DataSource *data,
						   media::Demuxer::EncryptedMediaInitDataCB encrypted_cb,
						   media::MediaLog *media_log );

/* HLS presentations (hls_tessronos.cc) */
bool br_is_hls_playlist( const std::string &bytes );
std::unique_ptr<media::Demuxer> br_hls_demuxer( scoped_refptr<base::SingleThreadTaskRunner> main_task_runner,
						scoped_refptr<base::SequencedTaskRunner> media_task_runner,
						const GURL &url, const url::Origin &origin,
						media::MediaLog *media_log,
						base::RepeatingCallback<void( base::TimeDelta )> request_seek,
						std::unique_ptr<media::TrackManager> tracks );

/* The wait of the process's loop ended (br_proc.cc) */
void br_loop_wake();

namespace {

/* ---------------------------------------------------------------- the media thread */

/*
 * The thread the pipelines run on (the demuxers, the renderers and the
 * decoders' calls), one for all the players, as a frame has one
 */
scoped_refptr<base::SingleThreadTaskRunner> media_task_runner()
{
	static base::Thread *thread;
	if ( thread == nullptr ) {
		thread = new base::Thread("media");
		base::Thread::Options options;
		options.stack_size = 1024 * 1024;
		if ( !thread->StartWithOptions(std::move(options)) ) printf("browser: media: no thread\n");
	}
	return thread->task_runner();
}

/* What the pipeline says goes to the console: its errors and warnings */
class BrMediaLog final : public media::MediaLog {
public:
	BrMediaLog() = default;

protected:
	void AddLogRecordLocked( std::unique_ptr<media::MediaLogRecord> record ) override
	{
		const char *what = nullptr;
		if ( record->type == media::MediaLogRecord::Type::kMediaStatus ) {
			what = "status";
		} else if ( record->type == media::MediaLogRecord::Type::kMessage
			    && ( record->params.Find("error") != nullptr
				 || record->params.Find("warning") != nullptr ) ) {
			what = "message";
		}
		if ( what == nullptr ) return;
		std::string text;
		base::JSONWriter::Write(record->params, &text);
		if ( text.size() > 400 ) text.resize(400);
		printf("browser: media: %s %s\n", what, text.c_str());
	}
};

/* ---------------------------------------------------------------- the frames */

/*
 * The video renderer's sink. The renderer gives it the frames to show
 * (Render, asked for from the application's loop) or one frame at once
 * (PaintSingleFrame: the first after a seek, while paused).
 */
class BrVideoSink final : public media::VideoRendererSink {
public:
	void Start( RenderCallback *callback ) override
	{
		base::AutoLock l(lock_);
		callback_ = callback;
	}
	void Stop() override
	{
		base::AutoLock l(lock_);
		callback_ = nullptr;
	}
	void PaintSingleFrame( scoped_refptr<media::VideoFrame> frame, bool repaint_duplicate ) override
	{
		{
			base::AutoLock l(lock_);
			if ( frame != frame_ || repaint_duplicate ) changed_ = true;
			frame_ = std::move(frame);
		}
		/* the loop, which may wait for long while nothing plays, looks at it now (br_media_tick) */
		br_loop_wake();
	}

	/* The renderer gives frames as time goes (Start): the loop looks at it often */
	bool Rendering()
	{
		base::AutoLock l(lock_);
		return callback_ != nullptr;
	}

	/* From the application's loop: the frame of the time now; true when it is another than before */
	bool Update()
	{
		base::AutoLock l(lock_);
		if ( callback_ != nullptr ) {
			base::TimeTicks now = base::TimeTicks::Now();
			scoped_refptr<media::VideoFrame> f =
				callback_->Render(now, now + base::Milliseconds(16), RenderCallback::RenderingMode::kNormal);
			if ( f && f != frame_ ) {
				frame_ = std::move(f);
				changed_ = true;
			}
		}
		bool changed = changed_;
		changed_ = false;
		return changed;
	}

	scoped_refptr<media::VideoFrame> Frame()
	{
		base::AutoLock l(lock_);
		return frame_;
	}

private:
	base::Lock				lock_;
	RenderCallback				*callback_ = nullptr;
	scoped_refptr<media::VideoFrame>	frame_;
	bool					changed_ = false;
};

/* ---------------------------------------------------------------- a MediaStream */

/* The frames of a stream's video track, handed to the player's sink as they come (on the track's thread) */
class BrStreamVideo final : public blink::MediaStreamVideoSink {
public:
	BrStreamVideo( const blink::WebMediaStreamTrack &track, BrVideoSink *sink, std::atomic<long> *frames )
	{
		ConnectToTrack(track,
			       base::BindRepeating([]( BrVideoSink *sink, std::atomic<long> *frames,
						       scoped_refptr<media::VideoFrame> f, base::TimeTicks ) {
				       sink->PaintSingleFrame(std::move(f), false);
				       ( *frames )++;
			       }, sink, frames),
			       IsSecure::kNo, UsesAlpha::kDefault);
	}
	~BrStreamVideo() override { DisconnectFromTrack(); }
};

/*
 * The sound of a stream's audio track (on the track's thread): made
 * 48 kHz stereo 16-bit, by the volume, and written to the sound ring while
 * the element plays
 */
class BrStreamAudio final : public blink::WebMediaStreamAudioSink {
public:
	explicit BrStreamAudio( const blink::WebMediaStreamTrack &track ) : track_(track)
	{
		br_snd_start();
		mix_ = br_mix_open();
		blink::WebMediaStreamAudioSink::AddToAudioTrack(this, track_);
	}
	~BrStreamAudio() override
	{
		blink::WebMediaStreamAudioSink::RemoveFromAudioTrack(this, track_);
		br_mix_close(mix_);
	}

	void SetPlaying( bool playing, double volume )
	{
		playing_ = playing;
		volume_ = (float)volume;
	}

	void OnSetFormat( const media::AudioParameters &params ) override
	{
		rate_ = params.sample_rate() > 0 ? params.sample_rate() : 48000;
		pos_ = 0;
	}

	void OnData( const media::AudioBus &bus, base::TimeTicks ) override
	{
		if ( !playing_ || bus.frames() <= 0 || bus.channels() <= 0 ) return;
		const float *l = bus.channel(0).data();
		const float *r = bus.channels() > 1 ? bus.channel(1).data() : l;
		float vol = volume_;
		double step = (double)rate_ / 48000.0;
		out_.clear();
		/* resampled by taking the sample nearest each output frame's time */
		for ( ; pos_ < bus.frames(); pos_ += step ) {
			int i = (int)pos_;
			out_.push_back(Sample(l[i] * vol));
			out_.push_back(Sample(r[i] * vol));
		}
		pos_ -= bus.frames();
		if ( !out_.empty() ) br_mix_write(mix_, out_.data(), (int)( out_.size() / 2 ));
	}

private:
	static short Sample( float v )
	{
		v = v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v;
		return (short)( v * 32767.0f );
	}

	blink::WebMediaStreamTrack	track_;
	std::atomic<bool>		playing_{false};
	std::atomic<float>		volume_{1.0f};
	int				rate_ = 48000;
	double				pos_ = 0;
	std::vector<short>		out_;
	int				mix_ = -1;	/* its stream in the mix */
};

/* What became of the frames painted: shown, uploaded as YUV planes, or made RGB on the processor */
std::atomic<long>	g_frames_shown{0}, g_frames_planes{0}, g_frames_rgb{0};

/*
 * The planes of a frame of three planes, 8 bits or 10 bits in 16 (the
 * YUV that Skia takes); kUnknown for another
 */
std::tuple<SkYUVAInfo::PlaneConfig, SkYUVAInfo::Subsampling> frame_planes( const media::VideoFrame &f )
{
	if ( f.BitDepth() == 8 || f.BitDepth() == 10 ) {
		switch ( f.format() ) {
		case media::PIXEL_FORMAT_I420:
		case media::PIXEL_FORMAT_YUV420P10:
			return { SkYUVAInfo::PlaneConfig::kY_U_V, SkYUVAInfo::Subsampling::k420 };
		case media::PIXEL_FORMAT_I422:
		case media::PIXEL_FORMAT_YUV422P10:
			return { SkYUVAInfo::PlaneConfig::kY_U_V, SkYUVAInfo::Subsampling::k422 };
		case media::PIXEL_FORMAT_I444:
		case media::PIXEL_FORMAT_YUV444P10:
			return { SkYUVAInfo::PlaneConfig::kY_U_V, SkYUVAInfo::Subsampling::k444 };
		default:
			break;
		}
	}
	return { SkYUVAInfo::PlaneConfig::kUnknown, SkYUVAInfo::Subsampling::kUnknown };
}

/*
 * The pixels of a frame, for Skia when it draws the frame's image. The
 * raster on the GPU (port/gpu/br_gpu.cc) asks for the YUV planes
 * (QueryYUVA, GetYUVAPlanes): they are copied as they are and uploaded
 * as textures, and a shader of Skia's makes them RGB as it draws. The
 * raster on the processor asks for RGB (GetPixels), made by libyuv.
 *
 * A frame of 10 bits (HEVC Main 10, VP9 profile 2, AV1 of 10 bits) goes
 * up as textures of 16 bits when the GPU takes them: each sample's 10
 * bits at the top and repeated below, so that 1023 is 1.0, and the
 * matrix of 10 bits (BT.2020 of 10 bits) makes the RGB. Without them it
 * goes up in 8 bits, each sample rounded to its top 8, with the matrix
 * of 8 bits.
 */
class BrFrameGenerator final : public cc::PaintImageGenerator {
public:
	explicit BrFrameGenerator( scoped_refptr<media::VideoFrame> frame )
		: cc::PaintImageGenerator(Info(*frame), frame->hdr_metadata()), frame_(std::move(frame)) {}

	static SkImageInfo Info( const media::VideoFrame &f )
	{
		return SkImageInfo::Make(f.visible_rect().width(), f.visible_rect().height(), kN32_SkColorType,
					 kOpaque_SkAlphaType, f.CompatRGBColorSpace().ToSkColorSpace());
	}

	sk_sp<const SkData> GetEncodedData() const override { return nullptr; }

	bool GetPixels( SkPixmap dst, size_t, cc::PaintImage::GeneratorClientId, uint32_t ) override
	{
		g_frames_rgb++;
		if ( dst.colorType() == kN32_SkColorType ) {
			media::PaintCanvasVideoRenderer::ConvertVideoFrameToRGBPixels(
				frame_.get(), base::span<uint8_t>(static_cast<uint8_t *>(dst.writable_addr()),
								  dst.computeByteSize()),
				dst.rowBytes(), kN32_SkColorType);
			return true;
		}
		SkBitmap b;
		if ( !b.tryAllocPixels(dst.info().makeColorType(kN32_SkColorType)) ) return false;
		media::PaintCanvasVideoRenderer::ConvertVideoFrameToRGBPixels(
			frame_.get(), base::span<uint8_t>(static_cast<uint8_t *>(b.getPixels()), b.computeByteSize()),
			b.rowBytes(), kN32_SkColorType);
		return b.pixmap().readPixels(dst);
	}

	bool QueryYUVA( const SkYUVAPixmapInfo::SupportedDataTypes &types, SkYUVAPixmapInfo *info ) const override
	{
		auto [config, sub] = frame_planes(*frame_);
		if ( config == SkYUVAInfo::PlaneConfig::kUnknown ) return false;
		SkYUVAPixmapInfo::DataType type = SkYUVAPixmapInfo::DataType::kUnorm8;
		if ( frame_->BitDepth() == 10 && types.supported(config, SkYUVAPixmapInfo::DataType::kUnorm16) ) {
			type = SkYUVAPixmapInfo::DataType::kUnorm16;
		}
		if ( !types.supported(config, type) ) return false;
		if ( info == nullptr ) return true;
		SkYUVColorSpace cs;
		int depth = ( type == SkYUVAPixmapInfo::DataType::kUnorm16 ) ? 10 : 8;
		if ( !frame_->ColorSpace().ToSkYUVColorSpace(depth, &cs) ) cs = kRec601_SkYUVColorSpace;
		*info = SkYUVAPixmapInfo(SkYUVAInfo({ frame_->visible_rect().width(), frame_->visible_rect().height() },
						    config, sub, cs),
					 type, nullptr);
		return true;
	}

	bool GetYUVAPlanes( const SkYUVAPixmaps &pixmaps, size_t, uint32_t, cc::PaintImage::GeneratorClientId ) override
	{
		if ( pixmaps.numPlanes() != 3 ) return false;
		bool deep = frame_->BitDepth() == 10;
		bool out16 = pixmaps.dataType() == SkYUVAPixmaps::DataType::kUnorm16;
		if ( out16 && !deep ) return false;
		for ( int p = 0; p < 3; p++ ) {
			const gfx::Size s = media::VideoFrame::PlaneSizeInSamples(frame_->format(), (size_t)p,
										 frame_->visible_rect().size());
			const SkPixmap &out = pixmaps.plane(p);
			if ( out.width() != s.width() || out.height() != s.height() ) return false;
			const uint8_t *src = frame_->visible_data((size_t)p);
			size_t stride = frame_->stride((size_t)p);
			uint8_t *dst = static_cast<uint8_t *>(out.writable_addr());
			for ( int y = 0; y < s.height(); y++ ) {
				uint8_t *d = dst + (size_t)y * out.rowBytes();
				const uint8_t *r = src + (size_t)y * stride;

				if ( !deep ) {
					memcpy(d, r, (size_t)s.width());
					continue;
				}
				const uint16_t *v = reinterpret_cast<const uint16_t *>(r);
				if ( out16 ) {
					uint16_t *d16 = reinterpret_cast<uint16_t *>(d);
					for ( int x = 0; x < s.width(); x++ ) {
						unsigned u = v[x] & 0x3FFu;
						d16[x] = (uint16_t)( ( u << 6 ) | ( u >> 4 ) );
					}
				} else {
					for ( int x = 0; x < s.width(); x++ ) {
						unsigned u = ( ( v[x] & 0x3FFu ) + 2 ) >> 2;
						d[x] = (uint8_t)( u > 255 ? 255 : u );
					}
				}
			}
		}
		g_frames_planes++;
		return true;
	}

private:
	scoped_refptr<media::VideoFrame>	frame_;
};

/* The image of the frame last painted by a player, kept while that frame is shown */
struct BrFrameImage {
	cc::PaintImage::Id	id = cc::PaintImage::GetNextId();
	media::VideoFrame::ID	frame_id;
	bool			have = false;
	cc::PaintImage		image;
};

/*
 * A frame drawn into the element's rectangle. A frame of YUV planes Skia
 * takes (I420, I422, I444 of 8 or 10 bits) not turned or mirrored is recorded as an
 * image whose generator gives the planes (BrFrameGenerator): rastered on
 * the GPU the planes become RGB there, on the processor libyuv makes RGB.
 * Another goes through PaintCanvasVideoRenderer as before. The image of
 * a frame is kept, so that painting the same frame again draws the
 * texture Skia made of it.
 */
void PaintFrame( media::PaintCanvasVideoRenderer &renderer, BrFrameImage &kept,
		 scoped_refptr<media::VideoFrame> frame, cc::PaintCanvas *canvas, const gfx::Rect &rect,
		 const cc::PaintFlags &flags )
{
	if ( !kept.have || kept.frame_id != frame->unique_id() ) g_frames_shown++;
	auto [config, sub] = frame_planes(*frame);
	(void)sub;
	const std::optional<media::VideoTransformation> &turn = frame->metadata().transformation;
	if ( config == SkYUVAInfo::PlaneConfig::kUnknown || frame->HasSharedImage()
	     || ( turn && ( turn->rotation != media::VIDEO_ROTATION_0 || turn->mirrored ) ) ) {
		media::PaintCanvasVideoRenderer::PaintParams params;
		params.dest_rect = gfx::RectF(rect);
		kept.have = false;
		kept.image = cc::PaintImage();
		renderer.Paint(std::move(frame), canvas, flags, params, nullptr);
		return;
	}
	if ( !kept.have || kept.frame_id != frame->unique_id() ) {
		kept.image = cc::PaintImageBuilder::WithDefault()
			.set_id(kept.id)
			.set_animation_type(cc::PaintImage::AnimationType::kVideo)
			.set_completion_state(cc::PaintImage::CompletionState::kDone)
			.set_paint_image_generator(sk_make_sp<BrFrameGenerator>(frame))
			.TakePaintImage();
		kept.frame_id = frame->unique_id();
		kept.have = true;
	}
	cc::PaintFlags f;
	f.setAlphaf(flags.getAlphaf());
	f.setBlendMode(flags.getBlendMode());
	const gfx::Rect &v = frame->visible_rect();
	canvas->drawImageRect(kept.image, SkRect::MakeWH((float)v.width(), (float)v.height()), gfx::RectToSkRect(rect),
			      cc::PaintFlags::FilterQualityToSkSamplingOptions(flags.getFilterQuality()), &f,
			      SkCanvas::kFast_SrcRectConstraint);
}

/* ---------------------------------------------------------------- the player */

class BrWebMediaPlayer;

blink::Vector<BrWebMediaPlayer *> &players()
{
	static auto *p = new blink::Vector<BrWebMediaPlayer *>();
	return *p;
}

/* The players made so far: each is known on the console by its number */
int	g_player_count = 0;

constexpr int kReportMs = 5000;		/* a player's report while it plays or waits to */

const char *buffering_name( media::BufferingState s )
{
	return s == media::BUFFERING_HAVE_ENOUGH ? "enough" : "nothing";
}

/*
 * A CDM's context as the pipeline's thread sees it. A library CDM
 * (media::CdmAdapter, the Widevine CDM) is single-threaded: all of it,
 * its decryptor too, is called on the thread it was made on, the
 * engine's, and checks that it is. The pipeline runs on a thread of its
 * own, so the decryptor's calls are posted to the CDM's thread and their answers
 * posted back to the thread that called.
 *
 * The context holds the player's reference to the CDM, and the calls it
 * posts name the CDM by its plain address. The last reference may be the
 * player's (the document gone, its MediaKeys collected), and a CDM goes
 * on whichever thread lets that one go: were the reference let go with the
 * pipeline's parts, on the pipeline's thread, the CDM would be destroyed
 * there while the calls the stopped pipeline posted (its decoders'
 * DeinitializeDecoder and CancelDecrypt) still wait for the CDM's thread,
 * and they would then run on a CDM no longer there. The reference is
 * therefore handed to the CDM's thread when the context goes, behind every
 * call posted before (cdm_release_on), and the CDM is let go there, after
 * them.
 */
void cdm_release_on( const scoped_refptr<base::SingleThreadTaskRunner> &thread, std::unique_ptr<media::CdmContextRef> ref )
{
	if ( !ref ) return;
	thread->PostTask(FROM_HERE, base::BindOnce([]( std::unique_ptr<media::CdmContextRef> ) {}, std::move(ref)));
}

class BrCdmOnThread final : public media::CdmContext, public media::Decryptor {
public:
	BrCdmOnThread( std::unique_ptr<media::CdmContextRef> ref, scoped_refptr<base::SingleThreadTaskRunner> cdm_thread )
		: ref_(std::move(ref)), cdm_(ref_->GetCdmContext()), thread_(std::move(cdm_thread)) {}

	~BrCdmOnThread() override
	{
		cdm_release_on(thread_, std::move(ref_));
	}

	std::unique_ptr<media::CallbackRegistration> RegisterEventCB( EventCB cb ) override
	{
		/* the registry posts each event to the thread that registered */
		return cdm_->RegisterEventCB(std::move(cb));
	}

	media::Decryptor *GetDecryptor() override
	{
		return this;
	}

	void Decrypt( StreamType type, scoped_refptr<media::DecoderBuffer> buf, DecryptCB cb ) override
	{
		On(base::BindOnce([]( media::CdmContext *c, StreamType t, scoped_refptr<media::DecoderBuffer> b, DecryptCB done ) {
			c->GetDecryptor()->Decrypt(t, std::move(b), std::move(done));
		}, cdm_, type, std::move(buf), base::BindPostTaskToCurrentDefault(std::move(cb))));
	}

	void CancelDecrypt( StreamType type ) override
	{
		On(base::BindOnce([]( media::CdmContext *c, StreamType t ) { c->GetDecryptor()->CancelDecrypt(t); }, cdm_, type));
	}

	void InitializeAudioDecoder( const media::AudioDecoderConfig &config, DecoderInitCB cb ) override
	{
		On(base::BindOnce([]( media::CdmContext *c, media::AudioDecoderConfig cf, DecoderInitCB done ) {
			c->GetDecryptor()->InitializeAudioDecoder(cf, std::move(done));
		}, cdm_, config, base::BindPostTaskToCurrentDefault(std::move(cb))));
	}

	void InitializeVideoDecoder( const media::VideoDecoderConfig &config, DecoderInitCB cb ) override
	{
		On(base::BindOnce([]( media::CdmContext *c, media::VideoDecoderConfig cf, DecoderInitCB done ) {
			c->GetDecryptor()->InitializeVideoDecoder(cf, std::move(done));
		}, cdm_, config, base::BindPostTaskToCurrentDefault(std::move(cb))));
	}

	void DecryptAndDecodeAudio( scoped_refptr<media::DecoderBuffer> buf, AudioDecodeCB cb ) override
	{
		On(base::BindOnce([]( media::CdmContext *c, scoped_refptr<media::DecoderBuffer> b, AudioDecodeCB done ) {
			c->GetDecryptor()->DecryptAndDecodeAudio(std::move(b), std::move(done));
		}, cdm_, std::move(buf), base::BindPostTaskToCurrentDefault(std::move(cb))));
	}

	void DecryptAndDecodeVideo( scoped_refptr<media::DecoderBuffer> buf, VideoDecodeCB cb ) override
	{
		On(base::BindOnce([]( media::CdmContext *c, scoped_refptr<media::DecoderBuffer> b, VideoDecodeCB done ) {
			c->GetDecryptor()->DecryptAndDecodeVideo(std::move(b), std::move(done));
		}, cdm_, std::move(buf), base::BindPostTaskToCurrentDefault(std::move(cb))));
	}

	void ResetDecoder( StreamType type ) override
	{
		On(base::BindOnce([]( media::CdmContext *c, StreamType t ) { c->GetDecryptor()->ResetDecoder(t); }, cdm_, type));
	}

	void DeinitializeDecoder( StreamType type ) override
	{
		On(base::BindOnce([]( media::CdmContext *c, StreamType t ) { c->GetDecryptor()->DeinitializeDecoder(t); },
				  cdm_, type));
	}

private:
	/* a call run on the CDM's thread, at once when it is the caller's */
	void On( base::OnceClosure call )
	{
		if ( thread_->BelongsToCurrentThread() ) {
			std::move(call).Run();
		} else {
			thread_->PostTask(FROM_HERE, std::move(call));
		}
	}

	std::unique_ptr<media::CdmContextRef>		ref_;		/* keeps the CDM; let go on its thread */
	media::CdmContext				*cdm_;
	scoped_refptr<base::SingleThreadTaskRunner>	thread_;
};

/*
 * What the pipeline's thread uses until the pipeline has stopped there:
 * let go on that thread, after it
 */
struct BrPipelineParts {
	std::unique_ptr<media::MediaLog>		media_log;
	std::unique_ptr<media::DefaultDecoderFactory>	decoders;
	std::unique_ptr<media::RendererImplFactory>	renderers;
	std::unique_ptr<BrVideoSink>			video_sink;
	std::unique_ptr<media::Demuxer>			demuxer;
	std::unique_ptr<media::DataSource>		data;
	/* the MediaKeys set before the pipeline runs, not yet given to it */
	std::unique_ptr<media::CdmContextRef>		pending_cdm;
	/* the contexts given to the pipeline, each with its reference to the CDM */
	std::unique_ptr<BrCdmOnThread>			cdm_on_thread;
	std::unique_ptr<BrCdmOnThread>			pending_cdm_on_thread;
	scoped_refptr<base::SingleThreadTaskRunner>	cdm_thread;	/* the CDM's (the engine's) */

	~BrPipelineParts()
	{
		/* a reference not given to the pipeline goes on the CDM's thread too */
		if ( cdm_thread ) cdm_release_on(cdm_thread, std::move(pending_cdm));
	}
};

class BrWebMediaPlayer final : public blink::WebMediaPlayer, public media::Pipeline::Client {
public:
	BrWebMediaPlayer( blink::MediaPlayerClient *client, blink::HTMLMediaElement *element,
			  blink::WebMediaPlayerEncryptedMediaClient *encrypted_client,
			  blink::WebContentDecryptionModule *initial_cdm,
			  scoped_refptr<base::SingleThreadTaskRunner> task_runner )
		: client_(client), element_(element), encrypted_client_(encrypted_client),
		  main_task_runner_(std::move(task_runner)), media_task_runner_(media_task_runner()),
		  parts_(new BrPipelineParts), id_(++g_player_count)
	{
		players().push_back(this);
		parts_->cdm_thread = main_task_runner_;
		parts_->media_log = std::make_unique<BrMediaLog>();
		parts_->decoders = std::make_unique<media::DefaultDecoderFactory>(nullptr);
		parts_->renderers = std::make_unique<media::RendererImplFactory>(
			parts_->media_log.get(), parts_->decoders.get(),
			base::BindRepeating([]() -> media::GpuVideoAcceleratorFactories * { return nullptr; }),
			media::MediaPlayerLoggingID(0), nullptr);
		parts_->video_sink = std::make_unique<BrVideoSink>();
		video_sink_ = parts_->video_sink.get();
		audio_sink_ = br_audio_sink();
		worker_task_runner_ = base::ThreadPool::CreateTaskRunner({});

		auto pipeline = std::make_unique<media::PipelineImpl>(
			media_task_runner_, main_task_runner_,
			base::BindRepeating(&BrWebMediaPlayer::CreateRenderer, base::Unretained(this)),
			parts_->media_log.get());
		controller_ = std::make_unique<media::PipelineController>(
			std::move(pipeline),
			base::BindRepeating(&BrWebMediaPlayer::OnStarted, weak_factory_.GetWeakPtr()),
			base::BindRepeating(&BrWebMediaPlayer::OnSeeked, weak_factory_.GetWeakPtr()),
			base::DoNothing(), base::DoNothing(), base::DoNothing(),
			base::BindRepeating(&BrWebMediaPlayer::OnError, weak_factory_.GetWeakPtr()));
		if ( initial_cdm != nullptr ) SetCdm(initial_cdm);
	}

	~BrWebMediaPlayer() override
	{
		blink::wtf_size_t pos = players().Find(this);
		if ( pos != blink::kNotFound ) players().EraseAt(pos);
		if ( started_ ) Report("let go");
		stream_video_.reset();
		stream_audio_.reset();
		br_rtc_audio_detach(stream_rtc_);
		stream_rtc_ = nullptr;
		client_ = nullptr;
		controller_->Stop();
		controller_.reset();
		/* the demuxer, the renderer's sinks and the log after the pipeline, on its thread */
		media_task_runner_->PostTask(FROM_HERE,
					     base::BindOnce([]( std::unique_ptr<BrPipelineParts> ) {},
							    std::move(parts_)));
	}

	LoadTiming Load( LoadType type, const blink::WebMediaPlayerSource &source, CorsMode, bool ) override
	{
		if ( source.IsMediaStream() ) {
			StartStream(source.GetAsMediaStream());
			return LoadTiming::kImmediate;
		}
		if ( type == kLoadTypeMediaSource ) {
			mse_ = true;
			main_task_runner_->PostTask(FROM_HERE, base::BindOnce(&BrWebMediaPlayer::StartMediaSource,
									      weak_factory_.GetWeakPtr()));
			return LoadTiming::kImmediate;
		}
		if ( source.IsURL() ) url_ = blink::KURL(source.GetAsURL()).GetString().Utf8();
		main_task_runner_->PostTask(FROM_HERE, base::BindOnce(&BrWebMediaPlayer::DoLoad,
								      weak_factory_.GetWeakPtr()));
		return LoadTiming::kImmediate;
	}

	/* From the application's loop: a resource waiting to be played, and the frame of the time now */
	/* Whether Tick has something to look at as time goes */
	bool WantsTicks()
	{
		return stream_ || ( !paused_ && !ended_ ) || ( deferred_ && element_ != nullptr && !element_->paused() )
		       || ( video_sink_ != nullptr && video_sink_->Rendering() );
	}

	void Tick()
	{
		if ( deferred_ && element_ != nullptr && !element_->paused() ) Fetch();
		if ( stream_ ) StreamTick();
		if ( video_sink_ != nullptr && video_sink_->Update() ) {
			base::TimeTicks now = base::TimeTicks::Now();
			if ( !presented_at_.is_null() ) {
				/* the time between frames, averaged over a few */
				frame_gap_ = frame_gap_.is_zero() ? now - presented_at_ : ( frame_gap_ * 7 + ( now - presented_at_ ) ) / 8;
			}
			presented_at_ = now;
			frames_changed_++;
			if ( client_ != nullptr ) client_->Repaint();
			/* a page's requestVideoFrameCallback told of the frame */
			if ( frame_wanted_ && client_ != nullptr ) {
				frame_wanted_ = false;
				client_->OnRequestVideoFrameCallback();
			}
		}
		/* where it stands, every kReportMs while it plays or waits to */
		if ( started_ && !paused_ && !ended_ ) {
			base::TimeTicks now = base::TimeTicks::Now();
			if ( last_report_.is_null() ) last_report_ = now;
			if ( now - last_report_ >= base::Milliseconds(kReportMs) ) Report("at");
		}
	}

	/* ------------------------------------------------ blink::WebMediaPlayer */

	void Play() override
	{
		paused_ = false;
		if ( stream_ ) {
			stream_started_ = base::TimeTicks::Now() - stream_time_;
			if ( stream_audio_ ) stream_audio_->SetPlaying(true, volume_);
			br_rtc_audio_play(stream_rtc_, true, volume_);
			return;
		}
		controller_->SetPlaybackRate(rate_);
		Report("play");
	}
	void Pause( PauseReason ) override
	{
		paused_ = true;
		if ( stream_ ) {
			stream_time_ = base::TimeTicks::Now() - stream_started_;
			if ( stream_audio_ ) stream_audio_->SetPlaying(false, volume_);
			br_rtc_audio_play(stream_rtc_, false, volume_);
			return;
		}
		controller_->SetPlaybackRate(0.0);
		paused_time_ = ended_ ? DurationTime() : MediaTime();
		Report("pause");
	}
	/*
	 * The element's preload: "none" and "metadata" (the default) leave
	 * a resource given by its address unfetched until the element plays,
	 * since it is fetched whole; "auto" (or autoplay) takes it now.
	 */
	void SetPreload( Preload preload ) override
	{
		preload_ = preload;
		if ( deferred_ && preload == kPreloadAuto ) Fetch();
	}
	void SetShouldPauseWhenFrameIsHidden( bool ) override {}
	void RecordAutoPictureInPictureInfo( const media::PictureInPictureEventsInfo::AutoPipInfo & ) override {}
	/* there is one output, known as "default": choosing it (or "") changes nothing */
	bool SetSinkId( const blink::WebString &sink_id, blink::WebSetSinkIdCompleteCallback callback ) override
	{
		if ( sink_id.IsEmpty() || sink_id.Utf8() == "default" ) {
			std::move(callback).Run(std::nullopt);
			return true;
		}
		std::move(callback).Run(blink::WebSetSinkIdError::kNotFound);
		return false;
	}
	void Seek( double seconds ) override
	{
		base::TimeDelta t = base::Seconds(seconds);
		ended_ = false;
		seeking_ = true;
		seek_time_ = t;
		if ( paused_ ) paused_time_ = t;
		if ( ready_state_ > kReadyStateHaveMetadata ) SetReadyState(kReadyStateHaveMetadata);
		printf("browser: media %d: seek to %.3f s\n", id_, seconds);
		controller_->Seek(t, true);
	}
	void SetRate( double rate ) override
	{
		rate_ = rate;
		if ( !paused_ ) controller_->SetPlaybackRate(rate);
	}
	void SetVolume( double volume ) override
	{
		volume_ = volume;
		if ( stream_audio_ ) stream_audio_->SetPlaying(!paused_, volume);
		br_rtc_audio_play(stream_rtc_, !paused_, volume);
		controller_->SetVolume((float)volume);
	}
	void SetLatencyHint( double ) override {}
	void SetPreservesPitch( bool preserves ) override { controller_->SetPreservesPitch(preserves); }
	void SetWasPlayedWithUserActivationAndHighMediaEngagement( bool ) override {}
	void SetVolumeMultiplier( double ) override {}
	/*
	 * A picture-in-picture window asked for the video: the window draws
	 * the player's frames itself (br_media_paint_player), so the surface
	 * is only a name for the session, made once
	 */
	void OnRequestPictureInPicture() override
	{
		if ( !surface_.is_valid() ) {
			surface_ = viz::SurfaceId(viz::FrameSinkId(1, (uint32_t)id_),
						  viz::LocalSurfaceId(1, base::UnguessableToken::Create()));
		}
	}
	std::optional<viz::SurfaceId> GetSurfaceId() override
	{
		if ( !surface_.is_valid() ) return std::nullopt;
		return surface_;
	}
	int GetPlayerId() override { return id_; }
	void SuspendForFrameClosed() override {}
	blink::WebTimeRanges Buffered() const override
	{
		blink::WebTimeRanges out;
		media::Ranges<base::TimeDelta> r = controller_->GetBufferedTimeRanges();
		for ( size_t i = 0; i < r.size(); ++i ) out.Add(r.start(i).InSecondsF(), r.end(i).InSecondsF());
		return out;
	}
	/* What has a known length can be sought anywhere in it: a MediaSource's page fetches what is not held */
	blink::WebTimeRanges Seekable() const override
	{
		blink::WebTimeRanges out;
		if ( ready_state_ < kReadyStateHaveMetadata ) return out;
		double d = Duration();
		if ( std::isfinite(d) && d > 0 ) out.Add(0.0, d);
		else if ( !mse_ ) return Buffered();
		return out;
	}
	bool HasVideo() const override { return has_video_; }
	bool HasAudio() const override { return has_audio_; }
	bool IsEnded() const override { return ended_; }
	gfx::Size NaturalSize() const override { return natural_size_; }
	gfx::Size VisibleSize() const override { return natural_size_; }
	bool Paused() const override { return paused_; }
	bool Seeking() const override { return seeking_; }
	double Duration() const override
	{
		if ( ready_state_ == kReadyStateHaveNothing ) return std::numeric_limits<double>::quiet_NaN();
		if ( stream_ ) return std::numeric_limits<double>::infinity();
		base::TimeDelta d = controller_->GetMediaDuration();
		if ( d == media::kInfiniteDuration ) return std::numeric_limits<double>::infinity();
		return d.InSecondsF();
	}
	double CurrentTime() const override
	{
		if ( stream_ ) return ( paused_ ? stream_time_ : base::TimeTicks::Now() - stream_started_ ).InSecondsF();
		if ( seeking_ ) return seek_time_.InSecondsF();
		if ( ended_ ) return Duration();
		if ( paused_ ) return paused_time_.InSecondsF();
		return MediaTime().InSecondsF();
	}
	NetworkState GetNetworkState() const override { return network_state_; }
	ReadyState GetReadyState() const override { return ready_state_; }
	blink::WebString GetErrorMessage() const override { return blink::WebString::FromUtf8(error_); }
	bool DidLoadingProgress() override
	{
		bool p = controller_->DidLoadingProgress() || progressed_;
		progressed_ = false;
		return p;
	}
	bool WouldTaintOrigin() const override { return false; }
	double MediaTimeForTimeValue( double t ) const override { return t; }
	unsigned DecodedFrameCount() const override { return controller_->GetStatistics().video_frames_decoded; }
	unsigned DroppedFrameCount() const override { return controller_->GetStatistics().video_frames_dropped; }
	uint64_t AudioDecodedByteCount() const override { return controller_->GetStatistics().audio_bytes_decoded; }
	uint64_t VideoDecodedByteCount() const override { return controller_->GetStatistics().video_bytes_decoded; }
	bool HasAvailableVideoFrame() const override { return video_sink_->Frame() != nullptr; }
	bool HasReadableVideoFrame() const override { return video_sink_->Frame() != nullptr; }
	void OnFrozen() override {}
	std::optional<media::VideoFrame::ID> CurrentFrameId() const override
	{
		scoped_refptr<media::VideoFrame> f = video_sink_->Frame();
		if ( !f ) return std::nullopt;
		return f->unique_id();
	}
	void Paint( cc::PaintCanvas *canvas, const gfx::Rect &rect, const cc::PaintFlags &flags, bool ) override;

	int id() const { return id_; }
	long frames_changed() const { return frames_changed_; }

	/* The frame now, in a window of its own 'w' x 'h': black round it, its aspect kept */
	bool PaintTo( SkCanvas *c, int w, int h )
	{
		scoped_refptr<media::VideoFrame> frame = video_sink_->Frame();
		c->clear(SK_ColorBLACK);
		if ( !frame || w <= 0 || h <= 0 ) return false;
		const gfx::Rect &v = frame->visible_rect();
		int fw = v.width() > 0 ? v.width() : w, fh = v.height() > 0 ? v.height() : h;
		int dw = w, dh = (int)( (long)w * fh / fw );
		if ( dh > h ) {
			dh = h;
			dw = (int)( (long)h * fw / fh );
		}
		cc::SkiaPaintCanvas pc(c);
		cc::PaintFlags flags;
		flags.setFilterQuality(cc::PaintFlags::FilterQuality::kLow);
		PaintFrame(frame_renderer_, window_image_, std::move(frame), &pc,
			   gfx::Rect(( w - dw ) / 2, ( h - dh ) / 2, dw, dh), flags);
		return true;
	}
	media::PaintCanvasVideoRenderer *GetPaintCanvasVideoRenderer() override { return &frame_renderer_; }
	/*
	 * The element lets the player go: it is deleted later, and until
	 * then the pipeline's news is told to nobody (the element no longer
	 * holds the player it would ask)
	 */
	void Shutdown() override
	{
		client_ = nullptr;
		element_ = nullptr;
		encrypted_client_ = nullptr;
	}
	bool IsVideoBeingCaptured() const override { return false; }
	/* the frame shown, for a canvas or WebGL that draws the element */
	scoped_refptr<media::VideoFrame> GetCurrentFrameThenUpdate() override { return video_sink_->Frame(); }

	/* A page waits for the next frame shown (requestVideoFrameCallback) */
	void RequestVideoFrameCallback() override { frame_wanted_ = true; }

	/* The frame shown last, as the callbacks are told of it */
	std::unique_ptr<VideoFramePresentationMetadata> GetVideoFramePresentationMetadata() override
	{
		auto m = std::make_unique<VideoFramePresentationMetadata>();
		scoped_refptr<media::VideoFrame> f = video_sink_ != nullptr ? video_sink_->Frame() : nullptr;
		m->presented_frames = (uint32_t)frames_changed_;
		m->presentation_time = presented_at_.is_null() ? base::TimeTicks::Now() : presented_at_;
		/*
		 * the next refresh after the frame was shown, and never before now:
		 * the callbacks run in the frame after it, and a slow frame must not
		 * make the page's frame expected in the past
		 */
		m->expected_display_time = std::max(m->presentation_time, base::TimeTicks::Now()) + base::Milliseconds(16);
		m->rendering_interval = base::Milliseconds(16);
		m->average_frame_duration = frame_gap_;
		if ( f ) {
			m->width = f->visible_rect().width();
			m->height = f->visible_rect().height();
			m->media_time = f->timestamp();
			m->metadata.MergeMetadataFrom(f->metadata());
		} else {
			m->width = natural_size_.width();
			m->height = natural_size_.height();
		}
		return m;
	}
	base::WeakPtr<blink::WebMediaPlayer> AsWeakPtr() override { return weak_factory_.GetWeakPtr(); }

	/*
	 * MediaKeys set on the element (setMediaKeys): its CDM's context goes
	 * to the pipeline, which decrypts with it. It cannot be taken away
	 * again, as frames may be being decrypted on the media thread.
	 */
	void SetContentDecryptionModule( blink::WebContentDecryptionModule *cdm,
					 blink::WebContentDecryptionModuleResult result ) override
	{
		if ( cdm == nullptr ) {
			result.CompleteWithError(blink::kWebContentDecryptionModuleExceptionInvalidStateError, 0,
						 "The existing ContentDecryptionModule object cannot be removed at this time.");
			return;
		}
		cdm_result_ = std::make_unique<blink::WebContentDecryptionModuleResult>(result);
		SetCdm(cdm);
	}

	/* ------------------------------------------------ media::Pipeline::Client */

	void OnError( media::PipelineStatus status ) override
	{
		printf("browser: media: pipeline error %d\n", (int)status.code());
		error_ = "the media could not be played (" + std::to_string((int)status.code()) + ")";
		SetNetworkState(ready_state_ == kReadyStateHaveNothing ? kNetworkStateFormatError
								       : kNetworkStateDecodeError);
	}
	void OnFallback( media::PipelineStatus ) override {}
	void OnEnded() override
	{
		if ( !controller_->IsStable() ) return;
		printf("browser: media %d: ended at %d ms\n", id_, (int)( Duration() * 1000 ));
		ended_ = true;
		if ( !paused_ && client_ != nullptr ) client_->TimeChanged();
	}
	void OnMetadata( const media::PipelineMetadata &metadata ) override
	{
		has_audio_ = metadata.has_audio;
		has_video_ = metadata.has_video;
		natural_size_ = metadata.natural_size;
		printf("browser: media: %dx%d%s%s\n", natural_size_.width(), natural_size_.height(),
		       has_video_ ? ", video" : "", has_audio_ ? ", sound" : "");
		SetReadyState(kReadyStateHaveMetadata);
		if ( client_ != nullptr ) {
			client_->SizeChanged();
			client_->DurationChanged();
		}
	}
	void OnBufferingStateChange( media::BufferingState state, media::BufferingStateChangeReason ) override
	{
		buffering_ = state;
		Report(state == media::BUFFERING_HAVE_ENOUGH ? "has enough" : "has nothing");
		if ( controller_->IsPendingSeek() ) return;
		if ( state == media::BUFFERING_HAVE_ENOUGH ) {
			SetReadyState(kReadyStateHaveEnoughData);
			if ( !mse_ ) SetNetworkState(kNetworkStateIdle);
			if ( notify_time_changed_ ) {
				notify_time_changed_ = false;
				if ( client_ != nullptr ) client_->TimeChanged();
			}
		} else if ( ready_state_ == kReadyStateHaveEnoughData && !seeking_ ) {
			/* played up to what has come: the page is to bring more */
			SetReadyState(kReadyStateHaveCurrentData);
		}
	}
	void OnDurationChange() override
	{
		if ( client_ != nullptr ) client_->DurationChanged();
	}
	void OnWaiting( media::WaitingReason reason ) override
	{
		if ( reason == media::WaitingReason::kNoCdm || reason == media::WaitingReason::kNoDecryptionKey ) {
			printf("browser: media: waiting for %s\n",
			       reason == media::WaitingReason::kNoCdm ? "MediaKeys" : "a key");
			if ( encrypted_client_ != nullptr ) {
				encrypted_client_->DidBlockPlaybackWaitingForKey();
				encrypted_client_->DidResumePlaybackBlockedForKey();
			}
		} else if ( reason == media::WaitingReason::kDecoderStateLost ) {
			controller_->OnDecoderStateLost();
		}
	}
	void OnVideoNaturalSizeChange( const gfx::Size &size ) override
	{
		if ( size == natural_size_ ) return;
		printf("browser: media: picture %dx%d\n", size.width(), size.height());
		natural_size_ = size;
		if ( client_ != nullptr ) client_->SizeChanged();
	}
	void OnVideoOpacityChange( bool ) override {}
	void OnVideoAverageKeyframeDistanceUpdate() override {}
	void OnAudioConfigChange( const media::AudioDecoderConfig & ) override {}
	void OnVideoConfigChange( const media::VideoDecoderConfig & ) override {}
	void OnAudioPipelineInfoChange( const media::AudioPipelineInfo &info ) override
	{
		printf("browser: media: sound decoder %s%s\n", media::GetDecoderName(info.decoder_type),
		       info.has_decrypting_demuxer_stream ? " (decrypting)" : "");
	}
	void OnVideoPipelineInfoChange( const media::VideoPipelineInfo &info ) override
	{
		printf("browser: media: video decoder %s%s\n", media::GetDecoderName(info.decoder_type),
		       info.has_decrypting_demuxer_stream ? " (decrypting)" : "");
	}
	void OnVideoFrameRateChange( std::optional<int> ) override {}

private:
	std::unique_ptr<media::Renderer> CreateRenderer( std::optional<media::RendererType> )
	{
		return parts_->renderers->CreateRenderer(media_task_runner_, worker_task_runner_, audio_sink_.get(),
							 video_sink_, media::RequestOverlayInfoCB(), gfx::ColorSpace());
	}

	void SetReadyState( ReadyState s )
	{
		ready_state_ = s;
		if ( client_ != nullptr ) client_->ReadyStateChanged();
	}
	void SetNetworkState( NetworkState s )
	{
		network_state_ = s;
		if ( client_ != nullptr ) client_->NetworkStateChanged();
	}

	void StartPipeline( media::Demuxer *demuxer )
	{
		printf("browser: media %d: pipeline started (%s)\n", id_, mse_ ? "MediaSource" : "resource");
		SetNetworkState(kNetworkStateLoading);
		controller_->Start(media::Pipeline::StartType::kNormal, demuxer, this, false, !mse_);
		if ( parts_->pending_cdm ) AttachCdm();
	}

	void OnStarted( media::PipelineStatus status )
	{
		if ( !status.is_ok() ) {
			OnError(status);
			return;
		}
		started_ = true;
		Report("running");
	}

	/*
	 * The renderer's clock, once the pipeline has started: until its
	 * renderers are made (they wait for the CDM of encrypted media) it
	 * has none, and the time stands where it was put
	 */
	base::TimeDelta MediaTime() const
	{
		return started_ ? controller_->GetMediaTime() : paused_time_;
	}

	void OnSeeked( bool time_updated )
	{
		seeking_ = false;
		seek_time_ = base::TimeDelta();
		if ( paused_ ) paused_time_ = MediaTime();
		if ( time_updated ) notify_time_changed_ = true;
		Report("sought");
	}

	/*
	 * One line on where the player stands: what happened (what), the
	 * time and rate, the element's ready state and the renderers' last
	 * buffering state, the ranges held, the frame shown and its time, the
	 * frames decoded and dropped, and the sound ring
	 */
	void Report( const char *what )
	{
		last_report_ = base::TimeTicks::Now();
		std::string held;
		media::Ranges<base::TimeDelta> r = controller_->GetBufferedTimeRanges();
		for ( size_t i = 0; i < r.size() && i < 3; ++i ) {
			char b[48];
			snprintf(b, sizeof(b), "%s%.3f-%.3f", i ? "," : "", r.start(i).InSecondsF(), r.end(i).InSecondsF());
			held += b;
		}
		if ( r.size() > 3 ) held += ",...";
		if ( held.empty() ) held = "none";
		double frame_at = -1;
		if ( scoped_refptr<media::VideoFrame> f = video_sink_->Frame() ) frame_at = f->timestamp().InSecondsF();
		media::PipelineStatistics st = controller_->GetStatistics();
		long taken = 0, dropped = 0, stalls = 0;
		int ring = 0, device = 0;
		const char *how = "";
		br_snd_state(&taken, &dropped, &ring, &device, &how, &stalls);
		printf("browser: media %d: %s %.3f s%s%s, rate %g, ready %d, %s, pipeline %s; held %s; "
		       "frame %.3f s, %ld shown, %u decoded, %u dropped; sound %s, ring %d, device %d, "
		       "%ld taken, %ld unheard, %ld stalls\n",
		       id_, what, MediaTime().InSecondsF(), paused_ ? " paused" : "", ended_ ? " ended" : "",
		       paused_ ? 0.0 : rate_, (int)ready_state_, buffering_name(buffering_),
		       !controller_->IsPipelineRunning() ? "not running"
		       : controller_->IsPendingSeek() ? "seeking"
		       : controller_->IsSuspended() ? "suspended"
		       : controller_->IsStable() ? "stable" : "changing",
		       held.c_str(), frame_at, frames_changed_, st.video_frames_decoded, st.video_frames_dropped,
		       how, ring, device, taken, dropped, stalls);
	}

	base::TimeDelta DurationTime() const
	{
		double d = Duration();
		return std::isfinite(d) ? base::Seconds(d) : MediaTime();
	}

	/* ------------------------------------------------ a MediaStream */

	/*
	 * A stream as the element's source: its first video track's frames to
	 * the sink, its first audio track's sound to the ring; it has what it
	 * needs once a frame has come (or at once, with no video)
	 */
	void StartStream( const blink::WebMediaStream &web )
	{
		stream_ = true;
		blink::MediaStreamDescriptor *d = web;
		if ( d == nullptr ) {
			SetNetworkState(kNetworkStateFormatError);
			return;
		}
		if ( !d->VideoComponents().empty() ) {
			has_video_ = true;
			stream_video_ = std::make_unique<BrStreamVideo>(blink::WebMediaStreamTrack(d->VideoComponents()[0].Get()),
									video_sink_, &stream_frames_);
		}
		if ( !d->AudioComponents().empty() ) {
			has_audio_ = true;
			/* a peer's sound is WebRTC's to mix and play; any other track's is played here */
			stream_rtc_ = br_rtc_audio_attach(element_ ? element_->GetExecutionContext() : nullptr, web);
			if ( stream_rtc_ != nullptr ) {
				br_rtc_audio_play(stream_rtc_, !paused_, volume_);
			} else {
				stream_audio_ = std::make_unique<BrStreamAudio>(
					blink::WebMediaStreamTrack(d->AudioComponents()[0].Get()));
				stream_audio_->SetPlaying(!paused_, volume_);
			}
		}
		printf("browser: media %d: a stream (%s%s)\n", id_, has_video_ ? "video" : "",
		       has_audio_ ? ( has_video_ ? ", sound" : "sound" ) : "");
		stream_started_ = base::TimeTicks::Now();
		SetNetworkState(kNetworkStateLoading);
		main_task_runner_->PostTask(FROM_HERE, base::BindOnce(&BrWebMediaPlayer::StreamTick, weak_factory_.GetWeakPtr()));
	}

	/* The stream's state told: its size when a frame of another size came, its ready state */
	void StreamTick()
	{
		if ( client_ == nullptr ) return;
		scoped_refptr<media::VideoFrame> f = video_sink_->Frame();
		if ( f && f->natural_size() != natural_size_ ) {
			natural_size_ = f->natural_size();
			client_->SizeChanged();
		}
		if ( ready_state_ < kReadyStateHaveEnoughData && ( !has_video_ || f ) ) {
			SetReadyState(kReadyStateHaveMetadata);
			SetReadyState(kReadyStateHaveEnoughData);
			SetNetworkState(kNetworkStateIdle);
		}
	}

	/* ------------------------------------------------ a MediaSource */

	void StartMediaSource()
	{
		printf("browser: media %d: MediaSource attached\n", id_);
		auto demuxer = std::make_unique<media::ChunkDemuxer>(
			base::BindPostTaskToCurrentDefault(
				base::BindOnce(&BrWebMediaPlayer::OnSourceOpened, weak_factory_.GetWeakPtr())),
			base::BindPostTaskToCurrentDefault(
				base::BindRepeating(&BrWebMediaPlayer::OnProgress, weak_factory_.GetWeakPtr())),
			base::BindPostTaskToCurrentDefault(
				base::BindRepeating(&BrWebMediaPlayer::OnEncrypted, weak_factory_.GetWeakPtr())),
			parts_->media_log.get());
		chunk_demuxer_ = demuxer.get();
		parts_->demuxer = std::move(demuxer);
		StartPipeline(chunk_demuxer_);
	}

	void OnSourceOpened()
	{
		if ( client_ == nullptr ) return;
		client_->MediaSourceOpened(std::make_unique<blink::WebMediaSourceImpl>(chunk_demuxer_));
	}

	void OnProgress() { progressed_ = true; }

	/* The media says it is encrypted: the page's MediaKeys are asked for (the 'encrypted' event) */
	void OnEncrypted( media::EmeInitDataType type, const std::vector<uint8_t> &init_data )
	{
		printf("browser: media: encrypted (%zu bytes of init data)\n", init_data.size());
		if ( encrypted_client_ != nullptr ) encrypted_client_->Encrypted(type, init_data);
	}

	/* ------------------------------------------------ a resource by its address */

	void DoLoad()
	{
		printf("browser: media %s\n", url_.c_str());
		if ( ( url_.compare(0, 5, "http:") == 0 || url_.compare(0, 6, "https:") == 0 )
		  && preload_ != kPreloadAuto ) {
			printf("browser: media fetched when it plays\n");
			deferred_ = true;
			SetNetworkState(kNetworkStateIdle);
			return;
		}
		if ( !StartFetch() ) SetNetworkState(kNetworkStateFormatError);
	}

	void Fetch()
	{
		if ( !deferred_ ) return;
		deferred_ = false;
		SetNetworkState(kNetworkStateLoading);
		if ( !StartFetch() ) SetNetworkState(kNetworkStateFormatError);
	}

	bool StartFetch()
	{
		if ( url_.compare(0, 5, "http:") == 0 || url_.compare(0, 6, "https:") == 0 ) {
			auto *ctx = new base::WeakPtr<BrWebMediaPlayer>(weak_factory_.GetWeakPtr());
			br_net_fetch_set_answer_on_main(1);
			int started = br_net_fetch_start(url_.c_str(), "GET", nullptr, 0, "", &Fetched, ctx);
			br_net_fetch_set_answer_on_main(0);
			if ( started ) return true;
			delete ctx;
		}
		return false;
	}

	static void Fetched( void *ctx, int ok, int status, const char *, const char *, const char *,
			     const char *data, unsigned len )
	{
		std::unique_ptr<base::WeakPtr<BrWebMediaPlayer>> self(static_cast<base::WeakPtr<BrWebMediaPlayer> *>(ctx));
		BrWebMediaPlayer *p = self->get();
		if ( p == nullptr ) return;
		if ( !ok || status < 200 || status >= 300 || len == 0 ) {
			printf("browser: media not fetched (%d, status %d)\n", ok, status);
			p->SetNetworkState(kNetworkStateFormatError);
			return;
		}
		p->StartResource(std::string(data, len));
	}

	/*
	 * The resource in memory, read by FFmpeg's demuxer; an HLS playlist
	 * is read again, with its segments, by the HLS demuxer through data
	 * sources of its own
	 */
	void StartResource( std::string bytes )
	{
		if ( br_is_hls_playlist(bytes) ) {
			auto no_track = base::BindRepeating([]( const media::MediaTrack & ) {});
			std::unique_ptr<media::Demuxer> hls = br_hls_demuxer(
				main_task_runner_, media_task_runner_, GURL(url_), url::Origin::Create(GURL(url_)),
				parts_->media_log.get(),
				base::BindPostTaskToCurrentDefault(
					base::BindRepeating(&BrWebMediaPlayer::OnDemuxerSeek, weak_factory_.GetWeakPtr())),
				std::make_unique<media::ForwardingTrackManager>(
					no_track, no_track,
					base::BindRepeating([]( const media::MediaTrack &, media::MediaTrack::State ) {})));
			if ( hls ) {
				printf("browser: media %d: an HLS presentation\n", id_);
				parts_->demuxer = std::move(hls);
				StartPipeline(parts_->demuxer.get());
				return;
			}
		}
		parts_->data = std::make_unique<media::MemoryDataSource>(std::move(bytes));
		parts_->demuxer = br_ffmpeg_demuxer(
			media_task_runner_, parts_->data.get(),
			base::BindPostTaskToCurrentDefault(
				base::BindRepeating(&BrWebMediaPlayer::OnEncrypted, weak_factory_.GetWeakPtr())),
			parts_->media_log.get());
		StartPipeline(parts_->demuxer.get());
	}

	/* The HLS demuxer asks for a seek (a live presentation moved on): the pipeline seeks there */
	void OnDemuxerSeek( base::TimeDelta t )
	{
		Seek(t.InSecondsF());
	}

	/* ------------------------------------------------ the CDM */

	void SetCdm( blink::WebContentDecryptionModule *cdm )
	{
		parts_->pending_cdm = blink::ToWebContentDecryptionModuleImpl(cdm)->GetCdmContextRef();
		if ( controller_->IsPipelineRunning() ) AttachCdm();
	}

	/*
	 * Given to the pipeline once it runs, as its thread is to call it
	 * (BrCdmOnThread); the context is kept as long as the pipeline may use it
	 */
	void AttachCdm()
	{
		parts_->pending_cdm_on_thread = std::make_unique<BrCdmOnThread>(std::move(parts_->pending_cdm),
										main_task_runner_);
		controller_->SetCdm(parts_->pending_cdm_on_thread.get(),
				    base::BindOnce(&BrWebMediaPlayer::OnCdmAttached, weak_factory_.GetWeakPtr()));
	}

	void OnCdmAttached( bool ok )
	{
		printf("browser: media: MediaKeys %s\n", ok ? "attached" : "not attached");
		if ( ok ) {
			parts_->cdm_on_thread = std::move(parts_->pending_cdm_on_thread);
			if ( cdm_result_ ) cdm_result_->Complete();
		} else {
			parts_->pending_cdm_on_thread.reset();
			if ( cdm_result_ ) {
				cdm_result_->CompleteWithError(blink::kWebContentDecryptionModuleExceptionNotSupportedError,
							       0, "Unable to set ContentDecryptionModule object");
			}
		}
		cdm_result_.reset();
	}


	blink::MediaPlayerClient		*client_;
	blink::HTMLMediaElement			*element_;
	blink::WebMediaPlayerEncryptedMediaClient *encrypted_client_;
	scoped_refptr<base::SingleThreadTaskRunner> main_task_runner_;
	scoped_refptr<base::SingleThreadTaskRunner> media_task_runner_;
	scoped_refptr<base::TaskRunner>		worker_task_runner_;
	std::unique_ptr<BrPipelineParts>	parts_;
	BrVideoSink				*video_sink_ = nullptr;
	scoped_refptr<media::AudioRendererSink>	audio_sink_;
	std::unique_ptr<media::PipelineController> controller_;
	media::ChunkDemuxer			*chunk_demuxer_ = nullptr;
	std::unique_ptr<blink::WebContentDecryptionModuleResult> cdm_result_;
	media::PaintCanvasVideoRenderer		frame_renderer_;
	BrFrameImage				frame_image_;
	BrFrameImage				window_image_;	/* the frame's image in a picture-in-picture window */
	viz::SurfaceId				surface_;	/* the session's name, once a window was asked for */
	std::string				url_;
	std::string				error_;
	Preload					preload_ = kPreloadAuto;
	bool					deferred_ = false;	/* waiting for play to fetch */
	bool					mse_ = false;
	bool					stream_ = false;	/* a MediaStream's, with no pipeline */
	std::unique_ptr<BrStreamVideo>		stream_video_;
	std::unique_ptr<BrStreamAudio>		stream_audio_;
	void					*stream_rtc_ = nullptr;	/* a peer's stream: its proxy (rtc_audio_tessronos.cc) */
	std::atomic<long>			stream_frames_{0};
	base::TimeTicks				stream_started_;	/* when its time was 0 */
	base::TimeDelta				stream_time_;		/* its time when it was paused */
	double					volume_ = 1.0;
	bool					has_audio_ = false, has_video_ = false;
	gfx::Size				natural_size_;
	bool					paused_ = true;
	bool					ended_ = false;
	bool					seeking_ = false;
	bool					notify_time_changed_ = false;
	bool					progressed_ = false;
	bool					started_ = false;
	double					rate_ = 1.0;
	int					id_;
	media::BufferingState			buffering_ = media::BUFFERING_HAVE_NOTHING;
	long					frames_changed_ = 0;	/* frames the sink gave, each once */
	base::TimeTicks				presented_at_;		/* when the last of them was */
	base::TimeDelta				frame_gap_;		/* the time between them, averaged */
	bool					frame_wanted_ = false;	/* requestVideoFrameCallback waits */
	base::TimeTicks				last_report_;
	base::TimeDelta				paused_time_;
	base::TimeDelta				seek_time_;
	NetworkState				network_state_ = kNetworkStateEmpty;
	ReadyState				ready_state_ = kReadyStateHaveNothing;
	base::WeakPtrFactory<BrWebMediaPlayer>	weak_factory_{this};
};

void BrWebMediaPlayer::Paint( cc::PaintCanvas *canvas, const gfx::Rect &rect, const cc::PaintFlags &flags, bool )
{
	if ( canvas == nullptr ) return;
	scoped_refptr<media::VideoFrame> frame = video_sink_->Frame();
	if ( !frame ) return;
	PaintFrame(frame_renderer_, frame_image_, std::move(frame), canvas, rect, flags);
}

}  // namespace

/*
 * The player of a media element (the frame client's CreateWebMediaPlayer),
 * with the element's encrypted media (its 'encrypted' event, and the
 * MediaKeys it may already have)
 */
std::unique_ptr<blink::WebMediaPlayer> br_media_player( blink::HTMLMediaElement &element,
							blink::WebMediaPlayerClient *client )
{
	blink::HTMLMediaElementEncryptedMedia &eme = blink::HTMLMediaElementEncryptedMedia::From(element);
	return std::make_unique<BrWebMediaPlayer>(static_cast<blink::MediaPlayerClient *>(client), &element, &eme,
						  eme.ContentDecryptionModule(),
						  element.GetDocument().GetTaskRunner(
							  blink::TaskType::kMediaElementEvent));
}

/*
 * Whether a picture of this size and rate is decoded in time: it is
 * decoded on the processor, and up to 854x480 at 30 frames a second is
 * what it keeps up with (MediaCapabilities' smooth, media_capabilities.patch)
 */
extern "C" int br_media_decodes_in_time( int width, int height, double framerate )
{
	if ( width > 0 && height > 0 && (long)width * height > 854L * 480L ) return 0;
	return framerate <= 31.0;
}

/*
 * Whether a MediaSource's stream of the size and rate a type's parameters
 * say (width, height, framerate) is decoded in time. An adaptive player
 * that asks with these parameters (isTypeSupported) is told the larger
 * ones are not played (media_source.patch).
 */
extern "C" int br_media_source_size_ok( const char *type )
{
	long w = 0, h = 0;
	double rate = 0;
	for ( const char *p = type; p != nullptr && ( p = strchr(p, ';') ) != nullptr; ) {
		++p;
		while ( *p == ' ' ) ++p;
		if ( strncmp(p, "width=", 6) == 0 ) w = strtol(p + 6, nullptr, 10);
		else if ( strncmp(p, "height=", 7) == 0 ) h = strtol(p + 7, nullptr, 10);
		else if ( strncmp(p, "framerate=", 10) == 0 ) rate = strtod(p + 10, nullptr);
	}
	return br_media_decodes_in_time((int)w, (int)h, rate);
}

/*
 * The frames painted since the start: the frames shown (each once,
 * however often it was painted), those whose YUV planes went to the GPU,
 * and those made RGB on the processor
 */
void br_media_frames( long *shown, long *planes, long *rgb )
{
	*shown = g_frames_shown.load();
	*planes = g_frames_planes.load();
	*rgb = g_frames_rgb.load();
}

/* Every player's turn of the loop */
void br_media_tick()
{
	for ( blink::wtf_size_t i = 0; i < players().size(); ++i ) players()[i]->Tick();
}

/* Whether a player is to be looked at often: it plays, or its frames come as time goes */
bool br_media_wants_ticks()
{
	for ( blink::wtf_size_t i = 0; i < players().size(); ++i ) {
		if ( players()[i]->WantsTicks() ) return true;
	}
	return false;
}


/* The player of a number (GetPlayerId), or null */
static BrWebMediaPlayer *player_of( int id )
{
	for ( blink::wtf_size_t i = 0; i < players().size(); ++i ) {
		if ( players()[i]->id() == id ) return players()[i];
	}
	return nullptr;
}

/* A player's frame now, in a window of its own (a video's picture-in-picture) */
bool br_media_paint_player( int id, SkCanvas *c, int w, int h )
{
	BrWebMediaPlayer *p = player_of(id);
	return p != nullptr && p->PaintTo(c, w, h);
}

bool br_media_player_paused( int id )
{
	BrWebMediaPlayer *p = player_of(id);
	return p == nullptr || p->Paused();
}

/* A count that changes with each frame the player shows, -1 when there is no such player */
long br_media_player_frames( int id )
{
	BrWebMediaPlayer *p = player_of(id);
	return p != nullptr ? p->frames_changed() : -1;
}
