/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_http.cc
 *	Addresses and HTTP/1.1 over the sockets of the system (lib/libcxxrt,
 *	include/tsposix/sys/socket.h).
 *
 *	One connection a request (Connection: close); the body comes whole,
 *	by Content-Length, chunked, or to the end of the connection, and is
 *	asked for without compression. Up to eight redirects are followed.
 *	An address of https is read through br_tls_* when a TLS layer is
 *	linked (weak symbols); without one it is refused with a message.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netdb.h>

#include <algorithm>
#include <string>
#include <vector>

#include "br.h"

/* A TLS layer, when one is linked: a session over a connected socket */
extern "C" {
void	*br_tls_open( int fd, const char *host ) __attribute__((weak));
int	br_tls_write( void *s, const void *buf, int n ) __attribute__((weak));
int	br_tls_read( void *s, void *buf, int n ) __attribute__((weak));
void	br_tls_close( void *s ) __attribute__((weak));
const char *br_tls_error( void ) __attribute__((weak));
}

/*
 * The network stack, when the engine brings it (port/net): then every
 * request goes through it, and its jar keeps the cookies
 */
extern "C" {
int	br_net_thread_up( void ) __attribute__((weak));
int	br_net_fetch_sync( const char *url, const char *method, const void *body,
			   unsigned body_len, const char *req_headers, char **buf,
			   unsigned *len, char *ctype, unsigned ctype_cap, int *status,
			   char **headers, char *final_url, unsigned url_cap,
			   int timeout_ms ) __attribute__((weak));
int	br_net_cookie_line( const char *url, int for_script, char *out, int cap ) __attribute__((weak));
void	br_net_cookie_set( const char *url, const char *line, int from_script ) __attribute__((weak));
}

static bool net_stack()
{
	return br_net_thread_up != nullptr && br_net_thread_up() != 0;
}

#define MAX_REDIRECTS	8
#define MAX_BODY	( 64L * 1024 * 1024 )
#define RECV_TMO_S	20

static std::string lower( std::string s )
{
	for ( auto &c : s ) c = (char)tolower((unsigned char)c);
	return s;
}

/* ---------------------------------------------------------------- addresses */

bool br_url_parse( const std::string &s, BrUrl *u )
{
	size_t	p = s.find("://"), q;

	*u = BrUrl();
	if ( s.compare(0, 6, "about:") == 0 ) {
		u->scheme = "about";
		u->path = s.substr(6);
		return true;
	}
	if ( p == std::string::npos ) return false;
	u->scheme = lower(s.substr(0, p));
	if ( u->scheme != "http" && u->scheme != "https" ) return false;
	p += 3;
	q = s.find_first_of("/?#", p);
	std::string hp = s.substr(p, ( q == std::string::npos ) ? std::string::npos : q - p);
	size_t at = hp.rfind('@');
	if ( at != std::string::npos ) hp = hp.substr(at + 1);
	size_t colon = hp.rfind(':');
	if ( colon != std::string::npos && hp.find(']') == std::string::npos ) {
		u->port = atoi(hp.c_str() + colon + 1);
		hp = hp.substr(0, colon);
	}
	u->host = lower(hp);
	if ( u->host.empty() ) return false;
	if ( u->port == 0 ) u->port = ( u->scheme == "https" ) ? 443 : 80;
	std::string rest = ( q == std::string::npos ) ? "/" : s.substr(q);
	size_t hash = rest.find('#');
	if ( hash != std::string::npos ) {
		u->fragment = rest.substr(hash + 1);
		rest = rest.substr(0, hash);
	}
	if ( rest.empty() || rest[0] != '/' ) rest = "/" + rest;
	u->path = rest;
	return true;
}

std::string br_url_string( const BrUrl &u )
{
	if ( u.scheme == "about" ) return "about:" + u.path;
	std::string s = u.scheme + "://" + u.host;
	if ( !( ( u.scheme == "http" && u.port == 80 ) || ( u.scheme == "https" && u.port == 443 ) ) ) {
		s += ":" + std::to_string(u.port);
	}
	s += u.path;
	if ( !u.fragment.empty() ) s += "#" + u.fragment;
	return s;
}

/* "a/./b/../c" made plain */
static std::string clean_path( const std::string &path )
{
	std::string	query;
	std::string	p = path;
	size_t		qm = p.find('?');

	if ( qm != std::string::npos ) {
		query = p.substr(qm);
		p = p.substr(0, qm);
	}
	std::vector<std::string> parts;
	size_t i = 0;
	bool trailing = !p.empty() && p.back() == '/';
	while ( i < p.size() ) {
		size_t j = p.find('/', i);
		std::string seg = p.substr(i, ( j == std::string::npos ) ? std::string::npos : j - i);
		if ( seg == ".." ) {
			if ( !parts.empty() ) parts.pop_back();
		} else if ( !seg.empty() && seg != "." ) {
			parts.push_back(seg);
		}
		if ( j == std::string::npos ) break;
		i = j + 1;
	}
	std::string out;
	for ( auto &s : parts ) out += "/" + s;
	if ( out.empty() || trailing ) out += "/";
	return out + query;
}

std::string br_url_resolve( const std::string &base, const std::string &ref )
{
	BrUrl	b, r;

	if ( ref.empty() ) return base;
	if ( br_url_parse(ref, &r) ) return br_url_string(r);
	if ( ref.compare(0, 11, "javascript:") == 0 || ref.compare(0, 7, "mailto:") == 0 ) return "";
	if ( !br_url_parse(base, &b) || b.scheme == "about" ) return "";
	if ( ref.compare(0, 2, "//") == 0 ) {
		return br_url_resolve(base, b.scheme + ":" + ref);
	}
	b.fragment.clear();
	if ( ref[0] == '#' ) {
		b.fragment = ref.substr(1);
		return br_url_string(b);
	}
	if ( ref[0] == '/' ) {
		b.path = ref;
	} else if ( ref[0] == '?' ) {
		size_t qm = b.path.find('?');
		b.path = b.path.substr(0, qm) + ref;
	} else {
		std::string dir = b.path.substr(0, b.path.find('?'));
		dir = dir.substr(0, dir.rfind('/') + 1);
		b.path = dir + ref;
	}
	size_t hash = b.path.find('#');
	if ( hash != std::string::npos ) {
		b.fragment = b.path.substr(hash + 1);
		b.path = b.path.substr(0, hash);
	}
	b.path = clean_path(b.path);
	return br_url_string(b);
}

/* ---------------------------------------------------------------- cookies */

/*
 * The cookies of the process: what the servers set (Set-Cookie) and what
 * the pages' scripts set (document.cookie), sent back with every request
 * whose host and path they match. They last as long as the process.
 */
struct Cookie {
	std::string	name, value, domain, path;
	bool		host_only = true;
	bool		secure = false;
	bool		http_only = false;
};

static std::vector<Cookie>	g_cookies;

static bool domain_match( const std::string &host, const Cookie &c )
{
	if ( c.host_only || host == c.domain ) return host == c.domain;
	return host.size() > c.domain.size()
	       && host.compare(host.size() - c.domain.size(), c.domain.size(), c.domain) == 0
	       && host[host.size() - c.domain.size() - 1] == '.';
}

static bool path_match( const std::string &path, const std::string &cp )
{
	if ( path.compare(0, cp.size(), cp) != 0 ) return false;
	return path.size() == cp.size() || cp.back() == '/' || path[cp.size()] == '/'
	       || path[cp.size()] == '?';
}

static std::string trim( const std::string &s )
{
	size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
	return ( a == std::string::npos ) ? std::string() : s.substr(a, b - a + 1);
}

/* A cookie line (Set-Cookie's value, or what a script sets) for the address */
void br_cookie_set( const std::string &url, const std::string &line, bool from_script )
{
	BrUrl	u;
	Cookie	c;
	bool	gone = false;

	if ( net_stack() ) {
		br_net_cookie_set(url.c_str(), line.c_str(), from_script ? 1 : 0);
		return;
	}
	if ( !br_url_parse(url, &u) ) return;
	size_t semi = line.find(';');
	std::string nv = line.substr(0, semi), attrs = ( semi == std::string::npos ) ? "" : line.substr(semi + 1);
	size_t eq = nv.find('=');
	if ( eq == std::string::npos ) return;
	c.name = trim(nv.substr(0, eq));
	c.value = trim(nv.substr(eq + 1));
	c.domain = lower(u.host);
	std::string dir = u.path.substr(0, u.path.find('?'));
	c.path = ( dir.rfind('/') == 0 || dir.rfind('/') == std::string::npos ) ? "/" : dir.substr(0, dir.rfind('/'));
	while ( !attrs.empty() ) {
		size_t e = attrs.find(';');
		std::string a = trim(attrs.substr(0, e));
		attrs = ( e == std::string::npos ) ? "" : attrs.substr(e + 1);
		size_t q = a.find('=');
		std::string k = lower(trim(a.substr(0, q))), v = ( q == std::string::npos ) ? "" : trim(a.substr(q + 1));
		if ( k == "domain" && !v.empty() ) {
			if ( v[0] == '.' ) v.erase(0, 1);
			v = lower(v);
			Cookie t;
			t.domain = v;
			t.host_only = false;
			if ( !domain_match(c.domain, t) ) return;	/* a host it may not set */
			c.domain = v;
			c.host_only = false;
		} else if ( k == "path" && !v.empty() && v[0] == '/' ) {
			c.path = v;
		} else if ( k == "secure" ) {
			c.secure = true;
		} else if ( k == "httponly" ) {
			c.http_only = true;
		} else if ( k == "max-age" ) {
			gone = atol(v.c_str()) <= 0;
		} else if ( k == "expires" ) {
			/* a date before this year's takes it away */
			size_t y = v.find_first_of("0123456789", v.find_first_of(" ,") + 1);
			for ( size_t p = y; p != std::string::npos && p < v.size(); ) {
				size_t n = v.find_first_not_of("0123456789", p);
				std::string d = v.substr(p, n == std::string::npos ? std::string::npos : n - p);
				if ( d.size() == 4 && atoi(d.c_str()) < 2026 ) gone = true;
				p = ( n == std::string::npos ) ? n : v.find_first_of("0123456789", n);
			}
		}
	}
	if ( from_script && c.http_only ) return;
	for ( size_t i = 0; i < g_cookies.size(); i++ ) {
		Cookie &o = g_cookies[i];
		if ( o.name == c.name && o.domain == c.domain && o.path == c.path ) {
			if ( from_script && o.http_only ) return;
			g_cookies.erase(g_cookies.begin() + (long)i);
			break;
		}
	}
	if ( !gone ) g_cookies.push_back(c);
}

/* The cookies for the address as a Cookie header's value */
std::string br_cookie_get( const std::string &url, bool for_script )
{
	BrUrl		u;
	std::string	out;

	if ( net_stack() ) {
		std::vector<char> line(16384);
		int n = br_net_cookie_line(url.c_str(), for_script ? 1 : 0, line.data(), (int)line.size());
		return std::string(line.data(), n > 0 ? (size_t)n : 0);
	}
	if ( !br_url_parse(url, &u) ) return out;
	std::string host = lower(u.host);
	for ( const Cookie &c : g_cookies ) {
		if ( !domain_match(host, c) || !path_match(u.path, c.path) ) continue;
		if ( c.secure && u.scheme != "https" ) continue;
		if ( for_script && c.http_only ) continue;
		if ( !out.empty() ) out += "; ";
		out += c.name + "=" + c.value;
	}
	return out;
}

/* ---------------------------------------------------------------- one exchange */

struct Conn {
	int	fd = -1;
	void	*tls = nullptr;

	int put( const void *p, int n )
	{
		if ( tls != nullptr ) return br_tls_write(tls, p, n);
		return (int)send(fd, p, (size_t)n, 0);
	}
	int get( void *p, int n )
	{
		if ( tls != nullptr ) return br_tls_read(tls, p, n);
		return (int)recv(fd, p, (size_t)n, 0);
	}
	~Conn()
	{
		if ( tls != nullptr ) br_tls_close(tls);
		if ( fd >= 0 ) close(fd);
	}
};

static bool dial( const BrUrl &u, Conn *c, std::string *err )
{
	struct addrinfo	hints, *res = nullptr;
	char		port[8];

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	snprintf(port, sizeof(port), "%d", u.port);
	if ( getaddrinfo(u.host.c_str(), port, &hints, &res) != 0 || res == nullptr ) {
		*err = "名前が引けません: " + u.host;
		return false;
	}
	c->fd = socket(AF_INET, SOCK_STREAM, 0);
	if ( c->fd < 0 ) {
		freeaddrinfo(res);
		*err = "ソケットが作れません";
		return false;
	}
	struct timeval tv = { RECV_TMO_S, 0 };
	(void)setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	if ( connect(c->fd, res->ai_addr, res->ai_addrlen) < 0 ) {
		freeaddrinfo(res);
		*err = "繋がりません: " + u.host + " (" + strerror(errno) + ")";
		return false;
	}
	freeaddrinfo(res);
	if ( u.scheme == "https" ) {
		if ( br_tls_open == nullptr ) {
			*err = "この版は HTTPS を読めません";
			return false;
		}
		c->tls = br_tls_open(c->fd, u.host.c_str());
		if ( c->tls == nullptr ) {
			*err = "TLS で繋がりません: " + u.host;
			if ( br_tls_error != nullptr && br_tls_error()[0] != '\0' ) {
				*err += std::string(" (") + br_tls_error() + ")";
			}
			return false;
		}
	}
	return true;
}

static std::string header_value( const std::string &head, const char *name )
{
	std::string	h = lower(head), key = std::string("\r\n") + lower(name) + ":";
	size_t		p = h.find(key);

	if ( p == std::string::npos ) return "";
	p += key.size();
	size_t e = head.find("\r\n", p);
	std::string v = head.substr(p, ( e == std::string::npos ) ? std::string::npos : e - p);
	while ( !v.empty() && ( v[0] == ' ' || v[0] == '\t' ) ) v.erase(0, 1);
	while ( !v.empty() && ( v.back() == ' ' || v.back() == '\t' ) ) v.pop_back();
	return v;
}

/* The chunked body made whole; false when it is not well formed */
static bool unchunk( const std::string &in, std::string *out )
{
	size_t p = 0;

	out->clear();
	while ( p < in.size() ) {
		size_t e = in.find("\r\n", p);
		if ( e == std::string::npos ) return false;
		long n = strtol(in.c_str() + p, nullptr, 16);
		p = e + 2;
		if ( n <= 0 ) return true;
		if ( p + (size_t)n > in.size() ) {
			out->append(in, p, std::string::npos);
			return false;
		}
		out->append(in, p, (size_t)n);
		p += (size_t)n + 2;
	}
	return true;
}

static bool exchange( const BrUrl &u, const std::string &method, const std::string &ctype,
		      const std::string &body, BrResponse *r, std::string *location,
		      void (*progress)( void *, long, long ), void *ctx )
{
	Conn		c;
	std::string	req, data;
	char		buf[16384];
	long		total = -1;
	size_t		head_end = std::string::npos;

	if ( !dial(u, &c, &r->error) ) return false;
	std::string cookies = br_cookie_get(br_url_string(u), false);
	req = method + " " + u.path + " HTTP/1.1\r\n"
	      "Host: " + u.host + "\r\n"
	      + ( cookies.empty() ? std::string() : "Cookie: " + cookies + "\r\n" ) +
	      "User-Agent: Mozilla/5.0 (TessronOS; aarch64) TessronOSBrowser/0.1\r\n"
	      "Accept: text/html,application/xhtml+xml,text/plain;q=0.9,*/*;q=0.8\r\n"
	      "Accept-Language: ja,en;q=0.8\r\n"
	      "Accept-Encoding: identity\r\n"
	      "Connection: close\r\n";
	if ( !body.empty() || method == "POST" ) {
		req += "Content-Type: " + ( ctype.empty() ? std::string("application/x-www-form-urlencoded") : ctype )
		       + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
	}
	req += "\r\n" + body;
	if ( c.put(req.data(), (int)req.size()) != (int)req.size() ) {
		r->error = "送れません";
		return false;
	}
	for ( ;; ) {
		int n = c.get(buf, sizeof(buf));
		if ( n <= 0 ) break;
		data.append(buf, (size_t)n);
		if ( head_end == std::string::npos ) {
			head_end = data.find("\r\n\r\n");
			if ( head_end != std::string::npos ) {
				std::string cl = header_value(data.substr(0, head_end + 2), "Content-Length");
				if ( !cl.empty() ) total = atol(cl.c_str());
			}
		}
		long got = ( head_end == std::string::npos ) ? 0 : (long)( data.size() - head_end - 4 );
		if ( progress != nullptr ) progress(ctx, got, total);
		if ( total >= 0 && got >= total ) break;
		if ( (long)data.size() > MAX_BODY ) break;
	}
	if ( head_end == std::string::npos ) {
		r->error = data.empty() ? "応答がありません" : "応答が読めません";
		return false;
	}
	std::string head = data.substr(0, head_end + 2);
	std::string rbody = data.substr(head_end + 4);
	if ( head.compare(0, 5, "HTTP/") != 0 ) {
		r->error = "HTTP の応答ではありません";
		return false;
	}
	r->status = atoi(head.c_str() + head.find(' ') + 1);
	{
		/* every Set-Cookie of the answer, a redirect's too */
		std::string lh = lower(head);
		for ( size_t p = lh.find("\r\nset-cookie:"); p != std::string::npos;
		      p = lh.find("\r\nset-cookie:", p + 2) ) {
			size_t v = p + 13, e = head.find("\r\n", v);
			br_cookie_set(br_url_string(u), head.substr(v, e == std::string::npos ? e : e - v), false);
		}
	}
	r->type = header_value(head, "Content-Type");
	std::string lt = lower(r->type);
	size_t cs = lt.find("charset=");
	if ( cs != std::string::npos ) {
		std::string v = r->type.substr(cs + 8);
		v.erase(std::remove(v.begin(), v.end(), '"'), v.end());
		r->charset = lower(v.substr(0, v.find_first_of("; ")));
	}
	if ( lower(header_value(head, "Transfer-Encoding")).find("chunked") != std::string::npos ) {
		(void)unchunk(rbody, &r->body);
	} else {
		r->body = rbody;
	}
	*location = header_value(head, "Location");
	return true;
}

/* The fetch through the network stack, waited for */
static bool fetch_net( const std::string &url, const std::string &method, const std::string &ctype,
		       const std::string &body, BrResponse *r )
{
	std::string	hdrs = ctype.empty() ? std::string() : "Content-Type: " + ctype + "\r\n";
	char		*buf = nullptr, *resp = nullptr;
	unsigned	len = 0;
	char		type[256] = "", final_url[4096] = "";
	int		status = 0;

	*r = BrResponse();
	r->url = url;
	int ok = br_net_fetch_sync(url.c_str(), method.c_str(), body.data(), (unsigned)body.size(),
				   hdrs.c_str(), &buf, &len, type, sizeof(type), &status, &resp,
				   final_url, sizeof(final_url), RECV_TMO_S * 3 * 1000);
	free(resp);
	if ( ok <= 0 ) {
		free(buf);
		r->error = "読めません: " + url;
		return false;
	}
	r->status = status;
	if ( final_url[0] != 0 ) r->url = final_url;
	r->type = type;
	std::string lt = lower(r->type);
	size_t cs = lt.find("charset=");
	if ( cs != std::string::npos ) {
		std::string v = r->type.substr(cs + 8);
		v.erase(std::remove(v.begin(), v.end(), '"'), v.end());
		r->charset = lower(v.substr(0, v.find_first_of("; ")));
	}
	r->body.assign(buf != nullptr ? buf : "", len);
	free(buf);
	return true;
}

bool br_fetch( const std::string &url, BrResponse *r,
	       void (*progress)( void *, long, long ), void *ctx,
	       const std::string &method_in, const std::string &ctype_in, const std::string &body_in )
{
	std::string	cur = url;
	std::string	method = method_in.empty() ? "GET" : method_in;
	std::string	ctype = ctype_in, body = body_in;

	if ( net_stack() && ( url.compare(0, 5, "http:") == 0 || url.compare(0, 6, "https:") == 0 ) ) {
		return fetch_net(url, method, ctype, body, r);
	}
	for ( int i = 0; i <= MAX_REDIRECTS; i++ ) {
		BrUrl		u;
		std::string	loc;

		*r = BrResponse();
		r->url = cur;
		if ( !br_url_parse(cur, &u) ) {
			r->error = "アドレスが読めません: " + cur;
			return false;
		}
		if ( !exchange(u, method, ctype, body, r, &loc, progress, ctx) ) return false;
		if ( r->status >= 300 && r->status < 400 && !loc.empty() ) {
			cur = br_url_resolve(cur, loc);
			/* a form sent again only for 307 and 308; else what it leads to is asked for */
			if ( r->status != 307 && r->status != 308 ) {
				method = "GET";
				ctype.clear();
				body.clear();
			}
			continue;
		}
		return true;
	}
	r->error = "転送が多すぎます";
	return false;
}
