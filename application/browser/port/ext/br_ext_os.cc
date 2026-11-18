/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_ext_os.cc
 *	ブラウザ: the objects behind the extensions (design 14.12). The object
 *	"ブラウザの拡張機能" (SYSDEF_BROWSER_EXT) is an ordinary object: record
 *	0 has a link to the object of each extension, whose record 1 is its
 *	ZIP or CRX as the file conversion took it in; record 1 is the state
 *	the browser keeps of them, as JSON. Nothing but the object layer is
 *	used here, the rest being br_ext.cc, which has Blink's headers.
 *
 *	A renderer does not open these objects: it asks the network process,
 *	which reads them for it and reads a package only when it is one of
 *	the list or one the browser named (store_service_tessronos.cc).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "../net/br_store_core.h"

/* the C library names these too, and the system's headers use them as types */
#undef _B
#undef _H
#undef _W

#include TS_CONFIG_H
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/uapp.h>
#include <ts/ob.h>
#include <ts/sysdef.h>
#include <ts/uuid.h>

#include "br_ext.h"

namespace {

/*
 * Whether the objects are asked of the network process (a renderer) or
 * opened here
 */
constexpr bool kExtAsked = true;

bool ext_remote()
{
	return kExtAsked && br_store_remote();
}

constexpr INT kStateRec = 1;
constexpr INT kFileRec = 1;

/* A UUID from its text: a program has no call for it */
bool uuid_of( const char *t, TS_UUID *u )
{
	int n = 0;
	for ( ; *t != 0 && n < 32; t++ ) {
		int v = ( *t >= '0' && *t <= '9' ) ? *t - '0' : ( *t >= 'a' && *t <= 'f' ) ? *t - 'a' + 10 : -1;
		if ( *t == '-' ) continue;
		if ( v < 0 ) return false;
		if ( n % 2 == 0 ) u->b[n / 2] = (UB)( v << 4 );
		else u->b[n / 2] |= (UB)v;
		n++;
	}
	return n == 32;
}

bool list_uuid( TS_UUID *u )
{
	return uuid_of(SYSDEF_BROWSER_EXT, u);
}

/*
 * For a read, change and write back: a browser of another process has the
 * same list, and each keeps the others out for that moment.
 */
ID open_rw( const TS_UUID *u )
{
	for ( int i = 0; ; i++ ) {
		ID key = ob_opn_obj(u, OB_OP_R | OB_OP_WRITE | OB_OP_RECORD | OB_O_WEXCL);
		if ( key > 0 || key != E_BUSY || i >= 100 ) return key;
		tk_dly_tsk(50);
	}
}

bool read_rec( ID k, INT rec, std::string *out )
{
	SZ size = 0, a = 0;
	if ( ob_rea_rec(k, rec, 0, NULL, 0, &size) < E_OK ) return false;
	out->resize((size_t)size);
	if ( size == 0 ) return true;
	if ( ob_rea_rec(k, rec, 0, &(*out)[0], size, &a) < E_OK ) return false;
	out->resize((size_t)a);
	return true;
}

bool write_rec( ID k, INT rec, const std::string &s )
{
	ER er = ob_rpl_rec(k, rec, s.data(), (SZ)s.size());
	if ( er < E_OK ) printf("browser: extensions: record %d cannot be written (%d)\n", (int)rec, (int)er);
	return er >= E_OK;
}

std::string uuid_text( const TS_UUID &u )
{
	char t[TS_UUID_STRLEN + 1];
	ts_uuid_text(&u, t);
	return t;
}

}  // namespace

/* The objects linked from the list: the UUIDs in text */
std::vector<std::string> br_ext_os_links()
{
	std::vector<std::string>	out;
	std::string			tad;
	TS_UUID				u;

	if ( ext_remote() ) {
		(void)br_store_remote_ext_list(&out);
		return out;
	}
	if ( !list_uuid(&u) ) return out;
	ID k = ob_opn_obj(&u, OB_OP_R);
	if ( k <= 0 ) return out;
	if ( read_rec(k, 0, &tad) ) {
		static const char kAt[] = "<link id=\"";
		for ( size_t at = tad.find(kAt); at != std::string::npos; at = tad.find(kAt, at + 1) ) {
			size_t from = at + sizeof(kAt) - 1;
			if ( from + TS_UUID_STRLEN <= tad.size() ) out.push_back(tad.substr(from, TS_UUID_STRLEN));
		}
	}
	ob_cls_obj(k);
	return out;
}

/* An extension's bytes: the record its object was given when the file came in */
bool br_ext_os_read( const std::string &uuid, std::string *bytes, std::string *name )
{
	TS_UUID u;
	if ( ext_remote() ) return br_store_remote_ext_read(uuid, bytes);
	if ( !uuid_of(uuid.c_str(), &u) ) return false;
	ID k = ob_opn_obj(&u, OB_OP_R);
	if ( k <= 0 ) return false;
	bool ok = read_rec(k, kFileRec, bytes);
	ob_cls_obj(k);
	(void)name;
	return ok;
}

/*
 * A link to the object added to the list, unless it is there. The link
 * counts as one more reference to it, so that the object stays when the
 * tray's or another box's link to it goes.
 */
bool br_ext_os_link( const std::string &uuid, const std::string &name )
{
	std::string	tad;
	TS_UUID		u, target, vobj;

	if ( ext_remote() ) return br_store_remote_ext_link(uuid, name);
	if ( !list_uuid(&u) || !uuid_of(uuid.c_str(), &target) ) return false;
	ID k = open_rw(&u);
	if ( k <= 0 ) {
		printf("browser: extensions: the list cannot be opened (%d)\n", (int)k);
		return false;
	}
	bool ok = read_rec(k, 0, &tad);
	if ( ok && tad.find("<link id=\"" + uuid) == std::string::npos ) {
		size_t at = tad.rfind("</document>");
		ER er = at != std::string::npos ? ob_lnk_obj(&target) : E_OBJ;
		if ( er < E_OK ) printf("browser: extensions: the object cannot be linked (%d)\n", (int)er);
		ok = er >= E_OK;
		if ( ok ) {
			(void)ts_gen_uuid(&vobj);
			std::string link = "<link id=\"" + uuid + "_0.xtad\" vobjid=\"" + uuid_text(vobj)
				+ "\" vobjleft=\"8\" vobjtop=\"8\" vobjright=\"248\" vobjbottom=\"33\" height=\"25\" chsz=\"14\""
				  " frcol=\"#000000\" chcol=\"#000000\" tbcol=\"#ffffff\" bgcol=\"#ffffff\""
				  " pictdisp=\"true\" namedisp=\"true\" framedisp=\"true\"/>\n";
			(void)name;
			tad.insert(at, link);
			ok = write_rec(k, 0, tad);
		}
	}
	ob_cls_obj(k);
	return ok;
}

/* The link taken away and the reference it held given up */
bool br_ext_os_unlink( const std::string &uuid )
{
	std::string	tad;
	TS_UUID		u, target;

	if ( ext_remote() ) return br_store_remote_ext_unlink(uuid);
	if ( !list_uuid(&u) || !uuid_of(uuid.c_str(), &target) ) return false;
	ID k = open_rw(&u);
	if ( k <= 0 ) return false;
	bool ok = read_rec(k, 0, &tad);
	size_t at = ok ? tad.find("<link id=\"" + uuid) : std::string::npos;
	if ( at != std::string::npos ) {
		size_t end = tad.find("/>", at);
		if ( end != std::string::npos ) {
			end += 2;
			if ( end < tad.size() && tad[end] == '\n' ) end++;
			tad.erase(at, end - at);
			ok = write_rec(k, 0, tad) && ob_unl_obj(&target) >= E_OK;
		}
	}
	ob_cls_obj(k);
	return ok;
}

bool br_ext_os_state_read( std::string *json )
{
	TS_UUID u;
	if ( ext_remote() ) return br_store_remote_ext_state(json);
	if ( !list_uuid(&u) ) return false;
	ID k = ob_opn_obj(&u, OB_OP_R);
	if ( k <= 0 ) return false;
	bool ok = read_rec(k, kStateRec, json);
	ob_cls_obj(k);
	return ok;
}

bool br_ext_os_state_write( const std::string &json )
{
	TS_UUID u;
	if ( ext_remote() ) return br_store_remote_ext_state_write(json);
	if ( !list_uuid(&u) ) return false;
	ID k = open_rw(&u);
	if ( k <= 0 ) return false;
	bool ok = write_rec(k, kStateRec, json);
	ob_cls_obj(k);
	return ok;
}
