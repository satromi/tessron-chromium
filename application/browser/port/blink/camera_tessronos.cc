/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	camera_tessronos.cc
 *	ブラウザ: the camera of getUserMedia's video (design 14.4.11): the
 *	video capture host Blink's VideoCaptureImpl speaks to.
 *
 *	The camera streams in the mode nearest the size the page's track asks
 *	for (port/media/br_cam.cc). A reader thread takes each new frame from
 *	the device and turns it into I420 (YUY2, UYVY and NV12 with libyuv;
 *	MJPEG decoded by Skia's JPEG codec, then libyuv); on the services
 *	thread the picture goes into one of the shared memory buffers each
 *	client was told of, and each client that is not paused is told it is
 *	ready. A buffer is used again once every client it went to has given
 *	it back. Every client of the host shares the one stream; the last to
 *	stop stops the camera.
 *
 *	The session a stream device was opened with (br_camera_session, from
 *	MediaStreamDispatcherHost) says which camera a client starts. A
 *	client of a display stream device's session is the screen's capture
 *	(screen_tessronos.cc): the host the frames' broker binds hands each
 *	client to the one of its session (Hosts).
 *
 *	A camera's track has an ImageCapture, which asks the state of its
 *	photos before the track counts as made: the camera has no controls
 *	of its own (each range only its value, the size its mode's), options
 *	set change nothing, and a photo is the newest picture as JPEG.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/memory/unsafe_shared_memory_region.h"
#include "base/time/time.h"
#include "base/unguessable_token.h"
#include "media/base/video_frame.h"
#include "media/base/video_frame_metadata.h"
#include "media/capture/mojom/video_capture.mojom-blink.h"
#include "media/capture/mojom/video_capture_buffer.mojom-blink.h"
#include "media/capture/mojom/image_capture.mojom-blink.h"
#include "third_party/skia/include/core/SkPixmap.h"
#include "third_party/skia/include/core/SkStream.h"
#include "third_party/skia/include/encode/SkJpegEncoder.h"
#include "media/capture/video_capture_types.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"
#include "third_party/libyuv/include/libyuv/convert.h"
#include "third_party/skia/include/codec/SkCodec.h"
#include "third_party/skia/include/codec/SkJpegDecoder.h"
#include "third_party/skia/include/core/SkData.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "ui/gfx/color_space.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"

#include "../media/br_cam.h"

void br_on_service_thread( base::OnceClosure task );

/* The capture of the screen (screen_tessronos.cc) */
bool br_screen_session_is( const base::UnguessableToken &session );
media::mojom::blink::VideoCaptureHost *br_screen_host();

namespace {

namespace mm = media::mojom::blink;

constexpr int kBuffers = 4;		/* the shared buffers of the stream */
constexpr int kPollMs = 8;		/* the reader looks for a new frame this often */
constexpr int kInFlight = 2;		/* pictures on their way to the services thread at most */

std::map<base::UnguessableToken, int>	g_sessions;	/* a stream device's session: its camera */

/* The mode nearest the size asked for: the nearest area, uncompressed before MJPEG when it is fast enough */
int pick_mode( int cam, const media::VideoCaptureParams &params, BrCamMode *out )
{
	static BrCamMode modes[BR_CAM_MODES];
	int n = br_cam_modes(cam, modes, BR_CAM_MODES);
	gfx::Size want = params.requested_format.frame_size;
	if ( want.IsEmpty() ) want = gfx::Size(640, 480);
	float fps = params.requested_format.frame_rate > 0 ? params.requested_format.frame_rate : 30;
	long area = (long)want.width() * want.height();
	int best = -1;
	long best_d = 0;
	bool best_ok = false;

	for ( int i = 0; i < n; i++ ) {
		long d = (long)modes[i].width * modes[i].height - area;
		if ( d < 0 ) d = -d;
		bool ok = modes[i].fastest > 0 && 1e7 / modes[i].fastest + 0.5 >= fps;
		if ( best < 0 || d < best_d || ( d == best_d && ok && !best_ok )
		     || ( d == best_d && ok == best_ok && modes[best].format == BR_CAM_MJPEG
			  && modes[i].format != BR_CAM_MJPEG ) ) {
			best = i;
			best_d = d;
			best_ok = ok;
		}
	}
	if ( best >= 0 ) *out = modes[best];
	return best;
}

/* A frame of the camera as I420, w x h: false when it cannot be read */
bool to_i420( const BrCamFrame &h, const uint8_t *src, int len, std::vector<uint8_t> &dst )
{
	int w = h.width, ht = h.height;
	int cw = ( w + 1 ) / 2, ch = ( ht + 1 ) / 2;
	dst.resize((size_t)w * ht + 2 * (size_t)cw * ch);
	uint8_t *y = dst.data(), *u = y + (size_t)w * ht, *v = u + (size_t)cw * ch;

	switch ( h.format ) {
	case BR_CAM_YUY2:
		if ( len < w * ht * 2 ) return false;
		return libyuv::YUY2ToI420(src, w * 2, y, w, u, cw, v, cw, w, ht) == 0;
	case BR_CAM_UYVY:
		if ( len < w * ht * 2 ) return false;
		return libyuv::UYVYToI420(src, w * 2, y, w, u, cw, v, cw, w, ht) == 0;
	case BR_CAM_NV12:
		if ( len < w * ht + 2 * cw * ch ) return false;
		return libyuv::NV12ToI420(src, w, src + w * ht, cw * 2, y, w, u, cw, v, cw, w, ht) == 0;
	case BR_CAM_MJPEG: {
		SkCodec::Result r;
		std::unique_ptr<SkCodec> codec = SkJpegDecoder::Decode(SkData::MakeWithoutCopy(src, (size_t)len), &r);
		if ( !codec ) return false;
		SkImageInfo info = SkImageInfo::Make(w, ht, kRGBA_8888_SkColorType, kOpaque_SkAlphaType);
		if ( codec->dimensions() != info.dimensions() ) return false;
		std::vector<uint8_t> rgba((size_t)w * ht * 4);
		r = codec->getPixels(info, rgba.data(), (size_t)w * 4);
		if ( r != SkCodec::kSuccess && r != SkCodec::kIncompleteInput ) return false;
		/* libyuv's ABGR is R, G, B, A in memory */
		return libyuv::ABGRToI420(rgba.data(), w * 4, y, w, u, cw, v, cw, w, ht) == 0;
	}
	default:
		return false;
	}
}

class Camera final : public mm::VideoCaptureHost {
public:
	void Bind( mojo::PendingReceiver<mm::VideoCaptureHost> r ) { receivers_.Add(this, std::move(r)); }

	void Start( const base::UnguessableToken &device_id, const base::UnguessableToken &session_id,
		    const media::VideoCaptureParams &params, mojo::PendingRemote<mm::VideoCaptureObserver> o ) override
	{
		auto it = g_sessions.find(session_id);
		int cam = it != g_sessions.end() ? it->second : 0;
		Client &c = clients_[device_id];
		c.observer.reset();
		c.observer.Bind(std::move(o));
		c.paused = false;
		c.held.clear();
		if ( !running_ ) {
			BrCamMode m;
			int mode = pick_mode(cam, params, &m);
			if ( mode < 0 || br_cam_open(cam, mode, 0) < 0 ) {
				printf("browser: camera: no mode to stream\n");
				c.observer->OnStateChanged(mm::VideoCaptureResult::NewErrorCode(
					media::VideoCaptureError::kVideoCaptureControllerInvalidOrUnsupportedVideoCaptureParametersRequested));
				clients_.erase(device_id);
				return;
			}
			size_ = gfx::Size(m.width, m.height);
			fps_ = m.interval > 0 ? 1e7f / m.interval : 30;
			MakeBuffers();
			StartReader();
			printf("browser: camera: %dx%d, %s\n", m.width, m.height,
			       m.format == BR_CAM_MJPEG ? "MJPEG" : m.format == BR_CAM_NV12 ? "NV12" : "YUV 4:2:2");
		}
		for ( int i = 0; i < kBuffers; i++ ) {
			c.observer->OnNewBuffer(i, mm::VideoBufferHandle::NewUnsafeShmemRegion(buffers_[i].region.Duplicate()));
		}
		c.observer->OnStateChanged(mm::VideoCaptureResult::NewState(mm::VideoCaptureState::STARTED));
	}

	void Stop( const base::UnguessableToken &device_id ) override
	{
		auto it = clients_.find(device_id);
		if ( it == clients_.end() ) return;
		for ( int b : it->second.held ) buffers_[b].holders--;
		if ( it->second.observer ) {
			it->second.observer->OnStateChanged(mm::VideoCaptureResult::NewState(mm::VideoCaptureState::STOPPED));
		}
		clients_.erase(it);
		if ( clients_.empty() ) StopAll();
	}

	void Pause( const base::UnguessableToken &device_id ) override
	{
		auto it = clients_.find(device_id);
		if ( it == clients_.end() ) return;
		it->second.paused = true;
		it->second.observer->OnStateChanged(mm::VideoCaptureResult::NewState(mm::VideoCaptureState::PAUSED));
	}

	void Resume( const base::UnguessableToken &device_id, const base::UnguessableToken &,
		     const media::VideoCaptureParams & ) override
	{
		auto it = clients_.find(device_id);
		if ( it == clients_.end() ) return;
		it->second.paused = false;
		it->second.observer->OnStateChanged(mm::VideoCaptureResult::NewState(mm::VideoCaptureState::RESUMED));
	}

	void RequestRefreshFrame( const base::UnguessableToken & ) override
	{
		if ( !last_.empty() ) Deliver(last_, false);
	}

	void ReleaseBuffer( const base::UnguessableToken &device_id, int32_t buffer_id,
			    const media::VideoCaptureFeedback & ) override
	{
		auto it = clients_.find(device_id);
		if ( it == clients_.end() || buffer_id < 0 || buffer_id >= kBuffers ) return;
		auto h = it->second.held.find(buffer_id);
		if ( h == it->second.held.end() ) return;
		it->second.held.erase(h);
		buffers_[buffer_id].holders--;
	}

	void GetDeviceSupportedFormats( const base::UnguessableToken &, const base::UnguessableToken &session_id,
					GetDeviceSupportedFormatsCallback cb ) override
	{
		auto it = g_sessions.find(session_id);
		std::move(cb).Run(br_camera_formats(it != g_sessions.end() ? it->second : 0));
	}

	void GetDeviceFormatsInUse( const base::UnguessableToken &, const base::UnguessableToken &,
				    GetDeviceFormatsInUseCallback cb ) override
	{
		blink::Vector<media::VideoCaptureFormat> v;
		if ( running_ ) v.push_back(media::VideoCaptureFormat(size_, fps_, media::PIXEL_FORMAT_I420));
		std::move(cb).Run(v);
	}

	void OnLog( const base::UnguessableToken &, const blink::String &message ) override
	{
		printf("browser: camera: %s\n", message.Utf8().c_str());
	}

	/* The newest picture (I420) and its size; false when the camera does not stream */
	bool Picture( std::vector<uint8_t> *pic, gfx::Size *size ) const
	{
		if ( !running_ || last_.empty() ) return false;
		*pic = last_;
		*size = size_;
		return true;
	}

	gfx::Size StreamSize() const { return running_ ? size_ : gfx::Size(); }

	/* The formats a camera gives, as I420 (the reader converts) */
	static blink::Vector<media::VideoCaptureFormat> br_camera_formats( int cam )
	{
		static BrCamMode modes[BR_CAM_MODES];
		blink::Vector<media::VideoCaptureFormat> v;
		int n = br_cam_modes(cam, modes, BR_CAM_MODES);
		for ( int i = 0; i < n; i++ ) {
			float fps = modes[i].fastest > 0 ? 1e7f / modes[i].fastest : 30;
			media::VideoCaptureFormat f(gfx::Size(modes[i].width, modes[i].height), fps, media::PIXEL_FORMAT_I420);
			bool seen = false;
			for ( const auto &g : v ) {
				if ( g.frame_size == f.frame_size ) {
					seen = true;
					break;
				}
			}
			if ( !seen ) v.push_back(f);
		}
		return v;
	}

private:
	struct Client {
		mojo::Remote<mm::VideoCaptureObserver>	observer;
		bool					paused = false;
		std::multiset<int>			held;
	};
	struct Buffer {
		base::UnsafeSharedMemoryRegion		region;
		base::WritableSharedMemoryMapping	map;
		int					holders = 0;
	};

	void MakeBuffers()
	{
		size_t bytes = media::VideoFrame::AllocationSize(media::PIXEL_FORMAT_I420, size_);
		for ( int i = 0; i < kBuffers; i++ ) {
			buffers_[i].region = base::UnsafeSharedMemoryRegion::Create(bytes);
			buffers_[i].map = buffers_[i].region.Map();
			buffers_[i].holders = 0;
		}
	}

	void StartReader()
	{
		running_ = true;
		quit_ = std::make_shared<std::atomic<bool>>(false);
		std::shared_ptr<std::atomic<bool>> quit = quit_;
		std::shared_ptr<std::atomic<int>> flight = flight_;
		Camera *self = this;
		std::thread([self, quit, flight]() {
			std::vector<uint8_t> raw((size_t)br_cam_frame_max() + 64);
			while ( !quit->load() ) {
				BrCamFrame h;
				int n = br_cam_read(raw.data(), (int)raw.size(), &h);
				if ( n < 0 ) {
					br_on_service_thread(base::BindOnce([]( Camera *c, std::shared_ptr<std::atomic<bool>> q ) {
						if ( !q->load() ) c->Gone();
					}, base::Unretained(self), quit));
					break;
				}
				if ( n == 0 || flight->load() >= kInFlight ) {
					usleep(kPollMs * 1000);
					continue;
				}
				std::vector<uint8_t> pic;
				if ( !to_i420(h, raw.data(), n, pic) ) continue;
				flight->fetch_add(1);
				br_on_service_thread(base::BindOnce([]( Camera *c, std::shared_ptr<std::atomic<bool>> q,
									std::shared_ptr<std::atomic<int>> f, std::vector<uint8_t> p ) {
					f->fetch_sub(1);
					if ( !q->load() ) c->Deliver(p, true);
				}, base::Unretained(self), quit, flight, std::move(pic)));
			}
		}).detach();
	}

	void StopAll()
	{
		if ( !running_ ) return;
		quit_->store(true);
		running_ = false;
		br_cam_close();
		for ( auto &b : buffers_ ) {
			b.map = base::WritableSharedMemoryMapping();
			b.region = base::UnsafeSharedMemoryRegion();
			b.holders = 0;
		}
		last_.clear();
		printf("browser: camera: closed\n");
	}

	/* The camera went: the tracks end */
	void Gone()
	{
		for ( auto &c : clients_ ) {
			if ( c.second.observer ) {
				c.second.observer->OnStateChanged(mm::VideoCaptureResult::NewErrorCode(
					media::VideoCaptureError::kVideoCaptureManagerDeviceConnectionLost));
			}
		}
		clients_.clear();
		StopAll();
	}

	void Deliver( const std::vector<uint8_t> &pic, bool keep )
	{
		int b = -1;
		for ( int i = 0; i < kBuffers && b < 0; i++ ) {
			if ( buffers_[i].holders == 0 && buffers_[i].map.IsValid() ) b = i;
		}
		if ( b < 0 || pic.size() > buffers_[b].map.size() ) return;	/* every buffer still held: dropped */
		memcpy(buffers_[b].map.memory(), pic.data(), pic.size());
		if ( keep ) last_ = pic;
		base::TimeTicks now = base::TimeTicks::Now();
		if ( first_.is_null() ) first_ = now;
		for ( auto &c : clients_ ) {
			if ( c.second.paused || !c.second.observer ) continue;
			media::VideoFrameMetadata md;
			md.reference_time = now;
			md.frame_rate = fps_;
			auto info = mm::VideoFrameInfo::New(now - first_, md, media::PIXEL_FORMAT_I420, size_, gfx::Rect(size_),
							   size_, false, gfx::ColorSpace::CreateREC601(), nullptr);
			c.second.observer->OnBufferReady(mm::ReadyBuffer::New(b, std::move(info)));
			c.second.held.insert(b);
			buffers_[b].holders++;
		}
	}

	mojo::ReceiverSet<mm::VideoCaptureHost>		receivers_;
	std::map<base::UnguessableToken, Client>	clients_;
	Buffer						buffers_[kBuffers];
	bool						running_ = false;
	gfx::Size					size_;
	float						fps_ = 30;
	base::TimeTicks					first_;
	std::vector<uint8_t>				last_;
	std::shared_ptr<std::atomic<bool>>		quit_ = std::make_shared<std::atomic<bool>>(false);
	std::shared_ptr<std::atomic<int>>		flight_ = std::make_shared<std::atomic<int>>(0);
};

Camera	*g_camera;

/* The host the page's captures speak to: each client handed to its session's, the screen's or the cameras' */
class Hosts final : public mm::VideoCaptureHost {
public:
	void Bind( mojo::PendingReceiver<mm::VideoCaptureHost> r ) { receivers_.Add(this, std::move(r)); }

	void Start( const base::UnguessableToken &device_id, const base::UnguessableToken &session_id,
		    const media::VideoCaptureParams &params, mojo::PendingRemote<mm::VideoCaptureObserver> o ) override
	{
		if ( br_screen_session_is(session_id) ) {
			screen_.insert(device_id);
		} else {
			screen_.erase(device_id);
		}
		Of(device_id)->Start(device_id, session_id, params, std::move(o));
	}
	void Stop( const base::UnguessableToken &device_id ) override
	{
		mm::VideoCaptureHost *h = Of(device_id);
		screen_.erase(device_id);
		h->Stop(device_id);
	}
	void Pause( const base::UnguessableToken &device_id ) override { Of(device_id)->Pause(device_id); }
	void Resume( const base::UnguessableToken &device_id, const base::UnguessableToken &session_id,
		     const media::VideoCaptureParams &params ) override
	{
		Of(device_id)->Resume(device_id, session_id, params);
	}
	void RequestRefreshFrame( const base::UnguessableToken &device_id ) override
	{
		Of(device_id)->RequestRefreshFrame(device_id);
	}
	void ReleaseBuffer( const base::UnguessableToken &device_id, int32_t buffer_id,
			    const media::VideoCaptureFeedback &feedback ) override
	{
		Of(device_id)->ReleaseBuffer(device_id, buffer_id, feedback);
	}
	void GetDeviceSupportedFormats( const base::UnguessableToken &device_id, const base::UnguessableToken &session_id,
					GetDeviceSupportedFormatsCallback cb ) override
	{
		Session(session_id)->GetDeviceSupportedFormats(device_id, session_id, std::move(cb));
	}
	void GetDeviceFormatsInUse( const base::UnguessableToken &device_id, const base::UnguessableToken &session_id,
				    GetDeviceFormatsInUseCallback cb ) override
	{
		Session(session_id)->GetDeviceFormatsInUse(device_id, session_id, std::move(cb));
	}
	void OnLog( const base::UnguessableToken &device_id, const blink::String &message ) override
	{
		Of(device_id)->OnLog(device_id, message);
	}

private:
	mm::VideoCaptureHost *Cameras()
	{
		if ( g_camera == nullptr ) g_camera = new Camera();
		return g_camera;
	}
	mm::VideoCaptureHost *Of( const base::UnguessableToken &device_id )
	{
		return screen_.count(device_id) ? br_screen_host() : Cameras();
	}
	mm::VideoCaptureHost *Session( const base::UnguessableToken &session_id )
	{
		return br_screen_session_is(session_id) ? br_screen_host() : Cameras();
	}

	mojo::ReceiverSet<mm::VideoCaptureHost>	receivers_;
	std::set<base::UnguessableToken>	screen_;	/* the clients of the screen's capture */
};

mm::RangePtr range( double v )
{
	auto r = mm::Range::New();
	r->min = r->max = r->current = v;
	r->step = 0;
	return r;
}

class Photos final : public mm::ImageCapture {
public:
	void Bind( mojo::PendingReceiver<mm::ImageCapture> r ) { receivers_.Add(this, std::move(r)); }

	void GetPhotoState( const blink::String &, GetPhotoStateCallback cb ) override
	{
		gfx::Size sz = g_camera != nullptr ? g_camera->StreamSize() : gfx::Size();
		auto st = mm::PhotoState::New();
		st->exposure_compensation = range(0);
		st->exposure_time = range(0);
		st->color_temperature = range(0);
		st->iso = range(0);
		st->brightness = range(0);
		st->contrast = range(0);
		st->saturation = range(0);
		st->sharpness = range(0);
		st->focus_distance = range(0);
		st->pan = range(0);
		st->tilt = range(0);
		st->zoom = range(1);
		st->width = range(sz.width());
		st->height = range(sz.height());
		std::move(cb).Run(std::move(st));
	}

	/* nothing of the camera's to set: what is asked is taken, and nothing changes */
	void SetPhotoOptions( const blink::String &, mm::PhotoSettingsPtr, SetPhotoOptionsCallback cb ) override
	{
		std::move(cb).Run(true);
	}

	/* The newest picture as JPEG; empty when the camera does not stream */
	void TakePhoto( const blink::String &, TakePhotoCallback cb ) override
	{
		std::vector<uint8_t> pic;
		gfx::Size sz;
		blink::Vector<uint8_t> jpeg;
		if ( g_camera != nullptr && g_camera->Picture(&pic, &sz) ) {
			int w = sz.width(), h = sz.height(), cw = ( w + 1 ) / 2, ch = ( h + 1 ) / 2;
			std::vector<uint8_t> rgba((size_t)w * h * 4);
			const uint8_t *y = pic.data(), *u = y + (size_t)w * h, *v = u + (size_t)cw * ch;
			SkDynamicMemoryWStream out;
			SkPixmap pm(SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kOpaque_SkAlphaType), rgba.data(),
				    (size_t)w * 4);
			if ( libyuv::I420ToABGR(y, w, u, cw, v, cw, rgba.data(), w * 4, w, h) == 0
			     && SkJpegEncoder::Encode(&out, pm, SkJpegEncoder::Options()) ) {
				jpeg.resize((blink::wtf_size_t)out.bytesWritten());
				out.copyTo(jpeg.data());
			}
		}
		printf("browser: camera: a photo of %u bytes\n", (unsigned)jpeg.size());
		std::move(cb).Run(mm::Blob::New(jpeg.empty() ? blink::String() : blink::String("image/jpeg"), std::move(jpeg)));
	}

private:
	mojo::ReceiverSet<mm::ImageCapture>	receivers_;
};

}  // namespace

/* A stream device opened on a camera: the session its capture starts with */
void br_camera_session( const base::UnguessableToken &session, int cam )
{
	g_sessions[session] = cam;
}

/* A camera's formats, for the capabilities of the media devices */
blink::Vector<media::VideoCaptureFormat> br_camera_formats( int cam )
{
	return Camera::br_camera_formats(cam);
}

/* The binder of the frames' broker */
void br_camera_init()
{
	blink::GetEmptyBrowserInterfaceBroker().SetBinderForTesting(
		mm::VideoCaptureHost::Name_, base::BindRepeating([]( mojo::ScopedMessagePipeHandle h ) {
			br_on_service_thread(base::BindOnce([]( mojo::PendingReceiver<mm::VideoCaptureHost> r ) {
				static Hosts *hosts = new Hosts();
				hosts->Bind(std::move(r));
			}, mojo::PendingReceiver<mm::VideoCaptureHost>(std::move(h))));
		}));
	blink::GetEmptyBrowserInterfaceBroker().SetBinderForTesting(
		mm::ImageCapture::Name_, base::BindRepeating([]( mojo::ScopedMessagePipeHandle h ) {
			br_on_service_thread(base::BindOnce([]( mojo::PendingReceiver<mm::ImageCapture> r ) {
				static Photos *svc = new Photos();
				svc->Bind(std::move(r));
			}, mojo::PendingReceiver<mm::ImageCapture>(std::move(h))));
		}));
}
