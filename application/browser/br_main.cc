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
 *	object, opens where the object keeps it, starts at the address the
 *	object holds, and writes the address, the history and the window's
 *	place back into it (br_obj.cc).
 *
 *	The page fills the window's work area. It is drawn with Skia into a
 *	surface of the work area and put in the window with dp_put_argb.
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
 *	itself; what it leaves (Backspace outside a field) is the window's.
 *	A key typed into a field of the page goes first to the system's
 *	kana-kanji converter in the input mode the message line shows: what
 *	it commits goes into the field, what it is still converting is shown
 *	in the field as the composition, and its candidates are listed under
 *	the caret, chosen with a number or a press.
 *
 *	The start-up argument is the page object's UUID. Text may follow it
 *	after a newline, or stand alone with an address to go to as its first
 *	line; the lines are for tests -- "follow" follows the page's first
 *	link once it is shown, "ready=<path>" makes that file when the last
 *	page is shown, "quit=<ms>" ends that long after, "mem" prints what
 *	the engine holds in memory before and after collecting its garbage,
 *	"trace" prints the events the window is sent.
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

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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
#ifdef BR_BLINK
#include "port/net/br_store.h"
#endif

bool (*br_stop_pushed)() = nullptr;

/* A program of Linux's stack size: the engine recurses through a page */
size_t __ts_main_stack_size = 8UL * 1024 * 1024;

namespace {

/* The face the pages are drawn in: this one in the 書体箱, or else the first there */
const char kFaceName[] = "NotoSansJP-Regular";

const int kFrameW = 16, kTitleH = 34;		/* the frame round the work area */

/* HID usages */
enum {
	K_ENTER = 0x28, K_ESC = 0x29, K_BS = 0x2A, K_SPACE = 0x2C, K_F5 = 0x3E,
	K_HOME = 0x4A, K_PGUP = 0x4B, K_END = 0x4D, K_PGDN = 0x4E, K_RIGHT = 0x4F,
	K_LEFT = 0x50, K_DOWN = 0x51, K_UP = 0x52, K_KPENTER = 0x58,
	K_A = 0x04, K_E = 0x08, K_L = 0x0F, K_R = 0x15, K_TAB = 0x2B
};
const UINT kCtrl = 0x11, kAlt = 0x44;		/* HID_MOD_LCTRL|RCTRL, LALT|RALT */

const char kHome[] =
	"<html><head><title>ブラウザ</title></head><body>"
	"<h1>ブラウザ</h1>"
	"<p>上のツールパネルの欄に http:// か https:// で始まるアドレスを打って Enter で開きます。"
	"開いた頁と履歴は、この実身に残ります。新しい頁の実身は原紙箱の「ブラウザ」から作ります。</p>"
	"<ul><li>Ctrl+L でアドレスの欄へ、Esc で頁へ戻ります。</li>"
	"<li>矢印・Page Up・Page Down・Space・Home・End で頁を巻き取り、ホイールも使えます。</li>"
	"<li>リンクを押すとその頁へ、Backspace か Alt+← で前の頁へ戻ります。"
	"右ボタンのメニューに、戻る・進む・再読込・アドレス入力があります。</li>"
	"<li>Ctrl+R か F5 で読み直し、Ctrl+E で閉じます。</li></ul>"
	"<h2>試しに</h2>"
	"<ul><li><a href=\"http://example.com/\">http://example.com/</a></li>"
	"<li><a href=\"http://10.0.2.2:8000/\">QEMU のホスト (10.0.2.2:8000)</a></li></ul>"
#ifdef BR_BLINK
	"<p><small>頁は Blink が組んで描き、スタイルとスクリプトと画像も扱います。"
	"入力欄には、かな漢字変換で日本語も打てます(設計書 17.19)。</small></p>"
#else
	"<p><small>この版は頁を文字と見出し・リストで組んで描き、スクリプトとスタイルは走らせません。"
	"Blink の描き手は同じ口で差し替えます(設計書 17.19)。</small></p>"
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
};

Ime	g_ime;

Browser	g_br;

long now_ms()
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

sk_sp<SkData> read_face()
{
	std::vector<unsigned char>	buf;
	const SZ			piece = 256 * 1024;
	SZ				got = 0, asz = 0;
	TS_UUID				face;
	ID				k;

	if ( ts_face_find(kFaceName, &face) < E_OK && ts_face_find(nullptr, &face) < E_OK ) {
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

/* The whole work area drawn and shown */
void show()
{
	SkCanvas	*c = g_br.surf->getCanvas();
	SkPaint		p;

	p.setAntiAlias(true);

	/* the page */
	c->save();
	c->clipRect(SkRect::MakeWH((float)g_br.w, (float)view_h()));
	if ( g_br.eng != nullptr ) {
		g_br.eng->paint(c, g_br.scroll, view_h());
	} else {
		c->clear(SK_ColorWHITE);
	}
	c->restore();

	/* what is happening, on the message line */
	if ( g_br.status != g_br.shown_status ) {
		g_br.shown_status = g_br.status;
		(void)wm_msg_put(g_br.status.c_str());
	}
	show_bars();

	if ( g_ime.list && g_br.eng != nullptr ) {
		/* the converter's candidates under the caret, a number before each */
		int cx = 8, cy = 8, ch = 16;
		(void)g_br.eng->caret(&cx, &cy, &ch);
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
		if ( top + g_ime.row_h * n + 4 > g_br.h ) top = (float)( cy - g_ime.row_h * n - 6 );
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

	SkPixmap pm;
	if ( !g_br.surf->peekPixels(&pm) ) return;
	for ( int y = 0; y < g_br.h; y++ ) {
		const uint32_t *row = pm.addr32(0, y);
		UW *out = &g_br.px[(size_t)y * g_br.w];
		for ( int x = 0; x < g_br.w; x++ ) out[x] = row[x] & 0x00FFFFFF;
	}
	(void)dp_put_argb(g_br.gid, 0, 0, g_br.px.data(), g_br.w, g_br.w, g_br.h, 0xFF000000);
	(void)wm_obj_flush(g_br.kw, nullptr);
}

void set_title( const std::string &t )
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
	(void)ob_set_atr(g_br.kw, (const UB *)j.data(), (SZ)j.size());
}

/*
 * The window's band names the page shown: its title, or its address
 * while it has none. Asked again after the page's own work, as a script
 * may change the title at any time.
 */
void show_title()
{
	std::string t = g_br.title.empty() ? g_br.url : g_br.title;
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
	int w = pos.wright - pos.wleft, h = pos.wbottom - pos.wtop;
	if ( w < 120 ) w = 120;
	if ( h < 40 ) h = 40;
	if ( w == g_br.w && h == g_br.h ) return false;
	g_br.w = w;
	g_br.h = h;
	g_br.surf = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(w, h));
	g_br.px.assign((size_t)w * h, 0);
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

void go( const std::string &to, bool record, const std::string &method = std::string(),
	 const std::string &ctype = std::string(), const std::string &body = std::string() );
void go_back();
void go_forward();

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
enum { N_PAGE = 0, N_TOOL = 2, N_PARTS = 3 };

struct Tool {
	TS_UUID		win, pnl, part[T_NPART];
	ID		kw = 0, pk = 0, key[T_NPART] = {};
	INT		gid = -1;
	int		w = 0, h = 0;		/* its work area */
	SkRect		field;			/* the address field, in the work area */
	std::string	text = "http://";
	bool		focus = false;
	bool		sel_all = false;	/* Ctrl+A: the next key acts on the whole address */
	bool		stop = false;		/* 停止 pressed while a page loads */
};

Tool	g_tool;

/* The address field drawn: white, a thin edge, the address, the caret when it has the focus */
void tool_redraw()
{
	if ( g_tool.kw <= 0 || g_tool.gid < 0 ) return;
	int w = (int)g_tool.field.width(), h = (int)g_tool.field.height();
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
	const std::string &t = g_tool.text;
	float adv = t.empty() ? 0 : f.measureText(t.data(), t.size(), SkTextEncoding::kUTF8);
	float tx = 6, ty = (float)h / 2 + 5;
	if ( tx + adv > w - 8 ) tx = w - 8 - adv;	/* the end of a long address in view */
	if ( g_tool.focus && g_tool.sel_all && !t.empty() ) {
		SkPaint hl;
		hl.setColor(0xFF99CCFF);
		c->drawRect(SkRect::MakeLTRB(tx - 2, 3, tx + adv + 2 > w - 2 ? w - 2 : tx + adv + 2, (float)h - 3), hl);
	}
	if ( !t.empty() ) c->drawSimpleText(t.data(), t.size(), SkTextEncoding::kUTF8, tx, ty, f, ink);
	if ( g_tool.focus ) {
		SkPaint cp;
		cp.setColor(0xFF2255CC);
		c->drawRect(SkRect::MakeXYWH(tx + adv + 1, 4, 1.5f, (float)h - 8), cp);
	}
	SkPixmap pm;
	if ( !s->peekPixels(&pm) ) return;
	std::vector<UW> px((size_t)w * h);
	for ( int y = 0; y < h; y++ ) {
		const uint32_t *row = pm.addr32(0, y);
		for ( int x = 0; x < w; x++ ) px[(size_t)y * w + x] = row[x] & 0x00FFFFFF;
	}
	(void)dp_put_argb(g_tool.gid, (INT)g_tool.field.left(), (INT)g_tool.field.top(), px.data(), w, w, h,
			  0xFF000000);
	(void)wm_obj_flush(g_tool.kw, nullptr);
}

/* The address shown after a page ("http://" when there is none to show) */
void tool_set_url( const std::string &url )
{
	g_tool.text = ( url.empty() || url.compare(0, 6, "about:") == 0 ) ? std::string("http://") : url;
	g_tool.sel_all = false;
	tool_redraw();
}

/* The field given the focus (Ctrl+L, the menu's アドレス入力), the whole address taken */
void tool_focus()
{
	g_tool.focus = true;
	g_tool.sel_all = !g_tool.text.empty();
	tool_redraw();
}

bool tool_open()
{
	T_OBWPOS	wp;
	T_OBCRE		c;
	T_OBNTF		req;
	SZ		asz = 0;
	char		json[256];

	if ( ob_rea_rec(g_br.kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) < E_OK ) return false;
	int top = wp.top - ( kToolH + kToolGap );
	if ( top < 30 ) top = 30;
	snprintf(json, sizeof(json), "{\"rect\":[%d,%d,%d,%d],\"attr\":%d,\"sub\":\"%s\"}", wp.left, top,
		 wp.left + kToolW, top + kToolH, WM_ATTR_FRAME, br_uuid_str(g_br.win.b).c_str());
	memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (CONST UB *)"";
	c.json = (CONST UB *)json;
	c.jsonsz = (SZ)strlen(json);
	if ( ob_cre_obj(&c, &g_tool.win) < E_OK ) return false;
	g_tool.kw = ob_opn_obj(&g_tool.win, OB_OP_ALL);
	g_tool.gid = ( g_tool.kw > 0 ) ? wm_obj_gid(g_tool.kw) : E_ID;
	if ( g_tool.kw <= 0 || g_tool.gid < 0
	     || ob_rea_rec(g_tool.kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) < E_OK ) {
		return false;
	}
	g_tool.w = wp.wright - wp.wleft;
	g_tool.h = wp.wbottom - wp.wtop;
	int pt = 3, pb = g_tool.h - 3;

	/* the switches, on a panel placed at the left of the work area */
	snprintf(json, sizeof(json), "{\"tessronos\":{\"part\":{\"sub\":\"panel\",\"kind\":%d,\"rect\":[0,0,%d,%d]}}}",
		 WM_PNL_BARE, 6 + 32 * T_NPART, g_tool.h);
	memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_PANEL;
	c.json = (CONST UB *)json;
	c.jsonsz = (SZ)strlen(json);
	if ( ob_cre_obj(&c, &g_tool.pnl) < E_OK ) return false;
	g_tool.pk = ob_opn_obj(&g_tool.pnl, OB_OP_ALL);
	T_OBMAP m;
	memset(&m, 0, sizeof(m));
	if ( g_tool.pk <= 0 || ob_map_rec(g_tool.pk, 0, g_tool.kw, &m) < E_OK ) return false;
	for ( int i = 0; i < T_NPART; i++ ) {
		snprintf(json, sizeof(json), "{\"tessronos\":{\"part\":{\"kind\":%d,\"rect\":[0,0,28,%d]}}}", MS_PARTS,
			 pb - pt);
		memset(&c, 0, sizeof(c));
		c.type = OB_T_WINDOW;
		c.sub = OB_S_PART;
		c.name = (CONST UB *)kToolLabel[i];
		c.json = (CONST UB *)json;
		c.jsonsz = (SZ)strlen(json);
		if ( ob_cre_obj(&c, &g_tool.part[i]) < E_OK ) return false;
		g_tool.key[i] = ob_opn_obj(&g_tool.part[i], OB_OP_ALL);
		memset(&m, 0, sizeof(m));
		m.x = 6 + 32 * i;
		m.y = pt;
		if ( g_tool.key[i] <= 0 || ob_map_rec(g_tool.key[i], 0, g_tool.pk, &m) < E_OK ) return false;
	}
	g_tool.field = SkRect::MakeLTRB((float)( 6 + 32 * T_NPART + 4 ), (float)pt, (float)( g_tool.w - 4 ),
					(float)pb);

	/* its presses, keys and redrawing, and the switches', come to the page's port */
	memset(&req, 0, sizeof(req));
	req.events = OB_E_PRESS | OB_E_KEY | OB_E_REDRAW | OB_E_CLOSE;
	req.id = N_TOOL;
	if ( ob_ntf_evt(g_tool.kw, OB_REC_ANY, &req, g_br.port) <= 0 ) return false;
	memset(&req, 0, sizeof(req));
	req.events = OB_E_CHANGE;
	req.id = N_PARTS;
	if ( ob_ntf_evt(g_tool.pk, OB_REC_ANY, &req, g_br.port) <= 0 ) return false;
	tool_redraw();
	printf("browser: tool panel %dx%d\n", g_tool.w, g_tool.h);
	return true;
}

void tool_close()
{
	for ( int i = 0; i < T_NPART; i++ ) {
		if ( g_tool.key[i] > 0 ) {
			ob_cls_obj(g_tool.key[i]);
			(void)ob_del_obj(&g_tool.part[i]);
		}
	}
	if ( g_tool.pk > 0 ) {
		ob_cls_obj(g_tool.pk);
		(void)ob_del_obj(&g_tool.pnl);
	}
	if ( g_tool.kw > 0 ) {
		ob_cls_obj(g_tool.kw);
		(void)ob_del_obj(&g_tool.win);
	}
	g_tool.kw = g_tool.pk = 0;
}

/* A press on the tool window outside the switches: the field takes the focus, or gives it up */
void tool_press( int x, int y )
{
	bool in = g_tool.field.contains((float)x, (float)y);
	if ( in != g_tool.focus ) {
		g_tool.focus = in;
		g_tool.sel_all = false;
		tool_redraw();
	}
}

/* A key while the field has the focus */
void tool_key( UINT code, UINT mods )
{
	bool ctrl = ( mods & kCtrl ) != 0;
	std::string &t = g_tool.text;

	if ( ctrl && code == K_A ) {
		g_tool.sel_all = !t.empty();
		tool_redraw();
		return;
	}
	if ( code == K_ENTER || code == K_KPENTER ) {
		std::string to = t;
		g_tool.focus = false;
		g_tool.sel_all = false;
		tool_redraw();
		if ( !to.empty() ) go(to, true);
		return;
	}
	if ( code == K_ESC ) {
		g_tool.focus = false;
		g_tool.sel_all = false;
		tool_set_url(g_br.url);
		return;
	}
	if ( code == K_BS ) {
		if ( g_tool.sel_all ) {
			t.clear();
		} else {
			/* one letter back, over the continuation bytes of UTF-8 */
			size_t n = t.size();
			while ( n > 0 && ( (unsigned char)t[n - 1] & 0xC0 ) == 0x80 ) n--;
			if ( n > 0 ) n--;
			t.resize(n);
		}
		g_tool.sel_all = false;
		tool_redraw();
		return;
	}
	UB ch = wm_key_char(code, mods);
	if ( ch < 0x20 || ch >= 0x7F || ctrl ) return;
	if ( g_tool.sel_all ) t.clear();		/* typing replaces the whole address */
	g_tool.sel_all = false;
	t += (char)ch;
	tool_redraw();
}

/* A switch of the panel pushed, known by its link record */
void tool_switch( INT recno )
{
	switch ( recno - OB_WR_KIND ) {
	case T_BACK:	go_back(); break;
	case T_FWD:	go_forward(); break;
	case T_RELOAD:	go(g_br.url, false); break;
	case T_STOP:	break;		/* nothing loads now: a load looks for it itself */
	default:	break;
	}
}

/*
 * While a page loads: whether 停止 has been pushed. Other notices that
 * come meanwhile are kept, to be handled when the load is over.
 */
std::vector<T_OBNTM>	g_later;

bool tool_stop_pushed()
{
	T_OBNTM	m;
	SZ	asz = 0;

	if ( g_tool.stop ) return true;
	while ( g_br.port > 0 && ob_rea_rec(g_br.port, 0, 0, &m, sizeof(m), &asz) >= E_OK ) {
		if ( m.id == N_PARTS && m.event == OB_E_CHANGE && m.recno - OB_WR_KIND == T_STOP ) {
			printf("browser: stopped\n");
			g_tool.stop = true;
			return true;
		}
		if ( g_later.size() < 256 ) g_later.push_back(m);
	}
	return false;
}

void go( const std::string &to, bool record, const std::string &method,
	 const std::string &ctype, const std::string &body )
{
	std::string	target = to;
	BrResponse	r;
	long		t0 = now_ms();

	if ( target.find("://") == std::string::npos && target.compare(0, 6, "about:") != 0 ) {
		target = "http://" + target;
	}
	if ( record && !g_br.url.empty() ) {
		g_br.back.push_back(g_br.url);
		g_br.fwd.clear();
	}
	g_br.url = target;
	g_br.status = "繋いでいます…";
	g_tool.stop = false;
	g_br.failed = false;
	show();

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
	} else if ( !br_fetch(target, &r, progress, nullptr, method, ctype, body) ) {
		std::string msg = r.error;
		r.body = "<html><head><title>読めません</title></head><body><h2>頁が読めません</h2><p>"
			 + msg + "</p><p>" + target + "</p></body></html>";
		r.url = target;
		r.charset = "utf-8";
		g_br.status = msg;
		g_br.failed = true;
		printf("browser: failed %s: %s\n", target.c_str(), msg.c_str());
	}
	g_br.url = r.url;
	g_br.eng->load(r.url, r.body, r.charset);
	g_br.title = g_br.eng->title();
	g_br.scroll = 0;
	relayout();
	if ( r.error.empty() ) {
		char buf[128];
		snprintf(buf, sizeof(buf), "  %d  %zu バイト  %ld ms", r.status, r.body.size(), now_ms() - t0);
		g_br.status = g_br.title.substr(0, 160) + buf;
		printf("browser: loaded %s status %d bytes %zu height %d title %s\n",
		       r.url.c_str(), r.status, r.body.size(), g_br.doc_h, g_br.title.c_str());
	}
	if ( g_br.obj.ok ) {
		/* the page object follows what is shown */
		g_br.obj.url = g_br.url;
		g_br.obj.back = g_br.back;
		g_br.obj.fwd = g_br.fwd;
		if ( !br_obj_save(g_br.obj, g_br.title) ) printf("browser: the page object was not written\n");
	}
	show_title();
	show();
	tool_set_url(g_br.url);
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

void go_back()
{
	if ( g_br.back.empty() ) return;
	g_br.fwd.push_back(g_br.url);
	std::string u = g_br.back.back();
	g_br.back.pop_back();
	go(u, false);
}

void go_forward()
{
	if ( g_br.fwd.empty() ) return;
	g_br.back.push_back(g_br.url);
	std::string u = g_br.fwd.back();
	g_br.fwd.pop_back();
	go(u, false);
}

/* Where the page asked to go, a form sent with POST with its body; false for nowhere */
bool follow_page()
{
	std::string to = g_br.eng->navigation();
	if ( to.empty() ) return false;
	std::string method, ctype, body;
	if ( g_br.eng->navigation_body(&method, &ctype, &body) ) {
		go(to, true, method, ctype, body);
	} else {
		go(to, true);
	}
	return true;
}

/* After input the page took: where it asked to go, or what it shows now */
void after_input()
{
	if ( follow_page() ) return;
	relayout();
	show();
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
		if ( end > 0 ) g_br.eng->commit(std::string(o.text, (size_t)end));
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
	g_br.eng->compose(end > base ? std::string(o.text + base, (size_t)( end - base )) : std::string(),
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
	g_br.eng->commit(buf);
	return true;
}

/*
 * A key typed while a field of the page has the focus, in the input mode
 * the message line shows: true when the converter (or full width) took
 * it, false when it goes to the page as a key
 */
bool ime_key( UINT code, UINT mods )
{
	UINT	mode = kc_mode();
	bool	han = ( mode & WM_MODE_HAN ) != 0;
	UB	ch = wm_key_char(code, mods);
	INT	r;

	/*
	 * 英語: the page takes the key as it is, whatever the width the mode
	 * says (a page's field wants the letters of the keyboard: addresses,
	 * names, numbers)
	 */
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
	r = kc_hid_key(g_ime.kid, code, mods, &g_ime.out);
	if ( r >= 0 && r != KC_NOTMINE ) {
		ime_take(r);
		return true;
	}
	/* nothing being composed: a space as the mode types it, the rest the page's */
	if ( ch == ' ' && !ime_busy() ) return ime_plain(' ', han);
	return ime_busy();
}

/* Whatever is being composed, committed (a press elsewhere, the address) */
void ime_commit()
{
	g_ime.list = false;
	if ( !ime_busy() ) return;
	INT r = kc_hid_key(g_ime.kid, K_ENTER, 0x02, &g_ime.out);
	if ( r >= 0 && r != KC_NOTMINE ) ime_take(r);
	g_ime.base = g_ime.out.cl[g_ime.out.n_cl];
	g_ime.list = false;
}

/* A press on the candidates: that one chosen; true when it was theirs */
bool ime_press( int x, int y )
{
	if ( !g_ime.list || g_ime.row_h <= 0 || !g_ime.box.contains((float)x, (float)y) ) return false;
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

/* A key; false when the window is to close */
bool key( UINT code, UINT mods )
{
	bool ctrl = ( mods & kCtrl ) != 0, alt = ( mods & kAlt ) != 0;

	if ( ctrl && code == K_E ) return false;
	if ( ctrl && code == K_L ) {
		start_address();
		return true;
	}
	if ( ( ctrl && code == K_R ) || code == K_F5 ) {
		go(g_br.url, false);
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
		if ( g_br.eng->editable() && ime_key(code, mods) ) {
			after_input();
			return true;
		}
		bool editable = g_br.eng->editable();
		bool took = g_br.eng->key(code, mods, key_letter(code, mods));
		if ( !took && !editable && code == K_BS ) {
			go_back();
			return true;
		}
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

/* Whether the item chosen has this code (the system's mn_is is not a call a process has) */
bool chose( const T_MNSEL &sel, const char *code )
{
	return strncmp((const char *)sel.code, code, sizeof(sel.code)) == 0;
}

/* The window's menu (its definition includes the menu every window has); false to close */
bool menu( const T_OBNTM &m )
{
	TS_UUID		def;
	T_MNSEL		sel;
	ID		mid;
	bool		keep = true;

	ime_commit();
	if ( !parse_uuid(SYSDEF_MENU_BROWSER, &def) || mn_cre_men(&def, &mid) < E_OK ) return true;
	if ( mn_pop_men(mid, g_br.kw, m.x, m.y, m.when, &sel) >= E_OK ) {
		printf("browser: menu: %s\n", (const char *)sel.code);
		if ( chose(sel, "close") ) {
			keep = false;
		} else if ( chose(sel, "back") ) {
			go_back();
		} else if ( chose(sel, "forward") ) {
			go_forward();
		} else if ( chose(sel, "reload") ) {
			go(g_br.url, false);
		} else if ( chose(sel, "address") ) {
			start_address();
		} else if ( chose(sel, "home") ) {
			go("about:home", true);
		}
	}
	(void)mn_del_men(mid);
	return keep;
}

/* A button went down (code 0 the left, 1 the right); false when the window is to close */
bool press( const T_OBNTM &m )
{
	int x = m.x, y = m.y;

	if ( m.code == 1 ) return menu(m);
	if ( x < 0 || y < 0 || x >= g_br.w || y >= view_h() ) {
		return true;		/* the frame, its bars: the desktop works them */
	}
	if ( ime_press(x, y) ) {
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
		g_br.eng->mouse(BR_M_DOWN, x, y, 0, m.mods);
		after_input();
		return true;
	}
	if ( g_br.eng->click(x, y + g_br.scroll) ) {
		/* the page took it: what it asks for, and what it shows now */
		after_input();
		return true;
	}
	std::string to = g_br.eng->link_at(x, y + g_br.scroll);
	if ( !to.empty() ) go(to, true);
	return true;
}

void release( const T_OBNTM &m )
{
	if ( m.code != 0 || g_br.eng == nullptr || !g_br.eng->input() ) return;
	g_br.eng->mouse(BR_M_UP, m.x, m.y, 0, m.mods);
	after_input();
}

struct Script {
	bool		has_obj = false;
	unsigned char	obj[16];
	std::string	url;
	bool		follow = false;
	bool		mem = false;
	bool		trace = false;
	std::string	ready;
	long		quit = -1;
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
	if ( n >= 16 && ( n == 16 || a[16] == '\n' ) ) {
		s.has_obj = true;
		memcpy(s.obj, a.data(), 16);
		a = ( n > 17 ) ? a.substr(17) : std::string();
		first = false;
	}
	a.resize(strnlen(a.c_str(), a.size()));
	size_t p = 0;
	while ( p < a.size() ) {
		size_t e = a.find('\n', p);
		std::string line = a.substr(p, ( e == std::string::npos ) ? std::string::npos : e - p);
		if ( first ) {
			if ( line.compare(0, 4, "http") == 0 || line.compare(0, 6, "about:") == 0 ) s.url = line;
			first = false;
		} else if ( line == "follow" ) {
			s.follow = true;
		} else if ( line == "mem" ) {
			s.mem = true;
		} else if ( line == "trace" ) {
			s.trace = true;
		} else if ( line.compare(0, 6, "ready=") == 0 ) {
			s.ready = line.substr(6);
		} else if ( line.compare(0, 5, "quit=") == 0 ) {
			s.quit = atol(line.c_str() + 5);
		}
		if ( e == std::string::npos ) break;
		p = e + 1;
	}
	return s;
}

/* Whether a window says it shows the object already; that window brought to the front */
bool already_shown( const BrObject &o )
{
	TS_UUID		list[64];
	INT		cnt = 0;
	std::string	want = "\"shows\":\"" + br_uuid_str(o.id) + "\"";
	bool		yes = false;

	if ( ob_lst_obj(OB_T_WINDOW, OB_S_WINDOW, nullptr, list, 64, &cnt) < E_OK ) return false;
	for ( INT i = 0; i < cnt && i < 64 && !yes; i++ ) {
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
	return yes;
}

}  // namespace

int main()
{
	T_OBCRE		c;
	T_OBNTF		req;
	TS_UUID		ch;
	Script		sc = read_script();
	BrObject	&o = g_br.obj;
	char		json[256];

	if ( sc.has_obj && !br_obj_open(sc.obj, &o) ) {
		printf("browser: the page object %s cannot be read\n", br_uuid_str(sc.obj).c_str());
		return 5;
	}
	if ( o.ok && already_shown(o) ) {
		/* one window an object: the one there is brought forward, and this one goes */
		(void)wm_msg_put("この頁の実身はもう開いています");
		printf("browser: %s is open already\n", br_uuid_str(o.id).c_str());
		return 0;
	}
	/* the window where the object keeps it, and the object it shows */
	if ( o.ok ) {
		snprintf(json, sizeof(json), "{\"rect\":[%d,%d,%d,%d],\"attr\":31,\"shows\":\"%s\"}",
			 o.x, o.y, o.x + o.w + kFrameW, o.y + o.h + kTitleH, br_uuid_str(o.id).c_str());
	} else {
		snprintf(json, sizeof(json), "{\"rect\":[%d,%d,%d,%d],\"attr\":31}",
			 o.x, o.y, o.x + o.w + kFrameW, o.y + o.h + kTitleH);
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
	g_br.kw = ob_opn_obj(&g_br.win, OB_OP_ALL);
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
	/* the page takes the pointer's moves and the buttons coming up too */
	req.events |= OB_E_RELEASE | OB_E_MOVE;
#endif
	if ( g_br.kw <= 0 || g_br.gid < 0 || g_br.port <= 0 || ob_ntf_evt(g_br.kw, OB_REC_ANY, &req, g_br.port) <= 0 ) {
		printf("browser: the window cannot be used\n");
		(void)ob_del_obj(&g_br.win);
		return 2;
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
#ifdef BR_BLINK
	g_br.eng = br_blink_engine(data);
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
	printf("browser: up, %dx%d\n", g_br.w, g_br.h);

	g_br.back = o.back;
	g_br.fwd = o.fwd;
	/*
	 * The address the page object keeps (the base holds the first one);
	 * an object that keeps none shows an empty page, and the browser
	 * started with no object its own page
	 */
	go(!sc.url.empty() ? sc.url : !o.url.empty() ? o.url : o.ok ? "about:blank" : "about:home", false);
	if ( sc.follow && g_br.eng != nullptr ) {
		/* the first link of the page, as a press on it would */
		std::string first = g_br.eng->first_link();
		for ( int y = 0; y < g_br.doc_h && first.empty(); y += 4 ) {
			for ( int x = 0; x < g_br.w && first.empty(); x += 8 ) first = g_br.eng->link_at(x, y);
		}
		if ( !first.empty() ) go(first, true);
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
	long quit_at = ( sc.quit >= 0 ) ? now_ms() + sc.quit : -1;

	for ( bool done = false; !done; ) {
		T_OBNTM	m;
		SZ	asz = 0;

		if ( quit_at >= 0 && now_ms() >= quit_at ) break;
		bool have = false;
		if ( !g_later.empty() ) {
			m = g_later.front();
			g_later.erase(g_later.begin());
			have = true;
		} else {
			have = ob_rea_rec(g_br.port, 0, 0, &m, sizeof(m), &asz) >= E_OK;
		}
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
			}
			(void)follow_page();
#ifdef BR_BLINK
			br_store_tick();
#endif
			tk_dly_tsk(10);
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
		if ( m.id == N_PARTS ) {
			if ( m.event == OB_E_CHANGE ) tool_switch(m.recno);
			continue;
		}
		if ( m.id == N_TOOL ) {
			/* the tool panel's own: its field, its keys, its drawing */
			if ( m.event == OB_E_PRESS && m.code == 0 ) tool_press(m.x, m.y);
			else if ( m.event == OB_E_PRESS && m.code == 1 ) done = !menu(m);
			else if ( m.event == OB_E_KEY ) done = !key(m.code, m.mods);
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
			if ( !key(m.code, m.mods) ) done = true;
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
		case OB_E_REDRAW:
			if ( read_size() ) relayout();
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
			if ( g_br.eng->input() ) {
				/* the scroller under the pointer: the page's, or one in it */
				g_br.eng->wheel(m.x, m.y, -m.dz);
				after_input();
			} else {
				scroll_by(-m.dz * 48);
			}
			break;
#endif
		default:
			break;
		}
	}

	if ( o.ok ) {
		/* where the window was, for the next time the object is opened */
		T_OBWPOS	wp;
		SZ		asz = 0;

		if ( ob_rea_rec(g_br.kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK && asz >= (SZ)sizeof(wp) ) {
			o.x = wp.left;
			o.y = wp.top;
			o.w = wp.wright - wp.wleft;
			o.h = wp.wbottom - wp.wtop;
			(void)br_obj_save_window(o);
		}
	}
	if ( g_ime.kid > 0 ) kc_close(g_ime.kid);
	tool_close();
	(void)wm_msg_put("");
	printf("browser: closed\n");
	ob_cls_obj(g_br.port);
	(void)ob_del_obj(&ch);
	ob_cls_obj(g_br.kw);
	(void)ob_del_obj(&g_br.win);
	int rc = ( !sc.ready.empty() && g_br.failed ) ? 4 : 0;
#ifdef BR_BLINK
	/*
	 * The engine's threads and its objects are not taken down one by one:
	 * the process ends here, and the system takes back what it had
	 */
	br_store_flush_all();
	fflush(stdout);
	_exit(rc);
#endif
	return rc;
}
