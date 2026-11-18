/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	filesystem_tessronos.cc
 *	ブラウザ: the file systems of the older File System API
 *	(webkitRequestFileSystem, webkitResolveLocalFileSystemURL) (design
 *	14.4.5).
 *
 *	What the browser's process keeps for Blink's modules/filesystem: an
 *	origin's temporary and persistent file system, a tree of directories
 *	and files whose bytes are in memory while the browser runs. Their
 *	addresses are filesystem: URLs ("filesystem:<origin>/temporary/a/b").
 *	Directories are made, listed, moved, copied and removed; a file is
 *	written from a Blob at a position and truncated, and read as a Blob of
 *	its bytes (RegisterBlob). The isolated and external file systems (of
 *	dropped files and of the system's) are not there.
 *
 *	The service is on a thread of its own: a page asks for a file's Blob
 *	with a call that waits for the answer, and the Blob is registered
 *	with the blob storage on its thread with another.
 */

#include <stdio.h>

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/files/safe_base_name.h"
#include "base/functional/bind.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/data_pipe_drainer.h"
#include "third_party/blink/public/mojom/blob/blob.mojom-blink.h"
#include "components/services/filesystem/public/mojom/types.mojom-blink.h"
#include "third_party/blink/public/mojom/filesystem/file_system.mojom-blink.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"
#include "third_party/blink/renderer/platform/blob/blob_data.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace {

namespace bm = blink::mojom::blink;
using Error = base::File::Error;

/* ---------------------------------------------------------------- the trees */

struct Node {
	bool			dir = false;
	std::vector<uint8_t>	bytes;
	base::Time		modified;
};

/* "<origin>/<type>" -> path ("/", "/a", "/a/b") -> node */
std::map<std::string, std::map<std::string, Node>> g_trees;

/* A filesystem: URL's tree and path; false when it names none */
bool parse( const blink::KURL &url, std::string *tree, std::string *path, bm::FileSystemType *type = nullptr )
{
	std::string s = url.GetString().Utf8();
	const std::string pre = "filesystem:";
	if ( s.compare(0, pre.size(), pre) != 0 ) return false;
	s = s.substr(pre.size());
	size_t q = s.find_first_of("?#");
	if ( q != std::string::npos ) s = s.substr(0, q);
	size_t scheme = s.find("://");
	if ( scheme == std::string::npos ) return false;
	size_t slash = s.find('/', scheme + 3);
	if ( slash == std::string::npos ) return false;
	std::string origin = s.substr(0, slash), rest = s.substr(slash + 1);
	size_t t = rest.find('/');
	std::string kind = rest.substr(0, t);
	if ( kind == "temporary" ) {
		if ( type ) *type = bm::FileSystemType::kTemporary;
	} else if ( kind == "persistent" ) {
		if ( type ) *type = bm::FileSystemType::kPersistent;
	} else {
		return false;
	}
	*tree = origin + "/" + kind;
	std::string p = ( t == std::string::npos ) ? "" : rest.substr(t + 1);
	std::string out;
	size_t i = 0;
	while ( i <= p.size() ) {	/* the path's parts, "." and ".." taken out */
		size_t j = p.find('/', i);
		if ( j == std::string::npos ) j = p.size();
		std::string part = p.substr(i, j - i);
		if ( part == ".." ) {
			size_t k = out.rfind('/');
			out = ( k == std::string::npos ) ? "" : out.substr(0, k);
		} else if ( !part.empty() && part != "." ) {
			out += "/" + blink::DecodeUrlEscapeSequences(blink::String::FromUtf8(part),
								     blink::DecodeUrlMode::kUtf8).Utf8();
		}
		i = j + 1;
	}
	*path = out.empty() ? "/" : out;
	return true;
}

std::string parent_of( const std::string &path )
{
	size_t k = path.rfind('/');
	return ( k == 0 || k == std::string::npos ) ? "/" : path.substr(0, k);
}

std::map<std::string, Node> &tree_of( const std::string &tree )
{
	auto &t = g_trees[tree];
	if ( t.find("/") == t.end() ) {
		t["/"].dir = true;
		t["/"].modified = base::Time::Now();
	}
	return t;
}

/* the entries under a path (all depths) */
std::vector<std::string> under( std::map<std::string, Node> &t, const std::string &path )
{
	std::vector<std::string> out;
	std::string pre = ( path == "/" ) ? "/" : path + "/";
	for ( auto it = t.lower_bound(pre); it != t.end() && it->first.compare(0, pre.size(), pre) == 0; ++it ) {
		if ( it->first != "/" ) out.push_back(it->first);
	}
	return out;
}

base::File::Info info_of( const Node &n )
{
	base::File::Info i;
	i.size = n.dir ? 0 : (int64_t)n.bytes.size();
	i.is_directory = n.dir;
	i.last_modified = n.modified;
	i.last_accessed = n.modified;
	i.creation_time = n.modified;
	return i;
}

/* ---------------------------------------------------------------- reading a Blob written into a file */

class BlobReader final : public bm::BlobReaderClient, public mojo::DataPipeDrainer::Client {
public:
	using Done = base::OnceCallback<void( bool, std::vector<uint8_t> )>;

	static void Read( mojo::PendingRemote<bm::Blob> blob, Done done )
	{
		auto *r = new BlobReader(std::move(done));
		mojo::ScopedDataPipeProducerHandle producer;
		mojo::ScopedDataPipeConsumerHandle consumer;
		if ( mojo::CreateDataPipe(64 * 1024, producer, consumer) != MOJO_RESULT_OK ) {
			r->Finish(false);
			return;
		}
		r->blob_.Bind(std::move(blob));
		r->drainer_ = std::make_unique<mojo::DataPipeDrainer>(r, std::move(consumer));
		r->blob_->ReadAll(std::move(producer), r->client_.BindNewPipeAndPassRemote());
	}

	void OnCalculatedSize( uint64_t, uint64_t ) override {}
	void OnComplete( int32_t status, uint64_t ) override
	{
		status_known_ = true;
		ok_ = status == 0;
		if ( drained_ ) Finish(ok_);
	}
	void OnDataAvailable( base::span<const uint8_t> data ) override { bytes_.insert(bytes_.end(), data.begin(), data.end()); }
	void OnDataComplete() override
	{
		drained_ = true;
		if ( status_known_ ) Finish(ok_);
	}

private:
	explicit BlobReader( Done done ) : done_(std::move(done)) {}

	void Finish( bool ok )
	{
		std::move(done_).Run(ok, std::move(bytes_));
		delete this;
	}

	Done					done_;
	mojo::Remote<bm::Blob>			blob_;
	mojo::Receiver<bm::BlobReaderClient>	client_{this};
	std::unique_ptr<mojo::DataPipeDrainer>	drainer_;
	std::vector<uint8_t>			bytes_;
	bool					status_known_ = false, ok_ = false, drained_ = false;
};

/* ---------------------------------------------------------------- the service */

class Manager final : public bm::FileSystemManager {
public:
	void Bind( mojo::PendingReceiver<bm::FileSystemManager> r, std::string origin )
	{
		receivers_.Add(this, std::move(r), std::move(origin));
	}

	void Open( bm::FileSystemType type, OpenCallback cb ) override
	{
		const std::string &origin = receivers_.current_context();
		const char *kind = type == bm::FileSystemType::kTemporary ? "temporary"
				 : type == bm::FileSystemType::kPersistent ? "persistent" : nullptr;
		if ( kind == nullptr || origin.empty() || origin == "null" ) {
			std::move(cb).Run(blink::String(), blink::KURL(), Error::FILE_ERROR_SECURITY);
			return;
		}
		tree_of(origin + "/" + kind);
		std::string name = origin.substr(origin.find("://") + 3) + ":" + ( type == bm::FileSystemType::kTemporary ? "Temporary" : "Persistent" );
		std::move(cb).Run(blink::String::FromUtf8(name), blink::KURL(blink::String::FromUtf8("filesystem:" + origin + "/" + kind + "/")),
				  Error::FILE_OK);
	}

	void ResolveURL( const blink::KURL &url, ResolveURLCallback cb ) override
	{
		std::string tree, path;
		bm::FileSystemType type;
		if ( !parse(url, &tree, &path, &type) ) {
			std::move(cb).Run(nullptr, base::FilePath(), false, Error::FILE_ERROR_INVALID_URL);
			return;
		}
		auto &t = tree_of(tree);
		auto n = t.find(path);
		if ( n == t.end() ) {
			std::move(cb).Run(nullptr, base::FilePath(), false, Error::FILE_ERROR_NOT_FOUND);
			return;
		}
		std::string origin = tree.substr(0, tree.rfind('/')), kind = tree.substr(tree.rfind('/') + 1);
		auto fsinfo = bm::FileSystemInfo::New(blink::String::FromUtf8(origin.substr(origin.find("://") + 3) + ":" + kind),
						      blink::KURL(blink::String::FromUtf8("filesystem:" + origin + "/" + kind + "/")), type);
		std::move(cb).Run(std::move(fsinfo), base::FilePath(path), n->second.dir, Error::FILE_OK);
	}

	void Move( const blink::KURL &src, const blink::KURL &dst, MoveCallback cb ) override
	{
		std::move(cb).Run(Transfer(src, dst, true));
	}
	void Copy( const blink::KURL &src, const blink::KURL &dst, CopyCallback cb ) override
	{
		std::move(cb).Run(Transfer(src, dst, false));
	}

	void Remove( const blink::KURL &url, bool recursive, RemoveCallback cb ) override
	{
		std::string tree, path;
		if ( !parse(url, &tree, &path) ) {
			std::move(cb).Run(Error::FILE_ERROR_INVALID_URL);
			return;
		}
		auto &t = tree_of(tree);
		auto n = t.find(path);
		if ( n == t.end() ) {
			std::move(cb).Run(Error::FILE_ERROR_NOT_FOUND);
			return;
		}
		if ( path == "/" ) {
			std::move(cb).Run(Error::FILE_ERROR_INVALID_OPERATION);
			return;
		}
		std::vector<std::string> inside = under(t, path);
		if ( n->second.dir && !inside.empty() && !recursive ) {
			std::move(cb).Run(Error::FILE_ERROR_NOT_EMPTY);
			return;
		}
		for ( auto &p : inside ) t.erase(p);
		t.erase(path);
		std::move(cb).Run(Error::FILE_OK);
	}

	void ReadMetadata( const blink::KURL &url, ReadMetadataCallback cb ) override
	{
		Node *n = Find(url);
		if ( n == nullptr ) {
			std::move(cb).Run(base::File::Info(), Error::FILE_ERROR_NOT_FOUND);
			return;
		}
		std::move(cb).Run(info_of(*n), Error::FILE_OK);
	}

	void Create( const blink::KURL &url, bool exclusive, bool is_directory, bool recursive, CreateCallback cb ) override
	{
		std::string tree, path;
		if ( !parse(url, &tree, &path) ) {
			std::move(cb).Run(Error::FILE_ERROR_INVALID_URL);
			return;
		}
		auto &t = tree_of(tree);
		auto n = t.find(path);
		if ( n != t.end() ) {
			if ( exclusive ) {
				std::move(cb).Run(Error::FILE_ERROR_EXISTS);
			} else if ( n->second.dir != is_directory ) {
				std::move(cb).Run(is_directory ? Error::FILE_ERROR_NOT_A_DIRECTORY : Error::FILE_ERROR_NOT_A_FILE);
			} else {
				std::move(cb).Run(Error::FILE_OK);
			}
			return;
		}
		std::string parent = parent_of(path);
		auto p = t.find(parent);
		if ( p == t.end() ) {
			if ( !recursive ) {
				std::move(cb).Run(Error::FILE_ERROR_NOT_FOUND);
				return;
			}
			for ( std::string q = parent; q != "/" && t.find(q) == t.end(); q = parent_of(q) ) {
				t[q].dir = true;
				t[q].modified = base::Time::Now();
			}
		} else if ( !p->second.dir ) {
			std::move(cb).Run(Error::FILE_ERROR_NOT_A_DIRECTORY);
			return;
		}
		Node &made = t[path];
		made.dir = is_directory;
		made.modified = base::Time::Now();
		std::move(cb).Run(Error::FILE_OK);
	}

	void Exists( const blink::KURL &url, bool is_directory, ExistsCallback cb ) override
	{
		Node *n = Find(url);
		if ( n == nullptr ) {
			std::move(cb).Run(Error::FILE_ERROR_NOT_FOUND);
		} else if ( n->dir != is_directory ) {
			std::move(cb).Run(is_directory ? Error::FILE_ERROR_NOT_A_DIRECTORY : Error::FILE_ERROR_NOT_A_FILE);
		} else {
			std::move(cb).Run(Error::FILE_OK);
		}
	}

	void ReadDirectory( const blink::KURL &url, mojo::PendingRemote<bm::FileSystemOperationListener> l ) override
	{
		mojo::Remote<bm::FileSystemOperationListener> listener(std::move(l));
		blink::Vector<filesystem::mojom::blink::DirectoryEntryPtr> entries;
		Error e = List(url, &entries);
		if ( e != Error::FILE_OK ) {
			listener->ErrorOccurred(e);
			return;
		}
		listener->ResultsRetrieved(std::move(entries), false);
	}

	void ReadDirectorySync( const blink::KURL &url, ReadDirectorySyncCallback cb ) override
	{
		blink::Vector<filesystem::mojom::blink::DirectoryEntryPtr> entries;
		Error e = List(url, &entries);
		std::move(cb).Run(std::move(entries), e);
	}

	void Write( const blink::KURL &url, mojo::PendingRemote<bm::Blob> blob, int64_t position,
		    mojo::PendingReceiver<bm::FileSystemCancellableOperation>,
		    mojo::PendingRemote<bm::FileSystemOperationListener> l ) override
	{
		auto listener = std::make_shared<mojo::Remote<bm::FileSystemOperationListener>>(std::move(l));
		WriteBlob(url, std::move(blob), position, base::BindOnce([]( std::shared_ptr<mojo::Remote<bm::FileSystemOperationListener>> l,
									      int64_t n, Error e ) {
			if ( e != Error::FILE_OK ) {
				( *l )->ErrorOccurred(e);
			} else {
				( *l )->DidWrite(n, true);
			}
		}, listener));
	}

	void WriteSync( const blink::KURL &url, mojo::PendingRemote<bm::Blob> blob, int64_t position, WriteSyncCallback cb ) override
	{
		WriteBlob(url, std::move(blob), position, std::move(cb));
	}

	void Truncate( const blink::KURL &url, int64_t length, mojo::PendingReceiver<bm::FileSystemCancellableOperation>,
		       TruncateCallback cb ) override
	{
		std::move(cb).Run(Resize(url, length));
	}
	void TruncateSync( const blink::KURL &url, int64_t length, TruncateSyncCallback cb ) override
	{
		std::move(cb).Run(Resize(url, length));
	}

	/* a file's state for its File, which is then read through RegisterBlob: no platform path */
	void CreateSnapshotFile( const blink::KURL &url, CreateSnapshotFileCallback cb ) override
	{
		Node *n = Find(url);
		if ( n == nullptr || n->dir ) {
			std::move(cb).Run(base::File::Info(), base::FilePath(),
					  n == nullptr ? Error::FILE_ERROR_NOT_FOUND : Error::FILE_ERROR_NOT_A_FILE, mojo::NullRemote());
			return;
		}
		std::move(cb).Run(info_of(*n), base::FilePath(), Error::FILE_OK, mojo::NullRemote());
	}

	void GetPlatformPath( const blink::KURL &, GetPlatformPathCallback cb ) override { std::move(cb).Run(base::FilePath()); }

	void RegisterBlob( const blink::String &type, const blink::KURL &url, uint64_t, std::optional<base::Time>,
			   RegisterBlobCallback cb ) override
	{
		Node *n = Find(url);
		auto data = std::make_unique<blink::BlobData>();
		data->SetContentType(type);
		uint64_t size = 0;
		if ( n != nullptr && !n->dir ) {
			data->AppendBytes(base::span<const uint8_t>(n->bytes));
			size = n->bytes.size();
		}
		std::move(cb).Run(blink::BlobDataHandle::Create(std::move(data), size));
	}

private:
	Node *Find( const blink::KURL &url )
	{
		std::string tree, path;
		if ( !parse(url, &tree, &path) ) return nullptr;
		auto &t = tree_of(tree);
		auto n = t.find(path);
		return n == t.end() ? nullptr : &n->second;
	}

	Error List( const blink::KURL &url, blink::Vector<filesystem::mojom::blink::DirectoryEntryPtr> *out )
	{
		std::string tree, path;
		if ( !parse(url, &tree, &path) ) return Error::FILE_ERROR_INVALID_URL;
		auto &t = tree_of(tree);
		auto n = t.find(path);
		if ( n == t.end() ) return Error::FILE_ERROR_NOT_FOUND;
		if ( !n->second.dir ) return Error::FILE_ERROR_NOT_A_DIRECTORY;
		for ( auto &p : under(t, path) ) {
			if ( parent_of(p) != path ) continue;
			std::string name = p.substr(p.rfind('/') + 1);
			auto base = base::SafeBaseName::Create(base::FilePath(name));
			if ( !base ) continue;
			out->push_back(filesystem::mojom::blink::DirectoryEntry::New(
				*base, blink::String::FromUtf8(name),
				t[p].dir ? filesystem::mojom::blink::FsFileType::DIRECTORY
					 : filesystem::mojom::blink::FsFileType::REGULAR_FILE));
		}
		return Error::FILE_OK;
	}

	Error Transfer( const blink::KURL &src, const blink::KURL &dst, bool move )
	{
		std::string st, sp, dt, dp;
		if ( !parse(src, &st, &sp) || !parse(dst, &dt, &dp) ) return Error::FILE_ERROR_INVALID_URL;
		if ( st != dt ) return Error::FILE_ERROR_SECURITY;
		auto &t = tree_of(st);
		auto s = t.find(sp);
		if ( s == t.end() ) return Error::FILE_ERROR_NOT_FOUND;
		if ( sp == "/" || dp == sp || dp.compare(0, sp.size() + 1, sp + "/") == 0 ) return Error::FILE_ERROR_INVALID_OPERATION;
		auto p = t.find(parent_of(dp));
		if ( p == t.end() ) return Error::FILE_ERROR_NOT_FOUND;
		if ( !p->second.dir ) return Error::FILE_ERROR_NOT_A_DIRECTORY;
		auto d = t.find(dp);
		if ( d != t.end() ) {
			if ( d->second.dir != s->second.dir ) {
				return d->second.dir ? Error::FILE_ERROR_NOT_A_FILE : Error::FILE_ERROR_NOT_A_DIRECTORY;
			}
			if ( d->second.dir && !under(t, dp).empty() ) return Error::FILE_ERROR_NOT_EMPTY;
		}
		std::vector<std::pair<std::string, Node>> moved;
		moved.emplace_back(dp, s->second);
		for ( auto &q : under(t, sp) ) moved.emplace_back(dp + q.substr(sp.size()), t[q]);
		if ( move ) {
			for ( auto &q : under(t, sp) ) t.erase(q);
			t.erase(sp);
		}
		for ( auto &m : moved ) {
			m.second.modified = base::Time::Now();
			t[m.first] = std::move(m.second);
		}
		return Error::FILE_OK;
	}

	Error Resize( const blink::KURL &url, int64_t length )
	{
		Node *n = Find(url);
		if ( n == nullptr ) return Error::FILE_ERROR_NOT_FOUND;
		if ( n->dir ) return Error::FILE_ERROR_NOT_A_FILE;
		if ( length < 0 ) return Error::FILE_ERROR_INVALID_OPERATION;
		n->bytes.resize((size_t)length);
		n->modified = base::Time::Now();
		return Error::FILE_OK;
	}

	void WriteBlob( const blink::KURL &url, mojo::PendingRemote<bm::Blob> blob, int64_t position,
			base::OnceCallback<void( int64_t, Error )> done )
	{
		if ( Node *n = Find(url); n == nullptr || n->dir || position < 0 ) {
			std::move(done).Run(0, n == nullptr ? Error::FILE_ERROR_NOT_FOUND
						 : n->dir ? Error::FILE_ERROR_NOT_A_FILE : Error::FILE_ERROR_INVALID_OPERATION);
			return;
		}
		BlobReader::Read(std::move(blob), base::BindOnce([]( Manager *self, blink::KURL url, int64_t position,
								       base::OnceCallback<void( int64_t, Error )> done,
								       bool ok, std::vector<uint8_t> bytes ) {
			Node *n = self->Find(url);
			if ( !ok || n == nullptr ) {
				std::move(done).Run(0, ok ? Error::FILE_ERROR_NOT_FOUND : Error::FILE_ERROR_FAILED);
				return;
			}
			if ( n->bytes.size() < (size_t)position + bytes.size() ) n->bytes.resize((size_t)position + bytes.size());
			std::copy(bytes.begin(), bytes.end(), n->bytes.begin() + position);
			n->modified = base::Time::Now();
			std::move(done).Run((int64_t)bytes.size(), Error::FILE_OK);
		}, base::Unretained(this), url, position, std::move(done)));
	}

	mojo::ReceiverSet<bm::FileSystemManager, std::string>	receivers_;
};

scoped_refptr<base::SingleThreadTaskRunner> fs_runner()
{
	static base::Thread *thread = [] {
		auto *t = new base::Thread("file systems");
		if ( !t->StartWithOptions(base::Thread::Options()) ) printf("browser: the file systems' thread did not start\n");
		return t;
	}();
	return thread->task_runner();
}

void bind_on_thread( mojo::PendingReceiver<bm::FileSystemManager> r, std::string origin )
{
	fs_runner()->PostTask(FROM_HERE, base::BindOnce([]( mojo::PendingReceiver<bm::FileSystemManager> r, std::string origin ) {
		static Manager *m = new Manager();
		m->Bind(std::move(r), std::move(origin));
	}, std::move(r), std::move(origin)));
}

}  // namespace

std::string br_page_origin();

/* A worker's broker's receiver: true when it is the file systems' manager */
bool br_filesystem_bind( mojo::GenericPendingReceiver &r, const std::string &origin )
{
	if ( auto f = r.As<bm::FileSystemManager>() ) {
		bind_on_thread(std::move(f), origin);
		return true;
	}
	return false;
}

/* The manager a context's files are registered through (the core's), one for each thread */
bm::FileSystemManager &br_filesystem_manager()
{
	thread_local mojo::Remote<bm::FileSystemManager> *remote = nullptr;
	if ( remote == nullptr ) {
		remote = new mojo::Remote<bm::FileSystemManager>();
		bind_on_thread(remote->BindNewPipeAndPassReceiver(), br_page_origin());
	}
	return *remote->get();
}

/* The binder of the frames' broker, once when the engine is up */
void br_filesystem_init()
{
	blink::GetEmptyBrowserInterfaceBroker().SetBinderForTesting(
		bm::FileSystemManager::Name_, base::BindRepeating([]( mojo::ScopedMessagePipeHandle h ) {
			bind_on_thread(mojo::PendingReceiver<bm::FileSystemManager>(std::move(h)), br_page_origin());
		}));
}
