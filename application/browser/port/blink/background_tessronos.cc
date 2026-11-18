/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	background_tessronos.cc
 *	ブラウザ: what a service worker is given to do in the background --
 *	Background Sync, Periodic Background Sync and the Content Index --
 *	answered on the services' thread (design 14.4.6).
 *
 *	  - a one-shot sync (registration.sync.register) is fired at the
 *	    registration's worker as a sync event once the page has the
 *	    promise resolved, the machine being taken as online; an event
 *	    that fails is tried again 5 and then 15 minutes later, the third
 *	    try its last chance;
 *	  - a periodic sync (registration.periodicSync.register) is fired as
 *	    a periodicsync event each time its interval, at least 12 hours
 *	    (the start-up line "periodicsync=" makes it shorter, for tests),
 *	    has passed since it was last fired or registered;
 *	  - the Content Index (registration.index) keeps what a site says it
 *	    has to read offline; there is no place that shows it, so no icon
 *	    is asked for and no contentdelete event is ever sent.
 *
 *	The pending syncs, the periodic ones and the index are kept between
 *	runs in the store "background" (record 5 of ブラウザの保存域, as
 *	JSON), by the service worker registration's id, which is kept too
 *	(service_worker_tessronos.cc). What a registration has goes with it
 *	when it is unregistered (br_background_forget).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/time/time.h"
#include "base/values.h"
#include "mojo/public/cpp/bindings/generic_pending_receiver.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "third_party/blink/public/mojom/background_sync/background_sync.mojom-blink.h"
#include "third_party/blink/public/mojom/content_index/content_index.mojom-blink.h"
#include "third_party/blink/public/mojom/service_worker/service_worker.mojom-blink.h"
#include "third_party/blink/public/mojom/service_worker/service_worker_event_status.mojom-blink.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "ui/gfx/geometry/size.h"

#include "../../br.h"
#include "../net/br_store.h"

void br_on_service_thread( base::OnceClosure task );				/* blob_tessronos.cc */
void br_net_post_delayed( base::OnceClosure work, int ms );			/* net_fetch_tessronos.cc */
void br_sw_dispatch( int64_t registration_id,
		     base::OnceCallback<void( blink::mojom::blink::ServiceWorker * )> send );	/* service_worker_tessronos.cc */
void br_sw_scope_of( int64_t registration_id, base::OnceCallback<void( std::string )> done );

/* The start-up line "periodicsync=": the shortest interval of a periodic sync, in ms; 0 the usual 12 hours */
int64_t br_periodic_sync_ms;

namespace {

namespace bm = blink::mojom::blink;

constexpr int64_t kPeriodicFloorMs = 12LL * 60 * 60 * 1000;
constexpr int kSyncTries = 3;
constexpr int kSyncFirstRetryMs = 5 * 60 * 1000;
constexpr int kSyncRetryFactor = 3;

struct OneShot {
	int64_t		reg;
	std::string	tag;
	int		tries = 0;
	bool		firing = false, again = false;
};

struct Periodic {
	int64_t		reg;
	std::string	tag;
	int64_t		interval;	/* what the page asked for, ms */
	int64_t		last;		/* when it was last fired or registered, ms since the epoch */
	bool		firing = false;
};

struct Icon {
	std::string	src;
	std::optional<std::string> sizes, type;
};

struct Content {
	std::string	id, title, description, launch_url;
	int		category = 0;
	std::vector<Icon> icons;
};

/* What the registrations have, guarded by g_lock: it is read on the store's thread and the page's too */
std::mutex			g_lock;
std::vector<OneShot>		g_syncs;
std::vector<Periodic>		g_periodic;
std::map<int64_t, std::vector<Content>> g_index;
int				g_store = -1;
bool				g_ticking;

int64_t now_ms()
{
	struct timespec t;
	clock_gettime(CLOCK_REALTIME, &t);
	return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int64_t floor_ms()
{
	return br_periodic_sync_ms > 0 ? br_periodic_sync_ms : kPeriodicFloorMs;
}

/* ---------------------------------------------------------------- the store */

int write_store( void *, unsigned char **data, int *len )
{
	base::DictValue d;
	{
		std::lock_guard<std::mutex> hold(g_lock);
		base::ListValue syncs, periodic, index;
		for ( const OneShot &s : g_syncs ) {
			base::DictValue e;
			e.Set("reg", base::NumberToString(s.reg));
			e.Set("tag", s.tag);
			e.Set("tries", s.tries);
			syncs.Append(std::move(e));
		}
		for ( const Periodic &p : g_periodic ) {
			base::DictValue e;
			e.Set("reg", base::NumberToString(p.reg));
			e.Set("tag", p.tag);
			e.Set("interval", base::NumberToString(p.interval));
			e.Set("last", base::NumberToString(p.last));
			periodic.Append(std::move(e));
		}
		for ( const auto &kv : g_index ) {
			for ( const Content &c : kv.second ) {
				base::DictValue e;
				e.Set("reg", base::NumberToString(kv.first));
				e.Set("id", c.id);
				e.Set("title", c.title);
				e.Set("description", c.description);
				e.Set("category", c.category);
				e.Set("url", c.launch_url);
				base::ListValue icons;
				for ( const Icon &i : c.icons ) {
					base::DictValue ie;
					ie.Set("src", i.src);
					if ( i.sizes ) ie.Set("sizes", *i.sizes);
					if ( i.type ) ie.Set("type", *i.type);
					icons.Append(std::move(ie));
				}
				e.Set("icons", std::move(icons));
				index.Append(std::move(e));
			}
		}
		d.Set("syncs", std::move(syncs));
		d.Set("periodic", std::move(periodic));
		d.Set("index", std::move(index));
	}
	std::string json;
	base::JSONWriter::Write(d, &json);
	*data = (unsigned char *)malloc(json.size() + 1);
	if ( *data == nullptr ) return 1;
	memcpy(*data, json.data(), json.size());
	*len = (int)json.size();
	return 0;
}

void dirty()
{
	if ( g_store >= 0 ) br_store_dirty(g_store);
}

int64_t num( const base::DictValue &d, const char *k )
{
	int64_t n = 0;
	const std::string *s = d.FindString(k);
	if ( s == nullptr || !base::StringToInt64(*s, &n) ) return 0;
	return n;
}

std::string str( const base::DictValue &d, const char *k )
{
	const std::string *s = d.FindString(k);
	return s ? *s : std::string();
}

void load_store()
{
	unsigned char *data = nullptr;
	int len = 0;
	if ( br_store_load("background", &data, &len) != 0 || data == nullptr ) {
		free(data);
		return;
	}
	std::optional<base::Value> v = base::JSONReader::Read(std::string((char *)data, (size_t)len),
							      base::JSON_PARSE_RFC);
	free(data);
	if ( !v || !v->is_dict() ) return;
	const base::DictValue &d = v->GetDict();
	std::lock_guard<std::mutex> hold(g_lock);
	if ( const base::ListValue *l = d.FindList("syncs") ) {
		for ( const base::Value &e : *l ) {
			if ( !e.is_dict() ) continue;
			OneShot s;
			s.reg = num(e.GetDict(), "reg");
			s.tag = str(e.GetDict(), "tag");
			s.tries = e.GetDict().FindInt("tries").value_or(0);
			if ( s.reg > 0 ) g_syncs.push_back(s);
		}
	}
	if ( const base::ListValue *l = d.FindList("periodic") ) {
		for ( const base::Value &e : *l ) {
			if ( !e.is_dict() ) continue;
			Periodic p;
			p.reg = num(e.GetDict(), "reg");
			p.tag = str(e.GetDict(), "tag");
			p.interval = num(e.GetDict(), "interval");
			p.last = num(e.GetDict(), "last");
			if ( p.reg > 0 ) g_periodic.push_back(p);
		}
	}
	if ( const base::ListValue *l = d.FindList("index") ) {
		for ( const base::Value &e : *l ) {
			if ( !e.is_dict() ) continue;
			const base::DictValue &x = e.GetDict();
			Content c;
			c.id = str(x, "id");
			c.title = str(x, "title");
			c.description = str(x, "description");
			c.category = x.FindInt("category").value_or(0);
			c.launch_url = str(x, "url");
			if ( const base::ListValue *icons = x.FindList("icons") ) {
				for ( const base::Value &ie : *icons ) {
					if ( !ie.is_dict() ) continue;
					Icon i;
					i.src = str(ie.GetDict(), "src");
					if ( const std::string *s = ie.GetDict().FindString("sizes") ) i.sizes = *s;
					if ( const std::string *s = ie.GetDict().FindString("type") ) i.type = *s;
					c.icons.push_back(std::move(i));
				}
			}
			int64_t reg = num(x, "reg");
			if ( reg > 0 && !c.id.empty() ) g_index[reg].push_back(std::move(c));
		}
	}
	printf("browser: background: %zu syncs, %zu periodic syncs, %zu sites' contents read back\n",
	       g_syncs.size(), g_periodic.size(), g_index.size());
}

/* ---------------------------------------------------------------- firing */

void fire_sync( int64_t reg, std::string tag );

/* A one-shot sync's event over: kept for another try, or gone */
void sync_done( int64_t reg, std::string tag, bool ok )
{
	int wait = -1;
	{
		std::lock_guard<std::mutex> hold(g_lock);
		for ( auto it = g_syncs.begin(); it != g_syncs.end(); ++it ) {
			if ( it->reg != reg || it->tag != tag ) continue;
			it->firing = false;
			if ( it->again ) {
				/* registered again while it fired: fired again */
				it->again = false;
				wait = 0;
			} else if ( ok || it->tries >= kSyncTries ) {
				if ( !ok ) printf("browser: background: the sync \"%s\" failed its last chance\n", tag.c_str());
				g_syncs.erase(it);
			} else {
				wait = kSyncFirstRetryMs;
				for ( int i = 1; i < it->tries; i++ ) wait *= kSyncRetryFactor;
			}
			break;
		}
	}
	dirty();
	if ( wait >= 0 ) {
		br_net_post_delayed(base::BindOnce([]( int64_t reg, std::string tag ) {
			br_on_service_thread(base::BindOnce(&fire_sync, reg, tag));
		}, reg, tag), wait);
	}
}

/* A one-shot sync fired at its registration's worker (on the services' thread) */
void fire_sync( int64_t reg, std::string tag )
{
	bool last = false;
	{
		std::lock_guard<std::mutex> hold(g_lock);
		OneShot *s = nullptr;
		for ( OneShot &o : g_syncs ) {
			if ( o.reg == reg && o.tag == tag ) s = &o;
		}
		if ( s == nullptr || s->firing ) return;
		s->firing = true;
		s->tries++;
		last = s->tries >= kSyncTries;
	}
	dirty();
	printf("browser: background: the sync \"%s\" fired%s\n", tag.c_str(), last ? " (its last chance)" : "");
	br_sw_dispatch(reg, base::BindOnce([]( int64_t reg, std::string tag, bool last, bm::ServiceWorker *w ) {
		if ( w == nullptr ) {
			br_on_service_thread(base::BindOnce(&sync_done, reg, tag, false));
			return;
		}
		w->DispatchSyncEvent(blink::String::FromUtf8(tag), last, base::Minutes(3),
				     base::BindOnce([]( int64_t reg, std::string tag, bm::ServiceWorkerEventStatus st ) {
			br_on_service_thread(base::BindOnce(&sync_done, reg, tag,
							    st == bm::ServiceWorkerEventStatus::COMPLETED));
		}, reg, tag));
	}, reg, tag, last));
}

/* The periodic syncs whose time has come fired; then again in a while */
void tick()
{
	std::vector<std::pair<int64_t, std::string>> due;
	int64_t now = now_ms(), next = floor_ms();
	{
		std::lock_guard<std::mutex> hold(g_lock);
		for ( Periodic &p : g_periodic ) {
			int64_t every = std::max(p.interval, floor_ms());
			int64_t left = p.last + every - now;
			if ( left <= 0 && !p.firing ) {
				p.firing = true;
				p.last = now;
				due.emplace_back(p.reg, p.tag);
				left = every;
			}
			next = std::min(next, std::max<int64_t>(left, 1000));
		}
		g_ticking = !g_periodic.empty();
	}
	if ( !due.empty() ) dirty();
	for ( auto &d : due ) {
		printf("browser: background: the periodic sync \"%s\" fired\n", d.second.c_str());
		br_sw_dispatch(d.first, base::BindOnce([]( int64_t reg, std::string tag, bm::ServiceWorker *w ) {
			auto over = [reg, tag]( bool ) {
				std::lock_guard<std::mutex> hold(g_lock);
				for ( Periodic &p : g_periodic ) {
					if ( p.reg == reg && p.tag == tag ) p.firing = false;
				}
			};
			if ( w == nullptr ) {
				over(false);
				return;
			}
			w->DispatchPeriodicSyncEvent(blink::String::FromUtf8(tag), base::Minutes(3),
						     base::BindOnce([]( std::function<void(bool)> over, bm::ServiceWorkerEventStatus st ) {
				over(st == bm::ServiceWorkerEventStatus::COMPLETED);
			}, std::function<void(bool)>(over)));
		}, d.first, d.second));
	}
	bool more;
	{
		std::lock_guard<std::mutex> hold(g_lock);
		more = g_ticking;
	}
	if ( more ) {
		br_net_post_delayed(base::BindOnce([]() { br_on_service_thread(base::BindOnce(&tick)); }), (int)next);
	}
}

/* The periodic syncs' clock started, when it is not going */
void start_ticking()
{
	{
		std::lock_guard<std::mutex> hold(g_lock);
		if ( g_ticking || g_periodic.empty() ) return;
		g_ticking = true;
	}
	br_net_post_delayed(base::BindOnce([]() { br_on_service_thread(base::BindOnce(&tick)); }), 1000);
}

/* Whether the registration is there: 'then' run on the services' thread with the answer */
void known( int64_t reg, base::OnceCallback<void( bool )> then )
{
	br_sw_scope_of(reg, base::BindOnce([]( base::OnceCallback<void( bool )> then, std::string scope ) {
		br_on_service_thread(base::BindOnce(std::move(then), !scope.empty()));
	}, std::move(then)));
}

/* ---------------------------------------------------------------- the services */

class OneShotSync final : public bm::OneShotBackgroundSyncService {
public:
	void Bind( mojo::PendingReceiver<bm::OneShotBackgroundSyncService> r ) { receivers_.Add(this, std::move(r)); }

	void Register( bm::SyncRegistrationOptionsPtr options, int64_t reg, RegisterCallback cb ) override
	{
		std::string tag = options->tag.Utf8();
		known(reg, base::BindOnce([]( int64_t reg, std::string tag, bm::SyncRegistrationOptionsPtr options,
					      RegisterCallback cb, bool there ) {
			if ( !there ) {
				std::move(cb).Run(bm::BackgroundSyncError::NO_SERVICE_WORKER, nullptr);
				return;
			}
			{
				std::lock_guard<std::mutex> hold(g_lock);
				bool found = false;
				for ( OneShot &o : g_syncs ) {
					if ( o.reg != reg || o.tag != tag ) continue;
					found = true;
					if ( o.firing ) o.again = true;
				}
				if ( !found ) {
					OneShot o;
					o.reg = reg;
					o.tag = tag;
					g_syncs.push_back(o);
				}
			}
			dirty();
			printf("browser: background: the sync \"%s\" registered\n", tag.c_str());
			std::move(cb).Run(bm::BackgroundSyncError::NONE, std::move(options));
		}, reg, tag, std::move(options), std::move(cb)));
	}

	/* The page has its promise: the sync fired now, the machine being online */
	void DidResolveRegistration( bm::BackgroundSyncRegistrationInfoPtr info ) override
	{
		fire_sync(info->service_worker_registration_id, info->tag.Utf8());
	}

	void GetRegistrations( int64_t reg, GetRegistrationsCallback cb ) override
	{
		blink::Vector<bm::SyncRegistrationOptionsPtr> out;
		{
			std::lock_guard<std::mutex> hold(g_lock);
			for ( const OneShot &o : g_syncs ) {
				if ( o.reg == reg ) out.push_back(bm::SyncRegistrationOptions::New(blink::String::FromUtf8(o.tag), -1));
			}
		}
		std::move(cb).Run(bm::BackgroundSyncError::NONE, std::move(out));
	}

private:
	mojo::ReceiverSet<bm::OneShotBackgroundSyncService>	receivers_;
};

class PeriodicSync final : public bm::PeriodicBackgroundSyncService {
public:
	void Bind( mojo::PendingReceiver<bm::PeriodicBackgroundSyncService> r ) { receivers_.Add(this, std::move(r)); }

	void Register( bm::SyncRegistrationOptionsPtr options, int64_t reg, RegisterCallback cb ) override
	{
		std::string tag = options->tag.Utf8();
		known(reg, base::BindOnce([]( int64_t reg, std::string tag, bm::SyncRegistrationOptionsPtr options,
					      RegisterCallback cb, bool there ) {
			if ( !there ) {
				std::move(cb).Run(bm::BackgroundSyncError::NO_SERVICE_WORKER, nullptr);
				return;
			}
			{
				std::lock_guard<std::mutex> hold(g_lock);
				bool found = false;
				for ( Periodic &p : g_periodic ) {
					if ( p.reg != reg || p.tag != tag ) continue;
					found = true;
					p.interval = options->min_interval;
				}
				if ( !found ) {
					Periodic p;
					p.reg = reg;
					p.tag = tag;
					p.interval = options->min_interval;
					p.last = now_ms();
					g_periodic.push_back(p);
				}
			}
			dirty();
			start_ticking();
			printf("browser: background: the periodic sync \"%s\" registered, every %lld ms\n", tag.c_str(),
			       (long long)std::max<int64_t>(options->min_interval, floor_ms()));
			std::move(cb).Run(bm::BackgroundSyncError::NONE, std::move(options));
		}, reg, tag, std::move(options), std::move(cb)));
	}

	void Unregister( int64_t reg, const blink::String &tag, UnregisterCallback cb ) override
	{
		std::string t = tag.Utf8();
		{
			std::lock_guard<std::mutex> hold(g_lock);
			for ( auto it = g_periodic.begin(); it != g_periodic.end(); ) {
				it = ( it->reg == reg && it->tag == t ) ? g_periodic.erase(it) : it + 1;
			}
		}
		dirty();
		std::move(cb).Run(bm::BackgroundSyncError::NONE);
	}

	void GetRegistrations( int64_t reg, GetRegistrationsCallback cb ) override
	{
		blink::Vector<bm::SyncRegistrationOptionsPtr> out;
		{
			std::lock_guard<std::mutex> hold(g_lock);
			for ( const Periodic &p : g_periodic ) {
				if ( p.reg == reg ) {
					out.push_back(bm::SyncRegistrationOptions::New(blink::String::FromUtf8(p.tag), p.interval));
				}
			}
		}
		std::move(cb).Run(bm::BackgroundSyncError::NONE, std::move(out));
	}

private:
	mojo::ReceiverSet<bm::PeriodicBackgroundSyncService>	receivers_;
};

class ContentIndex final : public bm::ContentIndexService {
public:
	void Bind( mojo::PendingReceiver<bm::ContentIndexService> r ) { receivers_.Add(this, std::move(r)); }

	/* No place shows the contents: no icon is asked for */
	void GetIconSizes( bm::ContentCategory, GetIconSizesCallback cb ) override
	{
		std::move(cb).Run(blink::Vector<gfx::Size>());
	}

	void Add( int64_t reg, bm::ContentDescriptionPtr description, const blink::Vector<SkBitmap> &, const blink::KURL &,
		  AddCallback cb ) override
	{
		Content c;
		c.id = description->id.Utf8();
		c.title = description->title.Utf8();
		c.description = description->description.Utf8();
		c.category = (int)description->category;
		c.launch_url = description->launch_url.Utf8();
		for ( const auto &i : description->icons ) {
			Icon icon;
			icon.src = i->src.Utf8();
			if ( !i->sizes.IsNull() ) icon.sizes = i->sizes.Utf8();
			if ( !i->type.IsNull() ) icon.type = i->type.Utf8();
			c.icons.push_back(std::move(icon));
		}
		known(reg, base::BindOnce([]( int64_t reg, Content c, AddCallback cb, bool there ) {
			if ( !there ) {
				std::move(cb).Run(bm::ContentIndexError::NO_SERVICE_WORKER);
				return;
			}
			{
				std::lock_guard<std::mutex> hold(g_lock);
				std::vector<Content> &v = g_index[reg];
				for ( auto it = v.begin(); it != v.end(); ) {
					it = ( it->id == c.id ) ? v.erase(it) : it + 1;
				}
				printf("browser: background: \"%s\" in the content index\n", c.title.c_str());
				v.push_back(std::move(c));
			}
			dirty();
			std::move(cb).Run(bm::ContentIndexError::NONE);
		}, reg, std::move(c), std::move(cb)));
	}

	void Delete( int64_t reg, const blink::String &id, DeleteCallback cb ) override
	{
		std::string i = id.Utf8();
		{
			std::lock_guard<std::mutex> hold(g_lock);
			auto f = g_index.find(reg);
			if ( f != g_index.end() ) {
				for ( auto it = f->second.begin(); it != f->second.end(); ) {
					it = ( it->id == i ) ? f->second.erase(it) : it + 1;
				}
				if ( f->second.empty() ) g_index.erase(f);
			}
		}
		dirty();
		std::move(cb).Run(bm::ContentIndexError::NONE);
	}

	void GetDescriptions( int64_t reg, GetDescriptionsCallback cb ) override
	{
		blink::Vector<bm::ContentDescriptionPtr> out;
		{
			std::lock_guard<std::mutex> hold(g_lock);
			auto f = g_index.find(reg);
			if ( f != g_index.end() ) {
				for ( const Content &c : f->second ) {
					blink::Vector<bm::ContentIconDefinitionPtr> icons;
					for ( const Icon &i : c.icons ) {
						icons.push_back(bm::ContentIconDefinition::New(
							blink::String::FromUtf8(i.src),
							i.sizes ? blink::String::FromUtf8(*i.sizes) : blink::String(),
							i.type ? blink::String::FromUtf8(*i.type) : blink::String()));
					}
					out.push_back(bm::ContentDescription::New(
						blink::String::FromUtf8(c.id), blink::String::FromUtf8(c.title),
						blink::String::FromUtf8(c.description), (bm::ContentCategory)c.category,
						std::move(icons), blink::String::FromUtf8(c.launch_url)));
				}
			}
		}
		std::move(cb).Run(bm::ContentIndexError::NONE, std::move(out));
	}

private:
	mojo::ReceiverSet<bm::ContentIndexService>	receivers_;
};

OneShotSync	*g_one_shot;
PeriodicSync	*g_periodic_svc;
ContentIndex	*g_content;

template <typename Interface, typename Service>
void BindOnThread( Service **svc, mojo::PendingReceiver<Interface> r )
{
	br_on_service_thread(base::BindOnce([]( Service **svc, mojo::PendingReceiver<Interface> r ) {
		if ( *svc == nullptr ) *svc = new Service();
		( *svc )->Bind(std::move(r));
	}, svc, std::move(r)));
}

template <typename Interface, typename Service>
void FrameBinder( Service **svc )
{
	blink::GetEmptyBrowserInterfaceBroker().SetBinderForTesting(
		Interface::Name_, base::BindRepeating([]( Service **svc, mojo::ScopedMessagePipeHandle h ) {
			BindOnThread<Interface, Service>(svc, mojo::PendingReceiver<Interface>(std::move(h)));
		}, svc));
}

}  // namespace

/*
 * At the start, after the service workers' registrations are read back:
 * what was kept read back, the syncs left pending fired, the periodic
 * ones' clock started, and the binders of the frames' broker set
 */
void br_background_init()
{
	g_store = br_store_register("background", &write_store, nullptr, 500);
	FrameBinder<bm::OneShotBackgroundSyncService, OneShotSync>(&g_one_shot);
	FrameBinder<bm::PeriodicBackgroundSyncService, PeriodicSync>(&g_periodic_svc);
	FrameBinder<bm::ContentIndexService, ContentIndex>(&g_content);
	br_on_service_thread(base::BindOnce([]() {
		load_store();
		std::vector<std::pair<int64_t, std::string>> pending;
		{
			std::lock_guard<std::mutex> hold(g_lock);
			for ( OneShot &o : g_syncs ) {
				o.firing = false;
				pending.emplace_back(o.reg, o.tag);
			}
		}
		for ( auto &p : pending ) {
			br_net_post_delayed(base::BindOnce([]( int64_t reg, std::string tag ) {
				br_on_service_thread(base::BindOnce(&fire_sync, reg, tag));
			}, p.first, p.second), 3000);
		}
		start_ticking();
	}));
}

/* A receiver a worker's broker is given: true when it is one of these services */
bool br_background_bind( mojo::GenericPendingReceiver &r )
{
	if ( auto s = r.As<bm::OneShotBackgroundSyncService>() ) {
		BindOnThread<bm::OneShotBackgroundSyncService, OneShotSync>(&g_one_shot, std::move(s));
		return true;
	}
	if ( auto p = r.As<bm::PeriodicBackgroundSyncService>() ) {
		BindOnThread<bm::PeriodicBackgroundSyncService, PeriodicSync>(&g_periodic_svc, std::move(p));
		return true;
	}
	if ( auto c = r.As<bm::ContentIndexService>() ) {
		BindOnThread<bm::ContentIndexService, ContentIndex>(&g_content, std::move(c));
		return true;
	}
	return false;
}

/* A service worker registration gone: its syncs and contents with it (from any thread) */
void br_background_forget( int64_t reg )
{
	{
		std::lock_guard<std::mutex> hold(g_lock);
		for ( auto it = g_syncs.begin(); it != g_syncs.end(); ) {
			it = ( it->reg == reg ) ? g_syncs.erase(it) : it + 1;
		}
		for ( auto it = g_periodic.begin(); it != g_periodic.end(); ) {
			it = ( it->reg == reg ) ? g_periodic.erase(it) : it + 1;
		}
		g_index.erase(reg);
	}
	dirty();
}
