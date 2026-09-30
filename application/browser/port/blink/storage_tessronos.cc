/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	storage_tessronos.cc
 *	localStorage and sessionStorage for the engine (design 17.19).
 *
 *	The storage module keeps its data in the browser process and reaches
 *	it through Mojo (StorageController, StorageNamespace, the cached
 *	areas). There is no browser process here: the window's Storage
 *	objects (DOMWindowStorage, StorageArea) are defined in this file on a
 *	store in the process, one list of keys and values for each origin
 *	and each kind (local, session), in the order the keys were first set.
 *	The local store is written back to a real object (port/net/
 *	br_store.cc) a couple of seconds after it changes and read back when
 *	the engine starts; the session store lasts as long as the process.
 *	The storage event is not sent (no other window of the same origin
 *	shares the store).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "third_party/blink/renderer/core/dom/quota_exceeded_error.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/modules/storage/dom_window_storage.h"
#include "third_party/blink/renderer/modules/storage/storage_area.h"
#include "third_party/blink/renderer/platform/bindings/exception_state.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"
#include "third_party/blink/renderer/platform/wtf/text/strcat.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

#include "../net/br_store.h"

namespace blink {

namespace {

using Items = std::vector<std::pair<String, String>>;

/* 5 MB of UTF-16 an origin, as the browser allows */
constexpr size_t kQuota = 5 * 1024 * 1024 / 2;

std::map<std::string, Items> &stores()
{
	static std::map<std::string, Items> s;
	return s;
}

size_t find_key( const Items &items, const String &key )
{
	for ( size_t i = 0; i < items.size(); i++ ) {
		if ( items[i].first == key ) return i;
	}
	return items.size();
}

size_t used( const Items &items )
{
	size_t n = 0;
	for ( const auto &kv : items ) n += kv.first.length() + kv.second.length();
	return n;
}

/* ------------------------------------------------ the local store kept in its object */

/*
 * "BRLS", 1, the number of origins, then for each its name, the number of
 * its entries and the entries, each string a 32-bit little-endian length
 * and its UTF-8
 */
constexpr char kLocal[] = " local";
int g_store_id = -1;

void put32( std::string *out, uint32_t v )
{
	for ( int i = 0; i < 4; i++ ) out->push_back((char)( v >> ( 8 * i ) ));
}

void put_str( std::string *out, const std::string &s )
{
	put32(out, (uint32_t)s.size());
	out->append(s);
}

struct Reader {
	const unsigned char	*p;
	size_t			n;
	bool			ok = true;

	uint32_t u32()
	{
		if ( n < 4 ) {
			ok = false;
			return 0;
		}
		uint32_t v = p[0] | ( p[1] << 8 ) | ( p[2] << 16 ) | ( (uint32_t)p[3] << 24 );
		p += 4;
		n -= 4;
		return v;
	}
	std::string str()
	{
		uint32_t len = u32();
		if ( !ok || len > n ) {
			ok = false;
			return std::string();
		}
		std::string s((const char *)p, len);
		p += len;
		n -= len;
		return s;
	}
};

bool is_local( const std::string &name )
{
	size_t k = sizeof(kLocal) - 1;
	return name.size() > k && name.compare(name.size() - k, k, kLocal) == 0;
}

int write_local( void *, unsigned char **data, int *len )
{
	std::string out("BRLS", 4);
	put32(&out, 1);
	uint32_t areas = 0;
	for ( const auto &a : stores() ) {
		if ( is_local(a.first) && !a.second.empty() ) areas++;
	}
	put32(&out, areas);
	for ( const auto &a : stores() ) {
		if ( !is_local(a.first) || a.second.empty() ) continue;
		put_str(&out, a.first.substr(0, a.first.size() - ( sizeof(kLocal) - 1 )));
		put32(&out, (uint32_t)a.second.size());
		for ( const auto &kv : a.second ) {
			put_str(&out, kv.first.Utf8());
			put_str(&out, kv.second.Utf8());
		}
	}
	*data = (unsigned char *)malloc(out.size());
	if ( *data == nullptr ) return -1;
	memcpy(*data, out.data(), out.size());
	*len = (int)out.size();
	return 0;
}

void changed( StorageArea::StorageType type )
{
	if ( type == StorageArea::StorageType::kLocalStorage ) br_store_dirty(g_store_id);
}

}  // namespace

/* ---------------------------------------------------------------- the window's two */

const char DOMWindowStorage::kSupplementName[] = "DOMWindowStorage";

DOMWindowStorage::DOMWindowStorage( LocalDOMWindow &window ) : Supplement<LocalDOMWindow>(window) {}

void DOMWindowStorage::Trace( Visitor *visitor ) const
{
	visitor->Trace(session_storage_);
	visitor->Trace(local_storage_);
	Supplement<LocalDOMWindow>::Trace(visitor);
}

DOMWindowStorage &DOMWindowStorage::From( LocalDOMWindow &window )
{
	DOMWindowStorage *supplement = Supplement<LocalDOMWindow>::From<DOMWindowStorage>(window);
	if ( !supplement ) {
		supplement = MakeGarbageCollected<DOMWindowStorage>(window);
		ProvideTo(window, supplement);
	}
	return *supplement;
}

StorageArea *DOMWindowStorage::sessionStorage( LocalDOMWindow &window, ExceptionState &es )
{
	return From(window).sessionStorage(es);
}

StorageArea *DOMWindowStorage::localStorage( LocalDOMWindow &window, ExceptionState &es )
{
	return From(window).localStorage(es);
}

StorageArea *DOMWindowStorage::sessionStorage( ExceptionState &es ) const
{
	StorageArea *s = GetOrCreateSessionStorage(es, {});
	if ( s != nullptr && !s->CanAccessStorage() ) {
		es.ThrowSecurityError(StorageArea::kAccessDeniedMessage);
		return nullptr;
	}
	return s;
}

StorageArea *DOMWindowStorage::localStorage( ExceptionState &es ) const
{
	StorageArea *s = GetOrCreateLocalStorage(es, {});
	if ( s != nullptr && !s->CanAccessStorage() ) {
		es.ThrowSecurityError(StorageArea::kAccessDeniedMessage);
		return nullptr;
	}
	return s;
}

void DOMWindowStorage::InitSessionStorage( mojo::PendingRemote<mojom::blink::StorageArea> ) const {}
void DOMWindowStorage::InitLocalStorage( mojo::PendingRemote<mojom::blink::StorageArea> ) const {}

StorageArea *DOMWindowStorage::GetOrCreateSessionStorage(
	ExceptionState &, mojo::PendingRemote<mojom::blink::StorageArea> ) const
{
	if ( !session_storage_ ) {
		session_storage_ = StorageArea::Create(GetSupplementable(), nullptr,
						       StorageArea::StorageType::kSessionStorage);
	}
	return session_storage_.Get();
}

StorageArea *DOMWindowStorage::GetOrCreateLocalStorage(
	ExceptionState &, mojo::PendingRemote<mojom::blink::StorageArea> ) const
{
	if ( !local_storage_ ) {
		local_storage_ = StorageArea::Create(GetSupplementable(), nullptr,
						     StorageArea::StorageType::kLocalStorage);
	}
	return local_storage_.Get();
}

/* ---------------------------------------------------------------- a Storage object */

const char StorageArea::kAccessDataMessage[] = "Storage is disabled inside 'data:' URLs.";
const char StorageArea::kAccessDeniedMessage[] = "Access is denied for this document.";
const char StorageArea::kAccessSandboxedMessage[] =
	"The document is sandboxed and lacks the 'allow-same-origin' flag.";

StorageArea *StorageArea::Create( LocalDOMWindow *window, scoped_refptr<CachedStorageArea> area,
				  StorageType type )
{
	return MakeGarbageCollected<StorageArea>(window, std::move(area), type, true);
}

StorageArea *StorageArea::CreateForInspectorAgent( LocalDOMWindow *window,
						   scoped_refptr<CachedStorageArea> area,
						   StorageType type )
{
	return MakeGarbageCollected<StorageArea>(window, std::move(area), type, false);
}

StorageArea::StorageArea( LocalDOMWindow *window, scoped_refptr<CachedStorageArea> area,
			  StorageType type, bool should_enqueue_events )
	: ExecutionContextClient(window),
	  cached_area_(std::move(area)),
	  storage_type_(type),
	  should_enqueue_events_(should_enqueue_events) {}

/* The store of this object's origin and kind */
static Items &items_of( const LocalDOMWindow *w, StorageArea::StorageType type )
{
	std::string name = w->GetSecurityOrigin()->ToString().Utf8();
	name += ( type == StorageArea::StorageType::kLocalStorage ) ? kLocal : " session";
	return stores()[name];
}

unsigned StorageArea::length( ExceptionState &es ) const
{
	if ( !CanAccessStorage() ) {
		es.ThrowSecurityError(kAccessDeniedMessage);
		return 0;
	}
	return (unsigned)items_of(DomWindow(), storage_type_).size();
}

String StorageArea::key( unsigned index, ExceptionState &es ) const
{
	if ( !CanAccessStorage() ) {
		es.ThrowSecurityError(kAccessDeniedMessage);
		return String();
	}
	const Items &items = items_of(DomWindow(), storage_type_);
	return index < items.size() ? items[index].first : String();
}

String StorageArea::getItem( const String &key, ExceptionState &es ) const
{
	if ( !CanAccessStorage() ) {
		es.ThrowSecurityError(kAccessDeniedMessage);
		return String();
	}
	const Items &items = items_of(DomWindow(), storage_type_);
	size_t i = find_key(items, key);
	return i < items.size() ? items[i].second : String();
}

NamedPropertySetterResult StorageArea::setItem( const String &key, const String &value,
						ExceptionState &es )
{
	if ( !CanAccessStorage() ) {
		es.ThrowSecurityError(kAccessDeniedMessage);
		return NamedPropertySetterResult::kIntercepted;
	}
	Items &items = items_of(DomWindow(), storage_type_);
	size_t i = find_key(items, key);
	size_t now = used(items) - ( i < items.size() ? key.length() + items[i].second.length() : 0 );
	if ( now + key.length() + value.length() > kQuota ) {
		QuotaExceededError::Throw(es, StrCat({"Setting the value of '", key,
						       "' exceeded the quota."}));
		return NamedPropertySetterResult::kIntercepted;
	}
	if ( i < items.size() ) {
		items[i].second = value;
	} else {
		items.emplace_back(key, value);
	}
	changed(storage_type_);
	return NamedPropertySetterResult::kIntercepted;
}

NamedPropertyDeleterResult StorageArea::removeItem( const String &key, ExceptionState &es )
{
	if ( !CanAccessStorage() ) {
		es.ThrowSecurityError(kAccessDeniedMessage);
		return NamedPropertyDeleterResult::kDidNotDelete;
	}
	Items &items = items_of(DomWindow(), storage_type_);
	size_t i = find_key(items, key);
	if ( i < items.size() ) {
		items.erase(items.begin() + (long)i);
		changed(storage_type_);
	}
	return NamedPropertyDeleterResult::kDeleted;
}

void StorageArea::clear( ExceptionState &es )
{
	if ( !CanAccessStorage() ) {
		es.ThrowSecurityError(kAccessDeniedMessage);
		return;
	}
	items_of(DomWindow(), storage_type_).clear();
	changed(storage_type_);
}

bool StorageArea::Contains( const String &key, ExceptionState &es ) const
{
	if ( !CanAccessStorage() ) {
		es.ThrowSecurityError(kAccessDeniedMessage);
		return false;
	}
	const Items &items = items_of(DomWindow(), storage_type_);
	return find_key(items, key) < items.size();
}

void StorageArea::NamedPropertyEnumerator( Vector<String> &names, ExceptionState &es )
{
	if ( !CanAccessStorage() ) {
		es.ThrowSecurityError(kAccessDeniedMessage);
		return;
	}
	for ( const auto &kv : items_of(DomWindow(), storage_type_) ) names.push_back(kv.first);
}

bool StorageArea::NamedPropertyQuery( const AtomicString &name, ExceptionState &es )
{
	if ( name == "length" ) return false;
	bool found = Contains(name, es);
	return found && !es.HadException();
}

void StorageArea::Trace( Visitor *visitor ) const
{
	ScriptWrappable::Trace(visitor);
	ExecutionContextClient::Trace(visitor);
}

/* A window with an origin of its own may: not an opaque one (data:, a sandbox) */
bool StorageArea::CanAccessStorage() const
{
	return DomWindow() != nullptr && !DomWindow()->GetSecurityOrigin()->IsOpaque();
}

KURL StorageArea::GetPageUrl() const
{
	return DomWindow() ? DomWindow()->Url() : KURL();
}

bool StorageArea::EnqueueStorageEvent( const String &, const String &, const String &, const KURL & )
{
	return true;
}

WebScopedVirtualTimePauser StorageArea::CreateWebScopedVirtualTimePauser(
	const char *, WebScopedVirtualTimePauser::VirtualTaskDuration )
{
	return WebScopedVirtualTimePauser();
}

LocalDOMWindow *StorageArea::GetDOMWindow()
{
	return DomWindow();
}

void StorageArea::RecordModificationInMetrics() {}
void StorageArea::OnDocumentActivatedForPrerendering() {}

}  // namespace blink

/* The local store read back from its object, and saved two seconds after each change */
void br_storage_restore()
{
	using namespace blink;
	unsigned char	*data = nullptr;
	int		len = 0, origins = 0;

	if ( g_store_id >= 0 ) return;
	g_store_id = br_store_register("storage", write_local, nullptr, 2000);
	if ( br_store_load("storage", &data, &len) != 0 ) return;
	Reader r{ data, (size_t)len };
	if ( len >= 8 && memcmp(data, "BRLS", 4) == 0 ) {
		r.p += 4;
		r.n -= 4;
		if ( r.u32() == 1 ) {
			uint32_t areas = r.u32();
			for ( uint32_t a = 0; a < areas && r.ok; a++ ) {
				std::string name = r.str();
				uint32_t count = r.u32();
				Items &items = stores()[name + kLocal];
				for ( uint32_t i = 0; i < count && r.ok; i++ ) {
					std::string k = r.str(), v = r.str();
					if ( r.ok ) items.emplace_back(String::FromUtf8(k), String::FromUtf8(v));
				}
				origins++;
			}
		}
	}
	free(data);
	printf("browser: local storage of %d origin(s) read back\n", origins);
}
