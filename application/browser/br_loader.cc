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
 *	A script's fetch and XMLHttpRequest are given the body through a data
 *	pipe holding all of it, which fetch() needs (its Response reads the
 *	body only as a stream). An event stream (EventSource), which stays
 *	open, is handed on as it comes: the response first, then each piece
 *	of the body.
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
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/task/single_thread_task_runner.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/data_pipe_drainer.h"
#include "mojo/public/cpp/system/simple_watcher.h"
#include "net/http/http_request_headers.h"
#include "services/network/public/mojom/data_pipe_getter.mojom.h"
#include "services/network/public/mojom/load_timing_info.mojom.h"
#include "services/network/public/mojom/fetch_api.mojom-shared.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/resource_request_body.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "net/base/schemeful_site.h"
#include "net/http/http_util.h"
#include "services/network/public/cpp/cors/cors.h"
#include "services/network/public/cpp/cross_origin_embedder_policy.h"
#include "services/network/public/cpp/cross_origin_resource_policy.h"
#include "services/network/public/cpp/document_isolation_policy.h"
#include "services/network/public/cpp/is_potentially_trustworthy.h"
#include "services/network/public/cpp/request_destination.h"
#include "services/network/public/cpp/data_element.h"
#include "services/network/public/cpp/resource_request_body.h"
#include "port/ext/br_ext.h"
#include "services/network/public/cpp/request_mode.h"
#include "url/origin.h"
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

/* The reason phrase of the response whose answer is being handed over, null outside its callback (port/net/net_fetch_tessronos.cc) */
extern "C" const char *br_net_fetch_status_text( void );
/* The reason phrase of the answer the last load waited for on this thread had (br_net_fetch_sync) */
extern "C" const char *br_net_fetch_sync_status_text( void );
/* And the protocol it came over (its ALPN), null outside the callback */
extern "C" const char *br_net_fetch_answer_protocol( void );
/* And the redirects it followed: how many (-1 outside the callback), and each one */
extern "C" int br_net_fetch_redirects( void );
extern "C" int br_net_fetch_redirect( int i, const char **url, int *status, const char **headers,
				      const char **new_url, const char **new_method, const char **new_referrer );

/*
 * A controlled document's loads go to its service worker first; the
 * document is a client of the workers by its UUID
 * (port/blink/service_worker_tessronos.cc)
 */
bool br_sw_controls( const std::string &client, const std::string &url );
void br_sw_subresource( const std::string &client, const network::ResourceRequest &request,
			base::OnceCallback<void(bool, int, std::string, std::string, std::string, std::string)> done );

namespace {

blink::WebURL web_url( const std::string &s )
{
	return blink::WebURL(blink::KURL(blink::String::FromUtf8(s)));
}

/* The network stack's error for the last failed request for 'url' (ERR_FAILED when it told none) */
int net_error_of( const std::string &url )
{
	int e = br_net_fetch_error(url.c_str(), nullptr, 0);
	return e < 0 ? e : net::ERR_FAILED;
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

/* Whether a request's body has parts in data pipes (a Blob's), which are read before it goes */
bool body_has_pipes( const network::ResourceRequest &request )
{
	if ( request.request_body == nullptr ) return false;
	for ( const auto &e : *request.request_body->elements() ) {
		if ( e.type() == network::DataElement::Tag::kDataPipe ) return true;
	}
	return false;
}

/*
 * The parts of a request's body in data pipes read, each into bytes in
 * its place, one after another; the request is then handed to 'done' (on
 * the thread that asked). It goes once it has handed it on.
 */
class BodyReader final : public mojo::DataPipeDrainer::Client {
public:
	using Done = base::OnceCallback<void( std::unique_ptr<network::ResourceRequest> )>;

	static void Read( std::unique_ptr<network::ResourceRequest> request, Done done )
	{
		(new BodyReader(std::move(request), std::move(done)))->Next();
	}

	void OnDataAvailable( base::span<const uint8_t> data ) override { bytes_.insert(bytes_.end(), data.begin(), data.end()); }

	void OnDataComplete() override
	{
		(*request_->request_body->elements_mutable())[at_] = network::DataElement(network::DataElementBytes(std::move(bytes_)));
		bytes_.clear();
		at_++;
		base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
			FROM_HERE, base::BindOnce(&BodyReader::Next, base::Unretained(this)));
	}

private:
	BodyReader( std::unique_ptr<network::ResourceRequest> request, Done done )
		: request_(std::move(request)), done_(std::move(done)) {}

	void Next()
	{
		drainer_.reset();
		getter_.reset();
		auto &els = *request_->request_body->elements_mutable();
		for ( ; at_ < els.size(); at_++ ) {
			if ( els[at_].type() != network::DataElement::Tag::kDataPipe ) continue;
			mojo::ScopedDataPipeProducerHandle producer;
			mojo::ScopedDataPipeConsumerHandle consumer;
			if ( mojo::CreateDataPipe(nullptr, producer, consumer) != MOJO_RESULT_OK ) break;
			getter_.Bind(els[at_].As<network::DataElementDataPipe>().CloneDataPipeGetter());
			getter_->Read(std::move(producer), base::BindOnce([]( int32_t, uint64_t ) {}));
			drainer_ = std::make_unique<mojo::DataPipeDrainer>(this, std::move(consumer));
			return;
		}
		std::move(done_).Run(std::move(request_));
		delete this;
	}

	std::unique_ptr<network::ResourceRequest>	request_;
	Done						done_;
	size_t						at_ = 0;
	std::vector<uint8_t>				bytes_;
	mojo::Remote<network::mojom::DataPipeGetter>	getter_;
	std::unique_ptr<mojo::DataPipeDrainer>		drainer_;
};

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
		/* a header's bytes are taken as Latin-1, each byte a character, as a browser takes them */
		resp->AddHttpHeaderField(blink::WebString::FromLatin1(line.substr(0, colon)),
					 blink::WebString::FromLatin1(line.substr(vs)));
	}
}

/* Whether the response's header lines have one of a name (lowered) */
bool has_header( const std::string &headers, const char *name )
{
	size_t n = strlen(name), at = 0;
	while ( at < headers.size() ) {
		size_t eol = headers.find("\r\n", at);
		if ( eol == std::string::npos ) eol = headers.size();
		if ( eol - at > n && headers[at + n] == ':' && base::EqualsCaseInsensitiveASCII(headers.substr(at, n), name) ) {
			return true;
		}
		at = eol + 2;
	}
	return false;
}

blink::WebURLResponse make_response( const std::string &url, int status, const std::string &ctype,
				     const std::string &headers, size_t len )
{
	blink::WebURLResponse resp(web_url(url));
	resp.SetHttpStatusCode(status);
	/*
	 * the status line's reason phrase, as the network stack had it (none
	 * over HTTP/2 and HTTP/3); a load waited for does not have it
	 */
	const char *reason = br_net_fetch_status_text();
	resp.SetHttpStatusText(reason != nullptr ? blink::WebString::FromLatin1(reason)
					       : blink::WebString::FromAscii(status == 200 ? "OK" : ""));
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
	/* the protocol it came over, which the resource's timing names (nextHopProtocol) */
	const char *proto = br_net_fetch_answer_protocol();
	if ( proto != nullptr && *proto != '\0' ) {
		std::string pr = proto;
		resp.SetWasAlpnNegotiated(pr != "unknown");
		resp.SetAlpnNegotiatedProtocol(blink::WebString::FromUtf8(pr));
		resp.SetWasFetchedViaSPDY(pr == "h2" || pr == "h3");
		resp.SetConnectionInfo(pr == "h3" ? net::HttpConnectionInfo::kQUIC_RFC_V1
				       : pr == "h2" ? net::HttpConnectionInfo::kHTTP2 : net::HttpConnectionInfo::kHTTP1_1);
	}
	/* the length known ahead only when the response said it (a progress event's lengthComputable) */
	resp.SetExpectedContentLength(has_header(headers, "content-length") ? (int64_t)len : -1);
	apply_headers(&resp, headers);
	return resp;
}


/* ------------------------------------------------ what the network service adds */

/*
 * A request as a browser's network service sends it, beside the headers
 * the page and Blink gave: the Origin header (a CORS request to another
 * origin, or a method other than GET and HEAD), the fetch metadata to a
 * potentially trustworthy address (Sec-Fetch-Site, -Mode, -Dest, and
 * -Storage-Access for a cross-site request with credentials), the
 * credentials and the priority (told to the stack by lines of its own,
 * which it takes off), and how its answer is to be checked: by CORS (a
 * CORS request to another origin), as an opaque answer (a no-cors request
 * to another origin), or refused (a same-origin request to another
 * origin). Every answer is also held to its Cross-Origin-Resource-Policy
 * by the requesting document's Cross-Origin-Embedder-Policy (corp_allows);
 * under COEP "credentialless" a no-cors request to another origin goes
 * without credentials.
 */
struct BrPlan {
	bool		cors = false;		/* the answer must pass the CORS check */
	bool		opaque = false;		/* the answer is opaque to the page */
	bool		refuse = false;		/* not to be sent at all */
	bool		preflight = false;	/* a CORS preflight goes first */
	std::string	unsafe;			/* Access-Control-Request-Headers of the preflight */
	std::string	site;			/* the Sec-Fetch-Site value, empty when none is sent */
	url::Origin	origin;			/* the initiator */
	network::mojom::CredentialsMode creds = network::mojom::CredentialsMode::kInclude;
	bool		has_initiator = false;
	bool		send_creds = true;	/* credentials go with it */
	GURL		original;		/* the URL first asked for */
	network::mojom::RequestMode mode = network::mojom::RequestMode::kNoCors;
	network::mojom::RequestDestination dest = network::mojom::RequestDestination::kEmpty;
	network::mojom::CrossOriginEmbedderPolicyValue coep = network::mojom::CrossOriginEmbedderPolicyValue::kNone;
};

/* Lines the stack reads and takes off before it sends the request */
constexpr char kBrCredentials[] = "X-TessronOS-Credentials";
constexpr char kBrPriority[] = "X-TessronOS-Priority";

/*
 * The names of the request's headers that are neither CORS-safelisted
 * nor ones a page may not set (those the browser adds itself), lowered,
 * sorted and without repeats: what a preflight asks the server to allow
 */
std::string unsafe_header_names( const net::HttpRequestHeaders &headers )
{
	std::vector<std::string> names, maybe;
	size_t safelisted_size = 0;
	for ( const auto &h : headers.GetHeaderVector() ) {
		if ( !net::HttpUtil::IsSafeHeader(h.key, h.value) ) continue;
		std::string name = base::ToLowerASCII(h.key);
		if ( !network::cors::IsCorsSafelistedHeader(name, h.value) ) {
			names.push_back(name);
		} else {
			maybe.push_back(name);
			safelisted_size += h.value.size();
		}
	}
	/* safelisted headers too long together are not safe after all */
	if ( safelisted_size > 1024 ) names.insert(names.end(), maybe.begin(), maybe.end());
	std::sort(names.begin(), names.end());
	names.erase(std::unique(names.begin(), names.end()), names.end());
	return base::JoinString(names, ",");
}

/* The line that tells the network stack what the extensions' rules are to know of a request */
std::string br_ext_line( const network::ResourceRequest &r );

BrPlan br_request_plan( const network::ResourceRequest &r, std::string *hdrs,
			 network::mojom::CrossOriginEmbedderPolicyValue coep )
{
	BrPlan plan;
	const GURL &url = r.url;
	plan.creds = r.credentials_mode;
	plan.original = url;
	plan.mode = r.mode;
	plan.dest = r.destination;
	plan.coep = coep;
	if ( !url.SchemeIsHTTPOrHTTPS() ) return plan;
	bool has_initiator = r.request_initiator.has_value();
	plan.has_initiator = has_initiator;
	if ( has_initiator ) plan.origin = *r.request_initiator;
	bool same_origin = has_initiator && plan.origin.IsSameOriginWith(url);
	bool cors_mode = r.mode == network::mojom::RequestMode::kCors ||
			 r.mode == network::mojom::RequestMode::kCorsWithForcedPreflight;
	/* an extension's own request to a host it declared is not held to the server's answer */
	bool ext_free = has_initiator && !same_origin && plan.origin.scheme() == "chrome-extension"
			&& br_ext_cors_free(plan.origin.Serialize(), url.spec());
	if ( has_initiator && !same_origin && !ext_free ) {
		if ( r.mode == network::mojom::RequestMode::kSameOrigin ) plan.refuse = true;
		else if ( cors_mode ) plan.cors = true;
		else if ( r.mode == network::mojom::RequestMode::kNoCors ) plan.opaque = true;
	}
	if ( plan.cors ) {
		plan.unsafe = unsafe_header_names(r.headers);
		plan.preflight = r.mode == network::mojom::RequestMode::kCorsWithForcedPreflight ||
				 !network::cors::IsCorsSafelistedMethod(r.method) || !plan.unsafe.empty();
	}
	bool get_or_head = r.method == "GET" || r.method == "HEAD";
	if ( has_initiator && !r.headers.HasHeader("Origin") && ( plan.cors || !get_or_head ) ) {
		hdrs->append("Origin: ").append(plan.origin.Serialize()).append("\r\n");
	}
	if ( network::IsUrlPotentiallyTrustworthy(url) ) {
		if ( !has_initiator ) plan.site = "none";
		else if ( same_origin ) plan.site = "same-origin";
		else if ( net::SchemefulSite(plan.origin) == net::SchemefulSite(url) ) plan.site = "same-site";
		else plan.site = "cross-site";
		hdrs->append("Sec-Fetch-Site: ").append(plan.site).append("\r\n");
		hdrs->append("Sec-Fetch-Mode: ").append(network::RequestModeToString(r.mode)).append("\r\n");
		hdrs->append("Sec-Fetch-Dest: ").append(network::RequestDestinationToString(
			r.destination, network::EmptyRequestDestinationOption::kUseFiveCharEmptyString)).append("\r\n");
		if ( plan.site == "cross-site" && r.credentials_mode == network::mojom::CredentialsMode::kInclude ) {
			hdrs->append("Sec-Fetch-Storage-Access: active\r\n");
		}
	}
	/* credentials: "omit", or "same-origin" to another origin, sends none */
	bool creds = r.credentials_mode == network::mojom::CredentialsMode::kInclude ||
		     ( r.credentials_mode == network::mojom::CredentialsMode::kSameOrigin && ( !has_initiator || same_origin ) );
	/* COEP "credentialless": a no-cors request to another origin is sent as if credentials were omitted */
	if ( coep == network::mojom::CrossOriginEmbedderPolicyValue::kCredentialless && plan.opaque ) creds = false;
	plan.send_creds = creds;
	if ( !creds ) hdrs->append(kBrCredentials).append(": omit\r\n");
	hdrs->append(kBrPriority).append(": ").append(base::NumberToString((int)r.priority))
		.append(r.priority_incremental ? " i" : "").append("\r\n");
	return plan;
}

/* A response header's value (the first of its name), if it has one */
std::optional<std::string> header_of( const std::string &headers, const char *name )
{
	/* a header given more than once is its values joined with ", ", as the network stack joins them */
	size_t n = strlen(name), at = 0;
	std::optional<std::string> out;
	while ( at < headers.size() ) {
		size_t eol = headers.find("\r\n", at);
		if ( eol == std::string::npos ) eol = headers.size();
		if ( eol - at > n && headers[at + n] == ':' &&
		     base::EqualsCaseInsensitiveASCII(std::string_view(headers).substr(at, n), name) ) {
			std::string v = headers.substr(at + n + 1, eol - at - n - 1);
			std::string t(base::TrimWhitespaceASCII(v, base::TRIM_ALL));
			out = out ? *out + ", " + t : t;
		}
		at = eol + 2;
	}
	return out;
}

/*
 * Whether the answer's Cross-Origin-Resource-Policy lets the requesting
 * document have it, as the network service checks it: a no-cors answer
 * from another origin (or site) that says "same-origin" (or "same-site")
 * is blocked, and under COEP "require-corp" one that says nothing is
 * too (network::CrossOriginResourcePolicy); why, said on the console as
 * a browser says it
 */
bool corp_allows( const BrPlan &plan, const std::string &url, const std::string &headers )
{
	GURL u(url);
	if ( !u.SchemeIsHTTPOrHTTPS() ) return true;
	network::CrossOriginEmbedderPolicy coep;
	coep.value = plan.coep;
	std::optional<url::Origin> initiator;
	if ( plan.has_initiator ) initiator = plan.origin;
	auto blocked = network::CrossOriginResourcePolicy::IsBlockedByHeaderValue(
		u, plan.original, initiator, header_of(headers, "Cross-Origin-Resource-Policy"), plan.mode, plan.dest,
		plan.send_creds, coep, nullptr, network::DocumentIsolationPolicy(), nullptr);
	if ( !blocked ) return true;
	printf("browser: console: Failed to load resource: net::ERR_BLOCKED_BY_RESPONSE (%s, reason %d)\n", url.c_str(),
	       (int)*blocked);
	return false;
}

/* Whether a CORS answer lets the page have it; why not, said on the console as a browser says it */
bool cors_allows( const BrPlan &plan, const std::string &url, const std::string &headers, const char *what )
{
	auto ok = network::cors::CheckAccess(GURL(url), header_of(headers, "Access-Control-Allow-Origin"),
					     header_of(headers, "Access-Control-Allow-Credentials"),
					     plan.creds, plan.origin);
	if ( ok.has_value() ) return true;
	printf("browser: console: Access to %s at '%s' from origin '%s' has been blocked by CORS policy (%d)\n",
	       what, url.c_str(), plan.origin.Serialize().c_str(), (int)ok.error().cors_error);
	return false;
}

/*
 * Whether a preflight's answer lets the request go: a 2xx status, the
 * origin allowed, and the method and the headers listed (or "*" when no
 * credentials go)
 */
bool preflight_allows( const BrPlan &plan, const std::string &url, const std::string &method, int status,
		       const std::string &headers )
{
	if ( status < 200 || status >= 300 ) {
		printf("browser: console: Access to fetch at '%s' from origin '%s' has been blocked by CORS policy: "
		       "Response to preflight request doesn't pass access control check: It does not have HTTP ok status.\n",
		       url.c_str(), plan.origin.Serialize().c_str());
		return false;
	}
	if ( !cors_allows(plan, url, headers, "fetch") ) return false;
	bool creds = plan.creds == network::mojom::CredentialsMode::kInclude;
	auto listed = [&]( const char *name, const std::string &want, bool exact ) {
		std::optional<std::string> v = header_of(headers, name);
		if ( !v ) return false;
		for ( const auto &t : base::SplitString(*v, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY) ) {
			if ( t == "*" && !creds ) return true;
			if ( exact ? t == want : base::EqualsCaseInsensitiveASCII(t, want) ) return true;
		}
		return false;
	};
	if ( !network::cors::IsCorsSafelistedMethod(method) && !listed("Access-Control-Allow-Methods", method, true) ) {
		printf("browser: console: Access to fetch at '%s' has been blocked by CORS policy: Method %s is not allowed "
		       "by Access-Control-Allow-Methods in preflight response.\n", url.c_str(), method.c_str());
		return false;
	}
	for ( const auto &h : base::SplitString(plan.unsafe, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY) ) {
		if ( !listed("Access-Control-Allow-Headers", h, false) ) {
			printf("browser: console: Access to fetch at '%s' has been blocked by CORS policy: Request header "
			       "field %s is not allowed by Access-Control-Allow-Headers in preflight response.\n",
			       url.c_str(), h.c_str());
			return false;
		}
	}
	return true;
}

/*
 * Preflights that passed, as a browser keeps them: by origin, address
 * and credentials, with the method and the headers allowed, for as long
 * as Access-Control-Max-Age says (5 seconds when it says nothing, at most
 * two hours)
 */
struct BrPreflightKept {
	base::TimeTicks	until;
	std::string	methods, headers;
	bool		any_method = false, any_header = false;
};

std::map<std::string, BrPreflightKept> &kept_preflights()
{
	static auto *m = new std::map<std::string, BrPreflightKept>();
	return *m;
}

std::mutex &kept_lock()
{
	static auto *m = new std::mutex();
	return *m;
}

std::string preflight_key( const BrPlan &plan, const std::string &url )
{
	return plan.origin.Serialize() + " " + url + ( plan.creds == network::mojom::CredentialsMode::kInclude ? " c" : "" );
}

void keep_preflight( const BrPlan &plan, const std::string &url, const std::string &headers )
{
	int64_t age = 5;
	if ( std::optional<std::string> a = header_of(headers, "Access-Control-Max-Age") ) {
		int64_t v;
		if ( base::StringToInt64(*a, &v) ) age = std::min<int64_t>(std::max<int64_t>(v, 0), 7200);
	}
	if ( age <= 0 ) return;
	BrPreflightKept k;
	k.until = base::TimeTicks::Now() + base::Seconds(age);
	bool creds = plan.creds == network::mojom::CredentialsMode::kInclude;
	k.methods = base::ToUpperASCII(header_of(headers, "Access-Control-Allow-Methods").value_or(""));
	k.headers = base::ToLowerASCII(header_of(headers, "Access-Control-Allow-Headers").value_or(""));
	for ( const auto &t : base::SplitString(k.methods, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY) ) {
		if ( t == "*" && !creds ) k.any_method = true;
	}
	for ( const auto &t : base::SplitString(k.headers, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY) ) {
		if ( t == "*" && !creds ) k.any_header = true;
	}
	std::lock_guard<std::mutex> hold(kept_lock());
	kept_preflights()[preflight_key(plan, url)] = k;
}

bool preflight_kept( const BrPlan &plan, const std::string &url, const std::string &method )
{
	std::lock_guard<std::mutex> hold(kept_lock());
	auto it = kept_preflights().find(preflight_key(plan, url));
	if ( it == kept_preflights().end() ) return false;
	if ( base::TimeTicks::Now() >= it->second.until ) {
		kept_preflights().erase(it);
		return false;
	}
	const BrPreflightKept &k = it->second;
	auto in = []( const std::string &list, const std::string &v ) {
		for ( const auto &t : base::SplitString(list, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY) ) {
			if ( t == v ) return true;
		}
		return false;
	};
	if ( !network::cors::IsCorsSafelistedMethod(method) && !k.any_method && !in(k.methods, base::ToUpperASCII(method)) ) {
		return false;
	}
	if ( !k.any_header ) {
		for ( const auto &h : base::SplitString(plan.unsafe, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY) ) {
			if ( !in(k.headers, h) ) return false;
		}
	}
	return true;
}

/*
 * The request's referrer as Blink worked it out, and the policy that
 * trims it again if the request is redirected; the network layer gives
 * both to the stack's request (net_fetch_tessronos.cc), which sends the
 * Referer
 */
std::string referrer_lines( const network::ResourceRequest &r )
{
	if ( !r.referrer.is_valid() ) return std::string();
	return "Referer: " + r.referrer.spec() + "\r\nX-TessronOS-Referrer-Policy: " +
	       base::NumberToString((int)r.referrer_policy) + "\r\n";
}

/* The preflight's own request: OPTIONS with what the request asks to be allowed, no credentials */
std::string preflight_headers( const network::ResourceRequest &r, const BrPlan &plan )
{
	std::string h = "Accept: */*\r\nAccess-Control-Request-Method: " + r.method + "\r\n";
	if ( !plan.unsafe.empty() ) h += "Access-Control-Request-Headers: " + plan.unsafe + "\r\n";
	h += "Origin: " + plan.origin.Serialize() + "\r\n";
	if ( !plan.site.empty() ) {
		h += "Sec-Fetch-Mode: cors\r\nSec-Fetch-Site: " + plan.site + "\r\nSec-Fetch-Dest: " +
		     network::RequestDestinationToString(r.destination,
							  network::EmptyRequestDestinationOption::kUseFiveCharEmptyString) + "\r\n";
	}
	h += referrer_lines(r);
	h += std::string(kBrCredentials) + ": omit\r\n";
	h += std::string(kBrPriority) + ": " + base::NumberToString((int)r.priority) + "\r\n";
	return h;
}

/*
 * The response types a CORS answer and an opaque one are given, so that
 * Blink filters them as it filters a browser's: a CORS answer shows the
 * safelisted headers and those Access-Control-Expose-Headers names (all
 * for "*" without credentials); an opaque answer shows nothing
 */
void type_response( blink::WebURLResponse *resp, const BrPlan &plan, const std::string &headers )
{
	if ( plan.opaque ) {
		resp->SetType(network::mojom::FetchResponseType::kOpaque);
		return;
	}
	if ( !plan.cors ) return;
	resp->SetType(network::mojom::FetchResponseType::kCors);
	std::vector<blink::WebString> exposed;
	bool creds = plan.creds == network::mojom::CredentialsMode::kInclude;
	std::optional<std::string> e = header_of(headers, "Access-Control-Expose-Headers");
	bool all = false;
	if ( e ) {
		for ( const auto &t : base::SplitString(*e, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY) ) {
			if ( t == "*" && !creds ) all = true;
			else exposed.push_back(blink::WebString::FromUtf8(t));
		}
	}
	if ( all ) {
		size_t at = 0;
		while ( at < headers.size() ) {
			size_t eol = headers.find("\r\n", at);
			if ( eol == std::string::npos ) eol = headers.size();
			size_t colon = headers.find(':', at);
			if ( colon != std::string::npos && colon < eol ) {
				exposed.push_back(blink::WebString::FromUtf8(headers.substr(at, colon - at)));
			}
			at = eol + 2;
		}
	}
	resp->SetCorsExposedHeaderNames(exposed);
}

class BrURLLoader;

/* A request's kind, as declarativeNetRequest's rules name it */
const char *ext_type_of( network::mojom::RequestDestination d )
{
	using D = network::mojom::RequestDestination;
	switch ( d ) {
	case D::kDocument:	return "main_frame";
	case D::kIframe:
	case D::kFrame:		return "sub_frame";
	case D::kStyle:
	case D::kXslt:		return "stylesheet";
	case D::kScript:
	case D::kWorker:
	case D::kSharedWorker:
	case D::kServiceWorker:	return "script";
	case D::kImage:		return "image";
	case D::kFont:		return "font";
	case D::kAudio:
	case D::kVideo:
	case D::kTrack:		return "media";
	case D::kObject:
	case D::kEmbed:		return "object";
	case D::kEmpty:		return "xmlhttprequest";
	default:		return "other";
	}
}

std::string br_ext_line( const network::ResourceRequest &r )
{
	if ( !br_ext_any() ) return std::string();
	std::string line = std::string("X-TessronOS-Resource: ") + ext_type_of(r.destination);
	if ( r.request_initiator.has_value() ) line += " " + r.request_initiator->Serialize();
	return line + "\r\n";
}

/*
 * The loaders alive, so that an answer for one that is gone is dropped.
 * The page's thread and the workers' make and end loaders, each its own.
 */
std::set<BrURLLoader *> &live_loaders()
{
	static auto *live = new std::set<BrURLLoader *>();
	return *live;
}

std::mutex &live_lock()
{
	static auto *m = new std::mutex();
	return *m;
}

std::atomic<unsigned> g_loader_seq;

bool is_live( BrURLLoader *l );

/* What an answer finds its way back by: an address alone may be another loader by then */
struct NetCtx {
	BrURLLoader	*loader;
	unsigned	id;
};

void delivered( void *ctx, int ok, int status, const char *url, const char *headers,
		const char *ctype, const char *data, unsigned len );

/* A preflight's way back: the loader, which may be gone, on the thread that asked */
struct PreCtx {
	base::WeakPtr<BrURLLoader>			loader;
	scoped_refptr<base::SingleThreadTaskRunner>	runner;
};

void preflighted( void *ctx, int ok, int status, const char *url, const char *headers,
		  const char *ctype, const char *data, unsigned len );
void streamed( void *ctx, int first, int status, const char *url, const char *headers,
	       const char *ctype, const char *data, unsigned len );

/* Whether a request asks for an event stream (EventSource): its body is handed on as it comes */
bool wants_stream( const net::HttpRequestHeaders &headers )
{
	std::optional<std::string> accept = headers.GetHeader("Accept");
	return accept.has_value() && accept->find("text/event-stream") != std::string::npos;
}

class BrURLLoader final : public blink::URLLoader {
public:
	/*
	 * A page's loader answers on the page's thread; a worker's on the
	 * thread that starts its load (the worker's)
	 */
	explicit BrURLLoader( bool on_main = true, const std::string &sw_client = std::string(),
			      network::mojom::CrossOriginEmbedderPolicyValue coep = network::mojom::CrossOriginEmbedderPolicyValue::kNone )
		: id_(++g_loader_seq), on_main_(on_main), sw_client_(sw_client), coep_(coep)
	{
		std::lock_guard<std::mutex> hold(live_lock());
		live_loaders().insert(this);
	}
	~BrURLLoader() override
	{
		{
			std::lock_guard<std::mutex> hold(live_lock());
			live_loaders().erase(this);
		}
		/* a stream still open (an EventSource closed, a script's body left unread) is let go */
		if ( ( stream_ || piped_ ) && !ended_ && !cancelled_ && net_ctx_ != nullptr ) br_net_fetch_cancel(net_ctx_);
	}

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
		std::string	hdrs = collect_headers(request->headers, std::string());
		BrPlan		plan = br_request_plan(*request, &hdrs, coep_);
		char		*buf = nullptr, *resp_headers = nullptr;
		unsigned	len = 0;
		char		ctype[128] = "", final_url[2048] = "";
		int		status = 0;

		hdrs += referrer_lines(*request) + br_ext_line(*request);
		if ( plan.refuse ) {
			error = blink::WebURLError(net::ERR_FAILED, web_url(url));
			return;
		}
		/* a preflight first, waited for too */
		if ( plan.preflight && !preflight_kept(plan, url, request->method) ) {
			char *pbuf = nullptr, *phead = nullptr;
			unsigned plen = 0;
			char pct[64] = "", pfinal[16] = "";
			int pstatus = 0;
			int pok = br_net_fetch_sync(url.c_str(), "OPTIONS", nullptr, 0, preflight_headers(*request, plan).c_str(),
						    &pbuf, &plen, pct, sizeof(pct), &pstatus, &phead, pfinal, sizeof(pfinal), 60000);
			std::string ph = phead != nullptr ? phead : "";
			free(pbuf);
			free(phead);
			if ( pok <= 0 || !preflight_allows(plan, url, request->method, pstatus, ph) ) {
				error = blink::WebURLError(net::ERR_FAILED, web_url(url));
				return;
			}
			keep_preflight(plan, url, ph);
		}
		int ok = br_net_fetch_sync(url.c_str(), request->method.c_str(), body.data(),
					   (unsigned)body.size(), hdrs.c_str(), &buf, &len, ctype,
					   sizeof(ctype), &status, &resp_headers, final_url,
					   sizeof(final_url), 60000);
		if ( ok <= 0 ) {
			printf("browser: %s: not fetched\n", url.c_str());
			free(buf);
			free(resp_headers);
			error = blink::WebURLError(net_error_of(url), web_url(url));
			return;
		}
		std::string rh = resp_headers != nullptr ? resp_headers : "";
		if ( ( plan.cors && !cors_allows(plan, final_url[0] != 0 ? final_url : url, rh, "XMLHttpRequest") )
		     || !corp_allows(plan, final_url[0] != 0 ? final_url : url, rh) ) {
			free(buf);
			free(resp_headers);
			error = blink::WebURLError(net::ERR_FAILED, web_url(url));
			return;
		}
		response = make_response(final_url[0] != 0 ? final_url : url, status, ctype, rh, len);
		response.SetHttpStatusText(blink::WebString::FromLatin1(br_net_fetch_sync_status_text()));
		type_response(&response, plan, rh);
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
		/* a body with parts in data pipes (a Blob's): they are read first, and the load goes on then */
		if ( body_has_pipes(*request) ) {
			url_ = request->url.spec();
			BodyReader::Read(std::move(request), base::BindOnce(&BrURLLoader::Go, weak_.GetWeakPtr()));
			return;
		}
		Go(std::move(request));
	}

	/* The load begun, its body in bytes */
	void Go( std::unique_ptr<network::ResourceRequest> request )
	{
		if ( !ExtensionsLet(request) ) return;
		/* a document a service worker controls: its worker is asked first */
		if ( on_main_ && !wants_stream(request->headers) && br_sw_controls(sw_client_, request->url.spec()) ) {
			url_ = request->url.spec();
			pipe_ = request->destination == network::mojom::RequestDestination::kEmpty;
			waiting_ = std::move(request);
			br_sw_subresource(sw_client_, *waiting_,
					  base::BindOnce(&BrURLLoader::FromWorker, weak_.GetWeakPtr()));
			return;
		}
		Start(std::move(request));
	}

	/*
	 * An address of an extension's: its files and calls are answered here.
	 * False when it needs no more doing. The rules on other requests are
	 * the network stack's, told of the request by br_ext_line.
	 */
	bool ExtensionsLet( std::unique_ptr<network::ResourceRequest> &request )
	{
		if ( !request->url.SchemeIs("chrome-extension") ) return true;
		BrExtReq q;
		BrExtAns ans;
		q.url = request->url.spec();
		if ( request->request_initiator.has_value() ) q.initiator = request->request_initiator->Serialize();
		q.method = request->method;
		q.type = ext_type_of(request->destination);
		if ( request->request_body ) {
			for ( const network::DataElement &el : *request->request_body->elements() ) {
				if ( el.type() == network::DataElement::Tag::kBytes ) {
					q.body += std::string(el.As<network::DataElementBytes>().AsStringPiece());
				}
			}
		}
		url_ = q.url;
		pipe_ = request->destination == network::mojom::RequestDestination::kEmpty;
		scoped_refptr<base::SequencedTaskRunner> runner = base::SequencedTaskRunner::GetCurrentDefault();
		base::WeakPtr<BrURLLoader> weak = weak_.GetWeakPtr();
		std::string at = q.url;
		auto later = [runner, weak, at]( BrExtAns a ) {
			runner->PostTask(FROM_HERE, base::BindOnce(&BrURLLoader::Served, weak, at, std::move(a)));
		};
		switch ( br_ext_serve_later(q, &ans, later) ) {
		case BR_EXT_PASS:
		case BR_EXT_REDIRECT:
			return true;
		case BR_EXT_LATER:
			return false;		/* answered when what it waits for comes */
		case BR_EXT_SERVED:
			base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
				FROM_HERE, base::BindOnce(&BrURLLoader::Served, weak_.GetWeakPtr(), q.url, std::move(ans)));
			return false;
		case BR_EXT_BLOCK:
			base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
				FROM_HERE, base::BindOnce(&BrURLLoader::Fail, weak_.GetWeakPtr(), net::ERR_BLOCKED_BY_CLIENT));
			return false;
		}
		return true;
	}

	void Served( std::string url, BrExtAns ans )
	{
		/* its type as a header too: what a module script is held to is that header */
		Deliver(1, ans.status, url, "Access-Control-Allow-Origin: *\r\nContent-Type: " + ans.ctype + "\r\n" + ans.headers, ans.ctype,
			ans.body.data(), (unsigned)ans.body.size());
	}

	/* What the page's service worker answered: its response, or the network asked instead */
	void FromWorker( bool fallback, int status, std::string url, std::string headers, std::string ctype,
			 std::string body )
	{
		if ( fallback ) {
			if ( waiting_ ) Start(std::move(waiting_));
			return;
		}
		waiting_.reset();
		printf("browser: %s answered by its service worker\n", url_.c_str());
		Deliver(1, status, url, headers, ctype, body.data(), (unsigned)body.size());
	}

	/* The request handed to the network stack */
	void Start( std::unique_ptr<network::ResourceRequest> request )
	{
		url_ = request->url.spec();
		std::string extra;
		plan_ = br_request_plan(*request, &extra, coep_);
		if ( plan_.refuse ) {
			printf("browser: console: Fetch API cannot load %s. Request mode is \"same-origin\" but the URL's "
			       "origin is not same as the request origin %s.\n", url_.c_str(), plan_.origin.Serialize().c_str());
			base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
				FROM_HERE, base::BindOnce(&BrURLLoader::Fail, weak_.GetWeakPtr(), net::ERR_FAILED));
			return;
		}
		if ( plan_.preflight && !preflight_kept(plan_, url_, request->method) ) {
			/* the request waits for its preflight's answer */
			preflighting_ = std::move(request);
			preflight_extra_ = extra;
			auto *c = new PreCtx{ weak_.GetWeakPtr(), base::SingleThreadTaskRunner::GetCurrentDefault() };
			if ( on_main_ ) br_net_fetch_set_answer_on_main(1);
			int started = br_net_fetch_start(url_.c_str(), "OPTIONS", nullptr, 0,
							 preflight_headers(*preflighting_, plan_).c_str(), preflighted, c);
			br_net_fetch_set_answer_on_main(0);
			if ( !started ) {
				delete c;
				base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
					FROM_HERE, base::BindOnce(&BrURLLoader::Fail, weak_.GetWeakPtr(), net::ERR_FAILED));
			}
			return;
		}
		Send(std::move(request), extra);
	}

	/* The preflight's answer, on the thread that asked: the request goes, or fails */
	void Preflighted( int ok, int status, const std::string &headers )
	{
		if ( !preflighting_ ) return;
		std::unique_ptr<network::ResourceRequest> request = std::move(preflighting_);
		if ( !ok || !preflight_allows(plan_, url_, request->method, status, headers) ) {
			Fail();
			return;
		}
		keep_preflight(plan_, url_, headers);
		Send(std::move(request), preflight_extra_);
	}

	/* The request handed to the network stack, with what the plan added */
	void Send( std::unique_ptr<network::ResourceRequest> request, const std::string &extra )
	{
		std::string body = collect_body(*request);
		sent_ = body.size();
		sent_at_ = base::TimeTicks::Now();
		sent_wall_ = base::Time::Now();
		redirect_mode_ = request->redirect_mode;
		std::string hdrs = collect_headers(request->headers, std::string()) + extra + br_ext_line(*request);
		if ( !request->headers.HasHeader("Referer") ) hdrs += referrer_lines(*request);
		/* how it uses the HTTP cache (its cache mode), a line the stack takes off */
		if ( request->load_flags != 0 ) hdrs += "X-TessronOS-Load-Flags: " + base::NumberToString(request->load_flags) + "\r\n";
		/*
		 * a request that does not follow redirects ("manual", "error") ends
		 * at the first, a line the stack takes off: its CORS check is made
		 * on that response, and where it leads is not asked for
		 */
		if ( redirect_mode_ != network::mojom::RedirectMode::kFollow ) hdrs += "X-TessronOS-Redirect: stop\r\n";
		/* the answer comes to the main thread's own queue */
		NetCtx *c = new NetCtx{ this, id_ };
		stream_ = wants_stream(request->headers);
		net_ctx_ = c;
		/*
		 * A script's fetch and XMLHttpRequest take the body through a
		 * data pipe: fetch() reads it only as a stream (its Response's
		 * body), never from data handed to the resource
		 */
		pipe_ = !stream_ && request->destination == network::mojom::RequestDestination::kEmpty;
		/*
		 * and the body goes into the pipe as it comes from the network
		 * (piped_): a script reads a response's body part by part as it
		 * arrives (a stream a page reads, a video fetched in pieces), and
		 * the request no longer holds the page's load once its response
		 * has come
		 */
		piped_ = pipe_;
		if ( on_main_ ) br_net_fetch_set_answer_on_main(1);
		int started = ( stream_ || piped_ )
			? br_net_fetch_stream(url_.c_str(), request->method.c_str(), body.data(),
					      (unsigned)body.size(), hdrs.c_str(), streamed, delivered, c)
			: br_net_fetch_start(url_.c_str(), request->method.c_str(), body.data(),
					     (unsigned)body.size(), hdrs.c_str(), delivered, c);
		br_net_fetch_set_answer_on_main(0);
		if ( !started ) {
			net_ctx_ = nullptr;
			stream_ = false;
			piped_ = false;
			delete c;
			/* told as a failure from a task of its own, never from inside this call */
			base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
				FROM_HERE, base::BindOnce(&BrURLLoader::Fail, weak_.GetWeakPtr(), net::ERR_FAILED));
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
		if ( piped_ ) {
			PipedEnd(ok != 0);
			return;
		}
		if ( stream_ ) {
			/* the end of a stream whose response and body have gone before */
			ended_ = true;
			if ( !ok && !responded_ ) {
				Fail(net_error_of(url_));
				return;
			}
			printf("browser: stream %s ended, %llu bytes\n", url_.c_str(), (unsigned long long)streamed_);
			client->DidFinishLoading(base::TimeTicks::Now(), (int64_t)streamed_, streamed_,
						 (int64_t)streamed_);
			return;
		}
		if ( !ok ) {
			printf("browser: %s: not fetched\n", url_.c_str());
			Fail(net_error_of(url_));
			return;
		}
		if ( ( plan_.cors && !cors_allows(plan_, url.empty() ? url_ : url, headers, "fetch") )
		     || !corp_allows(plan_, url.empty() ? url_ : url, headers) ) {
			Fail(net::ERR_BLOCKED_BY_RESPONSE);
			return;
		}
		printf("browser: fetched %s %d %u bytes %s\n", url_.c_str(), status, len, ctype.c_str());
		base::WeakPtr<BrURLLoader> self = weak_.GetWeakPtr();
		/* the body sent, told before the response (XMLHttpRequest's upload events) */
		if ( sent_ > 0 ) {
			client->DidSendData(sent_, sent_);
			if ( !self ) return;
		}
		if ( !TellRedirects(client) ) return;
		mojo::ScopedDataPipeProducerHandle producer;
		mojo::ScopedDataPipeConsumerHandle consumer;
		if ( pipe_ && len <= kPipeMax
		     && mojo::CreateDataPipe(len > 0 ? len : 1, producer, consumer) == MOJO_RESULT_OK ) {
			/* the whole body in the pipe, which then ends */
			if ( len > 0 ) {
				(void)producer->WriteAllData(base::as_bytes(base::span<const char>(data, len)));
			}
			producer.reset();
			blink::WebURLResponse resp = make_response(url.empty() ? url_ : url, status, ctype, headers, len);
			type_response(&resp, plan_, headers);
			Timing(&resp, headers);
			client->DidReceiveResponse(resp, std::move(consumer), std::nullopt);
			if ( !self ) return;
			client->DidFinishLoading(base::TimeTicks::Now(), (int64_t)len, len, (int64_t)len);
			return;
		}
		blink::WebURLResponse resp = make_response(url.empty() ? url_ : url, status, ctype, headers, len);
		type_response(&resp, plan_, headers);
		Timing(&resp, headers);
		client->DidReceiveResponse(resp, mojo::ScopedDataPipeConsumerHandle(), std::nullopt);
		if ( !self ) return;
		if ( len > 0 ) {
			client->DidReceiveDataForTesting(base::span<const char>(data, len));
			if ( !self ) return;
		}
		client->DidFinishLoading(base::TimeTicks::Now(), (int64_t)len, len, (int64_t)len);
	}

	/* A stream's response (with no length), then each piece of its body as it comes */
	void Part( int first, int status, const std::string &url, const std::string &headers,
		   const std::string &ctype, const char *data, unsigned len )
	{
		blink::URLLoaderClient *client = client_;
		if ( client == nullptr ) return;
		if ( piped_ ) {
			PipedPart(first != 0, status, url, headers, ctype, data, len);
			return;
		}
		if ( first ) {
			printf("browser: stream %s %d %s\n", url_.c_str(), status, ctype.c_str());
			responded_ = true;
			if ( ( plan_.cors && !cors_allows(plan_, url.empty() ? url_ : url, headers, "EventSource") )
			     || !corp_allows(plan_, url.empty() ? url_ : url, headers) ) {
				Fail();
				return;
			}
			if ( !TellRedirects(client) ) return;
			blink::WebURLResponse resp = make_response(url.empty() ? url_ : url, status, ctype,
								   headers, 0);
			resp.SetExpectedContentLength(-1);
			type_response(&resp, plan_, headers);
			client->DidReceiveResponse(resp, mojo::ScopedDataPipeConsumerHandle(), std::nullopt);
			return;
		}
		if ( len == 0 ) return;
		streamed_ += len;
		client->DidReceiveDataForTesting(base::span<const char>(data, len));
	}

	/*
	 * The response's timing: asked for when it was handed to the network
	 * stack, its connection (made or reused, the stack does not say) and
	 * its sending at that time, its headers now, as the resource's timing
	 * (PerformanceResourceTiming) reads them
	 */
	void Timing( blink::WebURLResponse *resp, const std::string &headers )
	{
		if ( sent_at_.is_null() ) return;
		base::TimeTicks now = base::TimeTicks::Now();
		network::mojom::LoadTimingInfo t;
		t.request_start = sent_at_;
		t.request_start_time = sent_wall_;
		t.connect_timing.domain_lookup_start = sent_at_;
		t.connect_timing.domain_lookup_end = sent_at_;
		t.connect_timing.connect_start = sent_at_;
		t.connect_timing.connect_end = sent_at_;
		t.send_start = sent_at_;
		t.send_end = sent_at_;
		t.receive_headers_start = now;
		t.receive_headers_end = now;
		t.receive_non_informational_headers_start = now;
		resp->SetLoadTiming(t);
		/*
		 * whether the page may see the timing's details: a same-origin
		 * response, or one whose Timing-Allow-Origin names the page's origin
		 * or "*", as the network service decides it
		 */
		bool allowed = !plan_.cors && !plan_.opaque;
		if ( !allowed ) {
			if ( std::optional<std::string> tao = header_of(headers, "Timing-Allow-Origin") ) {
				std::string mine = plan_.origin.Serialize();
				for ( const auto &v : base::SplitString(*tao, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY) ) {
					if ( v == "*" || v == mine ) allowed = true;
				}
			}
		}
		resp->SetTimingAllowPassed(allowed);
	}

	/* The network stack told to stop fetching what is still to come (once) */
	void CancelNet()
	{
		if ( ended_ || cancelled_ || net_ctx_ == nullptr ) return;
		cancelled_ = true;
		br_net_fetch_cancel(net_ctx_);
	}

	/*
	 * A script's response whose body goes into its data pipe as it comes:
	 * the response with the pipe at the first piece (after the CORS check,
	 * the upload and the redirects), then each piece written as the pipe
	 * takes it; what it cannot take yet waits, and is written when the pipe
	 * becomes writable again.
	 */
	void PipedPart( bool first, int status, const std::string &url, const std::string &headers,
			const std::string &ctype, const char *data, unsigned len )
	{
		blink::URLLoaderClient *client = client_;
		if ( first ) {
			responded_ = true;
			if ( ( plan_.cors && !cors_allows(plan_, url.empty() ? url_ : url, headers, "fetch") )
			     || !corp_allows(plan_, url.empty() ? url_ : url, headers) ) {
				piped_failed_ = true;
				CancelNet();
				Fail();
				return;
			}
			printf("browser: fetching %s %d %s\n", url_.c_str(), status, ctype.c_str());
			base::WeakPtr<BrURLLoader> self = weak_.GetWeakPtr();
			if ( sent_ > 0 ) {
				client->DidSendData(sent_, sent_);
				if ( !self ) return;
			}
			if ( !TellRedirects(client) ) {
				/* ended at a redirect (or failed there): the rest of the body is not wanted */
				if ( !self ) return;
				piped_failed_ = true;
				CancelNet();
				return;
			}
			int64_t length = -1;
			if ( std::optional<std::string> cl = header_of(headers, "content-length") ) {
				if ( !base::StringToInt64(*cl, &length) ) length = -1;
			}
			held_resp_ = make_response(url.empty() ? url_ : url, status, ctype, headers, 0);
			held_resp_.SetExpectedContentLength(length);
			type_response(&held_resp_, plan_, headers);
			Timing(&held_resp_, headers);
			/*
			 * The response is held for a moment: a body that ends within it
			 * is handed over whole with it, in one task, as a body that came
			 * at once is; a longer one goes on part by part from then
			 */
			held_ = true;
			base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
				FROM_HERE, base::BindOnce(&BrURLLoader::Release, weak_.GetWeakPtr()), base::Milliseconds(kHoldMs));
			return;
		}
		if ( piped_failed_ || len == 0 ) return;
		pending_.append(data, len);
		streamed_ += len;
		if ( !held_ ) Pump();
	}

	/* The response held, handed over with its pipe; the body goes on into it as it comes */
	void Release()
	{
		if ( !held_ || piped_failed_ || client_ == nullptr ) return;
		held_ = false;
		mojo::ScopedDataPipeConsumerHandle consumer;
		if ( mojo::CreateDataPipe(kPipeChunk, producer_, consumer) != MOJO_RESULT_OK ) {
			piped_failed_ = true;
			CancelNet();
			Fail();
			return;
		}
		base::WeakPtr<BrURLLoader> self = weak_.GetWeakPtr();
		client_->DidReceiveResponse(held_resp_, std::move(consumer), std::nullopt);
		if ( !self ) return;
		Pump();
	}

	/* The body's end (or its failure) */
	void PipedEnd( bool ok )
	{
		ended_ = true;
		if ( piped_failed_ ) return;
		if ( !responded_ ) {
			if ( !ok ) printf("browser: %s: not fetched\n", url_.c_str());
			Fail(ok ? net::ERR_FAILED : net_error_of(url_));
			return;
		}
		if ( held_ ) {
			/* the whole body in while the response was held: both handed over now */
			held_ = false;
			if ( !ok ) {
				Fail(net_error_of(url_));
				return;
			}
			size_t n = pending_.size();
			mojo::ScopedDataPipeProducerHandle producer;
			mojo::ScopedDataPipeConsumerHandle consumer;
			if ( n > kPipeMax || mojo::CreateDataPipe(n > 0 ? (uint32_t)n : 1u, producer, consumer) != MOJO_RESULT_OK ) {
				Fail();
				return;
			}
			if ( n > 0 ) (void)producer->WriteAllData(base::as_bytes(base::span<const char>(pending_.data(), n)));
			producer.reset();
			pending_.clear();
			blink::URLLoaderClient *client = client_;
			base::WeakPtr<BrURLLoader> self = weak_.GetWeakPtr();
			client->DidReceiveResponse(held_resp_, std::move(consumer), std::nullopt);
			if ( !self ) return;
			client->DidFinishLoading(base::TimeTicks::Now(), (int64_t)n, n, (int64_t)n);
			return;
		}
		piped_ok_ = ok;
		piped_done_ = true;
		Pump();
	}

	/* What waits written into the pipe, as much as it takes; the load ended when all of it is in */
	void Pump()
	{
		while ( pending_at_ < pending_.size() && producer_.is_valid() ) {
			size_t wrote = 0;
			MojoResult r = producer_->WriteData(
				base::as_bytes(base::span<const char>(pending_.data() + pending_at_, pending_.size() - pending_at_)),
				MOJO_WRITE_DATA_FLAG_NONE, wrote);
			if ( r == MOJO_RESULT_SHOULD_WAIT ) {
				if ( !watcher_ ) {
					watcher_ = std::make_unique<mojo::SimpleWatcher>(
						FROM_HERE, mojo::SimpleWatcher::ArmingPolicy::AUTOMATIC);
					watcher_->Watch(producer_.get(), MOJO_HANDLE_SIGNAL_WRITABLE,
							base::BindRepeating([]( base::WeakPtr<BrURLLoader> l, MojoResult ) {
								if ( l ) l->Pump();
							}, weak_.GetWeakPtr()));
				}
				return;
			}
			if ( r != MOJO_RESULT_OK ) {
				/*
				 * the reader let go of the body: the rest is not fetched,
				 * and the load ends with what it had
				 */
				watcher_.reset();
				producer_.reset();
				pending_.clear();
				pending_at_ = 0;
				piped_failed_ = true;
				CancelNet();
				if ( client_ != nullptr ) {
					client_->DidFinishLoading(base::TimeTicks::Now(), (int64_t)streamed_, streamed_,
								  (int64_t)streamed_);
				}
				return;
			}
			pending_at_ += wrote;
		}
		if ( pending_at_ >= pending_.size() ) {
			pending_.clear();
			pending_at_ = 0;
		}
		if ( !piped_done_ || !pending_.empty() ) return;
		piped_done_ = false;
		watcher_.reset();
		producer_.reset();
		if ( client_ == nullptr ) return;
		if ( !piped_ok_ ) {
			client_->DidFail(blink::WebURLError(net::ERR_FAILED, web_url(url_)), base::TimeTicks::Now(),
					 (int64_t)streamed_, streamed_, streamed_);
			return;
		}
		client_->DidFinishLoading(base::TimeTicks::Now(), (int64_t)streamed_, streamed_, (int64_t)streamed_);
	}

	/*
	 * The redirects the network stack followed, told to the client in turn
	 * as the network service tells them, from inside the answer's callback
	 * (br_net_fetch_redirect). A request whose redirect mode is "error"
	 * fails at the first; one whose mode is "manual" ends with the first
	 * redirect's response, as an opaque redirect. False when the response
	 * is not to be given (the client stopped at a redirect, or it ended so).
	 */
	bool TellRedirects( blink::URLLoaderClient *client )
	{
		base::WeakPtr<BrURLLoader> self = weak_.GetWeakPtr();
		int n = br_net_fetch_redirects();
		for ( int i = 0; i < n; i++ ) {
			const char *u = "", *h = "", *nu = "", *nm = "", *nr = "";
			int st = 0;
			if ( !br_net_fetch_redirect(i, &u, &st, &h, &nu, &nm, &nr) ) break;
			/* a CORS request's redirect is checked as its final response is */
			if ( plan_.cors && !cors_allows(plan_, u, h, "fetch") ) {
				Fail();
				return false;
			}
			if ( redirect_mode_ == network::mojom::RedirectMode::kError ) {
				printf("browser: %s: redirected, and the request does not follow redirects\n", url_.c_str());
				Fail();
				return false;
			}
			blink::WebURLResponse rr = make_response(u, st, "", h, 0);
			if ( redirect_mode_ == network::mojom::RedirectMode::kManual ) {
				rr.SetType(network::mojom::FetchResponseType::kOpaqueRedirect);
				mojo::ScopedDataPipeProducerHandle producer;
				mojo::ScopedDataPipeConsumerHandle consumer;
				if ( pipe_ ) (void)mojo::CreateDataPipe(1, producer, consumer);
				producer.reset();
				client->DidReceiveResponse(rr, std::move(consumer), std::nullopt);
				if ( !self ) return false;
				client->DidFinishLoading(base::TimeTicks::Now(), 0, 0, 0);
				return false;
			}
			bool raw = false;
			std::vector<std::string> removed;
			net::HttpRequestHeaders modified;
			if ( !client->WillFollowRedirect(web_url(nu), net::SiteForCookies::FromUrl(GURL(nu)),
							 blink::WebString::FromUtf8(nr), referrer_policy_,
							 blink::WebString::FromUtf8(nm), rr, raw, &removed, modified, false) ) {
				return false;
			}
			if ( !self ) return false;
		}
		return true;
	}

private:
	blink::URLLoaderClient		*client_ = nullptr;
	std::unique_ptr<network::ResourceRequest> waiting_;	/* asked of the page's service worker */
	std::string			url_;
	unsigned			id_;
	bool				on_main_;
	std::string			sw_client_;		/* the document loading, as a client of the service workers */
	network::mojom::CrossOriginEmbedderPolicyValue coep_;	/* the document's Cross-Origin-Embedder-Policy */
	static constexpr unsigned	kPipeMax = 64u << 20;	/* the most a pipe is made to hold */
	bool				pipe_ = false;		/* the body goes through a data pipe */
	bool				stream_ = false;	/* an event stream, handed on in pieces */
	static constexpr uint32_t	kPipeChunk = 512u << 10;	/* the size of a pipe a body goes through as it comes */
	bool				piped_ = false;		/* the body goes into the pipe as it comes */
	bool				piped_failed_ = false;	/* refused or ended at its response */
	bool				piped_done_ = false;	/* the network's end came, the pipe still taking it */
	bool				piped_ok_ = true;
	bool				cancelled_ = false;	/* the stack told to stop (CancelNet) */
	static constexpr int		kHoldMs = 100;		/* how long a response waits for its body to end */
	bool				held_ = false;		/* the response waits (Release) */
	blink::WebURLResponse		held_resp_;
	mojo::ScopedDataPipeProducerHandle	producer_;
	std::unique_ptr<mojo::SimpleWatcher>	watcher_;
	std::string			pending_;		/* what the pipe has not taken yet */
	size_t				pending_at_ = 0;
	bool				responded_ = false;
	bool				ended_ = false;
	void				*net_ctx_ = nullptr;	/* what the stream was asked with */
	uint64_t			streamed_ = 0;
	uint64_t			sent_ = 0;		/* the bytes of the request's body */
	base::TimeTicks			sent_at_;		/* when it was handed to the network stack */
	base::Time			sent_wall_;
	network::mojom::RedirectMode	redirect_mode_ = network::mojom::RedirectMode::kFollow;
	network::mojom::ReferrerPolicy	referrer_policy_ = network::mojom::ReferrerPolicy::kDefault;	/* told with a redirect (the stack applied the request's) */
	BrPlan				plan_;			/* what the network service would add and check */
	std::unique_ptr<network::ResourceRequest> preflighting_;	/* waiting for its preflight */
	std::string			preflight_extra_;
	base::WeakPtrFactory<BrURLLoader> weak_{this};

	/*
	 * The load failed with the network stack's error when it has one:
	 * Blink acts on some of them, as a font's first request, made only
	 * from the HTTP cache (cache-aware loading), that it asks again from
	 * the network when it fails with ERR_CACHE_MISS
	 */
	void Fail( int error = net::ERR_FAILED )
	{
		if ( client_ == nullptr ) return;
		client_->DidFail(blink::WebURLError(error, web_url(url_)), base::TimeTicks::Now(),
				 0, 0, 0);
	}
};

/*
 * An answer comes to the thread that asked, so a loader found alive here
 * is that thread's and stays alive while it is answered
 */
bool is_live( BrURLLoader *l )
{
	std::lock_guard<std::mutex> hold(live_lock());
	return live_loaders().count(l) != 0;
}

void delivered( void *ctx, int ok, int status, const char *url, const char *headers,
		const char *ctype, const char *data, unsigned len )
{
	std::unique_ptr<NetCtx> c(static_cast<NetCtx *>(ctx));
	if ( c == nullptr ) return;
	if ( !is_live(c->loader) || c->loader->id() != c->id ) return;
	c->loader->Deliver(ok, status, url != nullptr ? url : "", headers != nullptr ? headers : "",
			   ctype != nullptr ? ctype : "", data, len);
}

void preflighted( void *ctx, int ok, int status, const char *, const char *headers,
		  const char *, const char *, unsigned )
{
	std::unique_ptr<PreCtx> c(static_cast<PreCtx *>(ctx));
	if ( c == nullptr ) return;
	std::string h = headers != nullptr ? headers : "";
	c->runner->PostTask(FROM_HERE, base::BindOnce(&BrURLLoader::Preflighted, c->loader, ok, status, h));
}

/* A piece of a stream: its context stays until the end is told (delivered) */
void streamed( void *ctx, int first, int status, const char *url, const char *headers,
	       const char *ctype, const char *data, unsigned len )
{
	NetCtx *c = static_cast<NetCtx *>(ctx);
	if ( c == nullptr ) return;
	if ( !is_live(c->loader) || c->loader->id() != c->id ) return;
	c->loader->Part(first, status, url != nullptr ? url : "", headers != nullptr ? headers : "",
			ctype != nullptr ? ctype : "", data, len);
}

}  // namespace

std::unique_ptr<blink::URLLoader> br_blink_url_loader()
{
	return std::make_unique<BrURLLoader>();
}

/*
 * A frame's loader for its document, a client of the service workers by
 * its UUID, held to its Cross-Origin-Embedder-Policy (br_blink.cc)
 */
std::unique_ptr<blink::URLLoader> br_blink_frame_url_loader( const std::string &sw_client,
							    network::mojom::CrossOriginEmbedderPolicyValue coep )
{
	return std::make_unique<BrURLLoader>(true, sw_client, coep);
}

/* A worker's loader (port/blink/worker_tessronos.cc) */
std::unique_ptr<blink::URLLoader> br_blink_worker_url_loader()
{
	return std::make_unique<BrURLLoader>(false);
}
