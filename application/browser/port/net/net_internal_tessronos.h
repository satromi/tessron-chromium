/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	net_internal_tessronos.h
 *	ブラウザ: what the network stack's file (net_fetch_tessronos.cc) and
 *	the network process's (net_service_tessronos.cc) share (design 14.13.8).
 *
 *	In a renderer with a network process, the requests, the cookie jar and
 *	what is known of hosts are asked of that process; the answers are
 *	handed to the askers as the stack of the process itself hands them
 *	(on the asker's runner, with the response's redirects, reason phrase
 *	and protocol readable while the callback runs).
 */

#ifndef NET_INTERNAL_TESSRONOS_H
#define NET_INTERNAL_TESSRONOS_H

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/single_thread_task_runner.h"

#include "br_net.h"

class GURL;
namespace net {
class CanonicalCookie;
struct CookieChangeInfo;
}  // namespace net
struct BrClear;
struct BrExtReq;
struct BrExtAns;
struct BrExtHeader;

// A redirect a request followed: the address that answered with it, its
// status and header lines, and where it went with what method and referrer
struct BrRedirect {
  std::string url;
  int status = 0;
  std::string headers;
  std::string new_url;
  std::string new_method;
  std::string new_referrer;
};

// An answer as the asker is told of it
struct BrNetAnswer {
  int ok = 0;
  int status = 0;
  std::string url;
  std::string headers;
  std::string ctype;
  std::string status_text;
  std::string protocol;
  std::vector<BrRedirect> redirects;
  int error = 0;
};

// ---- net_fetch_tessronos.cc, for the renderer's side

// The runner an answer for a request started now goes to (the asker's)
scoped_refptr<base::SingleThreadTaskRunner> br_net_asker_runner();
// Run on the asker's runner, as the stack's own answers are
void br_net_to_asker(scoped_refptr<base::SingleThreadTaskRunner> origin,
                     base::OnceClosure work);
// The requests under way, as counted for the page's load
void br_net_count_up();
void br_net_count_down();
// An answer's 'done' and a stream's 'part' called with its response
// readable (br_net_fetch_redirects and the others), the end noted
void br_net_call_done(BrNetFetchDone done, void* ctx, const BrNetAnswer& a,
                      const char* data, unsigned len);
void br_net_call_part(BrNetFetchPart part, void* ctx, const BrNetAnswer& a);
// While a callback runs: the redirects of the answer it is told
const std::vector<BrRedirect>* br_net_answer_redirects();
// The asker of a request (its ctx) has gone: it is told the end at once, and
// the request is let go unless another asker waits for its answer
void br_net_fetch_drop(void* ctx);
// A cookie as bytes and back
std::string br_net_cookie_bytes(const net::CanonicalCookie& c);
std::unique_ptr<net::CanonicalCookie> br_net_cookie_of(const std::string& b);

// ---- net_service_tessronos.cc

// This process asks a network process (a renderer of the browser of
// several processes); set before the network stack is first used
bool br_net_remote();
int br_net_remote_fetch(const char* url, const char* method, const void* body,
                        unsigned body_len, const char* req_headers,
                        BrNetFetchPart part, BrNetFetchDone done, void* ctx,
                        scoped_refptr<base::SingleThreadTaskRunner> origin);
void br_net_remote_cancel(void* ctx);
// The renderer's thread the network process's answers come to
scoped_refptr<base::SingleThreadTaskRunner> br_net_remote_answers();
int br_net_remote_fetch_sync(const char* url, const char* method,
                             const void* body, unsigned body_len,
                             const char* req_headers, BrNetAnswer* a,
                             std::string* data, int timeout_ms,
                             const volatile int* cancel);
int br_net_remote_cookie_line(const char* url, int for_script, char* out,
                              int cap);
void br_net_remote_cookie_set(const char* url, const char* line,
                              int from_script);
bool br_net_remote_cookies(const GURL& url, std::vector<std::string>* out);
bool br_net_remote_cookie_put(const std::string& bytes, const GURL& url);
int br_net_remote_cookie_watch(
    const GURL& url,
    base::RepeatingCallback<void(const net::CookieChangeInfo&)> told);
void br_net_remote_cookie_unwatch(int watch);
void br_net_remote_hsts_add(const std::string& host);
bool br_net_remote_hsts_known(const std::string& host);
void br_net_remote_clear(const BrClear& c);
void br_net_remote_sites(std::set<std::string>* out);
int br_net_remote_protocols(char* out, unsigned cap);
bool br_net_remote_flush(int ms);

// In the network process: the hooks of the renderer whose request a header
// line names ("X-TessronOS-Hooks: <its number> <what it has>"), asked on the
// network thread. False when there is no such renderer (it has gone).
bool br_net_hook_rules(int owner, const BrExtReq& q, int* act,
                       std::string* redirect);
bool br_net_hook_headers(int owner, const BrExtReq& q, bool response,
                         std::vector<BrExtHeader>* ops);
void br_net_hook_web(int owner, const char* event, long long id,
                     const BrExtReq& q, int status, const std::string& more);
bool br_net_hook_serve(int owner, const BrExtReq& q, int* act, BrExtAns* ans);

#endif  // NET_INTERNAL_TESSRONOS_H
