/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	services_tessronos.cc
 *	ブラウザ: the services a browser answers a page's modules with,
 *	answered in this process (design 14.4.5).
 *
 *	- Permissions (navigator.permissions, and what Notification and
 *	  storage ask): those a browser gives a page without asking (writing
 *	  to the clipboard, the storage access of the page's own frames) are
 *	  granted; reading the clipboard is asked of the user on a panel over
 *	  the window (br_ask_permission), and the answer is kept for the
 *	  origin, also between runs (the store "perms"). A query of any other answers
 *	  "prompt" and a request of it is refused.
 *	- Notifications: their permission is asked of the user on a panel,
 *	  and the desktop shows them (port/net/br_notify.cc): a window's
 *	  notification is told when it is pressed or closed, a service
 *	  worker's notification gives its worker notificationclick and
 *	  notificationclose (br_sw_dispatch).
 *	- Web Push (PushMessaging): its permission is the notifications';
 *	  the subscriptions are the push service's channels
 *	  (port/net/push_tessronos.cc), and a message pushed is the push event
 *	  of the subscription's service worker.
 *	- The storage quota (navigator.storage.estimate()): nothing is used,
 *	  of a quota of 2 GB.
 *	- Web Locks (navigator.locks): the locks of the process, by name, held
 *	  shared or exclusive and granted in the order asked; one the holder
 *	  lets go of (its handle closed) goes to the next asker, and a steal
 *	  breaks those held.
 *	- BroadcastChannel: the channels of the process, by name; a message
 *	  is sent to every other connection of the same name, each on the
 *	  thread its connection is on.
 *	- Cache Storage: cache_storage_tessronos.cc, bound from here.
 *	- Shared workers: shared_worker_tessronos.cc, its binder given from here.
 *	- The clipboard (ClipboardHost, what copying and pasting in a page,
 *	  execCommand and navigator.clipboard use): the system's tray
 *	  (port/net/br_tray.cc). What is written goes in as a set when the
 *	  write is committed; what is read is the set in hand.
 *	- GpuDataManager (asked before a WebGPU context is made): no address
 *	  is kept from the 3D APIs.
 *
 *	A window reaches the first four through the frame's broker (the empty
 *	one, given binders here) and BroadcastChannel through the frame's
 *	associated interfaces; a worker reaches all of them through its broker
 *	(blob_tessronos.cc). The receivers are on the blob thread, except a
 *	window's channel connections, which are on the page's thread with the
 *	frame's associated interfaces.
 *
 *	The browser keeps these per origin; here the locks and the channels
 *	are kept per name for the whole process, which shows only one page at
 *	a time.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <thread>
#include <utility>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/synchronization/lock.h"
#include "base/values.h"
#include "base/task/single_thread_task_runner.h"
#include "mojo/public/cpp/bindings/associated_receiver.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "mojo/public/cpp/bindings/generic_pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_associated_receiver.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote_set.h"
#include "mojo/public/cpp/bindings/self_owned_associated_receiver.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "../net/br_data.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "third_party/blink/public/mojom/broadcastchannel/broadcast_channel.mojom-blink.h"
#include "third_party/blink/public/mojom/clipboard/clipboard.mojom-blink.h"
#include "third_party/blink/public/mojom/gpu/gpu.mojom-blink.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkData.h"
#include "third_party/skia/include/core/SkStream.h"
#include "third_party/skia/include/encode/SkPngEncoder.h"
#include "third_party/blink/public/mojom/payments/payment_request.mojom-blink.h"
#include "services/media_session/public/mojom/media_session.mojom-blink.h"
#include "third_party/blink/public/mojom/mediasession/media_session.mojom-blink.h"
#include "third_party/blink/public/mojom/locks/lock_manager.mojom-blink.h"
#include "third_party/blink/public/mojom/notifications/notification_service.mojom-blink.h"
#include "third_party/blink/public/mojom/service_worker/service_worker.mojom-blink.h"
#include "third_party/blink/public/mojom/push_messaging/push_messaging.mojom-blink.h"
#include "third_party/blink/public/mojom/permissions/permission.mojom-blink.h"
#include "third_party/blink/public/mojom/permissions/permission_status.mojom-blink.h"
#include "third_party/blink/public/mojom/notifications/notification.mojom-blink.h"
#include "third_party/blink/public/mojom/quota/quota_types.mojom-blink.h"
#include "third_party/blink/public/mojom/quota/quota_manager_host.mojom-blink.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"
#include "third_party/blink/renderer/bindings/core/v8/serialization/serialized_script_value.h"
#include "third_party/blink/renderer/core/messaging/blink_cloneable_message.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

#include "../net/br_store.h"
#include "../net/br_tray.h"
#include "../../br.h"

/* Cache Storage (cache_storage_tessronos.cc) */
bool br_cache_storage_bind( mojo::GenericPendingReceiver &r );
void br_cache_storage_init();

/* Shared workers (shared_worker_tessronos.cc) */
void br_shared_worker_init();

/* The files a page is given (files_tessronos.cc) */
bool br_files_bind( mojo::GenericPendingReceiver &r );
void br_files_init();

/* The factory of the Blobs of files (blob_tessronos.cc) */
void br_file_blobs_init();

/* navigator.credentials: passkeys, FedCM, passwords, one-time codes (credentials_tessronos.cc) */
void br_credentials_init();

/* The built-in AI (ai_tessronos.cc) and Web MIDI (midi_tessronos.cc) */
void br_ai_init();
void br_midi_init();
/* Direct Sockets, for the origins that act as isolated web apps (direct_sockets_tessronos.cc) */
void br_direct_sockets_init();
/* Web Printing, for the same origins (printing_tessronos.cc) */
void br_printing_init();
/* speechSynthesis and SpeechRecognition (speech_tessronos.cc) */
void br_speech_init();
/* WebNN on the processor (webnn_tessronos.cc) */
void br_webnn_init();
/* HEVC on the machine's decoder, when it has one (port/media/hevc_tessronos.cc) */
extern "C" void br_hevc_media_init( void ) __attribute__((weak));

/* The File System Access API's pickers and handles (file_system_access_tessronos.cc) */
void br_file_system_access_init();

/* The interfaces to a machine's devices, answered for this one (devices_tessronos.cc) */
void br_devices_init();
void br_serial_init();
void br_usb_hid_init();
void br_cookie_store_init();
void br_p2p_init();
void br_web_transport_init();
void br_buckets_init();
void br_filesystem_init();
void br_sw_dispatch( int64_t registration_id,
		     base::OnceCallback<void( blink::mojom::blink::ServiceWorker * )> send );	/* service_worker_tessronos.cc */
void br_sw_dispatch_scope( const std::string &scope,
			   base::OnceCallback<void( blink::mojom::blink::ServiceWorker * )> send );
void br_sw_scope_of( int64_t registration_id, base::OnceCallback<void( std::string )> done );
void br_pip_init();
void br_storage_access_init();
void br_sw_init();				/* service_worker_tessronos.cc */
void br_background_init();			/* background_tessronos.cc */
bool br_background_bind( mojo::GenericPendingReceiver &r );
void br_background_fetch_init();		/* background_fetch_tessronos.cc */
bool br_file_system_access_bind( mojo::GenericPendingReceiver &r );	/* file_system_access_tessronos.cc */
bool br_background_fetch_bind( mojo::GenericPendingReceiver &r );
void br_background_fetch_note( int n, int what );
extern const int kBrBgFetchNote;
bool br_filesystem_bind( mojo::GenericPendingReceiver &r, const std::string &origin );

/* The blob thread, where the services are (blob_tessronos.cc) */
void br_on_service_thread( base::OnceClosure task );

namespace {

namespace bm = blink::mojom::blink;

/* ---------------------------------------------------------------- permissions */

/* The answers the user gave, by origin and permission, kept on the services' thread */
std::map<std::pair<std::string, int>, bm::PermissionStatus> &Decided()
{
	static auto *m = new std::map<std::pair<std::string, int>, bm::PermissionStatus>();
	return *m;
}

/*
 * The answers on the microphone and the cameras, given on getUserMedia's
 * panel (devices_tessronos.cc): kept while the browser runs, as the
 * panel's answers are, and what a query of "microphone" or "camera"
 * answers for the origin
 */
std::map<std::pair<std::string, int>, bm::PermissionStatus> &MediaDecided()
{
	static auto *m = new std::map<std::pair<std::string, int>, bm::PermissionStatus>();
	return *m;
}

/* The pages' observers of a permission (navigator.permissions' status objects), by origin and permission */
struct Watch {
	std::string				origin;
	int					name;
	mojo::Remote<bm::PermissionObserver>	observer;
};

std::vector<std::unique_ptr<Watch>> &Watches()
{
	static auto *v = new std::vector<std::unique_ptr<Watch>>();
	return *v;
}

/*
 * The answers are kept between runs, in the store "perms" (record 7 of
 * ブラウザの保存域) as JSON: made on the services' thread when one is
 * given, copied by the store's writer
 */
int		g_perm_store = -1;
std::mutex	g_perm_lock;
std::map<std::string, std::string> g_perm_entries;	/* the origin, a line feed and the permission's number: the answer's */

std::string PermKey( const std::string &origin, int name )
{
	return origin + '\n' + base::NumberToString(name);
}

void SnapshotPerms( void *, br_store_delta *all )
{
	std::lock_guard<std::mutex> hold(g_perm_lock);
	for ( const auto &kv : g_perm_entries ) {
		br_store_delta_put(all, kv.first.data(), (int)kv.first.size(), kv.second.data(), (int)kv.second.size());
	}
}

void DecidedChanged();

}  // namespace

void br_perms_clear( const BrClear &c )
{
	bool any = false;
	for ( auto it = Decided().begin(); it != Decided().end(); ) {
		if ( br_clear_origin_hit(c, it->first.first) ) {
			it = Decided().erase(it);
			any = true;
		} else {
			++it;
		}
	}
	if ( any ) DecidedChanged();
}

namespace {

void DecidedChanged()
{
	std::map<std::string, std::string> entries;
	for ( const auto &kv : Decided() ) {
		entries[PermKey(kv.first.first, kv.first.second)] = base::NumberToString((int)kv.second);
	}
	{
		std::lock_guard<std::mutex> hold(g_perm_lock);
		g_perm_entries = std::move(entries);
	}
	if ( g_perm_store >= 0 ) br_store_dirty(g_perm_store);
}

/*
 * The origins the user lets have what an isolated web app has: kept with
 * the answers, as the permission numbered kIsolatedApp, granted
 */
constexpr int kIsolatedApp = 1000;
std::mutex		g_isolated_lock;
std::set<std::string>	g_isolated;

/* The answers of earlier runs read back (the store's bytes, on the services' thread) */
void DecidedOne( const std::string &origin, int name, bool has, int status )
{
	if ( has ) Decided()[{ origin, name }] = (bm::PermissionStatus)status;
	else Decided().erase({ origin, name });
	if ( name == kIsolatedApp ) {
		std::lock_guard<std::mutex> hold(g_isolated_lock);
		if ( has && (bm::PermissionStatus)status == bm::PermissionStatus::GRANTED ) g_isolated.insert(origin);
		else g_isolated.erase(origin);
	}
}

/* An older browser's record, a list in JSON */
void RestorePerms( std::string text )
{
	std::optional<base::Value> v = base::JSONReader::Read(text, base::JSON_PARSE_RFC);
	if ( !v || !v->is_list() ) return;
	std::map<std::string, std::string> entries;
	for ( const base::Value &e : v->GetList() ) {
		if ( !e.is_dict() ) continue;
		const std::string *origin = e.GetDict().FindString("origin");
		std::optional<int> name = e.GetDict().FindInt("name"), status = e.GetDict().FindInt("status");
		if ( origin == nullptr || !name || !status ) continue;
		DecidedOne(*origin, *name, true, *status);
		entries[PermKey(*origin, *name)] = base::NumberToString(*status);
	}
	{
		std::lock_guard<std::mutex> hold(g_perm_lock);
		g_perm_entries.insert(entries.begin(), entries.end());
	}
	/* entries of their own from the next save */
	if ( g_perm_store >= 0 ) br_store_dirty(g_perm_store);
	printf("browser: %zu answers on permissions read back\n", Decided().size());
}

/* What is asked of the user, and in which words; null for one that is not asked */
const char *AskedAs( bm::PermissionName name )
{
	switch ( name ) {
	case bm::PermissionName::CLIPBOARD_READ:
		return "トレーの内容の読み取り";
	case bm::PermissionName::NOTIFICATIONS:
		return "通知の表示";
	case bm::PermissionName::MIDI:
		return "MIDIの機器の使用";
	case bm::PermissionName::IDLE_DETECTION:
		return "利用者が操作しているかの検出";
	case bm::PermissionName::LOCAL_FONTS:
		return "この機械のフォントの一覧の読み取り";
	case bm::PermissionName::WINDOW_MANAGEMENT:
		return "画面の配置とウインドウの管理";
	default:
		return nullptr;
	}
}

}  // namespace

extern int64_t br_periodic_sync_ms;	/* the shortest periodic sync interval a test sets (background_tessronos.cc) */

namespace {

/*
 * What is given without asking, as Chrome on a desktop gives it: the
 * clipboard's writing, the storage access of a frame, the screen's wake
 * lock, the sensors, the syncs and fetches in the background, the pointer
 * and keyboard locks (asked for in full screen or by a user's click) and
 * payment handlers. Periodic sync goes only to an installed app: here to
 * a test that sets its interval ("periodicsync=" line), and is refused
 * otherwise; the storage access a top-level site asks for another is
 * asked.
 */
bm::PermissionStatus QueryStatus( const bm::PermissionDescriptorPtr &d )
{
	if ( d->name == bm::PermissionName::AUDIO_CAPTURE || d->name == bm::PermissionName::VIDEO_CAPTURE ) {
		auto m = MediaDecided().find({ br_page_origin(), (int)d->name });
		return m != MediaDecided().end() ? m->second : bm::PermissionStatus::ASK;
	}
	auto it = Decided().find({ br_page_origin(), (int)d->name });
	if ( it != Decided().end() ) return it->second;
	switch ( d->name ) {
	case bm::PermissionName::CLIPBOARD_WRITE:
	case bm::PermissionName::STORAGE_ACCESS:
	case bm::PermissionName::SCREEN_WAKE_LOCK:
	case bm::PermissionName::SENSORS:
	case bm::PermissionName::BACKGROUND_SYNC:
	case bm::PermissionName::BACKGROUND_FETCH:
	case bm::PermissionName::POINTER_LOCK:
	case bm::PermissionName::KEYBOARD_LOCK:
	case bm::PermissionName::PAYMENT_HANDLER:
		return bm::PermissionStatus::GRANTED;
	case bm::PermissionName::PERIODIC_BACKGROUND_SYNC:
		return br_periodic_sync_ms > 0 ? bm::PermissionStatus::GRANTED : bm::PermissionStatus::DENIED;
	default:
		return bm::PermissionStatus::ASK;
	}
}

/* Asked of the user in a browser; here only what is given without asking is given */
bm::PermissionStatus RequestStatus( const bm::PermissionDescriptorPtr &d )
{
	bm::PermissionStatus s = QueryStatus(d);
	return s == bm::PermissionStatus::GRANTED ? s : bm::PermissionStatus::DENIED;
}

bm::PermissionStatusWithDetailsPtr WithDetails( bm::PermissionStatus s )
{
	return bm::PermissionStatusWithDetails::New(s, nullptr);
}

/* An answer changed: the observers of the origin's permission told, as a page's status objects' change events */
void TellObservers( const std::string &origin, int name, bm::PermissionStatus s )
{
	std::vector<std::unique_ptr<Watch>> &w = Watches();
	for ( size_t i = 0; i < w.size(); ) {
		if ( !w[i]->observer.is_connected() ) {
			w.erase(w.begin() + (long)i);
			continue;
		}
		if ( w[i]->origin == origin && w[i]->name == name ) w[i]->observer->OnPermissionStatusChange(WithDetails(s));
		i++;
	}
}

class Permissions final : public bm::PermissionService {
public:
	void Bind( mojo::PendingReceiver<bm::PermissionService> r ) { receivers_.Add(this, std::move(r)); }

	void HasPermission( bm::PermissionDescriptorPtr p, HasPermissionCallback cb ) override
	{
		std::move(cb).Run(WithDetails(QueryStatus(p)));
	}
	void RegisterPageEmbeddedPermissionControl( blink::Vector<bm::PermissionDescriptorPtr>,
						    bm::EmbeddedPermissionRequestDescriptorPtr,
						    mojo::PendingRemote<bm::EmbeddedPermissionControlClient> ) override {}
	void RequestPageEmbeddedPermission( blink::Vector<bm::PermissionDescriptorPtr>,
					    bm::EmbeddedPermissionRequestDescriptorPtr,
					    RequestPageEmbeddedPermissionCallback cb ) override
	{
		std::move(cb).Run(bm::EmbeddedPermissionControlResult::kDismissed);
	}
	void RequestPermission( bm::PermissionDescriptorPtr p, RequestPermissionCallback cb ) override
	{
		const char *what = AskedAs(p->name);
		if ( what == nullptr || QueryStatus(p) != bm::PermissionStatus::ASK ) {
			std::move(cb).Run(WithDetails(RequestStatus(p)));
			return;
		}
		/*
		 * Asked on a thread of its own, so that the services go on
		 * while the panel is up; the answer comes back here
		 */
		std::string origin = br_page_origin();
		int name = (int)p->name;
		std::thread([origin, name, what, cb = std::move(cb)]() mutable {
			bool yes = br_ask_permission(origin, what);
			br_on_service_thread(base::BindOnce([]( std::string origin, int name, bool yes,
								RequestPermissionCallback cb ) {
				bm::PermissionStatus s = yes ? bm::PermissionStatus::GRANTED
							     : bm::PermissionStatus::DENIED;
				Decided()[{ origin, name }] = s;
				DecidedChanged();
				TellObservers(origin, name, s);
				std::move(cb).Run(WithDetails(s));
			}, origin, name, yes, std::move(cb)));
		}).detach();
	}
	void RequestPermissions( blink::Vector<bm::PermissionDescriptorPtr> ps, RequestPermissionsCallback cb ) override
	{
		blink::Vector<bm::PermissionStatusWithDetailsPtr> out;
		for ( const auto &p : ps ) out.push_back(WithDetails(RequestStatus(p)));
		std::move(cb).Run(std::move(out));
	}
	void RevokePermission( bm::PermissionDescriptorPtr p, RevokePermissionCallback cb ) override
	{
		std::move(cb).Run(WithDetails(QueryStatus(p)));
	}
	/* told when the user's answer for the page's origin changes (TellObservers) */
	void AddPermissionObserver( bm::PermissionDescriptorPtr d, bm::PermissionStatusWithDetailsPtr last,
				    mojo::PendingRemote<bm::PermissionObserver> o ) override
	{
		auto w = std::make_unique<Watch>();
		w->origin = br_page_origin();
		w->name = (int)d->name;
		w->observer.Bind(std::move(o));
		bm::PermissionStatus now = QueryStatus(d);
		if ( last && last->status != now ) w->observer->OnPermissionStatusChange(WithDetails(now));
		Watches().push_back(std::move(w));
	}
	void AddPageEmbeddedPermissionObserver( bm::PermissionDescriptorPtr, bm::PermissionStatus,
						mojo::PendingRemote<bm::PermissionObserver> o ) override
	{
		observers_.Add(std::move(o));
	}
	void NotifyEventListener( bm::PermissionDescriptorPtr, const blink::String &, bool ) override {}

private:
	mojo::ReceiverSet<bm::PermissionService>	receivers_;
	mojo::RemoteSet<bm::PermissionObserver>		observers_;
};

}  // namespace

/*
 * The user's answer on getUserMedia's panel for an origin (on the
 * services' thread): what a query of the microphone and the camera
 * answers from then on, and told to their observers
 */
void br_media_answered( const std::string &origin, bool audio, bool video, bool yes )
{
	bm::PermissionStatus s = yes ? bm::PermissionStatus::GRANTED : bm::PermissionStatus::DENIED;
	if ( audio ) {
		MediaDecided()[{ origin, (int)bm::PermissionName::AUDIO_CAPTURE }] = s;
		TellObservers(origin, (int)bm::PermissionName::AUDIO_CAPTURE, s);
	}
	if ( video ) {
		MediaDecided()[{ origin, (int)bm::PermissionName::VIDEO_CAPTURE }] = s;
		TellObservers(origin, (int)bm::PermissionName::VIDEO_CAPTURE, s);
	}
}

/* The answer so far on the microphone (video false) or the camera: 1 given, 0 refused, -1 not asked */
int br_media_decided( const std::string &origin, bool video )
{
	auto it = MediaDecided().find({ origin, (int)( video ? bm::PermissionName::VIDEO_CAPTURE
							     : bm::PermissionName::AUDIO_CAPTURE ) });
	if ( it == MediaDecided().end() ) return -1;
	return it->second == bm::PermissionStatus::GRANTED ? 1 : 0;
}

namespace {

/* ---------------------------------------------------------------- notifications */

/*
 * The media sessions of the page's frames (navigator.mediaSession): what
 * each says it plays and which actions it takes, and the client it is
 * told actions through. The browser's menu 〈メディア〉 asks one of them
 * (br_media_session_action): the one playing, else the last that said
 * anything.
 */
namespace ms = media_session::mojom;

struct SessionState {
	mojo::Remote<bm::MediaSessionClient>	client;
	bm::MediaSessionPlaybackState		state = bm::MediaSessionPlaybackState::NONE;
	std::set<ms::MediaSessionAction>	actions;
	std::string				title, artist;
	uint64_t				said = 0;	/* when it last said something */
};

std::mutex				g_ms_lock;	/* the states, read from the engine's thread */
std::map<mojo::ReceiverId, SessionState> g_ms;
uint64_t				g_ms_clock;

class MediaSessions final : public bm::MediaSessionService {
public:
	void Bind( mojo::PendingReceiver<bm::MediaSessionService> r )
	{
		mojo::ReceiverId id = receivers_.Add(this, std::move(r));
		receivers_.set_disconnect_handler(base::BindRepeating(&MediaSessions::Gone, base::Unretained(this)));
		std::lock_guard<std::mutex> hold(g_ms_lock);
		g_ms[id];
	}

	void SetClient( mojo::PendingRemote<bm::MediaSessionClient> client ) override
	{
		std::lock_guard<std::mutex> hold(g_ms_lock);
		SessionState &st = Mine();
		st.client.reset();
		st.client.Bind(std::move(client));
	}
	void SetPlaybackState( bm::MediaSessionPlaybackState state ) override
	{
		std::lock_guard<std::mutex> hold(g_ms_lock);
		SessionState &st = Mine();
		st.state = state;
		st.said = ++g_ms_clock;
	}
	void SetPositionState( ms::blink::MediaPositionPtr ) override {}
	void SetMetadata( bm::SpecMediaMetadataPtr metadata ) override
	{
		std::lock_guard<std::mutex> hold(g_ms_lock);
		SessionState &st = Mine();
		st.title = metadata ? metadata->title.Utf8() : std::string();
		st.artist = metadata ? metadata->artist.Utf8() : std::string();
		st.said = ++g_ms_clock;
		printf("browser: media session: %s%s%s\n", st.title.c_str(), st.artist.empty() ? "" : " / ",
		       st.artist.c_str());
	}
	void SetMicrophoneState( ms::MicrophoneState ) override {}
	void SetCameraState( ms::CameraState ) override {}
	void EnableAction( ms::MediaSessionAction action ) override
	{
		std::lock_guard<std::mutex> hold(g_ms_lock);
		SessionState &st = Mine();
		st.actions.insert(action);
		st.said = ++g_ms_clock;
	}
	void DisableAction( ms::MediaSessionAction action ) override
	{
		std::lock_guard<std::mutex> hold(g_ms_lock);
		Mine().actions.erase(action);
	}

private:
	SessionState &Mine() { return g_ms[receivers_.current_receiver()]; }

	void Gone()
	{
		std::lock_guard<std::mutex> hold(g_ms_lock);
		g_ms.erase(receivers_.current_receiver());
	}

	mojo::ReceiverSet<bm::MediaSessionService>	receivers_;
};

MediaSessions *g_media_sessions;

/*
 * A Payment Request (new PaymentRequest): there is no payment app nor
 * method in the system, so none can be made -- the page is told so, as
 * a browser tells it when it has none for the methods asked: show()
 * fails with NotSupportedError, canMakePayment() and
 * hasEnrolledInstrument() say no. One for each request.
 */
namespace pm = payments::mojom::blink;

class PaymentRequestImpl final : public pm::PaymentRequest {
public:
	void Init( mojo::PendingRemote<pm::PaymentRequestClient> client, blink::Vector<pm::PaymentMethodDataPtr> methods,
		   pm::PaymentDetailsPtr, pm::PaymentOptionsPtr ) override
	{
		client_.Bind(std::move(client));
		for ( const auto &m : methods ) {
			if ( !methods_.empty() ) methods_ += ", ";
			methods_ += m->supported_method.Utf8();
		}
	}
	void Show( bool, bool ) override
	{
		printf("browser: payment: no app for %s\n", methods_.c_str());
		if ( client_ ) {
			client_->OnError(pm::PaymentErrorReason::NOT_SUPPORTED,
					 blink::String::FromUtf8("The payment methods " + methods_ + " are not supported."));
		}
	}
	void UpdateWith( pm::PaymentDetailsPtr ) override {}
	void OnPaymentDetailsNotUpdated() override {}
	void Abort() override
	{
		if ( client_ ) client_->OnAbort(true);
	}
	void Complete( pm::PaymentComplete ) override
	{
		if ( client_ ) client_->OnComplete();
	}
	void Retry( pm::PaymentValidationErrorsPtr ) override
	{
		if ( client_ ) client_->OnError(pm::PaymentErrorReason::NOT_SUPPORTED, "There is no payment to retry.");
	}
	void CanMakePayment() override
	{
		if ( client_ ) client_->OnCanMakePayment(pm::CanMakePaymentQueryResult::CANNOT_MAKE_PAYMENT);
	}
	void HasEnrolledInstrument() override
	{
		if ( client_ ) client_->OnHasEnrolledInstrument(pm::HasEnrolledInstrumentQueryResult::HAS_NO_ENROLLED_INSTRUMENT);
	}

private:
	mojo::Remote<pm::PaymentRequestClient>	client_;
	std::string				methods_;
};

/*
 * The notifications shown, by the number the desktop knows them by: a
 * window's, with the listener it tells, or a service worker's, with its
 * registration and what it says (for getNotifications and its events)
 */
struct Shown {
	bool						persistent = false;
	blink::String					token;		/* a window's: its token */
	mojo::Remote<bm::NonPersistentNotificationListener>	listener;
	int64_t						registration = -1;	/* a worker's */
	blink::String					id;		/* and its notification id */
	bm::NotificationDataPtr				data;
};

std::map<int, Shown>	g_shown;		/* on the services' thread */
int			g_note_last = 0;

/* What the desktop says came of one, from its reader's thread */
void NoteAnswered( int n, int what, int which );

}  // namespace

/* The desktop's answers to the notifications watched (once; from any thread that shows one) */
void br_services_note_watch()
{
	static std::once_flag once;
	std::call_once(once, []() { br_note_watch(&NoteAnswered); });
}

namespace {

void NoteAnswered( int n, int what, int which )
{
	br_on_service_thread(base::BindOnce([]( int n, int what, int which ) {
		if ( n >= kBrBgFetchNote ) {
			/* a background fetch's */
			br_background_fetch_note(n, what);
			return;
		}
		auto it = g_shown.find(n);
		if ( it == g_shown.end() ) return;
		Shown &sh = it->second;
		printf("browser: notification %d %s\n", n,
		       what == BR_NOTE_CLICK ? "pressed" : what == BR_NOTE_CLOSED ? "closed" : "gone by itself");
		if ( !sh.persistent ) {
			if ( what == BR_NOTE_CLICK && sh.listener ) sh.listener->OnClick(base::BindOnce([] {}));
			if ( what == BR_NOTE_CLOSED && sh.listener ) sh.listener->OnClose(base::BindOnce([] {}));
			if ( what != BR_NOTE_GONE ) g_shown.erase(it);
			return;
		}
		if ( what == BR_NOTE_GONE ) return;	/* still there for getNotifications, as in a browser's list */
		blink::String id = sh.id;
		bm::NotificationDataPtr data = sh.data.Clone();
		int64_t reg = sh.registration;
		if ( what == BR_NOTE_CLOSED ) g_shown.erase(it);
		br_sw_dispatch(reg, base::BindOnce([]( blink::String id, bm::NotificationDataPtr data, bool click,
							int action, bm::ServiceWorker *w ) {
			if ( w == nullptr ) return;
			if ( click ) {
				w->DispatchNotificationClickEvent(id, std::move(data), action, blink::String(),
								  base::BindOnce([]( bm::ServiceWorkerEventStatus ) {}));
			} else {
				w->DispatchNotificationCloseEvent(id, std::move(data),
								  base::BindOnce([]( bm::ServiceWorkerEventStatus ) {}));
			}
		}, id, std::move(data), what == BR_NOTE_CLICK, which - 1));
	}, n, what, which));
}

/* The desktop asked to show one; false when it is not there */
bool ShowNote( int n, const bm::NotificationDataPtr &d )
{
	std::vector<std::string> actions;
	if ( d->actions ) {
		for ( const auto &a : *d->actions ) actions.push_back(a->title.Utf8());
	}
	std::string origin = br_page_origin();
	size_t p = origin.find("://");
	if ( p != std::string::npos ) origin = origin.substr(p + 3);
	return br_note_show(n, origin, d->title.Utf8(), d->body.Utf8(), actions, d->require_interaction);
}

class Notifications final : public bm::NotificationService {
public:
	void Bind( mojo::PendingReceiver<bm::NotificationService> r )
	{
		br_services_note_watch();
		receivers_.Add(this, std::move(r));
	}

	void GetPermissionStatus( GetPermissionStatusCallback cb ) override
	{
		std::move(cb).Run(QueryStatus(bm::PermissionDescriptor::New(bm::PermissionName::NOTIFICATIONS, nullptr)));
	}

	/* new Notification(): shown, its listener told it was, and later what came of it */
	void DisplayNonPersistentNotification( const blink::String &token, bm::NotificationDataPtr data,
					       bm::NotificationResourcesPtr,
					       mojo::PendingRemote<bm::NonPersistentNotificationListener> listener ) override
	{
		/* one with the same token (its tag) takes the old one's place */
		int n = 0;
		for ( auto &kv : g_shown ) {
			if ( !kv.second.persistent && kv.second.token == token ) n = kv.first;
		}
		if ( n == 0 ) n = ++g_note_last;
		Shown &sh = g_shown[n];
		sh.persistent = false;
		sh.token = token;
		sh.listener.reset();
		sh.listener.Bind(std::move(listener));
		if ( !ShowNote(n, data) ) {
			g_shown.erase(n);
			return;
		}
		sh.listener->OnShow();
	}

	/* notification.close(): taken back, and its close event fired */
	void CloseNonPersistentNotification( const blink::String &token ) override
	{
		for ( auto it = g_shown.begin(); it != g_shown.end(); ++it ) {
			if ( !it->second.persistent && it->second.token == token ) {
				br_note_close(it->first);
				if ( it->second.listener ) it->second.listener->OnClose(base::BindOnce([] {}));
				g_shown.erase(it);
				return;
			}
		}
	}

	/* registration.showNotification(): its events go to the worker */
	void DisplayPersistentNotification( int64_t registration, bm::NotificationDataPtr data,
					    bm::NotificationResourcesPtr, DisplayPersistentNotificationCallback cb ) override
	{
		if ( QueryStatus(bm::PermissionDescriptor::New(bm::PermissionName::NOTIFICATIONS, nullptr))
		     != bm::PermissionStatus::GRANTED ) {
			std::move(cb).Run(bm::PersistentNotificationError::PERMISSION_DENIED);
			return;
		}
		int n = 0;
		if ( !data->tag.empty() ) {
			for ( auto &kv : g_shown ) {
				if ( kv.second.persistent && kv.second.registration == registration
				     && kv.second.data && kv.second.data->tag == data->tag ) {
					n = kv.first;
				}
			}
		}
		if ( n == 0 ) n = ++g_note_last;
		Shown &sh = g_shown[n];
		sh.persistent = true;
		sh.registration = registration;
		sh.id = blink::String::FromUtf8("p" + std::to_string(n));
		sh.data = data.Clone();
		if ( !ShowNote(n, data) ) {
			g_shown.erase(n);
			std::move(cb).Run(bm::PersistentNotificationError::NOTIFICATION_SERVICE_NOT_FOUND);
			return;
		}
		std::move(cb).Run(bm::PersistentNotificationError::NONE);
	}

	void ClosePersistentNotification( const blink::String &id ) override
	{
		for ( auto it = g_shown.begin(); it != g_shown.end(); ++it ) {
			if ( it->second.persistent && it->second.id == id ) {
				br_note_close(it->first);
				g_shown.erase(it);
				return;
			}
		}
	}

	/* registration.getNotifications(): those of the registration still there, of its tag */
	void GetNotifications( int64_t registration, const blink::String &tag, bool, GetNotificationsCallback cb ) override
	{
		blink::Vector<blink::String> ids;
		blink::Vector<bm::NotificationDataPtr> datas;
		for ( auto &kv : g_shown ) {
			const Shown &sh = kv.second;
			if ( !sh.persistent || sh.registration != registration || !sh.data ) continue;
			if ( !tag.empty() && sh.data->tag != tag ) continue;
			ids.push_back(sh.id);
			datas.push_back(sh.data.Clone());
		}
		std::move(cb).Run(std::move(ids), std::move(datas));
	}

private:
	mojo::ReceiverSet<bm::NotificationService>	receivers_;
};

/* ---------------------------------------------------------------- Web Push */

blink::Vector<uint8_t> Bytes( const std::string &s )
{
	blink::Vector<uint8_t> v;
	v.Append(reinterpret_cast<const uint8_t *>(s.data()), static_cast<blink::wtf_size_t>(s.size()));
	return v;
}

bm::PushSubscriptionPtr MojoSub( const BrPushSub &p )
{
	return bm::PushSubscription::New(blink::KURL(blink::String::FromUtf8(p.endpoint)), std::nullopt,
					 bm::PushSubscriptionOptions::New(true, Bytes(p.app_key)), Bytes(p.p256dh),
					 Bytes(p.auth));
}

/* A scope's subscriptions, by the registration's number: the scope looked up on the page's thread */
void WithScope( int64_t registration, base::OnceCallback<void( std::string )> then )
{
	br_sw_scope_of(registration, base::BindOnce([]( base::OnceCallback<void( std::string )> then, std::string scope ) {
		br_on_service_thread(base::BindOnce(std::move(then), scope));
	}, std::move(then)));
}

class PushMessagingImpl final : public bm::PushMessaging {
public:
	void Bind( mojo::PendingReceiver<bm::PushMessaging> r ) { receivers_.Add(this, std::move(r)); }

	/*
	 * pushManager.subscribe(): messages shown to the user only (as
	 * Chrome asks), with the application server's key, and the
	 * notifications' permission, asked for when it has not been decided
	 */
	void Subscribe( int64_t registration, bm::PushSubscriptionOptionsPtr options, bool user_gesture,
			SubscribeCallback cb ) override
	{
		if ( !options->user_visible_only ) {
			printf("browser: push: refused, not userVisibleOnly\n");
			std::move(cb).Run(bm::PushRegistrationStatus::PERMISSION_DENIED, nullptr);
			return;
		}
		if ( options->application_server_key.empty() ) {
			std::move(cb).Run(bm::PushRegistrationStatus::NO_SENDER_ID, nullptr);
			return;
		}
		std::string key(options->application_server_key.begin(), options->application_server_key.end());
		bm::PermissionStatus st =
			QueryStatus(bm::PermissionDescriptor::New(bm::PermissionName::NOTIFICATIONS, nullptr));
		if ( st == bm::PermissionStatus::GRANTED ) {
			Go(registration, key, std::move(cb));
			return;
		}
		if ( st == bm::PermissionStatus::DENIED || !user_gesture ) {
			std::move(cb).Run(bm::PushRegistrationStatus::PERMISSION_DENIED, nullptr);
			return;
		}
		std::string origin = br_page_origin();
		std::thread([origin, registration, key, cb = std::move(cb)]() mutable {
			bool yes = br_ask_permission(origin, "通知の表示");
			br_on_service_thread(base::BindOnce([]( std::string origin, int64_t registration, std::string key,
								 bool yes, SubscribeCallback cb ) {
				Decided()[{ origin, (int)bm::PermissionName::NOTIFICATIONS }] =
					yes ? bm::PermissionStatus::GRANTED : bm::PermissionStatus::DENIED;
				DecidedChanged();
				if ( !yes ) {
					std::move(cb).Run(bm::PushRegistrationStatus::PERMISSION_DENIED, nullptr);
					return;
				}
				Go(registration, key, std::move(cb));
			}, origin, registration, key, yes, std::move(cb)));
		}).detach();
	}

	void Unsubscribe( int64_t registration, UnsubscribeCallback cb ) override
	{
		WithScope(registration, base::BindOnce([]( UnsubscribeCallback cb, std::string scope ) {
			bool did = !scope.empty() && br_push_unsubscribe(scope);
			std::move(cb).Run(bm::PushErrorType::NONE, did, blink::String());
		}, std::move(cb)));
	}

	void GetSubscription( int64_t registration, GetSubscriptionCallback cb ) override
	{
		WithScope(registration, base::BindOnce([]( GetSubscriptionCallback cb, std::string scope ) {
			BrPushSub p;
			if ( scope.empty() ) {
				std::move(cb).Run(bm::PushGetRegistrationStatus::NO_LIVE_SERVICE_WORKER, nullptr);
			} else if ( br_push_get(scope, &p) ) {
				std::move(cb).Run(bm::PushGetRegistrationStatus::SUCCESS, MojoSub(p));
			} else {
				std::move(cb).Run(bm::PushGetRegistrationStatus::REGISTRATION_NOT_FOUND, nullptr);
			}
		}, std::move(cb)));
	}

private:
	static void Go( int64_t registration, std::string key, SubscribeCallback cb )
	{
		WithScope(registration, base::BindOnce([]( std::string key, SubscribeCallback cb, std::string scope ) {
			if ( scope.empty() ) {
				std::move(cb).Run(bm::PushRegistrationStatus::NO_SERVICE_WORKER, nullptr);
				return;
			}
			auto shared = std::make_shared<SubscribeCallback>(std::move(cb));
			br_push_subscribe(scope, key, [shared]( bool ok, const BrPushSub &sub, const std::string &why ) {
				br_on_service_thread(base::BindOnce([]( std::shared_ptr<SubscribeCallback> cb, bool ok,
									BrPushSub sub, std::string why ) {
					if ( !ok ) {
						printf("browser: push: not subscribed: %s\n", why.c_str());
						std::move(*cb).Run(bm::PushRegistrationStatus::SERVICE_NOT_AVAILABLE, nullptr);
						return;
					}
					std::move(*cb).Run(bm::PushRegistrationStatus::SUCCESS_NEW_SUBSCRIPTION_FROM_PUSH_SERVICE,
							   MojoSub(sub));
				}, shared, ok, sub, why));
			});
		}, key, std::move(cb)));
	}

	mojo::ReceiverSet<bm::PushMessaging>	receivers_;
};

/* A message pushed: the push event of the scope's service worker */
void PushDelivered( const std::string &scope, const std::optional<std::string> &payload )
{
	blink::String data = payload ? blink::String::FromUtf8(*payload) : blink::String();
	br_sw_dispatch_scope(scope, base::BindOnce([]( std::string scope, blink::String data, bm::ServiceWorker *w ) {
		if ( w == nullptr ) {
			printf("browser: push: no worker of %s to give the message to\n", scope.c_str());
			return;
		}
		w->DispatchPushEvent(data, base::BindOnce([]( bm::ServiceWorkerEventStatus ) {}));
	}, scope, data));
}

/* ---------------------------------------------------------------- the quota */

class Quota final : public bm::QuotaManagerHost {
public:
	void Bind( mojo::PendingReceiver<bm::QuotaManagerHost> r ) { receivers_.Add(this, std::move(r)); }

	void QueryStorageUsageAndQuota( QueryStorageUsageAndQuotaCallback cb ) override
	{
		std::move(cb).Run(bm::QuotaStatusCode::kOk, 0, 2ll << 30, bm::UsageBreakdown::New());
	}

private:
	mojo::ReceiverSet<bm::QuotaManagerHost>	receivers_;
};

/* ---------------------------------------------------------------- Web Locks */

class Locks;

/* A lock held: its holder's handle; closing it lets the lock go (the receiver's end) */
class Handle final : public bm::LockHandle {};

class Locks final : public bm::LockManager {
public:
	void Bind( mojo::PendingReceiver<bm::LockManager> r ) { receivers_.Add(this, std::move(r)); }

	void RequestLock( const blink::String &name, bm::LockMode mode, bm::LockManager::WaitMode wait,
			  mojo::PendingAssociatedRemote<bm::LockRequest> request ) override
	{
		auto q = std::make_unique<Entry>();
		q->id = ++seq_;
		q->mode = mode;
		q->request.Bind(std::move(request));
		std::string key = name.Utf8();
		uint64_t id = q->id;
		q->request.set_disconnect_handler(base::BindOnce(&Locks::Aborted, base::Unretained(this), key, id));
		Queue &queue = queues_[key];
		if ( wait == bm::LockManager::WaitMode::PREEMPT ) {
			/* the locks held are broken: their handles closed */
			for ( auto it = queue.begin(); it != queue.end(); ) {
				if ( ( *it )->held ) it = queue.erase(it);
				else ++it;
			}
		} else if ( wait == bm::LockManager::WaitMode::NO_WAIT && !Grantable(queue, queue.end(), mode) ) {
			q->request->Failed();
			return;
		}
		queue.push_back(std::move(q));
		Grant(key);
	}

	void QueryState( QueryStateCallback cb ) override
	{
		blink::Vector<bm::LockInfoPtr> pending, held;
		for ( const auto &[key, queue] : queues_ ) {
			for ( const auto &e : queue ) {
				auto info = bm::LockInfo::New(blink::String::FromUtf8(key), e->mode, blink::String(""));
				( e->held ? held : pending ).push_back(std::move(info));
			}
		}
		std::move(cb).Run(std::move(pending), std::move(held));
	}

	/* A holder let its lock go */
	void Released( const std::string &key, uint64_t id ) { Remove(key, id); }

private:
	struct Entry {
		uint64_t					id = 0;
		bm::LockMode					mode = bm::LockMode::EXCLUSIVE;
		bool						held = false;
		mojo::AssociatedRemote<bm::LockRequest>		request;
		std::unique_ptr<mojo::AssociatedReceiver<bm::LockHandle>> handle;
		std::unique_ptr<Handle>				impl;
	};
	using Queue = std::deque<std::unique_ptr<Entry>>;

	/* Whether a lock of the mode could be granted with what comes before 'until' held or waiting */
	static bool Grantable( const Queue &queue, Queue::const_iterator until, bm::LockMode mode )
	{
		for ( auto it = queue.begin(); it != until; ++it ) {
			if ( mode == bm::LockMode::EXCLUSIVE ) return false;
			if ( ( *it )->mode == bm::LockMode::EXCLUSIVE || !( *it )->held ) return false;
		}
		return true;
	}

	void Grant( const std::string &key )
	{
		Queue &queue = queues_[key];
		for ( auto it = queue.begin(); it != queue.end(); ++it ) {
			Entry &e = **it;
			if ( e.held ) continue;
			if ( !Grantable(queue, it, e.mode) ) break;
			e.held = true;
			mojo::PendingAssociatedRemote<bm::LockHandle> remote;
			e.impl = std::make_unique<Handle>();
			e.handle = std::make_unique<mojo::AssociatedReceiver<bm::LockHandle>>(e.impl.get());
			e.handle->Bind(remote.InitWithNewEndpointAndPassReceiver());
			e.handle->set_disconnect_handler(base::BindOnce(&Locks::Released, base::Unretained(this), key, e.id));
			e.request->Granted(std::move(remote));
		}
	}

	void Aborted( const std::string &key, uint64_t id )
	{
		auto q = queues_.find(key);
		if ( q == queues_.end() ) return;
		for ( const auto &e : q->second ) {
			if ( e->id == id && e->held ) return;	/* held: its handle tells when it goes */
		}
		Remove(key, id);
	}

	void Remove( const std::string &key, uint64_t id )
	{
		auto q = queues_.find(key);
		if ( q == queues_.end() ) return;
		for ( auto it = q->second.begin(); it != q->second.end(); ++it ) {
			if ( ( *it )->id == id ) {
				/* let go after this call: the handle's receiver is what called */
				std::unique_ptr<Entry> gone = std::move(*it);
				q->second.erase(it);
				base::SingleThreadTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE, std::move(gone));
				break;
			}
		}
		if ( q->second.empty() ) {
			queues_.erase(q);
			return;
		}
		Grant(key);
	}

	mojo::ReceiverSet<bm::LockManager>	receivers_;
	std::map<std::string, Queue>		queues_;
	uint64_t				seq_ = 0;
};

/* ---------------------------------------------------------------- BroadcastChannel */

/* A connection to a channel: the client's end, used on the thread it is bound on */
struct Connection {
	std::string						name;
	scoped_refptr<base::SingleThreadTaskRunner>		runner;
	mojo::AssociatedRemote<bm::BroadcastChannelClient>	client;
};

class Channels;

/* What a connection sends: given to the others of its name */
class Sender final : public bm::BroadcastChannelClient {
public:
	Sender( Channels *channels, Connection *from ) : channels_(channels), from_(from) {}
	void OnMessage( blink::BlinkCloneableMessage message ) override;

private:
	Channels	*channels_;
	Connection	*from_;
};

class Channels {
public:
	/* A connection made on the current thread */
	void Connect( const blink::String &name, mojo::PendingAssociatedRemote<bm::BroadcastChannelClient> client,
		      mojo::PendingAssociatedReceiver<bm::BroadcastChannelClient> connection )
	{
		auto *c = new Connection();
		c->name = name.Utf8();
		c->runner = base::SingleThreadTaskRunner::GetCurrentDefault();
		c->client.Bind(std::move(client));
		{
			base::AutoLock hold(lock_);
			all_.push_back(c);
		}
		auto receiver = std::make_unique<mojo::AssociatedReceiver<bm::BroadcastChannelClient>>(
			new Sender(this, c), std::move(connection));
		mojo::AssociatedReceiver<bm::BroadcastChannelClient> *r = receiver.get();
		r->set_disconnect_handler(base::BindOnce(&Channels::Closed, base::Unretained(this), c,
							 std::move(receiver)));
	}

	/* A message to every other connection of the name, each from its own thread */
	void Send( Connection *from, blink::BlinkCloneableMessage &message )
	{
		base::AutoLock hold(lock_);
		for ( Connection *c : all_ ) {
			if ( c == from || c->name != from->name ) continue;
			blink::BlinkCloneableMessage m;
			m.message = blink::SerializedScriptValue::Create(message.message->GetWireData());
			/* and the Blobs the message holds, by their UUIDs, as a browser's channel carries them */
			for ( const auto &kv : message.message->BlobDataHandles() ) {
				m.message->BlobDataHandles().Set(blink::String(kv.key.Utf8()), kv.value);
			}
			m.sender_origin = message.sender_origin ? message.sender_origin->IsolatedCopy() : nullptr;
			m.sender_stack_trace_id = message.sender_stack_trace_id;
			m.sender_agent_cluster_id = message.sender_agent_cluster_id;
			m.locked_to_sender_agent_cluster = false;
			m.trace_id = message.trace_id;
			c->runner->PostTask(FROM_HERE, base::BindOnce(&Channels::Deliver, base::Unretained(this), c,
								      std::move(m)));
		}
	}

private:
	void Deliver( Connection *c, blink::BlinkCloneableMessage m )
	{
		{
			base::AutoLock hold(lock_);
			if ( std::find(all_.begin(), all_.end(), c) == all_.end() ) return;
		}
		if ( c->client.is_bound() ) c->client->OnMessage(std::move(m));
	}

	void Closed( Connection *c, std::unique_ptr<mojo::AssociatedReceiver<bm::BroadcastChannelClient>> r )
	{
		{
			base::AutoLock hold(lock_);
			all_.erase(std::remove(all_.begin(), all_.end(), c), all_.end());
		}
		bm::BroadcastChannelClient *impl = r->impl();
		r.reset();
		delete static_cast<Sender *>(impl);
		delete c;
	}

	base::Lock			lock_;
	std::vector<Connection *>	all_;
};

void Sender::OnMessage( blink::BlinkCloneableMessage message )
{
	if ( message.message ) channels_->Send(from_, message);
}

Channels &TheChannels()
{
	static auto *c = new Channels();
	return *c;
}

/* WebGL's and WebGPU's question: no address is kept from the 3D APIs */
class GpuDataManager final : public bm::GpuDataManager {
public:
	void Are3DAPIsBlockedForUrl( const blink::KURL &, Are3DAPIsBlockedForUrlCallback cb ) override
	{
		std::move(cb).Run(false);
	}
};

class ChannelProvider final : public bm::BroadcastChannelProvider {
public:
	void ConnectToChannel( const blink::String &name, mojo::PendingAssociatedRemote<bm::BroadcastChannelClient> client,
			       mojo::PendingAssociatedReceiver<bm::BroadcastChannelClient> connection ) override
	{
		TheChannels().Connect(name, std::move(client), std::move(connection));
	}
};

/* ---------------------------------------------------------------- the clipboard */

class Clipboard final : public bm::ClipboardHost {
public:
	void Bind( mojo::PendingReceiver<bm::ClipboardHost> r ) { receivers_.Add(this, std::move(r)); }

	void GetSequenceNumber( bm::ClipboardBuffer, GetSequenceNumberCallback cb ) override
	{
		std::move(cb).Run(absl::MakeUint128(0, br_tray_serial()));
	}
	void IsFormatAvailable( bm::ClipboardFormat format, bm::ClipboardBuffer, IsFormatAvailableCallback cb ) override
	{
		std::string t;
		std::move(cb).Run(format == bm::ClipboardFormat::kPlaintext && br_tray_text(&t));
	}
	void ReadAvailableTypes( bm::ClipboardBuffer, ReadAvailableTypesCallback cb ) override
	{
		blink::Vector<blink::String>	types;
		std::string			t;
		std::vector<uint8_t>		png;

		if ( br_tray_text(&t) ) types.push_back("text/plain");
		if ( br_tray_png(&png) ) types.push_back("image/png");
		std::move(cb).Run(types);
	}
	void ReadText( bm::ClipboardBuffer, ReadTextCallback cb ) override
	{
		std::string t;
		std::move(cb).Run(br_tray_text(&t) ? blink::String::FromUtf8(t) : blink::String(""));
	}
	void ReadHtml( bm::ClipboardBuffer, ReadHtmlCallback cb ) override
	{
		std::move(cb).Run(blink::String(""), blink::KURL(), 0, 0);
	}
	void ReadSvg( bm::ClipboardBuffer, ReadSvgCallback cb ) override { std::move(cb).Run(blink::String("")); }
	void ReadRtf( bm::ClipboardBuffer, ReadRtfCallback cb ) override { std::move(cb).Run(blink::String("")); }
	void ReadPng( bm::ClipboardBuffer, ReadPngCallback cb ) override
	{
		std::vector<uint8_t> png;
		if ( !br_tray_png(&png) ) png.clear();
		std::move(cb).Run(mojo_base::BigBuffer(base::span<const uint8_t>(png)));
	}
	void ReadFiles( bm::ClipboardBuffer, ReadFilesCallback cb ) override
	{
		std::move(cb).Run(bm::ClipboardFiles::New());
	}
	void ReadDataTransferCustomData( bm::ClipboardBuffer, const blink::String &,
					 ReadDataTransferCustomDataCallback cb ) override
	{
		std::move(cb).Run(blink::String(""));
	}
	void ReadAvailableCustomAndStandardFormats( ReadAvailableCustomAndStandardFormatsCallback cb ) override
	{
		blink::Vector<blink::String> types;
		std::string t;
		if ( br_tray_text(&t) ) types.push_back("text/plain");
		std::move(cb).Run(types);
	}
	void ReadUnsanitizedCustomFormat( const blink::String &, ReadUnsanitizedCustomFormatCallback cb ) override
	{
		std::move(cb).Run(mojo_base::BigBuffer());
	}

	/* what is written is kept until CommitWrite puts it in the tray as one set */
	void WriteText( const blink::String &text ) override
	{
		text_ = text.Utf8();
		have_ = true;
	}
	void WriteHtml( const blink::String &markup, const blink::KURL & ) override
	{
		html_ = markup.Utf8();
		have_ = true;
	}
	void WriteSvg( const blink::String & ) override {}
	void WriteSmartPasteMarker() override {}
	void WriteDataTransferCustomData( const blink::HashMap<blink::String, blink::String> & ) override {}
	void WriteBookmark( const blink::String &url, const blink::String & ) override
	{
		if ( text_.empty() ) text_ = url.Utf8();
		have_ = true;
	}
	void WriteImage( const SkBitmap &image ) override
	{
		SkDynamicMemoryWStream out;
		if ( SkPngEncoder::Encode(&out, image.pixmap(), {}) ) {
			sk_sp<SkData> d = out.detachAsData();
			png_.assign(d->bytes(), d->bytes() + d->size());
		}
		have_ = true;
	}
	void WriteUnsanitizedCustomFormat( const blink::String &, mojo_base::BigBuffer ) override {}
	void CommitWrite() override
	{
		if ( have_ ) {
			/* markup alone: its text, as the tray holds no markup */
			if ( text_.empty() && !html_.empty() ) text_ = TextOfMarkup(html_);
			std::string name = text_.substr(0, text_.find('\n'));
			if ( name.empty() ) name = png_.empty() ? "ブラウザから" : "ブラウザの画像";
			bool ok = br_tray_put(text_, png_, name);
			printf("browser: copied to the tray (%zu bytes of text, %zu of PNG)%s\n", text_.size(), png_.size(),
			       ok ? "" : ": the tray took nothing");
		}
		text_.clear();
		html_.clear();
		png_.clear();
		have_ = false;
	}
	void RegisterClipboardListener( mojo::PendingRemote<bm::ClipboardListener> l ) override
	{
		listeners_.Add(std::move(l));
	}

private:
	static std::string TextOfMarkup( const std::string &m )
	{
		std::string out;
		bool tag = false;
		for ( char c : m ) {
			if ( c == '<' ) tag = true;
			else if ( c == '>' ) tag = false;
			else if ( !tag ) out += c;
		}
		return out;
	}

	mojo::ReceiverSet<bm::ClipboardHost>	receivers_;
	mojo::RemoteSet<bm::ClipboardListener>	listeners_;
	std::string				text_, html_;
	std::vector<uint8_t>			png_;
	bool					have_ = false;
};

/* ---------------------------------------------------------------- the services by name */

Permissions *g_permissions;
Notifications *g_notifications;
PushMessagingImpl *g_push;
Quota *g_quota;
Locks *g_locks;
Clipboard *g_clipboard;

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

/* The origin's locks for a bucket's (buckets_tessronos.cc) */
void br_locks_bind( mojo::ScopedMessagePipeHandle pipe )
{
	BindOnThread<bm::LockManager, Locks>(&g_locks, mojo::PendingReceiver<bm::LockManager>(std::move(pipe)));
}

/*
 * A receiver a worker's broker is given: true when it is one of these
 * services (bound on the blob thread), false to leave it to the caller
 */
bool br_service_bind( mojo::GenericPendingReceiver &r )
{
	if ( auto p = r.As<bm::PermissionService>() ) {
		BindOnThread<bm::PermissionService, Permissions>(&g_permissions, std::move(p));
		return true;
	}
	if ( auto n = r.As<bm::NotificationService>() ) {
		BindOnThread<bm::NotificationService, Notifications>(&g_notifications, std::move(n));
		return true;
	}
	if ( auto pm = r.As<bm::PushMessaging>() ) {
		BindOnThread<bm::PushMessaging, PushMessagingImpl>(&g_push, std::move(pm));
		return true;
	}
	if ( auto q = r.As<bm::QuotaManagerHost>() ) {
		BindOnThread<bm::QuotaManagerHost, Quota>(&g_quota, std::move(q));
		return true;
	}
	if ( auto l = r.As<bm::LockManager>() ) {
		BindOnThread<bm::LockManager, Locks>(&g_locks, std::move(l));
		return true;
	}
	if ( br_cache_storage_bind(r) ) return true;
	if ( br_background_bind(r) ) return true;
	if ( br_background_fetch_bind(r) ) return true;
	if ( br_file_system_access_bind(r) ) return true;
	if ( br_files_bind(r) ) return true;
	if ( br_filesystem_bind(r, br_page_origin()) ) return true;
	if ( auto c = r.As<bm::ClipboardHost>() ) {
		BindOnThread<bm::ClipboardHost, Clipboard>(&g_clipboard, std::move(c));
		return true;
	}
	if ( auto g = r.As<bm::GpuDataManager>() ) {
		br_on_service_thread(base::BindOnce([]( mojo::PendingReceiver<bm::GpuDataManager> g ) {
			mojo::MakeSelfOwnedReceiver(std::make_unique<GpuDataManager>(), std::move(g));
		}, std::move(g)));
		return true;
	}
	if ( auto c = r.As<bm::BroadcastChannelProvider>() ) {
		br_on_service_thread(base::BindOnce([]( mojo::PendingReceiver<bm::BroadcastChannelProvider> c ) {
			mojo::MakeSelfOwnedReceiver(std::make_unique<ChannelProvider>(), std::move(c));
		}, std::move(c)));
		return true;
	}
	return false;
}

/*
 * An action of the menu 〈メディア〉 ("mplay": play or pause, "mprev",
 * "mnext", "mstop") told to the page's media session that takes it;
 * what came of it, for the message line
 */
std::string br_media_session_action( const std::string &code )
{
	mojo::ReceiverId	who = 0;
	uint64_t		best = 0;
	bool			found = false;
	ms::MediaSessionAction	act;
	std::string		title;

	std::lock_guard<std::mutex> hold(g_ms_lock);
	for ( auto &kv : g_ms ) {
		SessionState &st = kv.second;
		if ( !st.client.is_bound() ) continue;
		uint64_t rank = st.said + ( st.state == bm::MediaSessionPlaybackState::PLAYING ? ( 1ULL << 40 ) : 0 );
		if ( !found || rank > best ) {
			found = true;
			best = rank;
			who = kv.first;
		}
	}
	if ( !found ) return "このページはメディアの操作を受け付けていません";
	SessionState &st = g_ms[who];
	if ( code == "mplay" ) {
		act = ( st.state == bm::MediaSessionPlaybackState::PLAYING ) ? ms::MediaSessionAction::kPause
									      : ms::MediaSessionAction::kPlay;
	} else if ( code == "mprev" ) {
		act = ms::MediaSessionAction::kPreviousTrack;
	} else if ( code == "mnext" ) {
		act = ms::MediaSessionAction::kNextTrack;
	} else {
		act = ms::MediaSessionAction::kStop;
	}
	if ( st.actions.count(act) == 0 ) return "このページはその操作を受け付けていません";
	title = st.title;
	br_on_service_thread(base::BindOnce([]( mojo::ReceiverId who, ms::MediaSessionAction act ) {
		std::lock_guard<std::mutex> hold(g_ms_lock);
		auto it = g_ms.find(who);
		if ( it != g_ms.end() && it->second.client.is_bound() ) {
			it->second.client->DidReceiveAction(act, nullptr);
		}
	}, who, act));
	printf("browser: media session: action %d to %s\n", (int)act, title.c_str());
	return title.empty() ? std::string("ページに伝えました") : "「" + title + "」に伝えました";
}

/* The binders of the frames' broker, once when the engine is up */
bool br_isolated_allowed( const std::string &origin )
{
	std::lock_guard<std::mutex> hold(g_isolated_lock);
	return g_isolated.count(origin) != 0;
}

void br_isolated_allow_now( const std::string &origin )
{
	std::lock_guard<std::mutex> hold(g_isolated_lock);
	g_isolated.insert(origin);
}

void br_isolated_set( const std::string &origin, bool on )
{
	{
		std::lock_guard<std::mutex> hold(g_isolated_lock);
		if ( on ) {
			g_isolated.insert(origin);
		} else {
			g_isolated.erase(origin);
		}
	}
	printf("browser: %s %s as an isolated web app\n", origin.c_str(), on ? "treated" : "no longer treated");
	br_on_service_thread(base::BindOnce([]( std::string origin, bool on ) {
		if ( on ) {
			Decided()[{ origin, kIsolatedApp }] = bm::PermissionStatus::GRANTED;
		} else {
			Decided().erase({ origin, kIsolatedApp });
		}
		DecidedChanged();
	}, origin, on));
}

/* An answer another browser kept ('val' NULL: taken back), or one read back: known at once to the snapshot, to the services on their thread */
void TakePerm( void *, const char *key, int klen, const unsigned char *val, int vlen )
{
	std::string k(key, (size_t)klen);
	size_t nl = k.rfind('\n');
	int name = 0, status = 0;
	if ( nl == std::string::npos || !base::StringToInt(k.substr(nl + 1), &name) ) return;
	std::string v(val != nullptr ? (const char *)val : "", val != nullptr ? (size_t)vlen : 0);
	if ( val != nullptr && !base::StringToInt(v, &status) ) return;
	{
		std::lock_guard<std::mutex> hold(g_perm_lock);
		if ( val != nullptr ) g_perm_entries[k] = v;
		else g_perm_entries.erase(k);
	}
	br_on_service_thread(base::BindOnce(&DecidedOne, k.substr(0, nl), name, val != nullptr, status));
}

void RestorePermEntry( void *ctx, const char *key, int klen, const unsigned char *val, int vlen )
{
	if ( klen == 0 ) {
		if ( vlen > 0 ) br_on_service_thread(base::BindOnce(&RestorePerms, std::string((const char *)val, (size_t)vlen)));
		return;
	}
	TakePerm(ctx, key, klen, val, vlen);
}

void br_services_init()
{
	FrameBinder<bm::PermissionService, Permissions>(&g_permissions);
	FrameBinder<bm::NotificationService, Notifications>(&g_notifications);
	FrameBinder<bm::PushMessaging, PushMessagingImpl>(&g_push);
	g_perm_store = br_store_register_shared("perms", &SnapshotPerms, &TakePerm, nullptr, 500);
	(void)br_store_load_shared(g_perm_store, &RestorePermEntry, nullptr);
	br_sw_init();
	br_background_init();
	br_background_fetch_init();
	br_push_start(&PushDelivered);
	FrameBinder<bm::QuotaManagerHost, Quota>(&g_quota);
	FrameBinder<bm::LockManager, Locks>(&g_locks);
	FrameBinder<bm::ClipboardHost, Clipboard>(&g_clipboard);
	FrameBinder<bm::MediaSessionService, MediaSessions>(&g_media_sessions);
	blink::GetEmptyBrowserInterfaceBroker().SetBinderForTesting(
		pm::PaymentRequest::Name_, base::BindRepeating([]( mojo::ScopedMessagePipeHandle h ) {
			br_on_service_thread(base::BindOnce([]( mojo::PendingReceiver<pm::PaymentRequest> r ) {
				mojo::MakeSelfOwnedReceiver(std::make_unique<PaymentRequestImpl>(), std::move(r));
			}, mojo::PendingReceiver<pm::PaymentRequest>(std::move(h))));
		}));
	br_cache_storage_init();
	br_shared_worker_init();
	br_files_init();
	br_file_blobs_init();
	br_credentials_init();
	br_file_system_access_init();
	br_devices_init();
	br_ai_init();
	br_midi_init();
	br_direct_sockets_init();
	br_printing_init();
	br_speech_init();
	br_webnn_init();
	if ( br_hevc_media_init != nullptr ) br_hevc_media_init();
	br_serial_init();
	br_usb_hid_init();
	br_cookie_store_init();
	br_p2p_init();
	br_web_transport_init();
	br_buckets_init();
	br_filesystem_init();
	br_pip_init();
	br_storage_access_init();
}

/* A frame's BroadcastChannel connections on an associated endpoint (storage_access_tessronos.cc) */
void br_broadcast_frame_bind( mojo::ScopedInterfaceEndpointHandle handle )
{
	mojo::MakeSelfOwnedAssociatedReceiver(
		std::make_unique<ChannelProvider>(),
		mojo::PendingAssociatedReceiver<bm::BroadcastChannelProvider>(std::move(handle)));
}

/* A frame's associated interfaces: its BroadcastChannel connections, on the page's thread */
void br_services_frame_interfaces( blink::AssociatedInterfaceProvider *provider )
{
	provider->OverrideBinderForTesting(
		bm::BroadcastChannelProvider::Name_,
		base::BindRepeating([]( mojo::ScopedInterfaceEndpointHandle handle ) {
			mojo::MakeSelfOwnedAssociatedReceiver(
				std::make_unique<ChannelProvider>(),
				mojo::PendingAssociatedReceiver<bm::BroadcastChannelProvider>(std::move(handle)));
		}));
}
