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
 *	ブラウザ: the player behind <video> and <audio> (design 17.19).
 *
 *	The whole resource is fetched through the network stack, then
 *	demuxed and decoded in memory (media_decode.c). The player's clock
 *	runs on real time from where playing began; a paint asks the decoder
 *	for the frame of the clock's time, converts it to the element's size
 *	(libyuv) and draws it, with a small bar of controls when the element
 *	shows controls. The sound is pulled into the feeder's ring
 *	(br_snd.cc) on every turn of the application's loop, and the picture
 *	is held back by what waits there, so that the two agree.
 *
 *	The element's load algorithm must not be re-entered: the load, and
 *	the readiness it reports, run from tasks of their own.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "cc/paint/paint_canvas.h"
#include "cc/paint/paint_flags.h"
#include "cc/paint/paint_image_builder.h"
#include "third_party/blink/public/platform/web_media_player.h"
#include "third_party/blink/public/platform/web_media_player_source.h"
#include "third_party/blink/public/platform/web_time_range.h"
#include "third_party/blink/renderer/core/html/media/html_media_element.h"
#include "third_party/blink/renderer/core/layout/layout_object.h"
#include "third_party/blink/renderer/platform/media/media_player_client.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"
#include "third_party/libyuv/include/libyuv.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkPathBuilder.h"

#include "../net/br_net.h"

extern "C" {
int br_media_open(const void *data, unsigned len);
int br_media_info(int id, int *w, int *h, double *duration);
int br_media_frame_at(int id, double t, const unsigned char **y, const unsigned char **u,
		      const unsigned char **v, int *ystride, int *uvstride);
int br_media_has_audio(int id);
int br_media_has_video(int id);
int br_media_pull_audio(int id, short *out, int max_frames);
void br_media_close(int id);
int br_snd_start(void);
int br_snd_write(const short *frames, int nframes);
int br_snd_latency_ms(void);
int br_snd_buffered(void);
}

namespace {

class BrWebMediaPlayer;

blink::Vector<BrWebMediaPlayer *> &players()
{
	static auto *p = new blink::Vector<BrWebMediaPlayer *>();
	return *p;
}

class BrWebMediaPlayer final : public blink::WebMediaPlayer {
public:
	BrWebMediaPlayer( blink::MediaPlayerClient *client, blink::HTMLMediaElement *element,
			  scoped_refptr<base::SingleThreadTaskRunner> task_runner )
		: client_(client), element_(element), task_runner_(std::move(task_runner))
	{
		players().push_back(this);
	}
	~BrWebMediaPlayer() override
	{
		blink::wtf_size_t pos = players().Find(this);
		if ( pos != blink::kNotFound ) players().EraseAt(pos);
		if ( media_id_ >= 0 ) br_media_close(media_id_);
		free(bytes_);
	}

	LoadTiming Load( LoadType, const blink::WebMediaPlayerSource &source, CorsMode, bool ) override
	{
		if ( source.IsURL() ) url_ = blink::KURL(source.GetAsURL()).GetString().Utf8();
		if ( task_runner_ ) {
			task_runner_->PostTask(FROM_HERE, base::BindOnce(&BrWebMediaPlayer::DoLoad,
									 weak_factory_.GetWeakPtr()));
		}
		return LoadTiming::kImmediate;
	}

	/*
	 * From the application's loop: the clock moves between paints, so a
	 * playing element is painted again, reaching the end ends it, and
	 * the sound ring is kept ahead of the device
	 */
	void Tick()
	{
		/* a player waiting for play: the element was told to play (a script, or its controls) */
		if ( deferred_ && element_ != nullptr && !element_->paused() ) Fetch();
		if ( media_id_ < 0 || !playing_ ) return;
		if ( has_audio_ && !snd_open_ ) snd_open_ = br_snd_start() == 0;
		if ( std::isfinite(duration_) && duration_ > 0 && CurrentTime() >= duration_ ) {
			printf("browser: media ended at %d ms\n", (int)( duration_ * 1000 ));
			current_ = duration_;
			playing_ = false;
			client_->TimeChanged();
			return;
		}
		TopUpAudio();
		if ( has_video_ ) client_->Repaint();
	}

	void TopUpAudio()
	{
		if ( !has_audio_ || !snd_open_ || media_id_ < 0 || !playing_ ) return;
		for ( int i = 0; i < 16 && br_snd_buffered() + 4096 < 24000; ++i ) {
			short buf[4096 * 2];
			int n = br_media_pull_audio(media_id_, buf, 4096);
			if ( n <= 0 ) break;
			if ( volume_ < 0.999 ) {
				int scale = (int)( volume_ * 256.0 );
				for ( int k = 0; k < n * 2; ++k ) buf[k] = (short)( ( buf[k] * scale ) >> 8 );
			}
			br_snd_write(buf, n);
		}
	}

	void Play() override
	{
		if ( media_id_ < 0 ) return;
		if ( std::isfinite(duration_) && duration_ > 0 && current_ >= duration_ ) current_ = 0;
		playing_ = true;
		reported_ = current_;
		base_ = base::TimeTicks::Now() - base::Seconds(current_);
		if ( has_audio_ && !snd_open_ ) snd_open_ = br_snd_start() == 0;
	}
	/*
	 * The element's preload: "none" and "metadata" (the default) leave
	 * the resource unfetched until the element plays, since the player
	 * can only take all of it at once; "auto" (or autoplay) takes it now.
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
	void Pause( PauseReason ) override
	{
		current_ = CurrentTime();
		playing_ = false;
	}
	void Seek( double seconds ) override
	{
		reported_ = seconds;
		current_ = seconds;
		if ( playing_ ) base_ = base::TimeTicks::Now() - base::Seconds(current_);
		if ( client_ != nullptr ) {
			client_->TimeChanged();
			client_->Repaint();
		}
	}
	void SetRate( double ) override {}
	void SetVolume( double volume ) override { volume_ = volume; }
	void SetLatencyHint( double ) override {}
	void OnRequestPictureInPicture() override {}
	blink::WebTimeRanges Buffered() const override
	{
		blink::WebTimeRanges ranges;
		if ( duration_ > 0 ) ranges.Add(0.0, duration_);
		return ranges;
	}
	blink::WebTimeRanges Seekable() const override { return Buffered(); }
	bool HasVideo() const override { return has_video_; }
	bool IsEnded() const override
	{
		return std::isfinite(duration_) && duration_ > 0 && current_ >= duration_;
	}
	void SetPreservesPitch( bool ) override {}
	void SetWasPlayedWithUserActivationAndHighMediaEngagement( bool ) override {}
	void SetVolumeMultiplier( double ) override {}
	void SuspendForFrameClosed() override {}
	bool HasAudio() const override { return has_audio_; }
	gfx::Size NaturalSize() const override { return gfx::Size(width_, height_); }
	gfx::Size VisibleSize() const override { return gfx::Size(width_, height_); }
	bool Paused() const override { return !playing_; }
	bool Seeking() const override { return false; }
	double Duration() const override { return duration_; }
	double CurrentTime() const override
	{
		if ( !playing_ ) return current_;
		double t = ( base::TimeTicks::Now() - base_ ).InSecondsF();
		/* what is written now is heard later: the picture is held back as much */
		if ( has_audio_ && snd_open_ ) {
			t -= br_snd_latency_ms() / 1000.0;
			if ( t < 0 ) t = 0;
		}
		if ( std::isfinite(duration_) && duration_ > 0 && t > duration_ ) t = duration_;
		if ( t < reported_ ) t = reported_;
		reported_ = t;
		return t;
	}
	NetworkState GetNetworkState() const override { return network_state_; }
	ReadyState GetReadyState() const override { return ready_state_; }
	blink::WebString GetErrorMessage() const override
	{
		return blink::WebString::FromUtf8("unsupported media format");
	}
	bool DidLoadingProgress() override { return media_id_ >= 0; }
	bool WouldTaintOrigin() const override { return false; }
	double MediaTimeForTimeValue( double t ) const override { return t; }
	unsigned DecodedFrameCount() const override { return decoded_; }
	unsigned DroppedFrameCount() const override { return 0; }
	uint64_t AudioDecodedByteCount() const override { return 0; }
	uint64_t VideoDecodedByteCount() const override { return 0; }
	bool HasAvailableVideoFrame() const override { return has_video_; }
	bool HasReadableVideoFrame() const override { return has_video_; }
	void OnFrozen() override {}
	std::optional<media::VideoFrame::ID> CurrentFrameId() const override { return std::nullopt; }
	void Paint( cc::PaintCanvas *canvas, const gfx::Rect &rect, const cc::PaintFlags &flags,
		    bool ) override;
	void Shutdown() override {}
	bool IsVideoBeingCaptured() const override { return false; }
	/* frames are decoded into the player's own buffers and drawn from there */
	scoped_refptr<media::VideoFrame> GetCurrentFrameThenUpdate() override { return nullptr; }
	base::WeakPtr<blink::WebMediaPlayer> AsWeakPtr() override { return weak_factory_.GetWeakPtr(); }

	/* A press on the bar of controls drawn last: play or pause, or a place to go to */
	bool HandleClick( int x, int y )
	{
		if ( deferred_ && element_ != nullptr && element_->GetLayoutObject() != nullptr ) {
			/* not yet fetched: a press anywhere on the element plays it */
			gfx::Rect abs = element_->GetLayoutObject()->AbsoluteBoundingBoxRect();
			if ( !abs.Contains(x, y) ) return false;
			element_->Play();
			Fetch();
			return true;
		}
		if ( bar_.isEmpty() || !has_video_ || element_ == nullptr ) return false;
		if ( x < bar_.left() || x >= bar_.right() || y < bar_.top() || y >= bar_.bottom() ) return false;
		if ( x < bar_.left() + kCtlButtonWidth ) {
			if ( element_->paused() ) {
				element_->Play();
			} else {
				element_->pause();
			}
		} else if ( std::isfinite(duration_) && duration_ > 0 && track_w_ > 0 ) {
			double frac = ( x - track_x_ ) / (double)track_w_;
			frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
			element_->setCurrentTime(frac * duration_);
		}
		if ( client_ != nullptr ) client_->Repaint();
		return true;
	}

private:
	void DoLoad()
	{
		printf("browser: media %s\n", url_.c_str());
		if ( ( url_.compare(0, 5, "http:") == 0 || url_.compare(0, 6, "https:") == 0 )
		  && preload_ != kPreloadAuto ) {
			printf("browser: media fetched when it plays\n");
			deferred_ = true;
			network_state_ = kNetworkStateIdle;
			if ( client_ != nullptr ) client_->NetworkStateChanged();
			return;
		}
		if ( !StartFetch() ) ReportFormatError();
	}

	void Fetch()
	{
		if ( !deferred_ ) return;
		deferred_ = false;
		network_state_ = kNetworkStateLoading;
		if ( client_ != nullptr ) client_->NetworkStateChanged();
		if ( !StartFetch() ) ReportFormatError();
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
			p->ReportFormatError();
			return;
		}
		p->bytes_ = static_cast<char *>(malloc(len));
		if ( p->bytes_ == nullptr ) {
			p->ReportFormatError();
			return;
		}
		memcpy(p->bytes_, data, len);
		p->bytes_len_ = len;
		p->OpenMedia();
	}

	void OpenMedia()
	{
		media_id_ = br_media_open(bytes_, bytes_len_);
		if ( media_id_ < 0 ) {
			free(bytes_);
			bytes_ = nullptr;
			ReportFormatError();
			return;
		}
		br_media_info(media_id_, &width_, &height_, &duration_);
		has_audio_ = br_media_has_audio(media_id_) != 0;
		has_video_ = br_media_has_video(media_id_) != 0;
		printf("browser: media %dx%d, %d ms%s%s\n", width_, height_, (int)( duration_ * 1000 ),
		       has_video_ ? ", video" : "", has_audio_ ? ", sound" : "");
		network_state_ = kNetworkStateLoaded;
		ready_state_ = kReadyStateHaveEnoughData;
		if ( client_ != nullptr ) {
			client_->NetworkStateChanged();
			client_->DurationChanged();
			client_->SizeChanged();
			client_->ReadyStateChanged();
			client_->Repaint();
		}
	}

	void ReportFormatError()
	{
		network_state_ = kNetworkStateFormatError;
		if ( client_ != nullptr ) client_->NetworkStateChanged();
	}

	bool ConvertFrame( const unsigned char *y, const unsigned char *u, const unsigned char *v,
			   int ys, int uvs, int dw, int dh );
	void PaintImage( cc::PaintCanvas *canvas, const gfx::Rect &rect, const cc::PaintFlags &flags );
	void PaintControls( cc::PaintCanvas *canvas, const SkRect &local, const SkRect &dev );

	/*
	 * The image drawn for a frame shares rgb_'s pixels rather than copying
	 * them: a drawing is kept until a collection frees it, and a copy for
	 * every paint piles up
	 */
	sk_sp<SkImage> SharedFrameImage()
	{
		if ( !frame_sk_ || frame_sk_pixels_ != rgb_.getPixels() ) {
			rgb_.setImmutable();
			frame_sk_ = SkImages::RasterFromBitmap(rgb_);
			frame_sk_pixels_ = rgb_.getPixels();
		}
		return frame_sk_;
	}

	static constexpr SkScalar kCtlBarHeight = 24;
	static constexpr SkScalar kCtlButtonWidth = 26;

	blink::MediaPlayerClient		*client_;
	blink::HTMLMediaElement			*element_;
	scoped_refptr<base::SingleThreadTaskRunner> task_runner_;
	std::string				url_;
	char					*bytes_ = nullptr;
	unsigned				bytes_len_ = 0;
	int					media_id_ = -1;
	Preload					preload_ = kPreloadAuto;
	bool					deferred_ = false;	/* waiting for play to fetch */
	int					width_ = 0, height_ = 0;
	double					duration_ = 0;
	double					current_ = 0;
	bool					playing_ = false;
	bool					has_audio_ = false, has_video_ = false;
	bool					snd_open_ = false;
	double					volume_ = 1.0;
	mutable base::TimeTicks			base_;
	unsigned				decoded_ = 0;
	SkBitmap				rgb_;
	std::vector<uint8_t>			scaled_y_, scaled_u_, scaled_v_;
	SkRect					bar_ = SkRect::MakeEmpty();
	SkScalar				track_x_ = 0, track_w_ = 0;
	sk_sp<SkImage>				frame_sk_;
	const void				*frame_sk_pixels_ = nullptr;
	const cc::PaintImage::Id		frame_image_id_ = cc::PaintImage::GetNextId();
	mutable double				reported_ = 0;	/* the clock does not go back */
	NetworkState				network_state_ = kNetworkStateEmpty;
	ReadyState				ready_state_ = kReadyStateHaveNothing;
	base::WeakPtrFactory<BrWebMediaPlayer>	weak_factory_{this};
};

void BrWebMediaPlayer::Paint( cc::PaintCanvas *canvas, const gfx::Rect &rect, const cc::PaintFlags &flags,
			      bool )
{
	if ( canvas == nullptr || media_id_ < 0 || !has_video_ ) return;
	const unsigned char *y, *u, *v;
	int ys, uvs;
	if ( !br_media_frame_at(media_id_, CurrentTime(), &y, &u, &v, &ys, &uvs) ) return;
	++decoded_;
	if ( !ConvertFrame(y, u, v, ys, uvs, rect.width(), rect.height()) ) return;
	PaintImage(canvas, rect, flags);

	/*
	 * The controls in the element's box, in the page's coordinates for
	 * the press; drawn relative to the paint's rectangle
	 */
	const SkRect local = SkRect::MakeXYWH(rect.x(), rect.y(), rect.width(), rect.height());
	SkRect dev = local;
	if ( element_ != nullptr && element_->GetLayoutObject() != nullptr ) {
		gfx::Rect abs = element_->GetLayoutObject()->AbsoluteBoundingBoxRect();
		dev = SkRect::MakeXYWH(abs.x(), abs.y(), abs.width(), abs.height());
	}
	if ( element_ != nullptr && element_->ShouldShowControls() ) {
		PaintControls(canvas, local, dev);
	} else {
		bar_.setEmpty();
	}
}

/* The frame's planes into rgb_ at the size shown (BT.601), scaled first in its planes when that differs */
bool BrWebMediaPlayer::ConvertFrame( const unsigned char *y, const unsigned char *u, const unsigned char *v,
				     int ys, int uvs, int dw, int dh )
{
	if ( dw <= 0 || dh <= 0 ) {
		dw = width_;
		dh = height_;
	}
	if ( rgb_.width() != dw || rgb_.height() != dh ) {
		if ( !rgb_.tryAllocPixels(SkImageInfo::MakeN32(dw, dh, kOpaque_SkAlphaType)) ) return false;
	}
	if ( dw == width_ && dh == height_ ) {
		return libyuv::I420ToARGB(y, ys, u, uvs, v, uvs, static_cast<uint8_t *>(rgb_.getPixels()),
					  static_cast<int>(rgb_.rowBytes()), dw, dh) == 0;
	}
	const int cw = ( dw + 1 ) / 2, ch = ( dh + 1 ) / 2;
	scaled_y_.resize((size_t)dw * dh);
	scaled_u_.resize((size_t)cw * ch);
	scaled_v_.resize((size_t)cw * ch);
	if ( libyuv::I420Scale(y, ys, u, uvs, v, uvs, width_, height_, scaled_y_.data(), dw,
			       scaled_u_.data(), cw, scaled_v_.data(), cw, dw, dh,
			       libyuv::kFilterBilinear) != 0 ) {
		return false;
	}
	return libyuv::I420ToARGB(scaled_y_.data(), dw, scaled_u_.data(), cw, scaled_v_.data(), cw,
				  static_cast<uint8_t *>(rgb_.getPixels()), static_cast<int>(rgb_.rowBytes()),
				  dw, dh) == 0;
}

void BrWebMediaPlayer::PaintImage( cc::PaintCanvas *canvas, const gfx::Rect &rect, const cc::PaintFlags &flags )
{
	cc::PaintImage img = cc::PaintImageBuilder::WithDefault()
				     .set_id(frame_image_id_)
				     .set_image(SharedFrameImage(), cc::PaintImage::GetNextContentId())
				     .TakePaintImage();
	canvas->drawImageRect(img, SkRect::MakeWH(rgb_.width(), rgb_.height()),
			      SkRect::MakeXYWH(rect.x(), rect.y(), rect.width(), rect.height()),
			      SkSamplingOptions(), &flags, SkCanvas::kFast_SrcRectConstraint);
}

/* A strip of controls at the bottom: play or pause at the left, the place in the time across the rest */
void BrWebMediaPlayer::PaintControls( cc::PaintCanvas *canvas, const SkRect &local, const SkRect &dev )
{
	if ( dev.height() < kCtlBarHeight * 2 || dev.width() < 80 ) {
		bar_.setEmpty();
		return;
	}
	bar_ = SkRect::MakeLTRB(dev.left(), dev.bottom() - kCtlBarHeight, dev.right(), dev.bottom());
	canvas->save();
	canvas->translate(local.left() - dev.left(), local.bottom() - dev.bottom());

	cc::PaintFlags f;
	f.setAntiAlias(true);
	f.setStyle(cc::PaintFlags::kFill_Style);
	f.setColor(SkColorSetARGB(150, 16, 16, 16));
	canvas->drawRect(bar_, f);
	f.setColor(SK_ColorWHITE);
	SkScalar bx = bar_.left() + 7, by = bar_.top() + 5, bh = kCtlBarHeight - 10;
	if ( playing_ ) {
		canvas->drawRect(SkRect::MakeXYWH(bx, by, 4, bh), f);
		canvas->drawRect(SkRect::MakeXYWH(bx + 8, by, 4, bh), f);
	} else {
		SkPathBuilder tri;
		tri.moveTo(bx, by);
		tri.lineTo(bx + 12, by + bh / 2);
		tri.lineTo(bx, by + bh);
		tri.close();
		canvas->drawPath(tri.detach(), f);
	}
	track_x_ = bar_.left() + kCtlButtonWidth + 4;
	track_w_ = bar_.right() - 8 - track_x_;
	if ( track_w_ > 10 ) {
		SkScalar cy = bar_.centerY();
		f.setColor(SkColorSetARGB(180, 96, 96, 96));
		canvas->drawRect(SkRect::MakeLTRB(track_x_, cy - 2, track_x_ + track_w_, cy + 2), f);
		if ( duration_ > 0 ) {
			SkScalar frac = (SkScalar)( CurrentTime() / duration_ );
			frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
			f.setColor(SkColorSetARGB(235, 240, 240, 240));
			canvas->drawRect(SkRect::MakeLTRB(track_x_, cy - 2, track_x_ + track_w_ * frac, cy + 2), f);
		}
	} else {
		track_w_ = 0;
	}
	canvas->restore();
}

}  // namespace

/* The player of a media element (the frame client's CreateWebMediaPlayer) */
std::unique_ptr<blink::WebMediaPlayer> br_media_player( blink::HTMLMediaElement &element,
							blink::WebMediaPlayerClient *client )
{
	return std::make_unique<BrWebMediaPlayer>(static_cast<blink::MediaPlayerClient *>(client), &element,
						  element.GetDocument().GetTaskRunner(
							  blink::TaskType::kMediaElementEvent));
}

/* Every player's turn of the loop */
void br_media_tick()
{
	for ( blink::wtf_size_t i = 0; i < players().size(); ++i ) players()[i]->Tick();
}

/* A press at a point of the page on a player's controls; false for none */
bool br_media_click( int x, int y )
{
	for ( blink::wtf_size_t i = 0; i < players().size(); ++i ) {
		if ( players()[i]->HandleClick(x, y) ) return true;
	}
	return false;
}
