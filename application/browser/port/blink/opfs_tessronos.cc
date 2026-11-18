/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	opfs_tessronos.cc
 *	ブラウザ: the origin private file system (navigator.storage.getDirectory,
 *	design 14.4.5).
 *
 *	Each origin has a tree of directories and files, kept in the process:
 *	a directory its entries by name, a file its bytes and the time it was
 *	written. The handles a page holds (FileSystemDirectoryHandle,
 *	FileSystemFileHandle) are each a pipe to a node of the tree; a node
 *	taken out of the tree answers NotFoundError.
 *
 *	  - getFileHandle and getDirectoryHandle find an entry, or make one
 *	    with create; entries() lists a directory; removeEntry, remove,
 *	    move (a new name, another directory, or both) and resolve work
 *	    the tree.
 *	  - createWritable writes what the page wrote when the stream is
 *	    closed.
 *	  - createSyncAccessHandle (in a dedicated worker) is answered with a
 *	    delegate of the file -- the Read, Write, GetLength and SetLength a
 *	    browser answers in an incognito window -- whose calls work on the
 *	    file's bytes as they come; the handle locks the file until it is
 *	    closed. While it is open no writable stream is made, and the other
 *	    way round.
 *	  - A handle sent to another context (postMessage) goes as a transfer
 *	    token, which names its node.
 *
 *	The trees are kept between runs in the store "opfs" (record 8 of
 *	ブラウザの保存域): two seconds after one last changed the whole of them
 *	is made one record, read back when the browser starts.
 *
 *	A folder the user chose (showDirectoryPicker, a real object given out
 *	as a directory) is a tree of the same kind, not an origin's: two
 *	seconds after the page last changed it, the whole of it is taken in as
 *	a new real object in ブラウザの保存箱 (the one chosen is not written
 *	over), from a thread of its own.
 *
 *	Everything here is on the services' thread.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "base/files/file.h"
#include "base/functional/bind.h"
#include "base/time/time.h"
#include "base/unguessable_token.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "mojo/public/cpp/system/data_pipe_drainer.h"
#include "../net/br_data.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_access_handle_host.mojom-blink.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_cloud_identifier.mojom-blink.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_directory_handle.mojom-blink.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_error.mojom-blink.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_file_delegate_host.mojom-blink.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_file_handle.mojom-blink.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_file_writer.mojom-blink.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_transfer_token.mojom-blink.h"
#include "third_party/blink/renderer/platform/blob/blob_data.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

#include "../net/br_objfile.h"
#include "../net/br_store.h"

void br_on_service_thread( base::OnceClosure task );		/* blob_tessronos.cc */
void br_net_post_delayed( base::OnceClosure work, int ms );	/* net_fetch_tessronos.cc */

namespace {

namespace bm = blink::mojom::blink;

bm::FileSystemAccessErrorPtr Ok()
{
	return bm::FileSystemAccessError::New(bm::FileSystemAccessStatus::kOk, base::File::FILE_OK, "");
}

bm::FileSystemAccessErrorPtr Error( bm::FileSystemAccessStatus status, const char *why )
{
	return bm::FileSystemAccessError::New(status, base::File::FILE_ERROR_FAILED, why);
}

bm::FileSystemAccessErrorPtr NotFound()
{
	return bm::FileSystemAccessError::New(bm::FileSystemAccessStatus::kFileError, base::File::FILE_ERROR_NOT_FOUND,
					      "The entry is not there.");
}

/* ---------------------------------------------------------------- the trees */

struct Node {
	std::string					name;
	bool						dir = false;
	Node						*parent = nullptr;	/* null: a root, or out of the tree */
	std::map<std::string, std::shared_ptr<Node>>	kids;			/* a directory's */
	std::string					bytes;			/* a file's */
	base::Time					time;
	base::UnguessableToken				id = base::UnguessableToken::Create();
	int						writers = 0;		/* writable streams open */
	bool						locked = false;		/* an access handle open */
	bool						root = false;
};

std::map<std::string, std::shared_ptr<Node>>	g_roots;	/* by origin */

/* The folders the user chose: each its root, its name, and what was last taken in of it */
struct Picked {
	std::shared_ptr<Node>	root;
	std::string		name;
	std::string		saved;
};
std::vector<Picked>	g_picked;
std::map<base::UnguessableToken, std::weak_ptr<Node>> g_by_id;

void Changed();

/* Whether a node is still in its origin's tree */
bool InTree( const Node *n )
{
	while ( n != nullptr && !n->root ) n = n->parent;
	return n != nullptr;
}

/* A name an entry may have */
bool NameOk( const std::string &n )
{
	return !n.empty() && n != "." && n != ".." && n.find('/') == std::string::npos && n.find('\\') == std::string::npos;
}

std::shared_ptr<Node> NewNode( const std::string &name, bool dir, Node *parent )
{
	auto n = std::make_shared<Node>();
	n->name = name;
	n->dir = dir;
	n->parent = parent;
	n->time = base::Time::Now();
	g_by_id[n->id] = n;
	return n;
}

/* A node taken out of its directory, and all below it */
void Detach( Node *n )
{
	if ( n->parent != nullptr ) n->parent->kids.erase(n->name);
	n->parent = nullptr;
}

bool Busy( const Node *n )
{
	if ( n->locked || n->writers > 0 ) return true;
	for ( const auto &kv : n->kids ) {
		if ( Busy(kv.second.get()) ) return true;
	}
	return false;
}

/* ---------------------------------------------------------------- transfer tokens */

class Token final : public bm::FileSystemAccessTransferToken {
public:
	explicit Token( base::UnguessableToken id ) : id_(id) {}
	void GetInternalID( GetInternalIDCallback cb ) override { std::move(cb).Run(id_); }
	void Clone( mojo::PendingReceiver<bm::FileSystemAccessTransferToken> r ) override
	{
		mojo::MakeSelfOwnedReceiver(std::make_unique<Token>(id_), std::move(r));
	}

private:
	base::UnguessableToken	id_;
};

/* The node a token names, given to 'then' (null when it names none) */
void NodeOf( mojo::PendingRemote<bm::FileSystemAccessTransferToken> t,
	     base::OnceCallback<void( std::shared_ptr<Node> )> then )
{
	auto r = std::make_shared<mojo::Remote<bm::FileSystemAccessTransferToken>>(std::move(t));
	if ( !r->is_bound() ) {
		std::move(then).Run(nullptr);
		return;
	}
	bm::FileSystemAccessTransferToken *p = r->get();
	p->GetInternalID(base::BindOnce([]( std::shared_ptr<mojo::Remote<bm::FileSystemAccessTransferToken>>,
					    base::OnceCallback<void( std::shared_ptr<Node> )> then,
					    const base::UnguessableToken &id ) {
		auto it = g_by_id.find(id);
		std::shared_ptr<Node> n = it != g_by_id.end() ? it->second.lock() : nullptr;
		std::move(then).Run(n && InTree(n.get()) ? n : nullptr);
	}, r, std::move(then)));
}

/* ---------------------------------------------------------------- handles */

mojo::PendingRemote<bm::FileSystemAccessFileHandle> FileRemote( std::shared_ptr<Node> n );
mojo::PendingRemote<bm::FileSystemAccessDirectoryHandle> DirRemote( std::shared_ptr<Node> n );

/* A node moved: into another directory, under another name, or both */
bm::FileSystemAccessErrorPtr MoveTo( const std::shared_ptr<Node> &n, Node *to, const std::string &name )
{
	if ( !InTree(n.get()) || n->root ) return NotFound();
	if ( !NameOk(name) ) return Error(bm::FileSystemAccessStatus::kInvalidArgument, "That is not a name.");
	if ( Busy(n.get()) ) {
		return Error(bm::FileSystemAccessStatus::kNoModificationAllowedError, "The entry is in use.");
	}
	for ( Node *a = to; a != nullptr; a = a->parent ) {
		if ( a == n.get() ) {
			return Error(bm::FileSystemAccessStatus::kInvalidModificationError,
				     "A directory is not moved into itself.");
		}
	}
	auto there = to->kids.find(name);
	if ( there != to->kids.end() && there->second != n ) {
		if ( there->second->dir ) {
			return Error(bm::FileSystemAccessStatus::kInvalidModificationError, "A directory has that name.");
		}
		Detach(there->second.get());
	}
	std::shared_ptr<Node> keep = n;
	Detach(n.get());
	n->name = name;
	n->parent = to;
	to->kids[name] = n;
	Changed();
	return Ok();
}

/* What a page writes into a file, kept until the stream is closed */
class Writer final : public bm::FileSystemAccessFileWriter, public mojo::DataPipeDrainer::Client {
public:
	Writer( std::shared_ptr<Node> f, bool keep ) : file_(std::move(f))
	{
		file_->writers++;
		if ( keep ) data_ = file_->bytes;
	}
	~Writer() override { Done(); }

	void Write( uint64_t offset, mojo::ScopedDataPipeConsumerHandle stream, WriteCallback cb ) override
	{
		if ( drainer_ ) {
			std::move(cb).Run(Error(bm::FileSystemAccessStatus::kInvalidState, "A write is under way."), 0);
			return;
		}
		at_ = offset;
		got_.clear();
		write_cb_ = std::move(cb);
		drainer_ = std::make_unique<mojo::DataPipeDrainer>(this, std::move(stream));
	}
	void OnDataAvailable( base::span<const uint8_t> data ) override { got_.append(data.begin(), data.end()); }
	void OnDataComplete() override
	{
		if ( at_ + got_.size() > data_.size() ) data_.resize((size_t)( at_ + got_.size() ));
		data_.replace((size_t)at_, got_.size(), got_);
		drainer_.reset();
		if ( write_cb_ ) std::move(write_cb_).Run(Ok(), got_.size());
	}
	void Truncate( uint64_t length, TruncateCallback cb ) override
	{
		data_.resize((size_t)length);
		std::move(cb).Run(Ok());
	}
	void Close( CloseCallback cb ) override
	{
		if ( !InTree(file_.get()) ) {
			Done();
			std::move(cb).Run(NotFound());
			return;
		}
		file_->bytes = std::move(data_);
		file_->time = base::Time::Now();
		Done();
		Changed();
		std::move(cb).Run(Ok());
	}
	void Abort( AbortCallback cb ) override
	{
		Done();
		std::move(cb).Run(Ok());
	}

private:
	void Done()
	{
		if ( !done_ ) file_->writers--;
		done_ = true;
	}

	std::shared_ptr<Node>			file_;
	std::string				data_, got_;
	uint64_t				at_ = 0;
	bool					done_ = false;
	std::unique_ptr<mojo::DataPipeDrainer>	drainer_;
	WriteCallback				write_cb_;
};

/* An access handle's file (FileSystemSyncAccessHandle): its calls work on the bytes as they come */
class Delegate final : public bm::FileSystemAccessFileDelegateHost, public mojo::DataPipeDrainer::Client {
public:
	explicit Delegate( std::shared_ptr<Node> f ) : file_(std::move(f)) {}

	void Read( int64_t offset, int32_t n, ReadCallback cb ) override
	{
		const std::string &b = file_->bytes;
		if ( offset < 0 || n < 0 ) {
			std::move(cb).Run(std::nullopt, base::File::FILE_ERROR_INVALID_OPERATION, 0);
			return;
		}
		size_t at = std::min<size_t>((size_t)offset, b.size());
		size_t k = std::min<size_t>((size_t)n, b.size() - at);
		std::move(cb).Run(mojo_base::BigBuffer(base::as_byte_span(b).subspan(at, k)), base::File::FILE_OK, (int32_t)k);
	}
	void Write( int64_t offset, mojo::ScopedDataPipeConsumerHandle data, WriteCallback cb ) override
	{
		if ( drainer_ || offset < 0 ) {
			std::move(cb).Run(base::File::FILE_ERROR_INVALID_OPERATION, 0);
			return;
		}
		at_ = offset;
		got_.clear();
		write_cb_ = std::move(cb);
		drainer_ = std::make_unique<mojo::DataPipeDrainer>(this, std::move(data));
	}
	void OnDataAvailable( base::span<const uint8_t> data ) override { got_.append(data.begin(), data.end()); }
	void OnDataComplete() override
	{
		std::string &b = file_->bytes;
		if ( at_ + got_.size() > b.size() ) b.resize((size_t)( at_ + got_.size() ));
		b.replace((size_t)at_, got_.size(), got_);
		file_->time = base::Time::Now();
		drainer_.reset();
		Changed();
		if ( write_cb_ ) std::move(write_cb_).Run(base::File::FILE_OK, (int32_t)got_.size());
	}
	void GetLength( GetLengthCallback cb ) override
	{
		std::move(cb).Run(base::File::FILE_OK, (int64_t)file_->bytes.size());
	}
	void SetLength( int64_t length, SetLengthCallback cb ) override
	{
		if ( length < 0 ) {
			std::move(cb).Run(base::File::FILE_ERROR_INVALID_OPERATION);
			return;
		}
		file_->bytes.resize((size_t)length);
		file_->time = base::Time::Now();
		Changed();
		std::move(cb).Run(base::File::FILE_OK);
	}

private:
	std::shared_ptr<Node>			file_;
	std::string				got_;
	int64_t					at_ = 0;
	std::unique_ptr<mojo::DataPipeDrainer>	drainer_;
	WriteCallback				write_cb_;
};

/* The access handle's lock: let go when it is closed, or when its pipe goes */
class AccessHost final : public bm::FileSystemAccessAccessHandleHost {
public:
	explicit AccessHost( std::shared_ptr<Node> f ) : file_(std::move(f)) { file_->locked = true; }
	~AccessHost() override { file_->locked = false; }
	void Close( CloseCallback cb ) override
	{
		file_->locked = false;
		std::move(cb).Run();
	}

private:
	std::shared_ptr<Node>	file_;
};

class FileHandle final : public bm::FileSystemAccessFileHandle {
public:
	explicit FileHandle( std::shared_ptr<Node> n ) : node_(std::move(n)) {}

	void GetPermissionStatus( bm::FileSystemAccessPermissionMode, GetPermissionStatusCallback cb ) override
	{
		std::move(cb).Run(bm::PermissionStatus::GRANTED);
	}
	void RequestPermission( bm::FileSystemAccessPermissionMode, RequestPermissionCallback cb ) override
	{
		std::move(cb).Run(Ok(), bm::PermissionStatus::GRANTED);
	}
	void AsBlob( AsBlobCallback cb ) override
	{
		if ( !InTree(node_.get()) ) {
			std::move(cb).Run(NotFound(), base::File::Info(), nullptr);
			return;
		}
		auto data = std::make_unique<blink::BlobData>();
		data->AppendBytes(base::as_byte_span(node_->bytes));
		base::File::Info info;
		info.size = (int64_t)node_->bytes.size();
		info.last_modified = node_->time;
		std::move(cb).Run(Ok(), info, blink::BlobDataHandle::Create(std::move(data), node_->bytes.size()));
	}
	void CreateFileWriter( bool keep, bool, bm::FileSystemAccessWritableFileStreamLockMode,
			       CreateFileWriterCallback cb ) override
	{
		if ( !InTree(node_.get()) ) {
			std::move(cb).Run(NotFound(), mojo::NullRemote());
			return;
		}
		if ( node_->locked ) {
			std::move(cb).Run(Error(bm::FileSystemAccessStatus::kNoModificationAllowedError,
						"An access handle has the file."), mojo::NullRemote());
			return;
		}
		mojo::PendingRemote<bm::FileSystemAccessFileWriter> remote;
		mojo::MakeSelfOwnedReceiver(std::make_unique<Writer>(node_, keep), remote.InitWithNewPipeAndPassReceiver());
		std::move(cb).Run(Ok(), std::move(remote));
	}
	void Rename( const blink::String &name, RenameCallback cb ) override
	{
		std::move(cb).Run(node_->parent != nullptr ? MoveTo(node_, node_->parent, name.Utf8()) : NotFound());
	}
	void Move( mojo::PendingRemote<bm::FileSystemAccessTransferToken> to, const blink::String &name, MoveCallback cb ) override
	{
		NodeOf(std::move(to), base::BindOnce([]( std::shared_ptr<Node> n, std::string name, MoveCallback cb,
							 std::shared_ptr<Node> to ) {
			if ( !to || !to->dir ) {
				std::move(cb).Run(NotFound());
				return;
			}
			std::move(cb).Run(MoveTo(n, to.get(), name));
		}, node_, name.Utf8(), std::move(cb)));
	}
	void Remove( RemoveCallback cb ) override
	{
		if ( !InTree(node_.get()) ) {
			std::move(cb).Run(NotFound());
			return;
		}
		if ( Busy(node_.get()) ) {
			std::move(cb).Run(Error(bm::FileSystemAccessStatus::kNoModificationAllowedError, "The file is in use."));
			return;
		}
		Detach(node_.get());
		Changed();
		std::move(cb).Run(Ok());
	}
	void OpenAccessHandle( bm::FileSystemAccessAccessHandleLockMode, OpenAccessHandleCallback cb ) override
	{
		if ( !InTree(node_.get()) ) {
			std::move(cb).Run(NotFound(), nullptr, mojo::NullRemote());
			return;
		}
		if ( node_->locked || node_->writers > 0 ) {
			std::move(cb).Run(Error(bm::FileSystemAccessStatus::kNoModificationAllowedError,
						"The file is in use."), nullptr, mojo::NullRemote());
			return;
		}
		mojo::PendingRemote<bm::FileSystemAccessFileDelegateHost> delegate;
		mojo::MakeSelfOwnedReceiver(std::make_unique<Delegate>(node_), delegate.InitWithNewPipeAndPassReceiver());
		mojo::PendingRemote<bm::FileSystemAccessAccessHandleHost> host;
		mojo::MakeSelfOwnedReceiver(std::make_unique<AccessHost>(node_), host.InitWithNewPipeAndPassReceiver());
		std::move(cb).Run(Ok(), bm::FileSystemAccessAccessHandleFile::NewIncognitoFileDelegate(std::move(delegate)),
				  std::move(host));
	}
	void IsSameEntry( mojo::PendingRemote<bm::FileSystemAccessTransferToken> other, IsSameEntryCallback cb ) override
	{
		NodeOf(std::move(other), base::BindOnce([]( std::shared_ptr<Node> n, IsSameEntryCallback cb,
							    std::shared_ptr<Node> o ) {
			std::move(cb).Run(Ok(), o == n);
		}, node_, std::move(cb)));
	}
	void Transfer( mojo::PendingReceiver<bm::FileSystemAccessTransferToken> r ) override
	{
		mojo::MakeSelfOwnedReceiver(std::make_unique<Token>(node_->id), std::move(r));
	}
	void GetUniqueId( GetUniqueIdCallback cb ) override
	{
		std::move(cb).Run(Ok(), blink::String::FromUtf8(node_->id.ToString()));
	}
	void GetCloudIdentifiers( GetCloudIdentifiersCallback cb ) override
	{
		std::move(cb).Run(Ok(), blink::Vector<bm::FileSystemAccessCloudIdentifierPtr>());
	}

private:
	std::shared_ptr<Node>	node_;
};

class DirHandle final : public bm::FileSystemAccessDirectoryHandle {
public:
	explicit DirHandle( std::shared_ptr<Node> n ) : node_(std::move(n)) {}

	void GetPermissionStatus( bm::FileSystemAccessPermissionMode, GetPermissionStatusCallback cb ) override
	{
		std::move(cb).Run(bm::PermissionStatus::GRANTED);
	}
	void RequestPermission( bm::FileSystemAccessPermissionMode, RequestPermissionCallback cb ) override
	{
		std::move(cb).Run(Ok(), bm::PermissionStatus::GRANTED);
	}
	void GetFile( const blink::String &basename, bool create, GetFileCallback cb ) override
	{
		std::shared_ptr<Node> n;
		bm::FileSystemAccessErrorPtr e = Entry(basename.Utf8(), create, false, &n);
		std::move(cb).Run(std::move(e), n ? FileRemote(n) : mojo::NullRemote());
	}
	void GetDirectory( const blink::String &basename, bool create, GetDirectoryCallback cb ) override
	{
		std::shared_ptr<Node> n;
		bm::FileSystemAccessErrorPtr e = Entry(basename.Utf8(), create, true, &n);
		std::move(cb).Run(std::move(e), n ? DirRemote(n) : mojo::NullRemote());
	}
	void GetEntries( mojo::PendingRemote<bm::FileSystemAccessDirectoryEntriesListener> l ) override
	{
		mojo::Remote<bm::FileSystemAccessDirectoryEntriesListener> listener(std::move(l));
		blink::Vector<bm::FileSystemAccessEntryPtr> out;
		if ( !InTree(node_.get()) ) {
			listener->DidReadDirectory(NotFound(), std::move(out), false);
			return;
		}
		for ( auto &kv : node_->kids ) {
			bm::FileSystemAccessHandlePtr h = kv.second->dir ? bm::FileSystemAccessHandle::NewDirectory(DirRemote(kv.second))
									 : bm::FileSystemAccessHandle::NewFile(FileRemote(kv.second));
			out.push_back(bm::FileSystemAccessEntry::New(std::move(h), blink::String::FromUtf8(kv.first)));
		}
		listener->DidReadDirectory(Ok(), std::move(out), false);
	}
	void Rename( const blink::String &name, RenameCallback cb ) override
	{
		std::move(cb).Run(node_->parent != nullptr ? MoveTo(node_, node_->parent, name.Utf8())
							   : Error(bm::FileSystemAccessStatus::kInvalidModificationError,
								   "The root is not renamed."));
	}
	void Move( mojo::PendingRemote<bm::FileSystemAccessTransferToken> to, const blink::String &name, MoveCallback cb ) override
	{
		NodeOf(std::move(to), base::BindOnce([]( std::shared_ptr<Node> n, std::string name, MoveCallback cb,
							 std::shared_ptr<Node> to ) {
			if ( !to || !to->dir ) {
				std::move(cb).Run(NotFound());
				return;
			}
			std::move(cb).Run(MoveTo(n, to.get(), name));
		}, node_, name.Utf8(), std::move(cb)));
	}
	void Remove( bool recurse, RemoveCallback cb ) override
	{
		if ( node_->root ) {
			/* the root stays; what is in it goes */
			std::move(cb).Run(Clear(node_.get(), recurse));
			return;
		}
		std::move(cb).Run(Take(node_, recurse));
	}
	void RemoveEntry( const blink::String &basename, bool recurse, RemoveEntryCallback cb ) override
	{
		if ( !InTree(node_.get()) ) {
			std::move(cb).Run(NotFound());
			return;
		}
		auto it = node_->kids.find(basename.Utf8());
		if ( it == node_->kids.end() ) {
			std::move(cb).Run(NotFound());
			return;
		}
		std::move(cb).Run(Take(it->second, recurse));
	}
	void Resolve( mojo::PendingRemote<bm::FileSystemAccessTransferToken> child, ResolveCallback cb ) override
	{
		NodeOf(std::move(child), base::BindOnce([]( std::shared_ptr<Node> n, ResolveCallback cb, std::shared_ptr<Node> c ) {
			blink::Vector<blink::String> path;
			for ( Node *a = c.get(); a != nullptr; a = a->parent ) {
				if ( a == n.get() ) {
					path.Reverse();
					std::move(cb).Run(Ok(), std::move(path));
					return;
				}
				path.push_back(blink::String::FromUtf8(a->name));
			}
			std::move(cb).Run(Ok(), std::nullopt);
		}, node_, std::move(cb)));
	}
	void Transfer( mojo::PendingReceiver<bm::FileSystemAccessTransferToken> r ) override
	{
		mojo::MakeSelfOwnedReceiver(std::make_unique<Token>(node_->id), std::move(r));
	}
	void GetUniqueId( GetUniqueIdCallback cb ) override
	{
		std::move(cb).Run(Ok(), blink::String::FromUtf8(node_->id.ToString()));
	}
	void GetCloudIdentifiers( GetCloudIdentifiersCallback cb ) override
	{
		std::move(cb).Run(Ok(), blink::Vector<bm::FileSystemAccessCloudIdentifierPtr>());
	}

private:
	/* An entry found, or made when 'create' says so */
	bm::FileSystemAccessErrorPtr Entry( const std::string &name, bool create, bool dir, std::shared_ptr<Node> *out )
	{
		if ( !InTree(node_.get()) ) return NotFound();
		if ( !NameOk(name) ) return Error(bm::FileSystemAccessStatus::kInvalidArgument, "That is not a name.");
		auto it = node_->kids.find(name);
		if ( it != node_->kids.end() ) {
			if ( it->second->dir != dir ) {
				return bm::FileSystemAccessError::New(bm::FileSystemAccessStatus::kFileError,
								      dir ? base::File::FILE_ERROR_NOT_A_DIRECTORY
									  : base::File::FILE_ERROR_NOT_A_FILE,
								      "The entry is of the other kind.");
			}
			*out = it->second;
			return Ok();
		}
		if ( !create ) return NotFound();
		*out = NewNode(name, dir, node_.get());
		node_->kids[name] = *out;
		Changed();
		return Ok();
	}

	/* A directory emptied (the root's remove) */
	static bm::FileSystemAccessErrorPtr Clear( Node *n, bool recurse )
	{
		if ( !n->kids.empty() && !recurse ) {
			return Error(bm::FileSystemAccessStatus::kInvalidModificationError, "The directory is not empty.");
		}
		if ( Busy(n) ) return Error(bm::FileSystemAccessStatus::kNoModificationAllowedError, "An entry is in use.");
		std::vector<std::shared_ptr<Node>> kids;
		for ( auto &kv : n->kids ) kids.push_back(kv.second);
		for ( auto &k : kids ) Detach(k.get());
		Changed();
		return Ok();
	}

	/* An entry taken out of the tree */
	static bm::FileSystemAccessErrorPtr Take( const std::shared_ptr<Node> &n, bool recurse )
	{
		if ( !InTree(n.get()) ) return NotFound();
		if ( n->dir && !n->kids.empty() && !recurse ) {
			return Error(bm::FileSystemAccessStatus::kInvalidModificationError, "The directory is not empty.");
		}
		if ( Busy(n.get()) ) return Error(bm::FileSystemAccessStatus::kNoModificationAllowedError, "The entry is in use.");
		Detach(n.get());
		Changed();
		return Ok();
	}

	std::shared_ptr<Node>	node_;
};

mojo::PendingRemote<bm::FileSystemAccessFileHandle> FileRemote( std::shared_ptr<Node> n )
{
	mojo::PendingRemote<bm::FileSystemAccessFileHandle> remote;
	mojo::MakeSelfOwnedReceiver(std::make_unique<FileHandle>(std::move(n)), remote.InitWithNewPipeAndPassReceiver());
	return remote;
}

mojo::PendingRemote<bm::FileSystemAccessDirectoryHandle> DirRemote( std::shared_ptr<Node> n )
{
	mojo::PendingRemote<bm::FileSystemAccessDirectoryHandle> remote;
	mojo::MakeSelfOwnedReceiver(std::make_unique<DirHandle>(std::move(n)), remote.InitWithNewPipeAndPassReceiver());
	return remote;
}

/* ---------------------------------------------------------------- kept between runs */

/*
 * The record: "BRF1", the count of origins, each origin's key and its
 * root's entries; an entry is its name, a byte (1 a directory), and a
 * directory's count of entries and the entries, or a file's time
 * (microseconds since 1601) and bytes. Strings and byte runs have a
 * 32-bit length before them; numbers are little-endian.
 */
int		g_store = -1;
std::mutex	g_keep_lock;
std::string	g_kept;
bool		g_keep_due;

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

void PutKids( std::string &o, const Node &n )
{
	PutU32(o, (uint32_t)n.kids.size());
	for ( const auto &kv : n.kids ) {
		const Node &k = *kv.second;
		PutStr(o, k.name);
		o.push_back(k.dir ? 1 : 0);
		if ( k.dir ) {
			PutKids(o, k);
		} else {
			PutU64(o, (uint64_t)k.time.ToDeltaSinceWindowsEpoch().InMicroseconds());
			PutStr(o, k.bytes);
		}
	}
}

void MakeRecord()
{
	std::string o = "BRF1";
	PutU32(o, (uint32_t)g_roots.size());
	for ( const auto &kv : g_roots ) {
		PutStr(o, kv.first);
		PutKids(o, *kv.second);
	}
	{
		std::lock_guard<std::mutex> hold(g_keep_lock);
		g_kept = std::move(o);
		g_keep_due = false;
	}
	if ( g_store >= 0 ) br_store_dirty(g_store);
}

/* A chosen folder's entries, below its root, as files are taken in */
void Entries( const Node &n, const std::string &at, std::vector<BrTreeEntry> *out, std::string *sum )
{
	for ( const auto &kv : n.kids ) {
		const Node &k = *kv.second;
		BrTreeEntry e;
		e.path = at.empty() ? k.name : at + "/" + k.name;
		e.dir = k.dir;
		*sum += e.path + ( k.dir ? "/" : "" ) + '\n';
		if ( k.dir ) {
			out->push_back(std::move(e));
			Entries(k, at.empty() ? k.name : at + "/" + k.name, out, sum);
		} else {
			e.bytes.assign(k.bytes.begin(), k.bytes.end());
			*sum += k.bytes;
			out->push_back(std::move(e));
		}
	}
}

/* The chosen folders the page changed, taken in again as new real objects */
void SavePicked()
{
	for ( Picked &p : g_picked ) {
		std::vector<BrTreeEntry> entries;
		std::string sum;
		Entries(*p.root, std::string(), &entries, &sum);
		if ( sum == p.saved ) continue;
		p.saved = sum;
		std::string name = p.name;
		std::thread([name, entries]() {
			unsigned char id[16];
			(void)br_obj_save_tree(name, entries, id);
		}).detach();
	}
}

void Changed()
{
	{
		std::lock_guard<std::mutex> hold(g_keep_lock);
		if ( g_keep_due ) return;
		g_keep_due = true;
	}
	br_net_post_delayed(base::BindOnce([]() {
		br_on_service_thread(base::BindOnce([]() {
			MakeRecord();
			SavePicked();
		}));
	}), 2000);
}

int WriteRecord( void *, unsigned char **data, int *len )
{
	std::lock_guard<std::mutex> hold(g_keep_lock);
	*data = (unsigned char *)malloc(g_kept.size() + 1);
	if ( *data == nullptr ) return 1;
	memcpy(*data, g_kept.data(), g_kept.size());
	*len = (int)g_kept.size();
	return 0;
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
	uint8_t U8()
	{
		if ( at + 1 > s.size() ) {
			bad = true;
			return 0;
		}
		return (uint8_t)s[at++];
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
};

void ReadKids( Reader &rd, Node *n, int depth )
{
	uint32_t count = rd.U32();
	for ( uint32_t i = 0; i < count && !rd.bad && depth < 64; i++ ) {
		std::string name = rd.Str();
		bool dir = rd.U8() != 0;
		std::shared_ptr<Node> k = NewNode(name, dir, n);
		if ( dir ) {
			ReadKids(rd, k.get(), depth + 1);
		} else {
			k->time = base::Time::FromDeltaSinceWindowsEpoch(base::Microseconds((int64_t)rd.U64()));
			k->bytes = rd.Str();
		}
		if ( rd.bad ) break;
		n->kids[name] = k;
	}
}

void Restore( std::string rec )
{
	Reader rd{ rec };
	if ( rec.compare(0, 4, "BRF1") != 0 ) return;
	rd.at = 4;
	uint32_t n = rd.U32();
	for ( uint32_t i = 0; i < n && !rd.bad; i++ ) {
		std::string origin = rd.Str();
		std::shared_ptr<Node> root = NewNode(std::string(), true, nullptr);
		root->root = true;
		ReadKids(rd, root.get(), 0);
		if ( !rd.bad ) g_roots[origin] = root;
	}
	{
		std::lock_guard<std::mutex> hold(g_keep_lock);
		g_kept = std::move(rec);
	}
	printf("browser: origin private file systems: %zu read back%s\n", g_roots.size(), rd.bad ? " (the rest damaged)" : "");
}

}  // namespace

/* The trees of the origins a clearing takes gone: a handle still held finds its node out of the tree */
void br_opfs_clear( const BrClear &c )
{
	bool any = false;
	for ( auto it = g_roots.begin(); it != g_roots.end(); ) {
		if ( br_clear_origin_hit(c, it->first) ) {
			it = g_roots.erase(it);
			any = true;
		} else {
			++it;
		}
	}
	if ( any ) Changed();
}

void br_opfs_sites( std::set<std::string> *out )
{
	for ( const auto &kv : g_roots ) out->insert(br_site_of(kv.first));
}

/* The root of an origin's private file system, made when it is first asked for (on the services' thread) */
void br_opfs_root( const std::string &origin, mojo::PendingReceiver<bm::FileSystemAccessDirectoryHandle> r )
{
	br_site_touch(origin);
	std::shared_ptr<Node> &root = g_roots[origin];
	if ( !root ) {
		root = NewNode(std::string(), true, nullptr);
		root->root = true;
	}
	mojo::MakeSelfOwnedReceiver(std::make_unique<DirHandle>(root), std::move(r));
}

/* A handle sent to another context, by the token it went as (on the services' thread) */
void br_opfs_file_from_token( mojo::PendingRemote<bm::FileSystemAccessTransferToken> t,
			      mojo::PendingReceiver<bm::FileSystemAccessFileHandle> r )
{
	NodeOf(std::move(t), base::BindOnce([]( mojo::PendingReceiver<bm::FileSystemAccessFileHandle> r,
						std::shared_ptr<Node> n ) {
		if ( n && !n->dir ) mojo::MakeSelfOwnedReceiver(std::make_unique<FileHandle>(n), std::move(r));
	}, std::move(r)));
}

void br_opfs_dir_from_token( mojo::PendingRemote<bm::FileSystemAccessTransferToken> t,
			     mojo::PendingReceiver<bm::FileSystemAccessDirectoryHandle> r )
{
	NodeOf(std::move(t), base::BindOnce([]( mojo::PendingReceiver<bm::FileSystemAccessDirectoryHandle> r,
						std::shared_ptr<Node> n ) {
		if ( n && n->dir ) mojo::MakeSelfOwnedReceiver(std::make_unique<DirHandle>(n), std::move(r));
	}, std::move(r)));
}

/*
 * A folder the user chose, as a directory a page holds: its entries made
 * a tree (on the services' thread)
 */
void br_opfs_picked( const std::string &name, const std::vector<BrTreeEntry> &entries,
		     mojo::PendingReceiver<bm::FileSystemAccessDirectoryHandle> r )
{
	std::shared_ptr<Node> root = NewNode(name, true, nullptr);
	root->root = true;
	for ( const BrTreeEntry &e : entries ) {
		/* each directory on the way made, then the entry in the last */
		Node *at = root.get();
		size_t from = 0;
		while ( true ) {
			size_t slash = e.path.find('/', from);
			std::string part = e.path.substr(from, slash == std::string::npos ? std::string::npos : slash - from);
			if ( part.empty() ) break;
			bool last = ( slash == std::string::npos );
			auto it = at->kids.find(part);
			if ( it == at->kids.end() ) {
				std::shared_ptr<Node> k = NewNode(part, last ? e.dir : true, at);
				if ( last && !e.dir ) k->bytes.assign(e.bytes.begin(), e.bytes.end());
				at->kids[part] = k;
				at = k.get();
			} else {
				at = it->second.get();
			}
			if ( last ) break;
			from = slash + 1;
		}
	}
	Picked p;
	p.root = root;
	p.name = name;
	std::vector<BrTreeEntry> ignored;
	Entries(*root, std::string(), &ignored, &p.saved);
	g_picked.push_back(std::move(p));
	mojo::MakeSelfOwnedReceiver(std::make_unique<DirHandle>(root), std::move(r));
}

/* A deleted bucket's file system, gone (buckets_tessronos.cc) */
void br_opfs_delete_key( const std::string &key )
{
	br_on_service_thread(base::BindOnce([]( std::string key ) {
		auto it = g_roots.find(key);
		if ( it == g_roots.end() ) return;
		std::vector<std::shared_ptr<Node>> kids;
		for ( auto &kv : it->second->kids ) kids.push_back(kv.second);
		for ( auto &k : kids ) Detach(k.get());
		g_roots.erase(it);
		Changed();
	}, key));
}

/* The trees of earlier runs read back, once, as the engine starts */
void br_opfs_init()
{
	g_store = br_store_register("opfs", &WriteRecord, nullptr, 500);
	unsigned char *data = nullptr;
	int len = 0;
	if ( br_store_load("opfs", &data, &len) == 0 && data != nullptr && len > 0 ) {
		br_on_service_thread(base::BindOnce(&Restore, std::string((char *)data, (size_t)len)));
	}
	free(data);
}
