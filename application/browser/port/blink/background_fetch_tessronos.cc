/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	background_fetch_tessronos.cc
 *	ブラウザ: Background Fetch (registration.backgroundFetch), answered on
 *	the services' thread (design 14.4.6).
 *
 *	A fetch is a set of requests a service worker's registration asks to
 *	have fetched while it need not run: each is fetched in turn, on a
 *	thread of its own (br_fetch, which keeps the cookies), and what came
 *	(its status, address, type and body) is kept with the fetch. The
 *	pages and the worker watching it are told its progress and each
 *	request done. When the last is done the worker is sent
 *	backgroundfetchsuccess (every response came, with a status of 2xx)
 *	or backgroundfetchfail; it reads the responses through the
 *	registration (matchAll), as Blobs, until that event is over. A fetch
 *	aborted is sent backgroundfetchabort.
 *
 *	The desktop's notifications (br_notify.cc) say when a fetch is over;
 *	〈開く〉 on one is the worker's backgroundfetchclick. There is no place
 *	that shows icons, so none is asked for. A fetch is the browser's
 *	while it runs: one not over when the browser ends is not taken up
 *	again.
 */

#include <stdio.h>

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/time/time.h"
#include "mojo/public/cpp/bindings/generic_pending_receiver.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote_set.h"
#include "services/network/public/mojom/fetch_api.mojom-blink.h"
#include "third_party/blink/public/mojom/background_fetch/background_fetch.mojom-blink.h"
#include "third_party/blink/public/mojom/cache_storage/cache_storage.mojom-blink.h"
#include "third_party/blink/public/mojom/fetch/fetch_api_request.mojom-blink.h"
#include "third_party/blink/public/mojom/fetch/fetch_api_response.mojom-blink.h"
#include "third_party/blink/public/mojom/service_worker/service_worker.mojom-blink.h"
#include "third_party/blink/public/mojom/service_worker/service_worker_event_status.mojom-blink.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"
#include "third_party/blink/renderer/platform/blob/blob_data.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "ui/gfx/geometry/size.h"

#include "../../br.h"

void br_on_service_thread( base::OnceClosure task );				/* blob_tessronos.cc */
void br_sw_dispatch( int64_t registration_id,
		     base::OnceCallback<void( blink::mojom::blink::ServiceWorker * )> send );	/* service_worker_tessronos.cc */
void br_sw_scope_of( int64_t registration_id, base::OnceCallback<void( std::string )> done );
void br_services_note_watch();							/* services_tessronos.cc */

/* The notifications of fetches are numbered from here, apart from the pages' */
extern const int kBrBgFetchNote = 1 << 24;

namespace {

namespace bm = blink::mojom::blink;

/* One request of a fetch and what came of it */
struct Record {
	std::string	url, method;
	std::vector<std::pair<std::string, std::string>> headers;
	bool		done = false, failed = false;
	int		status = 0;
	std::string	final_url, type, body;
};

class Job;
std::map<int, Job *>	g_jobs;		/* every fetch, by its number; on the services' thread */
int			g_job_last;

/* A fetch: the registration's service of it, which pages and the worker hold */
class Job final : public bm::BackgroundFetchRegistrationService {
public:
	Job( int64_t reg, std::string id, std::string title, uint64_t total, std::vector<Record> recs )
		: serial_(++g_job_last), reg_(reg), id_(std::move(id)), title_(std::move(title)), total_(total),
		  records_(std::move(recs))
	{
		g_jobs[serial_] = this;
	}

	int serial() const { return serial_; }
	int64_t reg() const { return reg_; }
	const std::string &id() const { return id_; }
	/* still going, as far as GetRegistration and GetDeveloperIds are concerned */
	bool active() const { return !over_; }

	bm::BackgroundFetchRegistrationDataPtr Data()
	{
		return bm::BackgroundFetchRegistrationData::New(blink::String::FromUtf8(id_), 0, 0, total_, got_, result_,
								reason_);
	}

	/* The registration as a page or the worker is handed it, on a pipe of its own */
	bm::BackgroundFetchRegistrationPtr Registration()
	{
		mojo::PendingRemote<bm::BackgroundFetchRegistrationService> remote;
		receivers_.Add(this, remote.InitWithNewPipeAndPassReceiver());
		return bm::BackgroundFetchRegistration::New(Data(), std::move(remote));
	}

	/* The requests fetched one by one, off this thread */
	void Start()
	{
		std::vector<std::pair<std::string, std::string>> asks;
		for ( const Record &r : records_ ) asks.emplace_back(r.url, r.method);
		int serial = serial_;
		std::thread([serial, asks]() {
			for ( size_t i = 0; i < asks.size(); i++ ) {
				struct Ctx {
					int	serial;
					long	told = 0;
				} ctx{ serial };
				auto progress = []( void *c, long got, long ) {
					Ctx *x = (Ctx *)c;
					if ( got - x->told < 64 * 1024 ) return;
					x->told = got;
					br_on_service_thread(base::BindOnce([]( int serial, long got ) {
						auto it = g_jobs.find(serial);
						if ( it != g_jobs.end() ) it->second->Progress(got);
					}, x->serial, got));
				};
				auto *resp = new BrResponse();
				bool ok = br_fetch(asks[i].first, resp, progress, &ctx,
						   asks[i].second == "GET" ? std::string() : asks[i].second);
				bool stop = false;
				std::mutex m;
				std::condition_variable cv;
				bool answered = false;
				br_on_service_thread(base::BindOnce([]( int serial, size_t i, bool ok, BrResponse *resp, bool *stop,
									 std::mutex *m, std::condition_variable *cv,
									 bool *answered ) {
					bool more = false;
					auto it = g_jobs.find(serial);
					if ( it != g_jobs.end() ) more = it->second->Came(i, ok, resp);
					delete resp;
					std::lock_guard<std::mutex> hold(*m);
					*stop = !more;
					*answered = true;
					cv->notify_one();
				}, serial, i, ok, resp, &stop, &m, &cv, &answered));
				std::unique_lock<std::mutex> hold(m);
				cv.wait(hold, [&] { return answered; });
				if ( stop ) break;
			}
		}).detach();
	}

	/* Bytes coming for the request being fetched */
	void Progress( long got )
	{
		if ( over_ ) return;
		Tell(got_ + (uint64_t)got);
	}

	/* A request fetched (or not): kept, told; whether to go on to the next */
	bool Came( size_t i, bool ok, BrResponse *resp )
	{
		if ( over_ || i >= records_.size() ) return false;
		Record &r = records_[i];
		r.done = true;
		r.failed = !ok || !resp->error.empty() || resp->status == 0;
		r.status = resp->status;
		r.final_url = resp->url.empty() ? r.url : resp->url;
		r.type = resp->type;
		r.body = std::move(resp->body);
		got_ += r.body.size();
		printf("browser: background fetch \"%s\": %s, %d, %zu bytes\n", id_.c_str(), r.url.c_str(), r.status,
		       r.body.size());
		for ( auto &o : observers_ ) o->OnRequestCompleted(Request(r), Response(r));
		if ( r.failed ) {
			Finish(bm::BackgroundFetchResult::FAILURE, bm::BackgroundFetchFailureReason::FETCH_ERROR);
			return false;
		}
		if ( i + 1 < records_.size() ) {
			Tell(got_);
			return true;
		}
		bool good = true;
		for ( const Record &x : records_ ) {
			if ( x.status < 200 || x.status > 299 ) good = false;
		}
		Finish(good ? bm::BackgroundFetchResult::SUCCESS : bm::BackgroundFetchResult::FAILURE,
		       good ? bm::BackgroundFetchFailureReason::NONE : bm::BackgroundFetchFailureReason::BAD_STATUS);
		return false;
	}

	/* 〈開く〉 on the fetch's notification */
	void Clicked()
	{
		Dispatch(Kind::Click);
	}

	/* ---- the registration's service */
	void UpdateUI( const blink::String &title, const SkBitmap &, UpdateUICallback cb ) override
	{
		if ( !title.IsNull() ) title_ = title.Utf8();
		std::move(cb).Run(bm::BackgroundFetchError::NONE);
	}

	void Abort( AbortCallback cb ) override
	{
		if ( over_ ) {
			std::move(cb).Run(bm::BackgroundFetchError::INVALID_ID);
			return;
		}
		printf("browser: background fetch \"%s\" aborted\n", id_.c_str());
		Finish(bm::BackgroundFetchResult::FAILURE, bm::BackgroundFetchFailureReason::CANCELLED_BY_DEVELOPER);
		std::move(cb).Run(bm::BackgroundFetchError::NONE);
	}

	void MatchRequests( bm::FetchAPIRequestPtr match, bm::CacheQueryOptionsPtr options, bool all,
			    MatchRequestsCallback cb ) override
	{
		blink::Vector<bm::BackgroundFetchSettledFetchPtr> out;
		if ( !gone_ ) {
			bool ignore_search = options && options->ignore_search;
			std::string want = match ? Key(match->url.GetString().Utf8(), ignore_search) : std::string();
			for ( const Record &r : records_ ) {
				if ( match && Key(r.url, ignore_search) != want ) continue;
				out.push_back(bm::BackgroundFetchSettledFetch::New(Request(r), r.done ? Response(r) : nullptr));
				if ( !all ) break;
			}
		}
		std::move(cb).Run(std::move(out));
	}

	void AddRegistrationObserver( mojo::PendingRemote<bm::BackgroundFetchRegistrationObserver> o ) override
	{
		mojo::RemoteSetElementId e = observers_.Add(std::move(o));
		/* one that comes after the fetch went on (or ended) is told where it is at once */
		if ( got_ > 0 || over_ ) observers_.Get(e)->OnProgress(0, 0, total_, got_, result_, reason_);
		if ( gone_ ) observers_.Get(e)->OnRecordsUnavailable();
	}

private:
	enum class Kind { Success, Fail, Abort, Click };

	/* An address as requests are matched by: without its fragment, and its query when it is ignored */
	static std::string Key( std::string u, bool ignore_search )
	{
		size_t h = u.find('#');
		if ( h != std::string::npos ) u.resize(h);
		if ( ignore_search ) {
			size_t q = u.find('?');
			if ( q != std::string::npos ) u.resize(q);
		}
		return u;
	}

	static bm::FetchAPIRequestPtr Request( const Record &r )
	{
		auto q = bm::FetchAPIRequest::New();
		q->url = blink::KURL(blink::String::FromUtf8(r.url));
		q->method = blink::String::FromUtf8(r.method);
		for ( const auto &h : r.headers ) {
			q->headers.Set(blink::String::FromUtf8(h.first), blink::String::FromUtf8(h.second));
		}
		return q;
	}

	static bm::FetchAPIResponsePtr Response( const Record &r )
	{
		if ( r.failed ) return nullptr;
		auto p = bm::FetchAPIResponse::New();
		p->url_list.push_back(blink::KURL(blink::String::FromUtf8(r.final_url)));
		p->status_code = (uint16_t)r.status;
		p->status_text = ( r.status >= 200 && r.status <= 299 ) ? "OK" : "";
		p->response_type = network::mojom::FetchResponseType::kBasic;
		blink::String type = blink::String::FromUtf8(r.type);
		if ( !r.type.empty() ) {
			p->headers.Set("Content-Type", type);
			p->mime_type = type;
		}
		auto data = std::make_unique<blink::BlobData>();
		data->SetContentType(type);
		data->AppendBytes(base::as_byte_span(r.body));
		p->blob = blink::BlobDataHandle::Create(std::move(data), r.body.size());
		p->response_time = base::Time::Now();
		return p;
	}

	void Tell( uint64_t got )
	{
		for ( auto &o : observers_ ) o->OnProgress(0, 0, total_, got, result_, reason_);
	}

	/* The fetch over: told, the worker sent its event, the desktop told */
	void Finish( bm::BackgroundFetchResult result, bm::BackgroundFetchFailureReason reason )
	{
		over_ = true;
		result_ = result;
		reason_ = reason;
		Tell(got_);
		if ( reason == bm::BackgroundFetchFailureReason::CANCELLED_BY_DEVELOPER ) {
			Dispatch(Kind::Abort);
			return;
		}
		Dispatch(result == bm::BackgroundFetchResult::SUCCESS ? Kind::Success : Kind::Fail);
		std::string name = title_.empty() ? id_ : title_;
		char body[96];
		snprintf(body, sizeof(body), "%zu件、%llu KB", records_.size(), (unsigned long long)( got_ + 1023 ) / 1024);
		br_services_note_watch();
		(void)br_note_show(kBrBgFetchNote + serial_, origin_of(),
				   "「" + name + "」" + ( result == bm::BackgroundFetchResult::SUCCESS ? "を取得しました"
												      : "を取得できませんでした" ),
				   body, std::vector<std::string>(), false);
	}

	std::string origin_of() const
	{
		if ( records_.empty() ) return std::string();
		blink::KURL u(blink::String::FromUtf8(records_[0].url));
		return blink::StringView(u.Protocol()).ToString().Utf8() + "://" + blink::StringView(u.Host()).ToString().Utf8()
		       + ( u.HasPort() ? ":" + std::to_string(u.Port()) : "" );
	}

	/* The worker sent the event of 'kind'; once a success's or a failure's is over, the records go */
	void Dispatch( Kind kind )
	{
		int serial = serial_;
		br_sw_dispatch(reg_, base::BindOnce([]( bm::BackgroundFetchRegistrationPtr r, int serial, Kind kind,
							 bm::ServiceWorker *w ) {
			auto over = base::BindOnce([]( int serial, Kind kind, bm::ServiceWorkerEventStatus ) {
				br_on_service_thread(base::BindOnce([]( int serial, Kind kind ) {
					auto it = g_jobs.find(serial);
					if ( it != g_jobs.end() && ( kind == Kind::Success || kind == Kind::Fail ) ) {
						it->second->RecordsGone();
					}
				}, serial, kind));
			}, serial, kind);
			if ( w == nullptr ) {
				printf("browser: background fetch: no worker to tell\n");
				std::move(over).Run(bm::ServiceWorkerEventStatus::ABORTED);
				return;
			}
			switch ( kind ) {
			case Kind::Success:
				w->DispatchBackgroundFetchSuccessEvent(std::move(r), std::move(over));
				break;
			case Kind::Fail:
				w->DispatchBackgroundFetchFailEvent(std::move(r), std::move(over));
				break;
			case Kind::Abort:
				w->DispatchBackgroundFetchAbortEvent(std::move(r), std::move(over));
				break;
			case Kind::Click:
				w->DispatchBackgroundFetchClickEvent(std::move(r), std::move(over));
				break;
			}
		}, Registration(), serial, kind));
	}

	void RecordsGone()
	{
		gone_ = true;
		for ( auto &o : observers_ ) o->OnRecordsUnavailable();
	}

	int					serial_;
	int64_t					reg_;
	std::string				id_, title_;
	uint64_t				total_, got_ = 0;
	std::vector<Record>			records_;
	bm::BackgroundFetchResult		result_ = bm::BackgroundFetchResult::UNSET;
	bm::BackgroundFetchFailureReason	reason_ = bm::BackgroundFetchFailureReason::NONE;
	bool					over_ = false, gone_ = false;
	mojo::ReceiverSet<bm::BackgroundFetchRegistrationService> receivers_;
	mojo::RemoteSet<bm::BackgroundFetchRegistrationObserver>   observers_;
};

/* The fetch going under a developer's id for a registration, or null */
Job *Going( int64_t reg, const std::string &id )
{
	for ( auto &kv : g_jobs ) {
		if ( kv.second->reg() == reg && kv.second->id() == id && kv.second->active() ) return kv.second;
	}
	return nullptr;
}

class Service final : public bm::BackgroundFetchService {
public:
	void Bind( mojo::PendingReceiver<bm::BackgroundFetchService> r ) { receivers_.Add(this, std::move(r)); }

	void Fetch( int64_t reg, const blink::String &developer_id, blink::Vector<bm::FetchAPIRequestPtr> requests,
		    bm::BackgroundFetchOptionsPtr options, const SkBitmap &, bm::BackgroundFetchUkmDataPtr,
		    FetchCallback cb ) override
	{
		std::string id = developer_id.Utf8();
		if ( id.empty() || requests.empty() ) {
			std::move(cb).Run(bm::BackgroundFetchError::INVALID_ARGUMENT, nullptr);
			return;
		}
		std::vector<Record> recs;
		for ( const auto &q : requests ) {
			Record r;
			r.url = q->url.GetString().Utf8();
			r.method = q->method.empty() ? std::string("GET") : q->method.Utf8();
			for ( const auto &kv : q->headers ) r.headers.emplace_back(kv.key.Utf8(), kv.value.Utf8());
			recs.push_back(std::move(r));
		}
		std::string title = options ? options->title.Utf8() : std::string();
		uint64_t total = options ? options->download_total : 0;
		br_sw_scope_of(reg, base::BindOnce([]( int64_t reg, std::string id, std::string title, uint64_t total,
						       std::vector<Record> recs, FetchCallback cb, std::string scope ) {
			br_on_service_thread(base::BindOnce([]( int64_t reg, std::string id, std::string title, uint64_t total,
								std::vector<Record> recs, FetchCallback cb, std::string scope ) {
				if ( scope.empty() ) {
					std::move(cb).Run(bm::BackgroundFetchError::SERVICE_WORKER_UNAVAILABLE, nullptr);
					return;
				}
				if ( Going(reg, id) != nullptr ) {
					std::move(cb).Run(bm::BackgroundFetchError::DUPLICATED_DEVELOPER_ID, nullptr);
					return;
				}
				Job *j = new Job(reg, id, title, total, std::move(recs));
				printf("browser: background fetch \"%s\" started\n", id.c_str());
				std::move(cb).Run(bm::BackgroundFetchError::NONE, j->Registration());
				j->Start();
			}, reg, std::move(id), std::move(title), total, std::move(recs), std::move(cb), std::move(scope)));
		}, reg, std::move(id), std::move(title), total, std::move(recs), std::move(cb)));
	}

	void GetRegistration( int64_t reg, const blink::String &developer_id, GetRegistrationCallback cb ) override
	{
		Job *j = Going(reg, developer_id.Utf8());
		if ( j == nullptr ) {
			std::move(cb).Run(bm::BackgroundFetchError::NONE, nullptr);
			return;
		}
		std::move(cb).Run(bm::BackgroundFetchError::NONE, j->Registration());
	}

	void GetDeveloperIds( int64_t reg, GetDeveloperIdsCallback cb ) override
	{
		blink::Vector<blink::String> out;
		for ( auto &kv : g_jobs ) {
			if ( kv.second->reg() == reg && kv.second->active() ) out.push_back(blink::String::FromUtf8(kv.second->id()));
		}
		std::move(cb).Run(bm::BackgroundFetchError::NONE, std::move(out));
	}

	/* No place shows icons: none is asked for */
	void GetIconDisplaySize( GetIconDisplaySizeCallback cb ) override { std::move(cb).Run(gfx::Size()); }

private:
	mojo::ReceiverSet<bm::BackgroundFetchService>	receivers_;
};

Service	*g_service;

void BindService( mojo::PendingReceiver<bm::BackgroundFetchService> r )
{
	br_on_service_thread(base::BindOnce([]( mojo::PendingReceiver<bm::BackgroundFetchService> r ) {
		if ( g_service == nullptr ) g_service = new Service();
		g_service->Bind(std::move(r));
	}, std::move(r)));
}

}  // namespace

/* The binder of the frames' broker, once when the engine is up */
void br_background_fetch_init()
{
	blink::GetEmptyBrowserInterfaceBroker().SetBinderForTesting(
		bm::BackgroundFetchService::Name_, base::BindRepeating([]( mojo::ScopedMessagePipeHandle h ) {
			BindService(mojo::PendingReceiver<bm::BackgroundFetchService>(std::move(h)));
		}));
}

/* A receiver a worker's broker is given: true when it is this service */
bool br_background_fetch_bind( mojo::GenericPendingReceiver &r )
{
	if ( auto s = r.As<bm::BackgroundFetchService>() ) {
		BindService(std::move(s));
		return true;
	}
	return false;
}

/* What the desktop says came of a fetch's notification (on the services' thread) */
void br_background_fetch_note( int n, int what )
{
	auto it = g_jobs.find(n - kBrBgFetchNote);
	if ( it == g_jobs.end() ) return;
	if ( what == BR_NOTE_CLICK ) it->second->Clicked();
}
