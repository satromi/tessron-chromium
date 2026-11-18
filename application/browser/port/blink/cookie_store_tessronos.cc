/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	cookie_store_tessronos.cc
 *	ブラウザ: the cookies of a page's scripts through cookieStore
 *	(design 14.4.5).
 *
 *	What the network service does for Blink's modules/cookie_store, on
 *	the network stack's own jar (br_cookies.h): cookieStore.get and
 *	getAll read the cookies of the page's address a script may see,
 *	set and delete put a cookie made from what the script gave (a
 *	deleted one expired), and the change events come from the jar's own
 *	dispatcher of changes, whichever way the cookie changed (a response,
 *	document.cookie, cookieStore). HttpOnly cookies are neither shown
 *	nor told of. The jar is reached on the network's thread and waited
 *	for; the service is on the services' thread.
 */

#include <stdio.h>

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/read_only_shared_memory_region.h"
#include "base/memory/weak_ptr.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "net/cookies/canonical_cookie.h"
#include "net/cookies/cookie_inclusion_status.h"
#include "services/network/public/mojom/cookie_manager.mojom-blink.h"
#include "services/network/public/mojom/restricted_cookie_manager.mojom-blink.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "url/gurl.h"

#include "../../br.h"
#include "../net/br_cookies.h"

/* The services' thread (blob_tessronos.cc) */
void br_on_service_thread( base::OnceClosure task );

namespace {

namespace nm = network::mojom::blink;

GURL gurl_of( const blink::KURL &url )
{
	return GURL(url.GetString().Utf8());
}

nm::CookieAccessResultPtr allowed()
{
	return nm::CookieAccessResult::New(nm::CookieEffectiveSameSite::kUndefined, nm::CookieAccessSemantics::NONLEGACY,
					   nm::CookieScopeSemantics::NONLEGACY, net::CookieInclusionStatus(), true);
}

class Cookies final : public nm::RestrictedCookieManager {
public:
	~Cookies() override
	{
		for ( auto &w : watches_ ) br_net_cookie_unwatch(w.first);
	}

	void GetAllForUrl( const blink::KURL &url, const net::SiteForCookies &, const scoped_refptr<const blink::SecurityOrigin> &,
			   net::StorageAccessApiStatus, nm::CookieManagerGetOptionsPtr options, bool, bool, bool,
			   GetAllForUrlCallback cb ) override
	{
		std::string name = options->name.Utf8();
		blink::Vector<nm::CookieWithAccessResultPtr> out;
		for ( const net::CanonicalCookie &c : br_net_cookies(gurl_of(url)) ) {
			if ( options->match_type == nm::CookieMatchType::EQUALS ) {
				if ( !options->name.IsNull() && !name.empty() && c.Name() != name ) continue;
			} else if ( c.Name().compare(0, name.size(), name) != 0 ) {
				continue;
			}
			out.push_back(nm::CookieWithAccessResult::New(c, allowed()));
		}
		std::move(cb).Run(std::move(out));
	}

	void SetCanonicalCookie( nm::RestrictedCanonicalCookieParamsPtr p, const blink::KURL &url, const net::SiteForCookies &,
				 const scoped_refptr<const blink::SecurityOrigin> &, net::StorageAccessApiStatus, bool, bool,
				 SetCanonicalCookieCallback cb ) override
	{
		GURL u = gurl_of(url);
		net::CookieInclusionStatus status;
		if ( p->http_only ) {
			std::move(cb).Run(false);	/* a script makes no HttpOnly cookie */
			return;
		}
		std::unique_ptr<net::CanonicalCookie> c = net::CanonicalCookie::CreateSanitizedCookie(
			u, p->name.Utf8(), p->value.Utf8(), p->domain.Utf8(), p->path.Utf8(), p->creation, p->expires,
			p->last_access, p->secure, false, static_cast<net::CookieSameSite>(p->same_site),
			static_cast<net::CookiePriority>(p->priority), std::nullopt, &status);
		if ( !c ) {
			printf("browser: cookieStore: %s refused (%s)\n", p->name.Utf8().c_str(), status.GetDebugString().c_str());
			std::move(cb).Run(false);
			return;
		}
		std::move(cb).Run(br_net_cookie_put(std::move(c), u));
	}

	void AddChangeListener( const blink::KURL &url, const net::SiteForCookies &, const scoped_refptr<const blink::SecurityOrigin> &,
				net::StorageAccessApiStatus, mojo::PendingRemote<nm::CookieChangeListener> listener,
				AddChangeListenerCallback cb ) override
	{
		base::WeakPtr<Cookies> self = weak_.GetWeakPtr();
		auto id = std::make_shared<std::atomic<int>>(0);
		int w = br_net_cookie_watch(gurl_of(url), base::BindRepeating([]( base::WeakPtr<Cookies> self,
										   std::shared_ptr<std::atomic<int>> id,
										   const net::CookieChangeInfo &info ) {
			br_on_service_thread(base::BindOnce(&Cookies::Changed, self, id->load(), info));
		}, self, id));
		if ( w > 0 ) {
			*id = w;
			watches_[w].Bind(std::move(listener));
		}
		std::move(cb).Run();
	}

	void SetCookieFromString( const blink::KURL &url, const net::SiteForCookies &, const scoped_refptr<const blink::SecurityOrigin> &,
				  net::StorageAccessApiStatus, bool, bool, const blink::String &cookie ) override
	{
		br_cookie_set(url.GetString().Utf8(), cookie.Utf8(), true);
	}

	void GetCookiesString( const blink::KURL &url, const net::SiteForCookies &, const scoped_refptr<const blink::SecurityOrigin> &,
			       net::StorageAccessApiStatus, bool, bool, bool, bool, GetCookiesStringCallback cb ) override
	{
		std::string line = br_cookie_get(url.GetString().Utf8(), true);
		std::move(cb).Run(0, base::ReadOnlySharedMemoryRegion(), blink::String::FromUtf8(line));
	}

	void CookiesEnabledFor( const blink::KURL &, const net::SiteForCookies &, const scoped_refptr<const blink::SecurityOrigin> &,
				net::StorageAccessApiStatus, bool, CookiesEnabledForCallback cb ) override
	{
		std::move(cb).Run(true);
	}

private:
	/* a change in the jar: to the listener of the watch, unless it is an HttpOnly cookie */
	void Changed( int w, net::CookieChangeInfo info )
	{
		auto l = watches_.find(w);
		if ( l == watches_.end() || !l->second.is_connected() || info.cookie.IsHttpOnly() ) return;
		l->second->OnCookieChange(nm::CookieChangeInfo::New(info.cookie, allowed(),
								   static_cast<nm::CookieChangeCause>(info.cause)));
	}

	std::map<int, mojo::Remote<nm::CookieChangeListener>>	watches_;
	base::WeakPtrFactory<Cookies>				weak_{this};
};

}  // namespace

/* The binder of the frames' broker, once when the engine is up */
void br_cookie_store_init()
{
	blink::GetEmptyBrowserInterfaceBroker().SetBinderForTesting(
		nm::RestrictedCookieManager::Name_, base::BindRepeating([]( mojo::ScopedMessagePipeHandle h ) {
			br_on_service_thread(base::BindOnce([]( mojo::PendingReceiver<nm::RestrictedCookieManager> r ) {
				/* one for each page's window: its listeners go with it */
				mojo::MakeSelfOwnedReceiver(std::make_unique<Cookies>(), std::move(r));
			}, mojo::PendingReceiver<nm::RestrictedCookieManager>(std::move(h))));
		}));
}
