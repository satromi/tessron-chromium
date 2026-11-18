/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_settings.cc
 *	ブラウザ: the window 〈設定〉 opens (design 14.7.10).
 *
 *	A window of panels from the browser's data box, with tags, worked as
 *	the settings accessories' are: the window manager builds and draws
 *	the parts and this only reads and writes what they hold. It has a
 *	thread of its own, so that neither the page nor the window waits for
 *	the other: what it changes is handed to the thread the data lives on
 *	(br_data.h). With renderers the window is the browser's, as the
 *	other panels are, and what the renderers keep is read and changed
 *	through them (br_uidata.h).
 */

#include TS_CONFIG_H
#include <stdint.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/ui.h>
#include <ts/uapp.h>
#include <ts/sysdef.h>
#include <ts/wm.h>
#include <ts/fs.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "port/net/br_data.h"
#include "port/ext/br_ext.h"
#include "br_uidata.h"

void br_ext_ask_load();		/* br_main.cc: the panel of 〈拡張機能を読み込む〉 */

namespace {

/* The panels of the data box (SYSDEF_BOX_BROWSER) */
constexpr INT	PN_CONFIRM = 2, PN_SETTINGS = 8, PN_PASSWORD = 9;
constexpr INT	DLG_FIELD = 11, DLG_OK = 102;

/* The sheets, in the order of the tags */
constexpr INT	S_PASSWORD = 1, S_PASSKEY = 2, S_CLEAR = 3, S_HISTORY = 4, S_DOWNLOAD = 5, S_ADDRESS = 6, S_CARD = 7, S_EXT = 8;

/* The parts of the window */
constexpr INT	P_PW_LIST = 1, P_PW_SHOW = 2, P_PW_CHANGE = 3, P_PW_REMOVE = 4;
constexpr INT	P_PK_LIST = 10, P_PK_REMOVE = 11;
constexpr INT	P_EX_LIST = 90, P_EX_LOAD = 91, P_EX_TOGGLE = 92, P_EX_REMOVE = 93;
constexpr INT	P_CL_FIRST = 20, P_CL_PERIOD = 30, P_CL_SITES = 31, P_CL_DO = 32, P_CL_SAID = 33;
constexpr INT	P_HI_LIST = 40, P_HI_REMOVE = 41, P_HI_ALL = 42;
constexpr INT	P_DL_LIST = 50, P_DL_STOP = 51, P_DL_FORGET = 52;
constexpr INT	P_AD_LIST = 60, P_AD_FIRST = 61, P_AD_ADD = 70, P_AD_CHANGE = 71, P_AD_REMOVE = 72;
constexpr INT	P_CA_LIST = 80, P_CA_NAME = 81, P_CA_NUMBER = 82, P_CA_MONTH = 83, P_CA_YEAR = 84, P_CA_SHOW = 85;
constexpr INT	P_CA_ADD = 86, P_CA_CHANGE = 87, P_CA_REMOVE = 88;

/* The switches of 閲覧データ, in the order of their parts */
const unsigned kClearKinds[10] = {
	BR_CLEAR_COOKIE, BR_CLEAR_CACHE, BR_CLEAR_LOCAL, BR_CLEAR_IDB, BR_CLEAR_CACHES, BR_CLEAR_SW, BR_CLEAR_OPFS,
	BR_CLEAR_PERMS, BR_CLEAR_HISTORY, BR_CLEAR_DOWNLOADS,
};

UI				g_ui;
std::mutex			g_lock;		/* what follows, shared with the threads that open the window */
bool				g_running;
bool				g_raise;
bool				g_close;
bool				g_reopen;	/* asked for again while it was closing */
std::deque<std::string>		g_steps;

/* What the lists show now, for the row chosen and for a test */
std::vector<BrPasswordRow>	g_pw;
std::vector<BrPasskeyRow>	g_pk;
std::vector<std::string>	g_shown[256];

/* What a test answers the panels with in the place of a person: -1 none is set, the panel is put up */
INT		g_answer = -1;
std::string	g_new_password;

void show_list( INT part, const std::vector<std::string> &rows )
{
	std::vector<const char *> p;
	for ( const std::string &r : rows ) p.push_back(r.c_str());
	ER er = ui_names(&g_ui, part, p.data(), (INT)p.size());
	if ( er < E_OK ) {
		/* not taken: said, and the list left empty rather than showing what it showed before */
		printf("browser: settings list %d: %d rows not shown (%d)\n", (int)part, (int)rows.size(), (int)er);
		(void)ui_names(&g_ui, part, nullptr, 0);
		if ( part >= 0 && part < 256 ) g_shown[part].clear();
		return;
	}
	if ( part >= 0 && part < 256 ) g_shown[part] = rows;
}

/* The row chosen in a list, from 0; -1 when none is */
int chosen( INT part )
{
	return (int)ui_value(&g_ui, part) - 1;
}

/* Asked on the panel 確認: true when 了解 */
bool ask( const std::string &line1, const std::string &line2 )
{
	if ( g_answer >= 0 ) return g_answer == DLG_OK;
	const char *lines[5] = { line1.c_str(), line2.c_str(), " ", " ", " " };
	return ui_ask(&g_ui, PN_CONFIRM, lines, 5) == DLG_OK;
}

/* ---------------------------------------------------------------- passwords and passkeys */

void fill_passwords()
{
	bool show = ui_value(&g_ui, P_PW_SHOW) != 0;
	std::vector<std::string> rows;

	g_pw = br_ui_passwords();
	for ( const BrPasswordRow &p : g_pw ) {
		rows.push_back(p.origin + "\t" + ( p.id.empty() ? std::string("(名前なし)") : p.id ) + "\t"
			       + ( show ? p.password : std::string("●●●●●●●●") ));
	}
	show_list(P_PW_LIST, rows);
	ui_set_value(&g_ui, P_PW_LIST, 0);
}

void fill_passkeys()
{
	std::vector<std::string> rows;

	g_pk = br_ui_passkeys();
	for ( const BrPasskeyRow &k : g_pk ) rows.push_back(k.rp + "\t" + k.name);
	show_list(P_PK_LIST, rows);
	ui_set_value(&g_ui, P_PK_LIST, 0);
}

/* The extensions, as br_ext.cc has them; a change holds for pages opened after it */
std::vector<BrExtInfo>	g_ext;

void fill_extensions()
{
	std::vector<std::string> rows;

	g_ext = br_ui_exts();
	for ( const BrExtInfo &e : g_ext ) {
		rows.push_back(e.name + "\t" + e.version + "\t" + ( !e.error.empty() ? e.error : e.enabled ? "有効" : "無効" ));
	}
	show_list(P_EX_LIST, rows);
	ui_set_value(&g_ui, P_EX_LIST, 0);
}

/* The new password asked on its panel: true with it, typed twice alike */
bool ask_password( const BrPasswordRow &row, std::string *out )
{
	if ( g_answer >= 0 ) {
		*out = g_new_password;
		return g_answer == DLG_OK;
	}
	UI	q;
	T_OBNTM	m;
	INT	n = 0;
	bool	ok = false;

	if ( ui_dlg_open(&g_ui, PN_PASSWORD, &q) < E_OK ) return false;
	ui_set_text(&q, 1, row.origin.c_str());
	ui_set_text(&q, 2, row.id.empty() ? "(名前なし)" : row.id.c_str());
	ui_focus(&q, DLG_FIELD);
	for ( bool done = false; !done; ) {
		INT ev = ui_dlg_next(&g_ui, &q, &m, &n);
		if ( ev == UI_EV_BUTTON ) {
			if ( n == DLG_OK ) {
				char a[256], b[256];
				a[0] = b[0] = '\0';
				ui_text(&q, DLG_FIELD, a, sizeof(a));
				ui_text(&q, DLG_FIELD + 1, b, sizeof(b));
				ok = a[0] != '\0' && strcmp(a, b) == 0;
				if ( ok ) {
					*out = a;
				} else {
					ui_set_text(&q, 3, "2つの欄が同じではありません。");
					continue;
				}
				memset(a, 0, sizeof(a));
				memset(b, 0, sizeof(b));
			}
			done = true;
		} else if ( ev == UI_EV_CLOSE ) {
			done = true;
		} else if ( ev == UI_EV_NONE ) {
			tk_dly_tsk(20);
		}
	}
	ui_dlg_close(&q);
	return ok;
}

void password_change()
{
	int k = chosen(P_PW_LIST);
	if ( k < 0 || k >= (int)g_pw.size() ) return;
	std::string now;
	if ( !ask_password(g_pw[(size_t)k], &now) ) return;
	bool ok = br_ui_password_change(g_pw[(size_t)k].origin, g_pw[(size_t)k].id, now);
	printf("browser: settings: a password %s\n", ok ? "changed" : "not changed");
	fill_passwords();
}

void password_remove()
{
	int k = chosen(P_PW_LIST);
	if ( k < 0 || k >= (int)g_pw.size() ) return;
	const BrPasswordRow p = g_pw[(size_t)k];
	if ( !ask(p.origin + " の", "「" + p.id + "」のパスワードを削除しますか。") ) return;
	bool ok = br_ui_password_remove(p.origin, p.id);
	printf("browser: settings: a password %s\n", ok ? "removed" : "not removed");
	fill_passwords();
}

void passkey_remove()
{
	int k = chosen(P_PK_LIST);
	if ( k < 0 || k >= (int)g_pk.size() ) return;
	const BrPasskeyRow p = g_pk[(size_t)k];
	if ( !ask(p.rp + " の", "「" + p.name + "」のパスキーを削除しますか。") ) return;
	bool ok = br_ui_passkey_remove(p.id);
	printf("browser: settings: a passkey %s\n", ok ? "removed" : "not removed");
	fill_passkeys();
}

void extension_toggle()
{
	int k = chosen(P_EX_LIST);
	if ( k < 0 || k >= (int)g_ext.size() ) return;
	bool ok = br_ui_ext_enable(g_ext[(size_t)k].id, !g_ext[(size_t)k].enabled);
	printf("browser: settings: an extension %s\n", ok ? "changed" : "not changed");
	fill_extensions();
}

void extension_remove()
{
	int k = chosen(P_EX_LIST);
	if ( k < 0 || k >= (int)g_ext.size() ) return;
	const BrExtInfo e = g_ext[(size_t)k];
	if ( !ask("拡張機能", "「" + e.name + "」を削除しますか。") ) return;
	bool ok = br_ui_ext_remove(e.id);
	printf("browser: settings: an extension %s\n", ok ? "removed" : "not removed");
	fill_extensions();
}

/* ---------------------------------------------------------------- what was browsed: clearing */

std::vector<std::string>	g_sites;
bool				g_test_future;	/* a test's: a clearing from an hour on, which nothing is as new as */
std::vector<BrHistoryRow>	g_hist;
std::vector<BrDownloadRow>	g_dls;
std::vector<BrAddress>		g_addrs;
std::vector<BrCard>		g_cards;
UD				g_dl_at;

void fill_clear()
{
	std::vector<std::string> rows{ "すべてのサイト" };
	g_sites = br_ui_sites();
	rows.insert(rows.end(), g_sites.begin(), g_sites.end());
	show_list(P_CL_SITES, rows);
	ui_set_value(&g_ui, P_CL_SITES, 1);
}

void do_clear()
{
	BrClear c;
	for ( int i = 0; i < 10; i++ ) {
		if ( ui_value(&g_ui, P_CL_FIRST + i) != 0 ) c.kinds |= kClearKinds[i];
	}
	if ( c.kinds == 0 ) return;
	static const long long kHours[4] = { 1, 24, 24 * 7, 24 * 28 };
	INT period = ui_value(&g_ui, P_CL_PERIOD);
	if ( period >= 1 && period <= 4 ) c.since_us = br_now_us() - kHours[period - 1] * 3600 * 1000000;
	if ( g_test_future ) c.since_us = br_now_us() + 3600LL * 1000000;
	int k = chosen(P_CL_SITES);
	if ( !g_sites.empty() && k < 0 ) k = 0;
	if ( k >= 1 && k <= (int)g_sites.size() ) c.site = g_sites[(size_t)k - 1];
	if ( !ask(c.site.empty() ? "すべてのサイトの" : c.site + " の", "選んだデータを消去しますか。元に戻せません。") ) return;
	ui_set_text(&g_ui, P_CL_SAID, "消去しています…");
	br_ui_clear(c);
	ui_set_text(&g_ui, P_CL_SAID, "消去しました。");
	printf("browser: settings: cleared %#x since %lld of %s\n", c.kinds, c.since_us, c.site.empty() ? "all sites" : c.site.c_str());
	fill_clear();
}

/* ---------------------------------------------------------------- history */

std::string when_text( long long us )
{
	time_t t = (time_t)( us / 1000000 );
	struct tm tm;
	char b[32] = "";
	if ( localtime_r(&t, &tm) != nullptr ) strftime(b, sizeof(b), "%Y-%m-%d %H:%M", &tm);
	return b;
}

void fill_history()
{
	std::vector<std::string> rows;
	g_hist = br_history_list();
	for ( const BrHistoryRow &h : g_hist ) rows.push_back(when_text(h.when_us) + "\t" + h.title + "\t" + h.url);
	show_list(P_HI_LIST, rows);
	ui_set_value(&g_ui, P_HI_LIST, 0);
}

void history_remove()
{
	int k = chosen(P_HI_LIST);
	if ( k < 0 || k >= (int)g_hist.size() ) return;
	br_history_remove(g_hist[(size_t)k].url);
	fill_history();
}

void history_all()
{
	if ( !ask("閲覧履歴を", "すべて削除しますか。") ) return;
	BrClear c;
	c.kinds = BR_CLEAR_HISTORY;
	br_ui_clear(c);
	fill_history();
}

/* ---------------------------------------------------------------- downloads */

void fill_downloads()
{
	std::vector<std::string> rows;
	g_dls = br_ui_downloads();
	for ( const BrDownloadRow &d : g_dls ) {
		char how[64];
		static const char *const kState[] = { "受信中", "完了", "中止", "失敗" };
		if ( d.total > 0 ) snprintf(how, sizeof(how), "%lld%% (%lld KB)", d.got * 100 / d.total, d.got / 1024);
		else snprintf(how, sizeof(how), "%lld KB", d.got / 1024);
		rows.push_back(d.name + "\t" + kState[d.state < 0 || d.state > 3 ? 3 : d.state] + "\t" + how);
	}
	/* the row chosen stays while the counts move */
	INT was = ui_value(&g_ui, P_DL_LIST);
	show_list(P_DL_LIST, rows);
	ui_set_value(&g_ui, P_DL_LIST, was);
}

void download_stop()
{
	int k = chosen(P_DL_LIST);
	if ( k < 0 || k >= (int)g_dls.size() ) return;
	(void)br_ui_download_cancel(g_dls[(size_t)k].key);
	fill_downloads();
}

void download_forget()
{
	int k = chosen(P_DL_LIST);
	if ( k < 0 || k >= (int)g_dls.size() ) return;
	br_ui_download_forget(g_dls[(size_t)k].key);
	ui_set_value(&g_ui, P_DL_LIST, 0);
	fill_downloads();
}

/* ---------------------------------------------------------------- addresses and cards */

std::string box_text( INT part )
{
	char b[512];
	b[0] = '\0';
	ui_text(&g_ui, part, b, sizeof(b));
	return b;
}

void fill_addresses()
{
	std::vector<std::string> rows;
	std::vector<BrCard> no;
	(void)br_autofill_load(&g_addrs, &g_cards);
	for ( BrCard &c : g_cards ) std::fill(c.f[BR_CARD_NUMBER].begin(), c.f[BR_CARD_NUMBER].end(), '\0');
	g_cards.clear();
	for ( const BrAddress &a : g_addrs ) rows.push_back(a.f[BR_ADDR_NAME] + "\t" + a.f[BR_ADDR_REGION] + a.f[BR_ADDR_CITY] + a.f[BR_ADDR_LINE1]);
	show_list(P_AD_LIST, rows);
	ui_set_value(&g_ui, P_AD_LIST, 0);
}

void address_chosen()
{
	int k = chosen(P_AD_LIST);
	for ( int i = 0; i < BR_ADDR_FIELDS; i++ ) {
		bool has = k >= 0 && k < (int)g_addrs.size() && !g_addrs[(size_t)k].f[i].empty();
		ui_set_text(&g_ui, P_AD_FIRST + i, has ? g_addrs[(size_t)k].f[i].c_str() : "");
	}
}

/* What is kept of the addresses and cards changed: 'change' given both, read just before and written just after */
template <typename Change>
void autofill_change( Change change )
{
	std::vector<BrAddress> addrs;
	std::vector<BrCard> cards;
	if ( !br_autofill_load(&addrs, &cards) ) {
		printf("browser: settings: what is kept of addresses and cards cannot be read: left as it is\n");
		return;
	}
	change(addrs, cards);
	bool ok = br_autofill_save(addrs, cards);
	printf("browser: settings: addresses and cards %s\n", ok ? "saved" : "not saved");
	for ( BrCard &c : cards ) std::fill(c.f[BR_CARD_NUMBER].begin(), c.f[BR_CARD_NUMBER].end(), '\0');
}

void address_store( int part )
{
	int k = chosen(P_AD_LIST);
	if ( part != P_AD_ADD && ( k < 0 || k >= (int)g_addrs.size() ) ) return;
	if ( part == P_AD_REMOVE && !ask("この住所を", "削除しますか。") ) return;
	BrAddress now;
	for ( int i = 0; i < BR_ADDR_FIELDS; i++ ) now.f[i] = box_text(P_AD_FIRST + i);
	autofill_change([&]( std::vector<BrAddress> &addrs, std::vector<BrCard> & ) {
		if ( part == P_AD_ADD ) addrs.push_back(now);
		else if ( k < (int)addrs.size() && part == P_AD_CHANGE ) addrs[(size_t)k] = now;
		else if ( k < (int)addrs.size() ) addrs.erase(addrs.begin() + k);
	});
	fill_addresses();
}

/* The card's rows in the list: the number hidden but for its last four unless the switch is on */
void fill_cards()
{
	std::vector<std::string> rows;
	std::vector<BrAddress> no;
	std::vector<BrCard> cards;
	bool show = ui_value(&g_ui, P_CA_SHOW) != 0;
	(void)br_autofill_load(&no, &cards);
	for ( const BrCard &c : cards ) {
		const std::string &n = c.f[BR_CARD_NUMBER];
		rows.push_back(c.f[BR_CARD_NAME] + "\t" + ( show ? n : "****" + ( n.size() > 4 ? n.substr(n.size() - 4) : n ) ) + "\t"
			       + c.f[BR_CARD_MONTH] + "/" + c.f[BR_CARD_YEAR]);
	}
	for ( BrCard &c : cards ) std::fill(c.f[BR_CARD_NUMBER].begin(), c.f[BR_CARD_NUMBER].end(), '\0');
	show_list(P_CA_LIST, rows);
	ui_set_value(&g_ui, P_CA_LIST, 0);
}

void card_store( int part )
{
	int k = chosen(P_CA_LIST);
	if ( part != P_CA_ADD && k < 0 ) return;
	if ( part == P_CA_REMOVE && !ask("このカードを", "削除しますか。") ) return;
	BrCard now;
	now.f[BR_CARD_NAME] = box_text(P_CA_NAME);
	now.f[BR_CARD_NUMBER] = box_text(P_CA_NUMBER);
	now.f[BR_CARD_MONTH] = box_text(P_CA_MONTH);
	now.f[BR_CARD_YEAR] = box_text(P_CA_YEAR);
	std::string digits;
	for ( char ch : now.f[BR_CARD_NUMBER] ) {
		if ( ch >= '0' && ch <= '9' ) digits += ch;
	}
	if ( part != P_CA_REMOVE && digits.size() < 8 ) {
		if ( part == P_CA_ADD || !digits.empty() ) {
			printf("browser: settings: a card without a number is not kept\n");
			return;
		}
	}
	now.f[BR_CARD_NUMBER] = digits;
	autofill_change([&]( std::vector<BrAddress> &, std::vector<BrCard> &cards ) {
		if ( part == P_CA_ADD ) {
			cards.push_back(now);
		} else if ( k < (int)cards.size() && part == P_CA_CHANGE ) {
			/* the number's box left empty: the number stays, for it is never shown there */
			if ( now.f[BR_CARD_NUMBER].empty() ) now.f[BR_CARD_NUMBER] = cards[(size_t)k].f[BR_CARD_NUMBER];
			cards[(size_t)k] = now;
		} else if ( k < (int)cards.size() ) {
			cards.erase(cards.begin() + k);
		}
	});
	std::fill(digits.begin(), digits.end(), '\0');
	std::fill(now.f[BR_CARD_NUMBER].begin(), now.f[BR_CARD_NUMBER].end(), '\0');
	ui_set_text(&g_ui, P_CA_NUMBER, "");
	fill_cards();
}

/* ---------------------------------------------------------------- the window's parts */

void fill_sheet( INT sheet )
{
	if ( sheet == S_PASSWORD ) fill_passwords();
	else if ( sheet == S_PASSKEY ) fill_passkeys();
	else if ( sheet == S_CLEAR ) fill_clear();
	else if ( sheet == S_HISTORY ) fill_history();
	else if ( sheet == S_DOWNLOAD ) fill_downloads();
	else if ( sheet == S_ADDRESS ) fill_addresses();
	else if ( sheet == S_CARD ) fill_cards();
	else if ( sheet == S_EXT ) fill_extensions();
}

void pressed( INT part )
{
	switch ( part ) {
	case P_PW_CHANGE:	password_change(); break;
	case P_PW_REMOVE:	password_remove(); break;
	case P_PK_REMOVE:	passkey_remove(); break;
	case P_EX_LOAD:		br_ext_ask_load(); break;
	case P_EX_TOGGLE:	extension_toggle(); break;
	case P_EX_REMOVE:	extension_remove(); break;
	case P_CL_DO:		do_clear(); break;
	case P_HI_REMOVE:	history_remove(); break;
	case P_HI_ALL:		history_all(); break;
	case P_DL_STOP:		download_stop(); break;
	case P_DL_FORGET:	download_forget(); break;
	case P_AD_ADD:
	case P_AD_CHANGE:
	case P_AD_REMOVE:	address_store(part); break;
	case P_CA_ADD:
	case P_CA_CHANGE:
	case P_CA_REMOVE:	card_store(part); break;
	default:		break;
	}
}

void changed( INT part )
{
	if ( part == P_PW_SHOW ) fill_passwords();
	else if ( part == P_AD_LIST ) address_chosen();
	else if ( part == P_CA_SHOW ) fill_cards();
}

/* What a list and what a test asked for shows, written out for the test to read (fs layer: write, create, truncate) */
void dump_to( const std::string &path )
{
	std::string o;
	for ( int part = 0; part < 256; part++ ) {
		for ( const std::string &r : g_shown[part] ) o += std::to_string(part) + ":" + r + "\n";
	}
	INT fd = fs_open(path.c_str(), 0x0001 | 0x0100 | 0x0400);
	if ( fd < 0 ) return;
	(void)fs_write(fd, o.data(), (SZ)o.size());
	(void)fs_close(fd);
}

/*
 * A step of a test ("setstep="), through the same parts a person works:
 * sheet:N, pick:PART:ROW (from 1), set:PART:VALUE, press:PART,
 * text:PART:LETTERS, answer:N (the panels answered so: 102 了解, 0 no
 * more), pw:LETTERS (the new password its panel is answered with),
 * dump:PATH, histfill:N (N pages put in the history)
 */
void do_step( const std::string &s )
{
	size_t c = s.find(':');
	std::string what = s.substr(0, c), arg = ( c == std::string::npos ) ? std::string() : s.substr(c + 1);
	size_t c2 = arg.find(':');
	INT n = atoi(arg.c_str());
	std::string rest = ( c2 == std::string::npos ) ? std::string() : arg.substr(c2 + 1);

	printf("browser: settings: step %s\n", what == "pw" ? "pw" : s.c_str());
	if ( what == "sheet" ) {
		ui_show_sheet(&g_ui, n);
		fill_sheet(n);
	} else if ( what == "pick" ) {
		ui_set_value(&g_ui, n, atoi(rest.c_str()));
		changed(n);
	} else if ( what == "set" ) {
		ui_set_value(&g_ui, n, atoi(rest.c_str()));
		changed(n);
	} else if ( what == "text" ) {
		ui_set_text(&g_ui, n, rest.c_str());
	} else if ( what == "press" ) {
		pressed(n);
	} else if ( what == "answer" ) {
		g_answer = n > 0 ? n : -1;
	} else if ( what == "pw" ) {
		g_new_password = arg;
	} else if ( what == "dump" ) {
		dump_to(arg);
	} else if ( what == "wait" ) {
		tk_dly_tsk(n);
	} else if ( what == "future" ) {
		g_test_future = n != 0;
	} else if ( what == "site" ) {
		/* a site by its name, in the list or not */
		g_sites.clear();
		g_sites.push_back(arg);
		ui_set_value(&g_ui, P_CL_SITES, 2);
	} else if ( what == "hsts" ) {
		g_shown[255].assign(1, std::string("hsts=") + ( br_ui_hsts_known(arg) ? "1" : "0" ));
	} else if ( what == "histfill" ) {
		/* n pages visited, as many as a person's history holds after long use: for a test of the list */
		for ( int i = 0; i < n; i++ ) {
			std::string k = std::to_string(i);
			br_history_visit("http://fill.test/" + k + "/a/page/of/the/long/history/list", "ページ " + k);
		}
	}
}

/* The window shown until it is closed, on its own thread (window_thread) */
void window_run()
{
	T_OBNTM	m;
	INT	id = 0;
	bool	closing = false;

	/* no place kept: the program's own, from the corner of the screen new windows open on */
	if ( ui_open(&g_ui, SYSDEF_BOX_BROWSER, PN_SETTINGS, wm_new_x(180), wm_new_y(90)) < E_OK ) {
		printf("browser: settings: no window\n");
		return;
	}
	printf("browser: settings: open\n");
	fill_sheet(ui_sheet(&g_ui));
	while ( !closing ) {
		std::string step;
		{
			std::lock_guard<std::mutex> g(g_lock);
			if ( !g_steps.empty() ) {
				step = g_steps.front();
				g_steps.pop_front();
			}
			if ( g_close ) closing = true;
			if ( g_raise ) {
				g_raise = false;
				T_OBWPOS wp;
				SZ asz = 0;
				if ( ob_rea_rec(g_ui.kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
					wp.z = 0;
					(void)ob_wri_rec(g_ui.kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
				}
			}
		}
		if ( closing ) break;
		if ( !step.empty() ) {
			do_step(step);
			continue;
		}
		/* what is downloading, its counts moving, while its sheet is up */
		if ( ui_sheet(&g_ui) == S_DOWNLOAD && ui_now() >= g_dl_at ) {
			g_dl_at = ui_now() + 500;
			fill_downloads();
		}
		if ( ui_wait(&g_ui, &m, 100) < E_OK ) continue;
		switch ( ui_event(&g_ui, &m, &id) ) {
		case UI_EV_CLOSE:
			closing = true;
			break;
		case UI_EV_KEY:
			if ( ( m.mods & ( 0x01 | 0x10 ) ) != 0 ) {
				UB c = wm_key_char(m.code, m.mods);
				if ( c == 'e' || c == 'E' ) closing = true;
			}
			break;
		case UI_EV_CHANGE:
			changed(id);
			break;
		case UI_EV_BUTTON:
			pressed(id);
			break;
		case UI_EV_SHEET:
			fill_sheet(id);
			break;
		default:
			break;
		}
	}
	/* no password stays in memory longer than the window */
	for ( BrPasswordRow &p : g_pw ) std::fill(p.password.begin(), p.password.end(), '\0');
	g_pw.clear();
	g_addrs.clear();
	for ( auto &rows : g_shown ) rows.clear();
	ui_close(&g_ui);
	printf("browser: settings: closed\n");
}

/*
 * The window's thread: the window, and again when it was asked for while
 * it was closing (br_settings_open); nobody waits for its end
 */
void *window_thread( void * )
{
	ui_icon(SYSDEF_PROG_BROWSER);
	for ( ;; ) {
		window_run();
		std::lock_guard<std::mutex> g(g_lock);
		if ( !g_reopen ) {
			g_running = false;
			return nullptr;
		}
		g_reopen = false;
		g_close = false;
	}
}

}  // namespace

void br_settings_open()
{
	std::lock_guard<std::mutex> g(g_lock);
	if ( g_running ) {
		/* the window there brought forward, or shown again once the one closing has gone */
		if ( g_close ) {
			g_reopen = true;
		} else {
			g_raise = true;
		}
		return;
	}
	pthread_t	th;
	pthread_attr_t	at;
	pthread_attr_init(&at);
	pthread_attr_setstacksize(&at, 512 * 1024);
	pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	g_close = false;
	g_running = pthread_create(&th, &at, window_thread, nullptr) == 0;
	pthread_attr_destroy(&at);
}

/*
 * The window told to close; it closes on its own thread, which may be
 * waiting for a renderer's answer meanwhile (br_proc_data), and the
 * caller does not wait for it
 */
void br_settings_close()
{
	std::lock_guard<std::mutex> g(g_lock);
	g_reopen = false;
	if ( g_running ) g_close = true;
}

void br_settings_step( const std::string &step )
{
	std::lock_guard<std::mutex> g(g_lock);
	g_steps.push_back(step);
}
