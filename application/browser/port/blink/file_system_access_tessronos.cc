/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	file_system_access_tessronos.cc
 *	ブラウザ: the File System Access API's pickers and handles
 *	(showOpenFilePicker, showSaveFilePicker; design 14.7.4).
 *
 *	A file is a real object, as for <input type="file">:
 *
 *	  - showOpenFilePicker(): real objects chosen on the browser's panel
 *	    (panel 6), each given to the page as the one file it is given out
 *	    as. The page reads it (getFile()) and may write it: what it
 *	    writes is saved, when the writable stream is closed, as a new
 *	    real object in ブラウザの保存箱 (the object it came from is not
 *	    written over).
 *	  - showSaveFilePicker(): the name asked on a panel (prompt's), and a
 *	    new empty file; what the page writes is saved likewise.
 *	  - the origin's private file system (navigator.storage.getDirectory)
 *	    is opfs_tessronos.cc's;
 *	  - showDirectoryPicker(): a real object chosen on panel 6 that is
 *	    given out as a directory (a figure of links, as a cabinet), whose
 *	    files and folders the page reads and writes in memory; what it
 *	    changes is taken in as a new real object (opfs_tessronos.cc).
 *
 *	The handles and writers are on the services' thread; the panels are
 *	asked from a thread of their own.
 */

#include <stdio.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "base/files/file.h"
#include "base/functional/bind.h"
#include "base/time/time.h"
#include "base/unguessable_token.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_data_transfer_token.mojom-blink.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "mojo/public/cpp/system/data_pipe_drainer.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_access_handle_host.mojom-blink.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_cloud_identifier.mojom-blink.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_data_transfer_token.mojom-blink.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_directory_handle.mojom-blink.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_observer_host.mojom-blink.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_transfer_token.mojom-blink.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_error.mojom-blink.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_file_handle.mojom-blink.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_file_writer.mojom-blink.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_manager.mojom-blink.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"
#include "third_party/blink/renderer/platform/blob/blob_data.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

#include "../../br.h"
#include "../net/br_objfile.h"

/* The services' thread (blob_tessronos.cc) */
void br_on_service_thread( base::OnceClosure task );
/* The origin private file system (opfs_tessronos.cc) */
void br_opfs_root( const std::string &origin,
		   mojo::PendingReceiver<blink::mojom::blink::FileSystemAccessDirectoryHandle> r );
void br_opfs_file_from_token( mojo::PendingRemote<blink::mojom::blink::FileSystemAccessTransferToken> t,
			      mojo::PendingReceiver<blink::mojom::blink::FileSystemAccessFileHandle> r );
void br_opfs_dir_from_token( mojo::PendingRemote<blink::mojom::blink::FileSystemAccessTransferToken> t,
			     mojo::PendingReceiver<blink::mojom::blink::FileSystemAccessDirectoryHandle> r );
void br_opfs_init();
void br_opfs_picked( const std::string &name, const std::vector<BrTreeEntry> &entries,
		     mojo::PendingReceiver<blink::mojom::blink::FileSystemAccessDirectoryHandle> r );
/* The origin of the page shown (br_blink.cc) */
std::string br_page_origin();
/* A file's bytes (blob_tessronos.cc) */
bool br_file_bytes( const std::string &path, uint64_t offset, uint64_t length, std::vector<uint8_t> *out );

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

/* A file as the page holds it: its name and bytes, while the browser runs */
struct File {
	std::string	name;
	std::string	bytes;
	base::Time	time;
};

/* A dropped file's path and name, by its token's id (below) */
std::optional<std::pair<std::string, std::string>> DroppedFile( const base::UnguessableToken &id );

/* The bytes a page wrote, saved as a real object in ブラウザの保存箱, off the services' thread */
void Save( const std::string &name, const std::string &bytes )
{
	std::thread([name, bytes]() {
		unsigned char id[16];
		std::vector<uint8_t> b(bytes.begin(), bytes.end());
		(void)br_obj_save(name, b, id);
	}).detach();
}

/* What a page writes into a file, until it closes the stream */
class Writer final : public bm::FileSystemAccessFileWriter, public mojo::DataPipeDrainer::Client {
public:
	Writer( std::shared_ptr<File> f, bool keep ) : file_(std::move(f))
	{
		if ( keep ) data_ = file_->bytes;
	}

	void Write( uint64_t offset, mojo::ScopedDataPipeConsumerHandle stream, WriteCallback cb ) override
	{
		if ( drainer_ ) {
			std::move(cb).Run(Error(bm::FileSystemAccessStatus::kInvalidState, "a write is under way"), 0);
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
		if ( at_ > data_.size() ) data_.resize((size_t)at_);
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
		file_->bytes = data_;
		file_->time = base::Time::Now();
		printf("browser: file system access: %s written, %zu bytes\n", file_->name.c_str(), data_.size());
		Save(file_->name, data_);
		std::move(cb).Run(Ok());
	}
	void Abort( AbortCallback cb ) override { std::move(cb).Run(Ok()); }

private:
	std::shared_ptr<File>			file_;
	std::string				data_, got_;
	uint64_t				at_ = 0;
	std::unique_ptr<mojo::DataPipeDrainer>	drainer_;
	WriteCallback				write_cb_;
};

class FileHandle final : public bm::FileSystemAccessFileHandle {
public:
	explicit FileHandle( std::shared_ptr<File> f ) : file_(std::move(f)), id_(base::UnguessableToken::Create()) {}

	static mojo::PendingRemote<bm::FileSystemAccessFileHandle> Make( std::shared_ptr<File> f )
	{
		mojo::PendingRemote<bm::FileSystemAccessFileHandle> remote;
		mojo::MakeSelfOwnedReceiver(std::make_unique<FileHandle>(std::move(f)), remote.InitWithNewPipeAndPassReceiver());
		return remote;
	}

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
		auto data = std::make_unique<blink::BlobData>();
		data->AppendBytes(base::as_byte_span(file_->bytes));
		scoped_refptr<blink::BlobDataHandle> blob = blink::BlobDataHandle::Create(std::move(data), file_->bytes.size());
		base::File::Info info;
		info.size = (int64_t)file_->bytes.size();
		info.last_modified = file_->time;
		std::move(cb).Run(Ok(), info, blob);
	}
	void CreateFileWriter( bool keep, bool, bm::FileSystemAccessWritableFileStreamLockMode,
			       CreateFileWriterCallback cb ) override
	{
		mojo::PendingRemote<bm::FileSystemAccessFileWriter> remote;
		mojo::MakeSelfOwnedReceiver(std::make_unique<Writer>(file_, keep), remote.InitWithNewPipeAndPassReceiver());
		std::move(cb).Run(Ok(), std::move(remote));
	}
	void Rename( const blink::String &name, RenameCallback cb ) override
	{
		file_->name = name.Utf8();
		std::move(cb).Run(Ok());
	}
	void Move( mojo::PendingRemote<bm::FileSystemAccessTransferToken>, const blink::String &, MoveCallback cb ) override
	{
		std::move(cb).Run(Error(bm::FileSystemAccessStatus::kNotSupportedError, "There are no folders to move into."));
	}
	void Remove( RemoveCallback cb ) override
	{
		std::move(cb).Run(Error(bm::FileSystemAccessStatus::kNoModificationAllowedError,
					"A real object is not removed by a page."));
	}
	void OpenAccessHandle( bm::FileSystemAccessAccessHandleLockMode, OpenAccessHandleCallback cb ) override
	{
		std::move(cb).Run(Error(bm::FileSystemAccessStatus::kNotSupportedError,
					"Access handles are only of the origin private file system."),
				  nullptr, mojo::NullRemote());
	}
	void IsSameEntry( mojo::PendingRemote<bm::FileSystemAccessTransferToken>, IsSameEntryCallback cb ) override
	{
		std::move(cb).Run(Ok(), false);
	}
	void Transfer( mojo::PendingReceiver<bm::FileSystemAccessTransferToken> ) override {}
	void GetUniqueId( GetUniqueIdCallback cb ) override
	{
		std::move(cb).Run(Ok(), blink::String::FromUtf8(id_.ToString()));
	}
	void GetCloudIdentifiers( GetCloudIdentifiersCallback cb ) override
	{
		std::move(cb).Run(Ok(), blink::Vector<bm::FileSystemAccessCloudIdentifierPtr>());
	}

private:
	std::shared_ptr<File>		file_;
	base::UnguessableToken		id_;
};

/* What a picker gave, back on the services' thread */
struct Picked {
	bool			ok = false;
	std::string		why;
	std::vector<File>	files;
};

class Manager final : public bm::FileSystemAccessManager {
public:
	void Bind( mojo::PendingReceiver<bm::FileSystemAccessManager> r ) { receivers_.Add(this, std::move(r)); }

	void GetSandboxedFileSystem( GetSandboxedFileSystemCallback cb ) override
	{
		mojo::PendingRemote<bm::FileSystemAccessDirectoryHandle> root;
		br_opfs_root(br_page_origin(), root.InitWithNewPipeAndPassReceiver());
		std::move(cb).Run(Ok(), std::move(root));
	}
	void GetSandboxedFileSystemForDevtools( const blink::Vector<blink::String> &,
						GetSandboxedFileSystemForDevtoolsCallback cb ) override
	{
		GetSandboxedFileSystem(std::move(cb));
	}

	void ChooseEntries( bm::FilePickerOptionsPtr options, ChooseEntriesCallback cb ) override
	{
		const bm::TypeSpecificFilePickerOptionsUnion &u = *options->type_specific_options;
		if ( u.is_directory_picker_options() ) {
			/* a real object that is a directory of files, from a thread of its own */
			std::thread([cb = std::move(cb)]() mutable {
				std::vector<BrObjLink> links;
				std::string name;
				std::vector<BrTreeEntry> entries;
				bool ok = br_choose_objects(false, &links) && !links.empty()
					  && br_obj_export_tree(links[0].id, &name, &entries);
				br_on_service_thread(base::BindOnce([]( bool ok, std::string name, std::vector<BrTreeEntry> entries,
									ChooseEntriesCallback cb ) {
					blink::Vector<bm::FileSystemAccessEntryPtr> out;
					if ( !ok ) {
						std::move(cb).Run(Error(bm::FileSystemAccessStatus::kOperationAborted,
									"No folder of files was chosen."), std::move(out));
						return;
					}
					printf("browser: file system access: the page is given the folder %s (%zu entries)\n",
					       name.c_str(), entries.size());
					mojo::PendingRemote<bm::FileSystemAccessDirectoryHandle> dir;
					br_opfs_picked(name, entries, dir.InitWithNewPipeAndPassReceiver());
					out.push_back(bm::FileSystemAccessEntry::New(bm::FileSystemAccessHandle::NewDirectory(std::move(dir)),
										    blink::String::FromUtf8(name)));
					std::move(cb).Run(Ok(), std::move(out));
				}, ok, name, std::move(entries), std::move(cb)));
			}).detach();
			return;
		}
		bool save = u.is_save_file_picker_options();
		bool multiple = !save && u.get_open_file_picker_options()->can_select_multiple_files;
		std::string suggested = save ? u.get_save_file_picker_options()->suggested_name.Utf8() : std::string();
		std::thread([save, multiple, suggested, cb = std::move(cb)]() mutable {
			Picked p;
			if ( save ) {
				std::string name;
				p.ok = br_ask_page(BR_ASK_PROMPT, "保存するファイルの名前", suggested.empty() ? "untitled.txt" : suggested,
						   &name) && !name.empty();
				if ( p.ok ) p.files.push_back(File{ name, std::string(), base::Time::Now() });
			} else {
				std::vector<BrObjLink> links;
				if ( br_choose_objects(multiple, &links) ) {
					for ( const BrObjLink &l : links ) {
						std::string name;
						std::vector<uint8_t> bytes;
						if ( !br_obj_export(l.id, &name, &bytes) ) continue;
						p.files.push_back(File{ name, std::string(bytes.begin(), bytes.end()), base::Time::Now() });
					}
				}
				p.ok = !p.files.empty();
			}
			br_on_service_thread(base::BindOnce(&Manager::Chosen, std::move(p), std::move(cb)));
		}).detach();
	}

	/* a handle another context sent: the origin private file system's (the others are not sent) */
	void GetFileHandleFromToken( mojo::PendingRemote<bm::FileSystemAccessTransferToken> t,
				     mojo::PendingReceiver<bm::FileSystemAccessFileHandle> r ) override
	{
		br_opfs_file_from_token(std::move(t), std::move(r));
	}
	void GetDirectoryHandleFromToken( mojo::PendingRemote<bm::FileSystemAccessTransferToken> t,
					  mojo::PendingReceiver<bm::FileSystemAccessDirectoryHandle> r ) override
	{
		br_opfs_dir_from_token(std::move(t), std::move(r));
	}
	/* a dropped file (its token's): a handle of its bytes, read now */
	void GetEntryFromDataTransferToken( mojo::PendingRemote<bm::FileSystemAccessDataTransferToken> t,
					    GetEntryFromDataTransferTokenCallback cb ) override
	{
		auto token = std::make_shared<mojo::Remote<bm::FileSystemAccessDataTransferToken>>(std::move(t));
		bm::FileSystemAccessDataTransferToken *p = token->get();
		p->GetInternalId(base::BindOnce([]( std::shared_ptr<mojo::Remote<bm::FileSystemAccessDataTransferToken>>,
						    GetEntryFromDataTransferTokenCallback cb, const base::UnguessableToken &id ) {
			auto d = DroppedFile(id);
			if ( !d ) {
				std::move(cb).Run(Error(bm::FileSystemAccessStatus::kOperationFailed, "No entry is carried."), nullptr);
				return;
			}
			std::vector<uint8_t> bytes;
			(void)br_file_bytes(d->first, 0, UINT64_MAX, &bytes);
			auto f = std::make_shared<File>(File{ d->second, std::string(bytes.begin(), bytes.end()), base::Time::Now() });
			std::move(cb).Run(Ok(), bm::FileSystemAccessEntry::New(bm::FileSystemAccessHandle::NewFile(FileHandle::Make(f)),
									      blink::String::FromUtf8(d->second)));
		}, token, std::move(cb)));
	}
	void BindObserverHost( mojo::PendingReceiver<bm::FileSystemAccessObserverHost> ) override {}

private:
	static void Chosen( Picked p, ChooseEntriesCallback cb )
	{
		blink::Vector<bm::FileSystemAccessEntryPtr> entries;
		if ( !p.ok ) {
			std::move(cb).Run(Error(bm::FileSystemAccessStatus::kOperationAborted, "The user chose nothing."),
					  std::move(entries));
			return;
		}
		for ( File &f : p.files ) {
			printf("browser: file system access: the page is given %s (%zu bytes)\n", f.name.c_str(), f.bytes.size());
			std::string name = f.name;
			entries.push_back(bm::FileSystemAccessEntry::New(
				bm::FileSystemAccessHandle::NewFile(FileHandle::Make(std::make_shared<File>(std::move(f)))),
				blink::String::FromUtf8(name)));
		}
		std::move(cb).Run(Ok(), std::move(entries));
	}

	mojo::ReceiverSet<bm::FileSystemAccessManager>	receivers_;
};

Manager	*g_manager;

/* ---------------------------------------------------------------- the files of a drop */

/* id -> the file a drop carried (its path, its name) */
std::map<base::UnguessableToken, std::pair<std::string, std::string>> &Dropped()
{
	static auto *m = new std::map<base::UnguessableToken, std::pair<std::string, std::string>>();
	return *m;
}

class DropToken final : public bm::FileSystemAccessDataTransferToken {
public:
	explicit DropToken( base::UnguessableToken id ) : id_(id) {}

	void GetInternalId( GetInternalIdCallback cb ) override { std::move(cb).Run(id_); }
	void Clone( mojo::PendingReceiver<bm::FileSystemAccessDataTransferToken> r ) override
	{
		mojo::MakeSelfOwnedReceiver(std::make_unique<DropToken>(id_), std::move(r));
	}

private:
	base::UnguessableToken	id_;
};

}  // namespace

namespace {

std::optional<std::pair<std::string, std::string>> DroppedFile( const base::UnguessableToken &id )
{
	auto d = Dropped().find(id);
	if ( d == Dropped().end() ) return std::nullopt;
	return d->second;
}

}  // namespace

/* A dropped file's token (getAsFileSystemHandle): the file at path, by its name; from the page's thread */
mojo::PendingRemote<bm::FileSystemAccessDataTransferToken> br_fsa_drop_token( const std::string &path, const std::string &name )
{
	mojo::PendingRemote<bm::FileSystemAccessDataTransferToken> remote;
	base::UnguessableToken id = base::UnguessableToken::Create();
	br_on_service_thread(base::BindOnce([]( base::UnguessableToken id, std::string path, std::string name,
						mojo::PendingReceiver<bm::FileSystemAccessDataTransferToken> r ) {
		Dropped()[id] = {path, name};
		mojo::MakeSelfOwnedReceiver(std::make_unique<DropToken>(id), std::move(r));
	}, id, path, name, remote.InitWithNewPipeAndPassReceiver()));
	return remote;
}

/* A worker's broker's receiver: true when it is the File System Access manager */
bool br_file_system_access_bind( mojo::GenericPendingReceiver &r )
{
	if ( auto m = r.As<bm::FileSystemAccessManager>() ) {
		br_on_service_thread(base::BindOnce([]( mojo::PendingReceiver<bm::FileSystemAccessManager> r ) {
			if ( g_manager == nullptr ) g_manager = new Manager();
			g_manager->Bind(std::move(r));
		}, std::move(m)));
		return true;
	}
	return false;
}

/* The binder of the frames' broker, once when the engine is up; the private file systems read back */
void br_file_system_access_init()
{
	br_opfs_init();
	blink::GetEmptyBrowserInterfaceBroker().SetBinderForTesting(
		bm::FileSystemAccessManager::Name_, base::BindRepeating([]( mojo::ScopedMessagePipeHandle h ) {
			br_on_service_thread(base::BindOnce([]( mojo::PendingReceiver<bm::FileSystemAccessManager> r ) {
				if ( g_manager == nullptr ) g_manager = new Manager();
				g_manager->Bind(std::move(r));
			}, mojo::PendingReceiver<bm::FileSystemAccessManager>(std::move(h))));
		}));
}
