/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_proc.h
 *	ブラウザ: the browser's process and its renderers (design 14.13).
 *
 *	The browser's process (br_main.cc's window, br_proc.cc) keeps the
 *	windows, the input, the converter, the menus, the panels and the
 *	session history. Each renderer (br_rproc.cc) is the same program
 *	started again with the line "renderer" first: it runs Blink and V8
 *	for the pages of one site, with the stores and the GPU of its own;
 *	the network process (port/net/net_service_tessronos.cc) holds the one
 *	network stack, the cookie jar, the HTTP cache and what they keep.
 *	A page of a renderer is a BrEngine in the browser's
 *	process (br_proc.cc's RemotePage) whose calls go over Mojo
 *	(mojom/br_process.mojom).
 *
 *	br_proc.cc and br_rproc.cc include none of the system's headers:
 *	what they ask of the system goes through the br_sys_ functions that
 *	br_main.cc defines.
 */

#ifndef __BR_PROC_H__
#define __BR_PROC_H__

#include <stdint.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"

class BrEngine;
struct BrObjLink;
struct BrShareAsk;
struct BrShareChoice;
struct BrRect;

/* ---------------------------------------------------------------- the kind of process */

/* The first line of a renderer's start-up argument, and of the network process's */
#define BR_RENDERER_LINE	"renderer"
#define BR_NETWORK_LINE		"network"
#define BR_GPU_LINE		"gpu"

/*
 * Whether the window's pages are run by renderers of their own (the
 * default with Blink; a test's "process=single" keeps them in the
 * browser's process)
 */
extern bool	br_multi_process;

/* This process is a renderer, the network process or the GPU process */
extern bool	br_is_renderer;
extern bool	br_is_network;
extern bool	br_is_gpu;

/*
 * The calling process restricted (design 14.13.9; br_main.cc), as the
 * start-up line "sandbox=" says (off, refuse, kill): the renderer's set,
 * the network process's
 */
extern int	br_sandbox_mode;
void		br_sandbox_renderer();
void		br_sandbox_network();

/*
 * What a page saves, under 'name': in a renderer handed to the browser,
 * which keeps every download (br_rproc.cc, Host.KeepDownload; false when
 * it did not take them), and kept by the process that has the window
 * (br_blink.cc: ブラウザの保存箱, the tray, the list of downloads)
 */
bool		br_rproc_keep_download( const std::string &name, const std::string &bytes );

/*
 * In a renderer: whether 中止 has been asked for the navigation 'seq'
 * (Control.StopLoad naming it), which may come before the navigation has
 * begun on the main thread
 */
bool		br_rproc_stop_asked( unsigned seq );
void		br_keep_download( const std::string &name, std::string bytes );

/*
 * A download from the network, kept by the process that has the window
 * (br_blink.cc; design 14.7.4): its bytes taken in as a real object in
 * ブラウザの保存箱 as they come, by a thread of its own, its row in the
 * list of downloads moving with them. Begun with the name it is saved
 * under, its address, its length (-1 when not said) and its status (a
 * status that is not a success saves nothing: null), and what stops its
 * request where it comes from (called once, when it stops here first:
 * 中止 in the list, no byte for a minute, the object not made, the
 * browser closing). Each piece is handed with what tells its source it
 * has been taken (called once, any thread), so that no more than a
 * window of it is held; the end says whether all came well. What did
 * not end well leaves no object behind.
 */
struct BrDownload;
std::shared_ptr<BrDownload> br_download_begin( const std::string &name, const std::string &url, long long total,
					       int status, std::function<void()> stop );
void		br_download_bytes( const std::shared_ptr<BrDownload> &d, std::string piece,
				   std::function<void()> taken );
void		br_download_finish( const std::shared_ptr<BrDownload> &d, bool ok );
/* Whether it has stopped or ended here (its source may be let go) */
bool		br_download_over( const std::shared_ptr<BrDownload> &d );
/*
 * The browser closing: the downloads under way stopped as 中止 is, their
 * objects not kept, waited for up to 'wait_ms'
 */
void		br_downloads_close( int wait_ms );

/* The network process's life (port/net/net_service_tessronos.cc): it never returns */
void		br_network_main();

/* The network process's number, 0 while there is none */
int		br_proc_network_pid();

/* The number of the GPU process started last, 0 when none was (br_proc.cc) */
int		br_proc_gpu_pid();
/* And the GL it is on ("" when it has none) */
std::string	br_proc_gpu_renderer();
/* Something asked of the GPU process (GpuService.Query), waited for; "" without one */
std::string	br_proc_gpu_query( const std::string &what );
/*
 * The GPU process running now (0: none), whether it is not to be started
 * again (it ended three times within a minute), how many times it was
 * started again for a GPU that came after it, and the one let go of the
 * last time
 */
void		br_proc_gpu_state( int *pid, bool *none, int *arrivals, int *arrived_from );

/*
 * Mojo's core brought up (br_base_up): within the process alone, as the
 * broker of the browser's processes, or in a renderer, with the IO
 * thread their channels are watched on
 */
void		br_mojo_init();

/* ---------------------------------------------------------------- the browser's side (br_proc.cc) */

/*
 * //base and Mojo brought up as the broker of the processes, and the
 * first renderer started; 'lines' are the start-up lines a renderer is
 * given as the browser was (a test's v8=, features=, ...), 'window' the
 * UUID of the page's window
 */
bool		br_proc_start( const std::string &lines, const unsigned char window[16] );

/*
 * A page for the window in a renderer of its own: the one kept ready,
 * or one started now (it is waited for); null when none can be had.
 * Another is made ready meanwhile.
 */
BrEngine	*br_proc_new_page();

/*
 * A renderer's page let go: the page closed, and, when its renderer
 * shows no other, the renderer asked to write what it keeps, ended and
 * waited for, so that its memory goes back to the system
 */
void		br_proc_close( BrEngine *eng );

/*
 * The renderers kept (the one ready among them), and how many there may
 * be: by the machine's memory, unless set (a test's "renderers=")
 */
int		br_proc_count();
int		br_proc_limit();
void		br_proc_set_limit( int n );
/* The renderers that show pages (not the one ready) */
int		br_proc_live();

/*
 * A renderer made ready for the next page of another site, unless there
 * is one: started on a thread of its own (the window goes on meanwhile)
 */
void		br_proc_keep_ready();

/* The process number of a page's renderer; 0 for none */
int		br_proc_pid( BrEngine *eng );

/* A page's renderer ended at once (kill), as a renderer that fails ends */
bool		br_proc_kill( BrEngine *eng );

/*
 * A test's ("killnav=N"): the renderer of the N-th navigation the browser
 * sends from now on ended as one that fails ends, just after the
 * navigation is sent and before its answer: the renderer goes while the
 * page it shows is left; 0 for none
 */
extern int	br_proc_test_killnav;

/*
 * The site of an address as renderers are kept apart by (its scheme and
 * registrable domain); "" for an address of no site (about:, data:),
 * whose page stays in the renderer that shows the window
 */
std::string	br_proc_site( const std::string &url );

/*
 * An extension's object the user chose (or a test's line named): the
 * renderers may then read it and link it to the list, through the network
 * process, which reads no other object for them
 */
void		br_proc_ext_allow( const std::string &uuid );

/*
 * Commands and questions for the engine's side of a page, carried by its
 * renderer: a menu's code (what came of it for the message line), a value
 * a test reads by name, the stores written
 */
std::string	br_proc_command( BrEngine *eng, const std::string &code );
std::string	br_proc_query( BrEngine *eng, const std::string &what );
/*
 * The question asked and not waited for: 'told' is given the answer when it
 * comes (on the engine thread, in its loop), and not when the renderer goes
 * first. False, and nothing asked, while the same question of the same
 * renderer is still unanswered (what the window's loop says of its drawing
 * every few seconds, which is not to wait for a page's script)
 */
bool		br_proc_query_async( BrEngine *eng, const std::string &what, base::OnceCallback<void( std::string )> told );
bool		br_proc_flush( BrEngine *eng, int wait_ms );

/* From any thread: the load of a page's renderer stops (中止; 'now' also cuts what is under way) */
void		br_proc_stop_load( BrEngine *eng, bool now );

/*
 * What the engine thread's wait for a renderer's answer to a navigation
 * goes by, asked each time round the wait (br_main.cc): BR_WAIT_ON, the
 * answer is waited for; BR_WAIT_STOPPING, 中止 has been asked (the
 * renderer is told to stop, and its script ended when it holds it);
 * BR_WAIT_HELD, 中止 has not been taken in the time the window gives an
 * end (the renderer is held in work it cannot stop, and is ended);
 * BR_WAIT_CLOSING, the window goes (the renderer is ended with it)
 */
enum { BR_WAIT_ON, BR_WAIT_STOPPING, BR_WAIT_HELD, BR_WAIT_CLOSING };
int		br_nav_wait_verdict();

/* What the window's menu shows of the page, from any thread: its origin's isolated state, the extensions' buttons */
bool		br_proc_isolated( BrEngine *eng );
std::vector<std::string> br_proc_ext_actions( BrEngine *eng );
/* What the extensions added for what the last right press found */
std::vector<std::string> br_proc_ext_items( BrEngine *eng );
/* What the window's band says before the title: the page's badge, its screen shared */
std::string	br_proc_badge( BrEngine *eng );
std::string	br_proc_capture( BrEngine *eng );

/*
 * A video's picture-in-picture window, as the renderer asked for it: the
 * next ask and the page that told it, closed or pressed (play), the
 * player's frame drawn into a canvas of the window's, whether it is
 * paused and how many frames it has shown
 */
struct BrPipAsk;
class SkCanvas;
bool		br_proc_pip_next( BrPipAsk *a, BrEngine **from );
void		br_proc_pip_closed( BrEngine *from, int session );
void		br_proc_pip_press( BrEngine *from, int session, bool play );
bool		br_proc_paint_player( BrEngine *from, int session, int player, SkCanvas *c, int w, int h );
bool		br_proc_player_pixels( BrEngine *from, int session, SkCanvas *c );
/* The window of a video told that its player's frame asked for by br_proc_paint_player has come */
extern void	( *br_proc_player_painted )( BrEngine *from, int session );
bool		br_proc_player_paused( BrEngine *from, int player );
long		br_proc_player_frames( BrEngine *from, int player );

/* A real object let go over the page, kept in its renderer as a file the page reads; its path */
std::string	br_proc_keep_file( BrEngine *eng, const std::string &uuid, const std::string &name,
				   const std::vector<uint8_t> &bytes );

/* Widevine's state as the page's renderer has it, from any thread: being installed, there */
void		br_proc_wv_state( BrEngine *eng, bool *busy, bool *installed );

/* The window's page now, whose renderer the settings window asks (br_uidata.h) */
void		br_proc_current( BrEngine *eng );

/* Mojo's tasks of the browser's engine thread, run when it comes round; from another thread nothing */
void		br_proc_tick();

/*
 * The wait of the thread that runs a process's loop (the browser's
 * engine thread, a renderer's main thread), in the task loop of that
 * thread (Blink's scheduler, or the browser's Mojo tasks): up to ms, and
 * no longer than until a task has run there and none is left. The tasks
 * of that thread (a timer of a page, what another thread posts, a pipe's
 * message bound there) thus run while it waits, and the first of them
 * ends the wait, for the loop to see what came of it. br_loop_wake, from
 * any thread, posts a task there that does nothing and so ends the wait.
 * br_loop_bind is called by that thread before it first waits (and
 * before another thread may wake it). br_loop_wait answers whether a task
 * ended the wait (false: the time ran out).
 */
#define BR_WAIT_MOST_MS	1000	/* the longest wait, when nothing is due */
void		br_loop_bind();
bool		br_loop_wait( int ms );
void		br_loop_wake();

/*
 * For how long that thread has been in its work: since it last came
 * round its loop (br_loop_round, each time round) or a task began to run
 * in its wait; 0 while it waits with no task running. Read from any
 * thread (a renderer's IO thread answers the browser's Busy with it)
 */
void		br_loop_round();
long		br_loop_busy_ms();

/*
 * For how long the main thread of the page's renderer has been in its
 * work (br_loop_busy_ms, asked over Control on its IO thread), from any
 * thread; 0 when it waits, has gone, or the page is the browser's own.
 * A page held longer than BR_HELD_MS is not asked what waits for its
 * main thread (the menu's items for what is under the pointer, its
 * stores at the end): the browser does without
 */
#define BR_HELD_MS	500
long		br_proc_busy_ms( BrEngine *eng );

/* The renderer's answer the engine thread waits for (a call, its renderer, how long), or "" */
std::string	br_proc_calling();

/* The renderers ended with the browser */
void		br_proc_end();

/* ---------------------------------------------------------------- the renderer's side (br_rproc.cc) */

/* A renderer's life: it never returns */
void		br_renderer_main();

/* What the browser said of the window with the last turn (br_screen_size, br_window_rect) */
bool		br_rproc_window( int *x, int *y, int *w, int *h );
void		br_rproc_screen( int *w, int *h );
void		br_rproc_window_uuid( unsigned char id[16] );

/* The panels and the browser's loop, asked of the browser (br_main.cc's br_ask_page and the others) */
bool		br_rproc_ask_page( int kind, const std::string &message, const std::string &deflt, std::string *answer );
bool		br_rproc_ask_permission( const std::string &origin, const std::string &what );
int		br_rproc_choose( const std::vector<std::string> &names, int chosen );
bool		br_rproc_choose_objects( bool multiple, std::vector<BrObjLink> *out );
bool		br_rproc_pick_color( uint32_t *color );
bool		br_rproc_share_choose( const std::string &origin, const BrShareAsk &ask, BrShareChoice *out );
bool		br_rproc_share_screen_rect( const BrShareChoice &c, int *x, int *y, int *w, int *h );
void		br_rproc_main_command( const std::string &code );
void		br_rproc_ext_ask_load();

/* ---------------------------------------------------------------- the system's side (br_main.cc) */

/*
 * A renderer started: the browser's program again, with 'arg' as its
 * start-up argument, ending with this process; its number, or a
 * negative number
 */
int		br_sys_spawn( const std::string &arg );

/* A descriptor handed to the process 'pid', and one taken by this process (waiting up to 'ms') */
bool		br_sys_give( int pid, int fd );
int		br_sys_take( int ms );

/* A process ended at once, and one waited for up to 'ms': true when it has ended */
void		br_sys_kill( int pid );
bool		br_sys_reap( int pid, int ms, int *status );

/* The machine's memory in MB, and how much of it is free now */
long		br_sys_memory_mb();

/* What the lines asked of the GPU: "gpu=on", "gpu=off" or neither (BR_GPU_*) */
int		br_sys_gpu_mode();

/*
 * The GPU process's (br_main.cc): "gpufake=" put in before its GL comes
 * up, and the driver and the step the GL's platform took written there
 * after (nothing without the line)
 */
void		br_sys_gpu_fake_begin();
void		br_sys_gpu_fake_end();

/*
 * Whether the machine has a GPU the system drives. When it has none yet,
 * 'arrived' is called once, on a thread of its own, when one is
 * registered (the GPU's device object tells of it), with what it is
 */
bool		br_sys_gpu_watch( void (*arrived)( const char *what ) );

/* Whether a renderer's page is rastered in the GPU process ("gpuproc=on") */
bool		br_sys_gpuproc();
long		br_sys_free_mb();

/* The engine of this process brought up (the face, the GPU, Blink), and a page of it */
bool		br_sys_engine_up();
BrEngine	*br_sys_engine_page();

/* A renderer's loop set up as the browser's engine thread's is (its stop, its task) */
void		br_sys_renderer_setup();

/* The load under way to stop (中止); 'now' cuts what is under way. And the process to end (閉じる). */
void		br_sys_stop( bool now );
void		br_sys_closing();

/* A navigation's progress in a renderer, on the message line; true when the user stopped it */
bool		br_sys_progress( long got, long total );

/* What the engine's side does after a page's turn in a renderer (its stores, the extensions' tab) */
void		br_sys_after_turn( BrEngine *eng, bool primary );

/*
 * The window's page about to go to 'url' in a renderer: the extensions'
 * tab is there from now on, as the browser of one process has it while
 * the page loads (its scripts ask the extensions' workers meanwhile)
 */
void		br_sys_tab_at( const std::string &url );

/* A renderer's command of the menu carried out on the engine's side; what came of it */
std::string	br_sys_command( BrEngine *eng, const std::string &code );
std::string	br_sys_query( const std::string &what );

/*
 * A view the renderer rastered on the GPU into its page's memory ('px',
 * 'pitch' bytes a row), against the same view painted on the processor
 * ("gpucheck="; nothing without it). The totals are the query "gpucheck".
 */
void		br_sys_gpu_check( BrEngine *eng, int top, int w, int h, const void *px, size_t pitch );

/* The stores of a renderer written, waiting up to 'wait_ms': true when all were */
bool		br_sys_flush( int wait_ms );

/* The extensions' buttons, and what they add to the menu for what a right press found */
std::vector<std::string> br_sys_ext_actions();
std::vector<std::string> br_sys_ext_items( const std::string &link, const std::string &image,
					   const std::string &selection, bool editable );

/* ---------------------------------------------------------------- the window's GPU buffers */

/*
 * The window's GPU buffers (design 14.13.10). While the GPU process
 * rasters a renderer's page, the page's window is shown from two BOs the
 * GPU process made (the window's record OB_WR_GSURF): the page's frames
 * are rastered into them in turn, and the window manager shows each from
 * its BO, so that nothing is read back and the processor copies nothing.
 *
 * On the browser's engine thread (br_proc.cc), each time the window's
 * page 'eng' (its view 'w' x 'h') is shown: br_proc_gsurf_follow gives
 * the page buffers of that size made by the GPU process running now, and
 * takes them away when 'want' is false (the converter's list over the
 * page, "gsurf=off", no GPU process) or the page is not shown any more.
 * br_proc_gsurf_show then shows the frame held, whose rectangles the
 * page's damage() gave: a frame of the buffers is shown from its BO and
 * true is answered (the view is neither painted nor put; true too while
 * the window shows a buffer and no frame came); before a frame of the
 * page's memory, painted as before (false), the window lets the buffers
 * go, keeping what they showed.
 */
void		br_proc_gsurf_follow( BrEngine *eng, int w, int h, bool want );
bool		br_proc_gsurf_show( BrEngine *eng, const std::vector<BrRect> &rects );
/*
 * From the window's thread: the window shows buffer 'idx' under 'serial'
 * (the notice OB_E_CHANGE of OB_WR_GSURF); the frame is given back to the
 * renderer at the engine thread's next pump of the page, wherever it waits
 */
void		br_proc_gsurf_shown( int idx, unsigned serial );
/*
 * What the buffers did, for the console and a test's "procs=": how the
 * GPU process fills them ("drawn", "copied", "" none), the sets made, the
 * frames shown from them and the microseconds from the window being
 * asked to show each to its saying so, the times the window let them go
 * for frames of the page's memory
 */
struct BrGsurfStat {
	std::string	how;
	long		sets = 0, shown = 0, shown_us = 0, drops = 0;
};
void		br_proc_gsurf_stat( BrGsurfStat *s );

/*
 * The browser's (br_main.cc): the window's record OB_WR_GSURF written --
 * the two BOs by PRIME descriptors of them (OB_GS_SET), the one to show
 * with what changed in it (OB_GS_SHOW, at most 16 rectangles, more made
 * all of it), none (OB_GS_DROP) -- E_OK or the system's error; what the
 * window shows now (-1 none) and under which serial
 */
int		br_sys_gsurf_set( const int fd[2], int w, int h, unsigned pitch );
int		br_sys_gsurf_show( int idx, unsigned serial, const std::vector<BrRect> &rects );
int		br_sys_gsurf_drop();
bool		br_sys_gsurf_state( int *shown, unsigned *serial );
/* Whether the window's buffers are to be used: "gsurf=off" says not */
bool		br_sys_gsurf_on();

/*
 * The GPU process's (br_main.cc): a BO for a window's buffer, 'size'
 * bytes, in the process's own context of the GPU, mapped into the process
 * and made an object (OB_S_GPUBO); not entered in the GPU's space when
 * 'gpu' is false (the GL draws on the processor). The object lasts while
 * a key to it is open, the process's own ('keep') or one handed on. A
 * PRIME descriptor of it holds a key of its own to the object (to be
 * handed on, or taken in by the GL's EGL); -1 when none can be had.
 */
struct BrGsurfBo {
	unsigned	handle = 0;
	void		*map = nullptr;
	size_t		size = 0;
	unsigned char	uuid[16] = {};
	int		keep = 0;
};
bool		br_sys_gsurf_bo( size_t size, bool gpu, BrGsurfBo *b, std::string *why );
int		br_sys_gsurf_bo_fd( const BrGsurfBo *b );
void		br_sys_gsurf_bo_free( BrGsurfBo *b );

/* A renderer's: whether the views it rasters are compared with the processor's ("gpucheck=") */
bool		br_sys_gpu_checking();

#endif /* __BR_PROC_H__ */
