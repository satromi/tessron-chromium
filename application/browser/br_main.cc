/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_main.cc
 *	ブラウザ: an application that shows web pages in a window of its own
 *	(design 17.19). It runs as a process, opened on a page object: a real
 *	object made from its base in the template box, which the desktop
 *	gives it when a link to one is opened. The window says it shows that
 *	object, opens where the object keeps it and starts at the address the
 *	object holds, its home page. The pages gone to afterwards leave the
 *	object as it is: only the menu's 〈編集〉〈ホームページに設定〉 writes
 *	the address of the page shown into it; the window's place is written
 *	back when it closes (br_obj.cc).
 *
 *	The page fills the window's work area. It is drawn with Skia straight
 *	into the surface of the work area the system maps into the process
 *	(wm_obj_surf), only where it changed (what the engine says changed),
 *	and those rectangles are put in the window (wm_obj_put). Where the
 *	system gives no surface, the whole view is drawn into a surface of
 *	the process's own and put in the window with dp_put_argb. With a GPU
 *	(make GL=1, port/gpu/br_gpu.cc) the rectangles are rastered there by
 *	Skia's Ganesh and read back into the same pixels. What drawing takes
 *	is said on the console every 5 seconds while views are drawn.
 *	A window a page opens that keeps its opener (window.open, a
 *	document's picture-in-picture window) is a page of the same engine,
 *	shown in a window of its own that this process makes: its page, a tool
 *	panel of its own but for picture-in-picture, whose field says where the
 *	window really is, its title on its band; its page takes the keys (through
 *	the kana-kanji converter, as the page's window's do), the buttons,
 *	the pointer and the wheel, and goes where its links go. A
 *	window opened with no opener (a link to _blank) is a browser of its
 *	own, another process started on the address. A video's
 *	picture-in-picture window is a window of this process too, which
 *	draws the video's player frame after frame (port/blink/pip_tessronos.cc).
 *	The address and the switches to go back, forward, load again and
 *	stop are on a tool panel, a small window subordinate to the page's
 *	window above it; what is happening (loading, the page's size and
 *	time) is said on the system's message line.
 *
 *	Keys: in the address, letters, Backspace, Ctrl+A to take the whole
 *	of it, Enter to go, Esc to leave it. Elsewhere the arrows, Page Up
 *	and Page Down, Space, Home and End scroll; Backspace goes back.
 *	Ctrl+L goes to the address, Ctrl+R and F5 load again, Alt+Left and
 *	Alt+Right go back and forward, Ctrl+E closes. The wheel scrolls where
 *	the system tells of it. A press on a link follows it; the right
 *	button opens the window's menu (its definition takes in the menu
 *	every window has).
 *
 *	With an engine that runs the page (Blink), the keys, the buttons,
 *	the pointer's moves and the wheel go to the page, which scrolls
 *	itself; Backspace is the page's too (it does not go back, as in
 *	Chrome). The engine keeps the session history, the page's pushState
 *	and fragments among its entries; the address field, the band and
 *	the message line follow what the page does by itself (follow_doc).
 *	The right button is the page's contextmenu first, then the window's
 *	menu with the items for what is under the pointer. Ctrl and + - 0
 *	zoom, F11 and the page's requestFullscreen show it over the screen.
 *	A key typed into a field of the page goes first to the system's
 *	kana-kanji converter in the input mode the message line shows: what
 *	it commits goes into the field, what it is still converting is shown
 *	in the field as the composition, and its candidates are listed under
 *	the caret, chosen with a number or a press.
 *
 *	The start-up argument is the page object's UUID. Text may follow it
 *	after a newline, or stand alone with an address to go to as its first
 *	line; the lines are for tests -- "follow" follows the page's first
 *	link once it is shown, "sethome" then sets the page shown as the
 *	object's home page as the menu's item does, "title=<path>" writes
 *	there at the end the title of the page shown last, "ready=<path>"
 *	makes that file when the last
 *	page is shown, "quit=<ms>" ends that long after, "mem" prints what
 *	the engine holds in memory before and after collecting its garbage,
 *	"trace" prints the events the window is sent, "sound=<path>" writes
 *	there at the end how many frames Web Audio gave the sound device,
 *	"then=<address>" goes there once the page before it is shown (the
 *	line may be given again), "proto=<path>" writes there at the end how
 *	many responses came over HTTP/3, HTTP/2 and HTTP/1.1 and over which
 *	the first came, "forget" starts without what earlier runs learnt of
 *	servers (which speak HTTP/3, their QUIC sessions) and keeps nothing
 *	of this run's, "dns=system" looks names up through the system's
 *	resolver alone (no HTTPS records), "quic=off" keeps every request
 *	on TCP (no HTTP/3), "v8=<flags>" gives V8 those flags
 *	(--jitless: no machine code, to compare), "stat" makes F12 print
 *	what drawing the view has taken since the last F12, "draw=copy"
 *	draws the whole view each time and copies it in with dp_put_argb,
 *	as where the system gives no surface, "check=<path>" paints the
 *	whole view again after each paint of what changed and compares the
 *	two, writing there at the end how many views were compared and how
 *	many pixels differed, "script=<address>" fetches the script there
 *	once the page is shown and runs it in the page (the line may be
 *	given again), "secure=<origin>" treats pages from that origin as
 *	secure contexts (EME is only there), "gpu=on" rasters the page on
 *	the GPU whatever the OpenGL ES (softpipe too), "gpu=off" on the
 *	processor (the default is the GPU when the machine has one),
 *	"gpufake=<path>" has the DRM shim pretend the Raspberry Pi 5's V3D
 *	(its jobs end without drawing), so that on a machine without one the
 *	EGL platform takes the driver v3d, finds its check frame wrong and
 *	goes back to softpipe, writing there at the end the driver and the
 *	step it got to (ts_gl_probe_result), and implies "gpu=on",
 *	"gles=<major.minor>" has Mesa's contexts give OpenGL ES up to that
 *	version alone (3.0: what the V3D gives, so that softpipe refuses a
 *	context of 3.1 as the V3D does),
 *	"gpucheck=<path>" paints the whole view again on the processor
 *	after each view rastered on the GPU and compares the two, writing
 *	there at the end how many views and pixels were compared, how many
 *	pixels differed, how many by more than 48 in a colour, the largest
 *	difference, how many frames of videos were made RGB from their
 *	planes on the GPU and on the processor, and how many records of
 *	canvases were rastered on the GPU, "print=<path>" writes there at
 *	the end how many pages 印刷 printed and the desktop's last answer,
 *	"menu=<code>" chooses that item of the window's menu once the page is
 *	shown (the line may be given again), "wvrepo=<address>" fetches the
 *	Widevine CDM 〈Widevineを導入〉 installs from that copy of the archive
 *	(an https address, or the tests' server on 127.0.0.1).
 *	With "ready=" the exit code is 4 when the last page did not come.
 */

#include TS_CONFIG_H
#include <stdint.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/uapp.h>
#include <ts/sysdef.h>
#include <ts/face.h>
#include <ts/wm.h>
#include <ts/part.h>
#include <ts/ui.h>
#include <ts/snd.h>
#include "port/media/br_share.h"
#include "port/ext/br_ext.h"

/* What an extension declares, on its panel: true when its reader lets it in */
bool ext_ask_panel( const std::string &title, const std::string &name, const std::string &version,
		    const std::vector<std::string> &wants );
#include <ts/obdrop.h>
#include <ts/dtreq.h>
#include <ts/dt.h>
#include <ts/hid.h>
#include <ts/kinsoku.h>
#include <ts/sbx.h>
/* the kernel's calls (tk_get_tid, tk_ref_tsk) are C: the header does not say so itself */
extern "C" {
#include <tk/syscall.h>
}

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "port/base/counter_tessronos.h"
#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include "include/core/SkCanvas.h"
#include "include/core/SkData.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkPaint.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTypeface.h"
#include "include/ports/SkFontMgr_data.h"

#include "br.h"

/* The push service and the tests' keys, from the start-up lines "push=" and "pushkey=" */
#ifdef BR_BLINK
std::string g_step_host;		/* a test's "hstsadd=" */
extern std::string br_push_server;	/* port/net/push_tessronos.cc */
extern std::string br_push_test_key;
extern int64_t br_periodic_sync_ms;	/* port/blink/background_tessronos.cc */
extern std::string br_netlog;		/* port/net/net_fetch_tessronos.cc */
#else
std::string br_push_server, br_push_test_key;
int64_t br_periodic_sync_ms;
std::string br_netlog;			/* the start-up line "netlog=", kept for nothing */
#endif
#include "port/net/br_objfile.h"
#ifdef BR_BLINK
#include "port/net/br_data.h"
#include "port/net/br_tray.h"
#include "br_uidata.h"
#endif

/* A request to the desktop, its answer waited for (port/net/br_print.cc) */
/* A request's words (DT_TAIL_*) as long as they are, each with its kind */
typedef std::vector<std::pair<UINT, std::string>> BrDtParts;
ER br_ask_desktop( T_DTREQ *rq, const BrDtParts &parts, T_DTANS *an, int wait_ms, BrDtParts *words = nullptr );
#ifdef BR_BLINK
#include <ts/gpuapp.h>
#include "port/gpu/br_gpu.h"
#include "port/net/br_store.h"
#include "port/net/br_net.h"
#include "port/media/br_widevine.h"

/* The frames of the videos painted: shown, uploaded as YUV planes, made RGB on the processor (port/media) */
void br_media_frames( long *shown, long *planes, long *rgb );
/* The frames WebGL drew that were shown (port/webgl/gl_angle.cc, linked with make GL=1) */
long br_webgl_frames() __attribute__((weak));

/* The frames Web Audio has given the sound device (port/media/web_audio_tessronos.cc) */
long br_web_audio_frames();
/* The frames the sound device has taken (port/media/br_snd.cc) */
extern "C" long br_snd_frames( void );
#endif

bool (*br_stop_pushed)() = nullptr;
void (*br_turn_ended)( const char *doing, long ms ) = nullptr;

#ifdef BR_BLINK
#include "br_proc.h"
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
/* the descriptors handed to a renderer (lib/libcxxrt/ts_chan.c) */
extern "C" int ts_chan_give( int pid, const int *fds, int n );
extern "C" int ts_chan_take( int *fds, int max, int ms );
/* the window's pages in renderers of their own, unless a test's "process=single" (design 14.13) */
bool br_multi_process = true;
bool br_is_renderer = false;
bool br_is_network = false;
bool br_is_gpu = false;
/*
 * What the renderers and the network process do of their sandbox (design
 * 14.13.9), from the start-up line "sandbox=": 0 none ("off"), 1 the calls
 * shut out refused ("refuse", the default), 2 the process ended by them
 * ("kill")
 */
int br_sandbox_mode = 1;
/*
 * The renderer shut out of the objects as well ("sandbox_renderer=noobj"):
 * the stores' objects are the network process's (store_service_tessronos.cc);
 * what else a renderer opens is still to be moved out (14.13.9)
 */
bool br_sandbox_noobj = false;
#endif

/*
 * The engine thread's loop waits for what comes (br_loop_wait): a notice,
 * a command or a question queued for it from another thread wakes it
 */
void loop_wake()
{
#ifdef BR_BLINK
	br_loop_wake();
#endif
}

/*
 * The DRM shim's pretended V3D and how far the EGL platform got with the
 * GPU (tools/mesa/shim/ts_glport.h; linked with make GL=1)
 */
extern "C" void ts_gl_drm_fake( int on ) __attribute__((weak));
extern "C" int ts_gl_probe_result( char *driver, int n ) __attribute__((weak));

#ifdef BR_BLINK
/*
 * What the engine's side counts (the videos' frames, WebGL's, the GPU's
 * raster), of this process or, with renderers, of the window's page's
 * renderer (br_sys_query)
 */
BrEngine *main_engine();

void media_frames( long *shown, long *planes, long *rgb, long *webgl )
{
	if ( br_multi_process && !br_is_renderer ) {
		std::string v = br_proc_query(main_engine(), "mediaframes");
		*shown = *planes = *rgb = *webgl = 0;
		(void)sscanf(v.c_str(), "%ld %ld %ld %ld", shown, planes, rgb, webgl);
		return;
	}
	br_media_frames(shown, planes, rgb);
	*webgl = ( br_webgl_frames != nullptr ) ? br_webgl_frames() : 0;
}

bool gpu_stat( BrGpuStat *g )
{
	if ( br_multi_process && !br_is_renderer ) {
		std::string v = br_proc_query(main_engine(), "gpustat");
		return sscanf(v.c_str(), "%ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld", &g->views, &g->play_us,
			      &g->flush_us, &g->read_us, &g->max_us, &g->read_px, &g->canvases, &g->canvas_us, &g->trips,
			      &g->proc, &g->trip_us, &g->gs_views, &g->gs_take_us) >= 9;
	}
	if ( br_gpu_stat == nullptr ) return false;
	br_gpu_stat(g);
	return true;
}
#endif

/* A program of Linux's stack size: the engine recurses through a page */
size_t __ts_main_stack_size = 8UL * 1024 * 1024;

#ifdef BR_BLINK
/*
 * Where the engine's thread waits. The program is linked with its waits
 * on a condition and its locks wrapped (--wrap, tools/browser/build.sh):
 * when the engine's thread waits on a condition, or finds a lock taken,
 * the return addresses up its frame pointers are noted with the time,
 * and the window's thread prints them when that thread has not come
 * round for long (the addresses are looked up in the program's symbols
 * on the Linux side). A lock found free costs one try more.
 */
static pthread_t		g_engine_thread;
static volatile bool		g_engine_known = false;
static int			g_wait_depth = 0;		/* the engine's thread's own */
static std::atomic<long>	g_wait_since{0};		/* ms, 0: not waiting */
static const char *volatile	g_wait_what = "";
static uintptr_t		g_wait_frames[12];
static volatile int		g_wait_nframes = 0;

static long wait_now_ms()
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* On the engine's thread: a wait begins; its callers noted */
static bool wait_enter( const char *what )
{
	if ( !g_engine_known || !pthread_equal(pthread_self(), g_engine_thread) ) return false;
	if ( g_wait_depth++ > 0 ) return true;
	uintptr_t fp = (uintptr_t)__builtin_frame_address(0), lo = fp, hi = fp + __ts_main_stack_size;
	int n = 0;
	while ( n < 12 && fp >= lo && fp + 16 <= hi && ( fp & 7 ) == 0 ) {
		uintptr_t next = ((const uintptr_t *)fp)[0];
		g_wait_frames[n++] = ((const uintptr_t *)fp)[1];
		if ( next <= fp ) break;
		fp = next;
	}
	g_wait_nframes = n;
	g_wait_what = what;
	g_wait_since = wait_now_ms();
	return true;
}

static void wait_leave( bool mine )
{
	if ( mine && --g_wait_depth == 0 ) g_wait_since = 0;
}

extern "C" {
int __real_pthread_cond_wait( pthread_cond_t *c, pthread_mutex_t *m );
int __real_pthread_cond_timedwait( pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *t );
int __real_pthread_mutex_lock( pthread_mutex_t *m );

int __wrap_pthread_cond_wait( pthread_cond_t *c, pthread_mutex_t *m )
{
	bool mine = wait_enter("a condition");
	int r = __real_pthread_cond_wait(c, m);
	wait_leave(mine);
	return r;
}

int __wrap_pthread_cond_timedwait( pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *t )
{
	bool mine = wait_enter("a condition, with a time limit");
	int r = __real_pthread_cond_timedwait(c, m, t);
	wait_leave(mine);
	return r;
}

int __wrap_pthread_mutex_lock( pthread_mutex_t *m )
{
	if ( pthread_mutex_trylock(m) == 0 ) return 0;
	bool mine = wait_enter("a lock another thread holds");
	int r = __real_pthread_mutex_lock(m);
	wait_leave(mine);
	return r;
}
}

/*
 * The console without the wait. What the program writes on its standard
 * output and error goes to the system's console (tm_putstring), which
 * puts it out on the serial line a character at a time and returns when
 * the last has gone: on the Raspberry Pi 5, 115200 baud, some 11.5 KB a
 * second, whatever thread writes. A page whose parts come in all at once
 * has a line said for each, and the thread that says it (the engine's,
 * the network's) waited for the line. The program is linked with _write
 * wrapped (--wrap, tools/browser/build.sh): the lines of descriptors 1
 * and 2 are put in a ring of kConsoleBytes and a thread of its own gives
 * them to the console in the order they came; a writer waits only when
 * the ring is full. Before the process ends the ring is let empty
 * (br_console_drain). The time a thread spent in _write is kept for it
 * (br_console_ticks), for the measure of the engine's thread's turns
 * (br_blink.cc).
 */
extern "C" int __real__write( int fd, const void *buf, size_t n );
extern "C" void br_console_drain( long ms );

static void con_at_exit()
{
	br_console_drain(3000);
}

static const size_t		kConsoleBytes = 512 * 1024;
static pthread_mutex_t		g_con_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t		g_con_data = PTHREAD_COND_INITIALIZER;	/* something to put out */
static pthread_cond_t		g_con_room = PTHREAD_COND_INITIALIZER;	/* room in the ring, or empty */
static char			*g_con_ring;
static size_t			g_con_head, g_con_len;			/* the oldest byte, how many */
static bool			g_con_busy;				/* the thread puts some out */
static int			g_con_state;				/* 0 not started, 1 up, -1 none */
static thread_local uint64_t	g_con_ticks;

static inline uint64_t con_ticks_now()
{
	uint64_t c;

	BR_COUNTER_READ(c);
	return c;
}

static void *con_thread( void * )
{
	static char	out[4096];

	pthread_mutex_lock(&g_con_mu);
	for ( ;; ) {
		while ( g_con_len == 0 ) pthread_cond_wait(&g_con_data, &g_con_mu);
		size_t k = std::min(g_con_len, std::min(sizeof(out), kConsoleBytes - g_con_head));
		memcpy(out, g_con_ring + g_con_head, k);
		g_con_head = ( g_con_head + k ) % kConsoleBytes;
		g_con_len -= k;
		g_con_busy = true;
		pthread_mutex_unlock(&g_con_mu);
		(void)__real__write(1, out, k);
		pthread_mutex_lock(&g_con_mu);
		g_con_busy = false;
		pthread_cond_broadcast(&g_con_room);
	}
	return nullptr;
}

/* The ring and its thread, the first time; false when there are none (written at once then) */
static bool con_up()
{
	if ( g_con_state == 0 ) {
		pthread_t	th;
		pthread_attr_t	at;

		g_con_state = -1;
		g_con_ring = (char *)malloc(kConsoleBytes);
		if ( g_con_ring == nullptr ) return false;
		pthread_attr_init(&at);
		pthread_attr_setstacksize(&at, 64 * 1024);
		pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
		if ( pthread_create(&th, &at, con_thread, nullptr) == 0 ) {
			g_con_state = 1;
			atexit(con_at_exit);
		}
		pthread_attr_destroy(&at);
	}
	return g_con_state > 0;
}

extern "C" int __wrap__write( int fd, const void *buf, size_t n )
{
	if ( fd != 1 && fd != 2 ) return __real__write(fd, buf, n);
	uint64_t t0 = con_ticks_now();
	const char *p = (const char *)buf;
	size_t left = n;

	pthread_mutex_lock(&g_con_mu);
	if ( !con_up() ) {
		pthread_mutex_unlock(&g_con_mu);
		int r = __real__write(fd, buf, n);
		g_con_ticks += con_ticks_now() - t0;
		return r;
	}
	while ( left > 0 ) {
		while ( g_con_len == kConsoleBytes ) pthread_cond_wait(&g_con_room, &g_con_mu);
		size_t tail = ( g_con_head + g_con_len ) % kConsoleBytes;
		size_t k = std::min(left, std::min(kConsoleBytes - g_con_len, kConsoleBytes - tail));
		memcpy(g_con_ring + tail, p, k);
		g_con_len += k;
		p += k;
		left -= k;
		pthread_cond_signal(&g_con_data);
	}
	pthread_mutex_unlock(&g_con_mu);
	g_con_ticks += con_ticks_now() - t0;
	return (int)n;
}

/* The time this thread has spent writing on the console, in the counter's ticks */
uint64_t br_console_ticks()
{
	return g_con_ticks;
}

/* What is in the ring put out, waited for up to ms (also before V8 ends the process: platform-tessronos.cc) */
extern "C" void br_console_drain( long ms )
{
	struct timespec until;

	clock_gettime(CLOCK_REALTIME, &until);
	until.tv_sec += ms / 1000;
	until.tv_nsec += ( ms % 1000 ) * 1000000L;
	if ( until.tv_nsec >= 1000000000L ) {
		until.tv_sec++;
		until.tv_nsec -= 1000000000L;
	}
	pthread_mutex_lock(&g_con_mu);
	while ( g_con_state > 0 && ( g_con_len > 0 || g_con_busy ) ) {
		if ( pthread_cond_timedwait(&g_con_room, &g_con_mu, &until) != 0 ) break;
	}
	pthread_mutex_unlock(&g_con_mu);
}

/*
 * A thread that met a fault the process ends with (port/compat/ts_signal.c)
 * puts out what is in the ring itself, before the fault ends the process:
 * the lines said before it are not lost. When the ring's lock stays taken
 * (the fault was met while holding it) nothing is put out.
 */
extern "C" void br_console_flush_now( void )
{
	int tries;

	for ( tries = 0; tries < 200 && pthread_mutex_trylock(&g_con_mu) != 0; tries++ ) usleep(1000);
	if ( tries == 200 ) return;
	while ( g_con_state > 0 && g_con_len > 0 ) {
		size_t k = std::min(g_con_len, kConsoleBytes - g_con_head);
		(void)__real__write(1, g_con_ring + g_con_head, k);
		g_con_head = ( g_con_head + k ) % kConsoleBytes;
		g_con_len -= k;
	}
	pthread_mutex_unlock(&g_con_mu);
}
#endif

namespace {

/* The face the pages are drawn in: this one in the 書体箱, or else the first there */
const char kFaceName[] = "NotoSansJP-Regular";

const int kFrameW = 16, kTitleH = 34;		/* the frame round the work area */

/* HID usages */
enum {
	K_ENTER = 0x28, K_ESC = 0x29, K_BS = 0x2A, K_SPACE = 0x2C, K_F5 = 0x3E,
	K_HOME = 0x4A, K_PGUP = 0x4B, K_DEL = 0x4C, K_END = 0x4D, K_PGDN = 0x4E, K_RIGHT = 0x4F,
	K_LEFT = 0x50, K_DOWN = 0x51, K_UP = 0x52, K_KPENTER = 0x58,
	K_A = 0x04, K_E = 0x08, K_L = 0x0F, K_R = 0x15, K_TAB = 0x2B, K_F12 = 0x45,
	K_N = 0x11, K_F11 = 0x44, K_0 = 0x27, K_MINUS = 0x2D, K_EQUAL = 0x2E, K_SEMI = 0x33,
	K_KPMINUS = 0x56, K_KPPLUS = 0x57, K_KP0 = 0x62, K_F = 0x09, K_G = 0x0A, K_F3 = 0x3C
};
const UINT kShift = 0x22;			/* HID_MOD_LSHIFT|RSHIFT */
const UINT kCtrl = 0x11, kAlt = 0x44;		/* HID_MOD_LCTRL|RCTRL, LALT|RALT */

const char kHome[] =
	"<html><head><title>ブラウザ</title></head><body>"
	"<h1>ブラウザ</h1>"
	"<p>上のツールパネルの欄に http:// か https:// で始まるアドレスを打って Enter で開きます。"
	"ページの実身を開くと、その本文に書いたアドレス(ホームページ)を開きます。右ボタンのメニューの〈編集〉〈ホームページに設定〉で、いま開いているページをホームページにします。新しいページの実身は原紙箱の「ブラウザ」から作ります。</p>"
	"<ul><li>Ctrl+L でアドレスの欄へ、Esc でページへ戻ります。</li>"
	"<li>矢印・Page Up・Page Down・Space・Home・End でページをスクロールし、ホイールも使えます。</li>"
	"<li>リンクを押すとそのページへ、Backspace か Alt+← で前のページへ戻ります。"
	"右ボタンのメニューに、戻る・進む・再読込・アドレス入力・〈編集〉があります。</li>"
	"<li>Ctrl+R か F5 で読み直し、Ctrl+E で閉じます。</li></ul>"
	"<h2>試しに</h2>"
	"<ul><li><a href=\"http://example.com/\">http://example.com/</a></li>"
	"<li><a href=\"http://10.0.2.2:8000/\">QEMU のホスト (10.0.2.2:8000)</a></li></ul>"
#ifdef BR_BLINK
	"<p><small>ページは Blink が組んで描き、スタイルとスクリプトと画像も扱います。"
	"入力欄には、かな漢字変換で日本語も打てます(設計書 17.19)。</small></p>"
#else
	"<p><small>このバージョンはページを文字と見出し・リストで組んで描き、スクリプトとスタイルは走らせません。"
	"Blink の描画は同じインタフェースで差し替えます(設計書 17.19)。</small></p>"
#endif
	"</body></html>";

struct Browser {
	ID			kw = 0, port = 0;
	INT			gid = -1;
	TS_UUID			win;
	int			w = 0, h = 0;
	sk_sp<SkSurface>	surf;
	sk_sp<SkTypeface>	face;
	sk_sp<SkFontMgr>	mgr;
	BrEngine		*eng = nullptr;

	std::string		url, status, title;
	std::string		shown_title;		/* what the band was last given */
	std::string		shown_status;		/* what the message line was last given */
	bool			failed = false;		/* the last page did not come */
	int			scroll = 0, doc_h = 0;
	int			left = 0, doc_w = 0;	/* across, for an engine that scrolls it */
	T_OBWBARS		bars_shown;		/* what the window's bars were last given */
	bool			bars_set = false;
	bool			bar_pending = false;	/* a knob carried, to be followed */
	UINT			bar_which = 0;
	int			bar_pos = 0;
	std::vector<std::string> back, fwd;
	std::vector<UW>		px;
	T_WMSURF		shm;			/* the work area's surface, mapped in */
	bool			shared = false;		/* drawn in there and put, not copied in */
	bool			full = true;		/* the whole view to be painted next */
	bool			ime_shown = false;	/* the converter's list was laid over the last view */

	BrObject		obj;			/* the page object it was opened on */

	bool			moved = false;		/* a move of the pointer to give the page */
	int			move_x = 0, move_y = 0;
	unsigned		move_mods = 0;
};

/* かな漢字変換 of what is typed into the page: the session, and the candidates shown */
struct Ime {
	INT		kid = 0;
	T_KCOUT		out;
	int		base = 0;		/* where the composition starts in out.text */
	bool		list = false;
	UB		cand[4096];
	INT		ncand = 0, first = 0, total = 0, chosen = 0;
	SkRect		box;			/* where the list was drawn, in the work area */
	int		row_h = 0;
	UINT		kmode = WM_MODE_ROMAN;	/* the input mode what is being composed was typed in */
};

Ime	g_ime;
Browser	g_br;

}  // namespace

#ifdef BR_BLINK
BrEngine *main_engine()
{
	return g_br.eng;
}
#endif

namespace {

/*
 * The page the converter composes into: the page's window's, or that of
 * a window a page opened, whichever was typed into last. Its window
 * shows the candidates.
 */
BrEngine	*g_ime_eng = nullptr;

/* The page the converter's text goes to */
BrEngine *ime_target()
{
	return g_ime_eng != nullptr ? g_ime_eng : g_br.eng;
}

/* Whether the converter's candidates are shown over this page's window */
bool ime_list_on( const BrEngine *eng )
{
	return g_ime.list && ime_target() == eng;
}

long now_ms()
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

long now_us()
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

/* What drawing the view has taken since F12 was last pressed ("stat") */
struct Stat {
	long	frames = 0;			/* views put in the window */
	long	paint_us = 0;			/* the engine painting them */
	long	copy_us = 0;			/* the view copied into pixels of the window's kind */
	long	put_us = 0;			/* dp_put_argb, or wm_obj_put */
	long	flush_us = 0;			/* wm_obj_flush */
	long	painted = 0, put_px = 0, rects = 0;
	long	quiet = 0, quiet_us = 0;	/* views asked for in which nothing had changed */
};

Stat	g_stat;
bool	g_stat_on = false;
bool	g_copy = false;				/* "draw=copy" */
std::string g_check;				/* "check=": where the comparisons are written */
long	g_checked = 0, g_wrong = 0;		/* views compared, pixels that differed */
int	g_gpu_mode = 0;				/* "gpu=on" 1, "gpu=off" -1 (BR_GPU_*) */
/*
 * "gpuproc=on": a renderer's page is rastered in the GPU process; off (the
 * default until that raster is checked on the machines) on the renderer's
 * own GPU thread, as before the GPU process
 */
bool	g_gpuproc = false;
/*
 * With the GPU process: the window shown from the GPU's buffers the page
 * is rastered into (design 14.13.10); "gsurf=off" reads every view back
 * into the page's memory instead, as before them
 */
bool	g_gsurf = true;
bool	g_gpu = false;				/* the page rastered on the GPU */
std::string g_gpucheck;				/* "gpucheck=": where the comparisons with the processor go */
std::string g_gpufake;				/* "gpufake=": where the GPU's check comes to is written */

/* Views rastered on the GPU against the processor ("gpucheck=") */
struct GpuCheck {
	long	views = 0, px = 0;		/* views and pixels compared */
	long	differ = 0, far = 0;		/* pixels that differed, by more than kFar in a colour */
	int	most = 0;			/* the largest difference in a colour */
};
GpuCheck	g_gc;
const int	kFar = 48;

#ifdef BR_BLINK
/*
 * What the GPU process's raster has taken since it started (its query
 * "rasterstat", microseconds): the rasters (views and canvases), their
 * records played, their flushes; the readbacks, the waits for the drawing
 * to finish before them, the copies
 */
struct GpuWork {
	long	rasters = 0, play_us = 0, flush_us = 0, reads = 0, finish_us = 0, copy_us = 0;
};

GpuWork gpu_work()
{
	GpuWork w;
	std::string v = br_proc_gpu_query("rasterstat");
	if ( sscanf(v.c_str(), "%ld %ld %ld %ld %ld %ld", &w.rasters, &w.play_us, &w.flush_us, &w.reads, &w.finish_us,
		    &w.copy_us) != 6 ) {
		w = GpuWork();
	}
	return w;
}
#endif

/*
 * What drawing has taken in a spell of 5 s, said on the console at its
 * end while views are drawn: the views a second and what one took, and
 * on the GPU its parts, the canvases, the frames of videos and of WebGL
 */
struct Pace {
	long		t0 = 0;
	long		views = 0, us = 0, max_us = 0, px = 0;
#ifdef BR_BLINK
	BrGpuStat	gpu;
	GpuWork		work;
	BrGsurfStat	gs;
	long		shown = 0, planes = 0, rgb = 0, webgl = 0;
#endif
};
Pace	g_pace;
const long kPaceMs = 5000;

void stat_print()
{
	static int	nth;
	const Stat	&s = g_stat;
	long		f = ( s.frames > 0 ) ? s.frames : 1;

	printf("browser: stat %d: %ld frames; a frame: paint %ld us, copy %ld us, put %ld us, flush %ld us, "
	       "%ld px painted, %ld px put in %ld.%02ld rects%s\n", ++nth, s.frames, s.paint_us / f, s.copy_us / f,
	       s.put_us / f, s.flush_us / f, s.painted / f, s.put_px / f, s.rects / f, s.rects * 100 / f % 100,
	       g_gpu ? " (on the GPU)" : "");
	if ( s.quiet > 0 ) {
		printf("browser: stat: %ld views with nothing changed, %ld us each\n", s.quiet, s.quiet_us / s.quiet);
	}
	if ( !g_check.empty() ) {
		printf("browser: check: %ld views compared, %ld pixels differed\n", g_checked, g_wrong);
	}
	g_stat = Stat();
}

/* A face's bytes: the one of that name in the 書体箱, or (any) the first there when it is not */
sk_sp<SkData> read_face_named( const char *name, bool any )
{
	std::vector<unsigned char>	buf;
	const SZ			piece = 256 * 1024;
	SZ				got = 0, asz = 0;
	TS_UUID				face;
	ID				k;

	if ( ts_face_find(name, &face) < E_OK && ( !any || ts_face_find(nullptr, &face) < E_OK ) ) {
		return nullptr;
	}
	k = ob_opn_obj(&face, OB_OP_READ);
	if ( k <= 0 ) return nullptr;
	for ( ;; ) {
		buf.resize((size_t)( got + piece ));
		asz = 0;
		if ( ob_rea_rec(k, 1, (D)got, buf.data() + got, piece, &asz) < E_OK ) break;
		got += asz;
		if ( asz < piece ) break;
	}
	ob_cls_obj(k);
	return ( got > 0 ) ? SkData::MakeWithCopy(buf.data(), (size_t)got) : nullptr;
}

sk_sp<SkData> read_face()
{
	return read_face_named(kFaceName, true);
}

}  // namespace

/* A face of the 書体箱 by its name, none when it is not there (the engine's serif face) */
sk_sp<SkData> br_face_data( const char *name )
{
	return read_face_named(name, false);
}

/*
 * The steps of the start, each with its time: from main to the engine's
 * thread first coming round (beat "starting"), said on one line then
 * ("browser: start: ..."). The time before main (the program read and
 * its process made) is the system's line "elf: ...".
 */
namespace {
long		g_start_at, g_start_last;
std::string	g_start_steps;
}  // namespace

void br_start_mark( const char *what )
{
	long now = now_ms();

	if ( g_start_at == 0 ) {
		g_start_at = g_start_last = now;
		return;
	}
	if ( what == nullptr ) return;
	char buf[96];
	snprintf(buf, sizeof(buf), "%s%s %ld", g_start_steps.empty() ? "" : ", ", what, now - g_start_last);
	g_start_steps += buf;
	g_start_last = now;
}

namespace {

/* The line of the start, once ('who' says whose: the browser's, a renderer's) */
void start_say( const char *who = "browser: start" )
{
	if ( g_start_at == 0 ) return;
	printf("%s: %ld ms from main: %s ms\n", who, now_ms() - g_start_at, g_start_steps.c_str());
	g_start_at = 0;
}

int view_h()
{
	return g_br.h;
}

void clamp_scroll()
{
	int max = g_br.doc_h - view_h();
	if ( g_br.scroll > max ) g_br.scroll = max;
	if ( g_br.scroll < 0 ) g_br.scroll = 0;
}

SkFont ui_font( float size )
{
	SkFont f(g_br.face, size);
	f.setEdging(SkFont::Edging::kAntiAlias);
	return f;
}

/*
 * Where the view is in the page, on the window's scroll bars: down the
 * right side the page's height, along the foot its width. They are
 * written only when that changed, as the frame is drawn again then.
 */
void show_bars()
{
	T_OBWBARS	b;
	SZ		asz = 0;
	int		wide = ( g_br.doc_w > g_br.w ) ? g_br.doc_w : g_br.w;
	int		tall = ( g_br.doc_h > view_h() ) ? g_br.doc_h : view_h();

	memset(&b, 0, sizeof(b));
	b.bar[OB_BAR_R].hi = tall;
	b.bar[OB_BAR_R].clo = g_br.scroll;
	b.bar[OB_BAR_R].chi = g_br.scroll + view_h();
	b.bar[OB_BAR_B].hi = wide;
	b.bar[OB_BAR_B].clo = g_br.left;
	b.bar[OB_BAR_B].chi = g_br.left + g_br.w;
	if ( g_br.bars_set && memcmp(&b, &g_br.bars_shown, sizeof(b)) == 0 ) return;
	if ( ob_wri_rec(g_br.kw, OB_WR_BARS, 0, &b, 2 * sizeof(T_OBWBAR), &asz) >= E_OK ) {
		g_br.bars_shown = b;
		g_br.bars_set = true;
	}
}

long area( const BrRect &r )
{
	return (long)r.w * r.h;
}

BrRect around( const BrRect &a, const BrRect &b )
{
	BrRect	u;
	int	r = std::max(a.x + a.w, b.x + b.w), btm = std::max(a.y + a.h, b.y + b.h);

	u.x = std::min(a.x, b.x);
	u.y = std::min(a.y, b.y);
	u.w = r - u.x;
	u.h = btm - u.y;
	return u;
}

/*
 * What changed, cut to the view and made into at most WM_PUT_MAX
 * rectangles: one that overlaps or lies near another is joined to it
 * when the rectangle round both is no more than kSlack pixels larger
 * than the two (each rectangle put costs the system a walk of the
 * windows as it builds the screen), and when there are too many, the
 * last is joined to the one it adds least to.
 */
const long kSlack = 4096;

void gather( std::vector<BrRect> &v, int w, int h )
{
	std::vector<BrRect>	out;

	for ( BrRect r : v ) {
		int x1 = std::min(r.x + r.w, w), y1 = std::min(r.y + r.h, h);
		r.x = std::max(r.x, 0);
		r.y = std::max(r.y, 0);
		r.w = x1 - r.x;
		r.h = y1 - r.y;
		if ( r.w <= 0 || r.h <= 0 ) continue;
		for ( bool joined = true; joined; ) {
			joined = false;
			for ( size_t i = 0; i < out.size(); i++ ) {
				BrRect u = around(out[i], r);
				if ( area(u) <= area(out[i]) + area(r) + kSlack ) {
					r = u;
					out.erase(out.begin() + (long)i);
					joined = true;
					break;
				}
			}
		}
		if ( out.size() < (size_t)WM_PUT_MAX ) {
			out.push_back(r);
			continue;
		}
		size_t best = 0;
		for ( size_t i = 1; i < out.size(); i++ ) {
			if ( area(around(out[i], r)) - area(out[i]) < area(around(out[best], r)) - area(out[best]) ) {
				best = i;
			}
		}
		out[best] = around(out[best], r);
	}
	v.swap(out);
}

/*
 * The view just painted where it changed, against the whole view
 * painted again from the same state ("check="): a pixel that differs is
 * one the engine did not say had changed
 */
void check_view( const std::vector<BrRect> &dmg )
{
	sk_sp<SkSurface> s = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(g_br.w, view_h()));
	SkPixmap	a, b;
	long		bad = 0;
	int		fx = -1, fy = -1, x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1, most = 0;

	if ( s == nullptr || g_br.eng == nullptr ) return;
	if ( !s->peekPixels(&a) || !g_br.surf->peekPixels(&b) ) return;
	/* on the GPU the whole view is rastered there too: the same way, so the same pixels */
	std::vector<BrRect> whole(1);
	whole[0].w = g_br.w;
	whole[0].h = view_h();
	if ( !g_gpu || !g_br.eng->paint_gpu(whole, g_br.scroll, g_br.w, view_h(), a.writable_addr(), a.rowBytes()) ) {
		g_br.eng->paint(s->getCanvas(), g_br.scroll, view_h());
	}
	for ( int y = 0; y < view_h(); y++ ) {
		const uint32_t *p = a.addr32(0, y), *q = b.addr32(0, y);
		for ( int x = 0; x < g_br.w; x++ ) {
			if ( p[x] != q[x] ) {
				if ( bad++ == 0 ) {
					fx = x;
					fy = y;
				}
				x0 = std::min(x0, x);
				y0 = std::min(y0, y);
				x1 = std::max(x1, x);
				y1 = std::max(y1, y);
				for ( int sh = 0; sh < 24; sh += 8 ) {
					int e = (int)( ( p[x] >> sh ) & 0xFF ) - (int)( ( q[x] >> sh ) & 0xFF );
					most = std::max(most, e < 0 ? -e : e);
				}
			}
		}
	}
	if ( bad > 0 && g_br.shared ) {
		/*
		 * A window being deleted (the test's own end) takes its surface
		 * away under the view: what is compared then is not the view
		 */
		T_WMSURF sv;
		if ( wm_obj_surf(g_br.kw, &sv) < E_OK || sv.serial != g_br.shm.serial ) return;
	}
	g_checked++;
	g_wrong += bad;
	if ( bad > 0 ) {
		printf("browser: check: %ld pixels differ from the whole view, the first at %d,%d, within %d,%d-%d,%d, "
		       "by %d at most; painted in", bad, fx, fy, x0, y0, x1, y1, most);
		for ( const BrRect &r : dmg ) printf(" %d,%d %dx%d", r.x, r.y, r.w, r.h);
		printf("\n");
	}
}

/*
 * The view rastered on the GPU, against the whole view painted again on
 * the processor ("gpucheck="): the two differ a little where they draw
 * edges and text differently, not in what they draw. The alpha is
 * compared too: the view is opaque on both.
 *
 * With renderers it is made in the renderer, which has both: the
 * browser's window pixels are a copy of the renderer's page memory, and
 * the browser's engine paints from that same memory.
 */
void gpu_check_pixels( BrEngine *eng, int top, int w, int h, const void *px, size_t pitch )
{
	sk_sp<SkSurface> s = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(w, h));
	SkPixmap	a, b(SkImageInfo::MakeN32Premul(w, h), px, pitch);
	long		differ = 0, far = 0;
	int		most = 0, fx = -1, fy = -1;

	if ( s == nullptr || eng == nullptr || w <= 0 || h <= 0 ) return;
	eng->paint(s->getCanvas(), top, h);
	if ( !s->peekPixels(&a) ) return;
	for ( int y = 0; y < h; y++ ) {
		const uint32_t *p = a.addr32(0, y), *q = b.addr32(0, y);
		for ( int x = 0; x < w; x++ ) {
			uint32_t u = p[x], v = q[x];
			if ( u == v ) continue;
			int d = 0;
			for ( int sh = 0; sh < 32; sh += 8 ) {
				int e = (int)( ( u >> sh ) & 0xFF ) - (int)( ( v >> sh ) & 0xFF );
				if ( e < 0 ) e = -e;
				if ( e > d ) d = e;
			}
			differ++;
			if ( d > kFar && far++ == 0 ) {
				fx = x;
				fy = y;
			}
			if ( d > most ) most = d;
		}
	}
	g_gc.views++;
	g_gc.px += (long)w * h;
	g_gc.differ += differ;
	g_gc.far += far;
	if ( most > g_gc.most ) g_gc.most = most;
	if ( far > 0 ) {
		printf("browser: gpucheck: %ld pixels differ from the processor's by more than %d, the first at %d,%d "
		       "(%ld differ, by %d at most)\n", far, kFar, fx, fy, differ, most);
	}
}

void gpu_check_view()
{
	SkPixmap b;
	if ( g_br.surf->peekPixels(&b) ) gpu_check_pixels(g_br.eng, g_br.scroll, g_br.w, view_h(), b.addr(), b.rowBytes());
}

#ifdef BR_BLINK
/*
 * The GPU's lines of a spell: what the views rastered since 'g0' (the
 * window's GPU buffers since 's0', the GPU process's work since 'w0') took
 */
void pace_gpu_lines( const BrGpuStat &g, const BrGpuStat &g0, const BrGsurfStat &s0, const GpuWork &w00 )
{
	if ( g.views <= g0.views ) return;
	/*
	 * the same items on either side: in the renderer's GPU thread the
	 * record played, given to the driver and read back; with the GPU
	 * process the renderer's side of each (the record put in commands,
	 * sent, and the readback that waits for the GPU process's work),
	 * and on a line of its own what the GPU process did
	 */
	long n = g.views - g0.views;
	/* the readback over the views read back: those rastered into the window's GPU buffers are not */
	long rn = n - ( g.gs_views - g0.gs_views );
	printf("browser: draw: on the GPU a view: play %ld us, flush %ld us, read back %ld us (%ld kpx)%s; "
	       "%ld canvases rastered, %ld us each\n",
	       ( g.play_us - g0.play_us ) / n, ( g.flush_us - g0.flush_us ) / n,
	       rn > 0 ? ( g.read_us - g0.read_us ) / rn : 0, rn > 0 ? ( g.read_px - g0.read_px ) / rn / 1000 : 0,
	       g.proc ? " in the renderer, of the GPU process" : "", g.canvases - g0.canvases,
	       ( g.canvases > g0.canvases ) ? ( g.canvas_us - g0.canvas_us ) / ( g.canvases - g0.canvases ) : 0);
	BrGsurfStat s;
	br_proc_gsurf_stat(&s);
	long gn = g.gs_views - g0.gs_views, sn = s.shown - s0.shown;
	if ( gn > 0 || sn > 0 ) {
		/*
		 * the views rastered into the window's GPU buffers: from the end
		 * of each raster in the renderer to the GPU process having taken
		 * it in, and from the window being asked to show each to its
		 * saying it does (the GPU's work on it over)
		 */
		printf("browser: draw: window buffers (%s): %ld views rastered into them, taken in by the GPU process "
		       "%ld us after each; %ld shown, each %ld us after the window was asked; let go %ld times for the "
		       "page's memory\n", s.how.empty() ? "none" : s.how.c_str(), gn,
		       gn > 0 ? ( g.gs_take_us - g0.gs_take_us ) / gn : 0, sn,
		       sn > 0 ? ( s.shown_us - s0.shown_us ) / sn : 0, s.drops - s0.drops);
	}
	if ( g.proc ) {
		GpuWork w = gpu_work(), w0 = w00;
		if ( w.rasters < w0.rasters || w.reads < w0.reads ) w0 = GpuWork();	/* started again meanwhile */
		long trips = ( g.trips - g0.trips ) * 10 / n;
		long r = w.rasters - w0.rasters, k = w.reads - w0.reads;
		printf("browser: draw: in the GPU process: %ld.%ld round trips a view, %ld us each; %ld rasters (views and "
		       "canvases): play %ld us, flush %ld us each; %ld readbacks: the drawing finished %ld us, "
		       "copied %ld us each\n",
		       trips / 10, trips % 10, g.trip_us, r, r > 0 ? ( w.play_us - w0.play_us ) / r : 0,
		       r > 0 ? ( w.flush_us - w0.flush_us ) / r : 0, k, k > 0 ? ( w.finish_us - w0.finish_us ) / k : 0,
		       k > 0 ? ( w.copy_us - w0.copy_us ) / k : 0);
	}
}

/* The videos' and WebGL's lines of a spell of 'ms': the frames since the counts 'c0' */
struct MediaCount {
	long	shown = 0, planes = 0, rgb = 0, webgl = 0;
};

void pace_media_lines( long ms, const MediaCount &c, const MediaCount &c0 )
{
	if ( ms <= 0 ) return;
	if ( c.shown > c0.shown ) {
		printf("browser: draw: video %ld frames, %ld.%ld/s; %ld as YUV planes made RGB on the GPU, "
		       "%ld made RGB on the processor\n", c.shown - c0.shown, ( c.shown - c0.shown ) * 1000 / ms,
		       ( c.shown - c0.shown ) * 10000 / ms % 10, c.planes - c0.planes, c.rgb - c0.rgb);
	}
	if ( c.webgl > c0.webgl ) {
		printf("browser: draw: webgl %ld frames, %ld.%ld/s\n", c.webgl - c0.webgl, ( c.webgl - c0.webgl ) * 1000 / ms,
		       ( c.webgl - c0.webgl ) * 10000 / ms % 10);
	}
}

/*
 * With renderers, the counts of the window's page are its renderer's: they
 * are asked at the end of each spell and not waited for (a page's script
 * may hold its renderer for long, and the engine thread serves the other
 * windows and renderers meanwhile). The lines are said when the answer
 * comes, of the time since the answer before; a spell whose question is
 * still unanswered asks nothing more, and no line is said for it.
 */
struct PaceAsked {
	bool		have = false;		/* an answer came: what the next is set beside */
	const BrEngine	*eng = nullptr;		/* of which page */
	long		at = 0;			/* when it came */
	BrGpuStat	gpu;
	BrGsurfStat	gs;
	GpuWork		work;
	MediaCount	media;
};
PaceAsked	g_pace_gpu, g_pace_media;

void pace_ask()
{
	BrEngine *eng = main_engine();
	if ( eng == nullptr ) return;
	if ( g_gpu ) {
		(void)br_proc_query_async(eng, "gpustat", base::BindOnce([]( const BrEngine *eng, std::string v ) {
			PaceAsked &b = g_pace_gpu;
			BrGpuStat g;
			if ( sscanf(v.c_str(), "%ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld", &g.views, &g.play_us,
				    &g.flush_us, &g.read_us, &g.max_us, &g.read_px, &g.canvases, &g.canvas_us, &g.trips,
				    &g.proc, &g.trip_us, &g.gs_views, &g.gs_take_us) < 9 ) {
				return;
			}
			if ( b.have && b.eng == eng ) pace_gpu_lines(g, b.gpu, b.gs, b.work);
			b.have = true;
			b.eng = eng;
			b.at = now_ms();
			b.gpu = g;
			br_proc_gsurf_stat(&b.gs);
			b.work = g.proc ? gpu_work() : GpuWork();
		}, eng));
	}
	(void)br_proc_query_async(eng, "mediaframes", base::BindOnce([]( const BrEngine *eng, std::string v ) {
		PaceAsked &b = g_pace_media;
		MediaCount c;
		if ( sscanf(v.c_str(), "%ld %ld %ld %ld", &c.shown, &c.planes, &c.rgb, &c.webgl) < 4 ) return;
		long t = now_ms();
		if ( b.have && b.eng == eng ) pace_media_lines(t - b.at, c, b.media);
		b.have = true;
		b.eng = eng;
		b.at = t;
		b.media = c;
	}, eng));
}
#endif

/* The spell's end: what drawing took, on the console */
void pace_say( long ms )
{
	Pace	&p = g_pace;
	long	v = p.views > 0 ? p.views : 1;

	printf("browser: draw %ld.%ld s (%s): %ld views, %ld.%ld/s; a view %ld.%ld ms (at most %ld.%ld), %ld kpx\n",
	       ms / 1000, ms % 1000 / 100, g_gpu ? "gpu" : "cpu", p.views, p.views * 1000 / ms,
	       p.views * 10000 / ms % 10, p.us / v / 1000, p.us / v % 1000 / 100, p.max_us / 1000,
	       p.max_us % 1000 / 100, p.px / v / 1000);
#ifdef BR_BLINK
	if ( br_multi_process ) {
		pace_ask();
		return;
	}
	BrGpuStat g;
	if ( g_gpu && gpu_stat(&g) ) pace_gpu_lines(g, p.gpu, p.gs, p.work);
	MediaCount c, c0;
	media_frames(&c.shown, &c.planes, &c.rgb, &c.webgl);
	c0.shown = p.shown;
	c0.planes = p.planes;
	c0.rgb = p.rgb;
	c0.webgl = p.webgl;
	pace_media_lines(ms, c, c0);
#endif
}

/* A view put in the window: counted in the spell, which is said and begun again after 5 s */
void pace_view( long us, long px )
{
	Pace	&p = g_pace;
	long	t = now_ms();

	if ( p.t0 == 0 ) {
		p = Pace();
		p.t0 = t;
#ifdef BR_BLINK
		/* with renderers the counts are set beside the answer before (pace_ask) */
		if ( !br_multi_process ) {
			if ( g_gpu && gpu_stat(&p.gpu) && p.gpu.proc ) p.work = gpu_work();
			media_frames(&p.shown, &p.planes, &p.rgb, &p.webgl);
		}
#endif
	}
	p.views++;
	p.us += us;
	p.px += px;
	if ( us > p.max_us ) p.max_us = us;
	if ( t - p.t0 >= kPaceMs ) {
		pace_say(t - p.t0);
		p.t0 = 0;
	}
}

/* The view painted where it changed (all of it when it must be), and shown */
/*
 * The converter's candidates under the caret of a page, a number before
 * each, laid over its window's view ('area_h' high)
 */
void ime_draw_list( SkCanvas *c, BrEngine *eng, int area_h )
{
	SkPaint p;
	p.setAntiAlias(true);
	int cx = 8, cy = 8, ch = 16;
	(void)eng->caret(&cx, &cy, &ch);
	SkFont lf = ui_font(15);
	const UB *s = g_ime.cand;
	float w = 0;
	int n = 0;
	std::vector<std::string> rows;
	for ( ; n < g_ime.ncand && n < 9; n++ ) {
		char row[TSMOZC_TEXT_MAX + 8];
		snprintf(row, sizeof(row), "%d %s", n + 1, (const char *)s);
		rows.push_back(row);
		float rw = lf.measureText(row, strlen(row), SkTextEncoding::kUTF8);
		if ( rw > w ) w = rw;
		s += strlen((const char *)s) + 1;
	}
	g_ime.row_h = 20;
	float top = (float)( cy + ch + 2 );
	if ( top + g_ime.row_h * n + 4 > area_h ) top = (float)( cy - g_ime.row_h * n - 6 );
	g_ime.box = SkRect::MakeXYWH((float)cx, top, w + 12, (float)( g_ime.row_h * n + 4 ));
	p.setColor(SK_ColorWHITE);
	c->drawRect(g_ime.box, p);
	p.setStyle(SkPaint::kStroke_Style);
	p.setStrokeWidth(1);
	p.setColor(0xFF808080);
	c->drawRect(g_ime.box, p);
	p.setStyle(SkPaint::kFill_Style);
	for ( int i = 0; i < n; i++ ) {
		bool sel = ( g_ime.first + i == g_ime.chosen );
		float ry = g_ime.box.top() + 2 + (float)( i * g_ime.row_h );
		if ( sel ) {
			p.setColor(0xFF0050A0);
			c->drawRect(SkRect::MakeXYWH(g_ime.box.left() + 1, ry, g_ime.box.width() - 2,
						     (float)g_ime.row_h), p);
		}
		p.setColor(sel ? SK_ColorWHITE : 0xFF000000);
		c->drawSimpleText(rows[i].data(), rows[i].size(), SkTextEncoding::kUTF8,
				  g_ime.box.left() + 6, ry + 15, lf, p);
	}
}

void say_line();

void show()
{
	SkCanvas		*c = g_br.surf->getCanvas();
	SkPaint			p;
	std::vector<BrRect>	dmg;
	long			t0 = now_us(), t1;
	bool			all = true;

	p.setAntiAlias(true);

#ifdef BR_BLINK
	/*
	 * The window's GPU buffers (design 14.13.10): given to the page while
	 * the GPU process rasters it, taken away while the converter's list is
	 * laid over the page, which is then painted into the window's surface
	 */
	bool gs = false;
	if ( br_multi_process && g_br.shared && g_br.eng != nullptr ) {
		br_proc_gsurf_follow(g_br.eng, g_br.w, view_h(),
				     g_gpuproc && g_gsurf && !ime_list_on(g_br.eng) && !g_br.ime_shown);
	}
#endif
	/*
	 * what changed, which the engine is asked each time so that it stays
	 * in step. A renderer is asked even when the view is drawn whole and
	 * copied in ("draw=copy"): asking tells it the view and gives it the
	 * page's memory to draw into, without which it draws no frame; the
	 * view is then drawn whole whenever something changed
	 */
	bool asked = g_br.shared;
#ifdef BR_BLINK
	if ( br_multi_process ) asked = true;
#endif
	if ( asked && g_br.eng != nullptr && g_br.eng->damage(g_br.scroll, view_h(), &dmg) ) {
		all = g_br.full || ( !g_br.shared && !dmg.empty() );
#ifdef BR_BLINK
		/* a frame of the GPU buffers is shown from its BO: nothing is painted nor put */
		gs = br_multi_process && g_br.shared && br_proc_gsurf_show(g_br.eng, dmg);
#endif
	}
#ifdef BR_BLINK
	if ( gs ) {
		say_line();
		show_bars();
		if ( dmg.empty() ) {
			g_stat.quiet++;
			g_stat.quiet_us += now_us() - t0;
			return;
		}
		long gpx = 0;
		for ( const BrRect &r : dmg ) gpx += area(r);
		g_stat.frames++;
		g_stat.rects += (long)dmg.size();
		g_stat.put_px += gpx;
		g_stat.paint_us += now_us() - t0;
		pace_view(now_us() - t0, gpx);
		return;
	}
#endif
	/* the converter's list is laid over the page: the view is painted whole while it is there, and once after */
	if ( ime_list_on(g_br.eng) || g_br.ime_shown ) all = true;
	g_br.full = false;
	if ( all ) {
		BrRect r;
		r.w = g_br.w;
		r.h = view_h();
		dmg.assign(1, r);
	} else {
		gather(dmg, g_br.w, view_h());
	}

	/* the page: rastered on the GPU and read back into the window's pixels, or painted there */
	SkPixmap pm;
	bool gpu = g_gpu && g_br.eng != nullptr && !dmg.empty() && g_br.surf->peekPixels(&pm)
		   && g_br.eng->paint_gpu(dmg, g_br.scroll, g_br.w, view_h(), pm.writable_addr(), pm.rowBytes());
	for ( const BrRect &r : dmg ) {
		if ( !gpu ) {
			c->save();
			c->clipRect(SkRect::MakeXYWH((float)r.x, (float)r.y, (float)r.w, (float)r.h));
			if ( g_br.eng != nullptr ) {
				g_br.eng->paint(c, g_br.scroll, view_h());
			} else {
				c->clear(SK_ColorWHITE);
			}
			c->restore();
		}
		g_stat.painted += area(r);
	}
	t1 = now_us();
	/*
	 * With renderers, the whole view to compare with is the page's memory,
	 * which the browser may read only while it holds a frame: a view that
	 * took no frame (nothing changed) is not compared
	 */
#ifdef BR_BLINK
	if ( !g_check.empty() && !all && ( !br_multi_process || !dmg.empty() ) ) check_view(dmg);
#else
	if ( !g_check.empty() && !all ) check_view(dmg);
#endif
	if ( gpu && !g_gpucheck.empty() && !ime_list_on(g_br.eng) && !br_multi_process ) gpu_check_view();

	/* what is happening, on the message line (the link under the pointer before it) */
	say_line();
	show_bars();

	if ( ime_list_on(g_br.eng) ) ime_draw_list(c, g_br.eng, g_br.h);

	g_br.ime_shown = ime_list_on(g_br.eng);
	if ( dmg.empty() ) {
		/* nothing changed */
		g_stat.quiet++;
		g_stat.quiet_us += t1 - t0;
		return;
	}

	if ( g_br.shared ) {
		/* the rectangles drawn in the surface the window has, put in it */
		T_DPRECT	pr[WM_PUT_MAX];
		int		n = 0;
		long		t2 = now_us();

		for ( const BrRect &r : dmg ) {
			if ( n == WM_PUT_MAX ) break;
			pr[n].left = r.x;
			pr[n].top = r.y;
			pr[n].right = r.x + r.w;
			pr[n].bottom = r.y + r.h;
			g_stat.put_px += area(r);
			n++;
		}
		if ( wm_obj_put(g_br.kw, pr, n) < E_OK ) g_br.full = true;
		g_stat.put_us += now_us() - t2;
		g_stat.rects += n;
	} else {
		SkPixmap pm;
		long t2 = now_us(), t3, t4;
		if ( !g_br.surf->peekPixels(&pm) ) return;
		for ( int y = 0; y < g_br.h; y++ ) {
			const uint32_t *row = pm.addr32(0, y);
			UW *out = &g_br.px[(size_t)y * g_br.w];
			for ( int x = 0; x < g_br.w; x++ ) out[x] = row[x] & 0x00FFFFFF;
		}
		t3 = now_us();
		(void)dp_put_argb(g_br.gid, 0, 0, g_br.px.data(), g_br.w, g_br.w, g_br.h, 0xFF000000);
		t4 = now_us();
		(void)wm_obj_flush(g_br.kw, nullptr);
		g_stat.copy_us += t3 - t2;
		g_stat.put_us += t4 - t3;
		g_stat.flush_us += now_us() - t4;
		g_stat.put_px += (long)g_br.w * g_br.h;
		g_stat.rects++;
	}
	g_stat.paint_us += t1 - t0;
	g_stat.frames++;
	long px = 0;
	for ( const BrRect &r : dmg ) px += area(r);
	pace_view(t1 - t0, px);
}

void set_title_of( ID kw, const std::string &t );

void set_title( const std::string &t )
{
	set_title_of(g_br.kw, t);
}

/* A window's band given a name */
void set_title_of( ID kw, const std::string &t )
{
	/* no more than the band keeps, cut where a letter of UTF-8 begins */
	size_t n = t.size() < (size_t)( WM_TITLE_MAX - 8 ) ? t.size() : (size_t)( WM_TITLE_MAX - 8 );
	while ( n > 0 && n < t.size() && ( (unsigned char)t[n] & 0xC0 ) == 0x80 ) n--;
	std::string s = t.substr(0, n), j = "{\"name\":\"";
	for ( char ch : s ) {
		if ( ch == '"' || ch == '\\' ) j += '\\';
		if ( (unsigned char)ch >= 0x20 ) j += ch;
	}
	j += "\"}";
	(void)ob_set_atr(kw, (const UB *)j.data(), (SZ)j.size());
}

/*
 * The window's band names the page shown: its title, or its address
 * while it has none. Asked again after the page's own work, as a script
 * may change the title at any time.
 */
#ifdef BR_BLINK
void history_follow();
#endif

void show_title()
{
#ifdef BR_BLINK
	history_follow();
#endif
	std::string t = g_br.title.empty() ? g_br.url : g_br.title;
#if BR_BLINK
	/* the application's badge (navigator.setAppBadge) before the title */
	std::string badge = br_multi_process ? br_proc_badge(g_br.eng) : br_badge_text();
	if ( !badge.empty() ) t = "(" + badge + ") " + t;
	/* and, while a page shares the screen (getDisplayMedia), that it is shared */
	std::string capture = br_multi_process ? br_proc_capture(g_br.eng) : br_capture_text();
	if ( !capture.empty() ) t = "[" + capture + "] " + t;
#endif
	if ( t == g_br.shown_title ) return;
	g_br.shown_title = t;
	set_title(t.empty() ? std::string("ブラウザ") : t);
}

void progress( void *ctx, long got, long total )
{
	static long last;
	long t = now_ms();
	char buf[96];

	(void)ctx;
	if ( t - last < 300 ) return;
	last = t;
	if ( total > 0 ) {
		snprintf(buf, sizeof(buf), "読み込み中… %ld / %ld KB", got / 1024, total / 1024);
	} else {
		snprintf(buf, sizeof(buf), "読み込み中… %ld KB", got / 1024);
	}
	g_br.status = buf;
	show();
}

/* The work area's size as the window has it now; true when it changed */
bool read_size()
{
	T_OBWPOS	pos;
	SZ		asz = 0;

	memset(&pos, 0, sizeof(pos));
	if ( ob_rea_rec(g_br.kw, OB_WR_PLACE, 0, &pos, sizeof(pos), &asz) < E_OK ) return false;
	if ( !g_copy ) {
		/*
		 * The surface of the work area the system maps in: the same one
		 * while the size is the same, a new one after it changed
		 */
		T_WMSURF s;
		if ( wm_obj_surf(g_br.kw, &s) >= E_OK ) {
			if ( g_br.shared && s.serial == g_br.shm.serial ) return false;
			g_br.shm = s;
			g_br.shared = true;
			g_br.full = true;
			g_br.w = s.width;
			g_br.h = s.height;
			g_br.surf = SkSurfaces::WrapPixels(SkImageInfo::MakeN32Premul(s.width, s.height), s.addr,
							   (size_t)s.pitch);
			g_br.px.clear();
			return true;
		}
		g_br.shared = false;
	}
	int w = pos.wright - pos.wleft, h = pos.wbottom - pos.wtop;
	if ( w < 120 ) w = 120;
	if ( h < 40 ) h = 40;
	if ( w == g_br.w && h == g_br.h && g_br.px.size() == (size_t)w * h ) return false;
	g_br.w = w;
	g_br.h = h;
	g_br.surf = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(w, h));
	g_br.px.assign((size_t)w * h, 0);
	g_br.full = true;
	return true;
}

void relayout()
{
	if ( g_br.eng != nullptr ) {
#ifdef BR_BLINK
		g_br.doc_h = g_br.eng->layout(g_br.w);
#else
		g_br.doc_h = g_br.eng->layout(g_br.w - 8);
#endif
		/* an engine that scrolls the page says where the view is */
		int top = g_br.eng->scroll_top();
		if ( top >= 0 ) g_br.scroll = top;
		g_br.doc_w = g_br.eng->doc_width();
		g_br.left = g_br.eng->scroll_left();
	}
	clamp_scroll();
}

void go( const std::string &to, int how, const std::string &method = std::string(),
	 const std::string &ctype = std::string(), const std::string &body = std::string() );
void go_back();
void go_forward();
std::string omnibox( const std::string &typed );
void fullscreen_set( bool on, bool by_page );

/* ---------------------------------------------------------------- the tool panel */

/*
 * A small window of its own above the page's window, subordinate to it
 * (the system keeps it just in front of that window): framed, with no
 * band. The switches 戻る ←, 進む →, 再読込 再 and 停止 × are parts of a
 * panel placed on it; the address field to their right is drawn here,
 * takes the focus when pressed, is edited with the keys (Ctrl+A takes
 * the whole address, and what is typed next replaces it), and Enter goes
 * to what it holds. After each page it shows the page's address.
 */
const int kToolW = 560, kToolH = 48;		/* the window, outside */
const int kToolGap = 8;				/* between it and the page's window */
enum { T_BACK, T_FWD, T_RELOAD, T_STOP, T_NPART };
const char *const kToolLabel[T_NPART] = { "←", "→", "再", "×" };

/* What the notices on the port come from */
enum { N_PAGE = 0, N_TOOL = 2, N_PARTS = 3, N_GSURF = 4, N_SUB = 16 };	/* N_SUB + a window's number: the windows pages opened */
/* Added to a window's notice number: its panel's window, its panel's switches */
const UINT kNoteSubTool = 0x10000, kNoteSubParts = 0x20000, kNoteSubMask = 0xFFFF;

struct SubWin;

struct Tool {
	TS_UUID		win, pnl, part[T_NPART];
	ID		kw = 0, pk = 0, key[T_NPART] = {};
	INT		gid = -1;
	int		w = 0, h = 0;		/* its work area */
	SkRect		field;			/* the address field, in the work area */
	std::string	text = "http://";
	bool		focus = false;
	bool		sel_all = false;	/* Ctrl+A: the next key acts on the whole address */
	size_t		caret = 0;		/* where in the text (bytes) a letter typed goes */
	size_t		done_from = 0;		/* where in the text the completion starts; 0 none */
	float		text_x = 6;		/* where in the field the text starts, as drawn last */
	bool		off[T_NPART] = {};	/* the switches as last shown: not to be worked now */
	bool		off_set[T_NPART] = {};
	SubWin		*sub = nullptr;		/* the window a page opened that it is for; none: the browser's own */
};

Tool	g_tool;

/* What the field shows when it is not being edited, and where Enter goes: the panel's own window's */
std::string tool_address( Tool &tl );
void tool_go( Tool &tl, const std::string &to );

/* The address field drawn: white, a thin edge, the address, the caret when it has the focus */
void tool_redraw( Tool &tl )
{
	if ( tl.kw <= 0 || tl.gid < 0 ) return;
	int w = (int)tl.field.width(), h = (int)tl.field.height();
	if ( w < 8 || h < 8 ) return;
	sk_sp<SkSurface> s = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(w, h));
	if ( s == nullptr ) return;
	SkCanvas *c = s->getCanvas();
	c->clear(SK_ColorWHITE);
	SkPaint edge;
	edge.setStyle(SkPaint::kStroke_Style);
	edge.setColor(0xFF666666);
	c->drawRect(SkRect::MakeLTRB(0.5f, 0.5f, w - 0.5f, h - 0.5f), edge);
	SkFont f = ui_font(14);
	SkPaint ink;
	ink.setColor(SK_ColorBLACK);
	ink.setAntiAlias(true);
	const std::string &t = tl.text;
	float adv = t.empty() ? 0 : f.measureText(t.data(), t.size(), SkTextEncoding::kUTF8);
	size_t at = tl.caret < t.size() ? tl.caret : t.size();
	float cx = at == 0 ? 0 : f.measureText(t.data(), at, SkTextEncoding::kUTF8);
	float tx = 6, ty = (float)h / 2 + 5;
	/* the caret of a long address in view: its end, until the caret goes further left */
	if ( tx + cx > w - 8 ) tx = w - 8 - cx;
	tl.text_x = tx;
	if ( tl.focus && tl.sel_all && !t.empty() ) {
		SkPaint hl;
		hl.setColor(0xFF99CCFF);
		c->drawRect(SkRect::MakeLTRB(tx - 2, 3, tx + adv + 2 > w - 2 ? w - 2 : tx + adv + 2, (float)h - 3), hl);
	}
	if ( tl.done_from > 0 && tl.done_from < t.size() ) {
		SkPaint hl;
		hl.setColor(0xFF99CCFF);
		c->drawRect(SkRect::MakeLTRB(tx + f.measureText(t.data(), tl.done_from, SkTextEncoding::kUTF8), 3, tx + adv, (float)h - 3), hl);
	}
	if ( !t.empty() ) c->drawSimpleText(t.data(), t.size(), SkTextEncoding::kUTF8, tx, ty, f, ink);
	if ( tl.focus ) {
		SkPaint cp;
		cp.setColor(0xFF2255CC);
		c->drawRect(SkRect::MakeXYWH(tx + cx + 1, 4, 1.5f, (float)h - 8), cp);
	}
	SkPixmap pm;
	if ( !s->peekPixels(&pm) ) return;
	std::vector<UW> px((size_t)w * h);
	for ( int y = 0; y < h; y++ ) {
		const uint32_t *row = pm.addr32(0, y);
		for ( int x = 0; x < w; x++ ) px[(size_t)y * w + x] = row[x] & 0x00FFFFFF;
	}
	(void)dp_put_argb(tl.gid, (INT)tl.field.left(), (INT)tl.field.top(), px.data(), w, w, h,
			  0xFF000000);
	(void)wm_obj_flush(tl.kw, nullptr);
}

/* The addresses the field has shown, in turn, for a test ("addr=") */
std::vector<std::string>	g_addrs;

/* The letters typed in the field and the place among what they may complete to; the completion shown after them */
std::string	g_typed;
int		g_typed_skip;
#ifdef BR_BLINK
void tool_complete( Tool &tl )
{
	std::string to = br_history_complete(g_typed, g_typed_skip);
	if ( to.empty() ) return;
	/* what was typed stays as typed, the rest taken like a choice the next letter replaces */
	size_t at = to.find(g_typed);
	if ( at == std::string::npos ) return;
	tl.text = to;
	tl.caret = to.size();
	tl.done_from = at + g_typed.size();
}
#endif

/* The address shown after a page ("http://" when there is none to show) */
void tool_set_url( Tool &tl, const std::string &url )
{
	if ( g_addrs.empty() || g_addrs.back() != url ) g_addrs.push_back(url);
	/* a window a page opened says about:blank as it is: what its opener wrote there is not another site's */
	tl.text = ( url.empty() || ( tl.sub == nullptr && url.compare(0, 6, "about:") == 0 ) ) ? std::string("http://") : url;
	tl.caret = tl.text.size();
	tl.done_from = 0;
	g_typed.clear();
	tl.sel_all = false;
	tool_redraw(tl);
}

/* The field given the focus (Ctrl+L, the menu's アドレス入力), the whole address taken */
void tool_focus( Tool &tl )
{
	tl.focus = true;
	tl.caret = tl.text.size();
	tl.sel_all = !tl.text.empty();
	tool_redraw(tl);
}

/*
 * The panel of a window: 'kw' and 'win' that window's; the notices of the panel's
 * window and of its switches come to the page's port as 'note_tool' and 'note_parts'
 */
bool tool_open( Tool &tl, ID kw, const TS_UUID &win, UINT note_tool, UINT note_parts )
{
	T_OBWPOS	wp;
	T_OBCRE		c;
	T_OBNTF		req;
	SZ		asz = 0;
	char		json[256];

	if ( ob_rea_rec(kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) < E_OK ) return false;
	/* a small window's panel no wider than the window, and wide enough to read an address */
	int tw = tl.sub == nullptr ? kToolW : std::max(320, std::min(kToolW, wp.right - wp.left));
	int top = wp.top - ( kToolH + kToolGap );
	if ( top < 30 ) top = 30;
	snprintf(json, sizeof(json), "{\"rect\":[%d,%d,%d,%d],\"attr\":%d,\"sub\":\"%s\"}", wp.left, top,
		 wp.left + tw, top + kToolH, WM_ATTR_FRAME, br_uuid_str(win.b).c_str());
	memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (CONST UB *)"";
	c.json = (CONST UB *)json;
	c.jsonsz = (SZ)strlen(json);
	if ( ob_cre_obj(&c, &tl.win) < E_OK ) return false;
	tl.kw = ob_opn_obj(&tl.win, OB_OP_R | OB_OP_W | OB_OP_PROT);
	tl.gid = ( tl.kw > 0 ) ? wm_obj_gid(tl.kw) : E_ID;
	if ( tl.kw <= 0 || tl.gid < 0
	     || ob_rea_rec(tl.kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) < E_OK ) {
		return false;
	}
	tl.w = wp.wright - wp.wleft;
	tl.h = wp.wbottom - wp.wtop;
	int pt = 3, pb = tl.h - 3;

	/* the switches, on a panel placed at the left of the work area */
	snprintf(json, sizeof(json), "{\"tessronos\":{\"part\":{\"sub\":\"panel\",\"kind\":%d,\"rect\":[0,0,%d,%d]}}}",
		 WM_PNL_BARE, 6 + 32 * T_NPART, tl.h);
	memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_PANEL;
	c.json = (CONST UB *)json;
	c.jsonsz = (SZ)strlen(json);
	if ( ob_cre_obj(&c, &tl.pnl) < E_OK ) return false;
	tl.pk = ob_opn_obj(&tl.pnl, OB_OP_R | OB_OP_W | OB_OP_PROT);
	T_OBMAP m;
	memset(&m, 0, sizeof(m));
	if ( tl.pk <= 0 || ob_map_rec(tl.pk, 0, tl.kw, &m) < E_OK ) return false;
	for ( int i = 0; i < T_NPART; i++ ) {
		snprintf(json, sizeof(json), "{\"tessronos\":{\"part\":{\"kind\":%d,\"rect\":[0,0,28,%d]}}}", MS_PARTS,
			 pb - pt);
		memset(&c, 0, sizeof(c));
		c.type = OB_T_WINDOW;
		c.sub = OB_S_PART;
		c.name = (CONST UB *)kToolLabel[i];
		c.json = (CONST UB *)json;
		c.jsonsz = (SZ)strlen(json);
		if ( ob_cre_obj(&c, &tl.part[i]) < E_OK ) return false;
		tl.key[i] = ob_opn_obj(&tl.part[i], OB_OP_R | OB_OP_W | OB_OP_PROT);
		memset(&m, 0, sizeof(m));
		m.x = 6 + 32 * i;
		m.y = pt;
		if ( tl.key[i] <= 0 || ob_map_rec(tl.key[i], 0, tl.pk, &m) < E_OK ) return false;
	}
	tl.field = SkRect::MakeLTRB((float)( 6 + 32 * T_NPART + 4 ), (float)pt, (float)( tl.w - 4 ),
					(float)pb);

	/* its presses, keys and redrawing, and the switches', come to the page's port */
	memset(&req, 0, sizeof(req));
	req.events = OB_E_PRESS | OB_E_KEY | OB_E_REDRAW | OB_E_CLOSE;
	req.id = note_tool;
	if ( ob_ntf_evt(tl.kw, OB_REC_ANY, &req, g_br.port) <= 0 ) return false;
	memset(&req, 0, sizeof(req));
	req.events = OB_E_CHANGE;
	req.id = note_parts;
	if ( ob_ntf_evt(tl.pk, OB_REC_ANY, &req, g_br.port) <= 0 ) return false;
	tool_redraw(tl);
	printf("browser: tool panel %dx%d\n", tl.w, tl.h);
	return true;
}

void tool_close( Tool &tl )
{
	for ( int i = 0; i < T_NPART; i++ ) {
		if ( tl.key[i] > 0 ) {
			ob_cls_obj(tl.key[i]);
			(void)ob_del_obj(&tl.part[i]);
		}
	}
	if ( tl.pk > 0 ) {
		ob_cls_obj(tl.pk);
		(void)ob_del_obj(&tl.pnl);
	}
	if ( tl.kw > 0 ) {
		ob_cls_obj(tl.kw);
		(void)ob_del_obj(&tl.win);
	}
	tl.kw = tl.pk = 0;
}

/* A press on the tool window outside the switches: the field takes the focus, or gives it up */
void tool_press( Tool &tl, int x, int y )
{
	bool in = tl.field.contains((float)x, (float)y);
	if ( in ) {
		/* the caret before the letter pressed on */
		const std::string &t = tl.text;
		SkFont f = ui_font(14);
		float want = (float)x - tl.field.left() - tl.text_x;
		size_t at = 0;
		while ( at < t.size() ) {
			size_t n = at + 1;
			while ( n < t.size() && ( (unsigned char)t[n] & 0xC0 ) == 0x80 ) n++;
			if ( f.measureText(t.data(), n, SkTextEncoding::kUTF8) > want ) break;
			at = n;
		}
		tl.caret = at;
	}
	if ( in || in != tl.focus ) {
		tl.focus = in;
		tl.sel_all = false;
		tool_redraw(tl);
	}
}

/* A key while the field has the focus */
void tool_key( Tool &tl, UINT code, UINT mods )
{
	bool ctrl = ( mods & kCtrl ) != 0;
	std::string &t = tl.text;

	if ( ctrl && code == K_A ) {
		tl.sel_all = !t.empty();
		tool_redraw(tl);
		return;
	}
	if ( code == K_ENTER || code == K_KPENTER ) {
		std::string to = omnibox(t);
		tl.done_from = 0;
		tl.focus = false;
		tl.sel_all = false;
		tool_redraw(tl);
		if ( !to.empty() ) tool_go(tl, to);
		return;
	}
	if ( code == K_ESC ) {
		tl.focus = false;
		tl.sel_all = false;
		tool_set_url(tl, tool_address(tl));
		return;
	}
	size_t &at = tl.caret;
	if ( at > t.size() ) at = t.size();
#ifdef BR_BLINK
	if ( ( code == K_DOWN || code == K_UP ) && !g_typed.empty() ) {
		g_typed_skip += ( code == K_DOWN ) ? 1 : -1;
		if ( g_typed_skip < 0 || br_history_complete(g_typed, g_typed_skip).empty() ) {
			g_typed_skip = ( code == K_DOWN ) ? g_typed_skip - 1 : 0;
		}
		tool_complete(tl);
		tool_redraw(tl);
		return;
	}
#endif
	if ( tl.done_from > 0 ) {
		/* Right and End accept what is shown; a letter, Backspace and Delete take it away first */
		if ( code == K_RIGHT || code == K_END ) {
			tl.done_from = 0;
			at = t.size();
			tool_redraw(tl);
			return;
		}
		if ( code != K_ENTER && code != K_KPENTER ) {
			t.resize(tl.done_from);
			at = t.size();
			tl.done_from = 0;
		}
	}
	/* one letter back and on, over the continuation bytes of UTF-8 */
	auto before = [&]( size_t n ) {
		if ( n > 0 ) n--;
		while ( n > 0 && ( (unsigned char)t[n] & 0xC0 ) == 0x80 ) n--;
		return n;
	};
	auto after = [&]( size_t n ) {
		if ( n < t.size() ) n++;
		while ( n < t.size() && ( (unsigned char)t[n] & 0xC0 ) == 0x80 ) n++;
		return n;
	};
	if ( code == K_LEFT || code == K_RIGHT || code == K_HOME || code == K_END ) {
		/* the whole address taken: the first press leaves the caret at that end of it */
		if ( code == K_HOME || ( code == K_LEFT && tl.sel_all ) ) at = 0;
		else if ( code == K_END || ( code == K_RIGHT && tl.sel_all ) ) at = t.size();
		else at = ( code == K_LEFT ) ? before(at) : after(at);
		tl.sel_all = false;
		tool_redraw(tl);
		return;
	}
	if ( code == K_BS || code == K_DEL ) {
		if ( tl.sel_all ) {
			t.clear();
			at = 0;
		} else if ( code == K_BS ) {
			size_t n = before(at);
			t.erase(n, at - n);
			at = n;
		} else {
			t.erase(at, after(at) - at);
		}
		tl.sel_all = false;
		tool_redraw(tl);
		return;
	}
	UB ch = wm_key_char(code, mods);
	if ( ch < 0x20 || ch >= 0x7F || ctrl ) return;
	if ( tl.sel_all ) {
		t.clear();				/* typing replaces the whole address */
		at = 0;
	}
	tl.sel_all = false;
	t.insert(at++, 1, (char)ch);
#ifdef BR_BLINK
	/* typed at the end: what it may be going to, from the pages visited */
	if ( at == t.size() ) {
		g_typed = t;
		g_typed_skip = 0;
		tool_complete(tl);
	} else {
		g_typed.clear();
	}
#endif
	tool_redraw(tl);
}

/*
 * A switch shown as one that cannot be worked now, or as one that can: 戻る
 * and 進む when there is no entry that way, 停止 while nothing loads
 */
void tool_off( Tool &tl, int i, bool off )
{
	T_OBWPOS	wp;
	SZ		asz = 0;

	if ( i < 0 || i >= T_NPART || tl.key[i] <= 0 ) return;
	if ( tl.off_set[i] && tl.off[i] == off ) return;
	if ( ob_rea_rec(tl.key[i], OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) < E_OK ) return;
	wp.flags = off ? ( wp.flags | OB_WP_OFF ) : ( wp.flags & ~(UINT)OB_WP_OFF );
	if ( ob_wri_rec(tl.key[i], OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		tl.off[i] = off;
		tl.off_set[i] = true;
	}
}

bool can_back();
bool can_forward();

void tool_states( bool loading )
{
	tool_off(g_tool, T_BACK, !can_back());
	tool_off(g_tool, T_FWD, !can_forward());
	tool_off(g_tool, T_STOP, !loading);
}

/* A switch of the panel pushed, known by its link record */
void tool_switch( INT recno )
{
	switch ( recno - OB_WR_KIND ) {
	case T_BACK:	go_back(); break;
	case T_FWD:	go_forward(); break;
	case T_RELOAD:	go(g_br.url, BR_LOAD_RELOAD); break;
	case T_STOP:	break;		/* nothing loads now: a load looks for it itself */
	default:	break;
	}
}

/* The browser's own panel */
void tool_redraw() { tool_redraw(g_tool); }
void tool_set_url( const std::string &url ) { tool_set_url(g_tool, url); }
void tool_focus() { tool_focus(g_tool); }
void tool_close() { tool_close(g_tool); }
void tool_press( int x, int y ) { tool_press(g_tool, x, y); }
void tool_key( UINT code, UINT mods ) { tool_key(g_tool, code, mods); }
bool tool_open() { return tool_open(g_tool, g_br.kw, g_br.win, N_TOOL, N_PARTS); }

/* ---------------------------------------------------------------- the window's thread */

/*
 * The window's notices are taken by a thread of their own, not by the
 * engine's thread, which may be held for long by a page: its parts
 * still coming, a script running on, a fetch waited for. What must be
 * answered at once is answered there whatever the engine does: the
 * menu is shown (what is chosen in it is done on the engine's thread,
 * but 閉じる at once), 閉じる, the window deleted and Ctrl+E close the
 * window, and 中止 and Esc while a page loads stop it. Everything else
 * is queued for the engine's thread, which takes it in turn.
 *
 * Closing deletes the windows at once, after the window's place is
 * written into the page object; the engine's thread is asked to end
 * (the script running ended, the fetch waited for given up), and when
 * it has not ended within kQuitMs the process ends anyway.
 *
 * The engine's thread says where it is each time round (beat); when it
 * has not come round for kStuckMs, what it was doing is printed, with
 * where the page's script is when it runs one.
 */
const long		kQuitMs = 3000;
const long		kStuckMs = 5000;

bool parse_uuid( const char *s, TS_UUID *u );

std::mutex		g_ev_lock;
std::deque<T_OBNTM>	g_events;		/* notices for the engine's thread */
std::deque<std::string>	g_commands;		/* what was chosen in the menu, for it too */
std::atomic<bool>	g_closing{false};	/* the window is going: the engine's thread ends */
std::atomic<bool>	g_stop{false};		/* 中止: the load going on is to stop */
std::atomic<long>	g_stop_at{0};		/* when 中止 was asked (request_stop), 0 when it was not */
std::atomic<bool>	g_loading{false};	/* a page is being fetched or loaded */
std::atomic<bool>	g_main_ended{false};
std::atomic<int>	g_apart_starting{0};	/* browsers of their own being started (open_apart) */
std::atomic<long>	g_beat{0};		/* when the engine's thread last came round */
std::atomic<const char *> g_doing{"starting"};	/* and what it was doing */

void beat( const char *doing )
{
	long now = now_ms();

	if ( br_turn_ended != nullptr ) br_turn_ended(g_doing.load(), now - g_beat.load());
	g_beat = now;
	g_doing = doing;
}

/* The next notice for the engine's thread, or false */
bool next_event( T_OBNTM *m )
{
	std::lock_guard<std::mutex> g(g_ev_lock);
	if ( g_events.empty() ) return false;
	*m = g_events.front();
	g_events.pop_front();
	return true;
}

bool next_command( std::string *code )
{
	std::lock_guard<std::mutex> g(g_ev_lock);
	if ( g_commands.empty() ) return false;
	*code = g_commands.front();
	g_commands.pop_front();
	return true;
}

/* While a page loads, asked by it each time round: whether it is to stop */
bool tool_stop_pushed()
{
	beat("the page's parts coming");
	return g_stop || g_closing;
}

/* The engine's thread not seen for this long, in ms */
long main_quiet_ms()
{
	return now_ms() - g_beat.load();
}

/*
 * For how long the page has not come round: the engine's thread, and with
 * renderers also the main thread of the page's renderer (a script that
 * holds it, a call it carries out), in ms
 */
long page_held_ms()
{
	long q = main_quiet_ms();
#ifdef BR_BLINK
	if ( br_multi_process && g_br.eng != nullptr ) q = std::max(q, br_proc_busy_ms(g_br.eng));
#endif
	return q;
}

/* 中止: the load stops; a script that holds the page is ended */
void request_stop( const char *why )
{
	long held = page_held_ms();
	if ( !g_loading && held < 1000 ) return;
	printf("browser: stopped (%s)\n", why);
	if ( !g_stop ) g_stop_at = now_ms();
	g_stop = true;
#ifdef BR_BLINK
	if ( br_multi_process ) {
		br_proc_stop_load(g_br.eng, true);
	} else {
		br_net_fetch_cancel_sync(1);
	}
#endif
	if ( held >= 1000 && g_br.eng != nullptr ) g_br.eng->stop_script();
}

#ifdef BR_BLINK
}  // namespace

int br_nav_wait_verdict()
{
	if ( g_closing ) return BR_WAIT_CLOSING;
	if ( !g_stop ) return BR_WAIT_ON;
	long at = g_stop_at.load();
	return ( at != 0 && now_ms() - at >= kQuitMs ) ? BR_WAIT_HELD : BR_WAIT_STOPPING;
}

namespace {
#endif

bool fullscreen_was( T_OBWPOS *wp );

/* The window's place, for the next time the object is opened (where it was before full screen) */
void save_place()
{
	T_OBWPOS	wp;
	SZ		asz = 0;
	BrObject	&o = g_br.obj;

	if ( !o.ok || g_br.kw <= 0 ) return;
	if ( fullscreen_was(&wp)
	     || ( ob_rea_rec(g_br.kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK && asz >= (SZ)sizeof(wp) ) ) {
		/* from the corner of the screen its top left is on, and which screen (wm_place_keep) */
		T_DPRECT	view;
		T_WMPLACE	p;
		char		scr[12] = "";

		view.left = wp.left;
		view.top = wp.top;
		view.right = wp.right;
		view.bottom = wp.bottom;
		if ( wm_place_keep(&view, &p) < E_OK ) {
			p.r = view;
			p.screen = 0;
		}
		if ( p.screen != 0 ) snprintf(scr, sizeof(scr), "%08x", (unsigned)p.screen);
		o.x = p.r.left;
		o.y = p.r.top;
		o.screen = scr;
		o.kept = true;
		o.w = wp.wright - wp.wleft;
		o.h = wp.wbottom - wp.wtop;
		(void)br_obj_save_window(o);
	}
}

void subs_delete_windows();

/* The window goes at once, whatever the engine is doing */
void close_now( const char *why )
{
	if ( g_closing.exchange(true) ) return;
	printf("browser: closing (%s)\n", why);
	g_stop = true;
#ifdef BR_BLINK
	if ( br_multi_process ) {
		br_proc_stop_load(g_br.eng, true);
	} else {
		br_net_fetch_cancel_sync(1);
	}
#endif
	if ( page_held_ms() >= 500 && g_br.eng != nullptr ) g_br.eng->stop_script();
	save_place();
	(void)wm_msg_put("");
#ifdef BR_BLINK
	br_settings_close();
#endif
	if ( g_tool.kw > 0 ) (void)ob_del_obj(&g_tool.win);
	subs_delete_windows();
	(void)ob_del_obj(&g_br.win);
}

bool can_back();
bool can_forward();

/*
 * The window's menu: the code of what was chosen, "" for nothing. Shown by
 * the engine's thread after a press on the page ('ctx' what is under the
 * pointer: its items shown, 戻る and 進む greyed when there is nowhere to
 * go), else by the window's thread (those items hidden).
 */
std::string origin_of( const std::string &url );

#ifdef BR_BLINK
void wv_state( bool *busy, bool *installed );
#endif

std::string menu_pop( const T_OBNTM &m, const BrEngine::Context *ctx = nullptr )
{
	TS_UUID		def;
	T_MNSEL		sel;
	ID		mid;
	std::string	code;

	if ( !parse_uuid(SYSDEF_MENU_BROWSER, &def) || mn_cre_men(&def, &mid) < E_OK ) return code;
	bool link = ctx != nullptr && !ctx->link.empty(), image = ctx != nullptr && !ctx->image.empty();
	bool chosen = ctx != nullptr && !ctx->selection.empty();
	(void)mn_chg_atr(mid, nullptr, "openlink", link ? 0 : MN_HIDE);
	(void)mn_chg_atr(mid, nullptr, "copylink", link ? 0 : MN_HIDE);
	(void)mn_chg_atr(mid, nullptr, "saveimage", image ? 0 : MN_HIDE);
	(void)mn_chg_atr(mid, nullptr, "copyimage", image ? 0 : MN_HIDE);
	(void)mn_chg_atr(mid, nullptr, "copysel", chosen ? 0 : MN_HIDE);
	{
		std::string o = origin_of(g_br.url);
#ifdef BR_BLINK
		bool iso = br_multi_process ? br_proc_isolated(g_br.eng) : br_isolated_allowed(o);
#else
		bool iso = br_isolated_allowed(o);
#endif
		(void)mn_chg_atr(mid, nullptr, "isolated", o.empty() ? MN_GREY : iso ? MN_TICK : 0);
	}
#ifdef BR_BLINK
	{
		bool busy = false, installed = false;
		wv_state(&busy, &installed);
		(void)mn_chg_atr(mid, nullptr, "wvinstall", busy ? MN_GREY : 0);
		(void)mn_chg_atr(mid, nullptr, "wvremove", installed && !busy ? 0 : MN_GREY);
	}
#else
	(void)mn_chg_atr(mid, nullptr, "wvinstall", MN_HIDE);
	(void)mn_chg_atr(mid, nullptr, "wvremove", MN_HIDE);
	(void)mn_chg_atr(mid, nullptr, "settings", MN_HIDE);
#endif
	if ( ctx != nullptr ) {
		(void)mn_chg_atr(mid, nullptr, "back", can_back() ? 0 : MN_GREY);
		(void)mn_chg_atr(mid, nullptr, "forward", can_forward() ? 0 : MN_GREY);
		(void)mn_chg_atr(mid, nullptr, "paste", ctx->editable ? 0 : MN_GREY);
		(void)mn_chg_atr(mid, nullptr, "cut", ctx->editable && chosen ? 0 : MN_GREY);
	}
#ifdef BR_BLINK
	/* the buttons of the extensions, and what they added to the menu for what is under the pointer */
	auto rows_in = [&]( const char *list, const std::vector<std::string> &rows ) {
		std::string labels;
		for ( const std::string &r : rows ) {
			/* cut at a letter's start: the row is held to the label's size */
			size_t n = std::min(r.size(), (size_t)WM_LABEL_MAX - 1);
			while ( n > 0 && n < r.size() && ( (unsigned char)r[n] & 0xC0 ) == 0x80 ) n--;
			std::string l = r.substr(0, n);
			l.resize(WM_LABEL_MAX, '\0');
			labels += l;
		}
		(void)mn_set_lst(mid, nullptr, list, (const UB *)labels.data(), WM_LABEL_MAX, (INT)rows.size());
	};
	{
		BrExtMenuAt at;
		at.page = g_br.url;
		if ( ctx != nullptr ) {
			at.link = ctx->link;
			at.image = ctx->image;
			at.selection = ctx->selection;
			at.editable = ctx->editable;
		}
		if ( br_multi_process ) {
			rows_in("ext.action", br_proc_ext_actions(g_br.eng));
			rows_in("ext.ctx", ctx != nullptr ? br_proc_ext_items(g_br.eng) : std::vector<std::string>());
		} else {
			rows_in("ext.action", br_ext_action_rows());
			rows_in("ext.ctx", br_ext_menu_rows(at));
		}
	}
#endif
	if ( mn_pop_men(mid, g_br.kw, m.x, m.y, m.when, &sel) >= E_OK ) {
		code.assign((const char *)sel.code, strnlen((const char *)sel.code, sizeof(sel.code)));
		/* a row of a list the program fills: which one comes with its code */
		if ( code == "ext.action" || code == "ext.ctx" ) code += ":" + std::to_string((int)sel.index);
		printf("browser: menu: %s\n", code.c_str());
	}
	(void)mn_del_men(mid);
	return code;
}

/*
 * A right press on the page queued for the engine's thread (the page's
 * contextmenu first). The engine's thread may take long to come to it
 * (a page being fetched, a renderer starting): when it has not taken it
 * within BR_HELD_MS, the window's thread shows the menu itself, as for a
 * page held. Whichever thread takes it first shows the menu (under
 * g_ev_lock); the other passes it by.
 */
enum { kCtxNone, kCtxWaiting, kCtxTaken };
int		g_ctx_state = kCtxNone;
T_OBNTM		g_ctx_press;
long		g_ctx_since = 0;

/*
 * The window's menu for a right press the page left to it, with what is
 * under the pointer: handed by the engine's thread to the window's thread,
 * which shows it (window_menu) and passes on what was chosen, as for a
 * press it answers itself. The engine's thread goes on serving the pages
 * meanwhile. Under g_ev_lock.
 */
bool			g_ctx_menu = false;
T_OBNTM			g_ctx_menu_press;
BrEngine::Context	g_ctx_menu_at;

/* Whether the thread may answer the right press 'm': it is not the one queued, or it is and not yet taken */
bool ctx_take( const T_OBNTM &m )
{
	std::lock_guard<std::mutex> g(g_ev_lock);
	if ( g_ctx_state == kCtxNone || g_ctx_press.when != m.when || g_ctx_press.x != m.x || g_ctx_press.y != m.y ) {
		return true;
	}
	bool mine = g_ctx_state == kCtxWaiting;
	g_ctx_state = kCtxTaken;
	return mine;
}

/* The window's menu shown by the window's thread, and what is chosen in it passed on */
void window_menu( const T_OBNTM &m, const BrEngine::Context *ctx = nullptr )
{
	std::string code = menu_pop(m, ctx);
	if ( code == "close" ) {
		close_now("the menu's 閉じる");
	} else if ( !code.empty() ) {
		/* a page gone to instead: the one loading now stops */
		if ( code == "back" || code == "forward" || code == "reload" || code == "home" ) {
			request_stop("the menu");
		}
		std::lock_guard<std::mutex> g(g_ev_lock);
		g_commands.push_back(code);
	}
}

/* On the window's thread: a right press the engine's thread has not come to in time, answered here */
void ctx_overdue()
{
	T_OBNTM	m;
	{
		std::lock_guard<std::mutex> g(g_ev_lock);
		if ( g_ctx_state != kCtxWaiting || now_ms() - g_ctx_since < BR_HELD_MS ) return;
		m = g_ctx_press;
	}
	if ( !ctx_take(m) ) return;
	printf("browser: the engine's thread has not come to the right press in %ld ms: the menu is shown without it\n",
	       (long)BR_HELD_MS);
	window_menu(m);
}

/*
 * The browser's own panels (〈探す〉, Widevine's, the extensions'): put up
 * by the window's thread as its menus are, and what was answered passed on
 * to the engine's thread as a command (g_commands), so that the engine's
 * thread goes on serving the pages while a panel waits. Under g_ev_lock.
 */
std::deque<std::function<void()>>	g_panel_jobs;

/*
 * The panels handed over and not yet answered: counted up as one is
 * handed over, and down once its job has passed on what was answered
 */
std::atomic<int>			g_panels_open{0};

/*
 * A test's "later=" lines, carried out by the engine's loop one at a
 * time: each once the panels the ones before put up have been answered
 * and what was answered has been carried out (an extension's panel and
 * its "extadd:" before "settings", whose sheet is then read)
 */
std::deque<std::string>			g_later;

/* From the engine's thread: a panel to put up */
void panel_on_window( std::function<void()> job )
{
	std::lock_guard<std::mutex> g(g_ev_lock);
	g_panels_open++;
	g_panel_jobs.push_back(std::move(job));
}

/* From the window's thread: what a panel was answered, for the engine's thread */
void command_back( const std::string &code )
{
	{
		std::lock_guard<std::mutex> g(g_ev_lock);
		g_commands.push_back(code);
	}
	loop_wake();
}

/* On the window's thread: the panels handed over, in turn */
void panels_due()
{
	for ( ;; ) {
		std::function<void()> job;
		{
			std::lock_guard<std::mutex> g(g_ev_lock);
			if ( g_panel_jobs.empty() ) return;
			job = std::move(g_panel_jobs.front());
			g_panel_jobs.pop_front();
		}
		job();
		g_panels_open--;
		loop_wake();
	}
}

/* On the window's thread: the menu the engine's thread handed over, shown */
void ctx_menu_due()
{
	T_OBNTM			m;
	BrEngine::Context	at;
	{
		std::lock_guard<std::mutex> g(g_ev_lock);
		if ( !g_ctx_menu ) return;
		g_ctx_menu = false;
		m = g_ctx_menu_press;
		at = g_ctx_menu_at;
	}
	window_menu(m, &at);
}

/* A notice, on the window's thread: answered there, or queued for the engine's thread */
void take_event_one( const T_OBNTM &m )
{
	if ( m.event == OB_E_CLOSE || m.event == OB_E_DELETE ) {
		if ( m.id >= N_SUB ) {
			/* a window a page opened: it alone goes, on the engine's thread */
		} else if ( m.id == N_TOOL && m.event == OB_E_CLOSE ) {
			/* the panel alone goes: the engine's thread closes it */
		} else {
			close_now(m.event == OB_E_CLOSE ? "閉じる" : "the window deleted");
			return;
		}
	}
	if ( m.event == OB_E_KEY ) {
		if ( ( m.mods & kCtrl ) != 0 && m.code == K_E && m.id < N_SUB ) {
			close_now("Ctrl+E");
			return;
		}
		if ( m.code == K_ESC && g_loading ) request_stop("Esc");
	}
	if ( m.event == OB_E_PRESS && m.code == 1 && ( m.id == N_PAGE || m.id == N_TOOL ) ) {
#ifdef BR_BLINK
		/*
		 * On the page while it comes round: the page's contextmenu first,
		 * on the engine's thread. A page held (its script, a call in its
		 * renderer) would not answer: the menu is shown here, without the
		 * items for what is under the pointer
		 */
		if ( m.id == N_PAGE && !g_loading && page_held_ms() < BR_HELD_MS ) {
			std::lock_guard<std::mutex> g(g_ev_lock);
			if ( g_events.size() < 1024 ) {
				g_events.push_back(m);
				g_ctx_press = m;
				g_ctx_since = now_ms();
				g_ctx_state = kCtxWaiting;
			}
			return;
		}
#endif
		window_menu(m);
		return;
	}
	if ( m.id == N_PARTS && m.event == OB_E_CHANGE && m.recno - OB_WR_KIND == T_STOP ) {
		request_stop("中止");
		return;
	}
#ifdef BR_BLINK
	if ( m.id == N_GSURF ) {
		/*
		 * the window shows one of its GPU buffers: noted for the engine's
		 * thread, which gives the frame drawn into it back to the renderer
		 * wherever it waits (its loop, a test's wait, a navigation)
		 */
		if ( m.event == OB_E_CHANGE ) br_proc_gsurf_shown((int)m.code, (unsigned)m.x);
		return;
	}
#endif
	std::lock_guard<std::mutex> g(g_ev_lock);
	if ( g_events.size() < 1024 ) g_events.push_back(m);
}

/* A notice taken: the engine's thread, which may wait for long, looks at what came of it */
void take_event( const T_OBNTM &m )
{
	take_event_one(m);
	loop_wake();
}

ID	g_engine_tid = 0;		/* the engine's thread's task */

/* What the engine's thread is doing: its task's state, and the wait it is in with its callers */
void report_engine()
{
	T_RTSK	r;

	if ( g_engine_tid > 0 && tk_ref_tsk(g_engine_tid, &r) >= E_OK ) {
		printf("browser:   its task: state 0x%x, waiting for 0x%x (object %d), on processor %d\n",
		       (unsigned)r.tskstat, (unsigned)r.tskwait, (int)r.wid, (int)r.prcid);
	}
#ifdef BR_BLINK
	if ( br_multi_process ) {
		std::string c = br_proc_calling();
		if ( !c.empty() ) printf("browser:   it waits for %s\n", c.c_str());
	}
	long since = g_wait_since.load();
	if ( since == 0 ) {
		printf("browser:   it waits for no condition or lock of its own (it runs, or waits in the system)\n");
		return;
	}
	char line[12 * 20 + 1];
	int o = 0;
	for ( int i = 0; i < g_wait_nframes && i < 12; i++ ) {
		o += snprintf(line + o, sizeof(line) - (size_t)o, " %lx", (unsigned long)g_wait_frames[i]);
	}
	printf("browser:   it waits for %s since %ld ms, called from%s\n", g_wait_what, now_ms() - since, line);
#endif
}

/* How long the window's thread waits for a notice before it looks at the engine's thread again */
constexpr long	kWatchMs = 200;

void *window_thread( void * )
{
	long	said = 0;		/* the quiet of the engine's thread last said, in ms */
	long	closing_at = 0;

	for ( ;; ) {
		T_OBNTM	m;
		SZ	asz = 0;
		bool	any = false;

		while ( !g_closing && ob_rea_rec(g_br.port, 0, 0, &m, sizeof(m), &asz) >= E_OK ) {
			take_event(m);
			any = true;
		}
		if ( g_main_ended ) return nullptr;
		long quiet = main_quiet_ms();
		if ( quiet >= kStuckMs && quiet >= 2 * said ) {
			said = quiet;
			printf("browser: the engine's thread has not come round for %ld.%ld s: %s\n", quiet / 1000,
			       quiet % 1000 / 100, g_doing.load());
			report_engine();
			if ( g_br.eng != nullptr ) g_br.eng->report_script();
		} else if ( quiet < kStuckMs && said > 0 ) {
			printf("browser: the engine's thread is round again after %ld s\n", said / 1000);
			said = 0;
		}
		if ( g_closing ) {
			if ( closing_at == 0 ) closing_at = now_ms();
			/* a browser being started apart is let start first (the call into the system is not cut) */
			if ( now_ms() - closing_at >= kQuitMs && g_apart_starting == 0 ) {
				printf("browser: the engine's thread has not ended %ld ms after closing (%s); the process ends\n",
				       kQuitMs, g_doing.load());
				fflush(stdout);
#ifdef BR_BLINK
				if ( !br_multi_process ) br_net_cookies_flush(5000);
				br_console_drain(2000);
#endif
				ts_ext_prc(0);
			}
		}
#ifdef BR_BLINK
		ctx_overdue();
#endif
		ctx_menu_due();
		panels_due();
		if ( any ) continue;
		if ( g_closing ) {
			usleep(5000);
			continue;
		}
		/* nothing came: the next notice waited for, up to the time the engine's thread is watched at */
		if ( ob_rea_tmo(g_br.port, 0, 0, &m, sizeof(m), &asz, kWatchMs) >= E_OK ) take_event(m);
	}
}

bool window_thread_start()
{
	pthread_t	th;
	pthread_attr_t	at;

	pthread_attr_init(&at);
	pthread_attr_setstacksize(&at, 256 * 1024);
	pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	int rc = pthread_create(&th, &at, window_thread, nullptr);
	pthread_attr_destroy(&at);
	return rc == 0;
}

void follow_doc();

/* ---------------------------------------------------------------- what the address field is given */

bool	g_typed_upgrade = false;	/* the next go() is of an address typed with no scheme */
bool	g_finding = false;		/* the matches of a find in the page are marked (find_step) */

/* Whether a host is one HTTPS is not tried first for: an IP address, a name of one label, localhost */
bool plain_host( const std::string &host )
{
	if ( host.empty() || host == "localhost" || host.find('.') == std::string::npos ) return true;
	if ( host[0] == '[' ) return true;
	return host.find_first_not_of("0123456789.") == std::string::npos;
}

/*
 * What is typed into the address field, made into what to go to, as
 * Chrome's address bar makes it: an address with its scheme as it is;
 * a name with a dot (or localhost, or an IP address, with a port and a
 * path or not) as an address, over HTTPS first when it is a name of the
 * Internet (and over HTTP if that fails); anything else (words, a space)
 * a search of the default search engine
 */
std::string omnibox( const std::string &typed )
{
	std::string t = typed;
	t.erase(0, t.find_first_not_of(" \t"));
	while ( !t.empty() && ( t.back() == ' ' || t.back() == '\t' ) ) t.pop_back();
	if ( t.empty() ) return t;
	if ( t.find("://") != std::string::npos || t.compare(0, 6, "about:") == 0 || t.compare(0, 5, "data:") == 0 ) {
		return t;
	}
	bool space = t.find(' ') != std::string::npos;
	std::string hostport = t.substr(0, t.find_first_of("/?#"));
	std::string host = hostport.substr(0, hostport.rfind(':') != std::string::npos && hostport[0] != '['
						? hostport.rfind(':') : std::string::npos);
	bool port = host.size() != hostport.size();
	if ( !space && ( host.find('.') != std::string::npos || host == "localhost" || port ) && !host.empty()
	     && host.front() != '.' && host.back() != '.' ) {
		if ( plain_host(host) || port ) return "http://" + t;
		g_typed_upgrade = true;
		return "https://" + t;
	}
	static const char hex[] = "0123456789ABCDEF";
	std::string q;
	for ( unsigned char c : t ) {
		if ( isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' ) {
			q += (char)c;
		} else if ( c == ' ' ) {
			q += '+';
		} else {
			q += '%';
			q += hex[c >> 4];
			q += hex[c & 15];
		}
	}
	return "https://www.google.com/search?q=" + q;
}

/* Letters put in a page as text */
std::string html_text( const std::string &s )
{
	std::string o;
	for ( char c : s ) {
		if ( c == '<' ) o += "&lt;";
		else if ( c == '>' ) o += "&gt;";
		else if ( c == '&' ) o += "&amp;";
		else if ( c == '"' ) o += "&quot;";
		else o += c;
	}
	return o;
}

/*
 * What a page that could not be fetched shows: the page Chrome shows in
 * its place, in its words, with the network stack's name of the error
 * and a button that loads it again
 */
std::string error_page( const std::string &target, const BrResponse &r )
{
	BrUrl		u;
	std::string	host = ( br_url_parse(target, &u) && !u.host.empty() ) ? u.host : target;
	std::string	code = !r.code.empty() ? r.code : std::string("ERR_FAILED");
	std::string	head = "このサイトにアクセスできません", why;

	if ( code == "ERR_NAME_NOT_RESOLVED" ) {
		why = host + " のサーバーの IP アドレスが見つかりませんでした。";
	} else if ( code == "ERR_CONNECTION_REFUSED" ) {
		why = host + " で接続が拒否されました。";
	} else if ( code == "ERR_CONNECTION_TIMED_OUT" || code == "ERR_TIMED_OUT" ) {
		why = host + " からの応答時間が長すぎます。";
	} else if ( code == "ERR_CONNECTION_RESET" ) {
		why = "接続がリセットされました。";
	} else if ( code == "ERR_CONNECTION_CLOSED" || code == "ERR_EMPTY_RESPONSE" ) {
		head = "このページは動作していません";
		why = host + " からデータが送信されませんでした。";
	} else if ( code == "ERR_INVALID_RESPONSE" ) {
		head = "このページは動作していません";
		why = host + " から無効な応答が送信されました。";
	} else if ( code == "ERR_INTERNET_DISCONNECTED" || code == "ERR_ADDRESS_UNREACHABLE" ) {
		head = "インターネットに接続されていません";
		why = "ネットワークの接続を確かめてください。";
	} else if ( code.compare(0, 9, "ERR_CERT_") == 0 || code.compare(0, 8, "ERR_SSL_") == 0 ) {
		head = "この接続ではプライバシーが保護されません";
		why = host + " では、悪意のあるユーザーによって、パスワード、メッセージ、クレジット カードなどの情報が盗まれる可能性があります。";
	} else {
		why = host + " を読み込めませんでした。";
	}
	return "<!DOCTYPE html><html lang=\"ja\"><head><meta charset=\"utf-8\"><title>" + html_text(host)
	       + "</title><style>body{font-family:sans-serif;color:#202124;margin:72px 12% 0}"
		 "h1{font-size:1.6em;font-weight:normal;margin:0 0 16px}p{line-height:1.6}"
		 ".code{color:#5f6368;font-size:.85em;margin-top:16px}"
		 "button{margin-top:24px;padding:8px 16px;font-size:1em}</style></head><body><h1>"
	       + html_text(head) + "</h1><p>" + html_text(why) + "</p><p>次をお試しください:</p><ul>"
		 "<li>接続を確認する</li><li>アドレスのつづりを確かめる</li></ul><p class=\"code\">"
	       + html_text(code) + "</p><button onclick=\"location.reload()\">再読み込み</button></body></html>";
}

/* ---------------------------------------------------------------- 拡大・縮小 */

/* The zoom levels of Chrome, and 100 % among them */
const double	kZoom[] = { 0.25, 0.33, 0.5, 0.67, 0.75, 0.8, 0.9, 1.0, 1.1, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0, 4.0, 5.0 };
const int	kZoomOne = 7, kZoomN = (int)( sizeof(kZoom) / sizeof(kZoom[0]) );
std::map<std::string, int> g_zoom_of;		/* a host's level, as the user left it */
int		g_zoom_now = kZoomOne;		/* the page's */

std::string host_of( const std::string &url )
{
	BrUrl u;
	return br_url_parse(url, &u) ? u.host : std::string();
}

/* A page's origin, as Blink writes one: the scheme, the host, and the port when it is not the scheme's own */
std::string origin_of( const std::string &url )
{
	BrUrl u;
	if ( !br_url_parse(url, &u) || ( u.scheme != "http" && u.scheme != "https" ) ) return std::string();
	bool plain = u.port == 0 || ( u.scheme == "http" && u.port == 80 ) || ( u.scheme == "https" && u.port == 443 );
	return u.scheme + "://" + u.host + ( plain ? std::string() : ":" + std::to_string(u.port) );
}

/* The page shown at its host's level, as Chrome keeps one for each host */
void zoom_apply()
{
	if ( g_br.eng == nullptr ) return;
	auto it = g_zoom_of.find(host_of(g_br.url));
	int z = ( it != g_zoom_of.end() ) ? it->second : kZoomOne;
	if ( z == g_zoom_now ) return;
	g_zoom_now = z;
	g_br.eng->zoom(kZoom[z]);
}

/*
 * A page of another renderer about to take the window's place: at the
 * level the window shows now, which zoom_apply then follows
 */
void zoom_to( BrEngine *eng )
{
	if ( eng != nullptr && g_zoom_now != kZoomOne ) eng->zoom(kZoom[g_zoom_now]);
}

/* Ctrl+＋ (step 1), Ctrl+− (-1), Ctrl+0 (0: back to 100 %) */
void zoom_step( int step )
{
	if ( g_br.eng == nullptr ) return;
	int z = ( step == 0 ) ? kZoomOne : g_zoom_now + step;
	if ( z < 0 ) z = 0;
	if ( z >= kZoomN ) z = kZoomN - 1;
	g_zoom_of[host_of(g_br.url)] = z;
	if ( z == kZoomOne ) g_zoom_of.erase(host_of(g_br.url));
	g_zoom_now = z;
	g_br.eng->zoom(kZoom[z]);
	char buf[48];
	snprintf(buf, sizeof(buf), "拡大・縮小: %d%%", (int)( kZoom[z] * 100 + 0.5 ));
	g_br.status = buf;
	printf("browser: zoom %d%%\n", (int)( kZoom[z] * 100 + 0.5 ));
	relayout();
	g_br.full = true;
	show();
}

/* ---------------------------------------------------------------- full screen */

/*
 * The page over the whole screen (the page's requestFullscreen, or F11):
 * the window moved and sized so that its work area is the screen, its
 * frame and bars outside it, the tool panel hidden; and back where it
 * was. A page that asked is told when it is (fullscreen_done).
 */
struct FullScreen {
	bool		on = false, by_page = false;
	T_OBWPOS	was;		/* the window's place before */
	T_OBWPOS	tool_was;	/* and the tool panel's, which the window's moves carry along */
	bool		tool_kept = false;
};
FullScreen	g_fs;

/* The tool panel hidden over the whole screen, and shown again where it was */
void tool_show( bool shown )
{
	T_OBWPOS	wp;
	SZ		asz = 0;

	if ( g_tool.kw <= 0 || ob_rea_rec(g_tool.kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) < E_OK ) return;
	if ( !shown ) {
		g_fs.tool_was = wp;
		g_fs.tool_kept = true;
	} else if ( g_fs.tool_kept ) {
		wp = g_fs.tool_was;
		g_fs.tool_kept = false;
	}
	wp.flags = shown ? ( wp.flags | OB_WP_SHOWN ) : ( wp.flags & ~(UINT)OB_WP_SHOWN );
	(void)ob_wri_rec(g_tool.kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
}

void fullscreen_set( bool on, bool by_page )
{
	T_OBWPOS	wp, n;
	SZ		asz = 0;
	BrEngine	*e = g_br.eng;

	if ( e == nullptr ) return;
	if ( on == g_fs.on ) {
		if ( by_page ) e->fullscreen_done(on);
		return;
	}
	if ( ob_rea_rec(g_br.kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) < E_OK ) {
		if ( by_page ) e->fullscreen_done(false);
		return;
	}
	/*
	 * Full screen is the window manager's state (OB_WP_FULL): it makes
	 * the work area the whole screen, over the message line, with no
	 * frame or bars, and gives the window its place back when it is
	 * asked out of it. Where it was is kept here only for the object
	 * (save_place).
	 */
	n = wp;
	if ( on ) {
		g_fs.was = wp;
		n.flags |= OB_WP_FULL;
	} else {
		n.flags &= ~(UINT)OB_WP_FULL;
	}
	n.z = 0;
	n.flags |= OB_WP_SHOWN;
	if ( on ) tool_show(false);
	if ( ob_wri_rec(g_br.kw, OB_WR_PLACE, 0, &n, sizeof(n), &asz) < E_OK ) {
		if ( on ) tool_show(true);
		if ( by_page && on ) e->fullscreen_done(false);
		return;
	}
	bool page_was = g_fs.by_page;
	g_fs.on = on;
	g_fs.by_page = on && by_page;
	if ( !on ) tool_show(true);
	(void)read_size();
	relayout();
	g_br.full = true;
	show();
	if ( on ? by_page : page_was ) e->fullscreen_done(on);
	relayout();
	g_br.full = true;
	g_br.status = on ? "全画面表示です。Esc で元に戻ります" : "";
	show();
	printf("browser: full screen %s%s, %dx%d\n", on ? "on" : "off", by_page ? " (the page asked)" : "", g_br.w,
	       g_br.h);
}

/*
 * The windowing controls a page that acts as an isolated web app asked
 * for. The window system has no minimized state: a window minimized goes
 * behind every other; maximized, its outer edge is the screen's; restored,
 * it is back where it was, in front. A window that is not to be resized
 * is put back to its size whenever the user changes it (window_fixed).
 */
struct WindowControls {
	bool		kept = false;	/* the place before it was maximized or minimized */
	T_OBWPOS	was;
	bool		fixed = false;
	int		fixed_w = 0, fixed_h = 0;
};
WindowControls	g_wc;

void window_control( int op )
{
	T_OBWPOS	wp, n;
	SZ		asz = 0;

	if ( ob_rea_rec(g_br.kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) < E_OK ) return;
	n = wp;
	switch ( op ) {
	case BR_WIN_MAXIMIZE: {
		/* the outer edge the screen's the window is on */
		int sx = 0, sy = 0, sw = 0, sh = 0;
		br_screen_view(&sx, &sy, &sw, &sh);
		if ( sw <= 0 || sh <= 0 ) return;
		if ( !g_wc.kept ) {
			g_wc.was = wp;
			g_wc.kept = true;
		}
		n.left = sx;
		n.top = sy;
		n.right = sx + sw;
		n.bottom = sy + sh;
		n.z = 0;
		break;
	}
	case BR_WIN_MINIMIZE:
		if ( !g_wc.kept ) {
			g_wc.was = wp;
			g_wc.kept = true;
		}
		n.z = 0x7fff;
		break;
	case BR_WIN_RESTORE:
		if ( g_wc.kept ) n = g_wc.was;
		g_wc.kept = false;
		n.z = 0;
		break;
	case BR_WIN_FIXED:
		g_wc.fixed = true;
		g_wc.fixed_w = wp.right - wp.left;
		g_wc.fixed_h = wp.bottom - wp.top;
		printf("browser: window: not to be resized\n");
		return;
	case BR_WIN_RESIZABLE:
		g_wc.fixed = false;
		printf("browser: window: resizable\n");
		return;
	default:
		return;
	}
	n.flags |= OB_WP_SHOWN;
	if ( ob_wri_rec(g_br.kw, OB_WR_PLACE, 0, &n, sizeof(n), &asz) < E_OK ) return;
	printf("browser: window: %s, %dx%d\n", op == BR_WIN_MAXIMIZE ? "maximized" : op == BR_WIN_MINIMIZE ? "minimized" : "restored",
	       n.right - n.left, n.bottom - n.top);
	if ( read_size() ) relayout();
	g_br.full = true;
	show();
}

/* A window the page asked not to be resized put back to its size, after the user changed it */
void window_fixed()
{
	T_OBWPOS	wp;
	SZ		asz = 0;

	if ( !g_wc.fixed || ob_rea_rec(g_br.kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) < E_OK ) return;
	if ( wp.right - wp.left == g_wc.fixed_w && wp.bottom - wp.top == g_wc.fixed_h ) return;
	wp.right = wp.left + g_wc.fixed_w;
	wp.bottom = wp.top + g_wc.fixed_h;
	(void)ob_wri_rec(g_br.kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
}

bool fullscreen_was( T_OBWPOS *wp )
{
	if ( !g_fs.on ) return false;
	*wp = g_fs.was;
	return true;
}

/*
 * Esc or F11 over the whole screen: the window back where it was, and a
 * page that asked for it told it has left (as the browser leaving full
 * screen tells it, Fullscreen::DidExitFullscreen)
 */
void fullscreen_leave()
{
	fullscreen_set(false, false);
}

/* ---------------------------------------------------------------- what the page did by itself */

std::string	g_hover;		/* the link or tooltip under the pointer, on the message line */
BrEngine::Context	g_ctx;			/* what was under the pointer when the right button opened the menu */

/*
 * What a save says of itself ("said:", its start and its end) is shown
 * over the link under the pointer for this long, then the link again: a
 * word that comes once is read before the pointer's link hides it, as
 * Chrome keeps the status of its own and shows the link only while the
 * pointer is over one
 */
constexpr long	kSaidMs = 3000;
long		g_said_until;		/* until when the message line keeps what was said */

/* The message line: the link under the pointer, else what is happening */
void say_line()
{
	bool said = g_said_until != 0 && now_ms() < g_said_until;
	const std::string &line = !g_hover.empty() && !said ? g_hover : g_br.status;
	if ( line == g_br.shown_status ) return;
	g_br.shown_status = line;
	(void)wm_msg_put(line.c_str());
}

/*
 * Followed after the page's own work and after input: the address of the
 * document as the page has it now (history.pushState, a fragment, an entry
 * of the same document gone back to), its title, the link under the
 * pointer, full screen asked for or left, the switches that can be worked
 */
void follow_doc()
{
	BrEngine *e = g_br.eng;
	if ( e == nullptr || g_loading ) return;
	if ( e->history() ) {
		std::string u = e->url();
		if ( !u.empty() && u != g_br.url ) {
			g_br.url = u;
			if ( !g_tool.focus ) tool_set_url(u);
			printf("browser: address %s\n", u.c_str());
		}
	}
	std::string t = e->title();
	if ( t != g_br.title ) {
		g_br.title = t;
		show_title();
	}
#ifdef BR_BLINK
	if ( !br_multi_process ) br_ext_tab_set(g_br.url, g_br.title);
#endif
	std::string hv = e->hover();
	if ( hv != g_hover ) {
		g_hover = hv;
		say_line();
		if ( !hv.empty() ) g_addrs.push_back("> " + hv);
	} else if ( g_said_until != 0 && now_ms() >= g_said_until ) {
		/* the time of what was said is over: the link under the pointer again */
		g_said_until = 0;
		say_line();
	}
	int wa = e->window_asked();
	if ( wa != 0 ) window_control(wa);
	int fs = e->fullscreen_asked();
	if ( fs == BR_FS_ENTER ) {
		fullscreen_set(true, true);
	} else if ( fs == BR_FS_EXIT ) {
		fullscreen_set(false, true);
	}
	tool_states(false);
}

}  // namespace

/*
 * What a navigation fetches and shows, in the engine of the process that
 * runs the page: the browser's own with one process, a renderer's with
 * renderers (br_rproc.cc calls it for the browser's Navigate)
 */
int br_nav_fetch_load( BrEngine *eng, const BrNav &nav, BrNavResult *out,
		       void (*progress)( void *ctx, long got, long total ) )
{
	std::string	target = nav.target;
	const std::string &method = nav.method, &ctype = nav.ctype, &body = nav.body;
	BrResponse	r;
	bool		failed = false;
	struct NavDone { ~NavDone() { br_nav_active = false; } } nav_done;	/* on every way out */

#ifdef BR_BLINK
	/* in a renderer, the navigation begins here: its document's timing runs from here */
	if ( br_is_renderer && br_navigation_timing != nullptr ) br_navigation_timing(2);
	if ( br_is_renderer ) {
		/* a 中止 that came for this navigation before it began here is kept (Control.StopLoad's seq) */
		bool asked = br_rproc_stop_asked(nav.seq);
		g_stop = asked;
		br_net_fetch_cancel_sync(asked ? 1 : 0);
		eng->load_as(nav.how);
	}
	if ( !nav.restore.empty() ) eng->history_restore(nav.restore, nav.restore_at);
#endif
	if ( target == "about:home" || target == "about:" ) {
		r.url = target;
		r.body = kHome;
		r.charset = "utf-8";		/* the page's own text, written in UTF-8 */
		r.status = 200;
	} else if ( target == "about:blank" ) {
		r.url = target;
		r.body = "<html><head><title></title></head><body></body></html>";
		r.charset = "utf-8";
		r.status = 200;
#ifdef BR_BLINK
	} else if ( ( method.empty() || method == "GET" ) && br_sw_navigate(target, method, &r) ) {
		/* the service worker of the address's scope answered it */
#endif
	} else if ( ( br_nav_active = true, br_nav_from = nav.from,
		      !br_fetch(target, &r, progress, nullptr, method, ctype, body) )
		    && !( nav.upgraded && !g_stop && !g_closing && target.compare(0, 8, "https://") == 0
			  && br_fetch(target = "http://" + target.substr(8), &r, progress, nullptr, method, ctype, body) ) ) {
		if ( g_stop || g_closing ) {
			/* 中止 while the page was asked for: the page shown stays */
			printf("browser: %s not fetched: stopped\n", target.c_str());
			out->outcome = BR_NAV_STOPPED;
			return out->outcome;
		}
		out->error = r.error;
		r.body = error_page(target, r);
		r.url = target;
		r.charset = "utf-8";
		failed = true;
		printf("browser: failed %s: %s\n", target.c_str(), out->error.c_str());
	}
	br_nav_active = false;
#ifdef BR_BLINK
	/* a download handed over at its response is under way already: 中止 is for the page, not for it */
	bool handed = r.answer == BR_ANSWER_SAVED;
#else
	bool handed = false;
#endif
	if ( ( g_stop || g_closing ) && !handed ) {
		/* 中止 before the answer is shown or saved: it is neither, and the page shown stays */
		printf("browser: %s fetched, not shown: stopped\n", target.c_str());
		out->outcome = BR_NAV_STOPPED;
		return out->outcome;
	}
#ifdef BR_BLINK
	if ( !failed ) {
		/* a window's navigation is decided at its response (br_nav_head_fetch), anything else here */
		int answer = r.answer >= 0 ? r.answer : br_nav_answer(&r);
		if ( answer == BR_ANSWER_SAVED ) {
			/* what is saved is not shown: the page shown stays */
			out->outcome = BR_NAV_SAVED;
			return out->outcome;
		}
		if ( answer == BR_ANSWER_INVALID ) {
			r.code = "ERR_INVALID_RESPONSE";
			out->error = "無効な応答です: " + target;
			r.body = error_page(target, r);
			r.url = target;
			r.charset = "utf-8";
			failed = true;
			printf("browser: failed %s: a download answered with status %d\n", target.c_str(), r.status);
		}
	}
#endif
	beat("the page loading");
	if ( !failed ) eng->response(r.status, r.headers, r.protocol);
	eng->load(r.url, r.body, r.charset, failed ? std::string() : r.type);
#ifdef BR_BLINK
	if ( br_is_renderer ) {
		g_stop = false;
		br_net_fetch_cancel_sync(0);
	}
#endif
	out->outcome = failed ? BR_NAV_FAILED : BR_NAV_SHOWN;
	out->url = r.url;
	out->status = r.status;
	out->bytes = (long)r.body.size();
	out->title = eng->title();
	return out->outcome;
}

int BrEngine::navigate( const BrNav &nav, BrNavResult *out )
{
	return br_nav_fetch_load(this, nav, out, progress);
}

namespace {

#ifdef BR_BLINK
/* ---------------------------------------------------------------- renderers and the session history */

/*
 * The window's session history across its renderers (design 14.13). A
 * run is what one renderer showed in turn: the pages of one site and
 * the entries its engine made, same-document ones among them. A
 * navigation to another site starts a new run in a renderer of its own,
 * after the one shown (the runs after it go, as forward entries do).
 * A run stops being shown with its entries taken and its page closed
 * (run_leave): its document is unloaded, and what it played, recorded
 * or took from a camera ends with it. Going back past a run's first
 * entry, or forward past its last, goes to the run next to it, restored
 * from the entries it had (their page states) in a new renderer.
 */
struct Run {
	BrEngine			*eng = nullptr;		/* null when it is not shown, or its renderer has gone */
	std::string			site;			/* "" until a page of a site is shown */
	std::vector<std::string>	states, urls;		/* its entries, when it was last shown */
	int				at = -1;
};

int				g_last_gone_status = 0;	/* how the last renderer reaped ended (br_sys_reap) */
std::vector<Run>		g_runs(1);
std::string			g_proclog;		/* what the renderers did, for a test's "procs=" */

void proc_log( const std::string &line )
{
	g_proclog += line + "\n";
}
int				g_run = 0;
/* The next go() restores a run's entries in this page (go_run) */
std::vector<std::string>	g_nav_restore;
int				g_nav_restore_at = -1;
BrEngine			*g_nav_engine = nullptr;

/* The entries of a run kept, as it stops being shown */
void run_keep( Run &run )
{
	if ( run.eng == nullptr || run.eng->gone() ) return;
	std::vector<std::string> st, urls;
	int at = -1;
	if ( run.eng->history_save(&st, &urls, &at) && at >= 0 ) {
		run.states.swap(st);
		run.urls.swap(urls);
		run.at = at;
	}
}

bool run_has_subs( const Run &run );
void run_leave( Run &run );

/*
 * The renderers kept within the limit (br_proc_limit), with room for the
 * one kept ready: those of the runs farthest from the one shown go first,
 * their entries kept; a renderer that shows a window a page opened
 * stays. The one kept ready is started after.
 */
void runs_trim()
{
	while ( br_proc_live() + 1 > br_proc_limit() ) {
		int far = -1, dist = 0;
		for ( int i = 0; i < (int)g_runs.size(); i++ ) {
			const Run &r = g_runs[(size_t)i];
			if ( i == g_run || r.eng == nullptr || r.eng == g_br.eng || run_has_subs(r) ) continue;
			int d = i < g_run ? g_run - i : i - g_run;
			if ( d > dist ) {
				dist = d;
				far = i;
			}
		}
		if ( far < 0 ) break;
		Run &r = g_runs[(size_t)far];
		int pid = br_proc_pid(r.eng);
		printf("browser: the renderer of %s (%d) is let go: %d renderers are kept at most\n",
		       r.site.c_str(), pid, br_proc_limit());
		long free0 = br_sys_free_mb();
		br_proc_close(r.eng);
		long free1 = br_sys_free_mb();
		printf("browser: renderer %d let go: %ld MB free before, %ld MB after\n", pid, free0, free1);
		proc_log("closed pid=" + std::to_string(pid) + " freed=" + std::to_string(free1 - free0));
		r.eng = nullptr;
	}
	br_proc_keep_ready();
}

/* The runs after the one shown gone, with their renderers (a new entry after it) */
void runs_cut()
{
	while ( (int)g_runs.size() > g_run + 1 ) {
		Run &r = g_runs.back();
		if ( r.eng != nullptr && r.eng != g_br.eng && !run_has_subs(r) ) br_proc_close(r.eng);
		g_runs.pop_back();
	}
}
#endif

void zoom_to( BrEngine *eng );
#ifdef BR_BLINK
void renderer_gone();
#endif

void go( const std::string &to, int how, const std::string &method,
	 const std::string &ctype, const std::string &body )
{
	std::string	target = to, before = g_br.url;
	BrNav		nav;
	BrNavResult	r;
	long		t0 = now_ms();
	bool		hist = g_br.eng != nullptr && g_br.eng->history();
	bool		upgraded = g_typed_upgrade;	/* typed with no scheme: https first, http when it fails */
	BrEngine	*eng = g_br.eng;

	g_typed_upgrade = false;

	if ( g_closing ) return;
	/* the navigation begins: its document's timing runs from here (a renderer's: from its own start) */
#ifdef BR_BLINK
	if ( !br_multi_process && br_navigation_timing != nullptr ) br_navigation_timing(2);
#else
	if ( br_navigation_timing != nullptr ) br_navigation_timing(2);
#endif
	if ( target.find("://") == std::string::npos && target.compare(0, 6, "about:") != 0 ) {
		target = "http://" + target;
	}
	/* leaving the page shown: its beforeunload, which may ask to stay */
	if ( hist ) {
		g_br.eng->load_as(how);
		if ( !g_br.eng->may_leave(how == BR_LOAD_RELOAD) ) {
			printf("browser: %s not gone to: the page asked to stay\n", target.c_str());
			tool_set_url(g_br.url);
			return;
		}
	}
	/* a page shown over the whole screen comes back into its window first */
	if ( g_fs.on ) fullscreen_set(false, false);
	bool recorded = !hist && ( how == BR_LOAD_NEW || how == BR_LOAD_PAGE ) && !g_br.url.empty();
	if ( recorded ) {
		g_br.back.push_back(g_br.url);
		g_br.fwd.clear();
	}
	g_br.url = target;
	g_br.status = "繋いでいます…";
	g_stop = false;
	g_stop_at = 0;
#ifdef BR_BLINK
	if ( !br_multi_process ) br_net_fetch_cancel_sync(0);
#endif
	g_loading = true;
	g_br.failed = false;
	g_hover.clear();
	g_finding = false;
	tool_states(true);
	show();
	beat("fetching the page");

	nav.target = target;
	nav.how = how;
	nav.method = method;
	nav.ctype = ctype;
	nav.body = body;
	nav.from = ( how == BR_LOAD_PAGE ) ? before : std::string();
	nav.upgraded = upgraded;
#ifdef BR_BLINK
	std::string site;
	if ( br_multi_process ) {
		/*
		 * Another site than the run shown has: a page in a renderer of
		 * its own (the one kept ready). The browser's own pages and
		 * blank documents stay where they are.
		 */
		site = br_proc_site(target);
		Run &cur = g_runs[(size_t)g_run];
		if ( !g_nav_restore.empty() ) {
			nav.restore.swap(g_nav_restore);
			nav.restore_at = g_nav_restore_at;
			if ( g_nav_engine != nullptr ) eng = g_nav_engine;
			g_nav_engine = nullptr;
		} else if ( !site.empty() && !cur.site.empty() && site != cur.site && how != BR_LOAD_RELOAD ) {
			BrEngine *fresh = br_proc_new_page();
			if ( fresh != nullptr ) {
				printf("browser: %s is of another site than %s: a renderer of its own (%d)\n",
				       site.c_str(), cur.site.c_str(), br_proc_pid(fresh));
				eng = fresh;
				zoom_to(eng);
			} else {
				printf("browser: no renderer for %s: shown in the one of %s\n", site.c_str(), cur.site.c_str());
			}
		}
	}
#endif
	int outcome = eng->navigate(nav, &r);
#ifdef BR_BLINK
	/* the run's site is the one of the page it shows, after the redirects that took it there */
	if ( br_multi_process && outcome != BR_NAV_FAILED ) {
		std::string shown = br_proc_site(r.url);
		if ( !shown.empty() ) site = shown;
	}
	if ( br_multi_process && eng->gone() && outcome == BR_NAV_STOPPED && eng == g_br.eng && !g_closing ) {
		/*
		 * 中止 not taken by the renderer of the page shown, held in its
		 * work: it was ended, and the page is shown as one whose renderer
		 * has ended, at the address it was at
		 */
		if ( recorded ) g_br.back.pop_back();
		g_br.url = before;
		renderer_gone();
		g_br.status = "読み込みを中止しました。応答しないページのプロセスを終了しました";
		show();
		return;
	}
	if ( br_multi_process && eng->gone() && outcome != BR_NAV_STOPPED ) {
		/* the renderer ended while it fetched or loaded the page */
		outcome = BR_NAV_FAILED;
		r.error = "ページを表示していたプロセスが終了しました";
	}
#endif
	if ( outcome == BR_NAV_STOPPED || outcome == BR_NAV_SAVED ) {
#ifdef BR_BLINK
		if ( eng != g_br.eng ) br_proc_close(eng);	/* the renderer started for it is not wanted */
#endif
		if ( recorded ) g_br.back.pop_back();
		g_br.url = before;
		/* what is saved says so on the message line itself, when it begins and when it is done ("said:") */
		g_br.status = ( outcome == BR_NAV_STOPPED ) ? "読み込みを中止しました" : "";
		g_loading = false;
		tool_states(false);
		show();
		tool_set_url(g_br.url);
		return;
	}
	if ( outcome == BR_NAV_FAILED ) {
		g_br.status = r.error;
		g_br.failed = true;
	}
#ifdef BR_BLINK
	int left = -1;		/* the run shown before, left once the page is shown */
	if ( br_multi_process ) {
		Run &cur = g_runs[(size_t)g_run];
		if ( !nav.restore.empty() ) {
			/* a run that was left: it has a renderer again (go_run leaves the one shown before) */
			cur.eng = eng;
			if ( eng != g_br.eng ) g_br.eng = eng;
			g_ime_eng = nullptr;
			if ( cur.site.empty() ) cur.site = site;
		} else if ( eng != g_br.eng ) {
			/* the page is shown in another renderer: a new run after the one shown, which is left */
			run_keep(cur);
			left = g_run;
			runs_cut();
			Run nr;
			nr.eng = eng;
			nr.site = site;
			proc_log("switch from=" + std::to_string(br_proc_pid(g_br.eng)) + " to=" + std::to_string(br_proc_pid(eng))
				 + " site=" + site);
			g_runs.push_back(nr);
			g_run = (int)g_runs.size() - 1;
			g_br.eng = eng;
			g_ime_eng = nullptr;
		} else {
			if ( !site.empty() ) cur.site = site;
			/* a new entry in the run shown: the runs after it go */
			if ( how == BR_LOAD_NEW || how == BR_LOAD_PAGE ) runs_cut();
		}
	}
#endif
	g_br.url = r.url;
	g_loading = false;
	if ( !g_closing ) {
		/* a 中止 is for the load it stopped: the page's own requests wait again */
		g_stop = false;
#ifdef BR_BLINK
		if ( !br_multi_process ) br_net_fetch_cancel_sync(0);
#endif
	}
	beat("the page shown");
	g_br.full = true;			/* a new page: painted whole */
	g_br.title = r.title;
	g_br.scroll = 0;
	zoom_apply();
	relayout();
	if ( r.error.empty() ) {
		char buf[128];
		snprintf(buf, sizeof(buf), "  %d  %ld バイト  %ld ms", r.status, r.bytes, now_ms() - t0);
		g_br.status = g_br.title.substr(0, 160) + buf;
		printf("browser: loaded %s status %d bytes %ld height %d title %s\n",
		       r.url.c_str(), r.status, r.bytes, g_br.doc_h, g_br.title.c_str());
	}
	/* the document's address as it has it after its scripts ran (a pushState while it loaded) */
	if ( hist && !g_br.eng->url().empty() ) g_br.url = g_br.eng->url();
#ifdef BR_BLINK
	if ( !br_multi_process ) br_ext_tab_set(g_br.url, g_br.title);
	if ( br_multi_process ) proc_log("shown pid=" + std::to_string(br_proc_pid(g_br.eng)) + " title=" + g_br.title);
#endif
	show_title();
	tool_states(false);
	show();
	tool_set_url(g_br.url);
#ifdef BR_BLINK
	if ( br_multi_process && left >= 0 ) {
		/* the new page is up: the one it replaced is closed, as a document navigated away from is unloaded */
		run_leave(g_runs[(size_t)left]);
		runs_trim();
	}
#endif
}

/*
 * A wheel turned over a page, to the engine in notches: across (code
 * HID_WHEEL_H, dz > 0 to the right, as a finger moved to the left turns
 * it) or up and down (dz > 0 turned away, upwards)
 */
void wheel_to( BrEngine *eng, const T_OBNTM &m )
{
	if ( m.code == HID_WHEEL_H ) {
		eng->wheel(m.x, m.y, m.dz, 0);
	} else {
		eng->wheel(m.x, m.y, 0, -m.dz);
	}
}

void scroll_by( int d )
{
	int was = g_br.scroll;
	g_br.scroll += d;
	clamp_scroll();
	if ( g_br.scroll != was ) show();
}

/* A bar of the window worked: the view goes where it was asked to */
void scroll_bar( UINT which, int pos )
{
	if ( g_br.eng == nullptr ) return;
	if ( which == OB_BAR_B ) {
		int most = g_br.doc_w - g_br.w;
		if ( pos > most ) pos = most;
		if ( pos < 0 ) pos = 0;
		g_br.eng->set_scroll_left(pos);
		g_br.left = g_br.eng->scroll_left();
		show();
		return;
	}
	scroll_by(pos - g_br.scroll);
	show_bars();		/* the knob back where the page is, had it gone no further */
}

bool follow_page();
void after_input();
void ime_commit();

/*
 * 'offset' entries back or forward in the session history: the engine's,
 * which the page's pushState and fragments make entries of too (an entry
 * of the document shown is gone to there, another is fetched), or the
 * window's own list of addresses
 */
#ifdef BR_BLINK
/*
 * To the run before or after the one shown (design 14.13): its entries
 * restored in a new renderer and the entry it was at fetched; the run
 * shown is left once that page is shown
 */
void go_run( int k )
{
	int from = g_run;
	Run &cur = g_runs[(size_t)g_run];
	Run &to = g_runs[(size_t)k];
	BrEngine *was = g_br.eng;
	run_keep(cur);
	if ( to.at < 0 || to.at >= (int)to.urls.size() ) return;
	BrEngine *fresh = br_proc_new_page();
	if ( fresh == nullptr ) return;
	printf("browser: history: to the run of %s, restored in a new renderer (%d), entry %d of %zu\n",
	       to.site.c_str(), br_proc_pid(fresh), to.at, to.urls.size());
	proc_log("restored pid=" + std::to_string(br_proc_pid(fresh)) + " entry=" + std::to_string(to.at) + " of="
		 + std::to_string(to.urls.size()));
	g_run = k;
	g_nav_restore = to.states;
	g_nav_restore_at = to.at;
	g_nav_engine = fresh;
	go(to.urls[(size_t)to.at], BR_LOAD_HISTORY);
	if ( g_br.eng != fresh ) {
		/* not shown there (stopped): the run shown stays */
		g_run = (int)( std::find_if(g_runs.begin(), g_runs.end(), []( const Run &r ) { return r.eng == g_br.eng; })
			       - g_runs.begin() );
		if ( g_run >= (int)g_runs.size() ) g_run = 0;
		return;
	}
	if ( from != k && g_runs[(size_t)from].eng == was ) run_leave(g_runs[(size_t)from]);
	runs_trim();
}
#endif

void go_history( int offset )
{
#ifdef BR_BLINK
	/* past the first or the last entry of the run shown: the run next to it */
	if ( br_multi_process && g_br.eng != nullptr && !g_br.eng->can_go(offset) ) {
		int k = g_run + ( offset < 0 ? -1 : 1 );
		if ( k >= 0 && k < (int)g_runs.size() ) {
			ime_commit();
			go_run(k);
			return;
		}
	}
#endif
	if ( g_br.eng != nullptr && g_br.eng->history() ) {
		ime_commit();
		int r = g_br.eng->history_go(offset);
		printf("browser: history %+d: %s\n", offset,
		       r == BR_HIST_FETCH ? "another document" : r == BR_HIST_DONE ? "in the document" : "no entry there");
		if ( r == BR_HIST_FETCH ) {
			(void)follow_page();
		} else if ( r == BR_HIST_DONE ) {
			after_input();
		}
		return;
	}
	std::vector<std::string> &from = offset < 0 ? g_br.back : g_br.fwd, &to = offset < 0 ? g_br.fwd : g_br.back;
	if ( from.empty() ) return;
	to.push_back(g_br.url);
	std::string u = from.back();
	from.pop_back();
	go(u, BR_LOAD_HISTORY);
}

void go_back()
{
	go_history(-1);
}

void go_forward()
{
	go_history(1);
}

bool can_back()
{
#ifdef BR_BLINK
	if ( br_multi_process && g_run > 0 ) return true;
#endif
	return ( g_br.eng != nullptr && g_br.eng->history() ) ? g_br.eng->can_go(-1) : !g_br.back.empty();
}

bool can_forward()
{
#ifdef BR_BLINK
	if ( br_multi_process && g_run + 1 < (int)g_runs.size() ) return true;
#endif
	return ( g_br.eng != nullptr && g_br.eng->history() ) ? g_br.eng->can_go(1) : !g_br.fwd.empty();
}

/* Where the page asked to go, a form sent with POST with its body; false for nowhere */
bool follow_page()
{
	std::string to = g_br.eng->navigation();
	if ( to.empty() ) return false;
	std::string method, ctype, body;
	if ( g_br.eng->navigation_body(&method, &ctype, &body) ) {
		go(to, BR_LOAD_PAGE, method, ctype, body);
	} else {
		go(to, BR_LOAD_PAGE);
	}
	return true;
}

void follow_doc();

/* After input the page took: where it asked to go, or what it shows now */
void after_input()
{
	if ( follow_page() ) return;
	relayout();
	show();
	follow_doc();
}

/* ---------------------------------------------------------------- かな漢字変換 */

bool ime_busy()
{
	return g_ime.kid > 0 && g_ime.out.cl[g_ime.out.n_cl] > g_ime.base;
}

/*
 * What the converter gave: the committed clauses into the page's field,
 * the rest shown there as the composition, the candidates fetched
 */
void ime_take( INT r )
{
	const T_KCOUT &o = g_ime.out;

	g_ime.base = 0;
	if ( o.n_out > 0 ) {
		int end = o.cl[o.n_out];
		if ( end > 0 ) ime_target()->commit(std::string(o.text, (size_t)end));
		g_ime.base = end;
	}
	int base = g_ime.base, end = o.cl[o.n_cl];
	std::vector<int> clauses;
	int target = -1;
	for ( int i = o.n_out; i <= o.n_cl; i++ ) {
		int at = o.cl[i] < base ? base : o.cl[i];
		if ( i == o.clause && !o.yomi ) target = (int)clauses.size();
		clauses.push_back(at - base);
	}
	int caret = ( o.caret >= base && o.caret <= end ) ? o.caret - base : end - base;
	ime_target()->compose(end > base ? std::string(o.text + base, (size_t)( end - base )) : std::string(),
			  clauses, target, caret);
	g_ime.list = false;
	if ( ( r & TSMOZC_LIST ) != 0 && ime_busy() ) {
		INT sel = kc_list(g_ime.kid, g_ime.cand, sizeof(g_ime.cand), &g_ime.ncand, &g_ime.first,
				  &g_ime.total);
		if ( sel > 0 && g_ime.ncand > 0 ) {
			g_ime.list = true;
			g_ime.chosen = sel;
		}
	}
}

/* Text typed as it is (半角) or in full width, into the page's field */
bool ime_plain( UB ch, bool han )
{
	char	buf[4];
	UW	u;

	if ( ch < 0x20 || ch >= 0x7F ) return false;
	u = han ? ch : ( ch == ' ' ) ? 0x3000 : 0xFF01 + ( ch - 0x21 );
	if ( u < 0x80 ) {
		buf[0] = (char)u;
		buf[1] = 0;
	} else {
		buf[0] = (char)( 0xE0 | ( u >> 12 ) );
		buf[1] = (char)( 0x80 | ( ( u >> 6 ) & 0x3F ) );
		buf[2] = (char)( 0x80 | ( u & 0x3F ) );
		buf[3] = 0;
	}
	ime_target()->commit(buf);
	return true;
}

void ime_commit();

/*
 * A key typed while a field of the page has the focus, in the input mode
 * it came in (T_OBNTM imode): true when the converter (or full width)
 * took it, false when it goes to the page as a key. A key in 英語 that
 * comes while something is being composed -- 英数 itself, or one typed
 * after it -- commits that first.
 */
bool ime_key( UINT code, UINT mods, UINT mode )
{
	bool	han = ( mode & WM_MODE_HAN ) != 0;
	UB	ch = wm_key_char(code, mods);
	INT	r;

	/*
	 * 英語: the page takes the key as it is, whatever the width the mode
	 * says (a page's field wants the letters of the keyboard: addresses,
	 * names, numbers)
	 */
	if ( ( mode & WM_MODE_ALPH ) != 0 && ime_busy() ) ime_commit();
	if ( ( mods & ( kCtrl | kAlt ) ) != 0 || ( mode & WM_MODE_ALPH ) != 0 ) return false;
	if ( g_ime.kid <= 0 ) {
		g_ime.kid = kc_open();
		if ( g_ime.kid <= 0 ) {
			g_ime.kid = 0;
			return false;
		}
	}
	if ( g_ime.list && ch >= '1' && ch <= '9' && ch - '1' < g_ime.ncand ) {
		r = kc_choose(g_ime.kid, g_ime.first + ( ch - '1' ), &g_ime.out);
		if ( r >= 0 && r != KC_NOTMINE ) ime_take(r);
		return true;
	}
	g_ime.kmode = mode;
	r = kc_hid_key(g_ime.kid, code, mods, mode, &g_ime.out);
	if ( r >= 0 && r != KC_NOTMINE ) {
		ime_take(r);
		return true;
	}
	/* nothing being composed: a space as the mode types it, the rest the page's */
	if ( ch == ' ' && !ime_busy() ) return ime_plain(' ', han);
	return ime_busy();
}

/*
 * A key typed into a field of a page ('eng'): what was being composed
 * for another page is committed there first, and this one becomes the
 * converter's target
 */
bool ime_key_for( BrEngine *eng, UINT code, UINT mods, UINT mode )
{
	if ( eng != ime_target() ) {
		ime_commit();
		g_ime_eng = eng;
	}
	return ime_key(code, mods, mode);
}

/* Whatever is being composed, committed (a press elsewhere, the address) */
void ime_commit()
{
	g_ime.list = false;
	if ( !ime_busy() ) return;
	INT r = kc_hid_key(g_ime.kid, K_ENTER, 0x02, g_ime.kmode, &g_ime.out);
	if ( r >= 0 && r != KC_NOTMINE ) ime_take(r);
	g_ime.base = g_ime.out.cl[g_ime.out.n_cl];
	g_ime.list = false;
}

/* A press on the candidates: that one chosen; true when it was theirs */
bool ime_press( const BrEngine *eng, int x, int y )
{
	if ( !ime_list_on(eng) || g_ime.row_h <= 0 || !g_ime.box.contains((float)x, (float)y) ) return false;
	int row = (int)( ( (float)y - g_ime.box.top() - 2 ) / (float)g_ime.row_h );
	if ( row >= 0 && row < g_ime.ncand ) {
		INT r = kc_choose(g_ime.kid, g_ime.first + row, &g_ime.out);
		if ( r >= 0 && r != KC_NOTMINE ) ime_take(r);
	}
	return true;
}

/* ---------------------------------------------------------------- keys and the pointer */

/* The letter a key gives the page: Enter and Tab as theirs, 0 for none */
unsigned key_letter( UINT code, UINT mods )
{
	if ( code == K_ENTER || code == K_KPENTER ) return '\r';
	if ( code == K_TAB ) return '\t';
	UB ch = wm_key_char(code, mods);
	return ( ch >= 0x20 && ch < 0x7F ) ? ch : 0;
}

void start_address()
{
	ime_commit();
	tool_focus();
}

/* The page's eyedropper (below) */
bool picking();
void pick_answer( bool ok, uint32_t color );

void open_apart( const std::string &to );
void fullscreen_leave();

/*
 * Find in the page (Ctrl+F, the menu's 〈ページの中を検索...〉): the words asked
 * on the browser's panel (問い合わせ), the matches marked in the page and
 * the first brought into view; the message line says which of how many.
 * F3 and Ctrl+G go to the next, with Shift to the one before; Esc ends it.
 */
std::string	g_find;			/* the words found last */

void find_step( int step )
{
	if ( g_br.eng == nullptr || g_find.empty() ) return;
	int active = 0, n = g_br.eng->find(g_find, step, &active);
	char buf[64];
	if ( n > 0 ) {
		snprintf(buf, sizeof(buf), "%d / %d%s", active, n, n >= 1000 ? "+" : "");
		g_br.status = "「" + g_find + "」 " + buf + " 件(F3 で次、Shift+F3 で前、Esc で終わり)";
	} else {
		g_br.status = "「" + g_find + "」は見つかりません";
	}
	g_finding = n > 0;
	printf("browser: find %s: %d of %d\n", g_find.c_str(), active, n);
	relayout();
	show();
}

/* 〈探す〉: the words asked on a panel of the window's thread; they come back as "findword:" */
void find_ask()
{
	ime_commit();
	std::string deflt = g_find;
	panel_on_window([deflt] {
		std::string words;
		if ( !br_ask_page(BR_ASK_PROMPT, "ページの中で探す文字を入れてください", deflt, &words) || words.empty() ) return;
		command_back("findword:" + words);
	});
}

void find_end()
{
	int active = 0;
	if ( g_br.eng != nullptr ) (void)g_br.eng->find(std::string(), 0, &active);
	g_finding = false;
	g_br.status.clear();
	relayout();
	show();
}

/* A key, in the input mode it came in; false when the window is to close */
bool key( UINT code, UINT mods, UINT mode )
{
	bool ctrl = ( mods & kCtrl ) != 0, alt = ( mods & kAlt ) != 0;

	if ( code == K_ESC && picking() ) {
		pick_answer(false, 0);
		return true;
	}
	/* find in the page */
	if ( ctrl && !alt && code == K_F ) {
		find_ask();
		return true;
	}
	if ( code == K_F3 || ( ctrl && code == K_G ) ) {
		if ( g_find.empty() ) {
			find_ask();
		} else {
			find_step(( mods & kShift ) != 0 ? -1 : 1);
		}
		return true;
	}
	if ( code == K_ESC && g_finding && !g_fs.on ) {
		find_end();
		return true;
	}
	/* over the whole screen: Esc and F11 bring the page back into its window; F11 takes it there */
	if ( ( code == K_ESC || code == K_F11 ) && g_fs.on ) {
		fullscreen_leave();
		return true;
	}
	if ( code == K_F11 ) {
		fullscreen_set(true, false);
		return true;
	}
	if ( ctrl && code == K_E ) return false;
	/* 拡大・縮小: Ctrl and ＋ (the keys it is on, the keypad's), −, 0 */
	if ( ctrl && !alt && ( code == K_EQUAL || code == K_SEMI || code == K_KPPLUS ) ) {
		zoom_step(1);
		return true;
	}
	if ( ctrl && !alt && ( code == K_MINUS || code == K_KPMINUS ) ) {
		zoom_step(-1);
		return true;
	}
	if ( ctrl && !alt && ( code == K_0 || code == K_KP0 ) ) {
		zoom_step(0);
		return true;
	}
	/* Ctrl+N: a new window, a browser of its own; Alt+Home: the home page */
	if ( ctrl && code == K_N ) {
		open_apart("about:home");
		return true;
	}
	if ( alt && code == K_HOME ) {
		go(!g_br.obj.url.empty() ? g_br.obj.url : std::string("about:home"), BR_LOAD_NEW);
		return true;
	}
	if ( g_stat_on && code == K_F12 ) {
		stat_print();
		return true;
	}
	if ( ctrl && code == K_L ) {
		start_address();
		return true;
	}
	if ( ( ctrl && code == K_R ) || code == K_F5 ) {
		go(g_br.url, BR_LOAD_RELOAD);
		return true;
	}
	if ( alt && code == K_LEFT ) {
		go_back();
		return true;
	}
	if ( alt && code == K_RIGHT ) {
		go_forward();
		return true;
	}
	if ( g_tool.focus ) {
		tool_key(code, mods);
		return true;
	}
	if ( g_br.eng != nullptr && g_br.eng->input() ) {
		/* the page's: the converter first when a field of it has the focus */
		if ( g_br.eng->editable() && ime_key_for(g_br.eng, code, mods, mode) ) {
			after_input();
			return true;
		}
		(void)g_br.eng->key(code, mods, key_letter(code, mods));
		after_input();
		return true;
	}
	int page = view_h() - 40;
	switch ( code ) {
	case K_DOWN:	scroll_by(40); break;
	case K_UP:	scroll_by(-40); break;
	case K_PGDN:
	case K_SPACE:	scroll_by(page); break;
	case K_PGUP:	scroll_by(-page); break;
	case K_HOME:	scroll_by(-g_br.scroll); break;
	case K_END:	scroll_by(g_br.doc_h); break;
	case K_BS:	go_back(); break;
	default:	break;
	}
	return true;
}

bool parse_uuid( const char *s, TS_UUID *u )
{
	int n = 0;

	for ( ; *s != '\0' && n < 32; s++ ) {
		int v = ( *s >= '0' && *s <= '9' ) ? *s - '0' : ( *s >= 'a' && *s <= 'f' ) ? *s - 'a' + 10
			: ( *s >= 'A' && *s <= 'F' ) ? *s - 'A' + 10 : -1;
		if ( v < 0 ) {
			if ( *s == '-' ) continue;
			return false;
		}
		UB *b = (UB *)u + n / 2;
		*b = ( n % 2 == 0 ) ? (UB)( v << 4 ) : (UB)( *b | v );
		n++;
	}
	return n == 32;
}


/*
 * 〈編集〉〈ホームページに設定〉: the page object opens at the page shown
 * from now on. Nothing else writes its address: going to other pages,
 * back, forward and loading again leave it as it is.
 */
void set_home()
{
	if ( !g_br.obj.ok ) {
		g_br.status = "ページの実身から開いていないので、ホームページは設定できません";
	} else {
		g_br.obj.url = g_br.url;
		bool ok = br_obj_set_home(g_br.obj, g_br.title);
		printf("browser: home page %s%s\n", ok ? "set to " : "not written: ", g_br.url.c_str());
		g_br.status = ok ? "ホームページに設定しました" : "ホームページを設定できませんでした";
	}
	show();
}

#ifdef BR_BLINK
/* 〈Widevine を導入〉〈Widevine を外す〉, and the installation ended (below) */
void wv_install_ask();
void wv_install_go();
void wv_install_done();
void wv_remove_ask();
void wv_remove_go();
#endif

#ifdef BR_BLINK
/*
 * The page shown, as it stands, in the history: a new address once, and
 * the title it has come to have afterwards
 */
void history_follow()
{
	static std::string last;
	if ( g_br.url != last ) {
		last = g_br.url;
		br_history_visit(last, g_br.title);
	} else if ( !last.empty() ) {
		br_history_title(last, g_br.title);
	}
}

#endif

/*
 * What was chosen in the window's menu (the window's thread shows it,
 * menu_pop), done on the engine's thread; false to close
 */
/*
 * What an extension declares, on its panel, before it comes in: the reader
 * decides. The same panel asks for what it wants later (permissions.request).
 */
void ext_load_asked( const std::string &uuid )
{
	BrExtInfo info;
	if ( !br_ui_ext_preview(uuid, &info) ) {
		g_br.status = "拡張機能を読み込めません: " + info.error;
		printf("browser: extension: not an extension: %s\n", info.error.c_str());
		show();
		return;
	}
	/* asked on a panel of the window's thread; taken in ("extadd:") or not ("extnot") on this one */
	panel_on_window([uuid, info] {
		if ( !ext_ask_panel("「" + info.name + "」を読み込みますか。", info.name, info.version, info.wants) ) {
			printf("browser: extension: not taken, by the panel: %s\n", info.name.c_str());
			command_back("extnot");
			return;
		}
		command_back("extadd:" + uuid);
	});
}

void ext_permission( const std::string &id, int msg )
{
	std::string name;
	std::vector<std::string> wants;
	if ( !br_ui_ext_perm_words(id, msg, &name, &wants) ) return;
	/* asked on a panel of the window's thread; the answer comes back as "extpermis:" */
	panel_on_window([id, msg, name, wants] {
		bool yes = ext_ask_panel("「" + name + "」が権限を求めています。", name, std::string(), wants);
		command_back("extpermis:" + std::string(yes ? "1" : "0") + std::to_string(msg) + " " + id);
	});
}

#ifdef BR_BLINK
/*
 * The tests' waits: the page's engine let run for 'ms', and its frames
 * shown as the window's loop shows them (a renderer draws its next only
 * once the last was shown)
 */
void pump_for( long ms )
{
	for ( long until = now_ms() + ms; now_ms() < until; ) {
		if ( g_br.eng != nullptr && g_br.eng->pump() ) {
			relayout();
			show();
			g_br.eng->frame_shown();
		}
		usleep(10000);
	}
}

/* The views rastered on the GPU so far, and that count waited for to pass 'v0' (up to 'ms') */
long gpu_views()
{
	BrGpuStat g;
	return gpu_stat(&g) ? g.views : 0;
}

long gpu_views_after( long v0, long ms )
{
	long v = gpu_views();
	for ( long until = now_ms() + ms; v <= v0 && now_ms() < until; v = gpu_views() ) pump_for(100);
	return v;
}
#endif

bool menu_do( const std::string &code )
{
	if ( code.compare(0, 5, "said:") == 0 ) {
		/* a save's word for the message line (br_objfile.cc), in its turn after what the window said before */
		g_br.status = code.substr(5);
		g_said_until = now_ms() + kSaidMs;
		say_line();
		return true;
	}
	ime_commit();
	if ( code == "close" ) {
		return false;
	} else if ( code == "back" ) {
		go_back();
	} else if ( code == "forward" ) {
		go_forward();
	} else if ( code == "reload" ) {
		go(g_br.url, BR_LOAD_RELOAD);
	} else if ( code == "address" ) {
		start_address();
	} else if ( code == "home" ) {
		go("about:home", BR_LOAD_NEW);
	} else if ( code.compare(0, 8, "extopen ") == 0 ) {
		open_apart(code.substr(8));
	} else if ( code.compare(0, 8, "extpage ") == 0 ) {
		if ( g_br.eng != nullptr ) g_br.eng->open_page(code.substr(8));
	} else if ( code.compare(0, 6, "extgo ") == 0 ) {
		go(code.substr(6), BR_LOAD_NEW);
	} else if ( code.compare(0, 8, "extperm ") == 0 ) {
		size_t sp = code.find(' ', 8);
		if ( sp != std::string::npos ) ext_permission(code.substr(8, sp - 8), atoi(code.c_str() + sp + 1));
	} else if ( code.compare(0, 10, "extpermis:") == 0 && code.size() > 11 ) {
		/* the panel's answer: yes or no, the message's number, the extension's id */
		size_t sp = code.find(' ', 11);
		if ( sp != std::string::npos ) br_ui_ext_perm_answer(code.substr(sp + 1), atoi(code.c_str() + 11), code[10] == '1');
	} else if ( code.compare(0, 7, "extask ") == 0 ) {
		ext_load_asked(code.substr(7));
#ifdef BR_BLINK
	} else if ( br_multi_process && ( code.compare(0, 11, "ext.action:") == 0 || code.compare(0, 8, "ext.ctx:") == 0 ) ) {
		std::string said = br_proc_command(g_br.eng, code);
		if ( !said.empty() ) (void)wm_msg_put(said.c_str());
	} else if ( code == "killrenderer" ) {
		/* a test's: the page's renderer ended as one that fails ends */
		(void)br_proc_kill(g_br.eng);
	} else if ( code == "killnetwork" ) {
		/* a test's: the network process ended as one that fails ends, and the one started again waited for */
		int old = br_proc_network_pid(), now = 0;
		if ( old > 0 ) br_sys_kill(old);
		for ( int i = 0; i < 100; i++ ) {
			now = br_proc_network_pid();
			if ( now > 0 && now != old ) break;
			usleep(100 * 1000);
		}
		proc_log("netkill old=" + std::to_string(old) + " new=" + std::to_string(now));
	} else if ( code == "killgpu" ) {
		/*
		 * a test's: the GPU process ended as one that fails ends; the one
		 * started again waited for (or the browser's giving up), then the
		 * page's views drawn again in it
		 */
		int old = 0, now = 0, arrivals = 0, from = 0;
		bool none = false;
		br_proc_gpu_state(&old, &none, &arrivals, &from);
		long v0 = gpu_views();
		if ( old > 0 ) br_sys_kill(old);
		for ( int i = 0; i < 100; i++ ) {
			br_proc_gpu_state(&now, &none, &arrivals, &from);
			if ( none || ( now > 0 && now != old ) ) break;
			pump_for(100);
		}
		long v1 = gpu_views_after(v0, none ? 3000 : 20000);
		proc_log("gpukill old=" + std::to_string(old) + " new=" + std::to_string(now) + " none=" +
			 std::to_string(none ? 1 : 0) + " views=" + std::to_string(v0) + ">" + std::to_string(v1));
	} else if ( code == "waitgpu" ) {
		/*
		 * a test's: the GPU process started again for a GPU registered
		 * after it waited for (up to 30 s), then the views drawn again in it
		 */
		int now = 0, arrivals = 0, from = 0;
		bool none = false;
		br_proc_gpu_state(&now, &none, &arrivals, &from);
		long v0 = gpu_views();
		for ( int i = 0; i < 300 && arrivals == 0; i++ ) {
			pump_for(100);
			br_proc_gpu_state(&now, &none, &arrivals, &from);
		}
		for ( int i = 0; i < 100 && arrivals > 0 && ( now <= 0 || now == from ); i++ ) {
			pump_for(100);
			br_proc_gpu_state(&now, &none, &arrivals, &from);
		}
		long v1 = arrivals > 0 ? gpu_views_after(v0, 20000) : v0;
		proc_log("gpuarrived n=" + std::to_string(arrivals) + " old=" + std::to_string(from) + " new=" +
			 std::to_string(now) + " views=" + std::to_string(v0) + ">" + std::to_string(v1));
#endif
	} else if ( code.compare(0, 11, "ext.action:") == 0 ) {
		br_ext_action_click(atoi(code.c_str() + 11));
	} else if ( code.compare(0, 8, "ext.ctx:") == 0 ) {
		BrExtMenuAt at;
		at.page = g_br.url;
		at.link = g_ctx.link;
		at.image = g_ctx.image;
		at.selection = g_ctx.selection;
		at.editable = g_ctx.editable;
		br_ext_menu_click(atoi(code.c_str() + 8), at);
	} else if ( code == "extload" ) {
		/* 〈拡張機能を読み込む〉: the objects of ZIPs and CRXs the file conversion took in, chosen on a panel */
		panel_on_window([] {
			std::vector<BrObjLink> links;
			if ( !br_choose_objects(false, &links) ) return;
			for ( const BrObjLink &l : links ) {
				char t[TS_UUID_STRLEN + 1];
				TS_UUID u;
				memcpy(u.b, l.id, 16);
				ts_uuid_text(&u, t);
				command_back(std::string("extloadid:") + t);
			}
		});
	} else if ( code.compare(0, 10, "extloadid:") == 0 ) {
		ext_load_asked(code.substr(10));
	} else if ( code.compare(0, 7, "extadd:") == 0 ) {
		std::string why;
		g_br.status = br_ui_ext_add(code.substr(7), &why) ? "拡張機能を読み込みました。次に開くページから働きます"
								  : "拡張機能を読み込めません: " + why;
		show();
	} else if ( code == "extnot" ) {
		g_br.status = "拡張機能の読み込みを取り消しました";
		show();
	} else if ( code == "sethome" ) {
		set_home();
	} else if ( code == "isolated" ) {
		/* the page's origin given (or no longer given) what an isolated web app has, the page loaded again */
		std::string o = origin_of(g_br.url);
		if ( !o.empty() ) {
#ifdef BR_BLINK
			if ( br_multi_process ) {
				(void)br_proc_command(g_br.eng, "isolated:" + o);
			} else
#endif
			br_isolated_set(o, !br_isolated_allowed(o));
			go(g_br.url, BR_LOAD_RELOAD);
		}
	} else if ( code == "find" ) {
		find_ask();
	} else if ( code.compare(0, 9, "findword:") == 0 ) {
		g_find = code.substr(9);
		find_step(0);
	} else if ( code == "zoomin" || code == "zoomout" || code == "zoomreset" ) {
		zoom_step(code == "zoomin" ? 1 : code == "zoomout" ? -1 : 0);
	} else if ( code == "fullscreen" ) {
		if ( g_fs.on ) {
			fullscreen_leave();
		} else {
			fullscreen_set(true, false);
		}
	} else if ( code == "print" ) {
		if ( !g_br.eng->print() ) printf("browser: the page was not printed\n");
#ifdef BR_BLINK
	} else if ( code == "openlink" ) {
		if ( !g_ctx.link.empty() ) open_apart(g_ctx.link);
	} else if ( code == "copylink" || code == "copyimage" ) {
		/* the address into the tray, as text */
		const std::string &a = ( code == "copylink" ) ? g_ctx.link : g_ctx.image;
		bool ok = !a.empty() && br_tray_put(a, std::vector<uint8_t>(), "アドレス");
		printf("browser: %s %s the tray\n", a.c_str(), ok ? "put in" : "not put in");
	} else if ( code == "saveimage" ) {
		if ( !g_ctx.image.empty() ) {
			g_br.eng->save_url(g_ctx.image);
			g_br.status = "画像を保存箱に保存しています…";
			show();
		}
	} else if ( code == "copysel" || code == "copy" ) {
		g_br.eng->edit("Copy");
	} else if ( code == "cut" ) {
		g_br.eng->edit("Cut");
	} else if ( code == "paste" ) {
		g_br.eng->edit("Paste");
		after_input();
	} else if ( code == "selall" ) {
		g_br.eng->edit("SelectAll");
		after_input();
#endif
#ifdef BR_BLINK
	} else if ( br_multi_process && ( code == "hstsadd" || code == "flushstores" || code == "mplay"
					  || code == "mprev" || code == "mnext" || code == "mstop" ) ) {
		/* carried out by the page's renderer, where the stores and the media are */
		std::string said = br_proc_command(g_br.eng, code == "hstsadd" ? "hstsadd:" + g_step_host : code);
		if ( !said.empty() ) (void)wm_msg_put(said.c_str());
	} else if ( code == "settings" ) {
		br_settings_open();
	} else if ( code == "hstsadd" ) {
		br_net_hsts_add(g_step_host);
	} else if ( code == "flushstores" ) {
		/* what a clearing took is off the disk now, not at the next save */
		(void)br_store_flush_async();
	} else if ( code == "mplay" || code == "mprev" || code == "mnext" || code == "mstop" ) {
		/* 〈メディア〉: told to the page's media session */
		(void)wm_msg_put(br_media_session_action(code).c_str());
	} else if ( code == "wvinstall" || code == "wvremove" ) {
		/*
		 * Its panel is put up from the loop, as what comes next: the
		 * start-up lines' items are done before the page is said to
		 * be ready, and a panel waits for its answer
		 */
		std::lock_guard<std::mutex> g(g_ev_lock);
		g_commands.push_back(code == "wvinstall" ? "wvask" : "wvaskremove");
		loop_wake();
	} else if ( code == "wvask" ) {
		wv_install_ask();
	} else if ( code == "wvaskremove" ) {
		wv_remove_ask();
	} else if ( code == "wvgo" ) {
		wv_install_go();
	} else if ( code == "wvremovego" ) {
		wv_remove_go();
	} else if ( code == "wvdone" ) {
		wv_install_done();
#endif
	}
	return true;
}

#ifdef BR_BLINK
/*
 * Links or letters let go over the page (design 14.7.6): each link given
 * to the page as the one file its object is given out as, the letters as
 * text, where they were let go. Answered taken or not, which the one who
 * dropped them is told.
 */
void page_drop()
{
	T_OBDROP	*d;
	T_OBDRANS	a;
	SZ		asz = 0;
	std::vector<std::pair<std::string, std::string>> files;

	/* the drop whole: every link, or all the letters */
	d = ob_drop_read(g_br.kw);
	if ( d == NULL ) return;
	for ( INT i = 0; i < d->n; i++ ) {
		std::string name;
		std::vector<uint8_t> bytes;
		if ( !br_obj_export(d->v[i].target.b, &name, &bytes) ) continue;
		/* kept where the page reads it: in this process, or in the page's renderer */
		std::string path = br_multi_process ? br_proc_keep_file(g_br.eng, br_uuid_str(d->v[i].target.b), name, bytes)
						    : br_file_keep(br_uuid_str(d->v[i].target.b), name, std::move(bytes));
		files.emplace_back(path, name);
	}
	std::string text = ( d->n == 0 ) ? std::string((const char *)OB_DROP_LETTERS(d)) : std::string();
	bool took = ( !files.empty() || !text.empty() ) && g_br.eng->drop(d->x, d->y, files, text, d->mods);
	memset(&a, 0, sizeof(a));
	a.seq = d->seq;
	free(d);
	a.answer = took ? OB_DR_ACCEPT : OB_DR_REFUSE;
	if ( !took ) snprintf((char *)a.msg, sizeof(a.msg), "このページはここでは受け取りません");
	(void)ob_wri_rec(g_br.kw, OB_WR_DROP, 0, &a, sizeof(a), &asz);
	after_input();
}

/*
 * The pointer has left the window while the page drags something: the
 * desktop carries it on as letters (its address, or its text), to be let
 * go over another window that takes letters
 */
void carry_out()
{
	T_DTREQ		rq;
	T_DTANS		an;
	std::string	t = g_br.eng->dragging();

	memset(&rq, 0, sizeof(rq));
	memset(&an, 0, sizeof(an));
	rq.req = DT_RQ_CARRY;
	rq.flags = DT_F_TEXT;
	rq.recno = -1;
	/* one line, all of it, in the request's tail */
	for ( char &c : t ) {
		if ( c == '\r' || c == '\n' || c == '\t' ) c = ' ';
	}
	rq.look.w = 160;
	rq.look.h = 20;
	rq.look.frcol = rq.look.chcol = rq.look.tbcol = rq.look.bgcol = 0xFFFFFFFFU;
	rq.grab_x = 4;
	rq.grab_y = 10;
	memcpy(rq.win.b, g_br.win.b, 16);
	ER er = br_ask_desktop(&rq, { { DT_TAIL_TEXT, t } }, &an, 30 * 60 * 1000);
	printf("browser: the page's drag carried by the desktop (%d)\n", (int)er);
	g_br.eng->drag_away(er >= E_OK);
	after_input();
}
#endif

/* ---------------------------------------------------------------- the page's eyedropper */

/*
 * A page's EyeDropper waits for a colour: the message line asks for a
 * point, and the next press on the page gives the colour drawn there; Esc
 * gives none. The service's thread waits for the answer.
 */
std::mutex		g_pick_lock;
std::condition_variable	g_pick_cv;
bool			g_pick_wanted, g_pick_asked, g_pick_done, g_pick_ok;
uint32_t		g_pick_color;
std::string		g_pick_was;		/* the message line before it asked */

bool picking()
{
	std::lock_guard<std::mutex> hold(g_pick_lock);
	return g_pick_wanted && !g_pick_done;
}

void pick_answer( bool ok, uint32_t color )
{
	{
		std::lock_guard<std::mutex> hold(g_pick_lock);
		if ( !g_pick_wanted || g_pick_done ) return;
		g_pick_ok = ok;
		g_pick_color = color;
		g_pick_done = true;
	}
	g_pick_cv.notify_all();
	g_br.status = g_pick_was;
	printf("browser: eyedropper: %s %06x\n", ok ? "picked" : "given up", (unsigned)( color & 0xFFFFFF ));
}

/* From the loop: the message line asks for a point once a page has asked for a colour */
void pick_tick()
{
	{
		std::lock_guard<std::mutex> hold(g_pick_lock);
		if ( !g_pick_wanted || g_pick_done || g_pick_asked ) return;
		g_pick_asked = true;
	}
	g_pick_was = g_br.status;
	g_br.status = "色を取る点をページの上で押してください(Escでやめます)";
}

/* A press on the page while a colour is wanted: the colour of that point */
bool pick_press( int x, int y )
{
	if ( !picking() ) return false;
	uint32_t c = 0xFF000000U;
	SkPixmap pm;
	if ( g_br.surf != nullptr && g_br.surf->peekPixels(&pm) && x < pm.width() && y < pm.height() ) {
		c = (uint32_t)pm.getColor(x, y) | 0xFF000000U;
	}
	pick_answer(true, c);
	return true;
}

/*
 * The right button on the page: the page's contextmenu event first; when
 * the page leaves it, the window's menu, with the items for what is under
 * the pointer (a link, an image, the text chosen) shown. False to close.
 */
bool context_press( const T_OBNTM &m )
{
	g_ctx = BrEngine::Context();
	if ( g_br.eng != nullptr && g_br.eng->input() && m.x >= 0 && m.y >= 0 && m.x < g_br.w && m.y < view_h() ) {
		ime_commit();
		bool took = g_br.eng->context(m.x, m.y, &g_ctx);
		after_input();
		if ( took ) {
			printf("browser: the page took the right button\n");
			return true;
		}
	}
	/* the menu shown by the window's thread; what is chosen comes back as a command (window_menu) */
	{
		std::lock_guard<std::mutex> g(g_ev_lock);
		g_ctx_menu = true;
		g_ctx_menu_press = m;
		g_ctx_menu_at = g_ctx;
	}
	return true;
}

/* A button went down (code 0 the left, 1 the right); false to close */
bool press( const T_OBNTM &m )
{
	int x = m.x, y = m.y;

#ifdef BR_BLINK
	if ( m.code == 1 && !ctx_take(m) ) return true;	/* the window's thread showed the menu for it */
#endif
	if ( m.code == 1 ) return context_press(m);
	if ( x < 0 || y < 0 || x >= g_br.w || y >= view_h() ) {
		return true;		/* the frame, its bars: the desktop works them */
	}
	if ( pick_press(x, y) ) return true;
	if ( ime_press(g_br.eng, x, y) ) {
		after_input();
		return true;
	}
	if ( g_br.eng == nullptr ) return true;
	if ( g_tool.focus ) {
		/* a press on the page ends the address's editing */
		g_tool.focus = false;
		g_tool.sel_all = false;
		tool_redraw();
	}
	if ( g_br.eng->input() ) {
		ime_commit();
		/* the middle button (2) is the page's too: on a link it opens it in a window of its own */
		g_br.eng->mouse(BR_M_DOWN, x, y, m.code == 2 ? 1 : 0, m.mods);
		after_input();
		return true;
	}
	if ( m.code != 0 ) return true;
	if ( g_br.eng->click(x, y + g_br.scroll) ) {
		/* the page took it: what it asks for, and what it shows now */
		after_input();
		return true;
	}
	std::string to = g_br.eng->link_at(x, y + g_br.scroll);
	if ( !to.empty() ) go(to, BR_LOAD_NEW);
	return true;
}

void release( const T_OBNTM &m )
{
	if ( ( m.code != 0 && m.code != 2 ) || g_br.eng == nullptr || !g_br.eng->input() ) return;
	g_br.eng->mouse(BR_M_UP, m.x, m.y, m.code == 2 ? 1 : 0, m.mods);
	after_input();
}

/* ---------------------------------------------------------------- the windows pages open */

#ifndef BR_BLINK
}  // namespace

/* The engine that shows text has no videos, and opens no windows (br.h) */
bool br_pip_next( BrPipAsk * ) { return false; }
void br_pip_closed( int ) {}
void br_pip_press( int, bool ) {}
bool br_media_paint_player( int, SkCanvas *c, int, int )
{
	c->clear(SK_ColorBLACK);
	return false;
}
bool br_media_player_paused( int ) { return true; }
long br_media_player_frames( int ) { return -1; }
/* and no isolated web apps */
bool br_isolated_allowed( const std::string & ) { return false; }
void br_isolated_set( const std::string &, bool ) {}

namespace {
#endif

/*
 * A window a page opened with its opener: a page of the same engine in a
 * window of this process, with a tool panel of its own. Its notices come to the
 * page's port with N_SUB + its number. It is drawn as the page's window
 * is, where the engine says it changed, in the surface of its work area.
 * A video's picture-in-picture window has no engine but a player, drawn
 * whole each time it shows another frame.
 */
struct SubWin {
	BrEngine		*eng = nullptr;
	int			player = -1;		/* a video's window: its player */
	int			session = 0;		/* and its session (pip_tessronos.cc) */
	BrEngine		*owner = nullptr;	/* and, with renderers, a page of the renderer whose player it is */
	long			frames = -1;		/* the player's count when it was last drawn */
	TS_UUID			win;
	ID			kw = 0;
	INT			gid = -1;
	UINT			id = 0;
	int			w = 0, h = 0;
	T_WMSURF		shm;
	bool			shared = false;
	sk_sp<SkSurface>	surf;
	std::vector<UW>		px;
	bool			full = true;
	bool			pip = false;
	std::string		url, title, shown_title;
	bool			ime_shown = false;	/* the converter's list was laid over the last view */
	bool			moved = false;
	int			move_x = 0, move_y = 0;
	unsigned		move_mods = 0;
	Tool			tool;			/* its address and switches; none in a picture-in-picture window */
};

std::vector<SubWin *>		g_subs;
std::mutex			g_subs_lock;	/* g_subs, changed on the engine's thread, read by the window's when it closes */
std::vector<BrEngine *>		g_gone;		/* the engines of windows closed, kept: Blink may still name their pages */
std::vector<std::string>	g_apart_lines;	/* given to the browsers opened apart (tests) */
UINT				g_next_sub = 0;
BrEngine			*g_pip_from = nullptr;	/* the page of the renderer that asked for the window being opened */

/*
 * A video's picture-in-picture window: the player is the engine's side's,
 * this process's or, with renderers, the one of the renderer that asked
 * for the window (pip_tessronos.cc, br_proc.cc)
 */
bool pip_next( BrPipAsk *a )
{
#ifdef BR_BLINK
	if ( br_multi_process ) return br_proc_pip_next(a, &g_pip_from);
#endif
	return br_pip_next(a);
}

void pip_closed( BrEngine *owner, int session )
{
#ifdef BR_BLINK
	if ( br_multi_process ) {
		br_proc_pip_closed(owner, session);
		return;
	}
#endif
	(void)owner;
	br_pip_closed(session);
}

bool player_paused( BrEngine *owner, int player )
{
#ifdef BR_BLINK
	if ( br_multi_process ) return br_proc_player_paused(owner, player);
#endif
	(void)owner;
	return br_media_player_paused(player);
}

long player_frames( BrEngine *owner, int player )
{
#ifdef BR_BLINK
	if ( br_multi_process ) return br_proc_player_frames(owner, player);
#endif
	(void)owner;
	return br_media_player_frames(player);
}

bool paint_player( BrEngine *owner, int session, int player, SkCanvas *c, int w, int h )
{
#ifdef BR_BLINK
	if ( br_multi_process ) return br_proc_paint_player(owner, session, player, c, w, h);
#endif
	(void)owner;
	(void)session;
	return br_media_paint_player(player, c, w, h);
}

void pip_press( BrEngine *owner, int session, bool play )
{
#ifdef BR_BLINK
	if ( br_multi_process ) {
		br_proc_pip_press(owner, session, play);
		return;
	}
#endif
	(void)owner;
	br_pip_press(session, play);
}

#ifdef BR_BLINK
/* Whether a window a page opened shows a page of the run's renderer, which then stays */
bool run_has_subs( const Run &run )
{
	int pid = br_proc_pid(run.eng);
	if ( pid <= 0 ) return false;
	for ( SubWin *w : g_subs ) {
		if ( w->eng != nullptr && br_proc_pid(w->eng) == pid ) return true;
	}
	return false;
}
#endif

SubWin *sub_of( UINT id )
{
	for ( SubWin *w : g_subs ) {
		if ( w->id == ( id & kNoteSubMask ) ) return w;
	}
	return nullptr;
}

/* Its work area's surface, as read_size has the page's window's; true when it changed */
bool sub_size( SubWin &s )
{
	T_OBWPOS	pos;
	SZ		asz = 0;

	memset(&pos, 0, sizeof(pos));
	if ( ob_rea_rec(s.kw, OB_WR_PLACE, 0, &pos, sizeof(pos), &asz) < E_OK ) return false;
	if ( !g_copy ) {
		T_WMSURF m;
		if ( wm_obj_surf(s.kw, &m) >= E_OK ) {
			if ( s.shared && m.serial == s.shm.serial ) return false;
			s.shm = m;
			s.shared = true;
			s.full = true;
			s.w = m.width;
			s.h = m.height;
			s.surf = SkSurfaces::WrapPixels(SkImageInfo::MakeN32Premul(m.width, m.height), m.addr,
							(size_t)m.pitch);
			s.px.clear();
			return true;
		}
		s.shared = false;
	}
	int w = pos.wright - pos.wleft, h = pos.wbottom - pos.wtop;
	if ( w < 40 ) w = 40;
	if ( h < 30 ) h = 30;
	if ( w == s.w && h == s.h && s.px.size() == (size_t)w * h ) return false;
	s.w = w;
	s.h = h;
	s.surf = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(w, h));
	s.px.assign((size_t)w * h, 0);
	s.full = true;
	return true;
}

/* Its page drawn where it changed (whole after a new page or size), and put in the window */
void sub_put_all( SubWin &s );

void sub_show( SubWin &s )
{
	if ( s.surf == nullptr ) return;
	if ( s.eng == nullptr ) {
		/* a video's window: the player's frame, whole */
		s.frames = player_frames(s.owner, s.player);
		(void)paint_player(s.owner, s.session, s.player, s.surf->getCanvas(), s.w, s.h);
		sub_put_all(s);
		return;
	}
	(void)s.eng->layout(s.w);
	int top = s.eng->scroll_top();
	if ( top < 0 ) top = 0;
	std::vector<BrRect> dmg;
	bool all = !s.eng->damage(top, s.h, &dmg) || s.full;
	s.full = false;
	/* the converter's list over its view: painted whole while it is there, and once after */
	bool listed = ime_list_on(s.eng);
	if ( listed || s.ime_shown ) all = true;
	s.ime_shown = listed;
	if ( all ) {
		BrRect r;
		r.w = s.w;
		r.h = s.h;
		dmg.assign(1, r);
	} else {
		gather(dmg, s.w, s.h);
	}
	if ( dmg.empty() ) return;
	SkCanvas *c = s.surf->getCanvas();
	for ( const BrRect &r : dmg ) {
		c->save();
		c->clipRect(SkRect::MakeXYWH((float)r.x, (float)r.y, (float)r.w, (float)r.h));
		s.eng->paint(c, top, s.h);
		c->restore();
	}
	if ( listed ) ime_draw_list(c, s.eng, s.h);
	if ( s.shared ) {
		T_DPRECT	pr[WM_PUT_MAX];
		int		n = 0;
		for ( const BrRect &r : dmg ) {
			if ( n == WM_PUT_MAX ) break;
			pr[n].left = r.x;
			pr[n].top = r.y;
			pr[n].right = r.x + r.w;
			pr[n].bottom = r.y + r.h;
			n++;
		}
		if ( wm_obj_put(s.kw, pr, n) < E_OK ) s.full = true;
		return;
	}
	SkPixmap pm;
	if ( !s.surf->peekPixels(&pm) ) return;
	for ( int y = 0; y < s.h; y++ ) {
		const uint32_t *row = pm.addr32(0, y);
		UW *out = &s.px[(size_t)y * s.w];
		for ( int x = 0; x < s.w; x++ ) out[x] = row[x] & 0x00FFFFFF;
	}
	(void)dp_put_argb(s.gid, 0, 0, s.px.data(), s.w, s.w, s.h, 0xFF000000);
	(void)wm_obj_flush(s.kw, nullptr);
}

/* The whole of its surface put in the window */
void sub_put_all( SubWin &s )
{
	if ( s.shared ) {
		T_DPRECT r = { 0, 0, s.w, s.h };
		if ( wm_obj_put(s.kw, &r, 1) < E_OK ) s.full = true;
		return;
	}
	SkPixmap pm;
	if ( !s.surf->peekPixels(&pm) ) return;
	for ( int y = 0; y < s.h; y++ ) {
		const uint32_t *row = pm.addr32(0, y);
		UW *out = &s.px[(size_t)y * s.w];
		for ( int x = 0; x < s.w; x++ ) out[x] = row[x] & 0x00FFFFFF;
	}
	(void)dp_put_argb(s.gid, 0, 0, s.px.data(), s.w, s.w, s.h, 0xFF000000);
	(void)wm_obj_flush(s.kw, nullptr);
}

#ifdef BR_BLINK
/* A video's window whose player's frame its renderer has drawn (br_proc_paint_player): drawn, and put in the window */
void player_painted( BrEngine *owner, int session )
{
	for ( SubWin *s : g_subs ) {
		if ( s->eng != nullptr || s->owner != owner || s->session != session || s->surf == nullptr ) continue;
		if ( br_proc_player_pixels(owner, session, s->surf->getCanvas()) ) sub_put_all(*s);
	}
}
#endif

/* Its band: the page's title, or its address while it has none */
void sub_title( SubWin &s )
{
	if ( s.eng == nullptr ) return;
	std::string t = s.eng->title();
	if ( !t.empty() ) s.title = t;
	std::string shown = !s.title.empty() ? s.title : s.url.empty() ? std::string("ブラウザ") : s.url;
	if ( shown == s.shown_title ) return;
	s.shown_title = shown;
	set_title_of(s.kw, shown);
}

/* An address its page goes to: fetched and loaded there, as go() does for the page's window */
bool sub_go( SubWin &s, const std::string &to, const std::string &method, const std::string &ctype,
	     const std::string &body, int how = -1 )
{
	BrResponse	r;
	bool		failed = false;

	beat("a window's page fetched");
#ifdef BR_BLINK
	if ( br_multi_process ) {
		/*
		 * The page's renderer fetches it, as it fetches the window's own
		 * (go): the network, the service workers and the extensions'
		 * own pages are there
		 */
		BrNav		nav;
		BrNavResult	res;
		nav.target = to;
		nav.how = ( how >= 0 ) ? how : BR_LOAD_PAGE;
		nav.method = method;
		nav.ctype = ctype;
		nav.body = body;
		int outcome = s.eng->navigate(nav, &res);
		if ( outcome == BR_NAV_SAVED ) return true;
		if ( outcome == BR_NAV_STOPPED ) return false;
		s.url = res.url;
		s.title.clear();
		printf("browser: a window's page %s status %d bytes %ld title %s\n", res.url.c_str(), res.status,
		       res.bytes, res.title.c_str());
		s.full = true;
		sub_title(s);
		sub_show(s);
		if ( s.tool.kw > 0 ) tool_set_url(s.tool, s.url);
		return outcome == BR_NAV_SHOWN;
	}
#endif
	if ( to == "about:blank" ) {
		r.url = to;
		r.body = "<html><head><title></title></head><body></body></html>";
		r.charset = "utf-8";
		r.status = 200;
	} else if ( !br_fetch(to, &r, nullptr, nullptr, method, ctype, body) ) {
		r.body = "<html><head><title>読めません</title></head><body><h2>ページが読めません</h2><p>"
			 + r.error + "</p><p>" + to + "</p></body></html>";
		r.url = to;
		r.charset = "utf-8";
		failed = true;
		printf("browser: failed %s: %s\n", to.c_str(), r.error.c_str());
	}
#ifdef BR_BLINK
	if ( !failed ) {
		int answer = br_nav_answer(&r);
		if ( answer == BR_ANSWER_SAVED ) return true;
		if ( answer == BR_ANSWER_INVALID ) {
			r.code = "ERR_INVALID_RESPONSE";
			r.body = error_page(to, r);
			r.url = to;
			r.charset = "utf-8";
			failed = true;
		}
	}
#endif
	s.url = r.url;
	s.title.clear();
	beat("a window's page loading");
	if ( !failed ) s.eng->response(r.status, r.headers, r.protocol);
	if ( how >= 0 ) s.eng->load_as(how);
	s.eng->load(r.url, r.body, r.charset, failed ? std::string() : r.type);
	printf("browser: a window's page %s status %d bytes %zu title %s\n", r.url.c_str(), r.status,
	       r.body.size(), s.eng->title().c_str());
	s.full = true;
	sub_title(s);
	sub_show(s);
	if ( s.tool.kw > 0 ) tool_set_url(s.tool, s.url);
	return !failed;
}

std::string tool_address( Tool &tl )
{
	return tl.sub != nullptr ? tl.sub->url : g_br.url;
}

void tool_go( Tool &tl, const std::string &to )
{
	if ( tl.sub == nullptr ) {
		go(to, BR_LOAD_NEW);
		return;
	}
	/* typed with no scheme: https first, http when it fails */
	bool upgraded = g_typed_upgrade;
	g_typed_upgrade = false;
	if ( !sub_go(*tl.sub, to, std::string(), std::string(), std::string(), BR_LOAD_NEW) && upgraded
	     && to.compare(0, 8, "https://") == 0 ) {
		(void)sub_go(*tl.sub, "http://" + to.substr(8), std::string(), std::string(), std::string(), BR_LOAD_NEW);
	}
}

/* The address of its document as the page has it now, in its field: where the window really is */
void sub_follow( SubWin &s )
{
	if ( s.eng == nullptr || !s.eng->history() ) return;
	std::string u = s.eng->url();
	if ( !u.empty() && u != s.url ) {
		s.url = u;
		printf("browser: window %u address %s\n", s.id - N_SUB, u.c_str());
		if ( s.tool.kw > 0 && !s.tool.focus ) tool_set_url(s.tool, u);
	}
	if ( s.tool.kw > 0 ) {
		tool_off(s.tool, T_BACK, !s.eng->can_go(-1));
		tool_off(s.tool, T_FWD, !s.eng->can_go(1));
		tool_off(s.tool, T_STOP, true);		/* its page is fetched in one turn of the loop */
	}
}

/* Back or forward in its page's history: another document is fetched from where its navigation is taken */
void sub_history( SubWin &s, int offset )
{
	if ( s.eng == nullptr || !s.eng->history() ) return;
	if ( s.eng->history_go(offset) == BR_HIST_DONE ) sub_show(s);
}

/* A window for a page another page opened, where the page asked, else down and right of the page's window */
void sub_open( const BrOpened &o, int player = -1, int session = 0 )
{
	T_OBWPOS	wp;
	T_OBCRE		c;
	T_OBNTF		req;
	SZ		asz = 0;
	char		json[160];

	/*
	 * The page's place is from the corner of the screen its window is on,
	 * the one screen the page is told of (br_screen_rect); with none, down
	 * and right of the page's window, on the same screen
	 */
	int sx = 0, sy = 0, sw = 0, sh = 0;
	br_screen_view(&sx, &sy, &sw, &sh);
	memset(&wp, 0, sizeof(wp));
	(void)ob_rea_rec(g_br.kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
	int k = (int)( g_subs.size() % 6 ) + 1;
	int x = o.x >= 0 ? sx + o.x : wp.left + 40 * k;
	int y = o.y >= 0 ? sy + o.y : wp.top + 40 * k;
	bool has_tool = !o.pip && player < 0;
	if ( has_tool && y < sy + 30 + kToolH + kToolGap ) y = sy + 30 + kToolH + kToolGap;
	auto *s = new SubWin;
	s->eng = o.eng;
	s->pip = o.pip;
	s->player = player;
	s->session = session;
	s->owner = ( player >= 0 ) ? g_pip_from : nullptr;
	s->id = N_SUB + g_next_sub++;
	snprintf(json, sizeof(json), "{\"rect\":[%d,%d,%d,%d],\"attr\":%d}", x, y, x + o.w + kFrameW,
		 y + o.h + kTitleH, WM_ATTR_FRAME | WM_ATTR_TITLE | WM_ATTR_RESIZE
					   | ( ( o.pip || player >= 0 ) ? WM_ATTR_FLOAT : 0 ));	/* picture-in-picture floats */
	memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (CONST UB *)( o.pip ? "ピクチャインピクチャ" : "ブラウザ" );
	c.json = (CONST UB *)json;
	c.jsonsz = (SZ)strlen(json);
	if ( ob_cre_obj(&c, &s->win) < E_OK ) {
		printf("browser: no window for a page opened\n");
		if ( o.eng != nullptr ) {
			o.eng->closed();
			g_gone.push_back(o.eng);
		} else {
			pip_closed(g_pip_from, session);
		}
		delete s;
		return;
	}
	s->kw = ob_opn_obj(&s->win, OB_OP_R | OB_OP_W | OB_OP_PROT);
	s->gid = ( s->kw > 0 ) ? wm_obj_gid(s->kw) : E_ID;
	/* the work area as large as the page asked, whatever this frame takes */
	memset(&wp, 0, sizeof(wp));
	if ( s->kw > 0 && ob_rea_rec(s->kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		int dw = o.w - ( wp.wright - wp.wleft ), dh = o.h - ( wp.wbottom - wp.wtop );
		if ( dw != 0 || dh != 0 ) {
			wp.right += dw;
			wp.bottom += dh;
			(void)ob_wri_rec(s->kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
		}
	}
	memset(&req, 0, sizeof(req));
	req.events = OB_E_CLOSE | OB_E_PRESS | OB_E_KEY | OB_E_REDRAW | OB_E_DELETE | OB_E_RELEASE | OB_E_MOVE;
#ifdef OB_E_WHEEL
	req.events |= OB_E_WHEEL;
#endif
	req.id = s->id;
	if ( s->kw <= 0 || s->gid < 0 || ob_ntf_evt(s->kw, OB_REC_ANY, &req, g_br.port) <= 0 ) {
		printf("browser: the window of a page opened cannot be used\n");
		if ( s->kw > 0 ) ob_cls_obj(s->kw);
		(void)ob_del_obj(&s->win);
		if ( o.eng != nullptr ) {
			o.eng->closed();
			g_gone.push_back(o.eng);
		} else {
			pip_closed(g_pip_from, session);
		}
		delete s;
		return;
	}
	{
		std::lock_guard<std::mutex> hold(g_subs_lock);
		g_subs.push_back(s);
	}
	(void)sub_size(*s);
	printf("browser: window %u opened, %dx%d%s\n", s->id - N_SUB, s->w, s->h, s->pip ? " (picture-in-picture)" : "");
	if ( has_tool ) {
		s->tool.sub = s;
		if ( !tool_open(s->tool, s->kw, s->win, kNoteSubTool + s->id, kNoteSubParts + s->id) ) {
			printf("browser: no tool panel for window %u\n", s->id - N_SUB);
		}
		sub_follow(*s);
		tool_set_url(s->tool, s->url);
	}
	sub_title(*s);
	sub_show(*s);
}

/* The windows the pages opened, gone at once with the browser's (from the window's thread) */
void subs_delete_windows()
{
	std::lock_guard<std::mutex> hold(g_subs_lock);
	for ( SubWin *s : g_subs ) {
		if ( s->tool.kw > 0 ) (void)ob_del_obj(&s->tool.win);
		(void)ob_del_obj(&s->win);
	}
}

/* The window goes, and its page with it */
void sub_close( SubWin *s, const char *why, bool by_page = true )
{
	printf("browser: window %u closed (%s)\n", s->id - N_SUB, why);
	if ( s->eng != nullptr && ime_target() == s->eng ) {
		/* what was being composed there goes in before the page goes */
		ime_commit();
		g_ime_eng = nullptr;
	}
	if ( s->eng != nullptr ) {
#ifdef BR_BLINK
		if ( br_multi_process && br_proc_pid(s->eng) > 0 ) {
			br_proc_close(s->eng);
		} else {
			s->eng->closed();
		}
#else
		s->eng->closed();
#endif
		g_gone.push_back(s->eng);
	} else if ( !by_page ) {
		/* a video's window the user closed: the page is told */
		pip_closed(s->owner, s->session);
	}
	tool_close(s->tool);
	if ( s->kw > 0 ) ob_cls_obj(s->kw);
	(void)ob_del_obj(&s->win);
	{
		std::lock_guard<std::mutex> hold(g_subs_lock);
		g_subs.erase(std::remove(g_subs.begin(), g_subs.end(), s), g_subs.end());
	}
	delete s;
}

#ifdef BR_BLINK
/*
 * A run no longer shown, left as a document navigated away from is: its
 * entries are kept (run_keep, before), its page is closed, so that its
 * document is unloaded and its players, recorders and camera streams end
 * with it, and the video windows of its players close. Its renderer ends
 * and gives its memory back unless a window a page opened shows a page
 * of it (that one ends when the last of them closes, sub_close).
 */
void run_leave( Run &run )
{
	BrEngine *eng = run.eng;
	if ( eng == nullptr ) return;
	bool subs = run_has_subs(run);
	run.eng = nullptr;
	std::vector<SubWin *> pips;
	for ( SubWin *w : g_subs ) {
		if ( w->eng == nullptr && w->owner == eng ) pips.push_back(w);
	}
	for ( SubWin *w : pips ) sub_close(w, "its page left");
	int pid = br_proc_pid(eng);
	long free0 = br_sys_free_mb(), t0 = now_ms();
	br_proc_close(eng);
	long free1 = br_sys_free_mb();
	if ( subs ) {
		printf("browser: the page of %s left: its renderer %d stays for the windows its pages opened\n",
		       run.site.c_str(), pid);
		proc_log("left pid=" + std::to_string(pid) + " kept for its windows");
		return;
	}
	printf("browser: the page of %s left: renderer %d ended in %ld ms, %ld MB free before, %ld MB after\n",
	       run.site.c_str(), pid, now_ms() - t0, free0, free1);
	proc_log("left pid=" + std::to_string(pid) + " freed=" + std::to_string(free1 - free0));
}
#endif

/*
 * Starting a browser of its own takes the system seconds: it reads the
 * program, some hundreds of megabytes, into a new space. That is done on
 * a thread of its own, so that the engine's thread goes on with the page
 * meanwhile. g_apart_starting counts the starts not over yet; the
 * process does not end while one is (apart_wait).
 */
struct ApartStart {
	TS_UUID		prog;
	std::string	to, arg;
	long		since;
	bool		stores;		/* the stores' writing waited for first */
	unsigned	ticket;		/* br_store_wait's */
};

void *apart_thread( void *a )
{
	ApartStart	*s = (ApartStart *)a;
	TS_UUID		pu;
	T_OBCRE		c;

#ifdef BR_BLINK
	/* what this one keeps written, so that the new browser reads it: done by the stores' thread */
	if ( s->stores ) {
		long w0 = now_ms();
		int done = br_store_wait(s->ticket, 10000);
		printf("browser: the stores written for the browser of its own in %ld ms%s\n", now_ms() - w0,
		       done ? "" : " (not all: it starts all the same)");
	}
#endif
	memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = s->prog;
	c.arg = s->arg.c_str();
	c.argsz = (SZ)s->arg.size() + 1;
	ER er = ob_cre_obj(&c, &pu);
	printf("browser: %s opened in a browser of its own, %ld ms after it was asked%s\n", s->to.c_str(),
	       now_ms() - s->since, er >= E_OK ? "" : " (it did not start)");
	delete s;
	g_apart_starting--;
	return nullptr;
}

/* The browsers being started apart, started before the process ends (at most 30 s) */
void apart_wait()
{
	for ( int i = 0; i < 3000 && g_apart_starting > 0; i++ ) usleep(10000);
}

/*
 * An address opened with no opener: a browser of its own, the window
 * in a process of its own. What this one keeps
 * (cookies, storage) is written first, so that the new browser starts
 * with it: the engine's thread only hands the changes over
 * (br_store_flush_async), the stores' own thread writes them, and the
 * thread that starts the new browser waits for that, not this one.
 */
void open_apart( const std::string &to )
{
	ApartStart	*s = new ApartStart;
	pthread_t	th;
	pthread_attr_t	at;

	if ( !parse_uuid(SYSDEF_PROG_BROWSER, &s->prog) ) {
		delete s;
		return;
	}
	s->since = now_ms();
	s->stores = false;
	s->ticket = 0;
#ifdef BR_BLINK
	if ( br_multi_process ) {
		/* the page's renderer keeps the cookies and the storage: written before the new browser starts */
		long w0 = now_ms();
		bool done = br_proc_flush(g_br.eng, 10000);
		printf("browser: the renderer wrote what it keeps for the browser of its own in %ld ms%s\n", now_ms() - w0,
		       done ? "" : " (not all: it starts all the same)");
	} else {
		s->ticket = br_store_flush_async();
		s->stores = true;
	}
#endif
	s->to = to;
	s->arg = to + "\n";
	for ( const std::string &l : g_apart_lines ) s->arg += l + "\n";
	g_apart_starting++;
	pthread_attr_init(&at);
	pthread_attr_setstacksize(&at, 64 * 1024);
	pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	int rc = pthread_create(&th, &at, apart_thread, s);
	pthread_attr_destroy(&at);
	if ( rc != 0 ) {
		/* no thread to be had: started here, the engine's thread waiting */
		(void)apart_thread(s);
	}
}

/*
 * Each time round the engine's loop: the windows the pages opened, the
 * addresses to open apart, and each window's page -- what it closed,
 * where it goes, its frames
 */
void subs_tick()
{
	BrPipAsk a;
	while ( pip_next(&a) ) {
		SubWin *v = nullptr;
		for ( SubWin *s : g_subs ) {
			if ( s->eng == nullptr && s->session == a.session ) v = s;
		}
		if ( a.what == BrPipAsk::kOpen ) {
			BrOpened o;
			o.w = a.w;
			o.h = a.h;
			o.pip = true;
			sub_open(o, a.player, a.session);
		} else if ( v != nullptr && a.what == BrPipAsk::kUpdate ) {
			v->player = a.player;
			sub_show(*v);
		} else if ( v != nullptr ) {
			sub_close(v, "the page left picture-in-picture");
		}
	}
	std::vector<BrEngine *> engines(1, g_br.eng);
	for ( SubWin *s : g_subs ) {
		if ( s->eng != nullptr ) engines.push_back(s->eng);
	}
	for ( BrEngine *e : engines ) {
		BrOpened o;
		while ( e->opened(&o) ) sub_open(o);
		for ( std::string to = e->apart(); !to.empty(); to = e->apart() ) open_apart(to);
	}
	for ( size_t i = 0; i < g_subs.size(); ) {
		SubWin *s = g_subs[i];
		if ( s->eng == nullptr ) {
			/* a video's window: drawn again when the player shows another frame */
			if ( player_frames(s->owner, s->player) != s->frames ) sub_show(*s);
			i++;
			continue;
		}
		if ( s->eng->close_asked() ) {
			sub_close(s, s->pip ? "its opener's page went, or its script closed it" : "its script closed it");
			continue;
		}
		std::string to = s->eng->navigation();
		if ( !to.empty() ) {
			std::string method, ctype, body;
			(void)s->eng->navigation_body(&method, &ctype, &body);
			sub_go(*s, to, method, ctype, body);
		}
		if ( s->moved ) {
			s->moved = false;
			s->eng->mouse(BR_M_MOVE, s->move_x, s->move_y, 0, s->move_mods);
		}
		if ( s->eng->pump() ) {
			sub_show(*s);
			s->eng->frame_shown();
		}
		sub_title(*s);
		sub_follow(*s);
		i++;
	}
}

/* How long the loop may sleep: the soonest any window's page wants its next frame */
int subs_idle_ms( int ms )
{
	for ( SubWin *s : g_subs ) ms = std::min(ms, s->eng != nullptr ? s->eng->idle_ms() : 10);
	return ms;
}

/* A notice of a window a page opened */
void sub_event( const T_OBNTM &m )
{
	SubWin *s = sub_of(m.id);
	if ( s == nullptr ) return;
	if ( ( m.id & ~kNoteSubMask ) == kNoteSubParts ) {
		/* a switch of its panel */
		if ( m.event != OB_E_CHANGE || s->eng == nullptr ) return;
		switch ( m.recno - OB_WR_KIND ) {
		case T_BACK:	sub_history(*s, -1); break;
		case T_FWD:	sub_history(*s, 1); break;
		case T_RELOAD:	(void)sub_go(*s, s->url, std::string(), std::string(), std::string(), BR_LOAD_RELOAD); break;
		default:	break;
		}
		return;
	}
	if ( ( m.id & ~kNoteSubMask ) == kNoteSubTool ) {
		/* its panel's window: its field, its drawing; the panel alone goes */
		if ( m.event == OB_E_PRESS && m.code == 0 ) tool_press(s->tool, m.x, m.y);
		else if ( m.event == OB_E_REDRAW ) tool_redraw(s->tool);
		else if ( m.event == OB_E_CLOSE ) tool_close(s->tool);
		else if ( m.event == OB_E_KEY && s->tool.focus ) tool_key(s->tool, m.code, m.mods);
		return;
	}
	if ( s->eng == nullptr ) {
		/* a video's window: closed, drawn again, or pressed to play or pause */
		if ( m.event == OB_E_CLOSE || m.event == OB_E_DELETE
		     || ( m.event == OB_E_KEY && ( m.mods & kCtrl ) != 0 && m.code == K_E ) ) {
			sub_close(s, "閉じる", false);
		} else if ( m.event == OB_E_REDRAW ) {
			(void)sub_size(*s);
			sub_show(*s);
		} else if ( m.event == OB_E_PRESS && m.code == 0 ) {
			pip_press(s->owner, s->session, player_paused(s->owner, s->player));
		}
		return;
	}
	switch ( m.event ) {
	case OB_E_CLOSE:
	case OB_E_DELETE:
		sub_close(s, "閉じる");
		return;
	case OB_E_REDRAW:
		(void)sub_size(*s);
		s->full = true;
		sub_show(*s);
		return;
	case OB_E_PRESS:
		if ( m.x < 0 || m.y < 0 || m.x >= s->w || m.y >= s->h ) return;
		if ( s->tool.focus ) {
			/* a press on the page ends the address's editing */
			s->tool.focus = false;
			s->tool.sel_all = false;
			tool_redraw(s->tool);
		}
		if ( ime_press(s->eng, m.x, m.y) ) break;
		if ( ime_target() == s->eng ) ime_commit();
		s->eng->mouse(BR_M_DOWN, m.x, m.y, m.code == 1 ? 2 : 0, m.mods);
		break;
	case OB_E_RELEASE:
		s->eng->mouse(BR_M_UP, m.x, m.y, m.code == 1 ? 2 : 0, m.mods);
		break;
	case OB_E_MOVE:
		s->moved = true;
		s->move_x = m.x;
		s->move_y = m.y;
		s->move_mods = m.mods;
		return;
	case OB_E_KEY:
		if ( ( m.mods & kCtrl ) != 0 && m.code == K_E ) {
			sub_close(s, "Ctrl+E");
			return;
		}
		if ( ( m.mods & kCtrl ) != 0 && m.code == K_L && s->tool.kw > 0 ) {
			ime_commit();
			tool_focus(s->tool);
			return;
		}
		if ( ( m.mods & kAlt ) != 0 && ( m.code == K_LEFT || m.code == K_RIGHT ) ) {
			sub_history(*s, m.code == K_LEFT ? -1 : 1);
			return;
		}
		if ( s->tool.focus ) {
			tool_key(s->tool, m.code, m.mods);
			return;
		}
		/* a field of its page: the converter first, as in the page's window */
		if ( s->eng->editable() && ime_key_for(s->eng, m.code, m.mods, m.imode) ) break;
		(void)s->eng->key(m.code, m.mods, key_letter(m.code, m.mods));
		break;
#ifdef OB_E_WHEEL
	case OB_E_WHEEL:
		wheel_to(s->eng, m);
		break;
#endif
	default:
		return;
	}
	sub_show(*s);
}

struct Script {
	bool		has_obj = false;
	unsigned char	obj[16];
	std::string	url;
	bool		follow = false;
	bool		sethome = false;	/* 〈ホームページに設定〉 once the last page is shown */
	bool		mem = false;
	bool		trace = false;
	std::string	ready;
	long		quit = -1;
	std::string	sound;		/* where the frames Web Audio played are counted */
	std::string	title;		/* where the title of the page shown last is written at the end */
	std::string	addr;		/* where the addresses the field showed are written at the end */
	std::vector<std::string> then;	/* addresses gone to after the first, in turn */
	std::string	proto;		/* where the responses are counted by protocol */
	std::string	print;		/* where what 印刷 came to is written at the end */
	std::vector<std::string> menu;	/* menu items chosen once the first page is shown, in turn */
	std::vector<std::string> later;	/* the same after "ready=" is written: one that puts up a panel, for the test to answer */
	std::vector<std::string> scripts;	/* scripts run in the page once it is shown */
	std::vector<std::string> apart;	/* lines given to the browsers windows with no opener start */
	std::string	lines;		/* the lines after the object or the first address, as they came */
	std::vector<std::string> extadd;	/* extensions a test adds */
	std::string	procs;		/* where what the renderers did is written at the end */
};

/*
 * The start-up argument: the page object's UUID (16 bytes) as the
 * desktop gives it, then, for tests, a newline and lines of text; or the
 * text alone, its first line an address.
 */
Script read_script()
{
	Script	s;
	SZ	n = ts_get_arg(nullptr, 0);

	if ( n <= 0 || n > 4096 ) return s;
	std::string a((size_t)n, '\0');
	(void)ts_get_arg(&a[0], n);
	bool first = true;
#ifdef BR_BLINK
	/* a renderer: its first line says so, the browser's own lines follow (with no object first) */
	if ( a.compare(0, sizeof(BR_RENDERER_LINE), BR_RENDERER_LINE "\n") == 0 ) {
		br_is_renderer = true;
		a = a.substr(sizeof(BR_RENDERER_LINE));
		first = false;
	} else if ( a.compare(0, sizeof(BR_NETWORK_LINE), BR_NETWORK_LINE "\n") == 0 ) {
		/* the network process: the browser's lines follow, as a renderer's do */
		br_is_network = true;
		a = a.substr(sizeof(BR_NETWORK_LINE));
		first = false;
	} else if ( a.compare(0, sizeof(BR_GPU_LINE), BR_GPU_LINE "\n") == 0 ) {
		/* the GPU process: the browser's lines follow, as a renderer's do */
		br_is_gpu = true;
		a = a.substr(sizeof(BR_GPU_LINE));
		first = false;
	} else
#endif
	if ( n >= 16 && ( n == 16 || a[16] == '\n' ) ) {
		s.has_obj = true;
		memcpy(s.obj, a.data(), 16);
		a = ( n > 17 ) ? a.substr(17) : std::string();
		first = false;
	}
	a.resize(strnlen(a.c_str(), a.size()));
	/* what the browser gives its renderers: its own lines, with no object and no first address */
	s.lines = a;
	if ( !first ) {
		/* the object's lines begin after its UUID */
	} else {
		size_t e = a.find('\n');
		s.lines = ( e == std::string::npos ) ? std::string() : a.substr(e + 1);
	}
	size_t p = 0;
	while ( p < a.size() ) {
		size_t e = a.find('\n', p);
		std::string line = a.substr(p, ( e == std::string::npos ) ? std::string::npos : e - p);
		if ( first ) {
			if ( line.compare(0, 4, "http") == 0 || line.compare(0, 6, "about:") == 0 ) s.url = line;
			first = false;
		} else if ( line == "follow" ) {
			s.follow = true;
		} else if ( line.compare(0, 7, "extadd=") == 0 ) {
			s.extadd.push_back(line.substr(7));
		} else if ( line == "sethome" ) {
			s.sethome = true;
		} else if ( line.compare(0, 6, "title=") == 0 ) {
			s.title = line.substr(6);
		} else if ( line.compare(0, 5, "addr=") == 0 ) {
			s.addr = line.substr(5);
		} else if ( line == "mem" ) {
			s.mem = true;
		} else if ( line == "trace" ) {
			s.trace = true;
		} else if ( line.compare(0, 6, "ready=") == 0 ) {
			s.ready = line.substr(6);
		} else if ( line.compare(0, 5, "quit=") == 0 ) {
			s.quit = atol(line.c_str() + 5);
		} else if ( line.compare(0, 6, "sound=") == 0 ) {
			s.sound = line.substr(6);
		} else if ( line.compare(0, 13, "periodicsync=") == 0 ) {
			br_periodic_sync_ms = atoll(line.c_str() + 13);
		} else if ( line.compare(0, 7, "netlog=") == 0 ) {
			br_netlog = line.substr(7);
		} else if ( line.compare(0, 5, "push=") == 0 ) {
			br_push_server = line.substr(5);
		} else if ( line.compare(0, 8, "pushkey=") == 0 ) {
			br_push_test_key = line.substr(8);
		} else if ( line.compare(0, 6, "apart=") == 0 ) {
			s.apart.push_back(line.substr(6));
		} else if ( line.compare(0, 5, "then=") == 0 ) {
			s.then.push_back(line.substr(5));
		} else if ( line.compare(0, 6, "proto=") == 0 ) {
			s.proto = line.substr(6);
		} else if ( line.compare(0, 6, "print=") == 0 ) {
			s.print = line.substr(6);
		} else if ( line.compare(0, 6, "later=") == 0 ) {
			s.later.push_back(line.substr(6));
		} else if ( line.compare(0, 5, "menu=") == 0 ) {
			s.menu.push_back(line.substr(5));
#ifdef BR_BLINK
		} else if ( line.compare(0, 8, "hstsadd=") == 0 ) {
			g_step_host = line.substr(8);
			s.menu.push_back("hstsadd");
		} else if ( line.compare(0, 8, "setstep=") == 0 ) {
			br_settings_step(line.substr(8));
#endif
		} else if ( line.compare(0, 7, "script=") == 0 ) {
			s.scripts.push_back(line.substr(7));
		} else if ( line == "stat" ) {
			g_stat_on = true;
		} else if ( line == "draw=copy" ) {
			g_copy = true;
		} else if ( line.compare(0, 6, "check=") == 0 ) {
			g_check = line.substr(6);
		} else if ( line == "gpu=on" ) {
			g_gpu_mode = 1;
		} else if ( line == "gpuproc=on" ) {
			g_gpuproc = true;
		} else if ( line == "gpuproc=off" ) {
			g_gpuproc = false;
		} else if ( line == "gsurf=off" ) {
			g_gsurf = false;
		} else if ( line == "gsurf=on" ) {
			g_gsurf = true;
		} else if ( line == "gpu=off" ) {
			g_gpu_mode = -1;
		} else if ( line.compare(0, 9, "gpucheck=") == 0 ) {
			g_gpucheck = line.substr(9);
		} else if ( line.compare(0, 8, "gpufake=") == 0 ) {
			g_gpufake = line.substr(8);
			g_gpu_mode = 1;
		} else if ( line.compare(0, 5, "gles=") == 0 ) {
			/* the highest OpenGL ES version Mesa's contexts give (MESA_GLES_VERSION_OVERRIDE) */
			setenv("MESA_GLES_VERSION_OVERRIDE", line.c_str() + 5, 1);
#ifdef BR_BLINK
		} else if ( line == "forget" ) {
			br_net_keep_servers = 0;
		} else if ( line == "newboot" ) {
			br_net_new_boot = 1;
		} else if ( line == "dns=system" ) {
			br_net_dns_https = 0;
		} else if ( line == "quic=off" ) {
			br_net_quic = 0;
		} else if ( line == "process=single" ) {
			br_multi_process = false;
		} else if ( line.compare(0, 8, "killnav=") == 0 ) {
			br_proc_test_killnav = atoi(line.c_str() + 8);
		} else if ( line == "sandbox=off" ) {
			br_sandbox_mode = 0;
		} else if ( line == "sandbox=refuse" ) {
			br_sandbox_mode = 1;
		} else if ( line == "sandbox=kill" ) {
			br_sandbox_mode = 2;
		} else if ( line == "sandbox_renderer=noobj" ) {
			br_sandbox_noobj = true;
		} else if ( line == "sandbox_renderer=store" ) {
			br_sandbox_noobj = false;
		} else if ( line.compare(0, 6, "procs=") == 0 ) {
			s.procs = line.substr(6);
		} else if ( line.compare(0, 10, "renderers=") == 0 ) {
			br_proc_set_limit(atoi(line.c_str() + 10));
		} else if ( line.compare(0, 3, "v8=") == 0 ) {
			br_v8_flags = line.substr(3);
		} else if ( line.compare(0, 9, "isolated=") == 0 ) {
			br_isolated_allow_now(line.substr(9));
		} else if ( line.compare(0, 7, "wvrepo=") == 0 ) {
			br_wv_set_repo(line.substr(7));
		} else if ( line.compare(0, 9, "features=") == 0 ) {
			if ( !br_features.empty() ) br_features += ",";
			br_features += line.substr(9);
		} else if ( line.compare(0, 7, "secure=") == 0 ) {
			if ( !br_secure_origins.empty() ) br_secure_origins += ",";
			br_secure_origins += line.substr(7);
		} else if ( line.compare(0, 6, "trace=") == 0 ) {
			if ( !br_trace.empty() ) br_trace += ",";
			br_trace += line.substr(6);
#endif
		}
		if ( e == std::string::npos ) break;
		p = e + 1;
	}
#ifdef BR_BLINK
	/* extensions run where the pages are: in each renderer, or in this process with one process */
	if ( br_is_renderer || !br_multi_process ) {
		for ( const std::string &x : s.extadd ) br_ext_add_later(x);
	} else {
		/* the renderers read and link them through the network process, told of them by the browser */
		for ( const std::string &x : s.extadd ) br_proc_ext_allow(x);
	}
#else
	for ( const std::string &x : s.extadd ) br_ext_add_later(x);
#endif
	return s;
}

/* Whether a window says it shows the object already; that window brought to the front */
bool already_shown( const BrObject &o )
{
	TS_UUID		*list = nullptr;
	std::string	want = "\"shows\":\"" + br_uuid_str(o.id) + "\"";
	bool		yes = false;
	/* every window there is, a page at a time (ui_lst_obj): more than a page are up with many panels */
	INT		cnt = ui_lst_obj(OB_T_WINDOW, OB_S_WINDOW, &list);

	if ( cnt < 0 ) return false;
	for ( INT i = 0; i < cnt && !yes; i++ ) {
		static char	j[1024];
		SZ		asz = 0;
		ID		k = ob_opn_obj(&list[i], OB_OP_ATRRD);

		if ( k <= 0 ) continue;
		if ( ob_get_atr(k, (UB *)j, sizeof(j) - 1, &asz) >= E_OK ) {
			j[asz] = 0;
			yes = strstr(j, want.c_str()) != nullptr;
		}
		ob_cls_obj(k);
		if ( yes ) {
			/* nearest the viewer, when the window lets this process move it */
			T_OBWPOS	wp;
			ID		kw = ob_opn_obj(&list[i], OB_OP_READ | OB_OP_WRITE);

			if ( kw > 0 ) {
				if ( ob_rea_rec(kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
					wp.z = 0;
					(void)ob_wri_rec(kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
				}
				ob_cls_obj(kw);
			}
		}
	}
	free(list);
	return yes;
}

/* ---------------------------------------------------------------- the page's questions */

/*
 * The panels of the browser's data box (SYSDEF_BOX_BROWSER) the page's
 * questions are asked with: alert, confirm and prompt, and the choice of
 * a <select>. Their words are parts 1 to DLG_LINES, prompt's field part
 * 11, the list part 11, the buttons 101 (取消) and 102 (了解).
 */
/* When a test's "quit=" ends the run: a panel up then is put away unanswered */
long	g_quit_at = -1;

constexpr INT	PN_ALERT = 1, PN_CONFIRM = 2, PN_PROMPT = 3, PN_SELECT = 4, PN_PERMIT = 5, PN_FILES = 6;
constexpr INT	PN_SHARE = 20, DLG_SHARE_SOUND = 12;	/* what to share; its switch for the sound */
constexpr INT	PN_EXT = 21;				/* what an extension declares, its list part DLG_FIELD */
constexpr INT	DLG_FIELD = 11, DLG_CANCEL = 101, DLG_OK = 102;
constexpr int	DLG_LINES = 5, DLG_PROMPT_LINES = 4;
constexpr int	DLG_COLS = 52;			/* the width of a line, in half-width letters */

/* The letters of text from a to b, but for the control characters other than the tab */
static std::string dlg_clean( const std::string &text, size_t a, size_t b )
{
	std::string	l;

	for ( size_t i = a; i < b; i++ ) {
		unsigned char c = (unsigned char)text[i];
		if ( c >= 0x20 || c == '\t' ) l.push_back((char)c);
	}
	return l;
}

/*
 * The text cut into lines of at most DLG_COLS half-width letters (a wide
 * letter two), at most n: at its line feeds, and where a line is full as
 * 禁則 lets it (kin_break, <ts/kinsoku.h>), the spaces after the break
 * passed over
 */
std::vector<std::string> dlg_lines( const std::string &text, int n )
{
	std::vector<std::string>	out;
	const UB			*s = (const UB *)text.c_str();
	size_t				ls = 0, i = 0;
	int				cols = 0;

	while ( i < text.size() ) {
		unsigned char c = (unsigned char)text[i];
		size_t len = ( c < 0x80 ) ? 1 : ( c < 0xE0 ) ? 2 : ( c < 0xF0 ) ? 3 : 4;
		if ( i + len > text.size() ) len = text.size() - i;
		int w = ( c < 0x80 ) ? 1 : 2;
		if ( c == '\n' ) {
			out.push_back(dlg_clean(text, ls, i));
			ls = ++i;
			cols = 0;
			continue;
		}
		if ( c < 0x20 && c != '\t' ) w = 0;
		if ( cols + w > DLG_COLS && i > ls ) {
			size_t b = ls + (size_t)kin_break(s + ls, (INT)( i - ls ));
			out.push_back(dlg_clean(text, ls, b));
			while ( b < text.size() && text[b] == ' ' ) b++;
			ls = i = b;
			cols = 0;
			continue;
		}
		cols += w;
		i += len;
	}
	if ( ls < text.size() || out.empty() ) out.push_back(dlg_clean(text, ls, text.size()));
	if ( (int)out.size() > n ) {
		out.resize((size_t)n);
		out.back() += "…";
	}
	return out;
}

/* What else comes to a panel's channel, when nothing is done with it */
void no_other( UI *, const T_OBNTM & ) {}

/* The names of a panel's list or pop-up; not taken, it is said and the part is left empty */
void dlg_names( UI *q, INT num, const std::vector<const char *> &p )
{
	ER er = ui_names(q, num, p.data(), (INT)p.size());
	if ( er < E_OK ) {
		printf("browser: panel part %d: %d names not shown (%d)\n", (int)num, (int)p.size(), (int)er);
		(void)ui_names(q, num, nullptr, 0);
	}
}

/*
 * A panel of the data box worked over the page's window until it is
 * answered: the button pushed (DLG_OK, DLG_CANCEL), 0 for Esc or the panel
 * closed. 'fill' sets its parts first, 'read' reads them before it goes,
 * and what else comes to the panel (links dropped on it) is given to
 * 'other'. The page's thread waits here, as a browser's does while a
 * question of the page is up; the window's own notices wait on its
 * channel.
 */
template <typename Fill, typename Read, typename Other = decltype(&no_other)>
INT dlg_work( INT num, Fill fill, Read read, Other other = &no_other )
{
	UI	u, q;
	T_OBCRE	c;
	TS_UUID	ch;
	T_OBNTM	m;
	INT	n = 0, ev, answer = -1;

	memset(&u, 0, sizeof(u));
	if ( ui_uuid(SYSDEF_BOX_BROWSER, &u.box) < E_OK ) return 0;
	u.kw = g_br.kw;
	u.w = g_br.w;
	u.h = view_h();
	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( ob_cre_obj(&c, &ch) < E_OK ) return 0;
	u.port = ob_opn_obj(&ch, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT);
	if ( u.port <= 0 ) {
		(void)ob_del_obj(&ch);
		return 0;
	}
	if ( ui_dlg_open(&u, num, &q) < E_OK ) {
		printf("browser: the panel %d of the data box cannot be made\n", (int)num);
		ob_cls_obj(u.port);
		(void)ob_del_obj(&ch);
		return 0;
	}
	fill(&q);
	{
		/* what is dropped on the panel's window comes to the same channel */
		T_OBNTF req;
		memset(&req, 0, sizeof(req));
		req.events = OB_E_DROP;
		req.id = 4;
		(void)ob_ntf_evt(q.kw, OB_REC_ANY, &req, u.port);
	}
	while ( answer < 0 && !g_closing && ( g_quit_at < 0 || now_ms() < g_quit_at ) ) {
		ev = ui_dlg_next(&u, &q, &m, &n);
		if ( ev == UI_EV_BUTTON ) {
			answer = n;
		} else if ( ev == UI_EV_CLOSE ) {
			answer = 0;
		} else if ( ev == UI_EV_OTHER ) {
			other(&q, m);
		} else if ( ev == UI_EV_NONE ) {
			tk_dly_tsk(20);
		}
	}
	if ( answer < 0 ) answer = 0;
	read(&q, answer);
	ui_dlg_close(&q);
	ob_cls_obj(u.port);
	(void)ob_del_obj(&ch);
	/* the page's window is drawn again where the panel was */
	g_br.full = true;
	return answer;
}

void dlg_set_lines( UI *q, const std::vector<std::string> &l, int n )
{
	for ( int i = 0; i < n; i++ ) {
		ui_set_text(q, i + 1, i < (int)l.size() && !l[i].empty() ? l[i].c_str() : " ");
	}
}

#ifdef BR_BLINK
/* ---------------------------------------------------------------- Widevine (port/media/br_widevine.cc) */

/* The panel that says what Widevine is and where it comes from: 取消 (101), 導入 (102) */
constexpr INT	PN_WIDEVINE = 7;

/* The installing thread has ended: the engine's thread takes it over (wvdone) */
void wv_ended()
{
	std::lock_guard<std::mutex> g(g_ev_lock);
	g_commands.push_back("wvdone");
	loop_wake();
}

/*
 * Widevine is installed by the engine's side, where the media are: this
 * process's, or the window's page's renderer (br_sys_command,
 * br_sys_query), whose end of the installing comes back as "wvdone".
 * The panels are the browser's.
 */
void wv_state( bool *busy, bool *installed )
{
	if ( br_multi_process ) {
		br_proc_wv_state(g_br.eng, busy, installed);
		return;
	}
	*busy = br_wv_busy();
	*installed = br_wv_installed();
}

/* What the engine's side answered: "1" or "0", a newline, its words */
bool wv_said( const std::string &a, std::string *msg )
{
	size_t nl = a.find('\n');
	*msg = ( nl == std::string::npos ) ? std::string() : a.substr(nl + 1);
	return a.compare(0, 1, "1") == 0;
}

/* 〈Widevine を導入〉: the panel first; the package is fetched only when 導入 is chosen on it */
void wv_install_ask()
{
	bool busy = false, installed = false;
	wv_state(&busy, &installed);
	if ( busy ) return;
	/* texts 3 and 4: where the CDM is fetched from for this processor */
	std::string from1, from2;
	if ( br_multi_process ) {
		(void)wv_said("1\n" + br_proc_query(g_br.eng, "wvsource"), &from1);
		size_t nl = from1.find('\n');
		if ( nl != std::string::npos ) {
			from2 = from1.substr(nl + 1);
			from1.resize(nl);
		}
	} else {
		br_wv_source(&from1, &from2);
	}
	/* the panel on the window's thread; 導入 comes back as "wvgo" */
	panel_on_window([from1, from2] {
		INT a = dlg_work(PN_WIDEVINE, [&]( UI *q ) {
			ui_set_text(q, 3, from1.c_str());
			ui_set_text(q, 4, from2.c_str());
		}, []( UI *, INT ) {});
		printf("browser: widevine: the panel was answered %s\n", a == DLG_OK ? "導入" : "取消");
		if ( a == DLG_OK ) command_back("wvgo");
	});
}

/* 導入 chosen on Widevine's panel: the package fetched */
void wv_install_go()
{
	if ( br_multi_process ? br_proc_command(g_br.eng, "wvinstall") == "1" : br_wv_install_start(wv_ended) ) {
		g_br.status = "Widevineを取ってきています…";
		show();
	}
}

void wv_install_done()
{
	std::string msg;
	bool ok;
	if ( br_multi_process ) {
		ok = wv_said(br_proc_command(g_br.eng, "wvfinish"), &msg);
	} else {
		ok = br_wv_finish(&msg);
		if ( ok ) br_eme_widevine_changed();
	}
	g_br.status = msg;
	show();
	if ( !ok ) {
		std::vector<std::string> l = dlg_lines("Widevineを導入できませんでした。\n" + msg, DLG_LINES);
		panel_on_window([l] { (void)dlg_work(PN_ALERT, [&]( UI *q ) { dlg_set_lines(q, l, DLG_LINES); }, []( UI *, INT ) {}); });
	}
}

/* 〈Widevine を外す〉: asked first; the user's CDM object is deleted */
void wv_remove_ask()
{
	std::vector<std::string> l = dlg_lines("Widevine(GoogleのCDM)を外します。導入したCDMの実身は消え、"
					       "Widevineを求めるページは再生できなくなります。\nよろしいですか。", DLG_LINES);
	/* the question on the window's thread; 了解 comes back as "wvremovego" */
	panel_on_window([l] {
		INT a = dlg_work(PN_CONFIRM, [&]( UI *q ) { dlg_set_lines(q, l, DLG_LINES); }, []( UI *, INT ) {});
		printf("browser: widevine: removing it was answered %s\n", a == DLG_OK ? "了解" : "取消");
		if ( a == DLG_OK ) command_back("wvremovego");
	});
}

/* 了解 to taking Widevine away: the user's CDM object deleted */
void wv_remove_go()
{
	std::string msg;
	if ( br_multi_process ) {
		(void)wv_said(br_proc_command(g_br.eng, "wvremove"), &msg);
	} else if ( br_wv_remove(&msg) ) {
		br_eme_widevine_changed();
	}
	g_br.status = msg;
	show();
}
#endif

}  // namespace

std::string br_zone_name()
{
	char	name[DT_ZONE_MAX], b[16];
	INT	tz = 0, a;

	if ( dt_getsyszone(name, sizeof(name)) > 0 ) return name;
	(void)dt_getsystz(&tz);
	if ( tz == 0 ) return "UTC";
	a = ( tz < 0 ) ? -tz : tz;
	snprintf(b, sizeof(b), "GMT%c%02d:%02d", ( tz < 0 ) ? '-' : '+', (int)( a / 60 ), (int)( a % 60 ));
	return b;
}

void br_screen_size( int *w, int *h )
{
#ifdef BR_BLINK
	if ( br_is_renderer ) {
		br_rproc_screen(w, h);
		return;
	}
#endif
	int	x = 0, y = 0;

	br_screen_view(&x, &y, w, h);
}

void br_screen_view( int *x, int *y, int *w, int *h )
{
	T_OBWPOS	wp;
	T_DPRECT	on;
	SZ		asz = 0;

	on.left = on.top = on.right = on.bottom = 0;
	if ( g_br.kw > 0 && ob_rea_rec(g_br.kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK && wp.right > wp.left ) {
		wm_screen_of(( wp.left + wp.right ) / 2, ( wp.top + wp.bottom ) / 2, &on);
	} else {
		wm_screen_new_rect(&on);
	}
	if ( on.right <= on.left || on.bottom <= on.top ) {
		/* a system that does not answer either: the view */
		INT sw = 0, sh = 0;
		wm_screen(&sw, &sh);
		on.left = on.top = 0;
		on.right = sw;
		on.bottom = sh;
	}
	*x = on.left;
	*y = on.top;
	*w = on.right - on.left;
	*h = on.bottom - on.top;
}

bool br_window_rect( int *x, int *y, int *w, int *h )
{
#ifdef BR_BLINK
	if ( br_is_renderer ) return br_rproc_window(x, y, w, h);
#endif
	T_OBWPOS	wp;
	SZ		asz = 0;
	int		sx = 0, sy = 0, sw = 0, sh = 0;

	if ( g_br.kw <= 0 ) return false;
	br_screen_view(&sx, &sy, &sw, &sh);
	if ( g_fs.on ) {
		/* over the whole screen the window is the screen */
		*x = 0;
		*y = 0;
		*w = sw;
		*h = sh;
		return sw > 0 && sh > 0;
	}
	if ( ob_rea_rec(g_br.kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) < E_OK ) return false;
	*x = wp.left - sx;
	*y = wp.top - sy;
	*w = wp.right - wp.left;
	*h = wp.bottom - wp.top;
	return *w > 0 && *h > 0;
}

bool br_ask_page( int kind, const std::string &message, const std::string &deflt, std::string *answer )
{
#ifdef BR_BLINK
	if ( br_is_renderer ) return br_rproc_ask_page(kind, message, deflt, answer);
#endif
	INT	num = ( kind == BR_ASK_CONFIRM ) ? PN_CONFIRM : ( kind == BR_ASK_PROMPT ) ? PN_PROMPT : PN_ALERT;
	int	nl = ( kind == BR_ASK_PROMPT ) ? DLG_PROMPT_LINES : DLG_LINES;
	std::vector<std::string> l = dlg_lines(message, nl);
	std::string got;

	printf("browser: the page asks (%s): %s\n",
	       kind == BR_ASK_CONFIRM ? "confirm" : kind == BR_ASK_PROMPT ? "prompt" : "alert", message.c_str());
	INT a = dlg_work(num,
		[&]( UI *q ) {
			dlg_set_lines(q, l, nl);
			if ( kind == BR_ASK_PROMPT ) ui_set_text(q, DLG_FIELD, deflt.c_str());
		},
		[&]( UI *q, INT ans ) {
			if ( kind == BR_ASK_PROMPT && ans == DLG_OK ) {
				char buf[1024];
				INT k = ui_text(q, DLG_FIELD, buf, (INT)sizeof(buf) - 1);
				buf[( k > 0 && k < (INT)sizeof(buf) ) ? k : 0] = 0;
				got = buf;
			}
		});
	printf("browser: the page was answered %s\n", a == DLG_OK ? "了解" : "取消");
	if ( answer != nullptr ) *answer = got;
	return a == DLG_OK;
}

/* A colour the user picks on the page (EyeDropper), from a service's thread: false when given up */
bool br_pick_color( uint32_t *color )
{
#ifdef BR_BLINK
	if ( br_is_renderer ) return br_rproc_pick_color(color);
#endif
	std::unique_lock<std::mutex> hold(g_pick_lock);
	if ( g_pick_wanted ) return false;	/* one at a time */
	g_pick_wanted = true;
	g_pick_asked = g_pick_done = false;
	loop_wake();
	printf("browser: eyedropper: a colour is asked for\n");
	g_pick_cv.wait(hold, [] { return g_pick_done; });
	g_pick_wanted = false;
	*color = g_pick_color;
	return g_pick_ok;
}

bool br_ask_permission( const std::string &origin, const std::string &what )
{
#ifdef BR_BLINK
	if ( br_is_renderer ) return br_rproc_ask_permission(origin, what);
#endif
	static std::mutex	one;		/* one panel at a time */
	std::lock_guard<std::mutex> hold(one);
	std::vector<std::string> l = dlg_lines(origin + "が" + what + "を求めています。", 3);

	l.push_back("許可しますか。");
	printf("browser: %s asks for %s\n", origin.c_str(), what.c_str());
	INT a = dlg_work(PN_PERMIT, [&]( UI *q ) { dlg_set_lines(q, l, 4); }, []( UI *, INT ) {});
	printf("browser: %s %s\n", what.c_str(), a == DLG_OK ? "given" : "refused");
	return a == DLG_OK;
}

/*
 * What to share, on the panel of the data box: the whole screen and the
 * windows, nearest the viewer first, listed by what they are; the switch
 * takes the sound along. A window has the sound of its own process only;
 * a window of the system's tasks has none to give. The safe answer is the
 * panel's default.
 */
/* A screen as the screen object's record 1 says it (SCREENi lines, design 18.7) */
struct BrScreen {
	int		x = 0, y = 0, w = 0, h = 0;	/* its rectangle in the view */
	std::string	name;				/* the output that shows it, "" for none of its own */
	unsigned	ident = 0;			/* its display's ident from the EDID, 0 for none */
};

/*
 * The screens there are now, the main one first (10.4.8): read from the
 * screen object, as the screens' settings read them, so that the
 * browser's process asks the system no more than any program may. One,
 * the main screen, where the record lists none.
 */
std::vector<BrScreen> screens_now()
{
	std::vector<BrScreen> v;
	CONST TS_UUID u = OB_UUID_DISPLAY_INIT;
	std::string t;
	ID key = ob_opn_obj(&u, OB_OP_READ);
	if ( key > 0 ) {
		std::vector<char> b(16384);
		SZ asz = 0;
		if ( ob_rea_rec(key, OB_DSP_MODE, 0, b.data(), (SZ)b.size(), &asz) >= E_OK && asz > 0 ) {
			t.assign(b.data(), (size_t)std::min<SZ>(asz, (SZ)b.size()));
		}
		ob_cls_obj(key);
	}
	for ( size_t at = 0; at < t.size(); ) {
		size_t end = t.find('\n', at);
		if ( end == std::string::npos ) end = t.size();
		std::string line = t.substr(at, end - at);
		at = end + 1;
		if ( line.compare(0, 6, "SCREEN") != 0 || line.size() < 8 || line[6] < '0' || line[6] > '9' || line[7] != '\t' ) {
			continue;
		}
		BrScreen sc;
		char name[32] = "";
		unsigned ident = 0;
		int got = sscanf(line.c_str() + 8, "%d %d %dx%d %31s %x", &sc.x, &sc.y, &sc.w, &sc.h, name, &ident);
		if ( got < 4 || sc.w <= 0 || sc.h <= 0 ) continue;
		sc.name = name;
		sc.ident = ident;
		v.push_back(sc);
	}
	if ( v.empty() ) {
		T_DPRECT m;
		m.left = m.top = m.right = m.bottom = 0;
		wm_screen_main_rect(&m);
		BrScreen sc;
		sc.x = m.left;
		sc.y = m.top;
		sc.w = m.right - m.left;
		sc.h = m.bottom - m.top;
		if ( sc.w <= 0 || sc.h <= 0 ) {
			INT w = 0, h = 0;
			wm_screen(&w, &h);
			sc.w = w;
			sc.h = h;
		}
		v.push_back(sc);
	}
	return v;
}

bool br_share_screen_rect( const BrShareChoice &c, int *x, int *y, int *w, int *h )
{
#ifdef BR_BLINK
	if ( br_is_renderer ) return br_rproc_share_screen_rect(c, x, y, w, h);
#endif
	std::vector<BrScreen> v = screens_now();
	const BrScreen *found = nullptr;
	if ( !c.out_name.empty() ) {
		/* the screen of the same output, wherever it is now */
		for ( const BrScreen &sc : v ) {
			if ( sc.name == c.out_name && ( c.out_ident == 0 || sc.ident == 0 || sc.ident == c.out_ident ) ) {
				found = &sc;
				break;
			}
		}
	} else if ( c.screen >= 0 && c.screen < (int)v.size() && ( c.screen == 0 || v[(size_t)c.screen].name.empty() ) ) {
		/* the main screen, or the column of one output of that number */
		found = &v[(size_t)c.screen];
	}
	if ( found == nullptr ) return false;
	*x = found->x;
	*y = found->y;
	*w = found->w;
	*h = found->h;
	return true;
}

bool br_share_choose( const std::string &origin, const BrShareAsk &ask, BrShareChoice *out )
{
#ifdef BR_BLINK
	if ( br_is_renderer ) return br_rproc_share_choose(origin, ask, out);
#endif
	static std::mutex		one;		/* one panel at a time */
	std::lock_guard<std::mutex>	hold(one);
	std::vector<T_WMSHARE>		wins(64);
	std::vector<BrShareChoice>	items;
	std::vector<std::string>	names;
	std::vector<std::string>	l = dlg_lines(origin + "が画面の共有を求めています。", 1);
	int				got = -1, sound = 0, first = 0;
	INT				n;

	while ( ( n = wm_share_list(wins.data(), (INT)wins.size()) ) >= (INT)wins.size() ) wins.resize(wins.size() * 2);
	if ( !ask.no_screen ) {
		/*
		 * a row for each screen, the main one first, as 〈識別〉 numbers
		 * them; the one screen there is, the whole screen
		 */
		std::vector<BrScreen> scr = screens_now();
		for ( size_t i = 0; i < scr.size(); i++ ) {
			BrShareChoice c;
			c.kind = BR_SHARE_SCREEN;
			c.screen = (int)i;
			c.out_name = scr[i].name;
			c.out_ident = scr[i].ident;
			c.sx = scr[i].x;
			c.sy = scr[i].y;
			c.sw = scr[i].w;
			c.sh = scr[i].h;
			if ( scr.size() == 1 ) {
				c.name = "画面全体";
			} else {
				char t[96];
				snprintf(t, sizeof(t), "画面 %d（%s%s%d×%d）", (int)i + 1,
					 i == 0 ? "主画面" : scr[i].name.c_str(), i == 0 || !scr[i].name.empty() ? " " : "",
					 scr[i].w, scr[i].h);
				c.name = t;
			}
			c.audio = !ask.no_system_audio;
			c.mon_mode = ask.own_audio_out ? SND_MON_EXCEPT : SND_MON_ALL;
			c.mon_pid = 0;
			items.push_back(c);
			names.push_back(c.name);
		}
	}
	for ( INT i = 0; i < n; i++ ) {
		const T_WMSHARE &w = wins[(size_t)i];
		if ( ask.no_self && w.gid == g_br.gid ) continue;
		BrShareChoice c;
		c.kind = ( w.flags & WM_SHARE_SAMEPROG ) ? BR_SHARE_BROWSER : BR_SHARE_WINDOW;
		c.wid = w.wid;
		c.serial = w.serial;
		c.name = w.title[0] != 0 ? w.title : "(名前なし)";
		c.audio = w.pid > 0;
		c.mon_mode = SND_MON_ONLY;
		c.mon_pid = w.pid;
#ifdef BR_BLINK
		/*
		 * With renderers, what a window of this browser plays is played by
		 * the renderer of its page, not by the browser's process: its
		 * sound is that renderer's
		 */
		if ( br_multi_process && w.pid == ts_get_pid() && br_proc_pid(g_br.eng) > 0 ) {
			c.mon_pid = br_proc_pid(g_br.eng);
		}
#endif
		items.push_back(c);
		names.push_back(( c.kind == BR_SHARE_BROWSER ? "ブラウザ: " : "ウインドウ: " ) + c.name);
	}
	for ( size_t i = 0; i < items.size(); i++ ) {
		if ( items[i].kind == ask.prefer ) {
			first = (int)i;
			break;
		}
	}
	printf("browser: %s asks to share; %d to choose from\n", origin.c_str(), (int)items.size());
	INT a = dlg_work(PN_SHARE,
		[&]( UI *q ) {
			std::vector<const char *> p;
			for ( const std::string &s : names ) p.push_back(s.c_str());
			ui_set_text(q, 1, l[0].c_str());
			dlg_names(q, DLG_FIELD, p);
			ui_set_value(q, DLG_FIELD, items.empty() ? 0 : first + 1);
		},
		[&]( UI *q, INT ans ) {
			if ( ans == DLG_OK ) {
				got = ui_value(q, DLG_FIELD) - 1;
				sound = ui_value(q, DLG_SHARE_SOUND);
			}
		});
	if ( a != DLG_OK || got < 0 || got >= (int)items.size() ) return false;
	*out = items[(size_t)got];
	out->audio = out->audio && ask.audio && sound != 0;
	if ( out->mon_mode == SND_MON_EXCEPT ) {
		/* the whole sound but this browser's: with renderers, the page's renderer plays it */
		out->mon_pid = ts_get_pid();
#ifdef BR_BLINK
		if ( br_multi_process && br_proc_pid(g_br.eng) > 0 ) out->mon_pid = br_proc_pid(g_br.eng);
#endif
	}
	return true;
}

/* 〈設定〉's button: the panel is put up from the window's own loop, as the menu's is */
void br_ext_ask_load()
{
#ifdef BR_BLINK
	if ( br_is_renderer ) {
		br_rproc_ext_ask_load();
		return;
	}
#endif
	std::lock_guard<std::mutex> g(g_ev_lock);
	g_commands.push_back("extload");
	loop_wake();
}

/* What an extension asked for that is done on the engine's thread: a page, the tab sent somewhere, a panel */
void br_ext_command( const std::string &code )
{
#ifdef BR_BLINK
	if ( br_is_renderer ) {
		br_rproc_main_command(code);
		return;
	}
#endif
	std::lock_guard<std::mutex> g(g_ev_lock);
	g_commands.push_back(code);
	loop_wake();
}

void br_window_uuid( unsigned char id[16] )
{
#ifdef BR_BLINK
	if ( br_is_renderer ) {
		br_rproc_window_uuid(id);
		return;
	}
#endif
	memcpy(id, g_br.win.b, 16);
}

bool br_choose_objects( bool multiple, std::vector<BrObjLink> *out )
{
#ifdef BR_BLINK
	if ( br_is_renderer ) return br_rproc_choose_objects(multiple, out);
#endif
	std::vector<BrObjLink>	links = br_tray_links();
	std::vector<int>	picked;

	auto names = [&]( UI *q ) {
		std::vector<const char *> p;
		for ( const BrObjLink &l : links ) p.push_back(l.name.empty() ? "(名前なし)" : l.name.c_str());
		dlg_names(q, DLG_FIELD, p);
		ui_set_value(q, DLG_FIELD, links.empty() ? 0 : 1);
	};
	printf("browser: the page asks for %s; %d in the tray\n", multiple ? "files" : "a file", (int)links.size());
	INT a = dlg_work(PN_FILES, names,
		[&]( UI *q, INT ans ) {
			int v = ui_value(q, DLG_FIELD);
			if ( ans == DLG_OK && v >= 1 && v <= (int)links.size() ) picked.push_back(v - 1);
		},
		[&]( UI *q, const T_OBNTM &m ) {
			/* links dropped on the panel: added to the list, the last chosen */
			if ( m.event != OB_E_DROP ) return;
			T_OBDROP *d = ui_drop(q);
			if ( d == NULL ) return;
			for ( INT i = 0; i < d->n; i++ ) {
				BrObjLink l;
				memcpy(l.id, d->v[i].target.b, 16);
				l.name = br_obj_name(l.id);
				if ( l.name.empty() ) l.name = (const char *)ob_drop_name(d, i, nullptr);
				links.push_back(l);
			}
			ui_drop_answer(q, d, OB_DR_ACCEPT, "");
			names(q);
			ui_set_value(q, DLG_FIELD, (INT)links.size());
		});
	out->clear();
	for ( int k : picked ) out->push_back(links[(size_t)k]);
	printf("browser: %zu chosen\n", out->size());
	return a == DLG_OK && !out->empty();
}

bool ext_ask_panel( const std::string &title, const std::string &name, const std::string &version,
		    const std::vector<std::string> &wants )
{
	for ( const std::string &w : wants ) printf("browser: extension %s wants: %s\n", name.c_str(), w.c_str());
	INT a = dlg_work(PN_EXT,
		[&]( UI *q ) {
			std::vector<const char *> p;
			for ( const std::string &w : wants ) p.push_back(w.c_str());
			ui_set_text(q, 1, title.c_str());
			ui_set_text(q, 2, version.empty() ? " " : ( "バージョン " + version ).c_str());
			dlg_names(q, DLG_FIELD, p);
		},
		[&]( UI *, INT ) {});
	printf("browser: extension panel answered %d\n", (int)a);
	return a == DLG_OK;
}

int br_choose( const std::vector<std::string> &names, int chosen )
{
#ifdef BR_BLINK
	if ( br_is_renderer ) return br_rproc_choose(names, chosen);
#endif
	int	got = -1;

	if ( names.empty() ) return -1;
	INT a = dlg_work(PN_SELECT,
		[&]( UI *q ) {
			std::vector<const char *> p;
			for ( const std::string &n : names ) p.push_back(n.empty() ? " " : n.c_str());
			dlg_names(q, DLG_FIELD, p);
			ui_set_value(q, DLG_FIELD, chosen >= 0 ? chosen + 1 : 0);
		},
		[&]( UI *q, INT ans ) {
			if ( ans == DLG_OK ) got = ui_value(q, DLG_FIELD) - 1;
		});
	printf("browser: chosen %d of %d\n", a == DLG_OK ? got : -1, (int)names.size());
	return ( a == DLG_OK ) ? got : -1;
}

#ifdef BR_BLINK
namespace {

sk_sp<SkData>	g_face_data;		/* the face the pages are drawn in */

/*
 * The raster on the GPU, before the engine makes its first canvas: by
 * default on a machine with a GPU only (on softpipe Skia on the
 * processor is the faster), "gpu=on" on any OpenGL ES there is
 */
void gpu_up()
{
	if ( !g_gpufake.empty() && ts_gl_drm_fake != nullptr ) {
		/*
		 * before the first EGL display: every one is tried on the pretended
		 * GPU, the V3D on the Raspberry Pi, a Kaby Lake GT2 on x86-64
		 * (whose Mesa has iris and no v3d)
		 */
#if defined(__x86_64__)
		ts_gl_drm_fake(2);
		printf("browser: gpu: Intel's GPU is pretended (gpufake=)\n");
#else
		ts_gl_drm_fake(1);
		printf("browser: gpu: the V3D is pretended (gpufake=)\n");
#endif
	}
#if defined(__x86_64__)
	{
		/* x86-64: the GPU the system drives (Mesa's iris on Intel's), on one line for the board's log */
		T_GPUINFO xi;
		if ( gp_ref_gpu(&xi) >= E_OK ) {
			printf("browser: gpu: the machine's GPU is %s (kind %u, version %u, revision %u)\n",
			       xi.kind == GPU_KIND_INTEL ? "Intel's" : "not Intel's", xi.kind, xi.ver, xi.rev);
		} else {
			printf("browser: gpu: the machine has no GPU the system drives (softpipe)\n");
		}
	}
#endif
	if ( br_gpu_start != nullptr && br_multi_process && g_gpuproc ) {
		/* a renderer: the GPU process says whether the page is rastered there */
		g_gpu = br_gpu_start(g_gpu_mode);
	} else if ( br_gpu_start != nullptr ) {
		T_GPUINFO gi;
		if ( g_gpu_mode == BR_GPU_AUTO && gp_ref_gpu(&gi) < E_OK ) {
			printf("browser: gpu: the machine has no GPU, the page is rastered on the processor (gpu=on: on softpipe)\n");
		} else {
			g_gpu = br_gpu_start(g_gpu_mode);
		}
	}
}

/*
 * What the window shows when the renderer of its page has ended: the
 * page Chrome shows then, in its words, with a button that loads the
 * page again
 */
std::string gone_page( const std::string &url, int status )
{
	std::string code = "RESULT_CODE_KILLED";
	if ( WIFEXITED(status) ) code = "RESULT_CODE_EXITED " + std::to_string(WEXITSTATUS(status));
	else if ( WIFSIGNALED(status) && WTERMSIG(status) != SIGKILL ) code = "SIGNAL " + std::to_string(WTERMSIG(status));
	return "<!DOCTYPE html><html lang=\"ja\"><head><meta charset=\"utf-8\"><title>" + html_text(url)
	       + "</title><style>body{font-family:sans-serif;color:#202124;margin:72px 12% 0}"
		 "h1{font-size:1.6em;font-weight:normal;margin:0 0 16px}p{line-height:1.6}"
		 ".code{color:#5f6368;font-size:.85em;margin-top:16px}"
		 "button{margin-top:24px;padding:8px 16px;font-size:1em}</style></head><body>"
		 "<h1>エラーが発生しました</h1><p>このウェブページの表示中に問題が発生しました。</p>"
		 "<p class=\"code\">エラーコード: " + html_text(code) + "</p>"
		 "<button onclick=\"location.reload()\">再読み込み</button></body></html>";
}

/*
 * The renderer of the window's page has ended (it failed, or was ended):
 * the browser goes on, and the window shows the page above in a new
 * renderer, at the same address, so that 再読み込み loads it again
 */
void renderer_gone()
{
	BrEngine *dead = g_br.eng;
	int pid = br_proc_pid(dead);
	printf("browser: the renderer %d of %s has ended: the page is shown as failed\n", pid, g_br.url.c_str());
	BrEngine *fresh = br_proc_new_page();
	Run &cur = g_runs[(size_t)g_run];
	cur.eng = fresh;
	cur.states.clear();
	cur.urls.clear();
	cur.at = -1;
	g_ime_eng = nullptr;
	if ( fresh == nullptr ) {
		/* no renderer to be had: the window is closed, as nothing can be shown in it */
		printf("browser: no renderer to be had: the window closes\n");
		close_now("no renderer");
		return;
	}
	g_br.eng = fresh;
	br_proc_close(dead);
	proc_log("gone pid=" + std::to_string(pid) + " new=" + std::to_string(br_proc_pid(fresh)));
	br_proc_keep_ready();
	g_br.status = "ページを表示していたプロセスが終了しました";
	g_br.failed = true;
	g_loading = false;
	g_hover.clear();
	zoom_to(fresh);
	fresh->load(g_br.url, gone_page(g_br.url, g_last_gone_status), "utf-8", std::string());
	g_br.title = fresh->title();
	g_br.full = true;
	g_br.scroll = 0;
	relayout();
	show_title();
	tool_states(false);
	show();
	tool_set_url(g_br.url);
}

}  // namespace

/* The engine of a renderer: the face, the GPU, Blink (br_rproc.cc) */
bool br_sys_engine_up()
{
	g_face_data = read_face();
	if ( g_face_data == nullptr ) {
		printf("browser: renderer: no face to draw letters in\n");
		return false;
	}
	br_start_mark("the face");
	gpu_up();
	br_start_mark("the GPU");
	br_blink_up(g_face_data);
	br_start_mark("Blink, ready for a page");
	start_say("browser: renderer: start");
	return true;
}

BrEngine *br_sys_engine_page()
{
	/* a renderer kept ready waited for this page: its turns are timed from here, not from its start */
	g_beat = now_ms();
	return br_blink_engine(g_face_data);
}

/* ---------------------------------------------------------------- what br_proc.cc and br_rproc.cc ask of the system */

/*
 * A renderer is a process object of the browser's program, made to end
 * with this process (tessronos.process.withParent) and named by its role
 * among the browser's processes (tessronos.process.role), which a person
 * reads after the program's name where the process is shown (a panel of
 * its abnormal end, the list of processes): 「ブラウザ(レンダラ)」, not the
 * browser itself; its number is read from the object's metadata
 */
int br_sys_spawn( const std::string &arg )
{
	static const char	with_parent[] = "{\"tessronos\":{\"process\":{\"withParent\":true}}}";
	T_OBCRE			c;
	TS_UUID			prog, pu;
	char			j[512];
	SZ			asz = 0;

	if ( !parse_uuid(SYSDEF_PROG_BROWSER, &prog) ) return E_NOEXS;
	memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = prog;
	c.arg = arg.c_str();
	c.argsz = (SZ)arg.size() + 1;
	std::string kind = arg.substr(0, arg.find('\n'));
	const char *role = kind == BR_RENDERER_LINE ? "レンダラ"
			 : kind == BR_NETWORK_LINE ? "ネットワーク"
			 : kind == BR_GPU_LINE ? "GPU" : "";
	std::string meta = std::string("{\"tessronos\":{\"process\":{\"withParent\":true")
			   + ( *role != '\0' ? std::string(",\"role\":\"") + role + "\"" : std::string() ) + "}}}";
	c.json = (CONST UB *)meta.c_str();
	c.jsonsz = (SZ)meta.size();
	ER er = ob_cre_obj(&c, &pu);
	if ( er < E_OK ) return (int)er;
	ID k = ob_opn_obj(&pu, OB_OP_ATRRD | OB_OP_ATRWR);
	if ( k <= 0 ) return (int)k;
	if ( ob_get_atr(k, (UB *)j, sizeof(j) - 1, &asz) < E_OK ) asz = 0;
	j[asz] = '\0';
	if ( strstr(j, "\"withParent\":true") == nullptr ) {
		(void)ob_set_atr(k, (CONST UB *)with_parent, (SZ)( sizeof(with_parent) - 1 ));
	}
	ob_cls_obj(k);
	const char *at = strstr(j, "\"pid\":");
	return ( at != nullptr ) ? (int)strtol(at + 6, nullptr, 10) : E_OBJ;
}

bool br_sys_give( int pid, int fd )
{
	return ts_chan_give(pid, &fd, 1) == 0;
}

int br_sys_take( int ms )
{
	int fd = -1;
	return ( ts_chan_take(&fd, 1, ms) == 1 ) ? fd : -1;
}

void br_sys_kill( int pid )
{
	(void)kill(pid, SIGKILL);
}

bool br_sys_reap( int pid, int ms, int *status )
{
	int st = 0;
	for ( long until = now_ms() + ms;; ) {
		int r = waitpid(pid, &st, WNOHANG);
		if ( r == pid || ( r < 0 && errno == ECHILD ) ) {
			if ( status != nullptr ) *status = st;
			g_last_gone_status = st;
			return true;
		}
		if ( now_ms() >= until ) return false;
		usleep(10000);
	}
}

namespace {
/* A number of the system's information (its object's record OB_SYS_INFO), 'none' when it is not there */
long sys_info_num( const char *key, long none )
{
	TS_UUID	u;
	UB	*b = nullptr;

	if ( ui_uuid(SYSDEF_SYSTEM, &u) < E_OK ) return none;
	INT len = ui_rec_all(&u, OB_SYS_INFO, 0, &b);
	if ( len <= 0 || b == nullptr ) return none;
	std::string t((const char *)b, (size_t)len), k = std::string("\n") + key + "\t";
	free(b);
	size_t at = ( "\n" + t ).find(k);
	return ( at == std::string::npos ) ? none : atol(t.c_str() + at + k.size() - 1);
}
}

long br_sys_memory_mb()
{
	return sys_info_num("MEMORY_MB", 1024);
}

#ifdef BR_BLINK
int br_sys_gpu_mode()
{
	return g_gpu_mode;
}

bool br_sys_gpuproc()
{
	return g_gpuproc;
}

void br_sys_gpu_fake_begin()
{
	if ( g_gpufake.empty() || ts_gl_drm_fake == nullptr ) return;
#if defined(__x86_64__)
	ts_gl_drm_fake(2);
	printf("browser: gpu process: Intel's GPU is pretended (gpufake=)\n");
#else
	ts_gl_drm_fake(1);
	printf("browser: gpu process: the V3D is pretended (gpufake=)\n");
#endif
}

void br_sys_gpu_fake_end()
{
	if ( g_gpufake.empty() || ts_gl_probe_result == nullptr ) return;
	char drv[16], n[48];
	int step = ts_gl_probe_result(drv, sizeof(drv));
	int len = snprintf(n, sizeof(n), "driver=%s step=%d\n", drv, step);
	INT fd = fs_open(g_gpufake.c_str(), 0x0001 | 0x0100 | 0x0400);
	if ( fd >= 0 ) {
		(void)fs_write(fd, n, len);
		fs_close(fd);
	}
	printf("browser: gpu process: the pretended GPU: %s", n);
}

namespace {

struct GpuWatch {
	ID	key = 0;		/* the GPU's device object, gpu0 */
	ID	port = 0;		/* the channel its notices come to */
	void	(*arrived)( const char *what ) = nullptr;
};

GpuWatch g_gpu_watch;

/* A thread of its own: each notice of the object is a look again at whether a GPU is registered */
void *gpu_watch_thread( void * )
{
	GpuWatch &w = g_gpu_watch;
	for ( ;; ) {
		T_OBNTM m;
		SZ asz = 0;
		ER er = ob_rea_tmo(w.port, 0, 0, &m, sizeof(m), &asz, TMO_FEVR);
		if ( er < E_OK ) {
			printf("browser: gpu process: the GPU's object tells no more (%d)\n", (int)er);
			break;
		}
		T_GPUINFO gi;
		if ( gp_ref_gpu(&gi) < E_OK ) continue;
		char what[48];
		snprintf(what, sizeof(what), "kind %u", (unsigned)gi.kind);
		w.arrived(what);
		break;
	}
	ob_cls_obj(w.port);
	ob_cls_obj(w.key);
	return nullptr;
}

}  // namespace

bool br_sys_gpu_watch( void (*arrived)( const char *what ) )
{
	static const TS_UUID gpu = OB_UUID_GPU_INIT;
	GpuWatch &w = g_gpu_watch;
	T_GPUINFO gi;

	/* asked to be told before looking, so that a GPU registered between the two is not missed */
	w.key = ob_opn_obj(&gpu, OB_OP_ATRRD);
	if ( w.key > 0 ) {
		T_OBCRE c;
		TS_UUID ch;
		memset(&c, 0, sizeof(c));
		c.type = OB_T_CHANNEL;
		w.port = ( ob_cre_obj(&c, &ch) >= E_OK ) ? ob_opn_obj(&ch, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT) : E_NOEXS;
		T_OBNTF req;
		memset(&req, 0, sizeof(req));
		req.events = OB_E_CHANGE;
		if ( w.port <= 0 || ob_ntf_evt(w.key, OB_GP_STATE, &req, w.port) <= 0 ) {
			printf("browser: gpu process: the GPU's object cannot tell of a GPU that comes later\n");
			if ( w.port > 0 ) ob_cls_obj(w.port);
			ob_cls_obj(w.key);
			w.key = w.port = 0;
		}
	}
	if ( gp_ref_gpu(&gi) >= E_OK ) {
		if ( w.key > 0 ) {
			ob_cls_obj(w.port);
			ob_cls_obj(w.key);
			w.key = w.port = 0;
		}
		return true;
	}
	if ( w.key <= 0 ) return false;
	w.arrived = arrived;
	pthread_t th;
	if ( pthread_create(&th, nullptr, gpu_watch_thread, nullptr) != 0 ) {
		ob_cls_obj(w.port);
		ob_cls_obj(w.key);
		w.key = w.port = 0;
		return false;
	}
	pthread_detach(th);
	return false;
}
#endif

long br_sys_free_mb()
{
	return sys_info_num("FREE_MB", 0);
}

void br_sys_renderer_setup()
{
	br_stop_pushed = tool_stop_pushed;
	g_engine_tid = tk_get_tid();
	g_engine_thread = pthread_self();
	g_engine_known = true;
}

void br_sys_stop( bool now )
{
	g_stop = true;
	br_net_fetch_cancel_sync(now ? 1 : 0);
}

void br_sys_closing()
{
	g_closing = true;
	g_stop = true;
	br_net_fetch_cancel_sync(1);
	br_settings_close();
	br_store_flush_all();
	fflush(stdout);
	br_console_drain(2000);
}

bool br_sys_progress( long got, long total )
{
	char buf[96];

	if ( total > 0 ) {
		snprintf(buf, sizeof(buf), "読み込み中… %ld / %ld KB", got / 1024, total / 1024);
	} else {
		snprintf(buf, sizeof(buf), "読み込み中… %ld KB", got / 1024);
	}
	(void)wm_msg_put(buf);
	return g_stop || g_closing;
}

namespace {
std::string	g_turn_url;		/* the window's page in a renderer, as the extensions' tab */
BrExtMenuAt	g_ctx_at;		/* what the last right press found, for the extensions' items */
}

void br_sys_tab_at( const std::string &url )
{
	g_turn_url = url;
	br_ext_tab_set(url, std::string());
}

void br_sys_after_turn( BrEngine *eng, bool primary )
{
	br_store_tick();
	if ( primary ) {
		g_turn_url = eng->url();
		br_ext_tab_set(g_turn_url, eng->title());
	}
}

std::vector<std::string> br_sys_ext_actions()
{
	return br_ext_action_rows();
}

std::vector<std::string> br_sys_ext_items( const std::string &link, const std::string &image,
					   const std::string &selection, bool editable )
{
	g_ctx_at.page = g_turn_url;
	g_ctx_at.link = link;
	g_ctx_at.image = image;
	g_ctx_at.selection = selection;
	g_ctx_at.editable = editable;
	return br_ext_menu_rows(g_ctx_at);
}

std::string br_sys_command( BrEngine *eng, const std::string &code )
{
	(void)eng;
	printf("browser: renderer: command %s\n", code.c_str());
	if ( code.compare(0, 11, "ext.action:") == 0 ) {
		br_ext_action_click(atoi(code.c_str() + 11));
	} else if ( code.compare(0, 8, "ext.ctx:") == 0 ) {
		br_ext_menu_click(atoi(code.c_str() + 8), g_ctx_at);
	} else if ( code.compare(0, 9, "isolated:") == 0 ) {
		std::string o = code.substr(9);
		br_isolated_set(o, !br_isolated_allowed(o));
	} else if ( code.compare(0, 8, "hstsadd:") == 0 ) {
		br_net_hsts_add(code.substr(8));
	} else if ( code == "flushstores" ) {
		(void)br_store_flush_async();
	} else if ( code == "mplay" || code == "mprev" || code == "mnext" || code == "mstop" ) {
		return br_media_session_action(code);
	} else if ( code == "wvinstall" ) {
		/* its end told to the browser's loop, which takes it over (wv_install_done) */
		return br_wv_install_start([] { br_rproc_main_command("wvdone"); }) ? "1" : "0";
	} else if ( code == "wvfinish" || code == "wvremove" ) {
		std::string msg;
		bool ok = ( code == "wvfinish" ) ? br_wv_finish(&msg) : br_wv_remove(&msg);
		if ( ok ) br_eme_widevine_changed();
		return std::string(ok ? "1" : "0") + "\n" + msg;
	}
	return std::string();
}

std::string br_sys_query( const std::string &what )
{
	char n[256];

	if ( what == "webaudio" ) return std::to_string(br_web_audio_frames());
	if ( what == "mediaframes" ) {
		long shown = 0, planes = 0, rgb = 0, webgl = 0;
		media_frames(&shown, &planes, &rgb, &webgl);
		snprintf(n, sizeof(n), "%ld %ld %ld %ld", shown, planes, rgb, webgl);
		return n;
	}
	if ( what == "gpustat" ) {
		BrGpuStat g;
		if ( !gpu_stat(&g) ) return std::string();
		snprintf(n, sizeof(n), "%ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld", g.views, g.play_us, g.flush_us,
			 g.read_us, g.max_us, g.read_px, g.canvases, g.canvas_us, g.trips, g.proc, g.trip_us, g.gs_views,
			 g.gs_take_us);
		return n;
	}
	if ( what == "gpuprobe" ) {
		/* the driver the EGL platform took on the pretended GPU and the step it got to ("gpufake=") */
		if ( ts_gl_probe_result == nullptr ) return std::string();
		char drv[16];
		int step = ts_gl_probe_result(drv, sizeof(drv));
		snprintf(n, sizeof(n), "driver=%s step=%d\n", drv, step);
		return n;
	}
	if ( what == "gpucheck" ) {
		snprintf(n, sizeof(n), "%ld %ld %ld %ld %d", g_gc.views, g_gc.px, g_gc.differ, g_gc.far, g_gc.most);
		return n;
	}
	if ( what == "wvsource" ) {
		std::string a, b;
		br_wv_source(&a, &b);
		return a + "\n" + b;
	}
	if ( what == "snd" ) return std::to_string(br_snd_frames());
	if ( what == "protocols" ) {
		(void)br_net_protocols(n, sizeof(n));
		return n;
	}
	if ( what == "print" ) {
		int len = br_print_report(n, sizeof(n));
		return std::string(n, len > 0 ? (size_t)len : 0);
	}
	return std::string();
}

void br_sys_gpu_check( BrEngine *eng, int top, int w, int h, const void *px, size_t pitch )
{
	if ( !g_gpucheck.empty() ) gpu_check_pixels(eng, top, w, h, px, pitch);
}

bool br_sys_gpu_checking()
{
	return !g_gpucheck.empty();
}

/* ---------------------------------------------------------------- the window's GPU buffers (design 14.13.10) */

bool br_sys_gsurf_on()
{
	return g_gsurf;
}

/* The descriptors of the libraries' own (lib/libgl/gl_drm.c): a PRIME descriptor and the key it holds */
extern "C" ID ts_gpu_fd_key( int fd ) __attribute__((weak));
extern "C" int ts_gpu_fd_from_key( ID key ) __attribute__((weak));

namespace {

ER gsurf_write( const T_OBWGSURF &g )
{
	SZ asz = 0;

	return ob_wri_rec(g_br.kw, OB_WR_GSURF, 0, &g, sizeof(g), &asz);
}

/* The GPU process's context the BOs of the windows' buffers are made in */
INT	g_gs_ctx = 0;

}  // namespace

int br_sys_gsurf_set( const int fd[2], int w, int h, unsigned pitch )
{
	T_OBWGSURF g;

	if ( ts_gpu_fd_key == nullptr ) return E_NOSPT;
	memset(&g, 0, sizeof(g));
	g.op = OB_GS_SET;
	for ( int i = 0; i < OB_GS_BUFS; i++ ) {
		ID k = ts_gpu_fd_key(fd[i]);
		if ( k <= 0 ) return E_OBJ;
		g.bo[i] = k;
	}
	g.w = w;
	g.h = h;
	g.pitch = pitch;
	g.format = OB_GS_XRGB8888;
	return gsurf_write(g);
}

int br_sys_gsurf_show( int idx, unsigned serial, const std::vector<BrRect> &rects )
{
	T_OBWGSURF g;

	memset(&g, 0, sizeof(g));
	g.op = OB_GS_SHOW;
	g.idx = idx;
	g.serial = (UW)serial;
	/* none, or more than the record takes: all of it */
	if ( rects.size() <= (size_t)OB_GS_RECTS ) {
		for ( const BrRect &r : rects ) {
			if ( r.w <= 0 || r.h <= 0 ) continue;
			T_OBGSRECT &o = g.rect[g.nrect++];
			o.left = r.x;
			o.top = r.y;
			o.right = r.x + r.w;
			o.bottom = r.y + r.h;
		}
	}
	return gsurf_write(g);
}

int br_sys_gsurf_drop()
{
	T_OBWGSURF g;

	memset(&g, 0, sizeof(g));
	g.op = OB_GS_DROP;
	return gsurf_write(g);
}

bool br_sys_gsurf_state( int *shown, unsigned *serial )
{
	T_OBWGSTAT st;
	SZ asz = 0;

	memset(&st, 0, sizeof(st));
	if ( ob_rea_rec(g_br.kw, OB_WR_GSURF, 0, &st, sizeof(st), &asz) < E_OK || asz != (SZ)sizeof(st) ) return false;
	*shown = st.shown;
	*serial = st.shown_serial;
	return true;
}

bool br_sys_gsurf_bo( size_t size, bool gpu, BrGsurfBo *b, std::string *why )
{
	T_GPBO		bo;
	T_OBGBMAKE	mk;
	T_OBCRE		c;
	TS_UUID		u;
	void		*p = nullptr;
	ER		er;

	if ( g_gs_ctx <= 0 ) {
		INT ctx = gp_opn_gpu();
		if ( ctx <= 0 ) {
			*why = "no context of the GPU (" + std::to_string((int)ctx) + ")";
			return false;
		}
		g_gs_ctx = ctx;
	}
	er = gp_cre_bo(g_gs_ctx, (SZ)size, gpu ? 0 : GPU_BO_NOGPU, &bo);
	if ( er < E_OK ) {
		*why = "no BO of " + std::to_string(size) + " bytes (" + std::to_string((int)er) + ")";
		return false;
	}
	er = gp_map_bo(g_gs_ctx, bo.handle, &p);
	if ( er >= E_OK ) {
		memset(&mk, 0, sizeof(mk));
		mk.ctx = g_gs_ctx;
		mk.handle = bo.handle;
		memset(&c, 0, sizeof(c));
		c.type = OB_T_STORAGE;
		c.sub = OB_S_GPUBO;
		c.arg = &mk;
		c.argsz = sizeof(mk);
		er = ob_cre_obj(&c, &u);
	}
	ID keep = ( er >= E_OK ) ? ob_opn_obj(&u, OB_OP_R) : (ID)er;
	if ( keep <= 0 ) {
		*why = "the BO made no object (" + std::to_string((int)keep) + ")";
		(void)gp_del_bo(g_gs_ctx, bo.handle);
		return false;
	}
	b->handle = bo.handle;
	b->map = p;
	b->size = (size_t)bo.size;
	memcpy(b->uuid, &u, sizeof(b->uuid));
	b->keep = keep;
	return true;
}

int br_sys_gsurf_bo_fd( const BrGsurfBo *b )
{
	TS_UUID u;

	if ( b->keep <= 0 || ts_gpu_fd_from_key == nullptr ) return -1;
	memcpy(&u, b->uuid, sizeof(u));
	ID k = ob_opn_obj(&u, OB_OP_R);
	if ( k <= 0 ) return -1;
	int fd = ts_gpu_fd_from_key(k);
	if ( fd < 0 ) (void)ob_cls_obj(k);
	return fd;
}

void br_sys_gsurf_bo_free( BrGsurfBo *b )
{
	if ( b->keep <= 0 ) return;
	/* the BO goes once no key to its object is open and the window holds it no more */
	(void)gp_del_bo(g_gs_ctx, b->handle);
	(void)ob_cls_obj(b->keep);
	*b = BrGsurfBo();
}

bool br_sys_flush( int wait_ms )
{
	unsigned ticket = br_store_flush_async();
	return br_store_wait(ticket, wait_ms) != 0;
}

/*
 * The calling process restricted (design 5.9, 14.13.9) as the start-up
 * line "sandbox=" says: groups shut out, refused or ending the process
 */
static void sandbox_enter( unsigned groups, const char *who )
{
	if ( br_sandbox_mode == 0 ) {
		printf("browser: %s: no sandbox (sandbox=off)\n", who);
		return;
	}
	unsigned how = ( br_sandbox_mode == 1 ) ? TS_SBX_REFUSE : 0;
	if ( ts_sandbox_enter(groups, how) != 0 ) {
		printf("browser: %s: the sandbox was not entered (errno %d)\n", who, errno);
		return;
	}
	printf("browser: %s: in its sandbox (%x, %s)\n", who, groups, how ? "refused" : "ended");
}

/*
 * The renderer: the GPU stays open to it while it rasters there, and the
 * devices while it opens the sound's, the microphone's and the camera's
 * itself. The objects stay open too unless "sandbox_renderer=noobj": the
 * stores' are opened by the network process, but other things a renderer
 * opens by UUID or by name (the extensions, Widevine ...) are not yet.
 */
void br_sandbox_renderer()
{
	sandbox_enter(br_sandbox_noobj ? ( TS_SBX_RENDERER_STORE | TS_SBX_OBJ ) : TS_SBX_RENDERER_STORE, "renderer");
}

/* The network process: the sockets and the stores' objects */
void br_sandbox_network()
{
	sandbox_enter(TS_SBX_NETWORK, "network");
}
#endif

int main()
{
	T_OBCRE		c;
	T_OBNTF		req;
	TS_UUID		ch;
	Script		sc = read_script();
	BrObject	&o = g_br.obj;
	char		json[256];

	br_start_mark(nullptr);
	g_beat = now_ms();		/* the first turn of the engine's thread is the start, from here */
#ifdef BR_BLINK
	if ( br_is_renderer ) {
		/* a renderer's too: its turns (the page's calls) are timed from its start */
		br_renderer_main();
		return 0;
	}
	if ( br_is_network ) {
		br_network_main();
		return 0;
	}
	if ( br_is_gpu ) {
		/* only a program linked with the system's OpenGL ES (make GL=1) has one */
		if ( br_gpu_main == nullptr ) {
			printf("browser: gpu process: the program has no OpenGL ES (make GL=1)\n");
			return 1;
		}
		br_gpu_main();
		return 0;
	}
#endif
	if ( sc.has_obj && !br_obj_open(sc.obj, &o) ) {
		printf("browser: the page object %s cannot be read\n", br_uuid_str(sc.obj).c_str());
		return 5;
	}
#ifdef BR_BLINK
	/* a line "trace=" in the page object's text, as the start-up line (the user's look at a site) */
	if ( o.ok && !o.trace.empty() ) {
		if ( !br_trace.empty() ) br_trace += ",";
		br_trace += o.trace;
		printf("browser: trace=%s, from the page object\n", o.trace.c_str());
	}
#endif
	if ( o.ok && already_shown(o) ) {
		/* one window an object: the one there is brought forward, and this one goes */
		(void)wm_msg_put("このページの実身はもう開いています");
		printf("browser: %s is open already\n", br_uuid_str(o.id).c_str());
		return 0;
	}
	/*
	 * The window where the object keeps it, on the screen it was kept on
	 * (the main screen when that one is not there); with none kept, the
	 * program's own place on the screen new windows open on
	 */
	INT vx = o.x, vy = o.y;
	if ( o.kept ) {
		wm_place_xywh(&vx, &vy, o.w + kFrameW, o.h + kTitleH, o.screen.c_str());
	} else {
		vx = wm_new_x(o.x);
		vy = wm_new_y(o.y);
	}
	if ( o.ok ) {
		snprintf(json, sizeof(json), "{\"rect\":[%d,%d,%d,%d],\"attr\":31,\"shows\":\"%s\"}",
			 vx, vy, vx + o.w + kFrameW, vy + o.h + kTitleH, br_uuid_str(o.id).c_str());
	} else {
		snprintf(json, sizeof(json), "{\"rect\":[%d,%d,%d,%d],\"attr\":31}",
			 vx, vy, vx + o.w + kFrameW, vy + o.h + kTitleH);
	}
	memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (CONST UB *)( o.ok && !o.name.empty() ? o.name.c_str() : "ブラウザ" );
	c.json = (CONST UB *)json;
	c.jsonsz = (SZ)strlen(json);
	if ( ob_cre_obj(&c, &g_br.win) < E_OK ) {
		printf("browser: no window\n");
		return 1;
	}
	g_br.kw = ob_opn_obj(&g_br.win, OB_OP_R | OB_OP_W | OB_OP_PROT);
	g_br.gid = ( g_br.kw > 0 ) ? wm_obj_gid(g_br.kw) : E_ID;
	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	g_br.port = ( ob_cre_obj(&c, &ch) >= E_OK )
	       ? ob_opn_obj(&ch, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT) : E_NOEXS;
	memset(&req, 0, sizeof(req));
	req.events = OB_E_CLOSE | OB_E_PRESS | OB_E_KEY | OB_E_REDRAW | OB_E_DELETE;
#ifdef OB_E_WHEEL
	req.events |= OB_E_WHEEL;
#endif
	req.events |= OB_E_SCROLL;
#ifdef BR_BLINK
	/*
	 * the page takes the pointer's moves and the buttons coming up too,
	 * links and letters let go over it, and the pointer leaving it while
	 * the page drags something
	 */
	req.events |= OB_E_RELEASE | OB_E_MOVE | OB_E_DROP | OB_E_TEXT | OB_E_LEAVE;
#endif
	ER ntf = ( g_br.kw > 0 && g_br.gid >= 0 && g_br.port > 0 )
	       ? ob_ntf_evt(g_br.kw, OB_REC_ANY, &req, g_br.port) : E_OBJ;
	if ( ntf <= 0 ) {
		/* what was refused: the window's key, its drawing, the channel, the events */
		printf("browser: the window cannot be used (key %d, gid %d, channel %d, events %d)\n",
		       (int)g_br.kw, (int)g_br.gid, (int)g_br.port, (int)ntf);
		(void)ob_del_obj(&g_br.win);
		return 2;
	}
#ifdef BR_BLINK
	if ( br_multi_process && g_gpuproc && g_gsurf ) {
		/* the window says when it shows one of its GPU buffers (design 14.13.10) */
		T_OBNTF gq;
		memset(&gq, 0, sizeof(gq));
		gq.events = OB_E_CHANGE;
		gq.id = N_GSURF;
		ER gn = ob_ntf_evt(g_br.kw, OB_WR_GSURF, &gq, g_br.port);
		if ( gn <= 0 ) {
			printf("browser: the window has no GPU buffers (%d): each view is read back from the GPU process\n",
			       (int)gn);
			g_gsurf = false;
		}
	}
#endif
	br_start_mark("the window");
	/*
	 * The window's notices are taken from now on: the engine's thread
	 * takes seconds more to come up (the face, the first renderer, Blink),
	 * and a press meanwhile is answered as while it is held (the menu,
	 * 閉じる, Ctrl+E)
	 */
	beat("starting");
	g_engine_tid = tk_get_tid();
#ifdef BR_BLINK
	g_engine_thread = pthread_self();
	g_engine_known = true;
#endif
	if ( !window_thread_start() ) {
		printf("browser: no thread for the window\n");
		(void)ob_del_obj(&g_br.win);
		return 6;
	}

	sk_sp<SkData> data = read_face();
	if ( data != nullptr ) {
		g_br.mgr = SkFontMgr_New_Custom_Data(SkSpan<sk_sp<SkData>>(&data, 1));
		g_br.face = g_br.mgr->makeFromData(data);
	}
	if ( g_br.face == nullptr ) {
		printf("browser: no face to draw letters in\n");
		(void)ob_del_obj(&g_br.win);
		return 3;
	}
	br_start_mark("the face");
#ifdef BR_BLINK
	g_face_data = data;
	if ( br_multi_process ) {
		/* the pages in renderers of their own: the browser keeps no engine, nor a GPU of its own */
		br_proc_player_painted = &player_painted;
		if ( br_proc_start(sc.lines, g_br.win.b) ) g_br.eng = br_proc_new_page();
		if ( g_br.eng == nullptr ) {
			printf("browser: no renderer could be started: the pages are run in the browser's process\n");
			br_multi_process = false;
		} else {
			g_runs[0].eng = g_br.eng;
			proc_log("first pid=" + std::to_string(br_proc_pid(g_br.eng)) + " net=" +
				 std::to_string(br_proc_network_pid()));
			br_proc_keep_ready();
			g_gpu = true;		/* asked of the renderer, which says when it rasters on the processor */
			br_start_mark("the first renderer");
		}
	}
	if ( !br_multi_process ) {
		gpu_up();
		br_start_mark("the GPU");
		g_br.eng = br_blink_engine(data);
		br_start_mark("the engine's rest");
	}
#else
	g_br.eng = br_text_engine(g_br.face.get());
#endif
	{
		/* the work area as large as the object keeps it, whatever the frame takes */
		T_OBWPOS	wp;
		SZ		asz = 0;

		if ( ob_rea_rec(g_br.kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK && asz >= (SZ)sizeof(wp) ) {
			int dw = o.w - ( wp.wright - wp.wleft ), dh = o.h - ( wp.wbottom - wp.wtop );
			if ( dw != 0 || dh != 0 ) {
				wp.right += dw;
				wp.bottom += dh;
				(void)ob_wri_rec(g_br.kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
			}
		}
	}
	(void)read_size();
	g_br.status = "";
	if ( !tool_open() ) printf("browser: no tool panel\n");
	br_stop_pushed = tool_stop_pushed;
	br_start_mark("the tool panel");
	start_say();
	beat("starting");
	printf("browser: up, %dx%d\n", g_br.w, g_br.h);

	/*
	 * The address the page object keeps (the base holds the first one);
	 * an object that keeps none shows an empty page, and the browser
	 * started with no object its own page
	 */
	go(!sc.url.empty() ? sc.url : !o.url.empty() ? o.url : o.ok ? "about:blank" : "about:home", BR_LOAD_NEW);
	if ( sc.follow && g_br.eng != nullptr ) {
		/* the first link of the page, as a press on it would */
		std::string first = g_br.eng->first_link();
		for ( int y = 0; y < g_br.doc_h && first.empty(); y += 4 ) {
			for ( int x = 0; x < g_br.w && first.empty(); x += 8 ) first = g_br.eng->link_at(x, y);
		}
		if ( !first.empty() ) go(first, BR_LOAD_NEW);
	}
	for ( const std::string &to : sc.then ) go(to, BR_LOAD_NEW);
	for ( const std::string &js : sc.scripts ) g_br.eng->run_script(js);
	if ( sc.sethome ) set_home();
	for ( const std::string &code : sc.menu ) {
		/* what the page told the browser's services while it loaded has arrived first */
		for ( long until = now_ms() + 300; now_ms() < until; ) {
			(void)g_br.eng->pump();
			usleep(10000);
		}
		(void)menu_do(code);
	}
	if ( sc.mem ) {
		printf("browser: memory: %s\n", g_br.eng->memory(false).c_str());
		printf("browser: memory after collecting: %s\n", g_br.eng->memory(true).c_str());
	}
	if ( !sc.ready.empty() ) {
		/* the file layer's write, create and truncate (include/ts/fs.h) */
		INT fd = fs_open(sc.ready.c_str(), 0x0001 | 0x0100 | 0x0400);
		if ( fd >= 0 ) fs_close(fd);
	}
	g_later.assign(sc.later.begin(), sc.later.end());
	g_apart_lines = sc.apart;
	long quit_at = ( sc.quit >= 0 ) ? now_ms() + sc.quit : -1;
	g_quit_at = quit_at;
	std::string title_kept = g_br.title;		/* the title last said */
	long mem_said = now_ms();			/* the memory last said */

#ifdef BR_BLINK
	/* what another thread queues from now on wakes the loop's waits */
	br_loop_bind();
#endif
	for ( bool done = false; !done; ) {
		T_OBNTM		m;
		std::string	cmd;

		beat("its loop");
		if ( g_closing ) break;
		if ( g_stop ) {
			/* round again after a 中止: what the page asks for is waited for again */
			g_stop = false;
#ifdef BR_BLINK
			if ( !br_multi_process ) br_net_fetch_cancel_sync(0);
#endif
		}
#ifdef BR_BLINK
		if ( br_multi_process ) {
			br_proc_tick();
			if ( g_br.eng->gone() ) renderer_gone();
			br_proc_current(g_br.eng);
		}
#endif
		if ( quit_at >= 0 && now_ms() >= quit_at ) break;
		if ( next_command(&cmd) ) {
			/* what was chosen in the menu, which the window's thread showed */
			if ( !menu_do(cmd) ) done = true;
			continue;
		}
		if ( !g_later.empty() && g_panels_open.load() == 0 ) {
			/* a test's next "later=" line: the panels of the ones before answered, and the answers carried out */
			cmd = g_later.front();
			g_later.pop_front();
			if ( !menu_do(cmd) ) done = true;
			continue;
		}
		bool have = next_event(&m);
		if ( !have ) {
			/* nothing has come: the last move given, the engine's own work, and what it asks for */
			if ( g_br.moved ) {
				g_br.moved = false;
				g_br.eng->mouse(BR_M_MOVE, g_br.move_x, g_br.move_y, 0, g_br.move_mods);
			}
			if ( g_br.bar_pending ) {
				/* only the last place of a carried knob is drawn */
				g_br.bar_pending = false;
				scroll_bar(g_br.bar_which, g_br.bar_pos);
			}
			if ( g_br.eng->pump() ) {
				relayout();
				show();
				g_br.title = g_br.eng->title();
				show_title();
				g_br.eng->frame_shown();
			}
			/* a title the page's script set later: on the window */
			{
				std::string t = g_br.eng->title();
				if ( t != g_br.title || t != title_kept ) {
					g_br.title = t;
					show_title();
					if ( o.ok && t != title_kept ) printf("browser: title %s\n", t.c_str());
				}
				title_kept = t;
				/* the band again: the badge (navigator.setAppBadge) may have changed alone */
				show_title();
			}
			/* what the page's memory is made of, on the console as the kernel's mem: line is */
			if ( now_ms() - mem_said >= 30000 ) {
				mem_said = now_ms();
				printf("browser: memory: %s\n", g_br.eng->memory(false).c_str());
			}
			(void)follow_page();
			follow_doc();
			subs_tick();
			pick_tick();
#ifdef BR_BLINK
			br_store_tick();
#endif
			{
				/* until the next frame, the end of the run or what comes, whichever is first */
				int ms = subs_idle_ms(g_br.eng->idle_ms());
				if ( quit_at >= 0 ) ms = (int)std::max(1L, std::min((long)ms, quit_at - now_ms()));
#ifdef BR_BLINK
				(void)br_loop_wait(ms);
#else
				tk_dly_tsk(ms);
#endif
				/* the wait is no part of the turn's time (a slow turn is said, br_turn_ended) */
				g_beat = now_ms();
			}
			continue;
		}
		if ( sc.trace && m.event != OB_E_MOVE && m.event != OB_E_REDRAW ) {
			printf("browser: event %u code 0x%x mods 0x%x at %d,%d dz %d\n", m.event, m.code, m.mods,
			       m.x, m.y, m.dz);
		}
		if ( sc.trace && m.id != N_PAGE ) {
			printf("browser: event %u from %u record %d code 0x%x at %d,%d\n", m.event, m.id, m.recno,
			       m.code, m.x, m.y);
		}
		if ( m.id >= N_SUB ) {
			sub_event(m);
			continue;
		}
		if ( m.id == N_PARTS ) {
			if ( m.event == OB_E_CHANGE ) tool_switch(m.recno);
			continue;
		}
		if ( m.id == N_TOOL ) {
			/* the tool panel's own: its field, its keys, its drawing */
			if ( m.event == OB_E_PRESS && m.code == 0 ) tool_press(m.x, m.y);
			else if ( m.event == OB_E_KEY ) done = !key(m.code, m.mods, m.imode);
			else if ( m.event == OB_E_REDRAW ) tool_redraw();
			else if ( m.event == OB_E_CLOSE ) tool_close();	/* the panel alone goes */
			continue;
		}
		switch ( m.event ) {
		case OB_E_CLOSE:
		case OB_E_DELETE:
			done = true;
			break;
		case OB_E_KEY:
			if ( !key(m.code, m.mods, m.imode) ) done = true;
			break;
		case OB_E_PRESS:
			if ( !press(m) ) done = true;
			break;
		case OB_E_RELEASE:
			release(m);
			break;
		case OB_E_MOVE:
			/* only the last of a run of moves goes to the page */
			if ( g_br.eng->input() ) {
				g_br.moved = true;
				g_br.move_x = m.x;
				g_br.move_y = m.y;
				g_br.move_mods = m.mods;
			}
			break;
#ifdef BR_BLINK
		case OB_E_DROP:
			page_drop();
			break;
		case OB_E_LEAVE:
			if ( !g_br.eng->dragging().empty() ) {
				carry_out();
			} else {
				/* off the page: what was under the pointer is left (its mouseout, the message line) */
				g_br.eng->mouse(BR_M_MOVE, -1, -1, 0, 0);
				follow_doc();
			}
			break;
#endif
		case OB_E_REDRAW:
			window_fixed();
			if ( read_size() ) relayout();
			g_br.full = true;		/* what the window holds is to be drawn again whole */
			show();
			break;
		case OB_E_SCROLL:
			if ( m.y == OB_SCR_KNOB ) {
				g_br.bar_pending = true;
				g_br.bar_which = m.code;
				g_br.bar_pos = m.x;
			} else {
				g_br.bar_pending = false;
				scroll_bar(m.code, m.x);
			}
			break;
#ifdef OB_E_WHEEL
		case OB_E_WHEEL:
			if ( ( m.mods & kCtrl ) != 0 && m.dz != 0 && m.code != HID_WHEEL_H ) {
				zoom_step(m.dz > 0 ? 1 : -1);
			} else if ( g_br.eng->input() ) {
				/* the scroller under the pointer: the page's, or one in it */
				wheel_to(g_br.eng, m);
				after_input();
			} else if ( m.code != HID_WHEEL_H ) {
				/* the page the browser lays out itself scrolls only up and down */
				scroll_by(-m.dz * 48);
			}
			break;
#endif
		default:
			break;
		}
	}

	if ( !g_check.empty() ) {
		/* the views compared and the pixels that differed, as text */
		char n[64];
		int len = snprintf(n, sizeof(n), "%ld %ld\n", g_checked, g_wrong);
		INT fd = fs_open(g_check.c_str(), 0x0001 | 0x0100 | 0x0400);
		if ( fd >= 0 ) {
			(void)fs_write(fd, n, len);
			fs_close(fd);
		}
		printf("browser: check: %ld views compared, %ld pixels differed\n", g_checked, g_wrong);
	}
	if ( !g_gpucheck.empty() ) {
		/*
		 * The views and pixels compared with the processor's, those that
		 * differed, the largest difference; the frames of videos made RGB
		 * on the GPU and on the processor, the canvases' records rastered
		 * on the GPU, the views rastered on the GPU
		 */
		long shown = 0, planes = 0, rgb = 0, canvases = 0, gviews = 0;
#ifdef BR_BLINK
		long webgl = 0;
		media_frames(&shown, &planes, &rgb, &webgl);
		BrGpuStat gs;
		if ( gpu_stat(&gs) ) {
			canvases = gs.canvases;
			gviews = gs.views;
		}
		if ( br_multi_process && !br_is_renderer ) {
			/* the window's page's renderer compared its views (gpu_check_pixels) */
			std::string v = br_proc_query(main_engine(), "gpucheck");
			if ( sscanf(v.c_str(), "%ld %ld %ld %ld %d", &g_gc.views, &g_gc.px, &g_gc.differ, &g_gc.far, &g_gc.most) != 5 ) {
				g_gc = GpuCheck();
			}
		}
#endif
		char n[160];
		int len = snprintf(n, sizeof(n), "%ld %ld %ld %ld %d %ld %ld %ld %ld\n", g_gc.views, g_gc.px, g_gc.differ,
				   g_gc.far, g_gc.most, planes, rgb, canvases, gviews);
		INT fd = fs_open(g_gpucheck.c_str(), 0x0001 | 0x0100 | 0x0400);
		if ( fd >= 0 ) {
			(void)fs_write(fd, n, len);
			fs_close(fd);
		}
		printf("browser: gpucheck: %ld views, %ld pixels compared; %ld differed, %ld by more than %d, by %d at most\n",
		       g_gc.views, g_gc.px, g_gc.differ, g_gc.far, kFar, g_gc.most);
	}
	if ( g_pace.t0 != 0 && g_pace.views > 0 ) pace_say(now_ms() - g_pace.t0 > 0 ? now_ms() - g_pace.t0 : 1);
	if ( g_stat_on ) stat_print();
	/* the engine's thread ends by itself: the window's thread lets the process be */
	g_main_ended = true;
	/* where the window was, for the next time the object is opened (closing wrote it already) */
	if ( !g_closing ) save_place();
	if ( g_ime.kid > 0 ) kc_close(g_ime.kid);
	tool_close();
	while ( !g_subs.empty() ) sub_close(g_subs.back(), "the browser closed");
	(void)wm_msg_put("");
	printf("browser: closed\n");
	ob_cls_obj(g_br.port);
	(void)ob_del_obj(&ch);
	ob_cls_obj(g_br.kw);
	(void)ob_del_obj(&g_br.win);
	int rc = ( !sc.ready.empty() && g_br.failed ) ? 4 : 0;
#ifdef BR_BLINK
	if ( !sc.procs.empty() ) {
		/* what the renderers did, a line each, and the GPU process started last */
		if ( br_proc_gpu_pid() > 0 ) {
			proc_log("gpu pid=" + std::to_string(br_proc_gpu_pid()) + " renderer=" + br_proc_gpu_renderer());
			/* the window's GPU buffers: how they were filled, the sets made, the frames shown, the times let go */
			BrGsurfStat gs;
			br_proc_gsurf_stat(&gs);
			proc_log("gsurf how=" + ( gs.how.empty() ? std::string("none") : gs.how ) + " sets=" +
				 std::to_string(gs.sets) + " shown=" + std::to_string(gs.shown) + " drops=" +
				 std::to_string(gs.drops));
		}
		INT fd = fs_open(sc.procs.c_str(), 0x0001 | 0x0100 | 0x0400);
		if ( fd >= 0 ) {
			(void)fs_write(fd, g_proclog.data(), (SZ)g_proclog.size());
			fs_close(fd);
		}
	}
#endif
	if ( !sc.addr.empty() ) {
		/* the addresses the field showed, a line each */
		std::string all;
		for ( const std::string &a : g_addrs ) all += a + "\n";
		INT fd = fs_open(sc.addr.c_str(), 0x0001 | 0x0100 | 0x0400);
		if ( fd >= 0 ) {
			(void)fs_write(fd, all.data(), (SZ)all.size());
			fs_close(fd);
		}
	}
	if ( !sc.title.empty() ) {
		/* the title of the page shown last, as it is */
		INT fd = fs_open(sc.title.c_str(), 0x0001 | 0x0100 | 0x0400);
		if ( fd >= 0 ) {
			(void)fs_write(fd, g_br.title.data(), (SZ)g_br.title.size());
			fs_close(fd);
		}
	}
#ifdef BR_BLINK
	if ( !sc.sound.empty() ) {
		/* how many frames Web Audio gave the sound device, as text */
		char n[32];
		long frames = br_multi_process ? atol(br_proc_query(g_br.eng, "webaudio").c_str()) : br_web_audio_frames();
		int len = snprintf(n, sizeof(n), "%ld\n", frames);
		INT fd = fs_open(sc.sound.c_str(), 0x0001 | 0x0100 | 0x0400);
		if ( fd >= 0 ) {
			(void)fs_write(fd, n, len);
			fs_close(fd);
		}
		printf("browser: web audio: %ld frames played\n", frames);
	}
	if ( br_snd_frames() > 0 ) {
		printf("browser: snd: %ld frames given to the sound device\n", br_snd_frames());
	}
	if ( !sc.proto.empty() ) {
		/* the responses by the protocol they came over, as text */
		char n[96];
		int len;
		if ( br_multi_process ) {
			std::string v = br_proc_query(g_br.eng, "protocols");
			len = snprintf(n, sizeof(n), "%s", v.c_str());
		} else {
			len = br_net_protocols(n, sizeof(n));
		}
		INT fd = fs_open(sc.proto.c_str(), 0x0001 | 0x0100 | 0x0400);
		if ( fd >= 0 ) {
			(void)fs_write(fd, n, len);
			fs_close(fd);
		}
		printf("browser: net: responses by protocol: %s", n);
	}
	if ( !sc.print.empty() ) {
		/* the pages printed and the desktop's last answer, as text */
		char n[48];
		int len;
		if ( br_multi_process ) {
			std::string v = br_proc_query(g_br.eng, "print");
			len = snprintf(n, sizeof(n), "%s", v.c_str());
		} else {
			len = br_print_report(n, sizeof(n));
		}
		INT fd = fs_open(sc.print.c_str(), 0x0001 | 0x0100 | 0x0400);
		if ( fd >= 0 ) {
			(void)fs_write(fd, n, len);
			fs_close(fd);
		}
	}
	if ( !g_gpufake.empty() && ts_gl_probe_result != nullptr && !( br_multi_process && g_gpuproc ) ) {
		/*
		 * the driver the EGL platform took and the step it got to, as text:
		 * of the process that tried the GPU, the page's renderer when there
		 * are renderers (this one never tries it), this one alone otherwise;
		 * the GPU process writes its own
		 */
		char drv[16], n[48];
		int len;
		if ( br_multi_process ) {
			std::string v = br_proc_query(g_br.eng, "gpuprobe");
			len = snprintf(n, sizeof(n), "%s", v.c_str());
		} else {
			int step = ts_gl_probe_result(drv, sizeof(drv));
			len = snprintf(n, sizeof(n), "driver=%s step=%d\n", drv, step);
		}
		INT fd = fs_open(g_gpufake.c_str(), 0x0001 | 0x0100 | 0x0400);
		if ( fd >= 0 ) {
			(void)fs_write(fd, n, len);
			fs_close(fd);
		}
		printf("browser: gpu: the pretended GPU: %s", n);
	}
	/*
	 * The engine's threads and its objects are not taken down one by one:
	 * the process ends here, and the system takes back what it had
	 */
#ifdef BR_BLINK
	br_settings_close();
	/* the downloads still coming stopped, their objects not kept half made, their rows written */
	br_downloads_close(3000);
	/* the renderers write what they keep and end; their memory goes back */
	if ( br_multi_process ) br_proc_end();
#endif
	br_store_flush_all();
	apart_wait();
	fflush(stdout);
	br_console_drain(5000);
	_exit(rc);
#endif
	return rc;
}

#ifdef BR_BLINK
void br_main_command( const std::string &code )
{
#ifdef BR_BLINK
	if ( br_is_renderer ) {
		br_rproc_main_command(code);
		return;
	}
#endif
	std::lock_guard<std::mutex> g(g_ev_lock);
	g_commands.push_back(code);
	loop_wake();
}
#endif
