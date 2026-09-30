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
 *	ブラウザ: what the engine keeps between runs, whole, in real
 *	objects (design 17.19).
 *
 *	The cookies, the pages' local storage and the HTTP cache each live
 *	in one object of the system that the learning box links (as the
 *	learning of the kana-kanji converter does): its record 1 holds the
 *	bytes, read whole when the browser starts and written whole a while
 *	after the store changed. The objects have fixed identities
 *	(SYSDEF_BROWSER_*) and are made with the system (etc/def).
 *
 *	The record starts with 12 bytes -- "BRS1", the length of what
 *	follows and its CRC-32, little-endian -- so that a record left half
 *	written reads as none rather than as damaged data.
 *
 *	Stores register a writer and are marked dirty when they change; the
 *	application's loop calls br_store_tick, which saves a store once it
 *	has been dirty for its delay, and br_store_flush_all as it ends.
 */

#include TS_CONFIG_H
#include <stdint.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/uapp.h>
#include <ts/sysdef.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "br_store.h"

namespace {

constexpr int kRec = 1;
constexpr SZ kChunk = 64 * 1024;
constexpr int kHdr = 12;

struct Named {
	const char	*name;
	const char	*uuid;
};

const Named kObjects[] = {
	{ "cookie", SYSDEF_BROWSER_COOKIE },
	{ "storage", SYSDEF_BROWSER_STORAGE },
	{ "cache", SYSDEF_BROWSER_CACHE },
};

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

bool uuid_of( const char *name, TS_UUID *u )
{
	for ( const Named &n : kObjects ) {
		if ( strcmp(n.name, name) == 0 ) return parse_uuid(n.uuid, u);
	}
	return false;
}

unsigned crc32_of( const unsigned char *p, size_t n )
{
	unsigned c = 0xffffffffu;
	for ( size_t i = 0; i < n; i++ ) {
		c ^= p[i];
		for ( int k = 0; k < 8; k++ ) c = ( c >> 1 ) ^ ( 0xedb88320u & ( 0u - ( c & 1u ) ) );
	}
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

/* The size of record 1, -1 when there is none */
long rec_size( ID key )
{
	T_OBREC	r[8];
	INT	cnt = 0;

	if ( ob_lst_rec(key, r, 8, &cnt) < E_OK ) return -1;
	for ( INT i = 0; i < cnt; i++ ) {
		if ( r[i].recno == kRec ) return (long)r[i].size;
	}
	return -1;
}

long now_ms()
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

}  // namespace

extern "C" int br_store_load( const char *name, unsigned char **data, int *len )
{
	TS_UUID	u;

	*data = nullptr;
	*len = 0;
	if ( !uuid_of(name, &u) ) return -1;
	ID key = ob_opn_obj(&u, OB_OP_R);
	if ( key <= 0 ) return -2;
	long size = rec_size(key);
	if ( size < kHdr ) {
		ob_cls_obj(key);
		return -3;
	}
	unsigned char *buf = (unsigned char *)malloc((size_t)size);
	if ( buf == nullptr ) {
		ob_cls_obj(key);
		return -4;
	}
	long off = 0;
	while ( off < size ) {
		SZ want = ( size - off > kChunk ) ? kChunk : (SZ)( size - off ), got = 0;
		if ( ob_rea_rec(key, kRec, (D)off, buf + off, want, &got) < E_OK || got <= 0 ) break;
		off += got;
	}
	ob_cls_obj(key);
	if ( off < size || memcmp(buf, "BRS1", 4) != 0 ) {
		free(buf);
		return -5;
	}
	unsigned plen = get32(buf + 4);
	if ( plen > (unsigned)( size - kHdr ) || crc32_of(buf + kHdr, plen) != get32(buf + 8) ) {
		printf("browser: store %s is damaged, left alone\n", name);
		free(buf);
		return -6;
	}
	memmove(buf, buf + kHdr, plen);
	*data = buf;
	*len = (int)plen;
	return 0;
}

extern "C" int br_store_save( const char *name, const void *data, int len )
{
	TS_UUID	u;

	if ( !uuid_of(name, &u) ) return -1;
	ID key = ob_opn_obj(&u, OB_OP_R | OB_OP_WRITE | OB_OP_RECORD);
	if ( key <= 0 ) return -2;
	if ( rec_size(key) < 0 ) {
		INT recno = -1;
		if ( ob_apd_rec(key, OB_RT_SYSDATA, 0, &recno) < E_OK || recno != kRec ) {
			ob_cls_obj(key);
			return -3;
		}
	}
	unsigned char hdr[kHdr];
	memcpy(hdr, "BRS1", 4);
	put32(hdr + 4, (unsigned)len);
	put32(hdr + 8, crc32_of((const unsigned char *)data, (size_t)len));
	int rc = 0;
	SZ asz = 0;
	if ( ob_wri_rec(key, kRec, 0, hdr, kHdr, &asz) < E_OK || asz != kHdr ) rc = -4;
	for ( long off = 0; rc == 0 && off < len; ) {
		SZ part = ( len - off > kChunk ) ? kChunk : (SZ)( len - off );
		asz = 0;
		if ( ob_wri_rec(key, kRec, (D)( kHdr + off ), (const unsigned char *)data + off, part, &asz) < E_OK
		     || asz != part ) {
			rc = -4;
			break;
		}
		off += part;
	}
	if ( rc == 0 ) (void)ob_trn_rec(key, kRec, (UD)( kHdr + len ));
	ob_cls_obj(key);
	if ( rc < 0 ) printf("browser: writing the store %s failed\n", name);
	return rc;
}

/* ---------------------------------------------------------------- registered stores */

namespace {

constexpr int kMaxStores = 8;

struct Store {
	const char		*name;
	br_store_writer		write;
	void			*ctx;
	unsigned		delay_ms;
	volatile int		dirty;
	long			dirty_since;
};

Store g_stores[kMaxStores];
int g_nstores;

void save_one( Store *s )
{
	unsigned char *data = nullptr;
	int len = 0;
	long t0 = now_ms();

	s->dirty = 0;
	if ( s->write(s->ctx, &data, &len) != 0 ) return;
	int rc = br_store_save(s->name, data, len);
	free(data);
	if ( rc == 0 ) {
		printf("browser: store %s saved, %d bytes in %ld ms\n", s->name, len, now_ms() - t0);
	} else {
		printf("browser: store %s not saved (%d)\n", s->name, rc);
	}
}

}  // namespace

extern "C" int br_store_register( const char *name, br_store_writer write, void *ctx, unsigned delay_ms )
{
	if ( g_nstores >= kMaxStores ) return -1;
	Store &s = g_stores[g_nstores];
	s.name = name;
	s.write = write;
	s.ctx = ctx;
	s.delay_ms = delay_ms;
	s.dirty = 0;
	return g_nstores++;
}

extern "C" void br_store_dirty( int id )
{
	if ( id < 0 || id >= g_nstores || g_stores[id].dirty ) return;
	g_stores[id].dirty_since = now_ms();
	g_stores[id].dirty = 1;
}

extern "C" void br_store_tick( void )
{
	long now = now_ms();
	for ( int i = 0; i < g_nstores; i++ ) {
		if ( !g_stores[i].dirty || now - g_stores[i].dirty_since < (long)g_stores[i].delay_ms ) continue;
		save_one(&g_stores[i]);
	}
}

extern "C" void br_store_flush_all( void )
{
	for ( int i = 0; i < g_nstores; i++ ) {
		if ( g_stores[i].dirty ) save_one(&g_stores[i]);
	}
}
