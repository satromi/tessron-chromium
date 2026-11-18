/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	store_service_tessronos.cc
 *	ブラウザ: the stores of the pages' storage, opened, read and written
 *	by the network process for the renderers (design 14.8, 14.13.8).
 *
 *	A renderer keeps the pages' storage in memory (localStorage, IndexedDB,
 *	Cache Storage, the service workers, the permissions ...) and saves what
 *	changed through br_store.h, as the browser of one process does. The
 *	objects the stores are kept in it does not open: the lower layer of
 *	br_store.cc asks the network process instead (br.mojom.Store), the bytes
 *	kept and the changes to be written going over the pipe. The renderer can
 *	then be shut out of the objects (TS_SBX_OBJ, 14.13.9).
 *
 *	The passwords and passkeys ("creds") and the addresses and cards
 *	("autofill") never go over the pipe as bytes. For the passwords and
 *	passkeys a renderer asks for operations (br_cred_core.h), answered only
 *	for an origin of the site the browser gave that renderer
 *	(NetworkService.SetSite): what a renderer says of itself is not taken.
 *	The addresses and cards are the browser's (Host.AutofillChoose).
 *
 *	The extensions' list, their packages and the state kept of them are
 *	read and written here too. A package is read only when it is one of
 *	the list or one the browser named (NetworkService.ExtAllow: the object
 *	the user chose, or a test's line), and only such an object is linked
 *	to the list: the pipe is no way to read another object by its UUID.
 *
 *	In the network process each renderer's pipe is answered on a thread of
 *	the stores' own ("store calls"): a store's object another process holds
 *	is waited for up to 30 seconds, which would hold up the network's calls.
 *
 *	In a renderer, the calls are made on a thread of their own ("store
 *	asks"), the pipe bound there: the stores are saved from threads of the
 *	port that have no sequence of Mojo's, where a synchronous call cannot
 *	wait. When the network process has gone, the calls fail (the stores
 *	are not read, the saves are tried again later) until the browser gives
 *	a pipe to the new one (Control.NetworkAgain).
 */

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/thread.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/unique_receiver_set.h"
#include "third_party/boringssl/src/include/openssl/mem.h"

#include "application/browser/mojom/br_process.mojom.h"

#include "../ext/br_ext.h"
#include "br_cred_core.h"
#include "br_store_core.h"

namespace {

namespace mj = br::mojom;

std::vector<uint8_t> bytes_of( const std::string &s )
{
	const uint8_t *b = reinterpret_cast<const uint8_t *>(s.data());
	return std::vector<uint8_t>(b, b + s.size());
}

std::string str_of( const std::vector<uint8_t> &v )
{
	return std::string(reinterpret_cast<const char *>(v.data()), v.size());
}

void wipe( std::string *s )
{
	if ( !s->empty() ) OPENSSL_cleanse(&( *s )[0], s->size());
}

void wipe( std::vector<uint8_t> *v )
{
	if ( !v->empty() ) OPENSSL_cleanse(v->data(), v->size());
}

long now_ms()
{
	return (long)std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

/* FNV-1a over bytes, continued from h */
uint64_t fnv( uint64_t h, const std::string &s )
{
	for ( unsigned char c : s ) {
		h ^= c;
		h *= 0x100000001b3ULL;
	}
	return h;
}

/* The bytes an array<uint8> of n takes in a message: its header and its bytes, to 8 */
size_t arr_bytes( size_t n )
{
	return 8 + ( ( n + 7 ) & ~(size_t)7 );
}

/* ================================================================ the network process's side */

base::Thread				*g_calls;	/* "store calls" */
mojo::UniqueReceiverSet<mj::Store>	*g_stores;	/* on g_calls */

/* Each renderer's site, as the browser said it; the extensions' objects the browser named */
std::mutex				g_site_lock;
std::condition_variable			g_site_said;
std::map<int, std::string>		g_sites;
std::set<std::string>			g_ext_allowed;

/*
 * A store a renderer may not ask for, said on the console: the network's
 * own, and the ones kept sealed, whose bytes stay here
 */
bool refused( const std::string &name )
{
	if ( br_store_for_pages(name.c_str()) && name != "creds" && name != "autofill" ) return false;
	printf("browser: store: a renderer asked for the store \"%.40s\": refused\n", name.c_str());
	return true;
}

class StoreImpl final : public mj::Store {
public:
	explicit StoreImpl( int renderer ) : renderer_(renderer) {}

	/*
	 * The pipe gone with a large answer the last thing on it: what was
	 * answered, to be set beside what the renderer said (br_proc.cc, "found
	 * a bad message"). A renderer that refused an answer asks nothing more
	 * here (its calls fail on the pipe Mojo closed), while one that took it
	 * goes on to read its other stores and to save.
	 */
	~StoreImpl() override
	{
		if ( big_bytes_ == 0 || calls_ != big_call_ ) return;
		printf("browser: store: renderer %d's pipe closed %ld ms after the answer of %s(%s): rc %d, %zu entries, "
		       "about %zu bytes, FNV %016llx\n", renderer_, now_ms() - big_at_, big_what_, big_name_.c_str(), big_rc_,
		       big_n_, big_bytes_, (unsigned long long)big_hash_);
	}

	void Load( const std::string &name, bool once, LoadCallback cb ) override
	{
		std::string got;
		uint64_t live = 0;
		calls_++;
		int rc = refused(name) ? -1 : br_store_core_load(name.c_str(), once, &got, &live);
		note("Load", name, rc, 1, 32 + arr_bytes(got.size()), fnv(0xcbf29ce484222325ULL, got));
		std::vector<uint8_t> out = bytes_of(got);
		wipe(&got);
		std::move(cb).Run(rc, std::move(out), live);
	}

	void LoadEntries( const std::string &name, bool once, LoadEntriesCallback cb ) override
	{
		BrStoreEntries e;
		uint64_t live = 0;
		calls_++;
		int rc = refused(name) ? -1 : br_store_core_entries(name.c_str(), once, &e, &live);
		{
			size_t sz = 32 + 8;
			uint64_t h = 0xcbf29ce484222325ULL;
			for ( const auto &kv : e ) {
				sz += 8 + 32 + arr_bytes(kv.first.size()) + arr_bytes(kv.second.size());
				h = fnv(fnv(h, kv.first), kv.second);
			}
			note("LoadEntries", name, rc, e.size(), sz, h);
		}
		std::vector<mj::StoreOpPtr> out;
		out.reserve(e.size());
		for ( auto &kv : e ) {
			out.push_back(mj::StoreOp::New(1, bytes_of(kv.first), bytes_of(kv.second)));
			wipe(&kv.second);
		}
		std::move(cb).Run(rc, std::move(out), live);
	}

	void Version( const std::string &name, VersionCallback cb ) override
	{
		uint64_t v = 0;
		calls_++;
		bool ok = !refused(name) && br_store_core_version(name.c_str(), &v);
		std::move(cb).Run(ok, v);
	}

	void Save( const std::string &name, const std::vector<uint8_t> &bytes, SaveCallback cb ) override
	{
		std::string b = str_of(bytes);
		calls_++;
		int rc = refused(name) ? -1 : br_store_core_save(name.c_str(), b);
		wipe(&b);
		std::move(cb).Run(rc);
	}

	void Apply( const std::string &name, std::vector<mj::StoreOpPtr> ops, bool whole, bool afresh, uint64_t live,
		    ApplyCallback cb ) override
	{
		std::vector<BrStoreOp> d;
		calls_++;
		d.reserve(ops.size());
		for ( auto &op : ops ) {
			if ( !op ) continue;
			d.push_back({ op->kind, str_of(op->key), str_of(op->val) });
			wipe(&op->val);
		}
		bool fresh = false;
		long rc = refused(name) ? -1 : br_store_core_apply(name.c_str(), &d, whole, afresh, &live, &fresh);
		for ( BrStoreOp &op : d ) wipe(&op.val);
		std::move(cb).Run(rc, fresh, live);
	}

	/* ---------------------------------------------------------------- the passwords and passkeys */

	void PasswordsFor( const std::string &origin, PasswordsForCallback cb ) override
	{
		std::vector<mj::SavedPasswordPtr> out;
		if ( of_site(origin, "the passwords") ) {
			for ( BrSavedPassword &p : br_cred_here_passwords(origin) ) {
				out.push_back(mj::SavedPassword::New(p.id, p.name, p.password));
				wipe(&p.password);
			}
		}
		std::move(cb).Run(std::move(out));
	}

	void PasswordKeep( const std::string &origin, mj::SavedPasswordPtr p, PasswordKeepCallback cb ) override
	{
		bool kept = false;
		if ( p && of_site(origin, "a password kept") ) {
			BrSavedPassword s{ p->id, p->name, p->password };
			kept = br_cred_here_password_keep(origin, s);
			wipe(&s.password);
			wipe(&p->password);
		}
		std::move(cb).Run(kept);
	}

	void PasskeysFor( const std::string &origin, const std::string &rp, PasskeysForCallback cb ) override
	{
		std::vector<mj::PasskeyFoundPtr> out;
		if ( of_site(origin, "the passkeys") ) {
			for ( const BrPasskeyFound &k : br_cred_here_passkeys(origin, rp) ) {
				out.push_back(mj::PasskeyFound::New(bytes_of(k.id), k.user_name));
			}
		}
		std::move(cb).Run(std::move(out));
	}

	void PasskeyMake( const std::string &origin, const std::string &rp, const std::vector<uint8_t> &user_id,
			  const std::string &user_name, const std::vector<std::vector<uint8_t>> &exclude,
			  PasskeyMakeCallback cb ) override
	{
		BrPasskeyMade m;
		int rc = BR_CRED_REFUSED;
		if ( of_site(origin, "a passkey made") ) {
			std::vector<std::string> ex;
			for ( const auto &x : exclude ) ex.push_back(str_of(x));
			rc = br_cred_here_passkey_make(origin, rp, str_of(user_id), user_name, ex, &m);
		}
		std::move(cb).Run(rc, bytes_of(m.id), bytes_of(m.auth_data), bytes_of(m.attestation), bytes_of(m.spki));
	}

	void PasskeySign( const std::string &origin, const std::string &rp, const std::vector<uint8_t> &id,
			  const std::string &client_data, PasskeySignCallback cb ) override
	{
		BrPasskeySigned s;
		int rc = BR_CRED_REFUSED;
		if ( of_site(origin, "a sign-in with a passkey") ) {
			rc = br_cred_here_passkey_sign(origin, rp, str_of(id), client_data, &s);
		}
		std::move(cb).Run(rc, bytes_of(s.auth_data), bytes_of(s.signature), bytes_of(s.user_handle));
	}

	/* ---------------------------------------------------------------- the extensions */

	void ExtList( ExtListCallback cb ) override { std::move(cb).Run(true, br_ext_os_links()); }

	void ExtRead( const std::string &uuid, ExtReadCallback cb ) override
	{
		std::string bytes, name;
		bool ok = ext_may_read(uuid) && br_ext_os_read(uuid, &bytes, &name);
		std::move(cb).Run(ok, bytes_of(bytes));
	}

	void ExtState( ExtStateCallback cb ) override
	{
		std::string json;
		bool ok = br_ext_os_state_read(&json);
		std::move(cb).Run(ok, json);
	}

	void ExtStateWrite( const std::string &json, ExtStateWriteCallback cb ) override
	{
		std::move(cb).Run(br_ext_os_state_write(json));
	}

	void ExtLink( const std::string &uuid, const std::string &name, ExtLinkCallback cb ) override
	{
		bool allowed = ext_named(uuid);
		if ( !allowed ) printf("browser: store: renderer %d asked to link %.40s to the extensions: refused\n", renderer_,
				       uuid.c_str());
		std::move(cb).Run(allowed && br_ext_os_link(uuid, name));
	}

	void ExtUnlink( const std::string &uuid, ExtUnlinkCallback cb ) override
	{
		std::vector<std::string> list = br_ext_os_links();
		bool listed = std::find(list.begin(), list.end(), uuid) != list.end();
		std::move(cb).Run(listed && br_ext_os_unlink(uuid));
	}

private:
	/* A large answer kept in mind (64KB and more), for the pipe closing with no call after it (~StoreImpl) */
	void note( const char *what, const std::string &name, int rc, size_t n, size_t bytes, uint64_t hash )
	{
		if ( bytes < 64 * 1024 ) return;
		big_what_ = what;
		big_name_ = name.substr(0, 40);
		big_rc_ = rc;
		big_n_ = n;
		big_bytes_ = bytes;
		big_hash_ = hash;
		big_at_ = now_ms();
		big_call_ = calls_;
	}

	/*
	 * Whether 'origin' is of the renderer's site. A renderer the browser
	 * has given no site yet (it is being given one for the page it is to
	 * show) is waited for a little; one that keeps none is refused.
	 */
	bool of_site( const std::string &origin, const char *what )
	{
		std::string want = br_cred_site_of(origin), have;
		if ( !want.empty() ) {
			std::unique_lock<std::mutex> hold(g_site_lock);
			(void)g_site_said.wait_for(hold, std::chrono::seconds(2), [this] {
				auto it = g_sites.find(renderer_);
				return it != g_sites.end() && !it->second.empty();
			});
			auto it = g_sites.find(renderer_);
			if ( it != g_sites.end() ) have = it->second;
		}
		if ( !want.empty() && want == have ) return true;
		printf("browser: store: renderer %d of the site %s asked for %s of %.120s: refused\n", renderer_,
		       have.empty() ? "(none)" : have.c_str(), what, origin.c_str());
		return false;
	}

	/*
	 * Whether the browser named the object. Its word comes on its own pipe
	 * and may come a little after the renderer was asked to read the object:
	 * it is waited for a moment.
	 */
	bool ext_named( const std::string &uuid )
	{
		std::unique_lock<std::mutex> hold(g_site_lock);
		return g_site_said.wait_for(hold, std::chrono::seconds(1), [&] { return g_ext_allowed.count(uuid) != 0; });
	}

	bool ext_may_read( const std::string &uuid )
	{
		std::vector<std::string> list = br_ext_os_links();
		if ( std::find(list.begin(), list.end(), uuid) != list.end() || ext_named(uuid) ) return true;
		printf("browser: store: renderer %d asked for the object %.40s as an extension: refused\n", renderer_,
		       uuid.c_str());
		return false;
	}

	int	renderer_;
	const char	*big_what_ = "";
	std::string	big_name_;
	int		big_rc_ = 0;
	size_t		big_n_ = 0, big_bytes_ = 0;
	uint64_t	big_hash_ = 0;
	long		big_at_ = 0;
	unsigned	calls_ = 0, big_call_ = 0;	/* the store's calls answered, and which the large one was */
};

/* ================================================================ the renderer's side */

std::atomic<bool>		g_remote{false};
base::Thread			*g_asks;	/* "store asks" */
mojo::Remote<mj::Store>		*g_store;	/* on g_asks */

/* 'f' run on the asks' thread, where the pipe is, and waited for: false when there is no pipe yet */
bool on_asks( const std::function<void( mojo::Remote<mj::Store> & )> &f )
{
	if ( g_asks == nullptr ) return false;
	struct Wait {
		std::mutex		m;
		std::condition_variable	cv;
		bool			done = false;
		const std::function<void( mojo::Remote<mj::Store> & )> *f;
	} w;
	w.f = &f;
	g_asks->task_runner()->PostTask(FROM_HERE, base::BindOnce([]( Wait *w ) {
		( *w->f )(*g_store);
		std::lock_guard<std::mutex> hold(w->m);
		w->done = true;
		w->cv.notify_one();
	}, base::Unretained(&w)));
	std::unique_lock<std::mutex> hold(w.m);
	w.cv.wait(hold, [&w] { return w.done; });
	return true;
}

/*
 * The first call to the stores that failed on the pipe it had (Mojo
 * closed it, or the network process went): which, on what, and after how
 * long, to be set beside the network process's line of the same (~StoreImpl)
 */
void call_failed( const char *what, const char *name, long since, mojo::Remote<mj::Store> &s )
{
	static std::atomic<bool> told{false};
	if ( told.exchange(true) ) return;
	printf("browser: renderer: the call %s(%.40s) to the stores failed after %ld ms (the pipe %s)\n", what, name,
	       now_ms() - since, s.is_connected() ? "still there" : "closed");
}

std::vector<mj::StoreOpPtr> ops_of( const std::vector<BrStoreOp> &ops )
{
	std::vector<mj::StoreOpPtr> out;
	out.reserve(ops.size());
	for ( const BrStoreOp &op : ops ) out.push_back(mj::StoreOp::New(op.kind, bytes_of(op.key), bytes_of(op.val)));
	return out;
}

}  // namespace

/* ================================================================ the network process's functions */

void br_store_serve( int renderer, mojo::PendingReceiver<br::mojom::Store> store )
{
	if ( !store.is_valid() ) return;
	{
		/* a new renderer of the number, or the same after this process was started again: no site until told */
		std::lock_guard<std::mutex> hold(g_site_lock);
		g_sites.erase(renderer);
	}
	if ( g_calls == nullptr ) {
		g_calls = new base::Thread("store calls");
		if ( !g_calls->Start() ) {
			printf("browser: network: the thread of the stores did not start\n");
			delete g_calls;
			g_calls = nullptr;
			return;
		}
	}
	g_calls->task_runner()->PostTask(FROM_HERE, base::BindOnce([]( int renderer, mojo::PendingReceiver<br::mojom::Store> r ) {
		if ( g_stores == nullptr ) g_stores = new mojo::UniqueReceiverSet<mj::Store>;
		g_stores->Add(std::make_unique<StoreImpl>(renderer), std::move(r));
	}, renderer, std::move(store)));
}

void br_store_site( int renderer, const std::string &site )
{
	std::lock_guard<std::mutex> hold(g_site_lock);
	g_sites[renderer] = site;
	g_site_said.notify_all();
	printf("browser: store: renderer %d is of the site %s\n", renderer, site.c_str());
}

void br_store_ext_allow( const std::string &uuid )
{
	std::lock_guard<std::mutex> hold(g_site_lock);
	g_ext_allowed.insert(uuid);
	g_site_said.notify_all();
}

/* ================================================================ the renderer's functions */

bool br_store_remote( void )
{
	return g_remote.load(std::memory_order_acquire);
}

/* The renderer's pipe to the stores: at its start, and again when the network process was started again */
void br_store_remote_attach( mojo::PendingRemote<br::mojom::Store> pipe )
{
	if ( g_asks == nullptr ) {
		g_asks = new base::Thread("store asks");
		if ( !g_asks->Start() ) {
			/* the stores are then neither read nor written: no object is opened here instead */
			printf("browser: renderer: the thread for the stores did not start\n");
			g_asks = nullptr;
			g_remote = true;
			return;
		}
	}
	bool again = g_remote.load();
	g_asks->task_runner()->PostTask(FROM_HERE, base::BindOnce([]( mojo::PendingRemote<br::mojom::Store> p ) {
		if ( g_store == nullptr ) g_store = new mojo::Remote<mj::Store>;
		g_store->reset();
		g_store->Bind(std::move(p));
	}, std::move(pipe)));
	g_remote = true;
	if ( again ) printf("browser: renderer: a pipe to the stores started again\n");
}

int br_store_remote_load( const char *name, bool once, std::string *bytes, uint64_t *live )
{
	int32_t rc = -2;
	std::vector<uint8_t> got;
	*live = 0;
	(void)on_asks([&]( mojo::Remote<mj::Store> &s ) {
		long t = now_ms();
		bool was = s.is_bound() && s.is_connected();
		if ( !s.is_bound() || !s->Load(name, once, &rc, &got, live) ) {
			rc = -2;
			if ( was ) call_failed("Load", name, t, s);
		}
	});
	if ( rc == 0 ) *bytes = str_of(got);
	wipe(&got);
	return rc;
}

int br_store_remote_entries( const char *name, bool once, BrStoreEntries *out, uint64_t *live )
{
	int32_t rc = -2;
	std::vector<mj::StoreOpPtr> got;
	*live = 0;
	out->clear();
	(void)on_asks([&]( mojo::Remote<mj::Store> &s ) {
		long t = now_ms();
		bool was = s.is_bound() && s.is_connected();
		if ( !s.is_bound() || !s->LoadEntries(name, once, &rc, &got, live) ) {
			rc = -2;
			if ( was ) call_failed("LoadEntries", name, t, s);
		}
	});
	if ( rc == 0 ) {
		for ( mj::StoreOpPtr &e : got ) {
			if ( !e ) continue;
			( *out )[str_of(e->key)] = str_of(e->val);
			wipe(&e->val);
		}
	}
	return rc;
}

bool br_store_remote_version( const char *name, uint64_t *version )
{
	bool ok = false;
	(void)on_asks([&]( mojo::Remote<mj::Store> &s ) {
		if ( !s.is_bound() || !s->Version(name, &ok, version) ) ok = false;
	});
	return ok;
}

int br_store_remote_save( const char *name, const std::string &bytes )
{
	int32_t rc = -2;
	std::vector<uint8_t> b = bytes_of(bytes);
	(void)on_asks([&]( mojo::Remote<mj::Store> &s ) {
		if ( !s.is_bound() || !s->Save(name, b, &rc) ) rc = -2;
	});
	wipe(&b);
	return rc;
}

long br_store_remote_apply( const char *name, const std::vector<BrStoreOp> &ops, bool whole, bool afresh,
			    uint64_t *live, bool *fresh )
{
	int64_t written = -2;
	uint64_t now = *live;
	*fresh = false;
	std::vector<mj::StoreOpPtr> m = ops_of(ops);
	(void)on_asks([&]( mojo::Remote<mj::Store> &s ) {
		long t = now_ms();
		bool was = s.is_bound() && s.is_connected();
		if ( !s.is_bound() || !s->Apply(name, std::move(m), whole, afresh, *live, &written, fresh, &now) ) {
			written = -2;
			if ( was ) call_failed("Apply", name, t, s);
		}
	});
	if ( written >= 0 ) *live = now;
	return (long)written;
}

/* ---------------------------------------------------------------- the passwords and passkeys, asked */

std::vector<BrSavedPassword> br_store_remote_passwords( const std::string &origin )
{
	std::vector<mj::SavedPasswordPtr> got;
	std::vector<BrSavedPassword> out;
	(void)on_asks([&]( mojo::Remote<mj::Store> &s ) {
		if ( !s.is_bound() || !s->PasswordsFor(origin, &got) ) got.clear();
	});
	for ( mj::SavedPasswordPtr &p : got ) {
		if ( !p ) continue;
		out.push_back({ p->id, p->name, p->password });
		wipe(&p->password);
	}
	return out;
}

bool br_store_remote_password_keep( const std::string &origin, const BrSavedPassword &p )
{
	bool kept = false;
	(void)on_asks([&]( mojo::Remote<mj::Store> &s ) {
		if ( !s.is_bound() || !s->PasswordKeep(origin, mj::SavedPassword::New(p.id, p.name, p.password), &kept) ) {
			kept = false;
		}
	});
	return kept;
}

std::vector<BrPasskeyFound> br_store_remote_passkeys( const std::string &origin, const std::string &rp )
{
	std::vector<mj::PasskeyFoundPtr> got;
	std::vector<BrPasskeyFound> out;
	(void)on_asks([&]( mojo::Remote<mj::Store> &s ) {
		if ( !s.is_bound() || !s->PasskeysFor(origin, rp, &got) ) got.clear();
	});
	for ( const mj::PasskeyFoundPtr &k : got ) {
		if ( k ) out.push_back({ str_of(k->id), k->user_name });
	}
	return out;
}

int br_store_remote_passkey_make( const std::string &origin, const std::string &rp, const std::string &user_id,
				  const std::string &user_name, const std::vector<std::string> &exclude,
				  BrPasskeyMade *out )
{
	int32_t rc = BR_CRED_FAILED;
	std::vector<uint8_t> id, ad, att, pub;
	std::vector<std::vector<uint8_t>> ex;
	for ( const std::string &x : exclude ) ex.push_back(bytes_of(x));
	(void)on_asks([&]( mojo::Remote<mj::Store> &s ) {
		if ( !s.is_bound() || !s->PasskeyMake(origin, rp, bytes_of(user_id), user_name, ex, &rc, &id, &ad, &att, &pub) ) {
			rc = BR_CRED_FAILED;
		}
	});
	out->id = str_of(id);
	out->auth_data = str_of(ad);
	out->attestation = str_of(att);
	out->spki = str_of(pub);
	return rc;
}

int br_store_remote_passkey_sign( const std::string &origin, const std::string &rp, const std::string &id,
				  const std::string &client_data, BrPasskeySigned *out )
{
	int32_t rc = BR_CRED_FAILED;
	std::vector<uint8_t> ad, sig, user;
	(void)on_asks([&]( mojo::Remote<mj::Store> &s ) {
		if ( !s.is_bound() || !s->PasskeySign(origin, rp, bytes_of(id), client_data, &rc, &ad, &sig, &user) ) {
			rc = BR_CRED_FAILED;
		}
	});
	out->auth_data = str_of(ad);
	out->signature = str_of(sig);
	out->user_handle = str_of(user);
	return rc;
}

/* ---------------------------------------------------------------- the extensions' objects, asked */

bool br_store_remote_ext_list( std::vector<std::string> *uuids )
{
	bool ok = false;
	uuids->clear();
	(void)on_asks([&]( mojo::Remote<mj::Store> &s ) {
		if ( !s.is_bound() || !s->ExtList(&ok, uuids) ) ok = false;
	});
	return ok;
}

bool br_store_remote_ext_read( const std::string &uuid, std::string *bytes )
{
	bool ok = false;
	std::vector<uint8_t> got;
	(void)on_asks([&]( mojo::Remote<mj::Store> &s ) {
		if ( !s.is_bound() || !s->ExtRead(uuid, &ok, &got) ) ok = false;
	});
	if ( ok ) *bytes = str_of(got);
	return ok;
}

bool br_store_remote_ext_state( std::string *json )
{
	bool ok = false;
	(void)on_asks([&]( mojo::Remote<mj::Store> &s ) {
		if ( !s.is_bound() || !s->ExtState(&ok, json) ) ok = false;
	});
	return ok;
}

bool br_store_remote_ext_state_write( const std::string &json )
{
	bool ok = false;
	(void)on_asks([&]( mojo::Remote<mj::Store> &s ) {
		if ( !s.is_bound() || !s->ExtStateWrite(json, &ok) ) ok = false;
	});
	return ok;
}

bool br_store_remote_ext_link( const std::string &uuid, const std::string &name )
{
	bool ok = false;
	(void)on_asks([&]( mojo::Remote<mj::Store> &s ) {
		if ( !s.is_bound() || !s->ExtLink(uuid, name, &ok) ) ok = false;
	});
	return ok;
}

bool br_store_remote_ext_unlink( const std::string &uuid )
{
	bool ok = false;
	(void)on_asks([&]( mojo::Remote<mj::Store> &s ) {
		if ( !s.is_bound() || !s->ExtUnlink(uuid, &ok) ) ok = false;
	});
	return ok;
}
