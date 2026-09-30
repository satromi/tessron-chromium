/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_loader.cc
 *	ブラウザ: the loader Blink fetches a page's subresources through
 *	(style sheets, images, scripts, frames, a script's XMLHttpRequest
 *	and fetch; design 17.19).
 *
 *	A request is handed to the network stack on the network thread
 *	(port/net/net_fetch_tessronos.cc) as soon as Blink asks for it, so the
 *	requests of a page go on at once, over connections the stack keeps.
 *	Its answer comes back later as a task of the main thread, and the
 *	response with its whole body is handed to Blink's resource loader.
 *	The load may have ended while the answer was on its way (the page
 *	went elsewhere, the element was removed), so a loader is known by
 *	its address and a number, and an answer for one that is gone is
 *	dropped. A synchronous request (a script's synchronous
 *	XMLHttpRequest) waits for the network thread.
 */

#include TS_CONFIG_H

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <memory>
#include <set>
#include <string>

#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/task/single_thread_task_runner.h"
#include "net/http/http_request_headers.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/resource_request_body.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/platform/web_url.h"
#include "third_party/blink/public/platform/web_url_error.h"
#include "third_party/blink/public/platform/web_url_response.h"
#include "third_party/blink/renderer/platform/loader/fetch/url_loader/url_loader.h"
#include "third_party/blink/renderer/platform/loader/fetch/url_loader/url_loader_client.h"
#include "third_party/blink/renderer/platform/network/http_names.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/wtf/shared_buffer.h"
#include "net/base/net_errors.h"

#include "br.h"
#include "port/net/br_net.h"

namespace {

blink::WebURL web_url( const std::string &s )
{
	return blink::WebURL(blink::KURL(blink::String::FromUtf8(s)));
}

/*
 * The bare type of a Content-Type; from the address's ending when the
 * server says none, as a style sheet or an image of the wrong type is
 * refused or misread
 */
std::string guess_mime( const std::string &url, const std::string &ctype )
{
	std::string m = ctype.substr(0, ctype.find(';'));
	while ( !m.empty() && ( m.back() == ' ' || m.back() == '\t' ) ) m.pop_back();
	for ( char &ch : m ) ch = (char)tolower((unsigned char)ch);
	if ( !m.empty() ) return m;
	std::string path = url.substr(0, url.find_first_of("?#"));
	size_t dot = path.find_last_of('.');
	std::string ext = ( dot == std::string::npos ) ? "" : path.substr(dot + 1);
	for ( char &ch : ext ) ch = (char)tolower((unsigned char)ch);
	if ( ext == "css" ) return "text/css";
	if ( ext == "js" || ext == "mjs" ) return "application/javascript";
	if ( ext == "png" ) return "image/png";
	if ( ext == "jpg" || ext == "jpeg" ) return "image/jpeg";
	if ( ext == "gif" ) return "image/gif";
	if ( ext == "webp" ) return "image/webp";
	if ( ext == "avif" ) return "image/avif";
	if ( ext == "jxl" ) return "image/jxl";
	if ( ext == "ico" ) return "image/x-icon";
	if ( ext == "svg" ) return "image/svg+xml";
	if ( ext == "mp4" ) return "video/mp4";
	if ( ext == "webm" ) return "video/webm";
	if ( ext == "ogv" ) return "video/ogg";
	if ( ext == "mp3" ) return "audio/mpeg";
	if ( ext == "oga" || ext == "ogg" || ext == "opus" ) return "audio/ogg";
	if ( ext == "wav" ) return "audio/wav";
	if ( ext == "flac" ) return "audio/flac";
	return "application/octet-stream";
}

/*
 * A request's headers as "Name: value" lines parted by CRLF. The length
 * of the body is left out: the stack counts it itself.
 */
std::string collect_headers( const net::HttpRequestHeaders &headers, const std::string &referrer )
{
	std::string out;
	bool has_referer = false;
	for ( net::HttpRequestHeaders::Iterator it(headers); it.GetNext(); ) {
		std::string lower;
		for ( char ch : it.name() ) lower.push_back((char)tolower((unsigned char)ch));
		if ( lower == "content-length" ) continue;
		if ( lower == "referer" ) has_referer = true;
		out.append(it.name()).append(": ").append(it.value()).append("\r\n");
	}
	if ( !has_referer && !referrer.empty() ) out.append("Referer: ").append(referrer).append("\r\n");
	return out;
}

/* A request's body, as the bytes of its elements */
std::string collect_body( const network::ResourceRequest &request )
{
	std::string body;
	if ( request.request_body == nullptr ) return body;
	for ( const auto &e : *request.request_body->elements() ) {
		if ( e.type() == network::DataElement::Tag::kBytes ) {
			auto piece = e.As<network::DataElementBytes>().AsStringPiece();
			body.append(piece.data(), piece.size());
		}
	}
	return body;
}

/* The response's header lines put on it, for scripts to read back */
void apply_headers( blink::WebURLResponse *resp, const std::string &headers )
{
	size_t at = 0;
	while ( at < headers.size() ) {
		size_t eol = headers.find("\r\n", at);
		if ( eol == std::string::npos ) eol = headers.size();
		std::string line = headers.substr(at, eol - at);
		at = eol + 2;
		size_t colon = line.find(':');
		if ( colon == std::string::npos || colon == 0 ) continue;
		size_t vs = colon + 1;
		while ( vs < line.size() && ( line[vs] == ' ' || line[vs] == '\t' ) ) vs++;
		resp->AddHttpHeaderField(blink::WebString::FromUtf8(line.substr(0, colon)),
					 blink::WebString::FromUtf8(line.substr(vs)));
	}
}

blink::WebURLResponse make_response( const std::string &url, int status, const std::string &ctype,
				     const std::string &headers, size_t len )
{
	blink::WebURLResponse resp(web_url(url));
	resp.SetHttpStatusCode(status);
	resp.SetHttpStatusText(blink::WebString::FromAscii(status == 200 ? "OK" : ""));
	resp.SetMimeType(blink::WebString::FromUtf8(guess_mime(url, ctype)));
	std::string lc = ctype;
	for ( char &ch : lc ) ch = (char)tolower((unsigned char)ch);
	size_t cs = lc.find("charset=");
	if ( cs != std::string::npos ) {
		std::string v = ctype.substr(cs + 8);
		v = v.substr(0, v.find_first_of("; "));
		v.erase(std::remove(v.begin(), v.end(), '"'), v.end());
		resp.SetTextEncodingName(blink::WebString::FromUtf8(v));
	}
	resp.SetExpectedContentLength((int64_t)len);
	apply_headers(&resp, headers);
	return resp;
}

class BrURLLoader;

/* The loaders alive, so that an answer for one that is gone is dropped */
std::set<BrURLLoader *> &live_loaders()
{
	static auto *live = new std::set<BrURLLoader *>();
	return *live;
}

unsigned g_loader_seq;

/* What an answer finds its way back by: an address alone may be another loader by then */
struct NetCtx {
	BrURLLoader	*loader;
	unsigned	id;
};

void delivered( void *ctx, int ok, int status, const char *url, const char *headers,
		const char *ctype, const char *data, unsigned len );

class BrURLLoader final : public blink::URLLoader {
public:
	BrURLLoader() : id_(++g_loader_seq) { live_loaders().insert(this); }
	~BrURLLoader() override { live_loaders().erase(this); }

	unsigned id() const { return id_; }

	void LoadSynchronously( std::unique_ptr<network::ResourceRequest> request,
				scoped_refptr<const blink::SecurityOrigin>, bool, bool,
				base::TimeDelta, blink::URLLoaderClient *,
				blink::WebURLResponse &response,
				std::optional<blink::WebURLError> &error,
				scoped_refptr<blink::SharedBuffer> &data,
				int64_t &encoded_data_length, uint64_t &encoded_body_length,
				scoped_refptr<blink::BlobDataHandle> &,
				std::unique_ptr<blink::ResourceLoadInfoNotifierWrapper> ) override
	{
		std::string	url = request->url.spec();
		std::string	body = collect_body(*request);
		std::string	hdrs = collect_headers(request->headers, request->referrer.spec());
		char		*buf = nullptr, *resp_headers = nullptr;
		unsigned	len = 0;
		char		ctype[128] = "", final_url[2048] = "";
		int		status = 0;

		int ok = br_net_fetch_sync(url.c_str(), request->method.c_str(), body.data(),
					   (unsigned)body.size(), hdrs.c_str(), &buf, &len, ctype,
					   sizeof(ctype), &status, &resp_headers, final_url,
					   sizeof(final_url), 60000);
		if ( ok <= 0 ) {
			printf("browser: %s: not fetched\n", url.c_str());
			free(buf);
			free(resp_headers);
			error = blink::WebURLError(net::ERR_FAILED, web_url(url));
			return;
		}
		response = make_response(final_url[0] != 0 ? final_url : url, status, ctype,
					 resp_headers != nullptr ? resp_headers : "", len);
		data = blink::SharedBuffer::Create(base::span<const char>(buf, len));
		encoded_data_length = (int64_t)len;
		encoded_body_length = len;
		free(buf);
		free(resp_headers);
	}

	void LoadAsynchronously( std::unique_ptr<network::ResourceRequest> request,
				 scoped_refptr<const blink::SecurityOrigin>, bool,
				 std::unique_ptr<blink::ResourceLoadInfoNotifierWrapper>,
				 blink::CodeCacheHost *, blink::URLLoaderClient *client ) override
	{
		client_ = client;
		url_ = request->url.spec();
		std::string body = collect_body(*request);
		std::string hdrs = collect_headers(request->headers, request->referrer.spec());
		/* the answer comes to the main thread's own queue */
		NetCtx *c = new NetCtx{ this, id_ };
		br_net_fetch_set_answer_on_main(1);
		int started = br_net_fetch_start(url_.c_str(), request->method.c_str(), body.data(),
						 (unsigned)body.size(), hdrs.c_str(), delivered, c);
		br_net_fetch_set_answer_on_main(0);
		if ( !started ) {
			delete c;
			/* told as a failure from a task of its own, never from inside this call */
			base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
				FROM_HERE, base::BindOnce(&BrURLLoader::Fail, weak_.GetWeakPtr()));
		}
	}

	void Freeze( blink::LoaderFreezeMode ) override {}
	void DidChangePriority( blink::WebURLRequest::Priority, int ) override {}
	scoped_refptr<base::SingleThreadTaskRunner> GetTaskRunnerForBodyLoader() override
	{
		return base::SingleThreadTaskRunner::GetCurrentDefault();
	}

	/*
	 * The response with no body of its own, then the body as one piece
	 * of data: an image resource takes its data only that way. A call to
	 * the client may end the load and delete the loader.
	 */
	void Deliver( int ok, int status, const std::string &url, const std::string &headers,
		      const std::string &ctype, const char *data, unsigned len )
	{
		blink::URLLoaderClient *client = client_;
		if ( client == nullptr ) return;
		if ( !ok ) {
			printf("browser: %s: not fetched\n", url_.c_str());
			Fail();
			return;
		}
		printf("browser: fetched %s %d %u bytes %s\n", url_.c_str(), status, len, ctype.c_str());
		base::WeakPtr<BrURLLoader> self = weak_.GetWeakPtr();
		client->DidReceiveResponse(make_response(url.empty() ? url_ : url, status, ctype, headers, len),
					   mojo::ScopedDataPipeConsumerHandle(), std::nullopt);
		if ( !self ) return;
		if ( len > 0 ) {
			client->DidReceiveDataForTesting(base::span<const char>(data, len));
			if ( !self ) return;
		}
		client->DidFinishLoading(base::TimeTicks::Now(), (int64_t)len, len, (int64_t)len);
	}

private:
	blink::URLLoaderClient		*client_ = nullptr;
	std::string			url_;
	unsigned			id_;
	base::WeakPtrFactory<BrURLLoader> weak_{this};

	void Fail()
	{
		if ( client_ == nullptr ) return;
		client_->DidFail(blink::WebURLError(net::ERR_FAILED, web_url(url_)), base::TimeTicks::Now(),
				 0, 0, 0);
	}
};

void delivered( void *ctx, int ok, int status, const char *url, const char *headers,
		const char *ctype, const char *data, unsigned len )
{
	std::unique_ptr<NetCtx> c(static_cast<NetCtx *>(ctx));
	if ( c == nullptr ) return;
	if ( live_loaders().count(c->loader) == 0 || c->loader->id() != c->id ) return;
	c->loader->Deliver(ok, status, url != nullptr ? url : "", headers != nullptr ? headers : "",
			   ctype != nullptr ? ctype : "", data, len);
}

}  // namespace

std::unique_ptr<blink::URLLoader> br_blink_url_loader()
{
	return std::make_unique<BrURLLoader>();
}
