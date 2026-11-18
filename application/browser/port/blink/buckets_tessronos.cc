/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	buckets_tessronos.cc
 *	ブラウザ: navigator.storageBuckets, a page's named buckets of storage
 *	(design 14.4.5).
 *
 *	A bucket is a storage of its own for the page's origin: its
 *	IndexedDB databases and its Cache Storage are those of the origin's
 *	key with the bucket's name after it (indexeddb_tessronos.cc,
 *	cache_storage_tessronos.cc), so they are apart from the default
 *	bucket's and from the other buckets', and the databases are kept
 *	with the others over runs. The locks are the origin's. A bucket has
 *	no directory (the origin private file system is not there). Its
 *	policies (persisted, durability, expiry) are kept while the browser
 *	runs; deleting a bucket deletes its databases and caches.
 *
 *	The managers and buckets are on the services' thread; the databases'
 *	factory is bound on the page's.
 */

#include <stdio.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "third_party/blink/public/mojom/buckets/bucket_manager_host.mojom-blink.h"
#include "third_party/blink/public/mojom/cache_storage/cache_storage.mojom-blink.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_directory_handle.mojom-blink.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_error.mojom-blink.h"
#include "third_party/blink/public/mojom/indexeddb/indexeddb.mojom-blink.h"
#include "third_party/blink/public/mojom/locks/lock_manager.mojom-blink.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

#include "../../br.h"

/* The services' thread (blob_tessronos.cc) */
void br_on_service_thread( base::OnceClosure task );
/* A bucket's databases and caches, by its key (indexeddb_tessronos.cc, cache_storage_tessronos.cc) */
void br_idb_bind_key( const std::string &key, mojo::ScopedMessagePipeHandle pipe );
void br_idb_delete_key( const std::string &key );
void br_cache_storage_bind_key( const std::string &key, mojo::ScopedMessagePipeHandle pipe );
void br_cache_storage_delete_key( const std::string &key );
void br_opfs_root( const std::string &origin,
		   mojo::PendingReceiver<blink::mojom::blink::FileSystemAccessDirectoryHandle> r );	/* opfs_tessronos.cc */
void br_opfs_delete_key( const std::string &key );
/* The origin's locks (services_tessronos.cc) */
void br_locks_bind( mojo::ScopedMessagePipeHandle pipe );

namespace {

namespace bm = blink::mojom::blink;

constexpr int64_t	kQuota = 2LL * 1024 * 1024 * 1024;

scoped_refptr<base::SingleThreadTaskRunner> &page_runner()
{
	static auto *r = new scoped_refptr<base::SingleThreadTaskRunner>();
	return *r;
}

/* the key of a bucket's storage: the origin's, then the bucket's name */
std::string key_of( const std::string &origin, const std::string &name )
{
	return origin + "^bucket=" + name;
}

struct Policies {
	bool				persisted = false;
	bm::BucketDurability		durability = bm::BucketDurability::kRelaxed;
	std::optional<base::Time>	expires;
};

class Bucket final : public bm::BucketHost {
public:
	Bucket( std::string key, Policies *policies ) : key_(std::move(key)), policies_(policies) {}

	void Bind( mojo::PendingReceiver<bm::BucketHost> r ) { receivers_.Add(this, std::move(r)); }

	void Persist( PersistCallback cb ) override
	{
		policies_->persisted = true;
		std::move(cb).Run(true, true);
	}
	void Persisted( PersistedCallback cb ) override { std::move(cb).Run(policies_->persisted, true); }
	void Estimate( EstimateCallback cb ) override { std::move(cb).Run(0, kQuota, true); }
	void Durability( DurabilityCallback cb ) override { std::move(cb).Run(policies_->durability, true); }
	void SetExpires( base::Time t, SetExpiresCallback cb ) override
	{
		policies_->expires = t;
		std::move(cb).Run(true);
	}
	void Expires( ExpiresCallback cb ) override { std::move(cb).Run(policies_->expires, true); }

	void GetIdbFactory( mojo::PendingReceiver<bm::IDBFactory> r ) override
	{
		std::string key = key_;
		page_runner()->PostTask(FROM_HERE, base::BindOnce([]( std::string key, mojo::ScopedMessagePipeHandle h ) {
			br_idb_bind_key(key, std::move(h));
		}, key, r.PassPipe()));
	}
	void GetLockManager( mojo::PendingReceiver<bm::LockManager> r ) override { br_locks_bind(r.PassPipe()); }
	void GetCaches( mojo::PendingReceiver<bm::CacheStorage> r ) override { br_cache_storage_bind_key(key_, r.PassPipe()); }
	/* the bucket's own private file system, under its key */
	void GetDirectory( GetDirectoryCallback cb ) override
	{
		mojo::PendingRemote<bm::FileSystemAccessDirectoryHandle> root;
		br_on_service_thread(base::BindOnce([]( std::string key,
							mojo::PendingReceiver<bm::FileSystemAccessDirectoryHandle> r ) {
			br_opfs_root(key, std::move(r));
		}, key_, root.InitWithNewPipeAndPassReceiver()));
		std::move(cb).Run(bm::FileSystemAccessError::New(bm::FileSystemAccessStatus::kOk, 0, ""), std::move(root));
	}
	void GetDirectoryForDevtools( const blink::Vector<blink::String> &, GetDirectoryForDevtoolsCallback cb ) override
	{
		GetDirectory(std::move(cb));
	}

private:
	std::string				key_;
	Policies				*policies_;
	mojo::ReceiverSet<bm::BucketHost>	receivers_;
};

/* origin -> name -> its policies and host */
struct Entry {
	Policies		policies;
	std::unique_ptr<Bucket>	host;
};
std::map<std::string, std::map<std::string, Entry>> g_buckets;

class Manager final : public bm::BucketManagerHost {
public:
	void Bind( mojo::PendingReceiver<bm::BucketManagerHost> r ) { receivers_.Add(this, std::move(r), br_page_origin()); }
	/* a worker's, for its own origin */
	void BindFor( const std::string &origin, mojo::PendingReceiver<bm::BucketManagerHost> r )
	{
		receivers_.Add(this, std::move(r), origin);
	}

	void OpenBucket( const blink::String &name, bm::BucketPoliciesPtr p, OpenBucketCallback cb ) override
	{
		const std::string &origin = receivers_.current_context();
		std::string n = name.Utf8();
		auto &all = g_buckets[origin];
		bool fresh = all.find(n) == all.end();
		Entry &e = all[n];
		if ( fresh ) {
			if ( p ) {
				if ( p->has_persisted ) e.policies.persisted = p->persisted;
				if ( p->has_durability ) e.policies.durability = p->durability;
				if ( p->expires ) e.policies.expires = p->expires;
			}
			e.host = std::make_unique<Bucket>(key_of(origin, n), &e.policies);
			printf("browser: bucket %s opened\n", n.c_str());
		}
		mojo::PendingRemote<bm::BucketHost> remote;
		e.host->Bind(remote.InitWithNewPipeAndPassReceiver());
		std::move(cb).Run(std::move(remote), bm::BucketError::kUnknown);
	}
	void GetBucketForDevtools( const blink::String &, mojo::PendingReceiver<bm::BucketHost> ) override {}
	void Keys( KeysCallback cb ) override
	{
		blink::Vector<blink::String> names;
		for ( auto &e : g_buckets[receivers_.current_context()] ) names.push_back(blink::String::FromUtf8(e.first));
		std::move(cb).Run(names, true);
	}
	void DeleteBucket( const blink::String &name, DeleteBucketCallback cb ) override
	{
		const std::string &origin = receivers_.current_context();
		std::string n = name.Utf8();
		auto &all = g_buckets[origin];
		all.erase(n);
		std::string key = key_of(origin, n);
		br_cache_storage_delete_key(key);
		br_opfs_delete_key(key);
		page_runner()->PostTask(FROM_HERE, base::BindOnce(&br_idb_delete_key, key));
		std::move(cb).Run(true);
	}

private:
	mojo::ReceiverSet<bm::BucketManagerHost, std::string>	receivers_;
};

}  // namespace

/* The one manager, on the services' thread */
static Manager *TheManager()
{
	static Manager *m = new Manager();
	return m;
}

/* The binder of the frames' broker, once when the engine is up (on the page's thread) */
void br_buckets_init()
{
	page_runner() = base::SingleThreadTaskRunner::GetCurrentDefault();
	blink::GetEmptyBrowserInterfaceBroker().SetBinderForTesting(
		bm::BucketManagerHost::Name_, base::BindRepeating([]( mojo::ScopedMessagePipeHandle h ) {
			br_on_service_thread(base::BindOnce([]( mojo::PendingReceiver<bm::BucketManagerHost> r ) {
				TheManager()->Bind(std::move(r));
			}, mojo::PendingReceiver<bm::BucketManagerHost>(std::move(h))));
		}));
}

/* The manager for a worker of an origin, asked through the worker's broker (blob_tessronos.cc) */
void br_buckets_bind_for( const std::string &origin, mojo::ScopedMessagePipeHandle h )
{
	br_on_service_thread(base::BindOnce([]( std::string origin, mojo::PendingReceiver<bm::BucketManagerHost> r ) {
		TheManager()->BindFor(origin, std::move(r));
	}, origin, mojo::PendingReceiver<bm::BucketManagerHost>(std::move(h))));
}
