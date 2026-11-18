/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	worker_tessronos.cc
 *	ブラウザ: what a dedicated worker (new Worker) asks of the browser,
 *	answered in this process (design 14.4.4).
 *
 *	Blink runs a dedicated worker itself: its thread, its V8 isolate,
 *	its global scope, the messages between it and the page (postMessage
 *	with the values and the objects transferred), its script and the
 *	loads it makes (fetch, XMLHttpRequest, importScripts). Around that a
 *	browser gives it three things, which are made here:
 *
 *	- The worker's host. Blink asks for it when the worker starts
 *	  (WebDedicatedWorkerHostFactoryClient::CreateWorkerHost) and goes
 *	  on when told that it is made and that the script is on its way.
 *	  Here the worker is told so at once, with a broker that answers for
 *	  Blobs and their URLs (blob_tessronos.cc), and with no script
 *	  fetched for it: the worker then fetches its script itself, on its
 *	  own thread, as a worker did before the browser fetched it. For a
 *	  blob: URL the token Blink resolved the URL to is waited for first,
 *	  and kept while the worker lives, so that the URL may be revoked as
 *	  soon as the worker is made (as pages do) and the script still found.
 *	- The fetch context of the worker's loads (WebWorkerFetchContext),
 *	  whose loader factory makes the same loader a page's loads use
 *	  (br_loader.cc). Its answers come back to the worker's thread. A
 *	  blob: URL the worker loads is resolved by Blink to the store's
 *	  loader factory and loaded through it.
 *	- The origin the worker runs in: that of its script's URL (a data:
 *	  URL's is opaque).
 */

#include <stdio.h>

#include <memory>
#include <string>

#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/task/single_thread_task_runner.h"
#include "base/unguessable_token.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/cookies/site_for_cookies.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/wrapper_shared_url_loader_factory.h"
#include "third_party/blink/public/common/loader/url_loader_throttle.h"
#include "third_party/blink/public/common/loader/worker_main_script_load_parameters.h"
#include "third_party/blink/public/mojom/blob/blob_url_store.mojom-blink.h"
#include "third_party/blink/public/mojom/browser_interface_broker.mojom-blink.h"
#include "third_party/blink/public/platform/web_dedicated_worker.h"
#include "third_party/blink/public/platform/web_dedicated_worker_host_factory_client.h"
#include "third_party/blink/public/platform/web_policy_container.h"
#include "third_party/blink/public/platform/web_security_origin.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/platform/web_url.h"
#include "third_party/blink/public/platform/web_worker_fetch_context.h"
#include "third_party/blink/renderer/platform/loader/fetch/url_loader/url_loader.h"
#include "third_party/blink/renderer/platform/loader/fetch/url_loader/url_loader_factory.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"

/* The loader of a page's loads, answering on the thread that starts it (br_loader.cc) */
std::unique_ptr<blink::URLLoader> br_blink_worker_url_loader();

/* A broker for the worker of an origin (blob_tessronos.cc) */
mojo::PendingRemote<blink::mojom::blink::BrowserInterfaceBroker> br_worker_broker( const std::string &origin );

namespace {

/* The worker's loads: through the network stack, as a page's are */
class BrWorkerLoaderFactory final : public blink::URLLoaderFactory {
public:
	std::unique_ptr<blink::URLLoader> CreateURLLoader(
		const network::ResourceRequest &, scoped_refptr<base::SingleThreadTaskRunner>,
		scoped_refptr<base::SingleThreadTaskRunner>, mojo::PendingRemote<blink::mojom::blink::KeepAliveHandle>,
		blink::BackForwardCacheLoaderHelper *,
		blink::Vector<std::unique_ptr<blink::URLLoaderThrottle>> ) override
	{
		return br_blink_worker_url_loader();
	}
};

class BrWorkerFetchContext final : public blink::WebWorkerFetchContext {
public:
	void SetTerminateSyncLoadEvent( base::WaitableEvent *e ) override { terminate_ = e; }

	/* made on the worker's thread, where its loads start */
	void InitializeOnWorkerThread( blink::AcceptLanguagesWatcher * ) override
	{
		factory_ = std::make_unique<BrWorkerLoaderFactory>();
	}

	blink::URLLoaderFactory *GetURLLoaderFactory() override { return factory_.get(); }

	/* a factory Blink resolved a URL to (a blob: URL's, through the store) */
	std::unique_ptr<blink::URLLoaderFactory> WrapURLLoaderFactory(
		blink::CrossVariantMojoRemote<network::mojom::URLLoaderFactoryInterfaceBase> factory ) override
	{
		return std::make_unique<blink::URLLoaderFactory>(
			base::MakeRefCounted<network::WrapperSharedURLLoaderFactory>(
				mojo::PendingRemote<network::mojom::URLLoaderFactory>(std::move(factory))),
			blink::Vector<blink::String>(), terminate_);
	}

	void FinalizeRequest( blink::WebURLRequest & ) override {}

	std::vector<std::unique_ptr<blink::URLLoaderThrottle>> CreateThrottles( const network::ResourceRequest & ) override
	{
		return {};
	}

	blink::mojom::ControllerServiceWorkerMode GetControllerServiceWorkerMode() const override
	{
		return blink::mojom::ControllerServiceWorkerMode::kNoController;
	}

	net::SiteForCookies SiteForCookies() const override { return net::SiteForCookies(); }

	std::optional<blink::WebSecurityOrigin> TopFrameOrigin() const override { return std::nullopt; }

	blink::WebString GetAcceptLanguages() const override { return blink::WebString::FromAscii("ja,en-US,en"); }

	bool IsDedicatedWorkerOrSharedWorkerFetchContext() const override { return true; }

private:
	~BrWorkerFetchContext() override = default;

	std::unique_ptr<BrWorkerLoaderFactory>	factory_;
	base::WaitableEvent			*terminate_ = nullptr;
};

/*
 * The host of one worker, from Blink's side: told at once that it is
 * made and that the script will be fetched by the worker
 */
class BrWorkerHostFactoryClient final : public blink::WebDedicatedWorkerHostFactoryClient {
public:
	explicit BrWorkerHostFactoryClient( blink::WebDedicatedWorker *worker ) : worker_(worker) {}

	void CreateWorkerHost( const blink::DedicatedWorkerToken &, const blink::WebURL &script_url,
			       network::mojom::CredentialsMode, const blink::WebFetchClientSettingsObject &,
			       blink::CrossVariantMojoRemote<blink::mojom::BlobURLTokenInterfaceBase> blob_url_token,
			       net::StorageAccessApiStatus ) override
	{
		url_ = script_url;
		std::string u = blink::KURL(url_).GetString().Utf8();
		if ( u.size() > 120 ) u = u.substr(0, 120) + "...";
		printf("browser: worker: %s\n", u.c_str());
		mojo::PendingRemote<blink::mojom::blink::BlobURLToken> token(std::move(blob_url_token));
		if ( token.is_valid() ) {
			/* the store has seen the URL's registration once it answers for the token */
			token_.Bind(std::move(token));
			token_->GetToken(mojo::WrapCallbackWithDefaultInvokeIfNotRun(
				base::BindOnce(&BrWorkerHostFactoryClient::TokenCame, weak_.GetWeakPtr()),
				base::UnguessableToken()));
			return;
		}
		base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
			FROM_HERE, base::BindOnce(&BrWorkerHostFactoryClient::Start, weak_.GetWeakPtr()));
	}

	scoped_refptr<blink::WebWorkerFetchContext> CloneWorkerFetchContext(
		blink::WebWorkerFetchContext *, scoped_refptr<base::SingleThreadTaskRunner> ) override
	{
		return base::MakeRefCounted<BrWorkerFetchContext>();
	}

private:
	void TokenCame( const base::UnguessableToken &t )
	{
		if ( t.is_empty() ) printf("browser: worker: its blob: URL is not registered\n");
		Start();
	}

	void Start()
	{
		if ( started_ ) return;
		started_ = true;
		blink::KURL url(url_);
		scoped_refptr<blink::SecurityOrigin> origin = blink::SecurityOrigin::Create(url);
		worker_->OnWorkerHostCreated(br_worker_broker(origin->ToString().Utf8()),
					     blink::CrossVariantMojoRemote<blink::mojom::DedicatedWorkerHostInterfaceBase>(),
					     blink::WebSecurityOrigin(origin));
		worker_->OnScriptLoadStarted(nullptr,
					     blink::CrossVariantMojoRemote<
						     blink::mojom::BackForwardCacheControllerHostInterfaceBase>(),
					     nullptr,
					     blink::CrossVariantMojoReceiver<blink::mojom::ReportingObserverInterfaceBase>(),
					     blink::CrossVariantMojoReceiver<blink::mojom::ReportingObserverInterfaceBase>());
	}

	blink::WebDedicatedWorker				*worker_;
	blink::WebURL						url_;
	mojo::Remote<blink::mojom::blink::BlobURLToken>		token_;
	bool							started_ = false;
	base::WeakPtrFactory<BrWorkerHostFactoryClient>		weak_{this};
};

}  // namespace

std::unique_ptr<blink::WebDedicatedWorkerHostFactoryClient> br_worker_host_factory_client(
	blink::WebDedicatedWorker *worker )
{
	return std::make_unique<BrWorkerHostFactoryClient>(worker);
}

scoped_refptr<blink::WebWorkerFetchContext> br_worker_fetch_context()
{
	return base::MakeRefCounted<BrWorkerFetchContext>();
}
