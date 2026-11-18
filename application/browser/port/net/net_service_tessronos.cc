/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	net_service_tessronos.cc
 *	ブラウザ: the network process, and the renderers' way to it
 *	(design 14.13.8).
 *
 *	The browser of several processes has one network process. It is the
 *	program again, started by the browser with the line "network" first,
 *	and holds the one network stack (net_fetch_tessronos.cc): the
 *	connections, the cookie jar, the HTTP cache, QUIC's sessions, the
 *	names looked up, the hosts that asked for HTTPS, and the stores they
 *	are kept in. Two renderers thus see the same cookies and the same
 *	sign-in, and a connection one opened serves the other; nothing of it
 *	is read back or merged between them.
 *
 *	Each renderer has a pipe of its own to it (br.mojom.Network), given with
 *	its invitation. The network process answers the pipes on a thread of
 *	its own ("network calls"), not on the network thread: what it is asked
 *	is what the stack's functions do, some of which hand work to the
 *	network thread and wait for it. A request's answer comes back to that
 *	thread and goes to the renderer on the request's own pipe
 *	(NetFetchClient), which the renderer closes to let the request go.
 *
 *	A renderer's extensions are its own: the network delegate asks the
 *	renderer that started a request (NetHooks) for their rules and tells
 *	it what webRequest's listeners are to see. The renderer answers on a
 *	thread that does nothing else. A request says whether the renderer
 *	has any (the line "X-TessronOS-Hooks"), so that one with none is not
 *	asked.
 *
 *	In a renderer, the stack's functions (br_net.h, br_cookies.h) ask the
 *	network process. The answers come to a thread of the renderer's ("net
 *	answers") and are handed to the askers on their runners, as the stack
 *	of the process itself hands them. When the network process has gone,
 *	what was under way fails, and the browser gives the renderer a pipe to
 *	the one it started again (Control.NetworkAgain).
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/run_loop.h"
#include "base/task/single_thread_task_executor.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "mojo/public/cpp/bindings/shared_remote.h"
#include "mojo/public/cpp/bindings/unique_receiver_set.h"
#include "mojo/public/cpp/platform/platform_channel_endpoint.h"
#include "mojo/public/cpp/system/invitation.h"
#include "net/base/net_errors.h"
#include "net/cookies/canonical_cookie.h"
#include "net/cookies/cookie_access_result.h"
#include "net/cookies/cookie_change_dispatcher.h"
#include "url/gurl.h"

#include "application/browser/mojom/br_process.mojom.h"

#include "../ext/br_ext.h"
#include "br_cookies.h"
#include "br_data.h"
#include "br_net.h"
#include "br_store.h"
#include "br_stream.h"
#include "net_internal_tessronos.h"

/* br_proc.cc and br_main.cc */
scoped_refptr<base::SingleThreadTaskRunner> br_proc_io_runner();
void br_base_up();
int br_sys_take( int ms );
void br_start_mark( const char *what );
void br_sandbox_network();
/*
 * A renderer's pipe to the stores, answered on a thread of their own, the
 * site the browser gave it, and an extension's object the browser named
 * (store_service_tessronos.cc)
 */
void br_store_serve( int renderer, mojo::PendingReceiver<br::mojom::Store> store );
void br_store_site( int renderer, const std::string &site );
void br_store_ext_allow( const std::string &uuid );

/* A renderer's WebSocket, on the network thread (port/blink/websocket_tessronos.cc) */
void br_ws_serve( std::string url, std::vector<std::string> protocols, std::string origin,
		  mojo::PendingReceiver<br::mojom::NetSocket> socket,
		  mojo::PendingRemote<br::mojom::NetSocketClient> client );
void br_net_post( base::OnceClosure work );

/* The stack's functions answered by the network process (net_fetch_tessronos.cc) */
extern "C" const char *br_net_fetch_status_text( void );
extern "C" const char *br_net_fetch_answer_protocol( void );
void br_net_hsts_add( const std::string &host );
bool br_net_hsts_known( const std::string &host );

namespace {

namespace mj = br::mojom;

long mono_ms()
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

std::vector<uint8_t> bytes_of( const void *p, size_t n )
{
	const uint8_t *b = static_cast<const uint8_t *>(p);
	return n > 0 && b != nullptr ? std::vector<uint8_t>(b, b + n) : std::vector<uint8_t>();
}

std::string str_of( const std::vector<uint8_t> &v )
{
	return std::string(reinterpret_cast<const char *>(v.data()), v.size());
}

mj::NetAnswerPtr to_mojo( const BrNetAnswer &a )
{
	auto m = mj::NetAnswer::New();
	m->ok = a.ok != 0;
	m->status = a.status;
	m->url = a.url;
	m->headers = a.headers;
	m->ctype = a.ctype;
	m->status_text = a.status_text;
	m->protocol = a.protocol;
	for ( const BrRedirect &r : a.redirects ) {
		m->redirects.push_back(mj::NetRedirect::New(r.url, r.status, r.headers, r.new_url, r.new_method,
							    r.new_referrer));
	}
	m->error = a.error;
	return m;
}

BrNetAnswer of_mojo( const mj::NetAnswerPtr &m )
{
	BrNetAnswer a;
	if ( !m ) return a;
	a.ok = m->ok ? 1 : 0;
	a.status = m->status;
	a.url = m->url;
	a.headers = m->headers;
	a.ctype = m->ctype;
	a.status_text = m->status_text;
	a.protocol = m->protocol;
	for ( const mj::NetRedirectPtr &r : m->redirects ) {
		BrRedirect b;
		b.url = r->url;
		b.status = r->status;
		b.headers = r->headers;
		b.new_url = r->new_url;
		b.new_method = r->new_method;
		b.new_referrer = r->new_referrer;
		a.redirects.push_back(std::move(b));
	}
	a.error = m->error;
	return a;
}

mj::NetExtReqPtr to_mojo( const BrExtReq &q )
{
	return mj::NetExtReq::New(q.url, q.initiator, q.type, q.method);
}

BrExtReq of_mojo( const mj::NetExtReqPtr &m )
{
	BrExtReq q;
	if ( !m ) return q;
	q.url = m->url;
	q.initiator = m->initiator;
	q.type = m->type;
	q.method = m->method;
	return q;
}

/* ================================================================ the renderer's side */

bool			g_remote;		/* this process asks a network process */
base::Thread		*g_answers;		/* where the answers come ("net answers") */
base::Thread		*g_hooks_thread;	/* where the extensions are asked ("net hooks") */
std::mutex		g_net_lock;
mojo::SharedRemote<mj::Network>	g_net;		/* the pipe to the network process, any thread */

mojo::SharedRemote<mj::Network> the_net()
{
	std::lock_guard<std::mutex> hold(g_net_lock);
	return g_net;
}

/* The ext line of a request: the rules (1) and webRequest's listeners (2) this renderer has */
int ext_has()
{
	return ( br_ext_any() ? 1 : 0 ) | ( br_ext_web_wanted() ? 2 : 0 );
}

/*
 * One request asked of the network process: its pipe bound on the answers'
 * thread, and its answer handed to the asker's runner. It ends once, by its
 * answer, by being let go (a stream), or by the pipe closing (the network
 * process gone), and deletes itself there.
 */
class Client final : public mj::NetFetchClient {
public:
	Client( std::string url, BrNetFetchPart part, BrNetFetchDone done, void *ctx,
		scoped_refptr<base::SingleThreadTaskRunner> origin )
		: url_(std::move(url)), part_(part), done_(done), ctx_(ctx), origin_(std::move(origin)) {}

	void bind( mojo::PendingReceiver<mj::NetFetchClient> r )
	{
		recv_.Bind(std::move(r));
		recv_.set_disconnect_handler(base::BindOnce(&Client::gone, base::Unretained(this)));
	}

	bool is_stream_of( void *ctx ) const { return part_ != nullptr && ctx_ == ctx; }

	void Response( mj::NetAnswerPtr head ) override
	{
		if ( part_ == nullptr || ended_ ) return;
		/* a stream: from its response on, it does not hold the page's load */
		bool counted = counted_;
		counted_ = false;
		br_net_to_asker(origin_, base::BindOnce([]( BrNetFetchPart part, void *ctx, BrNetAnswer a, bool counted ) {
			if ( counted ) br_net_count_down();
			br_net_call_part(part, ctx, a);
		}, part_, ctx_, of_mojo(head), counted));
	}

	void Piece( const std::vector<uint8_t> &data, PieceCallback taken ) override
	{
		/* taken as it comes: the asker here holds what it is handed, and the network process reads on */
		std::move(taken).Run();
		if ( part_ == nullptr || ended_ ) return;
		br_net_to_asker(origin_, base::BindOnce([]( BrNetFetchPart part, void *ctx, std::string piece ) {
			part(ctx, 0, 0, "", "", "", piece.data(), (unsigned)piece.size());
		}, part_, ctx_, str_of(data)));
	}

	void Done( mj::NetAnswerPtr answer, const std::vector<uint8_t> &body ) override
	{
		end(of_mojo(answer), str_of(body));
	}

	/*
	 * Taken out of this process (br_net_fetch_take): the pipe, with what
	 * has not been read from it yet, for the browser to go on with. The
	 * asker is told nothing more.
	 */
	mojo::PendingReceiver<mj::NetFetchClient> take();

	/* Let go by the asker: the pipe closed (the request goes) and the end told */
	void let_go()
	{
		recv_.reset();
		BrNetAnswer a;
		a.url = url_;
		a.error = net::ERR_ABORTED;
		end(a, std::string());
	}

private:
	void gone()
	{
		BrNetAnswer a;
		a.url = url_;
		a.error = net::ERR_FAILED;
		printf("browser: net: %.160s failed: the network process has gone\n", url_.c_str());
		end(a, std::string());
	}

	void end( BrNetAnswer a, std::string body );

	std::string				url_;
	BrNetFetchPart				part_;
	BrNetFetchDone				done_;
	void					*ctx_;
	scoped_refptr<base::SingleThreadTaskRunner> origin_;
	bool					counted_ = true;
	bool					ended_ = false;
	mojo::Receiver<mj::NetFetchClient>	recv_{this};
};

/* The requests under way, on the answers' thread: a stream is let go by its ctx */
std::set<Client *> &clients()
{
	static auto *c = new std::set<Client *>();
	return *c;
}

mojo::PendingReceiver<mj::NetFetchClient> Client::take()
{
	ended_ = true;
	if ( counted_ ) br_net_count_down();
	counted_ = false;
	mojo::PendingReceiver<mj::NetFetchClient> r = recv_.Unbind();
	clients().erase(this);
	base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE, this);
	return r;
}

void Client::end( BrNetAnswer a, std::string body )
{
	if ( ended_ ) return;
	ended_ = true;
	recv_.reset();
	br_net_to_asker(origin_, base::BindOnce([]( BrNetFetchDone done, void *ctx, std::string asked, BrNetAnswer a,
						   std::string body, bool counted ) {
		if ( counted ) br_net_count_down();
		/* what failed and what came over which protocol is noted by the address asked for */
		std::string got = a.url;
		if ( !a.ok || got.empty() ) a.url = asked;
		br_net_call_done(done, ctx, a, body.data(), (unsigned)body.size());
		(void)got;
	}, done_, ctx_, url_, std::move(a), std::move(body), counted_));
	clients().erase(this);
	base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE, this);
}

/* The extensions of this renderer, asked by the network process about its requests */
class Hooks final : public mj::NetHooks {
public:
	void Rules( mj::NetExtReqPtr rq, RulesCallback cb ) override
	{
		BrExtAns ans;
		BrExtAct act = br_ext_rules(of_mojo(rq), &ans);
		std::move(cb).Run((int)act, ans.redirect);
	}

	void HeaderRules( mj::NetExtReqPtr rq, bool response, HeaderRulesCallback cb ) override
	{
		std::vector<BrExtHeader> ops;
		br_ext_header_rules(of_mojo(rq), response ? nullptr : &ops, response ? &ops : nullptr);
		std::vector<mj::NetExtHeaderPtr> out;
		for ( const BrExtHeader &h : ops ) out.push_back(mj::NetExtHeader::New((int8_t)h.op, h.name, h.value));
		std::move(cb).Run(std::move(out));
	}

	void Web( const std::string &event, int64_t id, mj::NetExtReqPtr rq, int32_t status,
		  const std::string &more ) override
	{
		br_ext_web(event.c_str(), id, of_mojo(rq), status, more);
	}

	void Serve( mj::NetExtReqPtr rq, ServeCallback cb ) override
	{
		BrExtAns ans;
		BrExtAct act = br_ext_serve(of_mojo(rq), &ans);
		std::move(cb).Run((int)act, ans.status, ans.ctype, ans.headers, bytes_of(ans.body.data(), ans.body.size()));
	}
};

/* A change of the cookies of an address, to the watch's callback */
class Watcher final : public mj::NetCookieWatcher {
public:
	explicit Watcher( base::RepeatingCallback<void( const net::CookieChangeInfo & )> told ) : told_(std::move(told)) {}

	void Changed( const std::vector<uint8_t> &cookie, int32_t cause ) override
	{
		std::unique_ptr<net::CanonicalCookie> c = br_net_cookie_of(str_of(cookie));
		if ( !c ) return;
		told_.Run(net::CookieChangeInfo(*c, net::CookieAccessResult(), static_cast<net::CookieChangeCause>(cause)));
	}

private:
	base::RepeatingCallback<void( const net::CookieChangeInfo & )> told_;
};

/* The cookieStore's watches, kept to be made again on another network process */
struct Watch {
	GURL	url;
	base::RepeatingCallback<void( const net::CookieChangeInfo & )> told;
	std::unique_ptr<mojo::Receiver<mj::NetCookieWatcher>> recv;	/* on the answers' thread */
	std::unique_ptr<Watcher> impl;
};
std::map<int, Watch> &watches()
{
	static auto *w = new std::map<int, Watch>();
	return *w;
}
int g_next_watch;

/* On the answers' thread: the watch asked of the network process now */
void watch_on( int id )
{
	auto it = watches().find(id);
	if ( it == watches().end() ) return;
	Watch &w = it->second;
	mojo::PendingRemote<mj::NetCookieWatcher> remote;
	w.impl = std::make_unique<Watcher>(w.told);
	w.recv = std::make_unique<mojo::Receiver<mj::NetCookieWatcher>>(w.impl.get(),
									 remote.InitWithNewPipeAndPassReceiver());
	mojo::SharedRemote<mj::Network> n = the_net();
	if ( n ) n->CookieWatch(w.url.spec(), std::move(remote));
}

/* ================================================================ the network process's side */

base::Thread		*g_calls;		/* where the pipes are answered ("network calls") */
scoped_refptr<base::SingleThreadTaskRunner> g_main_runner;	/* the process's main thread: the stores' ticks */
std::mutex		g_hooks_lock;
std::map<int, mojo::SharedRemote<mj::NetHooks>> g_hooks;	/* by the renderer's number */
int			g_next_owner;

mojo::SharedRemote<mj::NetHooks> hooks_of( int owner )
{
	std::lock_guard<std::mutex> hold(g_hooks_lock);
	auto it = g_hooks.find(owner);
	return it != g_hooks.end() ? it->second : mojo::SharedRemote<mj::NetHooks>();
}

/* One request of a renderer's, on the calls' thread: the stack's answers go to its pipe */
struct ServerFetch {
	mojo::Remote<mj::NetFetchClient> client;
	std::string url;
	bool stream = false;
};

/*
 * The requests under way by their numbers, on the calls' thread. The stack
 * knows a request by its number (its ctx), never used again, so that
 * letting one go late cannot touch another
 */
std::map<uintptr_t, std::unique_ptr<ServerFetch>> &server_fetches()
{
	static auto *m = new std::map<uintptr_t, std::unique_ptr<ServerFetch>>();
	return *m;
}
uintptr_t g_next_fetch;

ServerFetch *server_fetch( void *ctx )
{
	auto it = server_fetches().find(reinterpret_cast<uintptr_t>(ctx));
	return it != server_fetches().end() ? it->second.get() : nullptr;
}

BrNetAnswer answer_now( ServerFetch *f, int ok, int status, const char *url, const char *headers, const char *ctype )
{
	BrNetAnswer a;
	a.ok = ok;
	a.status = status;
	a.url = url != nullptr ? url : "";
	a.headers = headers != nullptr ? headers : "";
	a.ctype = ctype != nullptr ? ctype : "";
	const char *st = br_net_fetch_status_text();
	a.status_text = st != nullptr ? st : "";
	const char *pr = br_net_fetch_answer_protocol();
	a.protocol = pr != nullptr ? pr : "";
	if ( const std::vector<BrRedirect> *r = br_net_answer_redirects() ) a.redirects = *r;
	if ( !ok ) a.error = br_net_fetch_error(f->url.c_str(), nullptr, 0);
	return a;
}

void server_part( void *ctx, int first, int status, const char *url, const char *headers, const char *ctype,
		  const char *data, unsigned len )
{
	ServerFetch *f = server_fetch(ctx);
	if ( f == nullptr || !f->client.is_connected() ) return;
	if ( first ) {
		f->client->Response(to_mojo(answer_now(f, 1, status, url, headers, ctype)));
		return;
	}
	/* the stream reads on as the asker answers that it has taken the pieces */
	f->client->Piece(bytes_of(data, len), base::BindOnce([]( void *ctx, unsigned n ) {
		br_net_fetch_taken(ctx, n);
	}, ctx, len));
}

void server_done( void *ctx, int ok, int status, const char *url, const char *headers, const char *ctype,
		  const char *data, unsigned len )
{
	ServerFetch *f = server_fetch(ctx);
	if ( f == nullptr ) return;
	if ( f->client.is_connected() ) {
		f->client->Done(to_mojo(answer_now(f, ok, status, url, headers, ctype)),
				f->stream ? std::vector<uint8_t>() : bytes_of(data, len));
	}
	server_fetches().erase(reinterpret_cast<uintptr_t>(ctx));
}

/* A renderer's pipe to the network process */
class NetworkImpl final : public mj::Network {
public:
	NetworkImpl() : owner_(++g_next_owner) {}
	~NetworkImpl() override
	{
		for ( auto &w : watches_ ) br_net_cookie_unwatch(w.first);
		std::lock_guard<std::mutex> hold(g_hooks_lock);
		g_hooks.erase(owner_);
	}

	void SetHooks( mojo::PendingRemote<mj::NetHooks> hooks ) override
	{
		std::lock_guard<std::mutex> hold(g_hooks_lock);
		g_hooks[owner_] = mojo::SharedRemote<mj::NetHooks>(std::move(hooks), br_proc_io_runner());
	}

	void Fetch( const std::string &url, const std::string &method, const std::vector<uint8_t> &body,
		    const std::string &headers, bool stream, int32_t ext,
		    mojo::PendingRemote<mj::NetFetchClient> client ) override
	{
		void *ctx = reinterpret_cast<void *>(++g_next_fetch);
		ServerFetch *f = ( server_fetches()[g_next_fetch] = std::make_unique<ServerFetch>() ).get();
		f->client.Bind(std::move(client));
		f->url = url;
		f->stream = stream;
		std::string lines = headers;
		if ( ext != 0 ) lines += "X-TessronOS-Hooks: " + std::to_string(owner_) + " " + std::to_string(ext) + "\r\n";
		/* let go by the renderer, or the renderer gone: the request goes, and its end is told to no one */
		f->client.set_disconnect_handler(base::BindOnce([]( void *ctx ) {
			br_net_fetch_drop(ctx);
		}, ctx));
		int ok = stream ? br_net_fetch_paced(url.c_str(), method.c_str(), body.data(), (unsigned)body.size(),
						     lines.c_str(), &server_part, &server_done, ctx, 0)
				: br_net_fetch_start(url.c_str(), method.c_str(), body.data(), (unsigned)body.size(),
						     lines.c_str(), &server_done, ctx);
		if ( !ok ) {
			BrNetAnswer a;
			a.url = url;
			a.error = net::ERR_FAILED;
			if ( f->client.is_connected() ) f->client->Done(to_mojo(a), std::vector<uint8_t>());
			server_fetches().erase(reinterpret_cast<uintptr_t>(ctx));
		}
	}

	void CookieLine( const std::string &url, bool for_script, CookieLineCallback cb ) override
	{
		std::string line(16384, '\0');
		int n = br_net_cookie_line(url.c_str(), for_script ? 1 : 0, &line[0], (int)line.size());
		line.resize(n > 0 ? (size_t)n : 0);
		std::move(cb).Run(line);
	}

	void CookieSet( const std::string &url, const std::string &line, bool from_script ) override
	{
		br_net_cookie_set(url.c_str(), line.c_str(), from_script ? 1 : 0);
	}

	void Cookies( const std::string &url, CookiesCallback cb ) override
	{
		std::vector<std::vector<uint8_t>> out;
		for ( const net::CanonicalCookie &c : br_net_cookies(GURL(url)) ) {
			std::string b = br_net_cookie_bytes(c);
			out.push_back(bytes_of(b.data(), b.size()));
		}
		std::move(cb).Run(std::move(out));
	}

	void CookiePut( const std::vector<uint8_t> &cookie, const std::string &url, CookiePutCallback cb ) override
	{
		std::unique_ptr<net::CanonicalCookie> c = br_net_cookie_of(str_of(cookie));
		std::move(cb).Run(c != nullptr && br_net_cookie_put(std::move(c), GURL(url)));
	}

	void CookieWatch( const std::string &url, mojo::PendingRemote<mj::NetCookieWatcher> watcher ) override
	{
		auto remote = std::make_shared<mojo::Remote<mj::NetCookieWatcher>>(std::move(watcher));
		scoped_refptr<base::SingleThreadTaskRunner> here = base::SingleThreadTaskRunner::GetCurrentDefault();
		/* the jar tells on the network thread; the pipe is this thread's */
		int id = br_net_cookie_watch(GURL(url), base::BindRepeating([](
			scoped_refptr<base::SingleThreadTaskRunner> here, std::weak_ptr<mojo::Remote<mj::NetCookieWatcher>> w,
			const net::CookieChangeInfo &info ) {
			std::string b = br_net_cookie_bytes(info.cookie);
			here->PostTask(FROM_HERE, base::BindOnce([]( std::weak_ptr<mojo::Remote<mj::NetCookieWatcher>> w,
								     std::string b, int cause ) {
				std::shared_ptr<mojo::Remote<mj::NetCookieWatcher>> r = w.lock();
				if ( r && r->is_connected() ) ( *r )->Changed(bytes_of(b.data(), b.size()), cause);
			}, w, std::move(b), (int)info.cause));
		}, here, std::weak_ptr<mojo::Remote<mj::NetCookieWatcher>>(remote)));
		if ( id <= 0 ) return;
		remote->set_disconnect_handler(base::BindOnce([]( NetworkImpl *self, int id ) {
			br_net_cookie_unwatch(id);
			self->watches_.erase(id);
		}, base::Unretained(this), id));
		watches_[id] = remote;
	}

	void HstsAdd( const std::string &host ) override { br_net_hsts_add(host); }

	void HstsKnown( const std::string &host, HstsKnownCallback cb ) override
	{
		std::move(cb).Run(br_net_hsts_known(host));
	}

	void Clear( uint32_t kinds, int64_t since_us, const std::string &site, ClearCallback cb ) override
	{
		BrClear c;
		c.kinds = kinds;
		c.since_us = since_us;
		c.site = site;
		br_net_data_clear(c);
		std::move(cb).Run();
	}

	void Sites( SitesCallback cb ) override
	{
		std::set<std::string> s;
		br_net_data_sites(&s);
		std::move(cb).Run(std::vector<std::string>(s.begin(), s.end()));
	}

	void Protocols( ProtocolsCallback cb ) override
	{
		char n[160];
		int len = br_net_protocols(n, sizeof(n));
		std::move(cb).Run(std::string(n, len > 0 ? (size_t)std::min<int>(len, sizeof(n) - 1) : 0));
	}

	void Flush( int32_t wait_ms, FlushCallback cb ) override;

	void WebSocket( const std::string &url, const std::vector<std::string> &protocols, const std::string &origin,
			mojo::PendingReceiver<mj::NetSocket> socket, mojo::PendingRemote<mj::NetSocketClient> client ) override
	{
		br_net_post(base::BindOnce(&br_ws_serve, url, protocols, origin, std::move(socket), std::move(client)));
	}

private:
	int	owner_;
	std::map<int, std::shared_ptr<mojo::Remote<mj::NetCookieWatcher>>> watches_;
};

/* What the network keeps written: the stores' bytes made on the main thread, the writes waited for here */
bool flush_stores( int wait_ms )
{
	auto ticket = std::make_shared<std::pair<std::mutex, std::condition_variable>>();
	auto got = std::make_shared<std::pair<bool, unsigned>>(false, 0);
	g_main_runner->PostTask(FROM_HERE, base::BindOnce([]( decltype(ticket) t, decltype(got) g ) {
		unsigned n = br_store_flush_async();
		std::lock_guard<std::mutex> hold(t->first);
		*g = { true, n };
		t->second.notify_all();
	}, ticket, got));
	{
		std::unique_lock<std::mutex> hold(ticket->first);
		if ( !ticket->second.wait_for(hold, std::chrono::milliseconds(wait_ms), [&] { return got->first; }) ) return false;
	}
	return br_store_wait(got->second, wait_ms) != 0;
}

void NetworkImpl::Flush( int32_t wait_ms, FlushCallback cb )
{
	std::move(cb).Run(flush_stores(wait_ms));
}

mojo::UniqueReceiverSet<mj::Network>	*g_networks;	/* on the calls' thread */

void end_network( const char *why )
{
	printf("browser: network: %s: what it keeps written, and it ends\n", why);
	br_store_flush_all();
	fflush(stdout);
	_exit(0);
}

/* The browser's pipe */
class ServiceImpl final : public mj::NetworkService {
public:
	void Connect( int32_t renderer, mojo::PendingReceiver<mj::Network> network,
		      mojo::PendingReceiver<mj::Store> store ) override
	{
		auto impl = std::make_unique<NetworkImpl>();
		g_networks->Add(std::move(impl), std::move(network));
		br_store_serve(renderer, std::move(store));
	}

	void SetSite( int32_t renderer, const std::string &site ) override { br_store_site(renderer, site); }

	void ExtAllow( const std::string &uuid ) override { br_store_ext_allow(uuid); }

	void Flush( int32_t wait_ms, FlushCallback cb ) override { std::move(cb).Run(flush_stores(wait_ms)); }

	void Closing() override
	{
		g_main_runner->PostTask(FROM_HERE, base::BindOnce([] { end_network("the browser ends"); }));
	}
};

}  // namespace

/* ================================================================ the renderer's functions */

bool br_net_remote()
{
	return g_remote;
}

/* The renderer's pipe to the network process: at its start, and again when that process was started again */
void br_net_remote_attach( mojo::PendingRemote<br::mojom::Network> pipe )
{
	if ( g_answers == nullptr ) {
		g_answers = new base::Thread("net answers");
		g_hooks_thread = new base::Thread("net hooks");
		if ( !g_answers->Start() || !g_hooks_thread->Start() ) {
			printf("browser: renderer: the threads for the network process did not start\n");
			return;
		}
	}
	mojo::SharedRemote<mj::Network> n(std::move(pipe), g_answers->task_runner());
	{
		std::lock_guard<std::mutex> hold(g_net_lock);
		bool again = static_cast<bool>(g_net);
		g_net = n;
		if ( again ) printf("browser: renderer: a pipe to the network process started again\n");
	}
	g_remote = true;
	mojo::PendingRemote<mj::NetHooks> hooks;
	g_hooks_thread->task_runner()->PostTask(FROM_HERE, base::BindOnce([]( mojo::PendingReceiver<mj::NetHooks> r ) {
		mojo::MakeSelfOwnedReceiver(std::make_unique<Hooks>(), std::move(r));
	}, hooks.InitWithNewPipeAndPassReceiver()));
	n->SetHooks(std::move(hooks));
	/* cookieStore's watches made again on this one */
	g_answers->task_runner()->PostTask(FROM_HERE, base::BindOnce([] {
		for ( auto &w : watches() ) watch_on(w.first);
	}));
}

int br_net_remote_fetch( const char *url, const char *method, const void *body, unsigned body_len,
			 const char *req_headers, BrNetFetchPart part, BrNetFetchDone done, void *ctx,
			 scoped_refptr<base::SingleThreadTaskRunner> origin )
{
	mojo::SharedRemote<mj::Network> n = the_net();
	if ( !n || url == nullptr || done == nullptr ) return 0;
	br_net_count_up();
	auto *c = new Client(url, part, done, ctx, std::move(origin));
	mojo::PendingRemote<mj::NetFetchClient> remote;
	g_answers->task_runner()->PostTask(FROM_HERE, base::BindOnce([]( Client *c,
									 mojo::PendingReceiver<mj::NetFetchClient> r ) {
		clients().insert(c);
		c->bind(std::move(r));
	}, c, remote.InitWithNewPipeAndPassReceiver()));
	n->Fetch(url, method != nullptr ? method : "GET", bytes_of(body, body_len),
		 req_headers != nullptr ? req_headers : "", part != nullptr, ext_has(), std::move(remote));
	return 1;
}

scoped_refptr<base::SingleThreadTaskRunner> br_net_remote_answers()
{
	return g_answers != nullptr ? g_answers->task_runner() : nullptr;
}

/* The browser's side of a download handed over (br_rproc.cc, Host.Download) */
bool br_rproc_download( br::mojom::DownloadHeadPtr head, std::vector<uint8_t> first, bool ended, bool ok,
			mojo::PendingReceiver<br::mojom::NetFetchClient> rest );

void *br_net_fetch_take( void *ctx )
{
	if ( !g_remote || g_answers == nullptr ) return nullptr;
	struct Took {
		std::mutex		m;
		std::condition_variable	cv;
		bool			done = false;
		mojo::PendingReceiver<mj::NetFetchClient> r;
	};
	auto t = std::make_shared<Took>();
	g_answers->task_runner()->PostTask(FROM_HERE, base::BindOnce([]( void *ctx, std::shared_ptr<Took> t ) {
		for ( Client *c : clients() ) {
			if ( c->is_stream_of(ctx) ) {
				t->r = c->take();
				break;
			}
		}
		/*
		 * Told after what the stream handed on before it was taken: those
		 * pieces were put on this thread before this, and go first
		 */
		base::SequencedTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE, base::BindOnce([]( std::shared_ptr<Took> t ) {
			std::lock_guard<std::mutex> hold(t->m);
			t->done = true;
			t->cv.notify_all();
		}, t));
	}, ctx, t));
	std::unique_lock<std::mutex> hold(t->m);
	t->cv.wait(hold, [&] { return t->done; });
	if ( !t->r.is_valid() ) return nullptr;
	return new mojo::PendingReceiver<mj::NetFetchClient>(std::move(t->r));
}

int br_net_fetch_give( void *taken, const char *name, const char *url, const char *type, long long total, int status,
		       const char *first, unsigned first_len, int ended, int ok )
{
	std::unique_ptr<mojo::PendingReceiver<mj::NetFetchClient>> rest(
		static_cast<mojo::PendingReceiver<mj::NetFetchClient> *>(taken));
	auto head = br::mojom::DownloadHead::New();
	head->name = name != nullptr ? name : "";
	head->url = url != nullptr ? url : "";
	head->type = type != nullptr ? type : "";
	head->total = total;
	head->status = status;
	return br_rproc_download(std::move(head), bytes_of(first, first_len), ended != 0, ok != 0,
				 rest ? std::move(*rest) : mojo::PendingReceiver<mj::NetFetchClient>()) ? 1 : 0;
}

void br_net_remote_cancel( void *ctx )
{
	if ( g_answers == nullptr ) return;
	g_answers->task_runner()->PostTask(FROM_HERE, base::BindOnce([]( void *ctx ) {
		for ( Client *c : clients() ) {
			if ( c->is_stream_of(ctx) ) {
				c->let_go();
				return;
			}
		}
	}, ctx));
}

namespace {
/* A request the asker waits for: answered into this on the answers' thread */
struct SyncWait {
	std::mutex		m;
	std::condition_variable	cv;
	bool			done = false;
	BrNetAnswer		a;
	std::string		data;
};

void sync_done( void *ctx, int ok, int status, const char *url, const char *headers, const char *ctype,
		const char *data, unsigned len )
{
	auto *w = static_cast<std::shared_ptr<SyncWait> *>(ctx);
	{
		std::lock_guard<std::mutex> hold((*w)->m);
		BrNetAnswer &a = (*w)->a;
		a.ok = ok;
		a.status = status;
		a.url = url != nullptr ? url : "";
		a.headers = headers != nullptr ? headers : "";
		a.ctype = ctype != nullptr ? ctype : "";
		const char *st = br_net_fetch_status_text();
		a.status_text = st != nullptr ? st : "";
		(*w)->data.assign(data != nullptr ? data : "", len);
		(*w)->done = true;
		(*w)->cv.notify_all();
	}
	delete w;
}
}  // namespace

int br_net_remote_fetch_sync( const char *url, const char *method, const void *body, unsigned body_len,
			      const char *req_headers, BrNetAnswer *a, std::string *data, int timeout_ms,
			      const volatile int *cancel )
{
	auto w = std::make_shared<SyncWait>();
	/* answered on the answers' thread, which never waits for the asker */
	if ( !br_net_remote_fetch(url, method, body, body_len, req_headers, nullptr, &sync_done,
				  new std::shared_ptr<SyncWait>(w), g_answers->task_runner()) ) {
		return -1;
	}
	std::unique_lock<std::mutex> hold(w->m);
	for ( int waited = 0; !w->done; waited += 100 ) {
		if ( *cancel ) {
			printf("browser: net fetch: %s given up (stopped)\n", url);
			return 0;
		}
		if ( waited >= timeout_ms ) {
			printf("browser: net fetch: %s not in after %d ms\n", url, timeout_ms);
			return 0;
		}
		w->cv.wait_for(hold, std::chrono::milliseconds(100));
	}
	*a = w->a;
	*data = std::move(w->data);
	return a->ok;
}

int br_net_remote_cookie_line( const char *url, int for_script, char *out, int cap )
{
	if ( cap <= 0 ) return 0;
	out[0] = 0;
	std::string line;
	mojo::SharedRemote<mj::Network> n = the_net();
	if ( !n || url == nullptr || !n->CookieLine(url, for_script != 0, &line) ) return 0;
	int len = (int)std::min<size_t>(line.size(), (size_t)cap - 1);
	memcpy(out, line.data(), (size_t)len);
	out[len] = 0;
	return len;
}

void br_net_remote_cookie_set( const char *url, const char *line, int from_script )
{
	mojo::SharedRemote<mj::Network> n = the_net();
	if ( n && url != nullptr && line != nullptr ) n->CookieSet(url, line, from_script != 0);
}

bool br_net_remote_cookies( const GURL &url, std::vector<std::string> *out )
{
	std::vector<std::vector<uint8_t>> all;
	mojo::SharedRemote<mj::Network> n = the_net();
	if ( !n || !n->Cookies(url.spec(), &all) ) return false;
	for ( const auto &b : all ) out->push_back(str_of(b));
	return true;
}

bool br_net_remote_cookie_put( const std::string &bytes, const GURL &url )
{
	bool stored = false;
	mojo::SharedRemote<mj::Network> n = the_net();
	return n && n->CookiePut(bytes_of(bytes.data(), bytes.size()), url.spec(), &stored) && stored;
}

int br_net_remote_cookie_watch( const GURL &url, base::RepeatingCallback<void( const net::CookieChangeInfo & )> told )
{
	if ( !url.is_valid() || g_answers == nullptr ) return 0;
	auto id = std::make_shared<int>(0);
	auto wait = std::make_shared<std::pair<std::mutex, std::condition_variable>>();
	auto made = std::make_shared<bool>(false);
	g_answers->task_runner()->PostTask(FROM_HERE, base::BindOnce([]( GURL url,
		base::RepeatingCallback<void( const net::CookieChangeInfo & )> told, std::shared_ptr<int> id,
		decltype(wait) wait, std::shared_ptr<bool> made ) {
		int w = ++g_next_watch;
		Watch &k = watches()[w];
		k.url = url;
		k.told = std::move(told);
		watch_on(w);
		std::lock_guard<std::mutex> hold(wait->first);
		*id = w;
		*made = true;
		wait->second.notify_all();
	}, url, std::move(told), id, wait, made));
	std::unique_lock<std::mutex> hold(wait->first);
	wait->second.wait(hold, [&] { return *made; });
	return *id;
}

void br_net_remote_cookie_unwatch( int watch )
{
	if ( g_answers == nullptr ) return;
	g_answers->task_runner()->PostTask(FROM_HERE, base::BindOnce([]( int w ) { watches().erase(w); }, watch));
}

void br_net_remote_hsts_add( const std::string &host )
{
	mojo::SharedRemote<mj::Network> n = the_net();
	if ( n ) n->HstsAdd(host);
}

bool br_net_remote_hsts_known( const std::string &host )
{
	bool known = false;
	mojo::SharedRemote<mj::Network> n = the_net();
	return n && n->HstsKnown(host, &known) && known;
}

void br_net_remote_clear( const BrClear &c )
{
	mojo::SharedRemote<mj::Network> n = the_net();
	if ( n ) (void)n->Clear(c.kinds, c.since_us, c.site);
}

void br_net_remote_sites( std::set<std::string> *out )
{
	std::vector<std::string> s;
	mojo::SharedRemote<mj::Network> n = the_net();
	if ( n && n->Sites(&s) ) out->insert(s.begin(), s.end());
}

int br_net_remote_protocols( char *out, unsigned cap )
{
	std::string text;
	mojo::SharedRemote<mj::Network> n = the_net();
	if ( cap == 0 ) return 0;
	if ( !n || !n->Protocols(&text) ) text.clear();
	size_t len = std::min<size_t>(text.size(), cap - 1);
	memcpy(out, text.data(), len);
	out[len] = 0;
	return (int)len;
}

bool br_net_remote_websocket( const std::string &url, const std::vector<std::string> &protocols,
			      const std::string &origin, mojo::PendingReceiver<br::mojom::NetSocket> socket,
			      mojo::PendingRemote<br::mojom::NetSocketClient> client )
{
	mojo::SharedRemote<mj::Network> n = the_net();
	if ( !n ) return false;
	n->WebSocket(url, protocols, origin, std::move(socket), std::move(client));
	return true;
}

bool br_net_remote_flush( int ms )
{
	bool done = false;
	mojo::SharedRemote<mj::Network> n = the_net();
	return n && n->Flush(ms, &done) && done;
}

/* ================================================================ the network process's functions */

bool br_net_hook_rules( int owner, const BrExtReq &q, int *act, std::string *redirect )
{
	mojo::SharedRemote<mj::NetHooks> h = hooks_of(owner);
	return h && h->Rules(to_mojo(q), act, redirect);
}

bool br_net_hook_headers( int owner, const BrExtReq &q, bool response, std::vector<BrExtHeader> *ops )
{
	std::vector<mj::NetExtHeaderPtr> got;
	mojo::SharedRemote<mj::NetHooks> h = hooks_of(owner);
	if ( !h || !h->HeaderRules(to_mojo(q), response, &got) ) return false;
	for ( const mj::NetExtHeaderPtr &g : got ) {
		BrExtHeader e;
		e.op = (char)g->op;
		e.name = g->name;
		e.value = g->value;
		ops->push_back(std::move(e));
	}
	return true;
}

void br_net_hook_web( int owner, const char *event, long long id, const BrExtReq &q, int status, const std::string &more )
{
	mojo::SharedRemote<mj::NetHooks> h = hooks_of(owner);
	if ( h ) h->Web(event, id, to_mojo(q), status, more);
}

bool br_net_hook_serve( int owner, const BrExtReq &q, int *act, BrExtAns *ans )
{
	std::vector<uint8_t> body;
	mojo::SharedRemote<mj::NetHooks> h = hooks_of(owner);
	if ( !h || !h->Serve(to_mojo(q), act, &ans->status, &ans->ctype, &ans->headers, &body) ) return false;
	ans->body = str_of(body);
	return true;
}

/* A network process's life: it never returns */
void br_network_main()
{
	br_base_up();
	br_start_mark("the base and Mojo");
	int fd = br_sys_take(30000);
	if ( fd < 0 ) {
		printf("browser: network: no channel was handed to it\n");
		_exit(1);
	}
	static base::SingleThreadTaskExecutor main_executor;
	g_main_runner = main_executor.task_runner();
	long t0 = mono_ms();
	/* the stack and what it keeps, read back, and the network thread */
	br_net_prepare();
	br_start_mark("the network");
	g_calls = new base::Thread("network calls");
	if ( !g_calls->Start() ) _exit(2);
	mojo::IncomingInvitation inv = mojo::IncomingInvitation::Accept(
		mojo::PlatformChannelEndpoint(mojo::PlatformHandle(base::ScopedFD(fd))));
	mojo::ScopedMessagePipeHandle sp = inv.ExtractMessagePipe(br::mojom::kNetworkServicePipe);
	g_calls->task_runner()->PostTask(FROM_HERE, base::BindOnce([]( mojo::ScopedMessagePipeHandle h ) {
		g_networks = new mojo::UniqueReceiverSet<mj::Network>;
		auto *recv = new mojo::Receiver<mj::NetworkService>(new ServiceImpl,
								    mojo::PendingReceiver<mj::NetworkService>(std::move(h)));
		recv->set_disconnect_handler(base::BindOnce([] {
			g_main_runner->PostTask(FROM_HERE, base::BindOnce([] { end_network("the browser has gone"); }));
		}));
	}, std::move(sp)));
	/* the system's objects of the start opened: from here on the sockets and the stores' objects */
	br_sandbox_network();
	printf("browser: network: ready in %ld ms\n", mono_ms() - t0);
	/* the stores saved when due: the cookies' on a thread of their own, the others from here */
	base::RepeatingTimer ticks;
	ticks.Start(FROM_HERE, base::Seconds(1), base::BindRepeating([] { br_store_tick(); }));
	base::RunLoop().Run();
	end_network("its loop ended");
}
