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
	std::string	body;
	std::string	error;		/* what went wrong, "" when it came */
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
 * The cookies of the process: set from a Set-Cookie line or a script's
 * document.cookie (which may not touch HttpOnly ones), and got as a
 * Cookie header's value for an address. br_fetch sends and takes them.
 */
void		br_cookie_set( const std::string &url, const std::string &line, bool from_script );
std::string	br_cookie_get( const std::string &url, bool for_script );

/* ---------------------------------------------------------------- the engine */

class BrEngine {
public:
	virtual ~BrEngine() {}

	/* A page: its address (for resolving links), its bytes and their charset */
	virtual void	load( const std::string &url, const std::string &body,
			      const std::string &charset ) = 0;

	/* Laid out for a width; the height of the whole page */
	virtual int	layout( int width ) = 0;

	/* The part of the page from 'top', 'height' high, drawn at (0, 0) */
	virtual void	paint( SkCanvas *c, int top, int height ) = 0;

	/* The link at a point of the page, "" for none */
	virtual std::string	link_at( int x, int y ) = 0;

	virtual std::string	title() = 0;

	/*
	 * The engine's own work since the last call (its tasks, timers and
	 * loads); true when the page is to be painted again
	 */
	virtual bool	pump() { return false; }

	/* An address the page asked to go to (a link, a form, a script), "" for none */
	virtual std::string	navigation() { return std::string(); }

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

	/* The address of the page's first link, "" when the engine cannot say */
	virtual std::string	first_link() { return std::string(); }

	/*
	 * Input given to the page itself, for an engine that runs the page
	 * (input() true). The pointer: a button down or up (BR_M_*, button
	 * 0 left, 2 right) or a move, at a point of the view (not of the
	 * whole page). A key: its HID usage, the modifiers and the letter it
	 * types (0 for none); true when the page took it, false when it is
	 * left to the window. The wheel: notches, down positive.
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
	virtual void	wheel( int x, int y, int notches ) { (void)x; (void)y; (void)notches; }

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
};

enum { BR_M_MOVE, BR_M_DOWN, BR_M_UP };

/* The text engine (br_text.cc) with the face it draws in */
BrEngine *br_text_engine( SkTypeface *face );

/* Blink (br_blink.cc), brought up on the first call, with the face pages are drawn in */
BrEngine *br_blink_engine( sk_sp<SkData> face );

/* What the application says it is to the servers */
const char *br_user_agent();

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
	int		x = 120, y = 80;	/* the window's work area on the screen */
	int		w = 800, h = 560;
	std::string	url;
	std::vector<std::string> back, fwd;
};

std::string	br_uuid_str( const unsigned char id[16] );

/* The object's name, window and state from its metadata */
bool		br_obj_open( const unsigned char id[16], BrObject *o );

/* The state and the page's title into the metadata, and record 0 made anew */
bool		br_obj_save( const BrObject &o, const std::string &title );

/* The window's place into the metadata */
bool		br_obj_save_window( const BrObject &o );

#endif /* __BR_H__ */
