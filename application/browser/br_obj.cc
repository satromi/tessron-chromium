/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_obj.cc
 *	The page object ブラウザ opens (design 17.19): a real object made from
 *	the base in the template box, holding what the browser shows.
 *
 *	Its metadata keeps the state under "webBrowser" -- "url", the page's
 *	"title", and the "back" and "forward" histories -- and the window's
 *	place under "window" ("pos", "width" and "height" of the work area),
 *	as the desktop's own windows keep it. Record 0 is a TAD document
 *	with the page's title and address, so that the object says what it
 *	is when it is opened with the basic text editor. Both are written
 *	each time a page is shown, and the window's place when it closes.
 *
 *	The metadata is read and written whole (ob_get_atr, ob_set_atr) with
 *	a small JSON tree that keeps every other member as it was, in its
 *	order.
 */

#include TS_CONFIG_H
#include <stdint.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/uapp.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "br.h"

namespace {

/* ---------------------------------------------------------------- JSON */

struct Json {
	enum Kind { NUL, BOOL, NUM, STR, ARR, OBJ } kind = NUL;
	std::string	text;		/* a number or true/false as written, a string decoded */
	std::vector<Json> items;	/* an array's items */
	std::vector<std::pair<std::string, Json>> members;

	Json *member( const std::string &k )
	{
		for ( auto &m : members ) if ( m.first == k ) return &m.second;
		return nullptr;
	}
	Json &set( const std::string &k )
	{
		Json *j = member(k);
		if ( j != nullptr ) return *j;
		members.emplace_back(k, Json());
		return members.back().second;
	}
	static Json str( const std::string &s )
	{
		Json j;
		j.kind = STR;
		j.text = s;
		return j;
	}
	static Json num( long v )
	{
		Json j;
		j.kind = NUM;
		j.text = std::to_string(v);
		return j;
	}
	static Json object()
	{
		Json j;
		j.kind = OBJ;
		return j;
	}
};

struct Parser {
	const char	*p, *e;

	void ws()
	{
		while ( p < e && ( *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ) ) p++;
	}
	static void put_utf8( std::string &o, unsigned c )
	{
		if ( c < 0x80 ) {
			o += (char)c;
		} else if ( c < 0x800 ) {
			o += (char)( 0xC0 | ( c >> 6 ) );
			o += (char)( 0x80 | ( c & 0x3F ) );
		} else if ( c < 0x10000 ) {
			o += (char)( 0xE0 | ( c >> 12 ) );
			o += (char)( 0x80 | ( ( c >> 6 ) & 0x3F ) );
			o += (char)( 0x80 | ( c & 0x3F ) );
		} else {
			o += (char)( 0xF0 | ( c >> 18 ) );
			o += (char)( 0x80 | ( ( c >> 12 ) & 0x3F ) );
			o += (char)( 0x80 | ( ( c >> 6 ) & 0x3F ) );
			o += (char)( 0x80 | ( c & 0x3F ) );
		}
	}
	bool hex4( unsigned *v )
	{
		if ( e - p < 4 ) return false;
		*v = 0;
		for ( int i = 0; i < 4; i++ ) {
			char c = *p++;
			*v <<= 4;
			if ( c >= '0' && c <= '9' ) *v |= (unsigned)( c - '0' );
			else if ( c >= 'a' && c <= 'f' ) *v |= (unsigned)( c - 'a' + 10 );
			else if ( c >= 'A' && c <= 'F' ) *v |= (unsigned)( c - 'A' + 10 );
			else return false;
		}
		return true;
	}
	bool string( std::string &o )
	{
		if ( p >= e || *p != '"' ) return false;
		p++;
		while ( p < e && *p != '"' ) {
			if ( *p != '\\' ) {
				o += *p++;
				continue;
			}
			if ( ++p >= e ) return false;
			char c = *p++;
			switch ( c ) {
			case 'n': o += '\n'; break;
			case 't': o += '\t'; break;
			case 'r': o += '\r'; break;
			case 'b': o += '\b'; break;
			case 'f': o += '\f'; break;
			case 'u': {
				unsigned v, w;
				if ( !hex4(&v) ) return false;
				if ( v >= 0xD800 && v < 0xDC00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u' ) {
					p += 2;
					if ( !hex4(&w) ) return false;
					v = 0x10000 + ( ( v - 0xD800 ) << 10 ) + ( w - 0xDC00 );
				}
				put_utf8(o, v);
				break;
			}
			default: o += c; break;
			}
		}
		if ( p >= e ) return false;
		p++;
		return true;
	}
	bool value( Json &j )
	{
		ws();
		if ( p >= e ) return false;
		if ( *p == '{' ) {
			j.kind = Json::OBJ;
			p++;
			ws();
			if ( p < e && *p == '}' ) {
				p++;
				return true;
			}
			for ( ;; ) {
				std::string k;
				ws();
				if ( !string(k) ) return false;
				ws();
				if ( p >= e || *p != ':' ) return false;
				p++;
				j.members.emplace_back(k, Json());
				if ( !value(j.members.back().second) ) return false;
				ws();
				if ( p < e && *p == ',' ) {
					p++;
					continue;
				}
				if ( p < e && *p == '}' ) {
					p++;
					return true;
				}
				return false;
			}
		}
		if ( *p == '[' ) {
			j.kind = Json::ARR;
			p++;
			ws();
			if ( p < e && *p == ']' ) {
				p++;
				return true;
			}
			for ( ;; ) {
				j.items.emplace_back();
				if ( !value(j.items.back()) ) return false;
				ws();
				if ( p < e && *p == ',' ) {
					p++;
					continue;
				}
				if ( p < e && *p == ']' ) {
					p++;
					return true;
				}
				return false;
			}
		}
		if ( *p == '"' ) {
			j.kind = Json::STR;
			return string(j.text);
		}
		const char *s = p;
		while ( p < e && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n'
		     && *p != '\r' && *p != '\t' ) p++;
		j.text.assign(s, (size_t)( p - s ));
		if ( j.text == "null" ) j.kind = Json::NUL;
		else if ( j.text == "true" || j.text == "false" ) j.kind = Json::BOOL;
		else j.kind = Json::NUM;
		return !j.text.empty();
	}
};

void dump_str( std::string &o, const std::string &s )
{
	o += '"';
	for ( unsigned char c : s ) {
		if ( c == '"' || c == '\\' ) {
			o += '\\';
			o += (char)c;
		} else if ( c == '\n' ) {
			o += "\\n";
		} else if ( c < 0x20 ) {
			char b[8];
			snprintf(b, sizeof(b), "\\u%04x", c);
			o += b;
		} else {
			o += (char)c;
		}
	}
	o += '"';
}

void dump( std::string &o, const Json &j, int depth )
{
	std::string pad((size_t)( depth + 1 ) * 2, ' '), end((size_t)depth * 2, ' ');

	switch ( j.kind ) {
	case Json::NUL:
		o += "null";
		break;
	case Json::BOOL:
	case Json::NUM:
		o += j.text;
		break;
	case Json::STR:
		dump_str(o, j.text);
		break;
	case Json::ARR:
		if ( j.items.empty() ) {
			o += "[]";
			break;
		}
		o += "[\n";
		for ( size_t i = 0; i < j.items.size(); i++ ) {
			o += pad;
			dump(o, j.items[i], depth + 1);
			o += ( i + 1 < j.items.size() ) ? ",\n" : "\n";
		}
		o += end + "]";
		break;
	case Json::OBJ:
		if ( j.members.empty() ) {
			o += "{}";
			break;
		}
		o += "{\n";
		for ( size_t i = 0; i < j.members.size(); i++ ) {
			o += pad;
			dump_str(o, j.members[i].first);
			o += ": ";
			dump(o, j.members[i].second, depth + 1);
			o += ( i + 1 < j.members.size() ) ? ",\n" : "\n";
		}
		o += end + "}";
		break;
	}
}

long num_of( Json *j, long dflt )
{
	return ( j != nullptr && j->kind == Json::NUM ) ? atol(j->text.c_str()) : dflt;
}

/* ---------------------------------------------------------------- the object */

TS_UUID uuid_of( const unsigned char id[16] )
{
	TS_UUID u;
	memcpy(u.b, id, 16);
	return u;
}

bool read_meta( const TS_UUID &u, Json *root )
{
	static char	buf[OB_ATR_MAX + 1];
	SZ		asz = 0;
	ID		k = ob_opn_obj(&u, OB_OP_ATRRD);

	if ( k <= 0 ) return false;
	ER er = ob_get_atr(k, (UB *)buf, OB_ATR_MAX, &asz);
	ob_cls_obj(k);
	if ( er < E_OK || asz <= 0 ) return false;
	Parser ps = { buf, buf + asz };
	*root = Json();
	return ps.value(*root) && root->kind == Json::OBJ;
}

bool write_meta( const TS_UUID &u, const Json &root )
{
	std::string s;

	dump(s, root, 0);
	s += "\n";
	if ( s.size() > OB_ATR_MAX ) return false;
	ID k = ob_opn_obj(&u, OB_OP_ATRRD | OB_OP_ATRWR);
	if ( k <= 0 ) return false;
	ER er = ob_set_atr(k, (const UB *)s.data(), (SZ)s.size());
	ob_cls_obj(k);
	return er >= E_OK;
}

std::string xml( const std::string &s )
{
	std::string o;
	for ( char c : s ) {
		switch ( c ) {
		case '<': o += "&lt;"; break;
		case '>': o += "&gt;"; break;
		case '&': o += "&amp;"; break;
		case '"': o += "&quot;"; break;
		default: o += c; break;
		}
	}
	return o;
}

Json strings( const std::vector<std::string> &v )
{
	Json a;
	a.kind = Json::ARR;
	for ( const auto &s : v ) a.items.push_back(Json::str(s));
	return a;
}

std::vector<std::string> strings_of( Json *a )
{
	std::vector<std::string> v;
	if ( a == nullptr || a->kind != Json::ARR ) return v;
	for ( auto &i : a->items ) if ( i.kind == Json::STR ) v.push_back(i.text);
	return v;
}

}  // namespace

std::string br_uuid_str( const unsigned char id[16] )
{
	static const char hex[] = "0123456789abcdef";
	std::string s;

	for ( int i = 0; i < 16; i++ ) {
		if ( i == 4 || i == 6 || i == 8 || i == 10 ) s += '-';
		s += hex[id[i] >> 4];
		s += hex[id[i] & 0x0F];
	}
	return s;
}

bool br_obj_open( const unsigned char id[16], BrObject *o )
{
	Json	root;
	TS_UUID	u = uuid_of(id);

	memcpy(o->id, id, 16);
	o->ok = false;
	if ( !read_meta(u, &root) ) return false;
	Json *n = root.member("name");
	if ( n != nullptr && n->kind == Json::STR ) o->name = n->text;
	Json *w = root.member("window");
	if ( w != nullptr && w->kind == Json::OBJ ) {
		Json *pos = w->member("pos");
		if ( pos != nullptr && pos->kind == Json::OBJ ) {
			o->x = (int)num_of(pos->member("x"), o->x);
			o->y = (int)num_of(pos->member("y"), o->y);
		}
		o->w = (int)num_of(w->member("width"), o->w);
		o->h = (int)num_of(w->member("height"), o->h);
	}
	Json *b = root.member("webBrowser");
	if ( b != nullptr && b->kind == Json::OBJ ) {
		Json *url = b->member("url");
		if ( url != nullptr && url->kind == Json::STR ) o->url = url->text;
		o->back = strings_of(b->member("back"));
		o->fwd = strings_of(b->member("forward"));
	}
	o->ok = true;
	return true;
}

bool br_obj_save( const BrObject &o, const std::string &title )
{
	Json root;

	TS_UUID u = uuid_of(o.id);
	if ( !o.ok || !read_meta(u, &root) ) return false;
	Json &b = root.set("webBrowser");
	if ( b.kind != Json::OBJ ) b = Json::object();
	b.set("url") = Json::str(o.url);
	b.set("title") = Json::str(title);
	b.set("back") = strings(o.back);
	b.set("forward") = strings(o.fwd);
	if ( !write_meta(u, root) ) return false;

	/* record 0: the page as a document */
	std::string t = "<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"" + xml(o.name) + "\">\n"
			"<document>\n<p>" + xml(title.empty() ? o.url : title) + "</p>\n<p>"
			+ xml(o.url) + "</p>\n</document>\n</tad>\n";
	ID k = ob_opn_obj(&u, OB_OP_READ | OB_OP_WRITE);
	if ( k <= 0 ) return false;
	SZ asz = 0;
	ER er = ob_wri_rec(k, 0, 0, t.data(), (SZ)t.size(), &asz);
	if ( er >= E_OK ) er = ob_trn_rec(k, 0, (UD)t.size());
	ob_cls_obj(k);
	return er >= E_OK;
}

bool br_obj_save_window( const BrObject &o )
{
	Json root;

	TS_UUID u = uuid_of(o.id);
	if ( !o.ok || !read_meta(u, &root) ) return false;
	Json &w = root.set("window");
	if ( w.kind != Json::OBJ ) w = Json::object();
	Json &pos = w.set("pos");
	if ( pos.kind != Json::OBJ ) pos = Json::object();
	pos.set("x") = Json::num(o.x);
	pos.set("y") = Json::num(o.y);
	w.set("width") = Json::num(o.w);
	w.set("height") = Json::num(o.h);
	return write_meta(u, root);
}
