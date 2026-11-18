/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	blob_tessronos.cc
 *	ブラウザ: the store of Blobs and of their URLs (blob:), which a
 *	browser keeps in its own process, kept here in this one (design 14.5).
 *
 *	Blink makes a Blob by registering its parts with the BlobRegistry
 *	(bytes it holds, bytes it gives when asked, slices of other Blobs)
 *	and reads it back through the Blob interface (FileReader, text(),
 *	arrayBuffer(), stream(), a request's body). URL.createObjectURL()
 *	registers the Blob under a blob: URL with the BlobURLStore, and a load
 *	of that URL (an image, fetch(), XMLHttpRequest, a worker's script) is
 *	made through the URLLoaderFactory the store resolves the URL to.
 *
 *	All of it lives on a thread of its own ("blob"): Blink asks the
 *	registry with calls that wait for the answer (Register), from the
 *	page's thread and from the workers', so the answer must come from a
 *	thread none of them holds. A Blob's bytes are kept whole in memory,
 *	one copy for each Blob (a slice copies what it takes). The parts
 *	given later (the bytes Blink hands over when asked, a slice of a Blob
 *	still being made) are waited for before the Blob is read.
 *
 *	A window reaches the store through the frame's associated interfaces,
 *	whose receivers are on the page's thread: a store there hands each
 *	call on, in order, to the one on the blob thread. A worker reaches it
 *	through the broker it is given when it starts (worker_tessronos.cc),
 *	and the registry through the platform's broker.
 */

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/ref_counted.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "base/unguessable_token.h"
#include "base/uuid.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "mojo/public/cpp/bindings/generic_pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_associated_receiver.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_associated_receiver.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/data_pipe_drainer.h"
#include "mojo/public/cpp/system/data_pipe_producer.h"
#include "mojo/public/cpp/system/string_data_source.h"
#include "net/base/net_errors.h"
#include "net/http/http_byte_range.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_util.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/mojom/data_pipe_getter.mojom.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "third_party/blink/public/common/thread_safe_browser_interface_broker_proxy.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"
#include "third_party/blink/public/mojom/blob/blob.mojom-blink.h"
#include "third_party/blink/public/mojom/blob/blob_registry.mojom-blink.h"
#include "third_party/blink/public/mojom/blob/blob_url_store.mojom-blink.h"
#include "third_party/blink/public/mojom/blob/file_backed_blob_factory.mojom-blink.h"
#include "third_party/blink/public/mojom/blob/data_element.mojom-blink.h"
#include "third_party/blink/public/mojom/gpu/gpu.mojom-blink.h"
#include "third_party/blink/public/mojom/browser_interface_broker.mojom-blink.h"
#include "third_party/blink/public/mojom/indexeddb/indexeddb.mojom-blink.h"
#include "third_party/blink/renderer/platform/blob/blob_data.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

/* The other services a worker's broker gives (services_tessronos.cc) */
bool br_service_bind( mojo::GenericPendingReceiver &r );
void br_pdf_plugins_bind( mojo::ScopedMessagePipeHandle h );
/* IndexedDB's factory for an origin, bound on the page's thread (indexeddb_tessronos.cc) */
void br_idb_bind_for( const std::string &origin, mojo::ScopedMessagePipeHandle pipe );
void br_buckets_bind_for( const std::string &origin, mojo::ScopedMessagePipeHandle h );	/* buckets_tessronos.cc */

/* The bytes of a file a page was given (files_tessronos.cc) */
bool br_file_bytes( const std::string &path, uint64_t offset, uint64_t length, std::vector<uint8_t> *out );

namespace {

namespace bm = blink::mojom::blink;

/* The most one Blob may hold: more is refused (the Blob reads as failed) */
constexpr uint64_t kBlobMax = 512ull << 20;

/* ---------------------------------------------------------------- the thread */

scoped_refptr<base::SingleThreadTaskRunner> BlobRunner()
{
	static base::Thread *thread = [] {
		auto *t = new base::Thread("blob");
		base::Thread::Options options;
		options.stack_size = 512 * 1024;
		if ( !t->StartWithOptions(std::move(options)) ) printf("browser: blob: the thread did not start\n");
		return t;
	}();
	return thread->task_runner();
}

/* What runs on the blob thread, handed there from whichever thread asks */
void OnBlobThread( base::OnceClosure task )
{
	scoped_refptr<base::SingleThreadTaskRunner> r = BlobRunner();
	if ( r->BelongsToCurrentThread() ) {
		std::move(task).Run();
		return;
	}
	r->PostTask(FROM_HERE, std::move(task));
}

/* ---------------------------------------------------------------- a Blob's bytes */

class BlobEntry;

/* The Blobs alive by their UUID (the blob thread's) */
std::map<std::string, BlobEntry *> &Blobs()
{
	static auto *m = new std::map<std::string, BlobEntry *>();
	return *m;
}

class BlobEntry final : public base::RefCounted<BlobEntry> {
public:
	BlobEntry( std::string uuid, std::string type ) : uuid_(std::move(uuid)), type_(std::move(type))
	{
		Blobs()[uuid_] = this;
	}

	const std::string &uuid() const { return uuid_; }
	const std::string &type() const { return type_; }
	std::string &bytes() { return bytes_; }
	uint64_t size() const { return bytes_.size(); }
	bool broken() const { return broken_; }
	void Break() { broken_ = true; }
	bool ready() const { return pending_ == 0; }

	/* A part that comes later; the Blob is read when none is left */
	void PartComing() { pending_++; }
	void PartDone()
	{
		if ( pending_ == 0 || --pending_ != 0 ) return;
		std::vector<base::OnceClosure> w;
		w.swap(waiting_);
		for ( base::OnceClosure &c : w ) std::move(c).Run();
	}

	void WhenReady( base::OnceClosure c )
	{
		if ( ready() ) std::move(c).Run();
		else waiting_.push_back(std::move(c));
	}

private:
	friend class base::RefCounted<BlobEntry>;
	~BlobEntry()
	{
		auto it = Blobs().find(uuid_);
		if ( it != Blobs().end() && it->second == this ) Blobs().erase(it);
	}

	std::string			uuid_, type_;
	std::string			bytes_;
	int				pending_ = 0;
	bool				broken_ = false;
	std::vector<base::OnceClosure>	waiting_;
};

scoped_refptr<BlobEntry> FindBlob( const std::string &uuid )
{
	auto it = Blobs().find(uuid);
	return it != Blobs().end() ? scoped_refptr<BlobEntry>(it->second) : nullptr;
}

/*
 * Bytes [off, off + len) of a Blob written into a data pipe as the pipe
 * takes them; done is told how it ended. The Blob is kept until then, and
 * its bytes do not change once it is ready.
 */
void WriteRange( scoped_refptr<BlobEntry> e, uint64_t off, uint64_t len,
		 mojo::ScopedDataPipeProducerHandle pipe, base::OnceCallback<void(MojoResult)> done )
{
	if ( len == 0 ) {
		pipe.reset();
		std::move(done).Run(MOJO_RESULT_OK);
		return;
	}
	auto producer = std::make_unique<mojo::DataPipeProducer>(std::move(pipe));
	mojo::DataPipeProducer *p = producer.get();
	auto source = std::make_unique<mojo::StringDataSource>(
		base::span<const char>(e->bytes().data() + off, (size_t)len),
		mojo::StringDataSource::AsyncWritingMode::STRING_STAYS_VALID_UNTIL_COMPLETION);
	p->Write(std::move(source),
		 base::BindOnce([]( std::unique_ptr<mojo::DataPipeProducer>, scoped_refptr<BlobEntry>,
				    base::OnceCallback<void(MojoResult)> done, MojoResult r ) {
			 std::move(done).Run(r);
		 }, std::move(producer), std::move(e), std::move(done)));
}

/* ---------------------------------------------------------------- loading a Blob as a URL */

/* The loader's own interface: a Blob's load has no redirects to follow */
class LoaderImpl final : public network::mojom::URLLoader {
public:
	void FollowRedirect( network::HttpRequestHeadersUpdateParams, const std::optional<GURL> & ) override {}
	void SetPriority( net::RequestPriority, int32_t ) override {}
};

void Complete( mojo::Remote<network::mojom::URLLoaderClient> &client, int error, uint64_t n )
{
	network::URLLoaderCompletionStatus status(error);
	status.encoded_data_length = base::ByteSize(n);
	status.encoded_body_length = base::ByteSize(n);
	status.decoded_body_length = base::ByteSize(n);
	client->OnComplete(status);
}

/*
 * A load of a Blob: its bytes as the body of a response of the Blob's
 * type, or the part a Range header names (206). Only GET is answered.
 */
void LoadBlob( scoped_refptr<BlobEntry> e, std::string method, net::HttpRequestHeaders headers,
	       mojo::PendingReceiver<network::mojom::URLLoader> loader,
	       mojo::PendingRemote<network::mojom::URLLoaderClient> client_remote )
{
	if ( loader.is_valid() ) mojo::MakeSelfOwnedReceiver(std::make_unique<LoaderImpl>(), std::move(loader));
	mojo::Remote<network::mojom::URLLoaderClient> client(std::move(client_remote));
	if ( !client ) return;
	if ( e == nullptr || e->broken() ) {
		Complete(client, net::ERR_FILE_NOT_FOUND, 0);
		return;
	}
	if ( method != "GET" ) {
		Complete(client, net::ERR_METHOD_NOT_SUPPORTED, 0);
		return;
	}
	uint64_t size = e->size(), off = 0, n = size;
	bool partial = false;
	std::optional<std::string> range = headers.GetHeader(net::HttpRequestHeaders::kRange);
	if ( range.has_value() ) {
		std::vector<net::HttpByteRange> ranges;
		if ( !net::HttpUtil::ParseRangeHeader(*range, &ranges) || ranges.size() != 1
		     || !ranges[0].ComputeBounds((int64_t)size) ) {
			Complete(client, net::ERR_REQUEST_RANGE_NOT_SATISFIABLE, 0);
			return;
		}
		off = (uint64_t)ranges[0].first_byte_position();
		n = (uint64_t)( ranges[0].last_byte_position() - ranges[0].first_byte_position() + 1 );
		partial = true;
	}
	/* the headers a blob: URL's loader gives: the length, the range, and the type even when it is empty */
	std::string raw = partial ? "HTTP/1.1 206 Partial Content\r\n\r\n" : "HTTP/1.1 200 OK\r\n\r\n";
	auto head = network::mojom::URLResponseHead::New();
	head->headers = base::MakeRefCounted<net::HttpResponseHeaders>(net::HttpUtil::AssembleRawHeaders(raw));
	head->headers->SetHeader("Content-Length", std::to_string(n));
	if ( partial ) {
		head->headers->SetHeader("Content-Range", "bytes " + std::to_string(off) + "-" + std::to_string(off + n - 1)
							  + "/" + std::to_string(size));
	}
	head->headers->SetHeader("Content-Type", e->type());
	std::string mime, charset;
	bool had_charset = false;
	if ( !e->type().empty() ) net::HttpUtil::ParseContentType(e->type(), &mime, &charset, &had_charset, nullptr);
	head->mime_type = mime;
	head->charset = charset;
	head->content_length = (int64_t)n;
	mojo::ScopedDataPipeProducerHandle producer;
	mojo::ScopedDataPipeConsumerHandle consumer;
	uint32_t cap = (uint32_t)std::min<uint64_t>(std::max<uint64_t>(n, 1), 512 * 1024);
	if ( mojo::CreateDataPipe(cap, producer, consumer) != MOJO_RESULT_OK ) {
		Complete(client, net::ERR_INSUFFICIENT_RESOURCES, 0);
		return;
	}
	client->OnReceiveResponse(std::move(head), std::move(consumer), std::nullopt);
	WriteRange(e, off, n, std::move(producer),
		   base::BindOnce([]( mojo::Remote<network::mojom::URLLoaderClient> client, uint64_t n,
				      MojoResult r ) {
			   Complete(client, r == MOJO_RESULT_OK ? net::OK : net::ERR_FAILED, n);
		   }, std::move(client), n));
}

/* ---------------------------------------------------------------- the Blob interface */

/* A Blob's bytes for a request's body */
class PipeGetter final : public network::mojom::DataPipeGetter {
public:
	explicit PipeGetter( scoped_refptr<BlobEntry> e ) : e_(std::move(e)) {}

	static void Bind( scoped_refptr<BlobEntry> e, mojo::PendingReceiver<network::mojom::DataPipeGetter> r )
	{
		mojo::MakeSelfOwnedReceiver(std::make_unique<PipeGetter>(std::move(e)), std::move(r));
	}

	void Read( mojo::ScopedDataPipeProducerHandle pipe, ReadCallback cb ) override
	{
		scoped_refptr<BlobEntry> e = e_;
		e->WhenReady(base::BindOnce([]( scoped_refptr<BlobEntry> e, mojo::ScopedDataPipeProducerHandle pipe,
						ReadCallback cb ) {
			if ( e->broken() ) {
				std::move(cb).Run(net::ERR_FAILED, 0);
				return;
			}
			std::move(cb).Run(net::OK, e->size());
			WriteRange(e, 0, e->size(), std::move(pipe), base::DoNothing());
		}, e, std::move(pipe), std::move(cb)));
	}

	void Clone( mojo::PendingReceiver<network::mojom::DataPipeGetter> r ) override { Bind(e_, std::move(r)); }

private:
	scoped_refptr<BlobEntry>	e_;
};

class BlobImpl final : public bm::Blob {
public:
	explicit BlobImpl( scoped_refptr<BlobEntry> e ) : e_(std::move(e)) {}

	static void Bind( scoped_refptr<BlobEntry> e, mojo::PendingReceiver<bm::Blob> r )
	{
		mojo::MakeSelfOwnedReceiver(std::make_unique<BlobImpl>(std::move(e)), std::move(r));
	}

	void Clone( mojo::PendingReceiver<bm::Blob> r ) override { Bind(e_, std::move(r)); }

	void AsDataPipeGetter( mojo::PendingReceiver<network::mojom::blink::DataPipeGetter> r ) override
	{
		PipeGetter::Bind(e_, mojo::PendingReceiver<network::mojom::DataPipeGetter>(r.PassPipe()));
	}

	void ReadAll( mojo::ScopedDataPipeProducerHandle pipe, mojo::PendingRemote<bm::BlobReaderClient> client ) override
	{
		ReadRange(0, UINT64_MAX, std::move(pipe), std::move(client));
	}

	void ReadRange( uint64_t off, uint64_t len, mojo::ScopedDataPipeProducerHandle pipe,
			mojo::PendingRemote<bm::BlobReaderClient> client ) override
	{
		scoped_refptr<BlobEntry> e = e_;
		e->WhenReady(base::BindOnce(&BlobImpl::ReadNow, e, off, len, std::move(pipe), std::move(client)));
	}

	void Load( mojo::PendingReceiver<network::mojom::blink::URLLoader> loader, const blink::String &method,
		   const net::HttpRequestHeaders &headers,
		   mojo::PendingRemote<network::mojom::blink::URLLoaderClient> client ) override
	{
		scoped_refptr<BlobEntry> e = e_;
		e->WhenReady(base::BindOnce(&LoadBlob, e, std::string(method.Utf8()), headers,
					    mojo::PendingReceiver<network::mojom::URLLoader>(loader.PassPipe()),
					    mojo::PendingRemote<network::mojom::URLLoaderClient>(client.PassPipe(), 0)));
	}

	void ReadSideData( ReadSideDataCallback cb ) override { std::move(cb).Run(std::nullopt); }

	void CaptureSnapshot( CaptureSnapshotCallback cb ) override { std::move(cb).Run(e_->size(), std::nullopt); }

	void GetInternalUUID( GetInternalUUIDCallback cb ) override
	{
		std::move(cb).Run(blink::String::FromUtf8(e_->uuid()));
	}

private:
	static void ReadNow( scoped_refptr<BlobEntry> e, uint64_t off, uint64_t len,
			     mojo::ScopedDataPipeProducerHandle pipe,
			     mojo::PendingRemote<bm::BlobReaderClient> client_remote )
	{
		mojo::Remote<bm::BlobReaderClient> client;
		if ( client_remote.is_valid() ) client.Bind(std::move(client_remote));
		if ( e->broken() ) {
			if ( client ) client->OnComplete(net::ERR_FAILED, 0);
			return;
		}
		uint64_t size = e->size();
		if ( off > size ) off = size;
		uint64_t n = std::min(len, size - off);
		if ( client ) client->OnCalculatedSize(size, n);
		WriteRange(e, off, n, std::move(pipe),
			   base::BindOnce([]( mojo::Remote<bm::BlobReaderClient> client, uint64_t n, MojoResult r ) {
				   if ( client ) client->OnComplete(r == MOJO_RESULT_OK ? net::OK : net::ERR_FAILED, n);
			   }, std::move(client), n));
	}

	scoped_refptr<BlobEntry>	e_;
};

/* ---------------------------------------------------------------- the registry */

/* Bytes Blink handed over when asked, put in their place */
void BytesCame( scoped_refptr<BlobEntry> e, uint64_t at, uint64_t len,
		std::unique_ptr<mojo::Remote<bm::BytesProvider>>, const blink::Vector<uint8_t> &data )
{
	if ( data.size() != len ) e->Break();
	else if ( len != 0 ) memcpy(&e->bytes()[at], data.data(), (size_t)len);
	e->PartDone();
}

/* A slice of another Blob, copied in when that Blob is whole */
void SliceFound( scoped_refptr<BlobEntry> e, uint64_t at, uint64_t off, uint64_t len,
		 std::unique_ptr<mojo::Remote<bm::Blob>>, const blink::String &uuid )
{
	scoped_refptr<BlobEntry> src = FindBlob(std::string(uuid.Utf8()));
	if ( src == nullptr ) {
		e->Break();
		e->PartDone();
		return;
	}
	src->WhenReady(base::BindOnce([]( scoped_refptr<BlobEntry> e, scoped_refptr<BlobEntry> src, uint64_t at,
					  uint64_t off, uint64_t len ) {
		if ( src->broken() || off > src->size() || len > src->size() - off ) e->Break();
		else if ( len != 0 ) memcpy(&e->bytes()[at], src->bytes().data() + off, (size_t)len);
		e->PartDone();
	}, e, src, at, off, len));
}

/* A body read to its end, made a Blob (a response's blob()) */
class StreamBlob final : public mojo::DataPipeDrainer::Client {
public:
	StreamBlob( const blink::String &type, mojo::ScopedDataPipeConsumerHandle data,
		    mojo::PendingAssociatedRemote<bm::ProgressClient> progress,
		    bm::BlobRegistry::RegisterFromStreamCallback cb )
		: type_(type), cb_(std::move(cb))
	{
		if ( progress.is_valid() ) progress_.Bind(std::move(progress));
		drainer_ = std::make_unique<mojo::DataPipeDrainer>(this, std::move(data));
	}

	void OnDataAvailable( base::span<const uint8_t> data ) override
	{
		if ( bytes_.size() + data.size() > kBlobMax ) {
			too_big_ = true;
			return;
		}
		bytes_.append(reinterpret_cast<const char *>(data.data()), data.size());
		if ( progress_ ) progress_->OnProgress(data.size());
	}

	void OnDataComplete() override
	{
		if ( too_big_ ) {
			std::move(cb_).Run(nullptr);
		} else {
			auto e = base::MakeRefCounted<BlobEntry>(base::Uuid::GenerateRandomV4().AsLowercaseString(),
								 std::string(type_.Utf8()));
			e->bytes().swap(bytes_);
			mojo::PendingRemote<bm::Blob> remote;
			BlobImpl::Bind(e, remote.InitWithNewPipeAndPassReceiver());
			std::move(cb_).Run(blink::BlobDataHandle::Create(blink::String::FromUtf8(e->uuid()), type_,
									 e->size(), std::move(remote)));
		}
		/* let go after the drainer has returned from telling of the end */
		base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE, this);
	}

private:
	blink::String					type_;
	bm::BlobRegistry::RegisterFromStreamCallback	cb_;
	mojo::AssociatedRemote<bm::ProgressClient>	progress_;
	std::unique_ptr<mojo::DataPipeDrainer>		drainer_;
	std::string					bytes_;
	bool						too_big_ = false;
};

class Registry final : public bm::BlobRegistry {
public:
	void Bind( mojo::PendingReceiver<bm::BlobRegistry> r ) { receivers_.Add(this, std::move(r)); }

	void Register( mojo::PendingReceiver<bm::Blob> blob, const blink::String &uuid, const blink::String &type,
		       const blink::String &, blink::Vector<bm::DataElementPtr> elements, RegisterCallback cb ) override
	{
		auto e = base::MakeRefCounted<BlobEntry>(std::string(uuid.Utf8()), std::string(type.Utf8()));
		uint64_t total = 0;
		/* a file's part: the bytes of a file the page was given, read now */
		std::vector<std::vector<uint8_t>> files;
		for ( const bm::DataElementPtr &el : elements ) {
			if ( el->is_bytes() ) {
				total += el->get_bytes()->length;
			} else if ( el->is_blob() ) {
				total += el->get_blob()->length;
			} else if ( el->is_file() ) {
				const bm::DataElementFilePtr &f = el->get_file();
				files.emplace_back();
				if ( br_file_bytes(f->path.value(), f->offset, f->length, &files.back()) ) {
					total += files.back().size();
				} else {
					e->Break();
				}
			} else {
				e->Break();
			}
		}
		if ( total > kBlobMax ) {
			printf("browser: blob: %llu bytes are more than a Blob may hold\n", (unsigned long long)total);
			e->Break();
		}
		/* the registration itself is a part: the Blob is read after all are in */
		e->PartComing();
		if ( !e->broken() ) {
			e->bytes().resize((size_t)total);
			uint64_t at = 0;
			size_t nfile = 0;
			for ( bm::DataElementPtr &el : elements ) {
				if ( el->is_file() ) {
					std::vector<uint8_t> &f = files[nfile++];
					if ( !f.empty() ) memcpy(&e->bytes()[at], f.data(), f.size());
					at += f.size();
				} else if ( el->is_bytes() ) {
					bm::DataElementBytesPtr &b = el->get_bytes();
					if ( b->embedded_data.has_value() && b->embedded_data->size() == b->length ) {
						if ( b->length != 0 ) memcpy(&e->bytes()[at], b->embedded_data->data(), (size_t)b->length);
					} else if ( b->data.is_valid() ) {
						e->PartComing();
						auto p = std::make_unique<mojo::Remote<bm::BytesProvider>>(std::move(b->data));
						mojo::Remote<bm::BytesProvider> *pp = p.get();
						(*pp)->RequestAsReply(mojo::WrapCallbackWithDefaultInvokeIfNotRun(
							base::BindOnce(&BytesCame, e, at, b->length, std::move(p)),
							blink::Vector<uint8_t>()));
					} else if ( b->length != 0 ) {
						e->Break();
					}
					at += b->length;
				} else if ( el->is_blob() ) {
					bm::DataElementBlobPtr &s = el->get_blob();
					e->PartComing();
					auto r = std::make_unique<mojo::Remote<bm::Blob>>(std::move(s->blob));
					mojo::Remote<bm::Blob> *rp = r.get();
					(*rp)->GetInternalUUID(mojo::WrapCallbackWithDefaultInvokeIfNotRun(
						base::BindOnce(&SliceFound, e, at, s->offset, s->length, std::move(r)),
						blink::String()));
					at += s->length;
				}
			}
		}
		BlobImpl::Bind(e, std::move(blob));
		std::move(cb).Run();
		e->PartDone();
	}

	void RegisterFromStream( const blink::String &type, const blink::String &, uint64_t,
				 mojo::ScopedDataPipeConsumerHandle data,
				 mojo::PendingAssociatedRemote<bm::ProgressClient> progress,
				 RegisterFromStreamCallback cb ) override
	{
		new StreamBlob(type, std::move(data), std::move(progress), std::move(cb));
	}

private:
	mojo::ReceiverSet<bm::BlobRegistry>	receivers_;
};

Registry &TheRegistry()
{
	static auto *r = new Registry();
	return *r;
}

/*
 * A Blob of a file the page was given (<input type="file">, a drop): its
 * bytes, kept under the file's path (files_tessronos.cc), read now
 */
void RegisterFileBlob( mojo::PendingReceiver<bm::Blob> blob, std::string uuid, std::string type,
		       bm::DataElementFilePtr file )
{
	auto e = base::MakeRefCounted<BlobEntry>(std::move(uuid), std::move(type));
	std::vector<uint8_t> bytes;
	e->PartComing();
	if ( file && br_file_bytes(file->path.value(), file->offset, file->length, &bytes) ) {
		e->bytes().assign(bytes.begin(), bytes.end());
	} else {
		printf("browser: blob: no file %s\n", file ? file->path.value().c_str() : "?");
		e->Break();
	}
	BlobImpl::Bind(e, std::move(blob));
	e->PartDone();
}

/*
 * The factory of the Blobs of files: what it is asked is done on the
 * blob thread, in the order it was asked
 */
class FileBlobs final : public bm::FileBackedBlobFactory {
public:
	void RegisterBlob( mojo::PendingReceiver<bm::Blob> blob, const blink::String &uuid,
			   const blink::String &type, bm::DataElementFilePtr file ) override
	{
		OnBlobThread(base::BindOnce(&RegisterFileBlob, std::move(blob), std::string(uuid.Utf8()),
					    std::string(type.Utf8()), std::move(file)));
	}

	void RegisterBlobSync( mojo::PendingReceiver<bm::Blob> blob, const blink::String &uuid,
			       const blink::String &type, bm::DataElementFilePtr file,
			       RegisterBlobSyncCallback cb ) override
	{
		RegisterBlob(std::move(blob), uuid, type, std::move(file));
		std::move(cb).Run();
	}
};

/* ---------------------------------------------------------------- blob: URLs */

/* What a blob: URL names: the Blob, once the store has learnt which it is */
class UrlRec final : public base::RefCounted<UrlRec> {
public:
	/* The Blob handed over is one of this store's: its UUID finds it */
	static scoped_refptr<UrlRec> Create( mojo::PendingRemote<bm::Blob> blob )
	{
		scoped_refptr<UrlRec> rec = base::MakeRefCounted<UrlRec>();
		rec->remote_.Bind(std::move(blob));
		rec->remote_->GetInternalUUID(mojo::WrapCallbackWithDefaultInvokeIfNotRun(
			base::BindOnce(&UrlRec::Found, rec), blink::String()));
		return rec;
	}

	void When( base::OnceCallback<void(scoped_refptr<BlobEntry>)> cb )
	{
		if ( resolved_ ) std::move(cb).Run(entry_);
		else waiting_.push_back(std::move(cb));
	}

private:
	friend class base::RefCounted<UrlRec>;
	~UrlRec() = default;

	void Found( const blink::String &uuid )
	{
		entry_ = FindBlob(std::string(uuid.Utf8()));
		resolved_ = true;
		remote_.reset();
		std::vector<base::OnceCallback<void(scoped_refptr<BlobEntry>)>> w;
		w.swap(waiting_);
		for ( auto &c : w ) std::move(c).Run(entry_);
	}

	mojo::Remote<bm::Blob>		remote_;
	scoped_refptr<BlobEntry>	entry_;
	bool				resolved_ = false;
	std::vector<base::OnceCallback<void(scoped_refptr<BlobEntry>)>> waiting_;
};

/* The URLs registered, and the URLs a live token keeps resolvable after they are revoked */
std::map<std::string, scoped_refptr<UrlRec>> &Urls()
{
	static auto *m = new std::map<std::string, scoped_refptr<UrlRec>>();
	return *m;
}

std::multimap<std::string, scoped_refptr<UrlRec>> &TokenUrls()
{
	static auto *m = new std::multimap<std::string, scoped_refptr<UrlRec>>();
	return *m;
}

std::string UrlKey( const blink::KURL &url )
{
	blink::KURL u = url;
	u.RemoveFragmentIdentifier();
	return std::string(u.GetString().Utf8());
}

scoped_refptr<UrlRec> FindUrl( const std::string &key )
{
	auto it = Urls().find(key);
	if ( it != Urls().end() ) return it->second;
	auto t = TokenUrls().find(key);
	return t != TokenUrls().end() ? t->second : nullptr;
}

/*
 * Loads of one blob: URL. A URL is registered through the store of the
 * one who made it and looked up through the store of the one who loads
 * it, and those are pipes of their own: a URL a storage access handle
 * made (its own store) and the frame fetches at once (the frame's) may be
 * looked up before its registration has come. One not found when it is
 * resolved is looked up again as it is loaded, and then for a while
 * (kLateTries times kLateMs) before it is taken as not registered.
 */
constexpr int kLateTries = 50;
constexpr int kLateMs = 10;

class UrlLoaderFactory final : public network::mojom::URLLoaderFactory {
public:
	UrlLoaderFactory( std::string key, scoped_refptr<UrlRec> rec ) : key_(std::move(key)), rec_(std::move(rec)) {}

	static void Bind( std::string key, scoped_refptr<UrlRec> rec,
			  mojo::PendingReceiver<network::mojom::URLLoaderFactory> r )
	{
		mojo::MakeSelfOwnedReceiver(std::make_unique<UrlLoaderFactory>(std::move(key), std::move(rec)), std::move(r));
	}

	void CreateLoaderAndStart( mojo::PendingReceiver<network::mojom::URLLoader> loader, int32_t, uint32_t,
				   const network::ResourceRequest &request,
				   mojo::PendingRemote<network::mojom::URLLoaderClient> client,
				   const net::MutableNetworkTrafficAnnotationTag & ) override
	{
		if ( rec_ == nullptr ) rec_ = FindUrl(key_);
		if ( rec_ == nullptr ) {
			Late(key_, request.url.spec(), request.method, request.headers, std::move(loader), std::move(client), 0);
			return;
		}
		Start(rec_, request.method, request.headers, std::move(loader), std::move(client));
	}

	void Clone( mojo::PendingReceiver<network::mojom::URLLoaderFactory> r ) override { Bind(key_, rec_, std::move(r)); }

private:
	static void Start( scoped_refptr<UrlRec> rec, std::string method, net::HttpRequestHeaders headers,
			   mojo::PendingReceiver<network::mojom::URLLoader> loader,
			   mojo::PendingRemote<network::mojom::URLLoaderClient> client )
	{
		rec->When(base::BindOnce([]( std::string method, net::HttpRequestHeaders headers,
					     mojo::PendingReceiver<network::mojom::URLLoader> loader,
					     mojo::PendingRemote<network::mojom::URLLoaderClient> client,
					     scoped_refptr<BlobEntry> e ) {
			if ( e == nullptr ) {
				LoadBlob(nullptr, method, headers, std::move(loader), std::move(client));
				return;
			}
			e->WhenReady(base::BindOnce(&LoadBlob, e, method, headers, std::move(loader), std::move(client)));
		}, std::move(method), std::move(headers), std::move(loader), std::move(client)));
	}

	/* the URL looked up again a little later, until its registration comes or the tries are spent */
	static void Late( std::string key, std::string spec, std::string method, net::HttpRequestHeaders headers,
			  mojo::PendingReceiver<network::mojom::URLLoader> loader,
			  mojo::PendingRemote<network::mojom::URLLoaderClient> client, int tries )
	{
		scoped_refptr<UrlRec> rec = FindUrl(key);
		if ( rec != nullptr ) {
			Start(rec, std::move(method), std::move(headers), std::move(loader), std::move(client));
			return;
		}
		if ( tries >= kLateTries ) {
			printf("browser: blob: %s is not registered\n", spec.c_str());
			LoadBlob(nullptr, method, headers, std::move(loader), std::move(client));
			return;
		}
		BlobRunner()->PostDelayedTask(FROM_HERE,
			base::BindOnce(&Late, std::move(key), std::move(spec), std::move(method), std::move(headers),
				       std::move(loader), std::move(client), tries + 1),
			base::Milliseconds(kLateMs));
	}

	std::string		key_;
	scoped_refptr<UrlRec>	rec_;
};

/* A token for a blob: URL (a worker's script, a navigation): it keeps the URL resolvable while it lives */
class TokenImpl final : public bm::BlobURLToken {
public:
	TokenImpl( std::string key, scoped_refptr<UrlRec> rec, base::UnguessableToken token )
		: key_(std::move(key)), rec_(std::move(rec)), token_(token)
	{
		it_ = TokenUrls().emplace(key_, rec_);
	}
	~TokenImpl() override { TokenUrls().erase(it_); }

	static void Bind( std::string key, scoped_refptr<UrlRec> rec, base::UnguessableToken token,
			  mojo::PendingReceiver<bm::BlobURLToken> r )
	{
		mojo::MakeSelfOwnedReceiver(std::make_unique<TokenImpl>(std::move(key), std::move(rec), token),
					    std::move(r));
	}

	void Clone( mojo::PendingReceiver<bm::BlobURLToken> r ) override { Bind(key_, rec_, token_, std::move(r)); }
	void GetToken( GetTokenCallback cb ) override { std::move(cb).Run(token_); }

private:
	std::string		key_;
	scoped_refptr<UrlRec>	rec_;
	base::UnguessableToken	token_;
	std::multimap<std::string, scoped_refptr<UrlRec>>::iterator	it_;
};

class UrlStore final : public bm::BlobURLStore {
public:
	void Bind( mojo::PendingReceiver<bm::BlobURLStore> r ) { receivers_.Add(this, std::move(r)); }

	void Register( mojo::PendingRemote<bm::Blob> blob, const blink::KURL &url, RegisterCallback cb ) override
	{
		if ( url.ProtocolIs("blob") && blob.is_valid() ) {
			Urls()[UrlKey(url)] = UrlRec::Create(std::move(blob));
		}
		std::move(cb).Run();
	}

	void Revoke( const blink::KURL &url ) override { Urls().erase(UrlKey(url)); }

	void ResolveAsURLLoaderFactory( const blink::KURL &url,
					mojo::PendingReceiver<network::mojom::blink::URLLoaderFactory> factory ) override
	{
		std::string key = UrlKey(url);

		UrlLoaderFactory::Bind(key, FindUrl(key),
				       mojo::PendingReceiver<network::mojom::URLLoaderFactory>(factory.PassPipe()));
	}

	void ResolveAsBlobURLToken( const blink::KURL &url, mojo::PendingReceiver<bm::BlobURLToken> token,
				    bool ) override
	{
		std::string key = UrlKey(url);
		scoped_refptr<UrlRec> rec = FindUrl(key);
		if ( rec == nullptr ) {
			printf("browser: blob: %s is not registered\n", key.c_str());
			return;
		}
		TokenImpl::Bind(key, rec, base::UnguessableToken::Create(), std::move(token));
	}

private:
	mojo::ReceiverSet<bm::BlobURLStore>	receivers_;
};

UrlStore &TheStore()
{
	static auto *s = new UrlStore();
	return *s;
}

/* ---------------------------------------------------------------- reaching them */

void BindRegistry( mojo::PendingReceiver<bm::BlobRegistry> r )
{
	OnBlobThread(base::BindOnce([]( mojo::PendingReceiver<bm::BlobRegistry> r ) {
		TheRegistry().Bind(std::move(r));
	}, std::move(r)));
}

void BindStore( mojo::PendingReceiver<bm::BlobURLStore> r )
{
	OnBlobThread(base::BindOnce([]( mojo::PendingReceiver<bm::BlobURLStore> r ) {
		TheStore().Bind(std::move(r));
	}, std::move(r)));
}

/*
 * The broker of a worker: the store and the registry; what else a worker
 * asks the browser for is not here, and its receiver is let go (the
 * worker sees the interface as disconnected), each name told once.
 */
void BindByName( mojo::GenericPendingReceiver r, const char *who )
{
	if ( br_service_bind(r) ) return;
	if ( auto store = r.As<bm::BlobURLStore>() ) {
		BindStore(std::move(store));
		return;
	}
	if ( auto reg = r.As<bm::BlobRegistry>() ) {
		BindRegistry(std::move(reg));
		return;
	}
	if ( auto f = r.As<bm::FileBackedBlobFactory>() ) {
		mojo::MakeSelfOwnedReceiver(std::make_unique<FileBlobs>(), std::move(f));
		return;
	}
	static std::set<std::string> told;
	std::string name = r.interface_name().value_or("?");
	OnBlobThread(base::BindOnce([]( std::string name, const char *who ) {
		if ( told.insert(name).second ) printf("browser: %s: %s is not given\n", who, name.c_str());
	}, name, who));
}

/*
 * A worker's broker: IndexedDB's factory for the worker's origin (the
 * databases the windows of the origin have, on the page's thread), and
 * the rest as above
 */
class WorkerBroker final : public bm::BrowserInterfaceBroker {
public:
	explicit WorkerBroker( std::string origin ) : origin_(std::move(origin)) {}

	void GetInterface( mojo::GenericPendingReceiver r ) override
	{
		if ( !origin_.empty() && r.interface_name() == std::string(bm::IDBFactory::Name_) ) {
			br_idb_bind_for(origin_, r.PassPipe());
			return;
		}
		/* the storage buckets of the worker's origin (buckets_tessronos.cc) */
		if ( !origin_.empty() && r.interface_name() == std::string("blink.mojom.BucketManagerHost") ) {
			br_buckets_bind_for(origin_, r.PassPipe());
			return;
		}
		BindByName(std::move(r), "worker");
	}

private:
	std::string	origin_;
};

/*
 * The platform's broker, which every thread asks for the registry, and
 * WebGPU for whether 3D APIs are blocked (services_tessronos.cc)
 */
class PlatformBroker final : public blink::ThreadSafeBrowserInterfaceBrokerProxy {
protected:
	void GetInterfaceImpl( mojo::GenericPendingReceiver r ) override
	{
		if ( auto reg = r.As<bm::BlobRegistry>() ) {
			BindRegistry(std::move(reg));
			return;
		}
		if ( r.interface_name() == bm::GpuDataManager::Name_ ) (void)br_service_bind(r);
		/* navigator.plugins (pdf_tessronos.cc) */
		if ( r.interface_name() == std::string_view("blink.mojom.PluginRegistry") ) br_pdf_plugins_bind(r.PassPipe());
	}

private:
	~PlatformBroker() override = default;
};

/*
 * A window's store, on the page's thread: its calls handed on, in the
 * order they came, to the store on the blob thread
 */
class FrameStore final : public bm::BlobURLStore {
public:
	FrameStore() { BindStore(remote_.BindNewPipeAndPassReceiver()); }

	void Register( mojo::PendingRemote<bm::Blob> blob, const blink::KURL &url, RegisterCallback cb ) override
	{
		remote_->Register(std::move(blob), url, std::move(cb));
	}
	void Revoke( const blink::KURL &url ) override { remote_->Revoke(url); }
	void ResolveAsURLLoaderFactory( const blink::KURL &url,
					mojo::PendingReceiver<network::mojom::blink::URLLoaderFactory> f ) override
	{
		remote_->ResolveAsURLLoaderFactory(url, std::move(f));
	}
	void ResolveAsBlobURLToken( const blink::KURL &url, mojo::PendingReceiver<bm::BlobURLToken> t,
				    bool top ) override
	{
		remote_->ResolveAsBlobURLToken(url, std::move(t), top);
	}

private:
	mojo::Remote<bm::BlobURLStore>	remote_;
};

}  // namespace

/* A Blob's bytes and type given to the one who asked, once all of it is in */
using BlobRead = base::OnceCallback<void(bool, std::string, std::string)>;

static void GiveBlob( BlobRead done, scoped_refptr<BlobEntry> e )
{
	if ( !e || e->broken() ) {
		std::move(done).Run(false, std::string(), std::string());
		return;
	}
	e->WhenReady(base::BindOnce([]( BlobRead done, scoped_refptr<BlobEntry> e ) {
		std::move(done).Run(!e->broken(), e->bytes(), e->type());
	}, std::move(done), e));
}

/*
 * The bytes of a Blob, by its UUID or by a blob: URL the store has, given
 * to 'done' on the services' thread with the Blob's type once all of it
 * is in; false when there is no such Blob (what a page downloads)
 */
void br_blob_read( const std::string &key, bool is_url, BlobRead done )
{
	OnBlobThread(base::BindOnce([]( std::string key, bool is_url, BlobRead done ) {
		if ( !is_url ) {
			GiveBlob(std::move(done), FindBlob(key));
			return;
		}
		/* a URL revoked is still found while a token of it lives (the download's) */
		scoped_refptr<UrlRec> rec = FindUrl(UrlKey(blink::KURL(blink::String::FromUtf8(key))));
		if ( rec == nullptr ) {
			std::move(done).Run(false, std::string(), std::string());
			return;
		}
		rec->When(base::BindOnce(&GiveBlob, std::move(done)));
	}, key, is_url, std::move(done)));
}

/* What runs on the thread of the services (services_tessronos.cc) */
void br_on_service_thread( base::OnceClosure task )
{
	OnBlobThread(std::move(task));
}

/*
 * A window's store of blob: URLs, on a pipe of its own straight to the
 * store on the blob thread (br_blink.cc, the patch public_url_manager.patch):
 * a load the window waits for finds its URL without the window's thread
 */
void br_blob_url_store( mojo::PendingReceiver<blink::mojom::blink::BlobURLStore> r )
{
	BindStore(std::move(r));
}

/* A broker for a worker of an origin that starts (worker_tessronos.cc, shared and service workers) */
mojo::PendingRemote<blink::mojom::blink::BrowserInterfaceBroker> br_worker_broker( const std::string &origin )
{
	mojo::PendingRemote<bm::BrowserInterfaceBroker> remote;
	mojo::PendingReceiver<bm::BrowserInterfaceBroker> r = remote.InitWithNewPipeAndPassReceiver();
	OnBlobThread(base::BindOnce([]( std::string origin, mojo::PendingReceiver<bm::BrowserInterfaceBroker> r ) {
		mojo::MakeSelfOwnedReceiver(std::make_unique<WorkerBroker>(origin), std::move(r));
	}, origin, std::move(r)));
	return remote;
}

/* The platform's broker (br_blink.cc's Platform) */
blink::ThreadSafeBrowserInterfaceBrokerProxy *br_platform_broker()
{
	static PlatformBroker *broker = [] {
		scoped_refptr<PlatformBroker> b = base::MakeRefCounted<PlatformBroker>();
		b->AddRef();	/* for the life of the program */
		return b.get();
	}();
	return broker;
}

/*
 * The factory of the Blobs of a window's files, through the frames'
 * broker (patch file_backed_blob_factory.patch) and bound on the blob
 * thread: the page waits on a file's Blob (its size, synchronously) right
 * after asking for it, and its own thread is not free to answer then
 */
void br_file_blobs_init()
{
	blink::GetEmptyBrowserInterfaceBroker().SetBinderForTesting(
		bm::FileBackedBlobFactory::Name_, base::BindRepeating([]( mojo::ScopedMessagePipeHandle h ) {
			OnBlobThread(base::BindOnce([]( mojo::PendingReceiver<bm::FileBackedBlobFactory> r ) {
				mojo::MakeSelfOwnedReceiver(std::make_unique<FileBlobs>(), std::move(r));
			}, mojo::PendingReceiver<bm::FileBackedBlobFactory>(std::move(h))));
		}));
}

/* A frame's blob: URLs' store on an associated endpoint (storage_access_tessronos.cc) */
void br_blob_frame_store_bind( mojo::ScopedInterfaceEndpointHandle handle )
{
	mojo::MakeSelfOwnedAssociatedReceiver(std::make_unique<FrameStore>(),
					      mojo::PendingAssociatedReceiver<bm::BlobURLStore>(std::move(handle)));
}

/* A frame's associated interfaces: its blob: URLs' store */
void br_blob_frame_interfaces( blink::AssociatedInterfaceProvider *provider )
{
	provider->OverrideBinderForTesting(
		bm::BlobURLStore::Name_,
		base::BindRepeating([]( mojo::ScopedInterfaceEndpointHandle handle ) {
			mojo::MakeSelfOwnedAssociatedReceiver(
				std::make_unique<FrameStore>(),
				mojo::PendingAssociatedReceiver<bm::BlobURLStore>(std::move(handle)));
		}));
}
