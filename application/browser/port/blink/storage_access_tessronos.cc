/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	storage_access_tessronos.cc
 *	ブラウザ: the handle of document.requestStorageAccess({...})
 *	(StorageAccessHandle), whose storage a browser gives (design 14.4.5).
 *
 *	A document granted its access (an iframe of another site; the top
 *	level document has its own storage already) gets a handle to its
 *	origin's own, first-party storage: the IndexedDB databases, Cache
 *	Storage and origin private file system kept under its origin's key
 *	(indexeddb_tessronos.cc, cache_storage_tessronos.cc,
 *	opfs_tessronos.cc), the Web Locks, and the shared workers and
 *	quota the frames' broker gives. Its blob: URLs' store and
 *	BroadcastChannel connections are the frame's (blob_tessronos.cc,
 *	services_tessronos.cc), and its localStorage and sessionStorage the
 *	window's own, kept by the window's origin (storage_tessronos.cc;
 *	patch global_storage_access_handle.patch).
 *
 *	The patch tells the origin (BrStorageAccessFor) right before it asks
 *	the broker for the handle, whose binder runs then, on the page's
 *	thread.
 */

#include <stdio.h>

#include <memory>
#include <string>
#include <utility>

#include "base/byte_size.h"
#include "base/functional/bind.h"
#include "mojo/public/cpp/bindings/generic_pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_associated_receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "third_party/blink/public/mojom/blob/blob_url_store.mojom-blink.h"
#include "third_party/blink/public/mojom/broadcastchannel/broadcast_channel.mojom-blink.h"
#include "third_party/blink/public/mojom/cache_storage/cache_storage.mojom-blink.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_directory_handle.mojom-blink.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_error.mojom-blink.h"
#include "third_party/blink/public/mojom/indexeddb/indexeddb.mojom-blink.h"
#include "third_party/blink/public/mojom/locks/lock_manager.mojom-blink.h"
#include "third_party/blink/public/mojom/quota/quota_manager_host.mojom-blink.h"
#include "third_party/blink/public/mojom/quota/quota_types.mojom-blink.h"
#include "third_party/blink/public/mojom/storage_access/storage_access_handle.mojom-blink.h"
#include "third_party/blink/public/mojom/worker/shared_worker_connector.mojom-blink.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"

#include "../../br.h"

/* The services' thread (blob_tessronos.cc) */
void br_on_service_thread( base::OnceClosure task );
/* An origin's databases and caches, by its key (indexeddb_tessronos.cc, cache_storage_tessronos.cc) */
void br_idb_bind_key( const std::string &key, mojo::ScopedMessagePipeHandle pipe );
void br_cache_storage_bind_key( const std::string &key, mojo::ScopedMessagePipeHandle pipe );
/* The origin's locks (services_tessronos.cc) */
void br_locks_bind( mojo::ScopedMessagePipeHandle pipe );
/* The origin's private file system (opfs_tessronos.cc) */
void br_opfs_root( const std::string &origin,
		   mojo::PendingReceiver<blink::mojom::blink::FileSystemAccessDirectoryHandle> r );
/* The frame's blob: URL store and BroadcastChannel provider (blob_tessronos.cc, services_tessronos.cc) */
void br_blob_frame_store_bind( mojo::ScopedInterfaceEndpointHandle handle );
void br_broadcast_frame_bind( mojo::ScopedInterfaceEndpointHandle handle );

namespace {

namespace bm = blink::mojom::blink;

/* The origin of the handle asked for next (BrStorageAccessFor) */
std::string g_for;

/* A receiver handed to the frames' broker, as the window's own is */
template <typename Interface>
void ByBroker( mojo::PendingReceiver<Interface> r )
{
	mojo::GenericPendingReceiver g(std::move(r));
	blink::GetEmptyBrowserInterfaceBroker().GetInterface(std::move(g));
}

class Handle final : public bm::StorageAccessHandle {
public:
	explicit Handle( std::string origin ) : origin_(std::move(origin)) {}

	void BindIndexedDB( mojo::PendingReceiver<bm::IDBFactory> r ) override { br_idb_bind_key(origin_, r.PassPipe()); }
	void BindLocks( mojo::PendingReceiver<bm::LockManager> r ) override { br_locks_bind(r.PassPipe()); }
	void BindCaches( mojo::PendingReceiver<bm::CacheStorage> r ) override
	{
		br_cache_storage_bind_key(origin_, r.PassPipe());
	}
	void BindSharedWorker( mojo::PendingReceiver<bm::SharedWorkerConnector> r ) override { ByBroker(std::move(r)); }

	void GetDirectory( GetDirectoryCallback cb ) override
	{
		mojo::PendingRemote<bm::FileSystemAccessDirectoryHandle> root;
		br_on_service_thread(base::BindOnce([]( std::string origin,
							mojo::PendingReceiver<bm::FileSystemAccessDirectoryHandle> r ) {
			br_opfs_root(origin, std::move(r));
		}, origin_, root.InitWithNewPipeAndPassReceiver()));
		std::move(cb).Run(bm::FileSystemAccessError::New(bm::FileSystemAccessStatus::kOk, base::File::FILE_OK, ""), std::move(root));
	}

	/* what navigator.storage.estimate() answers, from the same quota service */
	void Estimate( EstimateCallback cb ) override
	{
		auto quota = std::make_unique<mojo::Remote<bm::QuotaManagerHost>>();
		ByBroker((*quota).BindNewPipeAndPassReceiver());
		mojo::Remote<bm::QuotaManagerHost> *q = quota.get();
		(*q)->QueryStorageUsageAndQuota(base::BindOnce(
			[]( std::unique_ptr<mojo::Remote<bm::QuotaManagerHost>>, EstimateCallback cb,
			    bm::QuotaStatusCode status, int64_t usage, int64_t quota, bm::UsageBreakdownPtr ) {
				bool ok = status == bm::QuotaStatusCode::kOk;
				std::move(cb).Run(base::ByteSize(ok && usage > 0 ? (uint64_t)usage : 0),
						  base::ByteSize(ok && quota > 0 ? (uint64_t)quota : 0), ok);
			}, std::move(quota), std::move(cb)));
	}

	void BindBlobStorage( mojo::PendingAssociatedReceiver<bm::BlobURLStore> r ) override
	{
		br_blob_frame_store_bind(r.PassHandle());
	}
	void BindBroadcastChannel( mojo::PendingAssociatedReceiver<bm::BroadcastChannelProvider> r ) override
	{
		br_broadcast_frame_bind(r.PassHandle());
	}

private:
	std::string	origin_;
};

}  // namespace

/* Whose first-party storage the handle asked for next is (patch global_storage_access_handle.patch) */
void BrStorageAccessFor( const std::string &origin )
{
	g_for = origin;
}

/* The binder of the frames' broker, once when the engine is up (on the page's thread) */
void br_storage_access_init()
{
	blink::GetEmptyBrowserInterfaceBroker().SetBinderForTesting(
		bm::StorageAccessHandle::Name_, base::BindRepeating([]( mojo::ScopedMessagePipeHandle h ) {
			std::string origin = g_for.empty() ? br_page_origin() : g_for;
			g_for.clear();
			printf("browser: storage access handle for %s\n", origin.c_str());
			mojo::MakeSelfOwnedReceiver(std::make_unique<Handle>(origin),
						    mojo::PendingReceiver<bm::StorageAccessHandle>(std::move(h)));
		}));
}
