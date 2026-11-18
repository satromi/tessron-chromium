/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	pip_tessronos.cc
 *	ブラウザ: a video's picture-in-picture window (design 14.7.8).
 *
 *	What the browser's process answers a page's video with when it asks
 *	for a picture-in-picture window (PictureInPictureService): the
 *	application opens a window of its own and draws the video's player
 *	there, frame after frame (br_main.cc); the page is told the window's
 *	size. The session's Stop (exitPictureInPicture) closes the window;
 *	the window closed by the user stops the session, and the page is told
 *	(leavepictureinpicture). A press in the window plays or pauses the
 *	video through the element's player interface.
 *
 *	The service is on the services' thread; the application takes what it
 *	is to do on the engine's thread (br_pip_next), through a queue.
 */

#include <stdio.h>

#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

#include "base/functional/bind.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "media/mojo/mojom/media_player.mojom-blink.h"
#include "third_party/blink/public/mojom/picture_in_picture/picture_in_picture.mojom-blink.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"

#include "../../br.h"

void br_on_service_thread( base::OnceClosure task );

namespace {

namespace bm = blink::mojom::blink;

/* A window of the application wanted, or wanted gone */
std::mutex		g_lock;
std::deque<BrPipAsk>	g_asks;

void ask( const BrPipAsk &a )
{
	std::lock_guard<std::mutex> hold(g_lock);
	g_asks.push_back(a);
}

/* The window's size for a video: no wider than 400, its aspect kept */
void window_size( int vw, int vh, int *w, int *h )
{
	if ( vw <= 0 || vh <= 0 ) {
		vw = 16;
		vh = 9;
	}
	*w = vw > 400 ? 400 : vw;
	*h = (int)( (long)*w * vh / vw );
	if ( *h < 60 ) *h = 60;
}

class Session;
std::map<int, Session *>	g_sessions;	/* by number, on the services' thread */
int				g_last = 0;

/* One video in a window: its player, and the page's observer of the window */
class Session final : public bm::PictureInPictureSession {
public:
	Session( int n, mojo::PendingRemote<bm::PictureInPictureSessionObserver> observer )
		: n_(n), observer_(std::move(observer)) {}

	void Bind( mojo::PendingReceiver<bm::PictureInPictureSession> r )
	{
		receiver_.Bind(std::move(r));
		receiver_.set_disconnect_handler(base::BindOnce(&Session::Gone, base::Unretained(this)));
	}

	void SetPlayer( int player, mojo::PendingAssociatedRemote<media::mojom::blink::MediaPlayer> remote )
	{
		player_ = player;
		player_remote_.reset();
		if ( remote.is_valid() ) player_remote_.Bind(std::move(remote));
	}

	int player() const { return player_; }

	/* Another video of the page in the same window */
	void Update( uint32_t player_id, mojo::PendingAssociatedRemote<media::mojom::blink::MediaPlayer> remote,
		     const viz::SurfaceId &, const gfx::Size &size, bool ) override
	{
		SetPlayer((int)player_id, std::move(remote));
		BrPipAsk a;
		a.session = n_;
		a.player = player_;
		window_size(size.width(), size.height(), &a.w, &a.h);
		a.what = BrPipAsk::kUpdate;
		ask(a);
	}

	/* exitPictureInPicture: the window goes */
	void Stop( StopCallback cb ) override
	{
		Close();
		std::move(cb).Run();
	}

	void UpdateMediaPosition( media_session::mojom::blink::MediaPositionPtr ) override {}

	/* The window closed by the user: the page is told */
	void Stopped()
	{
		if ( observer_.is_bound() ) observer_->OnStopped();
		closed_ = true;
	}

	/* Another video took the window's place: this one's window goes, and its page is told */
	void Replaced()
	{
		Close();
		if ( observer_.is_bound() ) observer_->OnStopped();
	}

	void Press( bool play )
	{
		if ( !player_remote_.is_bound() ) return;
		if ( play ) player_remote_->RequestPlay(true);
		else player_remote_->RequestPause(true);
	}

private:
	void Close()
	{
		if ( closed_ ) return;
		closed_ = true;
		BrPipAsk a;
		a.session = n_;
		a.what = BrPipAsk::kClose;
		ask(a);
	}

	void Gone()
	{
		Close();
		g_sessions.erase(n_);
		delete this;
	}

	int							n_;
	int							player_ = -1;
	bool							closed_ = false;
	mojo::Receiver<bm::PictureInPictureSession>		receiver_{this};
	mojo::Remote<bm::PictureInPictureSessionObserver>	observer_;
	mojo::AssociatedRemote<media::mojom::blink::MediaPlayer>	player_remote_;
};

class Service final : public bm::PictureInPictureService {
public:
	void Bind( mojo::PendingReceiver<bm::PictureInPictureService> r ) { receivers_.Add(this, std::move(r)); }

	void StartSession( uint32_t player_id, mojo::PendingAssociatedRemote<media::mojom::blink::MediaPlayer> remote,
			   const viz::SurfaceId &, const gfx::Size &natural_size, bool,
			   mojo::PendingRemote<bm::PictureInPictureSessionObserver> observer, const gfx::Rect &, bool,
			   const media::VideoSpatialFormat &, StartSessionCallback cb ) override
	{
		/* one window at a time: a video already in one leaves it */
		for ( auto &kv : g_sessions ) kv.second->Replaced();
		int n = ++g_last;
		auto *s = new Session(n, std::move(observer));
		s->SetPlayer((int)player_id, std::move(remote));
		mojo::PendingRemote<bm::PictureInPictureSession> remote_session;
		s->Bind(remote_session.InitWithNewPipeAndPassReceiver());
		g_sessions[n] = s;

		BrPipAsk a;
		a.session = n;
		a.player = (int)player_id;
		window_size(natural_size.width(), natural_size.height(), &a.w, &a.h);
		a.what = BrPipAsk::kOpen;
		ask(a);
		printf("browser: picture-in-picture: video %u in a window %dx%d\n", player_id, a.w, a.h);
		std::move(cb).Run(std::move(remote_session), gfx::Size(a.w, a.h));
	}

private:
	mojo::ReceiverSet<bm::PictureInPictureService>	receivers_;
};

}  // namespace

/* What the application is to do next for the videos' windows, from the engine's thread */
bool br_pip_next( BrPipAsk *a )
{
	std::lock_guard<std::mutex> hold(g_lock);
	if ( g_asks.empty() ) return false;
	*a = g_asks.front();
	g_asks.pop_front();
	return true;
}

/* A video's window closed by the user */
void br_pip_closed( int session )
{
	br_on_service_thread(base::BindOnce([]( int n ) {
		auto it = g_sessions.find(n);
		if ( it != g_sessions.end() ) it->second->Stopped();
	}, session));
}

/* A press in a video's window: the video played, or paused */
void br_pip_press( int session, bool play )
{
	br_on_service_thread(base::BindOnce([]( int n, bool play ) {
		auto it = g_sessions.find(n);
		if ( it != g_sessions.end() ) it->second->Press(play);
	}, session, play));
}

/* The binder of the frames' broker, once when the engine is up */
void br_pip_init()
{
	blink::GetEmptyBrowserInterfaceBroker().SetBinderForTesting(
		bm::PictureInPictureService::Name_, base::BindRepeating([]( mojo::ScopedMessagePipeHandle h ) {
			br_on_service_thread(base::BindOnce([]( mojo::PendingReceiver<bm::PictureInPictureService> r ) {
				static Service *svc = new Service();
				svc->Bind(std::move(r));
			}, mojo::PendingReceiver<bm::PictureInPictureService>(std::move(h))));
		}));
}
