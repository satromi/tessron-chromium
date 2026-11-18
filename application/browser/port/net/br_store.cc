/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_store.cc
 *	ブラウザ: what the engine keeps between runs, in real objects
 *	(design 14.8).
 *
 *	The cookies, the pages' local storage and the HTTP cache each live
 *	in records of an object of the system that the learning box links.
 *	IndexedDB is record 2 of the local storage's object; what is known
 *	of servers (which speak HTTP/3, and the sessions to resume QUIC with)
 *	is record 2 of the cache's. The objects have fixed identities
 *	(SYSDEF_BROWSER_*) and are made with the system (etc/def).
 *
 *	A store is a set of entries, each a key and its bytes; a store kept
 *	whole is the one entry of the empty key. Its record is a log
 *	("BRS2"): a head of 32 bytes -- the magic, a generation, the length
 *	of the log that counts, and the head's CRC-32 -- then the entries
 *	put and deleted, each with its own head (what it is, the
 *	generation, the lengths, a CRC-32 of it all). A save of the changes
 *	of a store appends them and then writes the head with the new
 *	length: power lost before the head is written leaves the log as it
 *	was. When the log has grown to more than twice what its entries
 *	hold, and when a store kept whole is saved, the entries are written
 *	afresh into the store's other record (its number plus 16) under the
 *	next generation, its head last, and the first record is emptied;
 *	reading takes the record of the higher generation whose head is
 *	whole. Power lost at any point leaves one of the two as the last
 *	save left it.
 *
 *	A record an older browser wrote ("BRS1": the magic, the length and
 *	the CRC-32 of the bytes, then the bytes) is read as the entry of the
 *	empty key, and the next save writes the new form beside it.
 *
 *	The passwords and passkeys ("creds") are sealed: what the record
 *	holds is encrypted with AES-256-GCM under a key of the user's own,
 *	kept in an object only that user can read (see "sealed stores").
 *	They, the Widevine CDM's object of each user and the CDM's files are
 *	kept in the older form, written in place.
 *
 *	What a site leaves and what the browser learns of the user's browsing
 *	(the stores with a record in kObjects' 'user') belongs to the user the
 *	browser acts as (ob_get_crd): it is kept in an object of that user's,
 *	"ブラウザの利用者データ", owned by the user with the mode rw-------,
 *	found through record 13 of the local storage's object and held by a
 *	fixed count (see "the user's object"). What an older browser left in
 *	the records every user shared is moved there by the first user whose
 *	browser reads it.
 *
 *	The writing is done by a thread of the store's own: the engine's
 *	thread, and whichever thread a store's entries live on, only make
 *	the bytes of a save and hand them over (see "registered stores").
 *	A store several browsers keep at once is saved a changed entry at a time
 *	under the writers' exclusion, and each browser looks for what the others
 *	wrote: the cookies through br_store_watch, the others through
 *	br_store_register_shared. All the reading and writing
 *	of the objects in this process goes through one lock; an object another process has open to write is
 *	tried again for a while.
 */

#include TS_CONFIG_H
#include <stdint.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/uapp.h>
#include <ts/sysdef.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "third_party/boringssl/src/include/openssl/aead.h"
#include "third_party/boringssl/src/include/openssl/mem.h"
#include "third_party/boringssl/src/include/openssl/rand.h"

#include "br_store.h"
#include "br_store_core.h"

namespace {

/*
 * Bytes in one call of ob_wri_rec and ob_rea_rec. A write holds the
 * volume and the device until it is on the medium, and the engine's own
 * reads of the same volume wait for it: 256KB is about 85 ms on the
 * Raspberry Pi 5's card, where 1280KB would be 0.4 s (design 7.3).
 */
constexpr SZ kChunk = 256 * 1024;
constexpr int kHdr = 12;		/* the older record's head */
constexpr int kHead = 32;		/* the log's head */
constexpr int kSegHead = 20;		/* an entry's head in the log */
constexpr int kAlt = 16;		/* a store's other record: its number plus this */
constexpr unsigned char kPut = 1, kDel = 2, kKeep = 3;	/* kKeep: in a snapshot only, never written */

struct Named {
	const char	*name;
	const char	*uuid;
	int		rec;		/* the record the bytes are in; for a store by user, where an older browser left them */
	int		user;		/* the record in the user's own object, 0: the store is not by user */
};

const Named kObjects[] = {
	{ "cookie", SYSDEF_BROWSER_COOKIE, 1, 1 },
	{ "storage", SYSDEF_BROWSER_STORAGE, 1, 2 },
	{ "cache", SYSDEF_BROWSER_CACHE, 1, 3 },
	{ "indexeddb", SYSDEF_BROWSER_STORAGE, 2, 4 },	/* IndexedDB, beside localStorage */
	{ "quic", SYSDEF_BROWSER_CACHE, 2, 5 },		/* what is known of servers, beside the HTTP cache */
	{ "push", SYSDEF_BROWSER_STORAGE, 3, 6 },		/* Web Push's service and subscriptions (push_tessronos.cc) */
	{ "sw", SYSDEF_BROWSER_STORAGE, 4, 7 },		/* the service workers' registrations (service_worker_tessronos.cc) */
	{ "background", SYSDEF_BROWSER_STORAGE, 5, 8 },	/* syncs and the content index (background_tessronos.cc) */
	{ "caches", SYSDEF_BROWSER_STORAGE, 6, 9 },	/* Cache Storage (cache_storage_tessronos.cc) */
	{ "perms", SYSDEF_BROWSER_STORAGE, 7, 10 },		/* the user's answers on permissions (services_tessronos.cc) */
	{ "opfs", SYSDEF_BROWSER_STORAGE, 8, 11 },		/* the origins' private file systems (opfs_tessronos.cc) */
	{ "creds", SYSDEF_BROWSER_STORAGE, 9 },	/* the passkeys and passwords kept (credentials_tessronos.cc) */
	{ "widevine", SYSDEF_BROWSER_STORAGE, 10 },	/* each user's object of the Widevine CDM installed (br_widevine.cc) */
	{ "cdmfiles", SYSDEF_BROWSER_STORAGE, 11 },	/* what the CDM keeps, by user and origin (eme_tessronos.cc) */
	{ "autofill", SYSDEF_BROWSER_STORAGE, 12 },	/* the addresses and cards kept (br_data.cc) */
	{ "history", nullptr, 0, 12 },			/* the pages visited (br_data.cc) */
	{ "hsts", nullptr, 0, 13 },			/* the hosts that asked for HTTPS (net_fetch_tessronos.cc) */
	{ "downloads", nullptr, 0, 14 },		/* what was downloaded (br_data.cc) */
	{ "sites", nullptr, 0, 15 },			/* when each origin last used its storage (br_data.cc) */
};

/* The stores written in the older form, in place */
bool older_form( const char *name )
{
	return strcmp(name, "creds") == 0 || strcmp(name, "widevine") == 0 || strcmp(name, "cdmfiles") == 0;
}

/* A UUID's text (8-4-4-4-12 hexadecimal digits) as its 16 bytes, in the order written */
bool parse_uuid( const char *s, TS_UUID *u )
{
	unsigned char	b[16];
	int		n = 0;

	for ( ; *s != '\0' && n < 32; s++ ) {
		if ( *s == '-' ) continue;
		int v = ( *s >= '0' && *s <= '9' ) ? *s - '0'
			: ( *s >= 'a' && *s <= 'f' ) ? *s - 'a' + 10
			: ( *s >= 'A' && *s <= 'F' ) ? *s - 'A' + 10 : -1;
		if ( v < 0 ) return false;
		if ( n % 2 == 0 ) b[n / 2] = (unsigned char)( v << 4 );
		else b[n / 2] |= (unsigned char)v;
		n++;
	}
	if ( n != 32 || *s != '\0' ) return false;
	static_assert(sizeof(TS_UUID) == 16, "a UUID is 16 bytes");
	memcpy(u, b, sizeof(b));
	return true;
}

bool user_store_of( const Named &n, TS_UUID *u );

/* Where the store's bytes are: for a store by user, in the object of the user the browser acts as */
bool uuid_of( const char *name, TS_UUID *u, int *rec )
{
	for ( const Named &n : kObjects ) {
		if ( strcmp(n.name, name) == 0 ) {
			if ( n.user != 0 ) {
				*rec = n.user;
				return user_store_of(n, u);
			}
			*rec = n.rec;
			return parse_uuid(n.uuid, u);
		}
	}
	return false;
}

/* CRC-32 (the polynomial of zlib), a byte at a time from a table */
unsigned g_crc_table[256];
std::once_flag g_crc_once;

unsigned crc32_of( const unsigned char *p, size_t n, unsigned c = 0 )
{
	std::call_once(g_crc_once, []() {
		for ( unsigned i = 0; i < 256; i++ ) {
			unsigned v = i;
			for ( int k = 0; k < 8; k++ ) v = ( v >> 1 ) ^ ( 0xedb88320u & ( 0u - ( v & 1u ) ) );
			g_crc_table[i] = v;
		}
	});
	c ^= 0xffffffffu;
	for ( size_t i = 0; i < n; i++ ) c = g_crc_table[( c ^ p[i] ) & 0xff] ^ ( c >> 8 );
	return c ^ 0xffffffffu;
}

void put32( unsigned char *p, unsigned v )
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)( v >> 8 );
	p[2] = (unsigned char)( v >> 16 );
	p[3] = (unsigned char)( v >> 24 );
}

unsigned get32( const unsigned char *p )
{
	return (unsigned)p[0] | ( (unsigned)p[1] << 8 ) | ( (unsigned)p[2] << 16 ) | ( (unsigned)p[3] << 24 );
}

void put64( unsigned char *p, uint64_t v )
{
	put32(p, (unsigned)v);
	put32(p + 4, (unsigned)( v >> 32 ));
}

uint64_t get64( const unsigned char *p )
{
	return (uint64_t)get32(p) | ( (uint64_t)get32(p + 4) << 32 );
}

/* The size of record 'rec', -1 when there is none */
long rec_size( ID key, int rec )
{
	T_OBREC	r[32];
	INT	cnt = 0;

	if ( ob_lst_rec(key, r, 32, &cnt) < E_OK ) return -1;
	for ( INT i = 0; i < cnt && i < 32; i++ ) {
		if ( r[i].recno == rec ) return (long)r[i].size;
	}
	return -1;
}

long now_ms()
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* Every opening, reading and writing of the objects in this process, one at a time */
std::mutex g_io;

/*
 * The object 'u' opened: another process writing or reading it as one
 * version (the browsers of the windows opened apart share the objects)
 * keeps the others out for a moment (OB_O_WEXCL, OB_O_EXCL), and it is
 * tried again for up to 30 seconds: all a user's stores are one object,
 * and another browser may be reading or writing a large cache in it. A
 * later try at something that failed asks once only (g_open_once).
 */
thread_local bool g_open_once;
std::atomic<long> g_busy_until{0};

ID open_obj( const TS_UUID *u, UINT mode )
{
	/* a renderer asks the network process for what is kept: an object it would open here is a mistake, refused */
	if ( br_store_remote() ) {
		static std::atomic<bool> said{false};
		if ( !said.exchange(true) ) printf("browser: store: an object of the stores would be opened in a renderer: refused\n");
		return E_OACV;
	}
	/* one long wait that came to nothing: the other stores of the object, read next, do not each wait as long */
	int most = now_ms() < g_busy_until ? 1 : 600;
	for ( int i = 0; ; i++ ) {
		ID key = ob_opn_obj(u, mode);
		if ( key > 0 || key != E_BUSY || g_open_once ) return key;
		if ( i >= most ) {
			if ( most > 1 ) g_busy_until = now_ms() + 20000;
			return key;
		}
		usleep(50000);
	}
}

/* 'len' bytes of record 'rec' at 'off' read whole: true when they were */
bool read_at( ID key, int rec, long off, void *buf, long len )
{
	for ( long at = 0; at < len; ) {
		SZ want = ( len - at > kChunk ) ? kChunk : (SZ)( len - at ), got = 0;
		if ( ob_rea_rec(key, rec, (D)( off + at ), (char *)buf + at, want, &got) < E_OK || got <= 0 ) return false;
		at += got;
	}
	return true;
}

/* Why the last write_at of this thread failed: the error, and the bytes written of the part */
thread_local ER g_write_er;
thread_local long g_write_got;

/* 'len' bytes written to record 'rec' at 'off': true when they were */
bool write_at( ID key, int rec, long off, const void *buf, long len )
{
	for ( long at = 0; at < len; ) {
		SZ part = ( len - at > kChunk ) ? kChunk : (SZ)( len - at ), asz = 0;
		ER er = ob_wri_rec(key, rec, (D)( off + at ), (const char *)buf + at, part, &asz);
		if ( er < E_OK || asz != part ) {
			g_write_er = er;
			g_write_got = (long)asz;
			return false;
		}
		at += part;
	}
	return true;
}

/*
 * Whether record 'rec' holds something, or that cannot be told: a store with
 * no head that can be read is then not an empty store, and is not written over
 */
bool may_hold( ID key, int rec )
{
	T_OBREC	r[32];
	INT	cnt = 0;

	if ( ob_lst_rec(key, r, 32, &cnt) < E_OK ) return true;
	for ( INT i = 0; i < cnt && i < 32; i++ ) {
		if ( r[i].recno == rec ) return r[i].size > 0;
	}
	return false;
}

/* The records up to 'rec' made, empty, when they are not there: true when they are */
bool make_recs( ID key, int rec )
{
	for ( int r = 1; r <= rec; r++ ) {
		if ( rec_size(key, r) >= 0 ) continue;
		INT recno = -1;
		if ( ob_apd_rec(key, OB_RT_SYSDATA, 0, &recno) < E_OK || recno != r ) return false;
	}
	return true;
}

}  // namespace

/* ---------------------------------------------------------------- the older form */

namespace {

/* The older record 'rec' of the open object read whole, its 12-byte head checked: 0, or a negative number */
int old_read( const char *name, ID key, int rec, std::string *out )
{
	out->clear();
	long size = rec_size(key, rec);
	if ( size < kHdr ) return -3;
	std::string buf((size_t)size, '\0');
	if ( !read_at(key, rec, 0, &buf[0], size) || memcmp(buf.data(), "BRS1", 4) != 0 ) return -5;
	unsigned plen = get32((const unsigned char *)buf.data() + 4);
	if ( plen > (unsigned)( size - kHdr )
	     || crc32_of((const unsigned char *)buf.data() + kHdr, plen) != get32((const unsigned char *)buf.data() + 8) ) {
		printf("browser: store %s is damaged, left alone\n", name);
		return -6;
	}
	out->assign(buf, kHdr, plen);
	OPENSSL_cleanse(&buf[0], buf.size());
	return 0;
}

/* Record 'rec' of the object 'u' whole, in the older form: 0, or a negative number */
int raw_load( const char *name, const TS_UUID *u, int rec, std::string *out )
{
	out->clear();
	std::lock_guard<std::mutex> hold(g_io);
	ID key = open_obj(u, OB_OP_R);
	if ( key <= 0 ) return -2;
	int rc = old_read(name, key, rec, out);
	ob_cls_obj(key);
	return rc;
}

/*
 * Record 'rec' of the object 'u' made 'len' bytes, in the older form.
 * 'wipe': what the record held is first written over with zeroes where
 * it lies, so that none of it is left in the blocks the shorter record
 * gives back.
 */
int raw_save( const char *name, const TS_UUID *u, int rec, const void *data, int len, bool wipe )
{
	std::lock_guard<std::mutex> hold(g_io);
	/* the record written over and then in pieces: nobody reads it half written */
	ID key = open_obj(u, OB_OP_R | OB_OP_WRITE | OB_OP_RECORD | OB_O_EXCL);
	if ( key <= 0 ) return -2;
	if ( !make_recs(key, rec) ) {
		ob_cls_obj(key);
		return -3;
	}
	int rc = 0;
	long old = wipe ? rec_size(key, rec) : 0;
	if ( old > 0 ) {
		std::string zero((size_t)( old < kChunk ? old : kChunk ), '\0');
		for ( long off = 0; off < old && rc == 0; ) {
			long part = ( old - off > (long)zero.size() ) ? (long)zero.size() : old - off;
			if ( !write_at(key, rec, off, zero.data(), part) ) rc = -4;
			off += part;
		}
	}
	unsigned char hdr[kHdr];
	memcpy(hdr, "BRS1", 4);
	put32(hdr + 4, (unsigned)len);
	put32(hdr + 8, crc32_of((const unsigned char *)data, (size_t)len));
	if ( rc == 0 && !write_at(key, rec, 0, hdr, kHdr) ) rc = -4;
	if ( rc == 0 && !write_at(key, rec, kHdr, data, len) ) rc = -4;
	if ( rc == 0 ) (void)ob_trn_rec(key, rec, (UD)( kHdr + len ));
	ob_cls_obj(key);
	if ( rc < 0 ) printf("browser: writing the store %s failed (%d, %ld bytes of a part)\n", name, (int)g_write_er, g_write_got);
	return rc;
}

}  // namespace

/* ---------------------------------------------------------------- the log */

struct br_store_delta {
	std::vector<BrStoreOp>	ops;
	size_t		bytes = 0;
	bool		afresh = false;
};

namespace {

typedef std::map<std::string, std::string> Entries;

/* A record's head: what the record is */
struct Head {
	bool		valid = false;
	bool		older = false;	/* the older form: one entry of the empty key */
	unsigned	gen = 0;
	uint64_t	end = 0;	/* the bytes of the log that count, past the head */
};

Head head_of( ID key, int rec )
{
	Head h;
	long size = rec_size(key, rec);
	unsigned char b[kHead];
	if ( size < kHdr || !read_at(key, rec, 0, b, size < kHead ? kHdr : kHead) ) return h;
	if ( memcmp(b, "BRS1", 4) == 0 ) {
		h.valid = h.older = true;
		return h;
	}
	if ( size < kHead || memcmp(b, "BRS2", 4) != 0 || crc32_of(b, kHead - 4) != get32(b + kHead - 4) ) return h;
	h.gen = get32(b + 4);
	h.end = get64(b + 8);
	h.valid = (uint64_t)size >= kHead + h.end;
	return h;
}

/* Of a store's two records, the one to read: its number, and its head; 0 when neither holds a log */
int current( ID key, int rec, Head *h )
{
	Head a = head_of(key, rec), b = head_of(key, rec + kAlt);
	if ( a.valid && ( !b.valid || ( !a.older && a.gen >= b.gen ) ) ) {
		*h = a;
		return rec;
	}
	if ( b.valid ) {
		*h = b;
		return rec + kAlt;
	}
	*h = Head();
	return 0;
}

/* An entry's head and bytes put on the end of 'o' */
void seg_put( std::string &o, unsigned char kind, unsigned gen, const std::string &k, const std::string &v )
{
	unsigned char h[kSegHead];
	memset(h, 0, sizeof(h));
	h[0] = kind;
	put32(h + 4, gen);
	put32(h + 8, (unsigned)k.size());
	put32(h + 12, (unsigned)v.size());
	unsigned c = crc32_of(h, 16);
	c = crc32_of((const unsigned char *)k.data(), k.size(), c);
	c = crc32_of((const unsigned char *)v.data(), v.size(), c);
	put32(h + 16, c);
	o.append((const char *)h, sizeof(h));
	o += k;
	o += v;
}

/* The log 'b' (the bytes past the head) applied to 'out': false when an entry in it is damaged */
bool apply_log( const std::string &b, unsigned gen, Entries *out, size_t *live )
{
	const unsigned char *p = (const unsigned char *)b.data();
	size_t at = 0, n = b.size();
	while ( at < n ) {
		if ( n - at < (size_t)kSegHead ) return false;
		unsigned kl = get32(p + at + 8), vl = get32(p + at + 12);
		if ( get32(p + at + 4) != gen || kl > n - at - kSegHead || vl > n - at - kSegHead - kl ) return false;
		unsigned c = crc32_of(p + at, 16);
		c = crc32_of(p + at + kSegHead, (size_t)kl + vl, c);
		if ( c != get32(p + at + 16) ) return false;
		std::string k((const char *)p + at + kSegHead, kl);
		if ( p[at] == kPut ) {
			(*out)[k].assign((const char *)p + at + kSegHead + kl, vl);
		} else if ( p[at] == kDel ) {
			out->erase(k);
		} else {
			return false;
		}
		at += kSegHead + kl + vl;
	}
	if ( live != nullptr ) {
		*live = 0;
		for ( const auto &e : *out ) *live += kSegHead + e.first.size() + e.second.size();
	}
	return true;
}

/* The entries of the store's records in the open object: 0, or a negative number when there are none */
int log_read( const char *name, ID key, int rec, Entries *out, size_t *live )
{
	Head h;
	int at = current(key, rec, &h);
	out->clear();
	if ( at == 0 ) return may_hold(key, rec) || may_hold(key, rec + kAlt) ? -5 : -3;
	if ( h.older ) {
		std::string v;
		int rc = old_read(name, key, at, &v);
		if ( rc < 0 ) return rc;
		(*out)[std::string()] = std::move(v);
		if ( live != nullptr ) *live = kSegHead + (*out)[std::string()].size();
		return 0;
	}
	std::string b((size_t)h.end, '\0');
	if ( h.end > 0 && !read_at(key, at, kHead, &b[0], (long)h.end) ) return -5;
	if ( !apply_log(b, h.gen, out, live) ) {
		printf("browser: store %s: its log is damaged past an entry, the rest left out\n", name);
	}
	return 0;
}

/* The store 'name''s entries, read: 0, or a negative number */
int log_load( const char *name, Entries *out, size_t *live )
{
	TS_UUID	u;
	int	rec = 1;

	if ( br_store_remote() ) {
		uint64_t l = 0;
		int rc = br_store_remote_entries(name, g_open_once, out, &l);
		if ( live != nullptr ) *live = (size_t)l;
		return rc;
	}
	if ( !uuid_of(name, &u, &rec) ) return -1;
	std::lock_guard<std::mutex> hold(g_io);
	/*
	 * the heads and the log read as one version: without keeping the
	 * writers out, a writer in another process may empty the record being
	 * read (the log written afresh into the other record) between its head
	 * and its entries
	 */
	ID key = open_obj(&u, OB_OP_R | OB_O_WEXCL);
	if ( key <= 0 ) return -2;
	int rc = log_read(name, key, rec, out, live);
	ob_cls_obj(key);
	return rc;
}

/*
 * Which version of the store 'name' is kept, to compare with another look:
 * its generation and the length of its log. False when that cannot be told now.
 */
bool log_version( const char *name, uint64_t *version )
{
	TS_UUID	u;
	int	rec = 1;

	if ( br_store_remote() ) return br_store_remote_version(name, version);
	if ( !uuid_of(name, &u, &rec) ) return false;
	std::lock_guard<std::mutex> hold(g_io);
	g_open_once = true;
	ID key = open_obj(&u, OB_OP_R);
	g_open_once = false;
	if ( key <= 0 ) return false;
	Head h;
	int at = current(key, rec, &h);
	ob_cls_obj(key);
	*version = at == 0 ? 0 : ( (uint64_t)h.gen << 40 ) ^ h.end;
	return true;
}

/* The bytes of a head of generation 'gen' over a log of 'end' bytes */
void head_bytes( unsigned char b[kHead], unsigned gen, uint64_t end )
{
	memset(b, 0, kHead);
	memcpy(b, "BRS2", 4);
	put32(b + 4, gen);
	put64(b + 8, end);
	put32(b + kHead - 4, crc32_of(b, kHead - 4));
}

/*
 * A save of the store 'name': the delta 'd' appended to its log, or,
 * 'whole', or when the log would be more than twice what its entries
 * hold, every entry written afresh into the other record. 'live' is
 * what the entries hold, as this process knows it. Answers the bytes
 * written, or a negative number.
 */
long log_save( const char *name, const br_store_delta *d, bool whole, size_t *live, bool *fresh )
{
	TS_UUID	u;
	int	rec = 1;

	*fresh = false;
	if ( br_store_remote() ) {
		uint64_t l = *live;
		long rc = br_store_remote_apply(name, d->ops, whole, d->afresh, &l, fresh);
		*live = (size_t)l;
		if ( rc < 0 ) printf("browser: writing the store %s failed\n", name);
		return rc;
	}
	if ( !uuid_of(name, &u, &rec) ) return -1;
	std::lock_guard<std::mutex> hold(g_io);
	/* the head read, the log added to, the head written last: no other writer between, readers let in */
	ID key = open_obj(&u, OB_OP_R | OB_OP_WRITE | OB_OP_RECORD | OB_O_WEXCL);
	if ( key <= 0 ) return -2;
	long rc = -3;
	Head h;
	int at = make_recs(key, rec + kAlt) ? current(key, rec, &h) : -1;
	/* bytes with no head that reads: left as they are, whatever this save was to put there */
	if ( at < 0 || ( at == 0 && ( may_hold(key, rec) || may_hold(key, rec + kAlt) ) ) ) {
		printf("browser: store %s: %s (records %d %ld, %d %ld): not written\n", name,
		       at < 0 ? "its records could not be made" : "a record holds bytes with no head that reads",
		       rec, rec_size(key, rec), rec + kAlt, rec_size(key, rec + kAlt));
		ob_cls_obj(key);
		return -3;
	}
	size_t grown = h.end + d->bytes;
	bool afresh = whole || d->afresh || at == 0 || h.older || grown > 2 * ( *live + d->bytes ) + ( 1u << 20 );
	if ( !afresh ) {
		/* the changes on the end of the log, then the head that counts them */
		std::string o;
		o.reserve(d->bytes);
		for ( const auto &op : d->ops ) seg_put(o, op.kind, h.gen, op.key, op.val);
		unsigned char hb[kHead];
		head_bytes(hb, h.gen, h.end + o.size());
		if ( write_at(key, at, kHead + (long)h.end, o.data(), (long)o.size()) && write_at(key, at, 0, hb, kHead) ) {
			rc = (long)o.size();
			for ( const auto &op : d->ops ) {
				if ( op.kind == kPut ) *live += kSegHead + op.key.size() + op.val.size();
			}
		}
	} else {
		/* every entry, into the other record under the next generation, its head last */
		Entries e;
		if ( !whole && at != 0 && log_read(name, key, rec, &e, nullptr) < 0 ) {
			/* what is there not read: the log is not made afresh without it */
			ob_cls_obj(key);
			printf("browser: writing the store %s failed: what it holds could not be read\n", name);
			return -5;
		}
		for ( const auto &op : d->ops ) {
			if ( op.kind == kPut ) e[op.key] = op.val;
			else e.erase(op.key);
		}
		int to = ( at == rec ) ? rec + kAlt : rec;
		unsigned gen = ( at != 0 && !h.older ) ? h.gen + 1 : 1;
		std::string o;
		*live = 0;
		for ( const auto &kv : e ) {
			seg_put(o, kPut, gen, kv.first, kv.second);
			*live += kSegHead + kv.first.size() + kv.second.size();
		}
		unsigned char hb[kHead];
		head_bytes(hb, gen, o.size());
		if ( write_at(key, to, kHead, o.data(), (long)o.size()) && write_at(key, to, 0, hb, kHead) ) {
			(void)ob_trn_rec(key, to, (UD)( kHead + o.size() ));
			/* the record the log was in, emptied: the other is read from now on */
			if ( at != 0 ) (void)ob_trn_rec(key, at, 0);
			rc = (long)o.size();
			*fresh = true;
		}
	}
	ob_cls_obj(key);
	if ( rc < 0 ) printf("browser: writing the store %s failed (%d, %ld bytes of a part)\n", name, (int)g_write_er, g_write_got);
	return rc;
}

}  // namespace

/* ---------------------------------------------------------------- sealed stores */

namespace {

/*
 * A sealed store (the passwords and passkeys) is kept encrypted with
 * AES-256-GCM, each user's part with a key of that user's. The record is
 *
 *	"BRK1", the number of parts (4 bytes), then for each part:
 *	the user's UUID (16), the key object's UUID (16), the nonce (12),
 *	the length of what follows (4), the encrypted bytes and the tag (16)
 *
 * with the lengths little-endian, and "BRK1", the user and the key
 * object as the data the tag also covers, so that a part moved to
 * another user does not open. The key, 32 random bytes, is record 1
 * of an object of its own, "ブラウザの鍵", owned by the user with the
 * mode rw------- and held by a fixed count: another user, and a program
 * acting as another user, cannot open it (an administrator can). A part
 * of another user is kept as it is when this user's part is written.
 */
constexpr char kSealMagic[4] = { 'B', 'R', 'K', '1' };
constexpr size_t kKeyLen = 32, kNonceLen = 12, kTagLen = 16;
constexpr int kKeyRec = 1;

struct Part {
	TS_UUID		user, keyobj;
	std::string	nonce, sealed;
};

bool parts_of( const std::string &rec, std::vector<Part> *parts )
{
	parts->clear();
	const unsigned char *p = (const unsigned char *)rec.data();
	size_t n = rec.size(), at = 8;
	if ( n < 8 || memcmp(p, kSealMagic, 4) != 0 ) return false;
	unsigned cnt = get32(p + 4);
	for ( unsigned i = 0; i < cnt; i++ ) {
		if ( n - at < 16 + 16 + kNonceLen + 4 ) return false;
		Part part;
		memcpy(&part.user, p + at, 16);
		memcpy(&part.keyobj, p + at + 16, 16);
		part.nonce.assign((const char *)p + at + 32, kNonceLen);
		unsigned len = get32(p + at + 32 + kNonceLen);
		at += 32 + kNonceLen + 4;
		if ( len < kTagLen || n - at < len ) return false;
		part.sealed.assign((const char *)p + at, len);
		at += len;
		parts->push_back(std::move(part));
	}
	return true;
}

std::string record_of( const std::vector<Part> &parts )
{
	std::string o(kSealMagic, 4);
	unsigned char b[4];
	put32(b, (unsigned)parts.size());
	o.append((const char *)b, 4);
	for ( const Part &part : parts ) {
		o.append((const char *)&part.user, 16);
		o.append((const char *)&part.keyobj, 16);
		o.append(part.nonce);
		put32(b, (unsigned)part.sealed.size());
		o.append((const char *)b, 4);
		o.append(part.sealed);
	}
	return o;
}

std::string aad_of( const Part &part )
{
	std::string a(kSealMagic, 4);
	a.append((const char *)&part.user, 16);
	a.append((const char *)&part.keyobj, 16);
	return a;
}

bool same_uuid( const TS_UUID &a, const TS_UUID &b )
{
	return memcmp(&a, &b, sizeof(TS_UUID)) == 0;
}

/* The user the browser acts as */
bool me( TS_UUID *user, TS_UUID *group )
{
	T_OBCRD *c = (T_OBCRD *)calloc(1, sizeof(T_OBCRD));
	if ( c == nullptr ) return false;
	bool ok = ob_get_crd(c) >= E_OK;
	if ( ok ) {
		*user = c->user;
		*group = ( c->ngrp > 0 ) ? c->grp[0] : c->user;
	}
	free(c);
	return ok;
}

/* The key in the key object 'u': true when it could be read, is the user's own and is not open to others */
bool read_key( const TS_UUID &u, const TS_UUID &user, unsigned char key[kKeyLen] )
{
	T_OBPRT prt;		/* the head alone: a key with entries in its list is not the user's alone */
	if ( ob_get_prt(&u, &prt, sizeof(prt), NULL) < E_OK || !same_uuid(prt.owner, user) || ( prt.mode & 077 ) != 0 || prt.nacl != 0 ) {
		return false;
	}
	std::lock_guard<std::mutex> hold(g_io);
	ID k = open_obj(&u, OB_OP_R);
	if ( k <= 0 ) return false;
	SZ got = 0;
	bool ok = rec_size(k, kKeyRec) == (long)kKeyLen
		  && ob_rea_rec(k, kKeyRec, 0, key, (SZ)kKeyLen, &got) >= E_OK && got == (SZ)kKeyLen;
	ob_cls_obj(k);
	return ok;
}

/* A new key object for the user, beside the store's object, holding a new key */
bool make_key( const TS_UUID &store, const TS_UUID &user, const TS_UUID &group, TS_UUID *u,
	       unsigned char key[kKeyLen] )
{
	static const char kJson[] = "{\"name\":\"ブラウザの鍵\"}";
	static const char kTad[] =
		"<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"ブラウザの鍵\">\n<document>\n"
		"<p>ブラウザの鍵</p>\n"
		"<p>ブラウザが保存したパスワードとパスキーを暗号化する鍵。この利用者だけが読める。</p>\n"
		"</document>\n</tad>\n";
	T_OBPRT prt;
	memset(&prt, 0, sizeof(prt));
	prt.owner = user;
	prt.group = group;
	prt.mode = 0600;
	T_OBCRE c;
	memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = (CONST UB *)kJson;
	c.jsonsz = (SZ)strlen(kJson);
	c.near = store;
	c.prt = &prt;
	if ( ob_cre_obj(&c, u) < E_OK ) return false;
	if ( RAND_bytes(key, kKeyLen) != 1 ) {
		(void)ob_del_obj(u);
		return false;
	}
	ID k = ob_opn_obj(u, OB_OP_R | OB_OP_W | OB_O_EXCL);	/* a new object filled: not read half made */
	bool ok = k > 0;
	INT recno = -1;
	SZ asz = 0;
	if ( ok ) ok = ob_apd_rec(k, OB_RT_TAD, 0, &recno) >= E_OK && recno == 0
		       && ob_wri_rec(k, 0, 0, kTad, (SZ)strlen(kTad), &asz) >= E_OK;
	if ( ok ) ok = ob_apd_rec(k, OB_RT_SYSDATA, 0, &recno) >= E_OK && recno == kKeyRec
		       && ob_wri_rec(k, kKeyRec, 0, key, (SZ)kKeyLen, &asz) >= E_OK && asz == (SZ)kKeyLen;
	if ( k > 0 ) ob_cls_obj(k);
	if ( ok ) ok = ob_lnk_obj(u) >= E_OK;
	if ( !ok ) {
		(void)ob_del_obj(u);
		OPENSSL_cleanse(key, kKeyLen);
		return false;
	}
	return true;
}

bool seal( const unsigned char key[kKeyLen], Part *part, const std::string &plain )
{
	EVP_AEAD_CTX ctx;
	if ( !EVP_AEAD_CTX_init(&ctx, EVP_aead_aes_256_gcm(), key, kKeyLen, kTagLen, nullptr) ) return false;
	part->nonce.assign(kNonceLen, '\0');
	bool ok = RAND_bytes((uint8_t *)&part->nonce[0], kNonceLen) == 1;
	std::string aad = aad_of(*part);
	part->sealed.assign(plain.size() + kTagLen, '\0');
	size_t got = 0;
	ok = ok && EVP_AEAD_CTX_seal(&ctx, (uint8_t *)&part->sealed[0], &got, part->sealed.size(),
				     (const uint8_t *)part->nonce.data(), kNonceLen,
				     (const uint8_t *)plain.data(), plain.size(),
				     (const uint8_t *)aad.data(), aad.size());
	EVP_AEAD_CTX_cleanup(&ctx);
	if ( ok ) part->sealed.resize(got);
	return ok;
}

bool unseal( const unsigned char key[kKeyLen], const Part &part, std::string *plain )
{
	EVP_AEAD_CTX ctx;
	if ( !EVP_AEAD_CTX_init(&ctx, EVP_aead_aes_256_gcm(), key, kKeyLen, kTagLen, nullptr) ) return false;
	std::string aad = aad_of(part);
	plain->assign(part.sealed.size(), '\0');
	size_t got = 0;
	bool ok = EVP_AEAD_CTX_open(&ctx, (uint8_t *)&(*plain)[0], &got, plain->size(),
				    (const uint8_t *)part.nonce.data(), kNonceLen,
				    (const uint8_t *)part.sealed.data(), part.sealed.size(),
				    (const uint8_t *)aad.data(), aad.size());
	EVP_AEAD_CTX_cleanup(&ctx);
	if ( ok ) {
		plain->resize(got);
	} else {
		OPENSSL_cleanse(&(*plain)[0], plain->size());
		plain->clear();
	}
	return ok;
}

/*
 * This user's part of a sealed store made 'plain', the other users'
 * parts kept, and what the record held written over where it lies
 */
int seal_save( const char *name, const TS_UUID &store, int rec, const std::string &plain )
{
	TS_UUID user, group;
	if ( !me(&user, &group) ) return -7;
	std::string have;
	std::vector<Part> parts;
	int was = raw_load(name, &store, rec, &have);
	if ( was != 0 && was != -3 ) {
		/* the other users' parts and this user's would go with it */
		printf("browser: store %s: what it holds could not be read (%d), not saved\n", name, was);
		return was;
	}
	if ( was == 0 ) (void)parts_of(have, &parts);
	OPENSSL_cleanse(&have[0], have.size());
	unsigned char key[kKeyLen];
	Part mine;
	bool keyed = false;
	for ( size_t i = 0; i < parts.size(); i++ ) {
		if ( !same_uuid(parts[i].user, user) ) continue;
		mine.keyobj = parts[i].keyobj;
		keyed = read_key(mine.keyobj, user, key);
		if ( !keyed ) {
			/* a new key would leave what the part holds unreadable for good */
			printf("browser: store %s: the user's key cannot be read, not saved\n", name);
			return -10;
		}
		parts.erase(parts.begin() + (long)i);
		break;
	}
	if ( !keyed ) {
		if ( !make_key(store, user, group, &mine.keyobj, key) ) {
			printf("browser: store %s: no key could be made, not saved\n", name);
			return -8;
		}
		printf("browser: store %s: a key of the user's own made\n", name);
	}
	mine.user = user;
	bool ok = seal(key, &mine, plain);
	OPENSSL_cleanse(key, sizeof(key));
	if ( !ok ) return -9;
	parts.push_back(std::move(mine));
	std::string out = record_of(parts);
	return raw_save(name, &store, rec, out.data(), (int)out.size(), true);
}

/*
 * This user's part of a sealed store, opened: 0 with its bytes, or a
 * negative number. A record still in the plain form is read as it is,
 * and written again sealed at once (*resealed set), and the journal of
 * its volume wiped.
 */
int seal_load( const char *name, const TS_UUID &store, int rec, std::string *plain, bool *resealed )
{
	*resealed = false;
	std::string have;
	int rc = raw_load(name, &store, rec, &have);
	if ( rc < 0 ) return rc;
	std::vector<Part> parts;
	if ( !parts_of(have, &parts) ) {
		if ( have.empty() ) return -3;
		*plain = have;
		OPENSSL_cleanse(&have[0], have.size());
		rc = seal_save(name, store, rec, *plain);
		/* the copies of the plain bytes the volume's journal still holds written over too */
		ER wiped = ( rc == 0 ) ? ob_wip_vol(&store) : E_OK;
		printf("browser: store %s was kept plain: sealed again (%d), the journal wiped (%d)\n", name, rc, (int)wiped);
		*resealed = rc == 0;
		return 0;
	}
	TS_UUID user, group;
	if ( !me(&user, &group) ) return -7;
	for ( const Part &part : parts ) {
		if ( !same_uuid(part.user, user) ) continue;
		unsigned char key[kKeyLen];
		if ( !read_key(part.keyobj, user, key) ) {
			printf("browser: store %s: the user's key cannot be read\n", name);
			return -10;
		}
		bool ok = unseal(key, part, plain);
		OPENSSL_cleanse(key, sizeof(key));
		if ( !ok ) {
			printf("browser: store %s: the user's part does not open\n", name);
			return -11;
		}
		return 0;
	}
	return -3;
}

bool sealed_name( const char *name )
{
	return strcmp(name, "creds") == 0 || strcmp(name, "autofill") == 0;
}

/* ---------------------------------------------------------------- the user's object */

/*
 * Record 13 of the local storage's object: "BRU1", then for each user
 * their UUID and that of their object. It says only where to look: an
 * object is used only when it is the user's own and open to no one else,
 * as a key object is (read_key).
 */
constexpr int kUsersRec = 13;

/* 1 the user's alone, 0 it is not or is no more there, -1 that cannot be told now */
int object_is_mine( const TS_UUID &u, const TS_UUID &user )
{
	T_OBPRT prt;
	ER er = ob_get_prt(&u, &prt, sizeof(prt), NULL);
	if ( er == E_NOEXS ) return 0;
	if ( er < E_OK ) {
		printf("browser: the object listed for this user cannot be asked about (%d)\n", (int)er);
		return -1;
	}
	return same_uuid(prt.owner, user) && ( prt.mode & 077 ) == 0 && prt.nacl == 0 ? 1 : 0;
}

bool make_user_object( const TS_UUID &store, const TS_UUID &user, const TS_UUID &group, TS_UUID *u )
{
	static const char kJson[] = "{\"name\":\"ブラウザの利用者データ\"}";
	static const char kTad[] =
		"<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"ブラウザの利用者データ\">\n<document>\n"
		"<p>ブラウザの利用者データ</p>\n"
		"<p>ブラウザがこの利用者のために持つCookie、サイトのデータ、キャッシュ、履歴など。この利用者だけが読める。</p>\n"
		"</document>\n</tad>\n";
	T_OBPRT prt;
	memset(&prt, 0, sizeof(prt));
	prt.owner = user;
	prt.group = group;
	prt.mode = 0600;
	T_OBCRE c;
	memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = (CONST UB *)kJson;
	c.jsonsz = (SZ)strlen(kJson);
	c.near = store;
	c.prt = &prt;
	if ( ob_cre_obj(&c, u) < E_OK ) return false;
	ID k = ob_opn_obj(u, OB_OP_R | OB_OP_W | OB_O_EXCL);
	INT recno = -1;
	SZ asz = 0;
	bool ok = k > 0 && ob_apd_rec(k, OB_RT_TAD, 0, &recno) >= E_OK && recno == 0
		  && ob_wri_rec(k, 0, 0, kTad, (SZ)strlen(kTad), &asz) >= E_OK;
	if ( k > 0 ) ob_cls_obj(k);
	if ( ok ) ok = ob_lnk_obj(u) >= E_OK;
	if ( !ok ) (void)ob_del_obj(u);
	return ok;
}

/* The object of the user the browser acts as, found or made, the list held while it is */
bool find_user_object( TS_UUID *out )
{
	TS_UUID store, user, group;
	if ( !me(&user, &group) || !parse_uuid(SYSDEF_BROWSER_STORAGE, &store) ) return false;
	std::lock_guard<std::mutex> hold(g_io);
	/* a second browser starting waits here, and finds what this one made */
	ID key = open_obj(&store, OB_OP_R | OB_OP_WRITE | OB_OP_RECORD | OB_O_EXCL);
	if ( key <= 0 ) return false;
	std::string dir;
	std::vector<std::pair<TS_UUID, TS_UUID>> rows;
	int listed = old_read("users", key, kUsersRec, &dir);
	if ( listed != 0 && listed != -3 ) {
		/* a list that cannot be read is not an empty list: a new object would hide the old one for good */
		printf("browser: the list of the users' objects cannot be read (%d)\n", listed);
		ob_cls_obj(key);
		return false;
	}
	if ( listed == 0 && dir.size() >= 4 && memcmp(dir.data(), "BRU1", 4) == 0 ) {
		for ( size_t at = 4; at + 32 <= dir.size(); at += 32 ) {
			std::pair<TS_UUID, TS_UUID> r;
			memcpy(&r.first, dir.data() + at, 16);
			memcpy(&r.second, dir.data() + at + 16, 16);
			rows.push_back(r);
		}
	}
	bool ok = false, unknown = false;
	for ( const auto &r : rows ) {
		if ( ok || !same_uuid(r.first, user) ) continue;
		int mine = object_is_mine(r.second, user);
		if ( mine > 0 ) {
			*out = r.second;
			ok = true;
		} else if ( mine < 0 ) {
			unknown = true;
		} else {
			/* its row stays, and the object: what it holds is not given up by this */
			printf("browser: the object listed for this user is not theirs alone: not used\n");
		}
	}
	if ( !ok && unknown ) {
		ob_cls_obj(key);
		return false;
	}
	if ( !ok && make_user_object(store, user, group, out) ) {
		rows.emplace_back(user, *out);
		std::string o("BRU1", 4);
		for ( const auto &r : rows ) {
			o.append((const char *)&r.first, 16);
			o.append((const char *)&r.second, 16);
		}
		unsigned char hdr[kHdr];
		memcpy(hdr, "BRS1", 4);
		put32(hdr + 4, (unsigned)o.size());
		put32(hdr + 8, crc32_of((const unsigned char *)o.data(), o.size()));
		ok = make_recs(key, kUsersRec) && write_at(key, kUsersRec, 0, hdr, kHdr)
		     && write_at(key, kUsersRec, kHdr, o.data(), (long)o.size());
		if ( ok ) (void)ob_trn_rec(key, kUsersRec, (UD)( kHdr + o.size() ));
		printf("browser: an object of the user's own for what the browser keeps made (%d)\n", ok ? 1 : 0);
	}
	ob_cls_obj(key);
	return ok;
}

/*
 * What an older browser left in the record every user shared, moved to
 * the user's own record when that holds nothing yet. The shared record is
 * emptied only when the copy is written, with the writers kept out of it,
 * so that one user alone gets it.
 */
void move_shared( const Named &n, const TS_UUID &mine )
{
	TS_UUID old;
	if ( n.uuid == nullptr || n.rec <= 0 || !parse_uuid(n.uuid, &old) ) return;
	std::lock_guard<std::mutex> hold(g_io);
	ID src = open_obj(&old, OB_OP_R | OB_OP_WRITE | OB_OP_RECORD | OB_O_WEXCL);
	if ( src <= 0 ) return;
	Head sh;
	int sat = current(src, n.rec, &sh);
	if ( sat != 0 && ( sh.older || sh.end > 0 ) ) {
		ID dst = open_obj(&mine, OB_OP_R | OB_OP_WRITE | OB_OP_RECORD | OB_O_WEXCL);
		Head dh;
		if ( dst > 0 && make_recs(dst, n.user + kAlt) && current(dst, n.user, &dh) == 0 ) {
			Entries e;
			if ( log_read(n.name, src, n.rec, &e, nullptr) == 0 ) {
				std::string o;
				for ( const auto &kv : e ) seg_put(o, kPut, 1, kv.first, kv.second);
				unsigned char hb[kHead];
				head_bytes(hb, 1, o.size());
				if ( write_at(dst, n.user, kHead, o.data(), (long)o.size()) && write_at(dst, n.user, 0, hb, kHead) ) {
					(void)ob_trn_rec(src, n.rec, 0);
					(void)ob_trn_rec(src, n.rec + kAlt, 0);
					printf("browser: store %s moved to the user's own object, %lu bytes\n", n.name, (unsigned long)o.size());
				}
			}
		}
		if ( dst > 0 ) ob_cls_obj(dst);
	}
	ob_cls_obj(src);
}

std::mutex	g_user_lock;
bool		g_user_known;
TS_UUID		g_user_object;
std::map<std::string, bool> g_moved;

bool user_store_of( const Named &n, TS_UUID *u )
{
	std::lock_guard<std::mutex> hold(g_user_lock);
	if ( !g_user_known ) {
		g_user_known = find_user_object(&g_user_object);
		if ( !g_user_known ) {
			printf("browser: no object of the user's own: nothing is read or kept\n");
			return false;
		}
	}
	*u = g_user_object;
	if ( !g_moved[n.name] ) {
		g_moved[n.name] = true;
		move_shared(n, *u);
	}
	return true;
}

/*
 * The stores this process could not read, though something may be there. A
 * program then starts from nothing in memory, and a save of the whole store
 * would put that nothing over what is kept: it is refused until a read works.
 */
std::mutex g_unread_lock;
std::map<std::string, int> g_unread;

void note_read( const char *name, int rc )
{
	std::lock_guard<std::mutex> hold(g_unread_lock);
	if ( rc == 0 || rc == -3 ) {
		g_unread.erase(name);
		return;
	}
	if ( !g_unread.count(name) ) printf("browser: store %s not read (%d): started empty, what it holds is left alone\n", name, rc);
	g_unread[name] = rc;
}

bool is_unread( const char *name )
{
	std::lock_guard<std::mutex> hold(g_unread_lock);
	return g_unread.count(name) != 0;
}

/* What a store kept by entries holds, as this process knows it: for the choice to write its log afresh */
std::mutex g_live_lock;
std::map<std::string, size_t> g_live;

/*
 * A store's bytes read from its object, in this process: 0, or a negative
 * number. 'live' is set, and 'logged', for a store kept by entries whose
 * log was read.
 */
int load_here( const char *name, std::string *got, size_t *live, bool *logged )
{
	TS_UUID	u;
	int	rec = 1;

	*logged = false;
	if ( !uuid_of(name, &u, &rec) ) return -1;
	if ( sealed_name(name) ) {
		bool resealed = false;
		return seal_load(name, u, rec, got, &resealed);
	}
	if ( older_form(name) ) return raw_load(name, &u, rec, got);
	Entries e;
	int rc = log_load(name, &e, live);
	if ( rc != 0 ) return rc;
	*logged = true;
	auto it = e.find(std::string());
	if ( it == e.end() ) return -3;
	*got = std::move(it->second);
	return 0;
}

}  // namespace

extern "C" int br_store_load( const char *name, unsigned char **data, int *len )
{
	*data = nullptr;
	*len = 0;
	std::string got;
	size_t live = 0;
	bool logged = false;
	int rc;
	if ( br_store_remote() ) {
		uint64_t l = 0;
		rc = br_store_remote_load(name, g_open_once, &got, &l);
		live = (size_t)l;
		logged = ( rc == 0 || rc == -3 ) && !sealed_name(name) && !older_form(name);
	} else {
		rc = load_here(name, &got, &live, &logged);
	}
	if ( logged ) {
		std::lock_guard<std::mutex> hold(g_live_lock);
		g_live[name] = live;
	}
	note_read(name, rc);
	if ( rc < 0 ) return rc;
	unsigned char *buf = (unsigned char *)malloc(got.size() + 1);
	if ( buf == nullptr ) {
		OPENSSL_cleanse(&got[0], got.size());
		return -4;
	}
	memcpy(buf, got.data(), got.size());
	OPENSSL_cleanse(&got[0], got.size());
	*data = buf;
	*len = (int)got.size();
	return 0;
}

extern "C" int br_store_try_load( const char *name, unsigned char **data, int *len )
{
	g_open_once = true;
	int rc = br_store_load(name, data, len);
	g_open_once = false;
	return rc;
}

extern "C" void br_store_mark_unread( const char *name )
{
	note_read(name, -6);
}

extern "C" int br_store_load_entries( const char *name, br_store_entry_fn each, void *ctx )
{
	Entries e;
	size_t live = 0;
	int rc = log_load(name, &e, &live);
	note_read(name, rc);
	if ( rc < 0 ) return rc;
	{
		std::lock_guard<std::mutex> hold(g_live_lock);
		g_live[name] = live;
	}
	for ( const auto &kv : e ) {
		each(ctx, kv.first.data(), (int)kv.first.size(), (const unsigned char *)kv.second.data(),
		     (int)kv.second.size());
	}
	return 0;
}

namespace {

/* A save of the store 'name' made of 'd': 0, or a negative number */
int save_delta( const char *name, const br_store_delta *d, bool whole, long t0 )
{
	size_t live;
	{
		std::lock_guard<std::mutex> hold(g_live_lock);
		live = g_live[name];
	}
	bool fresh = false;
	long n = whole && is_unread(name) ? -12 : log_save(name, d, whole, &live, &fresh);
	{
		std::lock_guard<std::mutex> hold(g_live_lock);
		g_live[name] = live;
	}
	if ( whole && is_unread(name) ) {
		printf("browser: store %s not saved: it could not be read, and this would replace it\n", name);
		return -12;
	}
	if ( n < 0 ) {
		printf("browser: store %s not saved (%ld)\n", name, n);
		return (int)n;
	}
	if ( whole ) {
		printf("browser: store %s saved, %ld bytes in %ld ms\n", name, n, now_ms() - t0);
	} else {
		printf("browser: store %s saved, %d entries changed, %ld bytes written%s in %ld ms (%lu kept)\n", name,
		       (int)d->ops.size(), n, fresh ? " afresh" : "", now_ms() - t0, (unsigned long)live);
	}
	return 0;
}

}  // namespace

extern "C" int br_store_save( const char *name, const void *data, int len )
{
	TS_UUID	u;
	int	rec = 1;

	if ( is_unread(name) ) {
		printf("browser: store %s not saved: it could not be read, and this would replace it\n", name);
		return -12;
	}
	if ( sealed_name(name) || older_form(name) ) {
		std::string bytes((const char *)data, (size_t)len);
		int rc = br_store_remote() ? br_store_remote_save(name, bytes) : br_store_core_save(name, bytes);
		OPENSSL_cleanse(&bytes[0], bytes.size());
		return rc;
	}
	/* a store kept by entries: written as the one entry of the empty key (log_save asks the network process in a renderer) */
	if ( !br_store_remote() && !uuid_of(name, &u, &rec) ) return -1;
	br_store_delta d;
	d.ops.push_back({ kPut, std::string(), std::string((const char *)data, (size_t)len) });
	d.bytes = kSegHead + (size_t)len;
	return save_delta(name, &d, true, now_ms());
}

/* ---------------------------------------------------------------- the lower layer, for a renderer */

/*
 * What the network process does for a renderer (store_service_tessronos.cc):
 * the objects opened, read and written here, and nothing of what this
 * process keeps of its own stores (g_unread, g_live) touched by it.
 */

bool br_store_for_pages( const char *name )
{
	static const char *const kNetworks[] = { "cookie", "cache", "quic", "hsts" };
	for ( const char *n : kNetworks ) {
		if ( strcmp(n, name) == 0 ) return false;
	}
	for ( const Named &n : kObjects ) {
		if ( strcmp(n.name, name) == 0 ) return true;
	}
	return false;
}

int br_store_core_load( const char *name, bool once, std::string *bytes, uint64_t *live )
{
	size_t l = 0;
	bool logged = false, was = g_open_once;
	g_open_once = was || once;
	int rc = load_here(name, bytes, &l, &logged);
	g_open_once = was;
	*live = logged ? (uint64_t)l : 0;
	return rc;
}

int br_store_core_entries( const char *name, bool once, BrStoreEntries *out, uint64_t *live )
{
	size_t l = 0;
	bool was = g_open_once;
	g_open_once = was || once;
	int rc = log_load(name, out, &l);
	g_open_once = was;
	*live = (uint64_t)l;
	return rc;
}

bool br_store_core_version( const char *name, uint64_t *version )
{
	return log_version(name, version);
}

int br_store_core_save( const char *name, const std::string &bytes )
{
	TS_UUID	u;
	int	rec = 1;

	if ( !sealed_name(name) && !older_form(name) ) return -1;
	if ( !uuid_of(name, &u, &rec) ) return -1;
	if ( sealed_name(name) ) return seal_save(name, u, rec, bytes);
	return raw_save(name, &u, rec, bytes.data(), (int)bytes.size(), false);
}

long br_store_core_apply( const char *name, std::vector<BrStoreOp> *ops, bool whole, bool afresh,
			  uint64_t *live, bool *fresh )
{
	br_store_delta d;
	*fresh = false;
	if ( sealed_name(name) || older_form(name) ) return -1;
	for ( const BrStoreOp &op : *ops ) {
		/* only what a save writes: anything else would be read back as a log damaged */
		if ( op.kind != kPut && op.kind != kDel ) return -1;
		d.bytes += kSegHead + op.key.size() + op.val.size();
	}
	d.ops = std::move(*ops);
	d.afresh = afresh;
	size_t l = (size_t)*live;
	long rc = log_save(name, &d, whole, &l, fresh);
	*live = (uint64_t)l;
	*ops = std::move(d.ops);
	return rc;
}

/* ---------------------------------------------------------------- deltas */

extern "C" br_store_delta *br_store_delta_new( void )
{
	return new br_store_delta();
}

extern "C" void br_store_delta_put( br_store_delta *d, const void *key, int klen, const void *val, int vlen )
{
	d->ops.push_back({ kPut, std::string((const char *)key, (size_t)klen), std::string((const char *)val, (size_t)vlen) });
	d->bytes += kSegHead + (size_t)klen + (size_t)vlen;
}

extern "C" void br_store_delta_del( br_store_delta *d, const void *key, int klen )
{
	d->ops.push_back({ kDel, std::string((const char *)key, (size_t)klen), std::string() });
	d->bytes += kSegHead + (size_t)klen;
}

extern "C" void br_store_delta_keep( br_store_delta *d, const void *key, int klen )
{
	d->ops.push_back({ kKeep, std::string((const char *)key, (size_t)klen), std::string() });
}

extern "C" void br_store_delta_afresh( br_store_delta *d )
{
	d->afresh = true;
}

extern "C" int br_store_delta_count( const br_store_delta *d )
{
	return d == nullptr ? 0 : (int)d->ops.size();
}

extern "C" void br_store_delta_free( br_store_delta *d )
{
	delete d;
}

/* ---------------------------------------------------------------- registered stores */

namespace {

constexpr int kMaxStores = 24;

struct Job {
	int		store;
	bool		whole;
	br_store_delta	*d;
	long		since;
};

/* What an entry of a shared store held, without holding it a second time in memory */
struct Sum {
	unsigned	crc = 0;
	size_t		len = 0;
	bool operator==( const Sum &o ) const { return crc == o.crc && len == o.len; }
};

Sum sum_of( const std::string &v )
{
	Sum r;
	r.crc = crc32_of((const unsigned char *)v.data(), v.size());
	r.len = v.size();
	return r;
}

/* An entry another browser changed, for the owner */
struct Change {
	std::string	key;
	bool		put;
	std::string	val;
	Sum		sum;
};

constexpr long kRetryMs = 15000;		/* how soon what could not be written is tried again */
constexpr size_t kFailedMost = 64u << 20;	/* and how much of it is held in memory */

struct Store {
	const char		*name;
	br_store_writer		write;		/* a whole store */
	br_store_collect	collect;	/* a store kept by entries */
	void			*ctx;
	unsigned		delay_ms;
	volatile int		dirty;
	long			dirty_since;
	bool			asking;		/* asked for its changes, not yet answered */
	bool			saved_any;	/* last_crc is that of the bytes last handed over */
	std::deque<Job>		failed;		/* what could not be written (the disk away), in order: tried again */
	size_t			failed_bytes;
	long			retry_at;
	unsigned		last_crc;
	int			last_len;
	bool			own_thread;	/* due saves seen by the store's thread */
	br_store_told		told;		/* what other processes keep looked at */
	void			*told_ctx;
	unsigned		every_ms;
	long			looked_at;
	uint64_t		version;
	bool			version_known;
	br_store_snapshot	snap;		/* a shared store */
	br_store_take		take;
	std::map<std::string, Sum>	base;	/* what is kept, as this browser last wrote or took it */
	std::map<std::string, long>	handed;	/* written lately, perhaps not yet in what is read: when */
	std::vector<Change>	incoming;	/* for the owner, at the next tick */
};

Store g_stores[kMaxStores];
int g_nstores;

/*
 * The store's thread and what it is to write: jobs in the order they
 * were handed over. g_asked counts the stores asked for their changes
 * and g_answered the answers; g_queued and g_written count the jobs.
 */
std::mutex		g_lock;		/* the stores' states, the jobs and the counts */
std::condition_variable	g_cv;		/* a job, an answer, a job done */
std::deque<Job>		g_jobs;
unsigned		g_asked, g_answered, g_queued, g_written;
bool			g_thread, g_rounds;

void save_one( int i );

constexpr long kHandedMs = 60000;

/* A shared store read, what differs from what this browser knows put by for its owner: false when it could not be read */
bool shared_read( int i )
{
	Store &s = g_stores[i];
	Entries e;
	size_t live = 0;
	g_open_once = true;
	int rc = log_load(s.name, &e, &live);
	g_open_once = false;
	if ( rc < 0 && rc != -3 ) return false;
	std::vector<std::pair<std::string, Sum>> sums;
	sums.reserve(e.size());
	for ( const auto &kv : e ) sums.emplace_back(kv.first, sum_of(kv.second));
	long now = now_ms();
	std::lock_guard<std::mutex> hold(g_lock);
	auto ours = [&]( const std::string &k ) {
		auto h = s.handed.find(k);
		if ( h == s.handed.end() ) return false;
		if ( now - h->second < kHandedMs ) return true;
		s.handed.erase(h);
		return false;
	};
	s.incoming.clear();
	for ( const auto &ks : sums ) {
		auto b = s.base.find(ks.first);
		if ( b != s.base.end() && b->second == ks.second ) {
			s.handed.erase(ks.first);
			continue;
		}
		if ( !ours(ks.first) ) s.incoming.push_back({ ks.first, true, e[ks.first], ks.second });
	}
	for ( const auto &b : s.base ) {
		if ( e.find(b.first) == e.end() && !ours(b.first) ) s.incoming.push_back({ b.first, false, std::string(), Sum() });
	}
	return true;
}

/* What the store's thread sees to by itself, with no job to do: saves due, and what other processes wrote */
void store_rounds()
{
	long now = now_ms();

	for ( int i = 0; i < g_nstores; i++ ) {
		Store &s = g_stores[i];
		if ( s.own_thread && s.dirty && now - s.dirty_since >= (long)s.delay_ms ) save_one(i);
		if ( ( s.told == nullptr && s.snap == nullptr ) || now - s.looked_at < (long)s.every_ms ) continue;
		s.looked_at = now;
		uint64_t v = 0;
		if ( !log_version(s.name, &v) || ( s.version_known && v == s.version ) ) continue;
		if ( s.snap != nullptr ) {
			if ( shared_read(i) ) {
				s.version = v;
				s.version_known = true;
			}
			continue;
		}
		s.version = v;
		s.version_known = true;
		g_open_once = true;
		s.told(s.told_ctx);
		g_open_once = false;
	}
}

/* A thread of their own: a look at what is kept does not hold up the writing, nor a long write the saves due */
void *rounds_thread( void * )
{
	printf("browser: store: the rounds begin\n");
	for ( ;; ) {
		usleep(500000);
		store_rounds();
	}
	return nullptr;
}

void *store_thread( void * )
{
	for ( ;; ) {
		Job j;
		{
			std::unique_lock<std::mutex> hold(g_lock);
			g_cv.wait(hold, []() { return !g_jobs.empty(); });
			j = g_jobs.front();
			g_jobs.pop_front();
		}
		Store &s = g_stores[j.store];
		int rc;
		if ( j.whole && sealed_name(s.name) ) {
			const std::string &v = j.d->ops[0].val;
			rc = br_store_save(s.name, v.data(), (int)v.size());
			OPENSSL_cleanse(&j.d->ops[0].val[0], v.size());
			if ( rc == 0 ) printf("browser: store %s saved, %d bytes in %ld ms\n", s.name, (int)v.size(), now_ms() - j.since);
		} else {
			rc = save_delta(s.name, j.d, j.whole, j.since);
		}
		{
			std::lock_guard<std::mutex> hold(g_lock);
			if ( rc == 0 ) {
				delete j.d;
			} else if ( j.whole ) {
				/* written again from what is in memory then, once the disk may be back */
				delete j.d;
				s.saved_any = false;
				s.dirty = 1;
				s.dirty_since = now_ms() + kRetryMs;
			} else if ( s.failed_bytes + j.d->bytes > kFailedMost ) {
				printf("browser: store %s: %zu bytes of changes could not be written and are given up\n", s.name, j.d->bytes);
				delete j.d;
			} else {
				/* its changes are not asked for twice: kept, in order, ahead of what comes after */
				s.failed_bytes += j.d->bytes;
				s.failed.push_back(j);
				s.retry_at = now_ms() + kRetryMs;
			}
			g_written++;
		}
		g_cv.notify_all();
	}
	return nullptr;
}

/* The store's thread, made when it is first wanted. Called with g_lock held. */
void thread_up()
{
	if ( g_thread ) return;
	pthread_t	th;
	pthread_attr_t	at;
	pthread_attr_init(&at);
	pthread_attr_setstacksize(&at, 256 * 1024);
	pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	g_thread = pthread_create(&th, &at, store_thread, nullptr) == 0;
	pthread_attr_destroy(&at);
}

/* The thread of the rounds, made when a store first wants them. Called with g_lock held. */
void rounds_up()
{
	if ( g_rounds ) return;
	pthread_t	th;
	pthread_attr_t	at;
	pthread_attr_init(&at);
	pthread_attr_setstacksize(&at, 256 * 1024);
	pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	g_rounds = pthread_create(&th, &at, rounds_thread, nullptr) == 0;
	pthread_attr_destroy(&at);
}

/* A job for the store's thread. Called with g_lock held. */
void enqueue( int id, bool whole, br_store_delta *d, long since )
{
	thread_up();
	/* behind what waits to be written again: a store's changes stay in order */
	if ( !whole && !g_stores[id].failed.empty() ) {
		g_stores[id].failed_bytes += d->bytes;
		g_stores[id].failed.push_back({ id, whole, d, since });
		return;
	}
	g_jobs.push_back({ id, whole, d, since });
	g_queued++;
	g_cv.notify_all();
}

/* A shared store saved: what changed since this browser last wrote or took it */
void save_shared( int i, long t0 )
{
	Store &s = g_stores[i];
	br_store_delta all;

	s.dirty = 0;
	s.snap(s.ctx, &all);
	std::map<std::string, bool> there;
	for ( const auto &op : all.ops ) there[op.key] = true;
	br_store_delta *d = new br_store_delta();
	d->afresh = true;
	long now = now_ms();
	std::lock_guard<std::mutex> hold(g_lock);
	for ( auto &op : all.ops ) {
		if ( op.kind != kPut ) continue;
		Sum sum = sum_of(op.val);
		auto b = s.base.find(op.key);
		if ( b != s.base.end() && b->second == sum ) continue;
		s.base[op.key] = sum;
		s.handed[op.key] = now;
		d->bytes += kSegHead + op.key.size() + op.val.size();
		d->ops.push_back({ kPut, op.key, std::move(op.val) });
	}
	for ( auto b = s.base.begin(); b != s.base.end(); ) {
		if ( there.count(b->first) != 0 ) {
			++b;
			continue;
		}
		s.handed[b->first] = now;
		d->bytes += kSegHead + b->first.size();
		d->ops.push_back({ kDel, b->first, std::string() });
		b = s.base.erase(b);
	}
	if ( d->ops.empty() ) {
		delete d;
		return;
	}
	enqueue(i, false, d, t0);
}

/*
 * The store 'i' saved: a whole one's bytes made here and handed over
 * unless they are the same as the last; one kept by entries asked for
 * its changes. Called on the application's thread, without g_lock.
 */
void save_one( int i )
{
	Store &s = g_stores[i];
	long t0 = now_ms();

	if ( s.snap != nullptr ) {
		save_shared(i, t0);
		return;
	}
	if ( s.collect != nullptr ) {
		{
			std::lock_guard<std::mutex> hold(g_lock);
			if ( s.asking ) return;		/* answered later; still dirty, asked again then */
			s.dirty = 0;
			s.asking = true;
			g_asked++;
		}
		s.collect(s.ctx);
		return;
	}
	unsigned char *data = nullptr;
	int len = 0;
	s.dirty = 0;
	if ( s.write(s.ctx, &data, &len) != 0 ) return;
	unsigned crc = crc32_of(data, (size_t)len);
	br_store_delta *d = nullptr;
	{
		std::lock_guard<std::mutex> hold(g_lock);
		if ( !( s.saved_any && s.last_crc == crc && s.last_len == len ) ) {
			s.saved_any = true;
			s.last_crc = crc;
			s.last_len = len;
			d = new br_store_delta();
			d->ops.push_back({ kPut, std::string(), std::string((const char *)data, (size_t)len) });
			d->bytes = kSegHead + (size_t)len;
			enqueue(i, true, d, t0);
		}
	}
	if ( data != nullptr && sealed_name(s.name) ) OPENSSL_cleanse(data, (size_t)len);
	free(data);
}

}  // namespace

extern "C" int br_store_register( const char *name, br_store_writer write, void *ctx, unsigned delay_ms )
{
	std::lock_guard<std::mutex> hold(g_lock);
	if ( g_nstores >= kMaxStores ) return -1;
	Store &s = g_stores[g_nstores];
	s.name = name;
	s.write = write;
	s.collect = nullptr;
	s.ctx = ctx;
	s.delay_ms = delay_ms;
	s.dirty = 0;
	return g_nstores++;
}

extern "C" int br_store_register_keyed( const char *name, br_store_collect collect, void *ctx, unsigned delay_ms )
{
	std::lock_guard<std::mutex> hold(g_lock);
	if ( g_nstores >= kMaxStores ) return -1;
	Store &s = g_stores[g_nstores];
	s.name = name;
	s.write = nullptr;
	s.collect = collect;
	s.ctx = ctx;
	s.delay_ms = delay_ms;
	s.dirty = 0;
	return g_nstores++;
}

extern "C" int br_store_register_shared( const char *name, br_store_snapshot snapshot, br_store_take take, void *ctx, unsigned delay_ms )
{
	std::lock_guard<std::mutex> hold(g_lock);
	if ( g_nstores >= kMaxStores ) return -1;
	Store &s = g_stores[g_nstores];
	s.name = name;
	s.write = nullptr;
	s.collect = nullptr;
	s.snap = snapshot;
	s.take = take;
	s.ctx = ctx;
	s.delay_ms = delay_ms;
	s.dirty = 0;
	s.every_ms = 3000;
	s.looked_at = now_ms();
	thread_up();
	rounds_up();
	return g_nstores++;
}

extern "C" int br_store_load_shared( int id, br_store_entry_fn each, void *ctx )
{
	if ( id < 0 || id >= g_nstores ) return -1;
	Store &s = g_stores[id];
	Entries e;
	size_t live = 0;
	int rc = log_load(s.name, &e, &live);
	if ( rc < 0 ) return rc;
	{
		std::lock_guard<std::mutex> hold(g_lock);
		for ( const auto &kv : e ) s.base[kv.first] = sum_of(kv.second);
	}
	for ( const auto &kv : e ) {
		each(ctx, kv.first.data(), (int)kv.first.size(), (const unsigned char *)kv.second.data(), (int)kv.second.size());
	}
	return 0;
}

extern "C" void br_store_on_own_thread( int id )
{
	std::lock_guard<std::mutex> hold(g_lock);
	if ( id < 0 || id >= g_nstores ) return;
	g_stores[id].own_thread = true;
	thread_up();
	rounds_up();
}

extern "C" void br_store_watch( int id, br_store_told told, void *ctx, unsigned every_ms )
{
	std::lock_guard<std::mutex> hold(g_lock);
	if ( id < 0 || id >= g_nstores ) return;
	g_stores[id].told_ctx = ctx;
	g_stores[id].every_ms = every_ms;
	g_stores[id].looked_at = now_ms();
	g_stores[id].told = told;
	thread_up();
	rounds_up();
}

extern "C" void br_store_dirty( int id )
{
	if ( id < 0 || id >= g_nstores || g_stores[id].dirty ) return;
	g_stores[id].dirty_since = now_ms();
	g_stores[id].dirty = 1;
}

extern "C" void br_store_answer( int id, br_store_delta *d )
{
	std::lock_guard<std::mutex> hold(g_lock);
	if ( id >= 0 && id < g_nstores && g_stores[id].asking ) {
		g_stores[id].asking = false;
		g_answered++;
	}
	if ( d != nullptr && !d->ops.empty() && id >= 0 && id < g_nstores ) {
		enqueue(id, false, d, now_ms());
	} else {
		delete d;
	}
	g_cv.notify_all();
}

extern "C" void br_store_submit( int id, br_store_delta *d )
{
	std::lock_guard<std::mutex> hold(g_lock);
	if ( d != nullptr && !d->ops.empty() && id >= 0 && id < g_nstores ) {
		enqueue(id, false, d, now_ms());
	} else {
		delete d;
	}
}

extern "C" void br_store_tick( void )
{
	long now = now_ms();
	{
		std::lock_guard<std::mutex> hold(g_lock);
		for ( int i = 0; i < g_nstores; i++ ) {
			Store &s = g_stores[i];
			if ( s.failed.empty() || now < s.retry_at ) continue;
			printf("browser: store %s: %zu changes not yet written, tried again\n", s.name, s.failed.size());
			while ( !s.failed.empty() ) {
				g_jobs.push_back(s.failed.front());
				s.failed.pop_front();
				g_queued++;
			}
			s.failed_bytes = 0;
			g_cv.notify_all();
		}
	}
	/* what other browsers changed, after this browser's own is on its way */
	for ( int i = 0; i < g_nstores; i++ ) {
		Store &s = g_stores[i];
		if ( s.snap == nullptr ) continue;
		{
			std::lock_guard<std::mutex> hold(g_lock);
			if ( s.incoming.empty() ) continue;
		}
		if ( s.dirty ) save_one(i);
		std::vector<Change> got;
		{
			std::lock_guard<std::mutex> hold(g_lock);
			for ( Change &c : s.incoming ) {
				if ( s.handed.count(c.key) != 0 ) continue;
				if ( c.put ) s.base[c.key] = c.sum;
				else s.base.erase(c.key);
				got.push_back(std::move(c));
			}
			s.incoming.clear();
		}
		for ( const Change &c : got ) {
			s.take(s.ctx, c.key.data(), (int)c.key.size(), c.put ? (const unsigned char *)c.val.data() : nullptr, (int)c.val.size());
		}
	}
	for ( int i = 0; i < g_nstores; i++ ) {
		if ( g_stores[i].own_thread || !g_stores[i].dirty || now - g_stores[i].dirty_since < (long)g_stores[i].delay_ms ) continue;
		save_one(i);
	}
}

extern "C" unsigned br_store_flush_async( void )
{
	for ( int i = 0; i < g_nstores; i++ ) {
		if ( g_stores[i].dirty ) save_one(i);
	}
	std::lock_guard<std::mutex> hold(g_lock);
	return g_asked;
}

extern "C" unsigned br_store_flush_one( int id )
{
	if ( id >= 0 && id < g_nstores && g_stores[id].dirty ) save_one(id);
	std::lock_guard<std::mutex> hold(g_lock);
	return g_asked;
}

extern "C" int br_store_wait( unsigned ticket, int ms )
{
	auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
	std::unique_lock<std::mutex> hold(g_lock);
	/* the answers of the stores asked, then the jobs queued by then */
	if ( !g_cv.wait_until(hold, until, [ticket]() { return (int)( g_answered - ticket ) >= 0; }) ) return 0;
	unsigned queued = g_queued;
	return g_cv.wait_until(hold, until, [queued]() { return (int)( g_written - queued ) >= 0; }) ? 1 : 0;
}

extern "C" void br_store_flush_all( void )
{
	long t0 = now_ms();
	unsigned ticket = br_store_flush_async();
	if ( !br_store_wait(ticket, 60000) ) {
		printf("browser: the stores were not all written in %ld ms\n", now_ms() - t0);
	}
	for ( int i = 0; i < g_nstores; i++ ) {
		std::lock_guard<std::mutex> hold(g_lock);
		if ( !g_stores[i].failed.empty() || g_stores[i].dirty ) {
			printf("browser: store %s: not written when the browser ended\n", g_stores[i].name);
		}
	}
}
