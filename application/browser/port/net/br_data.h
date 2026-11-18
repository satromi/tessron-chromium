/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_data.h
 *	ブラウザ: what the settings window (br_settings.cc) reads and changes
 *	of what the browser keeps for the user (design 14.7.10).
 *
 *	Each call may be made from any thread but the one its data lives on:
 *	it hands the work to that thread and waits for the answer.
 */

#ifndef BR_DATA_H
#define BR_DATA_H

#include <set>
#include <string>
#include <vector>

/* ---------------------------------------------------------------- passwords and passkeys (credentials_tessronos.cc) */

struct BrPasswordRow {
	std::string	origin, id, password;
};

struct BrPasskeyRow {
	std::string	rp, name;
	std::string	id;		/* the credential's ID, as base64url */
};

std::vector<BrPasswordRow> br_passwords_list();
bool br_password_change( const std::string &origin, const std::string &id, const std::string &password );
bool br_password_remove( const std::string &origin, const std::string &id );

std::vector<BrPasskeyRow> br_passkeys_list();
bool br_passkey_remove( const std::string &id );

/* ---------------------------------------------------------------- clearing (br_data.cc, and where each thing lives) */

enum {
	BR_CLEAR_COOKIE = 0x01, BR_CLEAR_CACHE = 0x02, BR_CLEAR_LOCAL = 0x04, BR_CLEAR_IDB = 0x08,
	BR_CLEAR_CACHES = 0x10, BR_CLEAR_SW = 0x20, BR_CLEAR_OPFS = 0x40, BR_CLEAR_PERMS = 0x80,
	BR_CLEAR_HISTORY = 0x100, BR_CLEAR_DOWNLOADS = 0x200,
	BR_CLEAR_ALL_SITE_DATA = BR_CLEAR_LOCAL | BR_CLEAR_IDB | BR_CLEAR_CACHES | BR_CLEAR_SW | BR_CLEAR_OPFS,
	BR_RELOAD_CREDS = 0x10000,	/* in a notice: the passwords were changed by another browser */
};

struct BrClear {
	unsigned	kinds = 0;
	long long	since_us = 0;	/* from this time (microseconds of the real clock); 0 all of it */
	std::string	site;		/* of this site (br_site_of); "" every site */
};

long long br_now_us();
std::string br_site_of( const std::string &origin_or_host );
bool br_clear_host_hit( const BrClear &c, const std::string &host );
bool br_clear_origin_hit( const BrClear &c, const std::string &origin );
void br_site_touch( const std::string &origin );	/* an origin is using its storage */

void br_data_init();
void br_data_clear( const BrClear &c );
void br_data_notice( unsigned kinds, long long since_us, const std::string &site );
std::vector<std::string> br_data_sites();

/* Each part's own, handed to its thread and waited for */
void br_net_data_clear( const BrClear &c );			/* cookies, the HTTP cache, servers, HSTS */
void br_page_data_clear( const BrClear &c );			/* localStorage, IndexedDB, service workers */
void br_service_data_clear( const BrClear &c );			/* Cache Storage, private file systems, permissions */
void br_net_data_sites( std::set<std::string> *out );
void br_page_data_sites( std::set<std::string> *out );
void br_service_data_sites( std::set<std::string> *out );
void br_net_hsts_add( const std::string &host );		/* for a test */
bool br_net_hsts_known( const std::string &host );
void br_main_command( const std::string &code );		/* br_main.cc: done on the engine's thread */

/* ---------------------------------------------------------------- history */

struct BrHistoryRow {
	std::string	url, title;
	long long	when_us;
	int		count;
};

void br_history_visit( const std::string &url, const std::string &title );
void br_history_title( const std::string &url, const std::string &title );
std::vector<BrHistoryRow> br_history_list();
void br_history_remove( const std::string &url );
std::string br_history_complete( const std::string &typed, int skip );

/* ---------------------------------------------------------------- downloads */

enum { BR_DL_GOING = 0, BR_DL_DONE = 1, BR_DL_STOPPED = 2, BR_DL_FAILED = 3 };

struct BrDownloadRow {
	std::string	key, name, url;
	long long	when_us = 0, total = 0, got = 0;
	int		state = BR_DL_GOING;
};

int br_download_start( const std::string &name, const std::string &url );
bool br_download_progress( int id, long long got, long long total );	/* false: 中止 was asked for */
void br_download_name( int id, const std::string &name );
void br_download_end( int id, int state );
std::vector<BrDownloadRow> br_downloads_list();
bool br_download_cancel( const std::string &key );
void br_download_forget( const std::string &key );

/* ---------------------------------------------------------------- addresses and cards */

enum { BR_ADDR_NAME, BR_ADDR_ORG, BR_ADDR_ZIP, BR_ADDR_REGION, BR_ADDR_CITY, BR_ADDR_LINE1, BR_ADDR_LINE2, BR_ADDR_TEL, BR_ADDR_EMAIL, BR_ADDR_FIELDS };
enum { BR_CARD_NAME, BR_CARD_NUMBER, BR_CARD_MONTH, BR_CARD_YEAR, BR_CARD_FIELDS };

struct BrAddress { std::string f[BR_ADDR_FIELDS]; };
struct BrCard { std::string f[BR_CARD_FIELDS]; };

bool br_autofill_load( std::vector<BrAddress> *addrs, std::vector<BrCard> *cards );
bool br_autofill_save( const std::vector<BrAddress> &addrs, const std::vector<BrCard> &cards );
/*
 * An address ('card' false) or a card for a form of 'origin', chosen by the
 * user on a panel: its fields (BR_ADDR_* or BR_CARD_*), false when none
 * was. Only the one chosen leaves the process that reads what is kept: a
 * renderer asks the browser, which reads the store and shows the panel.
 */
bool br_autofill_choose( bool card, const std::string &origin, std::vector<std::string> *fields );
/* The same asked of the browser by a renderer (br_rproc.cc) */
bool br_rproc_autofill_choose( bool card, const std::string &origin, std::vector<std::string> *fields );

/* ---------------------------------------------------------------- the window (br_settings.cc) */

/* 〈設定〉: the window made, on a thread of its own, or brought to the front */
void br_settings_open();

/* The window taken away as the browser ends, its parts with it; waits for that, at most two seconds */
void br_settings_close();

/* Steps for a test ("setstep=" lines), done by the window's thread in turn once it is up */
void br_settings_step( const std::string &step );

#endif /* BR_DATA_H */
