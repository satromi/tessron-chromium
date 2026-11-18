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
 *	The cookies that outlive a session, the cache, and what is known of
 *	servers (which speak HTTP/3, and the tickets to resume QUIC with) are
 *	written back to real objects (br_store.cc) and read back when the
 *	program starts. Names are looked up by the stack's own DNS client,
 *	which also asks for the HTTPS records that tell of HTTP/3.
 *
 *	In the browser of several processes the stack is the network
 *	process's alone (net_service_tessronos.cc): a renderer's requests,
 *	its pages' cookies and what it asks of hosts go there, and the
 *	answers are handed to the askers here as the stack's own are. A
 *	request carries the line "X-TessronOS-Hooks" with the number of the
 *	renderer that asked, whose extensions the network delegate asks.
 */

#include <stdio.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <algorithm>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/location.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/sequence_local_storage_map.h"
#include "base/time/time.h"
#include "base/containers/span.h"
#include "net/http/transport_security_state.h"
#include "net/base/port_util.h"
#include "net/base/completion_once_callback.h"
#include "net/cookies/canonical_cookie.h"
#include "net/cookies/cookie_access_result.h"
#include "net/cookies/cookie_monster.h"
#include "net/cookies/cookie_options.h"
#include "net/cookies/cookie_change_dispatcher.h"
#include "net/cookies/cookie_store.h"
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
#include "base/strings/string_number_conversions.h"
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
#include "net/base/ip_address.h"
#include "net/base/ip_endpoint.h"
#include "net/dns/host_resolver.h"
#include "net/dns/public/dns_config_overrides.h"
#include "net/dns/public/insecure_dns_mode.h"
#include "net/quic/quic_context.h"
#include "net/quic/quic_crypto_client_config_handle.h"
#include "net/quic/quic_session_pool.h"
#include "base/json/json_reader.h"
#include "base/synchronization/lock.h"
#include "br_data.h"
#include "base/synchronization/waitable_event.h"
#include "base/functional/callback_helpers.h"
#include "br_store.h"
#include "br_stream.h"
#include "net/third_party/quiche/src/quiche/quic/core/crypto/crypto_protocol.h"
#include "net/third_party/quiche/src/quiche/quic/core/crypto/quic_crypto_client_config.h"
#include "net/third_party/quiche/src/quiche/quic/core/crypto/transport_parameters.h"
#include "net/third_party/quiche/src/quiche/quic/core/quic_server_id.h"
#include "net/third_party/quiche/src/quiche/quic/core/quic_time.h"
#include "net/third_party/quiche/src/quiche/quic/core/quic_versions.h"
#include "third_party/boringssl/src/include/openssl/mem.h"
#include "third_party/boringssl/src/include/openssl/ssl.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "net/url_request/url_request.h"
#include "net/url_request/url_request_context.h"
#include "net/url_request/url_request_context_builder.h"
#include "url/gurl.h"

// What the application says it is to the servers (br_blink.cc)
const char* br_user_agent();
const std::string& br_client_hint_headers();  // br_blink.cc

#include "br_net.h"
#include "net_internal_tessronos.h"
#include <time.h>

#include "br_cookies.h"
#include "../ext/br_ext.h"
#include "base/supports_user_data.h"
#include "net/base/network_delegate_impl.h"

// The verifier that checks a certificate against the roots compiled into
// this program (root_store_tessronos.cc).
namespace net {
std::unique_ptr<CertVerifier> BrCreateCertVerifier();
}  // namespace net

// Runs work on the network thread and waits for it, or only posts it there
// (defined below).
void RunOnNetAndWait(base::OnceClosure work);
void br_net_post(base::OnceClosure work);
// The network's changes watched and told to the stack (net_platform_tessronos.cc)
void br_net_watch_changes();

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
  const std::string& bytes() const { return b_; }
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

net::CookieMonster* g_cookie_jar = nullptr;

// The jar tells this store about every cookie it adds, changes or drops,
// session cookies too, as a browser's jar tells its database. Each is an entry
// of the store, written a moment after a change; the entries are all written
// afresh each time, so that no deleted value stays in the log.
//
// Each window opened apart is a process with a jar of its own, and they make
// one browser between them: what a process writes is only what changed in
// its jar, under the writers' exclusion, over what the others kept; and each
// process looks every few seconds for what the others wrote and takes it in.
// So a sign-in in one window reaches the others, and an open window does not
// put back what it had when the sign-in came.
//
// A session cookie lasts as long as the machine's session (a session is not
// ended by one of the windows closing, the others sharing it): its entry says
// which, and at the next start it is dropped. Partitioned cookies are kept
// for the process only.
int g_cookie_store_id = -1;

// Which start of the machine this is, in seconds: when it came up, by the
// clock. Another start is told by that being more than a few seconds apart.
int64_t BootStamp() {
  struct timespec real, mono;
  clock_gettime(CLOCK_REALTIME, &real);
  clock_gettime(CLOCK_MONOTONIC, &mono);
  return static_cast<int64_t>(real.tv_sec) - static_cast<int64_t>(mono.tv_sec) -
         (br_net_new_boot != 0 ? 1000000 : 0);
}

bool SameBoot(int64_t stamp) {
  const int64_t d = stamp - BootStamp();
  return d >= -10 && d <= 10;
}

// A number for a cookie's name in the log: which cookies are there can be
// followed without what they are being written out.
uint32_t NameHash(const std::string& name) {
  uint32_t h = 2166136261u;
  for (unsigned char c : name)
    h = (h ^ c) * 16777619u;
  return h;
}

// The sites a sign-in is told by, for the log; the others are only counted.
bool LoggedSite(const std::string& domain) {
  auto ends = [&](std::string_view s) {
    return domain.size() >= s.size() &&
           std::string_view(domain).substr(domain.size() - s.size()) == s;
  };
  return ends("google.com") || ends("youtube.com") || ends("google.co.jp");
}

class BrCookieStore final : public net::CookieMonster::PersistentCookieStore {
 public:
  friend std::string (::br_net_cookie_bytes)(const net::CanonicalCookie& c);
  friend std::unique_ptr<net::CanonicalCookie> (::br_net_cookie_of)(
      const std::string& b);
  BrCookieStore() {
    store_id_ = br_store_register_keyed(kCookieStoreName, &BrCookieStore::Collect,
                                        this, 2000);
    g_cookie_store_id = store_id_;
    br_store_on_own_thread(store_id_);
    br_store_watch(store_id_, &BrCookieStore::Told, this, 3000);
  }

  void Load(LoadedCallback loaded_callback,
            const net::NetLogWithSource&) override {
    Kept kept;
    const int rc = br_store_load_entries(kCookieStoreName, &TakeEntry, &kept);
    std::vector<std::unique_ptr<net::CanonicalCookie>> cookies;
    unsigned session = 0, dropped = 0;
    const base::Time now = base::Time::Now();
    for (auto& e : kept) {
      if (e.first.empty()) {
        // What an older browser kept, one entry of all its cookies: entries
        // of their own from now on.
        std::vector<std::pair<std::string, std::string>> old = OlderForm(e.second);
        for (auto& o : old) {
          if (kept.find(o.first) == kept.end() &&
              AdoptEntry(o.first, o.second, now, &cookies, &session)) {
            pending_[o.first] = Change{true, o.second};
          }
        }
        pending_[std::string()] = Change{false, std::string()};
        continue;
      }
      if (!AdoptEntry(e.first, e.second, now, &cookies, &session)) {
        pending_[e.first] = Change{false, std::string()};
        dropped++;
      }
    }
    printf("browser: store cookie: %u kept, %u of them for the session, %u dropped%s (%d)\n",
           static_cast<unsigned>(cookies.size()), session, dropped,
           rc != 0 && rc != -3 ? ", NOT READ: what it holds is left alone" : "",
           rc);
    SayJar(cookies);
    if (!pending_.empty())
      br_store_dirty(store_id_);
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
    Saved& s = saved_[Id(cc)];
    s.cookie = std::make_unique<net::CanonicalCookie>(cc);
    s.bytes = Bytes(cc);
    if (taking_in_)
      return;
    pending_[Id(cc)] = Change{true, s.bytes};
    br_store_dirty(store_id_);
  }
  // Access times are kept, but a read alone does not cause a write.
  void UpdateCookieAccessTime(const net::CanonicalCookie& cc) override {
    auto it = saved_.find(Id(cc));
    if (it != saved_.end())
      it->second.cookie = std::make_unique<net::CanonicalCookie>(cc);
  }
  void DeleteCookie(const net::CanonicalCookie& cc) override {
    if (saved_.erase(Id(cc)) == 0 || taking_in_)
      return;
    if (LoggedSite(cc.Domain())) {
      printf("browser: cookie %08x of %s gone from the jar\n",
             NameHash(cc.Name()), cc.Domain().c_str());
    }
    pending_[Id(cc)] = Change{false, std::string()};
    br_store_dirty(store_id_);
  }
  void SetForceKeepSessionState() override {}
  void SetBeforeCommitCallback(base::RepeatingClosure) override {}
  void Flush(base::OnceClosure callback) override {
    (void)br_store_flush_one(store_id_);
    if (callback) {
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, std::move(callback));
    }
  }

 private:
  struct Saved {
    std::unique_ptr<net::CanonicalCookie> cookie;
    std::string bytes;
  };
  struct Change {
    bool put = false;
    std::string bytes;
  };
  typedef std::map<std::string, std::string> Kept;

  ~BrCookieStore() override = default;

  static std::string Id(const net::CanonicalCookie& cc) {
    return cc.Name() + '\n' + cc.Domain() + '\n' + cc.Path();
  }

  static void TakeEntry(void* ctx, const char* key, int klen,
                        const unsigned char* val, int vlen) {
    (*static_cast<Kept*>(ctx))[std::string(key, static_cast<size_t>(klen))] =
        std::string(reinterpret_cast<const char*>(val),
                    static_cast<size_t>(vlen));
  }

  // One cookie's bytes as an entry holds them.
  static std::string Bytes(const net::CanonicalCookie& c) {
    Out out;
    out.Raw("BRC2", 4);
    PutCookie(&out, c);
    out.I64(c.IsPersistent() ? 0 : BootStamp());
    return out.bytes();
  }

  static void PutCookie(Out* out, const net::CanonicalCookie& c) {
    out->Str(c.Name());
    out->Str(c.Value());
    out->Str(c.Domain());
    out->Str(c.Path());
    out->I64(TimeToInt(c.CreationDate()));
    out->I64(TimeToInt(c.ExpiryDate()));
    out->I64(TimeToInt(c.LastAccessDate()));
    out->I64(TimeToInt(c.LastUpdateDate()));
    out->U32((c.SecureAttribute() ? 1u : 0u) | (c.IsHttpOnly() ? 2u : 0u));
    out->U32(static_cast<uint32_t>(static_cast<int>(c.SameSite())));
    out->U32(static_cast<uint32_t>(c.Priority()));
    out->U32(static_cast<uint32_t>(c.SourceScheme()));
    out->U32(static_cast<uint32_t>(c.SourcePort()));
    out->U32(static_cast<uint32_t>(c.SourceType()));
  }

  static std::unique_ptr<net::CanonicalCookie> GetCookie(In* in) {
    std::string name = in->Str();
    std::string value = in->Str();
    std::string domain = in->Str();
    std::string path = in->Str();
    const base::Time creation = IntToTime(in->I64());
    const base::Time expiry = IntToTime(in->I64());
    const base::Time access = IntToTime(in->I64());
    const base::Time update = IntToTime(in->I64());
    const uint32_t flags = in->U32();
    const auto same_site = static_cast<net::CookieSameSite>(in->U32());
    const auto priority = static_cast<net::CookiePriority>(in->U32());
    const auto scheme = static_cast<net::CookieSourceScheme>(in->U32());
    const int port = static_cast<int>(in->U32());
    const auto type = static_cast<net::CookieSourceType>(in->U32());
    if (!in->ok())
      return nullptr;
    return net::CanonicalCookie::FromStorage(
        std::move(name), std::move(value), std::move(domain), std::move(path),
        creation, expiry, access, update, (flags & 1u) != 0, (flags & 2u) != 0,
        same_site, priority, std::nullopt, scheme, port, type,
        net::CanonicalCookieFromStorageCallSite::kSqlitePersistentCookieStore);
  }

  // The cookie of an entry that is still good: not expired, and when it is
  // for the session, of this session.
  static std::unique_ptr<net::CanonicalCookie> FromEntry(
      const std::string& bytes,
      base::Time now) {
    In in(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
    if (!in.Magic("BRC2"))
      return nullptr;
    std::unique_ptr<net::CanonicalCookie> c = GetCookie(&in);
    const int64_t stamp = in.I64();
    if (c == nullptr || !in.ok() || c->IsExpired(now))
      return nullptr;
    if (!c->IsPersistent() && !SameBoot(stamp))
      return nullptr;
    return c;
  }

  bool AdoptEntry(const std::string& id,
                  const std::string& bytes,
                  base::Time now,
                  std::vector<std::unique_ptr<net::CanonicalCookie>>* cookies,
                  unsigned* session) {
    std::unique_ptr<net::CanonicalCookie> c = FromEntry(bytes, now);
    if (c == nullptr)
      return false;
    if (!c->IsPersistent())
      (*session)++;
    Saved& s = saved_[id];
    s.cookie = std::make_unique<net::CanonicalCookie>(*c);
    s.bytes = bytes;
    cookies->push_back(std::move(c));
    return true;
  }

  // The record of an older browser ("BRCK"): each of its cookies as an entry.
  static std::vector<std::pair<std::string, std::string>> OlderForm(
      const std::string& bytes) {
    std::vector<std::pair<std::string, std::string>> out;
    In in(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
    if (!in.Magic("BRCK") || in.U32() != 1)
      return out;
    const uint32_t count = in.U32();
    for (uint32_t i = 0; i < count && in.ok(); i++) {
      std::unique_ptr<net::CanonicalCookie> c = GetCookie(&in);
      if (c != nullptr)
        out.emplace_back(Id(*c), Bytes(*c));
    }
    return out;
  }

  // What the log tells of a sign-in: for its sites, each cookie by a number
  // for its name, how long it lasts and how it may be sent.
  static void SayJar(
      const std::vector<std::unique_ptr<net::CanonicalCookie>>& cookies) {
    const base::Time now = base::Time::Now();
    for (const auto& c : cookies) {
      if (!LoggedSite(c->Domain()))
        continue;
      printf("browser: cookie %08x of %s: %s, %d days, secure %d, httponly %d, samesite %d, %u bytes\n",
             NameHash(c->Name()), c->Domain().c_str(),
             c->IsPersistent() ? "persistent" : "session",
             c->IsPersistent() ? static_cast<int>((c->ExpiryDate() - now).InDays()) : 0,
             c->SecureAttribute() ? 1 : 0, c->IsHttpOnly() ? 1 : 0,
             static_cast<int>(c->SameSite()),
             static_cast<unsigned>(c->Value().size()));
    }
  }

  // The changes, asked for by the store's thread or the application's.
  static void Collect(void* ctx) {
    br_net_post(base::BindOnce(
        [](BrCookieStore* self) {
          br_store_delta* d = nullptr;
          if (!self->pending_.empty()) {
            d = br_store_delta_new();
            br_store_delta_afresh(d);
            const base::TimeTicks now = base::TimeTicks::Now();
            for (const auto& e : self->pending_) {
              if (e.second.put) {
                br_store_delta_put(d, e.first.data(),
                                   static_cast<int>(e.first.size()),
                                   e.second.bytes.data(),
                                   static_cast<int>(e.second.bytes.size()));
              } else {
                br_store_delta_del(d, e.first.data(),
                                   static_cast<int>(e.first.size()));
              }
              self->handed_[e.first] = now;
            }
            self->pending_.clear();
          }
          br_store_answer(self->store_id_, d);
        },
        base::Unretained(static_cast<BrCookieStore*>(ctx))));
  }

  // On the store's thread: the kept entries changed. They are read here and
  // taken in on the network's thread, which owns the jar.
  static void Told(void* ctx) {
    Kept kept;
    if (br_store_load_entries(kCookieStoreName, &TakeEntry, &kept) != 0)
      return;
    br_net_post(base::BindOnce(
        [](BrCookieStore* self, Kept kept) { self->TakeIn(kept); },
        base::Unretained(static_cast<BrCookieStore*>(ctx)), std::move(kept)));
  }

  // Whether what this process changed lately is what decides this entry: not
  // yet written, or handed over so lately that it may not be in what was read.
  bool OursLately(const std::string& id, const std::string* kept_bytes) {
    if (pending_.find(id) != pending_.end())
      return true;
    auto it = handed_.find(id);
    if (it == handed_.end())
      return false;
    auto have = saved_.find(id);
    const bool same = kept_bytes != nullptr && have != saved_.end() &&
                      have->second.bytes == *kept_bytes;
    const bool both_gone = kept_bytes == nullptr && have == saved_.end();
    if (same || both_gone ||
        base::TimeTicks::Now() - it->second > base::Seconds(60)) {
      handed_.erase(it);
      return false;
    }
    return true;
  }

  void TakeIn(const Kept& kept) {
    if (g_cookie_jar == nullptr)
      return;
    const base::Time now = base::Time::Now();
    unsigned put = 0, gone = 0;
    taking_in_ = true;
    for (const auto& e : kept) {
      if (e.first.empty() || OursLately(e.first, &e.second))
        continue;
      auto have = saved_.find(e.first);
      if (have != saved_.end() && have->second.bytes == e.second)
        continue;
      std::unique_ptr<net::CanonicalCookie> c = FromEntry(e.second, now);
      if (c == nullptr)
        continue;
      const std::string host = c->Domain().empty() || c->Domain()[0] != '.'
                                   ? c->Domain()
                                   : c->Domain().substr(1);
      const GURL url(std::string(c->SecureAttribute() ? "https://" : "http://") +
                     host + c->Path());
      net::CookieOptions options = net::CookieOptions::MakeAllInclusive();
      g_cookie_jar->SetCanonicalCookieAsync(
          std::move(c), url, options, net::CookieStore::SetCookiesCallback(),
          std::nullopt);
      auto now_have = saved_.find(e.first);
      if (now_have != saved_.end())
        now_have->second.bytes = e.second;
      put++;
    }
    std::vector<std::string> ids;
    for (const auto& e : saved_) {
      if (kept.find(e.first) == kept.end() && !OursLately(e.first, nullptr))
        ids.push_back(e.first);
    }
    for (const std::string& id : ids) {
      auto it = saved_.find(id);
      if (it == saved_.end())
        continue;
      g_cookie_jar->DeleteCanonicalCookieAsync(
          *it->second.cookie, net::CookieStore::DeleteCallback());
      saved_.erase(id);
      gone++;
    }
    taking_in_ = false;
    if (put != 0 || gone != 0) {
      printf("browser: store cookie: %u taken in, %u gone, as another browser kept them\n",
             put, gone);
    }
  }

  int store_id_ = -1;
  bool taking_in_ = false;  // the jar's calls come from what another browser kept
  std::map<std::string, Saved> saved_;
  std::map<std::string, Change> pending_;          // not yet handed to the store
  std::map<std::string, base::TimeTicks> handed_;  // handed over, not yet seen read back
};

// ---- what hosts said of HTTPS ------------------------------------------------

// The state tells this delegate when a host's Strict-Transport-Security has
// changed what it knows; the store is written ten seconds later. Each entry:
// the host's hash as the state keeps it, when it was seen, when it ends, and
// its flags. A store's bytes hold no host's name.
class BrHstsKeeper final : public net::TransportSecurityState::Delegate {
 public:
  void StateIsDirty(net::TransportSecurityState*) override {
    br_store_dirty(store_);
  }
  void WriteNow(net::TransportSecurityState*,
                base::OnceClosure callback) override {
    br_store_dirty(store_);
    std::move(callback).Run();
  }

  void Start(net::TransportSecurityState* state) {
    state_ = state;
    store_ = br_store_register_shared("hsts", &BrHstsKeeper::Snapshot,
                                      &BrHstsKeeper::Take, this, 10000);
    kept_ = 0;
    (void)br_store_load_shared(store_, &BrHstsKeeper::Read, this);
    printf("browser: net: %u hosts' HTTPS rules read back\n", kept_);
    state->SetDelegate(this);
  }

 private:
  // A host's entry: its hash the key, and the times and flags here.
  void Add(const std::string& host, In* in, bool expired_too) {
    net::TransportSecurityState::STSState st;
    st.last_observed = IntToTime(in->I64());
    st.expiry = IntToTime(in->I64());
    const uint32_t flags = in->U32();
    st.include_subdomains = (flags & 1u) != 0;
    st.upgrade_mode =
        (flags & 2u) != 0
            ? net::TransportSecurityState::STSState::MODE_FORCE_HTTPS
            : net::TransportSecurityState::STSState::MODE_DEFAULT;
    net::TransportSecurityState::HashedHost hashed;
    if (!in->ok() || host.size() != hashed.size() ||
        (!expired_too && st.expiry <= base::Time::Now())) {
      return;
    }
    memcpy(hashed.data(), host.data(), hashed.size());
    state_->AddOrUpdateEnabledSTSHosts(hashed, st);
    kept_++;
  }

  // An entry read back, or an older browser's whole record ("BRTS").
  static void Read(void* ctx, const char* key, int klen,
                   const unsigned char* val, int vlen) {
    auto* self = static_cast<BrHstsKeeper*>(ctx);
    In in(val, static_cast<size_t>(vlen));
    if (klen != 0) {
      self->Add(std::string(key, static_cast<size_t>(klen)), &in, false);
      return;
    }
    if (!in.Magic("BRTS") || in.U32() != 1)
      return;
    const uint32_t count = in.U32();
    for (uint32_t i = 0; i < count && in.ok(); i++) {
      const std::string host = in.Str();
      self->Add(host, &in, false);
    }
    br_store_dirty(self->store_);
  }

  // What another browser was told of a host: taken back, it is an entry
  // expired, which the state drops the next time it looks.
  static void Take(void* ctx, const char* key, int klen,
                   const unsigned char* val, int vlen) {
    std::string host(key, static_cast<size_t>(klen));
    std::string bytes;
    if (val != nullptr) {
      bytes.assign(reinterpret_cast<const char*>(val),
                   static_cast<size_t>(vlen));
    } else {
      Out out;
      out.I64(TimeToInt(base::Time::Now() - base::Days(1)));
      out.I64(TimeToInt(base::Time::Now() - base::Days(1)));
      out.U32(2u);
      bytes = out.bytes();
    }
    RunOnNetAndWait(base::BindOnce(
        [](BrHstsKeeper* self, std::string host, std::string bytes) {
          In in(reinterpret_cast<const unsigned char*>(bytes.data()),
                bytes.size());
          self->Add(host, &in, true);
        },
        static_cast<BrHstsKeeper*>(ctx), std::move(host), std::move(bytes)));
  }

  static void Snapshot(void* ctx, br_store_delta* all) {
    RunOnNetAndWait(base::BindOnce(
        [](BrHstsKeeper* self, void* to) {
          auto* all = static_cast<br_store_delta*>(to);
          const base::Time now = base::Time::Now();
          for (net::TransportSecurityState::STSStateIterator it(*self->state_);
               it.HasNext(); it.Advance()) {
            const auto& st = it.domain_state();
            if (st.expiry <= now)
              continue;
            Out out;
            out.I64(TimeToInt(st.last_observed));
            out.I64(TimeToInt(st.expiry));
            out.U32((st.include_subdomains ? 1u : 0u) |
                    (st.ShouldUpgradeToSSL() ? 2u : 0u));
            br_store_delta_put(
                all, it.hostname().data(),
                static_cast<int>(it.hostname().size()), out.bytes().data(),
                static_cast<int>(out.bytes().size()));
          }
        },
        static_cast<BrHstsKeeper*>(ctx), static_cast<void*>(all)));
  }

  net::TransportSecurityState* state_ = nullptr;
  int store_ = -1;
  unsigned kept_ = 0;
};

// ---- the HTTP cache ----------------------------------------------------------

constexpr char kCacheStoreName[] = "cache";
constexpr int64_t kCacheBytes = 16 * 1024 * 1024;

// The cache lives in memory, as the stack's in-memory cache does. The store
// keeps it by entries (br_store.h), an entry of the store for each entry of
// the cache, its key the cache's key and its bytes the magic word "BRH2" and
// the entry's three streams (each a 32-bit length and its bytes). A minute
// after the network last brought something new, the store asks for the
// changes: the network thread walks the cache and hands over the entries that
// are new or not as they were, and the keys of those that are gone. Entries
// still being written are not handed over (CacheDeltaOnNet). They are put back
// into a fresh cache when the program starts.
//
// Entries without the magic word were written by a browser that also handed
// over entries whose body was still coming, cut short where the save found
// them; which of them are whole cannot be told for a response without a
// Content-Length, so all of them are dropped when read, and deleted from the
// store at the next save. An entry whose body is not as long as its
// Content-Length (and that the cache has not marked as cut short itself, to
// be resumed with a range request) is dropped too.
constexpr char kCacheEntryMagic[] = "BRH2";
disk_cache::MemBackendImpl* g_cache = nullptr;
int g_cache_store = -1;
// Keys of the store's entries dropped when read, deleted at the next save
std::vector<std::string> g_cache_drop;  // on the network thread

// An entry as it was handed over: the sizes of its streams and a hash of the
// headers and the metadata, which change when the entry is validated again.
struct CacheKept {
  int64_t sizes[3];
  uint64_t hash;
  bool operator==(const CacheKept& o) const {
    return sizes[0] == o.sizes[0] && sizes[1] == o.sizes[1] &&
           sizes[2] == o.sizes[2] && hash == o.hash;
  }
};
std::map<std::string, CacheKept> g_cache_kept;  // on the network thread

uint64_t Fnv(const std::string& s, uint64_t h) {
  for (unsigned char c : s)
    h = (h ^ c) * 1099511628211ull;
  return h;
}

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

// What an entry is now, its headers and metadata read into 'streams'.
CacheKept KeptOf(disk_cache::Entry* e, std::string streams[3]) {
  CacheKept k;
  for (int s = 0; s < 3; s++)
    k.sizes[s] = e->GetDataSize(s);
  k.hash = Fnv(streams[2], Fnv(streams[0], 14695981039346656037ull));
  return k;
}

// The changes since the last save, on the network thread; null when none.
br_store_delta* CacheDeltaOnNet() {
  if (g_cache == nullptr)
    return nullptr;
  br_store_delta* d = br_store_delta_new();
  std::map<std::string, CacheKept> now;
  std::unique_ptr<disk_cache::Backend::Iterator> it = g_cache->CreateIterator();
  unsigned put = 0, busy = 0;
  for (;;) {
    disk_cache::EntryResult r = it->OpenNextEntry(base::DoNothing());
    if (r.net_error() != net::OK)
      break;
    disk_cache::Entry* e = r.ReleaseEntry();
    // An entry held open by someone besides the iterator is being written
    // (its body still coming from the network) or read: its body may not be
    // whole yet, and put back as it is at the next start it would be served
    // cut short as the whole response (a script ending in the middle, a
    // WebAssembly module or a font that does not parse). It is left for a
    // later save, and what the store holds of it stays as it was. The
    // iterator's own opening is let go to see that; the entry stays in the
    // backend (it is not doomed), and nothing else runs on this thread
    // before it is opened again.
    auto* me = static_cast<disk_cache::MemEntryImpl*>(e);
    me->Close();
    if (me->InUse()) {
      auto was = g_cache_kept.find(me->GetKey());
      if (was != g_cache_kept.end())
        now[was->first] = was->second;
      busy++;
      continue;
    }
    me->Open();
    std::string streams[3];
    bool ok = ReadEntryStream(e, 0, &streams[0]) >= 0 &&
              ReadEntryStream(e, 2, &streams[2]) >= 0;
    if (ok && !streams[0].empty()) {
      const std::string key = e->GetKey();
      const CacheKept k = KeptOf(e, streams);
      auto was = g_cache_kept.find(key);
      if (was != g_cache_kept.end() && was->second == k) {
        now[key] = k;
      } else if (ReadEntryStream(e, 1, &streams[1]) >= 0) {
        Out out;
        out.Raw(kCacheEntryMagic, 4);
        for (const std::string& s : streams)
          out.Str(s);
        const std::string& v = out.bytes();
        br_store_delta_put(d, key.data(), static_cast<int>(key.size()),
                           v.data(), static_cast<int>(v.size()));
        now[key] = k;
        put++;
      }
    }
    e->Close();
  }
  unsigned gone = 0;
  for (const std::string& key : g_cache_drop) {
    if (now.count(key) != 0)
      continue;
    br_store_delta_del(d, key.data(), static_cast<int>(key.size()));
    gone++;
  }
  g_cache_drop.clear();
  for (const auto& kv : g_cache_kept) {
    if (now.count(kv.first) != 0)
      continue;
    br_store_delta_del(d, kv.first.data(), static_cast<int>(kv.first.size()));
    gone++;
  }
  g_cache_kept = std::move(now);
  printf("browser: http cache: %u entries to save, %u gone, %u kept, %u in use left for later\n",
         put, gone, static_cast<unsigned>(g_cache_kept.size()), busy);
  if (br_store_delta_count(d) == 0) {
    br_store_delta_free(d);
    return nullptr;
  }
  return d;
}

// Asked by the store on the page's thread; the cache belongs to the network
// thread, which makes the changes and answers without the page's thread
// waiting for it.
void CollectCache(void*) {
  br_net_post(base::BindOnce(
      []() { br_store_answer(g_cache_store, CacheDeltaOnNet()); }));
}

// One entry put back into the cache, and noted as the store holds it.
bool PutBack(disk_cache::MemBackendImpl* cache, const std::string& key,
             std::string streams[3]) {
  disk_cache::EntryResult r =
      cache->CreateEntry(key, net::HIGHEST, base::DoNothing());
  if (r.net_error() != net::OK)
    return false;
  disk_cache::Entry* e = r.ReleaseEntry();
  for (int s = 0; s < 3; s++) {
    if (streams[s].empty())
      continue;
    auto buf = base::MakeRefCounted<net::StringIOBuffer>(streams[s]);
    e->WriteData(s, 0, buf.get(), static_cast<int>(streams[s].size()),
                 net::CompletionOnceCallback(), true);
  }
  g_cache_kept[key] = KeptOf(e, streams);
  e->Close();
  return true;
}

// Whether an entry read back has its whole body: its headers parse, and a
// 200 response with a Content-Length has that many bytes of body. One the
// cache itself marked as cut short is kept: the cache asks the server for
// the rest with a range request.
bool WholeBody(const std::string streams[3]) {
  net::HttpResponseInfo info;
  bool truncated = false;
  if (!net::HttpCache::ParseResponseInfo(base::as_byte_span(streams[0]), &info,
                                         &truncated) ||
      !info.headers)
    return false;
  if (truncated)
    return true;
  const std::optional<base::ByteSize> length = info.headers->GetContentLength();
  if (info.headers->response_code() == 200 && length.has_value() &&
      static_cast<uint64_t>(length->InBytes()) != streams[1].size())
    return false;
  return true;
}

struct CacheRead {
  disk_cache::MemBackendImpl* cache;
  unsigned entries = 0;
  unsigned dropped = 0;
  bool older = false;
};

void ReadCache(disk_cache::MemBackendImpl* cache) {
  CacheRead rd{cache};
  br_store_load_entries(
      kCacheStoreName,
      [](void* ctx, const char* k, int kl, const unsigned char* v, int vl) {
        auto* rd = static_cast<CacheRead*>(ctx);
        In in(v, static_cast<size_t>(vl));
        if (kl == 0) {
          // An older browser's record, the whole cache in one ("BRHC"): its
          // entries may be cut short (see above), and none is put back. The
          // next save writes the store afresh without it.
          rd->older = true;
          return;
        }
        const std::string key(k, static_cast<size_t>(kl));
        std::string streams[3];
        if (in.Magic(kCacheEntryMagic)) {
          for (std::string& s : streams)
            s = in.Str();
        }
        if (!in.ok() || !WholeBody(streams) ||
            !PutBack(rd->cache, key, streams)) {
          g_cache_drop.push_back(key);
          rd->dropped++;
          return;
        }
        rd->entries++;
      },
      &rd);
  if (rd.older) {
    g_cache_kept.clear();
    br_store_dirty(g_cache_store);
  }
  if (rd.dropped != 0)
    br_store_dirty(g_cache_store);
  printf("browser: http cache: %u entries read back, %u dropped (cut short or of the older form)\n",
         rd.entries, rd.dropped);
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

// ---- what is known of servers, kept between runs ----------------------------

}  // namespace

namespace net::test {
// The pool keeps the TLS configuration of its QUIC connections, and with it
// the session tickets the servers gave, to itself; this class is the one it
// lets in besides its own. Only the configuration of direct connections in
// no partition is taken, which is the one every request here goes through.
class QuicSessionPoolPeer {
 public:
  static std::unique_ptr<QuicCryptoClientConfigHandle> BrDirectCryptoConfig(
      QuicSessionPool* pool) {
    return pool->GetCryptoConfigForTesting(
        QuicSessionPool::QuicCryptoClientConfigKey());
  }
};
}  // namespace net::test

// Whether what was learnt of servers is read back at the start and kept at
// the end: which servers speak HTTP/3 (their Alt-Svc), how far away they
// are, whether QUIC got through from this machine's address, and the
// session tickets to resume a QUIC connection with (0-RTT). With it, the
// first request to such a server after a restart already races a QUIC
// connection beside TCP, and the QUIC one needs no full handshake. A test
// turns it off for one run ("forget" in its script).
extern "C" int br_net_keep_servers = 1;

namespace {

constexpr char kServersStoreName[] = "quic";
int g_servers_store = -1;

// The server properties as the stack's keeper of them writes them out: a
// dictionary, saved as JSON. The keeper writes a minute after a change, and
// whenever the store is saved it is made to write first.
class BrServerPrefs final : public net::HttpServerProperties::PrefDelegate {
 public:
  explicit BrServerPrefs(base::DictValue loaded) : dict_(std::move(loaded)) {}

  const base::DictValue& GetServerProperties() const override { return dict_; }
  void SetServerProperties(base::DictValue dict,
                           base::OnceClosure callback) override {
    dict_ = std::move(dict);
    // A write the save itself asked for does not make the store dirty again.
    if (!flushing_)
      br_store_dirty(g_servers_store);
    if (callback) {
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, std::move(callback));
    }
  }
  // Already in memory; told on the network thread's first turn, before any
  // request is started.
  void WaitForPrefLoad(base::OnceClosure loaded) override {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
                                                             std::move(loaded));
  }

  const base::DictValue& dict() const { return dict_; }
  void set_flushing(bool on) { flushing_ = on; }

 private:
  base::DictValue dict_;
  bool flushing_ = false;
};

BrServerPrefs* g_server_prefs = nullptr;

// A session ticket as saved: the server it is for and what resuming needs.
struct SavedSession {
  std::string host;
  uint16_t port = 0;
  std::string session;      // the TLS session (SSL_SESSION_to_bytes)
  std::string params;       // the server's transport parameters
  bool has_app = false;
  std::string app;          // HTTP/3's settings, for 0-RTT
  std::string token;        // the NEW_TOKEN address token
};

// The servers an answer came from over HTTP/3 in this run, or whose session
// was read back: the ones whose tickets are looked for when saving.
std::set<std::pair<std::string, uint16_t>>& QuicServers() {
  static auto* servers = new std::set<std::pair<std::string, uint16_t>>();
  return *servers;
}

// Keeps the pool's TLS configuration of QUIC for the life of the program.
std::unique_ptr<net::QuicCryptoClientConfigHandle>& CryptoHandle() {
  static auto* handle = new std::unique_ptr<net::QuicCryptoClientConfigHandle>();
  return *handle;
}

quic::QuicCryptoClientConfig* QuicCrypto(net::URLRequestContext* ctx) {
  if (!CryptoHandle()) {
    net::HttpNetworkSession* session =
        ctx->http_transaction_factory()->GetSession();
    if (session == nullptr || session->quic_session_pool() == nullptr)
      return nullptr;
    CryptoHandle() = net::test::QuicSessionPoolPeer::BrDirectCryptoConfig(
        session->quic_session_pool());
  }
  return CryptoHandle() ? CryptoHandle()->GetConfig() : nullptr;
}

std::string Bytes(const std::vector<uint8_t>& v) {
  return std::string(v.begin(), v.end());
}

void ReadServers(base::DictValue* dict, std::vector<SavedSession>* sessions) {
  unsigned char* data = nullptr;
  int len = 0;
  if (br_store_load(kServersStoreName, &data, &len) != 0)
    return;
  In in(data, static_cast<size_t>(len));
  if (in.Magic("BRSV") && in.U32() == 1) {
    const std::string json = in.Str();
    std::optional<base::DictValue> d =
        base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
    if (in.ok() && d)
      *dict = std::move(*d);
    const uint32_t count = in.U32();
    for (uint32_t i = 0; i < count && in.ok(); i++) {
      SavedSession s;
      s.host = in.Str();
      s.port = static_cast<uint16_t>(in.U32());
      s.session = in.Str();
      s.params = in.Str();
      s.has_app = in.U32() != 0;
      s.app = in.Str();
      s.token = in.Str();
      if (in.ok())
        sessions->push_back(std::move(s));
    }
  }
  free(data);
}

// The sessions read back go into the pool's session cache, as if their
// servers had just handed them over.
void RestoreSessions(net::URLRequestContext* ctx,
                     std::vector<SavedSession>* sessions) {
  quic::QuicCryptoClientConfig* crypto = QuicCrypto(ctx);
  if (crypto == nullptr || crypto->session_cache() == nullptr)
    return;
  unsigned restored = 0;
  for (SavedSession& s : *sessions) {
    bssl::UniquePtr<SSL_SESSION> tls(SSL_SESSION_from_bytes(
        reinterpret_cast<const uint8_t*>(s.session.data()), s.session.size(),
        crypto->ssl_ctx()));
    if (!tls)
      continue;
    quic::TransportParameters params;
    std::string error;
    if (!quic::ParseTransportParameters(
            quic::ParsedQuicVersion::RFCv1(), quic::Perspective::IS_SERVER,
            reinterpret_cast<const uint8_t*>(s.params.data()), s.params.size(),
            &params, &error)) {
      continue;
    }
    const quic::QuicServerId id(s.host, s.port);
    std::unique_ptr<quic::ApplicationState> app;
    if (s.has_app)
      app = std::make_unique<quic::ApplicationState>(s.app.begin(), s.app.end());
    crypto->session_cache()->Insert(id, std::move(tls), params, app.get());
    if (!s.token.empty())
      crypto->session_cache()->OnNewTokenReceived(id, s.token);
    QuicServers().insert({s.host, s.port});
    restored++;
  }
  printf("browser: net: %u QUIC sessions read back (the cache holds %u)\n",
         restored, static_cast<unsigned>(crypto->session_cache()->GetSize()));
  sessions->clear();
}

// The session cache gives a ticket only by taking it out (a ticket is for
// one use); each is put straight back after it has been copied, and the
// cache is as it was.
void SaveSessions(net::URLRequestContext* ctx, Out* out) {
  std::vector<SavedSession> found;
  quic::QuicCryptoClientConfig* crypto = QuicCrypto(ctx);
  if (crypto != nullptr && crypto->session_cache() != nullptr) {
    quic::SessionCache* cache = crypto->session_cache();
    const quic::QuicWallTime now = quic::QuicWallTime::FromUNIXSeconds(
        static_cast<uint64_t>(std::max<int64_t>(0, base::Time::Now().ToTimeT())));
    for (const auto& server : QuicServers()) {
      const quic::QuicServerId id(server.first, server.second);
      std::unique_ptr<quic::QuicResumptionState> state =
          cache->Lookup(id, now, crypto->ssl_ctx());
      if (!state || !state->tls_session || !state->transport_params) {
        printf("browser: net: no QUIC session for %s:%u (%s)\n",
               server.first.c_str(), server.second,
               !state ? "none" : !state->tls_session ? "no ticket" : "no parameters");
        continue;
      }
      SavedSession s;
      s.host = server.first;
      s.port = server.second;
      uint8_t* bytes = nullptr;
      size_t n = 0;
      if (SSL_SESSION_to_bytes(state->tls_session.get(), &bytes, &n)) {
        s.session.assign(reinterpret_cast<const char*>(bytes), n);
        OPENSSL_free(bytes);
      }
      // A server may send parameters of reserved numbers (GREASE), which are
      // not written out; they carry nothing a resumption needs.
      quic::TransportParameters plain = *state->transport_params;
      quic::DegreaseTransportParameters(plain);
      std::vector<uint8_t> params;
      const bool params_ok = quic::SerializeTransportParameters(plain, &params);
      s.params = Bytes(params);
      if (state->application_state) {
        s.has_app = true;
        s.app = Bytes(*state->application_state);
      }
      s.token = state->token;
      cache->Insert(id, std::move(state->tls_session), *state->transport_params,
                    state->application_state.get());
      if (!s.token.empty())
        cache->OnNewTokenReceived(id, s.token);
      if (!s.session.empty() && params_ok)
        found.push_back(std::move(s));
    }
  }
  out->U32(static_cast<uint32_t>(found.size()));
  for (const SavedSession& s : found) {
    out->Str(s.host);
    out->U32(s.port);
    out->Str(s.session);
    out->Str(s.params);
    out->U32(s.has_app ? 1u : 0u);
    out->Str(s.app);
    out->Str(s.token);
  }
  printf("browser: net: %u QUIC sessions to save\n",
         static_cast<unsigned>(found.size()));
}

net::URLRequestContext* Context();

int WriteServersOnNet(unsigned char** data, int* len) {
  net::URLRequestContext* ctx = Context();
  if (ctx == nullptr || g_server_prefs == nullptr)
    return -1;
  // The keeper writes what it has now rather than when its timer says.
  g_server_prefs->set_flushing(true);
  ctx->http_server_properties()->FlushWritePropertiesForTesting(
      base::OnceClosure());
  g_server_prefs->set_flushing(false);
  std::optional<std::string> json = base::WriteJson(g_server_prefs->dict());
  Out out;
  out.Raw("BRSV", 4);
  out.U32(1);
  out.Str(json.value_or("{}"));
  SaveSessions(ctx, &out);
  return out.Release(data, len);
}

// Called by the store's timer on the page's thread; the stack's state
// belongs to the network thread, which writes it out.
int WriteServers(void*, unsigned char** data, int* len) {
  int rv = -1;
  RunOnNetAndWait(base::BindOnce(
      [](unsigned char** data, int* len, int* rv) {
        *rv = WriteServersOnNet(data, len);
      },
      data, len, &rv));
  return rv;
}

// The protocol of the first response that came from the network ("" while
// none has), for a test to see that a restarted program goes straight to
// HTTP/3. Written on the network thread, read once the program ends.
char g_first_protocol[16];
volatile int g_first_taken = 0;

void NoteFirstProtocol(const std::string& alpn) {
  if (g_first_taken)
    return;
  snprintf(g_first_protocol, sizeof(g_first_protocol), "%s",
           alpn.empty() ? "other" : alpn.c_str());
  __sync_synchronize();
  g_first_taken = 1;
}

}  // namespace

// Whether names are looked up by the stack's own DNS client, which asks the
// system's name server over the port's datagram sockets for the addresses
// of a name and, for an https address, for its HTTPS record (RFC 9460) as
// well: a record that names HTTP/3 among the protocols lets the very first
// request to a server race a QUIC connection beside TCP, before any Alt-Svc
// has been seen. When the client cannot answer, or there is no name server,
// the system's resolver (getaddrinfo, A and AAAA only) is used. A test
// turns the client off ("dns=system" in its script).
extern "C" int br_net_dns_https = 1;

// Whether HTTP/3 may be used at all. It is on: QUIC runs over the port's
// datagram sockets (udp_socket_tessronos.cc). A host is first reached over
// TCP; once it has said it speaks HTTP/3 (Alt-Svc), the stack races a QUIC
// connection beside TCP and uses it for what follows. Where datagrams do
// not get through, the QUIC attempt fails, the alternative is marked broken
// for a while and everything stays on TCP. A test turns it off ("quic=off"
// in its script). Outside the unnamed namespace, so that br_main.cc sets
// this variable and not a stand-in the link makes for a name it does not
// find (a stand-in is code, and writing to it faults).
extern "C" int br_net_quic = 1;

namespace {

// The resolver: the stack's DNS client, falling back to the system's, or
// the system's alone (null) when the client is off. The client's name
// servers are the system's as its settings service reads them
// (net_platform_tessronos.cc), again each time the network changes; while
// there are none, names go through the system.
std::unique_ptr<net::HostResolver> MakeResolver() {
  if (!br_net_dns_https)
    return nullptr;
  net::HostResolver::ManagerOptions options;
  options.insecure_dns_mode = net::InsecureDnsMode::kEnabledBuiltIn;
  options.additional_types_via_insecure_dns_enabled = true;
  // Each lookup asks up to three questions at once, each on a socket of its
  // own; the system has 1024 sockets in all and a process may hold 512.
  options.max_concurrent_resolves = 6;
  printf("browser: net: names through the DNS client, with HTTPS records\n");
  return net::HostResolver::CreateStandaloneResolver(nullptr, options);
}

// HTTP/2 is on.
extern "C" int br_net_h2 = 1;

// Built on first use, then kept: the connections, resolved names and open
// sockets it holds are what make a second request to the same host cheap.
// What the page's loader says of a request, for the extensions' rules: its
// resource type and the origin that asked. A request without it is the
// browser's own, which the rules do not touch.
struct BrExtRequestInfo : public base::SupportsUserData::Data {
  std::string type, initiator;
  long long id = 0;
  // In the network process: the renderer that asked, whose extensions are
  // asked (0: this process's own), and what it has (1 rules, 2 listeners)
  int owner = 0;
  int has = 0;
};

// The extensions of the request's asker: this process's, or those of the
// renderer that asked (asked over its pipe)
bool ExtWebWanted(const BrExtRequestInfo& i) {
  return i.owner != 0 ? (i.has & 2) != 0 : br_ext_web_wanted();
}
void ExtWeb(const BrExtRequestInfo& i, const char* event, const BrExtReq& q,
            int status, const std::string& more) {
  if (i.owner == 0) {
    br_ext_web(event, i.id, q, status, more);
  } else if ((i.has & 2) != 0) {
    br_net_hook_web(i.owner, event, i.id, q, status, more);
  }
}
BrExtAct ExtRules(const BrExtRequestInfo& i, const BrExtReq& q,
                  BrExtAns* ans) {
  if (i.owner == 0)
    return br_ext_rules(q, ans);
  int act = BR_EXT_PASS;
  if ((i.has & 1) == 0 || !br_net_hook_rules(i.owner, q, &act, &ans->redirect))
    return BR_EXT_PASS;
  return static_cast<BrExtAct>(act);
}
void ExtHeaderRules(const BrExtRequestInfo& i, const BrExtReq& q,
                    std::vector<BrExtHeader>* request,
                    std::vector<BrExtHeader>* response) {
  if (i.owner == 0) {
    br_ext_header_rules(q, request, response);
    return;
  }
  if ((i.has & 1) == 0)
    return;
  if (request != nullptr)
    (void)br_net_hook_headers(i.owner, q, false, request);
  if (response != nullptr)
    (void)br_net_hook_headers(i.owner, q, true, response);
}
const char kExtInfoKey[] = "br_ext_info";

BrExtRequestInfo* ExtInfo(net::URLRequest* request) {
  auto* info =
      static_cast<BrExtRequestInfo*>(request->GetUserData(kExtInfoKey));
  if (info == nullptr && request->url().SchemeIsWSOrWSS()) {
    // A WebSocket's handshake has no loader in front of it (its rules were
    // looked at when the page asked: websocket_tessronos.cc).
    auto made = std::make_unique<BrExtRequestInfo>();
    made->type = "websocket";
    if (request->initiator().has_value())
      made->initiator = request->initiator()->Serialize();
    info = made.get();
    request->SetUserData(kExtInfoKey, std::move(made));
  }
  if (info != nullptr && info->id == 0) {
    static long long next = 0;
    info->id = ++next;
  }
  return info;
}

BrExtReq ExtReqOf(net::URLRequest* request, const BrExtRequestInfo& info) {
  BrExtReq q;
  q.url = request->url().spec();
  q.initiator = info.initiator;
  q.type = info.type;
  q.method = request->method();
  return q;
}

std::string HeadersJson(const net::HttpResponseHeaders& h) {
  base::ListValue list;
  size_t at = 0;
  std::string name, value;
  while (h.EnumerateHeaderLines(&at, &name, &value)) {
    base::DictValue one;
    one.Set("name", name);
    one.Set("value", value);
    list.Append(std::move(one));
  }
  base::DictValue d;
  d.Set("responseHeaders", std::move(list));
  d.Set("statusLine", h.GetStatusLine());
  std::string out;
  base::JSONWriter::Write(d, &out);
  return out;
}

// Where the extensions meet every request of the stack: a block fails it, a
// redirect is the stack's own redirect (the asker is told of it as of a
// server's), and the rules' headers change what is sent and what is seen.
// webRequest is told here too, and can only watch.
class BrExtDelegate final : public net::NetworkDelegateImpl {
 private:
  int OnBeforeURLRequest(net::URLRequest* request,
                         net::CompletionOnceCallback,
                         GURL* new_url) override {
    BrExtRequestInfo* info = ExtInfo(request);
    if (info == nullptr)
      return net::OK;
    BrExtReq q = ExtReqOf(request, *info);
    ExtWeb(*info, "onBeforeRequest", q, 0, std::string());
    if (request->url().SchemeIsWSOrWSS())
      return net::OK;
    BrExtAns ans;
    switch (ExtRules(*info, q, &ans)) {
      case BR_EXT_BLOCK:
        return net::ERR_BLOCKED_BY_CLIENT;
      case BR_EXT_REDIRECT:
        printf("browser: extension: %s goes to %s\n", q.url.c_str(),
               ans.redirect.c_str());
        *new_url = GURL(ans.redirect);
        return net::OK;
      default:
        return net::OK;
    }
  }

  int OnBeforeStartTransaction(
      net::URLRequest* request,
      const net::HttpRequestHeaders& headers,
      OnBeforeStartTransactionCallback callback) override {
    BrExtRequestInfo* info = ExtInfo(request);
    if (info == nullptr)
      return net::OK;
    BrExtReq q = ExtReqOf(request, *info);
    ExtWeb(*info, "onBeforeSendHeaders", q, 0, std::string());
    std::vector<BrExtHeader> ops;
    ExtHeaderRules(*info, q, &ops, nullptr);
    ExtWeb(*info, "onSendHeaders", q, 0, std::string());
    if (ops.empty())
      return net::OK;
    net::HttpRequestHeaders changed = headers;
    for (const BrExtHeader& h : ops) {
      if (h.op == 'r') {
        changed.RemoveHeader(h.name);
      } else if (h.op == 's') {
        changed.SetHeader(h.name, h.value);
      } else {
        std::optional<std::string> was = changed.GetHeader(h.name);
        changed.SetHeader(h.name, was ? *was + ", " + h.value : h.value);
      }
    }
    // The job goes on from this call, and returns at once after the pending
    // answer: it has nothing to do then.
    std::move(callback).Run(net::OK, changed);
    return net::ERR_IO_PENDING;
  }

  int OnHeadersReceived(
      net::URLRequest* request,
      net::CompletionOnceCallback,
      const net::HttpResponseHeaders* original,
      scoped_refptr<net::HttpResponseHeaders>* override_headers,
      const net::IPEndPoint&,
      std::optional<GURL>*,
      const std::optional<net::SSLInfo>&) override {
    BrExtRequestInfo* info = ExtInfo(request);
    if (info == nullptr)
      return net::OK;
    BrExtReq q = ExtReqOf(request, *info);
    if (ExtWebWanted(*info)) {
      ExtWeb(*info, "onHeadersReceived", q, original->response_code(),
             HeadersJson(*original));
    }
    std::vector<BrExtHeader> ops;
    ExtHeaderRules(*info, q, nullptr, &ops);
    if (ops.empty())
      return net::OK;
    auto changed =
        base::MakeRefCounted<net::HttpResponseHeaders>(original->raw_headers());
    for (const BrExtHeader& h : ops) {
      if (h.op == 'r')
        changed->RemoveHeader(h.name);
      else if (h.op == 's')
        changed->SetHeader(h.name, h.value);
      else
        changed->AddHeader(h.name, h.value);
    }
    *override_headers = std::move(changed);
    return net::OK;
  }

  void OnBeforeRedirect(net::URLRequest* request,
                        const GURL& new_location) override {
    BrExtRequestInfo* info = ExtInfo(request);
    if (info == nullptr || !ExtWebWanted(*info))
      return;
    base::DictValue d;
    d.Set("redirectUrl", new_location.spec());
    std::string more;
    base::JSONWriter::Write(d, &more);
    ExtWeb(*info, "onBeforeRedirect", ExtReqOf(request, *info),
           request->GetResponseCode(), more);
  }

  void OnResponseStarted(net::URLRequest* request, int net_error) override {
    BrExtRequestInfo* info = ExtInfo(request);
    if (info == nullptr || net_error != net::OK || !ExtWebWanted(*info))
      return;
    ExtWeb(*info, "onResponseStarted", ExtReqOf(request, *info),
           request->GetResponseCode(), std::string());
  }

  void OnCompleted(net::URLRequest* request,
                   bool started,
                   int net_error) override {
    BrExtRequestInfo* info = ExtInfo(request);
    if (info == nullptr || !ExtWebWanted(*info))
      return;
    BrExtReq q = ExtReqOf(request, *info);
    if (net_error == net::OK) {
      ExtWeb(*info, "onCompleted", q, request->GetResponseCode(),
             std::string());
      return;
    }
    base::DictValue d;
    d.Set("error", net::ErrorToString(net_error));
    std::string more;
    base::JSONWriter::Write(d, &more);
    ExtWeb(*info, "onErrorOccurred", q, 0, more);
  }
};

net::URLRequestContext* Context() {
  static net::URLRequestContext* ctx = [] {
    // The system's changes of the network told to what is built below
    br_net_watch_changes();
    // The system has 1024 sockets in all, the last 32 of them kept for the
    // kernel's own (its name lookups, DHCP), and a process may hold 512 (its
    // soft limit): the pool keeps well within them, so that a page asking
    // for many hosts at once waits for a socket instead of failing to get
    // one, and the QUIC sessions, the DNS client's questions and other
    // programs still have some. The cap is a hard one:
    // the engine's growing past it at random is turned off (br_blink.cc).
    // A socket the system has none left for comes back as ENFILE, which
    // the pool takes as ERR_INSUFFICIENT_RESOURCES.
    net::ClientSocketPoolManager::set_socket_soft_cap_per_pool_for_test(
        net::HttpNetworkSession::SocketPoolType::kNormal, 32);
    net::URLRequestContextBuilder builder;
    builder.set_network_delegate(std::make_unique<BrExtDelegate>());
    // A renderer with a network process builds one only for what has no
    // way there yet: it keeps nothing, so that the stores have the network
    // process as their one writer.
    const bool side = br_net_remote();
    if (side)
      printf("browser: net: a context of this renderer's own, nothing of it kept\n");
    // The jar, backed by the saved cookies.
    auto jar = std::make_unique<net::CookieMonster>(
        side ? nullptr : base::MakeRefCounted<BrCookieStore>(), nullptr);
    jar->SetPersistSessionCookies(true);
    g_cookie_jar = jar.get();
    builder.SetCookieStore(std::move(jar));
    // The cache, in memory and saved (see BrCacheBackendFactory). It is put
    // in place of the builder's own so that it can be read back into.
    builder.DisableHttpCache();
    if (!side)
    builder.SetWrapHttpNetworkLayerCallback(base::BindOnce(
        [](std::unique_ptr<net::HttpNetworkLayer> network_layer)
            -> std::unique_ptr<net::HttpTransactionFactory> {
          return std::make_unique<net::HttpCache>(
              std::move(network_layer),
              std::make_unique<BrCacheBackendFactory>(), nullptr);
        }));
    if (!side)
      g_cache_store = br_store_register_keyed(kCacheStoreName, &CollectCache,
                                              nullptr, 60000);
    // What earlier runs learnt of servers: their properties are handed to
    // the stack as it is built, their QUIC sessions once it is (below). The
    // store is saved ten seconds after something new came from the network.
    std::vector<SavedSession> sessions;
    if (br_net_keep_servers && !side) {
      base::DictValue dict;
      ReadServers(&dict, &sessions);
      const base::ListValue* servers = dict.FindList("servers");
      printf("browser: net: %u servers' properties read back\n",
             servers ? static_cast<unsigned>(servers->size()) : 0u);
      auto prefs = std::make_unique<BrServerPrefs>(std::move(dict));
      g_server_prefs = prefs.get();
      builder.SetHttpServerProperties(
          std::make_unique<net::HttpServerProperties>(std::move(prefs)));
      g_servers_store = br_store_register(kServersStoreName, &WriteServers,
                                          nullptr, 10000);
    }
    std::unique_ptr<net::HostResolver> resolver = MakeResolver();
    if (resolver)
      builder.set_host_resolver(std::move(resolver));
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
    // Brotli and Zstandard bodies are taken as a browser takes them, and
    // so asked for (Accept-Encoding: gzip, deflate, br, zstd)
    builder.set_enable_brotli(true);
    builder.set_enable_zstd(true);
    // HTTP/3 is used where a site advertises it (Alt-Svc); the first request
    // to a host always goes over TCP, and a QUIC connection that fails falls
    // back to it. br_net_quic set to 0 keeps everything on TCP.
    net::HttpNetworkSessionParams session_params;
    session_params.enable_quic = br_net_quic != 0;
    session_params.enable_http2 = br_net_h2 != 0;
    builder.set_http_network_session_params(session_params);
    // A QUIC connection stays on the socket it was opened with (the machine
    // has one network and its address does not move), is not given up as a
    // black hole (NBHD) while packets are slow to come, and its handshake is
    // given 15 seconds.
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
    if (!side)
      (new BrHstsKeeper())->Start(built->transport_security_state());
    printf("browser: net: context built, QUIC %s, %u versions\n",
            built->http_transaction_factory()->GetSession()->IsQuicEnabled()
                ? "on"
                : "off",
            static_cast<unsigned>(
                built->quic_context()->params()->supported_versions.size()));
    // Loading the jar is asked for now, as one job for every cookie, so
    // that it is complete before a page reads document.cookie.
    built->cookie_store()->GetAllCookiesAsync(base::DoNothing());
    if (br_net_keep_servers && !side && built->quic_context() != nullptr)
      RestoreSessions(built, &sessions);
    return built;
  }();
  return ctx;
}

constexpr int kReadChunk = 32 * 1024;

// A paced stream (br_net_fetch_paced) reads no further while this many
// bytes it handed on have not been taken: a download written as it comes
// holds no more than this of it in memory, however fast the network is.
constexpr size_t kPaceWindow = 512 * 1024;

// How many requests have been started and not yet reported. The load path
// reads this to know whether waiting a moment longer would bring in more of
// the page.
volatile unsigned g_outstanding = 0;

// The most requests that were under way at once since it was last asked.
volatile unsigned g_peak = 0;

// Responses that came from the network, by the protocol they came over:
// HTTP/3, HTTP/2, HTTP/1.1 and anything else (none named). Counted on the
// network thread, read once the program ends (br_net_protocols).
enum { kProtoH3, kProtoH2, kProtoH1, kProtoOther, kProtoKinds };
volatile unsigned g_by_protocol[kProtoKinds];

void CountProtocol(const std::string& alpn) {
  const int k = alpn == "h3"         ? kProtoH3
                : alpn == "h2"       ? kProtoH2
                : alpn == "http/1.1" ? kProtoH1
                                     : kProtoOther;
  __sync_add_and_fetch(&g_by_protocol[k], 1);
}

// The last requests that failed, with the network stack's error, so that
// the page that could not be fetched says why as Chrome's error page does
// (br_net_fetch_error).
std::mutex g_failed_lock;
std::deque<std::pair<std::string, int>> g_failed;

void NoteFailure(const std::string& url, int error) {
  std::lock_guard<std::mutex> hold(g_failed_lock);
  g_failed.emplace_back(url, error);
  while (g_failed.size() > 16)
    g_failed.pop_front();
}

// And the protocol the last answers for an address came over (h3, h2,
// http/1.1), which the document is told of (br_net_fetch_protocol).
std::deque<std::pair<std::string, std::string>> g_protocols;

void NoteProtocol(const std::string& url, const std::string& protocol) {
  std::lock_guard<std::mutex> hold(g_failed_lock);
  g_protocols.emplace_back(url, protocol);
  while (g_protocols.size() > 16)
    g_protocols.pop_front();
}

void OutstandingUp() {
  const unsigned n = __sync_add_and_fetch(&g_outstanding, 1);
  for (;;) {
    const unsigned p = g_peak;
    if (n <= p || __sync_bool_compare_and_swap(&g_peak, p, n))
      break;
  }
}

// The reason phrase of the response being handed over, while its callback
// runs on this thread (br_net_fetch_status_text)
thread_local const std::string* g_status_text = nullptr;

// And the protocol it came over (its ALPN: "h2", "h3", "http/1.1", or
// "unknown" for HTTP/1.1 in the clear)
thread_local const std::string* g_protocol = nullptr;

// The redirects of the response being handed over, while its callback runs
// on this thread (br_net_fetch_redirects)
thread_local const std::vector<BrRedirect>* g_redirects = nullptr;

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
  // task, which would otherwise wait for the next turn and the sockets'
  // poll before it. Bounded by a count and by time, so that a task which keeps
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

  // How long until the earliest task is due, in milliseconds rounded up
  // (0 when one is due now), -1 when there is none.
  long DueInMs() {
    int64_t when;
    {
      base::AutoLock hold(lock_);
      if (queue_.empty())
        return -1;
      when = queue_.begin()->first.first;
    }
    const int64_t now = base::TimeTicks::Now().since_origin().InMicroseconds();
    if (when <= now)
      return 0;
    return static_cast<long>((when - now + 999) / 1000);
  }

 private:
  ~NetRunner() override = default;

  base::Lock lock_;
  std::map<std::pair<int64_t, uint64_t>, base::OnceClosure> queue_;
  uint64_t seq_ = 0;
};

NetRunner* TheNetRunner();

// The page's thread runner, taken when the network thread starts. An answer for
// the page goes back through it; one for another thread (a dedicated worker's)
// goes to that thread's runner.
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
  origin->PostTask(FROM_HERE, std::move(closure));
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
  // kept per thread), and so is a sequence's local storage: what a task
  // keeps per sequence (the dispatch of Mojo's messages, for the
  // services that are answered here) has its map.
  static auto* current =
      new base::SingleThreadTaskRunner::CurrentHandleOverrideForTesting(
          TheNetRunner());
  static auto* storage =
      new base::internal::ScopedSetSequenceLocalStorageMapForCurrentThread(
          new base::internal::SequenceLocalStorageMap());
  (void)current;
  (void)storage;
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
    // the stack is the network process's: what is asked of it goes there
    if (!br_net_remote())
      Context();
  }
  if (!MainRunner()) {
    printf("browser: net task: no page runner to answer through; not started\n");
    return;
  }
  br_net_thread_start(br_net_remote() ? 0 : 1);
}

// How long the network thread may wait before the stack has a timer due.
extern "C" long br_net_due_in_ms(void) {
  return TheNetRunner()->DueInMs();
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

// The words of the start-up line "netlog=": the fetches whose bodies are
// written to the console (Fetch::NetLogged)
std::string br_netlog;

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

  // The body handed on piece by piece through |part| (br_net_fetch_stream)
  // instead of whole at the end.
  void SetStream(BrNetFetchPart part) { part_ = part; }

  // A stream whose asker tells when it has taken what was handed on
  // (Taken): reading waits while kPaceWindow bytes are not yet taken.
  void SetPaced() { paced_ = true; }

  // |n| bytes of what was handed on taken by the asker: reading goes on
  // once the bytes not yet taken are below the window again.
  void Taken(unsigned n) {
    untaken_ = n < untaken_ ? untaken_ - n : 0;
    if (!held_ || finished_ || !request_ || untaken_ >= kPaceWindow)
      return;
    held_ = false;
    ReadMore();
  }

  // Whether this is the stream asked for with |ctx|.
  bool IsStreamOf(void* ctx) const {
    return part_ != nullptr && ctx_ == ctx && !finished_;
  }

  // The asker went away: the connection is let go and the end told (and
  // dropped by the asker's side).
  void Cancel() {
    if (finished_)
      return;
    request_.reset();
    error_ = net::ERR_ABORTED;
    Finish(false, status_);
  }

  // The asker of |ctx| has gone (a renderer that ended, in the network
  // process): it is told the end now and nothing later. The request goes
  // with it unless another asker waits for the same answer.
  bool Drop(void* ctx) {
    if (finished_)
      return false;
    auto tell = [](BrNetFetchDone done, void* ctx, bool counted) {
      if (counted)
        OutstandingDown();
      done(ctx, 0, 0, "", "", "", "", 0);
    };
    if (ctx_ == ctx && done_ != nullptr) {
      if (waiters_.empty()) {
        Cancel();
        return true;
      }
      PostToOrigin(origin_, base::BindOnce(tell, done_, ctx_, counted_));
      done_ = nullptr;
      part_ = nullptr;
      counted_ = false;
      return true;
    }
    for (auto it = waiters_.begin(); it != waiters_.end(); ++it) {
      if (it->ctx == ctx) {
        PostToOrigin(it->origin, base::BindOnce(tell, it->done, it->ctx, true));
        waiters_.erase(it);
        return true;
      }
    }
    return false;
  }

  // The key this request can be shared under, empty when it cannot be:
  // only a plain GET with no body and no headers of its own, since a range
  // or a conditional asks for something else.
  void SetShareKey(const std::string& key) {
    share_key_ = key;
    if (!key.empty())
      SharableFetches()[key] = this;
  }

  // The fetches whose URL has one of the words of the start-up line
  // "netlog=" (separated by commas) have their bodies written to the
  // console, the request's and the first of the response's: for finding
  // out what a site says to its servers.
  static bool NetLogged(const std::string& url) {
    size_t at = 0;
    while (!br_netlog.empty() && at <= br_netlog.size()) {
      size_t comma = br_netlog.find(',', at);
      const std::string word = br_netlog.substr(
          at, comma == std::string::npos ? std::string::npos : comma - at);
      if (!word.empty() && url.find(word) != std::string::npos)
        return true;
      if (comma == std::string::npos)
        break;
      at = comma + 1;
    }
    return false;
  }

  static void NetLogBytes(const char* what, const std::string& url,
                          const char* p, size_t n, size_t total) {
    size_t shown = n < 2048 ? n : 2048, printable = 0;
    for (size_t i = 0; i < shown; i++) {
      const unsigned char c = (unsigned char)p[i];
      if ((c >= 0x20 && c < 0x7f) || c == '\n' || c == '\r' || c == '\t')
        printable++;
    }
    // text is shown up to 16K, other bytes up to 2K (in hex)
    const bool text = printable * 10 >= shown * 9;
    if (text)
      shown = n < 16384 ? n : 16384;
    printf("browser: netlog: %s %.120s: %zu bytes%s\n", what, url.c_str(),
           total, text ? " (text)" : " (hex)");
    for (size_t at = 0; at < shown; at += 96) {
      std::string line;
      for (size_t i = at; i < shown && i < at + 96; i++) {
        const unsigned char c = (unsigned char)p[i];
        if (text) {
          line += (c >= 0x20 && c < 0x7f) ? (char)c : '.';
        } else {
          char h[4];
          snprintf(h, sizeof(h), "%02x", c);
          line += h;
        }
      }
      printf("browser: netlog:   %s\n", line.c_str());
    }
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
    // An address of an extension's has no network under it: a page going to
    // it, and what a rule redirected there, are answered from the extension.
    if (gurl.SchemeIs("chrome-extension")) {
      net::HttpRequestHeaders lines;
      ParseHeaderLines(req_headers, &lines);
      ReadHooks(lines);
      std::string initiator = lines.GetHeader("X-TessronOS-Initiator").value_or(std::string());
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, base::BindOnce(&Fetch::AnswerFromExtension,
                                    base::Unretained(this), url,
                                    initiator == "none" ? std::string() : initiator));
      started_ = base::TimeTicks::Now();
      return true;
    }
    // A port a browser does not let a page reach is refused at once, as the
    // network service refuses it (net::IsPortAllowedForScheme)
    if (!net::IsPortAllowedForScheme(gurl.EffectiveIntPort(),
                                     gurl.scheme())) {
      printf("browser: net fetch: %s refused (a port not allowed)\n",
             url.c_str());
      return false;
    }
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
    ReadHooks(headers);
    headers.RemoveHeader("X-TessronOS-Hooks");
    // What the extensions' rules are told of it: "<type> <origin>" (a line
    // the stack takes off)
    if (std::optional<std::string> res =
            headers.GetHeader("X-TessronOS-Resource")) {
      auto info = std::make_unique<BrExtRequestInfo>();
      const size_t sp = res->find(' ');
      info->type = res->substr(0, sp);
      if (sp != std::string::npos)
        info->initiator = res->substr(sp + 1);
      info->owner = hooks_owner_;
      info->has = hooks_has_;
      ext_initiator_ = info->initiator;
      request_->SetUserData(kExtInfoKey, std::move(info));
      headers.RemoveHeader("X-TessronOS-Resource");
    }
    // A window's navigation (the browser's line): its site for cookies
    // follows its redirects, and it counts as a main frame's navigation,
    // as a browser's does. Otherwise a navigation redirected to another
    // site stays a cross-site request of the first address's site, whose
    // cookies of SameSite Lax (the default) are neither sent nor stored:
    // a sign-in page reached by redirects lost the cookie its first
    // response set, and its requests were refused (HTTP 400). The page
    // that asked for it is its initiator ("none" when the user did).
    if (std::optional<std::string> nav =
            headers.GetHeader("X-TessronOS-Navigation")) {
      if (*nav == "main") {
        request_->set_first_party_url_policy(
            net::RedirectInfo::FirstPartyURLPolicy::UPDATE_URL_ON_REDIRECT);
        request_->set_force_main_frame_for_same_site_cookies(true);
      }
      headers.RemoveHeader("X-TessronOS-Navigation");
    }
    if (std::optional<std::string> ini =
            headers.GetHeader("X-TessronOS-Initiator")) {
      if (*ini == "none") {
        request_->set_initiator(std::nullopt);
      } else {
        GURL from(*ini);
        if (from.is_valid())
          request_->set_initiator(url::Origin::Create(from));
      }
      headers.RemoveHeader("X-TessronOS-Initiator");
    }
    // The referrer goes to the request itself, not among the headers: the
    // HTTP job drops a Referer it finds there and sends the request's own
    // referrer, trimmed again on a redirect by the request's referrer
    // policy (the loader's line; Blink's default, strict-origin-when-
    // cross-origin, when it gives none)
    if (std::optional<std::string> ref = headers.GetHeader("Referer")) {
      request_->SetReferrer(*ref);
      headers.RemoveHeader("Referer");
    }
    {
      net::ReferrerPolicy policy =
          net::ReferrerPolicy::REDUCE_GRANULARITY_ON_TRANSITION_CROSS_ORIGIN;
      if (std::optional<std::string> rp =
              headers.GetHeader("X-TessronOS-Referrer-Policy")) {
        int v = 0;
        if (base::StringToInt(*rp, &v) && v >= 0 &&
            v <= static_cast<int>(net::ReferrerPolicy::MAX))
          policy = static_cast<net::ReferrerPolicy>(v);
        headers.RemoveHeader("X-TessronOS-Referrer-Policy");
      }
      request_->set_referrer_policy(policy);
    }
    // The loader's own lines, not sent: whether the request goes without
    // credentials (no cookies, no authentication; a CORS request whose
    // credentials mode leaves them out), and its priority as Blink set it
    // (with " i" when incremental), which HTTP/2 and HTTP/3 say to the
    // server in their priority signals
    if (std::optional<std::string> c =
            headers.GetHeader("X-TessronOS-Credentials")) {
      if (*c == "omit")
        request_->set_disallow_credentials();
      headers.RemoveHeader("X-TessronOS-Credentials");
    }
    // A request that does not follow redirects (its redirect mode "manual"
    // or "error"): it ends at the first one (OnReceivedRedirect)
    if (std::optional<std::string> rd =
            headers.GetHeader("X-TessronOS-Redirect")) {
      stop_at_redirect_ = *rd == "stop";
      headers.RemoveHeader("X-TessronOS-Redirect");
    }
    // How the request uses the HTTP cache (a fetch()'s cache mode: validate,
    // bypass, skip validation, only from the cache, not at all)
    if (std::optional<std::string> lf =
            headers.GetHeader("X-TessronOS-Load-Flags")) {
      int v = 0;
      constexpr int kCacheFlags =
          net::LOAD_VALIDATE_CACHE | net::LOAD_BYPASS_CACHE |
          net::LOAD_SKIP_CACHE_VALIDATION | net::LOAD_ONLY_FROM_CACHE |
          net::LOAD_DISABLE_CACHE;
      if (base::StringToInt(*lf, &v))
        request_->SetLoadFlags(request_->load_flags() | (v & kCacheFlags));
      headers.RemoveHeader("X-TessronOS-Load-Flags");
    }
    if (std::optional<std::string> pr =
            headers.GetHeader("X-TessronOS-Priority")) {
      int v = 0;
      std::string_view num(*pr);
      bool incremental = false;
      if (num.size() > 2 && num.substr(num.size() - 2) == " i") {
        incremental = true;
        num.remove_suffix(2);
      }
      if (base::StringToInt(num, &v) && v >= net::MINIMUM_PRIORITY &&
          v <= net::MAXIMUM_PRIORITY) {
        request_->SetPriority(static_cast<net::RequestPriority>(v));
        request_->SetPriorityIncremental(incremental);
      }
      headers.RemoveHeader("X-TessronOS-Priority");
    }
    // The client hints every request to a secure origin carries
    // (Sec-CH-UA, -Mobile, -Platform), unless the page set its own
    if (gurl.SchemeIsCryptographic()) {
      net::HttpRequestHeaders hints;
      ParseHeaderLines(br_client_hint_headers(), &hints);
      for (const auto& h : hints.GetHeaderVector())
        headers.SetHeaderIfMissing(h.key, h.value);
    }
    request_->SetExtraRequestHeaders(headers);

    logged_ = NetLogged(url);
    if (logged_) {
      printf("browser: netlog: headers %s\n", headers.ToString().c_str());
      NetLogBytes(method.c_str(), url, body.data(), body.size(), body.size());
    }

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
  // A redirect is followed by the stack; the asker is told of each one with
  // the response (the page's loader tells Blink, which may stop there)
  void OnReceivedRedirect(net::URLRequest* request,
                          const net::RedirectInfo& info,
                          bool* defer_redirect) override {
    BrRedirect r;
    r.url = request->url().spec();
    r.status = info.status_code;
    if (const net::HttpResponseHeaders* h = request->response_headers()) {
      size_t at = 0;
      std::string name, value;
      while (h->EnumerateHeaderLines(&at, &name, &value))
        r.headers += name + ": " + value + "\r\n";
    }
    r.new_url = info.new_url.spec();
    r.new_method = info.new_method;
    r.new_referrer = info.new_referrer;
    std::string redirect_headers = r.headers;
    redirects_.push_back(std::move(r));
    // To an extension's file: the stack has no such scheme, the extension
    // answers, and the asker sees a redirect that ended there.
    if (!stop_at_redirect_ && info.new_url.SchemeIs("chrome-extension")) {
      *defer_redirect = true;
      AnswerFromExtension(info.new_url.spec(), ext_initiator_);
      return;
    }
    if (!stop_at_redirect_)
      return;
    // A request whose redirect mode is "manual" or "error" ends at its first
    // redirect, which is not followed: the asker gets that response (the
    // CORS check is made on it, and the page sees it as an opaque redirect,
    // or the load fails), and what it leads to is never asked for.
    *defer_redirect = true;
    status_ = info.status_code;
    final_url_ = request->url().spec();
    protocol_ = request->response_info().alpn_negotiated_protocol;
    headers_ = redirect_headers;
    if (const net::HttpResponseHeaders* h = request->response_headers()) {
      ctype_ = h->GetNormalizedHeader("content-type").value_or(std::string());
      status_text_ = h->GetStatusText();
    }
    PostFirstPart();
    Finish(true, status_);
  }

  // The renderer that asked, in the network process ("X-TessronOS-Hooks:
  // <its number> <what it has>", a line the network process puts)
  void ReadHooks(const net::HttpRequestHeaders& lines) {
    std::optional<std::string> h = lines.GetHeader("X-TessronOS-Hooks");
    if (!h)
      return;
    if (sscanf(h->c_str(), "%d %d", &hooks_owner_, &hooks_has_) != 2)
      hooks_owner_ = hooks_has_ = 0;
  }

  // The answer to an address of an extension's
  void AnswerFromExtension(const std::string& url,
                           const std::string& initiator) {
    BrExtReq q;
    BrExtAns ans;
    q.url = url;
    q.initiator = initiator;
    final_url_ = url;
    int act = BR_EXT_PASS;
    if (hooks_owner_ != 0) {
      if (!br_net_hook_serve(hooks_owner_, q, &act, &ans))
        act = BR_EXT_PASS;
    } else {
      act = br_ext_serve(q, &ans);
    }
    if (act != BR_EXT_SERVED) {
      error_ = net::ERR_BLOCKED_BY_CLIENT;
      Finish(false, 0);
      return;
    }
    status_ = ans.status;
    ctype_ = ans.ctype;
    status_text_ = "OK";
    headers_ = "Access-Control-Allow-Origin: *\r\nContent-Type: " + ans.ctype + "\r\n" + ans.headers;
    PostFirstPart();
    if (part_ != nullptr) {
      PostToOrigin(origin_,
                   base::BindOnce(
                       [](BrNetFetchPart part, void* ctx, std::string piece) {
                         part(ctx, 0, 0, "", "", "", piece.data(),
                              (unsigned)piece.size());
                       },
                       part_, ctx_, ans.body));
    } else {
      data_.assign(ans.body.begin(), ans.body.end());
    }
    Finish(true, status_);
  }

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
      status_text_ = h->GetStatusText();
    }
    PostFirstPart();
    buf_ = base::MakeRefCounted<net::IOBufferWithSize>(kReadChunk);
    ReadMore();
  }

  void OnReadCompleted(net::URLRequest* request, int bytes_read) override {
    if (!Consume(bytes_read))
      return;
    ReadMore();
  }

 private:
  // A stream's response, handed to the asker before its body
  void PostFirstPart() {
    if (part_ != nullptr) {
      // A stream: the response now, and it no longer holds the page's load.
      PostToOrigin(origin_,
                   base::BindOnce(
                       [](BrNetFetchPart part, void* ctx, int status,
                          std::string url, std::string headers,
                          std::string ctype, std::string status_text,
                          std::string protocol,
                          std::vector<BrRedirect> redirects) {
                         OutstandingDown();
                         g_status_text = &status_text;
                         g_protocol = &protocol;
                         g_redirects = &redirects;
                         part(ctx, 1, status, url.c_str(), headers.c_str(),
                              ctype.c_str(), "", 0);
                         g_status_text = nullptr;
                         g_protocol = nullptr;
                         g_redirects = nullptr;
                       },
                       part_, ctx_, status_, final_url_, headers_, ctype_,
                       status_text_, protocol_, redirects_));
      counted_ = false;
    }
  }

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
      // A paced stream with its window full: read again when the asker
      // has taken some (Taken)
      if (paced_ && part_ != nullptr && untaken_ >= kPaceWindow) {
        held_ = true;
        return;
      }
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
    if (logged_ && logbuf_.size() < 16384)
      logbuf_.append(buf_->data(), buf_->data() + bytes_read);
    logtotal_ += (size_t)bytes_read;
    if (part_ != nullptr) {
      if (paced_)
        untaken_ += (size_t)bytes_read;
      PostToOrigin(origin_,
                   base::BindOnce(
                       [](BrNetFetchPart part, void* ctx, std::string piece) {
                         part(ctx, 0, 0, "", "", "", piece.data(),
                              (unsigned)piece.size());
                       },
                       part_, ctx_,
                       std::string(buf_->data(), buf_->data() + bytes_read)));
      return true;
    }
    data_.insert(data_.end(), buf_->data(), buf_->data() + bytes_read);
    return true;
  }

  void Finish(bool ok, int status) {
    if (finished_)
      return;
    finished_ = true;
    if (logged_) {
      char what[32];
      snprintf(what, sizeof(what), "answer %d", status);
      NetLogBytes(what, url_, logbuf_.data(), logbuf_.size(), logtotal_);
    }
    if (!ok) {
      printf("browser: net fetch: %s failed (%d)\n", url_.c_str(), error_);
      NoteFailure(url_, error_);
    }
    // Something new came from the network: the cache is saved a while later.
    // So may what is known of its server be, and a server that answered over
    // HTTP/3 has a session ticket worth keeping.
    if (ok)
      NoteProtocol(url_, protocol_);
    if (ok && request_ && !request_->was_cached()) {
      br_store_dirty(g_cache_store);
      br_store_dirty(g_servers_store);
      CountProtocol(protocol_);
      NoteFirstProtocol(protocol_);
      if (protocol_ == "h3") {
        const GURL& u = request_->url();
        QuicServers().insert(
            {std::string(u.host()),
             static_cast<uint16_t>(u.EffectiveIntPort())});
      }
    }
    printf("browser: net fetch: %s over '%s'\n", url_.c_str(),
            protocol_.empty() ? "(none)" : protocol_.c_str());
    // The answer goes back on the asker's runner: the page's code then runs
    // as the page's, not inside the stack's turn with the network runner
    // current. It counts as outstanding until it has been handed over, so
    // a caller waiting for "nothing outstanding" also has the answers.
    if (final_url_.empty())
      final_url_ = request_ ? request_->url().spec() : url_;
    auto deliver = [](BrNetFetchDone done, void* ctx, bool counted, int ok,
                      int status, std::string url, std::string headers,
                      std::string ctype, std::string status_text,
                      std::string protocol,
                      std::vector<BrRedirect> redirects,
                      std::vector<char> data) {
      if (counted)
        OutstandingDown();
      if (done != nullptr) {
        g_status_text = &status_text;
        g_protocol = &protocol;
        g_redirects = &redirects;
        done(ctx, ok, status, url.c_str(), headers.c_str(), ctype.c_str(),
             data.empty() ? "" : data.data(), (unsigned)data.size());
        g_status_text = nullptr;
        g_protocol = nullptr;
        g_redirects = nullptr;
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
                   base::BindOnce(deliver, w.done, w.ctx, true, ok ? 1 : 0,
                                  status, final_url_, headers_, ctype_,
                                  status_text_, protocol_, redirects_, data_));
    }
    waiters_.clear();
    PostToOrigin(origin_,
                 base::BindOnce(deliver, done_, ctx_, counted_, ok ? 1 : 0,
                                status, final_url_, headers_, ctype_,
                                status_text_, protocol_, redirects_,
                                std::move(data_)));
    // Not here: the stack is still inside this request's own machinery.
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE, this);
  }

  struct Waiter {
    BrNetFetchDone done;
    void* ctx;
    scoped_refptr<base::SingleThreadTaskRunner> origin;
  };

  BrNetFetchDone done_;
  BrNetFetchPart part_ = nullptr;
  void* ctx_;
  scoped_refptr<base::SingleThreadTaskRunner> origin_;
  std::vector<Waiter> waiters_;
  bool counted_ = true;  // still among the outstanding requests
  bool paced_ = false;   // reading waits for the asker (SetPaced)
  bool held_ = false;    // and is waiting now
  size_t untaken_ = 0;   // bytes handed on and not yet taken
  std::string share_key_;
  std::unique_ptr<net::URLRequest> request_;
  scoped_refptr<net::IOBufferWithSize> buf_;
  std::string url_;
  std::string final_url_;
  std::string protocol_;
  std::string body_;
  std::string headers_;
  std::string ctype_;
  std::string status_text_;  // the reason phrase of the status line
  std::vector<BrRedirect> redirects_;  // the redirects followed, in order
  std::vector<char> data_;
  int status_ = 0;
  bool stop_at_redirect_ = false;  // ends at its first redirect (manual, error)
  std::string ext_initiator_;  // the origin that asked, as the rules were told
  int hooks_owner_ = 0;  // the renderer that asked, in the network process
  int hooks_has_ = 0;
  bool logged_ = false;		// its bodies go to the console (netlog=)
  std::string logbuf_;
  size_t logtotal_ = 0;
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
               scoped_refptr<base::SingleThreadTaskRunner> origin,
               BrNetFetchPart part = nullptr,
               bool paced = false) {
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
           low.find("authorization:") != std::string::npos ||
           low.find("x-tessronos-redirect:") != std::string::npos ||
           low.find("x-tessronos-navigation:") != std::string::npos;
  };
  const bool sharable = body_len == 0 &&
                        method == "GET" &&
                        part == nullptr &&
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
  if (part != nullptr)
    f->SetStream(part);
  if (part != nullptr && paced)
    f->SetPaced();
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
// Kept per thread: a worker's thread starts fetches of its own meanwhile.
static thread_local int br_net_fetch_answer_on_main = 0;

extern "C" void br_net_fetch_set_answer_on_main(int on) {
  br_net_fetch_answer_on_main = on;
}

static int FetchStart(const char* url,
                      const char* method,
                      const void* body,
                      unsigned body_len,
                      const char* req_headers,
                      BrNetFetchPart part,
                      BrNetFetchDone done,
                      void* ctx,
                      bool paced = false,
                      bool off_thread = false);

extern "C" int br_net_fetch_start(const char* url,
                                  const char* method,
                                  const void* body,
                                  unsigned body_len,
                                  const char* req_headers,
                                  BrNetFetchDone done,
                                  void* ctx) {
  return FetchStart(url, method, body, body_len, req_headers, nullptr, done,
                    ctx);
}

extern "C" int br_net_fetch_stream(const char* url,
                                   const char* method,
                                   const void* body,
                                   unsigned body_len,
                                   const char* req_headers,
                                   BrNetFetchPart part,
                                   BrNetFetchDone done,
                                   void* ctx) {
  if (part == nullptr)
    return 0;
  return FetchStart(url, method, body, body_len, req_headers, part, done, ctx);
}

extern "C" int br_net_fetch_paced(const char* url,
                                  const char* method,
                                  const void* body,
                                  unsigned body_len,
                                  const char* req_headers,
                                  BrNetFetchPart part,
                                  BrNetFetchDone done,
                                  void* ctx,
                                  int off_thread) {
  if (part == nullptr)
    return 0;
  return FetchStart(url, method, body, body_len, req_headers, part, done, ctx,
                    true, off_thread != 0);
}

extern "C" void br_net_fetch_taken(void* ctx, unsigned n) {
  // A renderer's stream is paced by the network process, by the answers
  // to its pieces, which the renderer gives as they come
  if (br_net_remote() || n == 0)
    return;
  TheNetRunner()->PostTask(FROM_HERE, base::BindOnce(
                                          [](void* ctx, unsigned n) {
                                            BrNetScope scope;
                                            for (Fetch* f : LiveFetches()) {
                                              if (f->IsStreamOf(ctx)) {
                                                f->Taken(n);
                                                break;
                                              }
                                            }
                                          },
                                          ctx, n));
}

extern "C" int br_net_fetch_remote(void) {
  return br_net_remote() ? 1 : 0;
}

extern "C" void br_net_fetch_cancel(void* ctx) {
  if (br_net_remote())
    br_net_remote_cancel(ctx);
  TheNetRunner()->PostTask(FROM_HERE, base::BindOnce(
                                          [](void* ctx) {
                                            BrNetScope scope;
                                            for (Fetch* f : LiveFetches()) {
                                              if (f->IsStreamOf(ctx)) {
                                                f->Cancel();
                                                break;
                                              }
                                            }
                                          },
                                          ctx));
}

void br_net_fetch_drop(void* ctx) {
  TheNetRunner()->PostTask(FROM_HERE, base::BindOnce(
                                          [](void* ctx) {
                                            BrNetScope scope;
                                            for (Fetch* f : LiveFetches()) {
                                              if (f->Drop(ctx))
                                                break;
                                            }
                                          },
                                          ctx));
}

static int FetchStart(const char* url,
                      const char* method,
                      const void* body,
                      unsigned body_len,
                      const char* req_headers,
                      BrNetFetchPart part,
                      BrNetFetchDone done,
                      void* ctx,
                      bool paced,
                      bool off_thread) {
  if (url == nullptr || done == nullptr)
    return 0;
  scoped_refptr<base::SingleThreadTaskRunner> origin =
      base::SingleThreadTaskRunner::HasCurrentDefault()
          ? base::SingleThreadTaskRunner::GetCurrentDefault()
          : nullptr;
  if (br_net_fetch_answer_on_main && MainRunner())
    origin = MainRunner();
  // An asker that waits for the answer on its own thread: it is told on
  // the network's thread (the answers' thread of a renderer)
  const bool remote = br_net_remote() && strncmp(url, "chrome-extension:", 17) != 0;
  if (off_thread)
    origin = remote ? br_net_remote_answers() : scoped_refptr<base::SingleThreadTaskRunner>(TheNetRunner());
  // An extension's address is answered by this process's extensions
  if (remote)
    return br_net_remote_fetch(url, method, body, body_len, req_headers, part,
                               done, ctx, std::move(origin));
  std::string url_str(url);
  std::string method_str(method != nullptr ? method : "GET");
  std::string body_str(static_cast<const char*>(body ? body : ""), body_len);
  std::string headers_str(req_headers != nullptr ? req_headers : "");
  OutstandingUp();
  if (!br_net_thread_up() || br_on_net_thread()) {
    if (!StartOnNet(url_str, method_str, body_str, headers_str, done, ctx,
                    std::move(origin), part, paced)) {
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
             scoped_refptr<base::SingleThreadTaskRunner> origin,
             BrNetFetchPart part, bool paced) {
            if (StartOnNet(url, method, body, headers, done, ctx, origin,
                           part, paced))
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
          std::move(headers_str), done, ctx, std::move(origin), part, paced));
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
  std::string status_text;  // the reason phrase of the status line
  std::vector<char> data;
};

// The reason phrase of the answer the last load waited for on this thread
// had (br_net_fetch_sync_status_text).
thread_local std::string g_sync_status_text;

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
  // told while the answer is handed over (on the network thread)
  f->status_text = g_status_text != nullptr ? *g_status_text : "";
  f->data.assign(data, data + len);
  br_sem_post(f->sem);
}

// Set while the waits are to give up (br_net_fetch_cancel_sync).
std::atomic<int> g_sync_cancel{0};
// The same for the waits for the network process
volatile int g_sync_cancel_flag = 0;
}  // namespace

extern "C" void br_net_fetch_cancel_sync(int on) {
  g_sync_cancel = on;
  g_sync_cancel_flag = on;
}

extern "C" int br_net_fetch_cancel_asked(void) {
  return g_sync_cancel.load();
}

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
  if (br_net_remote() && url != nullptr &&
      strncmp(url, "chrome-extension:", 17) != 0) {
    BrNetAnswer a;
    std::string data;
    const int ok = br_net_remote_fetch_sync(url, method, body, body_len,
                                            req_headers, &a, &data, timeout_ms,
                                            &g_sync_cancel_flag);
    if (ok < 0)
      return 0;
    g_sync_status_text = a.status_text;
    if (status != nullptr)
      *status = a.status;
    if (headers != nullptr)
      *headers = strdup(a.headers.c_str());
    if (final_url != nullptr && url_cap > 0) {
      const size_t n = std::min<size_t>(a.url.size(), url_cap - 1);
      memcpy(final_url, a.url.data(), n);
      final_url[n] = 0;
    }
    if (ctype != nullptr && ctype_cap > 0) {
      const size_t n = std::min<size_t>(a.ctype.size(), ctype_cap - 1);
      memcpy(ctype, a.ctype.data(), n);
      ctype[n] = 0;
    }
    if (ok && buf != nullptr) {
      *buf = static_cast<char*>(malloc(data.size() + 1));
      if (*buf != nullptr) {
        memcpy(*buf, data.data(), data.size());
        (*buf)[data.size()] = 0;
        if (len != nullptr)
          *len = (unsigned)data.size();
      }
    }
    return ok;
  }
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
  // Waited in slices, so that 中止 and closing (br_net_fetch_cancel_sync)
  // end the wait within one.
  bool in = false;
  for (int waited = 0; waited < timeout_ms && !in;) {
    if (g_sync_cancel.load()) {
      printf("browser: net fetch: %s given up (stopped)\n", url);
      f->abandoned = 1;
      return 0;
    }
    const int slice = std::min(100, timeout_ms - waited);
    in = br_sem_wait_ms(f->sem, slice);
    waited += slice;
  }
  if (!in) {
    printf("browser: net fetch: %s not in after %d ms\n", url, timeout_ms);
    f->abandoned = 1;
    return 0;
  }
  const int ok = f->ok;
  g_sync_status_text = f->status_text;
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

extern "C" int br_net_fetch_error(const char* url, char* name, unsigned cap) {
  int error = 0;
  {
    std::lock_guard<std::mutex> hold(g_failed_lock);
    for (auto it = g_failed.rbegin(); it != g_failed.rend(); ++it) {
      if (url != nullptr && it->first == url) {
        error = it->second;
        break;
      }
    }
  }
  if (name != nullptr && cap > 0) {
    const std::string s = error != 0 ? net::ErrorToShortString(error) : "";
    const size_t n = std::min<size_t>(s.size(), cap - 1);
    memcpy(name, s.data(), n);
    name[n] = 0;
  }
  return error;
}

extern "C" int br_net_fetch_protocol(const char* url, char* name, unsigned cap) {
  std::string p;
  bool found = false;
  {
    std::lock_guard<std::mutex> hold(g_failed_lock);
    for (auto it = g_protocols.rbegin(); it != g_protocols.rend(); ++it) {
      if (url != nullptr && it->first == url) {
        p = it->second;
        found = true;
        break;
      }
    }
  }
  if (name != nullptr && cap > 0) {
    const size_t n = std::min<size_t>(p.size(), cap - 1);
    memcpy(name, p.data(), n);
    name[n] = 0;
  }
  return found ? 1 : 0;
}

extern "C" unsigned br_net_fetch_outstanding(void) {
  return g_outstanding;
}

extern "C" int br_net_fetch_redirects(void) {
  return g_redirects != nullptr ? static_cast<int>(g_redirects->size()) : -1;
}

extern "C" int br_net_fetch_redirect(int i, const char** url, int* status,
                                     const char** headers, const char** new_url,
                                     const char** new_method,
                                     const char** new_referrer) {
  if (g_redirects == nullptr || i < 0 ||
      static_cast<size_t>(i) >= g_redirects->size())
    return 0;
  const BrRedirect& r = (*g_redirects)[static_cast<size_t>(i)];
  *url = r.url.c_str();
  *status = r.status;
  *headers = r.headers.c_str();
  *new_url = r.new_url.c_str();
  *new_method = r.new_method.c_str();
  *new_referrer = r.new_referrer.c_str();
  return 1;
}

extern "C" const char* br_net_fetch_answer_protocol(void) {
  return g_protocol != nullptr ? g_protocol->c_str() : nullptr;
}

extern "C" const char* br_net_fetch_sync_status_text(void) {
  return g_sync_status_text.c_str();
}

extern "C" const char* br_net_fetch_status_text(void) {
  return g_status_text != nullptr ? g_status_text->c_str() : nullptr;
}

extern "C" unsigned br_net_fetch_peak(void) {
  return __sync_lock_test_and_set(&g_peak, g_outstanding);
}

extern "C" int br_net_protocols(char* out, unsigned cap) {
  if (br_net_remote())
    return br_net_remote_protocols(out, cap);
  return snprintf(out, cap, "h3=%u h2=%u http/1.1=%u other=%u first=%s\n",
                  g_by_protocol[kProtoH3], g_by_protocol[kProtoH2],
                  g_by_protocol[kProtoH1], g_by_protocol[kProtoOther],
                  g_first_taken ? g_first_protocol : "none");
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

// ---- the stack for the other parts of the port ----------------------------------

// The context every request goes through; only for use on the network thread.
// A test's: a host's rule added as a response's header would, and whether one is known.
void br_net_hsts_add(const std::string& host) {
  if (br_net_remote()) {
    br_net_remote_hsts_add(host);
    return;
  }
  RunOnNetAndWait(base::BindOnce(
      [](std::string host) {
        Context()->transport_security_state()->AddHSTS(
            host, base::Time::Now() + base::Days(365), false);
      },
      host));
}

bool br_net_hsts_known(const std::string& host) {
  if (br_net_remote())
    return br_net_remote_hsts_known(host);
  bool yes = false;
  RunOnNetAndWait(base::BindOnce(
      [](std::string host, bool* yes) {
        net::TransportSecurityState::STSState st;
        *yes = Context()->transport_security_state()->GetDynamicSTSState(host, &st) &&
               st.ShouldUpgradeToSSL();
      },
      host, base::Unretained(&yes)));
  return yes;
}

// A clearing's part on the network thread: answered when the jar has done it.
void br_net_data_clear(const BrClear& c) {
  if (br_net_remote()) {
    br_net_remote_clear(c);
    return;
  }
  auto done = std::make_shared<base::WaitableEvent>();
  br_net_post(base::BindOnce(
      [](BrClear c, std::shared_ptr<base::WaitableEvent> done) {
        net::URLRequestContext* ctx = Context();
        const base::Time from =
            c.since_us == 0 ? base::Time()
                            : base::Time::FromMillisecondsSinceUnixEpoch(
                                  c.since_us / 1000);
        if ((c.kinds & BR_CLEAR_CACHE) != 0 && g_cache != nullptr) {
          std::vector<std::string> gone;
          std::unique_ptr<disk_cache::Backend::Iterator> it =
              g_cache->CreateIterator();
          for (;;) {
            disk_cache::EntryResult r = it->OpenNextEntry(base::DoNothing());
            if (r.net_error() != net::OK)
              break;
            disk_cache::ScopedEntryPtr e(r.ReleaseEntry());
            // The cache's key ends with the address.
            const std::string key = e->GetKey();
            const size_t at = key.rfind(" http");
            const GURL url(at == std::string::npos ? key : key.substr(at + 1));
            if (br_clear_host_hit(c, std::string(url.host())) &&
                e->GetLastUsed() >= from) {
              gone.push_back(key);
            }
          }
          for (const std::string& key : gone)
            (void)g_cache->DoomEntry(key, net::IDLE, base::DoNothing());
          if (!gone.empty() && g_cache_store >= 0)
            br_store_dirty(g_cache_store);
          printf("browser: cache: %u entries cleared\n",
                 static_cast<unsigned>(gone.size()));
          // What is known of servers says where the user has been.
          if (c.site.empty() && c.since_us == 0 &&
              ctx->http_server_properties() != nullptr) {
            ctx->http_server_properties()->Clear(base::DoNothing());
          }
        }
        if ((c.kinds & BR_CLEAR_HISTORY) != 0 &&
            ctx->transport_security_state() != nullptr) {
          if (c.site.empty()) {
            ctx->transport_security_state()->DeleteAllDynamicDataBetween(
                from, base::Time::Max(), base::DoNothing());
          } else {
            ctx->transport_security_state()->DeleteDynamicDataForHost(c.site);
          }
        }
        if ((c.kinds & BR_CLEAR_COOKIE) == 0) {
          done->Signal();
          return;
        }
        ctx->cookie_store()->DeleteMatchingCookiesAsync(
            base::BindRepeating(
                [](BrClear c, base::Time from,
                   const net::CanonicalCookie& cc) {
                  return cc.CreationDate() >= from &&
                         br_clear_host_hit(c, cc.Domain());
                },
                c, from),
            base::BindOnce(
                [](std::shared_ptr<base::WaitableEvent> done, uint32_t n) {
                  printf("browser: cookies: %u cleared\n", n);
                  done->Signal();
                },
                done));
      },
      c, done));
  done->Wait();
}

void br_net_data_sites(std::set<std::string>* out) {
  if (br_net_remote()) {
    br_net_remote_sites(out);
    return;
  }
  auto done = std::make_shared<base::WaitableEvent>();
  br_net_post(base::BindOnce(
      [](std::set<std::string>* out,
         std::shared_ptr<base::WaitableEvent> done) {
        Context()->cookie_store()->GetAllCookiesAsync(base::BindOnce(
            [](std::set<std::string>* out,
               std::shared_ptr<base::WaitableEvent> done,
               const net::CookieList& all) {
              for (const net::CanonicalCookie& c : all)
                out->insert(br_site_of(c.Domain()));
              done->Signal();
            },
            out, done));
      },
      out, done));
  done->Wait();
}

net::URLRequestContext* br_net_context() {
  return Context();
}

// Runs |work| on the network thread, later, with the network runner current
// (the objects of the stack it makes post their own work there).
void br_net_post(base::OnceClosure work) {
  TheNetRunner()->PostTask(FROM_HERE,
                           base::BindOnce(
                               [](base::OnceClosure work) {
                                 BrNetScope scope;
                                 std::move(work).Run();
                               },
                               std::move(work)));
}

// Runs |work| on the network thread after |ms| milliseconds, as br_net_post.
void br_net_post_delayed(base::OnceClosure work, int ms) {
  TheNetRunner()->PostDelayedTask(FROM_HERE,
                                  base::BindOnce(
                                      [](base::OnceClosure work) {
                                        BrNetScope scope;
                                        std::move(work).Run();
                                      },
                                      std::move(work)),
                                  base::Milliseconds(ms));
}

// Runs |work| later on the page's thread (its main runner).
void br_net_post_main(base::OnceClosure work) {
  PostToOrigin(nullptr, std::move(work));
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
  if (br_net_remote())
    return br_net_remote_cookie_line(url, for_script, out, cap);
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
  if (br_net_remote()) {
    br_net_remote_cookie_set(url, line, from_script);
    return;
  }
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

// ---- the jar for cookieStore (br_cookies.h) ---------------------------------

namespace {

// The watches of cookieStore's change events, on the network's thread
std::map<int, std::unique_ptr<net::CookieChangeSubscription>>& Watches() {
  static auto* watches =
      new std::map<int, std::unique_ptr<net::CookieChangeSubscription>>();
  return *watches;
}

}  // namespace

net::CookieList br_net_cookies(const GURL& url) {
  net::CookieList out;
  if (br_net_remote()) {
    std::vector<std::string> all;
    if (br_net_remote_cookies(url, &all)) {
      for (const std::string& b : all) {
        std::unique_ptr<net::CanonicalCookie> c = br_net_cookie_of(b);
        if (c)
          out.push_back(*c);
      }
    }
    return out;
  }
  RunOnNetAndWait(base::BindOnce(
      [](const GURL& url, net::CookieList* out) {
        net::URLRequestContext* context = Context();
        if (!url.is_valid() || context == nullptr)
          return;
        net::CookieOptions options = net::CookieOptions::MakeAllInclusive();
        options.set_exclude_httponly();
        // the jar is in memory once read back: it answers at once
        context->cookie_store()->GetCookieListWithOptionsAsync(
            url, options, net::CookiePartitionKeyCollection(),
            base::BindOnce(
                [](net::CookieList* out,
                   const net::CookieAccessResultList& included,
                   const net::CookieAccessResultList&) {
                  for (const auto& c : included)
                    out->push_back(c.cookie);
                },
                out));
      },
      url, &out));
  return out;
}

bool br_net_cookie_put(std::unique_ptr<net::CanonicalCookie> cookie,
                       const GURL& url) {
  if (br_net_remote())
    return cookie && br_net_remote_cookie_put(br_net_cookie_bytes(*cookie), url);
  bool stored = false;
  RunOnNetAndWait(base::BindOnce(
      [](std::unique_ptr<net::CanonicalCookie> cookie, const GURL& url,
         bool* stored) {
        net::URLRequestContext* context = Context();
        if (!url.is_valid() || context == nullptr)
          return;
        net::CookieOptions options = net::CookieOptions::MakeAllInclusive();
        options.set_exclude_httponly();
        context->cookie_store()->SetCanonicalCookieAsync(
            std::move(cookie), url, options,
            base::BindOnce(
                [](bool* stored, net::CookieAccessResult result) {
                  *stored = result.status.IsInclude();
                },
                stored),
            std::nullopt);
      },
      std::move(cookie), url, &stored));
  return stored;
}

int br_net_cookie_watch(
    const GURL& url,
    base::RepeatingCallback<void(const net::CookieChangeInfo&)> told) {
  if (br_net_remote())
    return br_net_remote_cookie_watch(url, std::move(told));
  int id = 0;
  RunOnNetAndWait(base::BindOnce(
      [](const GURL& url,
         base::RepeatingCallback<void(const net::CookieChangeInfo&)> told,
         int* id) {
        static int next = 0;
        net::URLRequestContext* context = Context();
        if (!url.is_valid() || context == nullptr)
          return;
        *id = ++next;
        Watches()[*id] =
            context->cookie_store()->GetChangeDispatcher().AddCallbackForUrl(
                url, std::nullopt, std::move(told));
      },
      url, std::move(told), &id));
  return id;
}

void br_net_cookie_unwatch(int watch) {
  if (watch <= 0)
    return;
  if (br_net_remote()) {
    br_net_remote_cookie_unwatch(watch);
    return;
  }
  RunOnNetAndWait(
      base::BindOnce([](int watch) { Watches().erase(watch); }, watch));
}

// The cookies written, at most 'ms' from now: for a window closing while its
// page's thread does not come round. Any thread.
extern "C" void br_net_cookies_flush(int ms) {
  if (br_net_remote()) {
    (void)br_net_remote_flush(ms);
    return;
  }
  if (g_cookie_store_id >= 0)
    (void)br_store_wait(br_store_flush_one(g_cookie_store_id), ms);
}

// Start as a machine started again would (tests).
extern "C" int br_net_new_boot = 0;

// ---- for the network process's file (net_internal_tessronos.h) --------------

scoped_refptr<base::SingleThreadTaskRunner> br_net_asker_runner() {
  scoped_refptr<base::SingleThreadTaskRunner> origin =
      base::SingleThreadTaskRunner::HasCurrentDefault()
          ? base::SingleThreadTaskRunner::GetCurrentDefault()
          : nullptr;
  if (br_net_fetch_answer_on_main && MainRunner())
    origin = MainRunner();
  return origin;
}

void br_net_to_asker(scoped_refptr<base::SingleThreadTaskRunner> origin,
                     base::OnceClosure work) {
  PostToOrigin(std::move(origin), std::move(work));
}

void br_net_count_up() {
  OutstandingUp();
}

void br_net_count_down() {
  OutstandingDown();
}

void br_net_call_done(BrNetFetchDone done, void* ctx, const BrNetAnswer& a,
                      const char* data, unsigned len) {
  if (!a.ok)
    NoteFailure(a.url, a.error);
  else
    NoteProtocol(a.url, a.protocol);
  if (done == nullptr)
    return;
  g_status_text = &a.status_text;
  g_protocol = &a.protocol;
  g_redirects = &a.redirects;
  done(ctx, a.ok, a.status, a.url.c_str(), a.headers.c_str(), a.ctype.c_str(),
       data != nullptr ? data : "", len);
  g_status_text = nullptr;
  g_protocol = nullptr;
  g_redirects = nullptr;
}

void br_net_call_part(BrNetFetchPart part, void* ctx, const BrNetAnswer& a) {
  g_status_text = &a.status_text;
  g_protocol = &a.protocol;
  g_redirects = &a.redirects;
  part(ctx, 1, a.status, a.url.c_str(), a.headers.c_str(), a.ctype.c_str(), "",
       0);
  g_status_text = nullptr;
  g_protocol = nullptr;
  g_redirects = nullptr;
}

const std::vector<BrRedirect>* br_net_answer_redirects() {
  return g_redirects;
}

std::string br_net_cookie_bytes(const net::CanonicalCookie& c) {
  Out out;
  BrCookieStore::PutCookie(&out, c);
  return out.bytes();
}

std::unique_ptr<net::CanonicalCookie> br_net_cookie_of(const std::string& b) {
  In in(reinterpret_cast<const unsigned char*>(b.data()), b.size());
  return BrCookieStore::GetCookie(&in);
}
