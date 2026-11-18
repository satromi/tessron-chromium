/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_ext.cc
 *	ブラウザ: Manifest V3 extensions (design 14.12), without the layers a
 *	browser with a browser process has: one object per extension holding
 *	its ZIP or CRX, read wholly into memory when the browser starts.
 *
 *	- Its files are served as chrome-extension://<id>/<path>; a page gets
 *	  only what web_accessible_resources names.
 *	- Its content scripts run in an isolated world of their own in the
 *	  frames they match, at the three moments the manifest can name.
 *	- Its background service worker is a real service worker of its
 *	  origin, whose script is served with the chrome object in front.
 *	- chrome.* is JavaScript over one call: in a content script a function
 *	  of the world's that page scripts cannot reach, in the worker a fetch
 *	  of its own origin's /__brext/<op>, which only that origin may ask.
 *	- declarativeNetRequest rules are consulted by the network stack for
 *	  every request a page, a frame, a worker or a WebSocket makes (the
 *	  delegate in net_fetch_tessronos.cc), so that a redirect is a redirect
 *	  to whoever asked and a header is changed where it is sent and received.
 *	- What an extension declared is what it may do: a call is refused here
 *	  whatever the JavaScript above it was made to do.
 */

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/containers/span.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/time/time.h"
#include "base/values.h"
#include "third_party/boringssl/src/include/openssl/sha.h"
#include "third_party/re2/src/re2/re2.h"
#include "third_party/zlib/zlib.h"
#include "url/gurl.h"
#include "url/origin.h"

#include "br_ext.h"
#include "br_ext_shim.inc"

void br_ext_command( const std::string &code );		/* br_main.cc: done on the engine's thread: a page opened, the tab sent somewhere, a panel */

struct BrExtWorker {
	std::string	scope, script;
	bool		module;
};

namespace {

/* ---------------------------------------------------------------- declarativeNetRequest */

struct Rule {
	int				id = 0, priority = 1;
	int				action = 0;		/* A_* */
	std::string			redirect;		/* an address */
	std::string			substitution;		/* redirect.regexSubstitution */
	std::shared_ptr<base::DictValue> transform;		/* redirect.transform */
	std::vector<BrExtHeader>	req_headers, resp_headers;	/* modifyHeaders */
	std::string			filter;			/* urlFilter, lower case unless case sensitive */
	bool				left_host = false, left = false, right = false, sensitive = false;
	std::shared_ptr<re2::RE2>	regex;
	std::vector<std::string>	req_dom, no_req_dom, ini_dom, no_ini_dom;
	std::set<std::string>		types, no_types, methods, no_methods;
	int				party = 0;		/* 1 first, 2 third */
};
enum { A_BLOCK = 1, A_ALLOW, A_REDIRECT, A_UPGRADE, A_ALLOW_ALL, A_MODIFY };

bool glob_at( const char *f, const char *fe, const char *s, const char *se, bool anchored_end );

/* '^' stands for what ends a host or a path's part, or the address's end */
bool sep( char c )
{
	return !( ( c >= '0' && c <= '9' ) || ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' )
		  || c == '_' || c == '-' || c == '.' || c == '%' );
}

bool glob_at( const char *f, const char *fe, const char *s, const char *se, bool end )
{
	for ( ; f < fe; f++ ) {
		if ( *f == '*' ) {
			while ( f + 1 < fe && f[1] == '*' ) f++;
			if ( f + 1 == fe ) return true;
			for ( const char *t = s; t <= se; t++ ) {
				if ( glob_at(f + 1, fe, t, se, end) ) return true;
			}
			return false;
		}
		if ( *f == '^' ) {
			if ( s == se ) {
				/* the end of the address counts as one separator */
				return f + 1 == fe || glob_at(f + 1, fe, s, se, end);
			}
			if ( !sep(*s) ) return false;
			s++;
			continue;
		}
		if ( s == se || *s != *f ) return false;
		s++;
	}
	return !end || s == se;
}

std::string lower( std::string s )
{
	for ( char &c : s ) if ( c >= 'A' && c <= 'Z' ) c = (char)( c - 'A' + 'a' );
	return s;
}

bool dom_in( const std::string &host, const std::vector<std::string> &doms )
{
	for ( const std::string &d : doms ) {
		if ( host == d ) return true;
		if ( host.size() > d.size() && host.compare(host.size() - d.size(), d.size(), d) == 0
		     && host[host.size() - d.size() - 1] == '.' ) return true;
	}
	return false;
}

/* The last two labels, near enough a registrable domain to tell first party from third */
std::string site_of( const std::string &host )
{
	size_t b = host.rfind('.');
	if ( b == std::string::npos || b == 0 ) return host;
	b = host.rfind('.', b - 1);
	return b == std::string::npos ? host : host.substr(b + 1);
}

bool url_filter_hits( const Rule &r, const std::string &url, const std::string &host )
{
	if ( r.regex ) return re2::RE2::PartialMatch(url, *r.regex);
	if ( r.filter.empty() ) return true;
	std::string lc;
	const std::string &u = r.sensitive ? url : ( lc = lower(url) );
	const char *f = r.filter.data(), *fe = f + r.filter.size(), *s = u.data(), *se = s + u.size();
	if ( r.left_host ) {
		/* at the host's start or after any of its dots */
		size_t at = u.find(host);
		if ( at == std::string::npos ) return false;
		for ( size_t i = at; i < at + host.size(); i++ ) {
			if ( ( i == at || u[i - 1] == '.' ) && glob_at(f, fe, s + i, se, r.right) ) return true;
		}
		return false;
	}
	if ( r.left ) return glob_at(f, fe, s, se, r.right);
	for ( const char *t = s; t <= se; t++ ) {
		if ( glob_at(f, fe, t, se, r.right) ) return true;
	}
	return false;
}

bool rule_hits( const Rule &r, const BrExtReq &q, const std::string &host, const std::string &ini )
{
	if ( !r.types.empty() && !r.types.count(q.type) ) return false;
	if ( r.types.empty() && q.type == "main_frame" ) return false;	/* as the API says of a rule with no types */
	if ( r.no_types.count(q.type) ) return false;
	std::string method = lower(q.method);		/* the API's are in lower case, HTTP's in upper */
	if ( !r.methods.empty() && !r.methods.count(method) ) return false;
	if ( r.no_methods.count(method) ) return false;
	if ( !r.req_dom.empty() && !dom_in(host, r.req_dom) ) return false;
	if ( dom_in(host, r.no_req_dom) ) return false;
	if ( !r.ini_dom.empty() && !dom_in(ini, r.ini_dom) ) return false;
	if ( !ini.empty() && dom_in(ini, r.no_ini_dom) ) return false;
	if ( r.party != 0 ) {
		bool third = !ini.empty() && site_of(ini) != site_of(host);
		if ( ( r.party == 1 ) == third ) return false;
	}
	return url_filter_hits(r, q.url, host);
}

std::vector<std::string> strings_of( const base::DictValue &d, const char *key )
{
	std::vector<std::string> out;
	if ( const base::ListValue *l = d.FindList(key) ) {
		for ( const base::Value &v : *l ) if ( v.is_string() ) out.push_back(v.GetString());
	}
	return out;
}

std::set<std::string> set_of( const base::DictValue &d, const char *key )
{
	std::vector<std::string> v = strings_of(d, key);
	return std::set<std::string>(v.begin(), v.end());
}

/* The header operations of modifyHeaders; false when one is not a thing the API has */
bool headers_of( const base::ListValue *l, std::vector<BrExtHeader> *out )
{
	if ( l == nullptr ) return true;
	for ( const base::Value &v : *l ) {
		if ( !v.is_dict() ) return false;
		const std::string *n = v.GetDict().FindString("header"), *o = v.GetDict().FindString("operation");
		const std::string *val = v.GetDict().FindString("value");
		if ( n == nullptr || o == nullptr ) return false;
		BrExtHeader h;
		h.name = *n;
		if ( *o == "set" ) h.op = 's';
		else if ( *o == "append" ) h.op = 'a';
		else if ( *o == "remove" ) h.op = 'r';
		else return false;
		if ( h.op != 'r' ) {
			if ( val == nullptr ) return false;
			h.value = *val;
		}
		out->push_back(std::move(h));
	}
	return true;
}

/*
 * A rule from its JSON. What this browser does not do (a condition on the
 * response's headers, a regex that does not compile, and so on) makes no
 * rule, and the caller counts it, rather than a rule doing something else.
 */
bool rule_of( const base::DictValue &d, const std::string &base_url, Rule *r )
{
	const base::DictValue *ac = d.FindDict("action"), *co = d.FindDict("condition");
	if ( ac == nullptr || co == nullptr ) return false;
	r->id = d.FindInt("id").value_or(0);
	r->priority = d.FindInt("priority").value_or(1);
	const std::string *t = ac->FindString("type");
	if ( t == nullptr ) return false;
	if ( *t == "block" ) r->action = A_BLOCK;
	else if ( *t == "allow" ) r->action = A_ALLOW;
	else if ( *t == "upgradeScheme" ) r->action = A_UPGRADE;
	else if ( *t == "allowAllRequests" ) r->action = A_ALLOW_ALL;
	else if ( *t == "redirect" ) {
		r->action = A_REDIRECT;
		const base::DictValue *rd = ac->FindDict("redirect");
		if ( rd == nullptr ) return false;
		if ( const std::string *u = rd->FindString("url") ) r->redirect = *u;
		else if ( const std::string *p = rd->FindString("extensionPath") ) r->redirect = base_url + p->substr(*p != "" && (*p)[0] == '/' ? 1 : 0);
		else if ( const std::string *s = rd->FindString("regexSubstitution") ) r->substitution = *s;
		else if ( const base::DictValue *t = rd->FindDict("transform") ) r->transform = std::make_shared<base::DictValue>(t->Clone());
		else return false;
	} else if ( *t == "modifyHeaders" ) {
		r->action = A_MODIFY;
		if ( !headers_of(ac->FindList("requestHeaders"), &r->req_headers) || !headers_of(ac->FindList("responseHeaders"), &r->resp_headers) ) return false;
		if ( r->req_headers.empty() && r->resp_headers.empty() ) return false;
	} else {
		return false;
	}
	if ( co->FindList("responseHeaders") != nullptr || co->FindList("excludedResponseHeaders") != nullptr ) return false;
	r->sensitive = co->FindBool("isUrlFilterCaseSensitive").value_or(false);
	if ( const std::string *re = co->FindString("regexFilter") ) {
		re2::RE2::Options o;
		o.set_case_sensitive(r->sensitive);
		o.set_log_errors(false);
		auto rx = std::make_shared<re2::RE2>(*re, o);
		if ( !rx->ok() ) return false;
		r->regex = rx;
	} else if ( const std::string *f = co->FindString("urlFilter") ) {
		std::string s = r->sensitive ? *f : lower(*f);
		if ( s.compare(0, 2, "||") == 0 ) {
			r->left_host = true;
			s.erase(0, 2);
		} else if ( !s.empty() && s[0] == '|' ) {
			r->left = true;
			s.erase(0, 1);
		}
		if ( !s.empty() && s.back() == '|' ) {
			r->right = true;
			s.pop_back();
		}
		r->filter = s;
	}
	r->req_dom = strings_of(*co, "requestDomains");
	r->no_req_dom = strings_of(*co, "excludedRequestDomains");
	r->ini_dom = strings_of(*co, "initiatorDomains");
	r->no_ini_dom = strings_of(*co, "excludedInitiatorDomains");
	r->types = set_of(*co, "resourceTypes");
	r->no_types = set_of(*co, "excludedResourceTypes");
	r->methods = set_of(*co, "requestMethods");
	r->no_methods = set_of(*co, "excludedRequestMethods");
	if ( const std::string *dt = co->FindString("domainType") ) r->party = *dt == "firstParty" ? 1 : 2;
	return true;
}

/*
 * Rules kept so that a request looks at few: a filter "||host^..." is
 * filed under its host and looked for by the request's host and each of
 * its parents; only the rest are gone through one by one. Ad lists are
 * almost all of the first kind and run to tens of thousands.
 */
struct RuleSet {
	std::vector<Rule>						rules;
	std::unordered_map<std::string, std::vector<int>>		by_host;
	std::vector<int>						rest;
	void Index()
	{
		by_host.clear();
		rest.clear();
		for ( size_t i = 0; i < rules.size(); i++ ) {
			const Rule &r = rules[i];
			size_t e = r.left_host ? r.filter.find_first_of("/^*:") : std::string::npos;
			if ( r.left_host && !r.regex && !r.sensitive && e != 0 ) {
				by_host[r.filter.substr(0, e)].push_back((int)i);
			} else if ( !r.req_dom.empty() ) {
				/* its requestDomains say where as well as a filter's host would */
				for ( const std::string &d : r.req_dom ) by_host[lower(d)].push_back((int)i);
			} else {
				rest.push_back((int)i);
			}
		}
	}
};

/* ---------------------------------------------------------------- the extensions */

struct ContentScript {
	std::string			reg_id;		/* scripting.registerContentScripts's, "" the manifest's */
	std::vector<std::string>	matches, exclude;
	std::vector<std::string>	js, css;
	int				when = 2;	/* 0 start, 1 end, 2 idle */
	bool				all_frames = false, main_world = false;
};

/* A context's question that waits for an event: answered with the text, or after 'deadline' */
struct Waiter {
	long					since = 0;
	long					deadline = 0;
	std::function<void( BrExtAns )>		done;
};

void answer_json( const std::function<void( BrExtAns )> &done, std::string body )
{
	BrExtAns a;
	a.ctype = "application/json";
	a.body = std::move(body);
	done(std::move(a));
}

/*
 * What is waiting for its reader: everything since a number, which each
 * context keeps. A context asks for what came since (msg.poll) and, when
 * nothing has, waits (waiters) until something is pushed: nobody asks
 * over and over while nothing happens
 */
struct Log {
	std::deque<std::pair<long, std::string>>	items;
	long						next = 1;
	long						polled_ms = 0;	/* someone is there to answer */
	std::vector<Waiter>				waiters;

	/* What came since 'since', as msg.poll answers it */
	std::string since_text( long since ) const
	{
		std::string s = "{\"seq\":" + std::to_string(next - 1) + ",\"items\":[";
		bool first = true;
		for ( const auto &it : items ) {
			if ( it.first <= since ) continue;
			s += ( first ? "" : "," ) + it.second;
			first = false;
		}
		return s + "]}";
	}

	bool newer( long since ) const { return !items.empty() && items.back().first > since; }

	/* Someone is there: asked lately, or waiting now */
	bool there( long now ) const { return now - polled_ms < 2000 || !waiters.empty(); }

	void Push( std::string s )
	{
		items.emplace_back(next++, std::move(s));
		if ( items.size() > 256 ) items.pop_front();	/* no one has been taking them */
		std::vector<Waiter> w;
		w.swap(waiters);
		for ( Waiter &x : w ) answer_json(x.done, since_text(x.since));
	}
};

struct Alarm {
	long		when_ms = 0;
	double		period_min = 0;
};

struct MenuItem {
	std::string			id, title, parent;
	std::string			type = "normal";
	std::set<std::string>		contexts;
	std::vector<std::string>	doc_patterns, link_patterns;
	bool				checked = false, enabled = true, visible = true;
};

struct Ext {
	std::string				id, uuid, name, version, error;
	bool					enabled = true;
	bool					enabled_mine = false;	/* switched on or off by this process (〈設定〉) */
	std::map<int, Waiter>			answer_waiters;	/* msg.answer waiting for msg.reply, by the message's number */
	bool					removed = false;	/* its place stays: the worlds are numbered by it */
	base::DictValue				manifest;
	std::map<std::string, std::string>	files;
	std::vector<ContentScript>		scripts;
	base::ListValue				registered;	/* the scripts it registered, as it gave them */
	std::string				worker;		/* the path of its service worker */
	bool					worker_module = false;
	struct Accessible { std::vector<std::string> resources, matches; };
	std::vector<Accessible>			accessible;
	std::map<std::string, std::string>	ruleset_paths;	/* by id: read when first enabled, each can be megabytes */
	std::map<std::string, RuleSet>		rulesets;	/* by id */
	std::map<std::string, std::set<int>>	rules_off;	/* updateStaticRules */
	std::set<std::string>			enabled_sets;
	RuleSet					dynamic, session;
	base::ListValue				dynamic_raw, session_raw;
	base::DictValue				storage, storage_sync, storage_session;
	bool					first_done = false;	/* its worker has told onInstalled */
	/* what it may do: what it declared, and what it was granted later of what it could ask for */
	std::set<std::string>			perms, optional_perms;
	std::vector<std::string>		hosts, optional_hosts;
	std::string				active_origin;		/* activeTab: the page it was last clicked on */
	base::DictValue				messages;		/* _locales, keys in lower case */
	std::map<std::string, Alarm>		alarms;
	std::vector<MenuItem>			menu;
	std::string				act_title, act_badge, act_popup, act_color = "#000000", act_text_color = "#ffffff";
	bool					act_enabled = true;
	bool					web_on = false;		/* its worker listens to webRequest */
	/* what waits for its worker, its pages and its content scripts */
	Log					to_worker, to_page, to_content;
	std::map<int, std::string>		replies;
	/* allowAllRequests of the page shown: its priority, -1 none; worked out once per address */
	std::string				allow_url;
	int					allow_pri = -1;
	bool					allow_known = false;
	/* permissions.request waiting for its panel */
	std::map<int, base::DictValue>		asking;
};

std::mutex			g_lock;		/* the page's thread, the worker's loaders and the network's thread all come here */
std::condition_variable		g_waits;	/* with g_lock: a waiter made, for the waiters' thread */
thread_local const std::function<void( BrExtAns )> *g_later;	/* the loader's 'later', while it asks */
thread_local bool		g_deferred;	/* the question was put to wait */
constexpr long			kWaitMs = 20000;	/* a question waits this long at most, then is answered as it is */
std::vector<std::unique_ptr<Ext>> g_exts;
int				g_next_msg = 1;
std::string			g_tab_url, g_tab_title;
std::atomic<bool>		g_any{false};
std::atomic<int>		g_web_listeners{0};

long now_ms()
{
	return (long)base::Time::Now().InMillisecondsSinceUnixEpoch();
}

/* A path the manifest names, as the ZIP has it: "/js/a.js" and "js/a.js" are one file */
std::string norm_path( const std::string &p )
{
	size_t b = 0;
	while ( p.compare(b, 2, "./") == 0 ) b += 2;
	while ( b < p.size() && p[b] == '/' ) b++;
	return p.substr(b);
}

constexpr uint32_t kZipLocal = 0x04034b50, kZipCentral = 0x02014b50, kZipEnd = 0x06054b50;

uint32_t rd32( const std::string &s, size_t at )
{
	const unsigned char *p = (const unsigned char *)s.data() + at;
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

uint32_t rd16( const std::string &s, size_t at )
{
	const unsigned char *p = (const unsigned char *)s.data() + at;
	return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

bool inflate_raw( const char *in, size_t n, size_t want, std::string *out )
{
	z_stream z;
	memset(&z, 0, sizeof(z));
	if ( inflateInit2(&z, -15) != Z_OK ) return false;
	out->resize(want);
	z.next_in = (Bytef *)in;
	z.avail_in = (uInt)n;
	z.next_out = (Bytef *)&(*out)[0];
	z.avail_out = (uInt)want;
	int e = inflate(&z, Z_FINISH);
	inflateEnd(&z);
	return e == Z_STREAM_END && z.total_out == want;
}

/*
 * The files of a ZIP, or of a CRX, which is a ZIP after a header. The
 * central directory is what is believed, and an entry that would reach
 * outside the bytes or outside the extension's root is refused.
 */
bool unzip( const std::string &in, std::map<std::string, std::string> *files, std::string *why )
{
	size_t base = 0;
	if ( in.size() > 12 && in.compare(0, 4, "Cr24") == 0 ) {
		uint32_t ver = rd32(in, 4);
		base = ver >= 3 ? 12 + (size_t)rd32(in, 8) : 16 + (size_t)rd32(in, 8) + rd32(in, 12);
		if ( base >= in.size() ) { *why = "CRXのヘッダが壊れています"; return false; }
	}
	if ( in.size() < base + 22 ) { *why = "ZIPではありません"; return false; }
	size_t end = std::string::npos;
	for ( size_t i = in.size() - 22; ; i-- ) {
		if ( rd32(in, i) == kZipEnd ) { end = i; break; }
		if ( i == base || in.size() - i > 65557 ) break;
	}
	if ( end == std::string::npos ) { *why = "ZIPではありません"; return false; }
	int count = (int)rd16(in, end + 10);
	size_t at = base + rd32(in, end + 16);
	constexpr size_t kMost = 256u * 1024 * 1024;
	size_t total = 0;
	for ( int i = 0; i < count; i++ ) {
		if ( at + 46 > in.size() || rd32(in, at) != kZipCentral ) { *why = "ZIPの目次が壊れています"; return false; }
		uint32_t method = rd16(in, at + 10), csize = rd32(in, at + 20), usize = rd32(in, at + 24);
		uint32_t nlen = rd16(in, at + 28), xlen = rd16(in, at + 30), clen = rd16(in, at + 32);
		size_t local = base + rd32(in, at + 42);
		if ( at + 46 + nlen > in.size() ) { *why = "ZIPの目次が壊れています"; return false; }
		std::string name = in.substr(at + 46, nlen);
		at += 46 + (size_t)nlen + xlen + clen;
		if ( name.empty() || name.back() == '/' ) continue;
		if ( name[0] == '/' || name.find("..") != std::string::npos || name.find('\\') != std::string::npos ) {
			*why = "ZIPに不正な名前があります";
			return false;
		}
		if ( local + 30 > in.size() || rd32(in, local) != kZipLocal ) { *why = "ZIPの中身が壊れています"; return false; }
		size_t data = local + 30 + rd16(in, local + 26) + rd16(in, local + 28);
		total += usize;
		if ( data + csize > in.size() || total > kMost ) { *why = "ZIPの中身が壊れています"; return false; }
		std::string got;
		if ( method == 0 ) {
			got = in.substr(data, csize);
		} else if ( method != 8 || !inflate_raw(in.data() + data, csize, usize, &got) ) {
			*why = "ZIPの圧縮の形式を読めません";
			return false;
		}
		(*files)[name] = std::move(got);
	}
	return true;
}

/* chrome-extension://<id>: an id of Chrome's alphabet from the object's UUID, which stays the same */
std::string id_of( const std::string &uuid )
{
	unsigned char h[SHA256_DIGEST_LENGTH];
	SHA256((const uint8_t *)uuid.data(), uuid.size(), h);
	std::string id;
	for ( int i = 0; i < 16; i++ ) {
		id += (char)( 'a' + ( h[i] >> 4 ) );
		id += (char)( 'a' + ( h[i] & 15 ) );
	}
	return id;
}

std::string base_url( const Ext &e )
{
	return "chrome-extension://" + e.id + "/";
}

/* A match pattern <scheme>://<host>/<path> against an address */
bool pattern_hits( const std::string &p, const GURL &u )
{
	if ( p == "<all_urls>" ) {
		return u.SchemeIsHTTPOrHTTPS() || u.SchemeIs("file") || u.SchemeIs("ws") || u.SchemeIs("wss");
	}
	size_t sch = p.find("://");
	if ( sch == std::string::npos ) return false;
	std::string scheme = p.substr(0, sch);
	if ( scheme == "*" ? !( u.SchemeIsHTTPOrHTTPS() || u.SchemeIsWSOrWSS() ) : scheme != std::string(u.scheme()) ) return false;
	size_t slash = p.find('/', sch + 3);
	if ( slash == std::string::npos ) return false;
	std::string host = p.substr(sch + 3, slash - sch - 3), path = p.substr(slash);
	std::string uh = std::string(u.host());
	/* a pattern's host may name a port: the page's must then be the same */
	size_t colon = host.find(':');
	if ( colon != std::string::npos ) {
		if ( host.substr(colon + 1) != "*" && host.substr(colon + 1) != std::string(u.port()) ) return false;
		host.erase(colon);
	}
	if ( host != "*" ) {
		if ( host.compare(0, 2, "*.") == 0 ) {
			std::string d = host.substr(2);
			if ( !dom_in(uh, { d }) ) return false;
		} else if ( host != uh ) {
			return false;
		}
	}
	std::string up = std::string(u.path());
	if ( u.has_query() ) up += "?" + std::string(u.query());
	return glob_at(path.data(), path.data() + path.size(), up.data(), up.data() + up.size(), true);
}

/* The extension may read and change what goes to this address */
bool host_ok( const Ext &e, const GURL &to )
{
	/* a host granted for http or https is granted for its WebSockets too */
	GURL u = to;
	if ( to.SchemeIsWSOrWSS() ) {
		GURL::Replacements rp;
		rp.SetSchemeStr(to.SchemeIs("ws") ? "http" : "https");
		u = to.ReplaceComponents(rp);
	}
	for ( const std::string &h : e.hosts ) if ( pattern_hits(h, u) ) return true;
	return !e.active_origin.empty() && u.is_valid() && url::Origin::Create(u).Serialize() == e.active_origin;
}

void read_ruleset( Ext &e, const std::string &id )
{
	if ( e.rulesets.count(id) ) return;
	RuleSet &set = e.rulesets[id];
	auto pa = e.ruleset_paths.find(id);
	if ( pa == e.ruleset_paths.end() ) return;
	auto f = e.files.find(pa->second);
	if ( f == e.files.end() ) return;
	std::optional<base::Value> v = base::JSONReader::Read(f->second, base::JSON_ALLOW_TRAILING_COMMAS);
	int bad = 0;
	if ( v && v->is_list() ) {
		for ( const base::Value &rv : v->GetList() ) {
			Rule r;
			if ( rv.is_dict() && rule_of(rv.GetDict(), base_url(e), &r) ) set.rules.push_back(std::move(r));
			else bad++;
		}
	}
	set.Index();
	printf("browser: extension %s: ruleset %s: %zu rules (%d not taken)\n", e.id.c_str(), id.c_str(), set.rules.size(), bad);
}

/* "__MSG_name__" in a string of the manifest's, with the messages of the language */
std::string localized( const Ext &e, const std::string &s )
{
	std::string out = s;
	for ( size_t at = out.find("__MSG_"); at != std::string::npos; at = out.find("__MSG_", at + 1) ) {
		size_t end = out.find("__", at + 6);
		if ( end == std::string::npos ) break;
		const base::DictValue *m = e.messages.FindDict(lower(out.substr(at + 6, end - at - 6)));
		const std::string *t = m != nullptr ? m->FindString("message") : nullptr;
		out.replace(at, end + 2 - at, t != nullptr ? *t : std::string());
	}
	return out;
}

/* The messages: the default language's, with what the reader's language has over them */
void read_locales( Ext &e )
{
	std::vector<std::string> langs;
	if ( const std::string *d = e.manifest.FindString("default_locale") ) langs.push_back(*d);
	langs.push_back("ja");
	langs.push_back("ja_JP");
	for ( const std::string &l : langs ) {
		auto f = e.files.find("_locales/" + l + "/messages.json");
		if ( f == e.files.end() ) continue;
		std::optional<base::Value> v = base::JSONReader::Read(f->second, base::JSON_ALLOW_TRAILING_COMMAS);
		if ( !v || !v->is_dict() ) continue;
		for ( auto kv : v->GetDict() ) {
			if ( !kv.second.is_dict() ) continue;
			base::DictValue m = kv.second.GetDict().Clone();
			/* the placeholders' names too are told apart without case */
			if ( const base::DictValue *ph = m.FindDict("placeholders") ) {
				base::DictValue low;
				for ( auto p : *ph ) low.Set(lower(p.first), p.second.Clone());
				m.Set("placeholders", std::move(low));
			}
			e.messages.Set(lower(kv.first), std::move(m));
		}
	}
}

/* A script as registerContentScripts or the manifest give it (the two spell two names differently) */
ContentScript script_of( const base::DictValue &d )
{
	ContentScript s;
	if ( const std::string *i = d.FindString("id") ) s.reg_id = *i;
	s.matches = strings_of(d, "matches");
	s.exclude = strings_of(d, "exclude_matches");
	for ( const std::string &x : strings_of(d, "excludeMatches") ) s.exclude.push_back(x);
	for ( const std::string &f : strings_of(d, "js") ) s.js.push_back(norm_path(f));
	for ( const std::string &f : strings_of(d, "css") ) s.css.push_back(norm_path(f));
	s.all_frames = d.FindBool("all_frames").value_or(d.FindBool("allFrames").value_or(false));
	const std::string *w = d.FindString("run_at");
	if ( w == nullptr ) w = d.FindString("runAt");
	if ( w != nullptr ) s.when = *w == "document_start" ? 0 : *w == "document_end" ? 1 : 2;
	if ( const std::string *wd = d.FindString("world") ) s.main_world = *wd == "MAIN";
	return s;
}

/* The manifest read: what is wrong with it goes in e.error and the extension does nothing */
void read_manifest( Ext &e )
{
	auto m = e.files.find("manifest.json");
	if ( m == e.files.end() ) { e.error = "manifest.jsonがありません"; return; }
	std::optional<base::Value> v = base::JSONReader::Read(m->second, base::JSON_ALLOW_TRAILING_COMMAS | base::JSON_ALLOW_COMMENTS);
	if ( !v || !v->is_dict() ) { e.error = "manifest.jsonを読めません"; return; }
	e.manifest = std::move(v->GetDict());
	if ( e.manifest.FindInt("manifest_version").value_or(0) != 3 ) { e.error = "Manifest V3ではありません"; return; }
	read_locales(e);
	/* the manifest as getManifest answers it: its words in the reader's language */
	for ( const char *k : { "name", "short_name", "description" } ) {
		if ( const std::string *n = e.manifest.FindString(k) ) e.manifest.Set(k, localized(e, *n));
	}
	if ( const std::string *n = e.manifest.FindString("name") ) e.name = *n;
	if ( const std::string *n = e.manifest.FindString("version") ) e.version = *n;
	for ( const std::string &p : strings_of(e.manifest, "permissions") ) e.perms.insert(p);
	for ( const std::string &p : strings_of(e.manifest, "optional_permissions") ) e.optional_perms.insert(p);
	e.hosts = strings_of(e.manifest, "host_permissions");
	e.optional_hosts = strings_of(e.manifest, "optional_host_permissions");
	if ( const base::ListValue *cs = e.manifest.FindList("content_scripts") ) {
		for ( const base::Value &c : *cs ) {
			if ( c.is_dict() ) e.scripts.push_back(script_of(c.GetDict()));
		}
	}
	if ( const base::DictValue *b = e.manifest.FindDict("background") ) {
		if ( const std::string *w = b->FindString("service_worker") ) e.worker = norm_path(*w);
		if ( const std::string *t = b->FindString("type") ) e.worker_module = *t == "module";
	}
	if ( const base::ListValue *wa = e.manifest.FindList("web_accessible_resources") ) {
		for ( const base::Value &w : *wa ) {
			if ( !w.is_dict() ) continue;
			Ext::Accessible a;
			for ( const std::string &r : strings_of(w.GetDict(), "resources") ) a.resources.push_back(norm_path(r));
			a.matches = strings_of(w.GetDict(), "matches");
			e.accessible.push_back(std::move(a));
		}
	}
	if ( const base::DictValue *ac = e.manifest.FindDict("action") ) {
		if ( const std::string *t = ac->FindString("default_title") ) e.act_title = localized(e, *t);
		if ( const std::string *p = ac->FindString("default_popup") ) e.act_popup = norm_path(*p);
	}
	if ( e.act_title.empty() ) e.act_title = e.name;
	if ( const base::DictValue *dnr = e.manifest.FindDict("declarative_net_request") ) {
		if ( const base::ListValue *rr = dnr->FindList("rule_resources") ) {
			for ( const base::Value &r : *rr ) {
				if ( !r.is_dict() ) continue;
				const std::string *id = r.GetDict().FindString("id"), *path = r.GetDict().FindString("path");
				if ( id == nullptr || path == nullptr ) continue;
				e.ruleset_paths[*id] = norm_path(*path);
				if ( r.GetDict().FindBool("enabled").value_or(false) ) e.enabled_sets.insert(*id);
			}
		}
	}
	printf("browser: extension %s: %s %s, %zu content scripts, %zu rulesets (%zu on)%s\n", e.id.c_str(), e.name.c_str(),
	       e.version.c_str(), e.scripts.size(), e.ruleset_paths.size(), e.enabled_sets.size(), e.worker.empty() ? "" : ", a worker");
}

/* What it declares, as its reader is asked to allow it */
std::vector<std::string> wants_of( const std::set<std::string> &perms, const std::vector<std::string> &hosts,
				   const std::vector<ContentScript> &scripts )
{
	static const struct { const char *perm, *words; } kWords[] = {
		{ "storage", "データをブラウザに保存する" },
		{ "tabs", "開いているページのアドレスとタイトルを読む" },
		{ "activeTab", "拡張機能を押したときのページを扱う" },
		{ "scripting", "ページにスクリプトとスタイルを入れる" },
		{ "webRequest", "ページの通信を見る" },
		{ "declarativeNetRequest", "決めた規則で通信を止める、行き先を変える" },
		{ "declarativeNetRequestWithHostAccess", "許されたサイトの通信を規則で変える" },
		{ "declarativeNetRequestFeedback", "規則に合った通信を調べる" },
		{ "alarms", "決めた時刻に動く" },
		{ "contextMenus", "メニューに項目を足す" },
		{ "unlimitedStorage", "保存の量を制限しない" },
	};
	std::vector<std::string> out;
	for ( const std::string &p : perms ) {
		const char *w = nullptr;
		for ( const auto &k : kWords ) if ( p == k.perm ) w = k.words;
		out.push_back(w != nullptr ? w : p + "(このブラウザにはありません)");
	}
	for ( const std::string &h : hosts ) {
		out.push_back(h == "<all_urls>" || h == "*://*/*" ? "すべてのサイトのデータを読み、変える" : "サイトのデータを読み、変える: " + h);
	}
	std::set<std::string> seen;
	for ( const ContentScript &s : scripts ) {
		for ( const std::string &m : s.matches ) {
			if ( seen.insert(m).second ) {
				out.push_back(m == "<all_urls>" || m == "*://*/*" ? "すべてのページの内容を読み、変える" : "ページの内容を読み、変える: " + m);
			}
		}
	}
	if ( out.empty() ) out.push_back("特別な権限を求めていません");
	return out;
}

/*
 * The state record: what the object itself does not hold of each
 * extension. Every process that runs pages keeps the record (a browser
 * of one process; each renderer, 14.13) and writes it whole when
 * something of it changes there (its storage, its alarms). Whether an
 * extension is on is the user's word, said in one process's 〈設定〉: the
 * others take it from the record as it is when they write, and do not
 * write back what they read at their start
 */
void state_save()
{
	base::DictValue all;
	std::string was;
	std::optional<base::Value> st;
	if ( br_ext_os_state_read(&was) && !was.empty() ) st = base::JSONReader::Read(was, base::JSON_PARSE_RFC);
	for ( auto &p : g_exts ) {
		if ( p->removed ) continue;
		if ( !p->enabled_mine && st && st->is_dict() ) {
			if ( const base::DictValue *o = st->GetDict().FindDict(p->id) ) p->enabled = o->FindBool("enabled").value_or(p->enabled);
		}
		base::DictValue d;
		d.Set("enabled", p->enabled);
		d.Set("storage", p->storage.Clone());
		d.Set("sync", p->storage_sync.Clone());
		d.Set("first_done", p->first_done);
		base::ListValue sets;
		for ( const std::string &s : p->enabled_sets ) sets.Append(s);
		d.Set("rulesets", std::move(sets));
		d.Set("dynamic", p->dynamic_raw.Clone());
		d.Set("registered", p->registered.Clone());
		base::DictValue off;
		for ( auto &o : p->rules_off ) {
			base::ListValue l;
			for ( int r : o.second ) l.Append(r);
			off.Set(o.first, std::move(l));
		}
		d.Set("rules_off", std::move(off));
		base::DictValue als;
		for ( auto &a : p->alarms ) {
			base::DictValue x;
			x.Set("when", (double)a.second.when_ms);
			x.Set("period", a.second.period_min);
			als.Set(a.first, std::move(x));
		}
		d.Set("alarms", std::move(als));
		base::ListValue gp;
		for ( const std::string &s : p->perms ) if ( p->optional_perms.count(s) ) gp.Append(s);
		d.Set("granted", std::move(gp));
		base::ListValue gh;
		for ( const std::string &h : p->hosts ) {
			if ( std::find(p->optional_hosts.begin(), p->optional_hosts.end(), h) != p->optional_hosts.end() ) gh.Append(h);
		}
		d.Set("granted_hosts", std::move(gh));
		all.Set(p->id, std::move(d));
	}
	std::string text;
	if ( base::JSONWriter::Write(all, &text) && !br_ext_os_state_write(text) ) {
		printf("browser: extensions: the state could not be written\n");
		return;
	}
	/* the record says it now: from here on, what it says is taken as it is */
	for ( auto &p : g_exts ) p->enabled_mine = false;
}

void dynamic_read( Ext &e, const base::ListValue &raw )
{
	int bad = 0;
	e.dynamic.rules.clear();
	for ( const base::Value &v : raw ) {
		Rule r;
		if ( v.is_dict() && rule_of(v.GetDict(), base_url(e), &r) ) e.dynamic.rules.push_back(std::move(r));
		else bad++;
	}
	e.dynamic.Index();
	if ( bad > 0 ) printf("browser: extension %s: %d dynamic rules not taken\n", e.id.c_str(), bad);
}

Ext *ext_by_id( const std::string &id )
{
	for ( auto &p : g_exts ) if ( p->id == id && !p->removed ) return p.get();
	return nullptr;
}

void take_registered( Ext &e )
{
	e.scripts.erase(std::remove_if(e.scripts.begin(), e.scripts.end(),
				       []( const ContentScript &s ) { return !s.reg_id.empty(); }), e.scripts.end());
	for ( const base::Value &v : e.registered ) if ( v.is_dict() ) e.scripts.push_back(script_of(v.GetDict()));
}

std::unique_ptr<Ext> ext_load( const std::string &uuid, const base::DictValue *state )
{
	auto e = std::make_unique<Ext>();
	e->uuid = uuid;
	e->id = id_of(uuid);
	std::string bytes, name, why;
	if ( !br_ext_os_read(uuid, &bytes, &name) ) {
		e->error = "実身を読めません";
	} else if ( !unzip(bytes, &e->files, &why) ) {
		e->error = why;
	} else {
		read_manifest(*e);
	}
	if ( state != nullptr ) {
		if ( const base::DictValue *d = state->FindDict(e->id) ) {
			e->enabled = d->FindBool("enabled").value_or(true);
			e->first_done = d->FindBool("first_done").value_or(false);
			if ( const base::DictValue *s = d->FindDict("storage") ) e->storage = s->Clone();
			if ( const base::DictValue *s = d->FindDict("sync") ) e->storage_sync = s->Clone();
			if ( d->FindList("rulesets") ) {
				e->enabled_sets.clear();
				for ( const std::string &r : strings_of(*d, "rulesets") ) e->enabled_sets.insert(r);
			}
			if ( const base::ListValue *dyn = d->FindList("dynamic") ) {
				e->dynamic_raw = dyn->Clone();
				dynamic_read(*e, *dyn);
			}
			if ( const base::ListValue *reg = d->FindList("registered") ) {
				e->registered = reg->Clone();
				take_registered(*e);
			}
			if ( const base::DictValue *off = d->FindDict("rules_off") ) {
				for ( auto kv : *off ) {
					if ( !kv.second.is_list() ) continue;
					for ( const base::Value &r : kv.second.GetList() ) if ( r.is_int() ) e->rules_off[kv.first].insert(r.GetInt());
				}
			}
			if ( const base::DictValue *als = d->FindDict("alarms") ) {
				for ( auto kv : *als ) {
					if ( !kv.second.is_dict() ) continue;
					Alarm a;
					a.when_ms = (long)kv.second.GetDict().FindDouble("when").value_or(0);
					a.period_min = kv.second.GetDict().FindDouble("period").value_or(0);
					e->alarms[kv.first] = a;
				}
			}
			for ( const std::string &p : strings_of(*d, "granted") ) if ( e->optional_perms.count(p) ) e->perms.insert(p);
			for ( const std::string &h : strings_of(*d, "granted_hosts") ) e->hosts.push_back(h);
		}
	}
	if ( e->error.empty() ) for ( const std::string &s : e->enabled_sets ) read_ruleset(*e, s);
	if ( !e->error.empty() ) printf("browser: extension %s: %s\n", uuid.c_str(), e->error.c_str());
	return e;
}

/* ---------------------------------------------------------------- the API's one call */

std::string json_of( const base::Value &v )
{
	std::string s;
	base::JSONWriter::Write(v, &s);
	return s;
}

std::string json_str( const std::string &s )
{
	return json_of(base::Value(s));
}

std::string err_of( const std::string &why )
{
	return "{\"__err\":" + json_str(why) + "}";
}

base::DictValue *area_of( Ext &e, const std::string &name )
{
	return name == "session" ? &e.storage_session : name == "sync" ? &e.storage_sync : name == "local" ? &e.storage : nullptr;
}

/* Told to everyone of the extension: its worker, its pages and its content scripts */
void tell_all( Ext &e, const std::string &json, bool content )
{
	e.to_worker.Push(json);
	e.to_page.Push(json);
	if ( content ) e.to_content.Push(json);
}

void tell_event( Ext &e, const char *ev, base::ListValue args )
{
	base::DictValue env;
	env.Set("ev", ev);
	env.Set("args", std::move(args));
	std::string s = json_of(base::Value(std::move(env)));
	e.to_worker.Push(s);
	e.to_page.Push(s);
}

void storage_changed( Ext &e, const std::string &area, const base::DictValue &changes, const std::string &sid )
{
	if ( changes.empty() ) return;
	base::DictValue env;
	env.Set("ev", "storage");
	env.Set("area", area);
	env.Set("sid", sid);
	env.Set("changes", changes.Clone());
	tell_all(e, json_of(base::Value(std::move(env))), true);
}

base::DictValue tab_of( const Ext &e )
{
	base::DictValue t;
	t.Set("id", 1);
	t.Set("windowId", 1);
	t.Set("index", 0);
	t.Set("active", true);
	t.Set("highlighted", true);
	t.Set("pinned", false);
	t.Set("incognito", false);
	t.Set("status", "complete");
	/* its address and title only for what declared tabs, or may see that site */
	if ( e.perms.count("tabs") || host_ok(e, GURL(g_tab_url)) ) {
		t.Set("url", g_tab_url);
		t.Set("title", g_tab_title);
	}
	return t;
}

void update_rules( Ext &e, RuleSet &set, base::ListValue *raw, const base::DictValue &a, std::string *err )
{
	std::set<int> rm;
	if ( const base::ListValue *l = a.FindList("removeRuleIds") ) {
		for ( const base::Value &v : *l ) if ( v.is_int() ) rm.insert(v.GetInt());
	}
	/* whole or not at all, as the API has it: one bad rule refuses the call */
	std::vector<Rule> added;
	if ( const base::ListValue *l = a.FindList("addRules") ) {
		for ( const base::Value &v : *l ) {
			Rule r;
			if ( !v.is_dict() || !rule_of(v.GetDict(), base_url(e), &r) ) {
				*err = "a rule this browser does not take: " + json_of(v);
				return;
			}
			if ( rm.count(r.id) == 0 ) {
				for ( const Rule &o : set.rules ) {
					if ( o.id == r.id ) {
						*err = "Rule with id " + std::to_string(r.id) + " does not have a unique ID.";
						return;
					}
				}
			}
			added.push_back(std::move(r));
		}
	}
	base::ListValue kept;
	for ( const base::Value &v : *raw ) {
		if ( !v.is_dict() || rm.count(v.GetDict().FindInt("id").value_or(0)) ) continue;
		kept.Append(v.Clone());
	}
	if ( const base::ListValue *l = a.FindList("addRules") ) for ( const base::Value &v : *l ) kept.Append(v.Clone());
	std::vector<Rule> rules;
	for ( Rule &r : set.rules ) if ( !rm.count(r.id) ) rules.push_back(std::move(r));
	for ( Rule &r : added ) rules.push_back(std::move(r));
	set.rules = std::move(rules);
	set.Index();
	*raw = std::move(kept);
	e.allow_known = false;
}

MenuItem *menu_find( Ext &e, const std::string &id )
{
	for ( MenuItem &m : e.menu ) if ( m.id == id ) return &m;
	return nullptr;
}

void menu_props( MenuItem &m, const base::DictValue &o )
{
	if ( const std::string *t = o.FindString("title") ) m.title = *t;
	if ( const std::string *t = o.FindString("type") ) m.type = *t;
	if ( const std::string *t = o.FindString("parentId") ) m.parent = *t;
	if ( o.FindList("contexts") ) m.contexts = set_of(o, "contexts");
	if ( o.FindList("documentUrlPatterns") ) m.doc_patterns = strings_of(o, "documentUrlPatterns");
	if ( o.FindList("targetUrlPatterns") ) m.link_patterns = strings_of(o, "targetUrlPatterns");
	m.checked = o.FindBool("checked").value_or(m.checked);
	m.enabled = o.FindBool("enabled").value_or(m.enabled);
	m.visible = o.FindBool("visible").value_or(m.visible);
}

/* The extension's permission for an operation: what it does not declare is refused here, whoever asks */
const char *perm_of_op( const std::string &op )
{
	static const struct { const char *prefix, *perm; } kOps[] = {
		{ "storage.", "storage" }, { "alarms.", "alarms" }, { "scripting.", "scripting" }, { "web.", "webRequest" },
		{ "contextMenus.", "contextMenus" },
	};
	for ( const auto &k : kOps ) {
		if ( op.compare(0, strlen(k.prefix), k.prefix) == 0 ) return k.perm;
	}
	return nullptr;
}

bool op_allowed( const Ext &e, const std::string &op, const char *from, std::string *why )
{
	/* a content script has only these, as in Chrome: it runs among the page's things */
	if ( strcmp(from, "content") == 0 && op.compare(0, 8, "storage.") != 0 && op.compare(0, 4, "msg.") != 0 ) {
		*why = "chrome API " + op + " is not available to a content script";
		return false;
	}
	if ( const char *p = perm_of_op(op) ) {
		if ( !e.perms.count(p) ) {
			*why = "the extension does not declare the permission " + std::string(p);
			return false;
		}
	}
	if ( op.compare(0, 4, "dnr.") == 0 && !e.perms.count("declarativeNetRequest")
	     && !e.perms.count("declarativeNetRequestWithHostAccess") ) {
		*why = "the extension does not declare the permission declarativeNetRequest";
		return false;
	}
	if ( op.compare(0, 7, "action.") == 0 && e.manifest.FindDict("action") == nullptr ) {
		*why = "the extension has no action in its manifest";
		return false;
	}
	return true;
}

/* What a script of the worker's reaches, the page shown: only what it may see */
bool may_script( const Ext &e, std::string *why )
{
	if ( host_ok(e, GURL(g_tab_url)) ) return true;
	*why = "Cannot access contents of the page. Extension manifest must request permission to access the respective host.";
	return false;
}

std::string files_text( const Ext &e, const std::vector<std::string> &names, std::string *why )
{
	std::string out;
	for ( const std::string &n : names ) {
		auto f = e.files.find(norm_path(n));
		if ( f == e.files.end() ) {
			*why = "Could not load file: '" + n + "'.";
			return std::string();
		}
		out += f->second + "\n";
	}
	return out;
}

int world_of( const Ext &e )
{
	for ( size_t i = 0; i < g_exts.size(); i++ ) if ( g_exts[i].get() == &e ) return 1000 + (int)i;
	return 0;
}

void target_of( const base::DictValue &a, BrExtInject *in )
{
	const base::DictValue *t = a.FindDict("target");
	if ( t == nullptr ) return;
	in->all_frames = t->FindBool("allFrames").value_or(false);
	if ( const base::ListValue *l = t->FindList("frameIds") ) {
		for ( const base::Value &v : *l ) {
			if ( v.is_int() ) in->frames.push_back(v.GetInt());
			else if ( v.is_double() ) in->frames.push_back((long long)v.GetDouble());
		}
	}
}

void alarm_fire( Ext &e, long now )
{
	std::vector<std::string> gone;
	for ( auto &a : e.alarms ) {
		if ( a.second.when_ms > now ) continue;
		base::DictValue d;
		d.Set("name", a.first);
		d.Set("scheduledTime", (double)a.second.when_ms);
		if ( a.second.period_min > 0 ) d.Set("periodInMinutes", a.second.period_min);
		base::ListValue args;
		args.Append(std::move(d));
		tell_event(e, "alarm", std::move(args));
		if ( a.second.period_min > 0 ) a.second.when_ms = now + (long)( a.second.period_min * 60000 );
		else gone.push_back(a.first);
	}
	for ( const std::string &n : gone ) e.alarms.erase(n);
	if ( !gone.empty() ) state_save();
}

base::DictValue alarm_dict( const std::string &name, const Alarm &a )
{
	base::DictValue d;
	d.Set("name", name);
	d.Set("scheduledTime", (double)a.when_ms);
	if ( a.period_min > 0 ) d.Set("periodInMinutes", a.period_min);
	return d;
}

base::ListValue list_of( const std::vector<std::string> &v )
{
	base::ListValue l;
	for ( const std::string &s : v ) l.Append(s);
	return l;
}

/*
 * The waiters' thread: it sleeps until the first of the questions'
 * times and the alarms of the extensions whose workers wait, and is
 * woken when a question begins to wait. Due alarms are told (their
 * events answer the worker's question); questions whose time came are
 * answered as things are
 */
void *waits_thread( void * )
{
	std::unique_lock<std::mutex> hold(g_lock);
	for ( ;; ) {
		long now = now_ms(), next = now + kWaitMs;
		for ( auto &p : g_exts ) {
			Ext &e = *p;
			if ( !e.to_worker.waiters.empty() ) {
				alarm_fire(e, now);
				for ( auto &a : e.alarms ) next = std::min(next, a.second.when_ms);
			}
			for ( Log *q : { &e.to_worker, &e.to_page, &e.to_content } ) {
				std::vector<Waiter> keep;
				for ( Waiter &w : q->waiters ) {
					if ( w.deadline <= now ) {
						answer_json(w.done, q->since_text(w.since));
					} else {
						next = std::min(next, w.deadline);
						keep.push_back(std::move(w));
					}
				}
				q->waiters.swap(keep);
				if ( !q->waiters.empty() ) q->polled_ms = now;
			}
			for ( auto it = e.answer_waiters.begin(); it != e.answer_waiters.end(); ) {
				if ( it->second.deadline <= now ) {
					answer_json(it->second.done, "{\"has\":false}");
					it = e.answer_waiters.erase(it);
				} else {
					next = std::min(next, it->second.deadline);
					++it;
				}
			}
		}
		g_waits.wait_for(hold, std::chrono::milliseconds(std::max(next - now, 1L)));
	}
	return nullptr;
}

/* The waiters' thread started, once; with the lock held */
void waits_begin()
{
	static bool started;
	if ( started ) return;
	started = true;
	pthread_t	th;
	pthread_attr_t	at;
	pthread_attr_init(&at);
	pthread_attr_setstacksize(&at, 256 * 1024);
	pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	(void)pthread_create(&th, &at, waits_thread, nullptr);
	pthread_attr_destroy(&at);
}

/*
 * The answer to message 'id' (msg.reply, or what chrome.scripting's
 * script came to in the page): the first is the answer. When the asker
 * already waits for it (msg.answer), it is given at once; otherwise it is
 * kept for its msg.answer. With the lock held
 */
void reply_given( Ext &e, int id, std::string json )
{
	auto w = e.answer_waiters.find(id);
	if ( w != e.answer_waiters.end() ) {
		answer_json(w->second.done, "{\"has\":true,\"response\":" + json + "}");
		e.answer_waiters.erase(w);
		return;
	}
	if ( !e.replies.count(id) ) e.replies[id] = std::move(json);
	if ( e.replies.size() > 256 ) e.replies.erase(e.replies.begin());
}

/* One call of chrome.*, with the lock held: its answer as JSON */
std::string api_call( Ext &e, const std::string &op, const std::string &arg, const char *from )
{
	std::optional<base::Value> av = arg.empty() ? std::nullopt : base::JSONReader::Read(arg, base::JSON_PARSE_RFC);
	const base::DictValue *a = av && av->is_dict() ? &av->GetDict() : nullptr;
	std::string why;

	if ( !op_allowed(e, op, from, &why) ) {
		printf("browser: extension %s: refused: %s\n", e.id.c_str(), why.c_str());
		return err_of(why);
	}
	if ( op.compare(0, 8, "storage.") == 0 ) {
		if ( a == nullptr ) return err_of("bad call");
		const std::string *an = a->FindString("area");
		base::DictValue *area = an != nullptr ? area_of(e, *an) : nullptr;
		if ( area == nullptr ) return err_of("no such storage area");
		std::string sid = a->FindString("sid") != nullptr ? *a->FindString("sid") : std::string();
		base::DictValue changes;
		auto change = [&]( const std::string &k, const base::Value *now ) {
			base::DictValue c;
			if ( const base::Value *old = area->Find(k) ) c.Set("oldValue", old->Clone());
			if ( now != nullptr ) c.Set("newValue", now->Clone());
			changes.Set(k, std::move(c));
		};
		auto saved = [&]() {
			if ( *an != "session" ) state_save();
			storage_changed(e, *an, changes, sid);
			return std::string("null");
		};
		if ( op == "storage.keys" ) {
			base::ListValue l;
			for ( auto kv : *area ) l.Append(kv.first);
			return json_of(base::Value(std::move(l)));
		}
		if ( op == "storage.get" ) {
			const base::Value *k = a->Find("keys");
			if ( k == nullptr || k->is_none() ) return json_of(base::Value(area->Clone()));
			base::DictValue out;
			if ( k->is_string() ) {
				if ( const base::Value *v = area->Find(k->GetString()) ) out.Set(k->GetString(), v->Clone());
			} else if ( k->is_list() ) {
				for ( const base::Value &s : k->GetList() ) {
					if ( !s.is_string() ) continue;
					if ( const base::Value *v = area->Find(s.GetString()) ) out.Set(s.GetString(), v->Clone());
				}
			} else if ( k->is_dict() ) {
				for ( auto kv : k->GetDict() ) {
					const base::Value *v = area->Find(kv.first);
					out.Set(kv.first, v != nullptr ? v->Clone() : kv.second.Clone());
				}
			}
			return json_of(base::Value(std::move(out)));
		}
		if ( op == "storage.set" ) {
			const base::DictValue *items = a->FindDict("items");
			if ( items == nullptr ) return err_of("bad call");
			for ( auto kv : *items ) {
				change(kv.first, &kv.second);
				area->Set(kv.first, kv.second.Clone());
			}
			return saved();
		}
		if ( op == "storage.remove" ) {
			const base::Value *k = a->Find("keys");
			auto one = [&]( const std::string &s ) {
				if ( area->Find(s) != nullptr ) change(s, nullptr);
				area->Remove(s);
			};
			if ( k != nullptr && k->is_string() ) one(k->GetString());
			if ( k != nullptr && k->is_list() ) for ( const base::Value &s : k->GetList() ) if ( s.is_string() ) one(s.GetString());
			return saved();
		}
		if ( op == "storage.clear" ) {
			for ( auto kv : *area ) change(kv.first, nullptr);
			*area = base::DictValue();
			return saved();
		}
	}
	if ( op == "runtime.first" ) {
		bool first = !e.first_done;
		e.first_done = true;
		if ( first ) state_save();
		return first ? "true" : "false";
	}
	if ( op == "runtime.options" ) {
		const std::string *p = e.manifest.FindString("options_page");
		if ( p == nullptr ) {
			if ( const base::DictValue *ou = e.manifest.FindDict("options_ui") ) p = ou->FindString("page");
		}
		if ( p == nullptr ) return err_of("Could not create an options page.");
		br_ext_command("extpage " + base_url(e) + norm_path(*p));
		return "null";
	}
	if ( op == "msg.send" && a != nullptr ) {
		long now = now_ms();
		const std::string *to = a->FindString("to");
		bool to_tab = to != nullptr && *to == "tab";
		bool from_content = strcmp(from, "content") == 0;
		/* nobody there: refused at once, as Chrome does, rather than left waiting */
		bool worker_there = !e.worker.empty(), page_there = e.to_page.there(now);
		bool content_there = e.to_content.there(now);
		if ( to_tab ? !content_there : !( worker_there || page_there ) ) {
			return err_of("Could not establish connection. Receiving end does not exist.");
		}
		int id = g_next_msg++;
		base::DictValue env;
		env.Set("id", id);
		if ( const base::Value *m = a->Find("msg") ) env.Set("msg", m->Clone());
		for ( const char *k : { "sid", "url", "origin" } ) {
			if ( const std::string *s = a->FindString(k) ) env.Set(k, *s);
		}
		if ( from_content ) {
			env.Set("tab", tab_of(e));
			env.Set("frameId", a->FindDouble("frameId").value_or(0));
		}
		std::string s = json_of(base::Value(std::move(env)));
		if ( to_tab ) {
			e.to_content.Push(s);
		} else {
			if ( strcmp(from, "worker") != 0 ) e.to_worker.Push(s);
			e.to_page.Push(s);
		}
		return std::to_string(id);
	}
	if ( op == "msg.poll" ) {
		Log &q = strcmp(from, "content") == 0 ? e.to_content : strcmp(from, "page") == 0 ? e.to_page : e.to_worker;
		long now = now_ms();
		q.polled_ms = now;
		if ( strcmp(from, "worker") == 0 ) alarm_fire(e, now);
		long since = a != nullptr ? (long)a->FindDouble("since").value_or(a->FindInt("since").value_or(0)) : 0;
		/* -1: a context just made, which has nothing to catch up on */
		if ( since < 0 ) return "{\"seq\":" + std::to_string(q.next - 1) + ",\"items\":[]}";
		if ( !q.newer(since) && g_later != nullptr ) {
			/* nothing yet: the question waits for what comes (Push), or for its time */
			Waiter w;
			w.since = since;
			w.deadline = now + kWaitMs;
			w.done = *g_later;
			q.waiters.push_back(std::move(w));
			g_deferred = true;
			waits_begin();
			g_waits.notify_all();
			return std::string();
		}
		return q.since_text(since);
	}
	if ( op == "msg.reply" && a != nullptr ) {
		const base::Value *r = a->Find("response");
		reply_given(e, a->FindInt("id").value_or(0), r != nullptr ? json_of(*r) : "null");
		return "null";
	}
	if ( op == "msg.answer" && a != nullptr ) {
		/* {has:false} until the other side answered */
		int id = a->FindInt("id").value_or(0);
		auto it = e.replies.find(id);
		if ( it == e.replies.end() ) {
			if ( g_later == nullptr ) return "{\"has\":false}";
			/* not answered yet: the question waits for msg.reply, or for its time */
			Waiter w;
			w.deadline = now_ms() + kWaitMs;
			w.done = *g_later;
			e.answer_waiters[id] = std::move(w);
			g_deferred = true;
			waits_begin();
			g_waits.notify_all();
			return std::string();
		}
		std::string s = "{\"has\":true,\"response\":" + it->second + "}";
		e.replies.erase(it);
		return s;
	}
	if ( op == "tabs.query" ) {
		base::ListValue l;
		bool hit = true;
		if ( a != nullptr ) {
			/* the one tab there is, unless it is asked to be what it is not */
			if ( const base::Value *v = a->Find("url") ) {
				GURL u(g_tab_url);
				bool any = false;
				if ( v->is_string() ) any = pattern_hits(v->GetString(), u);
				if ( v->is_list() ) for ( const base::Value &p : v->GetList() ) any = any || ( p.is_string() && pattern_hits(p.GetString(), u) );
				hit = hit && any && ( e.perms.count("tabs") || host_ok(e, u) );
			}
			if ( a->FindBool("active").value_or(true) == false ) hit = false;
			if ( a->FindBool("pinned").value_or(false) ) hit = false;
		}
		if ( hit ) l.Append(tab_of(e));
		return json_of(base::Value(std::move(l)));
	}
	if ( ( op == "tabs.create" ) && a != nullptr ) {
		if ( const std::string *u = a->FindString("url") ) {
			/* relative to the extension, as a page of its own in this browser when it is its own */
			std::string to = GURL(*u).is_valid() ? *u : base_url(e) + norm_path(*u);
			br_ext_command(( to.compare(0, 19, "chrome-extension://") == 0 ? "extpage " : "extopen " ) + to);
		}
		base::DictValue t;
		t.Set("id", 2);
		t.Set("windowId", 2);
		t.Set("active", true);
		return json_of(base::Value(std::move(t)));
	}
	if ( op == "tabs.update" && a != nullptr ) {
		if ( const std::string *u = a->FindString("url") ) br_ext_command("extgo " + *u);
		return json_of(base::Value(tab_of(e)));
	}
	if ( op == "tabs.reload" ) {
		br_ext_command("reload");
		return "null";
	}
	if ( op == "permissions.getAll" ) {
		base::DictValue d;
		std::vector<std::string> ps(e.perms.begin(), e.perms.end());
		d.Set("permissions", list_of(ps));
		d.Set("origins", list_of(e.hosts));
		return json_of(base::Value(std::move(d)));
	}
	if ( op == "permissions.contains" && a != nullptr ) {
		bool all = true;
		for ( const std::string &p : strings_of(*a, "permissions") ) all = all && e.perms.count(p);
		for ( const std::string &o : strings_of(*a, "origins") ) {
			all = all && ( std::find(e.hosts.begin(), e.hosts.end(), o) != e.hosts.end()
				       || std::find(e.hosts.begin(), e.hosts.end(), "<all_urls>") != e.hosts.end() );
		}
		return all ? "true" : "false";
	}
	if ( op == "permissions.request" && a != nullptr ) {
		std::vector<std::string> need_p, need_h;
		for ( const std::string &p : strings_of(*a, "permissions") ) {
			if ( e.perms.count(p) ) continue;
			if ( !e.optional_perms.count(p) ) return err_of("Only permissions specified in the manifest may be requested.");
			need_p.push_back(p);
		}
		for ( const std::string &o : strings_of(*a, "origins") ) {
			if ( std::find(e.hosts.begin(), e.hosts.end(), o) != e.hosts.end() ) continue;
			if ( std::find(e.optional_hosts.begin(), e.optional_hosts.end(), o) == e.optional_hosts.end() ) {
				return err_of("Only permissions specified in the manifest may be requested.");
			}
			need_h.push_back(o);
		}
		if ( need_p.empty() && need_h.empty() ) return "true";
		/* the reader says, on a panel of the window's thread: the answer comes as a reply */
		int id = g_next_msg++;
		base::DictValue d;
		d.Set("permissions", list_of(need_p));
		d.Set("origins", list_of(need_h));
		e.asking[id] = std::move(d);
		br_ext_command("extperm " + e.id + " " + std::to_string(id));
		return "{\"wait\":" + std::to_string(id) + "}";
	}
	if ( op == "permissions.remove" && a != nullptr ) {
		bool any = false;
		for ( const std::string &p : strings_of(*a, "permissions") ) {
			if ( !e.optional_perms.count(p) ) return err_of("You cannot remove required permissions.");
			any = e.perms.erase(p) > 0 || any;
		}
		for ( const std::string &o : strings_of(*a, "origins") ) {
			if ( std::find(e.optional_hosts.begin(), e.optional_hosts.end(), o) == e.optional_hosts.end() ) {
				return err_of("You cannot remove required permissions.");
			}
			auto it = std::find(e.hosts.begin(), e.hosts.end(), o);
			if ( it != e.hosts.end() ) { e.hosts.erase(it); any = true; }
		}
		if ( any ) state_save();
		return any ? "true" : "false";
	}
	if ( op == "action.get" && a != nullptr ) {
		const std::string *w = a->FindString("what");
		if ( w == nullptr ) return "null";
		if ( *w == "title" ) return json_str(e.act_title);
		if ( *w == "text" ) return json_str(e.act_badge);
		if ( *w == "popup" ) return json_str(e.act_popup.empty() ? std::string() : base_url(e) + e.act_popup);
		if ( *w == "enabled" ) return e.act_enabled ? "true" : "false";
		if ( *w == "color" || *w == "textColor" ) {
			/* as the API answers it: four numbers. The menu's row shows no colour */
			return *w == "color" ? "[0,0,0,255]" : "[255,255,255,255]";
		}
		return "null";
	}
	if ( op == "action.set" && a != nullptr ) {
		const std::string *w = a->FindString("what");
		const base::DictValue *d = a->FindDict("details");
		if ( w == nullptr || d == nullptr ) return "null";
		if ( *w == "title" ) {
			if ( const std::string *t = d->FindString("title") ) e.act_title = t->empty() ? e.name : *t;
		} else if ( *w == "text" ) {
			if ( const std::string *t = d->FindString("text") ) e.act_badge = *t;
		} else if ( *w == "popup" ) {
			if ( const std::string *t = d->FindString("popup") ) {
				std::string b = base_url(e);
				e.act_popup = t->compare(0, b.size(), b) == 0 ? t->substr(b.size()) : norm_path(*t);
			}
		} else if ( *w == "enabled" ) {
			e.act_enabled = d->FindBool("enabled").value_or(true);
		}
		return "null";
	}
	if ( op == "action.open" ) {
		if ( e.act_popup.empty() ) return err_of("The extension has no popup.");
		br_ext_command("extpage " + base_url(e) + e.act_popup);
		return "null";
	}
	if ( op == "alarms.create" && a != nullptr ) {
		const std::string *n = a->FindString("name");
		const base::DictValue *in = a->FindDict("info");
		if ( n == nullptr || in == nullptr ) return err_of("bad call");
		Alarm al;
		double period = in->FindDouble("periodInMinutes").value_or(0);
		std::optional<double> when = in->FindDouble("when"), delay = in->FindDouble("delayInMinutes");
		long now = now_ms();
		al.period_min = period;
		al.when_ms = when ? (long)*when : delay ? now + (long)( *delay * 60000 ) : now + (long)( period * 60000 );
		e.alarms[*n] = al;
		state_save();
		return "null";
	}
	if ( op == "alarms.get" && a != nullptr ) {
		const std::string *n = a->FindString("name");
		auto it = n != nullptr ? e.alarms.find(*n) : e.alarms.end();
		return it == e.alarms.end() ? "null" : json_of(base::Value(alarm_dict(it->first, it->second)));
	}
	if ( op == "alarms.getAll" ) {
		base::ListValue l;
		for ( auto &x : e.alarms ) l.Append(alarm_dict(x.first, x.second));
		return json_of(base::Value(std::move(l)));
	}
	if ( op == "alarms.clear" && a != nullptr ) {
		const std::string *n = a->FindString("name");
		bool had = n != nullptr && e.alarms.erase(*n) > 0;
		if ( had ) state_save();
		return had ? "true" : "false";
	}
	if ( op == "alarms.clearAll" ) {
		bool had = !e.alarms.empty();
		e.alarms.clear();
		if ( had ) state_save();
		return had ? "true" : "false";
	}
	if ( op == "contextMenus.create" && a != nullptr ) {
		MenuItem m;
		const base::Value *iv = a->Find("id");
		m.id = iv != nullptr && iv->is_string() ? iv->GetString() : iv != nullptr ? json_of(*iv) : std::string();
		if ( menu_find(e, m.id) != nullptr ) return err_of("Cannot create item with duplicate id " + m.id);
		menu_props(m, *a);
		if ( m.contexts.empty() ) m.contexts.insert("page");
		e.menu.push_back(std::move(m));
		return "null";
	}
	if ( op == "contextMenus.update" && a != nullptr ) {
		const base::Value *iv = a->Find("id");
		const base::DictValue *p = a->FindDict("props");
		MenuItem *m = iv != nullptr && iv->is_string() ? menu_find(e, iv->GetString()) : nullptr;
		if ( m == nullptr || p == nullptr ) return err_of("Cannot find menu item");
		menu_props(*m, *p);
		return "null";
	}
	if ( op == "contextMenus.remove" && a != nullptr ) {
		const base::Value *iv = a->Find("id");
		if ( iv == nullptr || !iv->is_string() || menu_find(e, iv->GetString()) == nullptr ) return err_of("Cannot find menu item");
		std::string gone = iv->GetString();
		e.menu.erase(std::remove_if(e.menu.begin(), e.menu.end(), [&]( const MenuItem &m ) {
			return m.id == gone || m.parent == gone;
		}), e.menu.end());
		return "null";
	}
	if ( op == "contextMenus.removeAll" ) {
		e.menu.clear();
		return "null";
	}
	if ( op == "scripting.exec" && a != nullptr ) {
		if ( !may_script(e, &why) ) return err_of(why);
		BrExtInject in;
		in.world = world_of(e);
		in.msg = g_next_msg++;
		target_of(*a, &in);
		const std::string *wd = a->FindString("world");
		in.main_world = wd != nullptr && *wd == "MAIN";
		if ( const std::string *f = a->FindString("func") ) {
			const base::ListValue *args = a->FindList("args");
			in.code = "(" + *f + ").apply(null," + ( args != nullptr ? json_of(base::Value(args->Clone())) : std::string("[]") ) + ")";
		} else {
			in.code = files_text(e, strings_of(*a, "files"), &why);
			if ( !why.empty() ) return err_of(why);
		}
		br_ext_inject(in);
		return std::to_string(in.msg);
	}
	if ( op == "scripting.css" && a != nullptr ) {
		if ( !may_script(e, &why) ) return err_of(why);
		BrExtInject in;
		in.world = world_of(e);
		in.msg = g_next_msg++;
		target_of(*a, &in);
		in.remove = a->FindBool("remove").value_or(false);
		const std::string *o = a->FindString("origin");
		in.user_origin = o != nullptr && *o == "USER";
		if ( const std::string *c = a->FindString("css") ) in.css = *c;
		else in.css = files_text(e, strings_of(*a, "files"), &why);
		if ( !why.empty() ) return err_of(why);
		in.key = e.id + ":" + in.css;
		br_ext_inject(in);
		return std::to_string(in.msg);
	}
	if ( op == "scripting.register" && a != nullptr ) {
		const base::ListValue *l = a->FindList("scripts");
		if ( l == nullptr ) return err_of("bad call");
		for ( const base::Value &v : *l ) {
			const std::string *id = v.is_dict() ? v.GetDict().FindString("id") : nullptr;
			if ( id == nullptr || id->empty() || ( *id )[0] == '_' ) return err_of("Invalid content script id.");
			for ( const base::Value &r : e.registered ) {
				const std::string *o = r.is_dict() ? r.GetDict().FindString("id") : nullptr;
				if ( o != nullptr && *o == *id ) return err_of("Duplicate script ID '" + *id + "'");
			}
			files_text(e, strings_of(v.GetDict(), "js"), &why);
			files_text(e, strings_of(v.GetDict(), "css"), &why);
			if ( !why.empty() ) return err_of(why);
		}
		for ( const base::Value &v : *l ) e.registered.Append(v.Clone());
		take_registered(e);
		state_save();
		return "null";
	}
	if ( op == "scripting.update" && a != nullptr ) {
		const base::ListValue *l = a->FindList("scripts");
		if ( l == nullptr ) return err_of("bad call");
		for ( const base::Value &v : *l ) {
			const std::string *id = v.is_dict() ? v.GetDict().FindString("id") : nullptr;
			bool found = false;
			for ( base::Value &r : e.registered ) {
				const std::string *o = r.is_dict() ? r.GetDict().FindString("id") : nullptr;
				if ( id != nullptr && o != nullptr && *o == *id ) {
					r.GetDict().Merge(v.GetDict().Clone());
					found = true;
				}
			}
			if ( !found ) return err_of("Content script with ID '" + ( id != nullptr ? *id : std::string() ) + "' does not exist.");
		}
		take_registered(e);
		state_save();
		return "null";
	}
	if ( op == "scripting.unregister" ) {
		std::vector<std::string> ids = a != nullptr ? strings_of(*a, "ids") : std::vector<std::string>();
		bool all = a == nullptr || a->FindList("ids") == nullptr;
		base::ListValue kept;
		for ( const base::Value &r : e.registered ) {
			const std::string *o = r.is_dict() ? r.GetDict().FindString("id") : nullptr;
			if ( all || o == nullptr || std::find(ids.begin(), ids.end(), *o) != ids.end() ) continue;
			kept.Append(r.Clone());
		}
		e.registered = std::move(kept);
		take_registered(e);
		state_save();
		return "null";
	}
	if ( op == "scripting.registered" ) {
		std::vector<std::string> ids = a != nullptr ? strings_of(*a, "ids") : std::vector<std::string>();
		bool all = a == nullptr || a->FindList("ids") == nullptr;
		base::ListValue l;
		for ( const base::Value &r : e.registered ) {
			const std::string *o = r.is_dict() ? r.GetDict().FindString("id") : nullptr;
			if ( all || ( o != nullptr && std::find(ids.begin(), ids.end(), *o) != ids.end() ) ) l.Append(r.Clone());
		}
		return json_of(base::Value(std::move(l)));
	}
	if ( op == "web.listen" ) {
		if ( !e.web_on ) {
			e.web_on = true;
			g_web_listeners++;
		}
		return "null";
	}
	std::string err;
	if ( op == "dnr.getEnabled" ) {
		base::ListValue l;
		for ( const std::string &s : e.enabled_sets ) l.Append(s);
		return json_of(base::Value(std::move(l)));
	}
	if ( op == "dnr.updateEnabled" && a != nullptr ) {
		for ( const std::string &s : strings_of(*a, "enableRulesetIds") ) {
			if ( !e.ruleset_paths.count(s) ) return err_of("Invalid ruleset id: " + s + ".");
		}
		for ( const std::string &s : strings_of(*a, "disableRulesetIds") ) e.enabled_sets.erase(s);
		for ( const std::string &s : strings_of(*a, "enableRulesetIds") ) {
			read_ruleset(e, s);
			e.enabled_sets.insert(s);
		}
		e.allow_known = false;
		state_save();
		return "null";
	}
	if ( op == "dnr.updateStatic" && a != nullptr ) {
		const std::string *id = a->FindString("rulesetId");
		if ( id == nullptr || !e.ruleset_paths.count(*id) ) return err_of("Invalid ruleset id.");
		std::set<int> &off = e.rules_off[*id];
		if ( const base::ListValue *l = a->FindList("disableRuleIds") ) for ( const base::Value &v : *l ) if ( v.is_int() ) off.insert(v.GetInt());
		if ( const base::ListValue *l = a->FindList("enableRuleIds") ) for ( const base::Value &v : *l ) if ( v.is_int() ) off.erase(v.GetInt());
		e.allow_known = false;
		state_save();
		return "null";
	}
	if ( op == "dnr.getDisabled" && a != nullptr ) {
		const std::string *id = a->FindString("rulesetId");
		base::ListValue l;
		if ( id != nullptr ) for ( int r : e.rules_off[*id] ) l.Append(r);
		return json_of(base::Value(std::move(l)));
	}
	if ( op == "dnr.regexOk" && a != nullptr ) {
		base::DictValue d;
		const std::string *re = a->FindString("regex");
		re2::RE2::Options o;
		o.set_case_sensitive(a->FindBool("isCaseSensitive").value_or(false));
		o.set_log_errors(false);
		bool ok = re != nullptr && re2::RE2(*re, o).ok();
		d.Set("isSupported", ok);
		if ( !ok ) d.Set("reason", "syntaxError");
		return json_of(base::Value(std::move(d)));
	}
	auto rules_answer = [&]( const base::ListValue &raw ) {
		/* the filter of getDynamicRules and getSessionRules: only the ids asked for */
		const base::ListValue *ids = a != nullptr ? a->FindList("ruleIds") : nullptr;
		if ( ids == nullptr ) return json_of(base::Value(raw.Clone()));
		std::set<int> want;
		for ( const base::Value &v : *ids ) if ( v.is_int() ) want.insert(v.GetInt());
		base::ListValue l;
		for ( const base::Value &v : raw ) {
			if ( v.is_dict() && want.count(v.GetDict().FindInt("id").value_or(0)) ) l.Append(v.Clone());
		}
		return json_of(base::Value(std::move(l)));
	};
	if ( op == "dnr.updateDynamic" && a != nullptr ) {
		update_rules(e, e.dynamic, &e.dynamic_raw, *a, &err);
		if ( !err.empty() ) return err_of(err);
		state_save();
		return "null";
	}
	if ( op == "dnr.getDynamic" ) return rules_answer(e.dynamic_raw);
	if ( op == "dnr.updateSession" && a != nullptr ) {
		update_rules(e, e.session, &e.session_raw, *a, &err);
		return err.empty() ? "null" : err_of(err);
	}
	if ( op == "dnr.getSession" ) return rules_answer(e.session_raw);
	printf("browser: extension %s: chrome API %s is not done here\n", e.id.c_str(), op.c_str());
	return err_of("chrome API " + op + " is not done in this browser");
}

/* The order of the API's actions within a priority: allow over block over the rest */
int action_rank( int a )
{
	return a == A_ALLOW ? 4 : a == A_ALLOW_ALL ? 3 : a == A_BLOCK ? 2 : a == A_UPGRADE ? 1 : 0;
}

template <typename Look>
void each_candidate( const RuleSet &set, const std::set<int> *off, const std::string &host, Look look )
{
	auto one = [&]( int i ) {
		const Rule &r = set.rules[(size_t)i];
		if ( off == nullptr || !off->count(r.id) ) look(r);
	};
	for ( size_t at = 0; at != std::string::npos; ) {
		auto it = set.by_host.find(host.substr(at));
		if ( it != set.by_host.end() ) for ( int i : it->second ) one(i);
		size_t dot = host.find('.', at);
		at = dot == std::string::npos ? dot : dot + 1;
	}
	for ( int i : set.rest ) one(i);
}

/* Each set of the extension's in force */
template <typename Look>
void each_set( Ext &e, const std::string &host, Look look )
{
	for ( const std::string &s : e.enabled_sets ) {
		auto it = e.rulesets.find(s);
		auto off = e.rules_off.find(s);
		if ( it != e.rulesets.end() ) each_candidate(it->second, off != e.rules_off.end() ? &off->second : nullptr, host, look);
	}
	each_candidate(e.dynamic, nullptr, host, look);
	each_candidate(e.session, nullptr, host, look);
}

/* The extension's rules do something to this request only in this way */
int dnr_mode( const Ext &e )
{
	return e.perms.count("declarativeNetRequest") ? 1 : e.perms.count("declarativeNetRequestWithHostAccess") ? 2 : 0;
}

bool active_for_rules( const Ext &e )
{
	return e.enabled && !e.removed && e.error.empty() && dnr_mode(e) != 0;
}

/*
 * The request's address and its asker are both within the extension's hosts,
 * which a redirect and modifyHeaders need under declarativeNetRequest, and
 * every rule under declarativeNetRequestWithHostAccess.
 */
bool hosts_ok_for( const Ext &e, const GURL &u, const std::string &initiator )
{
	return host_ok(e, u) && ( initiator.empty() || host_ok(e, GURL(initiator + "/")) );
}

/*
 * allowAllRequests is for a document and all that it asks for: the rule that
 * matches the page shown holds under its priority for its requests. Only the
 * top document is known here, so a frame's own such rule holds for that
 * frame's document alone.
 */
int allow_all_priority( Ext &e )
{
	if ( e.allow_known && e.allow_url == g_tab_url ) return e.allow_pri;
	e.allow_known = true;
	e.allow_url = g_tab_url;
	e.allow_pri = -1;
	GURL u(g_tab_url);
	if ( !u.SchemeIsHTTPOrHTTPS() ) return -1;
	BrExtReq top;
	top.url = g_tab_url;
	top.type = "main_frame";
	top.method = "get";
	std::string host = lower(std::string(u.host()));
	each_set(e, host, [&]( const Rule &r ) {
		if ( r.action == A_ALLOW_ALL && r.priority > e.allow_pri && rule_hits(r, top, host, std::string()) ) e.allow_pri = r.priority;
	});
	return e.allow_pri;
}

bool is_document( const BrExtReq &q )
{
	return q.type == "main_frame" || q.type == "sub_frame";
}

/* The rule that decides for this extension, nullptr none */
const Rule *ext_best( Ext &e, const BrExtReq &q, const GURL &u, const std::string &host, const std::string &ini )
{
	int mode = dnr_mode(e);
	bool hosts = hosts_ok_for(e, u, q.initiator);
	if ( mode == 2 && !hosts ) return nullptr;
	int floor = is_document(q) ? -1 : allow_all_priority(e);
	const Rule *best = nullptr;
	each_set(e, host, [&]( const Rule &r ) {
		if ( r.action == A_MODIFY || r.priority <= floor ) return;
		if ( r.action == A_REDIRECT && !hosts ) return;
		if ( best != nullptr && ( r.priority < best->priority
					  || ( r.priority == best->priority && action_rank(r.action) <= action_rank(best->action) ) ) ) return;
		if ( rule_hits(r, q, host, ini) ) best = &r;
	});
	return best;
}

/* The address a redirect rule makes of this one */
std::string redirect_of( const Rule &r, const GURL &u )
{
	if ( !r.redirect.empty() ) return r.redirect;
	if ( !r.substitution.empty() && r.regex ) {
		/* the first match within the address is what the pattern replaces */
		std::string out = u.spec();
		return re2::RE2::Replace(&out, *r.regex, r.substitution) ? out : std::string();
	}
	if ( r.transform ) {
		GURL::Replacements rp;
		const base::DictValue &t = *r.transform;
		const std::string *v;
		std::string tmp[6];
		if ( ( v = t.FindString("scheme") ) ) rp.SetSchemeStr(tmp[0] = *v);
		if ( ( v = t.FindString("host") ) ) rp.SetHostStr(tmp[1] = *v);
		if ( ( v = t.FindString("port") ) ) { if ( v->empty() ) rp.ClearPort(); else rp.SetPortStr(tmp[2] = *v); }
		if ( ( v = t.FindString("path") ) ) rp.SetPathStr(tmp[3] = *v);
		if ( ( v = t.FindString("query") ) ) {
			if ( v->empty() ) rp.ClearQuery();
			else rp.SetQueryStr(tmp[4] = ( *v )[0] == '?' ? v->substr(1) : *v);
		}
		if ( ( v = t.FindString("fragment") ) ) {
			if ( v->empty() ) rp.ClearRef();
			else rp.SetRefStr(tmp[5] = ( *v )[0] == '#' ? v->substr(1) : *v);
		}
		return u.ReplaceComponents(rp).spec();
	}
	return std::string();
}

/* The extension's own, its asked-for: "__MSG_" is left to it. The JSON the shim is made with */
std::string info_of( const Ext &e )
{
	base::DictValue d;
	std::vector<std::string> ps(e.perms.begin(), e.perms.end());
	for ( const std::string &p : e.optional_perms ) ps.push_back(p);	/* the name is there, a call is refused until granted */
	d.Set("perms", list_of(ps));
	d.Set("messages", e.messages.Clone());
	d.Set("lang", "ja");
#if defined(__x86_64__)
	d.Set("arch", "x86-64");
#else
	d.Set("arch", "arm64");
#endif
	return json_of(base::Value(std::move(d)));
}

std::string manifest_text( const Ext &e )
{
	return json_of(base::Value(e.manifest.Clone()));
}

/* A file of the extension's reaches a page only if it says so, for that page's origin */
bool accessible_to_pages( const Ext &e, const std::string &path, const std::string &initiator )
{
	GURL page(initiator + "/");
	for ( const Ext::Accessible &a : e.accessible ) {
		bool origin_ok = false;
		for ( const std::string &m : a.matches ) origin_ok = origin_ok || pattern_hits(m, page);
		if ( !origin_ok ) continue;
		for ( const std::string &r : a.resources ) {
			if ( glob_at(r.data(), r.data() + r.size(), path.data(), path.data() + path.size(), true) ) return true;
		}
	}
	return false;
}

std::string mime_of( const std::string &path )
{
	static const struct { const char *ext, *mime; } kinds[] = {
		{ ".js", "text/javascript" }, { ".mjs", "text/javascript" }, { ".json", "application/json" },
		{ ".html", "text/html" }, { ".css", "text/css" }, { ".png", "image/png" }, { ".svg", "image/svg+xml" },
		{ ".gif", "image/gif" }, { ".jpg", "image/jpeg" }, { ".webp", "image/webp" }, { ".wasm", "application/wasm" },
		{ ".woff2", "font/woff2" }, { ".txt", "text/plain" }, { ".mp4", "video/mp4" }, { ".mp3", "audio/mpeg" },
	};
	for ( const auto &k : kinds ) {
		size_t n = strlen(k.ext);
		if ( path.size() >= n && path.compare(path.size() - n, n, k.ext) == 0 ) return k.mime;
	}
	return "application/octet-stream";
}

std::string worker_shim( const Ext &e )
{
	std::string shim = kExtWorkerShim;
	shim.replace(shim.find("__ID__"), 6, e.id);
	shim.replace(shim.find("__MANIFEST__"), 12, manifest_text(e));
	shim.replace(shim.find("__INFO__"), 8, info_of(e));
	return shim;
}

}  // namespace

/* ---------------------------------------------------------------- what the rest of the browser calls */

BrExtAct br_ext_serve_later( const BrExtReq &q, BrExtAns *ans, std::function<void( BrExtAns )> later )
{
	g_later = &later;
	g_deferred = false;
	BrExtAct act = br_ext_serve(q, ans);
	g_later = nullptr;
	return ( act == BR_EXT_SERVED && g_deferred ) ? BR_EXT_LATER : act;
}

BrExtAct br_ext_serve( const BrExtReq &q, BrExtAns *ans )
{
	GURL u(q.url);
	std::lock_guard<std::mutex> hold(g_lock);
	Ext *e = ext_by_id(std::string(u.host()));
	std::string path = std::string(u.path()).substr(1);
	ans->status = 200;
	ans->ctype = mime_of(path);
	bool own = q.initiator == "chrome-extension://" + std::string(u.host());
	if ( e == nullptr || !e->error.empty() || !e->enabled ) return BR_EXT_BLOCK;
	if ( path.compare(0, 8, "__brext/") == 0 ) {
		if ( !own ) return BR_EXT_BLOCK;		/* only the extension's own worker */
		ans->ctype = "application/json";
		ans->body = api_call(*e, path.substr(8), q.body, "worker");
		return BR_EXT_SERVED;
	}
	if ( !own && !q.initiator.empty() && !accessible_to_pages(*e, path, q.initiator) ) return BR_EXT_BLOCK;
	if ( path == "__brext_shim.js" && own ) {
		/* what a module worker imports first: its imports run before its own code */
		ans->ctype = "text/javascript";
		ans->body = worker_shim(*e);
		return BR_EXT_SERVED;
	}
	auto f = e->files.find(path);
	if ( f == e->files.end() ) return BR_EXT_BLOCK;
	ans->body = f->second;
	if ( path == e->worker ) {
		ans->body = e->worker_module ? "import '/__brext_shim.js';\n" + ans->body : worker_shim(*e) + ans->body;
	}
	if ( ans->ctype == "text/html" ) {
		/* the policy of an extension's pages: its own scripts only, unless its manifest says another */
		std::string csp = "script-src 'self' 'wasm-unsafe-eval'; object-src 'self';";
		if ( const base::DictValue *c = e->manifest.FindDict("content_security_policy") ) {
			if ( const std::string *p = c->FindString("extension_pages") ) csp = *p;
		}
		ans->headers = "Content-Security-Policy: " + csp + "\r\n";
	}
	return BR_EXT_SERVED;
}

bool br_ext_any()
{
	return g_any;
}

BrExtAct br_ext_rules( const BrExtReq &q, BrExtAns *ans )
{
	if ( !g_any ) return BR_EXT_PASS;
	GURL u(q.url);
	if ( !u.SchemeIsHTTPOrHTTPS() && !u.SchemeIsWSOrWSS() ) return BR_EXT_PASS;
	std::lock_guard<std::mutex> hold(g_lock);
	std::string host = lower(std::string(u.host())), ini = lower(std::string(GURL(q.initiator).host()));
	bool from_ext = q.initiator.compare(0, 19, "chrome-extension://") == 0;
	/*
	 * Each extension decides for itself and one's allow does not undo
	 * another's block: a block holds first, then the last installed
	 * extension's redirect or upgrade.
	 */
	const Rule *move = nullptr;
	Ext *mover = nullptr;
	for ( auto &p : g_exts ) {
		Ext &e = *p;
		if ( !active_for_rules(e) ) continue;
		/* another extension's requests are not its to touch */
		if ( from_ext && q.initiator != "chrome-extension://" + e.id ) continue;
		const Rule *r = ext_best(e, q, u, host, from_ext ? std::string() : ini);
		if ( r == nullptr ) continue;
		if ( r->action == A_BLOCK ) {
			printf("browser: extension: blocked %s\n", q.url.c_str());
			return BR_EXT_BLOCK;
		}
		if ( r->action == A_REDIRECT || r->action == A_UPGRADE ) {
			move = r;
			mover = &e;
		}
	}
	if ( move == nullptr ) return BR_EXT_PASS;
	(void)mover;
	if ( move->action == A_UPGRADE ) {
		if ( !u.SchemeIs("http") && !u.SchemeIs("ws") ) return BR_EXT_PASS;
		GURL::Replacements rp;
		rp.SetSchemeStr(u.SchemeIs("ws") ? "wss" : "https");
		ans->redirect = u.ReplaceComponents(rp).spec();
		return BR_EXT_REDIRECT;
	}
	ans->redirect = redirect_of(*move, u);
	/* a rule that leads back to the same address would go on for ever */
	if ( ans->redirect.empty() || ans->redirect == q.url ) return BR_EXT_PASS;
	return BR_EXT_REDIRECT;
}

void br_ext_header_rules( const BrExtReq &q, std::vector<BrExtHeader> *request, std::vector<BrExtHeader> *response )
{
	if ( !g_any ) return;
	GURL u(q.url);
	if ( !u.SchemeIsHTTPOrHTTPS() && !u.SchemeIsWSOrWSS() ) return;
	std::lock_guard<std::mutex> hold(g_lock);
	std::string host = lower(std::string(u.host())), ini = lower(std::string(GURL(q.initiator).host()));
	bool from_ext = q.initiator.compare(0, 19, "chrome-extension://") == 0;
	/* the last installed first, within each by priority */
	std::set<std::string> taken_req, taken_resp;
	for ( size_t i = g_exts.size(); i-- > 0; ) {
		Ext &e = *g_exts[i];
		if ( !active_for_rules(e) || ( from_ext && q.initiator != "chrome-extension://" + e.id ) ) continue;
		if ( !hosts_ok_for(e, u, q.initiator) ) continue;
		std::string who = from_ext ? std::string() : ini;
		int floor = is_document(q) ? -1 : allow_all_priority(e);
		/* a rule that allows the request at a higher priority keeps its headers as they are */
		each_set(e, host, [&]( const Rule &r ) {
			if ( ( r.action == A_ALLOW || r.action == A_ALLOW_ALL ) && r.priority > floor && rule_hits(r, q, host, who) ) floor = r.priority;
		});
		std::vector<const Rule *> hits;
		each_set(e, host, [&]( const Rule &r ) {
			if ( r.action == A_MODIFY && r.priority > floor && rule_hits(r, q, host, who) ) hits.push_back(&r);
		});
		std::sort(hits.begin(), hits.end(), []( const Rule *a, const Rule *b ) { return a->priority > b->priority; });
		for ( const Rule *r : hits ) {
			auto apply = [&]( const std::vector<BrExtHeader> &ops, std::vector<BrExtHeader> *out, std::set<std::string> *taken ) {
				if ( out == nullptr ) return;
				/* what a rule set or removed stays so: an append alone can join what was appended */
				for ( const BrExtHeader &h : ops ) {
					std::string n = lower(h.name);
					if ( taken->count(n) ) continue;
					out->push_back(h);
					if ( h.op != 'a' ) taken->insert(n);
				}
			};
			apply(r->req_headers, request, &taken_req);
			apply(r->resp_headers, response, &taken_resp);
		}
	}
}

bool br_ext_cors_free( const std::string &initiator, const std::string &url )
{
	if ( initiator.compare(0, 19, "chrome-extension://") != 0 ) return false;
	std::lock_guard<std::mutex> hold(g_lock);
	Ext *e = ext_by_id(initiator.substr(19));
	return e != nullptr && host_ok(*e, GURL(url));
}

bool br_ext_web_wanted()
{
	return g_web_listeners > 0;
}

void br_ext_web( const char *event, long long id, const BrExtReq &q, int status, const std::string &more )
{
	if ( g_web_listeners == 0 ) return;
	GURL u(q.url);
	std::lock_guard<std::mutex> hold(g_lock);
	std::optional<base::Value> extra = more.empty() ? std::nullopt : base::JSONReader::Read(more, base::JSON_PARSE_RFC);
	for ( auto &p : g_exts ) {
		Ext &e = *p;
		if ( !e.web_on || !e.enabled || e.removed || !e.perms.count("webRequest") ) continue;
		if ( !host_ok(e, u) ) continue;
		base::DictValue d = extra && extra->is_dict() ? extra->GetDict().Clone() : base::DictValue();
		d.Set("requestId", std::to_string(id));
		d.Set("url", q.url);
		d.Set("method", q.method);
		d.Set("type", q.type);
		d.Set("tabId", 1);
		d.Set("frameId", q.type == "main_frame" ? 0 : -1);
		d.Set("parentFrameId", -1);
		if ( !q.initiator.empty() ) d.Set("initiator", q.initiator);
		if ( status != 0 ) d.Set("statusCode", status);
		d.Set("timeStamp", (double)now_ms());
		base::DictValue env;
		env.Set("ev", "web");
		env.Set("name", event);
		env.Set("details", std::move(d));
		e.to_worker.Push(json_of(base::Value(std::move(env))));
	}
}

void br_ext_tab_set( const std::string &url, const std::string &title )
{
	std::lock_guard<std::mutex> hold(g_lock);
	bool moved = url != g_tab_url, named = title != g_tab_title;
	g_tab_url = url;
	g_tab_title = title;
	if ( !moved && !named ) return;
	for ( auto &p : g_exts ) {
		Ext &e = *p;
		if ( e.removed || !e.enabled ) continue;
		/* activeTab holds until the tab leaves that page's origin */
		if ( moved && !e.active_origin.empty() && url::Origin::Create(GURL(url)).Serialize() != e.active_origin ) e.active_origin.clear();
		base::DictValue change;
		if ( moved ) change.Set("url", url);
		if ( named ) change.Set("title", title);
		change.Set("status", "complete");
		base::ListValue args;
		args.Append(1);
		args.Append(std::move(change));
		args.Append(tab_of(e));
		tell_event(e, "tabUpdated", std::move(args));
	}
}

std::vector<BrExtInfo> br_ext_list()
{
	std::lock_guard<std::mutex> hold(g_lock);
	std::vector<BrExtInfo> out;
	for ( auto &p : g_exts ) {
		if ( !p->removed ) {
			out.push_back(BrExtInfo{ p->id, p->uuid, p->name, p->version, p->enabled, p->error,
						 wants_of(p->perms, p->hosts, p->scripts) });
		}
	}
	return out;
}

bool br_ext_preview( const std::string &uuid, BrExtInfo *out )
{
	std::unique_ptr<Ext> e = ext_load(uuid, nullptr);
	*out = BrExtInfo{ e->id, e->uuid, e->name, e->version, true, e->error, wants_of(e->perms, e->hosts, e->scripts) };
	return e->error.empty();
}

bool br_ext_enable( const std::string &id, bool on )
{
	std::lock_guard<std::mutex> hold(g_lock);
	Ext *e = ext_by_id(id);
	if ( e == nullptr ) return false;
	e->enabled = on;
	e->enabled_mine = true;
	state_save();
	return true;
}

bool br_ext_add( const std::string &uuid, std::string *why )
{
	std::unique_ptr<Ext> e = ext_load(uuid, nullptr);
	if ( !e->error.empty() ) {
		*why = e->error;
		return false;
	}
	std::lock_guard<std::mutex> hold(g_lock);
	if ( ext_by_id(e->id) != nullptr ) return true;
	if ( !br_ext_os_link(uuid, e->name) ) {
		*why = "拡張機能の一覧の実身に書けません";
		return false;
	}
	g_exts.push_back(std::move(e));
	g_any = true;
	state_save();
	return true;
}

bool br_ext_remove( const std::string &id )
{
	std::lock_guard<std::mutex> hold(g_lock);
	for ( size_t i = 0; i < g_exts.size(); i++ ) {
		if ( g_exts[i]->id != id || g_exts[i]->removed ) continue;
		bool ok = br_ext_os_unlink(g_exts[i]->uuid);
		if ( g_exts[i]->web_on ) g_web_listeners--;
		g_exts[i]->web_on = false;
		g_exts[i]->removed = true;
		g_exts[i]->enabled = false;
		state_save();
		return ok;
	}
	return false;
}

/* The extensions with a button, in the order of the menu's rows */
static std::vector<Ext *> action_exts()
{
	std::vector<Ext *> out;
	for ( auto &p : g_exts ) {
		Ext &e = *p;
		if ( e.removed || !e.enabled || !e.error.empty() || e.manifest.FindDict("action") == nullptr || !e.act_enabled ) continue;
		out.push_back(&e);
	}
	return out;
}

std::vector<std::string> br_ext_action_rows()
{
	std::lock_guard<std::mutex> hold(g_lock);
	std::vector<std::string> out;
	for ( Ext *e : action_exts() ) out.push_back(e->act_badge.empty() ? e->act_title : e->act_title + " (" + e->act_badge + ")");
	return out;
}

/* The tab, to one its user just asked to act on it: what activeTab grants */
static void grant_active( Ext &e )
{
	if ( e.perms.count("activeTab") && GURL(g_tab_url).is_valid() ) e.active_origin = url::Origin::Create(GURL(g_tab_url)).Serialize();
}

void br_ext_action_click( int row )
{
	std::lock_guard<std::mutex> hold(g_lock);
	std::vector<Ext *> rows = action_exts();
	if ( row < 0 || row >= (int)rows.size() ) return;
	Ext *e = rows[(size_t)row];
	grant_active(*e);
	if ( !e->act_popup.empty() ) {
		br_ext_command("extpage " + base_url(*e) + e->act_popup);
		return;
	}
	base::ListValue args;
	args.Append(tab_of(*e));
	tell_event(*e, "action", std::move(args));
}

static bool menu_applies( const Ext &e, const MenuItem &m, const BrExtMenuAt &at )
{
	if ( !m.visible || m.type == "separator" ) return false;
	bool hit = m.contexts.count("all") > 0;
	hit = hit || ( !at.link.empty() && m.contexts.count("link") );
	hit = hit || ( !at.image.empty() && m.contexts.count("image") );
	hit = hit || ( !at.selection.empty() && m.contexts.count("selection") );
	hit = hit || ( at.editable && m.contexts.count("editable") );
	hit = hit || ( at.link.empty() && at.image.empty() && at.selection.empty() && !at.editable && m.contexts.count("page") );
	if ( !hit ) return false;
	if ( !m.doc_patterns.empty() ) {
		GURL u(at.page);
		bool any = false;
		for ( const std::string &p : m.doc_patterns ) any = any || pattern_hits(p, u);
		if ( !any ) return false;
	}
	if ( !m.link_patterns.empty() ) {
		GURL u(at.link.empty() ? at.image : at.link);
		bool any = false;
		for ( const std::string &p : m.link_patterns ) any = any || pattern_hits(p, u);
		if ( !any ) return false;
	}
	(void)e;
	return true;
}

/* What the menu shows for what is under the pointer: its words and whose it is */
static std::vector<std::pair<Ext *, const MenuItem *>> menu_items_at( const BrExtMenuAt &at )
{
	std::vector<std::pair<Ext *, const MenuItem *>> out;
	for ( auto &p : g_exts ) {
		Ext &e = *p;
		if ( e.removed || !e.enabled || !e.error.empty() ) continue;
		for ( const MenuItem &m : e.menu ) if ( menu_applies(e, m, at) ) out.emplace_back(&e, &m);
	}
	return out;
}

std::vector<std::string> br_ext_menu_rows( const BrExtMenuAt &at )
{
	std::lock_guard<std::mutex> hold(g_lock);
	std::vector<std::string> out;
	for ( auto &x : menu_items_at(at) ) {
		const MenuItem &m = *x.second;
		/* a submenu is shown flat: its parent's words in front */
		std::string label = m.title;
		if ( !m.parent.empty() ) {
			if ( MenuItem *par = menu_find(*x.first, m.parent) ) label = par->title + " > " + label;
		}
		if ( m.type == "checkbox" || m.type == "radio" ) label = ( m.checked ? "[x] " : "[ ] " ) + label;
		size_t sel = label.find("%s");
		if ( sel != std::string::npos ) label.replace(sel, 2, at.selection);
		out.push_back(label);
	}
	return out;
}

void br_ext_menu_click( int row, const BrExtMenuAt &at )
{
	std::lock_guard<std::mutex> hold(g_lock);
	auto rows = menu_items_at(at);
	if ( row < 0 || row >= (int)rows.size() ) return;
	Ext *e = rows[(size_t)row].first;
	MenuItem *m = menu_find(*e, rows[(size_t)row].second->id);
	if ( m == nullptr ) return;
	grant_active(*e);
	base::DictValue info;
	info.Set("menuItemId", m->id);
	if ( !m->parent.empty() ) info.Set("parentMenuItemId", m->parent);
	info.Set("pageUrl", at.page);
	info.Set("frameId", 0);
	info.Set("editable", at.editable);
	if ( !at.link.empty() ) info.Set("linkUrl", at.link);
	if ( !at.image.empty() ) { info.Set("srcUrl", at.image); info.Set("mediaType", "image"); }
	if ( !at.selection.empty() ) info.Set("selectionText", at.selection);
	if ( m->type == "checkbox" ) {
		info.Set("wasChecked", m->checked);
		m->checked = !m->checked;
		info.Set("checked", m->checked);
	}
	base::ListValue args;
	args.Append(std::move(info));
	args.Append(tab_of(*e));
	tell_event(*e, "menu", std::move(args));
}

bool br_ext_perm_words( const std::string &id, int msg, std::string *name, std::vector<std::string> *wants )
{
	std::lock_guard<std::mutex> hold(g_lock);
	Ext *e = ext_by_id(id);
	if ( e == nullptr || !e->asking.count(msg) ) return false;
	const base::DictValue &d = e->asking[msg];
	std::vector<std::string> ps = strings_of(d, "permissions");
	*name = e->name;
	*wants = wants_of(std::set<std::string>(ps.begin(), ps.end()), strings_of(d, "origins"), std::vector<ContentScript>());
	return true;
}

void br_ext_perm_answer( const std::string &id, int msg, bool yes )
{
	std::lock_guard<std::mutex> hold(g_lock);
	Ext *e = ext_by_id(id);
	if ( e == nullptr ) return;
	auto it = e->asking.find(msg);
	if ( it == e->asking.end() ) return;
	if ( yes ) {
		for ( const std::string &p : strings_of(it->second, "permissions") ) e->perms.insert(p);
		for ( const std::string &o : strings_of(it->second, "origins") ) e->hosts.push_back(o);
		base::ListValue args;
		args.Append(it->second.Clone());
		tell_event(*e, "permAdded", std::move(args));
		state_save();
	}
	e->asking.erase(it);
	reply_given(*e, msg, yes ? "true" : "false");
}

/*
 * For br_ext_frames.cc, which has the frames and the worlds: the content
 * scripts of the frame at a moment, each with the world's number of its
 * extension, in the manifest's order.
 */
std::vector<BrExtRun> br_ext_scripts_for( const std::string &url, bool main_frame, int when )
{
	std::lock_guard<std::mutex> hold(g_lock);
	std::vector<BrExtRun> out;
	GURL u(url);
	for ( size_t i = 0; i < g_exts.size(); i++ ) {
		Ext &e = *g_exts[i];
		if ( !e.enabled || !e.error.empty() ) continue;
		for ( const ContentScript &s : e.scripts ) {
			if ( s.when != when || ( !main_frame && !s.all_frames ) ) continue;
			bool hit = false;
			for ( const std::string &m : s.matches ) hit = hit || pattern_hits(m, u);
			for ( const std::string &m : s.exclude ) hit = hit && !pattern_hits(m, u);
			if ( !hit ) continue;
			BrExtRun r;
			r.world = 1000 + (int)i;
			r.main_world = s.main_world;
			r.id = e.id;
			r.name = e.name;
			for ( const std::string &f : s.js ) {
				auto it = e.files.find(f);
				if ( it != e.files.end() ) r.code += it->second + "\n";
			}
			for ( const std::string &f : s.css ) {
				auto it = e.files.find(f);
				if ( it != e.files.end() ) r.css += it->second + "\n";
			}
			out.push_back(std::move(r));
		}
	}
	return out;
}

/* The API's one call from a content script's world or an extension's page */
std::string br_ext_api_world( int world, const std::string &op, const std::string &arg, bool page )
{
	std::lock_guard<std::mutex> hold(g_lock);
	size_t i = (size_t)( world - 1000 );
	if ( i >= g_exts.size() ) return "null";
	return api_call(*g_exts[i], op, arg, page ? "page" : "content");
}

/*
 * The same, for a frame's native function that can answer later: what
 * waits for an event (msg.poll with nothing new, msg.answer not yet
 * answered) is answered by 'later' when the event comes or after a while,
 * from another thread, with the extensions' lock held ('later' only
 * posts); *deferred then says so, and the answer here is nothing
 */
std::string br_ext_api_world_later( int world, const std::string &op, const std::string &arg, bool page,
				    std::function<void( BrExtAns )> later, bool *deferred )
{
	std::lock_guard<std::mutex> hold(g_lock);
	*deferred = false;
	size_t i = (size_t)( world - 1000 );
	if ( i >= g_exts.size() ) return "null";
	g_later = &later;
	g_deferred = false;
	std::string out = api_call(*g_exts[i], op, arg, page ? "page" : "content");
	g_later = nullptr;
	*deferred = g_deferred;
	return out;
}

std::string br_ext_manifest_of_world( int world )
{
	std::lock_guard<std::mutex> hold(g_lock);
	size_t i = (size_t)( world - 1000 );
	return i < g_exts.size() ? manifest_text(*g_exts[i]) : std::string("{}");
}

/* What the shim is made with in a frame: its frame's number goes in */
std::string br_ext_info_of_world( int world, long long frame )
{
	std::lock_guard<std::mutex> hold(g_lock);
	size_t i = (size_t)( world - 1000 );
	if ( i >= g_exts.size() ) return "{}";
	std::string s = info_of(*g_exts[i]);
	s.insert(s.size() - 1, ",\"frameId\":" + std::to_string(frame));
	return s;
}

std::string br_ext_id_of_world( int world )
{
	std::lock_guard<std::mutex> hold(g_lock);
	size_t i = (size_t)( world - 1000 );
	return i < g_exts.size() ? g_exts[i]->id : std::string();
}

/* The world of the extension whose page this address is, 0 none */
int br_ext_world_of_page( const std::string &url )
{
	GURL u(url);
	if ( !u.SchemeIs("chrome-extension") ) return 0;
	std::lock_guard<std::mutex> hold(g_lock);
	for ( size_t i = 0; i < g_exts.size(); i++ ) {
		if ( g_exts[i]->id == std::string(u.host()) && !g_exts[i]->removed && g_exts[i]->enabled ) return 1000 + (int)i;
	}
	return 0;
}

void br_ext_inject_done( int world, int msg, const std::string &json )
{
	std::lock_guard<std::mutex> hold(g_lock);
	size_t i = (size_t)( world - 1000 );
	/* the worker's executeScript may already wait for it */
	if ( i < g_exts.size() ) reply_given(*g_exts[i], msg, json);
}

/* The workers to start: their scopes, scripts and whether each is a module */
std::vector<BrExtWorker> br_ext_workers()
{
	std::lock_guard<std::mutex> hold(g_lock);
	std::vector<BrExtWorker> out;
	for ( auto &p : g_exts ) {
		if ( p->enabled && p->error.empty() && !p->worker.empty() ) {
			out.push_back(BrExtWorker{ base_url(*p), base_url(*p) + p->worker, p->worker_module });
		}
	}
	return out;
}

static std::vector<std::string> g_add_later;

void br_ext_add_later( const std::string &uuid )
{
	g_add_later.push_back(uuid);
}

/* Read from their object; the workers are started by the caller (br_ext_frames.cc) */
void br_ext_read_all()
{
	std::string text;
	std::optional<base::Value> st;
	if ( br_ext_os_state_read(&text) && !text.empty() ) st = base::JSONReader::Read(text, base::JSON_PARSE_RFC);
	const base::DictValue *state = st && st->is_dict() ? &st->GetDict() : nullptr;
	for ( const std::string &uuid : br_ext_os_links() ) {
		std::unique_ptr<Ext> e = ext_load(uuid, state);
		std::lock_guard<std::mutex> hold(g_lock);
		if ( ext_by_id(e->id) == nullptr ) g_exts.push_back(std::move(e));
		g_any = true;
	}
	/* after them: what the lines of a test add, which writes the state of all it knows */
	for ( const std::string &uuid : g_add_later ) {
		std::string why;
		if ( !br_ext_add(uuid, &why) ) printf("browser: extension %s: not taken: %s\n", uuid.c_str(), why.c_str());
	}
	g_add_later.clear();
}
