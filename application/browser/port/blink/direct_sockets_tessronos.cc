/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	direct_sockets_tessronos.cc
 *	ブラウザ: Direct Sockets (TCPSocket, UDPSocket, TCPServerSocket) on
 *	the system's sockets, for the origins the user lets have what only
 *	an isolated web app has (design 14.4.14).
 *
 *	Chrome gives these to isolated web apps only. This browser has no
 *	such apps; an origin the user allows (the menu, br_isolated_allowed)
 *	is an isolated context for Blink, and its pages see the interfaces.
 *
 *	  - TCP: the name resolved and the connection made on a thread of its
 *	    own; then the received bytes go into a data pipe the page reads
 *	    from (a thread reads the socket), and the bytes the page writes
 *	    into the other pipe are sent (a thread reads the pipe). The end
 *	    of either is told to the socket's observer.
 *	  - TCP server: bound and listening; each accept on a thread of its
 *	    own, the connection then as above.
 *	  - UDP: connected to a peer, or bound to a local address and sending
 *	    to any; the datagrams received are handed to the listener as the
 *	    page asks for more of them.
 */

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/wait.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/base/sockaddr_storage.h"
#include "services/network/public/mojom/network_param.mojom-blink.h"
#include "services/network/public/mojom/restricted_udp_socket.mojom-blink.h"
#include "services/network/public/mojom/ssl_config.mojom-blink.h"
#include "services/network/public/mojom/ssl_info.mojom-blink.h"
#include "services/network/public/mojom/tcp_socket.mojom-blink.h"
#include "services/network/public/mojom/tls_socket.mojom-blink.h"
#include "services/network/public/mojom/udp_socket.mojom-blink.h"
#include "third_party/blink/public/mojom/direct_sockets/direct_sockets.mojom-blink.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"

/* The services' thread (blob_tessronos.cc) */
void br_on_service_thread( base::OnceClosure task );

namespace {

namespace bm = blink::mojom::blink;
namespace nm = network::mojom::blink;

constexpr size_t kPipeBytes = 64 * 1024;

int net_error_of( int e )
{
	switch ( e ) {
	case ECONNREFUSED:	return net::ERR_CONNECTION_REFUSED;
	case ETIMEDOUT:		return net::ERR_CONNECTION_TIMED_OUT;
	case ECONNRESET:	return net::ERR_CONNECTION_RESET;
	case EADDRINUSE:	return net::ERR_ADDRESS_IN_USE;
	case ENETUNREACH:	return net::ERR_ADDRESS_UNREACHABLE;
	case EHOSTUNREACH:	return net::ERR_ADDRESS_UNREACHABLE;
	case EACCES:		return net::ERR_ACCESS_DENIED;
	default:		return net::ERR_FAILED;
	}
}

std::optional<net::IPEndPoint> local_of( int fd )
{
	net::SockaddrStorage ss;
	if ( getsockname(fd, ss.addr(), &ss.addr_len) != 0 ) return std::nullopt;
	net::IPEndPoint ep;
	if ( !ep.FromSockAddr(ss.addr(), ss.addr_len) ) return std::nullopt;
	return ep;
}

std::optional<net::IPEndPoint> peer_of( int fd )
{
	net::SockaddrStorage ss;
	if ( getpeername(fd, ss.addr(), &ss.addr_len) != 0 ) return std::nullopt;
	net::IPEndPoint ep;
	if ( !ep.FromSockAddr(ss.addr(), ss.addr_len) ) return std::nullopt;
	return ep;
}

/* The addresses of a name and port, IPv6 first as the resolver gives them */
std::vector<net::IPEndPoint> resolve( const std::string &host, uint16_t port )
{
	std::vector<net::IPEndPoint> out;
	struct addrinfo hints, *res = nullptr;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if ( getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 ) return out;
	for ( struct addrinfo *a = res; a != nullptr; a = a->ai_next ) {
		net::IPEndPoint ep;
		if ( ep.FromSockAddr(a->ai_addr, a->ai_addrlen) ) out.emplace_back(ep.address(), port);
	}
	freeaddrinfo(res);
	return out;
}

/*
 * A connected TCP socket and its two pipes, shared by the threads that
 * move its bytes and the object the page holds: the socket is closed when
 * the last of them lets go
 */
struct Stream {
	int					fd = -1;
	mojo::ScopedDataPipeProducerHandle	to_page;
	mojo::ScopedDataPipeConsumerHandle	from_page;
	mojo::Remote<nm::SocketObserver>	*observer = nullptr;	/* the services' thread's */
	std::atomic<bool>			closed{false};

	~Stream()
	{
		if ( fd >= 0 ) close(fd);
	}
};

void tell( std::shared_ptr<Stream> s, bool read, int err )
{
	br_on_service_thread(base::BindOnce([]( std::shared_ptr<Stream> s, bool read, int err ) {
		if ( s->observer == nullptr || !s->observer->is_bound() ) return;
		if ( read ) {
			( *s->observer )->OnReadError(err);
		} else {
			( *s->observer )->OnWriteError(err);
		}
	}, s, read, err));
}

/* The socket's bytes into the page's pipe, until the socket ends */
void pump_in( std::shared_ptr<Stream> s )
{
	std::vector<uint8_t> buf(16384);
	for ( ;; ) {
		ssize_t n = recv(s->fd, buf.data(), buf.size(), 0);
		if ( n <= 0 ) {
			tell(s, true, n == 0 ? net::OK : net_error_of(errno));
			break;
		}
		size_t at = 0;
		while ( at < (size_t)n ) {
			size_t done = 0;
			MojoResult r = s->to_page->WriteData(base::span<const uint8_t>(buf).subspan(at, (size_t)n - at),
							     MOJO_WRITE_DATA_FLAG_NONE, done);
			if ( r == MOJO_RESULT_SHOULD_WAIT ) {
				mojo::Wait(s->to_page.get(), MOJO_HANDLE_SIGNAL_WRITABLE);
				continue;
			}
			if ( r != MOJO_RESULT_OK ) {
				s->to_page.reset();
				shutdown(s->fd, SHUT_RD);
				return;
			}
			at += done;
		}
	}
	s->to_page.reset();
}

/* The page's bytes out on the socket, until the page closes its side */
void pump_out( std::shared_ptr<Stream> s )
{
	std::vector<uint8_t> buf(16384);
	for ( ;; ) {
		size_t got = 0;
		MojoResult r = s->from_page->ReadData(MOJO_READ_DATA_FLAG_NONE, base::span<uint8_t>(buf), got);
		if ( r == MOJO_RESULT_SHOULD_WAIT ) {
			mojo::Wait(s->from_page.get(), MOJO_HANDLE_SIGNAL_READABLE);
			continue;
		}
		if ( r != MOJO_RESULT_OK ) {
			shutdown(s->fd, SHUT_WR);	/* the page closed its writable side */
			break;
		}
		size_t at = 0;
		while ( at < got ) {
			ssize_t n = send(s->fd, buf.data() + at, got - at, 0);
			if ( n <= 0 ) {
				tell(s, false, net_error_of(errno));
				s->from_page.reset();
				return;
			}
			at += (size_t)n;
		}
	}
	s->from_page.reset();
}

class TcpConn final : public nm::TCPConnectedSocket {
public:
	TcpConn( std::shared_ptr<Stream> s, mojo::PendingRemote<nm::SocketObserver> observer ) : s_(std::move(s))
	{
		if ( observer ) {
			observer_.Bind(std::move(observer));
			s_->observer = &observer_;
		}
	}

	~TcpConn() override
	{
		s_->observer = nullptr;
		shutdown(s_->fd, SHUT_RDWR);
	}

	void UpgradeToTLS( const net::HostPortPair &, nm::TLSClientSocketOptionsPtr, const net::MutableNetworkTrafficAnnotationTag &,
			   mojo::PendingReceiver<nm::TLSClientSocket>, mojo::PendingRemote<nm::SocketObserver>,
			   UpgradeToTLSCallback cb ) override
	{
		std::move(cb).Run(net::ERR_NOT_IMPLEMENTED, mojo::ScopedDataPipeConsumerHandle(),
				  mojo::ScopedDataPipeProducerHandle(), nullptr);
	}

	void SetSendBufferSize( int32_t size, SetSendBufferSizeCallback cb ) override
	{
		int v = size;
		std::move(cb).Run(setsockopt(s_->fd, SOL_SOCKET, SO_SNDBUF, &v, sizeof(v)) == 0 ? net::OK : net::ERR_FAILED);
	}

	void SetReceiveBufferSize( int32_t size, SetReceiveBufferSizeCallback cb ) override
	{
		int v = size;
		std::move(cb).Run(setsockopt(s_->fd, SOL_SOCKET, SO_RCVBUF, &v, sizeof(v)) == 0 ? net::OK : net::ERR_FAILED);
	}

	void SetNoDelay( bool no_delay, SetNoDelayCallback cb ) override
	{
		int v = no_delay ? 1 : 0;
		std::move(cb).Run(setsockopt(s_->fd, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v)) == 0);
	}

	void SetKeepAlive( bool enable, int32_t, SetKeepAliveCallback cb ) override
	{
		int v = enable ? 1 : 0;
		std::move(cb).Run(setsockopt(s_->fd, SOL_SOCKET, SO_KEEPALIVE, &v, sizeof(v)) == 0);
	}

private:
	std::shared_ptr<Stream>			s_;
	mojo::Remote<nm::SocketObserver>	observer_;
};

/*
 * A connected socket made a Stream with its pipes, the threads started:
 * the page's ends of the pipes
 */
bool start_stream( int fd, std::shared_ptr<Stream> *out, mojo::ScopedDataPipeConsumerHandle *page_reads,
		   mojo::ScopedDataPipeProducerHandle *page_writes )
{
	auto s = std::make_shared<Stream>();
	s->fd = fd;
	if ( mojo::CreateDataPipe(kPipeBytes, s->to_page, *page_reads) != MOJO_RESULT_OK
	     || mojo::CreateDataPipe(kPipeBytes, *page_writes, s->from_page) != MOJO_RESULT_OK ) {
		return false;
	}
	std::thread(pump_in, s).detach();
	std::thread(pump_out, s).detach();
	*out = std::move(s);
	return true;
}

class TcpServer final : public nm::TCPServerSocket {
public:
	explicit TcpServer( int fd ) : fd_(fd) {}
	~TcpServer() override
	{
		shutdown(fd_, SHUT_RDWR);
		close(fd_);
	}

	void Accept( mojo::PendingRemote<nm::SocketObserver> observer, AcceptCallback cb ) override
	{
		int fd = fd_;
		std::thread([fd]( mojo::PendingRemote<nm::SocketObserver> observer, AcceptCallback cb ) {
			int c = accept(fd, nullptr, nullptr);
			int err = ( c < 0 ) ? net_error_of(errno) : net::OK;
			br_on_service_thread(base::BindOnce([]( int c, int err, mojo::PendingRemote<nm::SocketObserver> observer,
								AcceptCallback cb ) {
				std::shared_ptr<Stream> s;
				mojo::ScopedDataPipeConsumerHandle reads;
				mojo::ScopedDataPipeProducerHandle writes;
				if ( c < 0 || !start_stream(c, &s, &reads, &writes) ) {
					if ( c >= 0 ) close(c);
					std::move(cb).Run(err != net::OK ? err : net::ERR_FAILED, std::nullopt, mojo::NullRemote(),
							  mojo::ScopedDataPipeConsumerHandle(), mojo::ScopedDataPipeProducerHandle());
					return;
				}
				mojo::PendingRemote<nm::TCPConnectedSocket> remote;
				mojo::MakeSelfOwnedReceiver(std::make_unique<TcpConn>(s, std::move(observer)),
							    remote.InitWithNewPipeAndPassReceiver());
				std::move(cb).Run(net::OK, peer_of(c), std::move(remote), std::move(reads), std::move(writes));
			}, c, err, std::move(observer), std::move(cb)));
		}, std::move(observer), std::move(cb)).detach();
	}

private:
	int	fd_;
};

/* A UDP socket: datagrams handed to the listener as the page asks for more */
struct Datagrams {
	int					fd = -1;
	bool					connected = false;
	std::mutex				lock;
	std::condition_variable			wake;
	uint32_t				wanted = 0;
	bool					stop = false;
	mojo::Remote<nm::UDPSocketListener>	listener;	/* the services' thread's */

	~Datagrams()
	{
		if ( fd >= 0 ) close(fd);
	}
};

void udp_reader( std::shared_ptr<Datagrams> d )
{
	std::vector<uint8_t> buf(65536);
	for ( ;; ) {
		{
			std::unique_lock<std::mutex> hold(d->lock);
			d->wake.wait(hold, [&] { return d->stop || d->wanted > 0; });
			if ( d->stop ) return;
		}
		net::SockaddrStorage ss;
		ssize_t n = recvfrom(d->fd, buf.data(), buf.size(), 0, ss.addr(), &ss.addr_len);
		if ( n < 0 ) {
			int err = net_error_of(errno);
			{
				std::lock_guard<std::mutex> hold(d->lock);
				if ( d->stop ) return;
			}
			br_on_service_thread(base::BindOnce([]( std::shared_ptr<Datagrams> d, int err ) {
				if ( d->listener.is_bound() ) d->listener->OnReceived(err, std::nullopt, std::nullopt);
			}, d, err));
			return;
		}
		net::IPEndPoint from;
		bool have_from = from.FromSockAddr(ss.addr(), ss.addr_len);
		{
			std::lock_guard<std::mutex> hold(d->lock);
			d->wanted--;
		}
		std::vector<uint8_t> data(buf.begin(), buf.begin() + n);
		br_on_service_thread(base::BindOnce([]( std::shared_ptr<Datagrams> d, std::vector<uint8_t> data,
							std::optional<net::IPEndPoint> from ) {
			if ( !d->listener.is_bound() ) return;
			/* a connected socket's page is not told where its datagrams come from */
			d->listener->OnReceived(net::OK, d->connected ? std::nullopt : from,
						base::span<const uint8_t>(data));
		}, d, std::move(data), have_from ? std::optional<net::IPEndPoint>(from) : std::nullopt));
	}
}

class Udp final : public nm::RestrictedUDPSocket {
public:
	explicit Udp( std::shared_ptr<Datagrams> d ) : d_(std::move(d))
	{
		std::thread(udp_reader, d_).detach();
	}

	~Udp() override
	{
		{
			std::lock_guard<std::mutex> hold(d_->lock);
			d_->stop = true;
		}
		d_->wake.notify_all();
		shutdown(d_->fd, SHUT_RDWR);
	}

	void JoinGroup( const net::IPAddress &, const std::optional<net::IPAddress> &, JoinGroupCallback cb ) override
	{
		std::move(cb).Run(net::ERR_NOT_IMPLEMENTED);
	}

	void LeaveGroup( const net::IPAddress &, const std::optional<net::IPAddress> &, LeaveGroupCallback cb ) override
	{
		std::move(cb).Run(net::ERR_NOT_IMPLEMENTED);
	}

	void ReceiveMore( uint32_t n ) override
	{
		{
			std::lock_guard<std::mutex> hold(d_->lock);
			d_->wanted += n;
		}
		d_->wake.notify_all();
	}

	void Send( base::span<const uint8_t> data, SendCallback cb ) override
	{
		ssize_t n = send(d_->fd, data.data(), data.size(), 0);
		std::move(cb).Run(n < 0 ? net_error_of(errno) : net::OK);
	}

	void SendTo( base::span<const uint8_t> data, const net::HostPortPair &dest, net::DnsQueryType,
		     SendToCallback cb ) override
	{
		std::vector<net::IPEndPoint> to = resolve(dest.host(), dest.port());
		if ( to.empty() ) {
			std::move(cb).Run(net::ERR_NAME_NOT_RESOLVED);
			return;
		}
		net::SockaddrStorage ss;
		if ( !to[0].ToSockAddr(ss.addr(), &ss.addr_len) ) {
			std::move(cb).Run(net::ERR_ADDRESS_INVALID);
			return;
		}
		ssize_t n = sendto(d_->fd, data.data(), data.size(), 0, ss.addr(), ss.addr_len);
		std::move(cb).Run(n < 0 ? net_error_of(errno) : net::OK);
	}

private:
	std::shared_ptr<Datagrams>	d_;
};

/* A socket of the address's family, or -1 */
int socket_for( const net::IPEndPoint &ep, int type )
{
	return socket(ep.GetFamily() == net::ADDRESS_FAMILY_IPV6 ? AF_INET6 : AF_INET, type, 0);
}

class DirectSockets final : public bm::DirectSocketsService {
public:
	void Bind( mojo::PendingReceiver<bm::DirectSocketsService> r ) { receivers_.Add(this, std::move(r)); }

	void OpenTCPSocket( bm::DirectTCPSocketOptionsPtr options, mojo::PendingReceiver<nm::TCPConnectedSocket> receiver,
			    mojo::PendingRemote<nm::SocketObserver> observer, OpenTCPSocketCallback cb ) override
	{
		std::string host = options->remote_addr.host();
		uint16_t port = options->remote_addr.port();
		bool no_delay = options->no_delay;
		std::thread([host, port, no_delay]( mojo::PendingReceiver<nm::TCPConnectedSocket> receiver,
						    mojo::PendingRemote<nm::SocketObserver> observer, OpenTCPSocketCallback cb ) {
			int fd = -1, err = net::ERR_NAME_NOT_RESOLVED;
			for ( const net::IPEndPoint &ep : resolve(host, port) ) {
				net::SockaddrStorage ss;
				if ( !ep.ToSockAddr(ss.addr(), &ss.addr_len) ) continue;
				fd = socket_for(ep, SOCK_STREAM);
				if ( fd >= 0 && connect(fd, ss.addr(), ss.addr_len) == 0 ) break;
				err = net_error_of(errno);
				if ( fd >= 0 ) close(fd);
				fd = -1;
			}
			if ( fd >= 0 && no_delay ) {
				int v = 1;
				(void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v));
			}
			br_on_service_thread(base::BindOnce([]( int fd, int err, mojo::PendingReceiver<nm::TCPConnectedSocket> receiver,
								mojo::PendingRemote<nm::SocketObserver> observer,
								OpenTCPSocketCallback cb ) {
				std::shared_ptr<Stream> s;
				mojo::ScopedDataPipeConsumerHandle reads;
				mojo::ScopedDataPipeProducerHandle writes;
				if ( fd < 0 || !start_stream(fd, &s, &reads, &writes) ) {
					if ( fd >= 0 ) close(fd);
					std::move(cb).Run(fd < 0 ? err : net::ERR_FAILED, std::nullopt, std::nullopt,
							  mojo::ScopedDataPipeConsumerHandle(), mojo::ScopedDataPipeProducerHandle());
					return;
				}
				auto local = local_of(fd), peer = peer_of(fd);
				mojo::MakeSelfOwnedReceiver(std::make_unique<TcpConn>(s, std::move(observer)), std::move(receiver));
				std::move(cb).Run(net::OK, local, peer, std::move(reads), std::move(writes));
			}, fd, err, std::move(receiver), std::move(observer), std::move(cb)));
		}, std::move(receiver), std::move(observer), std::move(cb)).detach();
	}

	void OpenConnectedUDPSocket( bm::DirectConnectedUDPSocketOptionsPtr options,
				     mojo::PendingReceiver<nm::RestrictedUDPSocket> receiver,
				     mojo::PendingRemote<nm::UDPSocketListener> listener,
				     OpenConnectedUDPSocketCallback cb ) override
	{
		std::vector<net::IPEndPoint> to = resolve(options->remote_addr.host(), options->remote_addr.port());
		int fd = -1, err = net::ERR_NAME_NOT_RESOLVED;
		for ( const net::IPEndPoint &ep : to ) {
			net::SockaddrStorage ss;
			if ( !ep.ToSockAddr(ss.addr(), &ss.addr_len) ) continue;
			fd = socket_for(ep, SOCK_DGRAM);
			if ( fd >= 0 && connect(fd, ss.addr(), ss.addr_len) == 0 ) break;
			err = net_error_of(errno);
			if ( fd >= 0 ) close(fd);
			fd = -1;
		}
		if ( fd < 0 ) {
			std::move(cb).Run(err, std::nullopt, std::nullopt);
			return;
		}
		auto d = std::make_shared<Datagrams>();
		d->fd = fd;
		d->connected = true;
		if ( listener ) d->listener.Bind(std::move(listener));
		auto local = local_of(fd), peer = peer_of(fd);
		mojo::MakeSelfOwnedReceiver(std::make_unique<Udp>(d), std::move(receiver));
		std::move(cb).Run(net::OK, local, peer);
	}

	void OpenBoundUDPSocket( bm::DirectBoundUDPSocketOptionsPtr options, mojo::PendingReceiver<nm::RestrictedUDPSocket> receiver,
				 mojo::PendingRemote<nm::UDPSocketListener> listener, OpenBoundUDPSocketCallback cb ) override
	{
		net::SockaddrStorage ss;
		int fd = socket_for(options->local_addr, SOCK_DGRAM);
		if ( fd < 0 || !options->local_addr.ToSockAddr(ss.addr(), &ss.addr_len) || bind(fd, ss.addr(), ss.addr_len) != 0 ) {
			int err = net_error_of(errno);
			if ( fd >= 0 ) close(fd);
			std::move(cb).Run(err, std::nullopt);
			return;
		}
		auto d = std::make_shared<Datagrams>();
		d->fd = fd;
		if ( listener ) d->listener.Bind(std::move(listener));
		auto local = local_of(fd);
		mojo::MakeSelfOwnedReceiver(std::make_unique<Udp>(d), std::move(receiver));
		std::move(cb).Run(net::OK, local);
	}

	void OpenTCPServerSocket( bm::DirectTCPServerSocketOptionsPtr options, mojo::PendingReceiver<nm::TCPServerSocket> receiver,
				  OpenTCPServerSocketCallback cb ) override
	{
		net::SockaddrStorage ss;
		int fd = socket_for(options->local_addr, SOCK_STREAM), one = 1;
		if ( fd >= 0 ) (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		if ( fd < 0 || !options->local_addr.ToSockAddr(ss.addr(), &ss.addr_len) || bind(fd, ss.addr(), ss.addr_len) != 0
		     || listen(fd, options->backlog ? (int)*options->backlog : 16) != 0 ) {
			int err = net_error_of(errno);
			if ( fd >= 0 ) close(fd);
			std::move(cb).Run(err, std::nullopt);
			return;
		}
		auto local = local_of(fd);
		mojo::MakeSelfOwnedReceiver(std::make_unique<TcpServer>(fd), std::move(receiver));
		std::move(cb).Run(net::OK, local);
	}

private:
	mojo::ReceiverSet<bm::DirectSocketsService>	receivers_;
};

DirectSockets	*g_sockets;

}  // namespace

/* The binder of the frames' broker, once when the engine is up */
void br_direct_sockets_init()
{
	blink::GetEmptyBrowserInterfaceBroker().SetBinderForTesting(
		bm::DirectSocketsService::Name_, base::BindRepeating([]( mojo::ScopedMessagePipeHandle h ) {
			br_on_service_thread(base::BindOnce([]( mojo::PendingReceiver<bm::DirectSocketsService> r ) {
				if ( g_sockets == nullptr ) g_sockets = new DirectSockets();
				g_sockets->Bind(std::move(r));
			}, mojo::PendingReceiver<bm::DirectSocketsService>(std::move(h))));
		}));
}
