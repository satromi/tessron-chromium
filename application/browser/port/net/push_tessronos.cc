/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	push_tessronos.cc
 *	ブラウザ: Web Push's subscriptions and the messages pushed to them,
 *	through a push service that speaks autopush's protocol (design 14.4.11).
 *
 *	The browser keeps one WebSocket to the push service (by default
 *	Mozilla's, wss://push.services.mozilla.com/; another of the same
 *	protocol when the store or the start-up line names it), opened on the
 *	network thread when there is a subscription and kept open, made again
 *	after it drops. It says hello with the user agent's id (uaid) the
 *	service gave before, so that the subscriptions go on.
 *
 *	A subscription is a channel of the service: registered with the
 *	application server's key, the service answers with the endpoint the
 *	application server posts its messages to. The browser makes the
 *	subscription's keys (P-256, and the 16 bytes of its authentication
 *	secret), which the page hands to its server. A message the service
 *	brings is decrypted as RFC 8291 says (aes128gcm: ECDH with the
 *	sender's key, HKDF, AES-128-GCM), acknowledged, and given to whoever
 *	delivers it to the service worker of the subscription's scope.
 *
 *	What the browser keeps between runs is in the store "push": the
 *	service's address and uaid, and each subscription by its scope.
 */

#include <stdio.h>
#include <string.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/base64url.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/time/time.h"
#include "base/uuid.h"
#include "base/values.h"
#include "net/base/io_buffer.h"
#include "net/base/isolation_info.h"
#include "net/base/net_errors.h"
#include "net/http/http_request_headers.h"
#include "net/storage_access_api/status.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "net/url_request/url_request_context.h"
#include "net/websockets/websocket_channel.h"
#include "net/websockets/websocket_event_interface.h"
#include "net/websockets/websocket_frame.h"
#include "net/websockets/websocket_handshake_request_info.h"
#include "net/websockets/websocket_handshake_response_info.h"
#include "third_party/boringssl/src/include/openssl/aead.h"
#include "third_party/boringssl/src/include/openssl/bn.h"
#include "third_party/boringssl/src/include/openssl/digest.h"
#include "third_party/boringssl/src/include/openssl/ec.h"
#include "third_party/boringssl/src/include/openssl/ec_key.h"
#include "third_party/boringssl/src/include/openssl/ecdh.h"
#include "third_party/boringssl/src/include/openssl/hkdf.h"
#include "third_party/boringssl/src/include/openssl/nid.h"
#include "third_party/boringssl/src/include/openssl/rand.h"
#include "url/gurl.h"
#include "url/origin.h"

#include "../../br.h"
#include "br_store.h"

net::URLRequestContext *br_net_context();
void br_net_post( base::OnceClosure work );
void br_net_post_delayed( base::OnceClosure work, int ms );

std::string br_push_server;		/* the start-up line "push=" (br_main.cc); "" the store's or Mozilla's */
std::string br_push_test_key;		/* "pushkey=": the subscriptions' private key and secret, for tests */

namespace {

constexpr char kMozilla[] = "wss://push.services.mozilla.com/";

/* A subscription: a channel of the service, for a service worker's scope */
struct Sub {
	std::string	scope;
	std::string	channel;	/* its id at the service */
	std::string	endpoint;	/* where the application server posts */
	std::string	app_key;	/* the application server's key, its bytes */
	std::string	priv;		/* the subscription's P-256 key: the private 32 bytes */
	std::string	pub;		/* and the public 65 */
	std::string	auth;		/* the authentication secret, 16 bytes */
	bool		registered = false;	/* the service has answered for it */
};

using Done = std::function<void( bool ok, const BrPushSub &sub, const std::string &why )>;

std::mutex			g_lock;
std::string			g_server;		/* the service's address in use */
std::string			g_uaid;
std::map<std::string, Sub>	g_subs;			/* by scope */
std::map<std::string, Done>	g_waiting;		/* channel -> the subscriber waiting for its endpoint */
BrPushDeliver			g_deliver;
bool				g_loaded = false;
int				g_store = -1;

/* ---------------------------------------------------------------- what is kept */

std::string b64( const std::string &s )
{
	std::string out;
	base::Base64UrlEncode(s, base::Base64UrlEncodePolicy::OMIT_PADDING, &out);
	return out;
}

std::string unb64( const std::string &s )
{
	std::string out;
	if ( !base::Base64UrlDecode(s, base::Base64UrlDecodePolicy::IGNORE_PADDING, &out) ) out.clear();
	return out;
}

/* The store's bytes now: the service, the uaid and each subscription, as JSON */
int write_store( void *, unsigned char **data, int *len )
{
	std::lock_guard<std::mutex> hold(g_lock);
	base::DictValue d;
	d.Set("server", g_server);
	d.Set("uaid", g_uaid);
	base::ListValue subs;
	for ( const auto &kv : g_subs ) {
		const Sub &s = kv.second;
		if ( !s.registered ) continue;
		base::DictValue e;
		e.Set("scope", s.scope);
		e.Set("channel", s.channel);
		e.Set("endpoint", s.endpoint);
		e.Set("app_key", b64(s.app_key));
		e.Set("priv", b64(s.priv));
		e.Set("pub", b64(s.pub));
		e.Set("auth", b64(s.auth));
		subs.Append(std::move(e));
	}
	d.Set("subs", std::move(subs));
	std::string json;
	base::JSONWriter::Write(d, &json);
	*data = (unsigned char *)malloc(json.size() + 1);
	if ( *data == nullptr ) return 1;
	memcpy(*data, json.data(), json.size());
	*len = (int)json.size();
	return 0;
}

void load_store()
{
	if ( g_loaded ) return;
	g_loaded = true;
	g_store = br_store_register("push", &write_store, nullptr, 500);
	unsigned char *data = nullptr;
	int len = 0;
	if ( br_store_load("push", &data, &len) == 0 && data != nullptr ) {
		std::optional<base::Value> v = base::JSONReader::Read(std::string((char *)data, (size_t)len),
								      base::JSON_PARSE_RFC);
		if ( v && v->is_dict() ) {
			const base::DictValue &d = v->GetDict();
			if ( const std::string *s = d.FindString("server") ) g_server = *s;
			if ( const std::string *s = d.FindString("uaid") ) g_uaid = *s;
			if ( const base::ListValue *l = d.FindList("subs") ) {
				for ( const base::Value &e : *l ) {
					if ( !e.is_dict() ) continue;
					const base::DictValue &x = e.GetDict();
					Sub s;
					auto str = [&]( const char *k ) {
						const std::string *p = x.FindString(k);
						return p ? *p : std::string();
					};
					s.scope = str("scope");
					s.channel = str("channel");
					s.endpoint = str("endpoint");
					s.app_key = unb64(str("app_key"));
					s.priv = unb64(str("priv"));
					s.pub = unb64(str("pub"));
					s.auth = unb64(str("auth"));
					s.registered = true;
					if ( !s.scope.empty() && !s.channel.empty() ) g_subs[s.scope] = s;
				}
			}
		}
		printf("browser: push: %zu subscriptions read back\n", g_subs.size());
	}
	free(data);
	if ( !br_push_server.empty() ) g_server = br_push_server;
	if ( g_server.empty() ) g_server = kMozilla;
}

void dirty()
{
	if ( g_store >= 0 ) br_store_dirty(g_store);
}

BrPushSub public_of( const Sub &s )
{
	BrPushSub p;
	p.endpoint = s.endpoint;
	p.p256dh = s.pub;
	p.auth = s.auth;
	p.app_key = s.app_key;
	return p;
}

/* ---------------------------------------------------------------- the keys */

/* A new P-256 key: its private 32 bytes and its public 65 (uncompressed) */
bool new_key( std::string *priv, std::string *pub )
{
	bssl::UniquePtr<EC_KEY> k(EC_KEY_new_by_curve_name(NID_X9_62_prime256v1));
	if ( !k || !EC_KEY_generate_key(k.get()) ) return false;
	uint8_t p[32], q[65];
	if ( !BN_bn2bin_padded(p, sizeof(p), EC_KEY_get0_private_key(k.get())) ) return false;
	if ( EC_POINT_point2oct(EC_KEY_get0_group(k.get()), EC_KEY_get0_public_key(k.get()),
				POINT_CONVERSION_UNCOMPRESSED, q, sizeof(q), nullptr) != sizeof(q) ) {
		return false;
	}
	priv->assign((char *)p, sizeof(p));
	pub->assign((char *)q, sizeof(q));
	return true;
}

/* The public 65 bytes of a P-256 private key */
bool pub_of( const std::string &priv, std::string *pub )
{
	bssl::UniquePtr<EC_KEY> k(EC_KEY_new_by_curve_name(NID_X9_62_prime256v1));
	if ( !k ) return false;
	const EC_GROUP *g = EC_KEY_get0_group(k.get());
	bssl::UniquePtr<BIGNUM> d(BN_bin2bn((const uint8_t *)priv.data(), priv.size(), nullptr));
	bssl::UniquePtr<EC_POINT> p(EC_POINT_new(g));
	if ( !d || !p || !EC_POINT_mul(g, p.get(), d.get(), nullptr, nullptr, nullptr) ) return false;
	uint8_t q[65];
	if ( EC_POINT_point2oct(g, p.get(), POINT_CONVERSION_UNCOMPRESSED, q, sizeof(q), nullptr) != sizeof(q) ) {
		return false;
	}
	pub->assign((char *)q, sizeof(q));
	return true;
}

/* The ECDH secret of a subscription's key and a sender's public key */
bool ecdh( const std::string &priv, const std::string &peer, uint8_t out[32] )
{
	bssl::UniquePtr<EC_KEY> k(EC_KEY_new_by_curve_name(NID_X9_62_prime256v1));
	if ( !k ) return false;
	const EC_GROUP *g = EC_KEY_get0_group(k.get());
	bssl::UniquePtr<BIGNUM> d(BN_bin2bn((const uint8_t *)priv.data(), priv.size(), nullptr));
	bssl::UniquePtr<EC_POINT> p(EC_POINT_new(g));
	if ( !d || !p || !EC_KEY_set_private_key(k.get(), d.get()) ) return false;
	if ( !EC_POINT_oct2point(g, p.get(), (const uint8_t *)peer.data(), peer.size(), nullptr) ) return false;
	return ECDH_compute_key(out, 32, p.get(), k.get(), nullptr) == 32;
}

/*
 * A message's body decrypted (RFC 8291 with RFC 8188's aes128gcm): its
 * header (the salt, the record size, the sender's key), then records of
 * AES-128-GCM, each ending in its delimiter and padding
 */
bool decrypt( const Sub &s, const std::string &body, std::string *out )
{
	const uint8_t *b = (const uint8_t *)body.data();
	if ( body.size() < 21 ) return false;
	uint32_t rs = ( (uint32_t)b[16] << 24 ) | ( (uint32_t)b[17] << 16 ) | ( (uint32_t)b[18] << 8 ) | b[19];
	size_t idlen = b[20];
	if ( idlen != 65 || body.size() < 21 + idlen || rs < 18 ) return false;
	std::string salt((const char *)b, 16), sender((const char *)b + 21, idlen);

	uint8_t secret[32], ikm[32], cek[16], nonce[12];
	if ( !ecdh(s.priv, sender, secret) ) return false;
	std::string info = std::string("WebPush: info", 13) + std::string(1, '\0') + s.pub + sender;
	if ( !HKDF(ikm, sizeof(ikm), EVP_sha256(), secret, sizeof(secret), (const uint8_t *)s.auth.data(),
		   s.auth.size(), (const uint8_t *)info.data(), info.size()) ) {
		return false;
	}
	static const char kCek[] = "Content-Encoding: aes128gcm";
	static const char kNonce[] = "Content-Encoding: nonce";
	if ( !HKDF(cek, sizeof(cek), EVP_sha256(), ikm, sizeof(ikm), (const uint8_t *)salt.data(), salt.size(),
		   (const uint8_t *)kCek, sizeof(kCek)) ||
	     !HKDF(nonce, sizeof(nonce), EVP_sha256(), ikm, sizeof(ikm), (const uint8_t *)salt.data(), salt.size(),
		   (const uint8_t *)kNonce, sizeof(kNonce)) ) {
		return false;
	}
	bssl::ScopedEVP_AEAD_CTX ctx;
	if ( !EVP_AEAD_CTX_init(ctx.get(), EVP_aead_aes_128_gcm(), cek, sizeof(cek), EVP_AEAD_DEFAULT_TAG_LENGTH,
				nullptr) ) {
		return false;
	}
	out->clear();
	size_t at = 21 + idlen;
	for ( uint64_t seq = 0; at < body.size(); seq++ ) {
		size_t n = std::min<size_t>(rs, body.size() - at);
		uint8_t iv[12];
		memcpy(iv, nonce, sizeof(iv));
		for ( int i = 0; i < 8; i++ ) iv[11 - i] ^= (uint8_t)( seq >> ( 8 * i ) );
		std::string plain(n, '\0');
		size_t got = 0;
		if ( !EVP_AEAD_CTX_open(ctx.get(), (uint8_t *)plain.data(), &got, plain.size(), iv, sizeof(iv),
				       b + at, n, nullptr, 0) ) {
			return false;
		}
		plain.resize(got);
		/* the padding taken off: zeros back to the delimiter (2 for the last record) */
		size_t end = plain.size();
		while ( end > 0 && plain[end - 1] == 0 ) end--;
		if ( end == 0 ) return false;
		bool last = ( plain[end - 1] == 2 );
		out->append(plain.data(), end - 1);
		at += n;
		if ( last ) break;
	}
	return true;
}

/* A parameter of a header's value ("salt=..." in "keyid=a;salt=..."), "" when it has none */
std::string param_of( const std::string &v, const std::string &name )
{
	size_t at = 0;
	while ( at < v.size() ) {
		size_t end = v.find_first_of(";,", at);
		if ( end == std::string::npos ) end = v.size();
		std::string part = v.substr(at, end - at);
		size_t b = part.find_first_not_of(' ');
		if ( b != std::string::npos ) part = part.substr(b);
		if ( part.compare(0, name.size() + 1, name + "=") == 0 ) {
			std::string val = part.substr(name.size() + 1);
			if ( val.size() >= 2 && val.front() == '"' && val.back() == '"' ) val = val.substr(1, val.size() - 2);
			return val;
		}
		at = end + 1;
	}
	return std::string();
}

/*
 * A message in the older encoding, aesgcm (the drafts before RFC 8291;
 * the salt and the sender's key come in the Encryption and Crypto-Key
 * headers): the ECDH secret mixed with the auth secret, the key and the
 * nonce made with a context of both public keys, records of 'rs' bytes
 * (4096 unless said) and 16 of tag, each beginning with 2 bytes that say
 * how much padding follows
 */
bool decrypt_aesgcm( const Sub &s, const std::string &body, const std::string &encryption,
		     const std::string &crypto_key, std::string *out )
{
	std::string salt = unb64(param_of(encryption, "salt")), sender = unb64(param_of(crypto_key, "dh"));
	std::string rss = param_of(encryption, "rs");
	size_t rs = rss.empty() ? 4096 : (size_t)atol(rss.c_str());
	if ( salt.size() != 16 || sender.size() != 65 || s.pub.size() != 65 || rs < 2 ) return false;

	uint8_t secret[32], ikm[32], cek[16], nonce[12];
	if ( !ecdh(s.priv, sender, secret) ) return false;
	static const char kAuth[] = "Content-Encoding: auth";
	if ( !HKDF(ikm, sizeof(ikm), EVP_sha256(), secret, sizeof(secret), (const uint8_t *)s.auth.data(),
		   s.auth.size(), (const uint8_t *)kAuth, sizeof(kAuth)) ) {
		return false;
	}
	std::string context = std::string("P-256", 6);
	context += std::string(1, '\0') + std::string(1, (char)65) + s.pub;
	context += std::string(1, '\0') + std::string(1, (char)65) + sender;
	std::string ki = std::string("Content-Encoding: aesgcm", 25) + context;
	std::string ni = std::string("Content-Encoding: nonce", 24) + context;
	if ( !HKDF(cek, sizeof(cek), EVP_sha256(), ikm, sizeof(ikm), (const uint8_t *)salt.data(), salt.size(),
		   (const uint8_t *)ki.data(), ki.size()) ||
	     !HKDF(nonce, sizeof(nonce), EVP_sha256(), ikm, sizeof(ikm), (const uint8_t *)salt.data(), salt.size(),
		   (const uint8_t *)ni.data(), ni.size()) ) {
		return false;
	}
	bssl::ScopedEVP_AEAD_CTX ctx;
	if ( !EVP_AEAD_CTX_init(ctx.get(), EVP_aead_aes_128_gcm(), cek, sizeof(cek), EVP_AEAD_DEFAULT_TAG_LENGTH,
				nullptr) ) {
		return false;
	}
	const uint8_t *b = (const uint8_t *)body.data();
	out->clear();
	size_t at = 0;
	for ( uint64_t seq = 0; at < body.size(); seq++ ) {
		size_t n = std::min<size_t>(rs + 16, body.size() - at);
		uint8_t iv[12];
		memcpy(iv, nonce, sizeof(iv));
		for ( int i = 0; i < 8; i++ ) iv[11 - i] ^= (uint8_t)( seq >> ( 8 * i ) );
		std::string plain(n, '\0');
		size_t got = 0;
		if ( !EVP_AEAD_CTX_open(ctx.get(), (uint8_t *)plain.data(), &got, plain.size(), iv, sizeof(iv),
				       b + at, n, nullptr, 0) || got < 2 ) {
			return false;
		}
		size_t pad = ( (size_t)(uint8_t)plain[0] << 8 ) | (uint8_t)plain[1];
		if ( 2 + pad > got ) return false;
		out->append(plain.data() + 2 + pad, got - 2 - pad);
		at += n;
	}
	return true;
}

/* ---------------------------------------------------------------- the connection */

class Conn;
Conn		*g_conn = nullptr;		/* on the network thread */
bool		g_connecting = false;
int		g_retry_ms = 2000;

void push_connect();
void send_json( const base::DictValue &d );

class Events final : public net::WebSocketEventInterface {
public:
	void OnCreateURLRequest( net::URLRequest * ) override {}
	int OnURLRequestConnected( net::URLRequest *, const net::TransportInfo &, net::CompletionOnceCallback ) override
	{
		return net::OK;
	}
	void OnAddChannelResponse( std::unique_ptr<net::WebSocketHandshakeResponseInfo>, const std::string &,
				   const std::string & ) override;
	void OnDataFrame( bool fin, WebSocketMessageType, base::span<const char> payload ) override;
	bool HasPendingDataFrames() override { return false; }
	void OnSendDataFrameDone() override {}
	void OnClosingHandshake() override {}
	void OnDropChannel( bool, uint16_t code, const std::string &reason ) override;
	void OnFailChannel( const std::string &message, int, std::optional<int> ) override;
	void OnStartOpeningHandshake( std::unique_ptr<net::WebSocketHandshakeRequestInfo> ) override {}
	void OnSSLCertificateError( std::unique_ptr<SSLErrorCallbacks> callbacks, const GURL &, int net_error,
				    const net::SSLInfo &ssl_info, bool ) override
	{
		callbacks->CancelSSLRequest(net_error, &ssl_info);
	}
	int OnAuthRequired( const net::AuthChallengeInfo &, scoped_refptr<net::HttpResponseHeaders>,
			    const net::IPEndPoint &, base::OnceCallback<void( const net::AuthCredentials * )>,
			    std::optional<net::AuthCredentials> *credentials ) override
	{
		*credentials = std::nullopt;
		return net::OK;
	}

private:
	std::string	message_;
};

class Conn {
public:
	std::unique_ptr<net::WebSocketChannel>	channel;
	bool					open = false;	/* hello answered */
};

void drop_conn( const char *why )
{
	printf("browser: push: the service's connection dropped (%s); again in %d s\n", why, g_retry_ms / 1000);
	Conn *c = g_conn;
	g_conn = nullptr;
	g_connecting = false;
	/* deleted from a task of its own: this is a call of its channel's */
	br_net_post(base::BindOnce([]( Conn *c ) { delete c; }, c));
	br_net_post_delayed(base::BindOnce(&push_connect), g_retry_ms);
	g_retry_ms = std::min(g_retry_ms * 2, 300000);
}

void Events::OnAddChannelResponse( std::unique_ptr<net::WebSocketHandshakeResponseInfo>, const std::string &,
				   const std::string & )
{
	std::string uaid;
	{
		std::lock_guard<std::mutex> hold(g_lock);
		uaid = g_uaid;
	}
	base::DictValue hello;
	hello.Set("messageType", "hello");
	hello.Set("uaid", uaid);
	hello.Set("use_webpush", true);
	hello.Set("channelIDs", base::ListValue());
	br_net_post(base::BindOnce([]( base::DictValue hello ) {
		if ( g_conn == nullptr ) return;
		(void)g_conn->channel->ReadFrames();
		send_json(hello);
	}, std::move(hello)));
}

void register_channel( const Sub &s )
{
	base::DictValue r;
	r.Set("messageType", "register");
	r.Set("channelID", s.channel);
	if ( !s.app_key.empty() ) r.Set("key", b64(s.app_key));
	send_json(r);
}

/* A message of the service: hello, register, unregister, notification, ping */
void take( const std::string &json )
{
	std::optional<base::Value> v = base::JSONReader::Read(json, base::JSON_PARSE_RFC);
	if ( !v || !v->is_dict() ) return;
	const base::DictValue &d = v->GetDict();
	const std::string *type = d.FindString("messageType");
	if ( type == nullptr ) return;		/* a ping's answer */
	int status = d.FindInt("status").value_or(200);

	if ( *type == "hello" ) {
		const std::string *uaid = d.FindString("uaid");
		std::vector<Sub> again;
		{
			std::lock_guard<std::mutex> hold(g_lock);
			if ( uaid != nullptr && *uaid != g_uaid ) {
				/* a new uaid: the service knows none of the old channels */
				if ( !g_uaid.empty() ) printf("browser: push: the service gave a new uaid\n");
				g_uaid = *uaid;
				for ( auto &kv : g_subs ) kv.second.registered = false;
			}
			for ( auto &kv : g_subs ) {
				if ( !kv.second.registered ) again.push_back(kv.second);
			}
		}
		dirty();
		if ( g_conn != nullptr ) g_conn->open = true;
		g_retry_ms = 2000;
		printf("browser: push: connected to %s\n", g_server.c_str());
		for ( const Sub &s : again ) register_channel(s);
		return;
	}
	if ( *type == "register" ) {
		const std::string *ch = d.FindString("channelID");
		const std::string *ep = d.FindString("pushEndpoint");
		if ( ch == nullptr ) return;
		Done done;
		BrPushSub out;
		bool ok = ( status == 200 && ep != nullptr );
		{
			std::lock_guard<std::mutex> hold(g_lock);
			for ( auto &kv : g_subs ) {
				if ( kv.second.channel != *ch ) continue;
				if ( ok ) {
					kv.second.endpoint = *ep;
					kv.second.registered = true;
					out = public_of(kv.second);
				} else {
					g_subs.erase(kv.first);
				}
				break;
			}
			auto w = g_waiting.find(*ch);
			if ( w != g_waiting.end() ) {
				done = std::move(w->second);
				g_waiting.erase(w);
			}
		}
		dirty();
		printf("browser: push: channel %s %s\n", ch->c_str(), ok ? "registered" : "refused");
		if ( done ) done(ok, out, ok ? std::string() : "the push service refused the subscription");
		return;
	}
	if ( *type == "notification" ) {
		const std::string *ch = d.FindString("channelID");
		const std::string *ver = d.FindString("version");
		const std::string *data = d.FindString("data");
		const base::DictValue *headers = d.FindDict("headers");
		const std::string *enc = headers ? headers->FindString("encoding") : nullptr;
		const std::string *encryption = headers ? headers->FindString("encryption") : nullptr;
		const std::string *crypto_key = headers ? headers->FindString("crypto_key") : nullptr;
		if ( ch == nullptr ) return;
		/* acknowledged at once: the service keeps it no longer */
		base::DictValue ack;
		ack.Set("messageType", "ack");
		base::ListValue ups;
		base::DictValue up;
		up.Set("channelID", *ch);
		if ( ver != nullptr ) up.Set("version", *ver);
		up.Set("code", 100);
		ups.Append(std::move(up));
		ack.Set("updates", std::move(ups));
		send_json(ack);

		std::string scope;
		std::optional<std::string> payload;
		{
			std::lock_guard<std::mutex> hold(g_lock);
			for ( auto &kv : g_subs ) {
				if ( kv.second.channel != *ch ) continue;
				scope = kv.first;
				if ( data != nullptr ) {
					std::string plain;
					if ( enc != nullptr && *enc == "aes128gcm" && decrypt(kv.second, unb64(*data), &plain) ) {
						payload = plain;
					} else if ( enc != nullptr && *enc == "aesgcm" && encryption != nullptr
						    && crypto_key != nullptr
						    && decrypt_aesgcm(kv.second, unb64(*data), *encryption, *crypto_key, &plain) ) {
						payload = plain;
					} else {
						printf("browser: push: a message for %s not decrypted (%s)\n", scope.c_str(),
						       enc ? enc->c_str() : "no encoding");
						return;
					}
				}
				break;
			}
		}
		if ( scope.empty() ) return;
		printf("browser: push: a message for %s, %zu bytes\n", scope.c_str(), payload ? payload->size() : 0);
		if ( g_deliver ) g_deliver(scope, payload);
		return;
	}
}

void Events::OnDataFrame( bool fin, WebSocketMessageType, base::span<const char> payload )
{
	message_.append(payload.data(), payload.size());
	if ( !fin ) return;
	std::string m;
	m.swap(message_);
	br_net_post(base::BindOnce(&take, std::move(m)));
}

void Events::OnDropChannel( bool, uint16_t code, const std::string &reason )
{
	char why[96];
	snprintf(why, sizeof(why), "%u %s", code, reason.c_str());
	drop_conn(why);
}

void Events::OnFailChannel( const std::string &message, int, std::optional<int> )
{
	drop_conn(message.c_str());
}

void send_json( const base::DictValue &d )
{
	if ( g_conn == nullptr || g_conn->channel == nullptr ) return;
	std::string json;
	base::JSONWriter::Write(d, &json);
	auto buf = base::MakeRefCounted<net::IOBufferWithSize>(json.size());
	memcpy(buf->data(), json.data(), json.size());
	(void)g_conn->channel->SendFrame(true, net::WebSocketFrameHeader::kOpCodeText, std::move(buf), json.size());
}

/* The connection made, on the network thread, when there is none */
void push_connect()
{
	if ( g_conn != nullptr || g_connecting ) return;
	net::URLRequestContext *context = br_net_context();
	if ( context == nullptr ) return;
	std::string server;
	{
		std::lock_guard<std::mutex> hold(g_lock);
		server = g_server;
	}
	GURL url(server);
	if ( !url.is_valid() ) {
		printf("browser: push: the service's address %s is not one\n", server.c_str());
		return;
	}
	g_connecting = true;
	g_conn = new Conn;
	g_conn->channel = std::make_unique<net::WebSocketChannel>(std::make_unique<Events>(), context);
	url::Origin origin = url::Origin::Create(url);
	g_conn->channel->SendAddChannelRequest(url, { "push-notification" }, origin, net::StorageAccessApiStatus::kNone,
					       net::IsolationInfo::CreateForInternalRequest(origin),
					       net::HttpRequestHeaders(), net::WebSocketPriorityHint::kDefault,
					       TRAFFIC_ANNOTATION_FOR_TESTS);
	g_connecting = false;
}

/* Every 25 minutes while it is open: the service is told the browser is there */
void ping()
{
	if ( g_conn != nullptr && g_conn->open ) send_json(base::DictValue());
	br_net_post_delayed(base::BindOnce(&ping), 25 * 60 * 1000);
}

}  // namespace

void br_push_start( BrPushDeliver deliver )
{
	bool any;
	{
		std::lock_guard<std::mutex> hold(g_lock);
		g_deliver = std::move(deliver);
		load_store();
		any = !g_subs.empty();
	}
	if ( any ) br_net_post(base::BindOnce(&push_connect));
	br_net_post_delayed(base::BindOnce(&ping), 25 * 60 * 1000);
}

bool br_push_get( const std::string &scope, BrPushSub *out )
{
	std::lock_guard<std::mutex> hold(g_lock);
	load_store();
	auto it = g_subs.find(scope);
	if ( it == g_subs.end() || !it->second.registered ) return false;
	*out = public_of(it->second);
	return true;
}

void br_push_subscribe( const std::string &scope, const std::string &app_key,
			std::function<void( bool ok, const BrPushSub &sub, const std::string &why )> done )
{
	Sub s;
	{
		std::lock_guard<std::mutex> hold(g_lock);
		load_store();
		auto it = g_subs.find(scope);
		if ( it != g_subs.end() && it->second.registered ) {
			if ( it->second.app_key == app_key ) {
				BrPushSub out = public_of(it->second);
				done(true, out, std::string());
				return;
			}
			/* another server's key: the old subscription is not this one (as the spec says, it fails) */
			done(false, BrPushSub(), "A subscription with a different application server key already exists.");
			return;
		}
		s.scope = scope;
		s.channel = base::Uuid::GenerateRandomV4().AsLowercaseString();
		s.app_key = app_key;
		uint8_t auth[16];
		RAND_bytes(auth, sizeof(auth));
		s.auth.assign((char *)auth, sizeof(auth));
		size_t dot = br_push_test_key.find('.');
		if ( dot != std::string::npos ) {
			/* the tests' keys: what they encrypt for is known */
			s.priv = unb64(br_push_test_key.substr(0, dot));
			s.auth = unb64(br_push_test_key.substr(dot + 1));
			if ( !pub_of(s.priv, &s.pub) ) s.priv.clear();
		}
		if ( s.priv.empty() ? !new_key(&s.priv, &s.pub) : s.pub.empty() ) {
			done(false, BrPushSub(), "the subscription's keys could not be made");
			return;
		}
		g_subs[scope] = s;
		g_waiting[s.channel] = std::move(done);
	}
	br_net_post(base::BindOnce([]( Sub s ) {
		if ( g_conn != nullptr && g_conn->open ) {
			register_channel(s);
		} else {
			push_connect();		/* registered after hello */
		}
	}, s));
	/* the service not answering: the subscriber is not left waiting */
	br_net_post_delayed(base::BindOnce([]( std::string channel ) {
		Done done;
		{
			std::lock_guard<std::mutex> hold(g_lock);
			auto w = g_waiting.find(channel);
			if ( w == g_waiting.end() ) return;
			done = std::move(w->second);
			g_waiting.erase(w);
			for ( auto it = g_subs.begin(); it != g_subs.end(); ++it ) {
				if ( it->second.channel == channel && !it->second.registered ) {
					g_subs.erase(it);
					break;
				}
			}
		}
		printf("browser: push: the service did not answer\n");
		done(false, BrPushSub(), "the push service is not available");
	}, s.channel), 20000);
}

bool br_push_unsubscribe( const std::string &scope )
{
	std::string channel;
	{
		std::lock_guard<std::mutex> hold(g_lock);
		load_store();
		auto it = g_subs.find(scope);
		if ( it == g_subs.end() ) return false;
		channel = it->second.channel;
		g_subs.erase(it);
	}
	dirty();
	br_net_post(base::BindOnce([]( std::string channel ) {
		base::DictValue u;
		u.Set("messageType", "unregister");
		u.Set("channelID", channel);
		send_json(u);
	}, channel));
	return true;
}
