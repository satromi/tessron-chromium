/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	tcp_socket_tessronos.cc
 *	ブラウザ: net::TCPSocketPosix over nonblocking sockets polled by the
 *	network thread (design 17.19).
 *
 *	Upstream splits this in two: TCPSocketPosix keeps the policy and a
 *	SocketPosix underneath does the descriptor work, waiting for
 *	readiness with a message pump that watches descriptors. The pump of
 *	the process does not watch sockets, so the lower half is not used
 *	and this stands in for the whole of it: readiness is polled by the
 *	network thread each turn (br_tcp_pump_turn).
 *
 *	Requests that cannot be served at once are parked with their buffer
 *	and callback and retried. IPv4 only.
 */

#include "net/socket/tcp_socket_posix.h"

#include <stdio.h>

#include <vector>

#include "base/functional/callback.h"
#include "net/base/io_buffer.h"
#include "base/memory/ptr_util.h"
#include "net/base/ip_address.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/socket/socket_performance_watcher.h"
#include "net/socket/socket_posix.h"
#include "base/functional/bind.h"
#include "net/base/address_list.h"
#include "net/dns/host_resolver_system_task.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"

#include "br_stream.h"


// A bounded trace of what the sockets are asked to do: each connect, read,
// write and readiness report, with its result. Silent unless the budget is
// raised to watch the transport.
int g_trace_left = 0;

#define BSTRACE(...)                     \
  do {                                   \
    if (g_trace_left > 0) {              \
      g_trace_left--;                    \
      printf(__VA_ARGS__);              \
    }                                    \
  } while (0)


namespace net {

namespace {

// What this port keeps for a socket, held beside the object because the
// class cannot be given new members.
struct Stream {
  const TCPSocketPosix* owner = nullptr;
  int fd = -1;
  bool connecting = false;
  bool connected = false;
  IPEndPoint peer;

  // A read and a write can be waiting at once -- an encrypted connection
  // routinely has both -- so each keeps its own buffer and its own callback.
  // Sharing one slot loses whichever wait came second, and the caller is
  // left waiting for a callback that will never be made.
  bool reading = false;
  scoped_refptr<IOBuffer> read_buf;
  int read_len = 0;
  CompletionOnceCallback read_callback;

  // A wait started by ReadIfReady rather than Read. The two report
  // differently when they finish: Read answers with the bytes it took,
  // ReadIfReady answers that the socket is ready and is then asked
  // again. Reporting a count for the second is not a larger number --
  // it is the wrong kind of answer, and the caller refuses it.
  bool read_if_ready = false;

  bool writing = false;
  scoped_refptr<IOBuffer> write_buf;
  int write_len = 0;
  CompletionOnceCallback write_callback;
  // A connect in progress reports through its own callback.
  CompletionOnceCallback connect_callback;
};

std::vector<Stream>& Streams() {
  static std::vector<Stream>* list = new std::vector<Stream>();
  return *list;
}

Stream* Find(const TCPSocketPosix* owner) {
  for (auto& s : Streams()) {
    if (s.owner == owner)
      return &s;
  }
  return nullptr;
}

Stream& FindOrAdd(const TCPSocketPosix* owner) {
  if (Stream* s = Find(owner))
    return *s;
  Streams().push_back(Stream());
  Streams().back().owner = owner;
  return Streams().back();
}

void Drop(const TCPSocketPosix* owner) {
  auto& list = Streams();
  for (size_t i = list.size(); i > 0; --i) {
    if (list[i - 1].owner == owner)
      list.erase(list.begin() + (i - 1));
  }
}

// An endpoint crosses to br_stream as four address bytes and a host-order
// port. An address that is not IPv4 cannot be carried.
bool ToBridge(const IPEndPoint& endpoint, unsigned char out[4], int* port) {
  const IPAddress& address = endpoint.address();
  if (!address.IsIPv4())
    return false;
  const auto bytes = address.bytes();
  for (int i = 0; i < 4; ++i)
    out[i] = bytes[i];
  *port = endpoint.port();
  return true;
}

IPEndPoint FromBridge(const unsigned char addr[4], int port) {
  return IPEndPoint(IPAddress(addr[0], addr[1], addr[2], addr[3]),
                    static_cast<uint16_t>(port));
}

int MapBridgeResult(int rv) {
  if (rv > 0)
    return rv;
  if (rv == 0)
    return 0;
  if (rv == BR_STREAM_WOULDBLOCK)
    return ERR_IO_PENDING;
  return ERR_CONNECTION_FAILED;
}

}  // namespace

// static
std::unique_ptr<TCPSocketPosix> TCPSocketPosix::Create(
    std::unique_ptr<SocketPerformanceWatcher> socket_performance_watcher,
    NetLog* net_log,
    const NetLogSource& source) {
  return base::WrapUnique(new TCPSocketPosix(
      std::move(socket_performance_watcher), net_log, source));
}

// static
std::unique_ptr<TCPSocketPosix> TCPSocketPosix::Create(
    std::unique_ptr<SocketPerformanceWatcher> socket_performance_watcher,
    NetLogWithSource net_log_source) {
  return base::WrapUnique(new TCPSocketPosix(
      std::move(socket_performance_watcher), std::move(net_log_source)));
}

TCPSocketPosix::TCPSocketPosix(
    std::unique_ptr<SocketPerformanceWatcher> socket_performance_watcher,
    NetLog* net_log,
    const NetLogSource& source)
    : TCPSocketPosix(std::move(socket_performance_watcher),
                     NetLogWithSource::Make(net_log,
                                            NetLogSourceType::SOCKET)) {}

TCPSocketPosix::TCPSocketPosix(
    std::unique_ptr<SocketPerformanceWatcher> socket_performance_watcher,
    NetLogWithSource net_log_source)
    : socket_performance_watcher_(std::move(socket_performance_watcher)),
      net_log_(net_log_source) {}

TCPSocketPosix::~TCPSocketPosix() {
  Close();
}

int TCPSocketPosix::Open(AddressFamily family) {
  if (family != ADDRESS_FAMILY_IPV4)
    return ERR_ADDRESS_INVALID;
  Stream& s = FindOrAdd(this);
  if (s.fd >= 0)
    return ERR_UNEXPECTED;
  s.fd = br_stream_open();
  if (s.fd < 0) {
    Drop(this);
    return ERR_FAILED;
  }
  return OK;
}

int TCPSocketPosix::Bind(const IPEndPoint& address) {
  Stream* s = Find(this);
  if (s == nullptr || s->fd < 0)
    return ERR_SOCKET_NOT_CONNECTED;
  unsigned char addr[4];
  int port = 0;
  if (!ToBridge(address, addr, &port))
    return ERR_ADDRESS_INVALID;
  return br_stream_bind(s->fd, addr, port) < 0 ? ERR_FAILED : OK;
}

int TCPSocketPosix::Connect(const IPEndPoint& address,
                            CompletionOnceCallback callback) {
  Stream* s = Find(this);
  if (s == nullptr || s->fd < 0)
    return ERR_SOCKET_NOT_CONNECTED;

  unsigned char addr[4];
  int port = 0;
  if (!ToBridge(address, addr, &port))
    return ERR_ADDRESS_INVALID;

  s->peer = address;
  const int rv = br_stream_connect(s->fd, addr, port);
  if (rv == 0) {
    s->connected = true;
    return OK;
  }
  if (rv != BR_STREAM_WOULDBLOCK)
    return ERR_CONNECTION_FAILED;

  // Still being made: the network thread polls it and reports through here.
  s->connecting = true;
  s->connect_callback = std::move(callback);
  BSTRACE("bs: connect fd=%d port=%d pending", s->fd, (int)s->peer.port());
  return ERR_IO_PENDING;
}

bool TCPSocketPosix::IsValid() const {
  const Stream* s = Find(this);
  return s != nullptr && s->fd >= 0;
}

bool TCPSocketPosix::IsConnected() const {
  const Stream* s = Find(this);
  return s != nullptr && s->connected;
}

bool TCPSocketPosix::IsConnectedAndIdle() const {
  const Stream* s = Find(this);
  if (s == nullptr || !s->connected)
    return false;
  // Idle means nothing is waiting to be read.
  return br_stream_readable(s->fd) == 0;
}

void TCPSocketPosix::Close() {
  Stream* s = Find(this);
  if (s == nullptr)
    return;
  if (s->fd >= 0)
    br_stream_close(s->fd);
  Drop(this);
}

int TCPSocketPosix::Read(IOBuffer* buf,
                         int buf_len,
                         CompletionOnceCallback callback) {
  Stream* s = Find(this);
  if (s == nullptr || s->fd < 0 || !s->connected)
    return ERR_SOCKET_NOT_CONNECTED;

  const int rv = br_stream_recv(s->fd, buf->data(), buf_len);
  BSTRACE("bs: read fd=%d want=%d -> %d", s->fd, buf_len, rv);
  if (rv != BR_STREAM_WOULDBLOCK)
    return MapBridgeResult(rv);

  s->reading = true;
  s->read_if_ready = false;
  s->read_buf = buf;
  s->read_len = buf_len;
  s->read_callback = std::move(callback);
  return ERR_IO_PENDING;
}

// Take what is there, or say that nothing is yet. When nothing is, the
// callback is told only that the socket became readable -- no bytes are
// taken on the caller's behalf, and the caller asks again.
int TCPSocketPosix::ReadIfReady(IOBuffer* buf,
                                int buf_len,
                                CompletionOnceCallback callback) {
  Stream* s = Find(this);
  if (s == nullptr || s->fd < 0 || !s->connected)
    return ERR_SOCKET_NOT_CONNECTED;

  const int rv = br_stream_recv(s->fd, buf->data(), buf_len);
  BSTRACE("bs: readifready fd=%d want=%d -> %d", s->fd, buf_len, rv);
  if (rv != BR_STREAM_WOULDBLOCK)
    return MapBridgeResult(rv);

  s->reading = true;
  s->read_if_ready = true;
  // No buffer is held: this wait ends by reporting readiness, and the
  // caller supplies a buffer when it asks again.
  s->read_buf.reset();
  s->read_len = 0;
  s->read_callback = std::move(callback);
  return ERR_IO_PENDING;
}

int TCPSocketPosix::CancelReadIfReady() {
  Stream* s = Find(this);
  if (s != nullptr) {
    s->reading = false;
    s->read_if_ready = false;
    s->read_buf.reset();
    s->read_callback.Reset();
  }
  return OK;
}

int TCPSocketPosix::Write(
    IOBuffer* buf,
    int buf_len,
    CompletionOnceCallback callback,
    const NetworkTrafficAnnotationTag& traffic_annotation) {
  Stream* s = Find(this);
  if (s == nullptr || s->fd < 0 || !s->connected)
    return ERR_SOCKET_NOT_CONNECTED;

  const int rv = br_stream_send(s->fd, buf->data(), buf_len);
  BSTRACE("bs: write fd=%d len=%d -> %d", s->fd, buf_len, rv);
  if (rv != BR_STREAM_WOULDBLOCK)
    return MapBridgeResult(rv);

  s->writing = true;
  s->write_buf = buf;
  s->write_len = buf_len;
  s->write_callback = std::move(callback);
  return ERR_IO_PENDING;
}

int TCPSocketPosix::GetLocalAddress(IPEndPoint* address) const {
  const Stream* s = Find(this);
  if (s == nullptr || s->fd < 0)
    return ERR_SOCKET_NOT_CONNECTED;
  unsigned char addr[4] = {0, 0, 0, 0};
  int port = 0;
  if (br_stream_local(s->fd, addr, &port) < 0)
    return ERR_FAILED;
  *address = FromBridge(addr, port);
  return OK;
}

int TCPSocketPosix::GetPeerAddress(IPEndPoint* address) const {
  const Stream* s = Find(this);
  if (s == nullptr || !s->connected)
    return ERR_SOCKET_NOT_CONNECTED;
  *address = s->peer;
  return OK;
}

// ---- options -------------------------------------------------------------

void TCPSocketPosix::SetDefaultOptionsForClient() {
  SetNoDelay(true);
  SetKeepAlive(true, 45);
}

bool TCPSocketPosix::SetNoDelay(bool no_delay) {
  const Stream* s = Find(this);
  return s != nullptr && s->fd >= 0 &&
         br_stream_set_nodelay(s->fd, no_delay ? 1 : 0) == 0;
}

bool TCPSocketPosix::SetKeepAlive(bool enable, int delay) {
  const Stream* s = Find(this);
  return s != nullptr && s->fd >= 0 &&
         br_stream_set_keepalive(s->fd, enable ? 1 : 0, delay) == 0;
}

int TCPSocketPosix::SetReceiveBufferSize(int32_t size) {
  const Stream* s = Find(this);
  if (s == nullptr || s->fd < 0)
    return ERR_SOCKET_NOT_CONNECTED;
  return br_stream_set_recv_buffer(s->fd, size) < 0 ? ERR_FAILED : OK;
}

int TCPSocketPosix::SetSendBufferSize(int32_t size) {
  const Stream* s = Find(this);
  if (s == nullptr || s->fd < 0)
    return ERR_SOCKET_NOT_CONNECTED;
  return br_stream_set_send_buffer(s->fd, size) < 0 ? ERR_FAILED : OK;
}

int TCPSocketPosix::BindToNetwork(handles::NetworkHandle network) {
  return ERR_NOT_IMPLEMENTED;
}

void TCPSocketPosix::ApplySocketTag(const SocketTag& tag) {}

// The connect log records how many addresses were tried before one worked.
// Nothing here reads it back, so the two ends only keep the flag honest.
void TCPSocketPosix::StartLoggingMultipleConnectAttempts(
    const AddressList& addresses) {
  logging_multiple_connect_attempts_ = true;
}

void TCPSocketPosix::EndLoggingMultipleConnectAttempts(int net_error) {
  logging_multiple_connect_attempts_ = false;
}

SocketDescriptor TCPSocketPosix::SocketDescriptorForTesting() const {
  const Stream* s = Find(this);
  return s != nullptr ? s->fd : kInvalidSocket;
}

}  // namespace net


// Give parked requests a turn: the network thread calls this each turn, and
// it is the only place work that was not ready when asked for moves on.
extern "C" void br_net_enter(void);
extern "C" void br_net_leave(void);
extern "C" void br_net_run_due(void);

extern "C" int br_net_thread_up(void);
extern "C" void br_tcp_pump_turn(void);

// Called from the page's thread: once the network thread runs it moves the
// sockets, and this does nothing.
extern "C" void br_tcp_pump(void) {
  if (br_net_thread_up())
    return;
  br_tcp_pump_turn();
}

extern "C" void br_tcp_pump_turn(void) {
  // The stack's timers are due on real time, whoever pumps.
  br_net_run_due();
  br_net_enter();
  struct Leave {
    ~Leave() { br_net_leave(); }
  } leave;
  auto& list = net::Streams();
  for (size_t i = 0; i < list.size();) {
    net::Stream& s = list[i];

    if (s.connecting) {
      const int done = br_stream_connected(s.fd);
      if (done == 0) {
        ++i;
        continue;
      }
      s.connecting = false;
      s.connected = (done == 1);
      BSTRACE("bs: connect fd=%d done=%d", s.fd, done);
      net::CompletionOnceCallback cb = std::move(s.connect_callback);
      if (!cb.is_null())
        std::move(cb).Run(done == 1 ? net::OK : net::ERR_CONNECTION_FAILED);
      i = 0;
      continue;
    }

    if (s.writing) {
      const int ready = br_stream_writable(s.fd);
      if (ready != 0) {
        scoped_refptr<net::IOBuffer> buf = s.write_buf;
        const int len = s.write_len;
        const int fd = s.fd;
        net::CompletionOnceCallback cb = std::move(s.write_callback);
        s.writing = false;
        s.write_buf.reset();

        const int rv = ready < 0
                           ? net::ERR_CONNECTION_FAILED
                           : net::MapBridgeResult(
                                 br_stream_send(fd, buf->data(), len));
        if (rv == net::ERR_IO_PENDING) {
          // Not ready after all: park it again rather than report a failure
          // the caller cannot act on.
          s.writing = true;
          s.write_buf = buf;
          s.write_callback = std::move(cb);
        } else {
          if (!cb.is_null())
            std::move(cb).Run(rv);
          // Running a callback can add or remove sockets, so the walk starts
          // over rather than trusting the position it held.
          i = 0;
          continue;
        }
      }
    }

    if (s.reading) {
      const int ready = br_stream_readable(s.fd);
      if (ready != 0) {
        // A readiness wait ends by saying so: the caller takes the bytes
        // itself on the next call.
        if (s.read_if_ready) {
          net::CompletionOnceCallback cb = std::move(s.read_callback);
          s.reading = false;
          s.read_if_ready = false;
          BSTRACE("bs: readable fd=%d ready=%d", s.fd, ready);
          if (!cb.is_null())
            std::move(cb).Run(ready < 0 ? net::ERR_CONNECTION_FAILED
                                        : net::OK);
          i = 0;
          continue;
        }

        scoped_refptr<net::IOBuffer> buf = s.read_buf;
        const int len = s.read_len;
        const int fd = s.fd;
        net::CompletionOnceCallback cb = std::move(s.read_callback);
        s.reading = false;
        s.read_buf.reset();

        const int rv = ready < 0
                           ? net::ERR_CONNECTION_FAILED
                           : net::MapBridgeResult(
                                 br_stream_recv(fd, buf->data(), len));
        if (rv == net::ERR_IO_PENDING) {
          s.reading = true;
          s.read_buf = buf;
          s.read_callback = std::move(cb);
        } else {
          if (!cb.is_null())
            std::move(cb).Run(rv);
          i = 0;
          continue;
        }
      }
    }

    ++i;
  }
}
