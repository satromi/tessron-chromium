/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	screen_tessronos.cc
 *	ブラウザ: what a page shares (getDisplayMedia, design 14.4.8), the
 *	screen or a window: the video capture host of a display capture's
 *	stream device, as a browser's desktop capture device behind it would
 *	stream it. Each session is a capture of its own; the host the clients
 *	speak to hands each to its session's.
 *
 *	MediaStreamDispatcherHost (devices_tessronos.cc) gives a display
 *	stream device only when the person allows it on the browser's panel,
 *	each time a page asks, and opens it with a session of its own
 *	(br_screen_session); the capture of that session comes here
 *	(camera_tessronos.cc hands a client to the host of its session).
 *
 *	While it streams, a thread copies the screen chosen, its rectangle of
 *	the view with the pointer on it, or the window's work area whatever
 *	covers it (br_screen.cc), at the frame rate asked for (30 at most),
 *	makes it
 *	I420 with libyuv, scaled down to fit the size asked for when it is
 *	smaller, and on the services' thread puts it in one of the shared
 *	buffers the clients were told of. A buffer is used again once every
 *	client it went to has given it back. A window's size can change from
 *	one picture to the next, and each goes at its own size; when the
 *	window is closed, the clients are told the capture ended, and the
 *	page's track ends. The screen's rectangle is asked of the browser
 *	(br_share_screen_rect) every kScreenLookMs: a screen moved or of
 *	another size is followed, its pictures at its new size, and one taken
 *	away or turned to mirroring the main screen ends the capture as a
 *	closed window does. A screen is taken for gone only when two looks
 *	in a row do not find it: the outputs' layer sets new outputs by
 *	taking every one besides the main one off and putting them on again,
 *	and a look between the two sees the main screen alone. A picture
 *	larger than the buffers were made for has new ones made: those no
 *	client holds go at once (OnBufferDestroyed), the others when they are
 *	given back, and the clients are told of the new ones (OnNewBuffer)
 *	under numbers of their own. The browser's window says that
 *	the screen is shared before the page's title (br_capture_text) for
 *	as long as it streams.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <set>
#include <string>
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
#include "media/capture/video_capture_types.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "third_party/libyuv/include/libyuv/convert.h"
#include "third_party/libyuv/include/libyuv/scale.h"
#include "ui/gfx/color_space.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"

#include "../media/br_share.h"

void br_on_service_thread( base::OnceClosure task );

/* The screen's size and pixels (port/media/br_screen.cc) */
extern "C" int br_screen_size( int *w, int *h );
extern "C" int br_screen_grab( uint32_t *dst, int pitch, int x, int y, int w, int h );
extern "C" int br_window_grab( int wid, unsigned serial, uint32_t *dst, int pitch, int maxw, int maxh, int *w, int *h );

namespace {

namespace mm = media::mojom::blink;

constexpr int kBuffers = 4;
constexpr int kInFlight = 2;		/* pictures on their way to the services' thread at most */
constexpr float kMaxFps = 30;
constexpr int kScreenLookMs = 500;	/* how often a screen's rectangle is asked again */

std::atomic<int>	g_sharing{0};	/* captures that stream */

/* The size the screen is sent at: the screen's, scaled down to fit 'want' (even sides, for I420) */
gfx::Size fit( int sw, int sh, gfx::Size want )
{
	double k = 1.0;
	if ( !want.IsEmpty() ) k = std::min({ 1.0, (double)want.width() / sw, (double)want.height() / sh });
	int w = std::max(2, (int)( sw * k ) & ~1), h = std::max(2, (int)( sh * k ) & ~1);
	return gfx::Size(w, h);
}

class Screen final : public mm::VideoCaptureHost {
public:
	explicit Screen( const BrShareChoice &chosen )
		: wid_(chosen.kind == BR_SHARE_SCREEN ? 0 : chosen.wid), serial_(chosen.serial), chosen_(chosen) {}

	void Start( const base::UnguessableToken &device_id, const base::UnguessableToken &,
		    const media::VideoCaptureParams &params, mojo::PendingRemote<mm::VideoCaptureObserver> o ) override
	{
		Client &c = clients_[device_id];
		c.observer.reset();
		c.observer.Bind(std::move(o));
		c.paused = false;
		c.held.clear();
		if ( !running_ ) {
			int sx = 0, sy = 0, sw = 0, sh = 0;
			bool there = wid_ != 0 ? br_screen_size(&sw, &sh) != 0 : br_share_screen_rect(chosen_, &sx, &sy, &sw, &sh);
			if ( !there || sw < 2 || sh < 2 ) {
				printf("browser: screen: no screen to share\n");
				c.observer->OnStateChanged(mm::VideoCaptureResult::NewErrorCode(
					media::VideoCaptureError::kDesktopCaptureDeviceWebrtcDesktopCapturerHasFailed));
				clients_.erase(device_id);
				return;
			}
			screen_ = gfx::Size(sw, sh);
			want_ = params.requested_format.frame_size;
			size_ = fit(sw, sh, want_);
			fps_ = params.requested_format.frame_rate > 0 ? std::min(params.requested_format.frame_rate, kMaxFps)
								       : kMaxFps;
			gfx::Size most(screen_);
			if ( !want_.IsEmpty() ) most.SetToMin(want_);
			bytes_ = media::VideoFrame::AllocationSize(media::PIXEL_FORMAT_I420,
								   gfx::Size(most.width() + 1, most.height() + 1));
			MakeBuffers(nullptr);
			StartGrabber();
			g_sharing++;
			printf("browser: screen: shared, %dx%d of %dx%d at %.0f a second\n", size_.width(), size_.height(), sw,
			       sh, fps_);
		}
		for ( auto &b : buffers_ ) {
			if ( b.second.retired ) continue;
			c.observer->OnNewBuffer(b.first, mm::VideoBufferHandle::NewUnsafeShmemRegion(b.second.region.Duplicate()));
		}
		c.observer->OnStateChanged(mm::VideoCaptureResult::NewState(mm::VideoCaptureState::STARTED));
	}

	void Stop( const base::UnguessableToken &device_id ) override
	{
		auto it = clients_.find(device_id);
		if ( it == clients_.end() ) return;
		std::multiset<int> held = std::move(it->second.held);
		if ( it->second.observer ) {
			it->second.observer->OnStateChanged(mm::VideoCaptureResult::NewState(mm::VideoCaptureState::STOPPED));
		}
		clients_.erase(it);
		for ( int b : held ) Unhold(b);
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
		if ( !last_.empty() ) Deliver(last_, last_size_, false);
	}

	void ReleaseBuffer( const base::UnguessableToken &device_id, int32_t buffer_id,
			    const media::VideoCaptureFeedback & ) override
	{
		auto it = clients_.find(device_id);
		if ( it == clients_.end() ) return;
		auto h = it->second.held.find(buffer_id);
		if ( h == it->second.held.end() ) return;
		it->second.held.erase(h);
		Unhold(buffer_id);
	}

	void GetDeviceSupportedFormats( const base::UnguessableToken &, const base::UnguessableToken &,
					GetDeviceSupportedFormatsCallback cb ) override
	{
		blink::Vector<media::VideoCaptureFormat> v;
		int sx = 0, sy = 0, sw = 0, sh = 0;
		bool there = wid_ != 0 ? br_screen_size(&sw, &sh) != 0 : br_share_screen_rect(chosen_, &sx, &sy, &sw, &sh);
		if ( there && sw >= 2 && sh >= 2 ) {
			v.push_back(media::VideoCaptureFormat(gfx::Size(sw & ~1, sh & ~1), kMaxFps, media::PIXEL_FORMAT_I420));
		}
		std::move(cb).Run(v);
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
		printf("browser: screen: %s\n", message.Utf8().c_str());
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
		bool					retired = false;	/* too small: goes once it is given back */
	};

	/* kBuffers buffers of bytes_ under new numbers, the clients told of them when 'tell' */
	void MakeBuffers( std::map<base::UnguessableToken, Client> *tell )
	{
		for ( int i = 0; i < kBuffers; i++ ) {
			int id = next_buffer_++;
			Buffer &b = buffers_[id];
			b.region = base::UnsafeSharedMemoryRegion::Create(bytes_);
			b.map = b.region.Map();
			b.holders = 0;
			b.retired = false;
			if ( tell == nullptr ) continue;
			for ( auto &c : *tell ) {
				if ( c.second.observer ) {
					c.second.observer->OnNewBuffer(id, mm::VideoBufferHandle::NewUnsafeShmemRegion(b.region.Duplicate()));
				}
			}
		}
	}

	/* A buffer no longer on the clients' list */
	void Drop( int id )
	{
		for ( auto &c : clients_ ) {
			if ( c.second.observer ) c.second.observer->OnBufferDestroyed(id);
		}
		buffers_.erase(id);
	}

	/* A buffer given back by one who held it: a retired one goes when nobody holds it */
	void Unhold( int id )
	{
		auto it = buffers_.find(id);
		if ( it == buffers_.end() ) return;
		if ( --it->second.holders <= 0 ) {
			it->second.holders = 0;
			if ( it->second.retired ) Drop(id);
		}
	}

	/* Buffers for pictures of 'need' bytes: the ones there retired, new ones made and told of */
	void Grow( size_t need )
	{
		std::vector<int> free_now;
		for ( auto &b : buffers_ ) {
			b.second.retired = true;
			if ( b.second.holders == 0 ) free_now.push_back(b.first);
		}
		for ( int id : free_now ) Drop(id);
		bytes_ = need;
		MakeBuffers(&clients_);
		printf("browser: screen: buffers made anew for %zu bytes a picture\n", need);
	}

	void StartGrabber()
	{
		running_ = true;
		quit_ = std::make_shared<std::atomic<bool>>(false);
		std::shared_ptr<std::atomic<bool>> quit = quit_;
		std::shared_ptr<std::atomic<int>> flight = flight_;
		Screen *self = this;
		gfx::Size screen = screen_, want = want_;
		int wid = wid_;
		unsigned serial = serial_;
		BrShareChoice chosen = chosen_;
		long period_us = (long)( 1e6 / fps_ );
		std::thread([self, quit, flight, screen, want, wid, serial, chosen, period_us]() {
			int sw = screen.width(), sh = screen.height();
			std::vector<uint32_t> argb((size_t)sw * sh);
			std::vector<uint8_t> full((size_t)sw * sh + 2 * (size_t)( ( sw + 1 ) / 2 ) * ( ( sh + 1 ) / 2 ));
			/* the screen's rectangle in the view, asked again every kScreenLookMs */
			int rx = 0, ry = 0, rw = sw, rh = sh;
			bool gone = false;
			int missed = 0;		/* looks in a row that did not find the screen */
			base::TimeTicks looked;
			base::TimeTicks next = base::TimeTicks::Now();
			while ( !quit->load() ) {
				base::TimeTicks now = base::TimeTicks::Now();
				if ( now < next ) {
					usleep((useconds_t)std::min<int64_t>(( next - now ).InMicroseconds(), 50000));
					continue;
				}
				next += base::Microseconds(period_us);
				if ( next < now ) next = now + base::Microseconds(period_us);
				if ( flight->load() >= kInFlight ) continue;
				int gw = sw, gh = sh, got;
				if ( wid != 0 ) {
					got = br_window_grab(wid, serial, argb.data(), sw, sw, sh, &gw, &gh);
				} else {
					if ( looked.is_null() || now - looked >= base::Milliseconds(kScreenLookMs) ) {
						looked = now;
						int x = 0, y = 0, w = 0, h = 0;
						bool there = br_share_screen_rect(chosen, &x, &y, &w, &h);
						if ( !there ) {
							/* not found once: no picture until the next look says */
							gone = ++missed >= 2;
						} else {
							missed = 0;
						}
						if ( there && ( w != rw || h != rh || x != rx || y != ry ) ) {
							printf("browser: screen: the screen shared is now %dx%d at %d,%d\n", w, h, x, y);
							rx = x;
							ry = y;
							rw = w;
							rh = h;
							if ( (size_t)w * h > argb.size() ) {
								argb.resize((size_t)w * h);
								full.resize((size_t)w * h + 2 * (size_t)( ( w + 1 ) / 2 ) * ( ( h + 1 ) / 2 ));
							}
						}
					}
					if ( missed > 0 && !gone ) continue;
					gw = rw;
					gh = rh;
					got = gone ? 1 : br_screen_grab(argb.data(), rw, rx, ry, rw, rh);
				}
				if ( got > 0 ) {
					br_on_service_thread(base::BindOnce([]( Screen *s, std::shared_ptr<std::atomic<bool>> q ) {
						if ( !q->load() ) s->Ended();
					}, base::Unretained(self), quit));
					break;
				}
				if ( got < 0 || gw < 2 || gh < 2 ) continue;
				gfx::Size size = fit(gw, gh, want);
				int w = size.width(), h = size.height();
				int scw = ( gw + 1 ) / 2, sch = ( gh + 1 ) / 2;
				int cw = ( w + 1 ) / 2, ch = ( h + 1 ) / 2;
				std::vector<uint8_t> pic((size_t)w * h + 2 * (size_t)cw * ch);
				uint8_t *y = pic.data(), *u = y + (size_t)w * h, *v = u + (size_t)cw * ch;
				const uint8_t *src = reinterpret_cast<const uint8_t *>(argb.data());
				int rowpx = wid != 0 ? sw : rw;	/* the pixels to a row of what was copied */
				if ( w == gw && h == gh ) {
					libyuv::ARGBToI420(src, rowpx * 4, y, w, u, cw, v, cw, w, h);
				} else {
					uint8_t *fy = full.data(), *fu = fy + (size_t)gw * gh, *fv = fu + (size_t)scw * sch;
					libyuv::ARGBToI420(src, rowpx * 4, fy, gw, fu, scw, fv, scw, gw, gh);
					libyuv::I420Scale(fy, gw, fu, scw, fv, scw, gw, gh, y, w, u, cw, v, cw, w, h,
							  libyuv::kFilterBox);
				}
				flight->fetch_add(1);
				br_on_service_thread(base::BindOnce([]( Screen *s, std::shared_ptr<std::atomic<bool>> q,
									std::shared_ptr<std::atomic<int>> f, std::vector<uint8_t> p, gfx::Size z ) {
					f->fetch_sub(1);
					if ( !q->load() ) s->Deliver(p, z, true);
				}, base::Unretained(self), quit, flight, std::move(pic), size));
			}
		}).detach();
	}

	void StopAll()
	{
		if ( !running_ ) return;
		quit_->store(true);
		running_ = false;
		g_sharing--;
		buffers_.clear();
		last_.clear();
		printf("browser: screen: no longer shared\n");
	}

	/* The window is closed, or the screen gone: the clients' tracks end */
	void Ended()
	{
		printf("browser: screen: the %s shared has gone\n", wid_ != 0 ? "window" : "screen");
		for ( auto &c : clients_ ) {
			if ( c.second.observer ) {
				c.second.observer->OnStateChanged(mm::VideoCaptureResult::NewState(mm::VideoCaptureState::ENDED));
			}
		}
		clients_.clear();
		StopAll();
	}

	void Deliver( const std::vector<uint8_t> &pic, gfx::Size size, bool keep )
	{
		if ( !running_ ) return;
		if ( pic.size() > bytes_ ) Grow(pic.size());
		int b = -1;
		for ( auto &e : buffers_ ) {
			if ( !e.second.retired && e.second.holders == 0 && e.second.map.IsValid() ) {
				b = e.first;
				break;
			}
		}
		if ( b < 0 || pic.size() > buffers_[b].map.size() ) return;	/* every buffer still held: dropped */
		memcpy(buffers_[b].map.memory(), pic.data(), pic.size());
		if ( keep ) {
			last_ = pic;
			last_size_ = size;
		}
		if ( size != size_ ) {
			if ( wid_ == 0 ) printf("browser: screen: the pictures go at %dx%d\n", size.width(), size.height());
			size_ = size;
		}
		base::TimeTicks now = base::TimeTicks::Now();
		if ( first_.is_null() ) first_ = now;
		for ( auto &c : clients_ ) {
			if ( c.second.paused || !c.second.observer ) continue;
			media::VideoFrameMetadata md;
			md.reference_time = now;
			md.frame_rate = fps_;
			auto info = mm::VideoFrameInfo::New(now - first_, md, media::PIXEL_FORMAT_I420, size, gfx::Rect(size),
							   size, false, gfx::ColorSpace::CreateREC709(), nullptr);
			c.second.observer->OnBufferReady(mm::ReadyBuffer::New(b, std::move(info)));
			c.second.held.insert(b);
			buffers_[b].holders++;
		}
	}

	std::map<base::UnguessableToken, Client>	clients_;
	std::map<int, Buffer>				buffers_;	/* by the number the clients know it by */
	int						next_buffer_ = 0;
	size_t						bytes_ = 0;	/* what each is made for */
	bool						running_ = false;
	int						wid_;		/* the window, 0 the screen */
	unsigned					serial_;
	BrShareChoice					chosen_;	/* the screen chosen, found again by br_share_screen_rect */
	gfx::Size					screen_, size_, want_, last_size_;
	float						fps_ = kMaxFps;
	base::TimeTicks					first_;
	std::vector<uint8_t>				last_;
	std::shared_ptr<std::atomic<bool>>		quit_ = std::make_shared<std::atomic<bool>>(false);
	std::shared_ptr<std::atomic<int>>		flight_ = std::make_shared<std::atomic<int>>(0);
};

/* The host of every share: each client to its session's capture */
class Shares final : public mm::VideoCaptureHost {
public:
	void Add( const base::UnguessableToken &session, const BrShareChoice &chosen )
	{
		sessions_[session] = std::make_unique<Screen>(chosen);
	}
	bool Has( const base::UnguessableToken &session ) const { return sessions_.count(session) != 0; }

	void Start( const base::UnguessableToken &device_id, const base::UnguessableToken &session,
		    const media::VideoCaptureParams &params, mojo::PendingRemote<mm::VideoCaptureObserver> o ) override
	{
		auto it = sessions_.find(session);
		if ( it == sessions_.end() ) return;
		devices_[device_id] = it->second.get();
		it->second->Start(device_id, session, params, std::move(o));
	}
	void Stop( const base::UnguessableToken &device_id ) override
	{
		if ( Screen *s = Of(device_id) ) s->Stop(device_id);
		devices_.erase(device_id);
	}
	void Pause( const base::UnguessableToken &device_id ) override
	{
		if ( Screen *s = Of(device_id) ) s->Pause(device_id);
	}
	void Resume( const base::UnguessableToken &device_id, const base::UnguessableToken &session,
		     const media::VideoCaptureParams &params ) override
	{
		if ( Screen *s = Of(device_id) ) s->Resume(device_id, session, params);
	}
	void RequestRefreshFrame( const base::UnguessableToken &device_id ) override
	{
		if ( Screen *s = Of(device_id) ) s->RequestRefreshFrame(device_id);
	}
	void ReleaseBuffer( const base::UnguessableToken &device_id, int32_t buffer_id,
			    const media::VideoCaptureFeedback &feedback ) override
	{
		if ( Screen *s = Of(device_id) ) s->ReleaseBuffer(device_id, buffer_id, feedback);
	}
	void GetDeviceSupportedFormats( const base::UnguessableToken &device_id, const base::UnguessableToken &session,
					GetDeviceSupportedFormatsCallback cb ) override
	{
		auto it = sessions_.find(session);
		if ( it == sessions_.end() ) {
			std::move(cb).Run({});
			return;
		}
		it->second->GetDeviceSupportedFormats(device_id, session, std::move(cb));
	}
	void GetDeviceFormatsInUse( const base::UnguessableToken &device_id, const base::UnguessableToken &session,
				    GetDeviceFormatsInUseCallback cb ) override
	{
		auto it = sessions_.find(session);
		if ( it == sessions_.end() ) {
			std::move(cb).Run({});
			return;
		}
		it->second->GetDeviceFormatsInUse(device_id, session, std::move(cb));
	}
	void OnLog( const base::UnguessableToken &, const blink::String &message ) override
	{
		printf("browser: screen: %s\n", message.Utf8().c_str());
	}

private:
	Screen *Of( const base::UnguessableToken &device_id )
	{
		auto it = devices_.find(device_id);
		return it == devices_.end() ? nullptr : it->second;
	}

	/* a capture is kept as long as the program runs: its grabber may still have a task under way for it */
	std::map<base::UnguessableToken, std::unique_ptr<Screen>>	sessions_;
	std::map<base::UnguessableToken, Screen *>			devices_;
};

Shares *AllShares()
{
	static Shares *h = new Shares();
	return h;
}

}  // namespace

/* A display stream device opened (on the services' thread): its session's capture is that window's, wid 0 the screen's */
void br_screen_session( const base::UnguessableToken &session, const BrShareChoice &chosen )
{
	AllShares()->Add(session, chosen);
}

bool br_screen_session_is( const base::UnguessableToken &session )
{
	return AllShares()->Has(session);
}

/* The host of the shares' captures (on the services' thread) */
media::mojom::blink::VideoCaptureHost *br_screen_host()
{
	return AllShares();
}

/* What the browser's window says before the title while something is shared; "" when not */
std::string br_capture_text()
{
	return g_sharing.load() > 0 ? std::string("画面を共有中") : std::string();
}
