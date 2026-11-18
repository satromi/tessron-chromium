/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	service_worker_tessronos.cc
 *	ブラウザ: service workers, whose registrations and hosts a browser's
 *	process keeps, kept in this process (design 14.4.6).
 *
 *	A page registers a worker for a scope (navigator.serviceWorker,
 *	through WebServiceWorkerProvider): the registration is kept here, by
 *	its scope, while the browser runs, and its worker is started in this
 *	process (WebEmbeddedWorker, on a thread of its own; it fetches its
 *	script itself, as a dedicated worker does). The worker is installed
 *	and activated (its install and activate events), and from then on:
 *
 *	  - a document whose address is in the scope, the page's or an
 *	    iframe's, loaded after that or claimed by the worker
 *	    (clients.claim()), is controlled by it: the document's loads go
 *	    to the worker as fetch events first (br_sw_subresource, from
 *	    br_loader.cc), and what it does not answer goes to the network.
 *	    An about:blank or about:srcdoc iframe is controlled by what
 *	    controls its parent;
 *	  - going to an address in the scope asks the worker first too, in
 *	    the main frame (br_sw_navigate, from br_main.cc) and in an
 *	    iframe (br_sw_navigate_frame, from br_blink.cc);
 *	  - messages go between the documents and the worker both ways, and
 *	    the worker sees each document as one of its clients (Client),
 *	    which the document's frame knows by its UUID.
 *
 *	The browser's other services give the workers their events (sync,
 *	push, notifications, background fetch) through br_sw_dispatch.
 *
 *	The registrations, the workers and their hosts are on the page's
 *	thread, which starts the workers. What the page is handed (its
 *	registration objects, its worker objects) is on pipes of their own,
 *	for there is no browser's channel to associate them with.
 *
 *	A worker, once started, runs while the browser runs. When it has had
 *	no event for its idle delay (30 seconds) it asks its host to stop it
 *	(RequestTermination) and holds every event that comes after, unstarted,
 *	until it is answered. It is answered that it is not stopped, and its
 *	idle delay is set back to the default: a worker left unanswered keeps
 *	those events (the page's fetches among them) until each times out,
 *	five minutes later.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/run_loop.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "base/unguessable_token.h"
#include "base/values.h"
#include "mojo/public/cpp/bindings/associated_receiver.h"
#include "mojo/public/cpp/bindings/associated_receiver_set.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "mojo/public/cpp/bindings/pending_associated_receiver.h"
#include "mojo/public/cpp/bindings/pending_associated_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_associated_receiver.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "mojo/public/cpp/system/data_pipe_drainer.h"
#include "net/http/http_request_headers.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/wrapper_shared_url_loader_factory.h"
#include "third_party/blink/public/common/messaging/transferable_message.h"
#include "third_party/blink/public/common/tokens/tokens.h"
#include "third_party/blink/public/common/user_agent/user_agent_metadata.h"
#include "third_party/blink/public/mojom/associated_interfaces/associated_interfaces.mojom-blink.h"
#include "third_party/blink/public/mojom/browser_interface_broker.mojom-blink.h"
#include "third_party/blink/public/mojom/frame/policy_container.mojom-blink.h"
#include "third_party/blink/public/mojom/service_worker/controller_service_worker.mojom-blink.h"
#include "third_party/blink/public/mojom/fetch/fetch_api_request.mojom-blink.h"
#include "third_party/blink/public/mojom/fetch/fetch_api_response.mojom-blink.h"
#include "third_party/blink/public/mojom/script/script_type.mojom-shared.h"
#include "third_party/blink/public/mojom/service_worker/dispatch_fetch_event_params.mojom-blink.h"
#include "third_party/blink/public/mojom/service_worker/navigation_preload_state.mojom-blink.h"
#include "third_party/blink/public/mojom/service_worker/service_worker.mojom-blink.h"
#include "third_party/blink/public/mojom/service_worker/service_worker_ancestor_frame_type.mojom-blink.h"
#include "third_party/blink/public/mojom/service_worker/service_worker_client.mojom-blink.h"
#include "third_party/blink/public/mojom/service_worker/service_worker_database.mojom-blink.h"
#include "third_party/blink/public/mojom/service_worker/service_worker_error_type.mojom-blink.h"
#include "third_party/blink/public/mojom/service_worker/service_worker_event_status.mojom-blink.h"
#include "third_party/blink/public/mojom/service_worker/service_worker_fetch_response_callback.mojom-blink.h"
#include "third_party/blink/public/mojom/service_worker/service_worker_object.mojom-blink.h"
#include "third_party/blink/public/mojom/service_worker/service_worker_registration.mojom-blink.h"
#include "third_party/blink/public/mojom/service_worker/service_worker_state.mojom-blink.h"
#include "third_party/blink/public/mojom/service_worker/service_worker_stream_handle.mojom-blink.h"
#include "third_party/blink/public/mojom/worker/worker_content_settings_proxy.mojom-blink.h"
#include "third_party/blink/public/platform/interface_registry.h"
#include "third_party/blink/public/platform/modules/service_worker/web_service_worker_error.h"
#include "third_party/blink/public/platform/modules/service_worker/web_service_worker_object_info.h"
#include "third_party/blink/public/platform/modules/service_worker/web_service_worker_provider.h"
#include "third_party/blink/public/platform/modules/service_worker/web_service_worker_provider_client.h"
#include "third_party/blink/public/platform/modules/service_worker/web_service_worker_registration_object_info.h"
#include "third_party/blink/public/platform/modules/service_worker/web_service_worker_fetch_context.h"
#include "third_party/blink/public/platform/web_fetch_client_settings_object.h"
#include "third_party/blink/public/platform/web_policy_container.h"
#include "third_party/blink/public/platform/web_security_origin.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/platform/web_url.h"
#include "third_party/blink/public/platform/web_url_request.h"
#include "third_party/blink/public/web/modules/service_worker/web_service_worker_context_client.h"
#include "third_party/blink/public/web/modules/service_worker/web_service_worker_context_proxy.h"
#include "third_party/blink/public/web/web_embedded_worker.h"
#include "third_party/blink/public/web/web_embedded_worker_start_data.h"
#include "third_party/blink/renderer/core/messaging/blink_transferable_message.h"
#include "third_party/blink/renderer/platform/blob/blob_data.h"
#include "third_party/blink/renderer/bindings/core/v8/serialization/serialized_script_value.h"
#include "third_party/blink/renderer/platform/loader/fetch/url_loader/url_loader.h"
#include "third_party/blink/renderer/platform/loader/fetch/url_loader/url_loader_factory.h"
#include "third_party/blink/renderer/platform/network/http_header_map.h"
#include "third_party/blink/renderer/platform/storage/blink_storage_key.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "base/functional/callback_helpers.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "../net/br_data.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "url/origin.h"

#include "../../br.h"
#include "../net/br_store.h"

/* What the other parts of the port give */
mojo::PendingRemote<blink::mojom::blink::BrowserInterfaceBroker> br_worker_broker( const std::string &origin );	/* blob_tessronos.cc */
void br_on_service_thread( base::OnceClosure task );					/* blob_tessronos.cc */
void br_background_forget( int64_t registration_id );					/* background_tessronos.cc */
bool br_push_unsubscribe( const std::string &scope );					/* push_tessronos.cc */
void br_blob_read( const std::string &key, bool is_url,
		   base::OnceCallback<void(bool, std::string, std::string)> done );	/* blob_tessronos.cc */
const char *br_user_agent();								/* br_blink.cc */
blink::UserAgentMetadata br_user_agent_metadata();					/* br_blink.cc */
std::unique_ptr<blink::URLLoader> br_blink_worker_url_loader();				/* br_loader.cc */

void br_on_page_thread( base::OnceClosure work );

namespace {

namespace bm = blink::mojom::blink;

/* ---------------------------------------------------------------- what a worker is given */

/* What a worker may use: all of it, as the pages it runs for may */
class ContentSettings final : public bm::WorkerContentSettingsProxy {
public:
	void AllowIndexedDB( AllowIndexedDBCallback cb ) override { std::move(cb).Run(true); }
	void AllowCacheStorage( AllowCacheStorageCallback cb ) override { std::move(cb).Run(true); }
	void AllowWebLocks( AllowWebLocksCallback cb ) override { std::move(cb).Run(true); }
	void RequestFileSystemAccessSync( RequestFileSystemAccessSyncCallback cb ) override { std::move(cb).Run(true); }
};

/* The host of the worker's policy container: the worker keeps its policies itself */
class PolicyHost final : public bm::PolicyContainerHost {
public:
	void SetReferrerPolicy( network::mojom::blink::ReferrerPolicy, const base::UnguessableToken & ) override {}
	void AddContentSecurityPolicies( blink::Vector<network::mojom::blink::ContentSecurityPolicyPtr>,
					 const base::UnguessableToken & ) override {}
};

std::unique_ptr<blink::WebPolicyContainer> NewPolicyContainer()
{
	mojo::PendingAssociatedRemote<bm::PolicyContainerHost> remote;
	mojo::PendingAssociatedReceiver<bm::PolicyContainerHost> receiver = remote.InitWithNewEndpointAndPassReceiver();
	remote.EnableUnassociatedUsage();
	mojo::MakeSelfOwnedAssociatedReceiver(std::make_unique<PolicyHost>(), std::move(receiver));
	return std::make_unique<blink::WebPolicyContainer>(blink::WebPolicyContainerPolicies(), std::move(remote));
}

/* The interfaces a browser associates with the worker: none are asked of, or given */
class NoInterfaces final : public bm::AssociatedInterfaceProvider {
public:
	void GetAssociatedInterface( const blink::String &,
				     mojo::PendingAssociatedReceiver<bm::AssociatedInterface> ) override {}
};

/* The worker's own loads: through the network stack, as a dedicated worker's are */
class LoaderFactory final : public blink::URLLoaderFactory {
public:
	std::unique_ptr<blink::URLLoader> CreateURLLoader(
		const network::ResourceRequest &, scoped_refptr<base::SingleThreadTaskRunner>,
		scoped_refptr<base::SingleThreadTaskRunner>, mojo::PendingRemote<bm::KeepAliveHandle>,
		blink::BackForwardCacheLoaderHelper *,
		blink::Vector<std::unique_ptr<blink::URLLoaderThrottle>> ) override
	{
		return br_blink_worker_url_loader();
	}
};

class FetchContext final : public blink::WebServiceWorkerFetchContext {
public:
	void SetTerminateSyncLoadEvent( base::WaitableEvent * ) override {}
	void InitializeOnWorkerThread( blink::AcceptLanguagesWatcher * ) override
	{
		factory_ = std::make_unique<LoaderFactory>();
	}
	blink::URLLoaderFactory *GetURLLoaderFactory() override { return factory_.get(); }
	/* a factory Blink resolved a URL to (a blob: URL's, through the store) */
	std::unique_ptr<blink::URLLoaderFactory> WrapURLLoaderFactory(
		blink::CrossVariantMojoRemote<network::mojom::URLLoaderFactoryInterfaceBase> factory ) override
	{
		return std::make_unique<blink::URLLoaderFactory>(
			base::MakeRefCounted<network::WrapperSharedURLLoaderFactory>(
				mojo::PendingRemote<network::mojom::URLLoaderFactory>(std::move(factory))),
			blink::Vector<blink::String>(), nullptr);
	}
	void FinalizeRequest( blink::WebURLRequest & ) override {}
	std::vector<std::unique_ptr<blink::URLLoaderThrottle>> CreateThrottles( const network::ResourceRequest & ) override
	{
		return {};
	}
	bm::ControllerServiceWorkerMode GetControllerServiceWorkerMode() const override
	{
		return bm::ControllerServiceWorkerMode::kNoController;
	}
	net::SiteForCookies SiteForCookies() const override { return net::SiteForCookies(); }
	std::optional<blink::WebSecurityOrigin> TopFrameOrigin() const override { return std::nullopt; }
	blink::WebString GetAcceptLanguages() const override { return blink::WebString::FromAscii("ja,en-US,en"); }

private:
	~FetchContext() override = default;

	std::unique_ptr<LoaderFactory>	factory_;
};

/* A message of the worker (Blink's form) in the form a page's client is given */
blink::TransferableMessage ToTransferable( blink::BlinkTransferableMessage m )
{
	blink::TransferableMessage t;
	if ( m.message ) {
		base::span<const uint8_t> wire = m.message->GetWireData();
		t.owned_encoded_message.assign(wire.begin(), wire.end());
		t.encoded_message = t.owned_encoded_message;
	}
	if ( m.sender_origin ) t.sender_origin = m.sender_origin->ToUrlOrigin();
	t.sender_agent_cluster_id = m.sender_agent_cluster_id;
	t.locked_to_sender_agent_cluster = m.locked_to_sender_agent_cluster;
	for ( auto &p : m.ports ) t.ports.push_back(std::move(p));
	if ( m.user_activation ) {
		t.user_activation = blink::mojom::UserActivationSnapshot::New(m.user_activation->has_been_active,
									       m.user_activation->was_active);
	}
	return t;
}

/* ---------------------------------------------------------------- the registrations and their workers */

class Registration;
class Provider;

std::vector<Provider *>	&Providers()
{
	static auto *v = new std::vector<Provider *>();
	return *v;
}

/* The scopes registered, the longest first among those an address is in */
std::map<std::string, Registration *> &Registrations()
{
	static auto *m = new std::map<std::string, Registration *>();
	return *m;
}

int64_t	g_next_id = 1;

/*
 * A document as a client of the workers: its address, the UUID the
 * workers know it by, whether it is a page's main frame or an iframe's,
 * and the worker that controls it, or null
 */
class Version;
struct Client {
	std::string	url;
	std::string	uuid;
	bool		top = true;
	Version		*controller = nullptr;
	uint64_t	seq = 0;		/* the order the clients were made in */
	base::TimeTicks	made;
};

/* The clients there are, by their UUIDs */
std::map<std::string, Client> &Clients()
{
	static auto *m = new std::map<std::string, Client>();
	return *m;
}

/* A client by its UUID, or null */
Client *ClientOf( const std::string &uuid )
{
	auto it = Clients().find(uuid);
	return it != Clients().end() ? &it->second : nullptr;
}

/* The clients a worker controlled are let go of (it is redundant, or going) */
void Uncontrol( Version *v )
{
	for ( auto &kv : Clients() ) {
		if ( kv.second.controller == v ) kv.second.controller = nullptr;
	}
}

Registration *Match( const std::string &url );

/* The registrations written to the store ("sw") when they change */
void SaveRegistrations();

/* One worker of a registration (a version of its script), and the host it talks to */
class Version final : public blink::WebServiceWorkerContextClient, public bm::ServiceWorkerHost,
		      public bm::ServiceWorkerObjectHost {
public:
	Version( Registration *reg, const blink::KURL &script ) : id_(g_next_id++), reg_(reg), script_(script)
	{
		main_ = base::SingleThreadTaskRunner::GetCurrentDefault();
	}
	~Version() override
	{
		Uncontrol(this);
	}

	int64_t id() const { return id_; }
	const blink::KURL &script() const { return script_; }
	bm::ServiceWorkerState state() const { return state_; }
	bm::ServiceWorker *worker() { return worker_.get(); }

	/*
	 * An active worker read back from an earlier run is started when it
	 * is first wanted (an event for it, a page in its scope); it runs its
	 * script again, without install and activate events
	 */
	void Wake()
	{
		if ( started_ ) return;
		Start([this]( bool ok ) {
			if ( !ok ) printf("browser: service worker: %s did not run again\n", script_.GetString().Utf8().c_str());
		});
	}
	/*
	 * Where the page's fetch events go: a clone of the worker's controller
	 * (the first one only starts it, and takes no fetch events)
	 */
	bm::ControllerServiceWorker *controller()
	{
		if ( !page_controller_ && controller_ ) {
			controller_->Clone(page_controller_.BindNewPipeAndPassReceiver(), nullptr, nullptr);
		}
		return page_controller_ ? page_controller_.get() : nullptr;
	}

	/* The worker started: its script fetched and run; 'evaluated' told whether it ran */
	void Start( std::function<void(bool)> evaluated );

	void SetState( bm::ServiceWorkerState s )
	{
		state_ = s;
		for ( auto &o : objects_ ) {
			if ( o.is_bound() ) o->StateChanged(s);
		}
	}

	/*
	 * What a document is handed of this worker, on pipes of their own;
	 * what it sends the worker comes from that client (by its UUID)
	 */
	blink::WebServiceWorkerObjectInfo WebInfo( const std::string &client )
	{
		mojo::PendingAssociatedRemote<bm::ServiceWorkerObjectHost> host;
		hosts_.Add(this, host.InitWithNewEndpointAndPassReceiver(), client);
		host.EnableUnassociatedUsage();
		mojo::PendingAssociatedRemote<bm::ServiceWorkerObject> object;
		mojo::PendingAssociatedReceiver<bm::ServiceWorkerObject> receiver = object.InitWithNewEndpointAndPassReceiver();
		object.EnableUnassociatedUsage();
		objects_.emplace_back(std::move(object));
		return blink::WebServiceWorkerObjectInfo(id_, state_, blink::WebURL(script_), std::move(host),
							 std::move(receiver));
	}

	/* What the worker itself (or another worker) is handed of it, over the worker's pipe */
	bm::ServiceWorkerObjectInfoPtr MojoInfo()
	{
		auto info = bm::ServiceWorkerObjectInfo::New();
		info->version_id = id_;
		info->state = state_;
		info->url = script_;
		hosts_.Add(this, info->host_remote.InitWithNewEndpointAndPassReceiver(), std::string());
		mojo::PendingAssociatedRemote<bm::ServiceWorkerObject> object;
		info->receiver = object.InitWithNewEndpointAndPassReceiver();
		objects_.emplace_back(std::move(object));
		return info;
	}

	/* ---- the context client: called on the worker's thread or the page's */
	void WorkerContextStarted( blink::WebServiceWorkerContextProxy *proxy,
				   scoped_refptr<base::SequencedTaskRunner> worker_thread ) override
	{
		worker_thread_ = std::move(worker_thread);
		proxy->BindServiceWorker(std::move(worker_receiver_));
		proxy->BindControllerServiceWorker(std::move(controller_receiver_));
	}
	/*
	 * The worker, idle, asks to be stopped (on its thread); what is sent
	 * to it from now on waits in its queue until it is answered. It is
	 * answered from the page's thread, where its host is (KeepRunning).
	 */
	void RequestTermination( uint64_t, RequestTerminationCallback cb ) override
	{
		main_->PostTask(FROM_HERE, base::BindOnce(&Version::KeepRunning, self_, worker_thread_, std::move(cb)));
	}
	void DidEvaluateScript( bool success ) override
	{
		main_->PostTask(FROM_HERE, base::BindOnce(&Version::Evaluated, weak_.GetWeakPtr(), success));
	}
	void FailedToFetchClassicScript() override
	{
		main_->PostTask(FROM_HERE, base::BindOnce(&Version::Evaluated, weak_.GetWeakPtr(), false));
	}
	void FailedToFetchModuleScript() override { FailedToFetchClassicScript(); }
	void ReportException( const blink::WebString &message, int line, int, const blink::WebString &source ) override
	{
		printf("browser: service worker: %s (%s:%d)\n", message.Utf8().c_str(), source.Utf8().c_str(), line);
	}
	void ReportConsoleMessage( bm::ConsoleMessageSource, bm::ConsoleMessageLevel, const blink::WebString &message,
				   int line, const blink::WebString &source ) override
	{
		printf("browser: service worker: console: %s (%s:%d)\n", message.Utf8().c_str(), source.Utf8().c_str(),
		       line);
	}
	scoped_refptr<blink::WebServiceWorkerFetchContext> CreateWorkerFetchContextOnInitiatorThread() override
	{
		return base::MakeRefCounted<FetchContext>();
	}
	void OnNavigationPreloadResponse( int, std::unique_ptr<blink::WebURLResponse>,
					  mojo::ScopedDataPipeConsumerHandle ) override {}
	void OnNavigationPreloadComplete( int, base::TimeTicks, int64_t, int64_t, int64_t ) override {}
	void OnNavigationPreloadError( int, std::unique_ptr<blink::WebServiceWorkerError> ) override {}

	/* ---- the worker's host */
	void SetCachedMetadata( const blink::KURL &, base::span<const uint8_t> ) override {}
	void ClearCachedMetadata( const blink::KURL & ) override {}
	void GetClients( bm::ServiceWorkerClientQueryOptionsPtr options, GetClientsCallback cb ) override;
	void GetClient( const blink::String &uuid, GetClientCallback cb ) override;
	void OpenNewTab( const blink::KURL &, OpenNewTabCallback cb ) override
	{
		std::move(cb).Run(false, nullptr, "The browser opens no new windows.");
	}
	void OpenPaymentHandlerWindow( const blink::KURL &, OpenPaymentHandlerWindowCallback cb ) override
	{
		std::move(cb).Run(false, nullptr, "There is no payment handler.");
	}
	void PostMessageToClient( const blink::String &uuid, blink::BlinkTransferableMessage message ) override;
	void FocusClient( const blink::String &uuid, FocusClientCallback cb ) override;
	void NavigateClient( const blink::String &, const blink::KURL &, NavigateClientCallback cb ) override
	{
		std::move(cb).Run(false, nullptr, "The client is not navigated by its worker.");
	}
	void SkipWaiting( SkipWaitingCallback cb ) override;
	void ClaimClients( ClaimClientsCallback cb ) override;
	void AddRoutes( const blink::ServiceWorkerRouterRules &, AddRoutesCallback cb ) override
	{
		std::move(cb).Run(false);
	}

	/* ---- the worker as the page holds it */
	void PostMessageToServiceWorker( blink::BlinkTransferableMessage message ) override;
	void TerminateForTesting( TerminateForTestingCallback cb ) override { std::move(cb).Run(); }

	void Terminate()
	{
		if ( embedded_ ) embedded_->TerminateWorkerContext();
	}

private:
	void Evaluated( bool ok )
	{
		if ( evaluated_ ) {
			std::function<void(bool)> f = std::move(evaluated_);
			evaluated_ = nullptr;
			f(ok);
		}
	}

	/*
	 * The answer to the worker that asked to be stopped: it is not (it
	 * runs while the browser runs), so it starts what waits in its queue.
	 * Its idle delay is set back to the default first: after an event of
	 * its timed out, the worker makes it zero and would ask again as soon
	 * as its queue is empty.
	 */
	void KeepRunning( scoped_refptr<base::SequencedTaskRunner> worker_thread, RequestTerminationCallback cb )
	{
		if ( idle_asks_++ == 0 ) {
			printf("browser: service worker: %s idle; kept running\n", script_.GetString().Utf8().c_str());
		}
		if ( worker_ ) worker_->SetIdleDelay(base::Seconds(bm::kServiceWorkerDefaultIdleDelayInSeconds));
		if ( worker_thread ) worker_thread->PostTask(FROM_HERE, base::BindOnce(std::move(cb), false));
	}

	int64_t						id_;
	Registration					*reg_;
	blink::KURL					script_;
	bm::ServiceWorkerState				state_ = bm::ServiceWorkerState::kParsed;
	scoped_refptr<base::SingleThreadTaskRunner>	main_;
	std::unique_ptr<blink::WebEmbeddedWorker>	embedded_;
	mojo::Remote<bm::ServiceWorker>			worker_;
	mojo::PendingReceiver<bm::ServiceWorker>	worker_receiver_;
	mojo::Remote<bm::ControllerServiceWorker>	controller_, page_controller_;
	mojo::PendingReceiver<bm::ControllerServiceWorker> controller_receiver_;
	mojo::AssociatedReceiver<bm::ServiceWorkerHost>	host_{this};
	mojo::AssociatedReceiverSet<bm::ServiceWorkerObjectHost, std::string> hosts_;	/* each with its client's UUID */
	std::vector<mojo::AssociatedRemote<bm::ServiceWorkerObject>> objects_;
	mojo::AssociatedReceiver<bm::AssociatedInterfaceProvider> no_interfaces_receiver_{&no_interfaces_};
	NoInterfaces					no_interfaces_;
	mojo::PendingAssociatedRemote<bm::AssociatedInterfaceProvider> no_interfaces_remote_;
	std::function<void(bool)>			evaluated_;
	bool						started_ = false;
	scoped_refptr<base::SequencedTaskRunner>	worker_thread_;	/* the worker's own (WorkerContextStarted) */
	unsigned					idle_asks_ = 0;	/* times it asked to be stopped */
	base::WeakPtr<Version>				self_;		/* made on the page's thread, handed from the worker's */
	base::WeakPtrFactory<Version>			weak_{this};
};

/* A registration: its scope, its workers, and the hosts of what the pages hold of it */
class Registration final : public bm::ServiceWorkerRegistrationObjectHost {
public:
	Registration( const blink::KURL &scope ) : id_(g_next_id++), scope_(scope) {}

	/* One read back from an earlier run: its id, and its active worker, not started */
	Registration( const blink::KURL &scope, int64_t id, const blink::KURL &script ) : id_(id), scope_(scope)
	{
		active_ = std::make_unique<Version>(this, script);
		active_->SetState(bm::ServiceWorkerState::kActivated);
	}

	int64_t id() const { return id_; }
	const blink::KURL &scope() const { return scope_; }
	Version *active() { return active_.get(); }

	/* The active worker, started if it was not, or null */
	Version *Running()
	{
		if ( active_ ) active_->Wake();
		return active_.get();
	}
	Version *installing() { return installing_.get(); }
	Version *waiting() { return waiting_.get(); }

	/*
	 * A worker of 'script' installed and activated (the one there is
	 * kept when it is of the same script and active). 'done' is told
	 * once the script ran (true) or did not (false, with what went
	 * wrong).
	 */
	void Install( const blink::KURL &script, std::function<void(bool, const std::string &)> done );
	bool module() const { return module_; }
	void set_module( bool on ) { module_ = on; }

	/* What a document (a client, by its UUID) is handed of this registration, on pipes of their own */
	blink::WebServiceWorkerRegistrationObjectInfo WebInfo( const std::string &client )
	{
		mojo::PendingAssociatedRemote<bm::ServiceWorkerRegistrationObjectHost> host;
		hosts_.Add(this, host.InitWithNewEndpointAndPassReceiver());
		host.EnableUnassociatedUsage();
		mojo::PendingAssociatedRemote<bm::ServiceWorkerRegistrationObject> object;
		mojo::PendingAssociatedReceiver<bm::ServiceWorkerRegistrationObject> receiver =
			object.InitWithNewEndpointAndPassReceiver();
		object.EnableUnassociatedUsage();
		objects_.emplace_back(std::move(object));
		return blink::WebServiceWorkerRegistrationObjectInfo(
			id_, blink::WebURL(scope_), bm::ServiceWorkerUpdateViaCache::kImports, std::move(host),
			std::move(receiver), WebVersion(installing_.get(), client), WebVersion(waiting_.get(), client),
			WebVersion(active_.get(), client));
	}

	/* What a worker is handed of its registration, over its pipe */
	bm::ServiceWorkerRegistrationObjectInfoPtr MojoInfo()
	{
		auto info = bm::ServiceWorkerRegistrationObjectInfo::New();
		info->registration_id = id_;
		info->scope = scope_;
		info->update_via_cache = bm::ServiceWorkerUpdateViaCache::kImports;
		hosts_.Add(this, info->host_remote.InitWithNewEndpointAndPassReceiver());
		mojo::PendingAssociatedRemote<bm::ServiceWorkerRegistrationObject> object;
		info->receiver = object.InitWithNewEndpointAndPassReceiver();
		objects_.emplace_back(std::move(object));
		if ( installing_ ) info->installing = installing_->MojoInfo();
		if ( waiting_ ) info->waiting = waiting_->MojoInfo();
		if ( active_ ) info->active = active_->MojoInfo();
		return info;
	}

	/* The pages' registration objects told which workers there are now */
	void Changed()
	{
		for ( auto &o : objects_ ) {
			if ( !o.is_bound() ) continue;
			auto mask = bm::ChangedServiceWorkerObjectsMask::New(true, true, true);
			o->SetServiceWorkerObjects(std::move(mask), installing_ ? installing_->MojoInfo() : nullptr,
						   waiting_ ? waiting_->MojoInfo() : nullptr,
						   active_ ? active_->MojoInfo() : nullptr);
		}
	}

	/* The installing worker installed: it waits, or becomes the active one */
	void Installed();

	/* The waiting worker made the active one */
	void Activate();

	/* A worker asked not to wait (skipWaiting()) */
	void SkipWaiting( Version *v )
	{
		skip_ = true;
		if ( waiting_.get() == v ) Activate();
	}

	/* ---- the registration as the page holds it */
	void Update( bm::FetchClientSettingsObjectPtr, UpdateCallback cb ) override
	{
		std::move(cb).Run(bm::ServiceWorkerErrorType::kNone, blink::String());
	}
	void Unregister( UnregisterCallback cb ) override;
	std::string scope_string() const { return scope_.GetString().Utf8(); }
	void EnableNavigationPreload( bool, EnableNavigationPreloadCallback cb ) override
	{
		std::move(cb).Run(bm::ServiceWorkerErrorType::kNone, blink::String());
	}
	void GetNavigationPreloadState( GetNavigationPreloadStateCallback cb ) override
	{
		std::move(cb).Run(bm::ServiceWorkerErrorType::kNone, blink::String(),
				  bm::NavigationPreloadState::New(false, "true"));
	}
	void SetNavigationPreloadHeader( const blink::String &, SetNavigationPreloadHeaderCallback cb ) override
	{
		std::move(cb).Run(bm::ServiceWorkerErrorType::kNone, blink::String());
	}

private:
	bool				module_ = false;	/* an extension's worker of "type": "module" */
	static blink::WebServiceWorkerObjectInfo WebVersion( Version *v, const std::string &client )
	{
		if ( v != nullptr ) return v->WebInfo(client);
		return blink::WebServiceWorkerObjectInfo(bm::kInvalidServiceWorkerVersionId, bm::ServiceWorkerState::kParsed,
							 blink::WebURL(), mojo::NullAssociatedRemote(),
							 mojo::NullAssociatedReceiver());
	}

	int64_t							id_;
	blink::KURL						scope_;
	std::unique_ptr<Version>				installing_, waiting_, active_;
	mojo::AssociatedReceiverSet<bm::ServiceWorkerRegistrationObjectHost> hosts_;
	std::vector<mojo::AssociatedRemote<bm::ServiceWorkerRegistrationObject>> objects_;
	std::vector<Version *>					retired_;	/* kept: its pipes may still speak */
	bool							skip_ = false;
};

/* The registration whose scope holds the address, the longest scope if several do */
Registration *Match( const std::string &url )
{
	Registration *best = nullptr;
	size_t len = 0;
	for ( auto &kv : Registrations() ) {
		if ( url.compare(0, kv.first.size(), kv.first) == 0 && kv.first.size() >= len ) {
			best = kv.second;
			len = kv.first.size();
		}
	}
	return best;
}

/* ---------------------------------------------------------------- what a page holds */

/* The service worker container's provider of a window: of its document, a client by its UUID */
class Provider final : public blink::WebServiceWorkerProvider {
public:
	explicit Provider( const std::string &uuid ) : uuid_(uuid)
	{
		const Client *c = ClientOf(uuid);
		if ( c != nullptr ) url_ = c->url;
		Providers().push_back(this);
	}
	~Provider() override
	{
		auto &v = Providers();
		v.erase(std::remove(v.begin(), v.end(), this), v.end());
	}

	const std::string &url() const { return url_; }
	const std::string &uuid() const { return uuid_; }

	void SetClient( blink::WebServiceWorkerProviderClient *client ) override
	{
		client_ = client;
		/* a document loaded in the scope of an active worker is that worker's from the start */
		const Client *c = ClientOf(uuid_);
		if ( client_ != nullptr && c != nullptr && c->controller != nullptr ) {
			client_->SetController(c->controller->WebInfo(uuid_), false);
		}
	}

	/* The worker that controls the document now: its container told (claim, a new worker active) */
	void Controlled( Version *v )
	{
		if ( client_ != nullptr && v != nullptr ) client_->SetController(v->WebInfo(uuid_), true);
	}

	void Message( Version *from, blink::TransferableMessage m )
	{
		if ( client_ != nullptr ) client_->ReceiveMessage(from->WebInfo(uuid_), std::move(m));
	}

	void RegisterServiceWorker( const blink::WebURL &scope, const blink::WebURL &script, blink::mojom::ScriptType,
				    bm::ServiceWorkerUpdateViaCache, const blink::WebFetchClientSettingsObject &,
				    std::unique_ptr<WebServiceWorkerRegistrationCallbacks> cb ) override;

	void GetRegistration( const blink::WebURL &document,
			      std::unique_ptr<WebServiceWorkerGetRegistrationCallbacks> cb ) override
	{
		Registration *r = Match(blink::KURL(document).GetString().Utf8());
		if ( r == nullptr ) {
			cb->OnSuccess(Nothing());
			return;
		}
		cb->OnSuccess(r->WebInfo(uuid_));
	}

	void GetRegistrations( std::unique_ptr<WebServiceWorkerGetRegistrationsCallbacks> cb ) override
	{
		std::vector<blink::WebServiceWorkerRegistrationObjectInfo> out;
		scoped_refptr<const blink::SecurityOrigin> mine = blink::SecurityOrigin::CreateFromString(
			blink::String::FromUtf8(url_));
		for ( auto &kv : Registrations() ) {
			if ( blink::SecurityOrigin::Create(kv.second->scope())->IsSameOriginWith(mine.get()) ) {
				out.push_back(kv.second->WebInfo(uuid_));
			}
		}
		cb->OnSuccess(std::move(out));
	}

	void GetRegistrationForReady( GetRegistrationForReadyCallback cb ) override
	{
		Registration *r = Match(url_);
		if ( r != nullptr && r->active() != nullptr ) {
			std::move(cb).Run(r->WebInfo(uuid_));
			return;
		}
		ready_.push_back(std::move(cb));
	}

	/* A registration's worker active: the pages waiting for it told */
	void Activated( Registration *r )
	{
		if ( Match(url_) != r ) return;
		std::vector<GetRegistrationForReadyCallback> w;
		w.swap(ready_);
		for ( auto &c : w ) std::move(c).Run(r->WebInfo(uuid_));
	}

	bool ValidateScopeAndScriptURL( const blink::WebURL &scope, const blink::WebURL &script,
					blink::WebString *error ) override
	{
		blink::KURL s(scope), u(script);
		if ( !u.ProtocolIsInHttpFamily() || !s.ProtocolIsInHttpFamily() ) {
			*error = blink::WebString::FromUtf8("The URL protocol of the script and the scope must be http or https.");
			return false;
		}
		if ( !blink::SecurityOrigin::Create(s)->IsSameOriginWith(blink::SecurityOrigin::Create(u).get()) ) {
			*error = blink::WebString::FromUtf8("The origin of the scope does not match the script's.");
			return false;
		}
		return true;
	}

private:
	static blink::WebServiceWorkerRegistrationObjectInfo Nothing()
	{
		auto none = []() {
			return blink::WebServiceWorkerObjectInfo(bm::kInvalidServiceWorkerVersionId,
								 bm::ServiceWorkerState::kParsed, blink::WebURL(),
								 mojo::NullAssociatedRemote(), mojo::NullAssociatedReceiver());
		};
		return blink::WebServiceWorkerRegistrationObjectInfo(
			bm::kInvalidServiceWorkerRegistrationId, blink::WebURL(), bm::ServiceWorkerUpdateViaCache::kImports,
			mojo::NullAssociatedRemote(), mojo::NullAssociatedReceiver(), none(), none(), none());
	}

	std::string					uuid_, url_;
	blink::WebServiceWorkerProviderClient		*client_ = nullptr;
	std::vector<GetRegistrationForReadyCallback>	ready_;
};

/* A document as the workers see it among their clients */
bm::ServiceWorkerClientInfoPtr ClientInfo( const Client &d )
{
	auto c = bm::ServiceWorkerClientInfo::New();
	c->url = blink::KURL(blink::String::FromUtf8(d.url));
	c->frame_type = d.top ? bm::RequestContextFrameType::kTopLevel : bm::RequestContextFrameType::kNested;
	c->client_uuid = blink::String::FromUtf8(d.uuid);
	c->client_type = bm::ServiceWorkerClientType::kWindow;
	c->page_hidden = false;
	c->is_focused = d.top;
	c->last_focus_time = d.made;
	c->creation_time = d.made;
	return c;
}

/* The provider of a client's document, or null */
Provider *ProviderOf( const std::string &uuid )
{
	for ( Provider *p : Providers() ) {
		if ( p->uuid() == uuid ) return p;
	}
	return nullptr;
}

/* Whether two addresses are of the same origin */
bool SameOrigin( const std::string &a, const std::string &b )
{
	return blink::SecurityOrigin::CreateFromString(blink::String::FromUtf8(a))
		->IsSameOriginWith(blink::SecurityOrigin::CreateFromString(blink::String::FromUtf8(b)).get());
}

/* The clients in the order they were made */
std::vector<Client *> ClientsInOrder()
{
	std::vector<Client *> v;
	for ( auto &kv : Clients() ) v.push_back(&kv.second);
	std::sort(v.begin(), v.end(), []( const Client *a, const Client *b ) { return a->seq < b->seq; });
	return v;
}

/* ---------------------------------------------------------------- the workers' lives */

void Version::Start( std::function<void(bool)> evaluated )
{
	started_ = true;
	evaluated_ = std::move(evaluated);
	self_ = weak_.GetWeakPtr();
	worker_receiver_ = worker_.BindNewPipeAndPassReceiver();
	controller_receiver_ = controller_.BindNewPipeAndPassReceiver();

	mojo::PendingRemote<bm::WorkerContentSettingsProxy> settings;
	br_on_service_thread(base::BindOnce([]( mojo::PendingReceiver<bm::WorkerContentSettingsProxy> r ) {
		mojo::MakeSelfOwnedReceiver(std::make_unique<ContentSettings>(), std::move(r));
	}, settings.InitWithNewPipeAndPassReceiver()));

	auto data = std::make_unique<blink::WebEmbeddedWorkerStartData>(blink::WebFetchClientSettingsObject(
		blink::WebPolicyContainerPolicies(), blink::WebURL(), bm::InsecureRequestsPolicy::kDoNotUpgrade));
	data->script_url = blink::WebURL(script_);
	data->user_agent = blink::WebString::FromUtf8(br_user_agent());
	data->ua_metadata = br_user_agent_metadata();
	data->script_type = reg_->module() ? blink::mojom::ScriptType::kModule : blink::mojom::ScriptType::kClassic;
	data->service_worker_token = blink::ServiceWorkerToken();
	data->policy_container = NewPolicyContainer();

	printf("browser: service worker: %s starts\n", script_.GetString().Utf8().c_str());
	embedded_ = blink::WebEmbeddedWorker::Create(this);
	embedded_->StartWorkerContext(std::move(data), nullptr, std::move(settings),
				      blink::CrossVariantMojoRemote<blink::mojom::CacheStorageInterfaceBase>(),
				      br_worker_broker(blink::SecurityOrigin::Create(script_)->ToString().Utf8()),
				      blink::InterfaceRegistry::GetEmptyInterfaceRegistry(),
				      main_, blink::CrossVariantMojoReceiver<blink::mojom::ReportingObserverInterfaceBase>(),
				      blink::CrossVariantMojoReceiver<blink::mojom::ReportingObserverInterfaceBase>());

	/* the global scope's host, its registration and itself, before any event */
	mojo::PendingAssociatedRemote<bm::ServiceWorkerHost> host = host_.BindNewEndpointAndPassRemote();
	mojo::PendingAssociatedReceiver<bm::AssociatedInterfaceProvider> from_browser =
		no_interfaces_remote_.InitWithNewEndpointAndPassReceiver();
	mojo::PendingAssociatedRemote<bm::AssociatedInterfaceProvider> to_browser =
		no_interfaces_receiver_.BindNewEndpointAndPassRemote();
	scoped_refptr<const blink::SecurityOrigin> origin = blink::SecurityOrigin::Create(script_);
	worker_->InitializeGlobalScope(std::move(host), std::move(to_browser), std::move(from_browser),
				       reg_->MojoInfo(), MojoInfo(), bm::FetchHandlerExistence::UNKNOWN,
				       bm::AncestorFrameType::kNormalFrame,
				       blink::BlinkStorageKey::CreateFirstParty(origin->IsolatedCopy()));
}

void Registration::Install( const blink::KURL &script, std::function<void(bool, const std::string &)> done )
{
	if ( active_ && active_->script() == script ) {
		done(true, std::string());
		return;
	}
	installing_ = std::make_unique<Version>(this, script);
	Version *v = installing_.get();
	v->Start([this, v, done]( bool ok ) {
		if ( installing_.get() != v ) return;
		if ( !ok ) {
			printf("browser: service worker: %s did not run\n", v->script().GetString().Utf8().c_str());
			v->Terminate();
			retired_.push_back(installing_.release());
			done(false, "The script of the service worker could not be fetched or run.");
			return;
		}
		v->SetState(bm::ServiceWorkerState::kInstalling);
		Changed();
		for ( auto &o : objects_ ) {
			if ( o.is_bound() ) o->UpdateFound();
		}
		done(true, std::string());
		v->worker()->DispatchInstallEvent(base::BindOnce([]( Registration *reg, Version *v,
								     bm::ServiceWorkerEventStatus status, uint32_t ) {
			if ( reg->installing() != v ) return;
			if ( status != bm::ServiceWorkerEventStatus::COMPLETED ) {
				printf("browser: service worker: its install failed\n");
				v->SetState(bm::ServiceWorkerState::kRedundant);
				return;
			}
			reg->Installed();
		}, this, v));
	});
}

void Registration::Installed()
{
	installing_->SetState(bm::ServiceWorkerState::kInstalled);
	waiting_ = std::move(installing_);
	Changed();
	if ( !active_ || skip_ ) Activate();
}

void Registration::Activate()
{
	if ( !waiting_ ) return;
	skip_ = false;
	Version *old = active_.release();
	active_ = std::move(waiting_);
	Version *v = active_.get();
	if ( old != nullptr ) {
		/* the documents the worker before controlled are the new one's now */
		for ( Client *c : ClientsInOrder() ) {
			if ( c->controller != old ) continue;
			c->controller = v;
			if ( Provider *p = ProviderOf(c->uuid) ) p->Controlled(v);
		}
		old->SetState(bm::ServiceWorkerState::kRedundant);
		old->Terminate();
		Uncontrol(old);
		retired_.push_back(old);
	}
	v->SetState(bm::ServiceWorkerState::kActivating);
	Changed();
	v->worker()->DispatchActivateEvent(base::BindOnce([]( Registration *reg, Version *v,
							      bm::ServiceWorkerEventStatus ) {
		if ( reg->active() != v ) return;
		v->SetState(bm::ServiceWorkerState::kActivated);
		printf("browser: service worker: %s active for %s\n", v->script().GetString().Utf8().c_str(),
		       reg->scope().GetString().Utf8().c_str());
		SaveRegistrations();
		for ( Provider *p : std::vector<Provider *>(Providers()) ) p->Activated(reg);
	}, this, v));
}

void Registration::Unregister( UnregisterCallback cb )
{
	auto it = Registrations().find(scope_.GetString().Utf8());
	if ( it != Registrations().end() && it->second == this ) Registrations().erase(it);
	if ( active_ ) Uncontrol(active_.get());
	printf("browser: service worker: %s unregistered\n", scope_.GetString().Utf8().c_str());
	SaveRegistrations();
	br_background_forget(id_);
	std::move(cb).Run(bm::ServiceWorkerErrorType::kNone, blink::String());
}

void Version::GetClients( bm::ServiceWorkerClientQueryOptionsPtr options, GetClientsCallback cb )
{
	/*
	 * The documents it controls; with includeUncontrolled, every document
	 * of its origin. There are window clients only.
	 */
	blink::Vector<bm::ServiceWorkerClientInfoPtr> out;
	bool all = options && options->include_uncontrolled;
	bool windows = !options || options->client_type == bm::ServiceWorkerClientType::kWindow
		       || options->client_type == bm::ServiceWorkerClientType::kAll;
	std::string scope = reg_->scope().GetString().Utf8();
	for ( Client *c : ClientsInOrder() ) {
		if ( !windows ) break;
		if ( c->controller == this || ( all && SameOrigin(c->url, scope) ) ) out.push_back(ClientInfo(*c));
	}
	std::move(cb).Run(std::move(out));
}

void Version::GetClient( const blink::String &uuid, GetClientCallback cb )
{
	const Client *c = ClientOf(uuid.Utf8());
	std::move(cb).Run(c != nullptr && SameOrigin(c->url, script_.GetString().Utf8()) ? ClientInfo(*c) : nullptr);
}

void Version::PostMessageToClient( const blink::String &uuid, blink::BlinkTransferableMessage message )
{
	Provider *p = ProviderOf(uuid.Utf8());
	if ( p != nullptr ) p->Message(this, ToTransferable(std::move(message)));
}

void Version::FocusClient( const blink::String &uuid, FocusClientCallback cb )
{
	const Client *c = ClientOf(uuid.Utf8());
	if ( c == nullptr ) {
		std::move(cb).Run(bm::FocusResult::NewErrorCode(bm::FocusError::CLIENT_NOT_FOUND));
		return;
	}
	std::move(cb).Run(bm::FocusResult::NewClient(ClientInfo(*c)));
}

void Version::SkipWaiting( SkipWaitingCallback cb )
{
	reg_->SkipWaiting(this);
	std::move(cb).Run(true);
}

void Version::ClaimClients( ClaimClientsCallback cb )
{
	if ( reg_->active() != this ) {
		std::move(cb).Run(bm::ServiceWorkerErrorType::kState, "Only the active worker can claim clients.");
		return;
	}
	for ( Client *c : ClientsInOrder() ) {
		if ( Match(c->url) != reg_ || c->controller == this ) continue;
		c->controller = this;
		printf("browser: service worker: it claims %s\n", c->url.c_str());
		if ( Provider *p = ProviderOf(c->uuid) ) p->Controlled(this);
	}
	std::move(cb).Run(bm::ServiceWorkerErrorType::kNone, blink::String());
}

void Version::PostMessageToServiceWorker( blink::BlinkTransferableMessage message )
{
	Wake();
	if ( !worker_ ) return;
	auto e = bm::ExtendableMessageEvent::New();
	e->message = std::move(message);
	const Client *c = ClientOf(hosts_.current_context());
	if ( c == nullptr ) return;
	e->source_origin = blink::SecurityOrigin::CreateFromString(blink::String::FromUtf8(c->url));
	e->source_info_for_client = ClientInfo(*c);
	worker_->DispatchExtendableMessageEvent(std::move(e), base::BindOnce([]( bm::ServiceWorkerEventStatus ) {}));
}

void Provider::RegisterServiceWorker( const blink::WebURL &scope, const blink::WebURL &script, blink::mojom::ScriptType,
				      bm::ServiceWorkerUpdateViaCache, const blink::WebFetchClientSettingsObject &,
				      std::unique_ptr<WebServiceWorkerRegistrationCallbacks> cb )
{
	blink::KURL s(scope), u(script);
	Registration *&slot = Registrations()[s.GetString().Utf8()];
	if ( slot == nullptr ) slot = new Registration(s);
	Registration *reg = slot;
	std::shared_ptr<WebServiceWorkerRegistrationCallbacks> c(cb.release());
	printf("browser: service worker: %s registered for %s\n", u.GetString().Utf8().c_str(),
	       s.GetString().Utf8().c_str());
	reg->Install(u, [reg, c, client = uuid_]( bool ok, const std::string &why ) {
		if ( ok ) {
			c->OnSuccess(reg->WebInfo(client));
		} else {
			c->OnError(blink::WebServiceWorkerError(bm::ServiceWorkerErrorType::kScriptEvaluateFailed,
								blink::WebString::FromUtf8(why)));
		}
	});
}

/* ---------------------------------------------------------------- the registrations kept */

/*
 * The registrations with an active worker are kept between runs, an entry
 * of the store "sw" (port/net/br_store.cc) for each scope, its id and its
 * worker's script as JSON. Browsers of windows opened apart are processes of
 * their own and each runs its own workers: a registration another browser made
 * is taken in, one it gave up is unregistered here, and one it updated takes
 * the place of this one's unless this one's worker runs, which then stays
 * until this browser ends. The entries are made on the page's thread when
 * they change; the store's snapshot, on the application's, takes a copy.
 */
int		g_sw_store = -1;
std::mutex	g_sw_lock;
std::map<std::string, std::string> g_sw_entries;
/* A scope whose entry another browser wrote, and this browser's script when it did: what is written while that stays */
struct SwTaken {
	std::string	script, bytes;
};
std::map<std::string, SwTaken> g_sw_taken;

void SnapshotRegistrations( void *, br_store_delta *all )
{
	std::lock_guard<std::mutex> hold(g_sw_lock);
	for ( const auto &kv : g_sw_entries ) {
		br_store_delta_put(all, kv.first.data(), (int)kv.first.size(), kv.second.data(), (int)kv.second.size());
	}
}

void SaveRegistrations()
{
	std::map<std::string, std::string> entries;
	for ( auto &kv : Registrations() ) {
		Registration *r = kv.second;
		if ( r == nullptr || r->active() == nullptr
		     || r->active()->state() != bm::ServiceWorkerState::kActivated ) {
			continue;
		}
		std::string script = r->active()->script().GetString().Utf8();
		auto t = g_sw_taken.find(kv.first);
		if ( t != g_sw_taken.end() && t->second.script == script ) {
			entries[kv.first] = t->second.bytes;
			continue;
		}
		base::DictValue e;
		e.Set("id", base::NumberToString(r->id()));
		e.Set("script", script);
		std::string json;
		base::JSONWriter::Write(e, &json);
		entries[kv.first] = std::move(json);
	}
	{
		std::lock_guard<std::mutex> hold(g_sw_lock);
		g_sw_entries = std::move(entries);
	}
	if ( g_sw_store >= 0 ) br_store_dirty(g_sw_store);
}

/* A registration known from what is kept; a number another browser's taken here already is not used twice */
void AddKept( const std::string &scope, const std::string &id, const std::string &script )
{
	int64_t n = 0;
	if ( !base::StringToInt64(id, &n) || n <= 0 ) return;
	blink::KURL s(blink::String::FromUtf8(scope)), u(blink::String::FromUtf8(script));
	if ( !s.IsValid() || !u.IsValid() || Registrations().count(scope) != 0 ) return;
	for ( const auto &kv : Registrations() ) {
		if ( kv.second != nullptr && kv.second->id() == n ) n = g_next_id++;
	}
	Registrations()[scope] = new Registration(s, n, u);
	if ( n >= g_next_id ) g_next_id = n + 1;
}

bool ParseKept( const std::string &bytes, std::string *id, std::string *script )
{
	std::optional<base::Value> v = base::JSONReader::Read(bytes, base::JSON_PARSE_RFC);
	if ( !v || !v->is_dict() ) return false;
	const std::string *i = v->GetDict().FindString("id"), *c = v->GetDict().FindString("script");
	if ( i == nullptr || c == nullptr ) return false;
	*id = *i;
	*script = *c;
	return true;
}

/* A registration another browser changed, on the page's thread ('has' false: it gave it up) */
void TakeOnPage( const std::string &scope, bool has, const std::string &bytes )
{
	auto it = Registrations().find(scope);
	Registration *r = it != Registrations().end() ? it->second : nullptr;
	if ( !has ) {
		g_sw_taken.erase(scope);
		if ( r != nullptr ) r->Unregister(base::DoNothing());
		return;
	}
	std::string id, script;
	if ( !ParseKept(bytes, &id, &script) ) return;
	if ( r != nullptr && r->active() != nullptr ) {
		std::string mine = r->active()->script().GetString().Utf8();
		if ( mine == script || r->Running() != nullptr ) {
			g_sw_taken[scope] = SwTaken{ mine, bytes };
			return;
		}
		r->Unregister(base::DoNothing());
	}
	g_sw_taken.erase(scope);
	AddKept(scope, id, script);
	printf("browser: service worker: %s taken in from another browser\n", scope.c_str());
}

void TakeRegistration( void *, const char *key, int klen, const unsigned char *val, int vlen )
{
	std::string scope(key, (size_t)klen);
	std::string bytes(val != nullptr ? (const char *)val : "", val != nullptr ? (size_t)vlen : 0);
	{
		std::lock_guard<std::mutex> hold(g_sw_lock);
		if ( val != nullptr ) g_sw_entries[scope] = bytes;
		else g_sw_entries.erase(scope);
	}
	br_on_page_thread(base::BindOnce(&TakeOnPage, scope, val != nullptr, bytes));
}

/* The registration of each scope a clearing takes unregistered, its push subscription with it (on the page's thread) */
void ClearRegistrations( const BrClear &c )
{
	std::vector<Registration *> gone;
	for ( const auto &kv : Registrations() ) {
		if ( br_clear_origin_hit(c, url::Origin::Create(GURL(kv.first)).Serialize()) ) gone.push_back(kv.second);
	}
	for ( Registration *r : gone ) {
		(void)br_push_unsubscribe(r->scope_string());
		r->Unregister(base::DoNothing());
	}
}

void RegistrationSites( std::set<std::string> *out )
{
	for ( const auto &kv : Registrations() ) out->insert(br_site_of(kv.first));
}

/* An entry read back: a registration, or an older browser's list of them */
void RestoreEntry( void *, const char *key, int klen, const unsigned char *val, int vlen )
{
	std::string bytes((const char *)val, (size_t)vlen);
	if ( klen != 0 ) {
		std::string scope(key, (size_t)klen), id, script;
		if ( ParseKept(bytes, &id, &script) ) {
			AddKept(scope, id, script);
			std::lock_guard<std::mutex> hold(g_sw_lock);
			g_sw_entries[scope] = bytes;
		}
		return;
	}
	std::optional<base::Value> v = base::JSONReader::Read(bytes, base::JSON_PARSE_RFC);
	if ( !v || !v->is_list() ) return;
	for ( const base::Value &e : v->GetList() ) {
		if ( !e.is_dict() ) continue;
		const std::string *scope = e.GetDict().FindString("scope");
		const std::string *id = e.GetDict().FindString("id");
		const std::string *script = e.GetDict().FindString("script");
		if ( scope == nullptr || id == nullptr || script == nullptr ) continue;
		AddKept(*scope, *id, *script);
		base::DictValue one;
		one.Set("id", *id);
		one.Set("script", *script);
		std::string json;
		base::JSONWriter::Write(one, &json);
		std::lock_guard<std::mutex> hold(g_sw_lock);
		g_sw_entries[*scope] = std::move(json);
	}
	if ( g_sw_store >= 0 ) br_store_dirty(g_sw_store);
}

/* The registrations of earlier runs read back (on the page's thread, once) */
void RestoreRegistrations()
{
	static bool done;
	if ( done ) return;
	done = true;
	g_sw_store = br_store_register_shared("sw", &SnapshotRegistrations, &TakeRegistration, nullptr, 500);
	(void)br_store_load_shared(g_sw_store, &RestoreEntry, nullptr);
	printf("browser: service worker: %zu registrations read back\n", Registrations().size());
}

/* ---------------------------------------------------------------- fetch events */

/* A request of the page made the request of a fetch event */
bm::FetchAPIRequestPtr Request( const network::ResourceRequest &r, bool main )
{
	auto q = bm::FetchAPIRequest::New();
	q->url = blink::KURL(blink::String::FromUtf8(r.url.spec()));
	q->method = blink::String::FromUtf8(r.method.empty() ? "GET" : r.method);
	for ( const auto &h : r.headers.GetHeaderVector() ) {
		q->headers.Set(blink::String::FromUtf8(h.key), blink::String::FromUtf8(h.value));
	}
	q->mode = main ? network::mojom::RequestMode::kNavigate : r.mode;
	q->destination = main ? network::mojom::RequestDestination::kDocument : r.destination;
	q->credentials_mode = main ? network::mojom::CredentialsMode::kInclude : r.credentials_mode;
	q->redirect_mode = main ? network::mojom::RedirectMode::kManual : r.redirect_mode;
	q->is_main_resource_load = main;
	q->frame_type = main ? bm::RequestContextFrameType::kTopLevel : bm::RequestContextFrameType::kNone;
	if ( r.referrer.is_valid() ) {
		q->referrer = bm::Referrer::New(blink::KURL(blink::String::FromUtf8(r.referrer.spec())),
						network::mojom::ReferrerPolicy::kDefault);
	}
	return q;
}

/* What the worker answered: a response (its status, address, headers, type, body) or none (fallback) */
using Answered = base::OnceCallback<void(bool fallback, int status, std::string url, std::string headers,
					 std::string ctype, std::string body)>;

/*
 * The receiver of a fetch event's answer, on the page's thread; it goes
 * once it has told it. A fetch event the worker has not answered in ten
 * seconds is said on the console (Unanswered), and so is an answer that
 * came after more than two: a page whose requests wait on its worker is
 * followed from these lines.
 */
class FetchAnswer final : public bm::ServiceWorkerFetchResponseCallback, public mojo::DataPipeDrainer::Client {
public:
	static mojo::PendingRemote<bm::ServiceWorkerFetchResponseCallback> Make( Answered done, const std::string &url )
	{
		auto *a = new FetchAnswer(std::move(done), url);
		mojo::PendingRemote<bm::ServiceWorkerFetchResponseCallback> remote = a->receiver_.BindNewPipeAndPassRemote();
		a->receiver_.set_disconnect_handler(base::BindOnce(&FetchAnswer::Gone, base::Unretained(a)));
		return remote;
	}

	/* The fetch events waiting ten seconds or more for their answer, said at most every ten seconds */
	static void Unanswered()
	{
		static base::TimeTicks last;
		const base::TimeTicks now = base::TimeTicks::Now();
		if ( now - last < base::Seconds(10) ) return;
		last = now;
		for ( const FetchAnswer *a : Live() ) {
			if ( now - a->asked_ < base::Seconds(10) ) continue;
			printf("browser: service worker: %ds without an answer (%s, %zu waiting) for %.160s\n",
			       (int)( now - a->asked_ ).InSeconds(), a->waiting_ ? "its body being read" : "the event",
			       Live().size(), a->asked_url_.c_str());
		}
	}

	void OnResponse( bm::FetchAPIResponsePtr response, bm::ServiceWorkerFetchEventTimingPtr,
			 bm::ServiceWorkerFetchHandlerErrorsPtr ) override
	{
		Take(*response);
		if ( !response->blob ) {
			Tell(false, std::string());
			return;
		}
		scoped_refptr<base::SingleThreadTaskRunner> main = base::SingleThreadTaskRunner::GetCurrentDefault();
		br_blob_read(response->blob->Uuid().Utf8(), false, base::BindOnce(
			[]( scoped_refptr<base::SingleThreadTaskRunner> main, FetchAnswer *a, bool, std::string bytes,
			    std::string ) {
				main->PostTask(FROM_HERE, base::BindOnce(&FetchAnswer::Tell, base::Unretained(a), false,
									 std::move(bytes)));
			}, main, base::Unretained(this)));
		waiting_ = true;
	}

	void OnResponseStream( bm::FetchAPIResponsePtr response, bm::ServiceWorkerStreamHandlePtr stream,
			       bm::ServiceWorkerFetchEventTimingPtr, bm::ServiceWorkerFetchHandlerErrorsPtr ) override
	{
		Take(*response);
		waiting_ = true;
		drainer_ = std::make_unique<mojo::DataPipeDrainer>(this, std::move(stream->stream));
	}

	void OnFallback( std::optional<network::DataElementChunkedDataPipe>, bm::ServiceWorkerFetchEventTimingPtr,
			 bm::ServiceWorkerFetchHandlerErrorsPtr ) override
	{
		Tell(true, std::string());
	}

	void OnDataAvailable( base::span<const uint8_t> data ) override { body_.append(data.begin(), data.end()); }
	void OnDataComplete() override { Tell(false, std::move(body_)); }

private:
	FetchAnswer( Answered done, const std::string &url )
		: done_(std::move(done)), asked_url_(url), asked_(base::TimeTicks::Now())
	{
		Live().push_back(this);
	}
	~FetchAnswer() override { std::erase(Live(), this); }

	static std::vector<FetchAnswer *> &Live()
	{
		static std::vector<FetchAnswer *> v;
		return v;
	}

	void Take( const bm::FetchAPIResponse &r )
	{
		status_ = r.status_code;
		url_ = r.url_list.empty() ? std::string() : r.url_list.back().GetString().Utf8();
		for ( const auto &kv : r.headers ) {
			headers_ += kv.key.Utf8() + ": " + kv.value.Utf8() + "\r\n";
			if ( blink::EqualIgnoringAsciiCase(kv.key, "content-type") ) ctype_ = kv.value.Utf8();
		}
		if ( ctype_.empty() && !r.mime_type.empty() ) ctype_ = r.mime_type.Utf8();
	}

	void Tell( bool fallback, std::string body )
	{
		const base::TimeDelta took = base::TimeTicks::Now() - asked_;
		if ( took > base::Seconds(2) ) {
			printf("browser: service worker: answered after %d ms (%s) %.160s\n", (int)took.InMilliseconds(),
			       fallback ? "to the network" : "its response", asked_url_.c_str());
		}
		if ( done_ ) std::move(done_).Run(fallback, status_, url_, headers_, ctype_, std::move(body));
		delete this;
	}

	/* the worker went without answering: the network is asked instead */
	void Gone()
	{
		if ( waiting_ ) return;
		Tell(true, std::string());
	}

	Answered					done_;
	mojo::Receiver<bm::ServiceWorkerFetchResponseCallback> receiver_{this};
	std::unique_ptr<mojo::DataPipeDrainer>		drainer_;
	int						status_ = 0;
	std::string					url_, headers_, ctype_, body_;
	bool						waiting_ = false;
	std::string					asked_url_;	/* what the event asked for */
	base::TimeTicks					asked_;
};

}  // namespace

/* The provider of a window's service worker container (br_blink.cc's frame client) */
/* The page's thread, where the registrations are: known from the first page that asks */
scoped_refptr<base::SingleThreadTaskRunner> &PageRunner()
{
	static auto *r = new scoped_refptr<base::SingleThreadTaskRunner>();
	return *r;
}

std::unique_ptr<blink::WebServiceWorkerProvider> br_sw_provider( const std::string &client )
{
	if ( !PageRunner() ) PageRunner() = base::SingleThreadTaskRunner::GetCurrentDefault();
	RestoreRegistrations();
	return std::make_unique<Provider>(client);
}

/*
 * At the start, on the page's thread: the registrations of earlier runs
 * read back, so that the events for them (a push) find them before any
 * page is shown
 */
void br_sw_clear( const BrClear &c )
{
	ClearRegistrations(c);
}

void br_sw_sites( std::set<std::string> *out )
{
	RegistrationSites(out);
}

void br_sw_init()
{
	if ( !PageRunner() ) PageRunner() = base::SingleThreadTaskRunner::GetCurrentDefault();
	RestoreRegistrations();
}

/*
 * An event for the active worker of a registration, from any thread:
 * 'send' is run on the page's thread with the worker's interface, or with
 * null when the registration has no active worker (or there is none)
 */
void br_sw_dispatch( int64_t registration_id,
		     base::OnceCallback<void( blink::mojom::blink::ServiceWorker * )> send )
{
	auto run = base::BindOnce([]( int64_t id, base::OnceCallback<void( bm::ServiceWorker * )> send ) {
		for ( auto &kv : Registrations() ) {
			Registration *reg = kv.second;
			if ( reg != nullptr && reg->id() == id && reg->Running() != nullptr
			     && reg->active()->worker() != nullptr ) {
				std::move(send).Run(reg->active()->worker());
				return;
			}
		}
		std::move(send).Run(nullptr);
	}, registration_id, std::move(send));
	if ( !PageRunner() ) {
		std::move(run).Run();
		return;
	}
	PageRunner()->PostTask(FROM_HERE, std::move(run));
}

/* The scope of a registration, "" when there is none (on the page's thread) */
std::string br_sw_scope( int64_t registration_id )
{
	for ( auto &kv : Registrations() ) {
		if ( kv.second != nullptr && kv.second->id() == registration_id ) return kv.first;
	}
	return std::string();
}

/* The scope of a registration given to 'done' on the page's thread, "" when there is none */
void br_sw_scope_of( int64_t registration_id, base::OnceCallback<void( std::string )> done )
{
	auto run = base::BindOnce([]( int64_t id, base::OnceCallback<void( std::string )> done ) {
		std::move(done).Run(br_sw_scope(id));
	}, registration_id, std::move(done));
	if ( !PageRunner() ) {
		std::move(run).Run();
		return;
	}
	PageRunner()->PostTask(FROM_HERE, std::move(run));
}

/* As br_sw_dispatch, for the registration of a scope */
void br_sw_dispatch_scope( const std::string &scope,
			   base::OnceCallback<void( blink::mojom::blink::ServiceWorker * )> send )
{
	auto run = base::BindOnce([]( std::string scope, base::OnceCallback<void( bm::ServiceWorker * )> send ) {
		auto it = Registrations().find(scope);
		Registration *reg = ( it != Registrations().end() ) ? it->second : nullptr;
		if ( reg != nullptr && reg->Running() != nullptr && reg->active()->worker() != nullptr ) {
			std::move(send).Run(reg->active()->worker());
		} else {
			std::move(send).Run(nullptr);
		}
	}, scope, std::move(send));
	if ( !PageRunner() ) {
		std::move(run).Run();
		return;
	}
	PageRunner()->PostTask(FROM_HERE, std::move(run));
}

/*
 * A document made in a frame, at its address: a client of the workers,
 * whose UUID is answered ('uuid' when given: the one a navigation's fetch
 * event named as its resulting client). It is controlled by the active
 * worker of the scope it is in, if there is one; an iframe's about:blank
 * or about:srcdoc, by what controls its parent (a client by its UUID).
 */
std::string br_sw_document( const std::string &url, bool top, const std::string &parent, const std::string &uuid )
{
	static uint64_t seq;
	RestoreRegistrations();
	Client c;
	c.url = url;
	c.uuid = uuid.empty() ? base::UnguessableToken::Create().ToString() : uuid;
	c.top = top;
	c.seq = ++seq;
	c.made = base::TimeTicks::Now();
	if ( url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0 ) {
		Registration *r = Match(url);
		if ( r != nullptr && r->active() != nullptr
		     && r->active()->state() == bm::ServiceWorkerState::kActivated ) {
			c.controller = r->Running();
		}
	} else if ( !top && ( url == "about:blank" || url == "about:srcdoc" ) ) {
		const Client *p = ClientOf(parent);
		if ( p != nullptr ) c.controller = p->controller;
	}
	if ( c.controller != nullptr ) {
		printf("browser: service worker: %s%s is controlled\n", top ? "" : "the iframe's ", url.c_str());
	}
	std::string id = c.uuid;
	Clients()[id] = std::move(c);
	return id;
}

/* A document gone (its frame has another one, or went): it is no longer a client */
void br_sw_forget( const std::string &uuid )
{
	if ( !uuid.empty() ) Clients().erase(uuid);
}

/* The worker that controls a client and takes its fetch events, or null */
static Version *ControllerOf( const std::string &uuid )
{
	const Client *c = ClientOf(uuid);
	if ( c == nullptr || c->controller == nullptr || c->controller->controller() == nullptr ) return nullptr;
	return c->controller;
}

/* Whether a load of a document (a client by its UUID) goes to the worker that controls it first */
bool br_sw_controls( const std::string &client, const std::string &url )
{
	return ControllerOf(client) != nullptr
	       && ( url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0 );
}

/* A load of a document as a fetch event of the worker that controls it; its answer told to 'done' */
void br_sw_subresource( const std::string &client, const network::ResourceRequest &request, Answered done )
{
	Version *v = ControllerOf(client);
	if ( v == nullptr ) {
		std::move(done).Run(true, 0, std::string(), std::string(), std::string(), std::string());
		return;
	}
	auto params = bm::DispatchFetchEventParams::New();
	params->request = Request(request, false);
	params->client_id = blink::String::FromUtf8(client);
	params->resulting_client_id = blink::g_empty_string;	/* strings the message may not leave out */
	FetchAnswer::Unanswered();
	printf("browser: service worker: asked for %s\n", request.url.spec().c_str());
	v->controller()->DispatchFetchEventForSubresource(
		std::move(params), FetchAnswer::Make(std::move(done), request.url.spec()),
		base::BindOnce([]( std::string url, bm::ServiceWorkerEventStatus status ) {
			if ( status != bm::ServiceWorkerEventStatus::COMPLETED ) {
				printf("browser: service worker: the fetch event ended %d for %.160s\n", (int)status,
				       url.c_str());
			}
		}, request.url.spec()));
}

/*
 * An iframe going to an address: asked of the active worker of its scope
 * first, as a fetch event for a nested document whose resulting client
 * is 'uuid' (its destination "frame" for a <frame>, else "iframe"). False when there is no such worker; else true, and the
 * answer (or that the network is to be asked: fallback) is told to
 * 'done' on the page's thread.
 */
bool br_sw_navigate_frame( const std::string &url, const std::string &method, const std::string &referrer,
			   const std::string &uuid, bool frame_element, Answered done )
{
	RestoreRegistrations();
	Registration *reg = Match(url);
	if ( reg == nullptr || reg->Running() == nullptr || reg->active()->worker() == nullptr
	     || reg->active()->state() != bm::ServiceWorkerState::kActivated ) {
		return false;
	}
	network::ResourceRequest req;
	req.url = GURL(url);
	req.method = method.empty() ? "GET" : method;
	if ( !referrer.empty() ) req.referrer = GURL(referrer);
	auto params = bm::DispatchFetchEventParams::New();
	params->request = Request(req, true);
	params->request->destination = frame_element ? network::mojom::RequestDestination::kFrame
						     : network::mojom::RequestDestination::kIframe;
	params->request->frame_type = bm::RequestContextFrameType::kNested;
	params->client_id = blink::g_empty_string;
	params->resulting_client_id = blink::String::FromUtf8(uuid);
	printf("browser: service worker: %s asked for the iframe's %s\n",
	       reg->active()->script().GetString().Utf8().c_str(), url.c_str());
	reg->active()->worker()->DispatchFetchEventForMainResource(
		std::move(params), FetchAnswer::Make(std::move(done), url),
		base::BindOnce([]( bm::ServiceWorkerEventStatus ) {}));
	return true;
}

/*
 * Going to an address: asked of the active worker of its scope first, as
 * a fetch event, waited for (30 seconds at most). True with the answer
 * in 'r'; false when there is no such worker or it left it to the
 * network.
 */
bool br_sw_navigate( const std::string &url, const std::string &method, BrResponse *r )
{
	RestoreRegistrations();
	Registration *reg = Match(url);
	if ( reg == nullptr || reg->Running() == nullptr || reg->active()->worker() == nullptr
	     || reg->active()->state() != bm::ServiceWorkerState::kActivated ) {
		return false;
	}
	struct Wait {
		bool		answered = false, fallback = true;
		int		status = 0;
		std::string	url, headers, ctype, body;
	} w;
	base::RunLoop loop(base::RunLoop::Type::kNestableTasksAllowed);
	network::ResourceRequest req;
	req.url = GURL(url);
	req.method = method.empty() ? "GET" : method;
	auto params = bm::DispatchFetchEventParams::New();
	params->request = Request(req, true);
	params->client_id = blink::g_empty_string;
	params->resulting_client_id = blink::String::FromUtf8(base::UnguessableToken::Create().ToString());
	printf("browser: service worker: %s asked for %s\n", reg->active()->script().GetString().Utf8().c_str(),
	       url.c_str());
	reg->active()->worker()->DispatchFetchEventForMainResource(
		std::move(params),
		FetchAnswer::Make(base::BindOnce([]( Wait *w, base::OnceClosure quit, bool fallback, int status,
						     std::string u, std::string headers, std::string ctype,
						     std::string body ) {
			w->answered = true;
			w->fallback = fallback;
			w->status = status;
			w->url = std::move(u);
			w->headers = std::move(headers);
			w->ctype = std::move(ctype);
			w->body = std::move(body);
			std::move(quit).Run();
		}, &w, loop.QuitClosure()), url),
		base::BindOnce([]( bm::ServiceWorkerEventStatus ) {}));
	base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(FROM_HERE, loop.QuitClosure(),
									    base::Seconds(30));
	loop.Run();
	if ( !w.answered || w.fallback ) {
		printf("browser: service worker: %s left to the network\n", url.c_str());
		return false;
	}
	r->status = w.status;
	r->url = w.url.empty() ? url : w.url;
	r->type = w.ctype;
	size_t cs = w.ctype.find("charset=");
	if ( cs != std::string::npos ) r->charset = w.ctype.substr(cs + 8, w.ctype.find(';', cs) - cs - 8);
	r->body = std::move(w.body);
	printf("browser: service worker: answered %s, %d, %zu bytes\n", url.c_str(), r->status, r->body.size());
	return true;
}

/* An extension's background worker (port/ext/br_ext_frames.cc): no page asks for it, the browser does at its start */
void br_sw_start_extension( const std::string &scope, const std::string &script, bool module )
{
	blink::KURL s(blink::String::FromUtf8(scope)), u(blink::String::FromUtf8(script));
	Registration *&slot = Registrations()[s.GetString().Utf8()];
	if ( slot == nullptr ) slot = new Registration(s);
	slot->set_module(module);
	slot->Install(u, [script]( bool ok, const std::string &why ) {
		printf("browser: extension: worker %s: %s\n", script.c_str(), ok ? "running" : why.c_str());
	});
	/* kept from the last run with the same script, it is not started by that: it has its messages to take */
	(void)slot->Running();
}
