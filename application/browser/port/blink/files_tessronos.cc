/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	files_tessronos.cc
 *	ブラウザ: the files a page is given (design 14.7.4).
 *
 *	A file chosen for <input type="file"> is a real object given out as
 *	a file (port/net/br_objfile.cc). Blink knows a chosen file by its
 *	path: it asks the browser what the file is (FileUtilitiesHost, its
 *	size and time) and makes a Blob of the file's bytes from the path
 *	(a file element, which blob_tessronos.cc reads from here). The bytes
 *	are kept here under a path of their own, /obj/<UUID>/<name>, while
 *	the browser runs.
 */

#include <ctype.h>
#include <stdio.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/time/time.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "third_party/blink/public/mojom/file/file_utilities.mojom-blink.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"

/* The services' thread (blob_tessronos.cc) */
void br_on_service_thread( base::OnceClosure task );

namespace {

namespace bm = blink::mojom::blink;

struct File {
	std::vector<uint8_t>	bytes;
	base::Time		time;
};

std::mutex			g_lock;
std::map<std::string, File>	g_files;

class FileUtilities final : public bm::FileUtilitiesHost {
public:
	void Bind( mojo::PendingReceiver<bm::FileUtilitiesHost> r ) { receivers_.Add(this, std::move(r)); }

	void GetFileInfo( const base::FilePath &path, GetFileInfoCallback cb ) override
	{
		std::lock_guard<std::mutex> hold(g_lock);
		auto it = g_files.find(path.value());
		if ( it == g_files.end() ) {
			std::move(cb).Run(std::nullopt);
			return;
		}
		base::File::Info info;
		info.size = (int64_t)it->second.bytes.size();
		info.last_modified = it->second.time;
		info.last_accessed = it->second.time;
		info.creation_time = it->second.time;
		info.is_directory = false;
		std::move(cb).Run(info);
	}

private:
	mojo::ReceiverSet<bm::FileUtilitiesHost>	receivers_;
};

FileUtilities *g_utilities;

void Bind( mojo::PendingReceiver<bm::FileUtilitiesHost> r )
{
	br_on_service_thread(base::BindOnce([]( mojo::PendingReceiver<bm::FileUtilitiesHost> r ) {
		if ( g_utilities == nullptr ) g_utilities = new FileUtilities();
		g_utilities->Bind(std::move(r));
	}, std::move(r)));
}

}  // namespace

/* A file's bytes kept under its path; the path, as Blink is given it */
std::string br_file_keep( const std::string &uuid, const std::string &name, std::vector<uint8_t> bytes )
{
	/*
	 * Letters only of ASCII in the path: a path goes through the
	 * system's multibyte conversion where Chromium turns it to text and
	 * back on Linux, which does not keep other letters. The name the page
	 * sees is the file's display name; the path keeps its extension, which
	 * tells the file's type.
	 */
	std::string ext;
	size_t dot = name.rfind('.');
	if ( dot != std::string::npos && name.size() - dot <= 10 ) {
		ext = name.substr(dot);
		for ( char c : ext.substr(1) ) {
			if ( !isalnum((unsigned char)c) ) {
				ext.clear();
				break;
			}
		}
	}
	std::string path = "/obj/" + uuid + "/file" + ext;
	std::lock_guard<std::mutex> hold(g_lock);
	File &f = g_files[path];
	f.bytes.swap(bytes);
	f.time = base::Time::Now();
	return path;
}

/* Bytes of a kept file, from offset, length of them (~0 to its end); false when there is no such file */
bool br_file_bytes( const std::string &path, uint64_t offset, uint64_t length, std::vector<uint8_t> *out )
{
	std::lock_guard<std::mutex> hold(g_lock);
	auto it = g_files.find(path);
	if ( it == g_files.end() ) return false;
	const std::vector<uint8_t> &b = it->second.bytes;
	if ( offset > b.size() ) offset = b.size();
	uint64_t left = b.size() - offset;
	if ( length > left ) length = left;
	out->assign(b.begin() + (long)offset, b.begin() + (long)( offset + length ));
	return true;
}

/* A worker's broker's receiver: true when it is FileUtilitiesHost */
bool br_files_bind( mojo::GenericPendingReceiver &r )
{
	if ( auto f = r.As<bm::FileUtilitiesHost>() ) {
		Bind(std::move(f));
		return true;
	}
	return false;
}

/* The binder of the frames' broker, once when the engine is up */
void br_files_init()
{
	blink::GetEmptyBrowserInterfaceBroker().SetBinderForTesting(
		bm::FileUtilitiesHost::Name_, base::BindRepeating([]( mojo::ScopedMessagePipeHandle h ) {
			Bind(mojo::PendingReceiver<bm::FileUtilitiesHost>(std::move(h)));
		}));
}
