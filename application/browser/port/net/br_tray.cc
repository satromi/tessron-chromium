/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_tray.cc
 *	ブラウザ: the system's tray (include/ts/tray.h) as the pages'
 *	clipboard (design 14.4.5).
 *
 *	What a page copies goes into the tray as a set of its own, as what
 *	is copied in another application does: its text as an xmlTAD
 *	fragment, one paragraph a line, and a picture as PNG. What a page
 *	pastes is taken from the set in hand, whichever application put it
 *	there: the text of its first xmlTAD record (its tags taken out, a
 *	paragraph's end and a line break as a new line, the entities read),
 *	and its first picture.
 */

#include TS_CONFIG_H
#include <stdint.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/uapp.h>
#include <ts/tray.h>

#include <stdlib.h>
#include <string.h>

#include "br_tray.h"

namespace {

constexpr SZ	kReadMax = 16 * 1024 * 1024;	/* the largest record taken out */

void escape( std::string *out, const std::string &s )
{
	for ( char c : s ) {
		switch ( c ) {
		case '<': *out += "&lt;"; break;
		case '>': *out += "&gt;"; break;
		case '&': *out += "&amp;"; break;
		case '"': *out += "&quot;"; break;
		default:
			if ( (unsigned char)c >= 0x20 || c == '\t' ) *out += c;
			break;
		}
	}
}

/* A character's UTF-8 */
void put_utf8( std::string *out, uint32_t u )
{
	if ( u < 0x80 ) {
		*out += (char)u;
	} else if ( u < 0x800 ) {
		*out += (char)( 0xC0 | ( u >> 6 ) );
		*out += (char)( 0x80 | ( u & 0x3F ) );
	} else if ( u < 0x10000 ) {
		*out += (char)( 0xE0 | ( u >> 12 ) );
		*out += (char)( 0x80 | ( ( u >> 6 ) & 0x3F ) );
		*out += (char)( 0x80 | ( u & 0x3F ) );
	} else if ( u < 0x110000 ) {
		*out += (char)( 0xF0 | ( u >> 18 ) );
		*out += (char)( 0x80 | ( ( u >> 12 ) & 0x3F ) );
		*out += (char)( 0x80 | ( ( u >> 6 ) & 0x3F ) );
		*out += (char)( 0x80 | ( u & 0x3F ) );
	}
}

/* The text of an xmlTAD fragment: the tags out, </p> and <br/> a new line, the entities read */
std::string text_of( const char *s, size_t n )
{
	std::string	out;
	bool		in_doc = false, seen_doc = false;

	/* only what is inside <document>, when there is one: not the figure's or the header's words */
	if ( strstr(s, "<document") != nullptr ) seen_doc = true;
	for ( size_t i = 0; i < n; ) {
		if ( s[i] == '<' ) {
			size_t e = i;
			while ( e < n && s[e] != '>' ) e++;
			std::string tag(s + i + 1, e > i + 1 ? e - i - 1 : 0);
			if ( tag.compare(0, 8, "document") == 0 ) in_doc = true;
			else if ( tag.compare(0, 9, "/document") == 0 ) in_doc = false;
			else if ( ( !seen_doc || in_doc ) && ( tag == "/p" || tag.compare(0, 2, "br") == 0 ) ) out += '\n';
			i = ( e < n ) ? e + 1 : n;
			continue;
		}
		if ( seen_doc && !in_doc ) {
			i++;
			continue;
		}
		if ( s[i] == '&' ) {
			size_t e = i + 1;
			while ( e < n && e - i < 12 && s[e] != ';' ) e++;
			if ( e < n && s[e] == ';' ) {
				std::string ent(s + i + 1, e - i - 1);
				uint32_t u = 0;
				if ( ent == "lt" ) u = '<';
				else if ( ent == "gt" ) u = '>';
				else if ( ent == "amp" ) u = '&';
				else if ( ent == "quot" ) u = '"';
				else if ( ent == "apos" ) u = '\'';
				else if ( ent.size() > 1 && ent[0] == '#' ) {
					u = ( ent[1] == 'x' || ent[1] == 'X' ) ? (uint32_t)strtoul(ent.c_str() + 2, nullptr, 16)
									       : (uint32_t)strtoul(ent.c_str() + 1, nullptr, 10);
				}
				if ( u != 0 ) {
					put_utf8(&out, u);
					i = e + 1;
					continue;
				}
			}
		}
		if ( s[i] != '\r' && s[i] != '\n' ) out += s[i];
		i++;
	}
	while ( !out.empty() && out.back() == '\n' ) out.pop_back();
	return out;
}

/* The first record of a kind in the set in hand, whole */
bool first_of( UINT kind, std::vector<uint8_t> *out )
{
	T_TRINF	inf;
	SZ	asz = 0;

	for ( INT i = 0; i < TR_REC_MAX; i++ ) {
		if ( tr_ref_rec(TR_HAND, i, &inf) < E_OK ) break;
		if ( inf.kind != kind || inf.size <= 0 || inf.size > kReadMax ) continue;
		out->resize((size_t)inf.size);
		if ( tr_rea_rec(TR_HAND, i, out->data(), inf.size, &asz) < E_OK ) return false;
		out->resize((size_t)asz);
		return true;
	}
	return false;
}

}  // namespace

bool br_tray_put( const std::string &text, const std::vector<uint8_t> &png, const std::string &name )
{
	T_TRREC		r[2];
	INT		n = 0;
	std::string	frag;

	memset(r, 0, sizeof(r));
	if ( !text.empty() || png.empty() ) {
		frag = "<tad version=\"1.0\" encoding=\"UTF-8\"><document>";
		size_t at = 0;
		for ( ;; ) {
			size_t nl = text.find('\n', at);
			std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
			if ( !line.empty() && line.back() == '\r' ) line.pop_back();
			frag += "<p>";
			escape(&frag, line);
			frag += "</p>";
			if ( nl == std::string::npos ) break;
			at = nl + 1;
		}
		frag += "</document></tad>";
		r[n].kind = TR_TAD;
		r[n].data = frag.data();
		r[n].size = (SZ)frag.size();
		n++;
	}
	if ( !png.empty() ) {
		r[n].kind = TR_PNG;
		r[n].data = png.data();
		r[n].size = (SZ)png.size();
		n++;
	}
	/* the set's name: the start of the text, cut at a whole letter */
	std::string nm = name.empty() ? std::string("ブラウザから") : name;
	if ( nm.size() >= TR_NAME_MAX ) {
		size_t k = TR_NAME_MAX - 1;
		while ( k > 0 && ( (unsigned char)nm[k] & 0xC0 ) == 0x80 ) k--;
		nm.resize(k);
	}
	for ( char &c : nm ) {
		if ( (unsigned char)c < 0x20 ) c = ' ';
	}
	return tr_psh_dat(r, n, (CONST UB *)nm.c_str()) >= E_OK;
}

bool br_tray_text( std::string *text )
{
	std::vector<uint8_t> b;

	if ( !first_of(TR_TAD, &b) ) return false;
	b.push_back(0);
	*text = text_of((const char *)b.data(), b.size() - 1);
	return true;
}

bool br_tray_png( std::vector<uint8_t> *png )
{
	return first_of(TR_PNG, png);
}

uint64_t br_tray_serial()
{
	T_TRSTS	st;

	if ( tr_get_sts(&st) < E_OK || st.hand <= 0 || st.hand > TR_LEVELS ) return 0;
	const T_TRSET &s = st.set[st.hand];
	return ( (uint64_t)s.time * 31 ) ^ ( (uint64_t)st.nlevel << 48 ) ^ ( (uint64_t)st.hand << 56 ) ^ (uint64_t)s.nrec;
}
