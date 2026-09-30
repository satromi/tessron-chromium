/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_text.cc
 *	The text engine of ブラウザ (br.h): the HTML of a page read as
 *	text with its structure -- paragraphs, headings, lists, quotations,
 *	preformatted text, rules, line breaks, images by their alternative
 *	text -- laid out in lines of the window's width and painted with
 *	Skia in the system's face. Scripts and style sheets are not run;
 *	the page's own colours and positions are not taken.
 *
 *	The bytes are turned into UTF-8 from the charset the response or the
 *	page says (lib/libtxc for the Japanese ones), or the one they look
 *	like. Lines break at spaces, and anywhere between Japanese letters.
 */

#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#include <string>
#include <vector>

#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkPaint.h"
#include "include/core/SkTypeface.h"

#include "br.h"

extern "C" {
typedef long	SZ_;
int	txc_detect( const unsigned char *s, SZ_ n );
SZ_	txc_to_utf8( int enc, const unsigned char *s, SZ_ n, unsigned char *out, SZ_ max, int *p_bad );
int	txc_by_name( const unsigned char *name );
int	txc_utf8_get( const unsigned char *s, SZ_ n, int *p_c );
int	txc_utf8_put( int c, unsigned char *out );
}

#define TXC_UTF8	1

namespace {

const int	kMargin = 12;
const SkColor	kText = 0xFF202020;
const SkColor	kLink = 0xFF1A4DC2;
const SkColor	kGrey = 0xFF808080;

struct Style {
	float		size = 16;
	bool		bold = false;
	bool		pre = false;
	int		indent = 0;
	SkColor		color = kText;
	int		link = -1;		/* index into links */
};

/* What the page is, before it is laid out: runs of text and breaks */
struct Item {
	enum Kind { TEXT, BREAK, SPACE, RULE } kind;
	std::string	text;
	Style		st;
	int		gap = 0;		/* BREAK/SPACE: pixels below */
};

struct Frag {
	int		x, y, w, h, base;
	std::string	text;
	float		size;
	bool		bold;
	SkColor		color;
	int		link;
	bool		rule;
};

std::string lower( std::string s )
{
	for ( auto &c : s ) c = (char)tolower((unsigned char)c);
	return s;
}

/* The page's bytes in UTF-8 */
std::string to_utf8( const std::string &body, std::string charset )
{
	int enc = 0;

	if ( charset.empty() ) {
		/* a <meta charset> near the top */
		std::string head = lower(body.substr(0, 4096));
		size_t p = head.find("charset=");
		if ( p != std::string::npos ) {
			p += 8;
			while ( p < head.size() && ( head[p] == '"' || head[p] == '\'' ) ) p++;
			size_t e = head.find_first_of("\"' ;>/", p);
			charset = head.substr(p, e - p);
		}
	}
	charset = lower(charset);
	if ( charset == "utf-8" || charset == "utf8" ) {
		enc = TXC_UTF8;
	} else if ( charset == "shift_jis" || charset == "sjis" || charset == "x-sjis"
		 || charset == "windows-31j" || charset == "cp932" ) {
		enc = txc_by_name((const unsigned char *)"shift_jis");
	} else if ( charset == "euc-jp" || charset == "x-euc-jp" ) {
		enc = txc_by_name((const unsigned char *)"euc-jp");
	} else if ( charset == "iso-2022-jp" ) {
		enc = txc_by_name((const unsigned char *)"iso-2022-jp");
	}
	if ( enc <= 0 ) {
		enc = txc_detect((const unsigned char *)body.data(), (SZ_)body.size());
	}
	if ( enc == TXC_UTF8 || enc <= 0 ) {
		return body;
	}
	std::string out(body.size() * 3 + 16, '\0');
	int bad = 0;
	SZ_ n = txc_to_utf8(enc, (const unsigned char *)body.data(), (SZ_)body.size(),
			    (unsigned char *)&out[0], (SZ_)out.size(), &bad);
	if ( n < 0 ) return body;
	out.resize((size_t)n < out.size() ? (size_t)n : out.size());
	return out;
}

/* &amp; &#12354; &#x3042; and the common names */
std::string entities( const std::string &s )
{
	static const struct { const char *name; int code; } names[] = {
		{ "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' },
		{ "nbsp", 0xA0 }, { "copy", 0xA9 }, { "reg", 0xAE }, { "middot", 0xB7 },
		{ "laquo", 0xAB }, { "raquo", 0xBB }, { "times", 0xD7 }, { "hellip", 0x2026 },
		{ "mdash", 0x2014 }, { "ndash", 0x2013 }, { "rarr", 0x2192 }, { "larr", 0x2190 },
		{ "yen", 0xA5 }, { "bull", 0x2022 }, { "trade", 0x2122 },
	};
	std::string out;

	for ( size_t i = 0; i < s.size(); i++ ) {
		if ( s[i] != '&' ) {
			out += s[i];
			continue;
		}
		size_t e = s.find(';', i);
		if ( e == std::string::npos || e - i > 10 ) {
			out += s[i];
			continue;
		}
		std::string name = s.substr(i + 1, e - i - 1);
		int code = -1;
		if ( !name.empty() && name[0] == '#' ) {
			code = ( name.size() > 1 && ( name[1] == 'x' || name[1] == 'X' ) )
			     ? (int)strtol(name.c_str() + 2, nullptr, 16)
			     : atoi(name.c_str() + 1);
		} else {
			for ( auto &n : names ) {
				if ( name == n.name ) code = n.code;
			}
		}
		if ( code <= 0 || code > 0x10FFFF ) {
			out += s[i];
			continue;
		}
		unsigned char b[8];
		int k = txc_utf8_put(code == 0xA0 ? ' ' : code, b);
		out.append((const char *)b, (size_t)k);
		i = e;
	}
	return out;
}

std::string attr( const std::string &tag, const char *name )
{
	std::string	t = lower(tag), key = std::string(name) + "=";
	size_t		p = 0;

	for ( ;; ) {
		p = t.find(key, p);
		if ( p == std::string::npos ) return "";
		if ( p > 0 && !isspace((unsigned char)t[p - 1]) ) {
			p++;
			continue;
		}
		break;
	}
	p += key.size();
	char q = tag[p];
	if ( q == '"' || q == '\'' ) {
		size_t e = tag.find(q, p + 1);
		return entities(tag.substr(p + 1, e - p - 1));
	}
	size_t e = tag.find_first_of(" \t\r\n>", p);
	return entities(tag.substr(p, e - p));
}

bool is_cjk( int c )
{
	return ( c >= 0x2E80 && c <= 0x9FFF ) || ( c >= 0xF900 && c <= 0xFAFF )
	    || ( c >= 0xFF00 && c <= 0xFFEF ) || ( c >= 0x3000 && c <= 0x303F );
}

class TextEngine : public BrEngine {
public:
	explicit TextEngine( SkTypeface *face ) : face_(sk_ref_sp(face)) {}

	void load( const std::string &url, const std::string &body, const std::string &charset ) override
	{
		url_ = url;
		items_.clear();
		links_.clear();
		title_.clear();
		build(to_utf8(body, charset));
	}

	int layout( int width ) override;
	void paint( SkCanvas *c, int top, int height ) override;

	std::string link_at( int x, int y ) override
	{
		for ( auto &f : frags_ ) {
			if ( f.link >= 0 && x >= f.x && x < f.x + f.w && y >= f.y && y < f.y + f.h ) {
				return links_[(size_t)f.link];
			}
		}
		return "";
	}

	std::string title() override { return title_; }

private:
	void build( const std::string &html );
	void text( const std::string &s, const Style &st );
	void brk( int gap )
	{
		Item it;
		it.kind = Item::BREAK;
		it.gap = gap;
		items_.push_back(it);
	}

	SkFont font( const Style &st ) const
	{
		SkFont f(face_, st.size);
		f.setEdging(SkFont::Edging::kAntiAlias);
		f.setEmbolden(st.bold);
		return f;
	}

	sk_sp<SkTypeface>		face_;
	std::string			url_, title_;
	std::vector<Item>		items_;
	std::vector<std::string>	links_;
	std::vector<Frag>		frags_;
};

void TextEngine::text( const std::string &s, const Style &st )
{
	Item it;

	it.kind = Item::TEXT;
	it.text = s;
	it.st = st;
	items_.push_back(it);
}

void TextEngine::build( const std::string &html )
{
	std::vector<Style>	stack(1);
	std::string		pending;
	bool			in_title = false;
	size_t			i = 0;
	int			list_depth = 0;

	auto flush = [&]() {
		if ( pending.empty() ) return;
		const Style &st = stack.back();
		if ( in_title ) {
			title_ += pending;
		} else if ( st.pre ) {
			size_t a = 0;
			for ( ;; ) {
				size_t nl = pending.find('\n', a);
				text(pending.substr(a, nl - a), st);
				if ( nl == std::string::npos ) break;
				brk(0);
				a = nl + 1;
			}
		} else {
			std::string t;
			bool sp = false;
			for ( char c : pending ) {
				if ( isspace((unsigned char)c) ) {
					sp = true;
				} else {
					if ( sp ) t += ' ';
					sp = false;
					t += c;
				}
			}
			if ( sp ) t += ' ';
			if ( !t.empty() ) text(t, st);
		}
		pending.clear();
	};

	while ( i < html.size() ) {
		if ( html[i] != '<' ) {
			size_t e = html.find('<', i);
			pending += entities(html.substr(i, e - i));
			i = ( e == std::string::npos ) ? html.size() : e;
			continue;
		}
		if ( html.compare(i, 4, "<!--") == 0 ) {
			size_t e = html.find("-->", i + 4);
			i = ( e == std::string::npos ) ? html.size() : e + 3;
			continue;
		}
		size_t e = html.find('>', i);
		if ( e == std::string::npos ) break;
		std::string tag = html.substr(i + 1, e - i - 1);
		i = e + 1;
		bool close = !tag.empty() && tag[0] == '/';
		std::string name = lower(tag.substr(close ? 1 : 0));
		name = name.substr(0, name.find_first_of(" \t\r\n/"));

		if ( !close && ( name == "script" || name == "style" || name == "template"
			      || name == "svg" || name == "iframe" ) ) {
			size_t z = lower(html.substr(i)).find("</" + name);
			i = ( z == std::string::npos ) ? html.size() : i + z;
			continue;
		}
		flush();
		if ( name == "title" ) {
			in_title = !close;
			continue;
		}

		Style st = stack.back();
		bool block = false;
		int gap = 0;
		if ( name == "p" || name == "div" || name == "section" || name == "article"
		  || name == "header" || name == "footer" || name == "nav" || name == "main"
		  || name == "form" || name == "table" || name == "tr" || name == "center"
		  || name == "dl" || name == "dt" || name == "dd" || name == "figure"
		  || name == "aside" || name == "address" || name == "fieldset" ) {
			block = true;
			gap = ( name == "p" ) ? 8 : 0;
		} else if ( name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6' ) {
			block = true;
			gap = 10;
			static const float sz[] = { 30, 24, 20, 18, 16, 14 };
			st.size = sz[name[1] - '1'];
			st.bold = true;
		} else if ( name == "ul" || name == "ol" || name == "blockquote" ) {
			block = true;
			gap = 4;
			st.indent += 24;
			list_depth += close ? -1 : 1;
		} else if ( name == "li" ) {
			block = true;
		} else if ( name == "pre" ) {
			block = true;
			gap = 6;
			st.pre = true;
			st.size = 14;
		} else if ( name == "br" ) {
			brk(0);
			continue;
		} else if ( name == "hr" ) {
			Item it;
			it.kind = Item::RULE;
			it.st = st;
			items_.push_back(it);
			continue;
		} else if ( name == "td" || name == "th" ) {
			if ( !close ) text(" ", st);
			if ( name == "th" ) st.bold = true;
		} else if ( name == "b" || name == "strong" ) {
			st.bold = true;
		} else if ( name == "small" ) {
			st.size = st.size * 0.85f;
		} else if ( name == "a" ) {
			std::string href = attr(tag, "href");
			std::string to = href.empty() ? "" : br_url_resolve(url_, href);
			if ( !close && !to.empty() ) {
				links_.push_back(to);
				st.link = (int)links_.size() - 1;
				st.color = kLink;
			}
		} else if ( name == "img" ) {
			std::string alt = attr(tag, "alt");
			if ( !alt.empty() ) {
				Style s2 = st;
				s2.color = kGrey;
				text("[" + alt + "]", s2);
			}
			continue;
		} else if ( name == "input" || name == "button" || name == "textarea" || name == "select" ) {
			std::string v = attr(tag, "value");
			if ( v.empty() ) v = attr(tag, "placeholder");
			if ( lower(attr(tag, "type")) != "hidden" ) {
				Style s2 = st;
				s2.color = kGrey;
				text("[" + v + "]", s2);
			}
			if ( name != "textarea" && name != "select" && name != "button" ) continue;
		}

		bool self = !tag.empty() && tag.back() == '/';
		if ( block ) brk(gap);
		if ( close ) {
			if ( stack.size() > 1 ) stack.pop_back();
		} else if ( !self && name != "meta" && name != "link" && name != "!doctype" ) {
			stack.push_back(st);
			if ( stack.size() > 256 ) stack.erase(stack.begin() + 1);
		}
		if ( !close && name == "li" ) {
			Style s2 = stack.back();
			text("・", s2);
		}
	}
	flush();
	if ( title_.empty() ) title_ = url_;
}

int TextEngine::layout( int width )
{
	int	x = kMargin, y = kMargin, line_h = 0, line_base = 0;
	size_t	line_start = 0;

	frags_.clear();
	auto newline = [&]( int gap ) {
		/* the fragments of the line put on one baseline */
		for ( size_t k = line_start; k < frags_.size(); k++ ) {
			frags_[k].y = y + line_base - frags_[k].base;
			frags_[k].h = line_h;
		}
		if ( line_h == 0 && frags_.size() == line_start ) line_h = 0;
		y += line_h + gap;
		line_h = 0;
		line_base = 0;
		line_start = frags_.size();
		x = kMargin;
	};

	for ( auto &it : items_ ) {
		if ( it.kind == Item::BREAK ) {
			if ( frags_.size() > line_start || it.gap == 0 ) {
				newline(it.gap);
				if ( line_h == 0 && it.gap == 0 && frags_.size() == line_start ) y += 0;
			}
			continue;
		}
		if ( it.kind == Item::RULE ) {
			newline(4);
			Frag f = { kMargin, y, width - 2 * kMargin, 2, 0, "", 0, false, kGrey, -1, true };
			frags_.push_back(f);
			y += 8;
			line_start = frags_.size();
			continue;
		}
		SkFont		ft = font(it.st);
		SkFontMetrics	m;
		ft.getMetrics(&m);
		int		asc = (int)( -m.fAscent + 0.5f ), h = (int)( m.fDescent - m.fAscent + m.fLeading + 4 );
		int		left = kMargin + it.st.indent;
		const std::string &s = it.text;
		size_t		p = 0;

		if ( x < left ) x = left;
		while ( p < s.size() ) {
			/* the next piece that may not be broken: a word, or one Japanese letter */
			size_t q = p;
			int c = 0, k = txc_utf8_get((const unsigned char *)s.data() + q, (SZ_)( s.size() - q ), &c);
			if ( k <= 0 ) k = 1;
			if ( is_cjk(c) ) {
				q += (size_t)k;
			} else if ( c == ' ' ) {
				q += 1;
			} else {
				while ( q < s.size() ) {
					k = txc_utf8_get((const unsigned char *)s.data() + q, (SZ_)( s.size() - q ), &c);
					if ( k <= 0 ) k = 1;
					if ( c == ' ' || is_cjk(c) ) break;
					q += (size_t)k;
				}
			}
			std::string piece = s.substr(p, q - p);
			int w = (int)( ft.measureText(piece.data(), piece.size(), SkTextEncoding::kUTF8) + 0.5f );
			if ( x + w > width - kMargin && x > left && !it.st.pre ) {
				newline(0);
				x = left;
				if ( piece == " " ) {
					p = q;
					continue;
				}
			}
			Frag f = { x, y, w, h, asc, piece, it.st.size, it.st.bold, it.st.color, it.st.link, false };
			/* joined to the fragment before when it goes on in the same style */
			if ( frags_.size() > line_start ) {
				Frag &b = frags_.back();
				if ( !b.rule && b.link == f.link && b.size == f.size && b.bold == f.bold
				  && b.color == f.color && b.x + b.w == x ) {
					b.text += piece;
					b.w += w;
					x += w;
					if ( h > line_h ) line_h = h;
					if ( asc > line_base ) line_base = asc;
					p = q;
					continue;
				}
			}
			frags_.push_back(f);
			x += w;
			if ( h > line_h ) line_h = h;
			if ( asc > line_base ) line_base = asc;
			p = q;
		}
	}
	newline(0);
	return y + kMargin;
}

void TextEngine::paint( SkCanvas *c, int top, int height )
{
	SkPaint	p;

	p.setAntiAlias(true);
	c->clear(SK_ColorWHITE);
	for ( auto &f : frags_ ) {
		if ( f.y + f.h < top || f.y > top + height ) continue;
		if ( f.rule ) {
			p.setColor(0xFFC0C0C0);
			c->drawRect(SkRect::MakeXYWH((float)f.x, (float)( f.y - top ), (float)f.w, 2), p);
			continue;
		}
		Style st;
		st.size = f.size;
		st.bold = f.bold;
		SkFont ft = font(st);
		p.setColor(f.color);
		float by = (float)( f.y - top + f.base );
		c->drawSimpleText(f.text.data(), f.text.size(), SkTextEncoding::kUTF8,
				  (float)f.x, by, ft, p);
		if ( f.link >= 0 ) {
			c->drawRect(SkRect::MakeXYWH((float)f.x, by + 2, (float)f.w, 1), p);
		}
	}
}

}  // namespace

BrEngine *br_text_engine( SkTypeface *face )
{
	return new TextEngine(face);
}
