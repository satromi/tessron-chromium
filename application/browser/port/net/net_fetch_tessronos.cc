/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	net_fetch_tessronos.cc
 *	ブラウザ: fetching through the network stack (design 17.19).
 *
 *	Every request of the page (its subresources, its navigations and
 *	forms, a script's XMLHttpRequest and fetch) goes through Chromium's
 *	network stack, which keeps connections open between requests, speaks
 *	HTTP/2, follows redirects and keeps the cookie jar and the HTTP
 *	cache. The stack runs on the network thread (br_stream.cc); a caller
 *	starts a request and is told of it later, on its own thread, through
 *	a callback. Nothing is answered from inside the call that started it:
 *	a request answered there destroys its caller mid-flight.
 *
 *	One context is built and kept: its connection pool, resolved names
 *	and open sockets are what make a second request to the same host
 *	cheap, and requests to many hosts go on at once.
 *
 *	The context holds the program's only cookie jar and its HTTP cache.
 *	Scripts (document.cookie) and the application's own requests read
 *	and write the same jar through br_net_cookie_line/br_net_cookie_set.
 *	The cookies that outlive a session and the cache are written back to
 *	real objects (br_store.cc) and read back when the program starts.
 */

#include <stdio.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/location.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "base/containers/span.h"
#include "net/base/completion_once_callback.h"
#include "net/cookies/canonical_cookie.h"
#include "net/cookies/cookie_access_result.h"
#include "net/cookies/cookie_monster.h"
#include "net/cookies/cookie_options.h"
#include "net/cookies/cookie_partition_key_collection.h"
#include "net/cookies/site_for_cookies.h"
#include "net/disk_cache/disk_cache.h"
#include "net/disk_cache/memory/mem_backend_impl.h"
#include "net/http/http_cache.h"
#include "net/http/http_network_layer.h"
#include "net/http/no_vary_search_cache_storage_file_operations.h"
#include "url/origin.h"
#include "net/base/elements_upload_data_stream.h"
#include "net/base/io_buffer.h"
#include "net/base/net_errors.h"
#include "net/base/network_handle.h"
#include "net/base/request_priority.h"
#include "net/base/upload_bytes_element_reader.h"
#include "net/cert/cert_verifier.h"
#include "net/cert/cert_verify_result.h"
#include "net/cert/x509_certificate.h"
#include "net/base/isolation_info.h"
#include "net/base/network_anonymization_key.h"
#include "net/http/alternative_service.h"
#include "net/http/http_network_session.h"
#include "net/socket/client_socket_pool_manager.h"
#include "net/http/http_server_properties.h"
#include "net/http/http_response_info.h"
#include "url/scheme_host_port.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "base/json/json_writer.h"
#include "net/log/net_log.h"
#include "net/log/net_log_capture_mode.h"
#include "net/log/net_log_entry.h"
#include "net/log/net_log_event_type.h"
#include "net/log/net_log_with_source.h"
#include "net/proxy_resolution/configured_proxy_resolution_service.h"
#include "net/quic/quic_context.h"
#include "base/synchronization/lock.h"
#include "br_store.h"
#include "br_stream.h"
#include "net/third_party/quiche/src/quiche/quic/core/crypto/crypto_protocol.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "net/url_request/url_request.h"
#include "net/url_request/url_request_context.h"
#include "net/url_request/url_request_context_builder.h"
#include "url/gurl.h"

// What the application says it is to the servers (br_blink.cc)
const char* br_user_agent();

#include "br_net.h"

// The verifier that checks a certificate against the roots compiled into
// this program (root_store_tessronos.cc).
namespace net {
std::unique_ptr<CertVerifier> BrCreateCertVerifier();
}  // namespace net

// Runs work on the network thread and waits for it (defined below).
void RunOnNetAndWait(base::OnceClosure work);

namespace {

// ---- saved forms -----------------------------------------------------------
// Both stores are a magic word, a version and a count, then their items; a
// string is a 32-bit little-endian length and its bytes.

class Out {
 public:
  void U32(uint32_t v) {
    for (int i = 0; i < 4; i++)
      b_.push_back(static_cast<char>(v >> (8 * i)));
  }
  void I64(int64_t v) {
    for (int i = 0; i < 8; i++)
      b_.push_back(static_cast<char>(static_cast<uint64_t>(v) >> (8 * i)));
  }
  void Str(const char* p, size_t n) {
    U32(static_cast<uint32_t>(n));
    b_.append(p, n);
  }
  void Str(const std::string& s) { Str(s.data(), s.size()); }
  // Bytes as they are, without a length: the magic word.
  void Raw(const char* p, size_t n) { b_.append(p, n); }
  // Hands the bytes to the store as a malloc'd buffer.
  int Release(unsigned char** data, int* len) {
    *data = static_cast<unsigned char*>(malloc(b_.size() ? b_.size() : 1));
    if (*data == nullptr)
      return -1;
    memcpy(*data, b_.data(), b_.size());
    *len = static_cast<int>(b_.size());
    return 0;
  }

 private:
  std::string b_;
};

class In {
 public:
  In(const unsigned char* p, size_t n) : p_(p), n_(n) {}
  bool ok() const { return ok_; }
  uint32_t U32() {
    if (!Need(4))
      return 0;
    uint32_t v = p_[0] | (p_[1] << 8) | (p_[2] << 16) | (uint32_t(p_[3]) << 24);
    Skip(4);
    return v;
  }
  int64_t I64() {
    if (!Need(8))
      return 0;
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
      v = (v << 8) | p_[i];
    Skip(8);
    return static_cast<int64_t>(v);
  }
  std::string Str() {
    const uint32_t len = U32();
    if (!Need(len))
      return std::string();
    std::string s(reinterpret_cast<const char*>(p_), len);
    Skip(len);
    return s;
  }
  bool Magic(const char* m) {
    if (!Need(4) || memcmp(p_, m, 4) != 0)
      return ok_ = false;
    Skip(4);
    return true;
  }

 private:
  bool Need(size_t k) {
    if (!ok_ || n_ < k)
      ok_ = false;
    return ok_;
  }
  void Skip(size_t k) {
    p_ += k;
    n_ -= k;
  }
  const unsigned char* p_;
  size_t n_;
  bool ok_ = true;
};

int64_t TimeToInt(base::Time t) {
  return t.ToDeltaSinceWindowsEpoch().InMicroseconds();
}

base::Time IntToTime(int64_t v) {
  return base::Time::FromDeltaSinceWindowsEpoch(base::Microseconds(v));
}

// ---- cookies that outlive the program --------------------------------------

constexpr char kCookieStoreName[] = "cookie";

// The jar tells this store about every persistent cookie it adds, changes or
// drops; the store keeps a copy of each and writes them all back a moment
// after a change. Partitioned cookies are kept for the session only.
class BrCookieStore final : public net::CookieMonster::PersistentCookieStore {
 public:
  BrCookieStore() {
    store_id_ = br_store_register(kCookieStoreName, &BrCookieStore::Write,
                                  this, 2000);
  }

  void Load(LoadedCallback loaded_callback,
            const net::NetLogWithSource&) override {
    std::vector<std::unique_ptr<net::CanonicalCookie>> cookies = ReadSaved();
    for (const auto& c : cookies)
      saved_[Id(*c)] = std::make_unique<net::CanonicalCookie>(*c);
    printf("browser: cookies: %u read back\n", static_cast<unsigned>(cookies.size()));
    // Answered in place: the saved cookies are already in memory, and the
    // jar is then complete by the time Context() returns. Answered on a later
    // turn, as a store reading a disk would, the first page's inline script
    // reads document.cookie before that turn comes and gets nothing.
    std::move(loaded_callback).Run(std::move(cookies));
  }
  // The jar is loaded whole before its first use (Context() asks for every
  // cookie as soon as it is built), so there is nothing to load per domain.
  void LoadCookiesForKey(const std::string&,
                         LoadedCallback loaded_callback) override {
    std::move(loaded_callback)
        .Run(std::vector<std::unique_ptr<net::CanonicalCookie>>());
  }
  void AddCookie(const net::CanonicalCookie& cc) override {
    if (cc.IsPartitioned())
      return;
    saved_[Id(cc)] = std::make_unique<net::CanonicalCookie>(cc);
    br_store_dirty(store_id_);
  }
  // Access times are kept, but a read alone does not cause a write.
  void UpdateCookieAccessTime(const net::CanonicalCookie& cc) override {
    auto it = saved_.find(Id(cc));
    if (it != saved_.end())
      it->second = std::make_unique<net::CanonicalCookie>(cc);
  }
  void DeleteCookie(const net::CanonicalCookie& cc) override {
    if (saved_.erase(Id(cc)) != 0)
      br_store_dirty(store_id_);
  }
  void SetForceKeepSessionState() override {}
  void SetBeforeCommitCallback(base::RepeatingClosure) override {}
  void Flush(base::OnceClosure callback) override {
    if (callback) {
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, std::move(callback));
    }
  }

 private:
  ~BrCookieStore() override = default;

  static std::string Id(const net::CanonicalCookie& cc) {
    return cc.Name() + '\n' + cc.Domain() + '\n' + cc.Path();
  }

  // Called by the store's timer on the page's thread; the jar's copy belongs
  // to the network thread, which writes it out.
  static int Write(void* ctx, unsigned char** data, int* len) {
    int rv = -1;
    RunOnNetAndWait(base::BindOnce(
        [](void* ctx, unsigned char** data, int* len, int* rv) {
          *rv = WriteOnNet(ctx, data, len);
        },
        ctx, data, len, &rv));
    return rv;
  }

  static int WriteOnNet(void* ctx, unsigned char** data, int* len) {
    auto* self = static_cast<BrCookieStore*>(ctx);
    const base::Time now = base::Time::Now();
    Out out;
    out.Raw("BRCK", 4);
    out.U32(1);
    uint32_t count = 0;
    for (const auto& c : self->saved_) {
      if (c.second->IsPersistent() && !c.second->IsExpired(now))
        count++;
    }
    out.U32(count);
    for (const auto& e : self->saved_) {
      const net::CanonicalCookie& c = *e.second;
      if (!c.IsPersistent() || c.IsExpired(now))
        continue;
      out.Str(c.Name());
      out.Str(c.Value());
      out.Str(c.Domain());
      out.Str(c.Path());
      out.I64(TimeToInt(c.CreationDate()));
      out.I64(TimeToInt(c.ExpiryDate()));
      out.I64(TimeToInt(c.LastAccessDate()));
      out.I64(TimeToInt(c.LastUpdateDate()));
      out.U32((c.SecureAttribute() ? 1u : 0u) | (c.IsHttpOnly() ? 2u : 0u));
      out.U32(static_cast<uint32_t>(static_cast<int>(c.SameSite())));
      out.U32(static_cast<uint32_t>(c.Priority()));
      out.U32(static_cast<uint32_t>(c.SourceScheme()));
      out.U32(static_cast<uint32_t>(c.SourcePort()));
      out.U32(static_cast<uint32_t>(c.SourceType()));
    }
    return out.Release(data, len);
  }

  static std::vector<std::unique_ptr<net::CanonicalCookie>> ReadSaved() {
    std::vector<std::unique_ptr<net::CanonicalCookie>> cookies;
    unsigned char* data = nullptr;
    int len = 0;
    if (br_store_load(kCookieStoreName, &data, &len) != 0)
      return cookies;
    In in(data, static_cast<size_t>(len));
    const base::Time now = base::Time::Now();
    if (in.Magic("BRCK") && in.U32() == 1) {
      const uint32_t count = in.U32();
      for (uint32_t i = 0; i < count && in.ok(); i++) {
        std::string name = in.Str();
        std::string value = in.Str();
        std::string domain = in.Str();
        std::string path = in.Str();
        const base::Time creation = IntToTime(in.I64());
        const base::Time expiry = IntToTime(in.I64());
        const base::Time access = IntToTime(in.I64());
        const base::Time update = IntToTime(in.I64());
        const uint32_t flags = in.U32();
        const auto same_site = static_cast<net::CookieSameSite>(in.U32());
        const auto priority = static_cast<net::CookiePriority>(in.U32());
        const auto scheme = static_cast<net::CookieSourceScheme>(in.U32());
        const int port = static_cast<int>(in.U32());
        const auto type = static_cast<net::CookieSourceType>(in.U32());
        if (!in.ok() || expiry <= now)
          continue;
        std::unique_ptr<net::CanonicalCookie> c =
            net::CanonicalCookie::FromStorage(
                std::move(name), std::move(value), std::move(domain),
                std::move(path), creation, expiry, access, update,
                (flags & 1u) != 0, (flags & 2u) != 0, same_site, priority,
                std::nullopt, scheme, port, type,
                net::CanonicalCookieFromStorageCallSite::
                    kSqlitePersistentCookieStore);
        if (c)
          cookies.push_back(std::move(c));
      }
    }
    free(data);
    return cookies;
  }

  int store_id_ = -1;
  std::map<std::string, std::unique_ptr<net::CanonicalCookie>> saved_;
};

// ---- the HTTP cache ----------------------------------------------------------

constexpr char kCacheStoreName[] = "cache";
constexpr int64_t kCacheBytes = 16 * 1024 * 1024;

// The cache lives in memory, as the stack's in-memory cache does; its entries
// are written back to a real object a minute after the network last brought
// something new, and put back into a fresh cache when the program starts.
disk_cache::MemBackendImpl* g_cache = nullptr;
int g_cache_store = -1;

int ReadEntryStream(disk_cache::Entry* e, int stream, std::string* out) {
  const int64_t size = e->GetDataSize(stream);
  out->clear();
  if (size <= 0)
    return 0;
  auto buf = base::MakeRefCounted<net::IOBufferWithSize>(
      static_cast<size_t>(size));
  const int got = e->ReadData(stream, 0, buf.get(), static_cast<int>(size),
                              net::CompletionOnceCallback());
  if (got < 0)
    return got;
  out->assign(buf->data(), static_cast<size_t>(got));
  return got;
}

int WriteCacheOnNet(unsigned char** data, int* len);

// Called by the store's timer on the page's thread; the cache belongs to the
// network thread, which does the walk.
int WriteCache(void*, unsigned char** data, int* len) {
  int rv = -1;
  RunOnNetAndWait(base::BindOnce(
      [](unsigned char** data, int* len, int* rv) {
        *rv = WriteCacheOnNet(data, len);
      },
      data, len, &rv));
  return rv;
}

int WriteCacheOnNet(unsigned char** data, int* len) {
  if (g_cache == nullptr)
    return -1;
  Out out;
  out.Raw("BRHC", 4);
  out.U32(1);
  // The count is not known before the walk; entries follow a marker each and
  // a zero ends the list.
  std::unique_ptr<disk_cache::Backend::Iterator> it = g_cache->CreateIterator();
  unsigned entries = 0;
  for (;;) {
    disk_cache::EntryResult r = it->OpenNextEntry(base::DoNothing());
    if (r.net_error() != net::OK)
      break;
    disk_cache::Entry* e = r.ReleaseEntry();
    std::string streams[3];
    bool ok = true;
    for (int s = 0; s < 3 && ok; s++)
      ok = ReadEntryStream(e, s, &streams[s]) >= 0;
    if (ok && !streams[0].empty()) {
      out.U32(1);
      out.Str(e->GetKey());
      for (const std::string& s : streams)
        out.Str(s);
      entries++;
    }
    e->Close();
  }
  out.U32(0);
  printf("browser: http cache: %u entries to save\n", entries);
  return out.Release(data, len);
}

void ReadCache(disk_cache::MemBackendImpl* cache) {
  unsigned char* data = nullptr;
  int len = 0;
  if (br_store_load(kCacheStoreName, &data, &len) != 0)
    return;
  In in(data, static_cast<size_t>(len));
  unsigned entries = 0;
  if (in.Magic("BRHC") && in.U32() == 1) {
    while (in.ok() && in.U32() == 1) {
      const std::string key = in.Str();
      std::string streams[3];
      for (std::string& s : streams)
        s = in.Str();
      if (!in.ok())
        break;
      disk_cache::EntryResult r =
          cache->CreateEntry(key, net::HIGHEST, base::DoNothing());
      if (r.net_error() != net::OK)
        continue;
      disk_cache::Entry* e = r.ReleaseEntry();
      for (int s = 0; s < 3; s++) {
        if (streams[s].empty())
          continue;
        auto buf = base::MakeRefCounted<net::StringIOBuffer>(streams[s]);
        e->WriteData(s, 0, buf.get(), static_cast<int>(streams[s].size()),
                     net::CompletionOnceCallback(), true);
      }
      e->Close();
      entries++;
    }
  }
  free(data);
  printf("browser: http cache: %u entries read back\n", entries);
}

class BrCacheBackendFactory final : public net::HttpCache::BackendFactory {
 public:
  disk_cache::BackendResult CreateBackend(
      net::NetLog* net_log,
      base::OnceCallback<void(disk_cache::BackendResult)>) override {
    std::unique_ptr<disk_cache::MemBackendImpl> backend =
        disk_cache::MemBackendImpl::CreateBackend(kCacheBytes, net_log);
    if (!backend)
      return disk_cache::BackendResult::MakeError(net::ERR_FAILED);
    g_cache = backend.get();
    ReadCache(backend.get());
    return disk_cache::BackendResult::Make(std::move(backend));
  }
};

// Whether HTTP/3 may be used at all. It stays off: the port's sockets are
// streams only, and a virtual machine's NAT drops datagrams without a word.
// TCP and TLS carry everything.
extern "C" int br_net_quic = 0;
// HTTP/2 is on.
extern "C" int br_net_h2 = 1;

// Built on first use, then kept: the connections, resolved names and open
// sockets it holds are what make a second request to the same host cheap.
net::URLRequestContext* Context() {
  static net::URLRequestContext* ctx = [] {
    // The system has 64 sockets in all (lwIP's pool): the pool keeps within
    // half of them, so that a page asking for many hosts at once waits for a
    // socket instead of failing to get one, and the name lookups and other
    // programs still have some.
    net::ClientSocketPoolManager::set_socket_soft_cap_per_pool_for_test(
        net::HttpNetworkSession::SocketPoolType::kNormal, 32);
    net::URLRequestContextBuilder builder;
    // The jar, backed by the saved cookies.
    builder.SetCookieStore(std::make_unique<net::CookieMonster>(
        base::MakeRefCounted<BrCookieStore>(), nullptr));
    // The cache, in memory and saved (see BrCacheBackendFactory). It is put
    // in place of the builder's own so that it can be read back into.
    builder.DisableHttpCache();
    builder.SetWrapHttpNetworkLayerCallback(base::BindOnce(
        [](std::unique_ptr<net::HttpNetworkLayer> network_layer)
            -> std::unique_ptr<net::HttpTransactionFactory> {
          return std::make_unique<net::HttpCache>(
              std::move(network_layer),
              std::make_unique<BrCacheBackendFactory>(), nullptr);
        }));
    g_cache_store = br_store_register(kCacheStoreName, &WriteCache, nullptr,
                                      60000);
    // This machine reaches the network directly, and looking for a proxy
    // configuration would ask parts of the system that are not here.
    builder.set_proxy_resolution_service(
        net::ConfiguredProxyResolutionService::CreateDirect());
    std::unique_ptr<net::CertVerifier> verifier = net::BrCreateCertVerifier();
    if (verifier != nullptr)
      builder.SetCertVerifier(std::move(verifier));
    // Requests that come without these headers of their own (the application's
    // navigation, the cache revalidations) still name the browser and the
    // reader's languages.
    builder.set_user_agent(br_user_agent());
    builder.set_accept_language("ja,en-US;q=0.9,en;q=0.8");
    // HTTP/3 is used where a site advertises it (Alt-Svc); the first request
    // to a host always goes over TCP. br_net_quic set to 0 keeps everything
    // on TCP.
    net::HttpNetworkSessionParams session_params;
    session_params.enable_quic = br_net_quic != 0;
    session_params.enable_http2 = br_net_h2 != 0;
    builder.set_http_network_session_params(session_params);
    // The session keeps a QUIC context whether or not QUIC is used; should it
    // be switched on, a connection neither moves to a second socket nor is
    // given up as a black hole (NBHD), and a handshake is given 15 seconds.
    auto quic_context = std::make_unique<net::QuicContext>();
    quic_context->params()->allow_port_migration = false;
    quic_context->params()->client_connection_options.push_back(quic::kNBHD);
    quic_context->params()->connection_options.push_back(quic::kNBHD);
    quic_context->params()->max_time_before_crypto_handshake =
        base::Seconds(15);
    quic_context->params()->max_idle_time_before_crypto_handshake =
        base::Seconds(15);
    builder.set_quic_context(std::move(quic_context));
    net::URLRequestContext* built = builder.Build().release();
    printf("browser: net: context built, QUIC %s, %u versions\n",
            built->http_transaction_factory()->GetSession()->IsQuicEnabled()
                ? "on"
                : "off",
            static_cast<unsigned>(
                built->quic_context()->params()->supported_versions.size()));
    // Loading the jar is asked for now, as one job for every cookie, so
    // that it is complete before a page reads document.cookie.
    built->cookie_store()->GetAllCookiesAsync(base::DoNothing());
    return built;
  }();
  return ctx;
}

constexpr int kReadChunk = 32 * 1024;

// How many requests have been started and not yet reported. The load path
// reads this to know whether waiting a moment longer would bring in more of
// the page.
volatile unsigned g_outstanding = 0;

// The most requests that were under way at once since it was last asked.
volatile unsigned g_peak = 0;

void OutstandingUp() {
  const unsigned n = __sync_add_and_fetch(&g_outstanding, 1);
  for (;;) {
    const unsigned p = g_peak;
    if (n <= p || __sync_bool_compare_and_swap(&g_peak, p, n))
      break;
  }
}

void OutstandingDown() {
  for (;;) {
    const unsigned n = g_outstanding;
    if (n == 0 || __sync_bool_compare_and_swap(&g_outstanding, n, n - 1))
      return;
  }
}

// The network stack's task runner, on real time. Everything the stack posts
// goes here, and what is due runs on the network thread each time it moves
// the sockets.
class NetRunner final : public base::SingleThreadTaskRunner {
 public:
  bool PostDelayedTask(const base::Location&,
                       base::OnceClosure task,
                       base::TimeDelta delay) override {
    if (delay.is_negative())
      delay = base::TimeDelta();
    const int64_t when =
        (base::TimeTicks::Now() + delay).since_origin().InMicroseconds();
    {
      // Posted from the page's thread as well as this one's.
      base::AutoLock hold(lock_);
      queue_.emplace(std::make_pair(when, seq_++), std::move(task));
    }
    if (!br_on_net_thread())
      br_net_wake();
    return true;
  }
  bool PostNonNestableDelayedTask(const base::Location& from,
                                  base::OnceClosure task,
                                  base::TimeDelta delay) override {
    return PostDelayedTask(from, std::move(task), delay);
  }
  // The network thread runs everything here (or the page's, where the task
  // could not be made).
  bool RunsTasksInCurrentSequence() const override {
    return br_on_net_thread() || !br_net_thread_up();
  }

  // Runs what is due, in order, re-reading the clock for each one so that a
  // task posted with no delay while this is running still runs in the same
  // turn. That is what a real message loop does, and the stack counts on it:
  // it reads a batch, then hands the rest of the work back through a posted
  // task, which would otherwise wait a hundredth of a second for the next
  // turn. Bounded by a count and by time, so that a task which keeps
  // re-posting itself cannot hold the thread.
  void RunDue() {
    const base::TimeTicks start = base::TimeTicks::Now();
    for (int ran = 0; ran < 4096; ++ran) {
      const base::TimeTicks ticks = base::TimeTicks::Now();
      if (ticks - start > base::Milliseconds(50))
        break;
      base::OnceClosure task;
      {
        base::AutoLock hold(lock_);
        if (queue_.empty())
          break;
        auto it = queue_.begin();
        if (it->first.first > ticks.since_origin().InMicroseconds())
          break;
        task = std::move(it->second);
        queue_.erase(it);
      }
      std::move(task).Run();
    }
  }

 private:
  ~NetRunner() override = default;

  base::Lock lock_;
  std::map<std::pair<int64_t, uint64_t>, base::OnceClosure> queue_;
  uint64_t seq_ = 0;
};

NetRunner* TheNetRunner();

// The page's thread runner, taken when the network thread starts. An answer for
// the page goes back through it; one for a worker (whose queue only the
// page's thread may touch) is handed to the worker's runner from there.
scoped_refptr<base::SingleThreadTaskRunner>& MainRunner() {
  static auto* runner = new scoped_refptr<base::SingleThreadTaskRunner>();
  return *runner;
}

void PostToOrigin(scoped_refptr<base::SingleThreadTaskRunner> origin,
                  base::OnceClosure closure) {
  // The network runner as the origin means: answer here, on the network
  // task (a synchronous fetch, whose asker waits on a semaphore).
  if (origin && origin.get() == static_cast<base::SingleThreadTaskRunner*>(
                                    TheNetRunner())) {
    std::move(closure).Run();
    return;
  }
  scoped_refptr<base::SingleThreadTaskRunner>& main = MainRunner();
  if (!br_net_thread_up() || !main) {
    if (origin)
      origin->PostTask(FROM_HERE, std::move(closure));
    else
      std::move(closure).Run();
    return;
  }
  if (!origin || origin == main) {
    main->PostTask(FROM_HERE, std::move(closure));
    return;
  }
  main->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](scoped_refptr<base::SingleThreadTaskRunner> origin,
             base::OnceClosure closure) {
            origin->PostTask(FROM_HERE, std::move(closure));
          },
          std::move(origin), std::move(closure)));
}

void ReportSlowFetches(base::TimeTicks now);

NetRunner* TheNetRunner() {
  static NetRunner* runner = [] {
    scoped_refptr<NetRunner> r = base::MakeRefCounted<NetRunner>();
    r->AddRef();  // for the life of the program
    return r.get();
  }();
  return runner;
}

}  // namespace

// Entered around every call into the stack from outside it, so that what it
// posts (and every object it builds that keeps "the current runner") goes to
// the network runner.
class BrNetScope {
 public:
  BrNetScope() : override_(TheNetRunner()) {}

 private:
  base::SingleThreadTaskRunner::CurrentHandleOverrideForTesting override_;
};

extern "C" void br_tcp_pump_turn(void);

// One turn of the network thread: the sockets, and what is due.
extern "C" void br_net_thread_turn(void) {
  // The network runner is this thread's current one for good (the state is
  // kept per thread).
  static auto* current =
      new base::SingleThreadTaskRunner::CurrentHandleOverrideForTesting(
          TheNetRunner());
  (void)current;
  br_tcp_pump_turn();
}

// Starts the network thread. Called once on the page's thread after the engine
// is up: the stack is built here first, under the network runner, so that
// the task finds it ready and no first use races between the two tasks.
extern "C" void br_net_prepare(void) {
  MainRunner() = base::SingleThreadTaskRunner::HasCurrentDefault()
                     ? base::SingleThreadTaskRunner::GetCurrentDefault()
                     : nullptr;
  {
    BrNetScope scope;
    TheNetRunner();
    Context();
  }
  if (!MainRunner()) {
    printf("browser: net task: no page runner to answer through; not started\n");
    return;
  }
  br_net_thread_start();
}

extern "C" void br_net_run_due(void) {
  BrNetScope scope;
  TheNetRunner()->RunDue();
  // Every ten seconds, the requests that have been waiting for ten or more.
  static base::TimeTicks last_report;
  const base::TimeTicks now = base::TimeTicks::Now();
  if (now - last_report >= base::Seconds(10)) {
    last_report = now;
    ReportSlowFetches(now);
  }
}

// The same scope for the socket pumps in the other files, which enter and
// leave it around their completions. Scopes nest (a completion can start a
// fetch), so they are kept as a stack.
namespace {
std::vector<std::unique_ptr<BrNetScope>>& NetScopes() {
  static auto* scopes = new std::vector<std::unique_ptr<BrNetScope>>();
  return *scopes;
}
}  // namespace

extern "C" void br_net_enter(void) {
  NetScopes().push_back(std::make_unique<BrNetScope>());
}

extern "C" void br_net_leave(void) {
  if (!NetScopes().empty())
    NetScopes().pop_back();
}

namespace {

// One request. It owns itself: it stays alive until it has told the caller
// how it ended, and then removes itself.
class Fetch;

// Requests under way, for the report of those that take long.
std::set<Fetch*>& LiveFetches() {
  static auto* live = new std::set<Fetch*>();
  return *live;
}

// Bodyless requests under way, by method and address, so that a second ask
// for what is already coming waits for it instead of fetching it again. The
// cache cannot stand in for this: an answer larger than an eighth of the
// cache is never stored, and a page's big scripts are often asked for twice
// while it loads, once by the parser's look-ahead and once by the script
// step.
std::map<std::string, Fetch*>& SharableFetches() {
  static auto* shared = new std::map<std::string, Fetch*>();
  return *shared;
}

class Fetch : public net::URLRequest::Delegate {
 public:
  // |origin| is the runner of whoever asked; the answer is handed back
  // there rather than from inside the stack's own turn.
  Fetch(BrNetFetchDone done,
        void* ctx,
        scoped_refptr<base::SingleThreadTaskRunner> origin)
      : done_(done), ctx_(ctx), origin_(std::move(origin)) {
    LiveFetches().insert(this);
  }
  ~Fetch() override {
    LiveFetches().erase(this);
    if (!share_key_.empty()) {
      auto it = SharableFetches().find(share_key_);
      if (it != SharableFetches().end() && it->second == this)
        SharableFetches().erase(it);
    }
  }

  // Another caller wants what this request is already fetching: it is told
  // the same answer, on its own runner, when this one ends.
  void AddWaiter(BrNetFetchDone done,
                 void* ctx,
                 scoped_refptr<base::SingleThreadTaskRunner> origin) {
    waiters_.push_back({done, ctx, std::move(origin)});
  }

  // The key this request can be shared under, empty when it cannot be:
  // only a plain GET with no body and no headers of its own, since a range
  // or a conditional asks for something else.
  void SetShareKey(const std::string& key) {
    share_key_ = key;
    if (!key.empty())
      SharableFetches()[key] = this;
  }

  // One line about a request that has been under way a while: how long,
  // what the stack is doing with it (net/base/load_states_list.h numbers
  // the states), and how much body has come in.
  void ReportIfSlow(base::TimeTicks now) {
    if (finished_ || now - started_ < base::Seconds(10))
      return;
    const int state = request_ ? request_->GetLoadState().state : -1;
    printf("browser: net fetch: %ds on %.160s (state %d, %u bytes in)\n",
            (int)(now - started_).InSeconds(), url_.c_str(), state,
            (unsigned)data_.size());
  }

  bool Start(const std::string& url,
             const std::string& method,
             const std::string& body,
             const std::string& req_headers) {
    url_ = url;
    GURL gurl(url);
    if (!gurl.is_valid())
      return false;
    net::URLRequestContext* context = Context();
    if (context == nullptr)
      return false;

    request_ = context->CreateRequest(gurl, net::DEFAULT_PRIORITY, this,
                                      TRAFFIC_ANNOTATION_FOR_TESTS,
                                      net::handles::kInvalidNetworkHandle);
    if (request_ == nullptr)
      return false;
    if (!method.empty())
      request_->set_method(method);
    // Every request counts as the site's own. The page that asked is not
    // passed down here; without a site for cookies the jar would treat the
    // request as cross-site and hold back every cookie not marked
    // SameSite=None -- which is most of them.
    request_->set_site_for_cookies(net::SiteForCookies::FromUrl(gurl));
    request_->set_initiator(url::Origin::Create(gurl));

    net::HttpRequestHeaders headers;
    ParseHeaderLines(req_headers, &headers);
    request_->SetExtraRequestHeaders(headers);

    // A body is carried by the request only when there is one; the stack
    // counts its length itself.
    if (!body.empty()) {
      body_ = body;
      request_->set_upload(net::ElementsUploadDataStream::CreateWithReader(
          std::make_unique<net::UploadBytesElementReader>(
              base::as_byte_span(body_))));
    }

    started_ = base::TimeTicks::Now();
    request_->Start();
    return true;
  }

  // net::URLRequest::Delegate:
  void OnResponseStarted(net::URLRequest* request, int net_error) override {
    if (net_error != net::OK) {
      error_ = net_error;
      Finish(false, 0);
      return;
    }
    status_ = request->GetResponseCode();
    final_url_ = request->url().spec();
    protocol_ = request->response_info().alpn_negotiated_protocol;
    const net::HttpResponseHeaders* h = request->response_headers();
    if (h != nullptr) {
      size_t at = 0;
      std::string name, value;
      while (h->EnumerateHeaderLines(&at, &name, &value)) {
        headers_ += name;
        headers_ += ": ";
        headers_ += value;
        headers_ += "\r\n";
      }
      ctype_ = h->GetNormalizedHeader("content-type").value_or(std::string());
    }
    buf_ = base::MakeRefCounted<net::IOBufferWithSize>(kReadChunk);
    ReadMore();
  }

  void OnReadCompleted(net::URLRequest* request, int bytes_read) override {
    if (!Consume(bytes_read))
      return;
    ReadMore();
  }

 private:
  static void ParseHeaderLines(const std::string& lines,
                               net::HttpRequestHeaders* out) {
    size_t at = 0;
    while (at < lines.size()) {
      size_t eol = lines.find("\r\n", at);
      if (eol == std::string::npos)
        eol = lines.size();
      const std::string line = lines.substr(at, eol - at);
      at = eol + 2;
      const size_t colon = line.find(':');
      if (colon == std::string::npos || colon == 0)
        continue;
      size_t vs = colon + 1;
      while (vs < line.size() && (line[vs] == ' ' || line[vs] == '\t'))
        vs++;
      out->SetHeader(line.substr(0, colon), line.substr(vs));
    }
  }

  void ReadMore() {
    for (;;) {
      const int rv = request_->Read(buf_.get(), kReadChunk);
      if (rv == net::ERR_IO_PENDING)
        return;  // answered later, through OnReadCompleted
      if (!Consume(rv))
        return;
    }
  }

  // False once the body has ended, one way or the other.
  bool Consume(int bytes_read) {
    if (bytes_read < 0) {
      error_ = bytes_read;
      Finish(false, status_);
      return false;
    }
    if (bytes_read == 0) {
      Finish(true, status_);
      return false;
    }
    data_.insert(data_.end(), buf_->data(), buf_->data() + bytes_read);
    return true;
  }

  void Finish(bool ok, int status) {
    if (finished_)
      return;
    finished_ = true;
    if (!ok)
      printf("browser: net fetch: %s failed (%d)\n", url_.c_str(), error_);
    // Something new came from the network: the cache is saved a while later.
    if (ok && request_ && !request_->was_cached())
      br_store_dirty(g_cache_store);
    printf("browser: net fetch: %s over '%s'\n", url_.c_str(),
            protocol_.empty() ? "(none)" : protocol_.c_str());
    // The answer goes back on the asker's runner: the page's code then runs
    // as the page's, not inside the stack's turn with the network runner
    // current. It counts as outstanding until it has been handed over, so
    // a caller waiting for "nothing outstanding" also has the answers.
    if (final_url_.empty())
      final_url_ = request_ ? request_->url().spec() : url_;
    auto deliver = [](BrNetFetchDone done, void* ctx, int ok, int status,
                      std::string url, std::string headers, std::string ctype,
                      std::vector<char> data) {
      OutstandingDown();
      if (done != nullptr) {
        done(ctx, ok, status, url.c_str(), headers.c_str(), ctype.c_str(),
             data.empty() ? "" : data.data(), (unsigned)data.size());
      }
    };
    // No one else can join this request from here on.
    if (!share_key_.empty()) {
      auto it = SharableFetches().find(share_key_);
      if (it != SharableFetches().end() && it->second == this)
        SharableFetches().erase(it);
      share_key_.clear();
    }
    for (Waiter& w : waiters_) {
      PostToOrigin(w.origin,
                   base::BindOnce(deliver, w.done, w.ctx, ok ? 1 : 0, status,
                                  final_url_, headers_, ctype_, data_));
    }
    waiters_.clear();
    PostToOrigin(origin_,
                 base::BindOnce(deliver, done_, ctx_, ok ? 1 : 0, status,
                                final_url_, headers_, ctype_, std::move(data_)));
    // Not here: the stack is still inside this request's own machinery.
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE, this);
  }

  struct Waiter {
    BrNetFetchDone done;
    void* ctx;
    scoped_refptr<base::SingleThreadTaskRunner> origin;
  };

  BrNetFetchDone done_;
  void* ctx_;
  scoped_refptr<base::SingleThreadTaskRunner> origin_;
  std::vector<Waiter> waiters_;
  std::string share_key_;
  std::unique_ptr<net::URLRequest> request_;
  scoped_refptr<net::IOBufferWithSize> buf_;
  std::string url_;
  std::string final_url_;
  std::string protocol_;
  std::string body_;
  std::string headers_;
  std::string ctype_;
  std::vector<char> data_;
  int status_ = 0;
  int error_ = 0;
  bool finished_ = false;
  base::TimeTicks started_;
};

void ReportSlowFetches(base::TimeTicks now) {
  for (Fetch* f : LiveFetches())
    f->ReportIfSlow(now);
}

}  // namespace

// Starts one request. Returns 1 if it was taken, in which case |done| will be
// called exactly once, later; 0 if it could not be started at all, in which
// case |done| is never called and the caller should fall back.
namespace {

// The start of a request, on the network thread. 0 when it could not be
// started; the caller then answers the asker itself.
int StartOnNet(std::string url,
               std::string method,
               std::string body_str,
               std::string headers_str,
               BrNetFetchDone done,
               void* ctx,
               scoped_refptr<base::SingleThreadTaskRunner> origin) {
  const char* req_headers = headers_str.c_str();
  const unsigned body_len = (unsigned)body_str.size();
  BrNetScope scope;
  // A plain GET can join one already under way for the same address. The
  // headers the two asks carry may differ -- the parser's look-ahead sends
  // an Accept and a Referer where the script step sends none -- and the
  // file is the same; a header that asks for something else (a range, a
  // conditional, another identity) keeps the request to itself.
  auto asks_for_something_else = [](const char* h) {
    if (h == nullptr || h[0] == 0)
      return false;
    std::string low(h);
    for (char& c : low)
      c = (char)tolower((unsigned char)c);
    return low.find("range:") != std::string::npos ||
           low.find("if-none-match:") != std::string::npos ||
           low.find("if-modified-since:") != std::string::npos ||
           low.find("authorization:") != std::string::npos;
  };
  const bool sharable = body_len == 0 &&
                        method == "GET" &&
                        !asks_for_something_else(req_headers);
  const std::string share_key = sharable ? std::string("GET ") + url : "";
  if (sharable) {
    auto it = SharableFetches().find(share_key);
    if (it != SharableFetches().end()) {
      it->second->AddWaiter(done, ctx, std::move(origin));
      return 1;
    }
  }
  Fetch* f = new Fetch(done, ctx, std::move(origin));
  const bool started = f->Start(
      url, method,
      body_str,
      headers_str);
  if (!started) {
    delete f;
    return 0;
  }
  f->SetShareKey(share_key);
  return 1;
}

}  // namespace

// Set while the page's own thread starts a fetch: its answer then goes to
// the thread's main runner. The runner current at that moment is the queue
// of whatever task was running -- a timer, a media element, a frame's
// loading queue -- and a queue that is paused or shut down by the time the
// answer arrives (a removed frame, a document that went away) drops it
// without a word, leaving the asker waiting for good.
static int br_net_fetch_answer_on_main = 0;

extern "C" void br_net_fetch_set_answer_on_main(int on) {
  br_net_fetch_answer_on_main = on;
}

extern "C" int br_net_fetch_start(const char* url,
                                  const char* method,
                                  const void* body,
                                  unsigned body_len,
                                  const char* req_headers,
                                  BrNetFetchDone done,
                                  void* ctx) {
  if (url == nullptr || done == nullptr)
    return 0;
  scoped_refptr<base::SingleThreadTaskRunner> origin =
      base::SingleThreadTaskRunner::HasCurrentDefault()
          ? base::SingleThreadTaskRunner::GetCurrentDefault()
          : nullptr;
  if (br_net_fetch_answer_on_main && MainRunner())
    origin = MainRunner();
  std::string url_str(url);
  std::string method_str(method != nullptr ? method : "GET");
  std::string body_str(static_cast<const char*>(body ? body : ""), body_len);
  std::string headers_str(req_headers != nullptr ? req_headers : "");
  OutstandingUp();
  if (!br_net_thread_up() || br_on_net_thread()) {
    if (!StartOnNet(url_str, method_str, body_str, headers_str, done, ctx,
                    std::move(origin))) {
      OutstandingDown();
      return 0;
    }
    return 1;
  }
  // Handed to the network thread; a request it cannot start is answered as a
  // failure, which every asker handles as it handles one from the network.
  TheNetRunner()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](std::string url, std::string method, std::string body,
             std::string headers, BrNetFetchDone done, void* ctx,
             scoped_refptr<base::SingleThreadTaskRunner> origin) {
            if (StartOnNet(url, method, body, headers, done, ctx, origin))
              return;
            printf("browser: net fetch: %s could not be started\n", url.c_str());
            PostToOrigin(origin,
                         base::BindOnce(
                             [](BrNetFetchDone done, void* ctx) {
                               OutstandingDown();
                               done(ctx, 0, 0, "", "", "", "", 0);
                             },
                             done, ctx));
          },
          std::move(url_str), std::move(method_str), std::move(body_str),
          std::move(headers_str), done, ctx, std::move(origin)));
  return 1;
}

// A fetch the asker waits for (a navigation of the application, a synchronous
// request). The request runs on the network thread, which answers into a
// semaphore; the asker only waits. Returns 1 with the answer, 0 when the
// request failed, -1 when there is no network thread to ask (the caller then
// fetches another way).
namespace {
struct SyncFetch {
  int sem = -1;
  volatile int abandoned = 0;
  int ok = 0;
  int status = 0;
  std::string url;
  std::string headers;
  std::string ctype;
  std::vector<char> data;
};

void SyncFetchDone(void* ctx,
                   int ok,
                   int status,
                   const char* url,
                   const char* headers,
                   const char* ctype,
                   const char* data,
                   unsigned len) {
  auto* f = static_cast<SyncFetch*>(ctx);
  if (f->abandoned) {
    // The asker gave up waiting and left this to be freed here.
    br_sem_free(f->sem);
    delete f;
    return;
  }
  f->ok = ok;
  f->status = status;
  f->url = url != nullptr ? url : "";
  f->headers = headers != nullptr ? headers : "";
  f->ctype = ctype != nullptr ? ctype : "";
  f->data.assign(data, data + len);
  br_sem_post(f->sem);
}
}  // namespace

extern "C" int br_net_fetch_sync(const char* url,
                                 const char* method,
                                 const void* body,
                                 unsigned body_len,
                                 const char* req_headers,
                                 char** buf,
                                 unsigned* len,
                                 char* ctype,
                                 unsigned ctype_cap,
                                 int* status,
                                 char** headers,
                                 char* final_url,
                                 unsigned url_cap,
                                 int timeout_ms) {
  if (!br_net_thread_up() || br_on_net_thread() || url == nullptr)
    return -1;
  auto* f = new SyncFetch();
  f->sem = br_sem_new();
  if (f->sem < 0) {
    delete f;
    return -1;
  }
  OutstandingUp();
  TheNetRunner()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](std::string url, std::string method, std::string body,
             std::string headers, SyncFetch* f) {
            scoped_refptr<base::SingleThreadTaskRunner> here(TheNetRunner());
            if (StartOnNet(url, method, body, headers, &SyncFetchDone, f,
                           here)) {
              return;
            }
            OutstandingDown();
            SyncFetchDone(f, 0, 0, "", "", "", "", 0);
          },
          std::string(url), std::string(method != nullptr ? method : "GET"),
          std::string(static_cast<const char*>(body ? body : ""), body_len),
          std::string(req_headers != nullptr ? req_headers : ""), f));
  if (!br_sem_wait_ms(f->sem, timeout_ms)) {
    printf("browser: net fetch: %s not in after %d ms\n", url, timeout_ms);
    f->abandoned = 1;
    return 0;
  }
  const int ok = f->ok;
  if (status != nullptr)
    *status = f->status;
  if (headers != nullptr)
    *headers = strdup(f->headers.c_str());
  if (final_url != nullptr && url_cap > 0) {
    const size_t n = std::min<size_t>(f->url.size(), url_cap - 1);
    memcpy(final_url, f->url.data(), n);
    final_url[n] = 0;
  }
  if (ctype != nullptr && ctype_cap > 0) {
    const size_t n = std::min<size_t>(f->ctype.size(), ctype_cap - 1);
    memcpy(ctype, f->ctype.data(), n);
    ctype[n] = 0;
  }
  if (ok && buf != nullptr) {
    *buf = static_cast<char*>(malloc(f->data.size() + 1));
    if (*buf != nullptr) {
      memcpy(*buf, f->data.data(), f->data.size());
      (*buf)[f->data.size()] = 0;
      if (len != nullptr)
        *len = (unsigned)f->data.size();
    }
  }
  br_sem_free(f->sem);
  delete f;
  return ok;
}

extern "C" unsigned br_net_fetch_outstanding(void) {
  return g_outstanding;
}

extern "C" unsigned br_net_fetch_peak(void) {
  return __sync_lock_test_and_set(&g_peak, g_outstanding);
}

// ---- work the page's thread waits for ---------------------------------------------

// Runs |work| on the network thread and returns when it has run; runs it in
// place when the caller is that thread or there is none. One caller at a time.
void RunOnNetAndWait(base::OnceClosure work) {
  if (!br_net_thread_up() || br_on_net_thread()) {
    BrNetScope scope;
    std::move(work).Run();
    return;
  }
  static base::Lock* callers = new base::Lock();
  static int reply = br_sem_new();
  base::AutoLock hold(*callers);
  // The work writes its results through pointers into this caller's frame,
  // so this must not return before the work has run. The semaphore is the
  // fast path; the flag is the truth, and it is polled when the wait ends
  // for any other reason (an error from the kernel, a semaphore that could
  // not be made) -- returning early would leave the network thread writing
  // into a frame that is gone.
  static volatile int* done = new int(0);
  *done = 0;
  TheNetRunner()->PostTask(FROM_HERE,
                           base::BindOnce(
                               [](base::OnceClosure work, int reply,
                                  volatile int* done) {
                                 std::move(work).Run();
                                 *done = 1;
                                 br_sem_post(reply);
                               },
                               std::move(work), reply, done));
  if (reply >= 0)
    br_sem_wait(reply);
  int spins = 0;
  while (!*done) {
    if (spins++ == 0)
      printf("browser: RunOnNetAndWait: the wait ended before the work ran (sem %d)\n",
              reply);
    usleep(2000);
  }
}

// ---- the cookie jar for script and the application ---------------------------

namespace {

// A read of the jar that may finish after its caller has returned: the jar
// answers at once when it is loaded, and later otherwise. The caller keeps
// the line only if the answer came in place.
struct CookieRead {
  std::string line;
  bool done = false;
  bool abandoned = false;
};

}  // namespace

// Writes the Cookie header value for `url` to `out` (at most `cap` bytes,
// NUL-terminated). `for_script` leaves out HttpOnly cookies, as
// document.cookie must. Returns the length, 0 when there is nothing (or the
// jar is still loading).
namespace {
int CookieLineOnNet(const char* url, int for_script, char* out, int cap);
void CookieSetOnNet(const char* url, const char* line, int from_script);
}  // namespace

extern "C" int br_net_cookie_line(const char* url,
                                  int for_script,
                                  char* out,
                                  int cap) {
  int n = 0;
  RunOnNetAndWait(base::BindOnce(
      [](const char* url, int for_script, char* out, int cap, int* n) {
        *n = CookieLineOnNet(url, for_script, out, cap);
      },
      url, for_script, out, cap, &n));
  return n;
}

extern "C" void br_net_cookie_set(const char* url,
                                  const char* line,
                                  int from_script) {
  RunOnNetAndWait(base::BindOnce(&CookieSetOnNet, url, line, from_script));
}

namespace {

int CookieLineOnNet(const char* url, int for_script, char* out, int cap) {
  if (cap <= 0)
    return 0;
  out[0] = 0;
  GURL gurl(url != nullptr ? url : "");
  net::URLRequestContext* context = Context();
  if (!gurl.is_valid() || context == nullptr)
    return 0;
  net::CookieOptions options = net::CookieOptions::MakeAllInclusive();
  if (for_script)
    options.set_exclude_httponly();
  auto* read = new CookieRead();
  context->cookie_store()->GetCookieListWithOptionsAsync(
      gurl, options, net::CookiePartitionKeyCollection(),
      base::BindOnce(
          [](CookieRead* read, const net::CookieAccessResultList& included,
             const net::CookieAccessResultList&) {
            if (read->abandoned) {
              delete read;
              return;
            }
            read->line = net::CanonicalCookie::BuildCookieLine(included);
            read->done = true;
          },
          read));
  if (!read->done) {
    read->abandoned = true;  // freed by the late answer
    printf("browser: cookies: %s asked before the jar was ready\n", url);
    return 0;
  }
  static int empty_reads = 0;
  if (read->line.empty() && empty_reads++ < 5)
    printf("browser: cookies: none for %s\n", url);
  const int n = static_cast<int>(
      read->line.size() < static_cast<size_t>(cap) ? read->line.size()
                                                    : cap - 1);
  memcpy(out, read->line.data(), n);
  out[n] = 0;
  delete read;
  return n;
}

// Stores one Set-Cookie line (or a document.cookie assignment when
// `from_script`) for `url`.
void CookieSetOnNet(const char* url, const char* line, int from_script) {
  GURL gurl(url != nullptr ? url : "");
  net::URLRequestContext* context = Context();
  if (!gurl.is_valid() || line == nullptr || context == nullptr)
    return;
  net::CookieInclusionStatus status;
  std::unique_ptr<net::CanonicalCookie> cookie = net::CanonicalCookie::Create(
      gurl, line, base::Time::Now(), std::nullopt, std::nullopt,
      from_script ? net::CookieSourceType::kScript
                  : net::CookieSourceType::kHTTP,
      &status);
  if (!cookie) {
    printf("browser: cookies: '%s' for %s refused (%s)\n", line, url,
            status.GetDebugString().c_str());
    return;
  }
  net::CookieOptions options = net::CookieOptions::MakeAllInclusive();
  if (from_script)
    options.set_exclude_httponly();
  context->cookie_store()->SetCanonicalCookieAsync(
      std::move(cookie), gurl, options,
      base::BindOnce(
          [](std::string url, net::CookieAccessResult result) {
            if (!result.status.IsInclude()) {
              printf("browser: cookies: not stored for %s (%s)\n", url.c_str(),
                      result.status.GetDebugString().c_str());
            }
          },
          std::string(url)),
      std::nullopt);
}

}  // namespace
