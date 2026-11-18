/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_rproc.cc
 *	ブラウザ: a renderer's process (design 14.13).
 *
 *	A renderer is the browser's program started with the line "renderer"
 *	first and the browser's own start-up lines after it. It takes the end
 *	of the stream pair the browser hands it, accepts the invitation that
 *	comes over it, brings up Blink at once (it is kept ready for the next
 *	page of another site) and then does what the browser asks, one call
 *	at a time, on its main thread, which is Blink's.
 *
 *	The calls of Renderer and Page are run by a loop of this file's own
 *	(IpcRunner), not as Blink's tasks: a call such as Pump runs Blink's
 *	tasks itself (run_tasks), as the one process ran them from its loop,
 *	and a call taken as one of Blink's tasks would run them within a task.
 *	Control is answered on the IO thread, so that a script holding the
 *	main thread can be stopped. What the renderer asks of the browser
 *	(Host) may be asked from any of its threads.
 *
 *	The renderer ends when the browser asks it to (Control.Closing), or
 *	when its pipes are cut (the browser has gone).
 */

#include <algorithm>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "base/functional/bind.h"
#include "base/task/bind_post_task.h"
#include "base/memory/unsafe_shared_memory_region.h"
#include "base/memory/shared_memory_mapping.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "mojo/public/cpp/bindings/shared_remote.h"
#include "mojo/public/cpp/platform/platform_channel_endpoint.h"
#include "mojo/public/cpp/platform/platform_handle.h"
#include "mojo/public/cpp/system/invitation.h"
#include "mojo/public/cpp/system/platform_handle.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkSurface.h"

#include "application/browser/mojom/br_process.mojom.h"

#include "br.h"
#include "br_proc.h"
#include "port/media/br_share.h"
#include "port/net/br_objfile.h"
#include "port/media/br_widevine.h"
#include "br_uidata.h"
#include "port/gpu/br_gpu.h"

/* A step of the start, with its time (br_main.cc): said on the renderer's line of its start */
void br_start_mark( const char *what );


scoped_refptr<base::SingleThreadTaskRunner> br_proc_io_runner();	/* br_proc.cc */
/* The renderer's pipe to the network process (port/net/net_service_tessronos.cc) */
void br_net_remote_attach( mojo::PendingRemote<br::mojom::Network> pipe );
/* and to its stores (port/net/store_service_tessronos.cc) */
void br_store_remote_attach( mojo::PendingRemote<br::mojom::Store> pipe );

namespace {

/* ---------------------------------------------------------------- the main thread's loop */

/*
 * Whether the main thread is in the engine's work: a call of the browser's
 * (Renderer, Page) being run, a page's turn, or Blink's tasks while the
 * loop waits. A call of the browser's that begins while it is (taken in
 * a synchronous call's wait, inside that work) is said, with the return
 * addresses of the main thread's frames to name the wait it came in
 * (addr2line with the program's symbols): the first kNestSaid in the
 * process.
 */
int	g_busy = 0;

struct Busy {
	Busy() { g_busy++; }
	~Busy() { g_busy--; }
};

constexpr int kNestSaid = 8;
constexpr int kNestFrames = 24;

void nest_note( const char *iface, const char *call )
{
	static int said;

	if ( g_busy == 0 || said >= kNestSaid ) return;
	said++;
	void *lo = nullptr, *hi = nullptr;
	pthread_ts_stack(&lo, &hi);
	char line[kNestFrames * 19 + 1];
	int n = 0;
	line[0] = '\0';
	uintptr_t *fp = static_cast<uintptr_t *>(__builtin_frame_address(0));
	for ( int i = 0; i < kNestFrames; i++ ) {
		if ( (uintptr_t)fp < (uintptr_t)lo || (uintptr_t)fp + 2 * sizeof(uintptr_t) > (uintptr_t)hi
		     || ( (uintptr_t)fp & ( sizeof(uintptr_t) - 1 ) ) != 0 ) {
			break;
		}
		n += snprintf(line + n, sizeof(line) - (size_t)n, " %lx", (unsigned long)fp[1]);
		uintptr_t *up = reinterpret_cast<uintptr_t *>(fp[0]);
		if ( up <= fp ) break;
		fp = up;
	}
	printf("browser: renderer: %s.%s taken inside the engine's work (%d deep), in a wait of a synchronous call;"
	       " frames%s\n", iface, call, g_busy, line);
}

/* At the beginning of each call of the browser's: said when it is taken inside the engine's work, which it is then */
#define TAKEN( iface, call )	nest_note(iface, call); Busy busy_

/*
 * The tasks of the pipes bound on the main thread: posted by Mojo from
 * the IO thread, run by the renderer's loop one at a time
 */
class IpcRunner final : public base::SingleThreadTaskRunner {
public:
	IpcRunner() : main_(pthread_self()) {}

	bool PostDelayedTask( const base::Location &from, base::OnceClosure task, base::TimeDelta delay ) override
	{
		(void)from;
		{
			std::lock_guard<std::mutex> hold(lock_);
			tasks_.push_back(Task{ base::TimeTicks::Now() + delay, std::move(task) });
			posted_++;
		}
		/* the loop may wait in Blink's tasks (br_loop_wait): that wait ends */
		br_loop_wake();
		return true;
	}

	bool PostNonNestableDelayedTask( const base::Location &from, base::OnceClosure task,
					 base::TimeDelta delay ) override
	{
		return PostDelayedTask(from, std::move(task), delay);
	}

	bool RunsTasksInCurrentSequence() const override { return pthread_equal(pthread_self(), main_) != 0; }

	/*
	 * The tasks run as they come due, until the renderer is to end; when
	 * none is due, the pages take the turns that are due ('turns', which
	 * says in how many ms the next is; all of them after a wait that a
	 * task of Blink's ended, which may have changed what a page is), and
	 * the loop waits in Blink's tasks (br_loop_wait) for the first of a
	 * task of its own (which wakes that wait), a task of Blink's and a
	 * turn
	 */
	void run( const std::atomic<bool> &quit, long (*turns)( bool all ) )
	{
		bool woken = false;

		br_loop_bind();
		while ( !quit ) {
			base::OnceClosure task;
			long ms = BR_WAIT_MOST_MS;

			br_loop_round();
			{
				std::unique_lock<std::mutex> hold(lock_);
				base::TimeTicks now = base::TimeTicks::Now(), next = base::TimeTicks::Max();
				size_t pick = tasks_.size();
				for ( size_t i = 0; i < tasks_.size(); i++ ) {
					if ( tasks_[i].due <= now ) {
						pick = i;
						break;
					}
					if ( tasks_[i].due < next ) next = tasks_[i].due;
				}
				if ( pick != tasks_.size() ) {
					task = std::move(tasks_[pick].task);
					tasks_.erase(tasks_.begin() + (long)pick);
				} else if ( !next.is_max() ) {
					ms = ( next - now ).InMilliseconds() + 1;
				}
			}
			if ( task ) {
				std::move(task).Run();
				continue;
			}
			unsigned long seen;
			{
				std::lock_guard<std::mutex> hold(lock_);
				seen = posted_;
			}
			if ( turns != nullptr ) {
				Busy busy;
				ms = std::min(ms, turns(woken));
			}
			if ( ms > BR_WAIT_MOST_MS ) ms = BR_WAIT_MOST_MS;
			/* a task posted meanwhile (the turns post some) is not waited past */
			{
				std::lock_guard<std::mutex> hold(lock_);
				if ( posted_ != seen ) {
					woken = true;
					continue;
				}
			}
			{
				Busy busy;
				woken = ms <= 0 || br_loop_wait((int)ms);
			}
		}
	}

	void wake() { br_loop_wake(); }

private:
	~IpcRunner() override = default;

	struct Task {
		base::TimeTicks		due;
		base::OnceClosure	task;
	};

	pthread_t		main_;
	std::mutex		lock_;
	std::deque<Task>	tasks_;
	unsigned long		posted_ = 0;	/* tasks posted so far */
};

scoped_refptr<IpcRunner>		g_runner;
std::atomic<bool>			g_quit{false};
std::atomic<uint32_t>			g_stop_seq{0};	/* the navigation the last 中止 named (Control.StopLoad) */
mojo::SharedRemote<br::mojom::Host>	g_host;
base::Thread				*g_caller;	/* where a thread of no sequence asks the browser */

/*
 * A call to the browser (Host), from any of the renderer's threads. A
 * synchronous call waits by the sequence of the thread that makes it;
 * a thread of the port's own that has none (a pthread) has the call made
 * on the renderer's thread for such calls, and waits for it there.
 */
struct HostCall {
	std::function<bool()>	f;
	bool			r = false, done = false;
	std::mutex		m;
	std::condition_variable	cv;
};

bool on_host( std::function<bool()> f )
{
	if ( !g_host ) return false;
	if ( base::SequencedTaskRunner::HasCurrentDefault() || g_caller == nullptr ) return f();
	HostCall c;
	c.f = std::move(f);
	g_caller->task_runner()->PostTask(FROM_HERE, base::BindOnce([]( HostCall *c ) {
		bool r = c->f();
		std::lock_guard<std::mutex> hold(c->m);
		c->r = r;
		c->done = true;
		c->cv.notify_one();
	}, base::Unretained(&c)));
	std::unique_lock<std::mutex> hold(c.m);
	c.cv.wait(hold, [&c] { return c.done; });
	return c.r;
}

/* What the browser last said of the window, and its UUID */
std::mutex	g_win_lock;
bool		g_has_win = false;
int		g_win[4];
int		g_screen[2];
unsigned char	g_window_uuid[16];
BrEngine	*g_first;	/* the window's page, which a script may hold (Control) */

void end_now( const char *why )
{
	printf("browser: renderer: ends (%s)\n", why);
	g_quit = true;
	if ( g_runner ) g_runner->wake();
}

/* ---------------------------------------------------------------- a page */

class PageImpl;
std::vector<PageImpl *>	g_pages;	/* the pages of this renderer, which take their turns */

class PageImpl final : public br::mojom::Page {
public:
	PageImpl( BrEngine *eng, bool primary ) : eng_(eng), primary_(primary) { g_pages.push_back(this); }

	~PageImpl() override { g_pages.erase(std::remove(g_pages.begin(), g_pages.end(), this), g_pages.end()); }

	/* When the page's next turn is due, in ms of the monotonic clock */
	long next_turn() const { return next_turn_; }

	/* Whether the page takes turns (its frames are bound, its document not closed) */
	bool turning() const { return frames_.is_bound() && !closed_; }

	/*
	 * A turn of the page's own: the engine's tasks and frame callbacks
	 * (pump), the frame it made drawn and told, and what the page now is
	 * told when it differs. The next is when the engine says
	 */
	void turn_now( long now )
	{
		if ( closed_ ) return;
		bool again = eng_->pump();
		br_sys_after_turn(eng_, primary_);
		if ( again ) want_ = true;
		bool showing = in_flight_;
		frame();
		/*
		 * A frame begun that sends nothing to be shown now (nothing the
		 * page's paint shows changed, or the frame before is still being
		 * shown) ends with the turn, as a frame with no damage ends at
		 * its begin-frame: the next is paced from it at the refresh, as
		 * after a frame shown (FrameDone). Otherwise a page that asks for
		 * frames and changes nothing painted has one begun at every turn.
		 */
		if ( again && ( showing || !in_flight_ ) ) eng_->frame_shown();
		news();
		int ms = eng_->idle_ms();
		next_turn_ = now + ( ms < 1 ? 1 : ms );
	}

	/* The page's state told, when something happened or a part of it the browser shows changed */
	void news()
	{
		if ( !frames_.is_bound() || closed_ ) return;
		br::mojom::PageStatePtr s = state();
		bool happened = !s->navigation.empty() || !s->opened.empty() || !s->apart.empty() || !s->pip.empty()
				|| s->fullscreen_asked != 0 || s->window_asked != 0 || s->close_asked;
		std::string key = s->title + '\n' + s->url + '\n' + s->hover + '\n' + std::to_string(s->scroll_top) + ','
				  + std::to_string(s->doc_width) + ',' + std::to_string(s->scroll_left) + ','
				  + std::to_string(s->editable) + std::to_string(s->can_back) + std::to_string(s->can_forward)
				  + s->dragging + std::to_string(s->isolated) + std::to_string(s->wv_busy)
				  + std::to_string(s->wv_installed) + '\n' + s->badge + '\n' + s->capture + '\n' + s->origin;
		for ( const std::string &a : s->ext_actions ) key += '\n' + a;
		if ( !happened && key == told_ ) return;
		told_ = key;
		frames_->State(std::move(s));
	}

	/* What the page is after a call, as the browser keeps it */
	br::mojom::PageStatePtr state( bool repaint = false )
	{
		auto s = br::mojom::PageState::New();
		s->repaint = repaint;
		s->title = eng_->title();
		s->url = eng_->url();
		s->hover = eng_->hover();
		for ( std::string to = eng_->navigation(); !to.empty(); to = eng_->navigation() ) {
			s->navigation.push_back(to);
			std::string m, c, b;
			s->nav_post = eng_->navigation_body(&m, &c, &b);
			s->nav_method = m;
			s->nav_ctype = c;
			s->nav_body = b;
		}
		s->fullscreen_asked = eng_->fullscreen_asked();
		s->window_asked = eng_->window_asked();
		s->close_asked = eng_->close_asked();
		for ( BrOpened o; eng_->opened(&o); ) {
			mojo::PendingRemote<br::mojom::Page> remote;
			mojo::MakeSelfOwnedReceiver(std::make_unique<PageImpl>(o.eng, false),
						    remote.InitWithNewPipeAndPassReceiver(), g_runner);
			s->opened.push_back(br::mojom::OpenedPage::New(std::move(remote), o.x, o.y, o.w, o.h, o.pip));
		}
		for ( std::string a = eng_->apart(); !a.empty(); a = eng_->apart() ) s->apart.push_back(a);
		s->idle_ms = eng_->idle_ms();
		s->scroll_top = eng_->scroll_top();
		s->doc_width = eng_->doc_width();
		s->doc_height = eng_->doc_height();
		s->beforeunload = eng_->beforeunload();
		s->scroll_left = eng_->scroll_left();
		s->editable = eng_->editable();
		s->can_back = eng_->can_go(-1);
		s->can_forward = eng_->can_go(1);
		/* the entries told with the state when where the page is among them may have changed */
		std::string hk = s->url + '\n' + std::to_string(s->can_back) + std::to_string(s->can_forward);
		s->hist_at = -1;
		if ( hk != hist_told_ ) {
			int at = -1;
			if ( eng_->history_save(&s->hist_states, &s->hist_urls, &at) ) {
				hist_told_ = hk;
				s->hist_given = true;
				s->hist_at = at;
			}
		}
		s->dragging = eng_->dragging();
		s->badge = br_badge_text();
		s->capture = br_capture_text();
		s->origin = br_page_origin();
		s->isolated = !s->origin.empty() && br_isolated_allowed(s->origin);
		s->ext_actions = br_sys_ext_actions();
		if ( primary_ ) {
			/* the picture-in-picture windows the renderer's videos asked for, for the browser to open */
			for ( BrPipAsk a; br_pip_next(&a); ) {
				s->pip.push_back(br::mojom::PipAsk::New((int32_t)a.what, a.session, a.player, a.w, a.h));
			}
		}
		s->wv_busy = br_wv_busy();
		s->wv_installed = br_wv_installed();
		return s;
	}

	void SetCanvas( mojo::ScopedSharedBufferHandle pixels, int32_t w, int32_t h, int32_t pitch,
			SetCanvasCallback cb ) override
	{
		TAKEN("Page", "SetCanvas");
		base::UnsafeSharedMemoryRegion region = mojo::UnwrapUnsafeSharedMemoryRegion(std::move(pixels));
		base::WritableSharedMemoryMapping map = region.IsValid() ? region.Map() : base::WritableSharedMemoryMapping();
		if ( !map.IsValid() || (size_t)pitch * (size_t)h > map.size() ) {
			std::move(cb).Run(false);
			return;
		}
		map_ = std::move(map);
		w_ = w;
		h_ = h;
		pitch_ = (size_t)pitch;
		surf_ = SkSurfaces::WrapPixels(SkImageInfo::MakeN32Premul(w, h), map_.memory(), pitch_);
		/* new memory: the whole view is drawn into it with the next frame */
		whole_ = true;
		want_ = true;
		std::move(cb).Run(surf_ != nullptr);
	}

	void Load( const std::string &url, const std::string &body, const std::string &charset, const std::string &type,
		   LoadCallback cb ) override
	{
		TAKEN("Page", "Load");
		eng_->load(url, body, charset, type);
		std::move(cb).Run(state());
	}

	void Navigate( br::mojom::NavigationPtr n, NavigateCallback cb ) override
	{
		TAKEN("Page", "Navigate");
		BrNav nav;
		BrNavResult out;
		nav.target = n->target;
		nav.how = n->how;
		nav.method = n->method;
		nav.ctype = n->ctype;
		nav.body = n->body;
		nav.from = n->from;
		nav.upgraded = n->upgraded;
		nav.restore = n->restore;
		nav.restore_at = n->restore_at;
		nav.seq = n->seq;
		if ( primary_ ) br_sys_tab_at(nav.target);
		(void)br_nav_fetch_load(eng_, nav, &out, &PageImpl::progress);
		auto r = br::mojom::NavResult::New(out.outcome, out.url, out.status, (int64_t)out.bytes, out.error, out.title);
		std::move(cb).Run(std::move(r), state());
	}

	static void progress( void *ctx, long got, long total )
	{
		(void)ctx;
		bool stop = false;
		static long last;
		struct timespec t;
		clock_gettime(CLOCK_MONOTONIC, &t);
		long now = t.tv_sec * 1000 + t.tv_nsec / 1000000;
		if ( now - last < 300 ) return;
		last = now;
		if ( on_host([&] { return g_host->Progress(got, total, &stop); }) && stop ) br_sys_stop(true);
	}

	/* What the browser says of the window, kept for the page's window's page */
	void take_turn( const br::mojom::TurnPtr &t )
	{
		if ( !primary_ || !t ) return;
		std::lock_guard<std::mutex> hold(g_win_lock);
		g_has_win = t->has_window;
		g_win[0] = t->win_x;
		g_win[1] = t->win_y;
		g_win[2] = t->win_w;
		g_win[3] = t->win_h;
		g_screen[0] = t->screen_w;
		g_screen[1] = t->screen_h;
	}

	void Layout( int32_t width, br::mojom::TurnPtr t, LayoutCallback cb ) override
	{
		TAKEN("Page", "Layout");
		take_turn(t);
		int h = eng_->layout(width);
		std::move(cb).Run(h, state());
	}

	/* ---------------------------------------- the frames */

	void Frames( mojo::PendingRemote<br::mojom::FrameSink> sink ) override
	{
		TAKEN("Page", "Frames");
		frames_.Bind(std::move(sink), g_runner);
		next_turn_ = 0;		/* its turns begin */
	}

	void Window( br::mojom::TurnPtr t ) override { TAKEN("Page", "Window"); take_turn(t); }

	/*
	 * The browser tells a view only when the one it shows changed, and it
	 * has no frame of that view (one drawn where the page went by itself
	 * may have come before it followed): a whole frame of it is drawn,
	 * even when this is where the page already is
	 */
	void View( int32_t top, int32_t height ) override
	{
		TAKEN("Page", "View");
		bool moved = top != view_top_ || height != view_h_;
		view_top_ = top;
		view_h_ = height;
		view_told_ = true;
		whole_ = true;
		want_ = true;
		if ( moved && surf_ == nullptr ) {
			/* no memory to draw in yet: the engine is told the view all the same, as asking what changed tells it */
			std::vector<BrRect> d;
			(void)eng_->damage(top, height, &d);
		}
		frame();
	}

	void Redraw() override
	{
		TAKEN("Page", "Redraw");
		whole_ = true;
		want_ = true;
		frame();
	}

	void FrameDone() override
	{
		TAKEN("Page", "FrameDone");
		in_flight_ = false;
		eng_->frame_shown();
		frame();
	}

	/*
	 * A frame, when the page has one and the browser is not reading the
	 * last: what changed in the view drawn into the page's memory (on the
	 * GPU when the engine rasters there, else on the processor), and told
	 */
	void frame()
	{
		if ( !want_ || in_flight_ || !frames_.is_bound() || surf_ == nullptr || view_h_ <= 0 ) return;
		/*
		 * The view is where the browser last put it (a bar, a key of its
		 * own) when it has told one since the last frame; else where the
		 * page went by itself (the wheel, the keys, a script's scroll,
		 * a smooth scroll's steps), which the frame and the next state
		 * tell the browser
		 */
		if ( !view_told_ ) {
			int top = eng_->scroll_top();
			if ( top >= 0 && top != view_top_ ) {
				view_top_ = top;
				whole_ = true;
			}
		}
		view_told_ = false;
		std::vector<BrRect> d;
		bool known = eng_->damage(view_top_, view_h_, &d);
		int h = std::min(view_h_, h_);
		/*
		 * Into the window's GPU buffers while they are of the view's size and
		 * the page is rastered on a channel to their GPU process; a frame of
		 * the other kind than the last is of all the view
		 */
		bool gs = gs_.set > 0 && gs_.w == w_ && gs_.h == h_ && br_gpu_gsurf_able != nullptr
			  && br_gpu_gsurf_able(gs_.gpu_pid);
		if ( !known || whole_ || gs != gs_.used ) {
			BrRect r;
			r.w = w_;
			r.h = h;
			d.assign(1, r);
		}
		want_ = false;
		whole_ = false;
		if ( d.empty() ) return;
		if ( gs ) {
			gs_.used = true;
			if ( gs_frame(d, h) ) return;
			/* the GPU could not: this frame, all of it, into the page's memory */
			BrRect r;
			r.w = w_;
			r.h = h;
			d.assign(1, r);
		}
		gs_.used = false;
		bool gpu = eng_->paint_gpu(d, view_top_, w_, h, map_.memory(), pitch_);
		if ( !gpu ) {
			SkCanvas *c = surf_->getCanvas();
			for ( const BrRect &r : d ) {
				c->save();
				c->clipRect(SkRect::MakeXYWH((float)r.x, (float)r.y, (float)r.w, (float)r.h));
				eng_->paint(c, view_top_, h);
				c->restore();
			}
		}
		std::vector<br::mojom::RectPtr> rs;
		for ( const BrRect &r : d ) rs.push_back(br::mojom::Rect::New(r.x, r.y, r.w, r.h));
		in_flight_ = true;
		frames_->Frame(std::move(rs), view_top_, view_h_);
		/* the view rastered on the GPU against the processor's ("gpucheck="), after the frame has gone */
		if ( gpu ) br_sys_gpu_check(eng_, view_top_, w_, h, map_.memory(), pitch_);
	}

	void LinkAt( int32_t x, int32_t y, LinkAtCallback cb ) override { TAKEN("Page", "LinkAt"); std::move(cb).Run(eng_->link_at(x, y)); }
	void FirstLink( FirstLinkCallback cb ) override { TAKEN("Page", "FirstLink"); std::move(cb).Run(eng_->first_link()); }

	void HistoryGo( int32_t offset, HistoryGoCallback cb ) override
	{
		TAKEN("Page", "HistoryGo");
		int r = eng_->history_go(offset);
		std::move(cb).Run(r, state());
	}

	void HistoryState( HistoryStateCallback cb ) override
	{
		TAKEN("Page", "HistoryState");
		std::vector<std::string> st, urls;
		int at = -1;
		(void)eng_->history_save(&st, &urls, &at);
		std::move(cb).Run(st, urls, at);
	}

	void HistoryCut() override { TAKEN("Page", "HistoryCut"); eng_->history_cut(); }

	void LoadAs( int32_t how ) override { TAKEN("Page", "LoadAs"); eng_->load_as(how); }

	void Response( int32_t status, const std::string &headers, const std::string &protocol ) override
	{
		TAKEN("Page", "Response");
		eng_->response(status, headers, protocol);
	}

	void MayLeave( bool reload, MayLeaveCallback cb ) override { TAKEN("Page", "MayLeave"); std::move(cb).Run(eng_->may_leave(reload)); }

	void Zoom( double factor ) override
	{
		TAKEN("Page", "Zoom");
		eng_->zoom(factor);
		news();
	}

	void FullscreenDone( bool full, br::mojom::TurnPtr t ) override
	{
		TAKEN("Page", "FullscreenDone");
		take_turn(t);
		eng_->fullscreen_done(full);
		news();
	}

	void Context( int32_t x, int32_t y, ContextCallback cb ) override
	{
		TAKEN("Page", "Context");
		BrEngine::Context what;
		auto at = br::mojom::ContextAt::New();
		at->took = eng_->context(x, y, &what);
		at->link = what.link;
		at->image = what.image;
		at->selection = what.selection;
		at->editable = what.editable;
		at->ext_items = br_sys_ext_items(what.link, what.image, what.selection, what.editable);
		std::move(cb).Run(std::move(at), state());
	}

	void Edit( const std::string &command ) override
	{
		TAKEN("Page", "Edit");
		eng_->edit(command);
		news();
	}

	void SaveUrl( const std::string &url ) override { TAKEN("Page", "SaveUrl"); eng_->save_url(url); }

	void Find( const std::string &text, int32_t step, FindCallback cb ) override
	{
		TAKEN("Page", "Find");
		int active = 0;
		int n = eng_->find(text, step, &active);
		std::move(cb).Run(n, active);
	}

	void Click( int32_t x, int32_t y, ClickCallback cb ) override
	{
		TAKEN("Page", "Click");
		bool took = eng_->click(x, y);
		std::move(cb).Run(took, state());
	}

	void Print( PrintCallback cb ) override { TAKEN("Page", "Print"); std::move(cb).Run(eng_->print()); }

	void Drop( int32_t x, int32_t y, std::vector<br::mojom::DropFilePtr> files, const std::string &text, uint32_t mods,
		   DropCallback cb ) override
	{
		TAKEN("Page", "Drop");
		std::vector<std::pair<std::string, std::string>> fs;
		for ( auto &f : files ) fs.emplace_back(f->path, f->name);
		bool took = eng_->drop(x, y, fs, text, mods);
		std::move(cb).Run(took, state());
	}

	void DragAway( bool taken ) override
	{
		TAKEN("Page", "DragAway");
		eng_->drag_away(taken);
		news();
	}

	void Mouse( int32_t type, int32_t x, int32_t y, int32_t button, uint32_t mods ) override
	{
		TAKEN("Page", "Mouse");
		eng_->mouse(type, x, y, button, mods);
		news();
	}

	void Key( uint32_t usage, uint32_t mods, uint32_t ch, KeyCallback cb ) override
	{
		TAKEN("Page", "Key");
		bool took = eng_->key(usage, mods, ch);
		std::move(cb).Run(took, state());
	}

	void Wheel( int32_t x, int32_t y, int32_t right, int32_t down ) override
	{
		TAKEN("Page", "Wheel");
		eng_->wheel(x, y, right, down);
		news();
	}

	void SetScrollLeft( int32_t x ) override
	{
		TAKEN("Page", "SetScrollLeft");
		eng_->set_scroll_left(x);
		news();
	}

	void Compose( const std::string &text, const std::vector<int32_t> &clauses, int32_t target, int32_t caret ) override
	{
		TAKEN("Page", "Compose");
		std::vector<int> cl(clauses.begin(), clauses.end());
		eng_->compose(text, cl, target, caret);
		news();
	}

	void Commit( const std::string &text ) override
	{
		TAKEN("Page", "Commit");
		eng_->commit(text);
		news();
	}

	void Caret( CaretCallback cb ) override
	{
		TAKEN("Page", "Caret");
		int x = 0, y = 0, h = 0;
		bool shown = eng_->caret(&x, &y, &h);
		std::move(cb).Run(shown, x, y, h);
	}

	void Memory( bool collect, MemoryCallback cb ) override { TAKEN("Page", "Memory"); std::move(cb).Run(eng_->memory(collect)); }

	void RunScript( const std::string &url ) override { TAKEN("Page", "RunScript"); eng_->run_script(url); }

	void OpenPage( const std::string &url, OpenPageCallback cb ) override
	{
		TAKEN("Page", "OpenPage");
		eng_->open_page(url);
		std::move(cb).Run(state());
	}

	void SetGsurf( std::vector<br::mojom::GsurfImagePtr> images, int32_t w, int32_t h, int32_t gpu_pid, int32_t set,
		       SetGsurfCallback cb ) override
	{
		TAKEN("Page", "SetGsurf");
		Gs g;
		bool ok = images.empty() || ( images.size() == 2 && set > 0 && gpu_pid > 0 && w > 0 && h > 0 );
		for ( size_t i = 0; ok && i < images.size(); i++ ) {
			if ( !images[i] || images[i]->mailbox.size() != sizeof(g.mailbox[i]) ) {
				ok = false;
				break;
			}
			memcpy(g.mailbox[i], images[i]->mailbox.data(), sizeof(g.mailbox[i]));
		}
		if ( ok && !images.empty() ) {
			g.set = set;
			g.gpu_pid = gpu_pid;
			g.w = w;
			g.h = h;
		}
		/* the frames go into the new buffers, or the page's memory, from the next on, whole */
		g.used = gs_.used;
		gs_ = g;
		whole_ = true;
		want_ = true;
		std::move(cb).Run(ok);
		frame();
	}

	/*
	 * On the main thread: a frame rastered into the window's buffer 'buf'
	 * taken in by the GPU process ('done'), told to the browser; or lost
	 * with the channel, when the page's next frame goes into its memory
	 */
	void gs_taken( int set, int buf, std::vector<BrRect> rects, int top, int height, bool done )
	{
		if ( !done ) {
			gs_.fresh[0] = gs_.fresh[1] = true;
			gs_.last = -1;
			in_flight_ = false;
			whole_ = true;
			want_ = true;
			frame();
			return;
		}
		if ( !frames_.is_bound() || closed_ ) return;
		std::vector<br::mojom::RectPtr> rs;
		for ( const BrRect &r : rects ) rs.push_back(br::mojom::Rect::New(r.x, r.y, r.w, r.h));
		frames_->GsurfFrame(std::move(rs), top, height, set, buf);
	}

	/*
	 * The rectangles 'd' of the view rastered into the window's next
	 * buffer, with those the frame before drew into the other (or all of
	 * it, into one never drawn into); told once the GPU process has taken
	 * them in. False when the GPU could not.
	 */
	bool gs_frame( const std::vector<BrRect> &d, int h )
	{
		int k = ( gs_.last + 1 ) % 2;
		std::vector<BrRect> r;
		if ( gs_.fresh[k] ) {
			BrRect a;
			a.w = w_;
			a.h = h;
			r.push_back(a);
		} else {
			r = d;
			r.insert(r.end(), gs_.last_rects.begin(), gs_.last_rects.end());
		}
		br_gpu_gsurf_aim(gs_.mailbox[k]);
		bool ok = eng_->paint_gpu(r, view_top_, w_, h, map_.memory(), pitch_);
		br_gpu_gsurf_aim(nullptr);
		if ( !ok ) return false;
		gs_.fresh[k] = false;
		gs_.last = k;
		gs_.last_rects = d;
		in_flight_ = true;
		/* the view rastered there against the processor's ("gpucheck="), read back from the buffer */
		if ( br_sys_gpu_checking() && br_gpu_gsurf_read != nullptr
		     && br_gpu_gsurf_read(gs_.mailbox[k], w_, h, map_.memory(), pitch_) ) {
			br_sys_gpu_check(eng_, view_top_, w_, h, map_.memory(), pitch_);
		}
		PageImpl *self = this;
		unsigned id = id_;
		int set = gs_.set, top = view_top_, vh = view_h_;
		br_gpu_gsurf_after([self, id, set, k, d, top, vh]( bool done ) {
			if ( std::find(g_pages.begin(), g_pages.end(), self) == g_pages.end() || self->id_ != id ) return;
			self->gs_taken(set, k, d, top, vh, done);
		});
		return true;
	}

	void Closed( ClosedCallback cb ) override
	{
		TAKEN("Page", "Closed");
		eng_->closed();
		/* its document gone, the page takes no more turns and tells nothing */
		closed_ = true;
		next_turn_ = 1L << 60;
		std::move(cb).Run();
	}

private:
	BrEngine				*eng_;
	bool					primary_;
	base::WritableSharedMemoryMapping	map_;
	sk_sp<SkSurface>			surf_;
	int					w_ = 0, h_ = 0;
	mojo::Remote<br::mojom::FrameSink>	frames_;
	long					next_turn_ = 1L << 60;	/* none before the frames are bound */
	bool					closed_ = false;	/* its document closed (Closed) */
	std::string				told_;			/* what the state last told said */
	std::string				hist_told_;		/* where the page was among its entries when they were last told */
	int					view_top_ = 0, view_h_ = 0;
	bool					view_told_ = false;	/* the browser told a view since the last frame */
	bool					want_ = false, whole_ = true, in_flight_ = false;
	size_t					pitch_ = 0;
	/*
	 * The window's GPU buffers (SetGsurf): the browser's set, of the GPU
	 * process 'gpu_pid', 'w' x 'h'; the two shared images; which have never
	 * been drawn into; the buffer of the last frame and its rectangles,
	 * which the other buffer lacks; whether the last frame was of them
	 */
	struct Gs {
		int			set = 0, gpu_pid = 0, w = 0, h = 0;
		unsigned char		mailbox[2][16] = {};
		bool			fresh[2] = { true, true };
		int			last = -1;
		std::vector<BrRect>	last_rects;
		bool			used = false;
	};
	Gs					gs_;
	unsigned				id_ = ++s_ids;		/* the page's, for what comes back to it later */
	static unsigned				s_ids;
};

unsigned PageImpl::s_ids;

/* ---------------------------------------------------------------- the renderer as a whole */

class RendererImpl final : public br::mojom::Renderer {
public:
	void Prepare( PrepareCallback cb ) override { TAKEN("Renderer", "Prepare"); std::move(cb).Run(true); }

	void CreatePage( mojo::PendingReceiver<br::mojom::Page> page, const std::vector<uint8_t> &window,
			 CreatePageCallback cb ) override
	{
		TAKEN("Renderer", "CreatePage");
		if ( window.size() == 16 ) memcpy(g_window_uuid, window.data(), 16);
		BrEngine *eng = br_sys_engine_page();
		if ( eng == nullptr ) {
			std::move(cb).Run(false);
			return;
		}
		if ( g_first == nullptr ) g_first = eng;
		mojo::MakeSelfOwnedReceiver(std::make_unique<PageImpl>(eng, true), std::move(page), g_runner);
		printf("browser: renderer: a page for the window\n");
		std::move(cb).Run(true);
	}

	void Command( const std::string &code, CommandCallback cb ) override
	{
		TAKEN("Renderer", "Command");
		std::move(cb).Run(br_sys_command(nullptr, code));
	}

	void Query( const std::string &what, QueryCallback cb ) override { TAKEN("Renderer", "Query"); std::move(cb).Run(br_sys_query(what)); }

	void FlushStores( int32_t wait_ms, FlushStoresCallback cb ) override
	{
		TAKEN("Renderer", "FlushStores");
		std::move(cb).Run(br_sys_flush(wait_ms));
	}

	void PipClosed( int32_t session, PipClosedCallback cb ) override
	{
		TAKEN("Renderer", "PipClosed");
		players_.erase(session);
		br_pip_closed(session);
		std::move(cb).Run();
	}

	void PipPress( int32_t session, bool play, PipPressCallback cb ) override
	{
		TAKEN("Renderer", "PipPress");
		br_pip_press(session, play);
		std::move(cb).Run();
	}

	void PlayerCanvas( int32_t session, mojo::ScopedSharedBufferHandle pixels, int32_t w, int32_t h, int32_t pitch,
			   PlayerCanvasCallback cb ) override
	{
		TAKEN("Renderer", "PlayerCanvas");
		base::UnsafeSharedMemoryRegion region = mojo::UnwrapUnsafeSharedMemoryRegion(std::move(pixels));
		Player p;
		p.map = region.IsValid() ? region.Map() : base::WritableSharedMemoryMapping();
		if ( !p.map.IsValid() || (size_t)pitch * (size_t)h > p.map.size() ) {
			std::move(cb).Run(false);
			return;
		}
		p.w = w;
		p.h = h;
		p.surf = SkSurfaces::WrapPixels(SkImageInfo::MakeN32Premul(w, h), p.map.memory(), (size_t)pitch);
		bool ok = p.surf != nullptr;
		players_[session] = std::move(p);
		std::move(cb).Run(ok);
	}

	void PaintPlayer( int32_t session, int32_t player, PaintPlayerCallback cb ) override
	{
		TAKEN("Renderer", "PaintPlayer");
		auto it = players_.find(session);
		if ( it == players_.end() || it->second.surf == nullptr ) {
			std::move(cb).Run(false);
			return;
		}
		std::move(cb).Run(br_media_paint_player(player, it->second.surf->getCanvas(), it->second.w, it->second.h));
	}

	void PlayerState( int32_t player, PlayerStateCallback cb ) override
	{
		TAKEN("Renderer", "PlayerState");
		std::move(cb).Run(br_media_player_paused(player), (int64_t)br_media_player_frames(player));
	}

	void KeepFile( const std::string &uuid, const std::string &name, mojo::ScopedSharedBufferHandle bytes, uint64_t size,
		       KeepFileCallback cb ) override
	{
		TAKEN("Renderer", "KeepFile");
		base::UnsafeSharedMemoryRegion region = mojo::UnwrapUnsafeSharedMemoryRegion(std::move(bytes));
		base::WritableSharedMemoryMapping map = region.IsValid() ? region.Map() : base::WritableSharedMemoryMapping();
		std::vector<uint8_t> b;
		if ( map.IsValid() && size <= map.size() ) {
			const uint8_t *p = static_cast<const uint8_t *>(map.memory());
			b.assign(p, p + size);
		}
		std::move(cb).Run(br_file_keep(uuid, name, std::move(b)));
	}

private:
	/* The pixels of a picture-in-picture window's player, by session */
	struct Player {
		base::WritableSharedMemoryMapping	map;
		sk_sp<SkSurface>			surf;
		int					w = 0, h = 0;
	};
	std::map<int, Player>	players_;
};

/*
 * What the browser's windows read and change of what this renderer keeps
 * (br_uidata.h), on a thread of its own ("data"), as the settings window's
 * thread calls these with one process: some of them hand work to the
 * page's thread and wait for it (on_page_and_wait), and the page's thread
 * does that work when the browser next has it run its tasks (Pump). On
 * the page's thread they would wait for themselves
 */
base::Thread	*g_data_thread;

class DataImpl final : public br::mojom::Data {
public:
	void Ask( const std::string &op, const std::string &arg, AskCallback cb ) override
	{
		std::move(cb).Run(br_rproc_data(op, arg));
	}
};

/* On the IO thread: what the browser's window thread asks while the main thread may be held */
class ControlImpl final : public br::mojom::Control {
public:
	explicit ControlImpl( BrEngine **eng ) : eng_(eng) {}

	void StopScript() override
	{
		if ( *eng_ != nullptr ) ( *eng_ )->stop_script();
	}

	void ReportScript() override
	{
		if ( *eng_ != nullptr ) ( *eng_ )->report_script();
	}

	void StopLoad( bool now, uint32_t seq ) override
	{
		if ( seq != 0 ) g_stop_seq = seq;
		br_sys_stop(now);
	}

	/*
	 * The stores are written on a thread of their own: their writes are
	 * calls to the network process, whose pipes this IO thread carries
	 * and must not wait on
	 */
	void Closing() override
	{
		pthread_t th;
		pthread_attr_t at;
		pthread_attr_init(&at);
		pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
		if ( pthread_create(&th, &at, []( void * ) -> void * {
			br_sys_closing();
			end_now("the browser let it go");
			return nullptr;
		}, nullptr) != 0 ) {
			br_sys_closing();
			end_now("the browser let it go");
		}
		pthread_attr_destroy(&at);
	}

	void Busy( BusyCallback cb ) override { std::move(cb).Run(br_loop_busy_ms()); }

	void NetworkAgain( mojo::PendingRemote<br::mojom::Network> network,
			   mojo::PendingRemote<br::mojom::Store> store ) override
	{
		br_net_remote_attach(std::move(network));
		br_store_remote_attach(std::move(store));
	}

private:
	BrEngine	**eng_;
};

mojo::Receiver<br::mojom::Renderer>	*g_renderer_recv;
RendererImpl			*g_renderer;

}  // namespace

/* The pages' turns that are due (all of them: every page that has begun its turns), taken; in how many ms the next is */
long turns_due( bool all_now )
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	long now = (long)t.tv_sec * 1000 + t.tv_nsec / 1000000, next = BR_WAIT_MOST_MS;
	std::vector<PageImpl *> all = g_pages;
	for ( PageImpl *p : all ) {
		if ( std::find(g_pages.begin(), g_pages.end(), p) == g_pages.end() ) continue;
		if ( p->next_turn() <= now || ( all_now && p->turning() ) ) p->turn_now(now);
		if ( std::find(g_pages.begin(), g_pages.end(), p) == g_pages.end() ) continue;
		next = std::min(next, p->next_turn() - now);
	}
	return next < 0 ? 0 : next;
}

/* ---------------------------------------------------------------- the renderer's life */

/* Threads' types set without the control groups' paths (port/base/base_tessronos.cc) */
void br_thread_types_own();

void br_renderer_main()
{
	br_sys_renderer_setup();
	/* before any thread is started or given a type: the renderer may not look at paths once it is restricted */
	br_thread_types_own();
	br_base_up();
	br_start_mark("the base and Mojo");
	int fd = br_sys_take(30000);
	if ( fd < 0 ) {
		printf("browser: renderer: no channel was handed to it\n");
		_exit(1);
	}
	br_start_mark("the channel handed over");
	g_runner = base::MakeRefCounted<IpcRunner>();
	mojo::IncomingInvitation inv = mojo::IncomingInvitation::Accept(
		mojo::PlatformChannelEndpoint(mojo::PlatformHandle(base::ScopedFD(fd))));
	mojo::ScopedMessagePipeHandle rp = inv.ExtractMessagePipe(br::mojom::kRendererPipe);
	mojo::ScopedMessagePipeHandle cp = inv.ExtractMessagePipe(br::mojom::kControlPipe);
	mojo::ScopedMessagePipeHandle hp = inv.ExtractMessagePipe(br::mojom::kHostPipe);
	mojo::ScopedMessagePipeHandle dp = inv.ExtractMessagePipe(br::mojom::kDataPipe);
	mojo::ScopedMessagePipeHandle np = inv.ExtractMessagePipe(br::mojom::kNetworkPipe);
	mojo::ScopedMessagePipeHandle sp = inv.ExtractMessagePipe(br::mojom::kStorePipe);
	scoped_refptr<base::SingleThreadTaskRunner> io = br_proc_io_runner();
	/* the network is the network process's: asked there from the first request on */
	br_net_remote_attach(mojo::PendingRemote<br::mojom::Network>(std::move(np), 0));
	/* and so are the stores' objects: what Blink reads back as it comes up is asked there */
	br_store_remote_attach(mojo::PendingRemote<br::mojom::Store>(std::move(sp), 0));

	g_host = mojo::SharedRemote<br::mojom::Host>(mojo::PendingRemote<br::mojom::Host>(std::move(hp), 0), io);
	g_caller = new base::Thread("host calls");
	if ( !g_caller->Start() ) g_caller = nullptr;
	io->PostTask(FROM_HERE, base::BindOnce([]( mojo::ScopedMessagePipeHandle h ) {
		mojo::MakeSelfOwnedReceiver(std::make_unique<ControlImpl>(&g_first),
					    mojo::PendingReceiver<br::mojom::Control>(std::move(h)));
	}, std::move(cp)));
	br_start_mark("the pipes");

	/* Blink at once: the next page of another site is to come without waiting for it */
	if ( !br_sys_engine_up() ) _exit(2);

	g_renderer = new RendererImpl;
	g_renderer_recv = new mojo::Receiver<br::mojom::Renderer>(
		g_renderer, mojo::PendingReceiver<br::mojom::Renderer>(std::move(rp)), g_runner);
	g_renderer_recv->set_disconnect_handler(base::BindOnce([] { end_now("the browser has gone"); }));
	g_data_thread = new base::Thread("data");
	if ( !g_data_thread->Start() ) _exit(3);
	g_data_thread->task_runner()->PostTask(FROM_HERE, base::BindOnce([]( mojo::ScopedMessagePipeHandle h ) {
		mojo::MakeSelfOwnedReceiver(std::make_unique<DataImpl>(), mojo::PendingReceiver<br::mojom::Data>(std::move(h)));
	}, std::move(dp)));
	/* what the start opens by name done: the page's script runs only after this */
	br_sandbox_renderer();
	printf("browser: renderer: ready\n");
	g_runner->run(g_quit, &turns_due);
	br_sys_closing();
	fflush(stdout);
	_exit(0);
}

bool br_rproc_window( int *x, int *y, int *w, int *h )
{
	std::lock_guard<std::mutex> hold(g_win_lock);
	*x = g_win[0];
	*y = g_win[1];
	*w = g_win[2];
	*h = g_win[3];
	return g_has_win;
}

void br_rproc_screen( int *w, int *h )
{
	std::lock_guard<std::mutex> hold(g_win_lock);
	*w = g_screen[0];
	*h = g_screen[1];
}

void br_rproc_window_uuid( unsigned char id[16] )
{
	memcpy(id, g_window_uuid, 16);
}

bool br_rproc_ask_page( int kind, const std::string &message, const std::string &deflt, std::string *answer )
{
	bool ok = false;
	std::string a;
	if ( !on_host([&] { return g_host->AskPage(kind, message, deflt, &ok, &a); }) ) return false;
	if ( answer != nullptr ) *answer = a;
	return ok;
}

bool br_rproc_ask_permission( const std::string &origin, const std::string &what )
{
	bool given = false;
	return on_host([&] { return g_host->AskPermission(origin, what, &given); }) && given;
}

int br_rproc_choose( const std::vector<std::string> &names, int chosen )
{
	int32_t got = -1;
	if ( !on_host([&] { return g_host->Choose(names, chosen, &got); }) ) return -1;
	return got;
}

bool br_rproc_choose_objects( bool multiple, std::vector<BrObjLink> *out )
{
	bool ok = false;
	std::vector<std::vector<uint8_t>> ids;
	std::vector<std::string> names;
	out->clear();
	if ( !on_host([&] { return g_host->ChooseObjects(multiple, &ok, &ids, &names); }) ) return false;
	for ( size_t i = 0; i < ids.size() && i < names.size(); i++ ) {
		BrObjLink l;
		if ( ids[i].size() != 16 ) continue;
		memcpy(l.id, ids[i].data(), 16);
		l.name = names[i];
		out->push_back(l);
	}
	return ok && !out->empty();
}

bool br_rproc_pick_color( uint32_t *color )
{
	bool ok = false;
	uint32_t c = 0;
	if ( !on_host([&] { return g_host->PickColor(&ok, &c); }) ) return false;
	*color = c;
	return ok;
}

bool br_rproc_share_choose( const std::string &origin, const BrShareAsk &ask, BrShareChoice *out )
{
	bool ok = false, audio = false;
	int32_t kind = 0, wid = 0, mode = 0, pid = 0, screen = 0, sx = 0, sy = 0, sw = 0, sh = 0;
	uint32_t serial = 0, ident = 0;
	std::string name, out_name;
	if ( !on_host([&] {
		     return g_host->ShareChoose(origin, ask.audio, ask.no_system_audio, ask.own_audio_out, ask.no_self,
						ask.no_screen, ask.prefer, &ok, &kind, &wid, &serial, &name, &audio, &mode, &pid,
						&screen, &out_name, &ident, &sx, &sy, &sw, &sh);
	     }) ) {
		return false;
	}
	out->kind = kind;
	out->wid = wid;
	out->serial = serial;
	out->name = name;
	out->audio = audio;
	out->mon_mode = mode;
	out->mon_pid = pid;
	out->screen = screen;
	out->out_name = out_name;
	out->out_ident = ident;
	out->sx = sx;
	out->sy = sy;
	out->sw = sw;
	out->sh = sh;
	return ok;
}

/* The shared screen's rectangle now, as the browser finds it (br_share_screen_rect) */
bool br_rproc_share_screen_rect( const BrShareChoice &c, int *x, int *y, int *w, int *h )
{
	bool there = false;
	int32_t rx = 0, ry = 0, rw = 0, rh = 0;
	if ( !on_host([&] { return g_host->ShareScreenRect(c.screen, c.out_name, c.out_ident, &there, &rx, &ry, &rw, &rh); }) ) {
		return false;
	}
	*x = rx;
	*y = ry;
	*w = rw;
	*h = rh;
	return there;
}

void br_rproc_main_command( const std::string &code )
{
	(void)on_host([&] { return g_host->MainCommand(code); });
}

void br_rproc_ext_ask_load()
{
	(void)on_host([&] { return g_host->ExtAskLoad(); });
}

bool br_rproc_stop_asked( unsigned seq )
{
	return seq != 0 && g_stop_seq.load() == seq;
}

/* An address or a card chosen on the browser's panel: only the one chosen comes here (br_data.h) */
bool br_rproc_autofill_choose( bool card, const std::string &origin, std::vector<std::string> *fields )
{
	bool ok = false;
	fields->clear();
	if ( !on_host([&] { return g_host->AutofillChoose(card, origin, &ok, fields); }) ) return false;
	return ok;
}

/*
 * A download of a page here handed to the browser, which keeps it: false
 * when the browser did not take it. The call waits for the browser to have
 * the bytes, so the renderer may be ended once it returns.
 */
bool br_rproc_keep_download( const std::string &name, const std::string &bytes )
{
	base::UnsafeSharedMemoryRegion region = base::UnsafeSharedMemoryRegion::Create(bytes.empty() ? 1 : bytes.size());
	if ( !region.IsValid() ) return false;
	{
		base::WritableSharedMemoryMapping map = region.Map();
		if ( !map.IsValid() ) return false;
		if ( !bytes.empty() ) memcpy(map.memory(), bytes.data(), bytes.size());
	}
	bool taken = false;
	mojo::ScopedSharedBufferHandle h = mojo::WrapUnsafeSharedMemoryRegion(std::move(region));
	if ( !on_host([&] { return g_host->KeepDownload(name, std::move(h), (uint64_t)bytes.size(), &taken); }) ) {
		return false;
	}
	return taken;
}

/*
 * A download from the network handed to the browser at its response
 * (br_net_fetch_give): the browser goes on with the request and takes the
 * bytes in as they come. The call waits for the browser to have it, so
 * the renderer may be ended once it returns.
 */
bool br_rproc_download( br::mojom::DownloadHeadPtr head, std::vector<uint8_t> first, bool ended, bool ok,
			mojo::PendingReceiver<br::mojom::NetFetchClient> rest )
{
	bool taken = false;
	if ( !on_host([&] {
		return g_host->Download(std::move(head), first, ended, ok, std::move(rest), &taken);
	}) ) {
		return false;
	}
	return taken;
}

/* A channel to the GPU process (port/gpu/br_gpu_client.cc), asked of the browser */
bool br_rproc_gpu_channel( br::mojom::GpuChannelInfoPtr *out )
{
	return on_host([&] { return g_host->GpuChannel(out); });
}

/*
 * The same, not waited for (the browser may be starting the GPU process
 * again): the answer comes to the main thread
 */
void br_rproc_gpu_channel_later( base::OnceCallback<void(br::mojom::GpuChannelInfoPtr)> cb )
{
	if ( !g_host || !g_runner ) {
		std::move(cb).Run(nullptr);
		return;
	}
	g_host->GpuChannel(base::BindPostTask(g_runner, std::move(cb)));
}

/* From any thread: work for the main thread */
void br_rproc_post_main( base::OnceClosure work )
{
	if ( g_runner ) g_runner->PostTask(FROM_HERE, std::move(work));
}

/* From any thread: every page of the renderer drawn again whole, on the main thread */
void br_rproc_redraw_all()
{
	br_rproc_post_main(base::BindOnce([] {
		std::vector<PageImpl *> all = g_pages;
		for ( PageImpl *p : all ) {
			if ( std::find(g_pages.begin(), g_pages.end(), p) != g_pages.end() ) p->Redraw();
		}
	}));
}
