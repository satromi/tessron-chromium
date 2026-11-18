/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_data.cc
 *	ブラウザ: the pages visited, what was downloaded, the addresses and
 *	cards kept, and the clearing of what a site or the browsing left
 *	(design 14.7.10).
 *
 *	History, downloads and the sites' times are stores kept by entries,
 *	so that browsers of windows opened apart, each its own process, add
 *	to the same log and only what changes is written. A browser does not
 *	see another's memory: a clearing is also written as a notice in the
 *	"sites" store, which the other browsers look for and carry out too,
 *	or their next whole save would bring back what was cleared.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/values.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "third_party/boringssl/src/include/openssl/mem.h"
#include "third_party/boringssl/src/include/openssl/rand.h"
#include "url/gurl.h"

#include "br_data.h"
#include "br_store.h"

long long br_now_us()
{
	struct timespec t;
	clock_gettime(CLOCK_REALTIME, &t);
	return (long long)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

/* ---------------------------------------------------------------- a store of entries */

/* br_main.cc: whether the pages are run by renderers, and whether this process is one */
extern bool	br_multi_process, br_is_renderer;

namespace {

/* The entries in memory, and what has changed since the store last asked */
struct Keyed {
	const char			*name;
	int				id = -1;
	std::mutex			lock;
	std::map<std::string, std::string> rows;
	std::map<std::string, std::string> put;
	std::set<std::string>		del;

	explicit Keyed( const char *n ) : name(n) {}

	static void Collect( void *ctx )
	{
		Keyed *k = static_cast<Keyed *>(ctx);
		br_store_delta *d = br_store_delta_new();
		{
			std::lock_guard<std::mutex> hold(k->lock);
			for ( const auto &kv : k->put ) {
				br_store_delta_put(d, kv.first.data(), (int)kv.first.size(), kv.second.data(), (int)kv.second.size());
			}
			for ( const std::string &key : k->del ) br_store_delta_del(d, key.data(), (int)key.size());
			k->put.clear();
			k->del.clear();
		}
		br_store_answer(k->id, d);
	}

	void start()
	{
		id = br_store_register_keyed(name, &Keyed::Collect, this, 2000);
		reload();
	}

	/* What other browsers have added read in; what this one has not yet written still counts */
	void reload()
	{
		std::map<std::string, std::string> now;
		(void)br_store_load_entries(name, []( void *ctx, const char *k, int kl, const unsigned char *v, int vl ) {
			if ( kl > 0 ) ( *static_cast<std::map<std::string, std::string> *>(ctx) )[std::string(k, (size_t)kl)]
				.assign((const char *)v, (size_t)vl);
		}, &now);
		std::lock_guard<std::mutex> hold(lock);
		for ( const auto &kv : put ) now[kv.first] = kv.second;
		for ( const std::string &key : del ) now.erase(key);
		rows.swap(now);
	}

	void set( const std::string &key, const std::string &val )
	{
		{
			std::lock_guard<std::mutex> hold(lock);
			rows[key] = val;
			put[key] = val;
			del.erase(key);
		}
		br_store_dirty(id);
	}

	void erase( const std::string &key )
	{
		{
			std::lock_guard<std::mutex> hold(lock);
			rows.erase(key);
			put.erase(key);
			del.insert(key);
		}
		br_store_dirty(id);
	}

	std::map<std::string, std::string> all()
	{
		std::lock_guard<std::mutex> hold(lock);
		return rows;
	}
};

Keyed g_history("history"), g_downloads("downloads"), g_sites("sites");

constexpr size_t kHistoryMax = 5000;
const char kNotice[] = "\001n";		/* the keys of the notices in "sites" */

/*
 * The browser's process with renderers of its own has no engine (no
 * network, no pages' storage, no service thread): what those keep is
 * cleared, listed and reloaded by the renderers (br_proc.cc, design
 * 14.13). Its own here are the history and the list of downloads.
 */
bool engine_here()
{
	return !br_multi_process || br_is_renderer;
}

void put_i64( std::string &o, long long v )
{
	o.append((const char *)&v, 8);
}

long long get_i64( const std::string &s, size_t at )
{
	long long v = 0;
	if ( s.size() >= at + 8 ) memcpy(&v, s.data() + at, 8);
	return v;
}

/* ---------------------------------------------------------------- which site */

std::string site_of_host( const std::string &host_in )
{
	std::string host = host_in;
	while ( !host.empty() && host[0] == '.' ) host.erase(0, 1);
	std::string d = net::registry_controlled_domains::GetDomainAndRegistry(
		host, net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);
	return d.empty() ? host : d;
}

std::string host_of( const std::string &origin )
{
	GURL g(origin);
	return g.is_valid() ? std::string(g.host()) : origin;
}

}  // namespace

std::string br_site_of( const std::string &origin_or_host )
{
	return site_of_host(origin_or_host.find("://") != std::string::npos ? host_of(origin_or_host) : origin_or_host);
}

bool br_clear_host_hit( const BrClear &c, const std::string &host )
{
	return c.site.empty() || site_of_host(host) == c.site;
}

/*
 * Whether what an origin keeps is taken by a clearing. Storage does not
 * say when each thing in it was written: the time looked at is when the
 * origin last used it, so that a period takes all of what was used
 * during it, older parts too, and nothing of an origin left alone.
 */
bool br_clear_origin_hit( const BrClear &c, const std::string &origin )
{
	if ( !br_clear_host_hit(c, host_of(origin)) ) return false;
	if ( c.since_us == 0 ) return true;
	std::lock_guard<std::mutex> hold(g_sites.lock);
	auto it = g_sites.rows.find(origin);
	return it != g_sites.rows.end() && get_i64(it->second, 0) >= c.since_us;
}

void br_site_touch( const std::string &origin )
{
	if ( origin.empty() || origin == "null" ) return;
	long long now = br_now_us();
	{
		/* written when it is new, and then at most once an hour */
		std::lock_guard<std::mutex> hold(g_sites.lock);
		auto it = g_sites.rows.find(origin);
		if ( it != g_sites.rows.end() && now - get_i64(it->second, 0) < 3600LL * 1000000 ) return;
	}
	std::string v;
	put_i64(v, now);
	g_sites.set(origin, v);
}

/* ---------------------------------------------------------------- history */

void br_history_visit( const std::string &url, const std::string &title )
{
	if ( url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0 ) return;
	int count = 0;
	{
		std::lock_guard<std::mutex> hold(g_history.lock);
		auto it = g_history.rows.find(url);
		if ( it != g_history.rows.end() && it->second.size() >= 12 ) memcpy(&count, it->second.data() + 8, 4);
	}
	count++;
	std::string v;
	put_i64(v, br_now_us());
	v.append((const char *)&count, 4);
	v += title;
	g_history.set(url, v);
	bool over;
	{
		std::lock_guard<std::mutex> hold(g_history.lock);
		over = g_history.rows.size() > kHistoryMax;
	}
	if ( over ) {
		/* the page not visited for the longest goes */
		std::string oldest;
		long long when = 0;
		for ( const auto &kv : g_history.all() ) {
			long long w = get_i64(kv.second, 0);
			if ( oldest.empty() || w < when ) {
				oldest = kv.first;
				when = w;
			}
		}
		g_history.erase(oldest);
	}
}

void br_history_title( const std::string &url, const std::string &title )
{
	std::string v;
	{
		std::lock_guard<std::mutex> hold(g_history.lock);
		auto it = g_history.rows.find(url);
		if ( it == g_history.rows.end() || it->second.size() < 12 || it->second.compare(12, std::string::npos, title) == 0 ) return;
		v = it->second.substr(0, 12) + title;
	}
	g_history.set(url, v);
}

std::vector<BrHistoryRow> br_history_list()
{
	g_history.reload();
	std::vector<BrHistoryRow> out;
	for ( const auto &kv : g_history.all() ) {
		if ( kv.second.size() < 12 ) continue;
		int count = 0;
		memcpy(&count, kv.second.data() + 8, 4);
		out.push_back({ kv.first, kv.second.substr(12), get_i64(kv.second, 0), count });
	}
	std::sort(out.begin(), out.end(), []( const BrHistoryRow &a, const BrHistoryRow &b ) { return a.when_us > b.when_us; });
	return out;
}

void br_history_remove( const std::string &url )
{
	g_history.erase(url);
}

/*
 * The address the letters typed may be going to: of the pages whose
 * address, with or without the scheme and "www.", starts with them, the
 * one most visited, the last visited among equals; "" when none.
 * 'skip' passes over that many, for going through them with the arrows.
 */
std::string br_history_complete( const std::string &typed, int skip )
{
	struct Found {
		std::string	url;
		int		count;
		long long	when;
	};
	std::vector<Found> found;
	if ( typed.empty() ) return std::string();
	for ( const auto &kv : g_history.all() ) {
		if ( kv.second.size() < 12 ) continue;
		const std::string &u = kv.first;
		size_t at = 0;
		bool hit = u.compare(0, typed.size(), typed) == 0;
		if ( !hit ) {
			size_t p = u.find("://");
			at = ( p == std::string::npos ) ? 0 : p + 3;
			hit = u.compare(at, typed.size(), typed) == 0;
			if ( !hit && u.compare(at, 4, "www.") == 0 ) hit = u.compare(at + 4, typed.size(), typed) == 0;
		}
		if ( !hit ) continue;
		int count = 0;
		memcpy(&count, kv.second.data() + 8, 4);
		found.push_back({ u, count, get_i64(kv.second, 0) });
	}
	std::sort(found.begin(), found.end(), []( const Found &a, const Found &b ) {
		return a.count != b.count ? a.count > b.count : a.when > b.when;
	});
	return skip >= 0 && (size_t)skip < found.size() ? found[(size_t)skip].url : std::string();
}

/* ---------------------------------------------------------------- downloads */

namespace {

/* What is going now, in this browser: the settings window reads it, the downloading thread writes it */
std::mutex			g_dl_lock;
std::map<int, BrDownloadRow>	g_dl_now;
std::map<int, bool>		g_dl_stop;
int				g_dl_next = 1;

std::string dl_bytes( const BrDownloadRow &r )
{
	base::DictValue d;
	d.Set("name", r.name);
	d.Set("url", r.url);
	d.Set("when", (double)r.when_us);
	d.Set("total", (double)r.total);
	d.Set("got", (double)r.got);
	d.Set("state", r.state);
	std::string o;
	(void)base::JSONWriter::Write(d, &o);
	return o;
}

void dl_keep( int id, const BrDownloadRow &r )
{
	char key[40];
	snprintf(key, sizeof(key), "%020lld.%d.%d", r.when_us, (int)getpid(), id);
	g_downloads.set(key, dl_bytes(r));
}

}  // namespace

int br_download_start( const std::string &name, const std::string &url )
{
	std::lock_guard<std::mutex> hold(g_dl_lock);
	int id = g_dl_next++;
	BrDownloadRow r;
	r.key = "now:" + std::to_string(id);
	r.name = name;
	r.url = url;
	r.when_us = br_now_us();
	r.state = BR_DL_GOING;
	g_dl_now[id] = r;
	g_dl_stop[id] = false;
	return id;
}

bool br_download_progress( int id, long long got, long long total )
{
	std::lock_guard<std::mutex> hold(g_dl_lock);
	auto it = g_dl_now.find(id);
	if ( it == g_dl_now.end() ) return false;
	it->second.got = got;
	if ( total > 0 ) it->second.total = total;
	return !g_dl_stop[id];
}

void br_download_name( int id, const std::string &name )
{
	std::lock_guard<std::mutex> hold(g_dl_lock);
	auto it = g_dl_now.find(id);
	if ( it != g_dl_now.end() ) it->second.name = name;
}

void br_download_end( int id, int state )
{
	BrDownloadRow r;
	{
		std::lock_guard<std::mutex> hold(g_dl_lock);
		auto it = g_dl_now.find(id);
		if ( it == g_dl_now.end() ) return;
		r = it->second;
		g_dl_now.erase(it);
		g_dl_stop.erase(id);
	}
	r.state = state;
	dl_keep(id, r);
}

std::vector<BrDownloadRow> br_downloads_list()
{
	std::vector<BrDownloadRow> out;
	{
		std::lock_guard<std::mutex> hold(g_dl_lock);
		for ( const auto &kv : g_dl_now ) out.push_back(kv.second);
	}
	g_downloads.reload();
	auto kept = g_downloads.all();
	for ( auto it = kept.rbegin(); it != kept.rend(); ++it ) {
		std::optional<base::Value> v = base::JSONReader::Read(it->second, base::JSON_PARSE_RFC);
		if ( !v || !v->is_dict() ) continue;
		const base::DictValue &d = v->GetDict();
		BrDownloadRow r;
		r.key = it->first;
		const std::string *n = d.FindString("name"), *u = d.FindString("url");
		r.name = n != nullptr ? *n : std::string();
		r.url = u != nullptr ? *u : std::string();
		r.when_us = (long long)d.FindDouble("when").value_or(0);
		r.total = (long long)d.FindDouble("total").value_or(0);
		r.got = (long long)d.FindDouble("got").value_or(0);
		r.state = d.FindInt("state").value_or(BR_DL_FAILED);
		out.push_back(std::move(r));
	}
	return out;
}

bool br_download_cancel( const std::string &key )
{
	if ( key.compare(0, 4, "now:") != 0 ) return false;
	std::lock_guard<std::mutex> hold(g_dl_lock);
	auto it = g_dl_stop.find(atoi(key.c_str() + 4));
	if ( it == g_dl_stop.end() ) return false;
	it->second = true;
	return true;
}

void br_download_forget( const std::string &key )
{
	if ( key.compare(0, 4, "now:") != 0 ) g_downloads.erase(key);
}

/* ---------------------------------------------------------------- addresses and cards */

namespace {

const char *const kAddrKeys[BR_ADDR_FIELDS] = { "name", "org", "zip", "region", "city", "line1", "line2", "tel", "email" };
const char *const kCardKeys[BR_CARD_FIELDS] = { "name", "number", "month", "year" };

std::mutex g_af_lock;

template <typename Row>
void rows_from( const base::DictValue &all, const char *list, const char *const *keys, int n, std::vector<Row> *out )
{
	const base::ListValue *l = all.FindList(list);
	if ( l == nullptr ) return;
	for ( const base::Value &e : *l ) {
		if ( !e.is_dict() ) continue;
		Row r;
		for ( int i = 0; i < n; i++ ) {
			const std::string *s = e.GetDict().FindString(keys[i]);
			if ( s != nullptr ) r.f[i] = *s;
		}
		out->push_back(std::move(r));
	}
}

template <typename Row>
base::ListValue rows_to( const std::vector<Row> &rows, const char *const *keys, int n )
{
	base::ListValue l;
	for ( const Row &r : rows ) {
		base::DictValue d;
		for ( int i = 0; i < n; i++ ) d.Set(keys[i], r.f[i]);
		l.Append(std::move(d));
	}
	return l;
}

}  // namespace

/* The store is read each time and nothing of it stays in memory: the numbers are in the clear only while they are used */
bool br_autofill_load( std::vector<BrAddress> *addrs, std::vector<BrCard> *cards )
{
	std::lock_guard<std::mutex> hold(g_af_lock);
	unsigned char *data = nullptr;
	int len = 0;
	addrs->clear();
	cards->clear();
	int rc = br_store_load("autofill", &data, &len);
	if ( rc == -3 ) return true;		/* nothing kept yet */
	if ( rc != 0 || data == nullptr ) return false;
	std::string json((const char *)data, (size_t)len);
	OPENSSL_cleanse(data, (size_t)len);
	free(data);
	std::optional<base::Value> v = base::JSONReader::Read(json, base::JSON_PARSE_RFC);
	OPENSSL_cleanse(&json[0], json.size());
	if ( !v || !v->is_dict() ) return false;
	rows_from(v->GetDict(), "addresses", kAddrKeys, BR_ADDR_FIELDS, addrs);
	rows_from(v->GetDict(), "cards", kCardKeys, BR_CARD_FIELDS, cards);
	return true;
}

bool br_autofill_save( const std::vector<BrAddress> &addrs, const std::vector<BrCard> &cards )
{
	std::lock_guard<std::mutex> hold(g_af_lock);
	base::DictValue all;
	all.Set("addresses", rows_to(addrs, kAddrKeys, BR_ADDR_FIELDS));
	all.Set("cards", rows_to(cards, kCardKeys, BR_CARD_FIELDS));
	std::string json;
	(void)base::JSONWriter::Write(all, &json);
	bool ok = br_store_save("autofill", json.data(), (int)json.size()) == 0;
	OPENSSL_cleanse(&json[0], json.size());
	return ok;
}

int br_choose( const std::vector<std::string> &names, int chosen );	/* br_main.cc, or the browser's panel from a renderer */

bool br_autofill_choose( bool card, const std::string &origin, std::vector<std::string> *fields )
{
	fields->clear();
	if ( br_is_renderer ) return br_rproc_autofill_choose(card, origin, fields);
	std::vector<BrAddress> addrs;
	std::vector<BrCard> cards;
	if ( !br_autofill_load(&addrs, &cards) ) return false;
	std::vector<std::string> names;
	if ( card ) {
		for ( const BrCard &c : cards ) {
			const std::string &num = c.f[BR_CARD_NUMBER];
			names.push_back("****" + ( num.size() > 4 ? num.substr(num.size() - 4) : num ) + " " + c.f[BR_CARD_NAME] + " "
					+ c.f[BR_CARD_MONTH] + "/" + c.f[BR_CARD_YEAR]);
		}
	} else {
		for ( const BrAddress &a : addrs ) names.push_back(a.f[BR_ADDR_NAME] + " " + a.f[BR_ADDR_CITY] + a.f[BR_ADDR_LINE1]);
	}
	int k = names.empty() ? -1 : br_choose(names, 0);
	bool ok = k >= 0 && k < (int)names.size();
	if ( ok && card ) fields->assign(cards[(size_t)k].f, cards[(size_t)k].f + BR_CARD_FIELDS);
	if ( ok && !card ) fields->assign(addrs[(size_t)k].f, addrs[(size_t)k].f + BR_ADDR_FIELDS);
	for ( BrCard &c : cards ) OPENSSL_cleanse(&c.f[BR_CARD_NUMBER][0], c.f[BR_CARD_NUMBER].size());
	if ( ok ) printf("browser: %s chosen on the panel for a form of %s\n", card ? "a card" : "an address", origin.c_str());
	return ok;
}

/* ---------------------------------------------------------------- clearing */

namespace {

/* This browser's name in the notices, not to carry out its own */
unsigned g_me;
/*
 * When this process began (us): a notice made before it is in what the
 * process read from the disk, and is not carried out again (a clearing
 * carried out late would take what was kept after it)
 */
long long g_began_us;
std::mutex g_seen_lock;		/* the notice thread and whichever thread clears */
std::set<std::string> g_seen;

/* True the first time a notice's key is met */
bool seen_first( const std::string &key )
{
	std::lock_guard<std::mutex> hold(g_seen_lock);
	return g_seen.insert(key).second;
}

void clear_here( const BrClear &c )
{
	if ( engine_here() ) {
		br_net_data_clear(c);
		br_page_data_clear(c);
		br_service_data_clear(c);
	}
	if ( ( c.kinds & BR_CLEAR_HISTORY ) != 0 ) {
		g_history.reload();
		for ( const auto &kv : g_history.all() ) {
			if ( !br_clear_host_hit(c, host_of(kv.first)) ) continue;
			if ( c.since_us != 0 && get_i64(kv.second, 0) < c.since_us ) continue;
			g_history.erase(kv.first);
		}
	}
	if ( ( c.kinds & BR_CLEAR_DOWNLOADS ) != 0 ) {
		for ( const BrDownloadRow &r : br_downloads_list() ) {
			if ( r.state == BR_DL_GOING || !br_clear_host_hit(c, host_of(r.url)) ) continue;
			if ( c.since_us != 0 && r.when_us < c.since_us ) continue;
			g_downloads.erase(r.key);
		}
	}
}

/* Notices of the other browsers carried out; every three seconds, on a thread that may wait for the disk */
void *notice_thread( void * )
{
	for ( ;; ) {
		sleep(3);
		g_sites.reload();
		for ( const auto &kv : g_sites.all() ) {
			if ( kv.first.compare(0, 2, kNotice) != 0 || !seen_first(kv.first) ) continue;
			if ( atoll(kv.first.c_str() + 2) < g_began_us ) continue;
			std::optional<base::Value> v = base::JSONReader::Read(kv.second, base::JSON_PARSE_RFC);
			if ( !v || !v->is_dict() ) continue;
			const base::DictValue &d = v->GetDict();
			if ( (unsigned)d.FindDouble("from").value_or(0) == g_me ) continue;
			int kinds = d.FindInt("kinds").value_or(0);
			/* the passwords changed (an older browser's notice): each operation reads them anew (br_cred_core.cc) */
			BrClear c;
			c.kinds = (unsigned)kinds & ~(unsigned)BR_RELOAD_CREDS;
			c.since_us = (long long)d.FindDouble("since").value_or(0);
			const std::string *s = d.FindString("site");
			c.site = s != nullptr ? *s : std::string();
			printf("browser: another browser's clearing carried out (%#x)\n", c.kinds);
			if ( c.kinds != 0 ) clear_here(c);
		}
	}
	return nullptr;
}

}  // namespace

void br_data_notice( unsigned kinds, long long since_us, const std::string &site )
{
	long long now = br_now_us();
	base::DictValue d;
	d.Set("from", (double)g_me);
	d.Set("kinds", (int)kinds);
	d.Set("since", (double)since_us);
	d.Set("site", site);
	std::string o;
	(void)base::JSONWriter::Write(d, &o);
	char key[40];
	snprintf(key, sizeof(key), "%s%020lld", kNotice, now);
	(void)seen_first(key);
	g_sites.set(key, o);
	/* a browser that starts later reads what is left: a day's notices are enough for those running */
	for ( const auto &kv : g_sites.all() ) {
		if ( kv.first.compare(0, 2, kNotice) == 0 && atoll(kv.first.c_str() + 2) < now - 86400LL * 1000000 ) g_sites.erase(kv.first);
	}
}

void br_data_clear( const BrClear &c )
{
	clear_here(c);
	br_data_notice(c.kinds, c.since_us, c.site);
	if ( c.site.empty() && c.since_us == 0 ) {
		/* the sites' times of what is gone */
		for ( const auto &kv : g_sites.all() ) {
			if ( kv.first.compare(0, 2, kNotice) != 0 && ( c.kinds & BR_CLEAR_ALL_SITE_DATA ) == BR_CLEAR_ALL_SITE_DATA ) g_sites.erase(kv.first);
		}
	}
	br_main_command("flushstores");
}

std::vector<std::string> br_data_sites()
{
	std::set<std::string> found;
	for ( const auto &kv : g_sites.all() ) {
		if ( kv.first.compare(0, 2, kNotice) != 0 ) found.insert(br_site_of(kv.first));
	}
	for ( const auto &kv : g_history.all() ) found.insert(br_site_of(kv.first));
	if ( engine_here() ) {
		br_net_data_sites(&found);
		br_page_data_sites(&found);
		br_service_data_sites(&found);
	}
	found.erase(std::string());
	return std::vector<std::string>(found.begin(), found.end());
}

void br_data_init()
{
	static bool done;
	if ( done ) return;
	done = true;
	(void)RAND_bytes((uint8_t *)&g_me, sizeof(g_me));
	g_began_us = br_now_us();
	g_history.start();
	g_downloads.start();
	g_sites.start();
	pthread_t	th;
	pthread_attr_t	at;
	pthread_attr_init(&at);
	pthread_attr_setstacksize(&at, 256 * 1024);
	pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	(void)pthread_create(&th, &at, notice_thread, nullptr);
	pthread_attr_destroy(&at);
}
