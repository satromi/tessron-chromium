/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	cache_storage_tessronos.cc
 *	ブラウザ: Cache Storage (caches, design 14.4.5), the browser's side
 *	of the cache_storage module.
 *
 *	The module asks a CacheStorage of the frame's broker (a worker's,
 *	through its own) and works the caches it opens as associated
 *	interfaces of it. Here the caches of each origin are kept in the
 *	process, by name in the order they were made: each a list of
 *	entries, a request (its address, method and headers) and the
 *	response put for it, its body the Blob the module made of it. A
 *	request matches an entry as the Cache Storage specification has it:
 *	the address without its fragment (and without its query with
 *	ignoreSearch), GET unless ignoreMethod, and the request headers the
 *	response's Vary names unless ignoreVary. Put replaces the entries of
 *	the same address; a response given back is a copy that shares the
 *	body.
 *
 *	The caches are kept between runs, in the store "caches" (record 6 of
 *	ブラウザの保存域), by entries: the bytes of a body put are read from
 *	its Blob as it is put, and two seconds after the caches changed the
 *	caches and entries made and deleted since the last save are handed
 *	to the store, which writes them on its own thread. They are read
 *	back when the browser starts, each body made a Blob again.
 *
 *	The receivers are on the blob thread, where the Blobs are.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/types/expected.h"
#include "mojo/public/cpp/bindings/associated_receiver_set.h"
#include "mojo/public/cpp/bindings/generic_pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_associated_remote.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/unique_associated_receiver_set.h"
#include "third_party/blink/public/mojom/cache_storage/cache_storage.mojom-blink.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"
#include "third_party/blink/renderer/platform/blob/blob_data.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "../net/br_data.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

/* The blob thread, where the services are (blob_tessronos.cc) */
void br_on_service_thread( base::OnceClosure task );

/* The origin of the page shown (br_blink.cc) */
std::string br_page_origin();

/* A Blob's bytes, on the services' thread (blob_tessronos.cc) */
void br_blob_read( const std::string &key, bool is_url,
		   base::OnceCallback<void(bool, std::string, std::string)> done );
void br_net_post_delayed( base::OnceClosure work, int ms );	/* net_fetch_tessronos.cc */

#include "../net/br_store.h"

namespace {

namespace bm = blink::mojom::blink;

/* A body's bytes, as they are read from its Blob to be kept */
struct Body {
	bool		read = false;
	std::string	bytes;
};

struct Entry {
	uint64_t						seq = 0;	/* its number, in the order made */
	blink::KURL						url;
	blink::String						method;
	blink::HashMap<blink::String, blink::String>		headers;	/* the request's, names lower case */
	bm::FetchAPIResponsePtr					response;
	std::shared_ptr<Body>					body;		/* null: the response has none */
};

void Changed();

struct CacheData {
	uint64_t		seq = 0;	/* its number, in the order made */
	std::vector<Entry>	entries;
};

uint64_t NextSeq();

/* The caches of one origin, in the order they were made */
struct Origin {
	std::vector<std::pair<blink::String, std::shared_ptr<CacheData>>>	caches;

	std::shared_ptr<CacheData> Find( const blink::String &name )
	{
		for ( auto &c : caches ) {
			if ( c.first == name ) return c.second;
		}
		return nullptr;
	}
};

std::map<std::string, Origin> &Origins()
{
	static auto *m = new std::map<std::string, Origin>();
	return *m;
}

blink::KURL Key( const blink::KURL &u, bool ignore_search )
{
	blink::KURL k = u;
	k.RemoveFragmentIdentifier();
	if ( ignore_search ) k.SetQuery(blink::String());
	return k;
}

template <typename Map>
blink::String HeaderOf( const Map &h, const blink::String &name )
{
	for ( const auto &kv : h ) {
		if ( blink::EqualIgnoringAsciiCase(kv.key, name) ) return kv.value;
	}
	return blink::String();
}

bool Matches( const Entry &e, const bm::FetchAPIRequest &r, const bm::CacheQueryOptions &o )
{
	if ( !o.ignore_method && !r.method.empty() && r.method != "GET" ) return false;
	if ( Key(e.url, o.ignore_search) != Key(r.url, o.ignore_search) ) return false;
	if ( o.ignore_vary || !e.response ) return true;
	blink::String vary = HeaderOf(e.response->headers, "vary");
	if ( vary.empty() ) return true;
	blink::Vector<blink::String> names = vary.Split(',');
	for ( blink::String n : names ) {
		n = n.StripWhiteSpace();
		if ( n == "*" ) return false;
		blink::String want = HeaderOf(r.headers, n), had;
		auto it = e.headers.find(n.ToAsciiLower());
		if ( it != e.headers.end() ) had = it->value;
		if ( want != had ) return false;
	}
	return true;
}

/* The request kept for an entry, given back by keys() */
bm::FetchAPIRequestPtr RequestOf( const Entry &e )
{
	auto r = bm::FetchAPIRequest::New();
	r->url = e.url;
	r->method = e.method;
	for ( const auto &kv : e.headers ) r->headers.Set(kv.key, kv.value);
	return r;
}

bm::FetchAPIResponsePtr ResponseOf( const Entry &e, const blink::String &cache_name )
{
	bm::FetchAPIResponsePtr r = e.response.Clone();
	r->response_source = network::mojom::blink::FetchResponseSource::kCacheStorage;
	r->cache_storage_cache_name = cache_name;
	return r;
}

bm::CacheQueryOptions Options( const bm::CacheQueryOptionsPtr &p )
{
	bm::CacheQueryOptions o;
	if ( p ) {
		o.ignore_search = p->ignore_search;
		o.ignore_method = p->ignore_method;
		o.ignore_vary = p->ignore_vary;
	}
	return o;
}

/* ---------------------------------------------------------------- one cache */

class Cache final : public bm::CacheStorageCache {
public:
	Cache( std::shared_ptr<CacheData> data, const blink::String &name ) : data_(std::move(data)), name_(name) {}

	void Match( bm::FetchAPIRequestPtr request, bm::CacheQueryOptionsPtr options, bool, bool, int64_t,
		    MatchCallback cb ) override
	{
		bm::CacheQueryOptions o = Options(options);
		for ( const Entry &e : data_->entries ) {
			if ( Matches(e, *request, o) ) {
				std::move(cb).Run(bm::MatchResponse::NewResponse(ResponseOf(e, name_)));
				return;
			}
		}
		std::move(cb).Run(base::unexpected(bm::CacheStorageError::kErrorNotFound));
	}

	void MatchAll( bm::FetchAPIRequestPtr request, bm::CacheQueryOptionsPtr options, int64_t,
		       MatchAllCallback cb ) override
	{
		bm::CacheQueryOptions o = Options(options);
		blink::Vector<bm::FetchAPIResponsePtr> out;
		for ( const Entry &e : data_->entries ) {
			if ( !request || Matches(e, *request, o) ) out.push_back(ResponseOf(e, name_));
		}
		std::move(cb).Run(std::move(out));
	}

	void GetAllMatchedEntries( bm::FetchAPIRequestPtr request, bm::CacheQueryOptionsPtr options, int64_t,
				   GetAllMatchedEntriesCallback cb ) override
	{
		bm::CacheQueryOptions o = Options(options);
		blink::Vector<bm::CacheEntryPtr> out;
		for ( const Entry &e : data_->entries ) {
			if ( !request || Matches(e, *request, o) ) {
				out.push_back(bm::CacheEntry::New(RequestOf(e), ResponseOf(e, name_)));
			}
		}
		std::move(cb).Run(std::move(out));
	}

	void Keys( bm::FetchAPIRequestPtr request, bm::CacheQueryOptionsPtr options, int64_t, KeysCallback cb ) override
	{
		bm::CacheQueryOptions o = Options(options);
		blink::Vector<bm::FetchAPIRequestPtr> out;
		for ( const Entry &e : data_->entries ) {
			if ( !request || Matches(e, *request, o) ) out.push_back(RequestOf(e));
		}
		std::move(cb).Run(std::move(out));
	}

	void Batch( blink::Vector<bm::BatchOperationPtr> ops, int64_t, BatchCallback cb ) override
	{
		bm::CacheStorageError err = bm::CacheStorageError::kSuccess;
		for ( auto &op : ops ) {
			if ( !op || !op->request ) continue;
			if ( op->operation_type == bm::OperationType::kPut && op->response ) {
				/* the entries of the same request go: the one put takes their place */
				bm::CacheQueryOptions o;
				o.ignore_vary = true;
				Erase(*op->request, o);
				Entry e;
				e.seq = NextSeq();
				e.url = Key(op->request->url, false);
				e.method = op->request->method.empty() ? blink::String("GET") : op->request->method;
				for ( const auto &kv : op->request->headers ) e.headers.Set(kv.key.ToAsciiLower(), kv.value);
				e.response = std::move(op->response);
				if ( e.response->blob ) {
					/* the body's bytes, to be kept */
					e.body = std::make_shared<Body>();
					br_blob_read(e.response->blob->Uuid().Utf8(), false, base::BindOnce(
						[]( std::shared_ptr<Body> b, bool ok, std::string bytes, std::string ) {
							b->read = ok;
							b->bytes = std::move(bytes);
							Changed();
						}, e.body));
				}
				data_->entries.push_back(std::move(e));
			} else if ( op->operation_type == bm::OperationType::kDelete ) {
				bm::CacheQueryOptions o = Options(op->match_options);
				if ( Erase(*op->request, o) == 0 ) err = bm::CacheStorageError::kErrorNotFound;
			}
		}
		Changed();
		std::move(cb).Run(bm::CacheStorageVerboseError::New(err, blink::String()));
	}

	/* the code cache of a script kept with it: not kept */
	void WriteSideData( const blink::KURL &, base::Time, mojo_base::BigBuffer, int64_t, WriteSideDataCallback cb ) override
	{
		std::move(cb).Run(bm::CacheStorageError::kSuccess);
	}

private:
	size_t Erase( const bm::FetchAPIRequest &r, const bm::CacheQueryOptions &o )
	{
		size_t n = 0;
		for ( auto it = data_->entries.begin(); it != data_->entries.end(); ) {
			if ( Matches(*it, r, o) ) {
				it = data_->entries.erase(it);
				n++;
			} else {
				++it;
			}
		}
		return n;
	}

	std::shared_ptr<CacheData>	data_;
	blink::String			name_;
};

/* ---------------------------------------------------------------- the caches of an origin */

class Storage final : public bm::CacheStorage {
public:
	void Bind( mojo::PendingReceiver<bm::CacheStorage> r )
	{
		br_site_touch(br_page_origin());
		receivers_.Add(this, std::move(r), br_page_origin());
	}

	/* a bucket's caches: those of its key */
	void BindKey( mojo::PendingReceiver<bm::CacheStorage> r, const std::string &key )
	{
		receivers_.Add(this, std::move(r), key);
	}

	void Has( const blink::String &name, int64_t, HasCallback cb ) override
	{
		std::move(cb).Run(Mine().Find(name) ? bm::CacheStorageError::kSuccess
						    : bm::CacheStorageError::kErrorNotFound);
	}

	void Delete( const blink::String &name, int64_t, DeleteCallback cb ) override
	{
		Origin &o = Mine();
		for ( auto it = o.caches.begin(); it != o.caches.end(); ++it ) {
			if ( it->first == name ) {
				o.caches.erase(it);
				Changed();
				std::move(cb).Run(bm::CacheStorageError::kSuccess);
				return;
			}
		}
		std::move(cb).Run(bm::CacheStorageError::kErrorNotFound);
	}

	void Keys( int64_t, KeysCallback cb ) override
	{
		blink::Vector<blink::String> names;
		for ( const auto &c : Mine().caches ) names.push_back(c.first);
		std::move(cb).Run(names);
	}

	void Match( bm::FetchAPIRequestPtr request, bm::MultiCacheQueryOptionsPtr options, bool, bool, int64_t,
		    MatchCallback cb ) override
	{
		bm::CacheQueryOptions o = options ? Options(options->query_options) : bm::CacheQueryOptions();
		for ( const auto &c : Mine().caches ) {
			if ( options && !options->cache_name.IsNull() && options->cache_name != c.first ) continue;
			for ( const Entry &e : c.second->entries ) {
				if ( Matches(e, *request, o) ) {
					std::move(cb).Run(bm::MatchResponse::NewResponse(ResponseOf(e, c.first)));
					return;
				}
			}
			if ( options && !options->cache_name.IsNull() ) {
				std::move(cb).Run(base::unexpected(bm::CacheStorageError::kErrorNotFound));
				return;
			}
		}
		std::move(cb).Run(base::unexpected(( options && !options->cache_name.IsNull() )
						   ? bm::CacheStorageError::kErrorCacheNameNotFound
						   : bm::CacheStorageError::kErrorNotFound));
	}

	void Open( const blink::String &name, int64_t, OpenCallback cb ) override
	{
		Origin &o = Mine();
		std::shared_ptr<CacheData> d = o.Find(name);
		if ( !d ) {
			d = std::make_shared<CacheData>();
			d->seq = NextSeq();
			o.caches.emplace_back(name, d);
		}
		mojo::PendingAssociatedRemote<bm::CacheStorageCache> remote;
		caches_.Add(std::make_unique<Cache>(d, name), remote.InitWithNewEndpointAndPassReceiver());
		std::move(cb).Run(std::move(remote));
	}

private:
	Origin &Mine() { return Origins()[receivers_.current_context()]; }

	mojo::ReceiverSet<bm::CacheStorage, std::string>	receivers_;
	mojo::UniqueAssociatedReceiverSet<bm::CacheStorageCache>	caches_;
};

Storage *g_storage;

/* ---------------------------------------------------------------- kept between runs */

/*
 * The store "caches" is kept by entries (br_store.h): one for each
 * cache, its key "c" and its number, holding its origin's key and its
 * name, and one for each entry, its key "e" and its number, holding the
 * number of its cache, its request (address, method, headers), its
 * response (status, its text, type, addresses, headers, MIME type,
 * time) and its body (none, or its bytes) -- strings and byte runs as a
 * 32-bit length before them, numbers as 32 or 64 bits, little-endian.
 * The numbers are given out in turn, and as sixteen hexadecimal digits
 * in the keys they put the caches and the entries back in the order
 * they were made. A save writes only the caches and entries made and
 * deleted since the last, and an entry again when its body has been
 * read since. An older browser's record ("BRC1", the whole of them in
 * one) is read as before and written in the new form at the next save.
 */
int		g_store = -1;
constexpr size_t kKeepMax = 64u << 20;	/* bodies past this many bytes in all are not kept */

/* What the store holds, as this process last handed it over: by key, the bytes of the body kept */
struct Saved {
	bool	body;
	size_t	bytes;
};
std::map<std::string, Saved>	g_saved;	/* on the services' thread */
size_t				g_saved_bodies;

uint64_t NextSeq()
{
	static uint64_t seq;
	return ++seq;
}

void SeqAtLeast( uint64_t v )
{
	while ( NextSeq() < v ) ;
}

std::string KeyOf( char kind, uint64_t seq )
{
	char b[24];
	snprintf(b, sizeof(b), "%c%016llx", kind, (unsigned long long)seq);
	return b;
}

void PutU32( std::string &o, uint32_t v )
{
	for ( int i = 0; i < 4; i++ ) o.push_back((char)( v >> ( 8 * i ) ));
}
void PutU64( std::string &o, uint64_t v )
{
	for ( int i = 0; i < 8; i++ ) o.push_back((char)( v >> ( 8 * i ) ));
}
void PutStr( std::string &o, const std::string &v )
{
	PutU32(o, (uint32_t)v.size());
	o += v;
}
void PutStr( std::string &o, const blink::String &v )
{
	PutStr(o, v.IsNull() ? std::string() : v.Utf8());
}

struct Reader {
	const std::string	&s;
	size_t			at = 0;
	bool			bad = false;

	uint32_t U32()
	{
		if ( at + 4 > s.size() ) {
			bad = true;
			return 0;
		}
		uint32_t v = 0;
		for ( int i = 0; i < 4; i++ ) v |= (uint32_t)(uint8_t)s[at + i] << ( 8 * i );
		at += 4;
		return v;
	}
	uint64_t U64()
	{
		uint64_t lo = U32();
		return lo | (uint64_t)U32() << 32;
	}
	std::string Str()
	{
		uint32_t n = U32();
		if ( bad || at + n > s.size() ) {
			bad = true;
			return std::string();
		}
		std::string v = s.substr(at, n);
		at += n;
		return v;
	}
	blink::String WStr() { return blink::String::FromUtf8(Str()); }
};

/* An entry's request and response, and its body when 'body' */
void PutEntry( std::string &o, const Entry &e, bool body )
{
	PutStr(o, e.url.GetString());
	PutStr(o, e.method);
	PutU32(o, e.headers.size());
	for ( const auto &kv : e.headers ) {
		PutStr(o, kv.key);
		PutStr(o, kv.value);
	}
	const bm::FetchAPIResponse &r = *e.response;
	PutU32(o, r.status_code);
	PutStr(o, r.status_text);
	PutU32(o, (uint32_t)r.response_type);
	PutU32(o, r.url_list.size());
	for ( const auto &u : r.url_list ) PutStr(o, u.GetString());
	PutU32(o, r.headers.size());
	for ( const auto &kv : r.headers ) {
		PutStr(o, kv.key);
		PutStr(o, kv.value);
	}
	PutStr(o, r.mime_type);
	PutU64(o, (uint64_t)r.response_time.ToDeltaSinceWindowsEpoch().InMicroseconds());
	PutU32(o, body ? 1 : 0);
	if ( body ) PutStr(o, e.body->bytes);
}

/* An entry read back (what PutEntry wrote): false when it is damaged */
bool GetEntry( Reader &rd, Entry *e )
{
	e->url = blink::KURL(rd.WStr());
	e->method = rd.WStr();
	uint32_t nh = rd.U32();
	for ( uint32_t h = 0; h < nh && !rd.bad; h++ ) {
		blink::String key = rd.WStr();
		e->headers.Set(key, rd.WStr());
	}
	auto r = bm::FetchAPIResponse::New();
	r->status_code = (uint16_t)rd.U32();
	r->status_text = rd.WStr();
	r->response_type = (network::mojom::FetchResponseType)rd.U32();
	uint32_t nu = rd.U32();
	for ( uint32_t u = 0; u < nu && !rd.bad; u++ ) r->url_list.push_back(blink::KURL(rd.WStr()));
	uint32_t nrh = rd.U32();
	for ( uint32_t h = 0; h < nrh && !rd.bad; h++ ) {
		blink::String key = rd.WStr();
		r->headers.Set(key, rd.WStr());
	}
	blink::String mime = rd.WStr();
	if ( !mime.empty() ) r->mime_type = mime;
	r->response_time = base::Time::FromDeltaSinceWindowsEpoch(base::Microseconds((int64_t)rd.U64()));
	if ( rd.U32() != 0 ) {
		e->body = std::make_shared<Body>();
		e->body->read = true;
		e->body->bytes = rd.Str();
		auto blob = std::make_unique<blink::BlobData>();
		blob->SetContentType(mime.IsNull() ? blink::String("") : mime);
		blob->AppendBytes(base::as_byte_span(e->body->bytes));
		r->blob = blink::BlobDataHandle::Create(std::move(blob), e->body->bytes.size());
	}
	if ( rd.bad ) return false;
	e->response = std::move(r);
	return true;
}

/*
 * The changes since the last save, on the services' thread: the caches
 * and entries made, an entry whose body has been read since it was
 * handed over, and those that are gone. NULL when nothing changed.
 */
br_store_delta *MakeDelta()
{
	br_store_delta *d = br_store_delta_new();
	std::map<std::string, Saved> now;
	for ( auto &okv : Origins() ) {
		for ( auto &c : okv.second.caches ) {
			std::string ck = KeyOf('c', c.second->seq);
			auto it = g_saved.find(ck);
			if ( it == g_saved.end() ) {
				std::string v;
				PutStr(v, okv.first);
				PutStr(v, c.first);
				br_store_delta_put(d, ck.data(), (int)ck.size(), v.data(), (int)v.size());
				now[ck] = { false, 0 };
			} else {
				now[ck] = it->second;
			}
			for ( const Entry &e : c.second->entries ) {
				std::string ek = KeyOf('e', e.seq);
				auto was = g_saved.find(ek);
				bool readable = e.body && e.body->read;
				if ( was != g_saved.end() && ( was->second.body || !readable ) ) {
					now[ek] = was->second;
					continue;
				}
				size_t had = ( was != g_saved.end() ) ? was->second.bytes : 0;
				bool body = readable && g_saved_bodies - had + e.body->bytes.size() <= kKeepMax;
				if ( was != g_saved.end() && !body ) {
					now[ek] = was->second;	/* the body read, but no room for it */
					continue;
				}
				std::string v;
				PutU64(v, c.second->seq);
				PutEntry(v, e, body);
				br_store_delta_put(d, ek.data(), (int)ek.size(), v.data(), (int)v.size());
				g_saved_bodies = g_saved_bodies - had + ( body ? e.body->bytes.size() : 0 );
				now[ek] = { body, body ? e.body->bytes.size() : 0 };
			}
		}
	}
	for ( const auto &kv : g_saved ) {
		if ( now.count(kv.first) != 0 ) continue;
		br_store_delta_del(d, kv.first.data(), (int)kv.first.size());
		g_saved_bodies -= kv.second.bytes;
	}
	g_saved = std::move(now);
	if ( br_store_delta_count(d) == 0 ) {
		br_store_delta_free(d);
		return nullptr;
	}
	return d;
}

/* The caches changed: the store asks for the changes two seconds later (once for changes close together) */
void Changed()
{
	if ( g_store >= 0 ) br_store_dirty(g_store);
}

/* Asked by the store for its changes (on the application's thread): they are made where the caches are */
void Collect( void * )
{
	br_on_service_thread(base::BindOnce([]() { br_store_answer(g_store, MakeDelta()); }));
}

/* An older browser's record read back into the caches (on the services' thread): nothing of it is in the store's new form */
void RestoreOlder( const std::string &rec )
{
	Reader rd{ rec };
	if ( rec.compare(0, 4, "BRC1") != 0 ) return;
	rd.at = 4;
	uint32_t norigins = rd.U32();
	for ( uint32_t i = 0; i < norigins && !rd.bad; i++ ) {
		Origin &org = Origins()[rd.Str()];
		uint32_t ncaches = rd.U32();
		for ( uint32_t j = 0; j < ncaches && !rd.bad; j++ ) {
			blink::String name = rd.WStr();
			auto data = std::make_shared<CacheData>();
			data->seq = NextSeq();
			uint32_t nentries = rd.U32();
			for ( uint32_t k = 0; k < nentries && !rd.bad; k++ ) {
				Entry e;
				if ( !GetEntry(rd, &e) ) break;
				e.seq = NextSeq();
				data->entries.push_back(std::move(e));
			}
			if ( !rd.bad ) org.caches.emplace_back(name, data);
		}
	}
	if ( rd.bad ) printf("browser: cache storage: the older record is damaged past a point, the rest left out\n");
}

/* The store's entries read back into the caches (on the services' thread), each body made a Blob again */
void Restore( std::vector<std::pair<std::string, std::string>> kept )
{
	std::map<uint64_t, std::shared_ptr<CacheData>> caches;
	size_t entries = 0;
	for ( auto &kv : kept ) {
		if ( kv.first.empty() ) {
			RestoreOlder(kv.second);
			g_saved[kv.first] = { false, 0 };	/* deleted at the next save */
			continue;
		}
		uint64_t seq = strtoull(kv.first.c_str() + 1, nullptr, 16);
		Reader rd{ kv.second };
		if ( kv.first[0] == 'c' ) {
			std::string origin = rd.Str();
			blink::String name = rd.WStr();
			if ( rd.bad ) continue;
			auto data = std::make_shared<CacheData>();
			data->seq = seq;
			Origins()[origin].caches.emplace_back(name, data);
			caches[seq] = data;
			g_saved[kv.first] = { false, 0 };
			SeqAtLeast(seq);
		} else if ( kv.first[0] == 'e' ) {
			auto c = caches.find(rd.U64());
			Entry e;
			if ( c == caches.end() || !GetEntry(rd, &e) ) continue;	/* left out: deleted at the next save */
			e.seq = seq;
			size_t body = e.body ? e.body->bytes.size() : 0;
			c->second->entries.push_back(std::move(e));
			g_saved[kv.first] = { body > 0, body };
			g_saved_bodies += body;
			SeqAtLeast(seq);
			entries++;
		}
	}
	/* entries of no cache, and caches of an older record: a save makes the store what was read */
	Changed();
	printf("browser: cache storage: %zu origins, %zu entries read back\n", Origins().size(), entries);
}

void BindStorage( mojo::PendingReceiver<bm::CacheStorage> r )
{
	br_on_service_thread(base::BindOnce([]( mojo::PendingReceiver<bm::CacheStorage> r ) {
		if ( g_storage == nullptr ) g_storage = new Storage();
		g_storage->Bind(std::move(r));
	}, std::move(r)));
}

}  // namespace

/* A worker's broker's receiver: true when it is the Cache Storage */
bool br_cache_storage_bind( mojo::GenericPendingReceiver &r )
{
	if ( auto c = r.As<bm::CacheStorage>() ) {
		BindStorage(std::move(c));
		return true;
	}
	return false;
}

/* A bucket's caches (buckets_tessronos.cc), by its key */
void br_cache_storage_bind_key( const std::string &key, mojo::ScopedMessagePipeHandle pipe )
{
	br_on_service_thread(base::BindOnce([]( std::string key, mojo::PendingReceiver<bm::CacheStorage> r ) {
		if ( g_storage == nullptr ) g_storage = new Storage();
		g_storage->BindKey(std::move(r), key);
	}, key, mojo::PendingReceiver<bm::CacheStorage>(std::move(pipe))));
}

/* A deleted bucket's caches, gone */
void br_cache_storage_delete_key( const std::string &key )
{
	br_on_service_thread(base::BindOnce([]( std::string key ) {
		if ( Origins().erase(key) != 0 ) Changed();
	}, key));
}

void br_cache_storage_clear( const BrClear &c )
{
	bool any = false;
	for ( auto it = Origins().begin(); it != Origins().end(); ) {
		if ( br_clear_origin_hit(c, it->first) ) {
			it = Origins().erase(it);
			any = true;
		} else {
			++it;
		}
	}
	if ( any ) Changed();
}

void br_cache_storage_sites( std::set<std::string> *out )
{
	for ( const auto &kv : Origins() ) out->insert(br_site_of(kv.first));
}

/* The binder of the frames' broker, once when the engine is up; the caches of earlier runs read back */
void br_cache_storage_init()
{
	g_store = br_store_register_keyed("caches", &Collect, nullptr, 2000);
	std::vector<std::pair<std::string, std::string>> kept;
	if ( br_store_load_entries("caches", []( void *ctx, const char *k, int kl, const unsigned char *v, int vl ) {
		     static_cast<std::vector<std::pair<std::string, std::string>> *>(ctx)->emplace_back(
			     std::string(k, (size_t)kl), std::string((const char *)v, (size_t)vl));
	     }, &kept) == 0 && !kept.empty() ) {
		br_on_service_thread(base::BindOnce(&Restore, std::move(kept)));
	}
	blink::GetEmptyBrowserInterfaceBroker().SetBinderForTesting(
		bm::CacheStorage::Name_, base::BindRepeating([]( mojo::ScopedMessagePipeHandle h ) {
			BindStorage(mojo::PendingReceiver<bm::CacheStorage>(std::move(h)));
		}));
}
