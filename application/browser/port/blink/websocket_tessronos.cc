/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	websocket_tessronos.cc
 *	WebSocket for the engine (design 17.19).
 *
 *	A page's WebSocket talks to its channel, which in a browser sends the
 *	connection through Mojo to the network service; there is none here.
 *	The channel of this file (made by DOMWebSocket through the patch
 *	dom_websocket.patch) runs the connection on the network stack of the
 *	process instead: a net::WebSocketChannel on the network thread
 *	(port/net/net_fetch_tessronos.cc), with the stack's sockets, TLS,
 *	certificate check and cookie jar. The stack does the opening handshake,
 *	the framing, the masking and the closing handshake.
 *
 *	The page's side and the network thread's side know each other by a
 *	number: what one tells the other goes as a task through the other's
 *	runner, and a task for a number that is gone is dropped. A message is
 *	put together from its frames on the network thread and handed to the
 *	page whole.
 *
 *	In the browser of several processes the connection is the network
 *	process's (design 14.13.8): the renderer's side of the number is a
 *	pipe (br.mojom.NetSocket) on its network thread, which the network
 *	process answers with a NetSocket of this file whose events go back
 *	over the pipe (NetSocketClient). The extensions' rules are looked at
 *	by the renderer, whose extensions they are, before it asks.
 *
 *	A Blob sent is read first (the blob store's bytes, br_blob_read); what
 *	the page sends while it is read waits behind it, so that the messages
 *	go in the order they were sent.
 */

#include <stdio.h>
#include <string.h>

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "net/base/io_buffer.h"
#include "net/base/isolation_info.h"
#include "net/base/net_errors.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/storage_access_api/status.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "net/url_request/url_request_context.h"
#include "net/websockets/websocket_channel.h"
#include "net/websockets/websocket_event_interface.h"
#include "net/websockets/websocket_frame.h"
#include "net/websockets/websocket_handshake_request_info.h"
#include "net/websockets/websocket_handshake_response_info.h"
#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#include "third_party/blink/renderer/core/inspector/console_message.h"
#include "third_party/blink/renderer/core/typed_arrays/dom_array_buffer.h"
#include "third_party/blink/renderer/platform/blob/blob_data.h"
#include "third_party/blink/renderer/modules/websockets/websocket_channel.h"
#include "third_party/blink/renderer/modules/websockets/websocket_channel_client.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/heap/persistent.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"
#include "third_party/blink/renderer/platform/wtf/text/strcat.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "url/gurl.h"
#include "../ext/br_ext.h"
#include "url/origin.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "application/browser/mojom/br_process.mojom.h"

/* The network stack of the port (port/net/net_fetch_tessronos.cc) */
net::URLRequestContext *br_net_context();
void br_net_post( base::OnceClosure work );
void br_net_post_main( base::OnceClosure work );
/* A renderer of the browser of several processes, and its way to the network process (net_service_tessronos.cc) */
bool br_net_remote();
bool br_net_remote_websocket( const std::string &url, const std::vector<std::string> &protocols,
			      const std::string &origin, mojo::PendingReceiver<::br::mojom::NetSocket> socket,
			      mojo::PendingRemote<::br::mojom::NetSocketClient> client );
/* A Blob's bytes, by its UUID, on the services' thread (blob_tessronos.cc) */
void br_blob_read( const std::string &key, bool is_url,
		   base::OnceCallback<void(bool, std::string, std::string)> done );

namespace blink {

namespace {

/* ================================================================ the network thread's side */

class NetSocket;

/* The connections under way, by number; touched only on the network thread */
std::map<int, NetSocket *> &net_sockets()
{
	static auto *m = new std::map<int, NetSocket *>();
	return *m;
}

/* What the page's side is told; defined below, run on the page's thread */
void page_connected( int id, std::string protocol, std::string extensions );
void page_message( int id, bool text, std::string data );
void page_consumed( int id, uint64_t n );
void page_closing( int id );
void page_dropped( int id, bool clean, uint16_t code, std::string reason );
void page_failed( int id, std::string message );

/*
 * One connection: the stack's channel and what it tells. The channel
 * owns its events; it is deleted from inside the call that says it has
 * ended (OnDropChannel, OnFailChannel), as the stack requires.
 */
class NetSocket {
public:
	explicit NetSocket( int id ) : id_(id) {}

	void start( const GURL &url, std::vector<std::string> protocols, const url::Origin &origin );
	void send( bool text, std::string data );
	void close( uint16_t code, std::string reason );

	/* The channel deleted and the number let go: nothing more is told */
	static void drop( int id )
	{
		auto it = net_sockets().find(id);
		if ( it == net_sockets().end() ) return;
		NetSocket *s = it->second;
		net_sockets().erase(it);
		delete s;
	}

	/* The stack's channel says it has ended: deleted now, from inside that call */
	void ended() { drop(id_); }

	/* Frames read on for as long as the connection lasts */
	void read_on()
	{
		if ( channel_ == nullptr ) return;
		(void)channel_->ReadFrames();
	}

	/* What the connection tells: to the page, or over the pipe of the renderer that asked */
	void told_connected( std::string protocol, std::string extensions )
	{
		if ( client_ ) client_->Connected(protocol, extensions);
		else br_net_post_main(base::BindOnce(&page_connected, id_, protocol, extensions));
	}
	void told_message( bool text, std::string data )
	{
		if ( client_ ) client_->Message(text, std::vector<uint8_t>(data.begin(), data.end()));
		else br_net_post_main(base::BindOnce(&page_message, id_, text, std::move(data)));
	}
	void told_consumed( uint64_t n )
	{
		if ( client_ ) client_->Consumed(n);
		else br_net_post_main(base::BindOnce(&page_consumed, id_, n));
	}
	void told_closing()
	{
		if ( client_ ) client_->Closing();
		else br_net_post_main(base::BindOnce(&page_closing, id_));
	}
	void told_dropped( bool clean, uint16_t code, std::string reason )
	{
		if ( client_ ) client_->Dropped(clean, code, reason);
		else br_net_post_main(base::BindOnce(&page_dropped, id_, clean, code, reason));
	}
	void told_failed( std::string message )
	{
		if ( client_ ) client_->Failed(message);
		else br_net_post_main(base::BindOnce(&page_failed, id_, message));
	}

	int		id_;
	uint64_t	sent_ = 0;			/* bytes handed to the channel */
	std::unique_ptr<net::WebSocketChannel>	channel_;
	/* in the network process: the renderer's side, whose extensions have looked at it */
	mojo::Remote<::br::mojom::NetSocketClient>	client_;
};

class NetEvents final : public net::WebSocketEventInterface {
public:
	NetEvents( NetSocket *s, int id ) : socket_(s), id_(id) {}

	void OnCreateURLRequest( net::URLRequest * ) override {}

	int OnURLRequestConnected( net::URLRequest *, const net::TransportInfo &,
				   net::CompletionOnceCallback ) override
	{
		return net::OK;
	}

	void OnAddChannelResponse( std::unique_ptr<net::WebSocketHandshakeResponseInfo>,
				   const std::string &protocol, const std::string &extensions ) override
	{
		socket_->told_connected(protocol, extensions);
		/* reading starts from a task of its own, outside the stack's call */
		int id = id_;
		br_net_post(base::BindOnce([]( int id ) {
			auto it = net_sockets().find(id);
			if ( it != net_sockets().end() ) it->second->read_on();
		}, id));
	}

	void OnDataFrame( bool fin, WebSocketMessageType type, base::span<const char> payload ) override
	{
		if ( type != net::WebSocketFrameHeader::kOpCodeContinuation ) {
			text_ = ( type == net::WebSocketFrameHeader::kOpCodeText );
		}
		message_.append(payload.data(), payload.size());
		if ( !fin ) return;
		socket_->told_message(text_, std::move(message_));
		message_.clear();
	}

	/* Every frame is taken as it comes: the stack reads on */
	bool HasPendingDataFrames() override { return false; }

	void OnSendDataFrameDone() override
	{
		uint64_t n = socket_->sent_ - reported_;
		reported_ = socket_->sent_;
		if ( n > 0 ) socket_->told_consumed(n);
	}

	void OnClosingHandshake() override
	{
		socket_->told_closing();
	}

	void OnDropChannel( bool was_clean, uint16_t code, const std::string &reason ) override
	{
		socket_->told_dropped(was_clean, code, reason);
		socket_->ended();		/* deletes this */
	}

	void OnFailChannel( const std::string &message, int net_error,
			    std::optional<int> ) override
	{
		printf("browser: websocket: failed: %s (%d)\n", message.c_str(), net_error);
		socket_->told_failed(message);
		socket_->ended();		/* deletes this */
	}

	void OnStartOpeningHandshake( std::unique_ptr<net::WebSocketHandshakeRequestInfo> ) override {}

	/* A certificate the verifier refused is refused here too */
	void OnSSLCertificateError( std::unique_ptr<SSLErrorCallbacks> callbacks, const GURL &,
				    int net_error, const net::SSLInfo &ssl_info, bool ) override
	{
		callbacks->CancelSSLRequest(net_error, &ssl_info);
	}

	/* No credentials to give: the handshake goes on without */
	int OnAuthRequired( const net::AuthChallengeInfo &, scoped_refptr<net::HttpResponseHeaders>,
			    const net::IPEndPoint &, base::OnceCallback<void( const net::AuthCredentials * )>,
			    std::optional<net::AuthCredentials> *credentials ) override
	{
		*credentials = std::nullopt;
		return net::OK;
	}

private:
	NetSocket	*socket_;
	int		id_;
	bool		text_ = true;
	std::string	message_;
	uint64_t	reported_ = 0;
};

void NetSocket::start( const GURL &url, std::vector<std::string> protocols, const url::Origin &origin )
{
	net::URLRequestContext *context = br_net_context();
	if ( context == nullptr ) {
		told_failed("no network");
		drop(id_);
		return;
	}
	/*
	 * The extensions' rules: here and not at the handshake's request,
	 * since a WebSocket's handshake cannot follow a redirect. A rule's
	 * other address is simply the one connected to.
	 */
	GURL to = url;
	if ( client_ ) goto connect;
	{
	BrExtReq rq;
	BrExtAns ans;
	rq.url = url.spec();
	rq.initiator = origin.Serialize();
	rq.type = "websocket";
	rq.method = "GET";
	BrExtAct act = br_ext_rules(rq, &ans);
	if ( act == BR_EXT_BLOCK ) {
		told_failed("blocked by an extension");
		drop(id_);
		return;
	}
	if ( act == BR_EXT_REDIRECT && GURL(ans.redirect).SchemeIsWSOrWSS() ) to = GURL(ans.redirect);
	}
connect:
	channel_ = std::make_unique<net::WebSocketChannel>(std::make_unique<NetEvents>(this, id_), context);
	/* the request is the page's own: its cookies go with it */
	net::IsolationInfo isolation = net::IsolationInfo::CreateForInternalRequest(origin);
	channel_->SendAddChannelRequest(to, protocols, origin, net::StorageAccessApiStatus::kNone,
					isolation, net::HttpRequestHeaders(),
					net::WebSocketPriorityHint::kDefault, TRAFFIC_ANNOTATION_FOR_TESTS);
	/* a failure there has already deleted this */
}

void NetSocket::send( bool text, std::string data )
{
	if ( channel_ == nullptr ) return;
	auto buf = base::MakeRefCounted<net::IOBufferWithSize>(data.size());
	if ( !data.empty() ) memcpy(buf->data(), data.data(), data.size());
	sent_ += data.size();
	(void)channel_->SendFrame(true, text ? net::WebSocketFrameHeader::kOpCodeText
					     : net::WebSocketFrameHeader::kOpCodeBinary,
				  std::move(buf), data.size());
}

void NetSocket::close( uint16_t code, std::string reason )
{
	if ( channel_ == nullptr ) return;
	(void)channel_->StartClosingHandshake(code, reason);
}

/*
 * A renderer's connection that the network process holds: on the network
 * thread, by the page's number, its pipe and what comes back over it
 */
class RemoteSocket final : public ::br::mojom::NetSocketClient {
public:
	explicit RemoteSocket( int id ) : id_(id) {}

	void Connected( const std::string &protocol, const std::string &extensions ) override
	{
		br_net_post_main(base::BindOnce(&page_connected, id_, protocol, extensions));
	}
	void Message( bool text, const std::vector<uint8_t> &data ) override
	{
		br_net_post_main(base::BindOnce(&page_message, id_, text, std::string(data.begin(), data.end())));
	}
	void Consumed( uint64_t n ) override { br_net_post_main(base::BindOnce(&page_consumed, id_, n)); }
	void Closing() override { br_net_post_main(base::BindOnce(&page_closing, id_)); }
	void Dropped( bool clean, uint16_t code, const std::string &reason ) override
	{
		br_net_post_main(base::BindOnce(&page_dropped, id_, clean, code, reason));
		drop(id_);
	}
	void Failed( const std::string &message ) override
	{
		br_net_post_main(base::BindOnce(&page_failed, id_, message));
		drop(id_);
	}

	static std::map<int, std::unique_ptr<RemoteSocket>> &all()
	{
		static auto *m = new std::map<int, std::unique_ptr<RemoteSocket>>();
		return *m;
	}
	static RemoteSocket *of( int id )
	{
		auto it = all().find(id);
		return it != all().end() ? it->second.get() : nullptr;
	}
	static void drop( int id ) { all().erase(id); }

	/* The page's connection asked of the network process, past this renderer's extensions */
	static void open( int id, GURL url, std::vector<std::string> protocols, url::Origin origin )
	{
		BrExtReq rq;
		BrExtAns ans;
		rq.url = url.spec();
		rq.initiator = origin.Serialize();
		rq.type = "websocket";
		rq.method = "GET";
		BrExtAct act = br_ext_rules(rq, &ans);
		if ( act == BR_EXT_BLOCK ) {
			br_net_post_main(base::BindOnce(&page_failed, id, std::string("blocked by an extension")));
			return;
		}
		if ( act == BR_EXT_REDIRECT && GURL(ans.redirect).SchemeIsWSOrWSS() ) url = GURL(ans.redirect);
		auto r = std::make_unique<RemoteSocket>(id);
		mojo::PendingRemote<::br::mojom::NetSocketClient> client;
		r->recv_.Bind(client.InitWithNewPipeAndPassReceiver());
		r->recv_.set_disconnect_handler(base::BindOnce([]( int id ) {
			br_net_post_main(base::BindOnce(&page_failed, id, std::string("the network process has gone")));
			drop(id);
		}, id));
		if ( !br_net_remote_websocket(url.spec(), protocols, origin.Serialize(),
					      r->sock_.BindNewPipeAndPassReceiver(), std::move(client)) ) {
			br_net_post_main(base::BindOnce(&page_failed, id, std::string("no network")));
			return;
		}
		all()[id] = std::move(r);
	}

	mojo::Remote<::br::mojom::NetSocket>	sock_;

private:
	int					id_;
	mojo::Receiver<::br::mojom::NetSocketClient> recv_{this};
};

/* ================================================================ the page's side */

int g_next_id;

class BrWebSocketChannel;
void page_blob( int id, uint64_t seq, bool ok, std::string bytes );

/* The channels on the page's thread, by number */
std::map<int, WeakPersistent<BrWebSocketChannel>> &page_channels()
{
	static auto *m = new std::map<int, WeakPersistent<BrWebSocketChannel>>();
	return *m;
}

class BrWebSocketChannel final : public WebSocketChannel {
public:
	BrWebSocketChannel( ExecutionContext *context, WebSocketChannelClient *client )
		: context_(context), client_(client), id_(++g_next_id)
	{
		page_channels()[id_] = this;
	}

	bool Connect( const KURL &url, const String &protocol, network::mojom::blink::IPAddressSpace ) override
	{
		url_ = url;
		std::vector<std::string> protocols;
		/* the protocols the page asks for, parted by ", " */
		std::string list = protocol.Utf8();
		size_t at = 0;
		while ( at < list.size() ) {
			size_t comma = list.find(',', at);
			if ( comma == std::string::npos ) comma = list.size();
			std::string p = list.substr(at, comma - at);
			while ( !p.empty() && p.front() == ' ' ) p.erase(p.begin());
			while ( !p.empty() && p.back() == ' ' ) p.pop_back();
			if ( !p.empty() ) protocols.push_back(p);
			at = comma + 1;
		}
		url::Origin origin = context_->GetSecurityOrigin()->ToUrlOrigin();
		GURL gurl(url);
		int id = id_;
		br_net_post(base::BindOnce([]( int id, GURL url, std::vector<std::string> protocols,
					       url::Origin origin ) {
			if ( br_net_remote() ) {
				RemoteSocket::open(id, std::move(url), std::move(protocols), origin);
				return;
			}
			NetSocket *s = new NetSocket(id);
			net_sockets()[id] = s;
			s->start(url, std::move(protocols), origin);
		}, id, gurl, std::move(protocols), origin));
		connecting_ = true;
		return true;
	}

	void Send( const std::string &text, std::unique_ptr<SendCompletionWatcher> watcher ) override
	{
		queue_send(true, text);
		if ( watcher ) watcher->OnMessageSent(true);
	}

	void Send( const DOMArrayBuffer &buffer, size_t offset, size_t length,
		   std::unique_ptr<SendCompletionWatcher> watcher ) override
	{
		auto bytes = buffer.ByteSpan().subspan(offset, length);
		queue_send(false, std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size()));
		if ( watcher ) watcher->OnMessageSent(true);
	}

	/* A Blob: its bytes read, then sent as a binary message in its turn */
	void Send( scoped_refptr<BlobDataHandle> blob ) override
	{
		uint64_t seq = out_first_ + out_.size();
		out_.push_back(Out{ false, std::string(), false });
		int id = id_;
		br_blob_read(blob->Uuid().Utf8(), false, base::BindOnce(
			[]( int id, uint64_t seq, bool ok, std::string bytes, std::string ) {
				br_net_post_main(base::BindOnce(&page_blob, id, seq, ok, std::move(bytes)));
			}, id, seq));
	}

	/* A Blob's bytes come: what waits in front of nothing unread goes */
	void blob_read( uint64_t seq, bool ok, std::string bytes )
	{
		if ( seq < out_first_ || seq - out_first_ >= out_.size() ) return;
		if ( !ok ) {
			Fail("The Blob could not be read.", mojom::ConsoleMessageLevel::kError, nullptr);
			return;
		}
		Out &o = out_[seq - out_first_];
		o.data = std::move(bytes);
		o.ready = true;
		while ( !out_.empty() && out_.front().ready ) {
			post_send(out_.front().text, std::move(out_.front().data));
			out_.pop_front();
			out_first_++;
		}
	}

	void Close( int code, const String &reason ) override
	{
		uint16_t send_code = (uint16_t)( code == kCloseEventCodeNotSpecified ? kCloseEventCodeNoStatusRcvd
										       : code );
		int id = id_;
		std::string r = reason.Utf8();
		br_net_post(base::BindOnce([]( int id, uint16_t code, std::string reason ) {
			if ( RemoteSocket *r = RemoteSocket::of(id) ) r->sock_->Close(code, reason);
			auto it = net_sockets().find(id);
			if ( it != net_sockets().end() ) it->second->close(code, reason);
		}, id, send_code, std::move(r)));
	}

	void Fail( const String &reason, mojom::ConsoleMessageLevel level, SourceLocation * ) override
	{
		if ( gone_ ) return;
		String message = StrCat({"WebSocket connection to '", url_.ElidedString(), "' failed: ", reason});
		printf("browser: websocket: %s\n", message.Utf8().c_str());
		context_->AddConsoleMessage(MakeGarbageCollected<ConsoleMessage>(
			mojom::ConsoleMessageSource::kNetwork, level, message));
		/* the page is told from a task of its own, as the connection is let go */
		drop_net();
		int id = id_;
		br_net_post_main(base::BindOnce(&page_failed, id, std::string()));
	}

	void Disconnect() override
	{
		dispose();
	}

	void CancelHandshake() override
	{
		if ( connecting_ ) dispose();
	}

	void ApplyBackpressure() override {}
	void RemoveBackpressure() override {}

	void Trace( Visitor *visitor ) const override
	{
		visitor->Trace(context_);
		visitor->Trace(client_);
		WebSocketChannel::Trace(visitor);
	}

	/* ------------------------------------------------ told from the network thread */

	void connected( const std::string &protocol, const std::string &extensions )
	{
		if ( gone_ ) return;
		connecting_ = false;
		client_->DidConnect(String::FromUtf8(protocol), String::FromUtf8(extensions));
	}

	void message( bool text, const std::string &data )
	{
		if ( gone_ ) return;
		if ( text ) {
			String s = String::FromUtf8(data);
			if ( s.IsNull() && !data.empty() ) {
				Fail("Could not decode a text frame as UTF-8.",
				     mojom::ConsoleMessageLevel::kError, nullptr);
				return;
			}
			client_->DidReceiveTextMessage(s.IsNull() ? g_empty_string : s);
		} else {
			Vector<base::span<const uint8_t>> pieces;
			pieces.push_back(base::as_byte_span(data));
			client_->DidReceiveBinaryMessage(pieces);
		}
	}

	void consumed( uint64_t n )
	{
		if ( !gone_ ) client_->DidConsumeBufferedAmount(n);
	}

	void closing()
	{
		if ( !gone_ ) client_->DidStartClosingHandshake();
	}

	void dropped( bool clean, uint16_t code, const std::string &reason )
	{
		if ( gone_ ) return;
		WebSocketChannelClient *client = client_;
		dispose();
		client->DidClose(clean ? WebSocketChannelClient::kClosingHandshakeComplete
				       : WebSocketChannelClient::kClosingHandshakeIncomplete,
				 code, String::FromUtf8(reason));
	}

	void failed()
	{
		if ( gone_ ) return;
		WebSocketChannelClient *client = client_;
		client->DidError();
		if ( gone_ ) return;
		dispose();
		client->DidClose(WebSocketChannelClient::kClosingHandshakeIncomplete,
				 kCloseEventCodeAbnormalClosure, String());
	}

private:
	Member<ExecutionContext>	context_;
	Member<WebSocketChannelClient>	client_;
	KURL				url_;
	int				id_;
	bool				connecting_ = false;
	bool				gone_ = false;

	/* What waits to be sent behind a Blob being read, in order; out_first_ the number of the first */
	struct Out {
		bool		text;
		std::string	data;
		bool		ready;
	};
	std::deque<Out>			out_;
	uint64_t			out_first_ = 0;

	void queue_send( bool text, std::string data )
	{
		if ( out_.empty() ) {
			post_send(text, std::move(data));
			return;
		}
		out_.push_back(Out{ text, std::move(data), true });
	}

	void post_send( bool text, std::string data )
	{
		int id = id_;
		br_net_post(base::BindOnce([]( int id, bool text, std::string data ) {
			if ( RemoteSocket *r = RemoteSocket::of(id) ) {
				r->sock_->Send(text, std::vector<uint8_t>(data.begin(), data.end()));
				return;
			}
			auto it = net_sockets().find(id);
			if ( it != net_sockets().end() ) it->second->send(text, std::move(data));
		}, id, text, std::move(data)));
	}

	/* The connection let go on the network thread, without a word back */
	void drop_net()
	{
		int id = id_;
		br_net_post(base::BindOnce([]( int id ) {
			RemoteSocket::drop(id);
			NetSocket::drop(id);
		}, id));
	}

	/* Nothing more is told to the client; the number and the connection go */
	void dispose()
	{
		if ( gone_ ) return;
		gone_ = true;
		connecting_ = false;
		drop_net();
		page_channels().erase(id_);
	}
};

BrWebSocketChannel *page_channel( int id )
{
	auto it = page_channels().find(id);
	return ( it == page_channels().end() ) ? nullptr : it->second.Get();
}

void page_blob( int id, uint64_t seq, bool ok, std::string bytes )
{
	if ( BrWebSocketChannel *c = page_channel(id) ) c->blob_read(seq, ok, std::move(bytes));
}

void page_connected( int id, std::string protocol, std::string extensions )
{
	if ( BrWebSocketChannel *c = page_channel(id) ) c->connected(protocol, extensions);
}

void page_message( int id, bool text, std::string data )
{
	if ( BrWebSocketChannel *c = page_channel(id) ) c->message(text, data);
}

void page_consumed( int id, uint64_t n )
{
	if ( BrWebSocketChannel *c = page_channel(id) ) c->consumed(n);
}

void page_closing( int id )
{
	if ( BrWebSocketChannel *c = page_channel(id) ) c->closing();
}

void page_dropped( int id, bool clean, uint16_t code, std::string reason )
{
	if ( BrWebSocketChannel *c = page_channel(id) ) c->dropped(clean, code, reason);
}

/* The connection failed (or the page failed it): an error, then a close that was not clean */
void page_failed( int id, std::string )
{
	if ( BrWebSocketChannel *c = page_channel(id) ) c->failed();
}

}  // namespace

WebSocketChannel *BrCreateWebSocketChannel( ExecutionContext *context, WebSocketChannelClient *client )
{
	return MakeGarbageCollected<BrWebSocketChannel>(context, client);
}

namespace {

/* In the network process: what the renderer sends and closes, for the connection of a number */
class SocketImpl final : public ::br::mojom::NetSocket {
public:
	explicit SocketImpl( int id ) : id_(id) {}
	~SocketImpl() override { blink::NetSocket::drop(id_); }

	void Send( bool text, const std::vector<uint8_t> &data ) override
	{
		auto it = net_sockets().find(id_);
		if ( it != net_sockets().end() ) it->second->send(text, std::string(data.begin(), data.end()));
	}
	void Close( uint16_t code, const std::string &reason ) override
	{
		auto it = net_sockets().find(id_);
		if ( it != net_sockets().end() ) it->second->close(code, reason);
	}

private:
	int	id_;
};

int g_served;

}  // namespace

}  // namespace blink

/*
 * In the network process, on the network thread: a renderer's WebSocket
 * connected. Its numbers are this process's own (below 0); it lasts while
 * the renderer's pipe does.
 */
void br_ws_serve( std::string url, std::vector<std::string> protocols, std::string origin,
		  mojo::PendingReceiver<::br::mojom::NetSocket> socket,
		  mojo::PendingRemote<::br::mojom::NetSocketClient> client )
{
	int id = --blink::g_served;
	auto *s = new blink::NetSocket(id);
	s->client_.Bind(std::move(client));
	blink::net_sockets()[id] = s;
	mojo::MakeSelfOwnedReceiver(std::make_unique<blink::SocketImpl>(id), std::move(socket));
	s->start(GURL(url), std::move(protocols), url::Origin::Create(GURL(origin)));
}
