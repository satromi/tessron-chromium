/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_ext.h
 *	ブラウザ: Manifest V3 extensions (design 14.12)
 */

#ifndef BR_EXT_H
#define BR_EXT_H

#include <functional>
#include <string>
#include <vector>

/* The objects behind them (br_ext_os.cc) */
std::vector<std::string> br_ext_os_links();
bool br_ext_os_read( const std::string &uuid, std::string *bytes, std::string *name );
bool br_ext_os_link( const std::string &uuid, const std::string &name );
bool br_ext_os_unlink( const std::string &uuid );
bool br_ext_os_state_read( std::string *json );
bool br_ext_os_state_write( const std::string &json );

/* What the 〈設定〉 window lists and changes, and what brings one in */
struct BrExtInfo {
	std::string	id;		/* chrome-extension://<id>/ */
	std::string	uuid;		/* its object */
	std::string	name;
	std::string	version;
	bool		enabled;
	std::string	error;		/* why it does not work, or "" */
	std::vector<std::string> wants;	/* what it declares, in words for its reader */
};
std::vector<BrExtInfo> br_ext_list();
bool br_ext_preview( const std::string &uuid, BrExtInfo *out );	/* its object read and nothing kept: false when it is not an extension */
bool br_ext_enable( const std::string &id, bool on );
bool br_ext_add( const std::string &uuid, std::string *why );
void br_ext_add_later( const std::string &uuid );	/* before the engine is up: taken when it starts */
bool br_ext_remove( const std::string &id );

/* What the loader and the network stack do with a request (on whichever thread they are) */
enum BrExtAct { BR_EXT_PASS, BR_EXT_BLOCK, BR_EXT_REDIRECT, BR_EXT_SERVED, BR_EXT_LATER };
struct BrExtReq {
	std::string	url;
	std::string	initiator;	/* the origin that asks, "" none */
	std::string	type;		/* declarativeNetRequest's resource type */
	std::string	method;
	std::string	body;		/* what a call of the API carries */
};
struct BrExtAns {
	std::string	redirect;
	int		status = 200;
	std::string	ctype;
	std::string	headers;	/* lines of its own, a page's security policy */
	std::string	body;
};
BrExtAct br_ext_serve( const BrExtReq &rq, BrExtAns *ans );	/* a chrome-extension:// address */
/*
 * The same, for a loader that can answer later: what waits for an event
 * (msg.poll with nothing new, msg.answer not yet answered) is answered
 * by 'later' when the event comes or after a while (BR_EXT_LATER), from
 * another thread, with the extensions' lock held: 'later' only posts
 */
BrExtAct br_ext_serve_later( const BrExtReq &rq, BrExtAns *ans, std::function<void( BrExtAns )> later );
BrExtAct br_ext_rules( const BrExtReq &rq, BrExtAns *ans );	/* declarativeNetRequest's block, redirect, upgradeScheme */
bool br_ext_any();						/* any extension in this browser at all */
bool br_ext_cors_free( const std::string &initiator, const std::string &url );	/* an extension's own, to a host it has */

/* modifyHeaders: what the rules do to the request's and the response's headers, in order */
struct BrExtHeader {
	char		op;		/* 's' set, 'a' append, 'r' remove */
	std::string	name, value;
};
void br_ext_header_rules( const BrExtReq &rq, std::vector<BrExtHeader> *request, std::vector<BrExtHeader> *response );

/* webRequest: what was seen of a request, told to the workers that listen and may see its host */
bool br_ext_web_wanted();
void br_ext_web( const char *event, long long id, const BrExtReq &rq, int status, const std::string &more );

/* The page's tab, as tabs.query tells of it */
void br_ext_tab_set( const std::string &url, const std::string &title );

/* The menu's rows: an extension's toolbar button (action), and what it added to the menu on the page */
std::vector<std::string> br_ext_action_rows();
void br_ext_action_click( int row );
struct BrExtMenuAt {
	std::string	page, link, image, selection;
	bool		editable = false;
};
std::vector<std::string> br_ext_menu_rows( const BrExtMenuAt &at );
void br_ext_menu_click( int row, const BrExtMenuAt &at );
void br_ext_perm_answer( const std::string &id, int msg, bool yes );	/* permissions.request, answered on its panel */
bool br_ext_perm_words( const std::string &id, int msg, std::string *name, std::vector<std::string> *wants );

/* A content script of a frame at a moment, or one the worker asked for: world 0 is the page's own */
struct BrExtRun {
	int		world;
	std::string	id, name, code, css;
	bool		main_world = false;
};
struct BrExtInject {
	int		world = 0;	/* the extension's */
	int		msg = 0;	/* the answer goes there */
	std::string	code, css;
	std::string	key;		/* what names the sheet, to take it away */
	bool		remove = false, main_world = false, user_origin = false, all_frames = false;
	std::vector<long long> frames;	/* none: the page's top frame */
};
void br_ext_inject( const BrExtInject &what );			/* from any thread: done on the page's */
void br_ext_inject_done( int world, int msg, const std::string &json );

/* The extensions read and their workers started (on the page's thread, once Blink is up) */
void br_ext_start();

#endif
