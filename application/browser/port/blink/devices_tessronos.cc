/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	devices_tessronos.cc
 *	ブラウザ: what the web's interfaces to a machine's devices ask of a
 *	browser's process, answered for the machine there is (design 14.4.8).
 *
 *	The interfaces are Blink's own (navigator.getBattery, wakeLock, the
 *	sensors, IdleDetector, navigator.share, navigator.keyboard,
 *	setAppBadge, getInstalledRelatedApps,
 *	PresentationRequest); what they are told is what this machine has:
 *
 *	  - the battery: none, so as a machine on mains power is told
 *	    (charging, full);
 *	  - a wake lock is granted (the system does not put the screen to
 *	    sleep while a page shows);
 *	  - sensors (accelerometer, gyroscope, light, ...): none is there;
 *	  - idle: the user is active and the screen not locked;
 *	  - share: what a page shares (its title, text and address) goes into
 *	    the tray, as a copy does;
 *	  - the keyboard's layout map: the keys of the US layout the system
 *	    reads key codes in; locking keys is granted;
 *	  - MIDI: in midi_tessronos.cc;
 *	  - an application's badge: there is no place to show one;
 *	  - installed related applications: none;
 *	  - presentation: there is no second screen;
 *	  - Bluetooth: there is no adapter;
 *	  - cameras and microphones (navigator.mediaDevices, getUserMedia):
 *	    the cameras of the camera device, listed each under an id of 64 hexadecimal digits; the
 *	    microphone is the sound device's recording, when it records
 *	    (br_mic_available), listed as one input; getUserMedia asks the
 *	    user on a panel, the stream's sound is captured through the
 *	    platform (br_audio_capturer) and its pictures through the video
 *	    capture host (camera_tessronos.cc);
 *	  - speech: in speech_tessronos.cc;
 *	  - the device's posture: a screen that does not fold (continuous);
 *	  - the screen's orientation cannot be locked (a desktop's screen);
 *	  - XR: no headset; an immersive session finds no runtime (inline
 *	    sessions are the page's own and need none);
 *	  - the local fonts (queryLocalFonts): the fonts the browser draws
 *	    with, once the user has allowed it;
 *	  - vibration (navigator.vibrate): there is no vibrator, so a
 *	    vibration is over as soon as it is asked for;
 *	  - Compute Pressure (PressureObserver): the share of the processors'
 *	    time spent running tasks (br_cpu.cc), each second, as nominal,
 *	    fair, serious or critical;
 *	  - EyeDropper: the user picks a point of the page in the window
 *	    (br_pick_color), whose colour is given;
 *	  - how smoothly WebRTC's video is coded (MediaCapabilities'
 *	    encodingInfo and decodingInfo of the type "webrtc"): smooth up
 *	    to the size and rate the processor keeps up with
 *	    (br_media_decodes_in_time), there being no history of calls.
 *
 *	The services are on the services' thread.
 */

#include <stdio.h>

#include <limits>
#include <memory>
#include <map>
#include <set>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "media/midi/midi_service.mojom-blink.h"
#include "services/device/public/mojom/battery_monitor.mojom-blink.h"
#include "services/device/public/mojom/battery_status.mojom-blink.h"
#include "services/device/public/mojom/sensor.mojom-blink.h"
#include "services/device/public/mojom/sensor_provider.mojom-blink.h"
#include "services/device/public/mojom/vibration_manager.mojom-blink.h"
#include "media/base/audio_parameters.h"
#include "media/capture/video_capture_types.h"
#include "base/unguessable_token.h"
#include "third_party/boringssl/src/include/openssl/sha.h"
#include "third_party/blink/public/common/mediastream/media_stream_request.h"
#include "media/mojo/mojom/webrtc_video_perf.mojom-blink.h"
#include "third_party/blink/public/mojom/choosers/color_chooser.mojom-blink.h"
#include "third_party/blink/public/mojom/compute_pressure/web_pressure_manager.mojom-blink.h"
#include "third_party/blink/public/mojom/compute_pressure/web_pressure_update.mojom-blink.h"
#include "mojo/public/cpp/bindings/remote_set.h"
#include "services/device/public/mojom/wake_lock.mojom-blink.h"
#include "third_party/blink/public/mojom/badging/badging.mojom-blink.h"
#include "third_party/blink/public/mojom/device_posture/device_posture_provider.mojom-blink.h"
#include "third_party/blink/public/mojom/font_access/font_access.mojom-blink.h"
#include "third_party/blink/public/common/font_access/font_enumeration_table.pb.h"
#include "device/vr/public/mojom/vr_service.mojom-blink.h"
#include "base/memory/read_only_shared_memory_region.h"
#include "skia/ext/font_utils.h"
#include "third_party/skia/include/core/SkFontMgr.h"
#include "third_party/skia/include/core/SkString.h"
#include "third_party/skia/include/core/SkTypeface.h"
#include "services/device/public/mojom/screen_orientation.mojom-blink.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "mojo/public/cpp/bindings/self_owned_associated_receiver.h"
#include "third_party/blink/public/mojom/mediastream/media_devices.mojom-blink.h"
#include "third_party/blink/public/mojom/mediastream/media_stream.mojom-blink.h"
#include "third_party/blink/public/mojom/bluetooth/web_bluetooth.mojom-blink.h"
#include "third_party/blink/public/mojom/idle/idle_manager.mojom-blink.h"
#include "third_party/blink/public/mojom/installedapp/installed_app_provider.mojom-blink.h"
#include "third_party/blink/public/mojom/installedapp/related_application.mojom-blink.h"
#include "third_party/blink/public/mojom/keyboard_lock/keyboard_lock.mojom-blink.h"
#include "third_party/blink/public/mojom/presentation/presentation.mojom-blink.h"
#include "third_party/blink/public/mojom/wake_lock/wake_lock.mojom-blink.h"
#include "third_party/blink/public/mojom/webshare/webshare.mojom-blink.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

#include "../net/br_tray.h"

/* The services' thread (blob_tessronos.cc) */
void br_on_service_thread( base::OnceClosure task );
void br_net_post_delayed( base::OnceClosure work, int ms );	/* net_fetch_tessronos.cc */
extern "C" bool br_cpu_load( double *load, int *ncpu );		/* port/net/br_cpu.cc */
bool br_pick_color( uint32_t *color );				/* br_main.cc */
extern "C" int br_mic_available( void );			/* port/media/br_snd.cc */
extern "C" int br_snd_available( void );
extern "C" int br_cam_list( char (*names)[64], unsigned char (*ids)[16], int max );	/* port/media/br_cam.cc */
void br_camera_session( const base::UnguessableToken &session, int cam );	/* camera_tessronos.cc */
blink::Vector<media::VideoCaptureFormat> br_camera_formats( int cam );
void br_camera_init();
bool br_ask_permission( const std::string &origin, const std::string &what );	/* br_main.cc */
std::string br_page_origin();					/* br_blink.cc */
/* The answers on the microphone and the cameras (services_tessronos.cc) */
void br_media_answered( const std::string &origin, bool audio, bool video, bool yes );
int br_media_decided( const std::string &origin, bool video );
#include "../media/br_share.h"
/* The capture of the screen (screen_tessronos.cc) */
void br_screen_session( const base::UnguessableToken &session, const BrShareChoice &chosen );
void br_share_audio_session( const base::UnguessableToken &session, int mode, int pid );	/* port/media/br_audio_sink.cc */
extern "C" int br_media_decodes_in_time( int width, int height, double framerate );	/* media_player.cc */

namespace {

namespace bm = blink::mojom::blink;
namespace dm = device::mojom::blink;
namespace mm = midi::mojom::blink;

/* ---------------------------------------------------------------- the battery, a wake lock, sensors */

class Battery final : public dm::BatteryMonitor {
public:
	void Bind( mojo::PendingReceiver<dm::BatteryMonitor> r ) { receivers_.Add(this, std::move(r)); }

	/* the first query is answered at once; later ones only on a change, which never comes */
	void QueryNextStatus( QueryNextStatusCallback cb ) override
	{
		if ( told_.count(receivers_.current_receiver()) ) {
			waiting_.push_back(std::move(cb));
			return;
		}
		told_.insert(receivers_.current_receiver());
		std::move(cb).Run(dm::BatteryStatus::New(true, 0.0, std::numeric_limits<double>::infinity(), 1.0));
	}

private:
	mojo::ReceiverSet<dm::BatteryMonitor>	receivers_;
	std::set<mojo::ReceiverId>		told_;
	std::vector<QueryNextStatusCallback>	waiting_;
};

class WakeLock final : public dm::WakeLock {
public:
	void RequestWakeLock() override { printf("browser: a wake lock taken\n"); }
	void CancelWakeLock() override {}
	void AddClient( mojo::PendingReceiver<dm::WakeLock> r ) override
	{
		mojo::MakeSelfOwnedReceiver(std::make_unique<WakeLock>(), std::move(r));
	}
	void ChangeType( dm::WakeLockType, ChangeTypeCallback cb ) override { std::move(cb).Run(true); }
	void HasWakeLockForTests( HasWakeLockForTestsCallback cb ) override { std::move(cb).Run(true); }
};

class WakeLocks final : public bm::WakeLockService {
public:
	void Bind( mojo::PendingReceiver<bm::WakeLockService> r ) { receivers_.Add(this, std::move(r)); }
	void GetWakeLock( dm::WakeLockType, dm::WakeLockReason, const blink::String &,
			  mojo::PendingReceiver<dm::WakeLock> r ) override
	{
		mojo::MakeSelfOwnedReceiver(std::make_unique<WakeLock>(), std::move(r));
	}

private:
	mojo::ReceiverSet<bm::WakeLockService>	receivers_;
};

class Sensors final : public dm::SensorProvider {
public:
	void Bind( mojo::PendingReceiver<dm::SensorProvider> r ) { receivers_.Add(this, std::move(r)); }
	void GetSensor( dm::SensorType, mojo::PendingReceiver<dm::SensorClientController>, bool, GetSensorCallback cb ) override
	{
		std::move(cb).Run(dm::SensorCreationResult::ERROR_NOT_AVAILABLE, nullptr);
	}
	void CreateVirtualSensor( dm::SensorType, dm::VirtualSensorMetadataPtr, CreateVirtualSensorCallback cb ) override
	{
		std::move(cb).Run(dm::CreateVirtualSensorResult::kSensorTypeAlreadyOverridden);
	}
	void UpdateVirtualSensor( dm::SensorType, dm::SensorReadingRawPtr, UpdateVirtualSensorCallback cb ) override
	{
		std::move(cb).Run(dm::UpdateVirtualSensorResult::kSensorTypeNotOverridden);
	}
	void RemoveVirtualSensor( dm::SensorType, RemoveVirtualSensorCallback cb ) override { std::move(cb).Run(); }
	void GetVirtualSensorInformation( dm::SensorType, GetVirtualSensorInformationCallback cb ) override
	{
		std::move(cb).Run(dm::GetVirtualSensorInformationResult::NewError(
			dm::GetVirtualSensorInformationError::kSensorTypeNotOverridden));
	}

private:
	mojo::ReceiverSet<dm::SensorProvider>	receivers_;
};

/* ---------------------------------------------------------------- idle, share, keyboard */

class Idle final : public bm::IdleManager {
public:
	void Bind( mojo::PendingReceiver<bm::IdleManager> r ) { receivers_.Add(this, std::move(r)); }
	void AddMonitor( mojo::PendingRemote<bm::IdleMonitor> monitor, AddMonitorCallback cb ) override
	{
		monitors_.emplace_back(std::move(monitor));
		std::move(cb).Run(bm::IdleManagerError::kSuccess, bm::IdleState::New(std::nullopt, false));
	}

private:
	mojo::ReceiverSet<bm::IdleManager>		receivers_;
	std::vector<mojo::Remote<bm::IdleMonitor>>	monitors_;
};

class Sharing final : public bm::ShareService {
public:
	void Bind( mojo::PendingReceiver<bm::ShareService> r ) { receivers_.Add(this, std::move(r)); }
	void Share( const blink::String &title, const blink::String &text, const blink::KURL &url,
		    blink::Vector<bm::SharedFilePtr>, ShareCallback cb ) override
	{
		std::string t = title.Utf8(), all;
		for ( const std::string &line : { t, text.Utf8(), url.IsValid() ? url.GetString().Utf8() : std::string() } ) {
			if ( line.empty() ) continue;
			if ( !all.empty() ) all += "\n";
			all += line;
		}
		if ( all.empty() ) {
			std::move(cb).Run(bm::ShareError::INTERNAL_ERROR);
			return;
		}
		std::string name = t.empty() ? std::string("共有") : t;
		/* the tray is written off the services' thread; the page is told once it is in */
		std::thread([all, name, cb = std::move(cb)]() mutable {
			bool ok = br_tray_put(all, std::vector<uint8_t>(), name);
			printf("browser: shared into the tray: %s\n", name.c_str());
			br_on_service_thread(base::BindOnce([]( ShareCallback cb, bool ok ) {
				std::move(cb).Run(ok ? bm::ShareError::OK : bm::ShareError::INTERNAL_ERROR);
			}, std::move(cb), ok));
		}).detach();
	}

private:
	mojo::ReceiverSet<bm::ShareService>	receivers_;
};

class Keyboard final : public bm::KeyboardLockService {
public:
	void Bind( mojo::PendingReceiver<bm::KeyboardLockService> r ) { receivers_.Add(this, std::move(r)); }
	void RequestKeyboardLock( const blink::Vector<blink::String> &, RequestKeyboardLockCallback cb ) override
	{
		std::move(cb).Run(bm::KeyboardLockRequestResult::kSuccess);
	}
	void CancelKeyboardLock() override {}
	void GetKeyboardLayoutMap( GetKeyboardLayoutMapCallback cb ) override
	{
		auto r = bm::GetKeyboardLayoutMapResult::New();
		r->status = bm::GetKeyboardLayoutMapStatus::kSuccess;
		for ( char c = 'a'; c <= 'z'; c++ ) {
			r->layout_map.Set(blink::String::FromUtf8(std::string("Key") + (char)( c - 'a' + 'A' )),
					  blink::String::FromUtf8(std::string(1, c)));
		}
		for ( char c = '0'; c <= '9'; c++ ) {
			r->layout_map.Set(blink::String::FromUtf8(std::string("Digit") + c), blink::String::FromUtf8(std::string(1, c)));
		}
		static const char *const kPunct[][2] = {
			{ "Minus", "-" }, { "Equal", "=" }, { "BracketLeft", "[" }, { "BracketRight", "]" },
			{ "Backslash", "\\" }, { "Semicolon", ";" }, { "Quote", "'" }, { "Backquote", "`" },
			{ "Comma", "," }, { "Period", "." }, { "Slash", "/" },
			/* the key next to the left Shift of a 105-key board, as a US layout on Linux has it */
			{ "IntlBackslash", "<" },
		};
		for ( const auto &p : kPunct ) r->layout_map.Set(blink::String::FromUtf8(p[0]), blink::String::FromUtf8(p[1]));
		std::move(cb).Run(std::move(r));
	}

private:
	mojo::ReceiverSet<bm::KeyboardLockService>	receivers_;
};

/* ---------------------------------------------------------------- badges, related apps, presentation */

/* ---------------------------------------------------------------- Compute Pressure */

/* The processors' load each second, told to the observers as a state */
class Pressure final : public bm::WebPressureManager {
public:
	void Bind( mojo::PendingReceiver<bm::WebPressureManager> r ) { receivers_.Add(this, std::move(r)); }

	void AddClient( dm::PressureSource, mojo::PendingAssociatedRemote<bm::WebPressureClient> client,
			AddClientCallback cb ) override
	{
		clients_.Add(std::move(client));
		std::move(cb).Run(dm::PressureManagerAddClientResult::kOk);
		if ( !ticking_ ) {
			ticking_ = true;
			double l;
			int n;
			(void)br_cpu_load(&l, &n);	/* the first count, which later ones are measured against */
			Next();
		}
	}

private:
	void Next()
	{
		br_net_post_delayed(base::BindOnce([]( Pressure *self ) {
			br_on_service_thread(base::BindOnce(&Pressure::Tick, base::Unretained(self)));
		}, base::Unretained(this)), 1000);
	}

	void Tick()
	{
		if ( clients_.empty() ) {
			ticking_ = false;
			return;
		}
		double l = 0;
		int n = 0;
		if ( br_cpu_load(&l, &n) ) {
			dm::PressureState st = l < 0.6 ? dm::PressureState::kNominal
					     : l < 0.75 ? dm::PressureState::kFair
					     : l < 0.9 ? dm::PressureState::kSerious : dm::PressureState::kCritical;
			for ( auto &c : clients_ ) {
				c->OnPressureUpdated(bm::WebPressureUpdate::New(dm::PressureSource::kCpu, st,
										 base::TimeTicks::Now()));
			}
		}
		Next();
	}

	mojo::ReceiverSet<bm::WebPressureManager>		receivers_;
	mojo::AssociatedRemoteSet<bm::WebPressureClient>	clients_;
	bool							ticking_ = false;
};

/* ---------------------------------------------------------------- EyeDropper */

/* The colour of a point the user presses on the page, waited for off this thread */
class EyeDropper final : public bm::EyeDropperChooser {
public:
	void Bind( mojo::PendingReceiver<bm::EyeDropperChooser> r ) { receivers_.Add(this, std::move(r)); }

	void Choose( ChooseCallback cb ) override
	{
		std::thread([]( ChooseCallback cb ) {
			uint32_t color = 0;
			bool ok = br_pick_color(&color);
			br_on_service_thread(base::BindOnce([]( ChooseCallback cb, bool ok, uint32_t color ) {
				std::move(cb).Run(ok, color);
			}, std::move(cb), ok, color));
		}, std::move(cb)).detach();
	}

private:
	mojo::ReceiverSet<bm::EyeDropperChooser>	receivers_;
};

/* ---------------------------------------------------------------- WebRTC's coding, how smooth */

class WebrtcPerf final : public media::mojom::blink::WebrtcVideoPerfHistory {
public:
	void Bind( mojo::PendingReceiver<media::mojom::blink::WebrtcVideoPerfHistory> r ) { receivers_.Add(this, std::move(r)); }

	void GetPerfInfo( media::mojom::blink::WebrtcPredictionFeaturesPtr features, int32_t fps,
			  GetPerfInfoCallback cb ) override
	{
		/* the pixels of a frame as one row of them: the rule weighs the product */
		std::move(cb).Run(br_media_decodes_in_time(features->video_pixels, 1, fps) != 0);
	}

private:
	mojo::ReceiverSet<media::mojom::blink::WebrtcVideoPerfHistory>	receivers_;
};

/* No vibrator: a vibration is over at once */
class Vibration final : public dm::VibrationManager {
public:
	void Bind( mojo::PendingReceiver<dm::VibrationManager> r ) { receivers_.Add(this, std::move(r)); }
	void Vibrate( int64_t, VibrateCallback cb ) override { std::move(cb).Run(); }
	void Cancel( CancelCallback cb ) override { std::move(cb).Run(); }

private:
	mojo::ReceiverSet<dm::VibrationManager>	receivers_;
};

/*
 * The application's badge (navigator.setAppBadge): kept for the window's
 * band, which shows it before the page's title (br_main.cc, br_badge_text)
 */
std::mutex	g_badge_lock;
std::string	g_badge;

class Badge final : public bm::BadgeService {
public:
	void Bind( mojo::PendingReceiver<bm::BadgeService> r ) { receivers_.Add(this, std::move(r)); }
	void SetBadge( bm::BadgeValuePtr v ) override
	{
		std::string s = "●";
		if ( v && v->is_number() ) s = std::to_string(v->get_number() > 99 ? 99 : v->get_number())
						+ ( v->get_number() > 99 ? "+" : "" );
		std::lock_guard<std::mutex> hold(g_badge_lock);
		g_badge = s;
	}
	void ClearBadge() override
	{
		std::lock_guard<std::mutex> hold(g_badge_lock);
		g_badge.clear();
	}

private:
	mojo::ReceiverSet<bm::BadgeService>	receivers_;
};

class RelatedApps final : public bm::InstalledAppProvider {
public:
	void Bind( mojo::PendingReceiver<bm::InstalledAppProvider> r ) { receivers_.Add(this, std::move(r)); }
	void FilterInstalledApps( blink::Vector<bm::RelatedApplicationPtr>, const blink::KURL &, bool,
				  FilterInstalledAppsCallback cb ) override
	{
		std::move(cb).Run(blink::Vector<bm::RelatedApplicationPtr>());
	}

private:
	mojo::ReceiverSet<bm::InstalledAppProvider>	receivers_;
};

class Presentation final : public bm::PresentationService {
public:
	void Bind( mojo::PendingReceiver<bm::PresentationService> r ) { receivers_.Add(this, std::move(r)); }
	void SetController( mojo::PendingRemote<bm::PresentationController> c ) override
	{
		controller_.reset();
		controller_.Bind(std::move(c));
	}
	void SetReceiver( mojo::PendingRemote<bm::PresentationReceiver> ) override {}
	void SetDefaultPresentationUrls( const blink::Vector<blink::KURL> & ) override {}
	void ListenForScreenAvailability( const blink::KURL &url ) override
	{
		if ( controller_ ) controller_->OnScreenAvailabilityUpdated(url, bm::ScreenAvailability::UNAVAILABLE);
	}
	void StopListeningForScreenAvailability( const blink::KURL & ) override {}
	void StartPresentation( const blink::Vector<blink::KURL> &, StartPresentationCallback cb ) override
	{
		std::move(cb).Run(nullptr, bm::PresentationError::New(bm::PresentationErrorType::NO_AVAILABLE_SCREENS,
								       "There is no screen to present on."));
	}
	void ReconnectPresentation( const blink::Vector<blink::KURL> &, const blink::String &,
				    ReconnectPresentationCallback cb ) override
	{
		std::move(cb).Run(nullptr, bm::PresentationError::New(bm::PresentationErrorType::NO_PRESENTATION_FOUND,
								       "There is no presentation."));
	}
	void CloseConnection( const blink::KURL &, const blink::String & ) override {}
	void Terminate( const blink::KURL &, const blink::String & ) override {}

private:
	mojo::ReceiverSet<bm::PresentationService>	receivers_;
	mojo::Remote<bm::PresentationController>	controller_;
};

/* ---------------------------------------------------------------- Bluetooth: no adapter */

class Bluetooth final : public bm::WebBluetoothService {
public:
	void Bind( mojo::PendingReceiver<bm::WebBluetoothService> r ) { receivers_.Add(this, std::move(r)); }

	void GetAvailability( GetAvailabilityCallback cb ) override { std::move(cb).Run(false); }
	void RequestDevice( bm::WebBluetoothRequestDeviceOptionsPtr, RequestDeviceCallback cb ) override
	{
		std::move(cb).Run(kNone, nullptr);
	}
	void GetDevices( GetDevicesCallback cb ) override { std::move(cb).Run(blink::Vector<bm::WebBluetoothDevicePtr>()); }
	void ForgetDevice( const blink::WebBluetoothDeviceId &, ForgetDeviceCallback cb ) override { std::move(cb).Run(); }
	void RemoteServerConnect( const blink::WebBluetoothDeviceId &, mojo::PendingAssociatedRemote<bm::WebBluetoothServerClient>,
				  RemoteServerConnectCallback cb ) override
	{
		std::move(cb).Run(kNone);
	}
	void RemoteServerDisconnect( const blink::WebBluetoothDeviceId & ) override {}
	void RemoteServerGetPrimaryServices( const blink::WebBluetoothDeviceId &, bm::WebBluetoothGATTQueryQuantity,
					     const blink::String &, RemoteServerGetPrimaryServicesCallback cb ) override
	{
		std::move(cb).Run(kNone, std::nullopt);
	}
	void RemoteServiceGetCharacteristics( const blink::String &, bm::WebBluetoothGATTQueryQuantity, const blink::String &,
					      RemoteServiceGetCharacteristicsCallback cb ) override
	{
		std::move(cb).Run(kNone, std::nullopt);
	}
	void RemoteCharacteristicReadValue( const blink::String &, RemoteCharacteristicReadValueCallback cb ) override
	{
		std::move(cb).Run(kNone, base::span<const uint8_t>());
	}
	void RemoteCharacteristicWriteValue( const blink::String &, base::span<const uint8_t>, bm::WebBluetoothWriteType,
					     RemoteCharacteristicWriteValueCallback cb ) override
	{
		std::move(cb).Run(kNone);
	}
	void RemoteCharacteristicStartNotifications( const blink::String &,
						     mojo::PendingAssociatedRemote<bm::WebBluetoothCharacteristicClient>,
						     RemoteCharacteristicStartNotificationsCallback cb ) override
	{
		std::move(cb).Run(kNone);
	}
	void RemoteCharacteristicStopNotifications( const blink::String &, RemoteCharacteristicStopNotificationsCallback cb ) override
	{
		std::move(cb).Run();
	}
	void RemoteCharacteristicGetDescriptors( const blink::String &, bm::WebBluetoothGATTQueryQuantity, const blink::String &,
						 RemoteCharacteristicGetDescriptorsCallback cb ) override
	{
		std::move(cb).Run(kNone, std::nullopt);
	}
	void RemoteDescriptorReadValue( const blink::String &, RemoteDescriptorReadValueCallback cb ) override
	{
		std::move(cb).Run(kNone, base::span<const uint8_t>());
	}
	void RemoteDescriptorWriteValue( const blink::String &, base::span<const uint8_t>, RemoteDescriptorWriteValueCallback cb ) override
	{
		std::move(cb).Run(kNone);
	}
	void RequestScanningStart( mojo::PendingAssociatedRemote<bm::WebBluetoothAdvertisementClient>,
				   bm::WebBluetoothRequestLEScanOptionsPtr, RequestScanningStartCallback cb ) override
	{
		std::move(cb).Run(kNone);
	}
	void WatchAdvertisementsForDevice( const blink::WebBluetoothDeviceId &,
					   mojo::PendingAssociatedRemote<bm::WebBluetoothAdvertisementClient>,
					   WatchAdvertisementsForDeviceCallback cb ) override
	{
		std::move(cb).Run(kNone);
	}

private:
	static constexpr bm::WebBluetoothResult	kNone = bm::WebBluetoothResult::NO_BLUETOOTH_ADAPTER;
	mojo::ReceiverSet<bm::WebBluetoothService>	receivers_;
};

/* ---------------------------------------------------------------- cameras, microphones */

namespace med = media::mojom::blink;

/*
 * The origins the user let use the microphone and the cameras in this
 * run (MediaStreams): what the devices' names and ids are shown to
 */
std::set<std::string> &MicOrigins()
{
	static auto *s = new std::set<std::string>();
	return *s;
}

std::set<std::string> &CamOrigins()
{
	static auto *s = new std::set<std::string>();
	return *s;
}

class MediaDevices final : public bm::MediaDevicesDispatcherHost {
public:
	void Bind( mojo::PendingReceiver<bm::MediaDevicesDispatcherHost> r ) { receivers_.Add(this, std::move(r)); }

	/*
	 * The cameras the camera device has (CamId); the microphone when
	 * the sound device records; audio goes out through the one sound
	 * device, the default output (an AudioContext's and a media
	 * element's sinkId "" or "default")
	 */
	/*
	 * As a browser lists them to a page not yet let use them: one device
	 * of each kind there is, with no id, group or name (the microphone's
	 * permission shows the microphones and the outputs, the camera's the
	 * cameras), so that the page cannot tell the machine by its devices
	 */
	void EnumerateDevices( bool, bool, bool, bool video_caps, bool audio_caps, EnumerateDevicesCallback cb ) override
	{
		blink::Vector<blink::Vector<blink::WebMediaDeviceInfo>> lists(3);
		blink::Vector<bm::AudioInputDeviceCapabilitiesPtr> caps;
		std::string origin = br_page_origin();
		bool mic_ok = MicOrigins().count(origin) != 0, cam_ok = CamOrigins().count(origin) != 0;
		if ( br_mic_available() ) {
			lists[0].push_back(mic_ok ? blink::WebMediaDeviceInfo("default", "マイク", "mic")
						  : blink::WebMediaDeviceInfo("", "", ""));
			if ( audio_caps && mic_ok ) caps.push_back(MicCaps());
		}
		if ( br_snd_available() ) {
			lists[2].push_back(mic_ok ? blink::WebMediaDeviceInfo("default", "スピーカー", "speaker")
						  : blink::WebMediaDeviceInfo("", "", ""));
		}
		char names[BrCams][64];
		int n = br_cam_list(names, nullptr, BrCams);
		for ( int i = 0; i < n; i++ ) {
			if ( !cam_ok ) {
				lists[1].push_back(blink::WebMediaDeviceInfo("", "", ""));
				break;
			}
			std::string id = CamId(i);
			lists[1].push_back(blink::WebMediaDeviceInfo(id, names[i], id));
		}
		if ( !cam_ok ) video_caps = false;
		std::move(cb).Run(lists, video_caps ? CamCaps() : blink::Vector<bm::VideoInputDeviceCapabilitiesPtr>(),
				  std::move(caps));
	}
	void GetVideoInputCapabilities( GetVideoInputCapabilitiesCallback cb ) override
	{
		std::move(cb).Run(CamCaps());
	}
	void GetAllVideoInputDeviceFormats( const blink::String &id, GetAllVideoInputDeviceFormatsCallback cb ) override
	{
		std::move(cb).Run(br_camera_formats(CamOf(id)));
	}
	void GetAvailableVideoInputDeviceFormats( const blink::String &id, GetAvailableVideoInputDeviceFormatsCallback cb ) override
	{
		std::move(cb).Run(br_camera_formats(CamOf(id)));
	}

	static constexpr int BrCams = 4;

	/*
	 * A camera's device id for the page's origin: SHA-256 of the origin
	 * and the UUID (v7) of the camera's unit object, as 64 letters of
	 * lowercase hexadecimal -- the form a device id other than the
	 * default must have to pass the checks of the messages it travels in.
	 * The same camera has the same id for an origin across runs, and
	 * another for another origin, so that sites cannot tell one person by
	 * it between them.
	 */
	static std::string HashId( const unsigned char uuid[16], const std::string &origin )
	{
		static const char hex[] = "0123456789abcdef";
		std::string in = origin + "\n";
		in.append((const char *)uuid, 16);
		uint8_t d[SHA256_DIGEST_LENGTH];
		SHA256((const uint8_t *)in.data(), in.size(), d);
		std::string out;
		for ( uint8_t b : d ) {
			out += hex[b >> 4];
			out += hex[b & 15];
		}
		return out;
	}

	static std::string CamId( int cam )
	{
		char names[BrCams][64];
		unsigned char ids[BrCams][16];
		int n = br_cam_list(names, ids, BrCams);
		return ( cam >= 0 && cam < n ) ? HashId(ids[cam], br_page_origin()) : std::string();
	}

	/* The camera of a device id: anything not one of theirs is camera 0 */
	static int CamOf( const blink::String &id )
	{
		char names[BrCams][64];
		unsigned char ids[BrCams][16];
		std::string s = id.Utf8(), origin = br_page_origin();
		int n = br_cam_list(names, ids, BrCams);
		for ( int i = 0; i < n; i++ ) {
			if ( s == HashId(ids[i], origin) ) return i;
		}
		return 0;
	}

	static blink::Vector<bm::VideoInputDeviceCapabilitiesPtr> CamCaps()
	{
		blink::Vector<bm::VideoInputDeviceCapabilitiesPtr> v;
		char names[BrCams][64];
		unsigned char ids[BrCams][16];
		int n = br_cam_list(names, ids, BrCams);
		std::string origin = br_page_origin();
		for ( int i = 0; i < n; i++ ) {
			blink::String id = blink::String::FromUtf8(HashId(ids[i], origin));
			v.push_back(bm::VideoInputDeviceCapabilities::New(id, id, media::VideoCaptureControlSupport(),
									  br_camera_formats(i), bm::FacingMode::kNone,
									  std::nullopt));
		}
		return v;
	}
	void GetAudioInputCapabilities( GetAudioInputCapabilitiesCallback cb ) override
	{
		blink::Vector<bm::AudioInputDeviceCapabilitiesPtr> caps;
		if ( br_mic_available() ) caps.push_back(MicCaps());
		std::move(cb).Run(std::move(caps));
	}

	/* The microphone: 48 kHz stereo, buffers of 10 ms */
	static bm::AudioInputDeviceCapabilitiesPtr MicCaps()
	{
		media::AudioParameters p = MicParams();
		return bm::AudioInputDeviceCapabilities::New("default", "mic", p, true, p.channels(), p.sample_rate(),
							      base::Milliseconds(10));
	}

public:
	static media::AudioParameters MicParams()
	{
		return media::AudioParameters(media::AudioParameters::AUDIO_PCM_LOW_LATENCY,
					      media::ChannelLayoutConfig::Stereo(), 48000, 480);
	}
	void AddMediaDevicesListener( bool, bool, bool, mojo::PendingRemote<bm::MediaDevicesListener> ) override {}
	void SetCaptureHandleConfig( bm::CaptureHandleConfigPtr ) override {}
	void CloseFocusWindowOfOpportunity( const blink::String & ) override {}
	void ProduceSubCaptureTargetId( med::SubCaptureTargetType, ProduceSubCaptureTargetIdCallback cb ) override
	{
		std::move(cb).Run(blink::String());
	}
	void SetPreferredSinkId( const blink::String &, SetPreferredSinkIdCallback cb ) override
	{
		std::move(cb).Run(static_cast<mojo::NativeEnum>(media::OUTPUT_DEVICE_STATUS_ERROR_NOT_FOUND));
	}
	void SelectAudioOutput( const blink::String &, SelectAudioOutputCallback cb ) override
	{
		auto r = bm::SelectAudioOutputResult::New();
		r->status = bm::AudioOutputStatus::kNoDevices;
		std::move(cb).Run(std::move(r));
	}

private:
	mojo::ReceiverSet<bm::MediaDevicesDispatcherHost>	receivers_;
};

class MediaStreams final : public bm::MediaStreamDispatcherHost {
public:
	void Bind( mojo::PendingReceiver<bm::MediaStreamDispatcherHost> r ) { receivers_.Add(this, std::move(r)); }

	/*
	 * A stream of the microphone, of a camera, or of both, the user asked
	 * on a panel for what the origin has not been given yet (the answer
	 * kept for the origin and each of the two while the browser runs,
	 * and what navigator.permissions answers of them); refused when one
	 * of them was refused
	 */
	void GenerateStreams( int32_t, const blink::StreamControls &controls, bool, bm::StreamSelectionInfoPtr,
			      GenerateStreamsCallback cb ) override
	{
		if ( controls.video.stream_type == bm::MediaStreamType::DISPLAY_VIDEO_CAPTURE ) {
			Display(controls, std::move(cb));
			return;
		}
		bool audio = controls.audio.stream_type == bm::MediaStreamType::DEVICE_AUDIO_CAPTURE;
		bool video = controls.video.stream_type == bm::MediaStreamType::DEVICE_VIDEO_CAPTURE;
		bool other = ( controls.audio.requested() && !audio ) || ( controls.video.requested() && !video );
		char names[MediaDevices::BrCams][64];
		int ncam = video ? br_cam_list(names, nullptr, MediaDevices::BrCams) : 0;
		if ( other || ( !audio && !video ) || ( audio && !br_mic_available() ) || ( video && ncam == 0 ) ) {
			printf("browser: getUserMedia: there is no %s\n",
			       other ? "such source" : ( video && ncam == 0 ) ? "camera" : "microphone");
			std::move(cb).Run(bm::MediaStreamRequestResult::NO_HARDWARE, blink::String(""), nullptr, false);
			return;
		}
		int cam = 0;
		if ( video && !controls.video.device_ids.empty() ) {
			cam = MediaDevices::CamOf(blink::String::FromUtf8(controls.video.device_ids[0]));
			if ( cam >= ncam ) cam = 0;
		}
		Want w{audio, video, cam, video ? std::string(names[cam]) : std::string()};
		std::string origin = br_page_origin();
		int mic = audio ? br_media_decided(origin, false) : 1, camera = video ? br_media_decided(origin, true) : 1;
		if ( mic == 0 || camera == 0 || ( mic == 1 && camera == 1 ) ) {
			Answer(mic == 1 && camera == 1, w, std::move(cb));
			return;
		}
		bool ask_mic = mic < 0, ask_cam = camera < 0;
		std::string what = ask_mic && ask_cam ? "カメラとマイクの使用" : ask_cam ? "カメラの使用" : "マイクの使用";
		std::thread([this, origin, what, w, ask_mic, ask_cam, cb = std::move(cb)]() mutable {
			bool yes = br_ask_permission(origin, what);
			br_on_service_thread(base::BindOnce([]( MediaStreams *self, std::string origin, bool yes, bool ask_mic,
								bool ask_cam, Want w, GenerateStreamsCallback cb ) {
				br_media_answered(origin, ask_mic, ask_cam, yes);
				if ( yes && w.audio ) MicOrigins().insert(origin);
				if ( yes && w.video ) CamOrigins().insert(origin);
				self->Answer(yes, w, std::move(cb));
			}, base::Unretained(this), origin, yes, ask_mic, ask_cam, w, std::move(cb)));
		}).detach();
	}
	void FocusCapturedSurface( const blink::String &, bool ) override {}
	void CancelRequest( int32_t ) override {}
	void StopStreamDevice( const blink::String &, const std::optional<base::UnguessableToken> & ) override {}
	void OpenDevice( int32_t, const blink::String &, bm::MediaStreamType, OpenDeviceCallback cb ) override
	{
		std::move(cb).Run(false, blink::String(), blink::MediaStreamDevice());
	}
	void CloseDevice( const blink::String & ) override {}
	void SetCapturingLinkSecured( const std::optional<base::UnguessableToken> &, bm::MediaStreamType, bool ) override {}
	void ApplySubCaptureTarget( const base::UnguessableToken &, med::SubCaptureTargetType, const base::Token &, uint32_t,
				    ApplySubCaptureTargetCallback cb ) override
	{
		std::move(cb).Run(med::ApplySubCaptureTargetResult::kErrorGeneric);
	}
	void SendWheel( const base::UnguessableToken &, bm::CapturedWheelActionPtr ) override {}
	void UpdateZoomLevel( const base::UnguessableToken &, bm::ZoomLevelAction, UpdateZoomLevelCallback cb ) override
	{
		std::move(cb).Run(bm::CapturedSurfaceControlResult::kNoPermissionError);
	}
	void RequestCapturedSurfaceControlPermission( const base::UnguessableToken &,
						      RequestCapturedSurfaceControlPermissionCallback cb ) override
	{
		std::move(cb).Run(bm::CapturedSurfaceControlResult::kNoPermissionError);
	}
	void GetOpenDevice( int32_t, const base::UnguessableToken &, const base::UnguessableToken &, GetOpenDeviceCallback cb ) override
	{
		std::move(cb).Run(bm::MediaStreamRequestResult::INVALID_STATE, nullptr);
	}

	struct Want {
		bool		audio, video;
		int		cam;
		std::string	name;
	};

	/*
	 * getDisplayMedia: what the person chooses on the panel, asked each
	 * time as a browser shows its picker each time: the whole screen, a
	 * window, or a browser's window (what is a tab elsewhere). Its sound,
	 * when the page asked for it and the person turned it on, is a device
	 * of its own whose session the capture knows (br_share_audio_session).
	 */
	void Display( const blink::StreamControls &controls, GenerateStreamsCallback cb )
	{
		std::string origin = br_page_origin();
		BrShareAsk ask;
		ask.audio = controls.audio.stream_type == bm::MediaStreamType::DISPLAY_AUDIO_CAPTURE;
		ask.no_system_audio = controls.exclude_system_audio;
		ask.own_audio_out = controls.restrict_own_audio;
		ask.no_self = controls.exclude_self_browser_surface;
		ask.no_screen = controls.exclude_monitor_type_surfaces;
		ask.prefer = controls.preferred_display_surface == bm::PreferredDisplaySurface::MONITOR ? BR_SHARE_SCREEN
			   : controls.preferred_display_surface == bm::PreferredDisplaySurface::WINDOW ? BR_SHARE_WINDOW
			   : controls.preferred_display_surface == bm::PreferredDisplaySurface::BROWSER ? BR_SHARE_BROWSER : 0;
		std::thread([origin, ask, cb = std::move(cb)]() mutable {
			BrShareChoice ch;
			bool yes = br_share_choose(origin, ask, &ch);
			br_on_service_thread(base::BindOnce([]( bool yes, BrShareChoice ch, GenerateStreamsCallback cb ) {
				if ( !yes ) {
					printf("browser: getDisplayMedia: refused\n");
					std::move(cb).Run(bm::MediaStreamRequestResult::PERMISSION_DENIED, blink::String(""),
							  nullptr, false);
					return;
				}
				/* a screen's device: of its number, as a browser's id of a screen is of its display */
				bool whole = ch.kind == BR_SHARE_SCREEN;
				std::string id = whole ? "screen:" + std::to_string(ch.screen) + ":0"
						       : "window:" + std::to_string(ch.wid) + ":0";
				blink::MediaStreamDevice video(bm::MediaStreamType::DISPLAY_VIDEO_CAPTURE, id, ch.name);
				video.display_media_info = media::mojom::DisplayMediaInformation::New(
					whole ? media::mojom::DisplayCaptureSurfaceType::MONITOR
					      : ch.kind == BR_SHARE_BROWSER ? media::mojom::DisplayCaptureSurfaceType::BROWSER
									    : media::mojom::DisplayCaptureSurfaceType::WINDOW,
					true, whole ? media::mojom::CursorCaptureType::ALWAYS : media::mojom::CursorCaptureType::NEVER,
					nullptr, 100);
				base::UnguessableToken session = base::UnguessableToken::Create();
				video.set_session_id(session);
				br_screen_session(session, ch);
				std::optional<blink::MediaStreamDevice> audio;
				if ( ch.audio ) {
					media::AudioParameters p = MediaDevices::MicParams();
					audio = blink::MediaStreamDevice(bm::MediaStreamType::DISPLAY_AUDIO_CAPTURE, id, ch.name,
									 p.sample_rate(), p.channel_layout_config(), p.frames_per_buffer());
					base::UnguessableToken as = base::UnguessableToken::Create();
					audio->set_session_id(as);
					br_share_audio_session(as, ch.mon_mode, ch.mon_pid);
				}
				blink::Vector<bm::StreamDevicesPtr> devices;
				devices.push_back(bm::StreamDevices::New(audio, video));
				printf("browser: getDisplayMedia: kind %d (%s), %s\n", ch.kind, ch.name.c_str(),
				       ch.audio ? "with its sound" : "no sound");
				std::move(cb).Run(bm::MediaStreamRequestResult::OK,
						  blink::String::FromUtf8("display-" + base::UnguessableToken::Create().ToString()),
						  bm::StreamDevicesSet::New(std::move(devices)), false);
			}, yes, std::move(ch), std::move(cb)));
		}).detach();
	}

	void Answer( bool yes, const Want &w, GenerateStreamsCallback cb )
	{
		if ( !yes ) {
			printf("browser: getUserMedia: the %s was refused\n", w.video ? "camera" : "microphone");
			std::move(cb).Run(bm::MediaStreamRequestResult::PERMISSION_DENIED, blink::String(""), nullptr, false);
			return;
		}
		std::optional<blink::MediaStreamDevice> mic, cam;
		if ( w.audio ) {
			media::AudioParameters p = MediaDevices::MicParams();
			mic = blink::MediaStreamDevice(bm::MediaStreamType::DEVICE_AUDIO_CAPTURE, "default", "マイク",
						       p.sample_rate(), p.channel_layout_config(), p.frames_per_buffer());
			mic->set_session_id(base::UnguessableToken::Create());
			mic->group_id = "mic";
		}
		if ( w.video ) {
			std::string id = MediaDevices::CamId(w.cam);
			cam = blink::MediaStreamDevice(bm::MediaStreamType::DEVICE_VIDEO_CAPTURE, id, w.name,
						       media::VideoCaptureControlSupport(), media::MEDIA_VIDEO_FACING_NONE, id);
			base::UnguessableToken session = base::UnguessableToken::Create();
			cam->set_session_id(session);
			br_camera_session(session, w.cam);
		}
		blink::Vector<bm::StreamDevicesPtr> devices;
		devices.push_back(bm::StreamDevices::New(mic, cam));
		printf("browser: getUserMedia: %s\n", w.audio && w.video ? "the camera and the microphone"
						     : w.video ? "the camera" : "the microphone");
		std::move(cb).Run(bm::MediaStreamRequestResult::OK,
				  blink::String::FromUtf8("media-" + base::UnguessableToken::Create().ToString()),
				  bm::StreamDevicesSet::New(std::move(devices)), false);
	}
	void KeepDeviceAliveForTransfer( const base::UnguessableToken &, const base::UnguessableToken &,
					 KeepDeviceAliveForTransferCallback cb ) override
	{
		std::move(cb).Run(false);
	}

private:
	mojo::ReceiverSet<bm::MediaStreamDispatcherHost>	receivers_;
};

/* ---------------------------------------------------------------- posture, orientation */

class Posture final : public bm::DevicePostureProvider {
public:
	void Bind( mojo::PendingReceiver<bm::DevicePostureProvider> r ) { receivers_.Add(this, std::move(r)); }
	void AddListenerAndGetCurrentPosture( mojo::PendingRemote<bm::DevicePostureClient>,
					      AddListenerAndGetCurrentPostureCallback cb ) override
	{
		std::move(cb).Run(bm::DevicePostureType::kContinuous);
	}

private:
	mojo::ReceiverSet<bm::DevicePostureProvider>	receivers_;
};

/* a frame's: on the page's thread */
class Orientation final : public dm::ScreenOrientation {
public:
	void LockOrientation( dm::ScreenOrientationLockType, LockOrientationCallback cb ) override
	{
		std::move(cb).Run(dm::ScreenOrientationLockResult::SCREEN_ORIENTATION_LOCK_RESULT_ERROR_NOT_AVAILABLE);
	}
	void UnlockOrientation() override {}
};

/* ---------------------------------------------------------------- XR, the local fonts */

namespace vr = device::mojom::blink;

class Xr final : public vr::VRService {
public:
	void Bind( mojo::PendingReceiver<vr::VRService> r ) { receivers_.Add(this, std::move(r)); }

	void SetClient( mojo::PendingRemote<vr::VRServiceClient> ) override {}
	void RequestSession( vr::XRSessionOptionsPtr, RequestSessionCallback cb ) override
	{
		std::move(cb).Run(vr::RequestSessionResult::NewFailureReason(vr::RequestSessionError::NO_RUNTIME_FOUND));
	}
	void SupportsSession( vr::XRSessionOptionsPtr, SupportsSessionCallback cb ) override { std::move(cb).Run(false); }
	void ExitPresent( ExitPresentCallback cb ) override { std::move(cb).Run(); }
	void SetFramesThrottled( bool ) override {}
	void MakeXrCompatible( MakeXrCompatibleCallback cb ) override
	{
		std::move(cb).Run(vr::XrCompatibleResult::kNoDeviceAvailable);
	}

private:
	mojo::ReceiverSet<vr::VRService>	receivers_;
};

/* asked once a run (the permission's panel is the permission service's) */
class Fonts final : public bm::FontAccessManager {
public:
	void Bind( mojo::PendingReceiver<bm::FontAccessManager> r ) { receivers_.Add(this, std::move(r)); }

	void EnumerateLocalFonts( EnumerateLocalFontsCallback cb ) override
	{
		blink::FontEnumerationTable table;
		sk_sp<SkFontMgr> mgr = skia::DefaultFontMgr();
		for ( int i = 0; mgr && i < mgr->countFamilies(); i++ ) {
			sk_sp<SkFontStyleSet> set = mgr->createStyleSet(i);
			for ( int j = 0; set && j < set->count(); j++ ) {
				SkFontStyle style;
				SkString style_name, family, ps;
				set->getStyle(j, &style, &style_name);
				sk_sp<SkTypeface> face = set->createTypeface(j);
				if ( !face ) continue;
				face->getFamilyName(&family);
				if ( !face->getPostScriptName(&ps) ) ps = family;
				auto *f = table.add_fonts();
				f->set_postscript_name(ps.c_str());
				f->set_family(family.c_str());
				f->set_style(style_name.isEmpty() ? "Regular" : style_name.c_str());
				f->set_full_name(style_name.isEmpty() ? family.c_str()
								      : (std::string(family.c_str()) + " " + style_name.c_str()));
			}
		}
		std::string bytes = table.SerializeAsString();
		base::MappedReadOnlyRegion m = base::ReadOnlySharedMemoryRegion::Create(bytes.size() ? bytes.size() : 1);
		if ( !m.IsValid() ) {
			printf("browser: fonts: no shared memory for the table\n");
			std::move(cb).Run(bm::FontEnumerationStatus::kUnexpectedError, base::ReadOnlySharedMemoryRegion());
			return;
		}
		memcpy(m.mapping.memory(), bytes.data(), bytes.size());
		printf("browser: fonts: %d told\n", table.fonts_size());
		std::move(cb).Run(bm::FontEnumerationStatus::kOk, std::move(m.region));
	}

private:
	mojo::ReceiverSet<bm::FontAccessManager>	receivers_;
};

/* ---------------------------------------------------------------- the binders */

}  // namespace

/* The badge the page set, "" when none ("●" for a badge without a number) */
std::string br_badge_text()
{
	std::lock_guard<std::mutex> hold(g_badge_lock);
	return g_badge;
}

namespace {

template <typename Interface, typename Service>
void Binder()
{
	static Service *svc;
	blink::GetEmptyBrowserInterfaceBroker().SetBinderForTesting(
		Interface::Name_, base::BindRepeating([]( mojo::ScopedMessagePipeHandle h ) {
			br_on_service_thread(base::BindOnce([]( mojo::PendingReceiver<Interface> r ) {
				if ( svc == nullptr ) svc = new Service();
				svc->Bind(std::move(r));
			}, mojo::PendingReceiver<Interface>(std::move(h))));
		}));
}

}  // namespace

/* The binders of the frames' broker, once when the engine is up */
void br_devices_init()
{
	Binder<dm::BatteryMonitor, Battery>();
	Binder<bm::WakeLockService, WakeLocks>();
	Binder<dm::SensorProvider, Sensors>();
	Binder<bm::IdleManager, Idle>();
	Binder<bm::ShareService, Sharing>();
	Binder<bm::KeyboardLockService, Keyboard>();
	Binder<bm::BadgeService, Badge>();
	Binder<bm::InstalledAppProvider, RelatedApps>();
	Binder<bm::PresentationService, Presentation>();
	Binder<bm::WebBluetoothService, Bluetooth>();
	Binder<bm::MediaDevicesDispatcherHost, MediaDevices>();
	Binder<bm::MediaStreamDispatcherHost, MediaStreams>();
	Binder<bm::DevicePostureProvider, Posture>();
	Binder<vr::VRService, Xr>();
	Binder<bm::FontAccessManager, Fonts>();
	Binder<dm::VibrationManager, Vibration>();
	Binder<bm::WebPressureManager, Pressure>();
	Binder<bm::EyeDropperChooser, EyeDropper>();
	Binder<media::mojom::blink::WebrtcVideoPerfHistory, WebrtcPerf>();
	br_camera_init();
}

/* The frames' associated interfaces of the devices: the screen's orientation */
void br_devices_frame_interfaces( blink::AssociatedInterfaceProvider *provider )
{
	provider->OverrideBinderForTesting(
		dm::ScreenOrientation::Name_,
		base::BindRepeating([]( mojo::ScopedInterfaceEndpointHandle handle ) {
			mojo::MakeSelfOwnedAssociatedReceiver(
				std::make_unique<Orientation>(),
				mojo::PendingAssociatedReceiver<dm::ScreenOrientation>(std::move(handle)));
		}));
}
