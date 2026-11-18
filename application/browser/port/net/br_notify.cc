/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_notify.cc
 *	ブラウザ: the pages' notifications shown by the desktop (DT_RQ_NOTIFY,
 *	design 14.4.10).
 *
 *	A notification is a request to the desktop, which shows it in a
 *	small window of its own at the screen's top right. What comes of it
 *	-- a press of 〈開く〉 or of one of its buttons, its closing by the
 *	person, its going by itself -- is answered at one reply channel the
 *	browser keeps for all of them, which a thread of its own reads and
 *	hands to whoever showed it (br_note_watch), by the number the
 *	notification was shown with.
 */

#include TS_CONFIG_H
#include <stdint.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/uapp.h>
#include <ts/ob.h>
#include <ts/dtreq.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../br.h"

namespace {

std::mutex		g_lock;
bool			g_up = false;
TS_UUID			g_reply;			/* the channel the desktop answers at */
unsigned		g_seq = 0;
std::map<unsigned, int>	g_ids;				/* the request's seq -> the notification's number */
BrNoteWatch		g_watch = nullptr;

/* The answers, one at a time, handed on by the number they are for */
void watch( ID ka )
{
	std::vector<UB>	buf(sizeof(T_DTANS) + 256);

	for ( ;; ) {
		T_DTANS	an;
		SZ	asz = 0;
		ER	er = ob_rea_rec(ka, 0, 0, buf.data(), (SZ)buf.size(), &asz);

		if ( er == E_LIMIT && asz > (SZ)buf.size() ) {
			buf.resize((size_t)asz);	/* an answer with a longer tail: read again, whole */
			continue;
		}
		if ( er < E_OK || asz < (SZ)sizeof(an) ) {
			usleep(50000);
			continue;
		}
		memcpy(&an, buf.data(), sizeof(an));
		if ( an.how != DT_HOW_NOTE_CLICK && an.how != DT_HOW_NOTE_CLOSED && an.how != DT_HOW_NOTE_GONE ) {
			continue;		/* shown: nothing to tell */
		}
		int id = -1;
		BrNoteWatch w;
		{
			std::lock_guard<std::mutex> hold(g_lock);
			auto it = g_ids.find(an.seq);
			if ( it == g_ids.end() ) continue;
			id = it->second;
			g_ids.erase(it);
			w = g_watch;
		}
		if ( w != nullptr ) {
			w(id, an.how == DT_HOW_NOTE_CLICK ? BR_NOTE_CLICK
			      : an.how == DT_HOW_NOTE_CLOSED ? BR_NOTE_CLOSED : BR_NOTE_GONE,
			  (int)an.flags);
		}
	}
}

/* The reply channel and its reader, made the first time a notification is shown */
bool up()
{
	if ( g_up ) return true;
	T_OBCRE c;
	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( ob_cre_obj(&c, &g_reply) < E_OK ) return false;
	ID ka = ob_opn_obj(&g_reply, OB_OP_READ);
	if ( ka <= 0 ) {
		(void)ob_del_obj(&g_reply);
		return false;
	}
	std::thread(watch, ka).detach();
	g_up = true;
	return true;
}

/* The request sent with its words in its tail, each as long as it is (ts/dtreq.h, DT_TAIL_*) */
ER send( T_DTREQ *rq, const std::vector<std::pair<UINT, std::string>> &parts )
{
	TS_UUID		d;
	SZ		asz = 0, cap = 0;
	std::vector<UB>	msg;
	UINT		tsz = 0;

	for ( const auto &p : parts ) cap += dt_tail_room((SZ)p.second.size());
	msg.resize(sizeof(*rq) + (size_t)cap + 1);
	for ( const auto &p : parts ) {
		if ( dt_tail_put(msg.data() + sizeof(*rq), &tsz, cap, p.first, (CONST UB *)p.second.data(),
				 (SZ)p.second.size()) < E_OK ) return E_NOMEM;
	}
	rq->tailsz = tsz;
	memcpy(msg.data(), rq, sizeof(*rq));
	if ( ob_fnd_nam((CONST UB *)DT_REQ_NAME, &d) < E_OK ) return E_NOEXS;
	ID k = ob_opn_obj(&d, OB_OP_WRITE);
	if ( k <= 0 ) return (ER)k;
	ER er = ob_wri_rec(k, 0, 0, msg.data(), (SZ)( sizeof(*rq) + tsz ), &asz);
	ob_cls_obj(k);
	return er;
}

}  // namespace

void br_note_watch( BrNoteWatch w )
{
	std::lock_guard<std::mutex> hold(g_lock);
	g_watch = w;
}

bool br_note_show( int id, const std::string &source, const std::string &title, const std::string &body,
		   const std::vector<std::string> &actions, bool stay )
{
	T_DTREQ	rq;

	std::lock_guard<std::mutex> hold(g_lock);
	if ( !up() ) return false;
	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_NOTIFY;
	rq.seq = ++g_seq;
	rq.reply = g_reply;
	rq.recno = id;
	rq.flags = stay ? DT_F_NOTE_STAY : 0;
	/* the source, the title and body, and the two actions, each as long as it is */
	std::vector<std::pair<UINT, std::string>> parts = {
		{ DT_TAIL_NAME, source }, { DT_TAIL_TEXT, title + "\n" + body } };
	for ( size_t i = 0; i < actions.size() && i < 2; i++ ) parts.push_back({ i == 0 ? DT_TAIL_ASK1 : DT_TAIL_ASK2, actions[i] });
	/* a notification shown again under its number: what came of the old one is not told */
	for ( auto it = g_ids.begin(); it != g_ids.end(); ) {
		it = ( it->second == id ) ? g_ids.erase(it) : std::next(it);
	}
	if ( send(&rq, parts) < E_OK ) {
		printf("browser: notification %d: the desktop is not there\n", id);
		return false;
	}
	g_ids[rq.seq] = id;
	return true;
}

void br_note_close( int id )
{
	T_DTREQ	rq;

	std::lock_guard<std::mutex> hold(g_lock);
	if ( !g_up ) return;
	for ( auto it = g_ids.begin(); it != g_ids.end(); ) {
		it = ( it->second == id ) ? g_ids.erase(it) : std::next(it);
	}
	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_NOTIFY;
	rq.seq = ++g_seq;
	rq.reply = g_reply;
	rq.recno = id;
	rq.flags = DT_F_NOTE_CLOSE;
	(void)send(&rq, {});
}
