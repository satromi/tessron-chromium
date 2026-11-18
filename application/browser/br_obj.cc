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
 *	The address is the text of record 0, a TAD document: its first line
 *	that is an address ("://" in it, "about:" or "www." at its start,
 *	letters of full width read as the narrow ones) is the page opened,
 *	so an object whose text is written by hand, with the basic text
 *	editor or as a URL object is, opens at what it says: it is the
 *	object's home page. Going to other pages leaves it as it is; only
 *	〈ホームページに設定〉 (br_obj_set_home) writes the address of the page
 *	shown there, as the one paragraph, and the page's "title" under
 *	"webBrowser" in the metadata. The window's place is kept under
 *	"window" ("pos" from the top left of the screen it is on, "screen"
 *	when that is not the main screen, "width" and "height" of the work
 *	area), as the desktop's own windows keep it. An object of before, with no address
 *	in its text, is opened at the "url" the metadata kept; the histories
 *	("back", "forward") an object of before kept are not read, and are
 *	taken out when the home page is set.
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
#include <ts/obatr.h>

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
	UB	*buf = NULL;
	ID	k = ob_opn_obj(&u, OB_OP_ATRRD);

	if ( k <= 0 ) return false;
	INT n = ob_atr_read(k, 0, &buf);		/* the metadata whole */
	ob_cls_obj(k);
	if ( n <= 0 ) {
		free(buf);
		return false;
	}
	Parser ps = { (const char *)buf, (const char *)buf + n };
	*root = Json();
	bool ok = ps.value(*root) && root->kind == Json::OBJ;
	free(buf);
	return ok;
}

bool write_meta( const TS_UUID &u, const Json &root )
{
	std::string s;

	dump(s, root, 0);
	s += "\n";
	if ( s.size() > OB_META_LIMIT ) return false;
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


/* One character of U+0000..U+10FFFF as UTF-8 */
void utf8_put( std::string &o, unsigned long c )
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

/* The text of record 0: its tags taken out, a paragraph or a break a new line */
std::string record_text( const TS_UUID &u )
{
	std::string	raw, t;
	char		buf[4096];
	ID		k = ob_opn_obj(&u, OB_OP_READ);

	if ( k <= 0 ) return t;
	for ( D off = 0; raw.size() < 65536; ) {
		SZ asz = 0;
		if ( ob_rea_rec(k, 0, off, buf, (SZ)sizeof(buf), &asz) < E_OK || asz <= 0 ) break;
		raw.append(buf, (size_t)asz);
		off += asz;
		if ( asz < (SZ)sizeof(buf) ) break;
	}
	ob_cls_obj(k);
	for ( size_t i = 0; i < raw.size(); ) {
		if ( raw[i] == '<' ) {
			size_t e = raw.find('>', i);
			if ( e == std::string::npos ) break;
			std::string tag = raw.substr(i + 1, e - i - 1);
			if ( tag == "/p" || tag.compare(0, 2, "br") == 0 || tag == "p" ) t += '\n';
			i = e + 1;
		} else if ( raw[i] == '&' ) {
			size_t e = raw.find(';', i);
			std::string ent = ( e == std::string::npos ) ? "" : raw.substr(i + 1, e - i - 1);
			if ( ent == "amp" ) t += '&';
			else if ( ent == "lt" ) t += '<';
			else if ( ent == "gt" ) t += '>';
			else if ( ent == "quot" ) t += '"';
			else if ( ent == "apos" ) t += '\'';
			else if ( ent.size() > 1 && ent[0] == '#' ) {
				unsigned long c = ( ent[1] == 'x' || ent[1] == 'X' ) ? strtoul(ent.c_str() + 2, nullptr, 16)
										  : strtoul(ent.c_str() + 1, nullptr, 10);
				if ( c > 0 && c <= 0x10FFFF ) utf8_put(t, c);
			} else {
				t += '&';
				i++;
				continue;
			}
			i = e + 1;
		} else {
			t += raw[i++];
		}
	}
	return t;
}

/* A line with full-width letters read as the narrow ones, its spaces at either end taken off */
std::string narrow_trim( const std::string &l )
{
	std::string o;

	for ( size_t i = 0; i < l.size(); ) {
		unsigned char a = (unsigned char)l[i];
		if ( a == 0xEF && i + 2 < l.size() ) {
			unsigned long c = ( ( a & 0x0FUL ) << 12 ) | ( ( (unsigned char)l[i + 1] & 0x3FUL ) << 6 )
					| ( (unsigned char)l[i + 2] & 0x3FUL );
			if ( c >= 0xFF01 && c <= 0xFF5E ) {
				o += (char)( c - 0xFEE0 );
				i += 3;
				continue;
			}
		}
		if ( a == 0xE3 && i + 2 < l.size() && (unsigned char)l[i + 1] == 0x80
		  && (unsigned char)l[i + 2] == 0x80 ) {
			o += ' ';			/* the ideographic space */
			i += 3;
			continue;
		}
		o += l[i++];
	}
	size_t b = o.find_first_not_of(" \t\r\n");
	size_t e = o.find_last_not_of(" \t\r\n");
	return ( b == std::string::npos ) ? std::string() : o.substr(b, e - b + 1);
}

/*
 * The first line of the object's text that is an address, or "", and in
 * 'trace' what a line "trace=<what>" asks to be traced (as the start-up
 * line's, for the user to look at a site on the console; no other line
 * of the start-up is taken from an object)
 */
std::string url_in_text( const TS_UUID &u, std::string *trace )
{
	std::string t = record_text(u);

	for ( size_t i = 0; i <= t.size(); ) {
		size_t e = t.find('\n', i);
		if ( e == std::string::npos ) e = t.size();
		std::string l = narrow_trim(t.substr(i, e - i));
		if ( l.compare(0, 6, "trace=") == 0 && l.size() > 6 ) {
			if ( !trace->empty() ) *trace += ",";
			*trace += l.substr(6);
		}
		i = e + 1;
	}
	for ( size_t i = 0; i <= t.size(); ) {
		size_t e = t.find('\n', i);
		if ( e == std::string::npos ) e = t.size();
		std::string l = narrow_trim(t.substr(i, e - i));
		if ( l.find("://") != std::string::npos || l.compare(0, 6, "about:") == 0 ) return l;
		if ( l.compare(0, 4, "www.") == 0 ) return "https://" + l;
		i = e + 1;
	}
	return std::string();
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
			o->kept = true;
		}
		Json *scr = w->member("screen");
		if ( scr != nullptr && scr->kind == Json::STR ) o->screen = scr->text;
		o->w = (int)num_of(w->member("width"), o->w);
		o->h = (int)num_of(w->member("height"), o->h);
	}
	Json *b = root.member("webBrowser");
	if ( b != nullptr && b->kind == Json::OBJ ) {
		Json *url = b->member("url");
		if ( url != nullptr && url->kind == Json::STR ) o->url = url->text;
	}
	o->trace.clear();
	std::string t = url_in_text(u, &o->trace);
	if ( !t.empty() ) o->url = t;
	o->ok = true;
	return true;
}

bool br_obj_set_home( const BrObject &o, const std::string &title )
{
	Json root;

	TS_UUID u = uuid_of(o.id);
	if ( !o.ok || !read_meta(u, &root) ) return false;
	Json &b = root.set("webBrowser");
	if ( b.kind != Json::OBJ ) b = Json::object();
	/* the address is the text's, and the histories of before are not kept */
	for ( auto m = b.members.begin(); m != b.members.end(); ) {
		if ( m->first == "url" || m->first == "back" || m->first == "forward" ) {
			m = b.members.erase(m);
		} else {
			++m;
		}
	}
	b.set("title") = Json::str(title);
	if ( !write_meta(u, root) ) return false;

	/* record 0: the address, as the one paragraph of a document */
	std::string t = "<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"" + xml(o.name) + "\">\n"
			"<document>\n<p>" + xml(o.url) + "</p>\n</document>\n</tad>\n";
	ID k = ob_opn_obj(&u, OB_OP_READ | OB_OP_WRITE);
	if ( k <= 0 ) return false;
	/* replaced in one change: a reader sees the old record or the new one */
	ER er = ob_rpl_rec(k, 0, t.data(), (SZ)t.size());
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
	/* the main screen's place names none: one named before is made empty */
	if ( !o.screen.empty() || w.member("screen") != nullptr ) w.set("screen") = Json::str(o.screen);
	return write_meta(u, root);
}
