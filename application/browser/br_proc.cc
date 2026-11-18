/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_proc.cc
 *	ブラウザ: the browser's side of the renderers (design 14.13).
 *
 *	The browser's process is Mojo's broker. A renderer is started as the
 *	program again (br_sys_spawn); one end of a stream pair (socketpair)
 *	is handed to it (br_sys_give), and an invitation with three pipes
 *	goes over the other: Renderer and Page, which the engine thread calls,
 *	Control, which the window's thread may call while the engine thread
 *	waits in a call, and Host, which the renderer calls for the panels.
 *	Host is answered on a thread of its own, so that a panel the page
 *	asks for while the engine thread is in a call to the renderer is
 *	shown, and the engine thread's loop goes on while a panel waits
 *	(the colour picker is answered by a press on the page).
 *
 *	A page of a renderer is a RemotePage: a BrEngine whose every call is
 *	a call to the renderer, and whose calls that need an answer wait for
 *	it in the engine thread's loop (call_wait). What the page's state is after a
 *	call comes back with it and is kept, so that what the window's loop
 *	asks next (the title, where it asked to go, ...) is answered here. The
 *	page is painted by the renderer into memory both processes map (a
 *	memory object of the system); the window takes the rectangles it is
 *	told changed from there.
 *
 *	The network process (port/net/net_service_tessronos.cc) is started
 *	before the first renderer, on a thread of its own ("network"), which
 *	holds its pipe (NetworkService). Each renderer is given two pipes to
 *	it with its invitation, one to the network (kNetworkPipe) and one to
 *	the stores of the pages' storage (kStorePipe), whose other ends the
 *	browser hands to the network process (Connect). When the network
 *	process ends otherwise than with the browser, that thread starts it
 *	again and gives every renderer pipes to the new one
 *	(Control.NetworkAgain).
 *
 *	One renderer is kept ready, started while the page shown is used, as
 *	starting a program of this size and bringing up Blink take seconds.
 *	How many renderers there may be follows the machine's memory: 3 with
 *	4 GB, 6 with 8 GB, in proportion, 2 at the least.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <pthread.h>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "base/functional/callback_helpers.h"
#include "base/timer/timer.h"
#include "base/task/task_observer.h"
#include "base/task/current_thread.h"
#include "base/functional/bind.h"
#include "base/task/bind_post_task.h"
#include "base/memory/unsafe_shared_memory_region.h"
#include "base/memory/shared_memory_mapping.h"
#include "base/message_loop/message_pump_type.h"
#include "base/run_loop.h"
#include "base/task/single_thread_task_executor.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/thread.h"
#include "mojo/core/embedder/configuration.h"
#include "mojo/core/embedder/embedder.h"
#include "mojo/core/embedder/scoped_ipc_support.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "mojo/public/cpp/bindings/shared_remote.h"
#include "mojo/public/cpp/platform/platform_channel.h"
#include "mojo/public/cpp/system/invitation.h"
#include "mojo/public/c/system/invitation.h"
#include "mojo/public/cpp/system/platform_handle.h"
#include "net/base/schemeful_site.h"
#include "url/gurl.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkPaint.h"
#include "third_party/skia/include/core/SkPixmap.h"

#include "application/browser/mojom/br_process.mojom.h"

#include "br.h"
#include "br_proc.h"
#include "port/media/br_share.h"
#include "port/net/br_objfile.h"
#include "port/net/br_data.h"
#include "br_uidata.h"

/* br_main.cc: the browser's loop given a command, and the panel of 〈拡張機能を読み込む〉 */
void br_main_command( const std::string &code );
void br_ext_ask_load();

namespace {

base::Thread				*g_io;		/* where Mojo watches the channels */
std::unique_ptr<mojo::core::ScopedIPCSupport>	g_ipc;

long mono_ms()
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/*
 * A message from another process that this process found bad (its check
 * of the bindings failed, or ipcz's own): said with the process's kind,
 * which tells the side that refused it, and what Mojo says of it (the
 * interface, the method, a request or a response, the error). Only the
 * first few; the pipe is closed by Mojo as before.
 */
void bad_message( const MojoProcessErrorDetails *d )
{
	static std::atomic<int> told{0};
	if ( d == nullptr || ( d->flags & MOJO_PROCESS_ERROR_FLAG_DISCONNECTED ) != 0 ) return;
	if ( told.fetch_add(1) >= 4 ) return;
	const char *kind = br_is_renderer ? "renderer" : br_is_network ? "network process" : br_is_gpu ? "GPU process" : "browser";
	printf("browser: mojo: the %s %d found a bad message (at %ld ms): %.*s\n", kind, (int)getpid(), mono_ms(),
	       (int)std::min<uint32_t>(d->error_message_length, 400), d->error_message != nullptr ? d->error_message : "");
}

}  // namespace

void br_mojo_init()
{
	if ( !br_multi_process ) {
		/* the pages in this process: Mojo within it alone */
		mojo::core::Init();
		return;
	}
	mojo::core::Configuration config;
	config.is_broker_process = !br_is_renderer && !br_is_network && !br_is_gpu;
	mojo::core::Init(config);
	(void)MojoSetDefaultProcessErrorHandler(bad_message, nullptr);
	g_io = new base::Thread("mojo");
	if ( !g_io->StartWithOptions(base::Thread::Options(base::MessagePumpType::IO, 0)) ) {
		printf("browser: the thread of the channels did not start\n");
		return;
	}
	g_ipc = std::make_unique<mojo::core::ScopedIPCSupport>(g_io->task_runner(),
							     mojo::core::ScopedIPCSupport::ShutdownPolicy::FAST);
}

/* The IO thread's task runner, for the renderer's side (br_rproc.cc) */
scoped_refptr<base::SingleThreadTaskRunner> br_proc_io_runner()
{
	return g_io != nullptr ? g_io->task_runner() : nullptr;
}

namespace {

/* ---------------------------------------------------------------- the panels a renderer asks for */

/* A channel to the GPU process for the renderer 'pid', answered on the thread the call came on (below) */
void gpu_channel_for( int pid, br::mojom::Host::GpuChannelCallback cb );

/* ---------------------------------------------------------------- the downloads handed over */

/*
 * The thread the requests of the downloads handed over (Host.Download) are
 * answered on: it only passes their pieces on and answers that they are
 * taken, so a panel the panels' thread waits on holds no download up.
 */
base::Thread *dl_thread()
{
	static base::Thread *t = [] {
		auto *th = new base::Thread("downloads");
		base::Thread::Options o;
		o.message_pump_type = base::MessagePumpType::IO;
		if ( !th->StartWithOptions(std::move(o)) ) printf("browser: the downloads' thread did not start\n");
		return th;
	}();
	return t;
}

/*
 * One download's request, now the browser's: each piece goes to what
 * takes it in (br_download_bytes), and is answered once that has taken
 * it, which lets the network process read on; the end, or the pipe
 * closing (the network process gone), ends it. Its number lets it be let
 * go (the pipe closed: the network process drops the request) when the
 * download stops here first.
 */
class DownloadClient final : public br::mojom::NetFetchClient {
public:
	DownloadClient( int id, std::shared_ptr<BrDownload> d, mojo::PendingReceiver<br::mojom::NetFetchClient> r )
		: id_(id), d_(std::move(d)), recv_(this, std::move(r))
	{
		live()[id_] = this;
		recv_.set_disconnect_handler(base::BindOnce(&DownloadClient::end, base::Unretained(this), false));
	}
	~DownloadClient() override { live().erase(id_); }

	/* The downloads under way here by their numbers, on this thread */
	static std::map<int, DownloadClient *> &live()
	{
		static auto *m = new std::map<int, DownloadClient *>();
		return *m;
	}

	void Response( br::mojom::NetAnswerPtr ) override {}

	void Piece( const std::vector<uint8_t> &data, PieceCallback taken ) override
	{
		auto *cb = new PieceCallback(std::move(taken));
		scoped_refptr<base::SingleThreadTaskRunner> here = base::SingleThreadTaskRunner::GetCurrentDefault();
		br_download_bytes(d_, std::string(data.begin(), data.end()), [here, cb] {
			here->PostTask(FROM_HERE, base::BindOnce([]( PieceCallback *cb ) {
				std::move(*cb).Run();
				delete cb;
			}, cb));
		});
	}

	void Done( br::mojom::NetAnswerPtr answer, const std::vector<uint8_t> &body ) override
	{
		if ( !body.empty() ) br_download_bytes(d_, std::string(body.begin(), body.end()), nullptr);
		end(answer->ok && answer->error == 0);
	}

	/* Let go: the request goes with the pipe, and what takes it in has stopped already */
	void let_go() { delete this; }

private:
	void end( bool ok )
	{
		br_download_finish(d_, ok);
		delete this;
	}

	int					id_;
	std::shared_ptr<BrDownload>		d_;
	mojo::Receiver<br::mojom::NetFetchClient> recv_;
};

class HostImpl final : public br::mojom::Host {
public:
	void AskPage( int32_t kind, const std::string &message, const std::string &deflt, AskPageCallback cb ) override;

	void AskPermission( const std::string &origin, const std::string &what, AskPermissionCallback cb ) override
	{
		std::move(cb).Run(br_ask_permission(origin, what));
	}

	void Choose( const std::vector<std::string> &names, int32_t chosen, ChooseCallback cb ) override
	{
		std::move(cb).Run(br_choose(names, chosen));
	}

	void ChooseObjects( bool multiple, ChooseObjectsCallback cb ) override
	{
		std::vector<BrObjLink> links;
		bool ok = br_choose_objects(multiple, &links);
		std::vector<std::vector<uint8_t>> ids;
		std::vector<std::string> names;
		for ( const BrObjLink &l : links ) {
			ids.emplace_back(l.id, l.id + 16);
			names.push_back(l.name);
		}
		std::move(cb).Run(ok, ids, names);
	}

	void PickColor( PickColorCallback cb ) override
	{
		uint32_t c = 0;
		bool ok = br_pick_color(&c);
		std::move(cb).Run(ok, c);
	}

	void ShareChoose( const std::string &origin, bool audio, bool no_system_audio, bool own_audio_out, bool no_self,
			  bool no_screen, int32_t prefer, ShareChooseCallback cb ) override
	{
		BrShareAsk a;
		BrShareChoice c;
		a.audio = audio;
		a.no_system_audio = no_system_audio;
		a.own_audio_out = own_audio_out;
		a.no_self = no_self;
		a.no_screen = no_screen;
		a.prefer = prefer;
		bool ok = br_share_choose(origin, a, &c);
		std::move(cb).Run(ok, c.kind, c.wid, c.serial, c.name, c.audio, c.mon_mode, c.mon_pid, c.screen, c.out_name,
				  c.out_ident, c.sx, c.sy, c.sw, c.sh);
	}

	void ShareScreenRect( int32_t screen, const std::string &out_name, uint32_t out_ident,
			      ShareScreenRectCallback cb ) override
	{
		BrShareChoice c;
		c.kind = BR_SHARE_SCREEN;
		c.screen = screen;
		c.out_name = out_name;
		c.out_ident = out_ident;
		int x = 0, y = 0, w = 0, h = 0;
		bool there = br_share_screen_rect(c, &x, &y, &w, &h);
		std::move(cb).Run(there, x, y, w, h);
	}

	void Progress( int64_t got, int64_t total, ProgressCallback cb ) override
	{
		std::move(cb).Run(br_sys_progress((long)got, (long)total));
	}

	void MainCommand( const std::string &code, MainCommandCallback cb ) override
	{
		br_main_command(code);
		std::move(cb).Run();
	}

	void ExtAskLoad( ExtAskLoadCallback cb ) override
	{
		br_ext_ask_load();
		std::move(cb).Run();
	}

	/* The addresses and cards are read here and only the one the user chose goes to the renderer */
	void AutofillChoose( bool card, const std::string &origin, AutofillChooseCallback cb ) override
	{
		std::vector<std::string> fields;
		bool ok = br_autofill_choose(card, origin, &fields);
		std::move(cb).Run(ok, fields);
		for ( std::string &f : fields ) std::fill(f.begin(), f.end(), '\0');
	}

	/* A download of a renderer's page: the bytes taken before the answer, then kept here (br_keep_download) */
	void KeepDownload( const std::string &name, mojo::ScopedSharedBufferHandle bytes, uint64_t size,
			   KeepDownloadCallback cb ) override
	{
		std::string got;
		bool taken = false;
		base::UnsafeSharedMemoryRegion region = mojo::UnwrapUnsafeSharedMemoryRegion(std::move(bytes));
		if ( region.IsValid() && size <= region.GetSize() ) {
			base::WritableSharedMemoryMapping map = region.Map();
			if ( map.IsValid() ) {
				got.assign((const char *)map.memory(), (size_t)size);
				taken = true;
			}
		}
		std::string shown = name.empty() ? std::string("download") : name;
		std::move(cb).Run(taken);
		if ( !taken ) {
			printf("browser: a renderer's download %s could not be taken\n", shown.c_str());
			return;
		}
		printf("browser: a renderer's download %s taken, %zu bytes\n", shown.c_str(), got.size());
		br_keep_download(shown, std::move(got));
	}

	/*
	 * A download from the network handed over at its response: it is the
	 * browser's from here, its request answered on the downloads' thread
	 * and its bytes taken in as they come (br_download_begin)
	 */
	void Download( br::mojom::DownloadHeadPtr head, const std::vector<uint8_t> &first, bool ended, bool ok,
		       mojo::PendingReceiver<br::mojom::NetFetchClient> rest, DownloadCallback cb ) override
	{
		static std::atomic<int> next{0};
		int id = ++next;
		base::Thread *t = dl_thread();
		scoped_refptr<base::SingleThreadTaskRunner> runner = t->task_runner();
		std::string shown = head->name.empty() ? std::string("download") : head->name;
		printf("browser: a renderer's download %s handed over (%zu bytes came before, %s)\n", shown.c_str(),
		       first.size(), ended ? "the whole" : rest.is_valid() ? "the rest to come" : "no more");
		std::shared_ptr<BrDownload> d = br_download_begin(shown, head->url, head->total, head->status, [runner, id] {
			runner->PostTask(FROM_HERE, base::BindOnce([]( int id ) {
				auto it = DownloadClient::live().find(id);
				if ( it != DownloadClient::live().end() ) it->second->let_go();
			}, id));
		});
		std::move(cb).Run(true);
		if ( d == nullptr ) return;	/* not taken in: the pipe closes with 'rest', and the request goes */
		if ( !first.empty() ) br_download_bytes(d, std::string(first.begin(), first.end()), nullptr);
		if ( ended || !rest.is_valid() ) {
			br_download_finish(d, ended && ok);
			return;
		}
		runner->PostTask(FROM_HERE, base::BindOnce([]( int id, std::shared_ptr<BrDownload> d,
							       mojo::PendingReceiver<br::mojom::NetFetchClient> r ) {
			/* stopped already (its let-go came first, and found nothing): the request goes with the pipe */
			if ( br_download_over(d) ) return;
			new DownloadClient(id, std::move(d), std::move(r));
		}, id, std::move(d), std::move(rest)));
	}

	void GpuChannel( GpuChannelCallback cb ) override;
};

/* The panels' thread and the renderers' Host pipes bound there */
base::Thread				*g_host_thread;
HostImpl				*g_host;
mojo::ReceiverSet<br::mojom::Host, int>	*g_hosts;	/* each with its renderer's number */
base::Thread				*g_data_thread;	/* where the windows' data calls are made (br_proc_data) */

/* ---------------------------------------------------------------- the renderers */

struct Proc {
	int					pid = -1;
	mojo::Remote<br::mojom::Renderer>	renderer;	/* bound on the engine thread */
	mojo::SharedRemote<br::mojom::Control>	control;	/* any thread */
	mojo::SharedRemote<br::mojom::Data>	data;		/* the data calls' thread (br_proc_data) */
	mojo::PendingRemote<br::mojom::Renderer> pending;
	int					pages = 0;
	std::atomic<bool>			gone{false};
	std::atomic<bool>			closing{false};
	/*
	 * The site of the pages it shows, which the network process answers its
	 * passwords and passkeys for (NetworkService.SetSite): given before its
	 * first page is fetched, and fixed once one is shown (site_lock)
	 */
	std::string				site;
	bool					site_fixed = false;
};

std::mutex			g_lock;		/* g_procs and the renderer being started */
std::condition_variable		g_started;
std::vector<Proc *>		g_procs;
Proc				*g_ready = nullptr;	/* the one kept ready, not showing a page */
bool				g_starting = false;	/* one is being started for g_ready */
std::string			g_lines;
unsigned char			g_window[16];
base::SingleThreadTaskExecutor	*g_main;	/* the engine thread's, for its remotes */
pthread_t			g_main_thread;	/* and the thread itself */

/*
 * The call the engine thread waits in for a renderer's answer: which, to
 * which renderer, since when; said by the window's thread when the
 * engine thread has not come round for long (br_proc_calling)
 */
std::atomic<const char *>	g_calling{nullptr};
std::atomic<int>		g_calling_pid{0};
std::atomic<long>		g_calling_since{0};

struct Calling {
	Calling( const char *what, const Proc *p )
	{
		mine_ = pthread_equal(pthread_self(), g_main_thread) != 0 && g_calling.load() == nullptr;
		if ( !mine_ ) return;
		g_calling_pid = p != nullptr ? p->pid : -1;
		g_calling_since = mono_ms();
		g_calling = what;
	}
	~Calling()
	{
		if ( mine_ ) g_calling = nullptr;
	}
	bool	mine_;
};

/* From any thread: the channel of an ended renderer closed in the GPU process (below) */
void gpu_close_channel( int pid );

/* From any thread: a renderer's pipes, to the network and to the stores, handed to the network process (below) */
void net_connect( int pid, mojo::PendingReceiver<br::mojom::Network> r, mojo::PendingReceiver<br::mojom::Store> s );
/* From any thread: the site of the renderer 'pid' told to the network process (below) */
void net_site( int pid, const std::string &site );

/* Proc::site and Proc::site_fixed of every renderer */
std::mutex			g_site_lock;

/*
 * A renderer started, on the thread that calls it (seconds: the program
 * is read in): the stream pair, the program with the lines, its end
 * handed over, the invitation sent; null when it cannot be had
 */
Proc *start_one()
{
	long t0 = mono_ms();
	mojo::PlatformChannel channel;
	mojo::OutgoingInvitation inv;
	mojo::ScopedMessagePipeHandle rp = inv.AttachMessagePipe(br::mojom::kRendererPipe);
	mojo::ScopedMessagePipeHandle cp = inv.AttachMessagePipe(br::mojom::kControlPipe);
	mojo::ScopedMessagePipeHandle hp = inv.AttachMessagePipe(br::mojom::kHostPipe);
	mojo::ScopedMessagePipeHandle dp = inv.AttachMessagePipe(br::mojom::kDataPipe);
	mojo::ScopedMessagePipeHandle np = inv.AttachMessagePipe(br::mojom::kNetworkPipe);
	mojo::ScopedMessagePipeHandle sp = inv.AttachMessagePipe(br::mojom::kStorePipe);

	int pid = br_sys_spawn(std::string(BR_RENDERER_LINE) + "\n" + g_lines);
	if ( pid <= 0 ) {
		printf("browser: a renderer could not be started (%d)\n", pid);
		return nullptr;
	}
	mojo::PlatformChannelEndpoint remote = channel.TakeRemoteEndpoint();
	int fd = remote.TakePlatformHandle().ReleaseFD();
	if ( !br_sys_give(pid, fd) ) {
		printf("browser: the renderer %d was not given its channel\n", pid);
		close(fd);
		br_sys_kill(pid);
		(void)br_sys_reap(pid, 2000, nullptr);
		return nullptr;
	}
	close(fd);
	mojo::OutgoingInvitation::Send(std::move(inv), base::kNullProcessHandle, channel.TakeLocalEndpoint());
	net_connect(pid, mojo::PendingReceiver<br::mojom::Network>(std::move(np)),
		    mojo::PendingReceiver<br::mojom::Store>(std::move(sp)));

	auto *p = new Proc;
	p->pid = pid;
	p->pending = mojo::PendingRemote<br::mojom::Renderer>(std::move(rp), 0);
	p->control = mojo::SharedRemote<br::mojom::Control>(mojo::PendingRemote<br::mojom::Control>(std::move(cp), 0),
							     g_io->task_runner());
	p->data = mojo::SharedRemote<br::mojom::Data>(mojo::PendingRemote<br::mojom::Data>(std::move(dp), 0),
						       g_io->task_runner());
	g_host_thread->task_runner()->PostTask(FROM_HERE, base::BindOnce(
		[]( mojo::ScopedMessagePipeHandle h, int pid ) {
			g_hosts->Add(g_host, mojo::PendingReceiver<br::mojom::Host>(std::move(h)), pid);
		}, std::move(hp), pid));
	printf("browser: renderer %d started in %ld ms\n", pid, mono_ms() - t0);
	return p;
}

/* A renderer made ready on a thread of its own, unless there is one already */
void start_ready()
{
	{
		std::lock_guard<std::mutex> hold(g_lock);
		if ( g_ready != nullptr || g_starting ) return;
		g_starting = true;
	}
	std::thread([] {
		Proc *p = start_one();
		std::lock_guard<std::mutex> hold(g_lock);
		g_starting = false;
		if ( p != nullptr ) {
			g_procs.push_back(p);
			g_ready = p;
		}
		g_started.notify_all();
	}).detach();
}

/* The renderer kept ready taken for a page (it is waited for while it starts); null when there is none */
Proc *take_ready()
{
	std::unique_lock<std::mutex> hold(g_lock);
	if ( g_ready == nullptr && !g_starting ) {
		hold.unlock();
		start_ready();
		hold.lock();
	}
	g_started.wait(hold, [] { return g_ready != nullptr || !g_starting; });
	Proc *p = g_ready;
	g_ready = nullptr;
	return p;
}

/* For how long the renderer's main thread has been in its work (its IO thread answers), from any thread */
long busy_ms( Proc *p )
{
	int64_t ms = 0;
	if ( p->gone || !p->control->Busy(&ms) ) return 0;
	return (long)ms;
}

/* A call of Renderer that waits for its answer (below) */
template <typename Cb, typename Send, typename... Out>
bool renderer_wait( Proc *p, const char *what, Send send, Out *... out );

/*
 * A renderer ended and let go: what it keeps written first unless it has
 * failed. One whose main thread is held (a script that does not end)
 * would never answer FlushStores: it is told Closing at once, which its
 * IO thread carries out (it writes its stores there and ends), and it is
 * ended when it has not within the time
 */
void end_proc( Proc *p, bool flush )
{
	long held = flush && !p->gone ? busy_ms(p) : 0;
	if ( held > BR_HELD_MS ) {
		printf("browser: renderer %d is held in its work for %ld ms: not asked to write what it keeps\n", p->pid, held);
		flush = false;
	}
	if ( p->renderer.is_bound() && flush && !p->gone ) {
		bool done = false;
		long t0 = mono_ms();
		(void)renderer_wait<br::mojom::Renderer::FlushStoresCallback>(p, "FlushStores",
				[&]( auto cb ) {
					p->renderer->FlushStores(5000, std::move(cb));
				}, &done);
		printf("browser: renderer %d wrote what it keeps in %ld ms%s\n", p->pid, mono_ms() - t0,
		       done ? "" : " (not all)");
	}
	if ( !p->gone ) p->control->Closing();
	int status = 0;
	if ( !br_sys_reap(p->pid, 5000, &status) ) {
		printf("browser: renderer %d did not end: it is ended\n", p->pid);
		br_sys_kill(p->pid);
		(void)br_sys_reap(p->pid, 5000, &status);
	}
	printf("browser: renderer %d has ended (status 0x%x)\n", p->pid, status);
	gpu_close_channel(p->pid);
	/* the record stays: the pages that were its own still name it */
	p->gone = true;
	p->renderer.reset();
	std::lock_guard<std::mutex> hold(g_lock);
	g_procs.erase(std::remove(g_procs.begin(), g_procs.end(), p), g_procs.end());
}

/* ---------------------------------------------------------------- calls that wait for their answer */

/*
 * The browser's calls of Renderer and Page are not synchronous: a
 * renderer's main thread is never woken in a wait of its own to take one
 * (a script, a frame callback or a layout that waits for the network or
 * the browser would otherwise run them inside itself). A call whose answer
 * the browser needs is sent, and the engine thread waits for the answer in
 * its task loop, where the answer comes, as for Navigate. Meanwhile the
 * renderer may wait for the browser (Host), which the panels' thread
 * answers, so the two never wait for each other.
 *
 * The answer, as the call's callback gives it: its values, whether it came
 * (given) and whether the callback has gone (done: run, or let go with the
 * pipe)
 */
template <typename Sig> struct Answer;
template <typename... A> struct Answer<void( A... )> {
	bool				done = false, given = false;
	std::tuple<std::decay_t<A>...>	v;

	static void take( std::shared_ptr<Answer> a, base::ScopedClosureRunner, A... args )
	{
		a->v = std::tuple<std::decay_t<A>...>(std::forward<A>(args)...);
		a->given = true;
	}
};

/* How long a call is waited for once the window goes: as long as the engine thread is given then */
constexpr long kCloseWaitMs = 3000;

/* How long a page's beforeunload handlers are waited for before it is left all the same */
constexpr long kUnloadTimeoutMs = 500;

int	g_wait_depth = 0;	/* the waits for an answer under way on the engine thread, one inside another */

/*
 * The renderer whose question is up on a panel of the browser's (Host,
 * answered on the panels' thread), 0 for none: a wait with a time limit
 * does not count the time the user is asked
 */
std::atomic<int>	g_asking_pid{0};

/*
 * Waits for 'done' in the engine thread's task loop while the renderer is
 * there ('gone' false). 中止 tells a renderer held in its work (1 second
 * or more) to end its script, and one that has not taken it in the time
 * the window gives is ended; once the window goes, a call is waited for
 * kCloseWaitMs. With 'most_ms', a call is waited for that long at most,
 * not counting the time a panel asks the user for the renderer. True when
 * the answer's callback has gone.
 */
template <typename Gone>
bool wait_answer( Proc *p, const char *what, const bool &done, Gone gone, long most_ms = 0 )
{
	int verdict = BR_WAIT_ON;
	bool told = false, late = false;
	long closing_at = 0, waited = 0, last = mono_ms();
	if ( g_wait_depth > 0 ) printf("browser: the call %s to renderer %d is made inside a wait for another\n", what, p->pid);
	g_wait_depth++;
	while ( !done && !gone() ) {
		verdict = br_nav_wait_verdict();
		if ( verdict == BR_WAIT_HELD ) break;
		if ( verdict == BR_WAIT_CLOSING ) {
			if ( closing_at == 0 ) closing_at = mono_ms();
			if ( mono_ms() - closing_at >= kCloseWaitMs ) break;
		}
		if ( verdict == BR_WAIT_STOPPING && !told ) {
			told = true;
			if ( busy_ms(p) >= 1000 ) p->control->StopScript();
		}
		long now = mono_ms();
		if ( g_asking_pid.load() != p->pid ) waited += now - last;
		last = now;
		if ( most_ms > 0 && waited >= most_ms ) {
			late = true;
			break;
		}
		(void)br_loop_wait(100);
	}
	g_wait_depth--;
	if ( done ) return true;
	if ( !gone() ) {
		if ( late ) {
			printf("browser: renderer %d has not answered %s in %ld ms: gone on without it\n", p->pid, what, most_ms);
		} else if ( verdict == BR_WAIT_HELD ) {
			printf("browser: renderer %d has not taken 中止 in its work (%s): it is ended\n", p->pid, what);
			p->closing = true;
			br_sys_kill(p->pid);
			end_proc(p, false);
		} else {
			printf("browser: the window goes while renderer %d works: its answer to %s is not waited for\n",
			       p->pid, what);
		}
	}
	return false;
}

template <typename T, size_t... I, typename... Out>
void give_answer( T &v, std::index_sequence<I...>, Out *... out )
{
	( ( *out = std::move(std::get<I>(v)) ), ... );
}

/*
 * A call to a renderer that waits for its answer: 'send' is given the
 * callback (of the type Cb the call takes) and makes the call; the
 * answer's values are put in 'out', in turn. False when no answer came
 * (the renderer went, or the wait was given up)
 */
template <typename Cb, typename Gone, typename Send, typename... Out>
bool call_wait_within( Proc *p, const char *what, long most_ms, Gone gone, Send send, Out *... out )
{
	using A = Answer<typename Cb::RunType>;
	static_assert(sizeof...(Out) == std::tuple_size<decltype(A::v)>::value, "each of the answer's values has a place");
	if ( gone() ) return false;
	Calling call_(what, p);
	auto a = std::make_shared<A>();
	send(Cb(base::BindOnce(&A::take, a, base::ScopedClosureRunner(base::BindOnce([]( std::shared_ptr<A> a ) {
		a->done = true;
	}, a)))));
	if ( !wait_answer(p, what, a->done, gone, most_ms) || !a->given ) return false;
	give_answer(a->v, std::index_sequence_for<Out...>{}, out...);
	return true;
}

template <typename Cb, typename Gone, typename Send, typename... Out>
bool call_wait( Proc *p, const char *what, Gone gone, Send send, Out *... out )
{
	return call_wait_within<Cb>(p, what, 0, gone, send, out...);
}

/* A question of the page's on a panel; the renderer's wait with a time limit stands still meanwhile (g_asking_pid) */
void HostImpl::AskPage( int32_t kind, const std::string &message, const std::string &deflt, AskPageCallback cb )
{
	std::string answer;
	g_asking_pid = g_hosts->current_context();
	bool ok = br_ask_page(kind, message, deflt, &answer);
	g_asking_pid = 0;
	std::move(cb).Run(ok, answer);
}

/* A call of Renderer that waits for its answer */
template <typename Cb, typename Send, typename... Out>
bool renderer_wait( Proc *p, const char *what, Send send, Out *... out )
{
	return call_wait<Cb>(p, what, [p] { return p->gone.load(); }, send, out...);
}

/* ---------------------------------------------------------------- the network process */

base::Thread				*g_netq;	/* where its pipe is held ("network") */
mojo::Remote<br::mojom::NetworkService>	*g_netsvc;	/* on g_netq */
std::atomic<int>			g_net_pid{0};
/* A renderer's pipes to the network process: to the network and to the stores */
struct NetPipes {
	int						pid;
	mojo::PendingReceiver<br::mojom::Network>	net;
	mojo::PendingReceiver<br::mojom::Store>		store;
};
std::vector<NetPipes>			*g_net_waiting;	/* on g_netq, while there is none */
std::mutex				g_ext_lock;
std::vector<std::string>		g_ext_allowed;	/* the extensions' objects named (br_proc_ext_allow) */
std::vector<long>			g_net_starts;	/* when it was started, for how often it fails */
bool					g_net_ending = false;

void net_gone();
std::string site_of_pid( int pid );

/* On g_netq: the network process started and its pipe bound; false when it could not be */
bool net_start()
{
	long t0 = mono_ms();
	mojo::PlatformChannel channel;
	mojo::OutgoingInvitation inv;
	mojo::ScopedMessagePipeHandle sp = inv.AttachMessagePipe(br::mojom::kNetworkServicePipe);
	int pid = br_sys_spawn(std::string(BR_NETWORK_LINE) + "\n" + g_lines);
	if ( pid <= 0 ) {
		printf("browser: the network process could not be started (%d)\n", pid);
		return false;
	}
	mojo::PlatformChannelEndpoint remote = channel.TakeRemoteEndpoint();
	int fd = remote.TakePlatformHandle().ReleaseFD();
	if ( !br_sys_give(pid, fd) ) {
		printf("browser: the network process %d was not given its channel\n", pid);
		close(fd);
		br_sys_kill(pid);
		(void)br_sys_reap(pid, 2000, nullptr);
		return false;
	}
	close(fd);
	mojo::OutgoingInvitation::Send(std::move(inv), base::kNullProcessHandle, channel.TakeLocalEndpoint());
	delete g_netsvc;
	g_netsvc = new mojo::Remote<br::mojom::NetworkService>(
		mojo::PendingRemote<br::mojom::NetworkService>(std::move(sp), 0));
	g_netsvc->set_disconnect_handler(base::BindOnce(&net_gone));
	g_net_pid = pid;
	g_net_starts.push_back(t0);
	printf("browser: network process %d started in %ld ms\n", pid, mono_ms() - t0);
	{
		std::lock_guard<std::mutex> hold(g_ext_lock);
		for ( const std::string &u : g_ext_allowed ) ( *g_netsvc )->ExtAllow(u);
	}
	for ( auto &r : *g_net_waiting ) {
		( *g_netsvc )->Connect(r.pid, std::move(r.net), std::move(r.store));
		std::string site = site_of_pid(r.pid);
		if ( !site.empty() ) ( *g_netsvc )->SetSite(r.pid, site);
	}
	g_net_waiting->clear();
	return true;
}

/*
 * On g_netq: the network process ended without being let go. It is
 * waited for and started again, and each renderer given a pipe to the
 * new one; the requests under way on the old one have failed. One that
 * keeps failing (five starts within a minute) is not started again: the
 * renderers' requests then fail.
 */
void net_gone()
{
	int pid = g_net_pid.exchange(0);
	if ( g_net_ending ) return;
	int status = 0;
	if ( pid > 0 && !br_sys_reap(pid, 2000, &status) ) {
		br_sys_kill(pid);
		(void)br_sys_reap(pid, 2000, &status);
	}
	printf("browser: the network process %d has ended (status 0x%x): started again\n", pid, status);
	long now = mono_ms();
	int recent = 0;
	for ( long t : g_net_starts ) recent += ( now - t < 60000 ) ? 1 : 0;
	if ( recent >= 5 || !net_start() ) {
		printf("browser: the network process is not started again\n");
		return;
	}
	std::vector<Proc *> all;
	{
		std::lock_guard<std::mutex> hold(g_lock);
		all = g_procs;
	}
	for ( Proc *p : all ) {
		if ( p->gone ) continue;
		mojo::PendingRemote<br::mojom::Network> n;
		mojo::PendingRemote<br::mojom::Store> s;
		( *g_netsvc )->Connect(p->pid, n.InitWithNewPipeAndPassReceiver(), s.InitWithNewPipeAndPassReceiver());
		std::string site;
		{
			std::lock_guard<std::mutex> hold(g_site_lock);
			site = p->site;
		}
		if ( !site.empty() ) ( *g_netsvc )->SetSite(p->pid, site);
		p->control->NetworkAgain(std::move(n), std::move(s));
	}
}

/* From any thread: a renderer's pipes handed to the network process */
void net_connect( int pid, mojo::PendingReceiver<br::mojom::Network> r, mojo::PendingReceiver<br::mojom::Store> s )
{
	g_netq->task_runner()->PostTask(FROM_HERE, base::BindOnce([]( int pid, mojo::PendingReceiver<br::mojom::Network> r,
								       mojo::PendingReceiver<br::mojom::Store> s ) {
		if ( g_netsvc != nullptr && g_net_pid > 0 ) {
			( *g_netsvc )->Connect(pid, std::move(r), std::move(s));
		} else {
			g_net_waiting->push_back({ pid, std::move(r), std::move(s) });
		}
	}, pid, std::move(r), std::move(s)));
}

/*
 * The site told after the pipes on the same pipe to the network process,
 * so that it comes after the renderer's Connect; while there is no network
 * process it is told when one is started again (net_gone)
 */
void net_site( int pid, const std::string &site )
{
	g_netq->task_runner()->PostTask(FROM_HERE, base::BindOnce([]( int pid, std::string site ) {
		if ( g_netsvc != nullptr && g_net_pid > 0 ) ( *g_netsvc )->SetSite(pid, site);
	}, pid, site));
}

/* The site given to the renderer 'pid', "" for none (any thread) */
std::string site_of_pid( int pid )
{
	std::lock_guard<std::mutex> hold(g_lock);
	for ( Proc *p : g_procs ) {
		if ( p->pid != pid ) continue;
		std::lock_guard<std::mutex> hold_site(g_site_lock);
		return p->site;
	}
	return std::string();
}

/* Run on g_netq and waited for */
void on_netq( base::OnceClosure work )
{
	std::mutex m;
	std::condition_variable cv;
	bool done = false;
	g_netq->task_runner()->PostTask(FROM_HERE, base::BindOnce([]( base::OnceClosure work, std::mutex *m,
								      std::condition_variable *cv, bool *done ) {
		std::move(work).Run();
		std::lock_guard<std::mutex> hold(*m);
		*done = true;
		cv->notify_all();
	}, std::move(work), &m, &cv, &done));
	std::unique_lock<std::mutex> hold(m);
	cv.wait(hold, [&] { return done; });
}

/* ---------------------------------------------------------------- the GPU process */

base::Thread				*g_gpuq;	/* where its pipes are held ("gpu") */
mojo::Remote<br::mojom::GpuService>	*g_gpusvc;	/* on g_gpuq */
std::atomic<int>			g_gpu_pid{0};
std::atomic<int>			g_gpu_last_pid{0};	/* the one started last, for the tests */
std::vector<long>			g_gpu_fails;	/* when it ended by itself or could not start, for how often */
bool					g_gpu_ending = false;
std::atomic<bool>			g_gpu_none{false};	/* not started again: the pages are drawn without it */
std::atomic<int>			g_gpu_arrivals{0};	/* times it was started again for a GPU that came */
std::atomic<int>			g_gpu_arrived_from{0};	/* the one let go of then, the last time */
std::mutex				g_gpu_lock;	/* g_gpu_renderer */
std::string				g_gpu_renderer;	/* the GL it is on */

void gpu_gone( int pid );
void gpu_again();

class GpuHostImpl final : public br::mojom::GpuHost {
public:
	/* the machine's GPU was registered after the GPU process started on the processor's GL */
	void GpuArrived( const std::string &what ) override
	{
		printf("browser: the GPU process says the machine's GPU has come (%s): it is started again\n", what.c_str());
		g_gpuq->task_runner()->PostTask(FROM_HERE, base::BindOnce(&gpu_again));
	}
};

mojo::Receiver<br::mojom::GpuHost>	*g_gpuhost;	/* on g_gpuq */
GpuHostImpl				*g_gpuhost_impl;

/* On g_gpuq: how many times within the last minute it ended by itself, this one counted */
int gpu_failed()
{
	long now = mono_ms();
	g_gpu_fails.push_back(now);
	int recent = 0;
	for ( long t : g_gpu_fails ) recent += ( now - t < 60000 ) ? 1 : 0;
	return recent;
}

/*
 * On g_gpuq: the GPU process started, its pipes bound and its GL brought
 * up (Start); false when it could not be. It is started while the first
 * renderer is, as reading the program takes seconds; the renderers'
 * calls for a channel wait on g_gpuq behind it.
 */
bool gpu_start()
{
	long t0 = mono_ms();
	long free0 = br_sys_free_mb();
	mojo::PlatformChannel channel;
	mojo::OutgoingInvitation inv;
	mojo::ScopedMessagePipeHandle sp = inv.AttachMessagePipe(br::mojom::kGpuServicePipe);
	mojo::ScopedMessagePipeHandle hp = inv.AttachMessagePipe(br::mojom::kGpuHostPipe);
	int pid = br_sys_spawn(std::string(BR_GPU_LINE) + "\n" + g_lines);
	if ( pid <= 0 ) {
		printf("browser: the GPU process could not be started (%d)\n", pid);
		(void)gpu_failed();
		return false;
	}
	mojo::PlatformChannelEndpoint remote = channel.TakeRemoteEndpoint();
	int fd = remote.TakePlatformHandle().ReleaseFD();
	if ( !br_sys_give(pid, fd) ) {
		printf("browser: the GPU process %d was not given its channel\n", pid);
		close(fd);
		br_sys_kill(pid);
		(void)br_sys_reap(pid, 2000, nullptr);
		return false;
	}
	close(fd);
	mojo::OutgoingInvitation::Send(std::move(inv), base::kNullProcessHandle, channel.TakeLocalEndpoint());
	delete g_gpusvc;
	g_gpusvc = new mojo::Remote<br::mojom::GpuService>(
		mojo::PendingRemote<br::mojom::GpuService>(std::move(sp), 0));
	if ( g_gpuhost_impl == nullptr ) g_gpuhost_impl = new GpuHostImpl;
	delete g_gpuhost;
	g_gpuhost = new mojo::Receiver<br::mojom::GpuHost>(g_gpuhost_impl,
							   mojo::PendingReceiver<br::mojom::GpuHost>(std::move(hp)));
	g_gpu_pid = pid;
	g_gpu_last_pid = pid;
	bool ok = false, on_gpu = false;
	std::string renderer, why;
	if ( !( *g_gpusvc )->Start(br_sys_gpu_mode(), &ok, &on_gpu, &renderer, &why) || !ok ) {
		printf("browser: the GPU process %d has no GPU to give (%s)\n", pid, why.c_str());
	} else {
		printf("browser: GPU process %d started in %ld ms, on %s; %ld MB free before it, %ld MB after\n", pid,
		       mono_ms() - t0, renderer.c_str(), free0, br_sys_free_mb());
		std::lock_guard<std::mutex> hold(g_gpu_lock);
		g_gpu_renderer = renderer;
	}
	g_gpusvc->set_disconnect_handler(base::BindOnce(&gpu_gone, pid));
	return ok;
}

/*
 * On g_gpuq: the GPU process 'pid' ended without being let go (it failed,
 * or was ended as one that fails is). It is waited for and started again
 * at once; the renderers find their channels lost and ask for new ones.
 * One that ended three times within a minute is not started again: the
 * pages are then drawn without it (on the processor). Nothing is done for
 * a process the browser let go of itself, or one already seen to end.
 */
void gpu_gone( int pid )
{
	if ( pid <= 0 || g_gpu_pid != pid ) return;
	g_gpu_pid = 0;
	if ( g_gpu_ending ) return;
	int status = 0;
	if ( !br_sys_reap(pid, 2000, &status) ) {
		br_sys_kill(pid);
		(void)br_sys_reap(pid, 2000, &status);
	}
	int recent = gpu_failed();
	if ( recent >= 3 ) {
		g_gpu_none = true;
		printf("browser: the GPU process %d has ended (status 0x%x), %d times within a minute: it is not started again, "
		       "the pages are drawn without it\n", pid, status, recent);
		return;
	}
	printf("browser: the GPU process %d has ended (status 0x%x): started again\n", pid, status);
	(void)gpu_start();
}

/*
 * On g_gpuq: the machine's GPU came after the GPU process started: the
 * process is let go of and another started on the GPU. This is not
 * counted as a failure.
 */
void gpu_again()
{
	int pid = g_gpu_pid;
	if ( g_gpu_ending || pid <= 0 || g_gpusvc == nullptr ) return;
	g_gpu_pid = 0;
	g_gpu_arrived_from = pid;
	g_gpusvc->set_disconnect_handler(base::OnceClosure());
	( *g_gpusvc )->Closing();
	int status = 0;
	if ( !br_sys_reap(pid, 2000, &status) ) {
		br_sys_kill(pid);
		(void)br_sys_reap(pid, 2000, &status);
	}
	g_gpu_arrivals++;
	(void)gpu_start();
}

/* On g_gpuq: the GPU process there to be asked, started again when it is not (and may be) */
bool gpu_there()
{
	if ( g_gpusvc != nullptr && g_gpu_pid > 0 ) return true;
	if ( g_gpu_ending || g_gpu_none ) return false;
	long now = mono_ms();
	int recent = 0;
	for ( long t : g_gpu_fails ) recent += ( now - t < 60000 ) ? 1 : 0;
	if ( recent >= 3 ) {
		g_gpu_none = true;
		return false;
	}
	return gpu_start();
}

void gpu_channel_for( int pid, br::mojom::Host::GpuChannelCallback cb )
{
	if ( g_gpuq == nullptr ) {
		std::move(cb).Run(nullptr);
		return;
	}
	g_gpuq->task_runner()->PostTask(FROM_HERE, base::BindOnce([]( int pid, br::mojom::Host::GpuChannelCallback cb ) {
		br::mojom::GpuChannelInfoPtr ch;
		/*
		 * a renderer may find its channel lost before the browser sees the
		 * process end: the call then fails, and the process is taken as
		 * ended (started again) and asked once more
		 */
		for ( int i = 0; i < 2; i++ ) {
			ch = nullptr;
			if ( !gpu_there() ) break;
			if ( ( *g_gpusvc )->EstablishChannel(pid, (uint64_t)pid, &ch) ) break;
			ch = nullptr;
			gpu_gone(g_gpu_pid);
		}
		std::move(cb).Run(std::move(ch));
	}, pid, base::BindPostTask(base::SingleThreadTaskRunner::GetCurrentDefault(), std::move(cb))));
}

/* From any thread: the channel of an ended renderer closed in the GPU process */
void gpu_close_channel( int pid )
{
	if ( g_gpuq == nullptr ) return;
	g_gpuq->task_runner()->PostTask(FROM_HERE, base::BindOnce([]( int pid ) {
		if ( g_gpusvc != nullptr && g_gpu_pid > 0 ) ( *g_gpusvc )->CloseChannel(pid);
	}, pid));
}

void HostImpl::GpuChannel( GpuChannelCallback cb )
{
	gpu_channel_for(g_hosts->current_context(), std::move(cb));
}

/* ---------------------------------------------------------------- a page of a renderer */

class RemotePage;

/* What the renderers asked of picture-in-picture windows, in turn, with the page that told it */
std::deque<std::pair<RemotePage *, BrPipAsk>>	g_pip;

void pip_asked( RemotePage *from, const BrPipAsk &a )
{
	g_pip.emplace_back(from, a);
}

/* The engine thread's loop, woken when a renderer tells something (a frame, a page's state) */
void wake_loop()
{
	br_loop_wake();
}

/*
 * A page's frames as they come from its renderer (FrameSink, on the IO
 * thread): the rectangles drawn and not yet copied, and whether the
 * browser holds the page's memory (from a frame until FrameDone)
 */
struct Frames {
	std::mutex		lock;
	std::vector<BrRect>	rects;
	int			top = 0, height = 0;
	bool			held = false;
	/*
	 * A frame of the window's GPU buffers (design 14.13.10): of set
	 * 'gs_set' (0: one of the page's memory), drawn into 'gs_buf'; given
	 * back only once the window shows it (or it cannot be shown)
	 */
	int			gs_set = 0, gs_buf = -1;
	std::deque<br::mojom::PageStatePtr> states;	/* what the page told it is, in turn */
};

class FrameSinkImpl final : public br::mojom::FrameSink {
public:
	explicit FrameSinkImpl( std::shared_ptr<Frames> f ) : f_(std::move(f)) {}

	void Frame( std::vector<br::mojom::RectPtr> rects, int32_t top, int32_t height ) override
	{
		std::lock_guard<std::mutex> hold(f_->lock);
		for ( auto &r : rects ) {
			BrRect b;
			b.x = r->x;
			b.y = r->y;
			b.w = r->w;
			b.h = r->h;
			f_->rects.push_back(b);
		}
		f_->top = top;
		f_->height = height;
		f_->held = true;
		f_->gs_set = 0;
		f_->gs_buf = -1;
		wake_loop();
	}

	void GsurfFrame( std::vector<br::mojom::RectPtr> rects, int32_t top, int32_t height, int32_t set,
			 int32_t buf ) override
	{
		std::lock_guard<std::mutex> hold(f_->lock);
		f_->rects.clear();
		for ( auto &r : rects ) {
			BrRect b;
			b.x = r->x;
			b.y = r->y;
			b.w = r->w;
			b.h = r->h;
			f_->rects.push_back(b);
		}
		f_->top = top;
		f_->height = height;
		f_->held = true;
		f_->gs_set = set > 0 ? set : -1;
		f_->gs_buf = buf;
		wake_loop();
	}

	void State( br::mojom::PageStatePtr s ) override
	{
		{
			std::lock_guard<std::mutex> hold(f_->lock);
			f_->states.push_back(std::move(s));
		}
		wake_loop();
	}

private:
	std::shared_ptr<Frames>	f_;
};

void gs_tick();

class RemotePage final : public BrEngine {
public:
	RemotePage( Proc *p, mojo::PendingRemote<br::mojom::Page> page ) : proc_(p), frames_(std::make_shared<Frames>())
	{
		page_.Bind(std::move(page));
		page_.set_disconnect_handler(base::BindOnce([]( RemotePage *e ) { e->lost(); }, base::Unretained(this)));
		proc_->pages++;
		/* the frames come on the IO thread, whatever the engine thread is doing */
		mojo::PendingRemote<br::mojom::FrameSink> sink;
		mojo::PendingReceiver<br::mojom::FrameSink> recv = sink.InitWithNewPipeAndPassReceiver();
		g_io->task_runner()->PostTask(FROM_HERE, base::BindOnce([]( std::shared_ptr<Frames> f,
				mojo::PendingReceiver<br::mojom::FrameSink> r ) {
			mojo::MakeSelfOwnedReceiver(std::make_unique<FrameSinkImpl>(std::move(f)), std::move(r));
		}, frames_, std::move(recv)));
		page_->Frames(std::move(sink));
	}

	~RemotePage() override {}

	Proc *proc() const { return proc_; }

	/* The number of the navigation its answer is waited for, 0 for none; from any thread */
	uint32_t nav_seq() const { return nav_seq_.load(); }

	/* The renderer has gone: nothing more is asked of it */
	void lost()
	{
		if ( dead_ ) return;
		dead_ = true;
		bool closing = proc_->closing;
		proc_->gone = true;
		if ( !closing ) printf("browser: the renderer %d of the page %s has gone\n", proc_->pid, url_.c_str());
	}

	bool ok( bool sent )
	{
		if ( !sent ) lost();
		return sent && !dead_;
	}

	bool gone() override { return dead_ || proc_->gone; }

	/*
	 * A call of Page that waits for its answer (call_wait); a renderer whose
	 * pipe has gone without an answer is lost
	 */
	template <typename Cb, typename Send, typename... Out>
	bool page_wait( const char *what, Send send, Out *... out )
	{
		if ( call_wait<Cb>(proc_, what, [this] { return gone(); }, send, out...) ) return true;
		if ( !gone() && !page_.is_connected() ) lost();
		return false;
	}

	/* What the page is after a call */
	void take( br::mojom::PageStatePtr s )
	{
		if ( !s ) return;
		std::lock_guard<std::mutex> hold(state_lock_);
		title_ = s->title;
		url_ = s->url;
		hover_ = s->hover;
		for ( std::string &n : s->navigation ) nav_.push_back(std::move(n));
		if ( !s->navigation.empty() ) {
			nav_post_ = s->nav_post;
			nav_method_ = s->nav_method;
			nav_ctype_ = s->nav_ctype;
			nav_body_ = s->nav_body;
		}
		if ( s->fullscreen_asked != 0 ) fs_asked_ = s->fullscreen_asked;
		if ( s->window_asked != 0 ) win_asked_ = s->window_asked;
		close_asked_ = s->close_asked;
		for ( auto &o : s->opened ) {
			BrOpened b;
			b.eng = new RemotePage(proc_, std::move(o->page));
			b.x = o->x;
			b.y = o->y;
			b.w = o->w;
			b.h = o->h;
			b.pip = o->pip;
			opened_.push_back(b);
		}
		for ( std::string &a : s->apart ) apart_.push_back(std::move(a));
		scroll_top_ = s->scroll_top;
		doc_width_ = s->doc_width;
		if ( s->doc_height > 0 ) height_ = s->doc_height;
		beforeunload_ = s->beforeunload;
		if ( s->hist_given ) {
			hist_states_ = std::move(s->hist_states);
			hist_urls_ = std::move(s->hist_urls);
			hist_at_ = s->hist_at;
			hist_have_ = true;
		}
		scroll_left_ = s->scroll_left;
		editable_ = s->editable;
		can_back_ = s->can_back;
		can_forward_ = s->can_forward;
		dragging_ = s->dragging;
		badge_ = s->badge;
		capture_ = s->capture;
		origin_ = s->origin;
		isolated_ = s->isolated;
		ext_actions_ = s->ext_actions;
		wv_busy_ = s->wv_busy;
		wv_installed_ = s->wv_installed;
		repaint_ = s->repaint;
		for ( auto &a : s->pip ) {
			BrPipAsk p;
			p.what = (BrPipAsk::What)a->what;
			p.session = a->session;
			p.player = a->player;
			p.w = a->w;
			p.h = a->h;
			pip_asked(this, p);
		}
	}

	/* Widevine's state, from any thread (the window's menu) */
	void wv( bool *busy, bool *installed )
	{
		std::lock_guard<std::mutex> hold(state_lock_);
		*busy = wv_busy_;
		*installed = wv_installed_;
	}

	/* The memory the page is painted into, as large as the view */
	bool canvas( int w, int h )
	{
		if ( w <= 0 || h <= 0 ) return false;
		if ( map_.IsValid() && w == cw_ && h == ch_ ) return true;
		size_t pitch = (size_t)w * 4;
		base::UnsafeSharedMemoryRegion region = base::UnsafeSharedMemoryRegion::Create(pitch * (size_t)h);
		if ( !region.IsValid() ) return false;
		base::WritableSharedMemoryMapping map = region.Map();
		if ( !map.IsValid() ) return false;
		bool done = false;
		if ( !page_wait<br::mojom::Page::SetCanvasCallback>("SetCanvas",
				[&]( auto cb ) {
					page_->SetCanvas(mojo::WrapUnsafeSharedMemoryRegion(std::move(region)), w, h, (int32_t)pitch, std::move(cb));
				}, &done)
		     || !done ) {
			return false;
		}
		map_ = std::move(map);
		cw_ = w;
		ch_ = h;
		pitch_ = pitch;
		{
			/* a frame held was of the memory before: given back unread; the renderer draws the whole view anew */
			std::lock_guard<std::mutex> hold(frames_->lock);
			frames_->rects.clear();
		}
		release();
		return true;
	}

	/* ---------------------------------------- BrEngine */

	bool opened( BrOpened *o ) override
	{
		if ( opened_.empty() ) return false;
		*o = opened_.front();
		opened_.pop_front();
		return true;
	}

	std::string apart() override
	{
		if ( apart_.empty() ) return std::string();
		std::string a = apart_.front();
		apart_.pop_front();
		return a;
	}

	void open_page( const std::string &url ) override
	{
		br::mojom::PageStatePtr s;
		if ( !gone() && page_wait<br::mojom::Page::OpenPageCallback>("OpenPage",
				[&]( auto cb ) {
					page_->OpenPage(url, std::move(cb));
				}, &s) ) take(std::move(s));
	}

	bool close_asked() override { return close_asked_; }

	/*
	 * The page closed (its unload handlers run): waited for kUnloadTimeoutMs at most, as a page
	 * whose script holds its renderer would hold the window that lets it go
	 */
	void closed() override
	{
		if ( !gone() ) (void)call_wait_within<br::mojom::Page::ClosedCallback>(proc_, "Closed", kUnloadTimeoutMs,
				[this] { return gone(); },
				[&]( auto cb ) {
					page_->Closed(std::move(cb));
				});
		proc_->pages--;
		closed_ = true;
	}

	void load( const std::string &url, const std::string &body, const std::string &charset,
		   const std::string &type ) override
	{
		br::mojom::PageStatePtr s;
		if ( !gone() && page_wait<br::mojom::Page::LoadCallback>("Load",
				[&]( auto cb ) {
					page_->Load(url, body, charset, type, std::move(cb));
				}, &s) ) take(std::move(s));
	}

	int navigate( const BrNav &nav, BrNavResult *out ) override
	{
		auto n = br::mojom::Navigation::New();
		n->target = nav.target;
		n->how = nav.how;
		n->method = nav.method;
		n->ctype = nav.ctype;
		n->body = nav.body;
		n->from = nav.from;
		n->upgraded = nav.upgraded;
		n->restore = nav.restore;
		n->restore_at = nav.restore_at;
		static std::atomic<uint32_t> s_seq{0};
		uint32_t seq = ++s_seq;
		n->seq = seq;
		nav_seq_ = seq;
		struct Unseq {
			std::atomic<uint32_t> &s;
			~Unseq() { s = 0; }
		} unseq{nav_seq_};
		/* the site it is to show, before its page's script can ask for a password */
		std::string to = nav.target;
		if ( nav.upgraded && to.find("://") == std::string::npos ) to = "https://" + to;
		give_site(to, false);
		Calling call_("Navigate", proc_);
		if ( gone() ) {
			out->outcome = BR_NAV_FAILED;
			out->url = nav.target;
			return out->outcome;
		}
		/*
		 * The answer comes when the renderer has fetched the page and made
		 * its document, which may take long (a slow server, a large body).
		 * It is waited for in the engine thread's task loop, where it
		 * comes, and the wait looks at the window each time round: 中止
		 * tells the renderer to stop, a renderer that has not taken it in
		 * the time the window gives is ended, and closing the window ends
		 * the wait at once. The answer of a wait given up is let go.
		 */
		struct Answer {
			bool			done = false;
			br::mojom::NavResultPtr	r;
			br::mojom::PageStatePtr	s;
		};
		auto a = std::make_shared<Answer>();
		page_->Navigate(std::move(n), mojo::WrapCallbackWithDefaultInvokeIfNotRun(base::BindOnce(
			[]( std::shared_ptr<Answer> a, br::mojom::NavResultPtr r, br::mojom::PageStatePtr s ) {
				a->r = std::move(r);
				a->s = std::move(s);
				a->done = true;
			}, a), nullptr, nullptr));
		if ( br_proc_test_killnav > 0 && --br_proc_test_killnav == 0 ) {
			printf("browser: renderer %d is ended as it is sent the navigation to %s (killnav)\n", proc_->pid,
			       nav.target.substr(0, 160).c_str());
			br_sys_kill(proc_->pid);
		}
		int verdict = BR_WAIT_ON;
		bool told = false;
		while ( !a->done && !gone() ) {
			verdict = br_nav_wait_verdict();
			if ( verdict == BR_WAIT_HELD || verdict == BR_WAIT_CLOSING ) break;
			if ( verdict == BR_WAIT_STOPPING && !told ) {
				told = true;
				proc_->control->StopLoad(true, seq);
				if ( busy_ms(proc_) >= 1000 ) proc_->control->StopScript();
			}
			(void)br_loop_wait(100);
		}
		if ( !a->done && !gone() ) {
			if ( verdict == BR_WAIT_HELD ) {
				printf("browser: renderer %d has not taken 中止 in its work for %s: it is ended\n", proc_->pid,
				       nav.target.substr(0, 160).c_str());
				/*
				 * ended at once: its main thread would not write its
				 * stores, and Closing would wait for it in vain
				 */
				proc_->closing = true;
				br_sys_kill(proc_->pid);
				end_proc(proc_, false);
			} else {
				printf("browser: the window goes while renderer %d works for %s\n", proc_->pid,
				       nav.target.substr(0, 160).c_str());
			}
			out->outcome = BR_NAV_STOPPED;
			out->url = nav.target;
			return out->outcome;
		}
		if ( !a->r ) {
			if ( !gone() ) lost();
			out->outcome = BR_NAV_FAILED;
			out->url = nav.target;
			return out->outcome;
		}
		br::mojom::NavResultPtr r = std::move(a->r);
		take(std::move(a->s));
		/* the site of the page shown, after the redirects: the renderer's from now on */
		if ( r->outcome == BR_NAV_SHOWN ) give_site(r->url, true);
		out->outcome = r->outcome;
		out->url = r->url;
		out->status = r->status;
		out->bytes = (long)r->bytes;
		out->error = r->error;
		out->title = r->title;
		return out->outcome;
	}

	/*
	 * The width told, not waited for: the renderer lays the page out and
	 * answers with its height, as its states tell the height after a turn.
	 * The height given is the last one told; the answer comes in the
	 * engine thread's loop, and pump() then says the page changed, so that
	 * the window lays itself out again with it. One Layout is under way at
	 * a time; a width changed meanwhile is told when it is answered.
	 */
	int layout( int width ) override
	{
		width_ = width;
		ask_layout();
		return height_;
	}

	void ask_layout()
	{
		if ( gone() || layout_asked_ || width_ == told_width_ ) return;
		layout_asked_ = true;
		told_width_ = width_;
		page_->Layout(width_, turn(), base::BindOnce([]( RemotePage *e, int32_t h, br::mojom::PageStatePtr s ) {
			e->layout_asked_ = false;
			e->take(std::move(s));
			e->height_ = h;
			e->laid_out_ = true;
			e->ask_layout();
			wake_loop();
		}, base::Unretained(this)));
	}

	/*
	 * Whether the page's memory holds the view (top, height) and the
	 * browser holds it (a frame came and FrameDone is not said); when not,
	 * the whole view is asked for once, and nothing is copied meanwhile
	 */
	bool readable( int top, int height )
	{
		{
			std::lock_guard<std::mutex> hold(frames_->lock);
			if ( frames_->held && frames_->top == top && frames_->height == height ) return true;
		}
		if ( !redraw_asked_ && !gone() ) {
			redraw_asked_ = true;
			page_->Redraw();
		}
		return false;
	}

	void paint( SkCanvas *c, int top, int height ) override
	{
		SkIRect clip = c->getLocalClipBounds().roundOut();
		if ( !clip.intersect(SkIRect::MakeWH(cw_, std::min(height, ch_))) ) return;
		if ( gone() || !map_.IsValid() || !readable(top, height) ) return;
		{
			/* a window painted without asking what changed (its own pixels): the frame's rectangles are in this */
			std::lock_guard<std::mutex> hold(frames_->lock);
			frames_->rects.clear();
		}
		SkPixmap pm(SkImageInfo::MakeN32Premul(cw_, ch_), map_.memory(), pitch_);
		sk_sp<SkImage> img = SkImages::RasterFromPixmap(pm, nullptr, nullptr);
		SkPaint p;
		p.setBlendMode(SkBlendMode::kSrc);
		c->drawImageRect(img, SkRect::Make(clip), SkRect::Make(clip), SkSamplingOptions(), &p,
				 SkCanvas::kStrict_SrcRectConstraint);
	}

	/* The rectangles copied out of the page's memory into the window's pixels: no call is made */
	bool paint_gpu( const std::vector<BrRect> &rects, int top, int width, int height, void *px,
			size_t pitch ) override
	{
		if ( gone() || !map_.IsValid() ) return false;
		if ( !readable(top, height) ) return true;	/* the window keeps what it has until the frame comes */
		const uint8_t *from = static_cast<const uint8_t *>(map_.memory());
		for ( const BrRect &r : rects ) {
			int x0 = std::max(r.x, 0), x1 = std::min({ r.x + r.w, width, cw_ });
			int y0 = std::max(r.y, 0), y1 = std::min({ r.y + r.h, height, ch_ });
			if ( x1 <= x0 ) continue;
			for ( int y = y0; y < y1; y++ ) {
				memcpy(static_cast<uint8_t *>(px) + (size_t)y * pitch + (size_t)x0 * 4,
				       from + (size_t)y * pitch_ + (size_t)x0 * 4, (size_t)( x1 - x0 ) * 4);
			}
		}
		return true;
	}

	/*
	 * What changed: the rectangles of the frames that came for this view.
	 * The view the window shows is told to the renderer when it changes;
	 * its frames come after
	 */
	bool damage( int top, int height, std::vector<BrRect> *out ) override
	{
		out->clear();
		if ( gone() ) return false;
		/* the view told first: the page is laid out for it even before it is drawn (no width yet) */
		if ( top != view_top_ || height != view_h_ ) {
			view_top_ = top;
			view_h_ = height;
			page_->View(top, height);
		}
		if ( !canvas(width_ > 0 ? width_ : cw_, height) ) return false;
		bool stale = false, gs_stale = false;
		{
			std::lock_guard<std::mutex> hold(frames_->lock);
			if ( frames_->held && frames_->gs_set != 0 ) {
				/*
				 * a frame of the window's GPU buffers: shown whatever view
				 * it is of, as the renderer draws its next into the other
				 * buffer once it is; one of this view is asked for after it
				 */
				*out = std::move(frames_->rects);
				frames_->rects.clear();
				gs_stale = frames_->top != top || frames_->height != height;
			} else if ( frames_->held && frames_->top == top && frames_->height == height ) {
				*out = std::move(frames_->rects);
				frames_->rects.clear();
			} else if ( frames_->held ) {
				/* a frame of the view before (a scroll, a new size): given back unread, the next is of this one */
				frames_->rects.clear();
				stale = true;
			}
		}
		if ( stale ) release();
		if ( gs_stale && !redraw_asked_ && !gone() ) {
			redraw_asked_ = true;
			page_->Redraw();
		}
		return true;
	}

	/* ---------------------------------------- the window's GPU buffers (design 14.13.10) */

	/* Whether the page may be given buffers: its renderer there, the page not closed */
	bool gs_usable() { return !gone() && !closed_; }

	/* The frame held: 0 none, 1 one of the page's memory, 2 one of the buffers (its set, -1 none, and buffer) */
	int gs_held( int *set, int *buf )
	{
		std::lock_guard<std::mutex> hold(frames_->lock);
		if ( !frames_->held ) return 0;
		if ( frames_->gs_set == 0 ) return 1;
		*set = frames_->gs_set;
		*buf = frames_->gs_buf;
		return 2;
	}

	/* A frame of the buffers shown by the window, or not to be: given back to the renderer */
	void gs_done()
	{
		{
			std::lock_guard<std::mutex> hold(frames_->lock);
			if ( !frames_->held || frames_->gs_set == 0 ) return;
			frames_->gs_set = 0;
			frames_->gs_buf = -1;
			frames_->rects.clear();
		}
		release();
	}

	/* The buffers given to the page's renderer ('images' empty: taken away); false when it refused them */
	bool gs_give( std::vector<br::mojom::GsurfImagePtr> images, int w, int h, int gpu_pid, int set )
	{
		bool done = false;
		if ( !gs_usable() || !page_wait<br::mojom::Page::SetGsurfCallback>("SetGsurf",
				[&]( auto cb ) {
					page_->SetGsurf(std::move(images), w, h, gpu_pid, set, std::move(cb));
				}, &done) ) return false;
		return done;
	}

	std::string link_at( int x, int y ) override
	{
		std::string l;
		if ( !gone() ) (void)page_wait<br::mojom::Page::LinkAtCallback>("LinkAt",
				[&]( auto cb ) {
					page_->LinkAt(x, y, std::move(cb));
				}, &l);
		return l;
	}

	std::string title() override { return title_; }

	/* The window's place and the screen as they are now, for the page */
	static br::mojom::TurnPtr turn()
	{
		auto t = br::mojom::Turn::New();
		int x = 0, y = 0, w = 0, h = 0, sw = 0, sh = 0;
		t->has_window = br_window_rect(&x, &y, &w, &h);
		t->win_x = x;
		t->win_y = y;
		t->win_w = w;
		t->win_h = h;
		br_screen_size(&sw, &sh);
		t->screen_w = sw;
		t->screen_h = sh;
		return t;
	}

	/*
	 * The page's turn as the browser sees it: no call. The window's place
	 * told when it changed; what the page told it is since, taken; whether
	 * there is something to show (a frame, a state)
	 */
	bool pump() override
	{
		/* a buffer the window has shown meanwhile: its frame given back (it may be this page's) */
		gs_tick();
		if ( gone() ) return false;
		/* a frame drawn by a show that did not end in frame_shown (a scroll, the window drawn again) */
		release();
		br::mojom::TurnPtr t = turn();
		if ( !told_turn_ || !t->Equals(*told_turn_) ) {
			told_turn_ = t.Clone();
			page_->Window(std::move(t));
		}
		std::deque<br::mojom::PageStatePtr> got;
		bool frame;
		{
			std::lock_guard<std::mutex> hold(frames_->lock);
			got.swap(frames_->states);
			frame = !frames_->rects.empty();
		}
		for ( auto &st : got ) take(std::move(st));
		bool laid_out = laid_out_;
		laid_out_ = false;
		return frame || !got.empty() || laid_out;
	}

	/* The frame shown: the page's memory given back to the renderer for the next */
	void frame_shown() override
	{
		release();
	}

	/* The page's memory given back once every rectangle of the frame held has been copied out */
	void release()
	{
		{
			std::lock_guard<std::mutex> hold(frames_->lock);
			/* a frame of the window's GPU buffers goes back once the window shows it (gs_done) */
			if ( !frames_->held || !frames_->rects.empty() || frames_->gs_set != 0 ) return;
			frames_->held = false;
		}
		redraw_asked_ = false;
		if ( !gone() ) page_->FrameDone();
	}

	/* the page's turns are the renderer's: the loop waits for what it tells (wake_loop) */
	int idle_ms() override { return BR_WAIT_MOST_MS; }

	void stop_script() override
	{
		if ( !gone() ) proc_->control->StopScript();
	}

	void report_script() override
	{
		if ( !gone() ) proc_->control->ReportScript();
	}

	std::string navigation() override
	{
		if ( nav_.empty() ) return std::string();
		std::string to = nav_.front();
		nav_.pop_front();
		last_post_ = nav_.empty() && nav_post_;
		return to;
	}

	bool navigation_body( std::string *method, std::string *ctype, std::string *body ) override
	{
		if ( !last_post_ ) return false;
		*method = nav_method_;
		*ctype = nav_ctype_;
		*body = nav_body_;
		return true;
	}

	bool history() override { return true; }

	int history_go( int offset ) override
	{
		int32_t r = BR_HIST_NONE;
		br::mojom::PageStatePtr s;
		if ( gone() || !page_wait<br::mojom::Page::HistoryGoCallback>("HistoryGo",
				[&]( auto cb ) {
					page_->HistoryGo(offset, std::move(cb));
				}, &r, &s) ) return BR_HIST_NONE;
		take(std::move(s));
		return r;
	}

	bool can_go( int offset ) override { return offset < 0 ? can_back_ : can_forward_; }

	/*
	 * The page's entries, as the browser keeps them when the page stops
	 * being shown: the copy its renderer last gave, not waited for (a page
	 * whose script holds its renderer would hold the window that leaves
	 * it). The renderer gives them with its state when the page's address
	 * or where it is among them changes, and they are asked for again each
	 * time they are taken here; only a page that has given none waits.
	 */
	bool history_save( std::vector<std::string> *states, std::vector<std::string> *urls, int *at ) override
	{
		if ( hist_have_ ) {
			*states = hist_states_;
			*urls = hist_urls_;
			*at = hist_at_;
			ask_history();
			return !states->empty();
		}
		int32_t a = -1;
		if ( gone() || !page_wait<br::mojom::Page::HistoryStateCallback>("HistoryState",
				[&]( auto cb ) {
					page_->HistoryState(std::move(cb));
				}, states, urls, &a) ) return false;
		*at = a;
		return !states->empty();
	}

	/* The page's entries asked for, not waited for: their copy is kept when they come */
	void ask_history()
	{
		if ( gone() || hist_asked_ ) return;
		hist_asked_ = true;
		page_->HistoryState(base::BindOnce([]( RemotePage *e, const std::vector<std::string> &st,
						       const std::vector<std::string> &urls, int32_t at ) {
			e->hist_asked_ = false;
			e->hist_states_ = st;
			e->hist_urls_ = urls;
			e->hist_at_ = at;
			e->hist_have_ = true;
		}, base::Unretained(this)));
	}

	void history_cut() override
	{
		if ( !gone() ) page_->HistoryCut();
	}

	void load_as( int how ) override
	{
		if ( !gone() ) page_->LoadAs(how);
	}

	void response( int status, const std::string &headers, const std::string &protocol ) override
	{
		if ( !gone() ) page_->Response(status, headers, protocol);
	}

	std::string url() override { return url_; }

	/*
	 * A page with no beforeunload handler (its states tell) is left
	 * without asking its renderer. One with a handler is asked, and its
	 * answer waited for kUnloadTimeoutMs at most, not counting the time its
	 * panel asks the user; with none in that time the page is left
	 */
	bool may_leave( bool reload ) override
	{
		bool yes = true;
		if ( gone() || !beforeunload_ ) return true;
		if ( call_wait_within<br::mojom::Page::MayLeaveCallback>(proc_, "MayLeave", kUnloadTimeoutMs,
				[this] { return gone(); },
				[&]( auto cb ) {
					page_->MayLeave(reload, std::move(cb));
				}, &yes) ) {
			return yes;
		}
		return true;
	}

	std::string hover() override { return hover_; }

	void zoom( double factor ) override
	{
		if ( !gone() ) page_->Zoom(factor);
	}

	int fullscreen_asked() override
	{
		int f = fs_asked_;
		fs_asked_ = 0;
		return f;
	}

	int window_asked() override
	{
		int w = win_asked_;
		win_asked_ = 0;
		return w;
	}

	void fullscreen_done( bool full ) override
	{
		if ( !gone() ) page_->FullscreenDone(full, turn());
	}

	bool context( int x, int y, Context *what ) override
	{
		br::mojom::ContextAtPtr at;
		br::mojom::PageStatePtr s;
		if ( gone() || !page_wait<br::mojom::Page::ContextCallback>("Context",
				[&]( auto cb ) {
					page_->Context(x, y, std::move(cb));
				}, &at, &s) || !at ) return false;
		take(std::move(s));
		what->link = at->link;
		what->image = at->image;
		what->selection = at->selection;
		what->editable = at->editable;
		{
			std::lock_guard<std::mutex> hold(state_lock_);
			ext_items_ = at->ext_items;
		}
		return at->took;
	}

	void edit( const std::string &command ) override
	{
		if ( !gone() ) page_->Edit(command);
	}

	void save_url( const std::string &url ) override
	{
		if ( !gone() ) page_->SaveUrl(url);
	}

	int find( const std::string &text, int step, int *active ) override
	{
		int32_t n = 0, a = 0;
		if ( !gone() ) (void)page_wait<br::mojom::Page::FindCallback>("Find",
				[&]( auto cb ) {
					page_->Find(text, step, std::move(cb));
				}, &n, &a);
		*active = a;
		return n;
	}

	bool click( int x, int y ) override
	{
		bool took = false;
		br::mojom::PageStatePtr s;
		if ( !gone() && page_wait<br::mojom::Page::ClickCallback>("Click",
				[&]( auto cb ) {
					page_->Click(x, y, std::move(cb));
				}, &took, &s) ) take(std::move(s));
		return took;
	}

	bool print() override
	{
		bool done = false;
		if ( !gone() ) (void)page_wait<br::mojom::Page::PrintCallback>("Print",
				[&]( auto cb ) {
					page_->Print(std::move(cb));
				}, &done);
		return done;
	}

	bool drop( int x, int y, const std::vector<std::pair<std::string, std::string>> &files, const std::string &text,
		   unsigned mods ) override
	{
		std::vector<br::mojom::DropFilePtr> fs;
		for ( const auto &f : files ) fs.push_back(br::mojom::DropFile::New(f.first, f.second));
		bool took = false;
		br::mojom::PageStatePtr s;
		if ( !gone() && page_wait<br::mojom::Page::DropCallback>("Drop",
				[&]( auto cb ) {
					page_->Drop(x, y, std::move(fs), text, mods, std::move(cb));
				}, &took, &s) ) take(std::move(s));
		return took;
	}

	std::string dragging() override { return dragging_; }

	void drag_away( bool taken ) override
	{
		if ( !gone() ) page_->DragAway(taken);
	}

	std::string first_link() override
	{
		std::string l;
		if ( !gone() ) (void)page_wait<br::mojom::Page::FirstLinkCallback>("FirstLink",
				[&]( auto cb ) {
					page_->FirstLink(std::move(cb));
				}, &l);
		return l;
	}

	bool input() override { return true; }

	void mouse( int type, int x, int y, int button, unsigned mods ) override
	{
		if ( !gone() ) page_->Mouse(type, x, y, button, mods);
	}

	bool key( unsigned usage, unsigned mods, unsigned ch ) override
	{
		bool took = false;
		br::mojom::PageStatePtr s;
		if ( !gone() && page_wait<br::mojom::Page::KeyCallback>("Key",
				[&]( auto cb ) {
					page_->Key(usage, mods, ch, std::move(cb));
				}, &took, &s) ) take(std::move(s));
		return took;
	}

	void wheel( int x, int y, int right, int down ) override
	{
		if ( !gone() ) page_->Wheel(x, y, right, down);
	}

	int scroll_top() override { return scroll_top_; }
	int doc_width() override { return doc_width_; }
	int scroll_left() override { return scroll_left_; }

	void set_scroll_left( int x ) override
	{
		if ( !gone() ) page_->SetScrollLeft(x);
	}

	bool editable() override { return editable_; }

	void compose( const std::string &text, const std::vector<int> &clauses, int target, int caret ) override
	{
		std::vector<int32_t> cl(clauses.begin(), clauses.end());
		if ( !gone() ) page_->Compose(text, cl, target, caret);
	}

	void commit( const std::string &text ) override
	{
		if ( !gone() ) page_->Commit(text);
	}

	bool caret( int *x, int *y, int *h ) override
	{
		bool shown = false;
		int32_t cx = 0, cy = 0, ch = 0;
		if ( gone() || !page_wait<br::mojom::Page::CaretCallback>("Caret",
				[&]( auto cb ) {
					page_->Caret(std::move(cb));
				}, &shown, &cx, &cy, &ch) ) return false;
		*x = cx;
		*y = cy;
		*h = ch;
		return shown;
	}

	std::string memory( bool collect ) override
	{
		std::string m;
		if ( !gone() ) (void)page_wait<br::mojom::Page::MemoryCallback>("Memory",
				[&]( auto cb ) {
					page_->Memory(collect, std::move(cb));
				}, &m);
		return m;
	}

	void run_script( const std::string &url ) override
	{
		if ( !gone() ) page_->RunScript(url);
	}

	/* What the window's thread reads for its menu */
	bool isolated()
	{
		std::lock_guard<std::mutex> hold(state_lock_);
		return isolated_;
	}

	std::vector<std::string> ext_actions()
	{
		std::lock_guard<std::mutex> hold(state_lock_);
		return ext_actions_;
	}

	std::vector<std::string> ext_items()
	{
		std::lock_guard<std::mutex> hold(state_lock_);
		return ext_items_;
	}

	std::string badge()
	{
		std::lock_guard<std::mutex> hold(state_lock_);
		return badge_;
	}

	std::string capture()
	{
		std::lock_guard<std::mutex> hold(state_lock_);
		return capture_;
	}

private:
	/*
	 * The renderer's site made that of 'url', unless it was fixed by a page
	 * shown ('fix' fixes it): a renderer that has shown a page of one site
	 * is not given another's passwords for a page of the same renderer that
	 * goes elsewhere (a window it opened, a page shown there for want of a
	 * renderer)
	 */
	void give_site( const std::string &url, bool fix )
	{
		std::string site = br_proc_site(url);
		if ( site.empty() ) return;
		bool tell = false;
		{
			std::lock_guard<std::mutex> hold(g_site_lock);
			if ( !proc_->site_fixed && proc_->site != site ) {
				proc_->site = site;
				tell = true;
			} else if ( proc_->site_fixed && proc_->site != site && fix ) {
				printf("browser: renderer %d shows %s but stays of the site %s\n", proc_->pid, site.c_str(),
				       proc_->site.c_str());
			}
			if ( fix ) proc_->site_fixed = true;
		}
		if ( tell ) net_site(proc_->pid, site);
	}

	Proc					*proc_;
	mojo::Remote<br::mojom::Page>		page_;
	bool					dead_ = false;
	std::atomic<uint32_t>			nav_seq_{0};	/* the navigation waited for, 0 for none */

	std::mutex				state_lock_;	/* what the window's thread reads */
	std::string				title_, url_, hover_, dragging_, badge_, capture_, origin_;
	std::deque<std::string>			nav_;
	bool					nav_post_ = false, last_post_ = false;
	std::string				nav_method_, nav_ctype_, nav_body_;
	int					fs_asked_ = 0, win_asked_ = 0;
	bool					close_asked_ = false;
	std::deque<BrOpened>			opened_;
	std::deque<std::string>			apart_;
	int					scroll_top_ = 0, doc_width_ = 0, scroll_left_ = 0;
	bool					editable_ = false, can_back_ = false, can_forward_ = false;
	bool					isolated_ = false, repaint_ = false;
	bool					wv_busy_ = false, wv_installed_ = false;
	std::vector<std::string>		ext_actions_, ext_items_;

	int					width_ = 0, height_ = 0;
	int					told_width_ = -1;	/* the width the last Layout told */
	bool					layout_asked_ = false;	/* a Layout not yet answered */
	bool					laid_out_ = false;	/* one answered since pump() last looked */
	bool					beforeunload_ = false;	/* a document of the page has a beforeunload handler (its states tell) */
	/* the page's entries as its renderer last gave them (history_save) */
	std::vector<std::string>		hist_states_, hist_urls_;
	int					hist_at_ = -1;
	bool					hist_have_ = false, hist_asked_ = false;
	base::WritableSharedMemoryMapping	map_;
	int					cw_ = 0, ch_ = 0;
	size_t					pitch_ = 0;
	std::shared_ptr<Frames>			frames_;
	int					view_top_ = -1, view_h_ = -1;
	bool					redraw_asked_ = false;
	br::mojom::TurnPtr			told_turn_;	/* the window's place last told */
	bool					closed_ = false;
};

RemotePage *remote_of( BrEngine *eng )
{
	return br_multi_process ? static_cast<RemotePage *>(eng) : nullptr;
}

/* The renderer's remote bound on the engine thread, when the page first asks */
mojo::Remote<br::mojom::Renderer> &renderer_of( Proc *p )
{
	if ( !p->renderer.is_bound() && p->pending.is_valid() ) {
		p->renderer.Bind(std::move(p->pending));
		p->renderer.set_disconnect_handler(base::BindOnce([]( Proc *q ) { q->gone = true; }, base::Unretained(p)));
	}
	return p->renderer;
}

/* ---------------------------------------------------------------- the window's GPU buffers (design 14.13.10) */

long mono_us()
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000000L + t.tv_nsec / 1000;
}

/*
 * A set of the window's buffers: two BOs of the GPU process 'gpu_pid'
 * under its number 'gpu_id', 'w' x 'h', rows of 'pitch' bytes, held by
 * the browser as PRIME descriptors of them (keys to their objects) for as
 * long as the set is given to a page, and the shared images they are
 */
struct GsSet {
	int		id = 0;			/* the browser's number (> 0), the renderer's frames name it */
	int		gpu_id = 0, gpu_pid = 0;
	int		w = 0, h = 0;
	unsigned	pitch = 0;
	int		fd[2] = { -1, -1 };
	unsigned char	mailbox[2][16] = {};
};

/*
 * The page's window and its buffers, on the engine thread: the set given
 * to the window's page, the one the window was given (OB_GS_SET), the
 * show waited for; the window's thread notes the shows the window tells
 */
struct WinGs {
	RemotePage		*page = nullptr;	/* the page the set is given to */
	GsSet			set;
	int			laid = 0;		/* the set the window shows from, 0: its own pixels */
	unsigned		serial = 0;		/* the last show asked for */
	unsigned		waiting = 0;		/* the show whose notice is waited for, 0 none */
	RemotePage		*waiting_page = nullptr;
	long			asked_us = 0;
	bool			checked = false;	/* the window's record read once for a show long waited for */
	int			unable_pid = 0;		/* the GPU process that could not make buffers, or whose were refused */
	int			next_id = 0;
	std::string		how;
	bool			told = false;
	long			sets = 0, shown = 0, shown_us = 0, drops = 0;
};

WinGs				g_gs;
std::atomic<unsigned>		g_gs_shown{0};	/* the serial the window last told it shows, from the window's thread */

/* The answer of the GPU process to MakeGsurf, in the browser */
struct GsMade {
	int		id = 0, pid = 0;
	unsigned	pitch = 0;
	int		fd[2] = { -1, -1 };
	unsigned char	mailbox[2][16] = {};
	std::string	how, why;
};

/* On the engine thread: two buffers 'w' x 'h' asked of the GPU process running now, waited for */
bool gs_make( int w, int h, GsMade *out )
{
	if ( g_gpuq == nullptr ) {
		out->why = "no GPU process";
		return false;
	}
	std::mutex m;
	std::condition_variable cv;
	bool done = false;
	g_gpuq->task_runner()->PostTask(FROM_HERE, base::BindOnce([]( int w, int h, GsMade *out, std::mutex *m,
								      std::condition_variable *cv, bool *done ) {
		int32_t id = 0;
		uint32_t pitch = 0;
		std::vector<br::mojom::GsurfBufPtr> bufs;
		if ( g_gpusvc == nullptr || g_gpu_pid <= 0 ) {
			out->why = "no GPU process";
		} else if ( !( *g_gpusvc )->MakeGsurf(w, h, &id, &pitch, &bufs, &out->how, &out->why) ) {
			out->why = "the GPU process did not answer";
			out->how.clear();
		} else if ( !out->how.empty() && ( id <= 0 || bufs.size() != 2 ) ) {
			out->why = "the GPU process answered no two buffers";
			out->how.clear();
		} else if ( !out->how.empty() ) {
			out->id = id;
			out->pid = g_gpu_pid;
			out->pitch = pitch;
			for ( size_t i = 0; i < 2; i++ ) {
				base::ScopedFD fd = bufs[i]->bo.TakeFD();
				out->fd[i] = fd.release();
				const std::vector<uint8_t> &mb = bufs[i]->image->mailbox;
				if ( mb.size() == sizeof(out->mailbox[i]) ) memcpy(out->mailbox[i], mb.data(), mb.size());
			}
			if ( out->fd[0] < 0 || out->fd[1] < 0 ) {
				out->why = "the GPU process's BOs did not come";
				out->how.clear();
			}
		}
		std::lock_guard<std::mutex> hold(*m);
		*done = true;
		cv->notify_all();
	}, w, h, out, &m, &cv, &done));
	std::unique_lock<std::mutex> hold(m);
	cv.wait(hold, [&] { return done; });
	if ( out->how.empty() ) {
		for ( int &fd : out->fd ) {
			if ( fd >= 0 ) close(fd);
			fd = -1;
		}
		return false;
	}
	return true;
}

/* The show waited for over: its frame given back to the renderer (it was shown, or will not be) */
void gs_unwait()
{
	if ( g_gs.waiting == 0 ) return;
	RemotePage *p = g_gs.waiting_page;
	g_gs.waiting = 0;
	g_gs.waiting_page = nullptr;
	if ( p != nullptr ) p->gs_done();
}

/*
 * The set given to the window's page taken away from it and let go of:
 * the page draws into its memory again; the window keeps showing the
 * buffer it shows until the page's next frame
 */
void gs_take()
{
	gs_unwait();
	if ( g_gs.set.id == 0 ) return;
	if ( g_gs.page != nullptr && g_gs.page->gs_usable() ) {
		(void)g_gs.page->gs_give(std::vector<br::mojom::GsurfImagePtr>(), 0, 0, 0, 0);
	}
	for ( int &fd : g_gs.set.fd ) {
		if ( fd >= 0 ) close(fd);
		fd = -1;
	}
	int gid = g_gs.set.gpu_id, gpid = g_gs.set.gpu_pid;
	if ( g_gpuq != nullptr ) {
		g_gpuq->task_runner()->PostTask(FROM_HERE, base::BindOnce([]( int gid, int gpid ) {
			if ( g_gpusvc != nullptr && g_gpu_pid == gpid ) ( *g_gpusvc )->DropGsurf(gid);
		}, gid, gpid));
	}
	g_gs.set = GsSet();
	g_gs.page = nullptr;
}

/*
 * A show the window has told: its frame given back. A show waited for
 * long (a second) is looked up in the window's record once, should its
 * notice have been lost.
 */
void gs_tick()
{
	if ( g_gs.waiting == 0 ) return;
	unsigned told = g_gs_shown.load();
	bool shown = (int)( told - g_gs.waiting ) >= 0;
	if ( !shown && !g_gs.checked && mono_us() - g_gs.asked_us >= 1000000L ) {
		g_gs.checked = true;
		int idx = -1;
		unsigned serial = 0;
		if ( br_sys_gsurf_state(&idx, &serial) && (int)( serial - g_gs.waiting ) >= 0 ) {
			printf("browser: gpu: the window did not tell that it shows its buffer %d (show %u): read from its record\n",
			       idx, g_gs.waiting);
			shown = true;
		}
	}
	if ( !shown ) return;
	g_gs.shown++;
	g_gs.shown_us += mono_us() - g_gs.asked_us;
	gs_unwait();
}

}  // namespace

/* ---------------------------------------------------------------- the window's GPU buffers, the engine thread's calls */

void br_proc_gsurf_follow( BrEngine *eng, int w, int h, bool want )
{
	gs_tick();
	RemotePage *p = remote_of(eng);
	int pid = g_gpu_pid;
	bool ok = want && br_sys_gsurf_on() && p != nullptr && p->gs_usable() && pid > 0 && pid != g_gs.unable_pid
		  && w > 0 && h > 0;
	if ( !ok ) {
		gs_take();
		return;
	}
	if ( g_gs.set.id != 0 && ( g_gs.page != p || g_gs.set.gpu_pid != pid || g_gs.set.w != w || g_gs.set.h != h ) ) {
		/* another page shown, another GPU process, another size: a set of its own */
		gs_take();
	}
	if ( g_gs.set.id != 0 ) return;
	GsMade m;
	if ( !gs_make(w, h, &m) ) {
		g_gs.unable_pid = pid;
		printf("browser: gpu: the window can have no GPU buffers (%s): each view is read back from the GPU process\n",
		       m.why.c_str());
		return;
	}
	GsSet s;
	s.id = ++g_gs.next_id;
	s.gpu_id = m.id;
	s.gpu_pid = m.pid;
	s.w = w;
	s.h = h;
	s.pitch = m.pitch;
	std::vector<br::mojom::GsurfImagePtr> images;
	for ( int i = 0; i < 2; i++ ) {
		s.fd[i] = m.fd[i];
		memcpy(s.mailbox[i], m.mailbox[i], sizeof(s.mailbox[i]));
		auto im = br::mojom::GsurfImage::New();
		im->mailbox.assign(m.mailbox[i], m.mailbox[i] + sizeof(m.mailbox[i]));
		images.push_back(std::move(im));
	}
	g_gs.set = s;
	g_gs.page = p;
	g_gs.how = m.how;
	g_gs.sets++;
	if ( !p->gs_give(std::move(images), w, h, m.pid, s.id) ) {
		printf("browser: gpu: the page's renderer took no GPU buffers of the window: each view is read back\n");
		g_gs.page = nullptr;
		gs_take();
		g_gs.unable_pid = pid;
		return;
	}
	if ( !g_gs.told ) {
		g_gs.told = true;
		printf("browser: gpu: the page's window is shown from buffers of the GPU process (%s), %dx%d, no view read back\n",
		       m.how.c_str(), w, h);
	}
}

bool br_proc_gsurf_show( BrEngine *eng, const std::vector<BrRect> &rects )
{
	gs_tick();
	RemotePage *p = remote_of(eng);
	if ( p == nullptr ) return false;
	int set = 0, buf = -1;
	int kind = p->gs_held(&set, &buf);
	if ( kind == 1 ) {
		/* a frame of the page's memory: the window shows its own pixels again, from what the buffer showed */
		if ( g_gs.laid != 0 ) {
			gs_unwait();
			(void)br_sys_gsurf_drop();
			g_gs.laid = 0;
			g_gs.drops++;
		}
		return false;
	}
	if ( kind == 0 ) return g_gs.laid != 0;
	if ( set != g_gs.set.id || p != g_gs.page || buf < 0 || buf > 1 ) {
		/* of a set taken away since: not shown */
		p->gs_done();
		return g_gs.laid != 0;
	}
	if ( g_gs.laid != set ) {
		int er = br_sys_gsurf_set(g_gs.set.fd, g_gs.set.w, g_gs.set.h, g_gs.set.pitch);
		if ( er < 0 ) {
			printf("browser: gpu: the window did not take its GPU buffers (%d): each view is read back\n", er);
			p->gs_done();
			g_gs.unable_pid = g_gs.set.gpu_pid;
			gs_take();
			if ( g_gs.laid != 0 ) {
				(void)br_sys_gsurf_drop();
				g_gs.laid = 0;
			}
			return false;
		}
		g_gs.laid = set;
	}
	unsigned serial = ++g_gs.serial;
	if ( serial == 0 ) serial = ++g_gs.serial;
	int er = br_sys_gsurf_show(buf, serial, rects);
	if ( er < 0 ) {
		printf("browser: gpu: the window did not show its GPU buffer %d (%d)\n", buf, er);
		p->gs_done();
		return true;
	}
	g_gs.waiting = serial;
	g_gs.waiting_page = p;
	g_gs.asked_us = mono_us();
	g_gs.checked = false;
	return true;
}

void br_proc_gsurf_shown( int idx, unsigned serial )
{
	(void)idx;
	g_gs_shown.store(serial);
	wake_loop();
}

void br_proc_gsurf_stat( BrGsurfStat *s )
{
	s->how = g_gs.how;
	s->sets = g_gs.sets;
	s->shown = g_gs.shown;
	s->shown_us = g_gs.shown_us;
	s->drops = g_gs.drops;
}

/* ---------------------------------------------------------------- the loop's wait */

namespace {

/* The task runner of the thread that waits (br_loop_bind), kept for the process's life */
std::atomic<scoped_refptr<base::SingleThreadTaskRunner> *>	g_wait_runner{nullptr};

/*
 * Since when the thread that waits has been in its work (ms of the
 * monotonic clock), 0 while it waits with no task running: what the
 * Control's Busy answers from the IO thread
 */
std::atomic<long>	g_busy_since{0};

/*
 * While the thread waits: the first task that runs there ends the wait
 * once none is left. A task's run is work (a page's script, a timer);
 * between tasks the thread waits
 */
class WaitEnd final : public base::TaskObserver {
public:
	explicit WaitEnd( base::RunLoop *loop ) : loop_(loop) {}
	void WillProcessTask( const base::PendingTask &, bool ) override { g_busy_since = mono_ms(); }
	void DidProcessTask( const base::PendingTask & ) override
	{
		g_busy_since = 0;
		loop_->QuitWhenIdle();
	}

private:
	base::RunLoop	*loop_;
};

}  // namespace

void br_loop_bind()
{
	if ( g_wait_runner.load() != nullptr || !base::SingleThreadTaskRunner::HasCurrentDefault() ) return;
	g_wait_runner = new scoped_refptr<base::SingleThreadTaskRunner>(base::SingleThreadTaskRunner::GetCurrentDefault());
}

bool br_loop_wait( int ms )
{
	if ( ms <= 0 ) return false;
	if ( !base::SingleThreadTaskRunner::HasCurrentDefault() ) {
		/* a thread with no task loop (no engine came up): a plain sleep */
		struct timespec t = { ms / 1000, (long)( ms % 1000 ) * 1000000L };
		while ( nanosleep(&t, &t) != 0 && errno == EINTR ) ;
		return false;
	}
	br_loop_bind();
	base::RunLoop loop;
	bool timed_out = false;
	base::OneShotTimer timer;
	timer.Start(FROM_HERE, base::Milliseconds(ms), base::BindOnce([]( bool *t, base::RunLoop *l ) {
		*t = true;
		l->Quit();
	}, &timed_out, &loop));
	WaitEnd end(&loop);
	base::CurrentThread::Get()->AddTaskObserver(&end);
	g_busy_since = 0;
	loop.Run();
	g_busy_since = mono_ms();
	base::CurrentThread::Get()->RemoveTaskObserver(&end);
	return !timed_out;
}

void br_loop_round()
{
	g_busy_since = mono_ms();
}

long br_loop_busy_ms()
{
	long since = g_busy_since.load();
	return since == 0 ? 0 : mono_ms() - since;
}

void br_loop_wake()
{
	scoped_refptr<base::SingleThreadTaskRunner> *r = g_wait_runner.load();
	if ( r != nullptr ) (*r)->PostTask(FROM_HERE, base::DoNothing());
}

/* ---------------------------------------------------------------- what br_main.cc asks */

bool br_proc_start( const std::string &lines, const unsigned char window[16] )
{
	br_base_up();
	if ( g_io == nullptr ) return false;
	static base::SingleThreadTaskExecutor main_executor;
	g_main = &main_executor;
	g_main_thread = pthread_self();
	/* the history and the downloads' list are the browser's own (br_data.cc) */
	br_data_init();
	g_lines = lines;
	memcpy(g_window, window, 16);
	g_host = new HostImpl;
	g_host_thread = new base::Thread("panels");
	if ( !g_host_thread->Start() ) return false;
	g_data_thread = new base::Thread("data calls");
	if ( !g_data_thread->Start() ) return false;
	g_host_thread->task_runner()->PostTask(FROM_HERE, base::BindOnce([] {
		g_hosts = new mojo::ReceiverSet<br::mojom::Host, int>;
	}));
	printf("browser: renderers: %d at most (%ld MB of memory), one kept ready\n", br_proc_limit(),
	       br_sys_memory_mb());
	g_netq = new base::Thread("network");
	if ( !g_netq->Start() ) return false;
	bool net_up = false;
	on_netq(base::BindOnce([]( bool *up ) {
		g_net_waiting = new std::vector<NetPipes>;
		*up = net_start();
	}, &net_up));
	if ( !net_up ) return false;
	/* the GPU process started on a thread of its own while the first renderer is */
	g_gpuq = br_sys_gpuproc() ? new base::Thread("gpu") : nullptr;
	if ( g_gpuq != nullptr && g_gpuq->Start() ) {
		g_gpuq->task_runner()->PostTask(FROM_HERE, base::BindOnce([] { (void)gpu_start(); }));
	} else {
		g_gpuq = nullptr;
	}
	start_ready();
	return true;
}

BrEngine *br_proc_new_page()
{
	for ( int tries = 0; tries < 2; tries++ ) {
		long t0 = mono_ms();
		Proc *p = take_ready();
		if ( p == nullptr ) return nullptr;
		mojo::PendingRemote<br::mojom::Page> remote;
		mojo::PendingReceiver<br::mojom::Page> recv = remote.InitWithNewPipeAndPassReceiver();
		bool done = false;
		bool sent = renderer_wait<br::mojom::Renderer::CreatePageCallback>(p, "CreatePage",
				[&]( auto cb ) {
					renderer_of(p)->CreatePage(std::move(recv), std::vector<uint8_t>(g_window, g_window + 16), std::move(cb));
				}, &done);
		if ( !sent || !done ) {
			printf("browser: renderer %d gave no page: another is started\n", p->pid);
			p->gone = true;
			end_proc(p, false);
			continue;
		}
		printf("browser: a page of renderer %d, %ld ms after it was asked for\n", p->pid, mono_ms() - t0);
		return new RemotePage(p, std::move(remote));
	}
	return nullptr;
}

void br_proc_close( BrEngine *eng )
{
	RemotePage *e = remote_of(eng);
	if ( e == nullptr ) return;
	Proc *p = e->proc();
	e->closed();
	if ( p->pages <= 0 && !p->closing ) {
		p->closing = true;
		end_proc(p, true);
	}
}

int br_proc_count()
{
	std::lock_guard<std::mutex> hold(g_lock);
	int n = 0;
	for ( Proc *p : g_procs ) {
		if ( !p->closing ) n++;
	}
	return n + ( g_starting ? 1 : 0 );
}

int br_proc_live()
{
	std::lock_guard<std::mutex> hold(g_lock);
	int n = 0;
	for ( Proc *p : g_procs ) {
		if ( !p->closing && p != g_ready ) n++;
	}
	return n;
}

void br_proc_keep_ready()
{
	start_ready();
}

static int g_limit_set = 0;		/* br_proc_set_limit: 0 for the machine's memory to say */

void br_proc_set_limit( int n )
{
	g_limit_set = n < 2 ? 0 : n;
}

int br_proc_limit()
{
	if ( g_limit_set > 0 ) return g_limit_set;
	long mb = br_sys_memory_mb();
	long n = ( mb * 3 + 2048 ) / 4096;
	return n < 2 ? 2 : (int)n;
}

int br_proc_pid( BrEngine *eng )
{
	RemotePage *e = remote_of(eng);
	return e != nullptr ? e->proc()->pid : 0;
}

void br_proc_ext_allow( const std::string &uuid )
{
	{
		std::lock_guard<std::mutex> hold(g_ext_lock);
		if ( std::find(g_ext_allowed.begin(), g_ext_allowed.end(), uuid) != g_ext_allowed.end() ) return;
		g_ext_allowed.push_back(uuid);
	}
	/* before the network process is started (a test's lines), it is told at its start (net_start) */
	if ( g_netq == nullptr ) return;
	on_netq(base::BindOnce([]( std::string uuid ) {
		if ( g_netsvc != nullptr && g_net_pid > 0 ) ( *g_netsvc )->ExtAllow(uuid);
	}, uuid));
}

int br_proc_test_killnav = 0;

bool br_proc_kill( BrEngine *eng )
{
	RemotePage *e = remote_of(eng);
	if ( e == nullptr || e->proc()->pid <= 0 ) return false;
	printf("browser: renderer %d is ended (kill)\n", e->proc()->pid);
	br_sys_kill(e->proc()->pid);
	return true;
}

std::string br_proc_site( const std::string &url )
{
	GURL u(url);
	if ( !u.is_valid() || !u.SchemeIsHTTPOrHTTPS() ) return std::string();
	return net::SchemefulSite(u).Serialize();
}

std::string br_proc_command( BrEngine *eng, const std::string &code )
{
	RemotePage *e = remote_of(eng);
	std::string said;
	if ( e != nullptr && !e->gone() ) (void)renderer_wait<br::mojom::Renderer::CommandCallback>(e->proc(), "Command",
			[&]( auto cb ) {
				renderer_of(e->proc())->Command(code, std::move(cb));
			}, &said);
	return said;
}

std::string br_proc_query( BrEngine *eng, const std::string &what )
{
	RemotePage *e = remote_of(eng);
	std::string v;
	if ( e != nullptr && !e->gone() ) (void)renderer_wait<br::mojom::Renderer::QueryCallback>(e->proc(), "Query",
			[&]( auto cb ) {
				renderer_of(e->proc())->Query(what, std::move(cb));
			}, &v);
	return v;
}

/*
 * The questions asked and not waited for, by renderer and question: one at
 * a time, the engine thread's alone as the calls are
 */
std::set<std::pair<const Proc *, std::string>>	g_asked;

bool br_proc_query_async( BrEngine *eng, const std::string &what, base::OnceCallback<void( std::string )> told )
{
	RemotePage *e = remote_of(eng);
	if ( e == nullptr || e->gone() ) return false;
	auto key = std::make_pair(static_cast<const Proc *>(e->proc()), what);
	if ( !g_asked.insert(key).second ) return false;
	renderer_of(e->proc())->Query(what, base::BindOnce([]( std::pair<const Proc *, std::string> key,
							       base::OnceCallback<void( std::string )> told,
							       const std::string &v ) {
		g_asked.erase(key);
		std::move(told).Run(v);
	}, key, std::move(told)));
	return true;
}

bool br_proc_flush( BrEngine *eng, int wait_ms )
{
	RemotePage *e = remote_of(eng);
	bool done = false;
	if ( e != nullptr && !e->gone() ) (void)renderer_wait<br::mojom::Renderer::FlushStoresCallback>(e->proc(), "FlushStores",
			[&]( auto cb ) {
				renderer_of(e->proc())->FlushStores(wait_ms, std::move(cb));
			}, &done);
	return done;
}

std::string br_proc_calling()
{
	const char *what = g_calling.load();
	if ( what == nullptr ) return std::string();
	char line[160];
	snprintf(line, sizeof(line), "the answer of renderer %d to %s, for %ld ms", g_calling_pid.load(), what,
		 mono_ms() - g_calling_since.load());
	return line;
}

long br_proc_busy_ms( BrEngine *eng )
{
	RemotePage *e = remote_of(eng);
	return e != nullptr ? busy_ms(e->proc()) : 0;
}

void br_proc_stop_load( BrEngine *eng, bool now )
{
	RemotePage *e = remote_of(eng);
	if ( e != nullptr && !e->gone() ) e->proc()->control->StopLoad(now, e->nav_seq());
}

bool br_proc_isolated( BrEngine *eng )
{
	RemotePage *e = remote_of(eng);
	return e != nullptr && e->isolated();
}

std::vector<std::string> br_proc_ext_actions( BrEngine *eng )
{
	RemotePage *e = remote_of(eng);
	return e != nullptr ? e->ext_actions() : std::vector<std::string>();
}

std::vector<std::string> br_proc_ext_items( BrEngine *eng )
{
	RemotePage *e = remote_of(eng);
	return e != nullptr ? e->ext_items() : std::vector<std::string>();
}

std::string br_proc_badge( BrEngine *eng )
{
	RemotePage *e = remote_of(eng);
	return e != nullptr ? e->badge() : std::string();
}

std::string br_proc_capture( BrEngine *eng )
{
	RemotePage *e = remote_of(eng);
	return e != nullptr ? e->capture() : std::string();
}

/* ---------------------------------------------------------------- videos in windows of their own */

/* A video's window told that its player's frame has come into its memory (br_main.cc) */
void ( *br_proc_player_painted )( BrEngine *from, int session ) = nullptr;

namespace {

/* The pixels a picture-in-picture window's player is drawn into, by renderer and session */
struct PlayerPixels {
	base::WritableSharedMemoryMapping	map;
	int					w = 0, h = 0;
	size_t					pitch = 0;
	bool					painting = false;	/* a frame asked for and not answered yet */
	bool					painted = false;	/* the memory holds a frame */
};

std::map<std::pair<Proc *, int>, PlayerPixels>	g_players;

Proc *proc_of( BrEngine *eng )
{
	RemotePage *e = remote_of(eng);
	return ( e != nullptr && !e->gone() ) ? e->proc() : nullptr;
}

}  // namespace

bool br_proc_pip_next( BrPipAsk *a, BrEngine **from )
{
	if ( g_pip.empty() ) return false;
	*from = g_pip.front().first;
	*a = g_pip.front().second;
	g_pip.pop_front();
	return true;
}

void br_proc_pip_closed( BrEngine *from, int session )
{
	Proc *p = proc_of(from);
	if ( p == nullptr ) return;
	g_players.erase(std::make_pair(p, session));
	(void)renderer_wait<br::mojom::Renderer::PipClosedCallback>(p, "PipClosed",
			[&]( auto cb ) {
				renderer_of(p)->PipClosed(session, std::move(cb));
			});
}

void br_proc_pip_press( BrEngine *from, int session, bool play )
{
	Proc *p = proc_of(from);
	if ( p != nullptr ) (void)renderer_wait<br::mojom::Renderer::PipPressCallback>(p, "PipPress",
			[&]( auto cb ) {
				renderer_of(p)->PipPress(session, play, std::move(cb));
			});
}

/* The frame the memory holds drawn into the canvas: false when it holds none */
bool br_proc_player_pixels( BrEngine *from, int session, SkCanvas *c )
{
	Proc *p = proc_of(from);
	if ( p == nullptr ) return false;
	auto it = g_players.find(std::make_pair(p, session));
	if ( it == g_players.end() || !it->second.painted || !it->second.map.IsValid() ) return false;
	PlayerPixels &px = it->second;
	SkPixmap pm(SkImageInfo::MakeN32Premul(px.w, px.h), px.map.memory(), px.pitch);
	sk_sp<SkImage> img = SkImages::RasterFromPixmap(pm, nullptr, nullptr);
	SkPaint paint;
	paint.setBlendMode(SkBlendMode::kSrc);
	c->drawImage(img, 0, 0, SkSamplingOptions(), &paint);
	return true;
}

/*
 * A video's window drawn: the frame its memory holds, and the player's
 * frame now asked for, not waited for (the page's script may hold its
 * renderer, and the engine thread serves the other windows meanwhile).
 * When it has come, the window is told (br_proc_player_painted) and draws
 * it (br_proc_player_pixels). The memory is given to the renderer when the
 * window is new or of another size, which is waited for.
 */
bool br_proc_paint_player( BrEngine *from, int session, int player, SkCanvas *c, int w, int h )
{
	Proc *p = proc_of(from);
	if ( p == nullptr || w <= 0 || h <= 0 ) return false;
	PlayerPixels &px = g_players[std::make_pair(p, session)];
	if ( !px.map.IsValid() || px.w != w || px.h != h ) {
		size_t pitch = (size_t)w * 4;
		base::UnsafeSharedMemoryRegion region = base::UnsafeSharedMemoryRegion::Create(pitch * (size_t)h);
		if ( !region.IsValid() ) return false;
		base::WritableSharedMemoryMapping map = region.Map();
		bool done = false;
		if ( !map.IsValid()
		     || !renderer_wait<br::mojom::Renderer::PlayerCanvasCallback>(p, "PlayerCanvas",
				[&]( auto cb ) {
					renderer_of(p)->PlayerCanvas(session, mojo::WrapUnsafeSharedMemoryRegion(std::move(region)), w, h,
								     (int32_t)pitch, std::move(cb));
				}, &done)
		     || !done ) {
			return false;
		}
		px.map = std::move(map);
		px.w = w;
		px.h = h;
		px.pitch = pitch;
		px.painted = false;
	}
	if ( !px.painting && !p->gone ) {
		px.painting = true;
		renderer_of(p)->PaintPlayer(session, player, base::BindOnce([]( Proc *p, BrEngine *from, int session, bool done ) {
			auto it = g_players.find(std::make_pair(p, session));
			if ( it == g_players.end() ) return;
			it->second.painting = false;
			if ( !done ) return;
			it->second.painted = true;
			if ( br_proc_player_painted != nullptr ) br_proc_player_painted(from, session);
		}, p, from, session));
	}
	return br_proc_player_pixels(from, session, c);
}

bool br_proc_player_paused( BrEngine *from, int player )
{
	Proc *p = proc_of(from);
	bool paused = true;
	int64_t frames = -1;
	if ( p != nullptr ) (void)renderer_wait<br::mojom::Renderer::PlayerStateCallback>(p, "PlayerState",
			[&]( auto cb ) {
				renderer_of(p)->PlayerState(player, std::move(cb));
			}, &paused, &frames);
	return paused;
}

/*
 * How many frames the player has shown, looked at by the window's loop at
 * every round: the count its renderer last answered, and the question
 * asked again, not waited for, when it has been answered
 */
long br_proc_player_frames( BrEngine *from, int player )
{
	struct Count {
		long	frames = -1;
		bool	asked = false;
	};
	static std::map<std::pair<const Proc *, int>, std::shared_ptr<Count>> counts;
	Proc *p = proc_of(from);
	if ( p == nullptr || p->gone ) return -1;
	std::shared_ptr<Count> &n = counts[std::make_pair(static_cast<const Proc *>(p), player)];
	if ( !n ) n = std::make_shared<Count>();
	if ( !n->asked ) {
		n->asked = true;
		renderer_of(p)->PlayerState(player, base::BindOnce([]( std::shared_ptr<Count> n, bool, int64_t frames ) {
			n->frames = (long)frames;
			n->asked = false;
		}, n));
	}
	return n->frames;
}

/* ---------------------------------------------------------------- files let go over the page */

std::string br_proc_keep_file( BrEngine *eng, const std::string &uuid, const std::string &name,
			       const std::vector<uint8_t> &bytes )
{
	Proc *p = proc_of(eng);
	std::string path;
	if ( p == nullptr ) return path;
	base::UnsafeSharedMemoryRegion region = base::UnsafeSharedMemoryRegion::Create(bytes.empty() ? 1 : bytes.size());
	if ( !region.IsValid() ) return path;
	{
		base::WritableSharedMemoryMapping map = region.Map();
		if ( !map.IsValid() ) return path;
		if ( !bytes.empty() ) memcpy(map.memory(), bytes.data(), bytes.size());
	}
	(void)renderer_wait<br::mojom::Renderer::KeepFileCallback>(p, "KeepFile",
			[&]( auto cb ) {
				renderer_of(p)->KeepFile(uuid, name, mojo::WrapUnsafeSharedMemoryRegion(std::move(region)), (uint64_t)bytes.size(), std::move(cb));
			}, &path);
	return path;
}

void br_proc_wv_state( BrEngine *eng, bool *busy, bool *installed )
{
	RemotePage *e = remote_of(eng);
	*busy = false;
	*installed = false;
	if ( e != nullptr ) e->wv(busy, installed);
}

/* ---------------------------------------------------------------- what the windows read of the renderers */

namespace {

/* The renderer of the window's page (br_proc_current); the records are kept while the browser runs */
std::atomic<Proc *>		g_current{nullptr};

/* An operation on the data calls' thread: the window's page's renderer, and with 'all' every one */
std::string data_here( const std::string &op, const std::string &arg, bool all )
{
	std::string answer;
	Proc *cur = g_current.load();
	if ( cur != nullptr && !cur->gone ) (void)cur->data->Ask(op, arg, &answer);
	if ( !all ) return answer;
	std::vector<Proc *> others;
	{
		std::lock_guard<std::mutex> hold(g_lock);
		for ( Proc *p : g_procs ) {
			if ( p != cur && !p->gone && !p->closing ) others.push_back(p);
		}
	}
	for ( Proc *p : others ) {
		std::string a;
		(void)p->data->Ask(op, arg, &a);
		if ( cur == nullptr && answer.empty() ) answer = a;
	}
	return answer;
}

}  // namespace

void br_proc_current( BrEngine *eng )
{
	g_current = proc_of(eng);
}

/*
 * From any thread of the browser's (the settings window's, the
 * extensions' panels'): the call is made on the data calls' thread, which
 * has the sequence a Mojo call waits on, and waited for there. The engine
 * thread has no part in it: a window asking never waits for that thread,
 * nor that thread for a window. A renderer gone answers at once (its pipe
 * is closed).
 */
std::string br_proc_data( const std::string &op, const std::string &arg, bool all )
{
	if ( g_data_thread == nullptr ) return std::string();
	struct Job {
		std::string			op, arg, answer;
		bool				all = false, done = false;
		std::mutex			m;
		std::condition_variable		cv;
	};
	auto job = std::make_shared<Job>();
	job->op = op;
	job->arg = arg;
	job->all = all;
	g_data_thread->task_runner()->PostTask(FROM_HERE, base::BindOnce([]( std::shared_ptr<Job> j ) {
		std::string a = data_here(j->op, j->arg, j->all);
		std::lock_guard<std::mutex> hold(j->m);
		j->answer = a;
		j->done = true;
		j->cv.notify_all();
	}, job));
	std::unique_lock<std::mutex> hold(job->m);
	job->cv.wait(hold, [&job] { return job->done; });
	return job->answer;
}

void br_proc_tick()
{
	if ( g_main != nullptr && pthread_equal(pthread_self(), g_main_thread) ) base::RunLoop().RunUntilIdle();
}

void br_proc_end()
{
	std::vector<Proc *> all;
	{
		std::unique_lock<std::mutex> hold(g_lock);
		g_started.wait_for(hold, std::chrono::seconds(30), [] { return !g_starting; });
		all = g_procs;
	}
	for ( Proc *p : all ) {
		if ( !p->closing ) {
			p->closing = true;
			end_proc(p, true);
		}
	}
	if ( g_gpuq != nullptr ) {
		int gpid = 0;
		std::mutex m;
		std::condition_variable cv;
		bool done = false;
		g_gpuq->task_runner()->PostTask(FROM_HERE, base::BindOnce([]( int *gpid, std::mutex *m,
									      std::condition_variable *cv, bool *done ) {
			g_gpu_ending = true;
			*gpid = g_gpu_pid;
			if ( g_gpusvc != nullptr && *gpid > 0 ) ( *g_gpusvc )->Closing();
			std::lock_guard<std::mutex> hold(*m);
			*done = true;
			cv->notify_all();
		}, &gpid, &m, &cv, &done));
		{
			std::unique_lock<std::mutex> hold(m);
			cv.wait(hold, [&] { return done; });
		}
		if ( gpid > 0 ) {
			int status = 0;
			if ( !br_sys_reap(gpid, 5000, &status) ) {
				br_sys_kill(gpid);
				(void)br_sys_reap(gpid, 5000, &status);
			}
			printf("browser: the GPU process %d has ended (status 0x%x)\n", gpid, status);
		}
	}
	if ( g_netq == nullptr ) return;
	int pid = 0;
	on_netq(base::BindOnce([]( int *pid ) {
		g_net_ending = true;
		*pid = g_net_pid;
		if ( g_netsvc == nullptr || *pid <= 0 ) return;
		bool done = false;
		long t0 = mono_ms();
		(void)( *g_netsvc )->Flush(5000, &done);
		printf("browser: the network process wrote what it keeps in %ld ms%s\n", mono_ms() - t0,
		       done ? "" : " (not all)");
		( *g_netsvc )->Closing();
	}, &pid));
	if ( pid <= 0 ) return;
	int status = 0;
	if ( !br_sys_reap(pid, 5000, &status) ) {
		printf("browser: the network process %d did not end: it is ended\n", pid);
		br_sys_kill(pid);
		(void)br_sys_reap(pid, 5000, &status);
	}
	printf("browser: the network process %d has ended (status 0x%x)\n", pid, status);
}

int br_proc_network_pid()
{
	return g_net_pid;
}

int br_proc_gpu_pid()
{
	return g_gpu_last_pid;
}

std::string br_proc_gpu_renderer()
{
	std::lock_guard<std::mutex> hold(g_gpu_lock);
	return g_gpu_renderer;
}

/* From any thread: something asked of the GPU process (GpuService.Query), waited for; "" without one */
std::string br_proc_gpu_query( const std::string &what )
{
	if ( g_gpuq == nullptr ) return std::string();
	std::string v;
	std::mutex m;
	std::condition_variable cv;
	bool done = false;
	g_gpuq->task_runner()->PostTask(FROM_HERE, base::BindOnce([]( const std::string *what, std::string *v, std::mutex *m,
								      std::condition_variable *cv, bool *done ) {
		if ( g_gpusvc != nullptr && g_gpu_pid > 0 && !( *g_gpusvc )->Query(*what, v) ) v->clear();
		std::lock_guard<std::mutex> hold(*m);
		*done = true;
		cv->notify_all();
	}, &what, &v, &m, &cv, &done));
	std::unique_lock<std::mutex> hold(m);
	cv.wait(hold, [&] { return done; });
	return v;
}

/*
 * The GPU process running now (0: none), whether it is not to be started
 * again, how often a GPU that came restarted it and which one it let go
 * of the last time
 */
void br_proc_gpu_state( int *pid, bool *none, int *arrivals, int *arrived_from )
{
	*pid = g_gpu_pid;
	*none = g_gpu_none;
	*arrivals = g_gpu_arrivals;
	*arrived_from = g_gpu_arrived_from;
}
