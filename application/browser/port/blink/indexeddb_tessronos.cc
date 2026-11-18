/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	indexeddb_tessronos.cc
 *	The store behind IndexedDB for the engine (design 17.19).
 *
 *	The page's side of IndexedDB (modules/indexeddb: the requests, the
 *	transactions' life, the keys taken from values, the values' wrapping)
 *	is the engine's own. What it talks to through Mojo -- IDBFactory,
 *	IDBDatabase, IDBTransaction, IDBCursor, which a browser keeps in its
 *	own process on a database file -- is this file: the interfaces are
 *	bound in the process, on the page's thread, when the page asks the
 *	(empty) broker for IDBFactory.
 *
 *	The data is kept in memory: for each origin its databases, for each
 *	database its object stores, for each store its records in the order
 *	of their keys and its indexes. A key is kept as bytes whose order is
 *	the order of the keys (numbers, then dates, strings, binaries and
 *	arrays), so that the maps' own order is the one IndexedDB asks for.
 *	An index entry is the index key's bytes followed by the primary key's.
 *
 *	A transaction that writes takes a copy of its database first and puts
 *	it back when it is aborted. Transactions are not held back for one
 *	another: each request is answered as it comes.
 *
 *	The opens and deletions of a database are done one at a time, in the
 *	order they were asked, as a browser's connection coordinator does
 *	them: an open waits while a version change transaction of the
 *	database runs (so that it is given the stores the upgrade made); an
 *	upgrade and a deletion tell the other connections (versionchange) and
 *	wait until they have closed, telling the asker it is blocked when one
 *	of them ignores it. When a transaction that changed something has
 *	committed, its database is written, two seconds later, to record 2 of the
 *	object ブラウザの保存域 (port/net/br_store.cc), and read back when the
 *	engine starts.
 *
 *	A database is one entry of the store, written only when no transaction
 *	that may write it is running, so that what is kept is always what some
 *	transaction committed. Browsers of windows opened apart are processes of
 *	their own: one that has no connection to a database takes in at once
 *	what another browser wrote of it. With connections, it is taken in when
 *	they have all closed, and they are told (versionchange) when the version
 *	is another or the database is gone; until then this browser's connections
 *	see the database as it was, and what they commit is written only when the
 *	version is still theirs.
 *
 *	A value that carries Blobs or Files (and a large one, which the engine
 *	wraps in a Blob itself) is kept as the numbers of its external objects
 *	in front of its bytes. Each external object is kept apart, by its
 *	number: the Blob the page gave (held, and cloned for each value given
 *	back), its type, size and, for a File, its name and time, and its
 *	bytes, read from the blob store after it is put. The bytes are what is
 *	written with the databases; read back, they are made a Blob again the
 *	first time a value that names them is given back.
 */

#include "base/rand_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "mojo/public/cpp/bindings/associated_receiver.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "mojo/public/cpp/bindings/pending_associated_receiver.h"
#include "mojo/public/cpp/bindings/pending_associated_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_associated_receiver.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "third_party/blink/public/common/indexeddb/indexeddb_key.h"
#include "third_party/blink/public/common/indexeddb/indexeddb_key_path.h"
#include "third_party/blink/public/common/indexeddb/indexeddb_key_range.h"
#include "third_party/blink/public/common/indexeddb/indexeddb_metadata.h"
#include "third_party/blink/public/mojom/blob/blob.mojom.h"
#include "third_party/blink/public/mojom/indexeddb/indexeddb.mojom.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"
#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#include "third_party/blink/renderer/platform/blob/blob_data.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"
#include "../net/br_data.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"

#include "../net/br_store.h"

/* A Blob's bytes, by its UUID, on the services' thread (blob_tessronos.cc) */
void br_blob_read( const std::string &key, bool is_url,
		   base::OnceCallback<void(bool, std::string, std::string)> done );

namespace {

using blink::IndexedDBKey;
using blink::IndexedDBKeyPath;
using blink::IndexedDBKeyRange;
using blink::mojom::IDBException;
using blink::mojom::IDBKeyType;

/* ================================================================ keys as ordered bytes */

void put_double( std::string *out, double d )
{
	if ( d == 0 ) d = 0;			/* -0 is 0 */
	uint64_t bits;
	memcpy(&bits, &d, sizeof(bits));
	bits = ( bits >> 63 ) ? ~bits : ( bits | ( 1ull << 63 ) );
	for ( int i = 7; i >= 0; i-- ) out->push_back((char)( bits >> ( 8 * i ) ));
}

double get_double( const std::string &s, size_t *at )
{
	uint64_t bits = 0;
	for ( int i = 0; i < 8 && *at < s.size(); i++ ) bits = ( bits << 8 ) | (uint8_t)s[( *at )++];
	bits = ( bits >> 63 ) ? ( bits & ~( 1ull << 63 ) ) : ~bits;
	double d;
	memcpy(&d, &bits, sizeof(d));
	return d;
}

/*
 * A type byte, then: a number or a date as 8 bytes whose order is the
 * numbers'; a string's code units and a binary's bytes each after a 1, the
 * end a 0; an array's members one after another, the end a 0 (every
 * member starts above 0, so a shorter array comes first)
 */
void enc_key( const IndexedDBKey &k, std::string *out )
{
	switch ( k.type() ) {
	case IDBKeyType::Number:
		out->push_back(0x10);
		put_double(out, k.number());
		break;
	case IDBKeyType::Date:
		out->push_back(0x20);
		put_double(out, k.date());
		break;
	case IDBKeyType::String:
		out->push_back(0x30);
		for ( char16_t u : k.string() ) {
			out->push_back(1);
			out->push_back((char)( u >> 8 ));
			out->push_back((char)( u & 0xFF ));
		}
		out->push_back(0);
		break;
	case IDBKeyType::Binary:
		out->push_back(0x40);
		for ( char b : k.binary() ) {
			out->push_back(1);
			out->push_back(b);
		}
		out->push_back(0);
		break;
	case IDBKeyType::Array:
		out->push_back(0x50);
		for ( const IndexedDBKey &m : k.array() ) enc_key(m, out);
		out->push_back(0);
		break;
	default:
		break;
	}
}

std::string enc( const IndexedDBKey &k )
{
	std::string s;
	enc_key(k, &s);
	return s;
}

IndexedDBKey dec_at( const std::string &s, size_t *at )
{
	if ( *at >= s.size() ) return IndexedDBKey();
	uint8_t t = (uint8_t)s[( *at )++];
	switch ( t ) {
	case 0x10:
		return IndexedDBKey(get_double(s, at), IDBKeyType::Number);
	case 0x20:
		return IndexedDBKey(get_double(s, at), IDBKeyType::Date);
	case 0x30: {
		std::u16string str;
		while ( *at + 2 < s.size() + 0 && s[*at] == 1 ) {
			char16_t u = (char16_t)( ( (uint8_t)s[*at + 1] << 8 ) | (uint8_t)s[*at + 2] );
			str.push_back(u);
			*at += 3;
		}
		( *at )++;		/* the end */
		return IndexedDBKey(std::move(str));
	}
	case 0x40: {
		std::string bin;
		while ( *at + 1 < s.size() && s[*at] == 1 ) {
			bin.push_back(s[*at + 1]);
			*at += 2;
		}
		( *at )++;
		return IndexedDBKey(std::move(bin));
	}
	case 0x50: {
		IndexedDBKey::KeyArray arr;
		while ( *at < s.size() && s[*at] != 0 ) arr.push_back(dec_at(s, at));
		( *at )++;
		return IndexedDBKey(std::move(arr));
	}
	default:
		return IndexedDBKey();
	}
}

IndexedDBKey dec( const std::string &s )
{
	size_t at = 0;
	return dec_at(s, &at);
}

bool is_key( const IndexedDBKey &k )
{
	return k.type() == IDBKeyType::Number || k.type() == IDBKeyType::Date || k.type() == IDBKeyType::String
	       || k.type() == IDBKeyType::Binary || k.type() == IDBKeyType::Array;
}

/* A range's bounds as bytes; an empty bound is open to that end */
struct Range {
	std::string	lower, upper;
	bool		has_lower = false, has_upper = false;
	bool		lower_open = false, upper_open = false;

	explicit Range( const IndexedDBKeyRange &r )
	{
		if ( is_key(r.lower()) ) {
			has_lower = true;
			lower = enc(r.lower());
			lower_open = r.lower_open();
		}
		if ( is_key(r.upper()) ) {
			has_upper = true;
			upper = enc(r.upper());
			upper_open = r.upper_open();
		}
	}
	Range() = default;

	bool above_lower( const std::string &k ) const
	{
		return !has_lower || ( lower_open ? k > lower : k >= lower );
	}
	bool below_upper( const std::string &k ) const
	{
		return !has_upper || ( upper_open ? k < upper : k <= upper );
	}
	bool has( const std::string &k ) const { return above_lower(k) && below_upper(k); }
};

/* ================================================================ the data */

struct Index {
	/* index key + primary key -> the length of the index key */
	std::map<std::string, size_t>			entries;
	/* primary key -> its entries */
	std::map<std::string, std::set<std::string>>	of_primary;
};

struct Store {
	std::map<std::string, std::string>	records;	/* key -> the value's bytes */
	int64_t					key_gen = 0;	/* the last number generated */
	std::map<int64_t, Index>		indexes;
};

struct Data {
	blink::IndexedDBDatabaseMetadata	meta;
	std::map<int64_t, Store>		stores;
};

class Connection;

struct Database {
	Data			data;
	std::set<Connection *>	connections;
	bool			wrote = false;	/* committed here since it was last written */
};

/* An open or a deletion of a database asked, waiting for its turn */
struct Request {
	bool							is_delete = false;
	mojo::AssociatedRemote<blink::mojom::IDBFactoryClient>	client;
	mojo::PendingAssociatedRemote<blink::mojom::IDBDatabaseCallbacks> callbacks;
	int64_t							version = 0;
	mojo::PendingAssociatedReceiver<blink::mojom::IDBTransaction> version_change;
	int64_t							transaction_id = 0;
	int64_t							old_version = 0;
	bool							told = false;	/* the others told of it */
	bool							blocked = false;	/* the asker told it is blocked */
};

/* The requests of a database, by origin and name, and whether its version change transaction runs */
struct Coordinator {
	std::deque<std::unique_ptr<Request>>	queue;
	bool					upgrading = false;
};

std::map<std::pair<std::string, std::u16string>, Coordinator> &coordinators()
{
	static auto *m = new std::map<std::pair<std::string, std::u16string>, Coordinator>();
	return *m;
}

/* The requests of a database gone through as far as they can go now (the factory below) */
void idb_process( const std::string &origin, const std::u16string &name );
void idb_process_later( const std::string &origin, const std::u16string &name );
/* What another browser wrote of a database, taken in when its last connection here has gone */
void idb_take_waiting( const std::string &origin, const std::u16string &name );

/* origin -> name -> database */
std::map<std::string, std::map<std::u16string, Database>> &origins()
{
	static auto *o = new std::map<std::string, std::map<std::u16string, Database>>();
	return *o;
}

int g_store_id = -1;
/* The page's thread, where the databases are served (br_idb_init) */
scoped_refptr<base::SingleThreadTaskRunner> *g_idb_runner;

void changed()
{
	br_store_dirty(g_store_id);
}

blink::mojom::IDBErrorPtr error( IDBException code, const char *message )
{
	std::u16string m;
	for ( const char *p = message; *p != '\0'; p++ ) m.push_back((char16_t)*p);
	return blink::mojom::IDBError::New(code, m);
}

/* ================================================================ Blobs and Files in values */

/* An external object of values, by its number */
struct Extern {
	mojo::Remote<blink::mojom::Blob>	blob;		/* held: the page's, or one made of the bytes */
	scoped_refptr<blink::BlobDataHandle>	made;		/* the one made of the bytes, kept alive */
	std::u16string				type;
	int64_t					size = 0;
	bool					is_file = false;
	std::u16string				name;
	int64_t					modified_us = 0;	/* a File's time, microseconds since 1601 */
	bool					have_bytes = false;
	std::string				bytes;
};

std::mutex			g_ext_lock;	/* the bytes come on the services' thread */
std::map<uint64_t, Extern>	g_ext;
uint64_t			g_ext_last;

/* A record's bytes: "\0BX1", the count of external objects and their numbers, then the value's bytes */
constexpr char kExtTag[4] = { 0, 'B', 'X', '1' };

std::string tag_value( const std::vector<uint64_t> &ids, const std::string &bits )
{
	if ( ids.empty() ) return bits;
	std::string o(kExtTag, 4);
	for ( int i = 0; i < 4; i++ ) o.push_back((char)( ids.size() >> ( 8 * i ) ));
	for ( uint64_t id : ids ) {
		for ( int i = 0; i < 8; i++ ) o.push_back((char)( id >> ( 8 * i ) ));
	}
	return o + bits;
}

/* The numbers of a record's external objects, and where its value's bytes begin */
std::vector<uint64_t> ext_ids( const std::string &rec, size_t *at )
{
	std::vector<uint64_t> ids;
	*at = 0;
	if ( rec.size() < 8 || memcmp(rec.data(), kExtTag, 4) != 0 ) return ids;
	uint32_t n = 0;
	for ( int i = 0; i < 4; i++ ) n |= (uint32_t)(uint8_t)rec[4 + i] << ( 8 * i );
	if ( rec.size() < 8 + (size_t)n * 8 ) return ids;
	for ( uint32_t k = 0; k < n; k++ ) {
		uint64_t id = 0;
		for ( int i = 0; i < 8; i++ ) id |= (uint64_t)(uint8_t)rec[8 + k * 8 + i] << ( 8 * i );
		ids.push_back(id);
	}
	*at = 8 + (size_t)n * 8;
	return ids;
}

void changed();

/*
 * The external objects of a value put, kept by number (on the page's
 * thread): their Blobs held, their bytes read afterwards; false for one
 * that is not a Blob or a File
 */
bool keep_externals( std::vector<blink::mojom::IDBExternalObjectPtr> &objs, std::vector<uint64_t> *ids )
{
	for ( auto &o : objs ) {
		if ( !o->is_blob_or_file() ) return false;
	}
	for ( auto &o : objs ) {
		blink::mojom::IDBBlobInfoPtr &bi = o->get_blob_or_file();
		uint64_t id = ++g_ext_last;
		Extern e;
		e.type = bi->mime_type;
		e.size = bi->size;
		if ( bi->file ) {
			e.is_file = true;
			e.name = bi->file->name;
			e.modified_us = bi->file->last_modified.ToDeltaSinceWindowsEpoch().InMicroseconds();
		}
		e.blob.Bind(std::move(bi->blob));
		{
			std::lock_guard<std::mutex> hold(g_ext_lock);
			g_ext[id] = std::move(e);
			g_ext[id].blob->GetInternalUUID(base::BindOnce([]( uint64_t id, const std::string &uuid ) {
				br_blob_read(uuid, false, base::BindOnce([]( uint64_t id, bool ok, std::string bytes, std::string ) {
					std::lock_guard<std::mutex> hold(g_ext_lock);
					auto it = g_ext.find(id);
					if ( it == g_ext.end() || !ok ) return;
					it->second.bytes = std::move(bytes);
					it->second.have_bytes = true;
				}, id));
			}, id));
		}
		ids->push_back(id);
	}
	return true;
}

/* An external object given back with a value: a clone of its Blob, made of its bytes if it has none */
blink::mojom::IDBExternalObjectPtr external_of( uint64_t id )
{
	std::lock_guard<std::mutex> hold(g_ext_lock);
	auto it = g_ext.find(id);
	if ( it == g_ext.end() ) return nullptr;
	Extern &e = it->second;
	if ( !e.blob.is_bound() && e.have_bytes ) {
		auto data = std::make_unique<blink::BlobData>();
		data->SetContentType(blink::String::FromUtf8(base::UTF16ToUTF8(e.type)));
		data->AppendBytes(base::as_byte_span(e.bytes));
		e.made = blink::BlobDataHandle::Create(std::move(data), e.bytes.size());
		e.blob.Bind(mojo::PendingRemote<blink::mojom::Blob>(e.made->CloneBlobRemote().PassPipe(), 0));
	}
	if ( !e.blob.is_bound() ) return nullptr;
	auto bi = blink::mojom::IDBBlobInfo::New();
	e.blob->Clone(bi->blob.InitWithNewPipeAndPassReceiver());
	bi->mime_type = e.type;
	bi->size = e.size;
	if ( e.is_file ) {
		bi->file = blink::mojom::IDBFileInfo::New(
			e.name, base::Time::FromDeltaSinceWindowsEpoch(base::Microseconds(e.modified_us)));
	}
	return blink::mojom::IDBExternalObject::NewBlobOrFile(std::move(bi));
}

blink::mojom::IDBValuePtr value_of( const std::string &rec )
{
	auto v = blink::mojom::IDBValue::New();
	size_t at = 0;
	std::vector<uint64_t> ids = ext_ids(rec, &at);
	v->bits = mojo_base::BigBuffer(base::as_byte_span(rec).subspan(at));
	for ( uint64_t id : ids ) {
		blink::mojom::IDBExternalObjectPtr o = external_of(id);
		if ( o ) v->external_objects.push_back(std::move(o));
	}
	return v;
}

/* The primary key a value is given back with when its store makes keys and puts them in the value */
void inject( const blink::IndexedDBObjectStoreMetadata &m, const std::string &pk, IndexedDBKey *key,
	     IndexedDBKeyPath *path )
{
	if ( m.auto_increment && !m.key_path.IsNull() ) {
		*key = dec(pk);
		*path = m.key_path;
	} else {
		*key = IndexedDBKey();
		*path = IndexedDBKeyPath();
	}
}

/* ================================================================ index entries */

void index_remove_primary( Index &ix, const std::string &pk )
{
	auto it = ix.of_primary.find(pk);
	if ( it == ix.of_primary.end() ) return;
	for ( const std::string &e : it->second ) ix.entries.erase(e);
	ix.of_primary.erase(it);
}

/* Whether a unique index has the key for another primary key */
bool index_taken( const Index &ix, const std::string &ik, const std::string &pk )
{
	for ( auto it = ix.entries.lower_bound(ik); it != ix.entries.end(); ++it ) {
		if ( it->second != ik.size() || it->first.compare(0, ik.size(), ik) != 0 ) break;
		if ( it->first.compare(ik.size(), std::string::npos, pk) != 0 ) return true;
	}
	return false;
}

void index_add( Index &ix, const std::string &ik, const std::string &pk )
{
	std::string e = ik + pk;
	ix.entries[e] = ik.size();
	ix.of_primary[pk].insert(e);
}

/* ================================================================ cursors */

/* Bytes after every entry that starts with the key k (a key is never the start of another) */
std::string past( const std::string &k )
{
	return k + (char)0xFF;
}

/* Of two places in a map, the one further on, and the one nearer the start */
template <typename M>
typename M::iterator later( M &m, typename M::iterator a, typename M::iterator b )
{
	if ( a == m.end() || b == m.end() ) return m.end();
	return ( a->first < b->first ) ? b : a;
}

template <typename M>
typename M::iterator earlier( M &m, typename M::iterator a, typename M::iterator b )
{
	if ( a == m.end() ) return b;
	if ( b == m.end() ) return a;
	return ( a->first < b->first ) ? a : b;
}

/* A database by its origin and name: found again each time, as it may have been deleted */
struct DbRef {
	std::string	origin;
	std::u16string	name;

	Store *store( int64_t id ) const
	{
		auto o = origins().find(origin);
		if ( o == origins().end() ) return nullptr;
		auto d = o->second.find(name);
		if ( d == o->second.end() ) return nullptr;
		auto it = d->second.data.stores.find(id);
		return ( it == d->second.data.stores.end() ) ? nullptr : &it->second;
	}
};

/*
 * A position in a store (its records) or an index (its entries): the
 * entry's bytes, the key the range and the order look at, and the
 * primary key
 */
struct Pos {
	std::string	entry, key, primary;
};

class Cursor final : public blink::mojom::IDBCursor {
public:
	Cursor( DbRef db, int64_t store, int64_t index, Range range,
		blink::mojom::IDBCursorDirection dir, bool key_only )
		: db_(std::move(db)), store_(store), index_(index), range_(std::move(range)), dir_(dir),
		  key_only_(key_only) {}

	/* The first position; false when there is none */
	bool first( Pos *p );
	void make_result( const Pos &p, IndexedDBKey *key, IndexedDBKey *primary, blink::mojom::IDBValuePtr *value );

	void Advance( uint32_t count, AdvanceCallback callback ) override
	{
		Pos p = cur_;
		for ( uint32_t i = 0; i < count; i++ ) {
			if ( !step(p, nullptr, nullptr, &p) ) {
				std::move(callback).Run(blink::mojom::IDBCursorResult::NewEmpty(true));
				return;
			}
		}
		cur_ = p;
		std::move(callback).Run(result_of({ p }));
	}

	void Continue( IndexedDBKey key, IndexedDBKey primary_key, ContinueCallback callback ) override
	{
		std::string k = is_key(key) ? enc(key) : std::string();
		std::string pk = is_key(primary_key) ? enc(primary_key) : std::string();
		Pos p;
		if ( !step(cur_, k.empty() ? nullptr : &k, pk.empty() ? nullptr : &pk, &p) ) {
			std::move(callback).Run(blink::mojom::IDBCursorResult::NewEmpty(true));
			return;
		}
		cur_ = p;
		std::move(callback).Run(result_of({ p }));
	}

	void Prefetch( int32_t count, PrefetchCallback callback ) override
	{
		std::vector<Pos> got;
		prefetch_from_ = cur_;
		Pos p = cur_;
		for ( int32_t i = 0; i < count; i++ ) {
			if ( !step(p, nullptr, nullptr, &p) ) break;
			got.push_back(p);
		}
		if ( got.empty() ) {
			std::move(callback).Run(blink::mojom::IDBCursorResult::NewEmpty(true));
			return;
		}
		prefetched_ = got;
		cur_ = got.back();
		std::move(callback).Run(result_of(got));
	}

	void PrefetchReset( int32_t used ) override
	{
		if ( used <= 0 || prefetched_.empty() ) {
			cur_ = prefetch_from_;
		} else {
			cur_ = prefetched_[std::min((size_t)used, prefetched_.size()) - 1];
		}
		prefetched_.clear();
	}

	/* The next position from the current one */
	bool step_next( Pos *out ) { return step(cur_, nullptr, nullptr, out); }

	Pos	cur_;

private:
	DbRef		db_;
	int64_t		store_, index_;
	Range		range_;
	blink::mojom::IDBCursorDirection dir_;
	bool		key_only_;
	std::vector<Pos> prefetched_;
	Pos		prefetch_from_;

	bool step( const Pos &from, const std::string *key, const std::string *primary, Pos *out );
	blink::mojom::IDBCursorResultPtr result_of( const std::vector<Pos> &ps );
};

/* ================================================================ transactions and connections */

class Transaction;

class Connection final : public blink::mojom::IDBDatabase {
public:
	Connection( std::string origin, std::u16string name,
		    mojo::PendingAssociatedRemote<blink::mojom::IDBDatabaseCallbacks> callbacks )
		: origin_(std::move(origin)), name_(std::move(name)), callbacks_(std::move(callbacks))
	{
		if ( Database *d = db() ) d->connections.insert(this);
	}
	~Connection() override;
	bool writing() const;		/* a transaction of it that may write is running */

	Database *db()
	{
		auto o = origins().find(origin_);
		if ( o == origins().end() ) return nullptr;
		auto d = o->second.find(name_);
		return ( d == o->second.end() ) ? nullptr : &d->second;
	}

	Store *store( int64_t id )
	{
		Database *d = db();
		if ( d == nullptr ) return nullptr;
		auto it = d->data.stores.find(id);
		return ( it == d->data.stores.end() ) ? nullptr : &it->second;
	}

	const blink::IndexedDBObjectStoreMetadata *store_meta( int64_t id )
	{
		Database *d = db();
		if ( d == nullptr ) return nullptr;
		auto it = d->data.meta.object_stores.find(id);
		return ( it == d->data.meta.object_stores.end() ) ? nullptr : &it->second;
	}

	/* Bound to its end of the pipe; deleted when the page lets it go */
	void bind( mojo::PendingAssociatedReceiver<blink::mojom::IDBDatabase> r )
	{
		receiver_.Bind(std::move(r));
		receiver_.set_disconnect_handler(base::BindOnce([]( Connection *c ) { delete c; }, this));
	}

	blink::mojom::IDBDatabaseCallbacks *callbacks() { return callbacks_.get(); }

	DbRef ref() const { return DbRef{ origin_, name_ }; }

	/* The database removed; this connection then looks at nothing */
	void forget()
	{
		auto o = origins().find(origin_);
		if ( o != origins().end() ) o->second.erase(name_);
	}

	/* The transactions under way, by their numbers */
	std::map<int64_t, Transaction *>	transactions_;

	/* ------------------------------------------------ mojom::IDBDatabase */

	void RenameObjectStore( int64_t, int64_t store_id, const std::u16string &name ) override
	{
		if ( Database *d = db() ) {
			auto it = d->data.meta.object_stores.find(store_id);
			if ( it != d->data.meta.object_stores.end() ) it->second.name = name;
		}
	}

	void CreateTransaction( mojo::PendingAssociatedReceiver<blink::mojom::IDBTransaction> r,
				int64_t id, const std::vector<int64_t> &, blink::mojom::IDBTransactionMode mode,
				blink::mojom::IDBTransactionDurability ) override;

	/* the page kept this connection open over a versionchange: the one that waits for it is told it is blocked */
	void VersionChangeIgnored() override
	{
		auto co = coordinators().find({ origin_, name_ });
		if ( co == coordinators().end() || co->second.queue.empty() ) return;
		Request &r = *co->second.queue.front();
		if ( r.told && !r.blocked && r.client.is_bound() ) {
			r.blocked = true;
			r.client->Blocked(r.old_version);
		}
	}

	void Get( int64_t, int64_t store_id, int64_t index_id, IndexedDBKeyRange key_range, bool key_only,
		  GetCallback callback ) override
	{
		Store *s = store(store_id);
		const blink::IndexedDBObjectStoreMetadata *m = store_meta(store_id);
		if ( s == nullptr || m == nullptr ) {
			std::move(callback).Run(blink::mojom::IDBDatabaseGetResult::NewErrorResult(
				error(IDBException::kUnknownError, "No such object store.")));
			return;
		}
		Range r(key_range);
		std::string pk;
		if ( index_id == blink::IndexedDBIndexMetadata::kInvalidId ) {
			auto it = r.has_lower ? s->records.lower_bound(r.lower) : s->records.begin();
			if ( it != s->records.end() && !r.above_lower(it->first) ) ++it;
			if ( it == s->records.end() || !r.below_upper(it->first) ) {
				std::move(callback).Run(blink::mojom::IDBDatabaseGetResult::NewEmpty(true));
				return;
			}
			pk = it->first;
		} else {
			auto ix = s->indexes.find(index_id);
			if ( ix == s->indexes.end() ) {
				std::move(callback).Run(blink::mojom::IDBDatabaseGetResult::NewEmpty(true));
				return;
			}
			bool found = false;
			for ( auto it = r.has_lower ? ix->second.entries.lower_bound(r.lower) : ix->second.entries.begin();
			      it != ix->second.entries.end(); ++it ) {
				std::string ik = it->first.substr(0, it->second);
				if ( !r.above_lower(ik) ) continue;
				if ( !r.below_upper(ik) ) break;
				pk = it->first.substr(it->second);
				found = true;
				break;
			}
			if ( !found ) {
				std::move(callback).Run(blink::mojom::IDBDatabaseGetResult::NewEmpty(true));
				return;
			}
		}
		if ( key_only ) {
			std::move(callback).Run(blink::mojom::IDBDatabaseGetResult::NewKey(dec(pk)));
			return;
		}
		auto rv = blink::mojom::IDBReturnValue::New();
		rv->value = value_of(s->records[pk]);
		inject(*m, pk, &rv->primary_key, &rv->key_path);
		std::move(callback).Run(blink::mojom::IDBDatabaseGetResult::NewValue(std::move(rv)));
	}

	void GetAll( int64_t, int64_t store_id, int64_t index_id, IndexedDBKeyRange key_range,
		     blink::mojom::IDBGetAllResultType type, uint32_t max_count,
		     blink::mojom::IDBCursorDirection direction, GetAllCallback callback ) override
	{
		std::vector<blink::mojom::IDBRecordPtr> out;
		const blink::IndexedDBObjectStoreMetadata *m = store_meta(store_id);
		if ( store(store_id) == nullptr || m == nullptr ) {
			std::move(callback).Run(std::move(out), mojo::PendingAssociatedReceiver<
						blink::mojom::IDBDatabaseGetAllResultSink>());
			return;
		}
		Cursor c(ref(), store_id, index_id, Range(key_range), direction, false);
		Pos p;
		bool more = c.first(&p);
		while ( more && ( max_count == 0 || out.size() < max_count ) ) {
			auto rec = blink::mojom::IDBRecord::New();
			bool keys = ( type == blink::mojom::IDBGetAllResultType::Keys );
			bool values = ( type == blink::mojom::IDBGetAllResultType::Values );
			if ( !values ) rec->primary_key = dec(p.primary);
			if ( !keys ) {
				auto rv = blink::mojom::IDBReturnValue::New();
				rv->value = value_of(store(store_id)->records[p.primary]);
				inject(*m, p.primary, &rv->primary_key, &rv->key_path);
				rec->return_value = std::move(rv);
			}
			if ( type == blink::mojom::IDBGetAllResultType::Records
			     && index_id != blink::IndexedDBIndexMetadata::kInvalidId ) {
				rec->index_key = dec(p.key);
			}
			out.push_back(std::move(rec));
			c.cur_ = p;
			more = c.step_next(&p);
		}
		std::move(callback).Run(std::move(out),
					mojo::PendingAssociatedReceiver<blink::mojom::IDBDatabaseGetAllResultSink>());
	}

	void OpenCursor( int64_t, int64_t store_id, int64_t index_id, IndexedDBKeyRange key_range,
			 blink::mojom::IDBCursorDirection direction, bool key_only, blink::mojom::IDBTaskType,
			 OpenCursorCallback callback ) override
	{
		if ( store(store_id) == nullptr ) {
			std::move(callback).Run(blink::mojom::IDBDatabaseOpenCursorResult::NewErrorResult(
				error(IDBException::kUnknownError, "No such object store.")));
			return;
		}
		auto c = std::make_unique<Cursor>(ref(), store_id, index_id, Range(key_range), direction, key_only);
		Pos p;
		if ( !c->first(&p) ) {
			std::move(callback).Run(blink::mojom::IDBDatabaseOpenCursorResult::NewEmpty(true));
			return;
		}
		c->cur_ = p;
		auto v = blink::mojom::IDBDatabaseOpenCursorValue::New();
		blink::mojom::IDBValuePtr value;
		c->make_result(p, &v->key, &v->primary_key, &value);
		v->value = std::move(value);
		mojo::PendingAssociatedRemote<blink::mojom::IDBCursor> remote;
		mojo::MakeSelfOwnedAssociatedReceiver(std::move(c), remote.InitWithNewEndpointAndPassReceiver());
		v->cursor = std::move(remote);
		std::move(callback).Run(blink::mojom::IDBDatabaseOpenCursorResult::NewValue(std::move(v)));
	}

	void Count( int64_t, int64_t store_id, int64_t index_id, IndexedDBKeyRange key_range,
		    CountCallback callback ) override
	{
		if ( store(store_id) == nullptr ) {
			std::move(callback).Run(false, 0);
			return;
		}
		Cursor c(ref(), store_id, index_id, Range(key_range), blink::mojom::IDBCursorDirection::Next, true);
		Pos p;
		uint32_t n = 0;
		for ( bool more = c.first(&p); more; more = c.step_next(&p) ) {
			n++;
			c.cur_ = p;
		}
		std::move(callback).Run(true, n);
	}

	void DeleteRange( int64_t txn, int64_t store_id, IndexedDBKeyRange key_range,
			  DeleteRangeCallback callback ) override;

	void GetKeyGeneratorCurrentNumber( int64_t, int64_t store_id,
					   GetKeyGeneratorCurrentNumberCallback callback ) override
	{
		Store *s = store(store_id);
		if ( s == nullptr ) {
			std::move(callback).Run(-1, error(IDBException::kUnknownError, "No such object store."));
			return;
		}
		std::move(callback).Run(s->key_gen + 1, nullptr);
	}

	void Clear( int64_t txn, int64_t store_id, ClearCallback callback ) override;
	void CreateIndex( int64_t txn, int64_t store_id, const blink::IndexedDBIndexMetadata &index ) override;
	void DeleteIndex( int64_t txn, int64_t store_id, int64_t index_id ) override;

	void RenameIndex( int64_t, int64_t store_id, int64_t index_id, const std::u16string &name ) override
	{
		if ( Database *d = db() ) {
			auto it = d->data.meta.object_stores.find(store_id);
			if ( it == d->data.meta.object_stores.end() ) return;
			auto ix = it->second.indexes.find(index_id);
			if ( ix != it->second.indexes.end() ) ix->second.name = name;
		}
	}

	void Abort( int64_t transaction_id ) override;
	void DidBecomeInactive() override {}
	void UpdatePriority( int32_t ) override {}

private:
	std::string	origin_;
	std::u16string	name_;
	mojo::AssociatedRemote<blink::mojom::IDBDatabaseCallbacks>	callbacks_;
	mojo::AssociatedReceiver<blink::mojom::IDBDatabase>		receiver_{this};

	/* A write made in transaction 'txn': its copy for an abort taken first */
	void writing( int64_t txn );
	friend class Transaction;
	friend class Cursor;
};

/* An open that waits for its version change transaction to end */
struct PendingOpen {
	mojo::AssociatedRemote<blink::mojom::IDBFactoryClient>	client;
	int64_t							old_version;
	std::string						origin;		/* the database's, whose next request then goes on */
	std::u16string						name;
};

/* The version change transaction of an open ended: the database's next request goes on */
void upgrade_ended( const PendingOpen &open )
{
	auto co = coordinators().find({ open.origin, open.name });
	if ( co != coordinators().end() ) co->second.upgrading = false;
	idb_process_later(open.origin, open.name);
}

class Transaction final : public blink::mojom::IDBTransaction {
public:
	Transaction( Connection *c, int64_t id, blink::mojom::IDBTransactionMode mode )
		: conn_(c), id_(id), mode_(mode)
	{
		c->transactions_[id] = this;
	}
	~Transaction() override
	{
		if ( conn_ != nullptr ) conn_->transactions_.erase(id_);
	}
	bool writes() const { return !ended_ && mode_ != blink::mojom::IDBTransactionMode::ReadOnly; }

	void bind( mojo::PendingAssociatedReceiver<blink::mojom::IDBTransaction> r )
	{
		receiver_.Bind(std::move(r));
		/* let go without a commit: taken back */
		receiver_.set_disconnect_handler(base::BindOnce([]( Transaction *t ) {
			if ( !t->ended_ ) t->abort(false);
			delete t;
		}, this));
	}

	/* The copy an abort puts back, taken before the first write */
	void save()
	{
		if ( saved_ != nullptr || conn_ == nullptr ) return;
		Database *d = conn_->db();
		if ( d != nullptr ) saved_ = std::make_unique<Data>(d->data);
	}

	/* Taken back; the page told unless it asked for it without an answer */
	void abort( bool tell )
	{
		if ( ended_ ) return;
		ended_ = true;
		if ( conn_ != nullptr && saved_ != nullptr ) {
			if ( Database *d = conn_->db() ) {
				d->data = std::move(*saved_);
				/* a database whose first upgrade was taken back was never made */
				if ( d->data.meta.version == blink::IndexedDBDatabaseMetadata::NO_VERSION ) {
					conn_->forget();
				}
			}
		}
		saved_.reset();
		if ( tell && conn_ != nullptr && conn_->callbacks() != nullptr ) {
			conn_->callbacks()->Abort(id_, IDBException::kAbortError, std::u16string());
		}
		if ( open_ != nullptr ) {
			open_->client->Error(IDBException::kAbortError,
					     u"Version change transaction was aborted in upgradeneeded event handler.");
			upgrade_ended(*open_);
			open_.reset();
		}
	}

	void detach() { conn_ = nullptr; }

	/* ------------------------------------------------ mojom::IDBTransaction */

	void CreateObjectStore( int64_t store_id, const std::u16string &name, const IndexedDBKeyPath &key_path,
				bool auto_increment ) override
	{
		Database *d = conn_ != nullptr ? conn_->db() : nullptr;
		if ( d == nullptr ) return;
		save();
		blink::IndexedDBObjectStoreMetadata m(name, store_id, key_path, auto_increment);
		d->data.meta.object_stores[store_id] = m;
		if ( store_id > d->data.meta.max_object_store_id ) d->data.meta.max_object_store_id = store_id;
		d->data.stores[store_id] = Store();
		wrote_ = true;
	}

	void DeleteObjectStore( int64_t store_id ) override
	{
		Database *d = conn_ != nullptr ? conn_->db() : nullptr;
		if ( d == nullptr ) return;
		save();
		d->data.meta.object_stores.erase(store_id);
		d->data.stores.erase(store_id);
		wrote_ = true;
	}

	void Put( int64_t store_id, blink::mojom::IDBValuePtr value, IndexedDBKey key, blink::mojom::IDBPutMode mode,
		  std::vector<blink::IndexedDBIndexKeys> index_keys, PutCallback callback ) override
	{
		Store *s = conn_ != nullptr ? conn_->store(store_id) : nullptr;
		const blink::IndexedDBObjectStoreMetadata *m = conn_ != nullptr ? conn_->store_meta(store_id) : nullptr;
		if ( s == nullptr || m == nullptr ) {
			std::move(callback).Run(blink::mojom::IDBTransactionPutResult::NewErrorResult(
				error(IDBException::kUnknownError, "No such object store.")));
			return;
		}
		std::vector<uint64_t> ext;
		if ( !keep_externals(value->external_objects, &ext) ) {
			std::move(callback).Run(blink::mojom::IDBTransactionPutResult::NewErrorResult(
				error(IDBException::kDataError, "File System Access handles are not stored here.")));
			return;
		}
		save();
		/* a key made by the store, or a number given that moves its generator on */
		if ( m->auto_increment && !is_key(key) ) {
			if ( s->key_gen >= ( 1ll << 53 ) ) {
				std::move(callback).Run(blink::mojom::IDBTransactionPutResult::NewErrorResult(
					error(IDBException::kConstraintError, "The key generator is used up.")));
				return;
			}
			key = IndexedDBKey((double)( ++s->key_gen ), IDBKeyType::Number);
		} else if ( m->auto_increment && key.type() == IDBKeyType::Number && key.number() > (double)s->key_gen ) {
			s->key_gen = key.number() >= 9007199254740992.0 ? ( 1ll << 53 ) : (int64_t)key.number();
		}
		if ( !is_key(key) ) {
			std::move(callback).Run(blink::mojom::IDBTransactionPutResult::NewErrorResult(
				error(IDBException::kDataError, "No key.")));
			return;
		}
		std::string pk = enc(key);
		if ( mode == blink::mojom::IDBPutMode::AddOnly && s->records.count(pk) != 0 ) {
			std::move(callback).Run(blink::mojom::IDBTransactionPutResult::NewErrorResult(
				error(IDBException::kConstraintError, "Key already exists in the object store.")));
			return;
		}
		/* the unique indexes checked before anything is changed */
		for ( const blink::IndexedDBIndexKeys &ik : index_keys ) {
			auto meta = m->indexes.find(ik.id);
			auto ix = s->indexes.find(ik.id);
			if ( meta == m->indexes.end() || ix == s->indexes.end() || !meta->second.unique ) continue;
			for ( const IndexedDBKey &k : ik.keys ) {
				if ( is_key(k) && index_taken(ix->second, enc(k), pk) ) {
					std::move(callback).Run(blink::mojom::IDBTransactionPutResult::NewErrorResult(
						error(IDBException::kConstraintError,
						      "Unable to add key to index: at least one key does not "
						      "satisfy the uniqueness requirements.")));
					return;
				}
			}
		}
		s->records[pk] = tag_value(ext, std::string(reinterpret_cast<const char *>(value->bits.data()),
							   value->bits.size()));
		for ( auto &ix : s->indexes ) index_remove_primary(ix.second, pk);
		for ( const blink::IndexedDBIndexKeys &ik : index_keys ) {
			auto ix = s->indexes.find(ik.id);
			if ( ix == s->indexes.end() ) continue;
			for ( const IndexedDBKey &k : ik.keys ) {
				if ( is_key(k) ) index_add(ix->second, enc(k), pk);
			}
		}
		wrote_ = true;
		std::move(callback).Run(blink::mojom::IDBTransactionPutResult::NewKey(std::move(key)));
	}

	/* The entries of an index made for the records already there (after CreateIndex) */
	void SetIndexKeys( int64_t store_id, IndexedDBKey primary_key, blink::IndexedDBIndexKeys index_keys ) override
	{
		Store *s = conn_ != nullptr ? conn_->store(store_id) : nullptr;
		const blink::IndexedDBObjectStoreMetadata *m = conn_ != nullptr ? conn_->store_meta(store_id) : nullptr;
		if ( s == nullptr || m == nullptr || !is_key(primary_key) ) return;
		auto ix = s->indexes.find(index_keys.id);
		auto meta = m->indexes.find(index_keys.id);
		if ( ix == s->indexes.end() || meta == m->indexes.end() ) return;
		save();
		std::string pk = enc(primary_key);
		for ( const IndexedDBKey &k : index_keys.keys ) {
			if ( !is_key(k) ) continue;
			std::string ik = enc(k);
			if ( meta->second.unique && index_taken(ix->second, ik, pk) ) {
				/* the index cannot be made: the transaction fails */
				if ( conn_->callbacks() != nullptr ) {
					conn_->callbacks()->Abort(id_, IDBException::kConstraintError,
								  u"Unable to add key to index: at least one key does "
								  u"not satisfy the uniqueness requirements.");
				}
				abort(false);
				return;
			}
			index_add(ix->second, ik, pk);
		}
		wrote_ = true;
	}

	void SetIndexKeysDone() override {}

	void Commit( int64_t ) override
	{
		if ( ended_ ) return;
		ended_ = true;
		saved_.reset();
		if ( wrote_ || open_ != nullptr ) {
			if ( conn_ != nullptr && conn_->db() != nullptr ) conn_->db()->wrote = true;
			changed();
		}
		Connection *c = conn_;
		int64_t id = id_;
		if ( c != nullptr && c->callbacks() != nullptr ) c->callbacks()->Complete(id);
		if ( open_ != nullptr ) {
			/* the open that waited for the upgrade succeeds now */
			Database *d = c != nullptr ? c->db() : nullptr;
			if ( d != nullptr ) {
				open_->client->OpenSuccess(mojo::PendingAssociatedRemote<blink::mojom::IDBDatabase>(),
							   d->data.meta);
			}
			upgrade_ended(*open_);
			open_.reset();
		}
	}

	std::unique_ptr<PendingOpen>	open_;		/* set on a version change transaction */

private:
	friend class Connection;
	Connection	*conn_;
	int64_t		id_;
	blink::mojom::IDBTransactionMode mode_;
	bool		ended_ = false, wrote_ = false;
	std::unique_ptr<Data>	saved_;
	mojo::AssociatedReceiver<blink::mojom::IDBTransaction> receiver_{this};
};

bool Connection::writing() const
{
	for ( const auto &t : transactions_ ) {
		if ( t.second->writes() ) return true;
	}
	return false;
}

Connection::~Connection()
{
	for ( auto &t : transactions_ ) {
		t.second->abort(false);
		t.second->detach();
	}
	if ( Database *d = db() ) d->connections.erase(this);
	/* an upgrade or a deletion may have waited for this one to close */
	idb_process_later(origin_, name_);
}

void Connection::CreateTransaction( mojo::PendingAssociatedReceiver<blink::mojom::IDBTransaction> r, int64_t id,
				    const std::vector<int64_t> &, blink::mojom::IDBTransactionMode mode,
				    blink::mojom::IDBTransactionDurability )
{
	Transaction *t = new Transaction(this, id, mode);
	t->bind(std::move(r));
}

void Connection::writing( int64_t txn )
{
	auto it = transactions_.find(txn);
	if ( it != transactions_.end() ) it->second->save();
}

void Connection::DeleteRange( int64_t txn, int64_t store_id, IndexedDBKeyRange key_range,
			      DeleteRangeCallback callback )
{
	Store *s = store(store_id);
	if ( s == nullptr ) {
		std::move(callback).Run(false);
		return;
	}
	writing(txn);
	Range r(key_range);
	for ( auto it = r.has_lower ? s->records.lower_bound(r.lower) : s->records.begin(); it != s->records.end(); ) {
		if ( !r.above_lower(it->first) ) {
			++it;
			continue;
		}
		if ( !r.below_upper(it->first) ) break;
		for ( auto &ix : s->indexes ) index_remove_primary(ix.second, it->first);
		it = s->records.erase(it);
	}
	auto t = transactions_.find(txn);
	if ( t != transactions_.end() ) t->second->wrote_ = true;
	std::move(callback).Run(true);
}

void Connection::Clear( int64_t txn, int64_t store_id, ClearCallback callback )
{
	Store *s = store(store_id);
	if ( s == nullptr ) {
		std::move(callback).Run(false);
		return;
	}
	writing(txn);
	s->records.clear();
	for ( auto &ix : s->indexes ) ix.second = Index();
	auto t = transactions_.find(txn);
	if ( t != transactions_.end() ) t->second->wrote_ = true;
	std::move(callback).Run(true);
}

void Connection::CreateIndex( int64_t txn, int64_t store_id, const blink::IndexedDBIndexMetadata &index )
{
	Database *d = db();
	if ( d == nullptr ) return;
	auto m = d->data.meta.object_stores.find(store_id);
	Store *s = store(store_id);
	if ( m == d->data.meta.object_stores.end() || s == nullptr ) return;
	writing(txn);
	m->second.indexes[index.id] = index;
	if ( index.id > m->second.max_index_id ) m->second.max_index_id = index.id;
	s->indexes[index.id] = Index();
	auto t = transactions_.find(txn);
	if ( t != transactions_.end() ) t->second->wrote_ = true;
}

void Connection::DeleteIndex( int64_t txn, int64_t store_id, int64_t index_id )
{
	Database *d = db();
	if ( d == nullptr ) return;
	auto m = d->data.meta.object_stores.find(store_id);
	Store *s = store(store_id);
	if ( m == d->data.meta.object_stores.end() || s == nullptr ) return;
	writing(txn);
	m->second.indexes.erase(index_id);
	s->indexes.erase(index_id);
	auto t = transactions_.find(txn);
	if ( t != transactions_.end() ) t->second->wrote_ = true;
}

void Connection::Abort( int64_t transaction_id )
{
	auto it = transactions_.find(transaction_id);
	if ( it != transactions_.end() ) it->second->abort(true);
}

/* ================================================================ cursors' moves */

bool Cursor::first( Pos *p )
{
	using D = blink::mojom::IDBCursorDirection;
	Store *s = db_.store(store_);
	if ( s == nullptr ) return false;
	bool back = ( dir_ == D::Prev || dir_ == D::PrevNoDuplicate );
	if ( index_ == blink::IndexedDBIndexMetadata::kInvalidId ) {
		if ( !back ) {
			auto it = range_.has_lower ? s->records.lower_bound(range_.lower) : s->records.begin();
			while ( it != s->records.end() && !range_.above_lower(it->first) ) ++it;
			if ( it == s->records.end() || !range_.below_upper(it->first) ) return false;
			*p = Pos{ it->first, it->first, it->first };
			return true;
		}
		auto it = range_.has_upper ? s->records.upper_bound(range_.upper) : s->records.end();
		while ( it != s->records.begin() ) {
			--it;
			if ( !range_.below_upper(it->first) ) continue;
			if ( !range_.above_lower(it->first) ) return false;
			*p = Pos{ it->first, it->first, it->first };
			return true;
		}
		return false;
	}
	auto ixi = s->indexes.find(index_);
	if ( ixi == s->indexes.end() ) return false;
	auto &e = ixi->second.entries;
	if ( !back ) {
		for ( auto it = range_.has_lower ? e.lower_bound(range_.lower) : e.begin(); it != e.end(); ++it ) {
			std::string ik = it->first.substr(0, it->second);
			if ( !range_.above_lower(ik) ) continue;
			if ( !range_.below_upper(ik) ) return false;
			*p = Pos{ it->first, ik, it->first.substr(it->second) };
			return true;
		}
		return false;
	}
	/* from the end: the last entry in range, or for prevunique the first of its key */
	auto it = e.end();
	if ( range_.has_upper ) {
		/* every entry whose index key is at most the bound comes before this */
		it = e.lower_bound(past(range_.upper));
	}
	while ( it != e.begin() ) {
		--it;
		std::string ik = it->first.substr(0, it->second);
		if ( !range_.below_upper(ik) ) continue;
		if ( !range_.above_lower(ik) ) return false;
		if ( dir_ == D::PrevNoDuplicate ) {
			auto f = e.lower_bound(ik);
			*p = Pos{ f->first, ik, f->first.substr(f->second) };
		} else {
			*p = Pos{ it->first, ik, it->first.substr(it->second) };
		}
		return true;
	}
	return false;
}

/*
 * The next position from 'from' in the cursor's direction, within its
 * range; with 'key' (and 'primary'), the first at or past it
 */
bool Cursor::step( const Pos &from, const std::string *key, const std::string *primary, Pos *out )
{
	using D = blink::mojom::IDBCursorDirection;
	Store *s = db_.store(store_);
	if ( s == nullptr ) return false;
	bool back = ( dir_ == D::Prev || dir_ == D::PrevNoDuplicate );
	bool unique = ( dir_ == D::NextNoDuplicate || dir_ == D::PrevNoDuplicate );

	if ( index_ == blink::IndexedDBIndexMetadata::kInvalidId ) {
		auto &r = s->records;
		if ( !back ) {
			auto it = r.upper_bound(from.entry);
			if ( key != nullptr ) it = later(r, it, r.lower_bound(*key));
			if ( it == r.end() || !range_.below_upper(it->first) ) return false;
			*out = Pos{ it->first, it->first, it->first };
			return true;
		}
		auto it = r.lower_bound(from.entry);
		if ( key != nullptr ) it = earlier(r, it, r.upper_bound(*key));
		if ( it == r.begin() ) return false;
		--it;
		if ( !range_.above_lower(it->first) ) return false;
		*out = Pos{ it->first, it->first, it->first };
		return true;
	}

	auto ixi = s->indexes.find(index_);
	if ( ixi == s->indexes.end() ) return false;
	auto &e = ixi->second.entries;
	if ( !back ) {
		/* past the entry, or past every entry of its key */
		auto it = unique ? e.lower_bound(past(from.key)) : e.upper_bound(from.entry);
		if ( key != nullptr ) {
			it = later(e, it, e.lower_bound(primary != nullptr ? *key + *primary : *key));
		}
		if ( it == e.end() ) return false;
		std::string ik = it->first.substr(0, it->second);
		if ( !range_.below_upper(ik) ) return false;
		*out = Pos{ it->first, ik, it->first.substr(it->second) };
		return true;
	}
	/* before the entry, or before every entry of its key */
	auto it = unique ? e.lower_bound(from.key) : e.lower_bound(from.entry);
	if ( key != nullptr ) {
		it = earlier(e, it, primary != nullptr ? e.upper_bound(*key + *primary) : e.lower_bound(past(*key)));
	}
	if ( it == e.begin() ) return false;
	--it;
	std::string ik = it->first.substr(0, it->second);
	if ( !range_.above_lower(ik) ) return false;
	if ( unique ) it = e.lower_bound(ik);		/* the first of its key */
	*out = Pos{ it->first, ik, it->first.substr(it->second) };
	return true;
}

void Cursor::make_result( const Pos &p, IndexedDBKey *key, IndexedDBKey *primary, blink::mojom::IDBValuePtr *value )
{
	*key = dec(p.key);
	*primary = dec(p.primary);
	Store *s = db_.store(store_);
	if ( key_only_ || s == nullptr ) {
		*value = nullptr;
		return;
	}
	auto it = s->records.find(p.primary);
	*value = value_of(it != s->records.end() ? it->second : std::string());
}

blink::mojom::IDBCursorResultPtr Cursor::result_of( const std::vector<Pos> &ps )
{
	auto v = blink::mojom::IDBCursorValue::New();
	for ( const Pos &p : ps ) {
		IndexedDBKey k, pk;
		blink::mojom::IDBValuePtr value;
		make_result(p, &k, &pk, &value);
		v->keys.push_back(std::move(k));
		v->primary_keys.push_back(std::move(pk));
		v->values.push_back(value ? std::move(value) : blink::mojom::IDBValue::New());
	}
	return blink::mojom::IDBCursorResult::NewValues(std::move(v));
}

/* ================================================================ the factory */

class Factory final : public blink::mojom::IDBFactory {
public:
	explicit Factory( std::string origin ) : origin_(std::move(origin)) { br_site_touch(origin_); }

	void GetDatabaseInfo( GetDatabaseInfoCallback callback ) override
	{
		std::vector<blink::mojom::IDBNameAndVersionPtr> list;
		for ( const auto &d : origins()[origin_] ) {
			list.push_back(blink::mojom::IDBNameAndVersion::New(d.first, d.second.data.meta.version));
		}
		std::move(callback).Run(std::move(list), blink::mojom::IDBError::New(IDBException::kNoError, u""));
	}

	void Open( mojo::PendingAssociatedRemote<blink::mojom::IDBFactoryClient> pending_client,
		   mojo::PendingAssociatedRemote<blink::mojom::IDBDatabaseCallbacks> callbacks,
		   const std::u16string &name, int64_t version,
		   mojo::PendingAssociatedReceiver<blink::mojom::IDBTransaction> version_change,
		   int64_t transaction_id, int32_t, bool ) override
	{
		auto r = std::make_unique<Request>();
		r->client.Bind(std::move(pending_client));
		r->callbacks = std::move(callbacks);
		r->version = version;
		r->version_change = std::move(version_change);
		r->transaction_id = transaction_id;
		coordinators()[{ origin_, name }].queue.push_back(std::move(r));
		idb_process(origin_, name);
	}

	void DeleteDatabase( mojo::PendingAssociatedRemote<blink::mojom::IDBFactoryClient> pending_client,
			     const std::u16string &name, bool ) override
	{
		auto r = std::make_unique<Request>();
		r->is_delete = true;
		r->client.Bind(std::move(pending_client));
		coordinators()[{ origin_, name }].queue.push_back(std::move(r));
		idb_process(origin_, name);
	}

private:
	std::string	origin_;
};

/*
 * The database's requests, the first first, as far as they go now: an
 * open of the version there is answered at once, one of a lower version
 * refused; an upgrade or a deletion first tells the other connections and
 * waits until they have closed (each closing goes on from here), then
 * runs. An upgrade's version change transaction holds the requests after
 * it until it ends.
 */
void idb_process( const std::string &origin, const std::u16string &name )
{
	Coordinator &co = coordinators()[{ origin, name }];
	if ( !co.upgrading ) idb_take_waiting(origin, name);
	while ( !co.queue.empty() && !co.upgrading ) {
		Request &r = *co.queue.front();
		auto &dbs = origins()[origin];
		auto it = dbs.find(name);
		if ( r.is_delete ) {
			int64_t old = it != dbs.end() ? it->second.data.meta.version : 0;
			if ( it != dbs.end() && !it->second.connections.empty() ) {
				if ( !r.told ) {
					r.told = true;
					r.old_version = old;
					for ( Connection *o : it->second.connections ) {
						if ( o->callbacks() != nullptr ) {
							o->callbacks()->VersionChange(old, blink::IndexedDBDatabaseMetadata::NO_VERSION);
						}
					}
				}
				return;
			}
			if ( it != dbs.end() ) {
				dbs.erase(it);
				changed();
			}
			r.client->DeleteSuccess(old == blink::IndexedDBDatabaseMetadata::NO_VERSION ? 0 : old);
			co.queue.pop_front();
			continue;
		}
		bool is_new = ( it == dbs.end() );
		int64_t old = is_new ? blink::IndexedDBDatabaseMetadata::NO_VERSION : it->second.data.meta.version;
		int64_t version = r.version;
		if ( version == blink::IndexedDBDatabaseMetadata::NO_VERSION ) {
			version = ( old == blink::IndexedDBDatabaseMetadata::NO_VERSION ) ? 1 : old;
		}
		if ( old != blink::IndexedDBDatabaseMetadata::NO_VERSION && version < old ) {
			r.client->Error(IDBException::kVersionError,
					u"The requested version is less than the existing version.");
			co.queue.pop_front();
			continue;
		}
		if ( version != old && !is_new && !it->second.connections.empty() ) {
			/* an upgrade: the other connections are told, and it waits until they have closed */
			if ( !r.told ) {
				r.told = true;
				r.old_version = old;
				for ( Connection *o : it->second.connections ) {
					if ( o->callbacks() != nullptr ) o->callbacks()->VersionChange(old, version);
				}
			}
			return;
		}
		Database &d = dbs[name];
		if ( is_new ) {
			d.data.meta = blink::IndexedDBDatabaseMetadata(name);
			d.data.meta.version = blink::IndexedDBDatabaseMetadata::NO_VERSION;
		}
		std::unique_ptr<Request> req = std::move(co.queue.front());
		co.queue.pop_front();
		Connection *c = new Connection(origin, name, std::move(req->callbacks));
		mojo::PendingAssociatedRemote<blink::mojom::IDBDatabase> remote;
		c->bind(remote.InitWithNewEndpointAndPassReceiver());
		if ( version == old ) {
			req->client->OpenSuccess(std::move(remote), d.data.meta);
			continue;
		}
		auto open = std::make_unique<PendingOpen>();
		open->client = std::move(req->client);
		open->old_version = old;
		open->origin = origin;
		open->name = name;
		Transaction *t = new Transaction(c, req->transaction_id, blink::mojom::IDBTransactionMode::VersionChange);
		t->bind(std::move(req->version_change));
		t->save();
		d.data.meta.version = version;
		co.upgrading = true;
		open->client->UpgradeNeeded(std::move(remote),
					    old == blink::IndexedDBDatabaseMetadata::NO_VERSION
						    ? (int64_t)blink::IndexedDBDatabaseMetadata::DEFAULT_VERSION : old,
					    blink::mojom::IDBDataLoss::None, std::string(), d.data.meta);
		t->open_ = std::move(open);
		changed();
	}
}

/* The same from a task of its own: called where a connection or a transaction is going away */
void idb_process_later( const std::string &origin, const std::u16string &name )
{
	base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
		FROM_HERE, base::BindOnce(&idb_process, origin, name));
}

/* The origin of the script that asks: the factory is bound from inside indexedDB.open() */
std::string asking_origin()
{
	v8::Isolate *iso = v8::Isolate::GetCurrent();
	if ( iso != nullptr && iso->InContext() ) {
		blink::ExecutionContext *ec = blink::ExecutionContext::From(iso->GetCurrentContext());
		if ( ec != nullptr && ec->GetSecurityOrigin() != nullptr ) {
			return ec->GetSecurityOrigin()->ToString().Utf8();
		}
	}
	return "null";
}

void bind_factory( mojo::ScopedMessagePipeHandle pipe )
{
	mojo::MakeSelfOwnedReceiver(std::make_unique<Factory>(asking_origin()),
				    mojo::PendingReceiver<blink::mojom::IDBFactory>(std::move(pipe)));
}

/* ================================================================ kept in record 2 of ブラウザの保存域 */

/*
 * "BRID", 1, the number of origins; for each its name and databases; a
 * database its name, version, largest store number and stores; a store
 * its metadata, key generator, indexes (their metadata and entries) and
 * records. Numbers are 32 or 64 bits little-endian, strings a 32-bit
 * length and their bytes (UTF-16 as its code units).
 */
struct Out {
	std::string b;
	void u32( uint32_t v ) { for ( int i = 0; i < 4; i++ ) b.push_back((char)( v >> ( 8 * i ) )); }
	void i64( int64_t v ) { for ( int i = 0; i < 8; i++ ) b.push_back((char)( (uint64_t)v >> ( 8 * i ) )); }
	void str( const std::string &s ) { u32((uint32_t)s.size()); b += s; }
	void u16( const std::u16string &s )
	{
		u32((uint32_t)s.size());
		for ( char16_t c : s ) {
			b.push_back((char)( c & 0xFF ));
			b.push_back((char)( c >> 8 ));
		}
	}
	void path( const IndexedDBKeyPath &p )
	{
		b.push_back((char)p.type());
		if ( p.type() == blink::mojom::IDBKeyPathType::String ) u16(p.string());
		if ( p.type() == blink::mojom::IDBKeyPathType::Array ) {
			u32((uint32_t)p.array().size());
			for ( const auto &s : p.array() ) u16(s);
		}
	}
};

struct In {
	const unsigned char	*p;
	size_t			n;
	bool			ok = true;

	bool need( size_t k )
	{
		if ( !ok || n < k ) ok = false;
		return ok;
	}
	uint32_t u32()
	{
		if ( !need(4) ) return 0;
		uint32_t v = p[0] | ( p[1] << 8 ) | ( p[2] << 16 ) | ( (uint32_t)p[3] << 24 );
		p += 4;
		n -= 4;
		return v;
	}
	int64_t i64()
	{
		if ( !need(8) ) return 0;
		uint64_t v = 0;
		for ( int i = 7; i >= 0; i-- ) v = ( v << 8 ) | p[i];
		p += 8;
		n -= 8;
		return (int64_t)v;
	}
	uint8_t u8()
	{
		if ( !need(1) ) return 0;
		n--;
		return *p++;
	}
	std::string str()
	{
		uint32_t len = u32();
		if ( !need(len) ) return std::string();
		std::string s((const char *)p, len);
		p += len;
		n -= len;
		return s;
	}
	std::u16string u16()
	{
		uint32_t len = u32();
		if ( len > n / 2 || !need((size_t)len * 2) ) {
			ok = false;
			return std::u16string();
		}
		std::u16string s;
		for ( uint32_t i = 0; i < len; i++ ) s.push_back((char16_t)( p[2 * i] | ( p[2 * i + 1] << 8 ) ));
		p += (size_t)len * 2;
		n -= (size_t)len * 2;
		return s;
	}
	IndexedDBKeyPath path()
	{
		uint8_t t = u8();
		if ( t == (uint8_t)blink::mojom::IDBKeyPathType::String ) return IndexedDBKeyPath(u16());
		if ( t == (uint8_t)blink::mojom::IDBKeyPathType::Array ) {
			std::vector<std::u16string> a;
			uint32_t k = u32();
			for ( uint32_t i = 0; i < k && ok; i++ ) a.push_back(u16());
			return IndexedDBKeyPath(std::move(a));
		}
		return IndexedDBKeyPath();
	}
};

/* A database's bytes, with the numbers of the external objects its records name */
void write_db( Out &o, const Data &d, std::set<uint64_t> &named )
{
	o.u16(d.meta.name);
	o.i64(d.meta.version);
	o.i64(d.meta.max_object_store_id);
	o.u32((uint32_t)d.meta.object_stores.size());
	for ( const auto &sm : d.meta.object_stores ) {
		const blink::IndexedDBObjectStoreMetadata &m = sm.second;
		auto st = d.stores.find(sm.first);
		o.i64(m.id);
		o.u16(m.name);
		o.path(m.key_path);
		o.b.push_back(m.auto_increment ? 1 : 0);
		o.i64(m.max_index_id);
		o.i64(st != d.stores.end() ? st->second.key_gen : 0);
		o.u32((uint32_t)m.indexes.size());
		for ( const auto &im : m.indexes ) {
			o.i64(im.second.id);
			o.u16(im.second.name);
			o.path(im.second.key_path);
			o.b.push_back(im.second.unique ? 1 : 0);
			o.b.push_back(im.second.multi_entry ? 1 : 0);
			const Index *ix = nullptr;
			if ( st != d.stores.end() ) {
				auto f = st->second.indexes.find(im.first);
				if ( f != st->second.indexes.end() ) ix = &f->second;
			}
			o.u32(ix != nullptr ? (uint32_t)ix->entries.size() : 0);
			if ( ix != nullptr ) {
				for ( const auto &e : ix->entries ) {
					o.str(e.first);
					o.u32((uint32_t)e.second);
				}
			}
		}
		o.u32(st != d.stores.end() ? (uint32_t)st->second.records.size() : 0);
		if ( st != d.stores.end() ) {
			for ( const auto &r : st->second.records ) {
				o.str(r.first);
				o.str(r.second);
				size_t at = 0;
				for ( uint64_t id : ext_ids(r.second, &at) ) named.insert(id);
			}
		}
	}
}

/* The external objects named, those whose bytes are in */
void write_exts( Out &o, const std::set<uint64_t> &named )
{
	std::lock_guard<std::mutex> hold(g_ext_lock);
	uint32_t n = 0;
	for ( uint64_t id : named ) {
		auto it = g_ext.find(id);
		if ( it != g_ext.end() && it->second.have_bytes ) n++;
	}
	o.u32(n);
	for ( uint64_t id : named ) {
		auto it = g_ext.find(id);
		if ( it == g_ext.end() || !it->second.have_bytes ) continue;
		const Extern &e = it->second;
		o.i64((int64_t)id);
		o.u16(e.type);
		o.i64(e.size);
		o.b.push_back(e.is_file ? 1 : 0);
		o.u16(e.name);
		o.i64(e.modified_us);
		o.str(e.bytes);
	}
}

/* An entry's key: the origin, a zero byte and the name's UTF-16 */
std::string db_key( const std::string &origin, const std::u16string &name )
{
	std::string k = origin;
	k.push_back('\0');
	k.append((const char *)name.data(), name.size() * sizeof(char16_t));
	return k;
}

/* What another browser wrote of a database this one has connections to */
struct Waiting {
	bool		has = false;		/* false: it deleted it */
	std::string	bytes;
	int64_t		version = 0;
};
std::map<std::string, Waiting> g_waiting;

void snapshot_dbs( void *, br_store_delta *all )
{
	for ( auto &org : origins() ) {
		for ( auto &db : org.second ) {
			std::string key = db_key(org.first, db.first);
			bool writing = coordinators()[{ org.first, db.first }].upgrading;
			for ( Connection *c : db.second.connections ) writing = writing || c->writing();
			if ( writing ) {
				/* not until what it writes is committed: asked again */
				br_store_delta_keep(all, key.data(), (int)key.size());
				br_store_dirty(g_store_id);
				continue;
			}
			auto w = g_waiting.find(key);
			if ( w != g_waiting.end() ) {
				if ( !db.second.wrote || !w->second.has || w->second.version != db.second.data.meta.version ) {
					/* another browser's is newer, or of another version: this one's does not go over it */
					br_store_delta_keep(all, key.data(), (int)key.size());
					continue;
				}
				g_waiting.erase(w);
			}
			db.second.wrote = false;
			Out o;
			std::set<uint64_t> named;
			o.b.append("BRI3", 4);
			write_db(o, db.second.data, named);
			write_exts(o, named);
			br_store_delta_put(all, key.data(), (int)key.size(), o.b.data(), (int)o.b.size());
		}
	}
	/* those another browser made that are waiting, not gone from what is kept */
	for ( const auto &w : g_waiting ) br_store_delta_keep(all, w.first.data(), (int)w.first.size());
}

/* A database's bytes read: false when they are not whole */
bool read_db( In &in, Data &d )
{
	d.meta.name = in.u16();
	d.meta.version = in.i64();
	d.meta.max_object_store_id = in.i64();
	uint32_t stores = in.u32();
	for ( uint32_t s = 0; s < stores && in.ok; s++ ) {
		blink::IndexedDBObjectStoreMetadata m;
		m.id = in.i64();
		m.name = in.u16();
		m.key_path = in.path();
		m.auto_increment = in.u8() != 0;
		m.max_index_id = in.i64();
		Store st;
		st.key_gen = in.i64();
		uint32_t nix = in.u32();
		for ( uint32_t i = 0; i < nix && in.ok; i++ ) {
			blink::IndexedDBIndexMetadata im;
			im.id = in.i64();
			im.name = in.u16();
			im.key_path = in.path();
			im.unique = in.u8() != 0;
			im.multi_entry = in.u8() != 0;
			Index ix;
			uint32_t ne = in.u32();
			for ( uint32_t e = 0; e < ne && in.ok; e++ ) {
				std::string entry = in.str();
				uint32_t klen = in.u32();
				if ( klen > entry.size() ) {
					in.ok = false;
					break;
				}
				ix.entries[entry] = klen;
				ix.of_primary[entry.substr(klen)].insert(entry);
			}
			m.indexes[im.id] = im;
			st.indexes[im.id] = std::move(ix);
		}
		uint32_t nrec = in.u32();
		for ( uint32_t r = 0; r < nrec && in.ok; r++ ) {
			std::string k = in.str();
			st.records[k] = in.str();
		}
		d.meta.object_stores[m.id] = m;
		d.stores[m.id] = std::move(st);
	}
	return in.ok;
}

/* The external objects of what was read; one this browser has is left as it is, for the Blob it holds */
void read_exts( In &in )
{
	uint32_t n = in.u32();
	for ( uint32_t i = 0; i < n && in.ok; i++ ) {
		uint64_t id = (uint64_t)in.i64();
		Extern e;
		e.type = in.u16();
		e.size = in.i64();
		e.is_file = in.u8() != 0;
		e.name = in.u16();
		e.modified_us = in.i64();
		e.bytes = in.str();
		e.have_bytes = in.ok;
		if ( !in.ok ) break;
		std::lock_guard<std::mutex> hold(g_ext_lock);
		g_ext.emplace(id, std::move(e));
	}
}

/* An older browser's whole record */
void read_all( const unsigned char *data, size_t len, int *ndb )
{
	In in{ data, len };
	if ( len < 8 || memcmp(data, "BRID", 4) != 0 ) return;
	in.p += 4;
	in.n -= 4;
	uint32_t version = in.u32();
	if ( version != 1 && version != 2 ) return;
	uint32_t norg = in.u32();
	for ( uint32_t a = 0; a < norg && in.ok; a++ ) {
		std::string org = in.str();
		uint32_t dbs = in.u32();
		for ( uint32_t b = 0; b < dbs && in.ok; b++ ) {
			Data d;
			if ( read_db(in, d) ) {
				std::u16string name = d.meta.name;
				origins()[org][name].data = std::move(d);
				(*ndb)++;
			}
		}
	}
	if ( version >= 2 && in.ok ) read_exts(in);
	br_store_dirty(g_store_id);
}

/* A database's entry made the database there is here */
bool read_entry( const std::string &origin, const unsigned char *val, size_t vlen )
{
	In in{ val, vlen };
	if ( vlen < 4 || memcmp(val, "BRI3", 4) != 0 ) return false;
	in.p += 4;
	in.n -= 4;
	Data d;
	if ( !read_db(in, d) ) return false;
	read_exts(in);
	if ( !in.ok ) return false;
	std::u16string name = d.meta.name;
	Database &db = origins()[origin][name];
	db.data = std::move(d);
	db.wrote = false;
	return true;
}

void restore_entry( void *ctx, const char *key, int klen, const unsigned char *val, int vlen )
{
	int *ndb = (int *)ctx;
	if ( klen == 0 ) {
		read_all(val, (size_t)vlen, ndb);
		return;
	}
	const char *z = (const char *)memchr(key, 0, (size_t)klen);
	if ( z != nullptr && read_entry(std::string(key, (size_t)( z - key )), val, (size_t)vlen) ) (*ndb)++;
}

void idb_take_waiting( const std::string &origin, const std::u16string &name )
{
	std::string key = db_key(origin, name);
	auto w = g_waiting.find(key);
	if ( w == g_waiting.end() ) return;
	auto &dbs = origins()[origin];
	auto it = dbs.find(name);
	if ( it != dbs.end() && !it->second.connections.empty() ) return;
	if ( w->second.has ) {
		(void)read_entry(origin, (const unsigned char *)w->second.bytes.data(), w->second.bytes.size());
	} else if ( it != dbs.end() ) {
		dbs.erase(it);
	}
	g_waiting.erase(w);
}

/* A database another browser wrote ('val' NULL: deleted) */
void take_db( void *, const char *key, int klen, const unsigned char *val, int vlen )
{
	const char *z = klen > 0 ? (const char *)memchr(key, 0, (size_t)klen) : nullptr;
	if ( z == nullptr ) return;
	std::string origin(key, (size_t)( z - key ));
	std::u16string name((const char16_t *)( z + 1 ), (size_t)( key + klen - z - 1 ) / sizeof(char16_t));
	Waiting w;
	w.has = val != nullptr;
	w.version = blink::IndexedDBDatabaseMetadata::NO_VERSION;
	if ( w.has ) {
		w.bytes.assign((const char *)val, (size_t)vlen);
		In in{ val, (size_t)vlen };
		if ( vlen < 4 || memcmp(val, "BRI3", 4) != 0 ) return;
		in.p += 4;
		in.n -= 4;
		(void)in.u16();
		w.version = in.i64();
	}
	std::string k = db_key(origin, name);
	auto &dbs = origins()[origin];
	auto it = dbs.find(name);
	bool told = g_waiting.count(k) != 0 && g_waiting[k].version == w.version;
	g_waiting[k] = std::move(w);
	if ( it == dbs.end() || ( it->second.connections.empty() && !coordinators()[{ origin, name }].upgrading ) ) {
		idb_take_waiting(origin, name);
		return;
	}
	/* a page holding the database open is told it should let go when what it knows is no more so */
	int64_t now = it->second.data.meta.version, version = g_waiting[k].version;
	if ( !told && version != now ) {
		for ( Connection *o : it->second.connections ) {
			if ( o->callbacks() != nullptr ) o->callbacks()->VersionChange(now, version);
		}
	}
}

}  // namespace

/* Work for the page's thread, from any other, waited for by the caller's own means */
void br_on_page_thread( base::OnceClosure work )
{
	if ( g_idb_runner != nullptr ) (*g_idb_runner)->PostTask(FROM_HERE, std::move(work));
}

/* The databases of the origins a clearing takes gone: a connection finds its database again each time, and is told it is not there */
void br_idb_clear( const BrClear &c )
{
	bool any = false;
	for ( auto it = origins().begin(); it != origins().end(); ) {
		if ( !it->second.empty() && br_clear_origin_hit(c, it->first) ) {
			it = origins().erase(it);
			any = true;
		} else {
			++it;
		}
	}
	if ( any ) changed();
}

void br_idb_sites( std::set<std::string> *out )
{
	for ( const auto &kv : origins() ) {
		if ( !kv.second.empty() ) out->insert(br_site_of(kv.first));
	}
}

/* The store read back and the factory bound, once, after the engine is up */
void br_idb_init()
{
	if ( g_store_id >= 0 ) return;
	g_idb_runner = new scoped_refptr<base::SingleThreadTaskRunner>(base::SingleThreadTaskRunner::GetCurrentDefault());
	g_store_id = br_store_register_shared("indexeddb", snapshot_dbs, take_db, nullptr, 2000);
	int ndb = 0;
	(void)br_store_load_shared(g_store_id, restore_entry, &ndb);
	printf("browser: indexedDB: %d database(s) read back\n", ndb);
	/* the numbers of the external objects this browser makes, not those of another's */
	g_ext_last = ( base::RandUint64() | ( 1ull << 63 ) ) & ~0xFFFFFFFFull;
	blink::GetEmptyBrowserInterfaceBroker().SetBinderForTesting(blink::mojom::IDBFactory::Name_,
								      base::BindRepeating(&bind_factory));
}

/*
 * A worker's factory (blob_tessronos.cc): the databases of its origin,
 * served on the page's thread, where the windows' are, so that a worker
 * and the windows of an origin share them
 */
void br_idb_bind_for( const std::string &origin, mojo::ScopedMessagePipeHandle pipe )
{
	if ( g_idb_runner == nullptr ) return;
	(*g_idb_runner)->PostTask(FROM_HERE, base::BindOnce([]( std::string origin, mojo::ScopedMessagePipeHandle pipe ) {
		mojo::MakeSelfOwnedReceiver(std::make_unique<Factory>(origin),
					    mojo::PendingReceiver<blink::mojom::IDBFactory>(std::move(pipe)));
	}, origin, std::move(pipe)));
}

/* A bucket's databases (buckets_tessronos.cc): those kept under its key, on the page's thread */
void br_idb_bind_key( const std::string &key, mojo::ScopedMessagePipeHandle pipe )
{
	mojo::MakeSelfOwnedReceiver(std::make_unique<Factory>(key),
				    mojo::PendingReceiver<blink::mojom::IDBFactory>(std::move(pipe)));
}

/* A deleted bucket's databases, gone */
void br_idb_delete_key( const std::string &key )
{
	if ( origins().erase(key) > 0 ) changed();
}
