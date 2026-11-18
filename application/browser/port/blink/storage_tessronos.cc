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
 *	A change is told to the other windows of the same origin with the
 *	storage event, as a window of another tab would be told; the window
 *	that made it is not told.
 *
 *	The local store is kept an entry for each item (the origin, a zero byte
 *	and the item's key), so that browsers of windows opened apart, which are
 *	processes of their own, keep each other's items, as the tabs of a browser
 *	share them. What another browser changed is taken in and told with the
 *	storage event too; its url is then the origin.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "third_party/blink/renderer/core/dom/quota_exceeded_error.h"
#include "third_party/blink/renderer/core/event_type_names.h"
#include "third_party/blink/renderer/core/frame/frame.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/page/frame_tree.h"
#include "third_party/blink/renderer/core/page/page.h"
#include "third_party/blink/renderer/modules/storage/dom_window_storage.h"
#include "third_party/blink/renderer/modules/storage/storage_area.h"
#include "third_party/blink/renderer/modules/storage/storage_event.h"
#include "third_party/blink/renderer/platform/bindings/exception_state.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"
#include "third_party/blink/renderer/platform/wtf/text/strcat.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

#include "../net/br_data.h"
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
 * An older browser's record, kept as the entry of the empty key: "BRLS", 1,
 * the number of origins, then for each its name, the number of its entries
 * and the entries, each string a 32-bit little-endian length and its UTF-8
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

void snapshot_local( void *, br_store_delta *all )
{
	for ( const auto &a : stores() ) {
		if ( !is_local(a.first) ) continue;
		std::string org = a.first.substr(0, a.first.size() - ( sizeof(kLocal) - 1 ));
		org.push_back('\0');
		for ( const auto &kv : a.second ) {
			std::string key = org + kv.first.Utf8(), val = kv.second.Utf8();
			br_store_delta_put(all, key.data(), (int)key.size(), val.data(), (int)val.size());
		}
	}
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
	if ( type == StorageArea::StorageType::kLocalStorage ) br_site_touch(w->GetSecurityOrigin()->ToString().Utf8());
	return stores()[name];
}

/*
 * A change told to the other windows of the origin, on each one's Storage
 * object of the same kind: of every page for the local store, which all of
 * them share, and of the page's frames for the session store. 'from' is the
 * window that made it; none when another browser did. key, old_value and
 * new_value are null for clear().
 */
static void tell_frames( Frame *top, const String &origin, LocalDOMWindow *from, StorageArea::StorageType type,
			 const String &key, const String &old_value, const String &new_value, const KURL &url )
{
	for ( Frame *f = top; f != nullptr; f = f->Tree().TraverseNext() ) {
		auto *lf = DynamicTo<LocalFrame>(f);
		LocalDOMWindow *w = ( lf != nullptr ) ? lf->DomWindow() : nullptr;
		if ( w == nullptr || w == from || w->GetSecurityOrigin()->IsOpaque()
		     || w->GetSecurityOrigin()->ToString() != origin ) {
			continue;
		}
		NonThrowableExceptionState es;
		StorageArea *area = ( type == StorageArea::StorageType::kLocalStorage )
				    ? DOMWindowStorage::localStorage(*w, es)
				    : DOMWindowStorage::sessionStorage(*w, es);
		if ( area != nullptr ) area->EnqueueStorageEvent(key, old_value, new_value, url);
	}
}

static void tell_local( const String &origin, LocalDOMWindow *from, const String &key, const String &old_value,
			const String &new_value, const KURL &url )
{
	for ( Page *page : Page::OrdinaryPages() ) {
		tell_frames(page->MainFrame(), origin, from, StorageArea::StorageType::kLocalStorage, key, old_value, new_value, url);
	}
}

static void tell_others( LocalDOMWindow *from, StorageArea::StorageType type, const String &key,
			 const String &old_value, const String &new_value )
{
	if ( from == nullptr || from->GetFrame() == nullptr ) return;
	String origin = from->GetSecurityOrigin()->ToString();
	if ( type == StorageArea::StorageType::kLocalStorage ) {
		tell_local(origin, from, key, old_value, new_value, from->Url());
	} else {
		tell_frames(&from->GetFrame()->Tree().Top(), origin, from, type, key, old_value, new_value, from->Url());
	}
}

/* An item another browser changed: 'val' NULL when it is gone */
void br_storage_take( void *, const char *key, int klen, const unsigned char *val, int vlen )
{
	const char *z = klen > 0 ? (const char *)memchr(key, 0, (size_t)klen) : nullptr;
	if ( z == nullptr ) return;
	std::string org(key, (size_t)( z - key ));
	String item = String::FromUtf8(std::string(z + 1, (size_t)( key + klen - z - 1 )));
	Items &items = stores()[org + kLocal];
	size_t i = find_key(items, item);
	String old_value = i < items.size() ? items[i].second : String(), new_value;
	if ( val != nullptr ) {
		new_value = String::FromUtf8(std::string((const char *)val, (size_t)vlen));
		if ( i < items.size() ) items[i].second = new_value;
		else items.emplace_back(item, new_value);
	} else if ( i < items.size() ) {
		items.erase(items.begin() + (long)i);
	} else {
		return;
	}
	String origin = String::FromUtf8(org);
	tell_local(origin, nullptr, item, old_value, new_value, KURL(origin));
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
	String old_value;
	if ( i < items.size() ) {
		old_value = items[i].second;
		if ( old_value == value ) return NamedPropertySetterResult::kIntercepted;
		items[i].second = value;
	} else {
		items.emplace_back(key, value);
	}
	changed(storage_type_);
	tell_others(DomWindow(), storage_type_, key, old_value, value);
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
		String old_value = items[i].second;
		items.erase(items.begin() + (long)i);
		changed(storage_type_);
		tell_others(DomWindow(), storage_type_, key, old_value, String());
	}
	return NamedPropertyDeleterResult::kDeleted;
}

void StorageArea::clear( ExceptionState &es )
{
	if ( !CanAccessStorage() ) {
		es.ThrowSecurityError(kAccessDeniedMessage);
		return;
	}
	Items &items = items_of(DomWindow(), storage_type_);
	if ( items.empty() ) return;
	items.clear();
	changed(storage_type_);
	tell_others(DomWindow(), storage_type_, String(), String(), String());
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

bool StorageArea::EnqueueStorageEvent( const String &key, const String &old_value, const String &new_value,
				       const KURL &url )
{
	if ( !should_enqueue_events_ ) return true;
	if ( DomWindow() == nullptr ) return false;
	DomWindow()->EnqueueWindowEvent(*StorageEvent::Create(event_type_names::kStorage, key, old_value,
							      new_value, url, this),
					TaskType::kDOMManipulation);
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

/* What localStorage keeps of the origins a clearing takes emptied, in place: pages hold their items by reference */
void br_storage_clear( const BrClear &c )
{
	using namespace blink;
	bool any = false;
	for ( auto &kv : stores() ) {
		size_t n = kv.first.size(), l = strlen(kLocal);
		if ( n < l || kv.first.compare(n - l, l, kLocal) != 0 || kv.second.empty() ) continue;
		if ( !br_clear_origin_hit(c, kv.first.substr(0, n - l)) ) continue;
		kv.second.clear();
		any = true;
	}
	if ( any && g_store_id >= 0 ) br_store_dirty(g_store_id);
}

void br_storage_sites( std::set<std::string> *out )
{
	using namespace blink;
	for ( const auto &kv : stores() ) {
		size_t n = kv.first.size(), l = strlen(kLocal);
		if ( n >= l && kv.first.compare(n - l, l, kLocal) == 0 && !kv.second.empty() ) out->insert(br_site_of(kv.first.substr(0, n - l)));
	}
}

/* An entry of the store read back: an item, or an older browser's whole record */
static void restore_entry( void *ctx, const char *key, int klen, const unsigned char *val, int vlen )
{
	using namespace blink;
	int *origins = (int *)ctx;
	if ( klen > 0 ) {
		const char *z = (const char *)memchr(key, 0, (size_t)klen);
		if ( z == nullptr ) return;
		stores()[std::string(key, (size_t)( z - key )) + kLocal].emplace_back(
			String::FromUtf8(std::string(z + 1, (size_t)( key + klen - z - 1 ))),
			String::FromUtf8(std::string((const char *)val, (size_t)vlen)));
		return;
	}
	Reader r{ val, (size_t)vlen };
	if ( vlen < 8 || memcmp(val, "BRLS", 4) != 0 ) return;
	r.p += 4;
	r.n -= 4;
	if ( r.u32() != 1 ) return;
	uint32_t areas = r.u32();
	for ( uint32_t a = 0; a < areas && r.ok; a++ ) {
		std::string name = r.str();
		uint32_t count = r.u32();
		Items &items = stores()[name + kLocal];
		for ( uint32_t i = 0; i < count && r.ok; i++ ) {
			std::string k = r.str(), v = r.str();
			if ( r.ok ) items.emplace_back(String::FromUtf8(k), String::FromUtf8(v));
		}
		(*origins)++;
	}
	/* entries of their own from the next save, which this asks for */
	br_store_dirty(g_store_id);
}

/* The local store read back from its object, and saved two seconds after each change */
void br_storage_restore()
{
	using namespace blink;
	int origins = 0;

	if ( g_store_id >= 0 ) return;
	g_store_id = br_store_register_shared("storage", snapshot_local, br_storage_take, nullptr, 2000);
	(void)br_store_load_shared(g_store_id, restore_entry, &origins);
	for ( const auto &kv : stores() ) {
		if ( is_local(kv.first) && !kv.second.empty() ) origins++;
	}
	printf("browser: local storage of %d origin(s) read back\n", origins);
}
