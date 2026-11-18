/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	shared_worker_tessronos.cc
 *	ブラウザ: shared workers (new SharedWorker), whose service a browser
 *	keeps, kept in this process (design 14.4.4).
 *
 *	A page that makes a SharedWorker asks the frame's broker for a
 *	SharedWorkerConnector and connects to the worker of an address and a
 *	name with a message port. The connector here keeps the workers
 *	running, one for each address, name and origin of the pages: the
 *	first connection starts one in this process (WebSharedWorker, which
 *	runs it on a thread of its own and fetches its script itself, through
 *	the fetch context dedicated workers have, worker_tessronos.cc), and
 *	every connection is handed to it as a connect event with its port.
 *	What the worker tells its host (connected, its script not loaded, an
 *	exception) is told to the pages connected. A worker whose pages have
 *	all gone is ended.
 *
 *	The connector and the workers are on the page's thread, where Blink
 *	starts them; the worker's content settings (IndexedDB, Cache
 *	Storage, Web Locks: allowed) are answered on the services' thread,
 *	since the worker asks them while it waits.
 */

#include <stdio.h>

#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/task/single_thread_task_runner.h"
#include "base/unguessable_token.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/pending_associated_receiver.h"
#include "mojo/public/cpp/bindings/pending_associated_remote.h"
#include "mojo/public/cpp/bindings/self_owned_associated_receiver.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "services/metrics/public/cpp/ukm_source_id.h"
#include "third_party/blink/public/common/loader/worker_main_script_load_parameters.h"
#include "third_party/blink/public/common/messaging/message_port_descriptor.h"
#include "third_party/blink/public/common/tokens/tokens.h"
#include "third_party/blink/public/common/user_agent/user_agent_metadata.h"
#include "third_party/blink/public/mojom/blob/blob_url_store.mojom-blink.h"
#include "third_party/blink/public/mojom/browser_interface_broker.mojom-blink.h"
#include "third_party/blink/public/mojom/frame/policy_container.mojom-blink.h"
#include "third_party/blink/public/mojom/worker/shared_worker_client.mojom-blink.h"
#include "third_party/blink/public/mojom/worker/shared_worker_connector.mojom-blink.h"
#include "third_party/blink/public/mojom/worker/shared_worker_exception_details.mojom-blink.h"
#include "third_party/blink/public/mojom/worker/shared_worker_host.mojom-blink.h"
#include "third_party/blink/public/mojom/worker/shared_worker_info.mojom-blink.h"
#include "third_party/blink/public/mojom/worker/worker_content_settings_proxy.mojom-blink.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"
#include "third_party/blink/public/platform/web_content_security_policy_struct.h"
#include "third_party/blink/public/platform/web_fetch_client_settings_object.h"
#include "third_party/blink/public/platform/web_policy_container.h"
#include "third_party/blink/public/platform/web_security_origin.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/platform/web_url.h"
#include "third_party/blink/public/platform/web_worker_fetch_context.h"
#include "third_party/blink/public/web/web_shared_worker.h"
#include "third_party/blink/public/web/web_shared_worker_client.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

/* A broker for a worker (blob_tessronos.cc), the fetch context of a worker (worker_tessronos.cc) */
mojo::PendingRemote<blink::mojom::blink::BrowserInterfaceBroker> br_worker_broker( const std::string &origin );
scoped_refptr<blink::WebWorkerFetchContext> br_worker_fetch_context();

/* The services' thread (blob_tessronos.cc), the page's origin (br_blink.cc), the user agent */
void br_on_service_thread( base::OnceClosure task );
std::string br_page_origin();
const char *br_user_agent();
blink::UserAgentMetadata br_user_agent_metadata();				/* br_blink.cc */

namespace {

namespace bm = blink::mojom::blink;

/* What a shared worker may use: all of it, as the page it runs for may */
class ContentSettings final : public bm::WorkerContentSettingsProxy {
public:
	void AllowIndexedDB( AllowIndexedDBCallback cb ) override { std::move(cb).Run(true); }
	void AllowCacheStorage( AllowCacheStorageCallback cb ) override { std::move(cb).Run(true); }
	void AllowWebLocks( AllowWebLocksCallback cb ) override { std::move(cb).Run(true); }
	void RequestFileSystemAccessSync( RequestFileSystemAccessSyncCallback cb ) override { std::move(cb).Run(true); }
};

/*
 * The host of the worker's policy container: what the worker's
 * policies become (its referrer policy, its CSP) is kept by the worker
 * itself, and the host is not told
 */
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

class SharedWorker;

using Key = std::tuple<std::string, std::string, std::string>;	/* address, name, origin */

std::map<Key, SharedWorker *> &Workers()
{
	static auto *m = new std::map<Key, SharedWorker *>();
	return *m;
}

/* One running worker and the pages connected to it */
class SharedWorker final : public blink::WebSharedWorkerClient, public bm::SharedWorkerHost {
public:
	explicit SharedWorker( const Key &key ) : key_(key) {}

	void Start( const bm::SharedWorkerInfo &info )
	{
		mojo::PendingRemote<bm::SharedWorkerHost> host;
		host_.Bind(host.InitWithNewPipeAndPassReceiver());
		mojo::PendingRemote<bm::WorkerContentSettingsProxy> settings;
		br_on_service_thread(base::BindOnce([]( mojo::PendingReceiver<bm::WorkerContentSettingsProxy> r ) {
			mojo::MakeSelfOwnedReceiver(std::make_unique<ContentSettings>(), std::move(r));
		}, settings.InitWithNewPipeAndPassReceiver()));

		scoped_refptr<const blink::SecurityOrigin> from =
			blink::SecurityOrigin::CreateFromString(blink::String::FromUtf8(std::get<2>(key_)));
		scoped_refptr<const blink::SecurityOrigin> script = blink::SecurityOrigin::Create(info.url);
		blink::WebURL referrer;
		if ( info.outside_fetch_client_settings_object ) {
			referrer = blink::WebURL(info.outside_fetch_client_settings_object->outgoing_referrer);
		}
		printf("browser: shared worker: %s\n", info.url.GetString().Utf8().c_str());
		worker_ = blink::WebSharedWorker::CreateAndStart(
			blink::SharedWorkerToken(), blink::WebURL(info.url), info.options->type,
			info.options->credentials, blink::WebString(info.options->name),
			blink::WebSecurityOrigin(from), blink::WebSecurityOrigin(script),
			from->IsPotentiallyTrustworthy(), blink::WebString::FromUtf8(br_user_agent()),
			br_user_agent_metadata(), std::vector<blink::WebContentSecurityPolicy>(),
			blink::WebFetchClientSettingsObject(blink::WebPolicyContainerPolicies(), referrer,
							    blink::mojom::InsecureRequestsPolicy::kDoNotUpgrade),
			/* no DevTools: an empty token, so that no agent is made for it */
			base::UnguessableToken(), std::move(settings), br_worker_broker(script->ToString().Utf8()),
			/*pause_worker_context_on_start=*/false, /*worker_main_script_load_params=*/nullptr,
			NewPolicyContainer(), br_worker_fetch_context(), std::move(host), this,
			ukm::kInvalidSourceId, /*require_cross_site_request_for_cookies=*/false,
			blink::CrossVariantMojoReceiver<blink::mojom::ReportingObserverInterfaceBase>(),
			blink::CrossVariantMojoReceiver<blink::mojom::ReportingObserverInterfaceBase>(),
			/*is_cross_origin_isolated=*/false);
	}

	/* A page connected: told that the worker is there, its port given to the worker */
	void Connect( mojo::PendingRemote<bm::SharedWorkerClient> client, bm::SharedWorkerCreationContextType type,
		      blink::MessagePortDescriptor port )
	{
		int id = ++next_id_;
		auto c = std::make_unique<mojo::Remote<bm::SharedWorkerClient>>(std::move(client));
		c->set_disconnect_handler(base::BindOnce(&SharedWorker::Gone, base::Unretained(this), id));
		( *c )->OnCreated(type);
		clients_[id] = std::move(c);
		if ( failed_ ) {
			( *clients_[id] )->OnScriptLoadFailed(error_);
			return;
		}
		if ( worker_ ) worker_->Connect(id, std::move(port));
	}

	/* ---- what the worker tells its host */
	void OnConnected( int32_t id ) override
	{
		auto it = clients_.find(id);
		if ( it != clients_.end() ) ( *it->second )->OnConnected(blink::Vector<bm::WebFeature>());
	}
	void OnContextClosed() override { End(); }
	void OnReadyForInspection( mojo::PendingRemote<bm::DevToolsAgent>, mojo::PendingReceiver<bm::DevToolsAgentHost> ) override {}
	void OnScriptLoadFailed( const blink::String &message ) override
	{
		printf("browser: shared worker: its script is not loaded: %s\n", message.Utf8().c_str());
		failed_ = true;
		error_ = message;
		for ( auto &c : clients_ ) ( *c.second )->OnScriptLoadFailed(message);
	}
	void OnReportException( bm::SharedWorkerExceptionDetailsPtr details ) override
	{
		for ( auto &c : clients_ ) ( *c.second )->OnReportException(details.Clone());
	}
	void OnFeatureUsed( bm::WebFeature ) override {}

	/* the worker's context is gone: it is let go of after this call */
	void WorkerContextDestroyed() override
	{
		Forget();
		base::SingleThreadTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE, this);
	}

private:
	void Gone( int id )
	{
		clients_.erase(id);
		if ( clients_.empty() ) End();
	}

	/* no more pages: the worker is ended, and no page connects to it again */
	void End()
	{
		Forget();
		if ( worker_ && !ending_ ) {
			ending_ = true;
			worker_->TerminateWorkerContext();
		}
	}

	void Forget()
	{
		auto it = Workers().find(key_);
		if ( it != Workers().end() && it->second == this ) Workers().erase(it);
	}

	Key							key_;
	std::unique_ptr<blink::WebSharedWorker>			worker_;
	mojo::Receiver<bm::SharedWorkerHost>			host_{this};
	std::map<int, std::unique_ptr<mojo::Remote<bm::SharedWorkerClient>>>	clients_;
	int							next_id_ = 0;
	bool							failed_ = false, ending_ = false;
	blink::String						error_;
};

class Connector final : public bm::SharedWorkerConnector {
public:
	void Connect( bm::SharedWorkerInfoPtr info, mojo::PendingRemote<bm::SharedWorkerClient> client,
		      bm::SharedWorkerCreationContextType type, blink::MessagePortDescriptor port,
		      mojo::PendingRemote<bm::BlobURLToken> ) override
	{
		if ( !info || !info->options ) return;
		/*
		 * The worker belongs to the origin of the document that made it,
		 * a frame's own (its outgoing referrer is the document's address,
		 * its parent's for about:blank and srcdoc), not the page's: a
		 * frame of another origin gets a worker of its own origin. The
		 * page's origin stands in when the document has no address of
		 * its own.
		 */
		std::string from = br_page_origin();
		if ( info->outside_fetch_client_settings_object ) {
			const blink::KURL &doc = info->outside_fetch_client_settings_object->outgoing_referrer;
			if ( doc.IsValid() && doc.ProtocolIsInHttpFamily() ) {
				from = blink::SecurityOrigin::Create(doc)->ToString().Utf8();
			}
		}
		Key key(info->url.GetString().Utf8(), info->options->name.Utf8(), from);
		SharedWorker *&w = Workers()[key];
		if ( w == nullptr ) {
			w = new SharedWorker(key);
			w->Start(*info);
		}
		w->Connect(std::move(client), type, std::move(port));
	}
};

}  // namespace

/* The binder of the frames' broker, once when the engine is up: the connectors on the page's thread */
void br_shared_worker_init()
{
	blink::GetEmptyBrowserInterfaceBroker().SetBinderForTesting(
		bm::SharedWorkerConnector::Name_, base::BindRepeating([]( mojo::ScopedMessagePipeHandle h ) {
			mojo::MakeSelfOwnedReceiver(std::make_unique<Connector>(),
						    mojo::PendingReceiver<bm::SharedWorkerConnector>(std::move(h)));
		}));
}
