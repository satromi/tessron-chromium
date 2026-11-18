/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_print.cc
 *	ブラウザ: the pages of 印刷 sent to the desktop (design 14.7.5).
 *
 *	The pages are put in an object of pages as a process prints
 *	(include/ts/uiprn.h): each page a figure on A4 with margins of 57
 *	points, holding the page's picture where the margins leave off and
 *	the page's letters as pieces of text over it, and the picture a
 *	resource of the object beside the page's record. The
 *	desktop is asked with DT_RQ_PRINT; its panels say how, and it puts
 *	the PDF it writes on the desktop. The page waits for the answer, as
 *	window.print() waits while a browser's print dialog is up.
 */

#include TS_CONFIG_H
#include <stdint.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/uapp.h>
#include <ts/ob.h>
#include <ts/uiprn.h>
#include <ts/dtreq.h>
#include <ts/wm.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
#include <string>
#include <vector>

#include "../../br.h"

namespace {

/* The paper of the pages: A4 and its margins, in points */
constexpr int	kPaperW = 595, kPaperL = 842, kMargin = 57;

std::atomic<int>	g_printed{0};		/* pages the desktop printed */
std::atomic<int>	g_answer{1};		/* its last answer; 1 for none yet */

}  // namespace

/*
 * The desktop asked, its answer waited for (the person works its panels,
 * carries the outline). The words of the request -- a name, letters --
 * go in its tail (ts/dtreq.h, DT_TAIL_*), each as long as it is; the
 * answer is read whole, its tail with it (read again when the channel
 * says it is longer than the room), and its words are put in *words by
 * their kind when words is given.
 */
typedef std::vector<std::pair<UINT, std::string>> BrDtParts;

ER br_ask_desktop( T_DTREQ *rq, const BrDtParts &parts, T_DTANS *an, int wait_ms, BrDtParts *words )
{
	static unsigned	seq;
	TS_UUID		d, reply;
	T_OBCRE		c;
	SZ		asz = 0, cap = 0;
	ER		er;
	std::vector<UB>	tail, msg, ans(sizeof(T_DTANS) + 256);
	UINT		tsz = 0;

	for ( const auto &p : parts ) cap += dt_tail_room((SZ)p.second.size());
	tail.resize((size_t)cap + 1);
	for ( const auto &p : parts ) {
		if ( dt_tail_put(tail.data(), &tsz, cap, p.first, (CONST UB *)p.second.data(), (SZ)p.second.size()) < E_OK ) {
			return E_NOMEM;
		}
	}
	if ( ob_fnd_nam((CONST UB *)DT_REQ_NAME, &d) < E_OK ) return E_NOEXS;
	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( ob_cre_obj(&c, &reply) < E_OK ) return E_NOMEM;
	ID ka = ob_opn_obj(&reply, OB_OP_READ | OB_O_NOWAIT);
	if ( ka <= 0 ) {
		(void)ob_del_obj(&reply);
		return E_OBJ;
	}
	rq->seq = ++seq;
	rq->reply = reply;
	rq->tailsz = tsz;
	msg.resize(sizeof(*rq) + tsz);
	memcpy(msg.data(), rq, sizeof(*rq));
	if ( tsz > 0 ) memcpy(msg.data() + sizeof(*rq), tail.data(), tsz);
	ID k = ob_opn_obj(&d, OB_OP_WRITE);
	er = ( k > 0 ) ? ob_wri_rec(k, 0, 0, msg.data(), (SZ)msg.size(), &asz) : (ER)k;
	if ( k > 0 ) ob_cls_obj(k);
	for ( int t = 0; er >= E_OK; t += 20 ) {
		ER	re = ob_rea_rec(ka, 0, 0, ans.data(), (SZ)ans.size(), &asz);

		if ( re == E_LIMIT && asz > (SZ)ans.size() ) {
			ans.resize((size_t)asz);	/* longer than the room: still there, read again whole */
			continue;
		}
		if ( re >= E_OK && asz >= (SZ)sizeof(*an) && ((T_DTANS *)ans.data())->seq == rq->seq ) {
			memcpy(an, ans.data(), sizeof(*an));
			er = an->er;
			if ( words != nullptr ) {
				CONST UB *at = dt_ans_tail(an, asz);
				static const UINT kinds[] = { DT_TAIL_NAME, DT_TAIL_TEXT, DT_TAIL_APPLIST, DT_TAIL_DATA };

				words->clear();
				for ( UINT kd : kinds ) {
					SZ	l = 0;
					CONST UB *w = ( at != nullptr ) ? dt_tail_get(at, an->tailsz, kd, &l) : nullptr;

					if ( w != nullptr ) words->push_back({ kd, std::string((const char *)w, (size_t)l) });
				}
			}
			break;
		}
		if ( t >= wait_ms ) {
			er = E_TMOUT;
			break;
		}
		usleep(20000);
	}
	ob_cls_obj(ka);
	(void)ob_del_obj(&reply);
	return er;
}

bool br_print_send( const std::string &name, const std::vector<std::vector<uint8_t>> &pages,
		    const std::vector<std::string> &texts, int width, int height )
{
	T_UIPRN		up;
	ER		er;
	int		x0 = kMargin * 4 / 3, y0 = kMargin * 4 / 3;	/* the margins in pixels of 96 dpi */
	char		num[160];

	if ( ui_prn_open(&up, nullptr) < E_OK ) {
		printf("browser: print: no object for the pages\n");
		return false;
	}
	std::string id = br_uuid_str(up.obj.b);
	for ( size_t i = 0; i < pages.size(); i++ ) {
		std::string res = "_" + std::to_string(i) + "_1.png";
		er = ob_wri_res(up.key, (CONST UB *)res.c_str(), pages[i].data(), (SZ)pages[i].size());
		if ( er < E_OK ) {
			printf("browser: print: the picture of page %zu not written (%d)\n", i + 1, (int)er);
			break;
		}
		std::string xml = "<tad version=\"1.0\" encoding=\"UTF-8\">\n<paper type=\"fig\"";
		snprintf(num, sizeof(num), " length=\"%d\" width=\"%d\" top=\"%d\" bottom=\"%d\" left=\"%d\" right=\"%d\"/>\n",
			 kPaperL, kPaperW, kMargin, kMargin, kMargin, kMargin);
		xml += num;
		snprintf(num, sizeof(num), "<docmargin top=\"%d\" bottom=\"%d\" left=\"%d\" right=\"%d\"/>\n<figure>\n",
			 kMargin, kMargin, kMargin, kMargin);
		xml += num;
		snprintf(num, sizeof(num), "<image left=\"%d\" top=\"%d\" right=\"%d\" bottom=\"%d\" href=\"",
			 x0, y0, x0 + width, y0 + height);
		xml += num;
		xml += id + res + "\"/>\n";
		if ( i < texts.size() ) xml += texts[i];
		xml += "</figure>\n</tad>\n";
		if ( ui_prn_page(&up, (CONST UB *)xml.data(), (INT)xml.size()) < E_OK ) break;
	}
	er = ui_prn_ready(&up);
	if ( er >= E_OK ) {
		T_DTREQ	rq;
		T_DTANS	an;

		memset(&rq, 0, sizeof(rq));
		memset(&an, 0, sizeof(an));
		rq.req = DT_RQ_PRINT;
		rq.target = up.obj;
		rq.recno = -1;
		br_window_uuid(rq.win.b);
		/* the name of the file, as long as it is */
		er = br_ask_desktop(&rq, { { DT_TAIL_NAME, name } }, &an, 60 * 60 * 1000, nullptr);
	}
	printf("browser: print: %zu pages, the desktop answered %d\n", pages.size(), (int)er);
	g_answer = (int)er;
	if ( er >= E_OK ) g_printed += (int)pages.size();
	ui_prn_close(&up);
	return er >= E_OK;
}

extern std::atomic<int> g_print_texts;	/* port/blink/print_tessronos.cc */

/* What 印刷 came to, as text ("<pages printed> <the desktop's last answer> <runs of letters kept>") */
int br_print_report( char *buf, int cap )
{
	return snprintf(buf, (size_t)cap, "%d %d %d\n", g_printed.load(), g_answer.load(), g_print_texts.load());
}
