/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	udp_socket_tessronos.cc
 *	ブラウザ: net::UDPSocketPosix over the system's nonblocking datagram
 *	sockets, polled by the network thread (design 14.5).
 *
 *	QUIC (HTTP/3) runs over these: a session opens one socket, connects
 *	it to the server's address and reads and writes whole datagrams. The
 *	host resolver also opens one to see whether IPv6 reaches the world.
 *
 *	Upstream waits for readiness with a message pump that watches
 *	descriptors. The process has no such pump, so a read or a write that
 *	cannot be done at once is parked with its watcher; the network
 *	thread waits on the parked sockets between its turns
 *	(br_udp_watches), and in each turn (br_udp_pump_turn) tells the
 *	watcher of one that is ready, as the pump would.
 *
 *	The system's sockets take a datagram at a time (recvfrom and
 *	sendto): there is no segmentation offload and no coalesced receive.
 *	A read of several (ReadMultiple, which QUIC's reader uses) takes
 *	every datagram that waits, one call each, up to what the buffer
 *	holds, and hands them over together.
 *
 *	ECN: the type of service (the traffic class for IPv6) of what is
 *	sent is set with IP_TOS (IPV6_TCLASS); once QUIC asks for what
 *	arrives (SetRecvTos), each datagram is read with recvmsg, whose
 *	ancillary data carries the byte it arrived with.
 */

#include "net/socket/udp_socket_posix.h"

#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "net/base/io_buffer.h"
#include "net/base/ip_address.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/base/sockaddr_storage.h"
#include "net/log/net_log.h"
#include "net/log/net_log_event_type.h"
#include "net/log/net_log_source.h"
#include "net/log/net_log_source_type.h"
#include "net/socket/socket_descriptor.h"
#include "net/socket/socket_tag.h"
#include "net/socket/udp_socket_global_limits.h"

#include "br_stream.h"

namespace net {

namespace {

// A read or a write waiting for its socket: the descriptor looked at and
// the watcher told when it is ready.
struct Wait {
  int fd;
  bool write;
  base::MessagePumpForIO::FdWatcher* watcher;
};

std::vector<Wait>& Waits() {
  static std::vector<Wait>* list = new std::vector<Wait>();
  return *list;
}

void Watch(int fd, bool write, base::MessagePumpForIO::FdWatcher* watcher) {
  for (Wait& w : Waits()) {
    if (w.watcher == watcher) {
      w.fd = fd;
      w.write = write;
      return;
    }
  }
  Waits().push_back(Wait{fd, write, watcher});
}

void Unwatch(const base::MessagePumpForIO::FdWatcher* watcher) {
  auto& list = Waits();
  list.erase(std::remove_if(list.begin(), list.end(),
                            [watcher](const Wait& w) {
                              return w.watcher == watcher;
                            }),
             list.end());
}

bool StillWaiting(const Wait& w) {
  for (const Wait& x : Waits()) {
    if (x.watcher == w.watcher && x.fd == w.fd && x.write == w.write)
      return true;
  }
  return false;
}

// What a connected socket carried, told on the console: where it goes when
// it first sends, when the first answer comes and after how many datagrams
// sent, the first error it meets, and the counts when it is closed.
struct Traffic {
  std::string peer;
  unsigned sent = 0;
  unsigned received = 0;
  int32_t receive_buffer = 0;
  int error = 0;
};

std::map<int, Traffic>& Traffics() {
  static std::map<int, Traffic>* map = new std::map<int, Traffic>();
  return *map;
}

void Count(int fd, int rv, bool sent) {
  auto it = Traffics().find(fd);
  if (it == Traffics().end())
    return;
  Traffic& t = it->second;
  if (rv >= 0) {
    (sent ? t.sent : t.received)++;
    if (sent && t.sent == 1)
      printf("browser: net: udp to %s, receive buffer %d\n", t.peer.c_str(),
             static_cast<int>(t.receive_buffer));
    if (!sent && t.received == 1) {
      printf("browser: net: udp to %s: first answer after %u sent\n",
             t.peer.c_str(), t.sent);
    }
  } else if (rv != ERR_IO_PENDING && t.error == 0) {
    t.error = rv;
    printf("browser: net: udp to %s: %s after %u sent, %u received\n",
           t.peer.c_str(), ErrorToShortString(rv).c_str(), t.sent, t.received);
  }
}

// The stack refuses a buffer larger than it allows one socket to hold;
// a size it refuses is asked for again at half, down to this.
constexpr int32_t kSmallestBuffer = 16 * 1024;

int SetBuffer(int fd, int name, int32_t size) {
  for (int32_t s = size; s >= kSmallestBuffer; s /= 2) {
    if (setsockopt(fd, SOL_SOCKET, name, &s, sizeof(s)) == 0) {
      auto it = Traffics().find(fd);
      if (it != Traffics().end() && name == SO_RCVBUF)
        it->second.receive_buffer = s;
      return OK;
    }
  }
  // What the socket has already is kept: datagrams still flow, more of
  // them are dropped when the reader falls behind.
  return OK;
}

// The sockets that asked for the type of service of what they receive
std::set<int>& TosFds() {
  static std::set<int>* set = new std::set<int>();
  return *set;
}

// recvfrom, and through recvmsg the type of service (or traffic class) the
// datagram arrived with into |tos| when the socket asked for it (0 when
// it did not, or when it did not come)
ssize_t RecvTos(int fd,
                char* p,
                size_t len,
                sockaddr* addr,
                socklen_t* addr_len,
                uint8_t* tos) {
  *tos = 0;
  if (!TosFds().count(fd))
    return recvfrom(fd, p, len, MSG_DONTWAIT, addr, addr_len);
  iovec iov = {p, len};
  alignas(cmsghdr) unsigned char ctl[CMSG_SPACE(sizeof(int))];
  msghdr m = {};
  m.msg_name = addr;
  m.msg_namelen = addr_len ? *addr_len : 0;
  m.msg_iov = &iov;
  m.msg_iovlen = 1;
  m.msg_control = ctl;
  m.msg_controllen = sizeof(ctl);
  const ssize_t n = recvmsg(fd, &m, MSG_DONTWAIT);
  if (n < 0)
    return n;
  if (addr_len)
    *addr_len = m.msg_namelen;
  const cmsghdr* c = CMSG_FIRSTHDR(&m);
  if (c && c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_TOS)
    *tos = *CMSG_DATA(c);
  else if (c && c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_TCLASS)
    *tos = static_cast<uint8_t>(*reinterpret_cast<const int*>(CMSG_DATA(c)));
  return n;
}

// One datagram into |p|: its length, ERR_IO_PENDING when none waits,
// ERR_MSG_TOO_BIG when it filled |len| (it may have been cut short: the
// system does not say), or the error the socket holds. The type of
// service it came with goes into |tos|.
int RecvOne(int fd, char* p, int len, uint8_t* tos) {
  const ssize_t n =
      RecvTos(fd, p, static_cast<size_t>(len), nullptr, nullptr, tos);
  if (n < 0) {
    const int err = errno;
    if (err == EAGAIN || err == EWOULDBLOCK)
      return ERR_IO_PENDING;
    Count(fd, MapSystemError(err), false);
    return MapSystemError(err);
  }
  if (n == len)
    return ERR_MSG_TOO_BIG;
  Count(fd, static_cast<int>(n), false);
  return static_cast<int>(n);
}

// Every datagram that waits, one after another in |buf| while a whole one
// of |max| bytes still fits. One that may have been cut short is dropped,
// as a single read drops it. An error after some datagrams ends the batch
// and those are handed over; ERR_IO_PENDING when none waited.
base::expected<DatagramsMetadata, Error> RecvBatch(int fd,
                                                   IOBuffer* buf,
                                                   size_t buf_len,
                                                   size_t max) {
  DatagramsMetadata got;
  for (size_t off = 0; off + max <= buf_len;) {
    uint8_t tos = 0;
    const int n = RecvOne(fd, buf->data() + off, static_cast<int>(max), &tos);
    if (n == ERR_IO_PENDING)
      break;
    if (n == ERR_MSG_TOO_BIG)
      continue;
    if (n < 0) {
      if (got.empty())
        return base::unexpected(static_cast<Error>(n));
      break;
    }
    got.push_back(DatagramMetadata{
        .offset = off, .length = static_cast<size_t>(n), .tos = tos});
    off += static_cast<size_t>(n);
  }
  if (got.empty())
    return base::unexpected(ERR_IO_PENDING);
  return got;
}

}  // namespace

UDPSocketPosix::UDPSocketPosix(DatagramSocket::BindType bind_type,
                               net::NetLog* net_log,
                               const net::NetLogSource& source)
    : socket_(kInvalidSocket),
      bind_type_(bind_type),
      read_socket_watcher_(FROM_HERE),
      write_socket_watcher_(FROM_HERE),
      read_watcher_(this),
      write_watcher_(this),
      net_log_(NetLogWithSource::Make(net_log, NetLogSourceType::UDP_SOCKET)),
      bound_network_(handles::kInvalidNetworkHandle) {
  net_log_.BeginEventReferencingSource(NetLogEventType::SOCKET_ALIVE, source);
}

UDPSocketPosix::UDPSocketPosix(DatagramSocket::BindType bind_type,
                               NetLogWithSource source_net_log)
    : socket_(kInvalidSocket),
      bind_type_(bind_type),
      read_socket_watcher_(FROM_HERE),
      write_socket_watcher_(FROM_HERE),
      read_watcher_(this),
      write_watcher_(this),
      net_log_(source_net_log),
      bound_network_(handles::kInvalidNetworkHandle) {
  net_log_.BeginEventReferencingSource(NetLogEventType::SOCKET_ALIVE,
                                       net_log_.source());
}

UDPSocketPosix::~UDPSocketPosix() {
  Close();
  net_log_.EndEvent(NetLogEventType::SOCKET_ALIVE);
}

int UDPSocketPosix::Open(AddressFamily address_family) {
  if (socket_ != kInvalidSocket)
    return ERR_UNEXPECTED;
  if (address_family != ADDRESS_FAMILY_IPV4 &&
      address_family != ADDRESS_FAMILY_IPV6) {
    return ERR_ADDRESS_INVALID;
  }
  OwnedUDPSocketCount count = TryAcquireGlobalUDPSocketCount();
  if (count.empty())
    return ERR_INSUFFICIENT_RESOURCES;
  const int family = address_family == ADDRESS_FAMILY_IPV6 ? AF_INET6 : AF_INET;
  const int fd = socket(family, SOCK_DGRAM | SOCK_NONBLOCK, 0);
  if (fd < 0) {
    const int err = errno;
    printf("browser: net: no datagram socket (%d)\n", err);
    return MapSystemError(err);
  }
  owned_socket_count_ = std::move(count);
  socket_ = fd;
  addr_family_ = family;
  return OK;
}

int UDPSocketPosix::AdoptOpenedSocket(AddressFamily address_family,
                                      int socket) {
  if (socket_ != kInvalidSocket)
    return ERR_UNEXPECTED;
  OwnedUDPSocketCount count = TryAcquireGlobalUDPSocketCount();
  if (count.empty())
    return ERR_INSUFFICIENT_RESOURCES;
  if (!base::SetNonBlocking(socket))
    return MapSystemError(errno);
  owned_socket_count_ = std::move(count);
  socket_ = socket;
  addr_family_ = address_family == ADDRESS_FAMILY_IPV6 ? AF_INET6 : AF_INET;
  return OK;
}

void UDPSocketPosix::Close() {
  owned_socket_count_.Reset();
  if (socket_ == kInvalidSocket)
    return;

  // Whatever was parked is dropped unanswered, as upstream does.
  Unwatch(&read_watcher_);
  Unwatch(&write_watcher_);
  read_buf_.reset();
  read_buf_len_ = 0;
  read_multiple_maximum_packet_size_ = 0;
  read_callback_.Reset();
  read_multiple_callback_.Reset();
  recv_from_address_ = nullptr;
  write_buf_.reset();
  write_buf_len_ = 0;
  write_callback_.Reset();
  send_to_address_.reset();

  auto it = Traffics().find(socket_);
  if (it != Traffics().end()) {
    const Traffic& t = it->second;
    // A socket that never sent (the resolver's look at whether IPv6
    // reaches the world) goes without a word.
    if (t.sent > 0)
      printf("browser: net: udp to %s closed, %u sent, %u received, receive buffer %d, error %d\n",
           t.peer.c_str(), t.sent, t.received, static_cast<int>(t.receive_buffer), t.error);
    Traffics().erase(it);
  }
  TosFds().erase(socket_);
  close(socket_);
  socket_ = kInvalidSocket;
  addr_family_ = 0;
  is_connected_ = false;
  local_address_.reset();
  remote_address_.reset();
  tag_ = SocketTag();
}

int UDPSocketPosix::GetPeerAddress(IPEndPoint* address) const {
  if (!is_connected())
    return ERR_SOCKET_NOT_CONNECTED;
  if (!remote_address_) {
    SockaddrStorage storage;
    if (getpeername(socket_, storage.addr(), &storage.addr_len) != 0)
      return MapSystemError(errno);
    auto endpoint = std::make_unique<IPEndPoint>();
    if (!endpoint->FromSockAddr(storage.addr(), storage.addr_len))
      return ERR_ADDRESS_INVALID;
    remote_address_ = std::move(endpoint);
  }
  *address = *remote_address_;
  return OK;
}

int UDPSocketPosix::GetLocalAddress(IPEndPoint* address) const {
  if (!is_connected())
    return ERR_SOCKET_NOT_CONNECTED;
  if (!local_address_) {
    SockaddrStorage storage;
    if (getsockname(socket_, storage.addr(), &storage.addr_len) != 0)
      return MapSystemError(errno);
    auto endpoint = std::make_unique<IPEndPoint>();
    if (!endpoint->FromSockAddr(storage.addr(), storage.addr_len))
      return ERR_ADDRESS_INVALID;
    local_address_ = std::move(endpoint);
  }
  *address = *local_address_;
  return OK;
}

// ---- reading ---------------------------------------------------------------

int UDPSocketPosix::Read(IOBuffer* buf,
                         int buf_len,
                         CompletionOnceCallback callback) {
  return RecvFrom(buf, buf_len, nullptr, std::move(callback));
}

int UDPSocketPosix::RecvFrom(IOBuffer* buf,
                             int buf_len,
                             IPEndPoint* address,
                             CompletionOnceCallback callback) {
  if (socket_ == kInvalidSocket)
    return ERR_SOCKET_NOT_CONNECTED;
  CHECK(read_callback_.is_null());
  CHECK(read_multiple_callback_.is_null());

  const int nread = InternalRecvFrom(buf, buf_len, address);
  if (nread != ERR_IO_PENDING)
    return nread;

  Watch(socket_, false, &read_watcher_);
  read_buf_ = buf;
  read_buf_len_ = buf_len;
  recv_from_address_ = address;
  read_callback_ = std::move(callback);
  return ERR_IO_PENDING;
}

// The datagrams that wait, read one call each (the system has no call that
// takes several, and none coalesces them), handed over in one answer.
base::expected<DatagramsMetadata, Error> UDPSocketPosix::ReadMultiple(
    IOBuffer* buffer,
    size_t buf_len,
    size_t maximum_packet_size,
    base::OnceCallback<void(base::expected<DatagramsMetadata, Error>)>
        callback) {
  if (socket_ == kInvalidSocket)
    return base::unexpected(ERR_INVALID_HANDLE);
  CHECK(read_multiple_callback_.is_null());
  CHECK(read_callback_.is_null());
  CHECK(!callback.is_null());
  CHECK_GT(maximum_packet_size, 0u);
  CHECK_GE(buf_len, maximum_packet_size);

  last_tos_ = 0;
  base::expected<DatagramsMetadata, Error> got =
      RecvBatch(socket_, buffer, buf_len, maximum_packet_size);
  if (got.has_value() || got.error() != ERR_IO_PENDING)
    return got;

  Watch(socket_, false, &read_watcher_);
  read_buf_ = buffer;
  read_buf_len_ = static_cast<int>(buf_len);
  read_multiple_maximum_packet_size_ = maximum_packet_size;
  read_multiple_callback_ = std::move(callback);
  return base::unexpected(ERR_IO_PENDING);
}

int UDPSocketPosix::InternalRecvFrom(IOBuffer* buf,
                                     int buf_len,
                                     IPEndPoint* address) {
  SockaddrStorage storage;
  uint8_t tos = 0;
  const ssize_t n = RecvTos(socket_, buf->data(), static_cast<size_t>(buf_len),
                            storage.addr(), &storage.addr_len, &tos);
  if (n < 0) {
    const int err = errno;
    if (err == EAGAIN || err == EWOULDBLOCK)
      return ERR_IO_PENDING;
    Count(socket_, MapSystemError(err), false);
    return MapSystemError(err);
  }
  // A datagram that fills the buffer may have been cut short: the system
  // does not say whether it was, and the callers leave a byte spare.
  if (n == buf_len)
    return ERR_MSG_TOO_BIG;
  Count(socket_, static_cast<int>(n), false);
  last_tos_ = tos;
  if (address != nullptr) {
    if (is_connected_ && remote_address_) {
      *address = *remote_address_;
    } else if (!address->FromSockAddr(storage.addr(), storage.addr_len)) {
      return ERR_ADDRESS_INVALID;
    }
  }
  return static_cast<int>(n);
}

void UDPSocketPosix::ReadWatcher::OnFileCanReadWithoutBlocking(int) {
  if (!socket_->read_callback_.is_null()) {
    socket_->DidCompleteRead();
  } else if (!socket_->read_multiple_callback_.is_null()) {
    socket_->DidCompleteMultipleRead();
  }
}

void UDPSocketPosix::DidCompleteRead() {
  const int result =
      InternalRecvFrom(read_buf_.get(), read_buf_len_, recv_from_address_);
  if (result == ERR_IO_PENDING)
    return;
  read_buf_.reset();
  read_buf_len_ = 0;
  recv_from_address_ = nullptr;
  Unwatch(&read_watcher_);
  DoReadCallback(result);
}

void UDPSocketPosix::DidCompleteMultipleRead() {
  base::expected<DatagramsMetadata, Error> got =
      RecvBatch(socket_, read_buf_.get(), static_cast<size_t>(read_buf_len_),
                read_multiple_maximum_packet_size_);
  if (!got.has_value() && got.error() == ERR_IO_PENDING)
    return;
  read_buf_.reset();
  read_buf_len_ = 0;
  read_multiple_maximum_packet_size_ = 0;
  Unwatch(&read_watcher_);
  DoReadMultipleCallback(std::move(got));
}

void UDPSocketPosix::DoReadCallback(int rv) {
  // Running it may start the next read, so it is taken out first.
  std::move(read_callback_).Run(rv);
}

void UDPSocketPosix::DoReadMultipleCallback(
    base::expected<DatagramsMetadata, Error> rv) {
  std::move(read_multiple_callback_).Run(std::move(rv));
}

// ---- writing ---------------------------------------------------------------

int UDPSocketPosix::Write(
    IOBuffer* buf,
    int buf_len,
    CompletionOnceCallback callback,
    const NetworkTrafficAnnotationTag& traffic_annotation) {
  return SendToOrWrite(buf, buf_len, nullptr, std::move(callback));
}

int UDPSocketPosix::SendTo(IOBuffer* buf,
                           int buf_len,
                           const IPEndPoint& address,
                           CompletionOnceCallback callback) {
  return SendToOrWrite(buf, buf_len, &address, std::move(callback));
}

int UDPSocketPosix::SendToOrWrite(IOBuffer* buf,
                                  int buf_len,
                                  const IPEndPoint* address,
                                  CompletionOnceCallback callback) {
  if (socket_ == kInvalidSocket)
    return ERR_SOCKET_NOT_CONNECTED;
  CHECK(write_callback_.is_null());

  const int result = InternalSendTo(buf, buf_len, address);
  if (result != ERR_IO_PENDING)
    return result;

  Watch(socket_, true, &write_watcher_);
  write_buf_ = buf;
  write_buf_len_ = buf_len;
  if (address != nullptr)
    send_to_address_ = std::make_unique<IPEndPoint>(*address);
  write_callback_ = std::move(callback);
  return ERR_IO_PENDING;
}

int UDPSocketPosix::InternalSendTo(IOBuffer* buf,
                                   int buf_len,
                                   const IPEndPoint* address) {
  SockaddrStorage storage;
  struct sockaddr* to = nullptr;
  socklen_t to_len = 0;
  if (address != nullptr) {
    if (!address->ToSockAddr(storage.addr(), &storage.addr_len))
      return ERR_ADDRESS_INVALID;
    to = storage.addr();
    to_len = storage.addr_len;
  }
  const ssize_t n = sendto(socket_, buf->data(), static_cast<size_t>(buf_len),
                           MSG_DONTWAIT, to, to_len);
  if (n < 0) {
    const int err = errno;
    if (err == EAGAIN || err == EWOULDBLOCK)
      return ERR_IO_PENDING;
    Count(socket_, MapSystemError(err), true);
    return MapSystemError(err);
  }
  Count(socket_, static_cast<int>(n), true);
  return static_cast<int>(n);
}

void UDPSocketPosix::WriteWatcher::OnFileCanWriteWithoutBlocking(int) {
  if (!socket_->write_callback_.is_null())
    socket_->DidCompleteWrite();
}

void UDPSocketPosix::DidCompleteWrite() {
  const int result =
      InternalSendTo(write_buf_.get(), write_buf_len_, send_to_address_.get());
  if (result == ERR_IO_PENDING)
    return;
  write_buf_.reset();
  write_buf_len_ = 0;
  send_to_address_.reset();
  Unwatch(&write_watcher_);
  DoWriteCallback(result);
}

void UDPSocketPosix::DoWriteCallback(int rv) {
  std::move(write_callback_).Run(rv);
}

// ---- addresses -------------------------------------------------------------

int UDPSocketPosix::Connect(const IPEndPoint& address) {
  if (socket_ == kInvalidSocket)
    return ERR_SOCKET_NOT_CONNECTED;
  const int rv = InternalConnect(address);
  is_connected_ = (rv == OK);
  if (rv != OK)
    tag_ = SocketTag();
  return rv;
}

int UDPSocketPosix::InternalConnect(const IPEndPoint& address) {
  if (bind_type_ == DatagramSocket::RANDOM_BIND) {
    // A port of the system's choosing: it hands them out in turn from its
    // range of ephemeral ports.
    const size_t size = address.GetSockAddrFamily() == AF_INET
                            ? IPAddress::kIPv4AddressSize
                            : IPAddress::kIPv6AddressSize;
    const int rv = DoBind(IPEndPoint(IPAddress::AllZeros(size), 0));
    if (rv < 0)
      return rv;
  }
  SockaddrStorage storage;
  if (!address.ToSockAddr(storage.addr(), &storage.addr_len))
    return ERR_ADDRESS_INVALID;
  if (connect(socket_, storage.addr(), storage.addr_len) != 0)
    return MapSystemError(errno);
  remote_address_ = std::make_unique<IPEndPoint>(address);
  Traffics()[socket_].peer = address.ToString();
  local_address_.reset();
  return OK;
}

int UDPSocketPosix::Bind(const IPEndPoint& address) {
  if (socket_ == kInvalidSocket)
    return ERR_SOCKET_NOT_CONNECTED;
  const int rv = DoBind(address);
  if (rv < 0)
    return rv;
  is_connected_ = true;
  local_address_.reset();
  return rv;
}

int UDPSocketPosix::DoBind(const IPEndPoint& address) {
  SockaddrStorage storage;
  if (!address.ToSockAddr(storage.addr(), &storage.addr_len))
    return ERR_ADDRESS_INVALID;
  if (bind(socket_, storage.addr(), storage.addr_len) == 0)
    return OK;
  return MapSystemError(errno);
}

int UDPSocketPosix::BindToNetwork(handles::NetworkHandle network) {
  return ERR_NOT_IMPLEMENTED;
}

// ---- options ---------------------------------------------------------------

int UDPSocketPosix::SetReceiveBufferSize(int32_t size) {
  if (socket_ == kInvalidSocket)
    return ERR_SOCKET_NOT_CONNECTED;
  return SetBuffer(socket_, SO_RCVBUF, size);
}

int UDPSocketPosix::SetSendBufferSize(int32_t size) {
  if (socket_ == kInvalidSocket)
    return ERR_SOCKET_NOT_CONNECTED;
  return SetBuffer(socket_, SO_SNDBUF, size);
}

// The system has no switch for it; QUIC keeps its datagrams within the
// path's size and takes this answer as "not here".
int UDPSocketPosix::SetDoNotFragment() {
  return ERR_NOT_IMPLEMENTED;
}

// The type of service (traffic class) of what arrives is read from now on.
// An IPv6 socket here is IPv6 only, so the IPv4 option is not set beside
// it.
int UDPSocketPosix::SetRecvTos() {
  if (socket_ == kInvalidSocket)
    return ERR_SOCKET_NOT_CONNECTED;
  int on = 1;
  const int rv =
      addr_family_ == AF_INET6
          ? setsockopt(socket_, IPPROTO_IPV6, IPV6_RECVTCLASS, &on, sizeof(on))
          : setsockopt(socket_, IPPROTO_IP, IP_RECVTOS, &on, sizeof(on));
  if (rv != 0)
    return MapSystemError(errno);
  TosFds().insert(socket_);
  return OK;
}

void UDPSocketPosix::SetMsgConfirm(bool confirm) {}

int UDPSocketPosix::AllowAddressReuse() {
  if (socket_ == kInvalidSocket)
    return ERR_SOCKET_NOT_CONNECTED;
  int on = 1;
  return setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == 0
             ? OK
             : MapSystemError(errno);
}

int UDPSocketPosix::SetBroadcast(bool broadcast) {
  if (socket_ == kInvalidSocket)
    return ERR_SOCKET_NOT_CONNECTED;
  int on = broadcast ? 1 : 0;
  return setsockopt(socket_, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on)) == 0
             ? OK
             : MapSystemError(errno);
}

int UDPSocketPosix::AllowAddressSharingForMulticast() {
  return AllowAddressReuse();
}

// Multicast is not carried: the groups are neither joined nor left.
int UDPSocketPosix::JoinGroup(const IPAddress& group_address) const {
  return ERR_NOT_IMPLEMENTED;
}

int UDPSocketPosix::LeaveGroup(const IPAddress& group_address) const {
  return ERR_NOT_IMPLEMENTED;
}

int UDPSocketPosix::JoinSourceGroup(const IPAddress& group_address,
                                    const IPAddress& source_address) const {
  return ERR_NOT_IMPLEMENTED;
}

int UDPSocketPosix::LeaveSourceGroup(const IPAddress& group_address,
                                     const IPAddress& source_address) const {
  return ERR_NOT_IMPLEMENTED;
}

int UDPSocketPosix::SetMulticastInterface(uint32_t interface_index) {
  if (is_connected())
    return ERR_SOCKET_IS_CONNECTED;
  multicast_interface_ = interface_index;
  return OK;
}

int UDPSocketPosix::SetMulticastTimeToLive(int time_to_live) {
  if (is_connected())
    return ERR_SOCKET_IS_CONNECTED;
  if (time_to_live < 0 || time_to_live > 255)
    return ERR_INVALID_ARGUMENT;
  multicast_time_to_live_ = time_to_live;
  return OK;
}

int UDPSocketPosix::SetMulticastLoopbackMode(bool loopback) {
  if (is_connected())
    return ERR_SOCKET_IS_CONNECTED;
  if (loopback)
    socket_options_ |= SOCKET_OPTION_MULTICAST_LOOP;
  else
    socket_options_ &= ~SOCKET_OPTION_MULTICAST_LOOP;
  return OK;
}

int UDPSocketPosix::SetDiffServCodePoint(DiffServCodePoint dscp) {
  return SetTos(dscp, ECN_NO_CHANGE);
}

// The type of service (IPv4) or traffic class (IPv6) of what is sent; the
// part not given is kept as it is.
int UDPSocketPosix::SetTos(DiffServCodePoint dscp, EcnCodePoint ecn) {
  if (dscp == DSCP_NO_CHANGE && ecn == ECN_NO_CHANGE)
    return OK;
  if (socket_ == kInvalidSocket)
    return ERR_SOCKET_NOT_CONNECTED;
  const int level = addr_family_ == AF_INET6 ? IPPROTO_IPV6 : IPPROTO_IP;
  const int name = addr_family_ == AF_INET6 ? IPV6_TCLASS : IP_TOS;
  int v = (dscp << 2) | ecn;
  if (dscp == DSCP_NO_CHANGE || ecn == ECN_NO_CHANGE) {
    int old = 0;
    socklen_t size = sizeof(old);
    if (getsockopt(socket_, level, name, &old, &size) != 0)
      return MapSystemError(errno);
    v = dscp == DSCP_NO_CHANGE ? ((old & ~ECN_LAST) | ecn)
                               : ((old & ECN_LAST) | (dscp << 2));
  }
  if (setsockopt(socket_, level, name, &v, sizeof(v)) != 0)
    return MapSystemError(errno);
  return OK;
}

int UDPSocketPosix::SetIPv6Only(bool ipv6_only) {
  if (socket_ == kInvalidSocket)
    return ERR_SOCKET_NOT_CONNECTED;
  if (is_connected())
    return ERR_SOCKET_IS_CONNECTED;
  int on = ipv6_only ? 1 : 0;
  return setsockopt(socket_, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on)) == 0
             ? OK
             : MapSystemError(errno);
}

void UDPSocketPosix::DetachFromThread() {}

void UDPSocketPosix::ApplySocketTag(const SocketTag& tag) {
  tag_ = tag;
}

int UDPSocketPosix::SetIOSNetworkServiceType(int ios_network_service_type) {
  return OK;
}

// The payload is what a socket would send when the process dies with the
// connection open; this process does not send it.
void UDPSocketPosix::RegisterQuicConnectionClosePayload(
    base::span<uint8_t> payload) {}

void UDPSocketPosix::UnregisterQuicConnectionClosePayload() {}

}  // namespace net

// One look at the parked sockets, on the network thread (from
// br_tcp_pump_turn, inside its scope): each that became ready is told to
// its watcher, which reads or writes and runs the caller's callback.
extern "C" void br_udp_pump_turn(void) {
  using net::Wait;
  if (net::Waits().empty())
    return;
  // A callback can start or end waits, so the walk is over a copy and each
  // entry is checked to be still waited for before it is looked at.
  const std::vector<Wait> now = net::Waits();
  for (const Wait& w : now) {
    if (!net::StillWaiting(w))
      continue;
    const int ready = w.write ? br_stream_writable(w.fd) : br_stream_readable(w.fd);
    if (ready == 0)
      continue;
    // An error pending on the socket is ready too: the read or write that
    // follows takes it and reports it.
    if (w.write)
      w.watcher->OnFileCanWriteWithoutBlocking(w.fd);
    else
      w.watcher->OnFileCanReadWithoutBlocking(w.fd);
  }
}

// The parked sockets, for the network thread to wait on between turns.
extern "C" int br_udp_watches(BrWatch* out, int max) {
  int n = 0;
  for (const net::Wait& w : net::Waits()) {
    if (n >= max)
      break;
    out[n].fd = w.fd;
    out[n].events = w.write ? BR_WATCH_WRITE : BR_WATCH_READ;
    n++;
  }
  return n;
}
