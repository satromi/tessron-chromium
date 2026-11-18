/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br.h
 *	ブラウザ: the parts of the application (design 17.19).
 *
 *	br_main.cc is the window: the address, the page, the status line, the
 *	keys and the pointer. br_http.cc fetches what an address names over
 *	the sockets of the system. The page is laid out and painted by an
 *	engine behind the interface below: br_text.cc lays out the text of
 *	the HTML with its headings, lists, preformatted text and links, and
 *	paints it with Skia; Blink stands behind the same interface when it
 *	is built. br_obj.cc reads and writes the page object the application
 *	is opened on.
 */

#ifndef __BR_H__
#define __BR_H__

#include <stddef.h>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class SkCanvas;
class SkData;
class SkTypeface;
template <typename T> class sk_sp;

/* ---------------------------------------------------------------- fetching (br_http.cc) */

struct BrUrl {
	std::string	scheme;		/* "http", "https", "about" */
	std::string	host;
	int		port = 0;
	std::string	path;		/* from the first '/', with the query */
	std::string	fragment;
};

bool		br_url_parse( const std::string &s, BrUrl *u );
std::string	br_url_string( const BrUrl &u );
std::string	br_url_resolve( const std::string &base, const std::string &ref );

struct BrResponse {
	int		status = 0;
	std::string	url;		/* where it came from, after redirects */
	std::string	type;		/* Content-Type */
	std::string	charset;	/* from Content-Type, "" when not said */
	std::string	disposition;	/* Content-Disposition, "" when not said */
	std::string	body;
	std::string	error;		/* what went wrong, "" when it came */
	std::string	code;		/* and the network stack's name for it ("ERR_NAME_NOT_RESOLVED"), "" when none */
	std::string	headers;	/* the response's header lines ("Name: value\r\n" each), "" when not known */
	std::string	protocol;	/* what it came over ("h3", "h2", "http/1.1"), "" when not known */
	int		answer = -1;	/* a window's navigation: what it was decided to be at its response
					   (BR_ANSWER_*, br_nav_head_fetch), -1 when not yet */
};

/*
 * The thing an address names, following redirects. 'progress' is told
 * how much has come (bytes, and the whole when it is known, else -1).
 * A form sent with POST gives its method, the type of its body and the
 * body.
 */
bool br_fetch( const std::string &url, BrResponse *r,
	       void (*progress)( void *ctx, long got, long total ), void *ctx,
	       const std::string &method = std::string(), const std::string &ctype = std::string(),
	       const std::string &body = std::string() );

/*
 * A window's navigation being fetched by br_fetch (go): the address of
 * the page that asked for it, empty when the user did. Its request then
 * carries what a browser puts on a navigation's (br_navigation_headers,
 * with Blink), and the response's Accept-CH is learnt.
 */
extern bool		br_nav_active;
extern std::string	br_nav_from;
std::string	br_navigation_headers( const std::string &url, const std::string &from ) __attribute__((weak));
/*
 * The times of a window's navigation, for its document's timing: 2 when
 * the window is told to go, 0 when its request is sent, 1 when the answer
 * has come
 */
void		br_navigation_timing( int phase ) __attribute__((weak));

/*
 * A window's navigation fetched as it comes and decided at its response
 * (br_blink.cc): a page with its whole body, a download handed over to be
 * kept as it comes, an answer with an error page. 1 with what came (and
 * 'answer', BR_ANSWER_*), 0 when it failed or was given up, -1 when it
 * could not be asked for so (br_fetch then waits for it whole)
 */
struct BrNavFetched {
	int		answer = -1;
	int		status = 0;
	std::string	url, ctype, headers, body;
};
int		br_nav_head_fetch( const std::string &url, const std::string &method, const std::string &body,
				   const std::string &hdrs, int timeout_ms, BrNavFetched *out ) __attribute__((weak));

/*
 * The cookies of the process: set from a Set-Cookie line or a script's
 * document.cookie (which may not touch HttpOnly ones), and got as a
 * Cookie header's value for an address. br_fetch sends and takes them.
 */
void		br_cookie_set( const std::string &url, const std::string &line, bool from_script );
std::string	br_cookie_get( const std::string &url, bool for_script );

/* ---------------------------------------------------------------- the engine */

/* A rectangle of the view, in pixels */
struct BrRect {
	int	x = 0, y = 0, w = 0, h = 0;
};

class BrEngine;
struct BrNav;
struct BrNavResult;

/*
 * A window a page opened that keeps its opener (window.open, a document's
 * picture-in-picture window): its page, in the same engine, and where and
 * how large the page asked it be
 */
struct BrOpened {
	BrEngine	*eng = nullptr;
	int		x = -1, y = -1;		/* on the screen, -1 where the page did not say */
	int		w = 0, h = 0;		/* its page area */
	bool		pip = false;		/* a document's picture-in-picture window */
};

class BrEngine {
public:
	virtual ~BrEngine() {}

	/*
	 * The windows the page opened since the last call, one at a time:
	 * those that keep their opener, as engines of their own the
	 * application shows in windows of its own (opened()), and the
	 * addresses of those opened with no opener (a link to _blank), which
	 * the application opens in browsers of their own (apart(), "" for
	 * none)
	 */
	virtual bool		opened( BrOpened *o ) { (void)o; return false; }
	virtual std::string	apart() { return std::string(); }

	/* An extension's own page opened in a window of this process */
	virtual void		open_page( const std::string &url ) { (void)url; }

	/*
	 * The page's script closed its window (window.close()), or its
	 * opener's page went and took a picture-in-picture window with it;
	 * and the window gone, the page let go (the engine is then deleted)
	 */
	virtual bool	close_asked() { return false; }
	virtual void	closed() {}

	/*
	 * A page: its address (for resolving links), its bytes, their charset
	 * and their type (the bare type of the Content-Type; "" for HTML)
	 */
	virtual void	load( const std::string &url, const std::string &body,
			      const std::string &charset, const std::string &type ) = 0;

	/*
	 * Laid out for a width; the height of the whole page. A page of a
	 * renderer is told the width and answers when it has laid out: the
	 * height is the last it told (what it says with its state), and a
	 * page grown or laid out for the new width comes with its next state
	 */
	virtual int	layout( int width ) = 0;

	/*
	 * The part of the page from 'top', 'height' high, drawn at (0, 0),
	 * as far as the canvas is clipped
	 */
	virtual void	paint( SkCanvas *c, int top, int height ) = 0;

	/*
	 * As paint(), the view 'width' x 'height' from 'top', but rastered on
	 * the GPU (port/gpu/br_gpu.cc) in the rectangles 'rects' and read
	 * back into the pixels at 'px' (N32, 'pitch' bytes a row); false when
	 * the engine or the GPU cannot, and nothing was drawn
	 */
	virtual bool	paint_gpu( const std::vector<BrRect> &rects, int top, int width, int height,
				   void *px, size_t pitch )
	{
		(void)rects; (void)top; (void)width; (void)height; (void)px; (void)pitch;
		return false;
	}

	/*
	 * What of the view has changed since it was last asked, for an
	 * engine that knows: the page brought up to date for the view at
	 * 'top', 'height' high, and the rectangles of the view to paint
	 * again put in 'out' (none when nothing changed). paint() then
	 * draws that state as it is. False when the engine cannot say, and
	 * the whole view is to be painted.
	 */
	virtual bool	damage( int top, int height, std::vector<BrRect> *out )
	{
		(void)top; (void)height; (void)out;
		return false;
	}

	/* The link at a point of the page, "" for none */
	virtual std::string	link_at( int x, int y ) = 0;

	virtual std::string	title() = 0;

	/*
	 * The engine's own work since the last call (its tasks, timers and
	 * loads); true when the page is to be painted again
	 */
	virtual bool	pump() { return false; }

	/*
	 * After a pump that answered true, once the page has been painted
	 * and shown: the engine learns what its frame took. And how long the
	 * application's loop may sleep when nothing has come, in ms, so that
	 * the next frame the page waits for is not late.
	 */
	virtual void	frame_shown() {}
	virtual int	idle_ms() { return 10; }

	/*
	 * From another thread than the engine's, which a page holds: the
	 * script running now is ended (中止, closing), and where the page's
	 * script is is printed once the engine's thread next looks for
	 * interrupts (a script running on looks often)
	 */
	virtual void	stop_script() {}
	virtual void	report_script() {}

	/* An address the page asked to go to (a link, a form, a script), "" for none */
	virtual std::string	navigation() { return std::string(); }

	/*
	 * The session history, for an engine that keeps it (history() true):
	 * the entries its documents committed, those the page's own
	 * history.pushState and fragments made among them. history_go moves
	 * 'offset' entries back (negative) or forward: BR_HIST_NONE when there
	 * is no such entry, BR_HIST_DONE when the engine went there itself
	 * (an entry of the document shown, whose popstate it fires), and
	 * BR_HIST_FETCH when the application is to fetch the entry's address,
	 * which navigation() gives next. can_go says whether such an entry is
	 * there. The application's own list of addresses serves an engine
	 * that keeps none.
	 */
	virtual bool	history() { return false; }
	virtual int	history_go( int offset ) { (void)offset; return 0; }
	virtual bool	can_go( int offset ) { (void)offset; return false; }

	/*
	 * How the next load() commits (BR_LOAD_*): as a new entry, as the
	 * entry shown loaded again, or as the entry history_go went to;
	 * BR_LOAD_PAGE takes what the page's own navigation asked (a link, a
	 * location.replace(), its history.back()). The engine forgets it after
	 * each load().
	 */
	virtual void	load_as( int how ) { (void)how; }

	/*
	 * What the next load()'s bytes were answered with: the status, the
	 * header lines (its Content-Security-Policy, Permissions-Policy,
	 * Refresh, Link... apply to the document as they do in Chrome) and the
	 * protocol. The engine forgets it after each load().
	 */
	virtual void	response( int status, const std::string &headers, const std::string &protocol )
	{
		(void)status; (void)headers; (void)protocol;
	}

	/*
	 * The address of the document shown now, as the page has it (its
	 * pushState, replaceState and fragments followed); "" when the engine
	 * cannot say
	 */
	virtual std::string	url() { return std::string(); }

	/*
	 * Whether the page may be left (for a load the application starts,
	 * or loads again): its beforeunload handlers asked, and the user
	 * when one asks to stay. True when nothing holds it.
	 */
	virtual bool	may_leave( bool reload ) { (void)reload; return true; }
	/* Whether any of the page's documents has a beforeunload handler (may_leave asks only then) */
	virtual bool	beforeunload() { return true; }

	/*
	 * What the message line is to say of the place under the pointer: the
	 * address of the link there, else the tooltip (a title attribute), ""
	 * for nothing
	 */
	virtual std::string	hover() { return std::string(); }

	/* The page drawn larger or smaller (拡大・縮小): 1 is as the page is */
	virtual void	zoom( double factor ) { (void)factor; }

	/*
	 * The page asked for full screen (element.requestFullscreen) or to
	 * leave it: BR_FS_ENTER, BR_FS_EXIT, 0 for nothing, asked once. The
	 * application then shows the page over the whole screen, or as it
	 * was, and tells the engine with fullscreen_done.
	 */
	virtual int	fullscreen_asked() { return 0; }
	/*
	 * What the page asked of its window with the windowing controls
	 * (minimize, maximize, restore, setResizable), once: BR_WIN_*, 0 none
	 */
	virtual int	window_asked() { return 0; }
	virtual void	fullscreen_done( bool full ) { (void)full; }

	/*
	 * The right button pressed on the page at a point of the view: the
	 * page's contextmenu event; true when the page took it for itself
	 * (the window's menu is not shown then). 'what' is told what is
	 * there: the link's address, an image's address, the text chosen.
	 */
	struct Context {
		std::string	link, image, selection;
		bool		editable = false;
	};
	virtual bool	context( int x, int y, Context *what ) { (void)x; (void)y; (void)what; return false; }

	/* A command of the editor on the page: "Copy", "Cut", "Paste", "SelectAll" */
	virtual void	edit( const std::string &command ) { (void)command; }

	/* What an address names, saved as a real object in the save box (〈画像を保存〉) */
	virtual void	save_url( const std::string &url ) { (void)url; }

	/*
	 * Find in the page: the matches of 'text' marked, one gone to (step 0
	 * the first, 1 the next, -1 the one before); how many there are, and
	 * which one is gone to (from 1) in 'active'. "" takes the marks away.
	 */
	virtual int	find( const std::string &text, int step, int *active )
	{
		(void)text; (void)step;
		*active = 0;
		return 0;
	}

	/*
	 * What goes with the last address navigation() gave: a form sent
	 * with POST gives its method, the type of its body and the body;
	 * false for a plain GET
	 */
	virtual bool	navigation_body( std::string *method, std::string *ctype, std::string *body )
	{
		(void)method; (void)ctype; (void)body;
		return false;
	}

	/*
	 * A press of the left button at a point of the page, given to the
	 * page itself (its scripts, its links and controls); false when the
	 * engine leaves presses to the window (which follows link_at)
	 */
	virtual bool	click( int x, int y ) { (void)x; (void)y; return false; }

	/* 印刷 of the page shown (the menu's 〈印刷...〉); false when it was not printed */
	virtual bool	print() { return false; }

	/*
	 * Links let go over the page from another window, each a file kept
	 * under a path (br_file_keep), or letters ('text'), at a point of the
	 * view: dropped there as a browser drops what is dragged in from
	 * outside. False when the page did not take them.
	 */
	virtual bool	drop( int x, int y, const std::vector<std::pair<std::string, std::string>> &files,
			      const std::string &text, unsigned mods )
	{
		(void)x; (void)y; (void)files; (void)text; (void)mods;
		return false;
	}

	/*
	 * What the page is dragging, as letters (an address, or text), ""
	 * when it drags nothing; and, when the pointer has left the page
	 * with it, the drag ended there: carried into another window
	 * ('taken') or let go where nothing took it
	 */
	virtual std::string	dragging() { return std::string(); }
	virtual void	drag_away( bool taken ) { (void)taken; }

	/* The address of the page's first link, "" when the engine cannot say */
	virtual std::string	first_link() { return std::string(); }

	/*
	 * Input given to the page itself, for an engine that runs the page
	 * (input() true). The pointer: a button down or up (BR_M_*, button
	 * 0 left, 2 right) or a move, at a point of the view (not of the
	 * whole page). A key: its HID usage, the modifiers and the letter it
	 * types (0 for none); true when the page took it, false when it is
	 * left to the window. The wheel: notches across, to the right positive,
 * and up and down, down positive.
	 */
	virtual bool	input() { return false; }
	virtual void	mouse( int type, int x, int y, int button, unsigned mods )
	{
		(void)type; (void)x; (void)y; (void)button; (void)mods;
	}
	virtual bool	key( unsigned usage, unsigned mods, unsigned ch )
	{
		(void)usage; (void)mods; (void)ch;
		return false;
	}
	virtual void	wheel( int x, int y, int right, int down ) { (void)x; (void)y; (void)right; (void)down; }

	/*
	 * Where the view is in the page, for an engine that scrolls the page
	 * itself; -1 when the window scrolls it (paint's 'top')
	 */
	virtual int	scroll_top() { return -1; }

	/*
	 * The page's width, where the view is across it and a move there,
	 * for an engine that scrolls the page itself; 0 for a page no wider
	 * than the view
	 */
	virtual int	doc_width() { return 0; }
	/* The height of the whole page as it was last laid out, with no layout now; 0 when it cannot say */
	virtual int	doc_height() { return 0; }
	virtual int	scroll_left() { return 0; }
	virtual void	set_scroll_left( int x ) { (void)x; }

	/*
	 * Text input: whether an editable part of the page has the focus;
	 * the composition of the kana-kanji converter shown in it (UTF-8,
	 * the clauses as byte offsets from 0 to its end, the one being
	 * converted, the caret as a byte offset), "" to take it away; text
	 * committed into it; the caret's box in the view.
	 */
	virtual bool	editable() { return false; }
	virtual void	compose( const std::string &text, const std::vector<int> &clauses,
				 int target, int caret )
	{
		(void)text; (void)clauses; (void)target; (void)caret;
	}
	virtual void	commit( const std::string &text ) { (void)text; }
	virtual bool	caret( int *x, int *y, int *h ) { (void)x; (void)y; (void)h; return false; }

	/* What the engine holds in memory, after collecting its garbage when asked */
	virtual std::string	memory( bool collect ) { (void)collect; return std::string(); }

	/*
	 * A navigation fetched and shown (br_nav_fetch_load): by the engine's
	 * own process, or by the renderer whose page this is
	 */
	virtual int	navigate( const BrNav &nav, BrNavResult *out );

	/*
	 * The session history the engine keeps, each entry's state encoded as
	 * a page state (its address, state object, scroll, form state), and the
	 * one shown; and such a history given back to a new engine, which
	 * then commits the entry 'at' as the next load (BR_LOAD_HISTORY)
	 */
	virtual bool	history_save( std::vector<std::string> *states, std::vector<std::string> *urls, int *at )
	{
		(void)states; (void)urls; (void)at;
		return false;
	}
	virtual void	history_restore( const std::vector<std::string> &states, int at ) { (void)states; (void)at; }
	/* The entries after the one shown gone, as a new entry in another renderer takes their place */
	virtual void	history_cut() {}

	/* The renderer whose page this is is gone (it ended, or was ended): nothing is drawn or asked of it */
	virtual bool	gone() { return false; }

	/*
	 * For tests: the script at an address fetched and run in the page's
	 * main world when it comes, as the page's own scripts run
	 */
	virtual void	run_script( const std::string &url ) { (void)url; }
};

/*
 * A navigation of a window (go): where to, how it commits (BR_LOAD_*), a
 * form's body, the page that asked ("" when the user did), and an address
 * typed with no scheme (https first, http when it fails). An entry gone
 * back to in a new renderer comes with the history the browser kept.
 */
struct BrNav {
	std::string	target;
	int		how = 0;
	std::string	method, ctype, body;
	std::string	from;
	bool		upgraded = false;
	std::vector<std::string> restore;	/* the entries' page states, empty for none */
	int		restore_at = -1;
	unsigned	seq = 0;		/* the browser's number for it, which a 中止 names (renderers) */
};

enum { BR_NAV_SHOWN, BR_NAV_FAILED, BR_NAV_STOPPED, BR_NAV_SAVED };

struct BrNavResult {
	int		outcome = BR_NAV_SHOWN;
	std::string	url;		/* where it came from */
	int		status = 0;
	long		bytes = 0;
	std::string	error;		/* what went wrong, for the message line */
	std::string	title;
};

/*
 * What a navigation fetches and shows in 'eng', an engine of this
 * process: the address asked of the service worker, fetched (a form's
 * body sent), an attachment saved, an error page made, and the page
 * loaded (br_main.cc). 'progress' is told what has come.
 */
int br_nav_fetch_load( BrEngine *eng, const BrNav &nav, BrNavResult *out,
		       void (*progress)( void *ctx, long got, long total ) );

enum { BR_M_MOVE, BR_M_DOWN, BR_M_UP };
enum { BR_HIST_NONE, BR_HIST_DONE, BR_HIST_FETCH };
enum { BR_LOAD_PAGE, BR_LOAD_NEW, BR_LOAD_RELOAD, BR_LOAD_HISTORY };
enum { BR_FS_ENTER = 1, BR_FS_EXIT = 2 };
enum { BR_WIN_MINIMIZE = 1, BR_WIN_MAXIMIZE, BR_WIN_RESTORE, BR_WIN_FIXED, BR_WIN_RESIZABLE };

/* The text engine (br_text.cc) with the face it draws in */
BrEngine *br_text_engine( SkTypeface *face );

/* Blink (br_blink.cc), brought up on the first call, with the face pages are drawn in */
BrEngine *br_blink_engine( sk_sp<SkData> face );
/* Blink brought up, with no page yet (a renderer kept ready) */
void br_blink_up( sk_sp<SkData> face );
/* //base and Mojo brought up, without Blink (the browser's process, a renderer before its pages) */
void br_base_up();

/* Flags given to V8 when Blink is brought up (a test's "v8=" line) */
extern std::string br_v8_flags;
/* Blink's runtime features turned on or off, parted by commas, "-" before a name for off (a test's "features=" lines) */
extern std::string br_features;
/* Origins treated as secure contexts, parted by commas (a test's "secure=" lines) */
extern std::string br_secure_origins;
/* What a test asks to be traced on the console ("trace=" lines): "exceptions", those the browser's APIs throw */
extern std::string br_trace;

/*
 * The origins the user lets have what only an isolated web app has in
 * Chrome (Direct Sockets, the windowing controls, Web Printing): kept
 * between runs with the answers on permissions
 * (port/blink/services_tessronos.cc); any thread
 */
bool br_isolated_allowed( const std::string &origin );
void br_isolated_set( const std::string &origin, bool on );
/* An origin allowed for this run only, not kept (a test's "isolated=" line) */
void br_isolated_allow_now( const std::string &origin );

/* What the application says it is to the servers */
const char *br_user_agent();

/*
 * The screen the page is told of (screen.width and height): the one the
 * browser's window is on (its middle), else the one new windows open on;
 * its size in pixels (0 by 0 when the window manager is not up), and in
 * the browser its rectangle in the view
 */
void br_screen_size( int *w, int *h );
void br_screen_view( int *x, int *y, int *w, int *h );

/*
 * The page's window, its frame with it (window.screenX and outerWidth),
 * from the corner of that screen: false when there is none
 */
bool br_window_rect( int *x, int *y, int *w, int *h );

/*
 * The system's time zone, as ICU names it: its IANA name ("Asia/Tokyo"),
 * or its offset ("GMT+09:00") when it has no name
 */
std::string br_zone_name();

/*
 * A question the page asks (alert, confirm, prompt), on a panel of the
 * browser's data box over its window while the page waits: true when it
 * was agreed to (了解), and for prompt what was written in its field
 */
enum { BR_ASK_ALERT = 0, BR_ASK_CONFIRM = 1, BR_ASK_PROMPT = 2 };
bool br_ask_page( int kind, const std::string &message, const std::string &deflt, std::string *answer );

/* One of names chosen on a panel (a <select>'s list), 'chosen' shown chosen first: its index, or -1 */
int br_choose( const std::vector<std::string> &names, int chosen );

/*
 * A permission a page asks for, asked of the user on a panel: 'origin'
 * asks for 'what' (words of the panel). True when it was given (許可).
 * It may be called from any thread; it returns when it is answered.
 */
bool br_ask_permission( const std::string &origin, const std::string &what );

/*
 * The passwords of login forms (port/blink/credentials_tessronos.cc), from
 * the engine's thread: one sent offered to be kept for the origin, and one
 * kept found for a form's fields; both ask the user on a panel
 */
void br_password_offer( const std::string &origin, const std::string &user, const std::string &password );
bool br_password_find( const std::string &origin, std::string *user, std::string *password );

/* A colour the user picks on the page (EyeDropper), from a service's thread: false when given up */
bool br_pick_color( uint32_t *color );

/* The origin of the page shown (its main frame's document), from any thread */
std::string br_page_origin();

/*
 * Real objects chosen on a panel as the files a page asks for (<input
 * type="file">): the links in the tray, and links dropped on the panel.
 * False when none was chosen. 'multiple': the page takes more than one.
 */
struct BrObjLink;
bool br_choose_objects( bool multiple, std::vector<BrObjLink> *out );

/*
 * What a page fetched is, decided as Chrome decides it before a navigation
 * commits (br_blink.cc): BR_ANSWER_SHOW, a document to load (its type,
 * when the server's said nothing of the bytes, sniffed into r->type);
 * BR_ANSWER_SAVED, a download (an attachment, or a type no document is
 * made of), saved as a real object in ブラウザの保存箱 and not shown;
 * BR_ANSWER_INVALID, a download whose status is not a success, which is
 * neither saved nor shown but answered with an error page
 */
enum { BR_ANSWER_SHOW, BR_ANSWER_SAVED, BR_ANSWER_INVALID };
int br_nav_answer( BrResponse *r );

/* A file's bytes kept under a path of its own, the path given to Blink (port/blink/files_tessronos.cc) */
std::string br_file_keep( const std::string &uuid, const std::string &name, std::vector<uint8_t> bytes );

/*
 * Going to an address: asked of the service worker of its scope first;
 * true with its answer in r (port/blink/service_worker_tessronos.cc)
 */
bool br_sw_navigate( const std::string &url, const std::string &method, BrResponse *r );

/* The UUID of the page's window, which a request to the desktop names (br_main.cc) */
void br_window_uuid( unsigned char id[16] );

/* An action of the menu 〈メディア〉 told to the page's media session; what came of it (port/blink/services_tessronos.cc) */
std::string br_media_session_action( const std::string &code );

/* The application's badge the page set, shown before the title; "" when none (port/blink/devices_tessronos.cc) */
std::string br_badge_text();
/* What the window says before the title while a page shares the screen; "" when none does (port/blink/screen_tessronos.cc) */
std::string br_capture_text();

/* What 印刷 came to, as text: the pages printed and the desktop's last answer (port/net/br_print.cc) */
int br_print_report( char *buf, int cap );

/*
 * Asked by an engine while a page's parts are still coming: true when
 * the user has pushed 停止 (the tool panel, br_main.cc); NULL for none
 */
extern bool (*br_stop_pushed)();

/* ---------------------------------------------------------------- the page object (br_obj.cc) */

struct BrObject {
	unsigned char	id[16];		/* its UUID */
	bool		ok = false;	/* it was read */
	std::string	name;
	/*
	 * The window: its corner from the top left of the screen it is kept
	 * on (wm_place_keep), or, while none is kept, the program's own place
	 * from the corner of the screen new windows open on; its work area's
	 * size
	 */
	int		x = 120, y = 80;
	int		w = 800, h = 560;
	bool		kept = false;	/* "window" had a "pos" */
	std::string	screen;		/* the screen it is kept on: 8 hex digits, "" the main screen */
	std::string	url;		/* the home page: the address its text holds */
	std::string	trace;		/* a line "trace=<what>" of its text: traced on the console as the start-up line */
};

std::string	br_uuid_str( const unsigned char id[16] );

/* The object's name, window and home page */
bool		br_obj_open( const unsigned char id[16], BrObject *o );

/*
 * The home page set (〈ホームページに設定〉): record 0 made anew with
 * o.url as its one paragraph, and the page's title into the metadata
 */
bool		br_obj_set_home( const BrObject &o, const std::string &title );

/* The window's place into the metadata */
bool		br_obj_save_window( const BrObject &o );

/* ---------------------------------------------------------------- notifications (port/net/br_notify.cc) */

/*
 * A notification shown by the desktop under the number 'id' (another
 * with the same number takes its place): where it comes from, its title,
 * what it says, up to two buttons of its own; 'stay' keeps it until it is
 * answered. False when the desktop is not there. What comes of it is
 * handed to the watcher, on a thread of its own: BR_NOTE_CLICK with
 * 'which' 0 for 〈開く〉 and 1 or 2 for its buttons, BR_NOTE_CLOSED when
 * the person closed it, BR_NOTE_GONE when it went by itself.
 */
enum { BR_NOTE_CLICK = 1, BR_NOTE_CLOSED = 2, BR_NOTE_GONE = 3 };
typedef void (*BrNoteWatch)( int id, int what, int which );

void	br_note_watch( BrNoteWatch w );
bool	br_note_show( int id, const std::string &source, const std::string &title, const std::string &body,
		      const std::vector<std::string> &actions, bool stay );
void	br_note_close( int id );		/* taken back: nothing more is told of it */

/* ---------------------------------------------------------------- Web Push (port/net/push_tessronos.cc) */

/* A subscription as the page is given it: its endpoint, its keys' public parts, its server's key */
struct BrPushSub {
	std::string	endpoint;
	std::string	p256dh;		/* 65 bytes */
	std::string	auth;		/* 16 bytes */
	std::string	app_key;
};

/* A message pushed for a scope's service worker: its payload, none when it came empty */
typedef std::function<void( const std::string &scope, const std::optional<std::string> &payload )> BrPushDeliver;

void	br_push_start( BrPushDeliver deliver );		/* once: the subscriptions read back, the service connected */
bool	br_push_get( const std::string &scope, BrPushSub *out );
/* 'done' is called on the network thread or at once */
void	br_push_subscribe( const std::string &scope, const std::string &app_key,
			   std::function<void( bool ok, const BrPushSub &sub, const std::string &why )> done );
bool	br_push_unsubscribe( const std::string &scope );

/* ---------------------------------------------------------------- videos in windows of their own */

/*
 * What the application is to do for a video's picture-in-picture window
 * (port/blink/pip_tessronos.cc): open one for a player, show another
 * player in it, or close it
 */
struct BrPipAsk {
	enum What { kOpen, kUpdate, kClose };
	What	what = kOpen;
	int	session = 0;		/* the window's number */
	int	player = -1;		/* the video's player */
	int	w = 0, h = 0;		/* its work area */
};

bool	br_pip_next( BrPipAsk *a );
void	br_pip_closed( int session );		/* the user closed the window */
void	br_pip_press( int session, bool play );	/* a press in it: play, or pause */

/*
 * A player's frame now, drawn into the canvas 'w' x 'h' with its aspect
 * kept (port/media/media_player.cc): false when there is none; whether
 * the player is paused; a count that changes with each frame it shows
 */
bool	br_media_paint_player( int id, SkCanvas *c, int w, int h );
bool	br_media_player_paused( int id );
long	br_media_player_frames( int id );

#endif /* __BR_H__ */
