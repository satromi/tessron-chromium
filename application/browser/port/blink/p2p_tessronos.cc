/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	p2p_tessronos.cc
 *	ブラウザ: the sockets of WebRTC (RTCPeerConnection), as the network
 *	service gives them to a renderer (design 14.5).
 *
 *	Blink's WebRTC reaches the network through network.mojom's
 *	P2PSocketManager: the machine's interfaces, the lookup of names, and
 *	sockets it sends its packets on (ICE, STUN, TURN, DTLS, SRTP, SCTP).
 *	Here:
 *
 *	  - the interfaces are the system's (net::GetNetworkList), told once
 *	    when the page asks; the first IPv4 and IPv6 addresses that are
 *	    not loopback are the default ones;
 *	  - the sockets are datagram sockets of the network stack
 *	    (net::UDPServerSocket), bound to the address and in the range of
 *	    ports asked for, read and written on the network's thread; what
 *	    comes is handed to the page in the order it came, and each packet
 *	    sent is told of as sent;
 *	  - the stream sockets (ICE over TCP, TURN over TCP and TLS, and the
 *	    pseudo-TLS of "ssltcp" candidates) are the network service's own
 *	    (network::P2PSocketTcp and P2PSocketStunTcp, which frame the
 *	    packets on the stream as RFC 4571 and the STUN and TURN framing
 *	    say), made and run on the network's thread over the network
 *	    stack's connections (network::ProxyResolvingClientSocketFactory:
 *	    the stack's proxy settings, its TCP sockets and its TLS with the
 *	    browser's certificate checks);
 *	  - the names the page hides its addresses behind (mDNS, "<uuid>.local",
 *	    MdnsResponder) are kept by the browser for the whole process, and
 *	    a lookup of such a name is answered from them: two connections
 *	    on this machine find each other, as they would through the
 *	    responder of the network service. A name is not announced on the
 *	    network;
 *	  - other names are looked up by the network stack's resolver.
 *
 *	The managers and the sockets' ends towards the page are on the
 *	services' thread; the sockets themselves on the network's.
 */

#include <stdio.h>

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/uuid.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "net/base/host_port_pair.h"
#include "net/base/io_buffer.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/base/network_anonymization_key.h"
#include "net/base/network_interfaces.h"
#include "net/dns/host_resolver.h"
#include "net/dns/public/host_resolver_results.h"
#include "net/log/net_log_source.h"
#include "net/log/net_log_with_source.h"
#include "net/socket/udp_server_socket.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "net/url_request/url_request_context.h"
#include "services/network/p2p/socket.h"
#include "services/network/proxy_resolving_client_socket_factory.h"
#include "services/network/public/mojom/p2p.mojom.h"
#include "services/network/public/cpp/p2p_socket_type.h"
#include "services/network/public/mojom/mdns_responder.mojom-blink.h"
#include "services/network/public/mojom/p2p.mojom-blink.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

/* The services' thread (blob_tessronos.cc) */
void br_on_service_thread( base::OnceClosure task );
/* The network's thread and its stack (net_fetch_tessronos.cc) */
void br_net_post( base::OnceClosure work );
net::URLRequestContext *br_net_context();

namespace {

namespace nm = network::mojom::blink;

constexpr int	kRecvSize = 65536;

/* ---------------------------------------------------------------- the names of mDNS */

/* address -> name, and the way back; on the services' thread */
std::map<net::IPAddress, std::pair<std::string, int>>	g_names;
std::map<std::string, net::IPAddress>			g_addresses;

class Mdns final : public nm::MdnsResponder {
public:
	void Bind( mojo::PendingReceiver<nm::MdnsResponder> r ) { receivers_.Add(this, std::move(r)); }

	void CreateNameForAddress( const net::IPAddress &address, CreateNameForAddressCallback cb ) override
	{
		auto n = g_names.find(address);
		if ( n == g_names.end() ) {
			std::string name = base::Uuid::GenerateRandomV4().AsLowercaseString() + ".local";
			n = g_names.emplace(address, std::make_pair(name, 0)).first;
			g_addresses[name] = address;
		}
		n->second.second++;
		std::move(cb).Run(blink::String::FromUtf8(n->second.first), false);
	}

	void RemoveNameForAddress( const net::IPAddress &address, RemoveNameForAddressCallback cb ) override
	{
		auto n = g_names.find(address);
		bool removed = false;
		if ( n != g_names.end() && --n->second.second <= 0 ) {
			g_addresses.erase(n->second.first);
			g_names.erase(n);
			removed = true;
		}
		std::move(cb).Run(removed, false);
	}

private:
	mojo::ReceiverSet<nm::MdnsResponder>	receivers_;
};

/* ---------------------------------------------------------------- a socket, on the network's thread */

class Socket;

struct Sending {
	std::vector<uint8_t>	data;
	net::IPEndPoint		to;
	uint64_t		id;
	int32_t			rtc_id;
	bool			batch_end;	/* the last of a batch: told with the batch */
	bool			in_batch;
};

class NetSocket {
public:
	explicit NetSocket( base::WeakPtr<Socket> owner ) : owner_(owner) {}

	void Open( net::IPEndPoint local, uint16_t min_port, uint16_t max_port );
	void Send( Sending s )
	{
		queue_.push_back(std::move(s));
		if ( !sending_ ) Next();
	}
	void Close()
	{
		closed_ = true;
		socket_.reset();
		delete this;
	}

private:
	void Read();
	bool Came( int rv );
	void Next();
	void Sent( int rv );

	base::WeakPtr<Socket>			owner_;
	std::unique_ptr<net::UDPServerSocket>	socket_;
	scoped_refptr<net::IOBufferWithSize>	rbuf_;
	net::IPEndPoint				from_;
	std::deque<Sending>			queue_;
	scoped_refptr<net::IOBufferWithSize>	sbuf_;
	bool					sending_ = false;
	bool					closed_ = false;
};

/* ---------------------------------------------------------------- a socket, towards the page */

class Socket final : public nm::P2PSocket {
public:
	Socket( mojo::PendingRemote<nm::P2PSocketClient> client, mojo::PendingReceiver<nm::P2PSocket> receiver )
		: client_(std::move(client)), receiver_(this, std::move(receiver))
	{
		receiver_.set_disconnect_handler(base::BindOnce(&Socket::Gone, base::Unretained(this)));
		client_.set_disconnect_handler(base::BindOnce(&Socket::Gone, base::Unretained(this)));
	}

	void Start( const net::IPEndPoint &local, const network::P2PPortRange &range, const net::IPEndPoint &remote )
	{
		remote_ = remote;
		net_ = new NetSocket(weak_.GetWeakPtr());
		NetSocket *n = net_;
		br_net_post(base::BindOnce([]( NetSocket *n, net::IPEndPoint local, uint16_t lo, uint16_t hi ) {
			n->Open(local, lo, hi);
		}, n, local, range.min_port, range.max_port));
	}

	/* the page's packets */
	void Send( base::span<const uint8_t> data, const network::P2PPacketInfo &info ) override
	{
		Post(Sending{std::vector<uint8_t>(data.begin(), data.end()), info.destination, info.packet_id,
			     info.packet_options.packet_id, false, false});
	}
	void SendBatch( blink::Vector<nm::P2PSendPacketPtr> batch ) override
	{
		for ( blink::wtf_size_t i = 0; i < batch.size(); i++ ) {
			const network::P2PPacketInfo &info = batch[i]->packet_info;
			Post(Sending{std::vector<uint8_t>(batch[i]->data.begin(), batch[i]->data.end()), info.destination,
				     info.packet_id, info.packet_options.packet_id, i + 1 == batch.size(), true});
		}
	}
	void SetOption( network::P2PSocketOption, int32_t ) override {}

	/* from the network's thread */
	void Opened( net::IPEndPoint local )
	{
		if ( client_.is_bound() ) client_->SocketCreated(local, remote_);
	}
	void Failed()
	{
		printf("browser: p2p: a socket failed\n");
		Gone();
	}
	void Came( std::vector<uint8_t> data, net::IPEndPoint from, base::TimeTicks at )
	{
		if ( !client_.is_bound() ) return;
		blink::Vector<nm::P2PReceivedPacketPtr> packets;
		packets.push_back(nm::P2PReceivedPacket::New(base::span<const uint8_t>(data), from, at,
							     webrtc::EcnMarking::kNotEct));
		client_->DataReceived(std::move(packets));
	}
	void Sent( uint64_t id, int32_t rtc_id, bool in_batch, bool batch_end )
	{
		if ( !client_.is_bound() ) return;
		network::P2PSendPacketMetrics m(id, rtc_id, base::TimeTicks::Now().since_origin().InMilliseconds());
		if ( !in_batch ) {
			client_->SendComplete(m);
			return;
		}
		batch_.push_back(m);
		if ( batch_end ) {
			client_->SendBatchComplete(batch_);
			batch_.clear();
		}
	}

private:
	void Post( Sending s )
	{
		if ( net_ == nullptr ) return;
		NetSocket *n = net_;
		br_net_post(base::BindOnce([]( NetSocket *n, Sending s ) { n->Send(std::move(s)); }, n, std::move(s)));
	}

	/* the page let go of it, or it failed: the socket closed on the network's thread, and this one deleted */
	void Gone()
	{
		if ( net_ != nullptr ) {
			NetSocket *n = net_;
			net_ = nullptr;
			br_net_post(base::BindOnce([]( NetSocket *n ) { n->Close(); }, n));
		}
		base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE, this);
		receiver_.reset();
		client_.reset();
	}

	mojo::Remote<nm::P2PSocketClient>		client_;
	mojo::Receiver<nm::P2PSocket>			receiver_;
	net::IPEndPoint					remote_;
	NetSocket					*net_ = nullptr;
	blink::Vector<network::P2PSendPacketMetrics>	batch_;
	base::WeakPtrFactory<Socket>			weak_{this};
};

void NetSocket::Open( net::IPEndPoint local, uint16_t min_port, uint16_t max_port )
{
	int rv = net::ERR_FAILED;
	if ( min_port == 0 ) {
		socket_ = std::make_unique<net::UDPServerSocket>(nullptr, net::NetLogSource());
		rv = socket_->Listen(local);
	} else {
		for ( uint32_t p = min_port; p <= max_port && rv != net::OK; p++ ) {
			socket_ = std::make_unique<net::UDPServerSocket>(nullptr, net::NetLogSource());
			rv = socket_->Listen(net::IPEndPoint(local.address(), (uint16_t)p));
		}
	}
	net::IPEndPoint bound;
	if ( rv == net::OK ) rv = socket_->GetLocalAddress(&bound);
	if ( rv != net::OK ) {
		printf("browser: p2p: no socket on %s (%d)\n", local.ToString().c_str(), rv);
		br_on_service_thread(base::BindOnce(&Socket::Failed, owner_));
		socket_.reset();
		return;
	}
	br_on_service_thread(base::BindOnce(&Socket::Opened, owner_, bound));
	rbuf_ = base::MakeRefCounted<net::IOBufferWithSize>(kRecvSize);
	Read();
}

void NetSocket::Read()
{
	for ( ;; ) {
		if ( closed_ || !socket_ ) return;
		int rv = socket_->RecvFrom(rbuf_.get(), kRecvSize, &from_, base::BindOnce([]( NetSocket *n, int rv ) {
			if ( n->Came(rv) ) n->Read();
		}, base::Unretained(this)));
		if ( rv == net::ERR_IO_PENDING ) return;
		if ( !Came(rv) ) return;
	}
}

/* a datagram came, or an error: whether to read on */
bool NetSocket::Came( int rv )
{
	if ( closed_ ) return false;
	if ( rv >= 0 ) {
		std::vector<uint8_t> data(rbuf_->data(), rbuf_->data() + rv);
		br_on_service_thread(base::BindOnce(&Socket::Came, owner_, std::move(data), from_, base::TimeTicks::Now()));
		return true;
	}
	/* an ICMP's answer to an earlier packet ends no datagram socket */
	if ( rv == net::ERR_ADDRESS_UNREACHABLE || rv == net::ERR_CONNECTION_REFUSED || rv == net::ERR_CONNECTION_RESET
	     || rv == net::ERR_MSG_TOO_BIG ) {
		return true;
	}
	printf("browser: p2p: read error %d\n", rv);
	br_on_service_thread(base::BindOnce(&Socket::Failed, owner_));
	return false;
}

void NetSocket::Next()
{
	while ( !queue_.empty() && !closed_ && socket_ ) {
		Sending &s = queue_.front();
		sbuf_ = base::MakeRefCounted<net::IOBufferWithSize>(s.data.size());
		memcpy(sbuf_->data(), s.data.data(), s.data.size());
		sending_ = true;
		int rv = socket_->SendTo(sbuf_.get(), (int)s.data.size(), s.to, base::BindOnce([]( NetSocket *n, int rv ) {
			n->Sent(rv);
			n->Next();
		}, base::Unretained(this)));
		if ( rv == net::ERR_IO_PENDING ) return;
		Sent(rv);
	}
}

/* a packet went (or could not: it is told of as sent, as a datagram lost on the way would be) */
void NetSocket::Sent( int rv )
{
	sending_ = false;
	if ( queue_.empty() ) return;
	Sending s = std::move(queue_.front());
	queue_.pop_front();
	if ( rv < 0 ) {
		static int told;
		if ( told++ < 5 ) printf("browser: p2p: a packet to %s not sent (%d)\n", s.to.ToString().c_str(), rv);
	}
	br_on_service_thread(base::BindOnce(&Socket::Sent, owner_, s.id, s.rtc_id, s.in_batch, s.batch_end));
}

/* ---------------------------------------------------------------- the stream sockets, on the network's thread */

class StreamSockets final : public network::P2PSocket::Delegate {
public:
	static StreamSockets &Get()
	{
		static auto *s = new StreamSockets();
		return *s;
	}

	void Create( network::P2PSocketType type, net::IPEndPoint local, network::P2PPortRange range,
		     network::P2PHostAndIPEndPoint remote, net::NetworkTrafficAnnotationTag annotation,
		     mojo::PendingRemote<network::mojom::P2PSocketClient> client,
		     mojo::PendingReceiver<network::mojom::P2PSocket> receiver )
	{
		net::URLRequestContext *context = br_net_context();
		if ( context == nullptr ) return;	/* the pipes closed: the page told the socket failed */
		if ( !factory_ ) factory_ = std::make_unique<network::ProxyResolvingClientSocketFactory>(context);
		std::unique_ptr<network::P2PSocket> socket = network::P2PSocket::Create(
			this, std::move(client), std::move(receiver), type, annotation, nullptr, factory_.get(), nullptr,
			std::nullopt);
		if ( !socket ) return;
		network::P2PSocket *raw = socket.get();
		sockets_.push_back(std::move(socket));
		printf("browser: p2p: a stream socket (type %d) to %s\n", (int)type,
		       remote.hostname.empty() ? remote.ip_address.ToString().c_str() : remote.hostname.c_str());
		raw->Init(local, range.min_port, range.max_port, remote, net::NetworkAnonymizationKey());
	}

	/* a socket failed or the page let go of it: deleted, as the service's manager does */
	void DestroySocket( network::P2PSocket *socket ) override
	{
		for ( auto it = sockets_.begin(); it != sockets_.end(); ++it ) {
			if ( it->get() == socket ) {
				sockets_.erase(it);
				return;
			}
		}
	}
	void DumpPacket( base::span<const uint8_t>, bool ) override {}

private:
	std::unique_ptr<network::ProxyResolvingClientSocketFactory>	factory_;
	std::vector<std::unique_ptr<network::P2PSocket>>		sockets_;
};

/* ---------------------------------------------------------------- the manager */

class Manager final : public nm::P2PSocketManager {
public:
	void Bind( mojo::PendingReceiver<nm::P2PSocketManager> r ) { receivers_.Add(this, std::move(r)); }

	void StartNetworkNotifications( mojo::PendingRemote<nm::P2PNetworkNotificationClient> client ) override
	{
		auto remote = std::make_shared<mojo::Remote<nm::P2PNetworkNotificationClient>>(std::move(client));
		clients_.push_back(remote);
		br_net_post(base::BindOnce([]( std::weak_ptr<mojo::Remote<nm::P2PNetworkNotificationClient>> remote ) {
			net::NetworkInterfaceList list;
			net::GetNetworkList(&list, net::EXCLUDE_HOST_SCOPE_VIRTUAL_INTERFACES);
			br_on_service_thread(base::BindOnce([]( std::weak_ptr<mojo::Remote<nm::P2PNetworkNotificationClient>> remote,
								net::NetworkInterfaceList list ) {
				auto r = remote.lock();
				if ( !r || !r->is_bound() ) return;
				net::IPAddress v4, v6;
				blink::Vector<net::NetworkInterface> networks;
				for ( auto &n : list ) {
					if ( n.address.IsLoopback() ) continue;
					if ( n.address.IsIPv4() && v4.empty() ) v4 = n.address;
					if ( n.address.IsIPv6() && v6.empty() && !n.address.IsLinkLocal() ) v6 = n.address;
					networks.push_back(n);
				}
				printf("browser: p2p: %u interfaces, %s\n", (unsigned)networks.size(), v4.ToString().c_str());
				(*r)->NetworkListChanged(networks, v4, v6);
			}, remote, std::move(list)));
		}, std::weak_ptr<mojo::Remote<nm::P2PNetworkNotificationClient>>(remote)));
	}

	void GetHostAddress( const blink::String &host_name, std::optional<net::AddressFamily> family, bool,
			     GetHostAddressCallback cb ) override
	{
		std::string name = host_name.Utf8();
		auto a = g_addresses.find(name);
		if ( a != g_addresses.end() ) {
			std::move(cb).Run(blink::Vector<net::IPAddress>({a->second}));
			return;
		}
		if ( name.size() > 6 && name.compare(name.size() - 6, 6, ".local") == 0 ) {
			std::move(cb).Run(blink::Vector<net::IPAddress>());	/* a name of another machine's */
			return;
		}
		Resolve(name, family, std::move(cb));
	}

	void CreateSocket( network::P2PSocketType type, const net::IPEndPoint &local, const network::P2PPortRange &range,
			   const network::P2PHostAndIPEndPoint &remote, const net::MutableNetworkTrafficAnnotationTag &annotation,
			   const std::optional<base::UnguessableToken> &, mojo::PendingRemote<nm::P2PSocketClient> client,
			   mojo::PendingReceiver<nm::P2PSocket> receiver ) override
	{
		if ( type == network::P2P_SOCKET_UDP ) {
			(new Socket(std::move(client), std::move(receiver)))->Start(local, range, remote.ip_address);
			return;
		}
		/* the same pipes, bound on the network's thread to the service's stream socket */
		uint32_t version = client.version();
		mojo::PendingRemote<network::mojom::P2PSocketClient> c(client.PassPipe(), version);
		mojo::PendingReceiver<network::mojom::P2PSocket> r(receiver.PassPipe());
		br_net_post(base::BindOnce([]( network::P2PSocketType type, net::IPEndPoint local, network::P2PPortRange range,
					       network::P2PHostAndIPEndPoint remote, net::NetworkTrafficAnnotationTag annotation,
					       mojo::PendingRemote<network::mojom::P2PSocketClient> c,
					       mojo::PendingReceiver<network::mojom::P2PSocket> r ) {
			StreamSockets::Get().Create(type, local, range, remote, annotation, std::move(c), std::move(r));
		}, type, local, range, remote, net::NetworkTrafficAnnotationTag(annotation), std::move(c), std::move(r)));
	}

private:
	/* a lookup through the network stack's resolver, on the network's thread */
	struct Lookup {
		std::unique_ptr<net::HostResolver::ResolveHostRequest>	request;
		GetHostAddressCallback					cb;
	};

	static void Resolve( std::string name, std::optional<net::AddressFamily> family, GetHostAddressCallback cb )
	{
		br_net_post(base::BindOnce([]( std::string name, std::optional<net::AddressFamily> family,
					       GetHostAddressCallback cb ) {
			net::URLRequestContext *context = br_net_context();
			if ( context == nullptr ) {
				br_on_service_thread(base::BindOnce(std::move(cb), blink::Vector<net::IPAddress>()));
				return;
			}
			net::HostResolver::ResolveHostParameters params;
			if ( family == net::ADDRESS_FAMILY_IPV4 ) params.dns_query_type = net::DnsQueryType::A;
			if ( family == net::ADDRESS_FAMILY_IPV6 ) params.dns_query_type = net::DnsQueryType::AAAA;
			auto *l = new Lookup;
			l->cb = std::move(cb);
			l->request = context->host_resolver()->CreateRequest(net::HostPortPair(name, 0), net::NetworkAnonymizationKey(), net::handles::kInvalidNetworkHandle,
									    net::NetLogWithSource(), params);
			auto done = []( Lookup *l, int rv ) {
				blink::Vector<net::IPAddress> out;
				if ( rv == net::OK ) {
					for ( auto &e : l->request->GetAddressResults().endpoints() ) out.push_back(e.address());
				}
				br_on_service_thread(base::BindOnce(std::move(l->cb), std::move(out)));
				delete l;
			};
			int rv = l->request->Start(base::BindOnce(done, l));
			if ( rv != net::ERR_IO_PENDING ) done(l, rv);
		}, std::move(name), family, std::move(cb)));
	}

	mojo::ReceiverSet<nm::P2PSocketManager>						receivers_;
	std::vector<std::shared_ptr<mojo::Remote<nm::P2PNetworkNotificationClient>>>	clients_;
};

template <typename Interface, typename Service>
void Binder()
{
	static Service *svc;
	blink::GetEmptyBrowserInterfaceBroker().SetBinderForTesting(
		Interface::Name_, base::BindRepeating([]( mojo::ScopedMessagePipeHandle h ) {
			br_on_service_thread(base::BindOnce([]( mojo::PendingReceiver<Interface> r ) {
				if ( svc == nullptr ) svc = new Service();
				svc->Bind(std::move(r));
			}, mojo::PendingReceiver<Interface>(std::move(h))));
		}));
}

}  // namespace

/* The binders of the frames' broker, once when the engine is up */
void br_p2p_init()
{
	Binder<nm::P2PSocketManager, Manager>();
	Binder<nm::MdnsResponder, Mdns>();
}
