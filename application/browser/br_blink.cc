/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_blink.cc
 *	ブラウザ: the page laid out, painted and run by Blink (design 17.19),
 *	behind the engine interface of br.h.
 *
 *	Blink is brought up without the layers a browser normally wraps it
 *	in (the content layer, other processes, a compositor): the process
 *	sets up //base (the exit manager, the command line, the features,
 *	the pool of worker threads) and Mojo within the process, gives Blink
 *	a Platform of its own (resources from the embedded blink_resources.pak,
 *	the font the application draws in, the user agent), the main thread
 *	scheduler, the names and bindings of core, and V8's isolate. A page
 *	is a Page with one LocalFrame and its view, made with the empty
 *	clients of core and a few overrides: the chrome client notes that a
 *	new frame is wanted and prints the console, the frame client hands
 *	navigations to the application (a form sent with POST with its body)
 *	and loads subresources through the network stack on its own thread
 *	(br_loader.cc, port/net).
 *
 *	A load commits a navigation with the fetched bytes as the response.
 *	The view is as wide as the window's page area and as high as it; the
 *	application's scroll position is the layout viewport's. A frame the
 *	page asks for begins with the steps a widget runs before the
 *	lifecycle (the animation frame callbacks among them), and a paint
 *	runs the document's lifecycle and plays its paint record into the
 *	application's canvas. What changed since the last paint is found as
 *	a compositor finds it for a layer, from the paint artifact (a
 *	RasterInvalidator over the view), so that only that is painted.
 */

#include TS_CONFIG_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include "port/base/counter_tessronos.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <atomic>
#include <memory>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "media/base/audio_capturer_source.h"
#include "media/audio/audio_source_parameters.h"
#include "components/viz/common/frame_sinks/begin_frame_args.h"
#include "components/viz/common/frame_timing_details.h"
#include "ui/gfx/presentation_feedback.h"
#include "base/functional/callback_helpers.h"
#include "base/timer/timer.h"
#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/feature_list.h"
#include "base/memory/discardable_memory.h"
#include "base/memory/discardable_memory_allocator.h"
#include "base/memory/ref_counted_memory.h"
#include "base/memory_coordinator/dummy_memory_consumer_registry.h"
#include "base/memory_coordinator/memory_consumer_registry.h"
#include "base/message_loop/message_pump.h"
#include "base/message_loop/message_pump_type.h"
#include "base/run_loop.h"
#include "base/time/default_tick_clock.h"
#include "base/functional/bind.h"
#include "base/task/single_thread_task_runner.h"
#include "base/pending_task.h"
#include "base/task/current_thread.h"
#include "base/task/task_observer.h"
#include "base/threading/thread.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "base/unguessable_token.h"
#include "components/webcrypto/webcrypto_impl.h"
#include "cc/paint/draw_image.h"
#include "cc/paint/paint_record.h"
#include "mojo/core/embedder/embedder.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "services/metrics/public/cpp/ukm_source_id.h"
#include "skia/ext/font_utils.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "third_party/blink/public/common/mime_util/mime_util.h"
#include "third_party/blink/public/common/storage_key/storage_key.h"
#include "third_party/blink/public/common/thread_safe_browser_interface_broker_proxy.h"
#include "third_party/blink/public/platform/web_dedicated_worker.h"
#include "third_party/blink/public/platform/web_dedicated_worker_host_factory_client.h"
#include "third_party/blink/public/platform/web_worker_fetch_context.h"
#include "third_party/blink/public/common/tokens/tokens.h"
#include "third_party/blink/public/common/user_agent/user_agent_metadata.h"
#include "third_party/icu/source/i18n/unicode/timezone.h"
#include "third_party/icu/source/common/unicode/locid.h"
#include "ui/base/pointer/pointer_device.h"
#include "third_party/blink/public/platform/linux/web_sandbox_support.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/public/platform/modules/video_capture/web_video_capture_impl_manager.h"
#include "third_party/blink/public/platform/scheduler/web_thread_scheduler.h"
#include "media/base/audio_renderer_sink.h"
#include "third_party/blink/public/platform/web_audio_device.h"
#include "third_party/blink/public/platform/web_audio_latency_hint.h"
#include "third_party/blink/public/platform/web_audio_sink_descriptor.h"
#include "third_party/blink/public/platform/web_crypto.h"
#include "third_party/blink/public/platform/web_crypto_algorithm.h"
#include "third_party/blink/public/platform/web_data.h"
#include "third_party/blink/public/platform/web_font_render_style.h"
#include "third_party/blink/public/platform/web_media_player.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/web/web_navigation_params.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_context_snapshot.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_initializer.h"
#include "third_party/blink/renderer/core/core_initializer.h"
#include "third_party/blink/renderer/core/event_type_names.h"
#include "third_party/blink/renderer/core/scheduler/task_attribution_tracker_impl.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_binding_for_core.h"
#include "third_party/blink/renderer/platform/bindings/script_state.h"
#include "third_party/blink/renderer/platform/bindings/v8_per_isolate_data.h"
#include "third_party/blink/renderer/platform/crypto.h"
#include "third_party/blink/renderer/platform/runtime_enabled_features.h"
#include "third_party/blink/renderer/modules/gamepad/navigator_gamepad.h"
#include "third_party/blink/renderer/core/origin_trials/origin_trial_context.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/dom/element.h"
#include "third_party/blink/renderer/core/frame/animation_frame_timing_monitor.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/local_frame_view.h"
#include "third_party/blink/renderer/core/frame/picture_in_picture_controller.h"
#include "third_party/blink/renderer/modules/document_picture_in_picture/picture_in_picture_controller_impl.h"
#include "third_party/blink/renderer/modules/media_controls/media_controls_impl.h"
#include "third_party/blink/renderer/bindings/core/v8/script_promise_resolver.h"
#include "third_party/blink/renderer/core/dom/dom_exception.h"
#include "third_party/blink/renderer/core/frame/settings.h"
#include "third_party/blink/renderer/core/frame/visual_viewport.h"
#include "third_party/blink/renderer/core/input/event_handler.h"
#include "third_party/blink/renderer/core/html/canvas/html_canvas_element.h"
#include "third_party/blink/renderer/core/html/html_frame_element.h"
#include "third_party/blink/public/mojom/blob/blob.mojom-blink.h"
#include "third_party/blink/public/mojom/frame/frame.mojom-blink.h"
#include "third_party/blink/renderer/core/html/forms/file_chooser.h"
#include "base/json/json_writer.h"
#include "base/json/json_reader.h"
#include "third_party/blink/public/platform/web_runtime_features.h"
#include "third_party/blink/public/platform/web_network_state_notifier.h"
#include "third_party/blink/renderer/core/css/css_paint_image_generator.h"
#include "third_party/blink/renderer/modules/csspaint/css_paint_image_generator_impl.h"
#include "third_party/blink/renderer/core/css/background_color_paint_image_generator.h"
#include "third_party/blink/renderer/core/css/clip_path_paint_image_generator.h"
#include "third_party/blink/renderer/modules/csspaint/nativepaint/background_color_paint_image_generator_impl.h"
#include "third_party/blink/renderer/modules/csspaint/nativepaint/clip_path_paint_image_generator_impl.h"
#include "third_party/blink/renderer/core/clipboard/dragged_isolated_file_system.h"
#include "third_party/blink/renderer/modules/filesystem/dragged_isolated_file_system_impl.h"
#include "third_party/blink/public/platform/web_v8_value_converter.h"
#include "third_party/blink/public/platform/modules/service_worker/web_service_worker_provider.h"
#include "third_party/blink/public/common/page/drag_operation.h"
#include "third_party/blink/renderer/core/clipboard/data_object.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_data_transfer_token.mojom-blink.h"
#include "third_party/blink/public/platform/web_drag_data.h"
#include "third_party/blink/renderer/core/page/drag_controller.h"
#include "third_party/blink/renderer/core/page/drag_data.h"
#include "third_party/blink/renderer/core/html/forms/html_opt_group_element.h"
#include "third_party/blink/renderer/core/html/forms/html_option_element.h"
#include "third_party/blink/renderer/core/html/forms/html_select_element.h"
#include "third_party/blink/renderer/core/html/forms/popup_menu.h"
#include "third_party/blink/renderer/core/html/html_anchor_element.h"
#include "third_party/blink/renderer/core/html/html_hr_element.h"
#include "third_party/blink/renderer/core/html/html_frame_owner_element.h"
#include "third_party/blink/renderer/core/html_names.h"
#include "base/strings/string_number_conversions.h"
#include "base/system/sys_info.h"
#include "net/base/data_url.h"
#include "net/base/request_priority.h"
#include "net/base/schemeful_site.h"
#include "services/network/public/cpp/client_hints.h"
#include "services/network/public/cpp/is_potentially_trustworthy.h"
#include "services/network/public/cpp/cross_origin_embedder_policy.h"
#include "services/network/public/cpp/cross_origin_embedder_policy_parser.h"
#include "services/network/public/cpp/cross_origin_opener_policy.h"
#include "services/network/public/cpp/cross_origin_opener_policy_parser.h"
#include "services/network/public/cpp/cross_origin_resource_policy.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "services/network/public/mojom/load_timing_info.mojom.h"
#include "third_party/blink/public/common/device_memory/approximated_device_memory.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "url/url_util.h"
#include "third_party/blink/public/common/scheme_registry.h"
#include "third_party/blink/public/mojom/blob/blob_url_store.mojom-blink.h"
#include "third_party/blink/public/mojom/frame/policy_container.mojom-blink.h"
#include "services/network/public/cpp/web_sandbox_flags.h"
#include "third_party/blink/public/platform/web_policy_container.h"
#include "third_party/blink/renderer/core/frame/policy_container.h"
#include "third_party/blink/renderer/modules/canvas/canvas2d/canvas_rendering_context_2d.h"
#include "third_party/blink/renderer/modules/canvas/imagebitmap/image_bitmap_rendering_context.h"
#include "third_party/blink/renderer/modules/canvas/offscreencanvas2d/offscreen_canvas_rendering_context_2d.h"
#include "third_party/blink/renderer/core/offscreencanvas/offscreen_canvas.h"
#include "third_party/blink/renderer/modules/webaudio/audio_graph_tracer.h"
#include "third_party/blink/renderer/modules/webgl/webgl_context_factory.h"
#include "third_party/blink/renderer/modules/webgpu/gpu_canvas_context.h"
#include "third_party/blink/renderer/platform/network/encoded_form_data.h"
#include "third_party/blink/public/platform/web_media_player.h"
#include "third_party/blink/renderer/core/html/media/html_media_element.h"
#include "third_party/blink/public/platform/web_graphics_context_3d_provider.h"
#include "third_party/blink/renderer/core/html/html_collection.h"
#include "third_party/blink/public/mojom/input/scroll_direction.mojom-blink.h"
#include "third_party/blink/public/common/input/web_keyboard_event.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "third_party/blink/public/common/input/web_mouse_wheel_event.h"
#include "third_party/blink/public/platform/web_text_input_type.h"
#include "third_party/blink/renderer/core/editing/frame_selection.h"
#include "third_party/blink/renderer/core/editing/ime/ime_text_span.h"
#include "third_party/blink/renderer/core/editing/ime/input_method_controller.h"
#include "third_party/blink/renderer/platform/graphics/color.h"
#include "ui/events/keycodes/dom/dom_key.h"
#include "ui/events/types/scroll_types.h"
#include "third_party/blink/renderer/core/layout/hit_test_location.h"
#include "third_party/blink/renderer/core/layout/hit_test_request.h"
#include "third_party/blink/renderer/core/layout/hit_test_result.h"
#include "third_party/blink/renderer/core/layout/layout_view.h"
#include "third_party/blink/renderer/core/loader/document_loader.h"
#include "third_party/blink/renderer/core/loader/empty_clients.h"
#include "third_party/blink/public/platform/resource_load_info_notifier_wrapper.h"
#include "third_party/blink/public/platform/weak_wrapper_resource_load_info_notifier.h"
#include "third_party/blink/renderer/core/loader/frame_load_request.h"
#include "third_party/blink/renderer/core/loader/frame_loader.h"
#include "third_party/blink/renderer/core/loader/history_item.h"
#include "third_party/blink/public/common/page_state/page_state.h"
#include "third_party/blink/public/platform/web_content_security_policy_struct.h"
#include "third_party/blink/renderer/core/timing/dom_window_performance.h"
#include "third_party/blink/renderer/core/timing/window_performance.h"
#include "third_party/blink/renderer/core/timing/performance_timing.h"
#include "third_party/blink/renderer/core/timing/performance_timing_for_reporting.h"
#include "net/base/mime_sniffer.h"
#include "net/http/http_connection_info.h"
#include "net/http/http_content_disposition.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_util.h"
#include "third_party/blink/public/mojom/commit_result/commit_result.mojom-shared.h"
#include "third_party/blink/renderer/core/loader/navigation_policy.h"
#include "third_party/blink/renderer/core/fullscreen/fullscreen.h"
#include "third_party/blink/renderer/core/fullscreen/fullscreen_request_type.h"
#include "third_party/blink/renderer/core/editing/editor.h"
#include "third_party/blink/renderer/core/html/forms/html_form_element.h"
#include "third_party/blink/renderer/core/html/forms/html_input_element.h"
#include "third_party/blink/renderer/core/dom/element_traversal.h"
#include "third_party/blink/public/mojom/forms/form_control_type.mojom-shared.h"
#include "third_party/blink/renderer/core/editing/finder/find_buffer.h"
#include "third_party/blink/renderer/core/editing/finder/find_options.h"
#include "third_party/blink/renderer/core/editing/markers/document_marker_controller.h"
#include "third_party/blink/renderer/core/editing/visible_units.h"
#include "third_party/blink/renderer/core/editing/ephemeral_range.h"
#include "third_party/blink/renderer/core/frame/local_frame_view.h"
#include "third_party/blink/public/web/web_history_item.h"
#include "third_party/blink/public/web/web_frame_load_type.h"
#include "third_party/blink/public/web/web_history_commit_type.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"
#include "third_party/blink/renderer/core/page/focus_controller.h"
#include "third_party/blink/renderer/core/page/page.h"
#include "third_party/blink/renderer/core/page/page_animator.h"
#include "third_party/blink/renderer/core/paint/paint_layer_scrollable_area.h"
#include "third_party/blink/renderer/platform/fonts/generic_font_family_settings.h"
#include "third_party/blink/renderer/platform/graphics/paint/paint_artifact.h"
#include "third_party/blink/renderer/platform/graphics/paint/paint_chunk_subset.h"
#include "third_party/blink/renderer/platform/graphics/paint/property_tree_state.h"
#include "third_party/blink/renderer/platform/graphics/paint/raster_invalidator.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/heap/persistent.h"
#include "third_party/blink/renderer/platform/scheduler/public/agent_group_scheduler.h"
#include "third_party/blink/renderer/platform/scheduler/public/main_thread.h"
#include "third_party/blink/renderer/platform/scheduler/public/main_thread_scheduler.h"
#include "third_party/blink/renderer/platform/scheduler/public/thread.h"
#include "third_party/blink/renderer/platform/scheduler/main_thread/main_thread_scheduler_impl.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/wtf/allocator/partitions.h"
#include "third_party/blink/renderer/platform/instrumentation/instance_counters.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkData.h"
#include "third_party/skia/include/core/SkFontMgr.h"
#include "third_party/skia/include/core/SkFontStyle.h"
#include "third_party/skia/include/core/SkStream.h"
#include "third_party/skia/include/core/SkTypeface.h"
#include "third_party/skia/include/ports/SkFontMgr_data.h"
#include "v8/include/cppgc/heap-statistics.h"
#include "v8/include/v8-cppgc.h"
#include "v8/include/v8.h"

#include <malloc.h>

#include "br.h"
#include "br_proc.h"
#include "port/gpu/br_gpu.h"
#include "port/net/br_net.h"
#include "port/net/br_data.h"
#include "port/net/br_objfile.h"

/* The subresource loader (br_loader.cc) and the bindings (br_bindings.cc) */
std::unique_ptr<blink::URLLoader> br_blink_url_loader();
std::unique_ptr<blink::URLLoader> br_blink_frame_url_loader( const std::string &sw_client,
							    network::mojom::CrossOriginEmbedderPolicyValue coep );
void br_blink_bindings_init();
/* A PDF shown as an HTML page of its pages, "" when it is not read (port/blink/pdf_tessronos.cc) */
std::string br_pdf_page( const std::string &url, const std::string &bytes );

/* localStorage read back from its object (port/blink/storage_tessronos.cc) */
void br_storage_restore();

/* IndexedDB's store read back and bound (port/blink/indexeddb_tessronos.cc) */
void br_idb_init();

/* Blobs and their blob: URLs (port/blink/blob_tessronos.cc) */
blink::ThreadSafeBrowserInterfaceBrokerProxy *br_platform_broker();
void br_blob_frame_interfaces( blink::AssociatedInterfaceProvider *provider );

/* Permissions, notifications, the quota, Web Locks, BroadcastChannel (port/blink/services_tessronos.cc) */
void br_services_init();
void br_services_frame_interfaces( blink::AssociatedInterfaceProvider *provider );
blink::mojom::blink::FileSystemManager &br_filesystem_manager();
mojo::PendingRemote<blink::mojom::blink::FileSystemAccessDataTransferToken> br_fsa_drop_token( const std::string &path,
												const std::string &name );
void br_devices_frame_interfaces( blink::AssociatedInterfaceProvider *provider );

/*
 * The files a page is given (port/blink/files_tessronos.cc) and the bytes
 * of a Blob or a blob: URL (port/blink/blob_tessronos.cc)
 */
std::string br_file_keep( const std::string &uuid, const std::string &name, std::vector<uint8_t> bytes );
void br_blob_read( const std::string &key, bool is_url,
		   base::OnceCallback<void(bool, std::string, std::string)> done );
void br_on_service_thread( base::OnceClosure task );

/* Service workers, each document a client of them by its UUID (port/blink/service_worker_tessronos.cc) */
std::unique_ptr<blink::WebServiceWorkerProvider> br_sw_provider( const std::string &client );
std::string br_sw_document( const std::string &url, bool top, const std::string &parent, const std::string &uuid );
void br_sw_forget( const std::string &uuid );
bool br_sw_navigate_frame( const std::string &url, const std::string &method, const std::string &referrer,
			   const std::string &uuid, bool frame_element,
			   base::OnceCallback<void(bool, int, std::string, std::string, std::string, std::string)> done );

/* 印刷 of a frame's page (port/blink/print_tessronos.cc) */
bool br_print_frame( blink::LocalFrame *frame, const std::string &fallback );

/* Told the engine's thread's turns (br_main.cc; see "where the engine's thread's time goes" below) */
extern void (*br_turn_ended)( const char *doing, long ms );
/* A step of the start ended: its time, for the line of the start (br_main.cc) */
void br_start_mark( const char *what );
/* The WebGL calls' time of the calling thread, in the counter's ticks (port/webgl/gl_angle.cc, make GL=1) */
uint64_t br_webgl_ticks() __attribute__((weak));
/* The calling thread's time in writing on the console, in the counter's ticks (br_main.cc) */
uint64_t br_console_ticks() __attribute__((weak));

namespace blink {
/* What only an isolated web app has, given by the embedder (the patch isolated_context.patch) */
extern bool ( *g_isolated_context_hook )( const LocalDOMWindow & );
/* What a page downloads, given to the embedder (the patch local_frame.patch) */
extern void (*g_download_url_hook)( LocalFrame &, mojom::blink::DownloadURLParamsPtr );
/*
 * The monitor of the animation frames of a frame's page, for a frame that
 * has no widget (the patches local_frame.patch and paint_timing.patch;
 * weak, for an engine translated before them)
 */
extern AnimationFrameTimingMonitor *( *g_frame_timing_monitor_hook )( LocalFrame & ) __attribute__((weak));
/* A window's store of blob: URLs given by the embedder (the patch public_url_manager.patch; weak, as above) */
extern void ( *g_window_blob_url_store_hook )( mojo::PendingReceiver<mojom::blink::BlobURLStore> ) __attribute__((weak));
/* A worker's animation frames, given by the embedder (the patch begin_frame_provider.patch; weak, as above) */
extern void ( *g_worker_begin_frame_hook )( scoped_refptr<base::SingleThreadTaskRunner>,
					    base::OnceCallback<void( const viz::BeginFrameArgs & )> ) __attribute__((weak));
}

/* A redirect a navigation's request followed: the response that redirected, and where to */
struct BrNavRedirect {
	std::string	url;		/* the address that answered with the redirect */
	int		status = 0;
	std::string	headers;
	std::string	new_url, new_method, new_referrer;
};

/*
 * A dedicated worker's animation frame (its requestAnimationFrame, an
 * OffscreenCanvas drawn there): begun on the worker's thread at the next
 * boundary of a 60 Hz display's frames, which the page's frames are paced by
 */
void worker_begin_frame( scoped_refptr<base::SingleThreadTaskRunner> runner,
			 base::OnceCallback<void( const viz::BeginFrameArgs & )> frame )
{
	static std::atomic<uint64_t> seq{ viz::BeginFrameArgs::kStartingFrameNumber };
	const base::TimeDelta interval = base::Hertz(60);
	base::TimeTicks now = base::TimeTicks::Now();
	base::TimeTicks at = now.SnappedToNextTick(base::TimeTicks(), interval);
	runner->PostDelayedTask(FROM_HERE, base::BindOnce([]( base::OnceCallback<void( const viz::BeginFrameArgs & )> frame,
							       base::TimeDelta interval, uint64_t n ) {
		base::TimeTicks t = base::TimeTicks::Now();
		/* the frames' numbers a source of their own (2: the pages' are 1) */
		std::move(frame).Run(viz::BeginFrameArgs::Create(BEGINFRAME_FROM_HERE, 2, n, t, t + interval, interval,
								 viz::BeginFrameArgs::NORMAL));
	}, std::move(frame), interval, seq++), at - now);
}

/* The store of blob: URLs on its own thread (port/blink/blob_tessronos.cc) */
void br_blob_url_store( mojo::PendingReceiver<blink::mojom::blink::BlobURLStore> r );
/* The protocol an answer of the network stack came over, inside its callback (port/net/net_fetch_tessronos.cc) */
extern "C" const char *br_net_fetch_answer_protocol( void );
/* And the redirects it followed: how many (-1 outside the callback), and each one */
extern "C" int br_net_fetch_redirects( void );
/* The memory objects of memfd_create and the bytes reserved for them (lib/libcxxrt/ts_mman.c) */
extern "C" size_t ts_memfd_bytes( int *p_n );
extern "C" int br_net_fetch_redirect( int i, const char **url, int *status, const char **headers,
				      const char **new_url, const char **new_method, const char **new_referrer );

/* Dedicated workers (port/blink/worker_tessronos.cc) */
std::unique_ptr<blink::WebDedicatedWorkerHostFactoryClient> br_worker_host_factory_client(
	blink::WebDedicatedWorker *worker );
scoped_refptr<blink::WebWorkerFetchContext> br_worker_fetch_context();

/* The spelling of what is typed into a page's fields (port/spell/spell_tessronos.cc) */
blink::WebTextCheckClient *br_spell_client();

/* The players of the media elements (port/media/media_player.cc) */
std::unique_ptr<blink::WebMediaPlayer> br_media_player( blink::HTMLMediaElement &element,
							blink::WebMediaPlayerClient *client );
void br_media_tick();
bool br_media_wants_ticks();
constexpr int kMediaTickMs = 10;	/* how often a player that plays is looked at (br_media_tick) */
/* The microphone's capture (port/media/br_audio_sink.cc) */
scoped_refptr<media::AudioCapturerSource> br_audio_capturer( const base::UnguessableToken &session );

#include "port/ext/br_ext.h"
/* The extensions in the frames (port/ext/br_ext_frames.cc) */
void br_ext_world_context( v8::Local<v8::Context> context, int world );
void br_ext_page_context( v8::Local<v8::Context> context );
void br_ext_moment( blink::LocalFrame *frame, int when );
void br_ext_start();

/* The output of an AudioContext (port/media/web_audio_tessronos.cc) */
std::unique_ptr<blink::WebAudioDevice> BrCreateAudioDevice( const blink::WebAudioSinkDescriptor &sink,
							   unsigned channels,
							   media::AudioRendererSink::RenderCallback *callback );

/*
 * WebGL on ANGLE over the system's OpenGL ES (port/webgl/gl_angle.cc,
 * linked with make GL=1, so weak), and WebGL 1's GLES2 on the CPU
 * (port/webgl/gl_tessronos.cc) when ANGLE is not there or did not start
 */
std::unique_ptr<blink::WebGraphicsContext3DProvider> BrCreateAngleGLProvider(
	bool es3, blink::Platform::WebGLContextInfo *info ) __attribute__((weak));
std::unique_ptr<blink::WebGraphicsContext3DProvider> BrCreateGLProvider(
	blink::Platform::WebGLContextInfo *info );
/*
 * WebGL in the GPU process, on the renderer's channel to it
 * (port/webgl/gl_cmdbuf.cc, make GL=1, so weak): null when there is no
 * channel (gpuproc=off, process=single, gpu=off, no GPU process), and
 * 'later' true while a lost one is replaced
 */
std::unique_ptr<blink::WebGraphicsContext3DProvider> BrCreateCmdBufGLProvider(
	bool es3, bool prefer_low_power_gpu, bool fail_if_major_performance_caveat, const blink::WebURL &url,
	blink::Platform::WebGLContextInfo *info, bool *later ) __attribute__((weak));

static std::unique_ptr<blink::WebGraphicsContext3DProvider> BrWebGLProvider(
	bool es3, bool low_power, bool fail_caveat, const blink::WebURL &url, blink::Platform::WebGLContextInfo *info )
{
	if ( BrCreateCmdBufGLProvider != nullptr ) {
		bool later = false;
		std::unique_ptr<blink::WebGraphicsContext3DProvider> p =
			BrCreateCmdBufGLProvider(es3, low_power, fail_caveat, url, info, &later);
		/* while the GPU process is started again, none: Blink tries again to restore a lost context */
		if ( p || later ) return p;
	}
	if ( BrCreateAngleGLProvider != nullptr ) {
		std::unique_ptr<blink::WebGraphicsContext3DProvider> p = BrCreateAngleGLProvider(es3, info);
		if ( p ) return p;
	}
	if ( es3 ) {
		printf("browser: webgl: no WebGL 2 (it needs ANGLE: make GL=1)\n");
		return nullptr;
	}
	return BrCreateGLProvider(info);
}

/*
 * WebGPU on Dawn over the system's OpenGL ES (port/webgpu/webgpu_tessronos.cc,
 * linked with make GL=1, so weak); without it there is no adapter
 */
std::unique_ptr<blink::WebGraphicsContext3DProvider> BrCreateWebGPUProvider() __attribute__((weak));
std::unique_ptr<blink::WebGraphicsContext3DProvider> BrCreateWebGPUProviderToWaitOn() __attribute__((weak));
/*
 * WebGPU in the GPU process (port/webgpu/webgpu_cmdbuf.cc, make GL=1, so
 * weak): null when there is no channel ('later' while a lost one is
 * replaced) or the GPU process runs no Dawn ('none')
 */
std::unique_ptr<blink::WebGraphicsContext3DProvider> BrCreateCmdBufWebGPUProvider(
	const blink::WebURL &url, bool wait_on, bool *later, bool *none ) __attribute__((weak));

/*
 * A WebGPU context: in the GPU process when the renderer has a channel to
 * it, on Dawn in the renderer when there is none, or none at all (the
 * page's requestAdapter answers null)
 */
static std::unique_ptr<blink::WebGraphicsContext3DProvider> BrWebGPUProvider( const blink::WebURL &url, bool wait_on )
{
	if ( BrCreateCmdBufWebGPUProvider != nullptr ) {
		bool later = false, none = false;
		std::unique_ptr<blink::WebGraphicsContext3DProvider> p =
			BrCreateCmdBufWebGPUProvider(url, wait_on, &later, &none);
		/* while the GPU process is started again, or where it runs no Dawn, none */
		if ( p || later || none ) return p;
	}
	if ( wait_on ) return BrCreateWebGPUProviderToWaitOn != nullptr ? BrCreateWebGPUProviderToWaitOn() : nullptr;
	if ( BrCreateWebGPUProvider != nullptr ) return BrCreateWebGPUProvider();
	static bool told;
	if ( !told ) {
		printf("browser: webgpu: no WebGPU (it needs Dawn on OpenGL ES: make GL=1)\n");
		told = true;
	}
	return nullptr;
}

/* Brotli's decoder (third_party/brotli), for the compressed resources */
extern "C" int BrotliDecoderDecompress( size_t encoded_size, const uint8_t *encoded,
					size_t *decoded_size, uint8_t *decoded );
#define BROTLI_DECODER_RESULT_SUCCESS	1

/* The resources of Blink (style sheets, scripts of its controls), linked in */
extern "C" const unsigned char br_blink_pak[];
extern "C" const unsigned char br_blink_pak_end[];

/* Told of the paint artifact after each paint of a page (patch paint_hook.patch) */
namespace blink {
extern void (*g_non_composited_paint_hook)( LocalFrameView & );
}

/* What the browser says it is, as client hints (below) */
blink::UserAgentMetadata br_user_agent_metadata();
/* A face of the 書体箱 by its name (br_main.cc) */
sk_sp<SkData> br_face_data( const char *name );
/* The client hints origins asked for (below) */
void br_hints_learn( const std::string &url, const std::string &headers );
std::vector<network::mojom::WebClientHintsType> br_hints_for( const std::string &url );

/*
 * The system's time zone (ユーザ環境設定), ICU's default: the dates and
 * Intl of the scripts are in it. Looked at again every few seconds; when
 * it has changed, V8 is told, as a browser tells it when the machine's
 * zone changes, and the dates made from then on are in the new zone.
 */
static std::string g_zone;

static void follow_zone()
{
	std::string z = br_zone_name();
	if ( z == g_zone ) return;
	icu::TimeZone *tz = icu::TimeZone::createTimeZone(icu::UnicodeString::fromUTF8(z));
	if ( tz == nullptr || *tz == icu::TimeZone::getUnknown() ) {
		delete tz;
		return;
	}
	bool first = g_zone.empty();
	g_zone = z;
	/*
	 * the C library's local time too, at the zone's offset now: the local
	 * dates base makes (document.lastModified) are then those the
	 * scripts' Date reads
	 */
	{
		int32_t raw = 0, dst = 0;
		UErrorCode st = U_ZERO_ERROR;
		tz->getOffset((UDate)base::Time::Now().InMillisecondsFSinceUnixEpoch(), false, raw, dst, st);
		if ( U_SUCCESS(st) ) {
			int east = ( raw + dst ) / 60000, a = east < 0 ? -east : east;
			char tzs[32];
			snprintf(tzs, sizeof(tzs), "LCL%c%d:%02d", east > 0 ? '-' : '+', a / 60, a % 60);
			setenv("TZ", tzs, 1);
			tzset();
		}
	}
	icu::TimeZone::adoptDefault(tz);
	printf("browser: time zone %s\n", z.c_str());
	v8::Isolate *iso = v8::Isolate::GetCurrent();
	if ( !first && iso != nullptr ) {
		iso->DateTimeConfigurationChangeNotification(v8::Isolate::TimeZoneDetection::kSkip);
	}
}

static void follow_zone_later()
{
	base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
		FROM_HERE, base::BindOnce([] {
			follow_zone();
			follow_zone_later();
		}),
		base::Seconds(3));
}

namespace {

const char kFamily[] = "Noto Sans JP";
const char kSerifFamily[] = "Noto Serif JP";
const char kMonoFamily[] = "Noto Sans Mono CJK JP";
bool g_have_serif;		/* the serif face was found (BrFontMgr) */
bool g_have_mono;		/* the monospaced face was found (BrFontMgr) */

/* ---------------------------------------------------------------- resources */

/*
 * blink_resources.pak (version 5): a header, a table of (id, offset)
 * sorted by id with one entry more than there are resources, and a table
 * of aliases (id, index into the first). A resource compressed with
 * Brotli starts with 0x1e 0x9b and the size of its text in six bytes.
 */
class Pak {
public:
	bool find( int id, const uint8_t **p, size_t *n ) const
	{
		const uint8_t	*d = br_blink_pak;
		size_t		size = (size_t)( br_blink_pak_end - br_blink_pak );

		if ( size < 12 || rd32(d) != 5 ) return false;
		unsigned cnt = rd16(d + 8), aliases = rd16(d + 10);
		const uint8_t *ent = d + 12, *al = ent + 6 * ( cnt + 1 );
		int index = lookup(ent, 6, cnt, id);
		if ( index < 0 ) {
			int a = lookup(al, 4, aliases, id);
			if ( a < 0 ) return false;
			index = rd16(al + 4 * a + 2);
		}
		uint32_t off = rd32(ent + 6 * index + 2), end = rd32(ent + 6 * ( index + 1 ) + 2);
		if ( end < off || end > size ) return false;
		*p = d + off;
		*n = end - off;
		return true;
	}

	/* The text of a resource, decompressed */
	std::string text( int id ) const
	{
		const uint8_t	*p;
		size_t		n;

		if ( !find(id, &p, &n) ) return std::string();
		if ( n >= 8 && p[0] == 0x1e && p[1] == 0x9b ) {
			size_t want = 0;
			for ( int i = 0; i < 6; i++ ) want |= (size_t)p[2 + i] << ( 8 * i );
			std::string out(want, '\0');
			size_t got = want;
			if ( BrotliDecoderDecompress(n - 8, p + 8, &got, (uint8_t *)&out[0])
			     != BROTLI_DECODER_RESULT_SUCCESS ) {
				printf("browser: resource %d does not decompress\n", id);
				return std::string();
			}
			out.resize(got);
			return out;
		}
		return std::string((const char *)p, n);
	}

private:
	static unsigned rd16( const uint8_t *p ) { return p[0] | ( p[1] << 8 ); }
	static uint32_t rd32( const uint8_t *p )
	{
		return p[0] | ( p[1] << 8 ) | ( p[2] << 16 ) | ( (uint32_t)p[3] << 24 );
	}
	static int lookup( const uint8_t *t, int stride, unsigned cnt, int id )
	{
		int lo = 0, hi = (int)cnt - 1;
		while ( lo <= hi ) {
			int mid = ( lo + hi ) / 2, v = (int)rd16(t + stride * mid);
			if ( v == id ) return mid;
			if ( v < id ) lo = mid + 1; else hi = mid - 1;
		}
		return -1;
	}
};

Pak	g_pak;

/* ---------------------------------------------------------------- fonts */

/*
 * The faces the system has for pages: the sans-serif one, which every
 * family a page names and every generic family but serif and monospace
 * is, and of the 書体箱 the serif one (NotoSerifJP-Regular), which the
 * generic serif family is, and the monospaced one
 * (NotoSansMonoCJKjp-Regular), which the generic monospace family is, as
 * a Linux desktop with the Noto CJK faces has them; synthetic bold and
 * italic make the other styles. Without the serif or the monospaced face
 * that family is the sans-serif face too.
 */
class BrFontMgr final : public SkFontMgr {
public:
	explicit BrFontMgr( sk_sp<SkData> data )
		: inner_(SkFontMgr_New_Custom_Data(SkSpan<sk_sp<SkData>>(&data, 1))),
		  face_(inner_->makeFromData(data))
	{
		static const char *const object[kExtras] = { "NotoSerifJP-Regular", "NotoSansMonoCJKjp-Regular" };
		static const char *const family[kExtras] = { kSerifFamily, kMonoFamily };

		for ( int i = 0; i < kExtras; i++ ) {
			Extra &e = extra_[i];
			e.family = family[i];
			sk_sp<SkData> d = br_face_data(object[i]);
			if ( d == nullptr ) continue;
			e.mgr = SkFontMgr_New_Custom_Data(SkSpan<sk_sp<SkData>>(&d, 1));
			e.face = e.mgr->makeFromData(d);
			if ( e.face == nullptr ) e.mgr = nullptr;
		}
		g_have_serif = extra_[0].face != nullptr;
		g_have_mono = extra_[1].face != nullptr;
		printf("browser: fonts: %s, serif %s, monospace %s\n", kFamily, g_have_serif ? kSerifFamily : kFamily,
		       g_have_mono ? kMonoFamily : kFamily);
	}

protected:
	int onCountFamilies() const override
	{
		int n = 1;
		for ( const Extra &e : extra_ ) n += e.face != nullptr;
		return n;
	}
	void onGetFamilyName( int i, SkString *name ) const override
	{
		const Extra *e = nth(i);
		name->set(e != nullptr ? e->family : kFamily);
	}
	sk_sp<SkFontStyleSet> onCreateStyleSet( int i ) const override
	{
		const Extra *e = nth(i);
		return e != nullptr ? e->mgr->createStyleSet(0) : inner_->createStyleSet(0);
	}
	sk_sp<SkFontStyleSet> onMatchFamily( const char *family ) const override
	{
		const Extra *e = named(family);
		return e != nullptr ? e->mgr->createStyleSet(0) : inner_->createStyleSet(0);
	}
	sk_sp<SkTypeface> onMatchFamilyStyle( const char *family, const SkFontStyle & ) const override
	{
		/*
		 * The icon font the scrollbar's arrows are drawn from is not there:
		 * with no face the theme draws them as triangles, where the one
		 * face would give its private-use code points as missing glyphs.
		 */
		if ( family != nullptr && strcmp(family, "Segoe Fluent Icons") == 0 ) return nullptr;
		const Extra *e = named(family);
		return e != nullptr ? e->face : face_;
	}
	sk_sp<SkTypeface> onMatchFamilyStyleCharacter( const char *, const SkFontStyle &,
						       const char *[], int, SkUnichar c ) const override
	{
		return ( face_ != nullptr && face_->unicharToGlyph(c) != 0 ) ? face_ : nullptr;
	}
	sk_sp<SkTypeface> onMakeFromData( sk_sp<SkData> d, int ttc ) const override
	{
		return inner_->makeFromData(std::move(d), ttc);
	}
	sk_sp<SkTypeface> onMakeFromStreamIndex( std::unique_ptr<SkStreamAsset> s, int ttc ) const override
	{
		return inner_->makeFromStream(std::move(s), ttc);
	}
	sk_sp<SkTypeface> onMakeFromStreamArgs( std::unique_ptr<SkStreamAsset> s,
						const SkFontArguments &a ) const override
	{
		return inner_->makeFromStream(std::move(s), a);
	}
	sk_sp<SkTypeface> onMakeFromFile( const char *path, int ttc ) const override
	{
		return inner_->makeFromFile(path, ttc);
	}
	sk_sp<SkTypeface> onLegacyMakeTypeface( const char *, SkFontStyle ) const override
	{
		return face_;
	}

private:
	static constexpr int kExtras = 2;
	struct Extra {
		const char		*family = nullptr;
		sk_sp<SkFontMgr>	mgr;
		sk_sp<SkTypeface>	face;
	};

	/* The face of a family by its own name (the generic family is set to it), when the system has it */
	const Extra *named( const char *family ) const
	{
		if ( family == nullptr ) return nullptr;
		for ( const Extra &e : extra_ ) {
			if ( e.face != nullptr && strcasecmp(family, e.family) == 0 ) return &e;
		}
		return nullptr;
	}

	/* The family at an index of the list: 0 is the sans-serif face, then those the system has */
	const Extra *nth( int i ) const
	{
		for ( const Extra &e : extra_ ) {
			if ( e.face != nullptr && --i == 0 ) return &e;
		}
		return nullptr;
	}

	sk_sp<SkFontMgr>	inner_;
	sk_sp<SkTypeface>	face_;
	Extra			extra_[kExtras];
};

/* How glyphs are drawn: grey antialiasing and slight hinting */
class BrSandbox final : public blink::WebSandboxSupport {
public:
	bool GetFallbackFontForCharacter( blink::WebUChar32, const char *,
					  gfx::FallbackFontData * ) override
	{
		return false;
	}
	bool MatchFontByPostscriptNameOrFullFontName( const char *, gfx::FallbackFontData * ) override
	{
		return false;
	}
	void GetWebFontRenderStyleForStrike( const char *, int, bool, bool, float,
					     blink::WebFontRenderStyle *out ) override
	{
		out->use_bitmaps = 0;
		out->use_auto_hint = 0;
		out->use_hinting = 1;
		out->hint_style = 1;
		out->use_anti_alias = 1;
		out->use_subpixel_rendering = 0;
		out->use_subpixel_positioning = 0;
	}
};

/* ---------------------------------------------------------------- discardable memory */

/*
 * The decoded images the engine keeps (cc's decode cache, the frames of
 * an image) are in discardable memory, which a browser shares with its
 * other processes and takes back under pressure. Here it is memory of
 * the heap that is never taken back while it is held: locking it always
 * succeeds, and it goes when its owner lets it go.
 */
class BrDiscardableMemory final : public base::DiscardableMemory {
public:
	BrDiscardableMemory( size_t size, size_t *total )
		: data_(new uint8_t[size != 0 ? size : 1]), size_(size), total_(total)
	{
		*total_ += size_;
	}
	~BrDiscardableMemory() override
	{
		*total_ -= size_;
		delete[] data_;
	}
	bool Lock() override { return true; }
	void Unlock() override {}
	void *data() const override { return data_; }
	void DiscardForTesting() override {}
	base::trace_event::MemoryAllocatorDump *CreateMemoryAllocatorDump(
		const char *, base::trace_event::ProcessMemoryDump * ) const override
	{
		return nullptr;
	}

private:
	uint8_t	*data_;
	size_t	size_;
	size_t	*total_;
};

class BrDiscardableAllocator final : public base::DiscardableMemoryAllocator {
public:
	std::unique_ptr<base::DiscardableMemory> AllocateLockedDiscardableMemory( size_t size ) override
	{
		return std::make_unique<BrDiscardableMemory>(size, &total_);
	}
	size_t GetBytesAllocated() const override { return total_; }
	void ReleaseFreeMemory() override {}

private:
	size_t	total_ = 0;
};

/* ---------------------------------------------------------------- the platform */

/* window.chrome, as Chrome gives it to each page (loadTimes, csi, app) */
void install_chrome( v8::Isolate *iso, v8::Local<v8::Context> context );

/*
 * Values between Chromium's base::Value and a page's script (a FedCM
 * token, what a browser hands a page as a value): through JSON, which
 * both read and write
 */
class BrValueConverter final : public blink::WebV8ValueConverter {
public:
	void SetDateAllowed( bool ) override {}
	void SetRegExpAllowed( bool ) override {}

	v8::Local<v8::Value> ToV8Value( base::ValueView value, v8::Local<v8::Context> context ) override
	{
		v8::Isolate *iso = v8::Isolate::GetCurrent();
		std::optional<std::string> json = base::WriteJson(value);
		v8::Local<v8::String> text;
		v8::Local<v8::Value> out;
		if ( !json || !v8::String::NewFromUtf8(iso, json->data(), v8::NewStringType::kNormal,
						       (int)json->size()).ToLocal(&text)
		     || !v8::JSON::Parse(context, text).ToLocal(&out) ) {
			return v8::Null(iso);
		}
		return out;
	}

	std::unique_ptr<base::Value> FromV8Value( v8::Local<v8::Value> value, v8::Local<v8::Context> context ) override
	{
		v8::Local<v8::String> text;
		if ( !v8::JSON::Stringify(context, value).ToLocal(&text) ) return nullptr;
		v8::String::Utf8Value utf8(v8::Isolate::GetCurrent(), text);
		if ( *utf8 == nullptr ) return nullptr;
		std::optional<base::Value> v = base::JSONReader::Read(std::string_view(*utf8, utf8.length()),
								      base::JSON_PARSE_RFC);
		return v ? std::make_unique<base::Value>(std::move(*v)) : nullptr;
	}
};

/*
 * The Content-Security-Policy and Content-Security-Policy-Report-Only
 * lines of the responses documents are committed from, by the document's
 * address, until its CSP is made (BrPlatform::AppendContentSecurityPolicy):
 * the document is under the policies its response declared, as Chrome's
 * browser parses them into the navigation it commits
 */
struct BrCspLine {
	std::string	value;
	bool		report_only = false;
};
std::mutex					g_csp_lock;
std::vector<std::pair<std::string, std::vector<BrCspLine>>>	g_csp;

void csp_keep( const std::string &url, std::vector<BrCspLine> lines )
{
	std::lock_guard<std::mutex> hold(g_csp_lock);
	g_csp.erase(std::remove_if(g_csp.begin(), g_csp.end(),
				   [&]( const std::pair<std::string, std::vector<BrCspLine>> &e ) { return e.first == url; }),
		    g_csp.end());
	if ( lines.empty() ) return;
	g_csp.emplace_back(url, std::move(lines));
	while ( g_csp.size() > 32 ) g_csp.erase(g_csp.begin());
}

/*
 * A response's header lines given to the document committed from it (its
 * WebURLResponse): the status, the lines but those of the bytes as they
 * came (their length and encoding: the bytes given are decoded), the
 * protocol and the lengths; its policies kept for its CSP (csp_keep)
 */
void apply_response( blink::WebURLResponse &r, const std::string &url, int status, const std::string &headers,
		     const std::string &protocol, size_t body )
{
	static const char *const kReasons[][2] = {
		{ "200", "OK" }, { "201", "Created" }, { "204", "No Content" }, { "206", "Partial Content" },
		{ "301", "Moved Permanently" }, { "302", "Found" }, { "304", "Not Modified" }, { "400", "Bad Request" },
		{ "401", "Unauthorized" }, { "403", "Forbidden" }, { "404", "Not Found" }, { "410", "Gone" },
		{ "429", "Too Many Requests" }, { "500", "Internal Server Error" }, { "502", "Bad Gateway" },
		{ "503", "Service Unavailable" },
	};
	std::vector<BrCspLine> csp;
	std::string ctype;
	if ( status > 0 ) {
		r.SetHttpStatusCode(status);
		std::string code = std::to_string(status), reason;
		for ( const auto &k : kReasons ) {
			if ( code == k[0] ) reason = k[1];
		}
		r.SetHttpStatusText(blink::WebString::FromUtf8(reason));
	}
	size_t at = 0;
	while ( at < headers.size() ) {
		size_t end = headers.find('\n', at);
		std::string line = headers.substr(at, end == std::string::npos ? std::string::npos : end - at);
		at = ( end == std::string::npos ) ? headers.size() : end + 1;
		while ( !line.empty() && ( line.back() == '\r' || line.back() == ' ' ) ) line.pop_back();
		size_t colon = line.find(':');
		if ( colon == std::string::npos || colon == 0 ) continue;
		std::string name = line.substr(0, colon), value = line.substr(colon + 1), lname = name;
		value.erase(0, value.find_first_not_of(" \t"));
		for ( char &c : lname ) c = (char)tolower((unsigned char)c);
		if ( lname == "content-type" && ctype.empty() ) ctype = value;
		if ( lname == "content-length" || lname == "content-encoding" || lname == "transfer-encoding"
		     || lname == "content-type" ) {
			continue;
		}
		r.AddHttpHeaderField(blink::WebString::FromLatin1(name), blink::WebString::FromLatin1(value));
		if ( lname == "content-security-policy" ) csp.push_back({ value, false });
		if ( lname == "content-security-policy-report-only" ) csp.push_back({ value, true });
	}
	/*
	 * the type the document was made as, which its navigation timing
	 * names (contentType): the server's Content-Type when it is of that
	 * type, else the type alone (a PDF shown as its HTML page)
	 */
	{
		std::string mime = r.MimeType().Utf8(), lc = ctype;
		for ( char &c : lc ) c = (char)tolower((unsigned char)c);
		if ( !mime.empty() ) {
			bool same = lc.compare(0, mime.size(), mime) == 0 &&
				    ( lc.size() == mime.size() || lc[mime.size()] == ';' || lc[mime.size()] == ' ' );
			r.SetHttpHeaderField(blink::WebString::FromLatin1("Content-Type"),
					     blink::WebString::FromLatin1(same ? ctype : mime));
		}
	}
	if ( !protocol.empty() ) {
		/* "unknown": no ALPN (HTTP/1.1 in the clear); the timing then names the connection's protocol */
		r.SetWasAlpnNegotiated(protocol != "unknown");
		r.SetAlpnNegotiatedProtocol(blink::WebString::FromUtf8(protocol));
		r.SetWasFetchedViaSPDY(protocol == "h2" || protocol == "h3");
		r.SetConnectionInfo(protocol == "h3" ? net::HttpConnectionInfo::kQUIC_RFC_V1
				    : protocol == "h2" ? net::HttpConnectionInfo::kHTTP2
						       : net::HttpConnectionInfo::kHTTP1_1);
	}
	r.SetEncodedBodyLength(body);
	r.SetEncodedDataLength((int64_t)( body + headers.size() ));
	csp_keep(url, std::move(csp));
}

/*
 * The page's cross-origin isolation: the origin of its top-level
 * document when that document's response asked for it (COOP
 * "same-origin" with a COEP that isolates, in a secure context), as a
 * browser isolates the browsing context group of such a document. Every
 * document of the page is then in an agent cluster isolated with that
 * origin, which is what makes crossOriginIsolated true and lets the page
 * have SharedArrayBuffer (as far as the permissions policy
 * "cross-origin-isolated" lets each frame), and its dedicated workers
 * with it.
 */
bool			g_coi;
blink::WebSecurityOrigin g_coi_origin;

/*
 * The agent cluster a document's window joins: keyed by its origin, as
 * Chrome keys a document's by default (window.originAgentCluster is
 * true, and document.domain no longer joins it to the others of its
 * site), unless its response says "Origin-Agent-Cluster: ?0"; and,
 * within a cross-origin isolated page, isolated with the page. The
 * document's Cross-Origin-Embedder-Policy goes into its policies, where
 * its loads read it (br_loader.cc checks their Cross-Origin-Resource-Policy
 * by it).
 */
void set_agent_cluster( blink::WebNavigationParams *params, const std::string &headers, bool main_frame )
{
	blink::KURL url = params->url;
	if ( !url.ProtocolIsInHttpFamily() ) {
		if ( main_frame && !url.IsAboutBlankUrl() && !url.IsAboutSrcdocUrl() ) g_coi = false;
		return;
	}
	scoped_refptr<net::HttpResponseHeaders> h = base::MakeRefCounted<net::HttpResponseHeaders>(
		net::HttpUtil::AssembleRawHeaders("HTTP/1.1 200 OK\r\n" + headers + "\r\n"));
	bool secure = network::IsUrlPotentiallyTrustworthy(GURL(url));
	network::CrossOriginEmbedderPolicy coep;
	if ( secure ) coep = network::ParseCrossOriginEmbedderPolicy(*h);
	if ( params->policy_container ) params->policy_container->policies.cross_origin_embedder_policy = coep.value;
	blink::WebSecurityOrigin origin = blink::WebSecurityOrigin::Create(blink::WebURL(url));
	if ( main_frame ) {
		network::CrossOriginOpenerPolicy coop;
		if ( secure ) coop = network::ParseCrossOriginOpenerPolicy(*h);
		bool was = g_coi;
		g_coi = ( coop.value == network::mojom::CrossOriginOpenerPolicyValue::kSameOrigin
			  || coop.value == network::mojom::CrossOriginOpenerPolicyValue::kSameOriginPlusCoep )
			&& network::CompatibleWithCrossOriginIsolated(coep);
		g_coi_origin = origin;
		if ( g_coi || was ) printf("browser: the page is %scross-origin isolated\n", g_coi ? "" : "no longer ");
	}
	blink::WebOriginKeyedAgentClusterKey key;
	key.origin = origin;
	if ( g_coi ) {
		key.isolation_key = blink::WebCrossOriginIsolationKey{g_coi_origin, blink::mojom::CrossOriginIsolationMode::kConcrete};
	} else {
		std::string lh = headers;
		for ( char &c : lh ) c = (char)tolower((unsigned char)c);
		size_t at = lh.find("origin-agent-cluster:");
		if ( at != std::string::npos && ( at == 0 || lh[at - 1] == '\n' ) ) {
			size_t v = lh.find_first_not_of(" \t", at + 21);
			if ( v != std::string::npos && lh.compare(v, 2, "?0") == 0 ) return;
		}
	}
	params->agent_cluster_key = blink::WebAgentClusterKey(key);
}

/*
 * When the window's navigation was sent and answered (br_navigation_timing,
 * from br_fetch): the document's navigation and fetch start, and its
 * response's request and header times, so that its timing
 * (performance.timing, the navigation entry) runs from when it was asked
 * for, as a browser's does, not from the commit
 */
base::TimeTicks g_nav_began, g_nav_sent, g_nav_answered;

void apply_nav_timing( blink::WebNavigationParams *params )
{
	base::TimeTicks now = base::TimeTicks::Now();
	base::TimeTicks began = g_nav_began, sent = g_nav_sent, answered = g_nav_answered;
	g_nav_began = g_nav_sent = g_nav_answered = base::TimeTicks();
	if ( sent.is_null() || answered.is_null() || answered < sent || now - answered > base::Seconds(120) ) return;
	/* the navigation begins when the window is told to go, a little before its request is sent */
	if ( began.is_null() || began > sent ) began = sent;
	params->navigation_timings.navigation_start = began;
	params->navigation_timings.fetch_start = sent;
	network::mojom::LoadTimingInfo t;
	t.request_start = sent;
	t.request_start_time = base::Time::Now() - ( now - sent );
	t.send_start = sent;
	t.send_end = sent;
	t.receive_headers_start = answered;
	t.receive_headers_end = answered;
	t.receive_non_informational_headers_start = answered;
	params->response.SetLoadTiming(t);
}

/*
 * "trace=exceptions": each exception a browser API throws into a page's
 * script on the main thread, told once (the interface, the member and the
 * exception's text) after Blink has given it its context, as the page
 * sees it; the first 300 told, then a count every 100 more
 */
void traced_propagation( v8::ExceptionPropagationMessage m )
{
	blink::V8Initializer::ExceptionPropagationCallback(m);
	static std::set<std::string> told;
	static unsigned more;
	v8::Isolate *iso = m.GetIsolate();
	v8::HandleScope hs(iso);
	v8::TryCatch tc(iso);
	v8::String::Utf8Value iface(iso, m.GetInterfaceName()), prop(iso, m.GetPropertyName());
	std::string text;
	v8::Local<v8::String> s;
	if ( iso->InContext() && m.GetException()->ToString(iso->GetCurrentContext()).ToLocal(&s) ) {
		v8::String::Utf8Value t(iso, s);
		if ( *t != nullptr ) text = *t;
	}
	std::string key = std::string(*iface != nullptr ? *iface : "?") + "." + ( *prop != nullptr ? *prop : "?" ) + ": " + text;
	if ( told.size() < 300 && told.insert(key).second ) {
		printf("browser: api threw: %s\n", key.substr(0, 300).c_str());
	} else if ( ++more % 100 == 0 ) {
		printf("browser: api threw: %u more\n", more);
	}
}

class BrPlatform final : public blink::Platform {
public:
	/*
	 * navigator.cpuPerformance: the tier a browser gives a processor it
	 * does not know by name, from the number of its cores (1-2 low, 3-4
	 * mid, 5-12 high, more ultra)
	 */
	blink::mojom::PerformanceTier GetCpuPerformanceTier() override
	{
		int n = base::SysInfo::NumberOfProcessors();
		if ( n >= 13 ) return blink::mojom::PerformanceTier::kUltra;
		if ( n >= 5 ) return blink::mojom::PerformanceTier::kHigh;
		if ( n >= 3 ) return blink::mojom::PerformanceTier::kMid;
		if ( n >= 1 ) return blink::mojom::PerformanceTier::kLow;
		return blink::mojom::PerformanceTier::kUnknown;
	}

	/* the policies the document's response declared (apply_response) */
	void AppendContentSecurityPolicy( const blink::WebURL &url,
					  std::vector<blink::WebContentSecurityPolicyHeader> *out ) override
	{
		std::string u = blink::KURL(url).GetString().Utf8();
		std::lock_guard<std::mutex> hold(g_csp_lock);
		for ( const auto &e : g_csp ) {
			if ( e.first != u ) continue;
			for ( const BrCspLine &l : e.second ) {
				blink::WebContentSecurityPolicyHeader h;
				h.header_value = blink::WebString::FromLatin1(l.value);
				h.type = l.report_only ? network::mojom::ContentSecurityPolicyType::kReport
						       : network::mojom::ContentSecurityPolicyType::kEnforce;
				h.source = network::mojom::ContentSecurityPolicySource::kHTTP;
				out->push_back(h);
			}
		}
	}

	std::unique_ptr<blink::WebV8ValueConverter> CreateWebV8ValueConverter() override
	{
		return std::make_unique<BrValueConverter>();
	}

	/* the cameras' capture, for getUserMedia's video: its host is port/blink/camera_tessronos.cc */
	blink::WebVideoCaptureImplManager *GetVideoCaptureImplManager() override
	{
		static blink::WebVideoCaptureImplManager *manager = new blink::WebVideoCaptureImplManager();
		return manager;
	}

	/* the capture of a stream's local audio source, the microphone or a share's sound (port/media/br_audio_sink.cc) */
	scoped_refptr<media::AudioCapturerSource> NewAudioCapturerSource( blink::WebLocalFrame *,
									  const media::AudioSourceParameters &params ) override
	{
		return br_audio_capturer(params.session_id);
	}

	/*
	 * crypto.subtle's operations: Chromium's on BoringSSL (digests, keys,
	 * encryption, signatures, key agreement and derivation), run on the
	 * pool of worker threads and answered on the thread that asked
	 */
	blink::WebCrypto *Crypto() override { return &crypto_; }

	blink::WebSandboxSupport *GetSandboxSupport() override { return &sandbox_; }

	/*
	 * WebRTC's candidates on the loopback interface too: two connections
	 * of this machine reach each other with no network up
	 */
	bool AllowsLoopbackInPeerConnection() override { return true; }

	/* there is no video decoder in hardware for WebRTC: the media library's own */
	bool IsWebRtcHWDecodingEnabled() override { return false; }

	/*
	 * A renderer's IO thread: where a video track's frames are passed on
	 * (a canvas's or a media element's captureStream, MediaRecorder). A
	 * thread of its own; nothing of the browser's IPC is on it.
	 */
	scoped_refptr<base::SingleThreadTaskRunner> GetIOTaskRunner() const override
	{
		static base::Thread *thread = [] {
			auto *t = new base::Thread("io");
			if ( !t->StartWithOptions(base::Thread::Options()) ) printf("browser: the io thread did not start\n");
			return t;
		}();
		return thread->task_runner();
	}

	blink::WebString DefaultLocale() override { return blink::WebString::FromAscii("ja"); }

	blink::WebString UserAgent() override
	{
		return blink::WebString::FromAscii(br_user_agent());
	}

	bool HasDataResource( int id ) const override
	{
		const uint8_t	*p;
		size_t		n;
		return g_pak.find(id, &p, &n);
	}

	blink::WebData GetDataResource( int id, ui::ResourceScaleFactor ) override
	{
		const uint8_t	*p;
		size_t		n;
		if ( !g_pak.find(id, &p, &n) ) return blink::WebData();
		return blink::WebData(base::span<const uint8_t>(p, n));
	}

	std::string GetDataResourceString( int id ) override { return g_pak.text(id); }

	scoped_refptr<base::RefCountedMemory> GetDataResourceBytes( int id ) override
	{
		std::string s = g_pak.text(id);
		return base::MakeRefCounted<base::RefCountedString>(std::move(s));
	}

	/*
	 * Compositing is on the CPU: the drawing buffer of WebGL then keeps
	 * its colour buffers readable, which the canvas's paint reads back
	 */
	bool IsGpuCompositingDisabled() const override { return true; }

	/* WebGL 1 and 2 in the GPU process or on ANGLE, or WebGL 1 on the CPU (BrWebGLProvider) */
	std::unique_ptr<blink::WebGraphicsContext3DProvider> CreateWebGLGraphicsContextProvider(
		bool low_power, bool fail_caveat, WebGLContextType type, const blink::WebURL &url,
		WebGLContextInfo *info ) override
	{
		return BrWebGLProvider(type == kWebGL2ContextType, low_power, fail_caveat, url, info);
	}

	/* WebGPU in the GPU process or on Dawn here, or none (the page's requestAdapter answers null) */
	void CreateWebGPUGraphicsContext3DProviderAsync(
		const blink::WebURL &url, WebGPUReplyThread,
		base::OnceCallback<void( std::unique_ptr<blink::WebGraphicsContext3DProvider> )> callback ) override
	{
		std::move(callback).Run(BrWebGPUProvider(url, /*wait_on=*/false));
	}

	/* The same, at once: WebGL on WebGPU (features=WebGLOnWebGPU) asks for it so, and blocks for its answers */
	std::unique_ptr<blink::WebGraphicsContext3DProvider> CreateWebGPUGraphicsContext3DProvider(
		const blink::WebURL &url, WebGPUReplyThread ) override
	{
		return BrWebGPUProvider(url, /*wait_on=*/true);
	}

	std::unique_ptr<blink::WebGraphicsContext3DProvider>
	CreateSharedOffscreenGraphicsContext3DProvider() override
	{
		WebGLContextInfo info;
		return BrCreateGLProvider(&info);
	}

	/* An AudioContext's destination: the sound device, through the ring of the media elements */
	std::unique_ptr<blink::WebAudioDevice> CreateAudioDevice(
		const blink::WebAudioSinkDescriptor &sink, unsigned channels, const blink::WebAudioLatencyHint &,
		std::optional<float>, media::AudioRendererSink::RenderCallback *callback ) override
	{
		return BrCreateAudioDevice(sink, channels, callback);
	}

	/* What every thread asks the browser for: the registry of Blobs */
	blink::ThreadSafeBrowserInterfaceBrokerProxy *GetBrowserInterfaceBroker() override
	{
		return br_platform_broker();
	}

	/* The host of a dedicated worker the page or a worker starts */
	std::unique_ptr<blink::WebDedicatedWorkerHostFactoryClient> CreateDedicatedWorkerHostFactoryClient(
		blink::WebDedicatedWorker *worker, const blink::BrowserInterfaceBrokerProxy & ) override
	{
		return br_worker_host_factory_client(worker);
	}


private:
	BrSandbox	sandbox_;
	webcrypto::WebCryptoImpl	crypto_;
};

/* ---------------------------------------------------------------- core without modules */

/*
 * What modules would add to core: the tracer of Web Audio's graphs on
 * each page, and a picture-in-picture controller that allows none. The
 * media elements' player is made by the frame's client (br_media_player);
 * their controls are Blink's own; the inspector and the other module
 * features are not built in.
 */
class BrCoreInitializer final : public blink::CoreInitializer {
public:
	void RegisterInterfaces( mojo::BinderMap & ) override {}
	void InitLocalFrame( blink::LocalFrame & ) const override {}
	void InitServiceWorkerGlobalScope( blink::ServiceWorkerGlobalScope & ) const override {}
	void InstallSupplements( blink::LocalFrame & ) const override {}
	/* the media elements' controls: Blink's own */
	blink::MediaControls *CreateMediaControls( blink::HTMLMediaElement &element,
						   blink::ShadowRoot &shadow ) const override
	{
		return blink::MediaControlsImpl::Create(element, shadow);
	}
	/* the module's own: a document's picture-in-picture window, and a video's (port/media) */
	blink::PictureInPictureController *CreatePictureInPictureController(
		blink::Document &document ) const override
	{
		return blink::MakeGarbageCollected<blink::PictureInPictureControllerImpl>(document);
	}
	void InitInspectorAgentSession( blink::DevToolsSession *, blink::InspectorDOMAgent *,
					blink::InspectedFrames *, blink::Page * ) const override {}
	void InitWorkerInspectorAgentSession( blink::DevToolsSession *,
					      blink::WorkerGlobalScope * ) const override {}
	void OnClearWindowObjectInMainWorld( blink::Document &, const blink::Settings & ) const override {}
	std::unique_ptr<blink::WebMediaPlayer> CreateWebMediaPlayer(
		blink::WebLocalFrameClient *, blink::HTMLMediaElement &,
		const blink::WebMediaPlayerSource &, blink::WebMediaPlayerClient * ) const override
	{
		return nullptr;
	}
	blink::RemotePlaybackClient *CreateRemotePlaybackClient( blink::HTMLMediaElement & ) const override
	{
		return nullptr;
	}
	/* the tracer every AudioContext of the page reports to */
	void ProvideModulesToPage( blink::Page &page, const blink::SessionStorageNamespaceId & ) const override
	{
		blink::AudioGraphTracer::ProvideAudioGraphTracerTo(page);
	}
	void ForceNextWebGLContextCreationToFail() const override {}
	void CollectAllGarbageForAnimationAndPaintWorkletForTesting() const override {}
	void CloneSessionStorage( blink::Page *, const blink::SessionStorageNamespaceId & ) override {}
	void EvictSessionStorageCachedData( blink::Page * ) override {}
	void DidChangeManifest( blink::LocalFrame & ) override {}
	void NotifyOrientationChanged( blink::LocalFrame & ) override {}
	void DidUpdateScreens( blink::LocalFrame &, const display::ScreenInfos & ) override {}
	void SetLocalStorageArea( blink::LocalFrame &,
				  mojo::PendingRemote<blink::mojom::blink::StorageArea> ) override {}
	void SetSessionStorageArea( blink::LocalFrame &,
				    mojo::PendingRemote<blink::mojom::blink::StorageArea> ) override {}
	/* the older File System API's files, registered as Blobs (filesystem_tessronos.cc) */
	blink::mojom::blink::FileSystemManager &GetFileSystemManager( blink::ExecutionContext * ) override
	{
		return br_filesystem_manager();
	}
};

/* ---------------------------------------------------------------- the page's clients */

struct BrPage;

/*
 * The list of a <select> shown as a list: its options (each after its
 * group's label, indented) on a panel of the browser's data box
 * (br_choose), the one chosen given back to the element as a popup gives
 * it. The panel is asked from a task of its own after the press that
 * opened it, so that the press is over when the page waits on it.
 */
class BrPopupMenu final : public blink::PopupMenu {
public:
	explicit BrPopupMenu( blink::HTMLSelectElement &owner ) : owner_(&owner) {}

	void Trace( blink::Visitor *v ) const override
	{
		v->Trace(owner_);
		blink::PopupMenu::Trace(v);
	}

	void Show( ShowEventType ) override
	{
		if ( owner_ == nullptr || shown_ ) return;
		shown_ = true;
		owner_->GetDocument().GetTaskRunner(blink::TaskType::kUserInteraction)
			->PostTask(FROM_HERE, blink::BindOnce(&BrPopupMenu::Ask, blink::WrapPersistent(this)));
	}

	void Hide() override {}
	void UpdateFromElement( UpdateReason ) override {}
	void DisconnectClient() override { owner_ = nullptr; }

private:
	void Ask()
	{
		std::vector<std::string>	names;
		std::vector<int>		index;	/* the list index of each name, -1 a group's label */
		int				chosen = -1;

		if ( owner_ == nullptr ) {
			shown_ = false;
			return;
		}
		blink::HTMLSelectElement *sel = owner_.Get();
		const auto &items = sel->GetListItems();
		for ( blink::wtf_size_t i = 0; i < items.size(); i++ ) {
			blink::HTMLElement &e = *items[i];
			if ( sel->ItemIsDisplayNone(e, true) || blink::IsA<blink::HTMLHRElement>(e) ) continue;
			std::string label(sel->ItemText(e).Utf8());
			if ( blink::IsA<blink::HTMLOptGroupElement>(e) ) {
				names.push_back("【" + label + "】");
				index.push_back(-1);
				continue;
			}
			auto *o = blink::DynamicTo<blink::HTMLOptionElement>(e);
			bool grouped = o != nullptr && o->NearestAncestorOptgroup() != nullptr;
			names.push_back(( grouped ? "　" : "" ) + label);
			index.push_back(( o != nullptr && !o->IsDisabledFormControl() ) ? (int)i : -1);
			if ( o != nullptr && o->Selected() ) chosen = (int)names.size() - 1;
		}
		int k = br_choose(names, chosen);
		shown_ = false;
		if ( owner_ == nullptr ) return;
		sel = owner_.Get();
		sel->PopupDidHide();
		if ( k >= 0 && k < (int)index.size() && index[(size_t)k] >= 0 ) {
			sel->SelectOptionByPopup(index[(size_t)k]);
		}
	}

	blink::Member<blink::HTMLSelectElement>	owner_;
	bool					shown_ = false;
};

/*
 * <input type="file">: real objects chosen on the browser's panel (the
 * links in the tray, and links dropped on it), each given to the page as
 * the one file it is given out as and kept under a path of its own, which
 * the page's File reads. With webkitdirectory, one real object is chosen
 * and given out as a folder (a figure of links is the folder of what it
 * links to): every file in it, each with the names of the folders above
 * it, the chosen one first, which make its webkitRelativePath (the paths
 * kept are of ASCII only and do not carry the names). Nothing chosen
 * tells the page that the choice was given up.
 */
void ask_files( scoped_refptr<blink::FileChooser> chooser )
{
	namespace bm = blink::mojom::blink;
	const bm::FileChooserParams	&params = chooser->Params();
	std::vector<BrObjLink>		links;
	blink::Vector<bm::FileChooserFileInfoPtr> files;
	std::string			base;

	if ( params.mode == bm::FileChooserParams::Mode::kOpen ||
	     params.mode == bm::FileChooserParams::Mode::kOpenMultiple ) {
		bool multiple = params.mode == bm::FileChooserParams::Mode::kOpenMultiple;
		if ( br_choose_objects(multiple, &links) ) {
			for ( const BrObjLink &l : links ) {
				std::string name;
				std::vector<uint8_t> bytes;
				if ( !br_obj_export(l.id, &name, &bytes) ) continue;
				size_t n = bytes.size();
				std::string path = br_file_keep(br_uuid_str(l.id), name, std::move(bytes));
				printf("browser: the page is given %s (%zu bytes)\n", name.c_str(), n);
				files.push_back(bm::FileChooserFileInfo::NewNativeFile(bm::NativeFileInfo::New(
					base::FilePath(path), blink::String::FromUtf8(name), blink::Vector<blink::String>())));
			}
		}
	} else if ( params.mode == bm::FileChooserParams::Mode::kUploadFolder ) {
		std::string top;
		std::vector<BrTreeEntry> tree;
		if ( br_choose_objects(false, &links) && !links.empty() && br_obj_export_tree(links[0].id, &top, &tree) ) {
			std::string key = br_uuid_str(links[0].id);
			base = "/obj/" + key + "/top";
			int k = 0;
			for ( BrTreeEntry &e : tree ) {
				if ( e.dir ) continue;
				blink::Vector<blink::String> subdirs;
				subdirs.push_back(blink::String::FromUtf8(top));
				size_t from = 0, sl;
				while ( ( sl = e.path.find('/', from) ) != std::string::npos ) {
					subdirs.push_back(blink::String::FromUtf8(e.path.substr(from, sl - from)));
					from = sl + 1;
				}
				std::string name = e.path.substr(from);
				std::string path = br_file_keep(key + "/top/" + std::to_string(k++), name, std::move(e.bytes));
				files.push_back(bm::FileChooserFileInfo::NewNativeFile(bm::NativeFileInfo::New(
					base::FilePath(path), blink::String::FromUtf8(name), std::move(subdirs))));
			}
			printf("browser: the page is given the folder %s (%d files)\n", top.c_str(), k);
		}
	}
	if ( files.empty() ) {
		chooser->DidChooseFilesForEmbedder(nullptr);
		return;
	}
	chooser->DidChooseFilesForEmbedder(bm::FileChooserResult::New(std::move(files), base::FilePath(base)));
}

/* ---------------------------------------------------------------- what a page saves */

/* %XX of a name decoded */
std::string unescape( const std::string &s )
{
	std::string o;
	for ( size_t i = 0; i < s.size(); i++ ) {
		if ( s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2]) ) {
			o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
			i += 2;
		} else {
			o += s[i];
		}
	}
	return o;
}

/* The file name a Content-Disposition header gives (filename*=UTF-8'' first), "" for none */
std::string disposition_name( const std::string &headers )
{
	std::string low = headers;
	for ( char &c : low ) c = (char)tolower((unsigned char)c);
	size_t at = low.find("content-disposition:");
	if ( at == std::string::npos ) return std::string();
	size_t end = low.find('\n', at);
	std::string line = headers.substr(at, end == std::string::npos ? std::string::npos : end - at);
	std::string l = low.substr(at, line.size());
	size_t k = l.find("filename*=");
	if ( k != std::string::npos ) {
		std::string v = line.substr(k + 10);
		v = v.substr(0, v.find_first_of(";\r"));
		size_t q = v.find("''");
		if ( q != std::string::npos ) return unescape(v.substr(q + 2));
	}
	k = l.find("filename=");
	if ( k == std::string::npos ) return std::string();
	std::string v = line.substr(k + 9);
	if ( !v.empty() && v[0] == '"' ) {
		size_t q = v.find('"', 1);
		return v.substr(1, q == std::string::npos ? std::string::npos : q - 1);
	}
	return v.substr(0, v.find_first_of(";\r "));
}

/*
 * The name what is saved goes under: the one the page or the server
 * gave, else the last part of its address, given the extension of its
 * type when it has none (taking it in tells a text and a picture by it)
 */
std::string save_name( std::string name, const std::string &url, const std::string &type )
{
	static const struct { const char *type, *ext; } kExt[] = {
		{ "text/plain", ".txt" }, { "text/html", ".html" }, { "text/css", ".css" },
		{ "text/csv", ".csv" }, { "application/json", ".json" }, { "application/pdf", ".pdf" },
		{ "image/png", ".png" }, { "image/jpeg", ".jpg" }, { "image/gif", ".gif" },
		{ "image/bmp", ".bmp" }, { "image/webp", ".webp" }, { "image/svg+xml", ".svg" },
		{ "application/zip", ".zip" },
	};
	GURL g(url);
	if ( name.empty() && g.SchemeIsHTTPOrHTTPS() ) name = unescape(g.ExtractFileName());
	if ( name.empty() ) name = "download";
	if ( name.find('.') == std::string::npos ) {
		std::string t = type.substr(0, type.find(';'));
		for ( auto &e : kExt ) {
			if ( t == e.type ) {
				name += e.ext;
				break;
			}
		}
	}
	return name;
}

/*
 * The bytes made a real object in ブラウザの保存箱 with a link in the tray,
 * off the page's thread. A renderer hands them to the browser, which keeps
 * the downloads: the browser may end the renderer as soon as the page has
 * been answered (a renderer started for an attachment's site shows
 * nothing), and a save of its own would end with it.
 */
void save_bytes( const std::string &name, std::string bytes )
{
	if ( br_multi_process && br_is_renderer ) {
		if ( !br_rproc_keep_download(name, bytes) ) {
			printf("browser: %s not handed to the browser: not saved\n", name.c_str());
		}
		return;
	}
	/* the message line says it before the save, after what the window said of the page (br_main.cc, "said:") */
	br_main_command("said:「" + name + "」を保存しています…");
	std::thread([name, bytes = std::move(bytes)]() {
		unsigned char id[16];
		std::vector<uint8_t> b(bytes.begin(), bytes.end());
		int dl = br_download_start(name, std::string());
		(void)br_download_progress(dl, (long long)b.size(), (long long)b.size());
		br_download_end(dl, br_obj_save(name, b, id) ? BR_DL_DONE : BR_DL_FAILED);
	}).detach();
}

/*
 * What a page downloads (a link with the download attribute, a blob: or
 * data: URL it saves): the bytes are taken from the data: URL, read from
 * the Blob (the token the page gave keeps a revoked URL found until then)
 * or fetched, and saved as a real object.
 */
void save_from_net( const std::string &url, const std::string &given );

void save_download( blink::LocalFrame &, blink::mojom::blink::DownloadURLParamsPtr params )
{
	std::string url = params->url.GetString().Utf8();
	std::string given = params->suggested_name.IsNull() ? std::string() : params->suggested_name.Utf8();

	if ( params->data_url_blob ) {
		/* a data: URL, made a Blob by the frame: read by the Blob's UUID */
		printf("browser: the page saves a data: URL%s%s\n", given.empty() ? "" : " as ", given.c_str());
		br_on_service_thread(base::BindOnce([]( mojo::PendingRemote<blink::mojom::blink::Blob> pending,
							std::string given ) {
			auto *blob = new mojo::Remote<blink::mojom::blink::Blob>(std::move(pending));
			( *blob )->GetInternalUUID(base::BindOnce(
				[]( mojo::Remote<blink::mojom::blink::Blob> *blob, std::string given, const blink::String &uuid ) {
					delete blob;
					br_blob_read(uuid.Utf8(), false, base::BindOnce(
						[]( std::string given, bool ok, std::string bytes, std::string type ) {
							if ( ok ) save_bytes(save_name(given, std::string(), type), std::move(bytes));
						}, given));
				}, blob, given));
		}, std::move(params->data_url_blob), given));
		return;
	}
	printf("browser: the page saves %s%s%s\n", url.substr(0, 120).c_str(),
	       given.empty() ? "" : " as ", given.c_str());
	if ( params->url.ProtocolIs("blob") && params->blob_url_token ) {
		/*
		 * The URL's registration, the token and the URL's revocation go
		 * to the store through the page's thread, after this: the token
		 * is asked first, and once it answers the store has the URL, kept
		 * by the token while it lives (until the bytes are read)
		 */
		using Token = mojo::Remote<blink::mojom::blink::BlobURLToken>;
		br_on_service_thread(base::BindOnce([]( mojo::PendingRemote<blink::mojom::blink::BlobURLToken> pending,
							std::string given, std::string url ) {
			auto *token = new Token(std::move(pending));
			( *token )->GetToken(base::BindOnce([]( Token *token, std::string given, std::string url,
								const base::UnguessableToken & ) {
				br_blob_read(url, true, base::BindOnce(
					[]( Token *token, std::string given, std::string url, bool ok, std::string bytes,
					    std::string type ) {
						delete token;
						if ( !ok ) {
							printf("browser: %s is not there to save\n", url.c_str());
							return;
						}
						save_bytes(save_name(given, url, type), std::move(bytes));
					}, token, given, url));
			}, token, given, url));
		}, std::move(params->blob_url_token), given, url));
		return;
	}
	if ( !params->url.ProtocolIsInHttpFamily() ) {
		printf("browser: %s is not saved (only http, https, data and blob)\n", url.c_str());
		return;
	}
	save_from_net(url, given);
}

/* A download whose bytes stop coming for this long has failed */
constexpr long kDlIdleMs = 60000;

long dl_now_ms()
{
	return (long)std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

/* A header's value from a block of header lines ("Name: value\r\n" each), "" when it is not there */
std::string header_of( const std::string &headers, const char *name )
{
	std::string low = headers;
	for ( char &c : low ) c = (char)tolower((unsigned char)c);
	std::string key = std::string(name) + ":";
	for ( size_t at = 0; at < low.size(); ) {
		size_t end = low.find('\n', at);
		if ( end == std::string::npos ) end = low.size();
		if ( low.compare(at, key.size(), key) == 0 ) {
			std::string v = headers.substr(at + key.size(), end - at - key.size());
			v.erase(0, v.find_first_not_of(" \t"));
			while ( !v.empty() && ( v.back() == '\r' || v.back() == ' ' || v.back() == '\t' ) ) v.pop_back();
			return v;
		}
		at = end + 1;
	}
	return std::string();
}

/* The length a response says its body has, -1 when it does not */
long long length_of( const std::string &headers )
{
	std::string v = header_of(headers, "content-length");
	return v.empty() ? -1 : atoll(v.c_str());
}

/*
 * A request whose answer is looked at as it comes (a window's navigation,
 * a link's download): a paced stream (br_net_fetch_paced) told on the
 * network's thread, waited for by its asker on its own. What the answer
 * is, is decided from its response and the start of its body; a download
 * is then handed over (ns_hand_over) and the asker holds no more of it.
 * Its callbacks keep it (self) until its end.
 */
struct NavStream {
	std::mutex			m;
	std::condition_variable		cv;
	bool				head = false, ended = false;
	int				ok = 0, status = 0;
	std::string			url, headers, ctype, body;
	std::shared_ptr<BrDownload>	dl;	/* handed over in this process: the pieces go there */
	std::shared_ptr<NavStream>	self;
};

void ns_part( void *ctx, int first, int status, const char *url, const char *headers, const char *ctype,
	      const char *data, unsigned len )
{
	NavStream *s = static_cast<NavStream *>(ctx);
	std::unique_lock<std::mutex> hold(s->m);
	if ( first ) {
		s->head = true;
		s->status = status;
		s->url = url != nullptr ? url : "";
		s->headers = headers != nullptr ? headers : "";
		s->ctype = ctype != nullptr ? ctype : "";
		s->cv.notify_all();
		return;
	}
	if ( len == 0 ) return;
	/* kept until the piece is taken: the request is known by this address until then */
	std::shared_ptr<NavStream> keep = s->self;
	if ( s->dl != nullptr ) {
		std::shared_ptr<BrDownload> dl = s->dl;
		hold.unlock();
		br_download_bytes(dl, std::string(data, len), [keep, len] { br_net_fetch_taken(keep.get(), len); });
		return;
	}
	s->body.append(data, len);
	s->cv.notify_all();
	hold.unlock();
	br_net_fetch_taken(s, len);
}

void ns_done( void *ctx, int ok, int status, const char *url, const char *headers, const char *ctype,
	      const char *data, unsigned len )
{
	NavStream *s = static_cast<NavStream *>(ctx);
	std::shared_ptr<NavStream> keep;
	std::shared_ptr<BrDownload> dl;
	{
		std::lock_guard<std::mutex> hold(s->m);
		keep = std::move(s->self);
		s->ended = true;
		s->ok = ok;
		if ( !s->head ) {
			s->status = status;
			s->url = url != nullptr ? url : "";
			s->headers = headers != nullptr ? headers : "";
			s->ctype = ctype != nullptr ? ctype : "";
		}
		/* what takes it in keeps this stream (to stop it) until its end: not the other way round from here */
		dl = std::move(s->dl);
		if ( dl == nullptr && len > 0 ) s->body.append(data, len);
		s->cv.notify_all();
	}
	if ( dl != nullptr ) {
		if ( len > 0 ) br_download_bytes(dl, std::string(data, len), nullptr);
		br_download_finish(dl, ok != 0);
	}
}

/* A stream started for the asker to wait on; null when it could not be */
std::shared_ptr<NavStream> ns_start( const std::string &url, const std::string &method, const std::string &body,
				     const std::string &hdrs )
{
	auto s = std::make_shared<NavStream>();
	s->self = s;
	if ( !br_net_fetch_paced(url.c_str(), method.empty() ? "GET" : method.c_str(), body.data(), (unsigned)body.size(),
				 hdrs.c_str(), &ns_part, &ns_done, s.get(), 1) ) {
		s->self.reset();
		return nullptr;
	}
	return s;
}

/* The stream given up by its asker: its end still comes, and frees it */
void ns_let_go( const std::shared_ptr<NavStream> &s )
{
	std::lock_guard<std::mutex> hold(s->m);
	if ( !s->ended ) br_net_fetch_cancel(s.get());
}

/*
 * A download handed over at its response, to be kept by the process that
 * has the window as it comes. A renderer takes the request out of itself
 * and hands it to the browser with the bytes that came before
 * (Host.Download); the browser goes on with it, and the renderer may end.
 * In one process the pieces go to what takes them in here from now on.
 * False when it was not taken.
 */
bool ns_hand_over( const std::shared_ptr<NavStream> &s, const std::string &name, const std::string &url,
		   const std::string &type, long long total, int status )
{
	if ( br_net_fetch_remote() ) {
		void *taken = br_net_fetch_take(s.get());
		std::string first;
		bool ended, ok;
		{
			std::lock_guard<std::mutex> hold(s->m);
			first.swap(s->body);
			ended = s->ended;
			ok = s->ok != 0;
			/* taken: its callbacks are not called again */
			if ( taken != nullptr ) s->self.reset();
		}
		if ( !br_net_fetch_give(taken, name.c_str(), url.c_str(), type.c_str(), total, status, first.data(),
					(unsigned)first.size(), ended ? 1 : 0, ok ? 1 : 0) ) {
			printf("browser: %s not handed to the browser: not saved\n", name.c_str());
			return false;
		}
		return true;
	}
	std::shared_ptr<NavStream> keep = s;
	std::shared_ptr<BrDownload> dl = br_download_begin(name, url, total, status, [keep] { ns_let_go(keep); });
	std::lock_guard<std::mutex> hold(s->m);
	if ( dl == nullptr ) {
		if ( !s->ended ) br_net_fetch_cancel(s.get());
		return false;
	}
	if ( !s->body.empty() ) br_download_bytes(dl, std::move(s->body), nullptr);
	s->body.clear();
	if ( s->ended ) br_download_finish(dl, s->ok != 0);
	else s->dl = dl;
	return true;
}

/* A download that did not come to a response: a row of the list that failed */
void dl_failed_row( const std::string &name, const std::string &url )
{
	br_download_end(br_download_start(name, url), BR_DL_FAILED);
}

/*
 * What an address names, fetched for a link with the download attribute
 * and handed over at its response as a navigation's download is: saved
 * as a real object in the save box as it comes, its row in the list moving,
 * 中止 asked for from the settings window
 */
void save_from_net( const std::string &url, const std::string &given )
{
	std::thread([url, given]() {
		std::string shown = given.empty() ? save_name(std::string(), url, std::string()) : given;
		std::shared_ptr<NavStream> s = ns_start(url, "GET", std::string(), std::string());
		if ( s == nullptr ) {
			printf("browser: %s could not be fetched to save\n", url.c_str());
			dl_failed_row(shown, url);
			return;
		}
		std::unique_lock<std::mutex> hold(s->m);
		for ( long t0 = dl_now_ms(); !s->head && !s->ended; ) {
			if ( dl_now_ms() - t0 >= kDlIdleMs ) {
				hold.unlock();
				printf("browser: %s did not answer to be saved\n", url.c_str());
				ns_let_go(s);
				dl_failed_row(shown, url);
				return;
			}
			s->cv.wait_for(hold, std::chrono::milliseconds(200));
		}
		if ( !s->head && !s->ok ) {
			hold.unlock();
			printf("browser: %s could not be fetched to save\n", url.c_str());
			dl_failed_row(shown, url);
			return;
		}
		std::string from = !s->url.empty() ? s->url : url;
		std::string name = given.empty() ? disposition_name(s->headers) : given;
		name = save_name(name, from, s->ctype);
		std::string type = s->ctype;
		long long total = length_of(s->headers);
		int status = s->status;
		hold.unlock();
		(void)ns_hand_over(s, name, from, type, total, status);
	}).detach();
}

class BrChromeClient final : public blink::EmptyChromeClient {
public:
	explicit BrChromeClient( BrPage *p ) : page_(p) {}

	void ScheduleAnimation( const blink::LocalFrameView *, cc::BeginMainFrameReason,
				base::TimeDelta, bool ) override;

	/*
	 * When what a frame painted is on the screen: the paint timing (first
	 * paint, first contentful paint), the largest contentful paint and
	 * the other paint timings ask for it. The callbacks are answered with
	 * the time the next frame is shown (BlinkEngine::frame_shown), as a
	 * compositor answers with the time its frame was presented.
	 */
	void NotifyPresentationTime( blink::LocalFrame &, ReportTimeCallback callback ) override;

	/*
	 * A new window the page asked for (a link with target="_blank",
	 * window.open from a click, a document's picture-in-picture window):
	 * one that keeps its opener is a page of its own in this engine
	 * (br_open_window), which the application shows in a window of its
	 * own; one with no opener is opened by the application in a browser
	 * of its own, as Chrome opens it in a process of its own. One asked
	 * for with no click or key just before it (a script's popup) is not
	 * opened at all.
	 */
	blink::Page *CreateWindowDelegate( blink::LocalFrame *, const blink::FrameLoadRequest &,
					   const blink::AtomicString &, const blink::WebWindowFeatures &,
					   network::mojom::blink::WebSandboxFlags,
					   const blink::SessionStorageNamespaceId &, bool & ) override;

	/* window.close(), allowed (the window was opened by a script): the application closes it */
	void CloseWindow() override;

	/*
	 * Leaving a page whose beforeunload asks to stay: the user is asked on
	 * the browser's panel, as Chrome asks in its dialog (true: leave)
	 */
	bool CanOpenBeforeUnloadConfirmPanel() override { return true; }
	bool OpenBeforeUnloadConfirmPanelDelegate( blink::LocalFrame *, bool is_reload ) override
	{
		return br_ask_page(BR_ASK_CONFIRM,
				   is_reload ? "このサイトを再読み込みしますか?\n行った変更が保存されない可能性があります。"
					     : "このサイトから移動しますか?\n行った変更が保存されない可能性があります。",
				   std::string(), nullptr);
	}

	/*
	 * The window round the page on the screen (window.screenX, screenY,
	 * outerWidth, outerHeight): the browser's window with its frame, or
	 * for a window a page opened the page's own size
	 */
	gfx::Rect RootWindowRect( blink::LocalFrame &f ) override;

	/* The link and the tooltip under the pointer, for the message line (BlinkEngine::hover) */
	void ShowMouseOverURL( const blink::HitTestResult &r ) override;
	void UpdateTooltipUnderCursor( blink::LocalFrame &, const blink::String &text, blink::TextDirection ) override;

	/*
	 * element.requestFullscreen() and leaving it: the application shows
	 * the page over the whole screen, and tells the document when it has
	 * (BlinkEngine::fullscreen_done)
	 */
	void EnterFullscreen( blink::LocalFrame &, const blink::FullscreenOptions *, blink::FullscreenRequestType ) override;
	void ExitFullscreen( blink::LocalFrame & ) override;

	/*
	 * The windowing controls of an origin the user lets act as an isolated
	 * web app: the application does them to the window (BR_WIN_*) when it
	 * next looks, and the page is told they are done
	 */
	void Minimize( blink::LocalFrame &, WindowingControlsChangeCallback cb ) override;
	void Maximize( blink::LocalFrame &, WindowingControlsChangeCallback cb ) override;
	void Restore( blink::LocalFrame &, WindowingControlsChangeCallback cb ) override;
	void SetResizable( bool resizable, blink::LocalFrame &, WindowingControlsChangeCallback cb ) override;

	bool ShouldReportDetailedMessageForSourceAndSeverity(
		blink::LocalFrame &, blink::mojom::blink::ConsoleMessageLevel,
		const blink::String & ) override
	{
		return true;
	}

	void AddMessageToConsole( blink::LocalFrame *, blink::mojom::ConsoleMessageSource,
				  blink::mojom::ConsoleMessageLevel, const blink::String &message,
				  unsigned line, const blink::String &source,
				  const blink::String & ) override
	{
		printf("browser: console: %s (%s:%u)\n", message.Utf8().c_str(),
		       source.Utf8().c_str(), line);
	}

	/*
	 * An image's decode() asked for: the image is decoded when it is
	 * drawn, so the answer is yes, given from a task of its own. The
	 * image loader asks while it goes through its list of requests, and
	 * an answer given at once would take the request out of that list
	 * under its feet.
	 */
	void RequestDecode( blink::LocalFrame *frame, const cc::DrawImage &, base::OnceCallback<void(bool)> callback,
			    bool ) override
	{
		scoped_refptr<base::SingleThreadTaskRunner> runner =
			frame != nullptr ? frame->GetTaskRunner(blink::TaskType::kInternalDefault)
					 : base::SingleThreadTaskRunner::GetCurrentDefault();
		runner->PostTask(FROM_HERE, base::BindOnce(std::move(callback), true));
	}

	/* alert(), confirm() and prompt(): asked on the browser's panels while the page waits */
	bool OpenJavaScriptAlertDelegate( blink::LocalFrame *, const blink::String &message ) override
	{
		return br_ask_page(BR_ASK_ALERT, message.Utf8(), std::string(), nullptr);
	}

	bool OpenJavaScriptConfirmDelegate( blink::LocalFrame *, const blink::String &message ) override
	{
		return br_ask_page(BR_ASK_CONFIRM, message.Utf8(), std::string(), nullptr);
	}

	bool OpenJavaScriptPromptDelegate( blink::LocalFrame *, const blink::String &message,
					   const blink::String &default_value, blink::String &result ) override
	{
		std::string got;
		if ( !br_ask_page(BR_ASK_PROMPT, message.Utf8(), default_value.Utf8(), &got) ) return false;
		result = blink::String::FromUtf8(got);
		return true;
	}

	/* a <select>'s list */
	blink::PopupMenu *OpenPopupMenu( blink::LocalFrame &, blink::HTMLSelectElement &select ) override
	{
		return blink::MakeGarbageCollected<BrPopupMenu>(select);
	}

	/* window.print(): the frame's page printed while the page waits, as a browser's dialog holds it */
	void PrintDelegate( blink::LocalFrame *frame ) override
	{
		if ( frame != nullptr ) (void)br_print_frame(frame, frame->GetDocument()->Url().GetString().Utf8());
	}

	/*
	 * A drag the page starts (a draggable element, a selection, a link,
	 * an image): what it drags is kept, and the pointer's moves and its
	 * letting go are the drag's from then on (BlinkEngine::drag_mouse)
	 */
	void StartDragging( blink::LocalFrame *frame, const blink::WebDragData &data, blink::DragOperationsMask ops,
			    const SkBitmap &, const gfx::Vector2d &, const gfx::Rect & ) override;

	/* what the page does not take is not opened as a page of its own */
	bool AcceptsLoadDrops() const override { return false; }

	/*
	 * The screen (screen.width and height, colorDepth, the media
	 * queries of the device's size): the one screen, its size asked of
	 * the window manager each time, at 1 pixel to a CSS pixel
	 */
	const display::ScreenInfo &GetScreenInfo( blink::LocalFrame &f ) const override
	{
		return GetScreenInfos(f).current();
	}
	const display::ScreenInfo &GetOriginalScreenInfo( blink::LocalFrame &f ) const override
	{
		return GetScreenInfos(f).current();
	}
	const display::ScreenInfos &GetScreenInfos( blink::LocalFrame & ) const override
	{
		int w = 0, h = 0;
		br_screen_size(&w, &h);
		const gfx::Rect &r = screens_.current().rect;
		if ( w > 0 && h > 0 && ( r.width() != w || r.height() != h ) ) {
			display::ScreenInfo s;
			s.rect = gfx::Rect(0, 0, w, h);
			s.available_rect = s.rect;
			s.depth = 24;
			s.depth_per_component = 8;
			s.is_primary = true;
			s.orientation_type = w >= h ? display::mojom::ScreenOrientation::kLandscapePrimary
						    : display::mojom::ScreenOrientation::kPortraitPrimary;
			screens_ = display::ScreenInfos(s);
		}
		return screens_;
	}

	/* <input type="file">: asked from a task of its own after the click that opened it */
	void OpenFileChooser( blink::LocalFrame *frame, scoped_refptr<blink::FileChooser> chooser ) override
	{
		if ( frame == nullptr || chooser == nullptr ) return;
		frame->GetTaskRunner(blink::TaskType::kUserInteraction)
			->PostTask(FROM_HERE, base::BindOnce(&ask_files, std::move(chooser)));
	}

private:
	BrPage	*page_;
	mutable display::ScreenInfos screens_{ display::ScreenInfo() };
};

/* An iframe's commit (or move within its document) in the joint session history, and the iframe gone from it (after BlinkEngine) */
void page_frame_committed( BrPage *p, blink::LocalFrame *frame, blink::HistoryItem *item,
			   blink::WebHistoryCommitType type );
void page_frame_gone( BrPage *p, blink::LocalFrame *frame );
/* Set while a document is committed in a frame: the iframes of the one it replaces leave with it */
int g_doc_replacing = 0;

/*
 * What a frame's client does in every frame of the page, the main one and
 * those of its iframes
 */
class BrFrameClientBase : public blink::EmptyLocalFrameClient {
public:
	explicit BrFrameClientBase( BrPage *p ) : page_(p) {}

	blink::String UserAgent() override { return blink::String(br_user_agent()); }
	std::optional<blink::UserAgentMetadata> UserAgentMetadata() override { return br_user_agent_metadata(); }

	/* the spelling of what is typed into the frame's fields (port/spell/spell_tessronos.cc) */
	blink::WebTextCheckClient *GetTextCheckerClient() const override { return br_spell_client(); }

	/*
	 * What a load tells of itself as it goes (its response, its size),
	 * told to no one here; a loader of a URLLoaderFactory (blob: URLs)
	 * needs the wrapper all the same
	 */
	std::unique_ptr<blink::ResourceLoadInfoNotifierWrapper> CreateResourceLoadInfoNotifierWrapper() override
	{
		return std::make_unique<blink::ResourceLoadInfoNotifierWrapper>(
			base::WeakPtr<blink::WeakWrapperResourceLoadInfoNotifier>());
	}

	/* An iframe's frame, in this process, loaded by its own client (BrChildFrameClient) */
	blink::LocalFrame *CreateFrame( const blink::AtomicString &name, blink::HTMLFrameOwnerElement *owner ) override;

	/* the player of <video> and <audio> (port/media) */
	std::unique_ptr<blink::WebMediaPlayer> CreateWebMediaPlayer(
		blink::HTMLMediaElement &element, const blink::WebMediaPlayerSource &,
		blink::WebMediaPlayerClient *client ) override
	{
		return br_media_player(element, client);
	}

	/*
	 * the loads of the frame's document, which the service worker that
	 * controls it is asked of first, held to the document's
	 * Cross-Origin-Embedder-Policy
	 */
	std::unique_ptr<blink::URLLoader> CreateURLLoaderForTesting() override
	{
		network::mojom::CrossOriginEmbedderPolicyValue coep = network::mojom::CrossOriginEmbedderPolicyValue::kNone;
		if ( doc_frame_ && doc_frame_->DomWindow() != nullptr && doc_frame_->DomWindow()->GetPolicyContainer() != nullptr ) {
			coep = doc_frame_->DomWindow()->GetPolicyContainer()->GetPolicies().cross_origin_embedder_policy.value;
		}
		return br_blink_frame_url_loader(sw_client_, coep);
	}

	/* The loads of a dedicated worker the page starts (port/blink/worker_tessronos.cc) */
	/* navigator.serviceWorker: the registrations and workers kept in this process */
	std::unique_ptr<blink::WebServiceWorkerProvider> CreateServiceWorkerProvider() override
	{
		return br_sw_provider(sw_client_);
	}

	/*
	 * The frame's document made: a client of the service workers, and
	 * the one before it no longer ('uuid': the client a navigation's
	 * fetch event named, when there was one)
	 */
	void SwDocument( blink::LocalFrame *frame, const std::string &url, const std::string &uuid )
	{
		doc_frame_ = frame;
		std::string parent;
		if ( blink::Frame *p = frame->Tree().Parent() ) {
			auto *lp = blink::DynamicTo<blink::LocalFrame>(p);
			if ( lp != nullptr && lp->Client() != nullptr ) {
				parent = static_cast<BrFrameClientBase *>(lp->Client())->sw_client_;
			}
		}
		br_sw_forget(sw_client_);
		sw_client_ = br_sw_document(url, frame->IsMainFrame(), parent, uuid);
	}

	/* The frame gone: its document no longer a client */
	void Detached( blink::FrameDetachType ) override
	{
		br_sw_forget(sw_client_);
		sw_client_.clear();
	}

	scoped_refptr<blink::WebWorkerFetchContext> CreateWorkerFetchContext(
		blink::WebDedicatedWorkerHostFactoryClient * ) override
	{
		return br_worker_fetch_context();
	}

	/* a worklet's on the page's thread (a CSS paint worklet's): its loads as a worker's are */
	scoped_refptr<blink::WebWorkerFetchContext> CreateWorkletFetchContext() override
	{
		return br_worker_fetch_context();
	}

	/* The frame's associated interfaces: the store of its blob: URLs among them */
	blink::AssociatedInterfaceProvider *GetRemoteNavigationAssociatedInterfaces() override
	{
		if ( !interfaces_ ) {
			interfaces_ = std::make_unique<blink::AssociatedInterfaceProvider>(
				base::SingleThreadTaskRunner::GetCurrentDefault());
			br_blob_frame_interfaces(interfaces_.get());
			br_services_frame_interfaces(interfaces_.get());
			br_devices_frame_interfaces(interfaces_.get());
		}
		return interfaces_.get();
	}

	/*
	 * Each window's script in the main world as soon as its context is
	 * made, before the members that depend on the context (those of a
	 * secure context) and the runtime-enabled ones are installed:
	 * window.chrome, which then comes in the global's properties where it
	 * comes in a browser's
	 */
	void DidCreateScriptContext( v8::Local<v8::Context> context, int32_t world_id ) override
	{
		if ( world_id == 0 ) {
			install_chrome(v8::Isolate::GetCurrent(), context);
			br_ext_page_context(context);
		} else {
			br_ext_world_context(context, world_id);
		}
	}

	/* The moments an extension's content script can name */
	void RunScriptsAtDocumentElementAvailable() override { br_ext_moment(OwnFrame(), 0); }
	void RunScriptsAtDocumentReady( bool ) override { br_ext_moment(OwnFrame(), 1); }
	void RunScriptsAtDocumentIdle() override { br_ext_moment(OwnFrame(), 2); }
	blink::LocalFrame *OwnFrame();

	/*
	 * Each window's script in the main world, once its global is whole
	 * (the members of a secure context installed too): the gamepads'
	 * supplement, and the windowing controls of an origin that acts as an
	 * isolated web app
	 */
	void DispatchDidClearWindowObjectInMainWorld( blink::LocalDOMWindow *window ) override;

	/*
	 * The joint session history of the page, which the main frame's
	 * entries make: history.length, and history.back(), forward() and
	 * go() from any of its frames (BlinkEngine::history_go)
	 */
	unsigned BackForwardLength() override;
	bool NavigateBackForward( int offset, base::TimeTicks,
				  std::optional<blink::scheduler::TaskAttributionId> ) const override;

	/*
	 * The passwords of login forms, as a browser's password manager keeps
	 * them: a form sent with a password is offered to be kept for the
	 * origin, and a field of such a form taking the focus is offered the
	 * user name and password kept (port/blink/credentials_tessronos.cc);
	 * both are asked on a panel
	 */
	void DispatchWillSendSubmitEvent( blink::HTMLFormElement *form ) override;
	void FocusedElementChanged( blink::Element *element ) override;

	void Trace( blink::Visitor *v ) const override
	{
		v->Trace(offered_);
		v->Trace(filled_);
		v->Trace(doc_frame_);
		blink::EmptyLocalFrameClient::Trace(v);
	}

protected:
	BrPage	*page_;
	std::unique_ptr<blink::AssociatedInterfaceProvider>	interfaces_;
	blink::WeakMember<blink::Document>	filled_;	/* the document whose form was offered an address or a card */
	blink::WeakMember<blink::Document>	offered_;	/* the document whose fields were offered a password */
	blink::WeakMember<blink::LocalFrame>	doc_frame_;	/* the frame, once its first document is made */
	std::string	sw_client_;	/* the frame's document as a client of the service workers, its UUID */
	std::string	sw_next_;	/* the client a navigation under way named for its document (BrChildFrameClient) */
};

/* The main frame's client: its navigations go to the application, which fetches the page */
class BrFrameClient final : public BrFrameClientBase {
public:
	explicit BrFrameClient( BrPage *p ) : BrFrameClientBase(p) {}

	void BeginNavigation(
		const blink::ResourceRequest &request, const blink::KURL &,
		blink::mojom::RequestContextFrameType, blink::LocalDOMWindow *,
		blink::DocumentLoader *, blink::WebNavigationType, blink::NavigationPolicy,
		blink::WebFrameLoadType, blink::mojom::blink::ForceHistoryPush, bool, bool,
		blink::mojom::blink::TriggeringEventInfo, blink::HTMLFormElement *,
		network::mojom::CSPDisposition, mojo::PendingRemote<blink::mojom::blink::BlobURLToken>,
		base::TimeTicks, base::TimeTicks, const blink::String &,
		const blink::LocalFrameToken *, const base::UnguessableToken &,
		const blink::DocumentToken &, blink::SourceLocation *,
		mojo::PendingRemote<blink::mojom::blink::NavigationStateKeepAliveHandle>,
		bool, bool,
		mojo::PendingReceiver<blink::mojom::blink::NavigationResumeDeferredCommitListener>,
		std::optional<base::UnguessableToken> ) override;

	/*
	 * A document committed in the main frame, and a move within the
	 * document shown (history.pushState and replaceState, a fragment, an
	 * entry of the same document gone back to): the session history
	 * follows, and with it the address the window shows
	 */
	void DispatchDidCommitLoad( blink::HistoryItem *item, blink::WebHistoryCommitType type, bool,
				    const network::ParsedPermissionsPolicy &,
				    const blink::DocumentPolicyFeatureState & ) override;
	void DidFinishSameDocumentNavigation( blink::WebHistoryCommitType type, bool,
					      blink::mojom::blink::SameDocumentNavigationType, bool, bool, bool,
					      base::UnguessableToken, bool ) override;
};

/*
 * An iframe's client. Its navigations are made here, in the engine: the
 * document is fetched through the network stack (or made from the URL:
 * about:blank, about:srcdoc, data:) and committed into the frame, which
 * gets its view then. The frame is drawn, laid out and given input as a
 * part of the page's main frame, as a frame in the same process is.
 */
class BrChildFrameClient final : public BrFrameClientBase {
public:
	explicit BrChildFrameClient( BrPage *p ) : BrFrameClientBase(p) {}

	void SetFrame( blink::LocalFrame *f ) { frame_ = f; }

	void BeginNavigation(
		const blink::ResourceRequest &request, const blink::KURL &,
		blink::mojom::RequestContextFrameType, blink::LocalDOMWindow *,
		blink::DocumentLoader *, blink::WebNavigationType, blink::NavigationPolicy,
		blink::WebFrameLoadType, blink::mojom::blink::ForceHistoryPush, bool, bool,
		blink::mojom::blink::TriggeringEventInfo, blink::HTMLFormElement *,
		network::mojom::CSPDisposition, mojo::PendingRemote<blink::mojom::blink::BlobURLToken>,
		base::TimeTicks, base::TimeTicks, const blink::String &,
		const blink::LocalFrameToken *, const base::UnguessableToken &,
		const blink::DocumentToken &, blink::SourceLocation *,
		mojo::PendingRemote<blink::mojom::blink::NavigationStateKeepAliveHandle>,
		bool, bool,
		mojo::PendingReceiver<blink::mojom::blink::NavigationResumeDeferredCommitListener>,
		std::optional<base::UnguessableToken> ) override;

	/* the document committed: the frame's view, which its owner's layout sizes */
	void TransitionToCommittedForNewPage() override
	{
		if ( frame_ == nullptr || frame_->GetPage() == nullptr ) return;
		blink::LocalFrame *top = blink::DynamicTo<blink::LocalFrame>(&frame_->Tree().Top());
		gfx::Size size = ( top != nullptr && top->View() != nullptr ) ? top->View()->Size() : gfx::Size();
		frame_->CreateView(size, blink::Color::kTransparent);
	}

	/*
	 * A frame taken away leaves its parent's tree as well: one left there,
	 * its client gone, would be met by a look for the frames by name
	 * (window[name]) and taken for one that is there
	 */
	void Detached( blink::FrameDetachType type ) override
	{
		/* its entries leave the session history with it */
		if ( frame_ != nullptr ) page_frame_gone(page_, frame_);
		if ( type == blink::FrameDetachType::kRemove && frame_ != nullptr ) frame_->DetachFromParent();
		frame_ = nullptr;
		nav_seq_++;
		BrFrameClientBase::Detached(type);
	}

	/* An answer for the navigation started last, still wanted (its status and header lines when it came over the network) */
	void Commit( unsigned seq, const std::string &url, const std::string &ctype, const char *data, size_t len,
		     int status = 0, const std::string &headers = std::string(),
		     const std::string &protocol = std::string() );
	unsigned nav_seq() const { return nav_seq_; }
	/* The redirects the answer of the navigation under way followed, and when it came: told before it is committed */
	void Answered( std::vector<BrNavRedirect> redirects )
	{
		nav_redirects_ = std::move(redirects);
		nav_answered_ = base::TimeTicks::Now();
	}

	/*
	 * A document committed in the frame, and a move within the document
	 * shown (a fragment, history.pushState): the page's joint session
	 * history follows
	 */
	void DispatchDidCommitLoad( blink::HistoryItem *item, blink::WebHistoryCommitType type, bool,
				    const network::ParsedPermissionsPolicy &,
				    const blink::DocumentPolicyFeatureState & ) override;
	void DidFinishSameDocumentNavigation( blink::WebHistoryCommitType type, bool,
					      blink::mojom::blink::SameDocumentNavigationType, bool, bool, bool,
					      base::UnguessableToken, bool ) override;
	/*
	 * To an entry of the session history: the frame's document of 'item'
	 * fetched again and committed as that entry (its state object, its
	 * scroll)
	 */
	void GoToEntry( blink::HistoryItem *item );
	/* The document of a navigation fetched through the network stack, committed when it comes */
	void NetFetch( unsigned seq, const std::string &u, const std::string &method, const std::string &send,
		       const std::string &hdrs );

	void Trace( blink::Visitor *v ) const override
	{
		v->Trace(frame_);
		v->Trace(nav_item_);
		v->Trace(going_item_);
		BrFrameClientBase::Trace(v);
	}

private:
	blink::Member<blink::LocalFrame>	frame_;
	unsigned				nav_seq_ = 0;
	/*
	 * How the navigation under way commits (a new entry, the entry shown
	 * taken over or loaded again, an entry gone back or forward to, whose
	 * HistoryItem is given back), who sent it there (its referrer) and its
	 * times: begun, its request sent, its answer come, and the redirects
	 * the answer followed
	 */
	blink::WebFrameLoadType			nav_type_ = blink::WebFrameLoadType::kStandard;
	blink::Member<blink::HistoryItem>	nav_item_;
	blink::Member<blink::HistoryItem>	going_item_;	/* the entry GoToEntry asks the navigation for */
	blink::String				nav_referrer_;
	base::TimeTicks				nav_began_, nav_sent_, nav_answered_;
	std::vector<BrNavRedirect>		nav_redirects_;
	/*
	 * The origin of the document that started the navigation under way:
	 * the document committed is told it (its requestor), so that an
	 * about:blank or about:srcdoc a document navigated to takes that
	 * document's origin, as a browser commits it
	 */
	scoped_refptr<const blink::SecurityOrigin>	requestor_;
	blink::KURL					fallback_base_;	/* an about:blank or about:srcdoc document's base URL */
	bool						sync_blank_ = false;	/* the commit under way is the synchronous about:blank */
};


/* Whether a window's origin is one the user lets act as an isolated web app */
bool iwa_window( const blink::LocalDOMWindow &window )
{
	const blink::SecurityOrigin *o = window.GetSecurityOrigin();
	return o != nullptr && !o->IsOpaque() && br_isolated_allowed(o->ToString().Utf8());
}

/* The origin of the main frame's document, for the services' threads (br_page_origin) */
std::mutex	g_origin_lock;
std::string	g_origin;

/* Whether a page is the window the application opened (BrPage is below) */
bool page_primary( const BrPage *p );

void BrFrameClientBase::DispatchDidClearWindowObjectInMainWorld( blink::LocalDOMWindow *window )
{
	if ( window != nullptr && window->GetFrame() != nullptr && window->GetFrame()->IsMainFrame()
	     && page_primary(page_) ) {
		std::lock_guard<std::mutex> hold(g_origin_lock);
		g_origin = window->GetSecurityOrigin()->ToString().Utf8();
	}
	/* the frame's document, the page's or an iframe's: a service worker of its scope controls it */
	if ( window != nullptr && window->GetFrame() != nullptr ) {
		std::string next;
		next.swap(sw_next_);
		SwDocument(window->GetFrame(), window->Url().GetString().Utf8(), next);
	}
	blink::ScriptState *ss = ( window != nullptr && window->GetFrame() != nullptr )
				 ? blink::ToScriptStateForMainWorld(window->GetFrame()) : nullptr;
	if ( ss == nullptr || !ss->ContextIsValid() ) return;
	/*
	 * the gamepads' supplement made with each window, as a browser's
	 * frame client has the modules make it: a gamepadconnected listener
	 * added before the page first asks for the gamepads is then seen
	 */
	blink::NavigatorGamepad::From(*window->navigator());
	if ( iwa_window(*window) ) {
		printf("browser: an isolated web app's window: windowing controls %d\n",
		       blink::RuntimeEnabledFeatures::DesktopPWAsAdditionalWindowingControlsEnabled(window) ? 1 : 0);
	}
}

/* ---------------------------------------------------------------- window.chrome */

/*
 * window.chrome, as Chrome gives it to every page: loadTimes() and csi(),
 * the times of the document's load as Chrome's renderer reports them
 * (from the window's performance timing and the response), and app, the
 * API of hosted apps, which on a page that is no installed app answers
 * that nothing is installed. Its functions are native functions, as
 * Chrome's are, and the objects they answer have native data properties.
 */
namespace {

blink::LocalDOMWindow *current_window( v8::Isolate *iso )
{
	v8::Local<v8::Context> c = iso->GetCurrentContext();
	return c.IsEmpty() ? nullptr : blink::ToLocalDOMWindow(c);
}

void value_getter( v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value> &info )
{
	info.GetReturnValue().Set(info.Data());
}

/* the property stays a native data property whatever is written to it */
void empty_setter( v8::Local<v8::Name>, v8::Local<v8::Value>, const v8::PropertyCallbackInfo<v8::Boolean> & ) {}

bool native_prop( v8::Isolate *iso, v8::Local<v8::Context> ctx, v8::Local<v8::Object> o, const char *name,
		  v8::Local<v8::Value> v )
{
	v8::Local<v8::String> n = v8::String::NewFromUtf8(iso, name, v8::NewStringType::kInternalized).ToLocalChecked();
	return o->SetNativeDataProperty(ctx, n, value_getter, empty_setter, v).FromMaybe(false);
}

v8::Local<v8::String> v8_str( v8::Isolate *iso, const std::string &s )
{
	return v8::String::NewFromUtf8(iso, s.data(), v8::NewStringType::kNormal, (int)s.size()).ToLocalChecked();
}

/* The navigation's type as chrome.loadTimes() names it, and as chrome.csi() numbers it */
const char *load_times_type( blink::WebNavigationType t )
{
	switch ( t ) {
	case blink::kWebNavigationTypeLinkClicked:	return "LinkClicked";
	case blink::kWebNavigationTypeFormSubmitted:	return "FormSubmitted";
	case blink::kWebNavigationTypeBackForward:
	case blink::kWebNavigationTypeRestore:		return "BackForward";
	case blink::kWebNavigationTypeReload:		return "Reload";
	case blink::kWebNavigationTypeFormResubmittedBackForward:
	case blink::kWebNavigationTypeFormResubmittedReload:	return "Resubmitted";
	default:					return "Other";
	}
}

int csi_transition( blink::WebNavigationType t )
{
	switch ( t ) {
	case blink::kWebNavigationTypeLinkClicked:
	case blink::kWebNavigationTypeFormSubmitted:
	case blink::kWebNavigationTypeFormResubmittedBackForward:
	case blink::kWebNavigationTypeFormResubmittedReload:	return 0;
	case blink::kWebNavigationTypeBackForward:
	case blink::kWebNavigationTypeRestore:		return 6;
	case blink::kWebNavigationTypeReload:		return 16;
	default:					return 15;
	}
}

void chrome_load_times( const v8::FunctionCallbackInfo<v8::Value> &info )
{
	v8::Isolate *iso = info.GetIsolate();
	blink::LocalDOMWindow *w = current_window(iso);
	blink::DocumentLoader *dl = ( w != nullptr && w->GetFrame() != nullptr )
				    ? w->GetFrame()->Loader().GetDocumentLoader() : nullptr;
	if ( dl == nullptr ) {
		info.GetReturnValue().SetNull();
		return;
	}
	blink::WindowPerformance *perf = blink::DOMWindowPerformance::performance(*w);
	blink::PerformanceTiming *t = perf->timing();
	double nav = (double)t->navigationStart() / 1000.0;
	double first_paint = (double)perf->timingForReporting()->FirstPaintForMetrics() / 1000.0;
	const blink::ResourceResponse &r = dl->GetResponse();
	std::string type = load_times_type(dl->GetNavigationType());
	std::string alpn = r.AlpnNegotiatedProtocol().Utf8();
	std::string conn(net::HttpConnectionInfoToString(r.ConnectionInfo()));
	double commit = (double)t->responseStart() / 1000.0, dcl = (double)t->domContentLoadedEventEnd() / 1000.0;
	double load = (double)t->loadEventEnd() / 1000.0;

	v8::Local<v8::Context> ctx = iso->GetCurrentContext();
	v8::Local<v8::Object> o = v8::Object::New(iso);
	bool ok = native_prop(iso, ctx, o, "requestTime", v8::Number::New(iso, nav))
		  && native_prop(iso, ctx, o, "startLoadTime", v8::Number::New(iso, nav))
		  && native_prop(iso, ctx, o, "commitLoadTime", v8::Number::New(iso, commit))
		  && native_prop(iso, ctx, o, "finishDocumentLoadTime", v8::Number::New(iso, dcl))
		  && native_prop(iso, ctx, o, "finishLoadTime", v8::Number::New(iso, load))
		  && native_prop(iso, ctx, o, "firstPaintTime", v8::Number::New(iso, first_paint))
		  && native_prop(iso, ctx, o, "firstPaintAfterLoadTime", v8::Number::New(iso, 0.0))
		  && native_prop(iso, ctx, o, "navigationType", v8_str(iso, type))
		  && native_prop(iso, ctx, o, "wasFetchedViaSpdy", v8::Boolean::New(iso, r.WasFetchedViaSPDY()))
		  && native_prop(iso, ctx, o, "wasNpnNegotiated", v8::Boolean::New(iso, r.WasAlpnNegotiated()))
		  && native_prop(iso, ctx, o, "npnNegotiatedProtocol", v8_str(iso, alpn))
		  && native_prop(iso, ctx, o, "wasAlternateProtocolAvailable",
				 v8::Boolean::New(iso, r.WasAlternateProtocolAvailable()))
		  && native_prop(iso, ctx, o, "connectionInfo", v8_str(iso, conn));
	if ( ok ) {
		info.GetReturnValue().Set(o);
	} else {
		info.GetReturnValue().SetNull();
	}
}

void chrome_csi( const v8::FunctionCallbackInfo<v8::Value> &info )
{
	v8::Isolate *iso = info.GetIsolate();
	blink::LocalDOMWindow *w = current_window(iso);
	blink::DocumentLoader *dl = ( w != nullptr && w->GetFrame() != nullptr )
				    ? w->GetFrame()->Loader().GetDocumentLoader() : nullptr;
	if ( dl == nullptr ) {
		info.GetReturnValue().SetNull();
		return;
	}
	blink::PerformanceTiming *t = blink::DOMWindowPerformance::performance(*w)->timing();
	double start = (double)t->navigationStart();
	double now = base::Time::Now().InMillisecondsFSinceUnixEpoch();
	v8::Local<v8::Context> ctx = iso->GetCurrentContext();
	v8::Local<v8::Object> o = v8::Object::New(iso);
	bool ok = native_prop(iso, ctx, o, "startE", v8::Number::New(iso, start))
		  && native_prop(iso, ctx, o, "onloadT", v8::Number::New(iso, (double)t->domContentLoadedEventEnd()))
		  && native_prop(iso, ctx, o, "pageT", v8::Number::New(iso, now - start))
		  && native_prop(iso, ctx, o, "tran", v8::Number::New(iso, csi_transition(dl->GetNavigationType())));
	if ( ok ) {
		info.GetReturnValue().Set(o);
	} else {
		info.GetReturnValue().SetNull();
	}
}

/* chrome.app on a page that is not an installed app */
void app_is_installed( const v8::FunctionCallbackInfo<v8::Value> &info )
{
	info.GetReturnValue().Set(false);
}

void app_details( const v8::FunctionCallbackInfo<v8::Value> &info )
{
	info.GetReturnValue().SetNull();
}

void app_running_state( const v8::FunctionCallbackInfo<v8::Value> &info )
{
	info.GetReturnValue().Set(v8_str(info.GetIsolate(), "cannot_run"));
}

/* installState(callback): the callback told "not_installed" later, as the answer comes from the browser */
void app_install_state_answer( const v8::FunctionCallbackInfo<v8::Value> &info )
{
	v8::Isolate *iso = info.GetIsolate();
	if ( !info.Data()->IsFunction() ) return;
	v8::Local<v8::Value> arg = v8_str(iso, "not_installed");
	(void)info.Data().As<v8::Function>()->Call(iso->GetCurrentContext(), v8::Undefined(iso), 1, &arg);
}

void app_install_state( const v8::FunctionCallbackInfo<v8::Value> &info )
{
	v8::Isolate *iso = info.GetIsolate();
	if ( info.Length() < 1 || !info[0]->IsFunction() ) {
		iso->ThrowException(v8::Exception::TypeError(
			v8_str(iso, "Error in invocation of app.installState(function callback): No matching signature.")));
		return;
	}
	v8::Local<v8::Function> answer;
	if ( v8::Function::New(iso->GetCurrentContext(), app_install_state_answer, info[0]).ToLocal(&answer) ) {
		iso->EnqueueMicrotask(answer);
	}
}

/*
 * A native function put on an object: named, as app's are, or nameless
 * with a prototype of its own, as loadTimes and csi are (Chrome makes
 * those two from function templates, which may be called with new)
 */
bool put_fn( v8::Isolate *iso, v8::Local<v8::Context> ctx, v8::Local<v8::Object> o, const char *name,
	     v8::FunctionCallback cb, bool named )
{
	v8::Local<v8::Function> f;
	if ( !v8::Function::New(ctx, cb, v8::Local<v8::Value>(), 0,
				named ? v8::ConstructorBehavior::kThrow : v8::ConstructorBehavior::kAllow).ToLocal(&f) ) {
		return false;
	}
	if ( named ) f->SetName(v8_str(iso, name));
	return o->Set(ctx, v8_str(iso, name), f).FromMaybe(false);
}

v8::Local<v8::Object> string_table( v8::Isolate *iso, v8::Local<v8::Context> ctx,
				    std::initializer_list<std::pair<const char *, const char *>> kv )
{
	v8::Local<v8::Object> o = v8::Object::New(iso);
	for ( const auto &p : kv ) (void)o->Set(ctx, v8_str(iso, p.first), v8_str(iso, p.second));
	return o;
}

}  // namespace

void install_chrome( v8::Isolate *iso, v8::Local<v8::Context> context )
{
	if ( iso == nullptr || context.IsEmpty() ) return;
	v8::HandleScope		hs(iso);
	v8::Context::Scope	cs(context);
	v8::Local<v8::Object>	global = context->Global();
	v8::Local<v8::String>	name = v8_str(iso, "chrome");
	v8::Local<v8::Value>	have;
	v8::Local<v8::Object>	chrome;

	if ( global->Get(context, name).ToLocal(&have) && have->IsObject() ) {
		chrome = have.As<v8::Object>();
	} else {
		chrome = v8::Object::New(iso);
		if ( !global->Set(context, name, chrome).FromMaybe(false) ) return;
	}
	(void)put_fn(iso, context, chrome, "loadTimes", chrome_load_times, false);
	(void)put_fn(iso, context, chrome, "csi", chrome_csi, false);

	v8::Local<v8::Object> app = v8::Object::New(iso);
	(void)app->Set(context, v8_str(iso, "isInstalled"), v8::False(iso));
	(void)put_fn(iso, context, app, "getDetails", app_details, true);
	(void)put_fn(iso, context, app, "getIsInstalled", app_is_installed, true);
	(void)put_fn(iso, context, app, "installState", app_install_state, true);
	(void)put_fn(iso, context, app, "runningState", app_running_state, true);
	(void)app->Set(context, v8_str(iso, "InstallState"),
		       string_table(iso, context, { { "DISABLED", "disabled" }, { "INSTALLED", "installed" },
						    { "NOT_INSTALLED", "not_installed" } }));
	(void)app->Set(context, v8_str(iso, "RunningState"),
		       string_table(iso, context, { { "CANNOT_RUN", "cannot_run" }, { "READY_TO_RUN", "ready_to_run" },
						    { "RUNNING", "running" } }));
	(void)chrome->Set(context, v8_str(iso, "app"), app);
}


/* A page: its Page, its frame and what the application is to be told */
struct BrPage {
	blink::Persistent<blink::Page>		page;
	blink::Persistent<blink::LocalFrame>	frame;
	blink::Persistent<blink::AgentGroupScheduler>	agents;
	blink::Persistent<BrChromeClient>	chrome;
	blink::Persistent<BrFrameClient>	client;
	bool			wants_frame = false;	/* something to paint anew */
	/* the callbacks told when the next frame is on the screen (NotifyPresentationTime) */
	std::vector<blink::ChromeClient::ReportTimeCallback>	presented;
	std::string		go_to;			/* a navigation the page asked for */
	std::string		go_method, go_ctype, go_body;	/* and a form's, sent with POST */
	blink::Persistent<blink::DataObject>	drag;		/* what the page drags, null when nothing */
	blink::DragOperationsMask		drag_ops = blink::kDragOperationNone;
	blink::DragController::Operation	drag_op;	/* what the place under the pointer takes */
	bool			primary = true;		/* the window the application opened, not one a page opened */
	bool			close_asked = false;	/* window.close(), or the opener's page went (picture-in-picture) */
	void			*engine = nullptr;	/* the BlinkEngine that has it */
	base::UnguessableToken	group;			/* its browsing context group, which the windows it opens share */
	std::vector<std::string>	apart;		/* addresses to open in browsers of their own */
	blink::WebFrameLoadType	go_type = blink::WebFrameLoadType::kStandard;	/* how go_to commits */
	bool			left_asked = false;	/* go_to's beforeunload was asked when it began */
	std::string		hover_link, tooltip;	/* under the pointer: a link's address, a title */
	int			fs_ask = 0;		/* full screen asked for (BR_FS_*), not yet seen */
	int			win_ask = 0;		/* the window asked of (BR_WIN_*), not yet seen */
	/* the monitor of the thread's tasks that reports the frames' long tasks (BrLongTasks) */
	blink::Persistent<blink::AnimationFrameTimingMonitor>	timing;
	std::unique_ptr<blink::AnimationFrameTimingMonitor::Client>	timing_client;
};

/*
 * The long tasks and the long animation frames of a page's frames, as a
 * frame widget's monitor reports them: a task of the page's thread that
 * took 50 ms or more and ran a frame's script is told to the performance
 * timeline of each frame of the page (PerformanceLongTaskTiming). The
 * frames of the page are timed as a widget times them: from the task that
 * asked for the frame, through the frame's beginning (begin_frame), its
 * style and layout (update) and its paint, to when it was rendered; one of
 * 50 ms or more, or a task as long that rendered nothing, is a long
 * animation frame (PerformanceLongAnimationFrameTiming), given to the
 * frame's timeline when the frame is shown. The frames' tasks reach the
 * monitor, and the end of the rendering is taken from it, through
 * g_frame_timing_monitor_hook (timing_of).
 */
class BrLongTasks final : public blink::AnimationFrameTimingMonitor::Client {
public:
	explicit BrLongTasks( BrPage *p ) : page_(p) {}

	void ReportLongTaskTiming( base::TimeTicks start, base::TimeTicks end, blink::ExecutionContext *context ) override
	{
		if ( page_->frame == nullptr ) return;
		for ( blink::Frame *f = page_->frame.Get(); f != nullptr; f = f->Tree().TraverseNext() ) {
			auto *lf = blink::DynamicTo<blink::LocalFrame>(f);
			if ( lf == nullptr || lf->DomWindow() == nullptr ) continue;
			blink::DOMWindowPerformance::performance(*lf->DomWindow())
				->ReportLongTask(start, end, context, /*has_multiple_contexts=*/false);
		}
	}
	bool ShouldReportLongAnimationFrameTiming() const override { return &blink::g_frame_timing_monitor_hook != nullptr; }
	bool RequestedMainFramePending() override { return page_->wants_frame; }
	ukm::UkmRecorder *MainFrameUkmRecorder() override { return nullptr; }
	ukm::SourceId MainFrameUkmSourceId() override { return ukm::kInvalidSourceId; }

private:
	BrPage	*page_;
};

/* The session history of a page's engine, for its frames' clients (after BlinkEngine) */
void page_committed( BrPage *p, blink::HistoryItem *item, blink::WebHistoryCommitType type );
bool page_history_go( BrPage *p, int offset );
unsigned page_history_length( BrPage *p );

void BrChildFrameClient::DispatchDidCommitLoad( blink::HistoryItem *item, blink::WebHistoryCommitType type, bool,
						const network::ParsedPermissionsPolicy &,
						const blink::DocumentPolicyFeatureState & )
{
	if ( frame_ != nullptr ) page_frame_committed(page_, frame_, item, type);
}

void BrChildFrameClient::DidFinishSameDocumentNavigation( blink::WebHistoryCommitType type, bool,
							  blink::mojom::blink::SameDocumentNavigationType, bool, bool,
							  bool, base::UnguessableToken, bool )
{
	blink::DocumentLoader *dl = frame_ != nullptr ? frame_->Loader().GetDocumentLoader() : nullptr;
	if ( dl != nullptr ) page_frame_committed(page_, frame_, dl->GetHistoryItem(), type);
}

/*
 * A new window that keeps its opener: a page of its own in this engine,
 * in the opener's browsing context group and agents, so that the
 * opener's script reaches its document (defined after BlinkEngine)
 */
blink::Page *br_open_window( BrPage *opener, blink::LocalFrame *opener_frame, const blink::FrameLoadRequest &req,
			     const blink::WebWindowFeatures &features );

bool page_primary( const BrPage *p )
{
	return p->primary;
}

/* The frame this client is of, found in its page's tree: the client is told nothing of it */
blink::LocalFrame *BrFrameClientBase::OwnFrame()
{
	if ( page_ == nullptr || !page_->page ) return nullptr;
	for ( blink::Frame *f = page_->page->MainFrame(); f != nullptr; f = f->Tree().TraverseNext() ) {
		blink::LocalFrame *l = blink::DynamicTo<blink::LocalFrame>(f);
		if ( l != nullptr && l->Client() == this ) return l;
	}
	return nullptr;
}

void BrChromeClient::StartDragging( blink::LocalFrame *frame, const blink::WebDragData &data,
				    blink::DragOperationsMask ops, const SkBitmap &, const gfx::Vector2d &,
				    const gfx::Rect & )
{
	page_->drag = blink::DataObject::Create(frame != nullptr ? frame->DomWindow() : nullptr, data);
	page_->drag_ops = ops;
	page_->drag_op = blink::DragController::Operation();
	printf("browser: the page drags %u items\n", (unsigned)page_->drag->length());
}

void BrChromeClient::ScheduleAnimation( const blink::LocalFrameView *, cc::BeginMainFrameReason,
					base::TimeDelta, bool )
{
	page_->wants_frame = true;
}

void BrChromeClient::NotifyPresentationTime( blink::LocalFrame &, ReportTimeCallback callback )
{
	page_->presented.push_back(std::move(callback));
	page_->wants_frame = true;
}

blink::Page *BrChromeClient::CreateWindowDelegate( blink::LocalFrame *opener,
						   const blink::FrameLoadRequest &req,
						   const blink::AtomicString &, const blink::WebWindowFeatures &features,
						   network::mojom::blink::WebSandboxFlags,
						   const blink::SessionStorageNamespaceId &, bool & )
{
	const blink::ResourceRequest &r = req.GetResourceRequest();
	bool pip = req.GetPictureInPictureWindowOptions().has_value();
	if ( !pip && !blink::LocalFrame::HasTransientUserActivation(opener) ) {
		printf("browser: a window asked for with no click, not opened: %s\n",
		       r.Url().GetString().Utf8().c_str());
		return nullptr;
	}
	if ( !pip && features.noopener ) {
		/* no script reaches it: a browser of its own (window.open answers null) */
		std::string to = r.Url().IsValid() ? r.Url().GetString().Utf8() : std::string("about:blank");
		page_->apart.push_back(to);
		printf("browser: a new window asked for with no opener: %s, in a browser of its own\n", to.c_str());
		return nullptr;
	}
	return br_open_window(page_, opener, req, features);
}

void BrChromeClient::CloseWindow()
{
	page_->close_asked = true;
}

gfx::Rect BrChromeClient::RootWindowRect( blink::LocalFrame &f )
{
	int x = 0, y = 0, w = 0, h = 0;
	if ( page_->primary && br_window_rect(&x, &y, &w, &h) ) return gfx::Rect(x, y, w, h);
	blink::LocalFrameView *v = f.LocalFrameRoot().View();
	return v != nullptr ? gfx::Rect(v->Size()) : gfx::Rect();
}

void BrChromeClient::ShowMouseOverURL( const blink::HitTestResult &r )
{
	blink::KURL u = r.AbsoluteLinkURL();
	page_->hover_link = u.IsValid() && !u.IsEmpty() ? u.GetString().Utf8() : std::string();
}

void BrChromeClient::UpdateTooltipUnderCursor( blink::LocalFrame &, const blink::String &text, blink::TextDirection )
{
	page_->tooltip = text.Utf8();
}

void BrChromeClient::EnterFullscreen( blink::LocalFrame &, const blink::FullscreenOptions *, blink::FullscreenRequestType )
{
	page_->fs_ask = BR_FS_ENTER;
}

void BrChromeClient::ExitFullscreen( blink::LocalFrame & )
{
	page_->fs_ask = BR_FS_EXIT;
}

void BrChromeClient::Minimize( blink::LocalFrame &, WindowingControlsChangeCallback cb )
{
	page_->win_ask = BR_WIN_MINIMIZE;
	std::move(cb).Run(true);
}

void BrChromeClient::Maximize( blink::LocalFrame &, WindowingControlsChangeCallback cb )
{
	page_->win_ask = BR_WIN_MAXIMIZE;
	std::move(cb).Run(true);
}

void BrChromeClient::Restore( blink::LocalFrame &, WindowingControlsChangeCallback cb )
{
	page_->win_ask = BR_WIN_RESTORE;
	std::move(cb).Run(true);
}

void BrChromeClient::SetResizable( bool resizable, blink::LocalFrame &, WindowingControlsChangeCallback cb )
{
	page_->win_ask = resizable ? BR_WIN_RESIZABLE : BR_WIN_FIXED;
	std::move(cb).Run(true);
}

void BrFrameClient::BeginNavigation(
	const blink::ResourceRequest &request, const blink::KURL &,
	blink::mojom::RequestContextFrameType, blink::LocalDOMWindow *,
	blink::DocumentLoader *, blink::WebNavigationType, blink::NavigationPolicy policy,
	blink::WebFrameLoadType load_type, blink::mojom::blink::ForceHistoryPush, bool, bool,
	blink::mojom::blink::TriggeringEventInfo, blink::HTMLFormElement *,
	network::mojom::CSPDisposition, mojo::PendingRemote<blink::mojom::blink::BlobURLToken>,
	base::TimeTicks, base::TimeTicks, const blink::String &,
	const blink::LocalFrameToken *, const base::UnguessableToken &,
	const blink::DocumentToken &, blink::SourceLocation *,
	mojo::PendingRemote<blink::mojom::blink::NavigationStateKeepAliveHandle>,
	bool, bool,
	mojo::PendingReceiver<blink::mojom::blink::NavigationResumeDeferredCommitListener>,
	std::optional<base::UnguessableToken> )
{
	/*
	 * A window a page opened goes to about:blank first: its initial empty
	 * document, which has its opener's origin, is that document already
	 * (the opener may be writing into it)
	 */
	if ( !page_->primary && request.Url().IsAboutBlankUrl() && page_->frame != nullptr
	     && page_->frame->GetDocument() != nullptr && page_->frame->GetDocument()->IsInitialEmptyDocument() ) {
		return;
	}
	/*
	 * A link opened with Ctrl or the middle button, or with Shift (a new
	 * tab or window in Chrome): a browser of its own, as a link to _blank
	 * with no opener is
	 */
	if ( policy == blink::kNavigationPolicyNewBackgroundTab || policy == blink::kNavigationPolicyNewForegroundTab
	     || policy == blink::kNavigationPolicyNewWindow || policy == blink::kNavigationPolicyNewPopup ) {
		std::string to = request.Url().GetString().Utf8();
		page_->apart.push_back(to);
		printf("browser: %s asked for in a new window, in a browser of its own\n", to.c_str());
		return;
	}
	/* leaving the document: its beforeunload first, as a renderer asks it before it tells the browser */
	if ( page_->frame != nullptr && page_->frame->GetDocument() != nullptr
	     && !page_->frame->GetDocument()->IsInitialEmptyDocument() ) {
		base::TimeTicks opened, closed;
		if ( !page_->frame->Loader().ShouldClose(load_type == blink::WebFrameLoadType::kReload, false, opened,
							  closed) ) {
			printf("browser: the page asked to stay: %s not gone to\n", request.Url().GetString().Utf8().c_str());
			return;
		}
	}
	page_->left_asked = true;
	page_->go_type = load_type;
	/* the application fetches it and loads it as it does an address */
	page_->go_to = request.Url().GetString().Utf8();
	/* a form sent with POST carries its method, its body and the body's type */
	page_->go_method = request.HttpMethod().GetString().Utf8();
	page_->go_ctype.clear();
	page_->go_body.clear();
	if ( request.HttpBody() != nullptr ) {
		/* the body's bytes as they are (the form's encoding's, not text) */
		blink::Vector<char> bytes;
		request.HttpBody()->Flatten(bytes);
		page_->go_body.assign(bytes.data(), bytes.size());
		page_->go_ctype = request.HttpContentType().GetString().Utf8();
	}
}

void BrFrameClient::DispatchDidCommitLoad( blink::HistoryItem *item, blink::WebHistoryCommitType type, bool,
					   const network::ParsedPermissionsPolicy &,
					   const blink::DocumentPolicyFeatureState & )
{
	/*
	 * the windowing controls, a trial of Chrome's, to the document of an
	 * origin that acts as an isolated web app: before its script runs,
	 * so that its global has them when it is made
	 */
	blink::LocalDOMWindow *w = page_->frame != nullptr ? page_->frame->DomWindow() : nullptr;
	if ( w != nullptr && iwa_window(*w) ) {
		w->GetOriginTrialContext()->AddForceEnabledTrials(
			blink::Vector<blink::String>{ "DesktopPWAsAdditionalWindowingControls" });
	}
	page_committed(page_, item, type);
}

void BrFrameClient::DidFinishSameDocumentNavigation( blink::WebHistoryCommitType type, bool,
						     blink::mojom::blink::SameDocumentNavigationType, bool, bool,
						     bool, base::UnguessableToken, bool )
{
	blink::DocumentLoader *dl = page_->frame != nullptr ? page_->frame->Loader().GetDocumentLoader() : nullptr;
	if ( dl != nullptr ) page_committed(page_, dl->GetHistoryItem(), type);
}

/* A login form's user name field and password field (none: null), the password's with a value when 'filled' */
void login_fields( blink::HTMLFormElement &form, blink::HTMLInputElement **user, blink::HTMLInputElement **pass,
		   bool filled )
{
	*user = *pass = nullptr;
	blink::HTMLInputElement *last_text = nullptr;
	for ( blink::HTMLInputElement &in : blink::Traversal<blink::HTMLInputElement>::DescendantsOf(form) ) {
		blink::mojom::FormControlType t = in.FormControlType();
		if ( t == blink::mojom::FormControlType::kInputPassword ) {
			if ( *pass == nullptr && ( !filled || !in.Value().empty() ) ) {
				*pass = &in;
				*user = last_text;
			}
		} else if ( t == blink::mojom::FormControlType::kInputText || t == blink::mojom::FormControlType::kInputEmail
			    || t == blink::mojom::FormControlType::kInputTelephone ) {
			if ( *pass == nullptr && ( !filled || !in.Value().empty() ) ) last_text = &in;
		}
	}
}

int field_kind( blink::HTMLInputElement &in );
void autofill_offer( blink::HTMLFormElement &form, bool card, const std::string &origin );
constexpr int kCardBaseOf = 100;

std::string origin_of( blink::Document &d )
{
	return d.GetExecutionContext() != nullptr ? d.GetExecutionContext()->GetSecurityOrigin()->ToString().Utf8()
						  : std::string();
}

void BrFrameClientBase::DispatchWillSendSubmitEvent( blink::HTMLFormElement *form )
{
	if ( form == nullptr ) return;
	blink::HTMLInputElement *user, *pass;
	login_fields(*form, &user, &pass, true);
	if ( pass == nullptr ) return;
	br_password_offer(origin_of(form->GetDocument()), user != nullptr ? user->Value().Utf8() : std::string(),
			  pass->Value().Utf8());
}

void BrFrameClientBase::FocusedElementChanged( blink::Element *element )
{
	auto *in = blink::DynamicTo<blink::HTMLInputElement>(element);
	if ( in == nullptr || in->Form() == nullptr ) return;
	blink::HTMLInputElement *user, *pass;
	login_fields(*in->Form(), &user, &pass, false);
	if ( pass == nullptr ) {
		/* not a login form: an address or a card, once for each document; a card only for a secure page */
		int kind = field_kind(*in);
		blink::Document &doc = in->GetDocument();
		if ( kind < 0 || filled_ == &doc || doc.GetFrame() == nullptr ) return;
		bool card = kind >= kCardBaseOf;
		if ( card && ( doc.GetExecutionContext() == nullptr || !doc.GetExecutionContext()->IsSecureContext() ) ) return;
		filled_ = &doc;
		doc.GetFrame()->GetTaskRunner(blink::TaskType::kUserInteraction)->PostTask(FROM_HERE, base::BindOnce(
			[]( blink::Persistent<blink::HTMLFormElement> form, bool card, std::string origin ) {
				autofill_offer(*form, card, origin);
			}, blink::Persistent<blink::HTMLFormElement>(in->Form()), card, origin_of(doc)));
		return;
	}
	if ( in != pass && in != user ) return;
	blink::Document &d = in->GetDocument();
	if ( offered_ == &d || d.GetFrame() == nullptr ) return;
	offered_ = &d;
	/* asked from a task of its own, after the focus has moved */
	d.GetFrame()->GetTaskRunner(blink::TaskType::kUserInteraction)->PostTask(FROM_HERE, base::BindOnce(
		[]( blink::Persistent<blink::HTMLInputElement> user, blink::Persistent<blink::HTMLInputElement> pass,
		    std::string origin ) {
			std::string u, p;
			if ( !br_password_find(origin, &u, &p) ) return;
			if ( user != nullptr ) {
				user->SetValue(blink::String::FromUtf8(u), blink::TextFieldEventBehavior::kDispatchInputAndChangeEvent);
			}
			pass->SetValue(blink::String::FromUtf8(p), blink::TextFieldEventBehavior::kDispatchInputAndChangeEvent);
			printf("browser: the password kept for %s put in its form\n", origin.c_str());
		}, blink::Persistent<blink::HTMLInputElement>(user), blink::Persistent<blink::HTMLInputElement>(pass),
		origin_of(d)));
}

/*
 * What a form's field asks for, by its autocomplete attribute or its name
 * and id, as an index of the address's fields, of the card's fields plus
 * kCardBase, kCardExp for a month and year together; -1 not known
 */
constexpr int kCardBase = kCardBaseOf, kCardExp = 199;

int field_kind( blink::HTMLInputElement &in )
{
	static const struct { const char *word; int kind; } kAuto[] = {
		{ "cc-name", kCardBase + BR_CARD_NAME }, { "cc-number", kCardBase + BR_CARD_NUMBER },
		{ "cc-exp-month", kCardBase + BR_CARD_MONTH }, { "cc-exp-year", kCardBase + BR_CARD_YEAR }, { "cc-exp", kCardExp },
		{ "organization", BR_ADDR_ORG }, { "postal-code", BR_ADDR_ZIP }, { "address-level1", BR_ADDR_REGION },
		{ "address-level2", BR_ADDR_CITY }, { "address-line1", BR_ADDR_LINE1 }, { "street-address", BR_ADDR_LINE1 },
		{ "address-line2", BR_ADDR_LINE2 }, { "tel", BR_ADDR_TEL }, { "email", BR_ADDR_EMAIL }, { "name", BR_ADDR_NAME },
	};
	static const struct { const char *word; int kind; } kName[] = {
		{ "cardnumber", kCardBase + BR_CARD_NUMBER }, { "card-number", kCardBase + BR_CARD_NUMBER }, { "ccnumber", kCardBase + BR_CARD_NUMBER },
		{ "cardname", kCardBase + BR_CARD_NAME }, { "ccname", kCardBase + BR_CARD_NAME },
		{ "postal", BR_ADDR_ZIP }, { "zip", BR_ADDR_ZIP }, { "email", BR_ADDR_EMAIL }, { "mail", BR_ADDR_EMAIL },
		{ "phone", BR_ADDR_TEL }, { "tel", BR_ADDR_TEL }, { "company", BR_ADDR_ORG }, { "organization", BR_ADDR_ORG },
		{ "prefecture", BR_ADDR_REGION }, { "city", BR_ADDR_CITY }, { "address2", BR_ADDR_LINE2 }, { "address", BR_ADDR_LINE1 },
	};
	std::string a = in.FastGetAttribute(blink::html_names::kAutocompleteAttr).GetString().Utf8();
	std::transform(a.begin(), a.end(), a.begin(), ::tolower);
	/* the last word: "shipping street-address" */
	size_t sp = a.rfind(' ');
	if ( sp != std::string::npos ) a = a.substr(sp + 1);
	for ( const auto &e : kAuto ) {
		if ( a == e.word ) return e.kind;
	}
	if ( a == "off" || !a.empty() ) return -1;
	std::string n = in.GetName().GetString().Utf8() + " " + in.GetIdAttribute().GetString().Utf8();
	std::transform(n.begin(), n.end(), n.begin(), ::tolower);
	for ( const auto &e : kName ) {
		if ( n.find(e.word) != std::string::npos ) return e.kind;
	}
	return -1;
}

/* The form's fields filled from what was chosen on a panel: asked from a task of its own, after the focus has moved */
void autofill_offer( blink::HTMLFormElement &form, bool card, const std::string &origin )
{
	/* nothing goes into the page until the person has chosen one, and only that one comes here */
	std::vector<std::string> f;
	if ( !br_autofill_choose(card, origin, &f) ) return;
	size_t want = card ? BR_CARD_FIELDS : BR_ADDR_FIELDS;
	if ( f.size() < want ) return;
	int filled = 0;
	for ( blink::HTMLInputElement &in : blink::Traversal<blink::HTMLInputElement>::DescendantsOf(form) ) {
		int kind = field_kind(in);
		if ( kind < 0 || ( kind >= kCardBase ) != card ) continue;
		std::string v;
		if ( kind == kCardExp ) {
			v = f[BR_CARD_MONTH] + "/" + f[BR_CARD_YEAR];
		} else {
			v = f[(size_t)( card ? kind - kCardBase : kind )];
		}
		if ( v.empty() ) continue;
		in.SetValue(blink::String::FromUtf8(v), blink::TextFieldEventBehavior::kDispatchInputAndChangeEvent);
		filled++;
	}
	printf("browser: %d fields of a form of %s filled, chosen on the panel\n", filled, origin.c_str());
	for ( std::string &s : f ) std::fill(s.begin(), s.end(), '\0');
}

unsigned BrFrameClientBase::BackForwardLength()
{
	return page_history_length(page_);
}

bool BrFrameClientBase::NavigateBackForward( int offset, base::TimeTicks,
					     std::optional<blink::scheduler::TaskAttributionId> ) const
{
	return page_history_go(page_, offset);
}

/* ---------------------------------------------------------------- iframes */

blink::LocalFrame *BrFrameClientBase::CreateFrame( const blink::AtomicString &name,
						   blink::HTMLFrameOwnerElement *owner )
{
	if ( owner == nullptr ) return nullptr;
	blink::LocalFrame *parent = owner->GetDocument().GetFrame();
	if ( parent == nullptr || parent->GetPage() == nullptr || parent->DomWindow() == nullptr ) return nullptr;
	blink::Frame *last = parent->Tree().FirstChild();
	while ( last != nullptr && last->Tree().NextSibling() != nullptr ) last = last->Tree().NextSibling();
	auto *client = blink::MakeGarbageCollected<BrChildFrameClient>(page_);
	/* the parent's agents: a same-origin iframe's script reaches the parent's and back */
	auto *frame = blink::MakeGarbageCollected<blink::LocalFrame>(
		client, *parent->GetPage(), owner, parent, last,
		blink::FrameInsertType::kInsertInConstructor, blink::LocalFrameToken(),
		&parent->window_agent_factory(), nullptr, mojo::NullRemote(), base::DefaultTickClock::GetInstance());
	client->SetFrame(frame);
	frame->Tree().SetName(name);
	/*
	 * the initial empty document's policies are its parent's, and its
	 * sandbox flags the parent's with the iframe's sandbox attribute's,
	 * as a browser makes them
	 */
	auto policies = parent->DomWindow()->GetPolicyContainer()->GetPolicies().Clone();
	policies->sandbox_flags |= owner->GetFramePolicy().sandbox_flags;
	mojo::AssociatedRemote<blink::mojom::blink::PolicyContainerHost> host;
	std::ignore = host.BindNewEndpointAndPassDedicatedReceiver();
	/* a frame sandboxed from its origin starts with an opaque origin of its own, made from this token */
	bool opaque = ( policies->sandbox_flags & network::mojom::WebSandboxFlags::kOrigin )
		      != network::mojom::WebSandboxFlags::kNone;
	frame->Init(nullptr, blink::DocumentToken(),
		    std::make_unique<blink::PolicyContainer>(host.Unbind(), std::move(policies)),
		    parent->DomWindow()->GetStorageKey(), ukm::kInvalidSourceId, owner->GetDocument().BaseURL(),
		    opaque ? std::make_unique<base::UnguessableToken>(base::UnguessableToken::Create()) : nullptr);
	return frame;
}

/* The sandbox flags of an iframe's documents: its parent's document's, and its sandbox attribute's as it is now */
static network::mojom::WebSandboxFlags child_sandbox( blink::LocalFrame *frame )
{
	network::mojom::WebSandboxFlags f = network::mojom::WebSandboxFlags::kNone;
	blink::LocalFrame *parent = blink::DynamicTo<blink::LocalFrame>(frame->Tree().Parent());
	if ( parent != nullptr && parent->DomWindow() != nullptr && parent->DomWindow()->GetPolicyContainer() != nullptr ) {
		f = parent->DomWindow()->GetPolicyContainer()->GetPolicies().sandbox_flags;
	}
	if ( frame->Owner() != nullptr ) f |= frame->Owner()->GetFramePolicy().sandbox_flags;
	return f;
}

/*
 * The policies of a document an iframe commits from the network (or a
 * data: URL): the defaults, with the sandbox flags of its parent's
 * document and of the iframe's sandbox attribute as it is now. An
 * about:blank or about:srcdoc document keeps those of the document before
 * it (the initial empty document's, its parent's).
 */
static std::unique_ptr<blink::WebPolicyContainer> child_policies( blink::LocalFrame *frame )
{
	blink::WebPolicyContainerPolicies p;
	p.sandbox_flags = child_sandbox(frame);
	mojo::AssociatedRemote<blink::mojom::blink::PolicyContainerHost> host;
	std::ignore = host.BindNewEndpointAndPassDedicatedReceiver();
	return std::make_unique<blink::WebPolicyContainer>(std::move(p), host.Unbind());
}

/*
 * The policies of a document the main frame commits from the network (or
 * a data: URL): the defaults, which its own response's CSP is then added
 * to. Without them Blink keeps the window's policy container, and each
 * new document would be held to the CSPs of every document before it as
 * well as its own.
 */
static std::unique_ptr<blink::WebPolicyContainer> main_policies()
{
	blink::WebPolicyContainerPolicies p;
	mojo::AssociatedRemote<blink::mojom::blink::PolicyContainerHost> host;
	std::ignore = host.BindNewEndpointAndPassDedicatedReceiver();
	return std::make_unique<blink::WebPolicyContainer>(std::move(p), host.Unbind());
}

namespace {

/* What an iframe's fetch finds its way back by */
struct ChildNav {
	blink::Persistent<BrChildFrameClient>	client;
	unsigned				seq;
};

void child_delivered( void *ctx, int ok, int status, const char *url, const char *headers, const char *ctype,
		      const char *data, unsigned len )
{
	std::unique_ptr<ChildNav> n(static_cast<ChildNav *>(ctx));
	if ( n == nullptr || n->client == nullptr || n->client->nav_seq() != n->seq ) return;
	if ( !ok ) {
		printf("browser: frame: %s not fetched\n", url != nullptr ? url : "?");
		return;
	}
	/* the protocol it came over, while the answer is handed over (its navigation's timing names it) */
	const char *proto = br_net_fetch_answer_protocol();
	/* and the redirects it followed (the document's redirectCount, its redirect timing) */
	std::vector<BrNavRedirect> redirects;
	int nr = br_net_fetch_redirects();
	for ( int i = 0; i < nr; i++ ) {
		const char *u = "", *h = "", *nu = "", *nm = "", *nref = "";
		int st = 0;
		if ( !br_net_fetch_redirect(i, &u, &st, &h, &nu, &nm, &nref) ) break;
		redirects.push_back({ u, st, h, nu, nm, nref });
	}
	n->client->Answered(std::move(redirects));
	n->client->Commit(n->seq, url != nullptr ? url : "", ctype != nullptr ? ctype : "", data, len, status,
			  headers != nullptr ? headers : "", proto != nullptr ? proto : "");
}

/* Whether a form's body has files (Blobs) in it, whose bytes are read before it is sent */
bool form_has_blobs( const blink::EncodedFormData &form )
{
	for ( const blink::FormDataElement &e : form.Elements() ) {
		if ( e.type_ == blink::FormDataElement::kEncodedBlob && e.blob_data_handle_ ) return true;
	}
	return false;
}

/*
 * A form's body with its files: the bytes of each element in turn, a
 * Blob's read from the Blobs' store; told to 'done' on the thread that
 * asked
 */
struct FormReading {
	std::vector<std::pair<std::string, std::string>>	parts;	/* bytes, or the UUID of a Blob */
	size_t							at = 0;
	std::string						out;
	base::OnceCallback<void( std::string )>			done;
	scoped_refptr<base::SingleThreadTaskRunner>		runner;
};

void form_step( FormReading *r )
{
	while ( r->at < r->parts.size() && r->parts[r->at].second.empty() ) {
		r->out += r->parts[r->at].first;
		r->at++;
	}
	if ( r->at == r->parts.size() ) {
		std::move(r->done).Run(std::move(r->out));
		delete r;
		return;
	}
	br_blob_read(r->parts[r->at].second, false, base::BindOnce([]( FormReading *r, bool, std::string bytes, std::string ) {
		r->runner->PostTask(FROM_HERE, base::BindOnce([]( FormReading *r, std::string bytes ) {
			r->out += bytes;
			r->at++;
			form_step(r);
		}, r, std::move(bytes)));
	}, r));
}

void read_form_body( const blink::EncodedFormData &form, base::OnceCallback<void( std::string )> done )
{
	auto *r = new FormReading;
	r->done = std::move(done);
	r->runner = base::SingleThreadTaskRunner::GetCurrentDefault();
	for ( const blink::FormDataElement &e : form.Elements() ) {
		if ( e.type_ == blink::FormDataElement::kData ) {
			r->parts.emplace_back(std::string(e.data_.data(), e.data_.size()), std::string());
		} else if ( e.type_ == blink::FormDataElement::kEncodedBlob && e.blob_data_handle_ ) {
			r->parts.emplace_back(std::string(), e.blob_data_handle_->Uuid().Utf8());
		}
	}
	form_step(r);
}

}  // namespace

void BrChildFrameClient::NetFetch( unsigned seq, const std::string &u, const std::string &method,
				   const std::string &send, const std::string &hdrs )
{
	auto *n = new ChildNav{ blink::Persistent<BrChildFrameClient>(this), seq };
	nav_sent_ = base::TimeTicks::Now();
	br_net_fetch_set_answer_on_main(1);
	int started = br_net_fetch_start(u.c_str(), method.empty() ? "GET" : method.c_str(), send.data(),
					 (unsigned)send.size(), hdrs.c_str(), child_delivered, n);
	br_net_fetch_set_answer_on_main(0);
	if ( !started ) delete n;
}

void BrChildFrameClient::BeginNavigation(
	const blink::ResourceRequest &request, const blink::KURL &,
	blink::mojom::RequestContextFrameType, blink::LocalDOMWindow *origin_window,
	blink::DocumentLoader *, blink::WebNavigationType, blink::NavigationPolicy,
	blink::WebFrameLoadType load_type, blink::mojom::blink::ForceHistoryPush, bool, bool,
	blink::mojom::blink::TriggeringEventInfo, blink::HTMLFormElement *,
	network::mojom::CSPDisposition, mojo::PendingRemote<blink::mojom::blink::BlobURLToken>,
	base::TimeTicks, base::TimeTicks, const blink::String &,
	const blink::LocalFrameToken *, const base::UnguessableToken &,
	const blink::DocumentToken &, blink::SourceLocation *,
	mojo::PendingRemote<blink::mojom::blink::NavigationStateKeepAliveHandle>,
	bool, bool,
	mojo::PendingReceiver<blink::mojom::blink::NavigationResumeDeferredCommitListener>,
	std::optional<base::UnguessableToken> )
{
	if ( frame_ == nullptr ) return;
	unsigned seq = ++nav_seq_;
	requestor_ = request.RequestorOrigin();
	/* how it commits into the session history, who sent it there, and when it began */
	nav_type_ = load_type;
	nav_item_ = going_item_;
	going_item_ = nullptr;
	if ( nav_type_ == blink::WebFrameLoadType::kBackForward && nav_item_ == nullptr ) {
		nav_type_ = blink::WebFrameLoadType::kStandard;
	}
	nav_referrer_ = request.ReferrerString() == "no-referrer" ? blink::String() : blink::String(request.ReferrerString());
	nav_began_ = base::TimeTicks::Now();
	nav_sent_ = base::TimeTicks();
	nav_redirects_.clear();
	const blink::KURL &url = request.Url();
	std::string u = url.GetString().Utf8();
	/*
	 * the base URL an about:blank document takes, its initiator's, and an
	 * about:srcdoc document, its parent's (snapshotted now, as a browser does)
	 */
	fallback_base_ = blink::KURL();
	if ( url.IsEmpty() || url.IsAboutBlankUrl() || url.IsAboutSrcdocUrl() ) {
		blink::LocalFrame *parent = blink::DynamicTo<blink::LocalFrame>(frame_->Tree().Parent());
		blink::LocalDOMWindow *from = url.IsAboutSrcdocUrl() || origin_window == nullptr
					      ? ( parent != nullptr ? parent->DomWindow() : nullptr ) : origin_window;
		if ( from != nullptr && from->document() != nullptr ) fallback_base_ = from->document()->BaseURL();
	}
	scoped_refptr<base::SingleThreadTaskRunner> runner = base::SingleThreadTaskRunner::GetCurrentDefault();
	blink::Persistent<BrChildFrameClient> self(this);

	/* documents made from the URL, committed from a task of their own */
	std::string mime, charset, body;
	if ( url.IsEmpty() || url.IsAboutBlankUrl() ) {
		u = "about:blank";
		mime = "text/html";
	} else if ( url.IsAboutSrcdocUrl() ) {
		blink::HTMLFrameOwnerElement *owner = frame_->DeprecatedLocalOwner();
		mime = "text/html";
		charset = "UTF-8";
		if ( owner != nullptr ) body = owner->FastGetAttribute(blink::html_names::kSrcdocAttr).Utf8();
	} else if ( url.ProtocolIsData() ) {
		if ( !net::DataURL::Parse(GURL(u), &mime, &charset, &body) ) {
			printf("browser: frame: a data: URL that does not parse\n");
			return;
		}
	} else if ( url.ProtocolIs("blob") ) {
		/* a blob: URL's Blob, read from the store (the URL without its fragment), committed when it is in */
		blink::KURL whole = url;
		whole.RemoveFragmentIdentifier();
		br_blob_read(whole.GetString().Utf8(), true, base::BindOnce([]( scoped_refptr<base::SingleThreadTaskRunner> runner,
										blink::Persistent<BrChildFrameClient> c, unsigned seq,
										std::string u, bool ok, std::string bytes,
										std::string type ) {
			runner->PostTask(FROM_HERE, base::BindOnce([]( blink::Persistent<BrChildFrameClient> c, unsigned seq,
								       std::string u, bool ok, std::string bytes,
								       std::string type ) {
				if ( !ok ) {
					printf("browser: frame: %s is not a Blob there is\n", u.c_str());
					return;
				}
				c->Commit(seq, u, type.empty() ? "text/plain" : type, bytes.data(), bytes.size());
			}, c, seq, u, ok, std::move(bytes), std::move(type)));
		}, runner, self, seq, u));
		return;
	} else if ( !url.ProtocolIsInHttpFamily() && !url.ProtocolIs("chrome-extension") ) {
		printf("browser: frame: %s is not loaded (only http, https, blob, data and about)\n", u.c_str());
		return;
	} else {
		std::string method = request.HttpMethod().GetString().Utf8();
		std::string send, hdrs = "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n";
		if ( request.HttpBody() != nullptr ) {
			/* the body's bytes as they are (the form's encoding's, not text) */
			blink::Vector<char> bytes;
			request.HttpBody()->Flatten(bytes);
			send.assign(bytes.data(), bytes.size());
			std::string ct = request.HttpContentType().GetString().Utf8();
			if ( !ct.empty() ) hdrs += "Content-Type: " + ct + "\r\n";
		}
		std::string ref = request.ReferrerString().Utf8();
		if ( !ref.empty() && ref != "no-referrer" ) hdrs += "Referer: " + ref + "\r\n";
		if ( br_ext_any() ) {
			hdrs += "X-TessronOS-Resource: sub_frame";
			if ( request.RequestorOrigin() != nullptr ) hdrs += " " + request.RequestorOrigin()->ToString().Utf8();
			hdrs += "\r\n";
		}
		std::string shown = u.size() > 160 ? u.substr(0, 160) + "..." : u;
		printf("browser: frame: %s\n", shown.c_str());
		/* a form with files: their bytes read, and the form sent then */
		if ( request.HttpBody() != nullptr && form_has_blobs(*request.HttpBody()) ) {
			read_form_body(*request.HttpBody(), base::BindOnce([]( blink::Persistent<BrChildFrameClient> c,
									       unsigned seq, std::string u, std::string method,
									       std::string hdrs, std::string body ) {
				if ( seq == c->nav_seq() ) c->NetFetch(seq, u, method, body, hdrs);
			}, self, seq, u, method, hdrs));
			return;
		}
		/*
		 * a GET in the scope of an active service worker asks it first,
		 * as a fetch event whose resulting client is the document to come
		 */
		if ( send.empty() && ( method.empty() || method == "GET" ) ) {
			std::string uuid = base::UnguessableToken::Create().ToString();
			sw_next_ = uuid;
			bool frame_element = blink::IsA<blink::HTMLFrameElement>(frame_->DeprecatedLocalOwner());
			if ( br_sw_navigate_frame(u, "GET", ref == "no-referrer" ? std::string() : ref, uuid, frame_element,
						  base::BindOnce([]( blink::Persistent<BrChildFrameClient> c, unsigned seq,
								     std::string u, std::string hdrs, bool fallback, int status,
								     std::string url, std::string headers, std::string ctype,
								     std::string body ) {
				if ( seq != c->nav_seq() ) return;
				if ( fallback ) {
					c->NetFetch(seq, u, "GET", std::string(), hdrs);
					return;
				}
				printf("browser: frame: %s answered by its service worker\n", u.c_str());
				c->Commit(seq, url.empty() ? u : url, ctype.empty() ? "text/html" : ctype, body.data(),
					  body.size(), status, headers);
			}, self, seq, u, hdrs)) ) {
				return;
			}
		}
		NetFetch(seq, u, method, send, hdrs);
		return;
	}
	if ( !charset.empty() ) mime += "; charset=" + charset;
	/*
	 * The first navigation of a new iframe to about:blank (no src, or
	 * about:blank) from its parent's origin is committed at once, as a
	 * browser's renderer commits it: the iframe's load event comes while
	 * it is put in the document
	 */
	blink::LocalFrame *parent = blink::DynamicTo<blink::LocalFrame>(frame_->Tree().Parent());
	if ( u == "about:blank" && seq == 1 && frame_->GetDocument() != nullptr
	     && frame_->GetDocument()->IsInitialEmptyDocument() && request.HttpMethod() == "GET"
	     && parent != nullptr && parent->DomWindow() != nullptr && requestor_
	     && requestor_->IsSameOriginWith(parent->DomWindow()->GetSecurityOrigin()) ) {
		sync_blank_ = true;
		Commit(seq, u, mime, body.data(), body.size());
		sync_blank_ = false;
		return;
	}
	runner->PostTask(FROM_HERE, base::BindOnce([]( blink::Persistent<BrChildFrameClient> c, unsigned seq,
						       std::string u, std::string mime, std::string body ) {
		c->Commit(seq, u, mime, body.data(), body.size());
	}, self, seq, u, mime, body));
}

/*
 * Whether an iframe's document may not come into its parent under the
 * parent's Cross-Origin-Embedder-Policy, as a browser's navigation checks
 * it: when the parent's COEP is require-corp or credentialless, a
 * document from the network must have a COEP that is too, and must pass
 * the Cross-Origin-Resource-Policy check of a nested navigation
 * (network::CrossOriginResourcePolicy::IsNavigationBlocked)
 */
static bool coep_blocks_frame( blink::LocalFrame *frame, const std::string &url, const std::string &headers )
{
	blink::LocalFrame *parent = blink::DynamicTo<blink::LocalFrame>(frame->Tree().Parent());
	if ( parent == nullptr || parent->DomWindow() == nullptr || parent->DomWindow()->GetPolicyContainer() == nullptr ) {
		return false;
	}
	network::CrossOriginEmbedderPolicy parent_coep;
	parent_coep.value = parent->DomWindow()->GetPolicyContainer()->GetPolicies().cross_origin_embedder_policy.value;
	if ( !network::CompatibleWithCrossOriginIsolated(parent_coep) ) return false;
	GURL u(url);
	if ( !u.SchemeIsHTTPOrHTTPS() ) return false;
	auto head = network::mojom::URLResponseHead::New();
	head->headers = base::MakeRefCounted<net::HttpResponseHeaders>(
		net::HttpUtil::AssembleRawHeaders("HTTP/1.1 200 OK\r\n" + headers + "\r\n"));
	head->request_include_credentials = true;
	network::CrossOriginEmbedderPolicy coep;
	if ( network::IsUrlPotentiallyTrustworthy(u) ) coep = network::ParseCrossOriginEmbedderPolicy(*head->headers);
	if ( !network::CompatibleWithCrossOriginIsolated(coep) ) {
		printf("browser: console: Failed to load resource: net::ERR_BLOCKED_BY_RESPONSE (%s: the frame's document "
		       "has no Cross-Origin-Embedder-Policy that its parent's needs)\n", url.c_str());
		return true;
	}
	url::Origin initiator = parent->DomWindow()->GetSecurityOrigin()->ToUrlOrigin();
	auto blocked = network::CrossOriginResourcePolicy::IsNavigationBlocked(
		u, u, initiator, *head, network::mojom::RequestDestination::kIframe, parent_coep, nullptr);
	if ( blocked ) {
		printf("browser: console: Failed to load resource: net::ERR_BLOCKED_BY_RESPONSE (%s, reason %d)\n", url.c_str(),
		       (int)*blocked);
		return true;
	}
	return false;
}

void BrChildFrameClient::Commit( unsigned seq, const std::string &url, const std::string &ctype, const char *data,
				 size_t len, int status, const std::string &headers, const std::string &protocol )
{
	if ( seq != nav_seq_ || frame_ == nullptr ) return;
	if ( status > 0 && coep_blocks_frame(frame_, url, headers) ) {
		/*
		 * blocked: the frame shows the browser's error document instead,
		 * of an opaque origin, which its parent cannot look into
		 */
		static const char kBlocked[] = "<!DOCTYPE html><html><body></body></html>";
		auto params = std::make_unique<blink::WebNavigationParams>();
		params->url = blink::KURL("chrome-error://chromewebdata/");
		params->unreachable_url = blink::KURL(blink::String::FromUtf8(url));
		params->error_code = net::ERR_BLOCKED_BY_RESPONSE;
		params->frame_load_type = nav_type_;
		params->policy_container = child_policies(frame_);
		if ( frame_->Owner() != nullptr ) params->frame_policy = frame_->Owner()->GetFramePolicy();
		blink::WebNavigationParams::FillStaticResponse(params.get(), blink::WebString::FromUtf8("text/html"),
							       blink::WebString::FromUtf8("UTF-8"),
							       base::span<const char>(kBlocked, sizeof(kBlocked) - 1));
		nav_redirects_.clear();
		nav_began_ = nav_sent_ = nav_answered_ = base::TimeTicks();
		g_doc_replacing++;
		frame_->Loader().CommitNavigation(std::move(params), nullptr);
		g_doc_replacing--;
		return;
	}
	std::string mime = ctype.substr(0, ctype.find(';')), charset;
	while ( !mime.empty() && ( mime.back() == ' ' || mime.back() == '\t' ) ) mime.pop_back();
	for ( char &ch : mime ) ch = (char)tolower((unsigned char)ch);
	if ( mime.empty() ) mime = "text/html";
	std::string pdf;
	if ( mime == "application/pdf" || mime == "text/pdf" ) {
		pdf = br_pdf_page(url, std::string(data, len));
		if ( !pdf.empty() ) {
			mime = "text/html";
			data = pdf.data();
			len = pdf.size();
		}
	}
	const std::string ct = pdf.empty() ? ctype : std::string("text/html; charset=utf-8");
	std::string lc = ct;
	for ( char &ch : lc ) ch = (char)tolower((unsigned char)ch);
	size_t cs = lc.find("charset=");
	if ( cs != std::string::npos ) {
		charset = ct.substr(cs + 8);
		charset = charset.substr(0, charset.find_first_of("; "));
		charset.erase(std::remove(charset.begin(), charset.end(), '"'), charset.end());
	}
	auto params = std::make_unique<blink::WebNavigationParams>();
	params->url = blink::KURL(blink::String::FromUtf8(url));
	/*
	 * how it commits into the session history: a new entry, the entry
	 * shown taken over or loaded again (a reload), or an entry gone back
	 * or forward to, whose HistoryItem is given back
	 */
	params->frame_load_type = nav_type_;
	if ( nav_type_ == blink::WebFrameLoadType::kBackForward && nav_item_ != nullptr ) {
		params->history_item = blink::WebHistoryItem(nav_item_.Get());
	}
	if ( !nav_referrer_.empty() ) params->referrer = nav_referrer_;
	/*
	 * its timing (the document's PerformanceNavigationTiming): begun, its
	 * request sent, the redirects it followed, its answer come
	 */
	base::TimeTicks now = base::TimeTicks::Now();
	base::TimeTicks began = nav_began_.is_null() ? now : nav_began_;
	base::TimeTicks sent = nav_sent_.is_null() || nav_sent_ < began ? began : nav_sent_;
	base::TimeTicks answered = nav_answered_.is_null() || nav_answered_ < sent ? now : nav_answered_;
	params->navigation_timings.navigation_start = began;
	params->navigation_timings.fetch_start = sent;
	if ( status > 0 && !nav_redirects_.empty() ) {
		params->navigation_timings.redirect_start = sent;
		params->navigation_timings.redirect_end = answered;
		for ( const BrNavRedirect &r : nav_redirects_ ) {
			blink::WebNavigationParams::RedirectInfo info;
			info.new_url = blink::KURL(blink::String::FromUtf8(r.new_url));
			info.new_http_method = blink::WebString::FromUtf8(r.new_method.empty() ? "GET" : r.new_method);
			info.new_referrer = blink::WebString::FromUtf8(r.new_referrer);
			info.new_referrer_policy = network::mojom::ReferrerPolicy::kDefault;
			info.redirect_response = blink::WebURLResponse(blink::KURL(blink::String::FromUtf8(r.url)));
			apply_response(info.redirect_response, r.url, r.status, r.headers, protocol, 0);
			params->redirects.push_back(std::move(info));
		}
	}
	nav_redirects_.clear();
	nav_began_ = nav_sent_ = nav_answered_ = base::TimeTicks();
	if ( requestor_ ) params->requestor_origin = blink::WebSecurityOrigin(requestor_);
	if ( !fallback_base_.IsEmpty() ) params->fallback_base_url = fallback_base_;
	if ( sync_blank_ ) {
		/* committed at once over the initial empty document: its token and origin are kept */
		params->is_synchronous_commit_for_bug_778318 = true;
		params->document_token = frame_->GetDocument()->Token();
		params->origin_to_commit = blink::WebSecurityOrigin(frame_->DomWindow()->GetSecurityOrigin());
	}
	{
		blink::KURL k(blink::String::FromUtf8(url));
		if ( !k.IsAboutBlankUrl() && !k.IsAboutSrcdocUrl() ) params->policy_container = child_policies(frame_);
	}
	/* the iframe's own policy (its allow attribute's permissions policy), which the document is held to */
	if ( frame_->Owner() != nullptr ) params->frame_policy = frame_->Owner()->GetFramePolicy();
	/*
	 * a document of a frame sandboxed from its origin gets an opaque origin
	 * of its own, as a browser gives it: from its address, or for an
	 * about:blank or about:srcdoc document from its initiator's or parent's
	 */
	if ( !sync_blank_ && ( child_sandbox(frame_) & network::mojom::WebSandboxFlags::kOrigin )
			     != network::mojom::WebSandboxFlags::kNone ) {
		blink::KURL k(blink::String::FromUtf8(url));
		blink::LocalFrame *parent = blink::DynamicTo<blink::LocalFrame>(frame_->Tree().Parent());
		scoped_refptr<const blink::SecurityOrigin> from;
		if ( k.IsAboutSrcdocUrl() || ( k.IsAboutBlankUrl() && !requestor_ ) ) {
			if ( parent != nullptr && parent->DomWindow() != nullptr ) from = parent->DomWindow()->GetSecurityOrigin();
		} else if ( k.IsAboutBlankUrl() ) {
			from = requestor_;
		} else {
			from = blink::SecurityOrigin::Create(k);
		}
		if ( from ) params->origin_to_commit = blink::WebSecurityOrigin(from->DeriveNewOpaqueOrigin());
	}
	blink::WebNavigationParams::FillStaticResponse(params.get(), blink::WebString::FromUtf8(mime),
						       blink::WebString::FromUtf8(charset),
						       base::span<const char>(data, len));
	if ( status > 0 ) {
		apply_response(params->response, blink::KURL(blink::String::FromUtf8(url)).GetString().Utf8(), status, headers, protocol, len);
		network::mojom::LoadTimingInfo t;
		t.request_start = sent;
		t.request_start_time = base::Time::Now() - ( now - sent );
		t.send_start = sent;
		t.send_end = sent;
		t.receive_headers_start = answered;
		t.receive_headers_end = answered;
		t.receive_non_informational_headers_start = answered;
		params->response.SetLoadTiming(t);
	}
	set_agent_cluster(params.get(), headers, false);
	g_doc_replacing++;
	frame_->Loader().CommitNavigation(std::move(params), nullptr);
	g_doc_replacing--;
}

void BrChildFrameClient::GoToEntry( blink::HistoryItem *item )
{
	if ( frame_ == nullptr || item == nullptr ) return;
	/* a GET of the entry's address, committed as that entry (BeginNavigation takes going_item_) */
	blink::ResourceRequest request(item->Url());
	request.SetHttpMethod(blink::http_names::kGET);
	if ( frame_->DomWindow() != nullptr ) request.SetRequestorOrigin(frame_->DomWindow()->GetSecurityOrigin());
	going_item_ = item;
	printf("browser: history: the frame to %s\n", item->Url().GetString().Utf8().c_str());
	BeginNavigation(request, blink::KURL(), blink::mojom::RequestContextFrameType::kNested, nullptr, nullptr,
			blink::kWebNavigationTypeBackForward, blink::kNavigationPolicyCurrentTab,
			blink::WebFrameLoadType::kBackForward, blink::mojom::blink::ForceHistoryPush::kNo, false, false,
			blink::mojom::blink::TriggeringEventInfo::kNotFromEvent, nullptr,
			network::mojom::CSPDisposition::CHECK, mojo::NullRemote(), base::TimeTicks(), base::TimeTicks(),
			blink::String(), nullptr, base::UnguessableToken(), blink::DocumentToken(), nullptr,
			mojo::NullRemote(), false, false, mojo::NullReceiver(), std::nullopt);
	going_item_ = nullptr;
}

/* ---------------------------------------------------------------- where the engine's thread's time goes */

/*
 * The time of the engine's thread between two of its comes round (beat in
 * br_main.cc, which calls br_turn_ended): its tasks, and the three longest
 * of them with where each was posted from; in them, the calls into V8 (a
 * script run, a function or an event handler called; the promise jobs V8
 * runs by itself are not counted), V8's collections on this thread, and
 * the WebGL calls (ANGLE and the OpenGL ES under it, softpipe or the V3D),
 * and the writing of the console's lines (br_console_ticks: the wait for
 * room in the console's ring, or for the serial line when there is none);
 * and the page's frames: their callbacks, and their style, layout and
 * paint. A turn of kSlowTurnMs or more is said on the console with these
 * ("browser: slow: ..."), and so is each task of kSlowTaskMs or more as
 * it ends. Times are read from the processor's counter, which a process
 * reads without a call into the system. (The hook and the counters'
 * functions are declared at the top of the file, outside any namespace.)
 */
namespace {

const long	kSlowTurnMs = 1000;
const long	kSlowTaskMs = 500;

inline uint64_t ticks_now()
{
	uint64_t c;

	BR_COUNTER_READ(c);
	return c;
}

long ticks_ms( uint64_t t )
{
	static uint64_t f = 0;

	if ( f == 0 ) BR_COUNTER_FREQ(f);
	return ( f != 0 ) ? (long)( t * 1000 / f ) : 0;
}

/* What has been spent so far, in ticks: in V8, in its collections, in WebGL */
struct Spent {
	uint64_t	script = 0, gc = 0, webgl = 0, console = 0;
};

uint64_t	g_script_ticks, g_script_at;	/* the outermost call into V8 under way since g_script_at */
uint64_t	g_gc_ticks, g_gc_at;

Spent spent_now()
{
	Spent s;

	s.script = g_script_ticks;
	s.gc = g_gc_ticks;
	s.webgl = ( br_webgl_ticks != nullptr ) ? br_webgl_ticks() : 0;
	s.console = ( br_console_ticks != nullptr ) ? br_console_ticks() : 0;
	return s;
}

void v8_entered( v8::Isolate * )
{
	if ( g_script_at == 0 ) g_script_at = ticks_now();
}

void v8_completed( v8::Isolate * )
{
	if ( g_script_at != 0 ) {
		g_script_ticks += ticks_now() - g_script_at;
		g_script_at = 0;
	}
}

void gc_began( v8::Isolate *, v8::GCType, v8::GCCallbackFlags )
{
	if ( g_gc_at == 0 ) g_gc_at = ticks_now();
}

void gc_ended( v8::Isolate *, v8::GCType, v8::GCCallbackFlags )
{
	if ( g_gc_at != 0 ) {
		g_gc_ticks += ticks_now() - g_gc_at;
		g_gc_at = 0;
	}
}

struct TaskTime {
	uint64_t	t = 0;
	Spent		in;
	const char	*fn = nullptr, *file = nullptr;
	int		line = 0;
};

/* The turn under way: since the engine's thread last came round */
struct Turn {
	uint64_t	tasks = 0, frames = 0, lifecycle = 0;
	unsigned	n = 0;
	Spent		at;
	TaskTime	top[3];			/* its longest tasks, the longest first */
};
Turn	g_turn;

void say_task( const char *what, const TaskTime &k )
{
	printf("browser: %s%ld ms (script %ld, collector %ld, WebGL %ld, console %ld) posted from %s (%s:%d)\n", what,
	       ticks_ms(k.t), ticks_ms(k.in.script), ticks_ms(k.in.gc), ticks_ms(k.in.webgl), ticks_ms(k.in.console),
	       k.fn != nullptr ? k.fn : "?", k.file != nullptr ? k.file : "?", k.line);
}

/* The engine's thread's tasks, each timed when it is not run inside another */
class TaskWatch final : public base::TaskObserver {
public:
	void WillProcessTask( const base::PendingTask &, bool ) override
	{
		if ( depth_++ > 0 ) return;
		at_ = spent_now();
		start_ = ticks_now();
	}

	void DidProcessTask( const base::PendingTask &task ) override
	{
		if ( depth_ == 0 || --depth_ > 0 ) return;
		TaskTime k;
		Spent s = spent_now();
		k.t = ticks_now() - start_;
		k.in.script = s.script - at_.script;
		k.in.gc = s.gc - at_.gc;
		k.in.webgl = s.webgl - at_.webgl;
		k.in.console = s.console - at_.console;
		k.fn = task.posted_from.function_name();
		k.file = task.posted_from.file_name();
		k.line = task.posted_from.line_number();
		g_turn.tasks += k.t;
		g_turn.n++;
		for ( int i = 0; i < 3; i++ ) {
			if ( k.t <= g_turn.top[i].t ) continue;
			for ( int j = 2; j > i; j-- ) g_turn.top[j] = g_turn.top[j - 1];
			g_turn.top[i] = k;
			break;
		}
		if ( ticks_ms(k.t) >= kSlowTaskMs ) say_task("slow: a task of ", k);
	}

private:
	int		depth_ = 0;
	uint64_t	start_ = 0;
	Spent		at_;
};

TaskWatch	g_task_watch;

void turn_ended( const char *doing, long ms )
{
	if ( ms >= kSlowTurnMs ) {
		Spent s = spent_now();
		long tasks = ticks_ms(g_turn.tasks), frames = ticks_ms(g_turn.frames), life = ticks_ms(g_turn.lifecycle);
		printf("browser: slow: %ld ms while %s: tasks %ld ms (%u), frame callbacks %ld ms, style, layout and paint %ld ms, "
		       "outside these %ld ms; in all script %ld ms, collector %ld ms, WebGL %ld ms, console %ld ms\n",
		       ms, doing, tasks, g_turn.n, frames, life, ms - tasks - frames - life,
		       ticks_ms(s.script - g_turn.at.script), ticks_ms(s.gc - g_turn.at.gc), ticks_ms(s.webgl - g_turn.at.webgl),
		       ticks_ms(s.console - g_turn.at.console));
		for ( int i = 0; i < 3 && g_turn.top[i].t != 0; i++ ) say_task("slow:   a task of ", g_turn.top[i]);
	}
	g_turn = Turn();
	g_turn.at = spent_now();
}

/* The time of a part of a frame, added to the turn's (frame callbacks, or style, layout and paint) */
class FrameTime {
public:
	explicit FrameTime( uint64_t *to ) : to_(to), start_(ticks_now()) {}
	~FrameTime() { *to_ += ticks_now() - start_; }

private:
	uint64_t	*to_;
	uint64_t	start_;
};

/* The watching of the engine's thread's time begins (on that thread, once V8 is up) */
void watch_time( v8::Isolate *iso )
{
	if ( iso != nullptr ) {
		iso->AddBeforeCallEnteredCallback(v8_entered);
		iso->AddCallCompletedCallback(v8_completed);
		iso->AddGCPrologueCallback(gc_began);
		iso->AddGCEpilogueCallback(gc_ended);
	}
	if ( base::CurrentThread::IsSet() ) base::CurrentThread::Get()->AddTaskObserver(&g_task_watch);
	g_turn.at = spent_now();
	br_turn_ended = turn_ended;
}

}  // namespace

/* ---------------------------------------------------------------- bringing Blink up */

std::unique_ptr<blink::scheduler::WebThreadScheduler>	g_scheduler;
BrPlatform						*g_platform;

void base_up()
{
	static bool up;

	if ( up ) return;
	up = true;
	static base::AtExitManager exit_manager;

	/*
	 * The schemes of URLs a browser's content client and Chrome's add, as
	 * Chrome on Linux has them, before any thread parses a URL: they are
	 * parsed with a host and have their own origins (chrome-extension:,
	 * isolated-app:, ...), are secure, have no access (chrome-native:,
	 * chrome-error:), or are CORS-enabled
	 */
	{
		static const char *const standard[] = {
			"chrome-devtools", "chrome", "chrome-untrusted", "chrome-error", "chrome-extension",
			"isolated-app", "chrome-native", "chrome-search", "chrome-distiller",
		};
		static const char *const secure[] = {
			"chrome-search", "chrome-extension", "chrome-native", "isolated-app", "chrome-devtools",
			"chrome", "chrome-untrusted", "chrome-error",
		};
		static const char *const cors[] = { "chrome-extension", "isolated-app", "chrome", "chrome-untrusted" };
		for ( const char *s : standard ) url::AddStandardScheme(s, url::SCHEME_WITH_HOST);
		for ( const char *s : secure ) url::AddSecureScheme(s);
		for ( const char *s : cors ) url::AddCorsEnabledScheme(s);
		url::AddNoAccessScheme("chrome-native");
		url::AddNoAccessScheme("chrome-error");
		url::AddEmptyDocumentScheme("chrome-native");
		url::AddCSPBypassingScheme("chrome-extension");
		url::AddWebStorageScheme("isolated-app");
		blink::CommonSchemeRegistry::RegisterURLSchemeAsExtension("chrome-extension");
		blink::CommonSchemeRegistry::RegisterURLSchemeAsIsolatedApp("isolated-app");
	}

	/*
	 * The locale of the scripts' Intl and of Date's words: the browser's
	 * language, Japanese, as a renderer's ICU is given the browser's
	 * locale (navigator.language says the same)
	 */
	{
		UErrorCode st = U_ZERO_ERROR;
		icu::Locale::setDefault(icu::Locale("ja"), st);
		if ( U_FAILURE(st) ) printf("browser: the locale ja was not set (%d)\n", (int)st);
	}

	base::CommandLine::Init(0, nullptr);
	/*
	 * Origins a test treats as secure contexts ("secure=" lines): a page
	 * from them has what only a secure context has (EME, among others)
	 */
	if ( !br_secure_origins.empty() ) {
		base::CommandLine::ForCurrentProcess()->AppendSwitchASCII(
			"unsafely-treat-insecure-origin-as-secure", br_secure_origins);
	}
	{
		/*
		 * Of the image decoders written in Rust, those of JPEG XL and
		 * AVIF are built (tools/browser/rust.sh); BMP and ICO are read
		 * by the decoders in C++, as JPEG and PNG are. JPEG XL is off by
		 * default in the engine and is turned on. QUIC's reader takes
		 * the datagrams that wait in one read (ReadMultiple) rather than
		 * one read each.
		 *
		 * The TCP pool grows past its cap at random, as the engine has
		 * it by default (TcpSocketPoolLimitRandomization), so that a page
		 * cannot learn other sites' connections by filling it: the cap
		 * and as many again stay well within the process's own limit on
		 * sockets (design 9.2.2, 14.13.8).
		 */
		/*
		 * WebNN's interfaces are bound only while the base feature
		 * they are gated on is on: on with its runtime feature
		 * ("features=MachineLearningNeuralNetwork")
		 */
		std::string on = "JXLImageFormat,QuicUseReadMultiple";
		if ( ( "," + br_features + "," ).find(",MachineLearningNeuralNetwork,") != std::string::npos )
			on += ",WebMachineLearningNeuralNetwork";
		auto features = std::make_unique<base::FeatureList>();
		features->InitFromCommandLine(on, "RustyBmpFeature,RustyIcoFeature,RustyJpegFeature");
		base::FeatureList::SetInstance(std::move(features));
	}
	/* the scripts' dates and Intl in the system's time zone (follow_zone) */
	follow_zone();
	/* the memory the engine's parts give back when asked: nobody asks here */
	static base::ScopedMemoryConsumerRegistry<base::DummyMemoryConsumerRegistry> consumers;
	base::ThreadPoolInstance::CreateAndStartWithDefaultParams("browser");
	static BrDiscardableAllocator discardable;
	base::DiscardableMemoryAllocator::SetInstance(&discardable);
	/* within the process, or as the broker of the browser's processes or one of them (br_proc.cc) */
	br_mojo_init();
}

void start_blink( sk_sp<SkData> face )
{
	base_up();
	skia::OverrideDefaultSkFontMgr(sk_make_sp<BrFontMgr>(face));

	br_start_mark("the locale, the time zone and the threads");
	g_platform = new BrPlatform;
	blink::Platform::InitializeBlink();
	br_start_mark("Blink");
	/*
	 * the runtime features that follow a base::Feature take its state
	 * (on by default where the feature is), as in a browser's renderer
	 */
	blink::WebRuntimeFeatures::UpdateStatusFromBaseFeatures();
	/* what a browser turns on in the engine that Blink keeps off by itself (on Linux): Payment Request */
	blink::WebRuntimeFeatures::EnablePaymentRequest(true);
	/* EyeDropper: the page's window picks the colour (br_pick_color) */
	blink::WebRuntimeFeatures::EnableFeatureFromString("EyeDropperAPI", true);
	/*
	 * Off, as in Chrome on Linux, and on only by a "features=" line: the
	 * Content Index (registration.index, ContentIndex; stable only on
	 * Android), CSS.animationWorklet and WorkletAnimation
	 * (AnimationWorklet: the worklet has a thread of its own, and the
	 * animations are mutated on the main thread as each animation frame
	 * is produced, there being no compositor to mutate on), and
	 * navigator.share (WebShare: Chrome has it on Windows, macOS and
	 * ChromeOS). What they do is built in all the same.
	 */
	/*
	 * navigator.connection: the machine is on a wired network; the
	 * quality is a fixed estimate (the stack does not measure one)
	 */
	blink::WebNetworkStateNotifier::SetOnLine(true);
	blink::WebNetworkStateNotifier::SetWebConnection(blink::kWebConnectionTypeEthernet, 100.0);
	blink::WebNetworkStateNotifier::SetNetworkQuality(blink::WebEffectiveConnectionType::kType4G, base::Milliseconds(50),
							  base::Milliseconds(50), 10000);
	/*
	 * As Google Chrome 153 on a Linux desktop sets them: Web Bluetooth
	 * off (stable only on ChromeOS, Android, macOS and Windows), the touch
	 * events' handlers (ontouchstart and the rest) absent on a machine
	 * without a touch screen; on, the interfaces of service worker
	 * payment apps (PaymentManager), a.hrefTranslate (the browser
	 * translates pages), SerialPort.connected, Request.targetAddressSpace
	 * (local network access) and the URN replacement of Protected
	 * Audience (navigator.deprecatedURNToURL)
	 */
	blink::WebRuntimeFeatures::EnableWebBluetooth(false);
	blink::WebRuntimeFeatures::EnableFeatureFromString("TouchEventFeatureDetection", false);
	blink::WebRuntimeFeatures::EnablePaymentApp(true);
	blink::WebRuntimeFeatures::EnableFeatureFromString("TranslateService", true);
	blink::WebRuntimeFeatures::EnableFeatureFromString("SerialPortConnected", true);
	blink::WebRuntimeFeatures::EnableFeatureFromString("LocalNetworkAccessPermissionPolicy", true);
	blink::WebRuntimeFeatures::EnableFeatureFromString("AllowURNsInIframes", true);
	/*
	 * the animations of background-color on the compositor's thread
	 * (CompositeBGColorAnimation): off, as the browser's own feature of
	 * that name is off by default
	 */
	blink::WebRuntimeFeatures::EnableFeatureFromString("CompositeBGColorAnimation", false);
	/* Web Printing: its interfaces are an isolated context's only (iwa_window) */
	blink::WebRuntimeFeatures::EnableFeatureFromString("WebPrinting", true);
	/*
	 * What Chrome gives isolated web apps only: a window is an isolated
	 * context only when the user lets its origin act as an isolated web
	 * app (iwa_window), and Direct Sockets are turned on in such a
	 * window's document alone (commit_as)
	 */
	blink::g_isolated_context_hook = &iwa_window;
	/* a window's blob: URLs registered and resolved on the store's own thread, as a browser's are */
	if ( &blink::g_window_blob_url_store_hook != nullptr ) blink::g_window_blob_url_store_hook = &br_blob_url_store;
	/* a dedicated worker's animation frames, which no compositor service gives here */
	if ( &blink::g_worker_begin_frame_hook != nullptr ) blink::g_worker_begin_frame_hook = &worker_begin_frame;
	/*
	 * Blink's runtime features a test turns on or off ("features=" lines,
	 * a name for on, -name for off): the ones Chrome keeps for trials
	 * (MediaSourceExtensionsForWebCodecs, among others)
	 */
	for ( size_t at = 0; at < br_features.size(); ) {
		size_t end = br_features.find(',', at);
		std::string f = br_features.substr(at, end == std::string::npos ? std::string::npos : end - at);
		at = ( end == std::string::npos ) ? br_features.size() : end + 1;
		bool on = f.empty() || f[0] != '-';
		if ( !on ) f.erase(0, 1);
		if ( f.empty() ) continue;
		blink::WebRuntimeFeatures::EnableFeatureFromString(f, on);
		printf("browser: runtime feature %s %s\n", f.c_str(), on ? "on" : "off");
	}
	/*
	 * Resolved after the lines, as a browser resolves it after its
	 * switches: the containers of a range on AbstractRange.prototype
	 * (LegacyAbstractRange) only while OpaqueRange is off, so that the two
	 * shapes of the prototypes never come together
	 */
	blink::WebRuntimeFeatures::EnableFeatureFromString("LegacyAbstractRange",
							   !blink::RuntimeEnabledFeatures::OpaqueRangeEnabled());
	printf("browser: features: LockedMode %d WebNN %d Contacts %d SmartCard %d WebAppInstallation %d Handwriting %d"
	       " DigitalGoods %d WindowingControls %d DirectSockets %d WebShare %d ContentIndex %d AnimationWorklet %d"
	       " LanguageDetector %d Translator %d Summarizer %d LanguageModel %d\n",
	       blink::RuntimeEnabledFeatures::LockedModeEnabled(),
	       blink::RuntimeEnabledFeatures::MachineLearningNeuralNetworkEnabled(),
	       blink::RuntimeEnabledFeatures::ContactsManagerEnabled(),
	       blink::RuntimeEnabledFeatures::SmartCardEnabled(),
	       blink::RuntimeEnabledFeatures::WebAppInstallationEnabledByRuntimeFlag(),
	       blink::RuntimeEnabledFeatures::HandwritingRecognitionEnabled(),
	       blink::RuntimeEnabledFeatures::DigitalGoodsEnabledByRuntimeFlag(),
	       blink::RuntimeEnabledFeatures::DesktopPWAsAdditionalWindowingControlsEnabledByRuntimeFlag(),
	       blink::RuntimeEnabledFeatures::DirectSocketsEnabled(),
	       blink::RuntimeEnabledFeatures::WebShareEnabled(),
	       blink::RuntimeEnabledFeatures::ContentIndexEnabled(),
	       blink::RuntimeEnabledFeatures::AnimationWorkletEnabled(),
	       blink::RuntimeEnabledFeatures::LanguageDetectionAPIEnabled(),
	       blink::RuntimeEnabledFeatures::TranslationAPIEnabled(),
	       blink::RuntimeEnabledFeatures::AISummarizationAPIEnabled(),
	       blink::RuntimeEnabledFeatures::AIPromptAPIEnabled());
	g_scheduler = blink::scheduler::WebThreadScheduler::CreateMainThreadScheduler(
		base::MessagePump::Create(base::MessagePumpType::DEFAULT));
	blink::Platform::InitializeMainThread(g_platform, g_scheduler.get());
	/*
	 * Idle periods: a compositor tells the scheduler when a frame begins
	 * and when none is coming soon, and the scheduler runs the idle tasks
	 * (requestIdleCallback, the parser's and V8's idle work) in the time
	 * between. Here no compositor begins frames, so the main thread is
	 * told once that none is expected: it then gives the idle tasks long
	 * idle periods (up to 50 ms each, ending before the next delayed task
	 * is due) whenever it has nothing else to run.
	 */
	static_cast<blink::scheduler::MainThreadSchedulerImpl *>(g_scheduler.get())
		->BeginFrameNotExpectedSoon();
	follow_zone_later();

	/* the images of a CSS paint worklet (paint()), drawn on the page's thread */
	blink::CSSPaintImageGenerator::Init(blink::CSSPaintImageGeneratorImpl::Create);
	/*
	 * the native paint worklets of the animations of background-color and
	 * of clip-path. With no compositor thread a frame makes no generator
	 * of background-color, and an animation of clip-path, found unable to
	 * run on the compositor, is painted on the page's thread
	 */
	blink::BackgroundColorPaintImageGenerator::Init(blink::BackgroundColorPaintImageGeneratorImpl::Create);
	blink::ClipPathPaintImageGenerator::Init(blink::ClipPathPaintImageGeneratorImpl::Create);
	/* the file system a drop's files are entries of (webkitGetAsEntry) */
	blink::DraggedIsolatedFileSystem::Init(blink::DraggedIsolatedFileSystemImpl::PrepareForDataObject);

	br_start_mark("the features and the scheduler");
	static BrCoreInitializer core;
	core.Initialize();
	br_blink_bindings_init();
	br_start_mark("Blink's core");
	/* the contexts of modules a canvas gets here: 2D, drawn by Skia on the CPU */
	blink::HTMLCanvasElement::RegisterRenderingContextFactory(
		std::make_unique<blink::CanvasRenderingContext2D::Factory>());
	/*
	 * and WebGL 1 and 2, drawn by ANGLE on the system's OpenGL ES (WebGL
	 * 1 on the CPU without it: BrWebGLProvider)
	 */
	blink::HTMLCanvasElement::RegisterRenderingContextFactory(blink::WebGLContextFactory::MakeWebGL1());
	blink::HTMLCanvasElement::RegisterRenderingContextFactory(blink::WebGLContextFactory::MakeWebGL2());
	/* a canvas that shows an ImageBitmap as it is given ("bitmaprenderer") */
	blink::HTMLCanvasElement::RegisterRenderingContextFactory(
		std::make_unique<blink::ImageBitmapRenderingContext::Factory>());
	/* WebGPU's ("webgpu"), drawn by Dawn on the system's OpenGL ES (make GL=1) */
	blink::HTMLCanvasElement::RegisterRenderingContextFactory(
		std::make_unique<blink::GPUCanvasContext::Factory>());
	/* and an OffscreenCanvas's contexts, in a window or a worker: 2D on the processor, WebGL, bitmaprenderer, WebGPU */
	blink::OffscreenCanvas::RegisterRenderingContextFactory(
		std::make_unique<blink::OffscreenCanvasRenderingContext2D::Factory>());
	blink::OffscreenCanvas::RegisterRenderingContextFactory(blink::WebGLContextFactory::MakeWebGL1());
	blink::OffscreenCanvas::RegisterRenderingContextFactory(blink::WebGLContextFactory::MakeWebGL2());
	blink::OffscreenCanvas::RegisterRenderingContextFactory(
		std::make_unique<blink::ImageBitmapRenderingContext::Factory>());
	blink::OffscreenCanvas::RegisterRenderingContextFactory(
		std::make_unique<blink::GPUCanvasContext::Factory>());
	blink::V8PerIsolateData::SetTaskAttributionTrackerFactory(
		&blink::scheduler::TaskAttributionTrackerImpl::Create);

	/*
	 * WebAssembly's loads and stores are kept in a module's memory by the
	 * guard pages around it: one past it faults, the system hands the
	 * fault back to the process (ts_def_flt, port/compat/ts_signal.c), and
	 * V8's trap handler makes it the module's trap. Without it the code
	 * checks every access itself.
	 */
	if ( v8::V8::EnableWebAssemblyTrapHandler(true) ) {
		printf("browser: wasm: the trap handler is on\n");
	} else {
		printf("browser: wasm: no trap handler; the code checks its accesses\n");
	}

	/*
	 * V8 compiles JavaScript and WebAssembly to machine code in memory
	 * written and run by turns (port/v8/platform-tessronos.cc); a test
	 * may give it flags of its own (br_v8_flags)
	 */
	/*
	 * SharedArrayBuffer only in a cross-origin isolated context, as in
	 * Chrome: V8 leaves the constructor off the global and asks Blink for
	 * each new context
	 */
	{
		static const char kSabPerContext[] = "--enable-sharedarraybuffer-per-context";
		v8::V8::SetFlagsFromString(kSabPerContext, sizeof(kSabPerContext) - 1);
	}
	blink::V8Initializer::InitializeIsolateHolder(
		blink::V8ContextSnapshot::GetReferenceTable(), br_v8_flags.c_str());
	v8::Isolate *main_iso = blink::V8Initializer::InitializeMainThread();
	if ( main_iso != nullptr && ( "," + br_trace + "," ).find(",exceptions,") != std::string::npos ) {
		main_iso->SetExceptionPropagationCallback(traced_propagation);
	}
	watch_time(main_iso);
	blink::Partitions::InitializeArrayBufferPartition();
	blink::V8Initializer::InitializeInSandboxAllocator();
	br_start_mark("V8 and its snapshot");

	/* the network stack, on its thread, and what it keeps between runs */
	br_net_prepare();
	br_start_mark("the network");
	br_storage_restore();
	br_idb_init();
	br_services_init();
	blink::g_download_url_hook = &save_download;
	br_start_mark("the stores and services");
}

/* Everything the main thread has to do now */
/*
 * The engine thread's tasks run until none is left, or for 100 ms at most:
 * a body that comes in pieces (a video a page fetches, part by part) posts
 * a task for each, and the queue may never be empty while it comes; the
 * thread then goes back to its loop (frames, input) and runs the rest the
 * next time
 */
void run_tasks()
{
	base::RunLoop loop;
	/*
	 * the 100 ms end, by a timer: a loop that ended sooner (the queue
	 * empty) takes its end away with it, and the thread is not woken
	 * later for nothing (br_loop_wait ends at the first task that runs)
	 */
	base::OneShotTimer most;
	most.Start(FROM_HERE, base::Milliseconds(100), loop.QuitClosure());
	loop.QuitWhenIdle();
	loop.Run();
}

/* ---------------------------------------------------------------- keys */

/* The modifiers of the system's keyboard (HID_MOD_*) as Blink's */
int web_mods( unsigned mods )
{
	int m = 0;

	if ( mods & 0x22 ) m |= blink::WebInputEvent::kShiftKey;
	if ( mods & 0x11 ) m |= blink::WebInputEvent::kControlKey;
	if ( mods & 0x44 ) m |= blink::WebInputEvent::kAltKey;
	if ( mods & 0x88 ) m |= blink::WebInputEvent::kMetaKey;
	return m;
}

/* A key's HID usage as the virtual key code pages see (keyCode); 0 for none */
int vk_of( unsigned u )
{
	static const unsigned char punct[] = {		/* 0x2D - 0x38 */
		0xBD, 0xBB, 0xDB, 0xDD, 0xDC, 0xDC, 0xBA, 0xDE, 0xC0, 0xBC, 0xBE, 0xBF
	};
	static const unsigned char nav[] = {		/* 0x49 - 0x52 */
		0x2D, 0x24, 0x21, 0x2E, 0x23, 0x22, 0x27, 0x25, 0x28, 0x26
	};

	if ( u >= 0x04 && u <= 0x1D ) return 'A' + (int)( u - 0x04 );
	if ( u >= 0x1E && u <= 0x26 ) return '1' + (int)( u - 0x1E );
	switch ( u ) {
	case 0x27: return '0';
	case 0x28: case 0x58: return 0x0D;
	case 0x29: return 0x1B;
	case 0x2A: return 0x08;
	case 0x2B: return 0x09;
	case 0x2C: return 0x20;
	case 0x62: return 0x60;
	case 0x87: return 0xE2;
	case 0x89: return 0xDC;
	}
	if ( u >= 0x2D && u <= 0x38 ) return punct[u - 0x2D];
	if ( u >= 0x3A && u <= 0x45 ) return 0x70 + (int)( u - 0x3A );
	if ( u >= 0x49 && u <= 0x52 ) return nav[u - 0x49];
	if ( u >= 0x59 && u <= 0x61 ) return 0x61 + (int)( u - 0x59 );
	return 0;
}

/* What the key is to KeyboardEvent.key: its letter, or its name */
ui::DomKey dom_key_of( unsigned u, unsigned ch )
{
	static const ui::DomKey fkeys[] = {
		ui::DomKey::F1, ui::DomKey::F2, ui::DomKey::F3, ui::DomKey::F4,
		ui::DomKey::F5, ui::DomKey::F6, ui::DomKey::F7, ui::DomKey::F8,
		ui::DomKey::F9, ui::DomKey::F10, ui::DomKey::F11, ui::DomKey::F12
	};

	switch ( u ) {
	case 0x28: case 0x58: return ui::DomKey::ENTER;
	case 0x29: return ui::DomKey::ESCAPE;
	case 0x2A: return ui::DomKey::BACKSPACE;
	case 0x2B: return ui::DomKey::TAB;
	case 0x49: return ui::DomKey::INSERT;
	case 0x4A: return ui::DomKey::HOME;
	case 0x4B: return ui::DomKey::PAGE_UP;
	case 0x4C: return ui::DomKey::DEL;
	case 0x4D: return ui::DomKey::END;
	case 0x4E: return ui::DomKey::PAGE_DOWN;
	case 0x4F: return ui::DomKey::ARROW_RIGHT;
	case 0x50: return ui::DomKey::ARROW_LEFT;
	case 0x51: return ui::DomKey::ARROW_DOWN;
	case 0x52: return ui::DomKey::ARROW_UP;
	}
	if ( u >= 0x3A && u <= 0x45 ) return fkeys[u - 0x3A];
	if ( ch >= 0x20 && ch != 0x7F ) return ui::DomKey::FromCharacter(ch);
	return ui::DomKey::UNIDENTIFIED;
}

void fill_key( blink::WebKeyboardEvent *e, int vk, unsigned usage, unsigned ch )
{
	e->windows_key_code = vk;
	e->native_key_code = (int)usage;
	e->dom_code = (int)( 0x070000 | usage );	/* ui::DomCode is the USB usage */
	e->dom_key = dom_key_of(usage, ch);
}

/* The UTF-16 length of the first 'bytes' bytes of UTF-8 text */
unsigned u16_at( const std::string &text, int bytes )
{
	size_t n = ( bytes < 0 ) ? 0 : ( (size_t)bytes > text.size() ) ? text.size() : (size_t)bytes;
	return blink::String::FromUtf8(std::string_view(text.data(), n)).length();
}

/* ---------------------------------------------------------------- the engine */

class BlinkEngine final : public BrEngine {
public:
	BlinkEngine()
	{
		inval_ = blink::MakeGarbageCollected<blink::RasterInvalidator>(sink_);
		all_.push_back(this);
		blink::g_non_composited_paint_hook = &BlinkEngine::on_paint;
		if ( &blink::g_frame_timing_monitor_hook != nullptr ) {
			blink::g_frame_timing_monitor_hook = &BlinkEngine::timing_of;
		}
		make_page(nullptr, nullptr);
	}

	/* The page of a window another page opened, as large as asked (br_open_window) */
	BlinkEngine( BlinkEngine *opener, blink::LocalFrame *opener_frame, int w, int h, bool pip )
	{
		inval_ = blink::MakeGarbageCollected<blink::RasterInvalidator>(sink_);
		all_.push_back(this);
		opener_ = opener;
		pip_ = pip;
		w_ = w;
		h_ = h;
		make_page(opener, opener_frame);
	}

	~BlinkEngine() override
	{
		all_.erase(std::remove(all_.begin(), all_.end(), this), all_.end());
		for ( BlinkEngine *e : all_ ) {
			if ( e->opener_ == this ) e->opener_ = nullptr;
		}
	}

	/* The engine whose page this is, null when it has gone */
	static BlinkEngine *of( BrPage *p )
	{
		for ( BlinkEngine *e : all_ ) {
			if ( &e->p_ == p ) return e;
		}
		return nullptr;
	}

	/* The new window's page, given to the application at its next look (opened()) */
	void add_opened( BlinkEngine *child, int x, int y )
	{
		BrOpened o;
		o.eng = child;
		o.x = x;
		o.y = y;
		o.w = child->w_;
		o.h = child->h_;
		o.pip = child->pip_;
		opened_.push_back(o);
	}

	int view_w() const { return w_; }
	int view_h() const { return h_; }
	blink::Page *page() const { return p_.page.Get(); }

	/* An extension's own page (its popup, its options): a window of this process, which holds its state */
	void open_page( const std::string &url ) override
	{
		auto *child = new BlinkEngine(this, nullptr, 420, 560, false);
		add_opened(child, -1, -1);
		child->p_.go_to = url;
		printf("browser: an extension's page in a window: %s\n", url.c_str());
	}

	/* The frame of the first engine, the window's own page */
	static blink::LocalFrame *top_frame()
	{
		for ( BlinkEngine *e : all_ ) {
			if ( e->p_.primary && e->p_.page ) return blink::DynamicTo<blink::LocalFrame>(e->p_.page->MainFrame());
		}
		return nullptr;
	}

	bool opened( BrOpened *o ) override
	{
		if ( opened_.empty() ) return false;
		*o = opened_.front();
		opened_.erase(opened_.begin());
		return true;
	}

	std::string apart() override
	{
		if ( p_.apart.empty() ) return std::string();
		std::string to = p_.apart.front();
		p_.apart.erase(p_.apart.begin());
		return to;
	}

	bool close_asked() override { return p_.close_asked; }

	/*
	 * The window gone: the page detached, as a closed window's is (its
	 * opener sees window.closed, a picture-in-picture window's opener is
	 * told it left)
	 */
	void closed() override
	{
		if ( p_.page == nullptr ) return;
		if ( p_.timing ) {
			p_.timing->Shutdown();
			p_.timing = nullptr;
		}
		p_.page->WillBeDestroyed();
		run_tasks();
		p_.frame = nullptr;
		p_.page = nullptr;
	}

	/*
	 * How a document is committed into the session history: as a new
	 * entry, as the entry shown loaded again (its scroll kept) or taken
	 * over, or as the entry a traversal went to (its HistoryItem given
	 * back: the state object, the scroll). The Navigation API is given
	 * the entries of the document's origin next to it, as a browser gives
	 * them (navigation.entries(), canGoBack).
	 */
	void commit_as( blink::WebNavigationParams *params )
	{
		int how = ( load_as_ != BR_LOAD_PAGE ) ? load_as_ : page_load_;
		params->is_browser_initiated = ( load_as_ != BR_LOAD_PAGE );
		/*
		 * Direct Sockets, which a browser turns on in the document of an
		 * isolated web app only: the document of an origin the user lets
		 * act as one
		 */
		{
			scoped_refptr<const blink::SecurityOrigin> o = blink::SecurityOrigin::Create(blink::KURL(params->url));
			bool iwa = o != nullptr && !o->IsOpaque() && br_isolated_allowed(o->ToString().Utf8());
			if ( iwa ) params->modified_runtime_features[blink::mojom::RuntimeFeature::kDirectSockets] = true;
			/* the app's scope, which the windowing controls ask the document to be in: its origin */
			if ( p_.page ) {
				p_.page->GetSettings().SetWebAppScope(iwa ? blink::String::FromUtf8(o->ToString().Utf8() + "/")
									  : blink::String());
			}
		}
		load_as_ = BR_LOAD_PAGE;
		page_load_ = BR_LOAD_NEW;
		nav_left_asked_ = false;

		int target = at_ + 1;		/* where the entry goes in the list */
		blink::WebFrameLoadType type = blink::WebFrameLoadType::kStandard;
		if ( how == BR_LOAD_HISTORY && pending_ >= 0 && pending_ < (int)hist_.size() ) {
			type = blink::WebFrameLoadType::kBackForward;
			params->history_item = blink::WebHistoryItem(hist_[pending_].Get());
			target = pending_;
		} else if ( how == BR_LOAD_RELOAD && at_ >= 0 ) {
			type = blink::WebFrameLoadType::kReload;
			target = at_;
			pending_ = -1;
		} else if ( how == kLoadReplace && at_ >= 0 ) {
			type = blink::WebFrameLoadType::kReplaceCurrentItem;
			target = at_;
			pending_ = -1;
		} else {
			pending_ = -1;
		}
		params->frame_load_type = type;

		scoped_refptr<const blink::SecurityOrigin> origin = blink::SecurityOrigin::Create(params->url);
		auto same = [&]( int i ) {
			return blink::SecurityOrigin::Create(hist_[i]->Url())->IsSameOriginWith(origin.get());
		};
		/* the main frame's items, each once (the entries iframes made share them) */
		int first = target;
		while ( first > 0 && first - 1 < (int)hist_.size() && same(first - 1) ) first--;
		const blink::HistoryItem *last = nullptr;
		for ( int i = first; i < target && i < (int)hist_.size(); i++ ) {
			if ( hist_[i].Get() == last ) continue;
			last = hist_[i].Get();
			params->navigation_api_back_entries.push_back(blink::WebHistoryItem(hist_[i].Get()));
		}
		if ( type != blink::WebFrameLoadType::kStandard ) {
			last = target < (int)hist_.size() ? hist_[target].Get() : nullptr;
			for ( int i = target + 1; i < (int)hist_.size() && same(i); i++ ) {
				if ( hist_[i].Get() == last ) continue;
				last = hist_[i].Get();
				params->navigation_api_forward_entries.push_back(blink::WebHistoryItem(hist_[i].Get()));
			}
		}
	}

	/*
	 * The document is of the response's type: HTML, or what Blink makes a
	 * document of by itself (an image, plain text, a video or a sound,
	 * XML, SVG)
	 */
	void load( const std::string &url, const std::string &body,
		   const std::string &charset, const std::string &type ) override
	{
		std::string mime = type.substr(0, type.find(';'));
		while ( !mime.empty() && ( mime.back() == ' ' || mime.back() == '\t' ) ) mime.pop_back();
		for ( char &ch : mime ) ch = (char)tolower((unsigned char)ch);
		if ( mime.empty() ) mime = "text/html";
		/* a PDF: shown as the page of its pages PDFium draws (port/blink/pdf_tessronos.cc) */
		std::string pdf, cs = charset;
		if ( mime == "application/pdf" || mime == "text/pdf" ) {
			pdf = br_pdf_page(url, body);
			if ( !pdf.empty() ) {
				mime = "text/html";
				cs = "utf-8";
			}
		}
		const std::string &bytes = pdf.empty() ? body : pdf;
		auto params = std::make_unique<blink::WebNavigationParams>();
		params->url = blink::KURL(blink::String::FromUtf8(url));
		params->navigation_timings.navigation_start = base::TimeTicks::Now();
		blink::WebNavigationParams::FillStaticResponse(
			params.get(), blink::WebString::FromUtf8(mime),
			blink::WebString::FromAscii(cs.empty() ? std::string() : cs),
			base::span<const char>(bytes.data(), bytes.size()));
		commit_as(params.get());
		{
			/* an about:blank document keeps the policies of the one before it */
			blink::KURL k(params->url);
			if ( !k.IsAboutBlankUrl() && !k.IsAboutSrcdocUrl() ) params->policy_container = main_policies();
		}
		apply_response(params->response, blink::KURL(blink::String::FromUtf8(url)).GetString().Utf8(), resp_status_, resp_headers_, resp_protocol_, body.size());
		set_agent_cluster(params.get(), resp_headers_, true);
		apply_nav_timing(params.get());
		/* the client hints the origin asked for, now or before */
		br_hints_learn(url, resp_headers_);
		params->enabled_client_hints = br_hints_for(url);
		resp_status_ = 0;
		resp_headers_.clear();
		resp_protocol_.clear();
		p_.go_to.clear();
		/* the opener's page goes: its picture-in-picture window goes with it */
		for ( BlinkEngine *e : all_ ) {
			if ( e->opener_ == this && e->pip_ ) e->p_.close_asked = true;
		}
		if ( iso_.load() == nullptr ) iso_ = v8::Isolate::GetCurrent();
		(void)br_net_fetch_peak();
		base::TimeTicks start = base::TimeTicks::Now(), until = start + base::Seconds(kLoadWaitS);
		g_doc_replacing++;
		p_.frame->Loader().CommitNavigation(std::move(params), nullptr);
		g_doc_replacing--;
		run_tasks();
		/* what the documents before this one still hold, on the console */
		printf("browser: page %.200s committed: %s\n", url.c_str(), memory(false).c_str());
		/*
		 * The page's style sheets, scripts and images come in on the
		 * network thread all at once; the page is shown when they have
		 * (or after kLoadWaitS, when a server is slow to answer)
		 */
		while ( br_net_fetch_outstanding() != 0 && base::TimeTicks::Now() < until ) {
			if ( br_stop_pushed != nullptr && br_stop_pushed() ) {
				/* 停止: the loads cancelled, the page shown with what has come */
				p_.frame->Loader().StopAllLoaders(false);
				break;
			}
			usleep(5000);
			run_tasks();
			/*
			 * The page's frames go on while its parts come: its
			 * animation frame callbacks run, and the lifecycle finds
			 * what the observers see (the images loaded lazily that
			 * come into view are asked for then); it is shown after
			 */
			base::TimeTicks now = base::TimeTicks::Now();
			if ( p_.wants_frame && now - last_frame_ >= base::Milliseconds(kFrameMs) ) {
				last_frame_ = now;
				begin_frame(now);
				update();
			}
		}
		run_tasks();
		unsigned most = br_net_fetch_peak();
		printf("browser: the page's parts in %d ms, %u at most at once\n",
		       (int)( base::TimeTicks::Now() - start ).InMilliseconds(), most);
		scroll_ = 0;
	}

	int layout( int width ) override
	{
		if ( width != w_ ) {
			w_ = width;
			resize();
		}
		update();
		blink::LayoutView *lv = doc() != nullptr ? doc()->GetLayoutView() : nullptr;
		if ( lv == nullptr ) return h_;
		int dh = lv->DocumentRect().Height().ToInt();
		return dh > h_ ? dh : h_;
	}

	int doc_width() override
	{
		blink::LayoutView *lv = doc() != nullptr ? doc()->GetLayoutView() : nullptr;
		return ( lv != nullptr ) ? lv->DocumentRect().Width().ToInt() : 0;
	}

	/* The page's height as layout() gives it, from the layout as it is (after a turn it is up to date) */
	int doc_height() override
	{
		blink::LayoutView *lv = doc() != nullptr ? doc()->GetLayoutView() : nullptr;
		if ( lv == nullptr ) return h_;
		int dh = lv->DocumentRect().Height().ToInt();
		return dh > h_ ? dh : h_;
	}

	/* Whether a document of the page, the main one or an iframe's, has a beforeunload handler */
	bool beforeunload() override
	{
		for ( blink::Frame *f = p_.page != nullptr ? p_.page->MainFrame() : nullptr; f != nullptr; f = f->Tree().TraverseNext() ) {
			auto *lf = blink::DynamicTo<blink::LocalFrame>(f);
			if ( lf != nullptr && lf->DomWindow() != nullptr
			     && lf->DomWindow()->HasEventListeners(blink::event_type_names::kBeforeunload) ) {
				return true;
			}
		}
		return false;
	}

	int scroll_left() override
	{
		blink::PaintLayerScrollableArea *sa = view()->LayoutViewport();
		return ( sa != nullptr ) ? (int)sa->GetScrollOffset().x() : 0;
	}

	void set_scroll_left( int x ) override
	{
		blink::PaintLayerScrollableArea *sa = view()->LayoutViewport();
		if ( sa == nullptr ) return;
		sa->SetScrollOffset(blink::ScrollOffset((float)x, sa->GetScrollOffset().y()),
				    blink::mojom::blink::ScrollType::kProgrammatic,
				    cc::ScrollSourceType::kNone);
	}

	void paint( SkCanvas *c, int top, int height ) override
	{
		if ( !fresh_ || top != scroll_ || height != h_ ) {
			if ( height != h_ ) {
				h_ = height;
				resize();
			}
			scroll_to(top);
			update();
		}
		/* one record for every rectangle painted of the same state */
		if ( !rec_ok_ ) {
			rec_ = view()->GetPaintRecord();
			rec_ok_ = true;
		}
		c->save();
		c->clear(SK_ColorWHITE);
		rec_.Playback(c);
		c->restore();
	}

	/* The same record, rastered on the GPU's thread into the rectangles and read back */
	bool paint_gpu( const std::vector<BrRect> &rects, int top, int width, int height, void *px,
			size_t pitch ) override
	{
		if ( br_gpu_raster == nullptr ) return false;
		if ( !fresh_ || top != scroll_ || height != h_ ) {
			if ( height != h_ ) {
				h_ = height;
				resize();
			}
			scroll_to(top);
			update();
		}
		if ( !rec_ok_ ) {
			rec_ = view()->GetPaintRecord();
			rec_ok_ = true;
		}
		return br_gpu_raster(rec_, rects, width, height, px, pitch);
	}

	/* What changed since the last call, gathered at each paint of the page (painted()) */
	bool damage( int top, int height, std::vector<BrRect> *out ) override
	{
		if ( height != h_ ) {
			h_ = height;
			resize();
		}
		scroll_to(top);
		update();
		fresh_ = true;
		out->swap(changed_);
		changed_.clear();
		return true;
	}

	/*
	 * After each paint of the page (the lifecycle's paint, which tells of
	 * it through g_non_composited_paint_hook while the artifact it
	 * replaced is still whole): the raster invalidations a compositor
	 * would be given for one layer as large as the view, with the root's
	 * properties. They are the chunks of the paint artifact that
	 * appeared, went, moved or changed, and the display items in them
	 * painted anew, each as its bounds on the view, old and new; a
	 * scroll moves every chunk under the scroll's translation, so the
	 * view is painted whole then. The properties' changed flags are
	 * cleared after, as a compositor clears them after each update, so
	 * that a chunk whose properties have not changed since is passed over.
	 */
	void painted( blink::LocalFrameView &v )
	{
		if ( &v != view() ) return;
		const blink::PaintArtifact &art = v.GetPaintArtifact();
		sink_.out = &changed_;
		sink_.w = w_;
		sink_.h = h_;
		inval_->Generate(blink::PaintChunkSubset(art), gfx::Vector2dF(), gfx::Size(w_, h_),
				 blink::PropertyTreeState::Root());
		sink_.out = nullptr;
		if ( ++seq_ <= 0 ) seq_ = 1;
		for ( const blink::PaintChunk &ch : art.GetPaintChunks() ) {
			ch.properties.ClearChangedToRoot(seq_);
		}
		/* many small ones are kept as the rectangle round them */
		if ( changed_.size() > kChangedMax ) {
			BrRect u = changed_[0];
			for ( const BrRect &r : changed_ ) {
				int x1 = std::max(u.x + u.w, r.x + r.w), y1 = std::max(u.y + u.h, r.y + r.h);
				u.x = std::min(u.x, r.x);
				u.y = std::min(u.y, r.y);
				u.w = x1 - u.x;
				u.h = y1 - u.y;
			}
			changed_.assign(1, u);
		}
	}

	static void on_paint( blink::LocalFrameView &v )
	{
		for ( BlinkEngine *e : all_ ) {
			if ( e->p_.frame != nullptr && e->view() == &v ) e->painted(v);
		}
	}

	/* The monitor of the animation frames of a frame's page (BrLongTasks) */
	static blink::AnimationFrameTimingMonitor *timing_of( blink::LocalFrame &f )
	{
		for ( BlinkEngine *e : all_ ) {
			if ( e->p_.page != nullptr && e->p_.page.Get() == f.GetPage() ) return e->p_.timing.Get();
		}
		return nullptr;
	}

	std::string link_at( int x, int y ) override
	{
		if ( y < scroll_ || y >= scroll_ + h_ ) {
			/* the point brought into the view */
			scroll_to(y - h_ / 2 > 0 ? y - h_ / 2 : 0);
		}
		update();
		int vy = y - scroll_;
		blink::HitTestLocation loc(gfx::PointF((float)x, (float)vy));
		blink::HitTestResult r = p_.frame->GetEventHandler().HitTestResultAtLocation(loc);
		blink::KURL u = r.AbsoluteLinkURL();
		return u.IsValid() ? std::string(u.GetString().Utf8()) : std::string();
	}

	bool click( int x, int y ) override
	{
		if ( y < scroll_ || y >= scroll_ + h_ ) {
			scroll_to(y - h_ / 2 > 0 ? y - h_ / 2 : 0);
		}
		update();
		gfx::PointF at((float)x, (float)( y - scroll_ ));
		blink::WebMouseEvent down(blink::WebInputEvent::Type::kMouseDown, at, at,
					  blink::WebPointerProperties::Button::kLeft, 1,
					  blink::WebInputEvent::kLeftButtonDown, base::TimeTicks::Now());
		down.SetFrameScale(1);
		p_.frame->GetEventHandler().HandleMousePressEvent(down);
		blink::WebMouseEvent up(blink::WebInputEvent::Type::kMouseUp, at, at,
					blink::WebPointerProperties::Button::kLeft, 1,
					blink::WebInputEvent::kNoModifiers, base::TimeTicks::Now());
		up.SetFrameScale(1);
		p_.frame->GetEventHandler().HandleMouseReleaseEvent(up);
		run_tasks();
		return true;
	}

	/*
	 * The pointer moves or lets go while the page drags something: the
	 * page is where it may go, as for a drag the browser carries over
	 * its window. Let go where the page takes it, it is dropped there;
	 * elsewhere the drag is left.
	 */
	void drag_mouse( int type, gfx::PointF at, unsigned mods )
	{
		blink::DragController &dc = p_.frame->GetPage()->GetDragController();
		p_.drag->SetModifiers(web_mods(mods));
		blink::DragData data(p_.drag.Get(), at, at, p_.drag_ops, false);

		if ( type == BR_M_MOVE ) {
			p_.drag_op = dc.DragEnteredOrUpdated(&data, *p_.frame);
			if ( ( (int)p_.drag_op.operation & (int)p_.drag_ops ) == 0 ) {
				p_.drag_op = blink::DragController::Operation();
			}
			return;
		}
		if ( type == BR_M_DOWN ) return;
		ui::mojom::blink::DragOperation done = p_.drag_op.operation;
		if ( done != ui::mojom::blink::DragOperation::kNone ) {
			dc.PerformDrop(&data, *p_.frame, p_.drag_op);
		} else {
			dc.DragExited(&data, *p_.frame);
		}
		printf("browser: the page's drag let go (%s)\n",
		       done != ui::mojom::blink::DragOperation::kNone ? "dropped" : "not taken");
		drag_end(at, done);
	}

	/* The page's drag over: its source told how it ended */
	void drag_end( gfx::PointF at, ui::mojom::blink::DragOperation done )
	{
		blink::DragController &dc = p_.frame->GetPage()->GetDragController();
		blink::WebMouseEvent ev(blink::WebInputEvent::Type::kMouseMove, at, at,
					blink::WebPointerProperties::Button::kLeft, 0,
					blink::WebInputEvent::kNoModifiers, base::TimeTicks::Now());
		ev.SetFrameScale(1);
		p_.frame->GetEventHandler().DragSourceEndedAt(ev, done);
		dc.DragEnded();
		p_.drag = nullptr;
		p_.drag_op = blink::DragController::Operation();
		down_ = false;
	}

	std::string dragging() override
	{
		if ( p_.drag == nullptr ) return std::string();
		blink::String t = p_.drag->GetData("text/uri-list");
		if ( t.empty() ) t = p_.drag->GetData("text/plain");
		if ( t.empty() ) t = p_.drag->GetData("text/html");
		std::string out = t.Utf8();
		return out.empty() ? std::string(" ") : out;
	}

	void drag_away( bool taken ) override
	{
		if ( p_.drag == nullptr ) return;
		blink::DragData data(p_.drag.Get(), gfx::PointF(-1, -1), gfx::PointF(-1, -1), p_.drag_ops, false);
		p_.frame->GetPage()->GetDragController().DragExited(&data, *p_.frame);
		printf("browser: the page's drag carried out of the window (%s)\n", taken ? "taken" : "not taken");
		drag_end(gfx::PointF(-1, -1), taken ? ui::mojom::blink::DragOperation::kCopy
						      : ui::mojom::blink::DragOperation::kNone);
		run_tasks();
	}

	bool drop( int x, int y, const std::vector<std::pair<std::string, std::string>> &files,
		   const std::string &text, unsigned mods ) override
	{
		update();
		blink::DataObject *d = blink::DataObject::Create();
		for ( const auto &f : files ) {
			d->AddFilename(p_.frame->DomWindow(), blink::String::FromUtf8(f.first),
				       blink::String::FromUtf8(f.second), blink::String(),
				       base::MakeRefCounted<blink::FileSystemAccessDropData>(
					       br_fsa_drop_token(f.first, f.second)));
		}
		if ( !text.empty() ) d->SetData("text/plain", blink::String::FromUtf8(text));
		d->SetModifiers(web_mods(mods));
		gfx::PointF at((float)x, (float)y);
		blink::DragData data(d, at, at, blink::kDragOperationCopy, false);
		blink::DragController &dc = p_.frame->GetPage()->GetDragController();
		/* in, then over the place, as a browser tells a drag from outside before its drop */
		(void)dc.DragEnteredOrUpdated(&data, *p_.frame);
		blink::DragController::Operation op = dc.DragEnteredOrUpdated(&data, *p_.frame);
		bool took = ( (int)op.operation & (int)blink::kDragOperationCopy ) != 0;
		if ( took ) {
			dc.PerformDrop(&data, *p_.frame, op);
			p_.frame->GetEventHandler().ReportDragEnd();
		} else {
			dc.DragExited(&data, *p_.frame);
		}
		printf("browser: %zu files and %zu letters let go over the page: %s\n", files.size(), text.size(),
		       took ? "dropped" : "not taken");
		run_tasks();
		return took;
	}

	bool print() override
	{
		if ( doc() == nullptr || doc()->GetFrame() == nullptr ) return false;
		return br_print_frame(doc()->GetFrame(), doc()->Url().GetString().Utf8());
	}

	std::string first_link() override
	{
		if ( doc() == nullptr ) return std::string();
		blink::HTMLCollection *links = doc()->links();
		for ( unsigned i = 0; links != nullptr && i < links->length(); i++ ) {
			auto *a = blink::DynamicTo<blink::HTMLAnchorElement>(links->item(i));
			if ( a != nullptr && a->Href().IsValid() ) return a->Href().GetString().Utf8();
		}
		return std::string();
	}

	std::string title() override
	{
		return doc() != nullptr ? std::string(doc()->title().Utf8()) : std::string();
	}

	bool pump() override
	{
		if ( iso_.load() == nullptr ) iso_ = v8::Isolate::GetCurrent();
		run_tasks();
		br_media_tick();
		/*
		 * A frame the page asked for begins at a refresh of the screen
		 * (kRefreshUs, 60 Hz), the first one at or after the last frame
		 * and kCostShare times what that frame took on this thread (its
		 * callbacks, the lifecycle, the paint and the showing): a page
		 * whose frames cost much is painted less often, and its scripts
		 * and loads keep their share of the thread
		 */
		base::TimeTicks now = base::TimeTicks::Now();
		if ( !p_.wants_frame || now < next_frame_ ) return false;
		last_frame_ = now;
		frame_began_ = now;
		begin_frame(now);
		return true;
	}

	void frame_shown() override
	{
		/* what asked when the frame would be on the screen: now, a frame of the refresh's length */
		if ( !p_.presented.empty() ) {
			std::vector<blink::ChromeClient::ReportTimeCallback> told;
			told.swap(p_.presented);
			viz::FrameTimingDetails details;
			details.presentation_feedback = gfx::PresentationFeedback(base::TimeTicks::Now(),
										  base::Microseconds(kRefreshUs), 0);
			for ( auto &cb : told ) std::move(cb).Run(details);
		}
		if ( frame_began_.is_null() ) return;
		base::TimeTicks now = base::TimeTicks::Now();
		base::TimeDelta cost = ( now - frame_began_ ) * kCostShare;
		base::TimeDelta refresh = base::Microseconds(kRefreshUs);
		frame_began_ = base::TimeTicks();
		next_frame_ = next_refresh(last_frame_ + ( cost > refresh ? cost : refresh )
					   - base::Microseconds(kEarlyUs));
	}

	/*
	 * The page's tasks and timers run while the loop waits (br_loop_wait);
	 * what is due besides them is the frame the page asked for, and the
	 * next look of a player that plays (br_media_tick: the frame of the
	 * time now, from the media's threads)
	 */
	int idle_ms() override
	{
		int most = br_media_wants_ticks() ? kMediaTickMs : BR_WAIT_MOST_MS;
		if ( !p_.wants_frame ) return most;
		int64_t ms = ( next_frame_ - base::TimeTicks::Now() ).InMilliseconds();
		return ms < 1 ? 1 : ms > most ? most : (int)ms;
	}

	/*
	 * From the window's thread while this one is held: the script that
	 * runs now is ended (V8 lets any thread ask it to), and where the
	 * page's script is is printed by the engine's thread itself when it
	 * next looks for interrupts, which a script running on does often
	 */
	void stop_script() override
	{
		v8::Isolate *iso = iso_.load();
		if ( iso == nullptr ) return;
		printf("browser: the page's script is ended\n");
		iso->TerminateExecution();
	}

	void report_script() override
	{
		v8::Isolate *iso = iso_.load();
		if ( iso != nullptr ) iso->RequestInterrupt(&BlinkEngine::print_script, nullptr);
	}

	static void print_script( v8::Isolate *iso, void * )
	{
		v8::HandleScope hs(iso);
		v8::Local<v8::StackTrace> st = v8::StackTrace::CurrentStackTrace(iso, 8);
		int n = st->GetFrameCount();
		printf("browser: the page's script runs, %d frames:\n", n);
		for ( int i = 0; i < n; i++ ) {
			v8::Local<v8::StackFrame> f = st->GetFrame(iso, (uint32_t)i);
			v8::String::Utf8Value fn(iso, f->GetFunctionName());
			v8::String::Utf8Value sn(iso, f->GetScriptName());
			std::string where = *sn != nullptr ? *sn : "?";
			if ( where.size() > 160 ) where.resize(160);
			printf("browser:   %s (%s:%d:%d)\n", *fn != nullptr && **fn != 0 ? *fn : "(anonymous)", where.c_str(),
			       f->GetLineNumber(), f->GetColumn());
		}
	}

	/* The first refresh at or after t, counted at kRefreshUs from when the engine began */
	base::TimeTicks next_refresh( base::TimeTicks t ) const
	{
		if ( t <= refresh_origin_ ) return refresh_origin_;
		int64_t us = ( t - refresh_origin_ ).InMicroseconds();
		int64_t n = ( us + kRefreshUs - 1 ) / kRefreshUs;
		return refresh_origin_ + base::Microseconds(n * kRefreshUs);
	}

	/*
	 * The steps of the event loop's rendering update that come before
	 * the lifecycle, as a widget runs them at each frame it begins: the
	 * resize and scroll events, media queries' listeners, the animations
	 * and their events, and the callbacks of requestAnimationFrame. The
	 * frame asked for is taken first, so that a callback that asks for
	 * the next one is not lost; the lifecycle (style, layout, paint, then
	 * the intersection and resize observers) follows when the frame is
	 * painted (layout(), damage()). The animation clock is let go again
	 * when no callback waits for the next frame.
	 */
	void begin_frame( base::TimeTicks now )
	{
		p_.wants_frame = false;
		if ( doc() == nullptr ) return;
		/* the frame's rendering begins, for its timing (BrLongTasks) */
		if ( p_.timing != nullptr && &blink::g_frame_timing_monitor_hook != nullptr ) {
			p_.timing->BeginMainFrame(*p_.frame->DomWindow(), viz::BeginFrameId(kBeginFrameSource, ++begin_frames_));
		}
		blink::PageAnimator &a = p_.page->Animator();
		{
			FrameTime t(&g_turn.frames);
			a.ServiceScriptedAnimations(now);
		}
		run_tasks();
		a.PostAnimate();
	}

	std::string navigation() override
	{
		std::string to;
		to.swap(p_.go_to);
		if ( !to.empty() ) {
			nav_method_ = std::move(p_.go_method);
			nav_ctype_ = std::move(p_.go_ctype);
			nav_body_ = std::move(p_.go_body);
			nav_left_asked_ = p_.left_asked;
			switch ( p_.go_type ) {
			case blink::WebFrameLoadType::kBackForward:		page_load_ = BR_LOAD_HISTORY; break;
			case blink::WebFrameLoadType::kReload:
			case blink::WebFrameLoadType::kReloadBypassingCache:	page_load_ = BR_LOAD_RELOAD; break;
			case blink::WebFrameLoadType::kReplaceCurrentItem:	page_load_ = kLoadReplace; break;
			default:						page_load_ = BR_LOAD_NEW; break;
			}
		}
		p_.left_asked = false;
		p_.go_type = blink::WebFrameLoadType::kStandard;
		p_.go_method.clear();
		p_.go_ctype.clear();
		p_.go_body.clear();
		return to;
	}

	bool navigation_body( std::string *method, std::string *ctype, std::string *body ) override
	{
		if ( nav_method_ != "POST" ) return false;
		*method = nav_method_;
		*ctype = nav_ctype_;
		*body = nav_body_;
		return true;
	}

	/* ------------------------------------------------ the session history */

	bool history() override { return true; }

	/*
	 * The entry 'offset' from the one shown, -1 when there is none. The
	 * window's back and forward ('by_user') pass over the entries an iframe
	 * made with no user activation (an advertisement's frame moving on its
	 * own): going back undoes the entries up to and including the latest
	 * one made otherwise, going forward redoes up to the next such one.
	 */
	int target_of( int offset, bool by_user ) const
	{
		if ( offset == 0 || at_ < 0 ) return -1;
		if ( !by_user ) {
			int to = at_ + offset;
			return ( to >= 0 && to < (int)hist_.size() ) ? to : -1;
		}
		int to = at_;
		for ( int n = offset < 0 ? -offset : offset; n > 0; n-- ) {
			if ( offset < 0 ) {
				int k = to;
				while ( k > 0 && skip_[k] ) k--;
				if ( k <= 0 ) return -1;
				to = k - 1;
			} else {
				int k = to + 1;
				while ( k < (int)hist_.size() && skip_[k] ) k++;
				if ( k >= (int)hist_.size() ) return -1;
				to = k;
			}
		}
		return to;
	}

	bool can_go( int offset ) override { return target_of(offset, true) >= 0; }

	int history_go( int offset ) override
	{
		int r = go_entry(offset, true);
		run_tasks();
		return r;
	}

	/* An iframe's item in an entry, null when the entry does not know its document */
	blink::HistoryItem *sub_of( int i, const blink::LocalFrame *f ) const
	{
		for ( const FrameItem &fi : subs_[(size_t)i] ) {
			if ( fi.frame.Get() == f ) return fi.item.Get();
		}
		return nullptr;
	}

	/*
	 * To the entry 'offset' from the one shown. Each frame whose document
	 * differs there goes to it: one of the document it shows (the same
	 * document sequence number: its pushState's, its fragments') is
	 * committed in it from a task of its own, as history traversals are,
	 * which fires its navigate, popstate and hashchange events; another
	 * document is fetched (the main frame's by the application, an
	 * iframe's by its client) and committed as that entry (its state
	 * object, its scroll). A page's traversal asked for while another is
	 * under way waits for it, and its entry is reckoned from where that
	 * one ends (history.go() twice goes twice as far).
	 */
	int go_entry( int offset, bool by_user )
	{
		if ( !by_user && pending_ >= 0 && base::TimeTicks::Now() - pending_since_ < base::Seconds(10) ) {
			queued_go_.push_back(offset);
			return BR_HIST_DONE;
		}
		int to = target_of(offset, by_user);
		if ( to < 0 ) return BR_HIST_NONE;
		blink::HistoryItem *target = hist_[to].Get(), *now = hist_[at_].Get();
		pending_ = to;
		pending_since_ = base::TimeTicks::Now();
		if ( target != now ) {
			blink::DocumentLoader *dl = p_.frame->Loader().GetDocumentLoader();
			if ( !( dl != nullptr && doc() != nullptr
				&& target->DocumentSequenceNumber() == now->DocumentSequenceNumber()
				&& p_.frame->IsNavigationAllowed() ) ) {
				/* another document in the main frame: its iframes load anew in it */
				p_.go_to = target->Url().GetString().Utf8();
				p_.go_method.clear();
				p_.go_ctype.clear();
				p_.go_body.clear();
				p_.go_type = blink::WebFrameLoadType::kBackForward;
				p_.left_asked = false;
				return BR_HIST_FETCH;
			}
			p_.frame->GetTaskRunner(blink::TaskType::kInternalNavigationAssociated)
				->PostTask(FROM_HERE, base::BindOnce(&BlinkEngine::commit_entry, base::Unretained(this), to,
								     by_user));
		}
		bool moved = target != now;
		std::vector<blink::LocalFrame *> asked;
		for ( const FrameItem &fi : subs_[(size_t)to] ) {
			blink::LocalFrame *f = fi.frame.Get();
			if ( f == nullptr || f == p_.frame || f->Client() == nullptr || fi.item == nullptr ) continue;
			if ( std::find(asked.begin(), asked.end(), f) != asked.end() ) continue;
			asked.push_back(f);
			blink::DocumentLoader *fdl = f->Loader().GetDocumentLoader();
			blink::HistoryItem *cur = fdl != nullptr ? fdl->GetHistoryItem() : nullptr;
			if ( cur == fi.item.Get() ) continue;
			moved = true;
			bool same = cur != nullptr && cur->DocumentSequenceNumber() == fi.item->DocumentSequenceNumber()
				    && f->IsNavigationAllowed();
			f->GetTaskRunner(blink::TaskType::kInternalNavigationAssociated)->PostTask(FROM_HERE, base::BindOnce(
				[]( BlinkEngine *e, blink::WeakPersistent<blink::LocalFrame> f,
				    blink::Persistent<blink::HistoryItem> item, int to, bool same, bool by_user ) {
					e->commit_frame_entry(f.Get(), item.Get(), to, same, by_user);
				}, base::Unretained(this), blink::WeakPersistent<blink::LocalFrame>(f),
				blink::Persistent<blink::HistoryItem>(fi.item.Get()), to, same, by_user));
		}
		if ( !moved ) {
			/* nothing shown differs (the frames that made the entries are gone): the entry is the one shown */
			at_ = to;
			traversed();
		}
		return BR_HIST_DONE;
	}

	/* A traversal ended (its entry committed): the next one the page asked for meanwhile goes */
	void traversed()
	{
		pending_ = -1;
		if ( queued_go_.empty() || p_.frame == nullptr ) return;
		int offset = queued_go_.front();
		queued_go_.erase(queued_go_.begin());
		p_.frame->GetTaskRunner(blink::TaskType::kInternalNavigationAssociated)->PostTask(FROM_HERE, base::BindOnce(
			[]( BlinkEngine *e, int offset ) { (void)e->go_entry(offset, false); }, base::Unretained(this), offset));
	}

	/* An iframe to its item of the entry 'to', in the document it shows or as a document fetched again */
	void commit_frame_entry( blink::LocalFrame *f, blink::HistoryItem *item, int to, bool same, bool by_user )
	{
		if ( f == nullptr || item == nullptr || f->Client() == nullptr || ( to != pending_ && to != at_ ) ) return;
		blink::DocumentLoader *dl = f->Loader().GetDocumentLoader();
		/* there already (another traversal took it there meanwhile) */
		if ( dl != nullptr && dl->GetHistoryItem() == item ) return;
		if ( same && dl != nullptr && f->IsNavigationAllowed() ) {
			blink::mojom::CommitResult r = dl->CommitSameDocumentNavigation(
				item->Url(), blink::WebFrameLoadType::kBackForward, item,
				blink::ClientRedirectPolicy::kNotClientRedirect, false, nullptr, false, nullptr,
				blink::mojom::blink::TriggeringEventInfo::kNotFromEvent, by_user, false, std::nullopt, false);
			printf("browser: history: a frame to entry %d of %zu, %s (%d)\n", to, hist_.size(),
			       item->Url().GetString().Utf8().c_str(), (int)r);
			if ( r != blink::mojom::CommitResult::RestartCrossDocument ) return;
		}
		static_cast<BrChildFrameClient *>(f->Client())->GoToEntry(item);
	}

	void commit_entry( int to, bool by_user )
	{
		if ( to != pending_ || to < 0 || to >= (int)hist_.size() || p_.frame == nullptr ) return;
		blink::DocumentLoader *dl = p_.frame->Loader().GetDocumentLoader();
		if ( dl == nullptr || !p_.frame->IsNavigationAllowed() ) return;
		blink::HistoryItem *item = hist_[to].Get();
		printf("browser: history: to entry %d of %zu, %s\n", to, hist_.size(), item->Url().GetString().Utf8().c_str());
		blink::mojom::CommitResult r = dl->CommitSameDocumentNavigation(
			item->Url(), blink::WebFrameLoadType::kBackForward, item,
			blink::ClientRedirectPolicy::kNotClientRedirect, false, nullptr, false, nullptr,
			blink::mojom::blink::TriggeringEventInfo::kNotFromEvent, by_user, false, std::nullopt, false);
		if ( r == blink::mojom::CommitResult::RestartCrossDocument ) {
			/* a frameset, or the document gone meanwhile: the entry fetched as a document of its own */
			p_.go_to = item->Url().GetString().Utf8();
			p_.go_type = blink::WebFrameLoadType::kBackForward;
			p_.left_asked = false;
		}
	}

	/*
	 * A document committed, or a move within the one shown, in the main
	 * frame: a new entry after the one shown (the entries after it go),
	 * the entry gone to, or the one shown taken over (a reload, a
	 * replaceState, location.replace())
	 */
	void committed( blink::HistoryItem *item, blink::WebHistoryCommitType type )
	{
		if ( item == nullptr ) return;
		if ( type == blink::kWebStandardCommit || at_ < 0 ) {
			cut_after(at_);
			hist_.emplace_back(item);
			subs_.emplace_back();
			skip_.push_back(false);
			at_ = (int)hist_.size() - 1;
		} else if ( type == blink::kWebBackForwardCommit && pending_ >= 0 && pending_ < (int)hist_.size() ) {
			at_ = pending_;
			main_taken_over(item);
		} else {
			main_taken_over(item);
		}
		keep_last();
		traversed();
	}

	/*
	 * An iframe's commit, or a move within its document: a new entry after
	 * the one shown (the main frame's item and the other iframes' as they
	 * are), the entry gone to, or the iframe's item in the entry shown
	 * taken over (its first document, a reload, a replaceState)
	 */
	void frame_committed( blink::LocalFrame *f, blink::HistoryItem *item, blink::WebHistoryCommitType type )
	{
		if ( f == nullptr || item == nullptr || at_ < 0 || at_ >= (int)hist_.size() ) return;
		printf("browser: history: a frame's %s %s\n",
		       type == blink::kWebStandardCommit ? "new entry" : type == blink::kWebBackForwardCommit ? "entry gone to"
												 : "entry taken over",
		       item->Url().GetString().Utf8().c_str());
		if ( type == blink::kWebStandardCommit ) {
			cut_after(at_);
			hist_.push_back(hist_[(size_t)at_]);
			subs_.push_back(subs_[(size_t)at_]);
			skip_.push_back(!f->HasStickyUserActivation());
			at_ = (int)hist_.size() - 1;
			set_sub(at_, f, item);
		} else {
			bool went = false;
			if ( type == blink::kWebBackForwardCommit && pending_ >= 0 && pending_ < (int)hist_.size() ) {
				at_ = pending_;
				went = true;
			}
			blink::HistoryItem *old = sub_of(at_, f);
			if ( old == nullptr ) {
				set_sub(at_, f, item);
			} else {
				/* every entry that had the iframe's item has the new one (the same document state) */
				for ( std::vector<FrameItem> &s : subs_ ) {
					for ( FrameItem &fi : s ) {
						if ( fi.frame.Get() == f && fi.item.Get() == old ) fi.item = item;
					}
				}
			}
			if ( went ) {
				keep_last();
				traversed();
				return;
			}
		}
		keep_last();
	}

	/*
	 * An iframe taken away (not with its parent's document going): its
	 * items leave the entries, and an entry it alone made goes with them
	 */
	void frame_gone( blink::LocalFrame *f )
	{
		for ( std::vector<FrameItem> &s : subs_ ) {
			s.erase(std::remove_if(s.begin(), s.end(), [f]( const FrameItem &fi ) {
				return fi.frame.Get() == f || fi.frame.Get() == nullptr;
			}), s.end());
		}
		for ( int i = (int)hist_.size() - 1; i > 0; i-- ) {
			if ( !same_entry(i, i - 1) ) continue;
			hist_.erase(hist_.begin() + i);
			subs_.erase(subs_.begin() + i);
			skip_.erase(skip_.begin() + i);
			if ( at_ >= i ) at_--;
			if ( pending_ >= i ) pending_--;
		}
	}

	unsigned history_length() const { return hist_.empty() ? 1u : (unsigned)hist_.size(); }

	/* The entries after 'at' gone (a new entry after it) */
	void cut_after( int at )
	{
		if ( at < 0 ) return;
		hist_.resize((size_t)at + 1);
		subs_.resize((size_t)at + 1);
		skip_.resize((size_t)at + 1);
	}

	/* The main frame's item in the entry shown, and in the entries of the same item, taken over */
	void main_taken_over( blink::HistoryItem *item )
	{
		blink::HistoryItem *old = hist_[(size_t)at_].Get();
		for ( auto &h : hist_ ) {
			if ( h.Get() == old ) h = item;
		}
	}

	void set_sub( int i, blink::LocalFrame *f, blink::HistoryItem *item )
	{
		for ( FrameItem &fi : subs_[(size_t)i] ) {
			if ( fi.frame.Get() == f ) {
				fi.item = item;
				return;
			}
		}
		subs_[(size_t)i].push_back({ blink::WeakPersistent<blink::LocalFrame>(f), blink::Persistent<blink::HistoryItem>(item) });
	}

	/* Whether two entries show the same: the main frame's item and each iframe's */
	bool same_entry( int a, int b ) const
	{
		if ( hist_[(size_t)a].Get() != hist_[(size_t)b].Get() || subs_[(size_t)a].size() != subs_[(size_t)b].size() ) {
			return false;
		}
		for ( const FrameItem &fi : subs_[(size_t)a] ) {
			if ( sub_of(b, fi.frame.Get()) != fi.item.Get() ) return false;
		}
		return true;
	}

	/* the oldest go once there are many, as a browser's list keeps its last 50 */
	void keep_last()
	{
		while ( hist_.size() > kHistMax ) {
			hist_.erase(hist_.begin());
			subs_.erase(subs_.begin());
			skip_.erase(skip_.begin());
			at_--;
			if ( pending_ >= 0 ) pending_--;
		}
	}

	void load_as( int how ) override { load_as_ = how; }

	/*
	 * The entries of the main frame, as the browser keeps them when this
	 * renderer is to go: each item's page state (its address, its state
	 * object, its scroll, its form's state, its navigation API key)
	 */
	bool history_save( std::vector<std::string> *states, std::vector<std::string> *urls, int *at ) override
	{
		states->clear();
		urls->clear();
		for ( const auto &h : hist_ ) {
			states->push_back(h.Get() != nullptr ? h->ToPageState().ToEncodedData() : std::string());
			urls->push_back(h.Get() != nullptr ? std::string(h->UrlString().Utf8()) : std::string());
		}
		*at = at_;
		return !hist_.empty();
	}

	void history_cut() override { cut_after(at_); }

	/*
	 * A history kept by the browser given to this new page: its entries
	 * made items again, the one 'at' committed by the next load as an
	 * entry gone to (its state object and scroll restored, as a browser
	 * restores a session)
	 */
	void history_restore( const std::vector<std::string> &states, int at ) override
	{
		if ( states.empty() || at < 0 || at >= (int)states.size() ) return;
		hist_.clear();
		subs_.clear();
		skip_.clear();
		for ( const std::string &s : states ) {
			hist_.emplace_back(blink::HistoryItem::Create(blink::PageState::CreateFromEncodedData(s)));
			subs_.emplace_back();
			skip_.push_back(false);
		}
		at_ = at;
		pending_ = at;
		pending_since_ = base::TimeTicks::Now();
		load_as_ = BR_LOAD_HISTORY;
	}

	void response( int status, const std::string &headers, const std::string &protocol ) override
	{
		resp_status_ = status;
		resp_headers_ = headers;
		resp_protocol_ = protocol;
	}

	std::string url() override
	{
		return doc() != nullptr ? std::string(doc()->Url().GetString().Utf8()) : std::string();
	}

	bool may_leave( bool reload ) override
	{
		if ( load_as_ == BR_LOAD_PAGE && nav_left_asked_ ) return true;	/* asked when the page began it */
		if ( doc() == nullptr || doc()->IsInitialEmptyDocument() ) return true;
		base::TimeTicks opened, closed;
		bool ok = p_.frame->Loader().ShouldClose(reload, false, opened, closed);
		run_tasks();
		return ok;
	}

	std::string hover() override
	{
		return !p_.hover_link.empty() ? p_.hover_link : p_.tooltip;
	}

	/* The page zoomed as Chrome zooms it: its CSS pixels larger, devicePixelRatio with them */
	void zoom( double factor ) override
	{
		if ( factor <= 0 ) return;
		p_.frame->SetLayoutZoomFactor((float)factor);
		update();
	}

	/* ------------------------------------------------ full screen */

	int window_asked() override
	{
		int a = p_.win_ask;
		p_.win_ask = 0;
		return a;
	}

	int fullscreen_asked() override
	{
		int a = p_.fs_ask;
		p_.fs_ask = 0;
		if ( a == BR_FS_ENTER ) fs_entering_ = true;
		return a;
	}

	/*
	 * The window shows the page over the whole screen now (the view is
	 * the screen's size), or as it was: the document's request resolved,
	 * or its leaving done (the page's own exitFullscreen(), or the
	 * window leaving it, Esc: DidExitFullscreen takes both). A request the
	 * window could not grant is refused.
	 */
	void fullscreen_done( bool full ) override
	{
		blink::Document *d = doc();
		if ( d == nullptr ) return;
		update();
		if ( fs_entering_ ) {
			fs_entering_ = false;
			blink::Fullscreen::DidResolveEnterFullscreenRequest(*d, full);
			fs_ = full;
		} else if ( !full && fs_ ) {
			fs_ = false;
			blink::Fullscreen::DidExitFullscreen(*d);
		}
		run_tasks();
	}

	/* ------------------------------------------------ the right button's menu, the editor */

	bool context( int x, int y, Context *what ) override
	{
		update();
		gfx::PointF		at((float)x, (float)y);
		base::TimeTicks		now = base::TimeTicks::Now();
		blink::EventHandler	&h = p_.frame->GetEventHandler();

		blink::HitTestLocation loc(at);
		blink::HitTestResult r = h.HitTestResultAtLocation(loc);
		blink::KURL link = r.AbsoluteLinkURL(), image = r.AbsoluteImageURL();
		what->link = link.IsValid() && !link.IsEmpty() ? link.GetString().Utf8() : std::string();
		what->image = image.IsValid() && !image.IsEmpty() ? image.GetString().Utf8() : std::string();
		what->editable = r.IsContentEditable();
		/* the press, then contextmenu, as on Linux the menu opens at the button going down */
		blink::WebMouseEvent down(blink::WebInputEvent::Type::kMouseDown, at, at,
					  blink::WebPointerProperties::Button::kRight, 1,
					  blink::WebInputEvent::kRightButtonDown, now);
		down.SetFrameScale(1);
		h.HandleMousePressEvent(down);
		blink::WebInputEventResult res = h.SendContextMenuEvent(down);
		blink::WebMouseEvent up(blink::WebInputEvent::Type::kMouseUp, at, at,
					blink::WebPointerProperties::Button::kRight, 1,
					blink::WebInputEvent::kNoModifiers, now);
		up.SetFrameScale(1);
		h.HandleMouseReleaseEvent(up);
		run_tasks();
		what->selection = p_.frame->Selection().SelectedText().Utf8();
		return res == blink::WebInputEventResult::kHandledApplication;
	}

	void edit( const std::string &command ) override
	{
		update();
		blink::LocalFrame *f = p_.frame.Get();
		/* the frame with the focus, an iframe's among them */
		if ( blink::LocalFrame *focused = p_.page->GetFocusController().FocusedFrame() ) f = focused;
		f->GetEditor().ExecuteCommand(blink::String::FromUtf8(command));
		run_tasks();
	}

	void save_url( const std::string &url ) override
	{
		save_from_net(url, std::string());
	}

	/* ------------------------------------------------ find in the page */

	/*
	 * The matches of 'text' in the page, as Chrome's find bar finds them
	 * (letters whatever their case, in the text as it is shown, the main
	 * frame's): each marked as a find marks it, the one that is gone to
	 * marked as the active one and brought into the view. step 0 starts at
	 * the first, 1 goes to the next, -1 to the one before (round the end).
	 * Answers how many there are and, in 'active', which one is gone to
	 * (from 1); "" takes the marks away.
	 */
	int find( const std::string &text, int step, int *active ) override
	{
		*active = 0;
		blink::Document *d = doc();
		if ( d == nullptr ) return 0;
		update();
		d->Markers().RemoveMarkersOfTypes(blink::DocumentMarker::MarkerTypes::TextMatch());
		if ( text.empty() ) {
			find_at_ = -1;
			update();
			return 0;
		}
		blink::String s = blink::String::FromUtf8(text);
		blink::PositionInFlatTree end = blink::PositionInFlatTree::LastPositionInNode(*d);
		blink::EphemeralRangeInFlatTree rest(blink::PositionInFlatTree::FirstPositionInNode(*d), end);
		std::vector<blink::EphemeralRange> found;
		while ( found.size() < kFindMax && !rest.IsCollapsed() ) {
			blink::EphemeralRangeInFlatTree m = blink::FindBuffer::FindMatchInRange(
				rest, s, blink::FindOptions().SetCaseInsensitive(true));
			if ( m.IsNull() || m.IsCollapsed() ) break;
			found.emplace_back(blink::ToPositionInDomTree(m.StartPosition()),
					   blink::ToPositionInDomTree(m.EndPosition()));
			rest = blink::EphemeralRangeInFlatTree(m.EndPosition(), end);
		}
		int n = (int)found.size();
		if ( n == 0 ) {
			find_at_ = -1;
			update();
			return 0;
		}
		find_at_ = ( step == 0 || find_at_ < 0 ) ? 0 : ( ( find_at_ + step ) % n + n ) % n;
		for ( const blink::EphemeralRange &r : found ) {
			d->Markers().AddTextMatchMarker(r, blink::TextMatchMarker::MatchStatus::kInactive);
		}
		const blink::EphemeralRange &a = found[(size_t)find_at_];
		(void)d->Markers().SetTextMatchMarkersActive(a, true);
		/* the match brought into the view, to its middle when it is out of it, as a find brings it */
		blink::Node *at = a.StartPosition().ComputeContainerNode();
		blink::Element *e = at != nullptr ? ( at->IsElementNode() ? blink::To<blink::Element>(at) : at->parentElement() )
						  : nullptr;
		if ( e != nullptr ) e->scrollIntoViewIfNeeded(true);
		update();
		scroll_top();
		*active = find_at_ + 1;
		return n;
	}

	/* ------------------------------------------------ input given to the page */

	bool input() override { return true; }

	void mouse( int type, int x, int y, int button, unsigned mods ) override
	{
		update();
		if ( p_.drag != nullptr ) {
			drag_mouse(type, gfx::PointF((float)x, (float)y), mods);
			run_tasks();
			return;
		}
		gfx::PointF		at((float)x, (float)y);
		base::TimeTicks		now = base::TimeTicks::Now();
		blink::EventHandler	&h = p_.frame->GetEventHandler();
		int			m = web_mods(mods);
		blink::WebPointerProperties::Button b =
			button == 2 ? blink::WebPointerProperties::Button::kRight
			: button == 1 ? blink::WebPointerProperties::Button::kMiddle
			: blink::WebPointerProperties::Button::kLeft;

		if ( type == BR_M_MOVE ) {
			if ( down_ ) m |= blink::WebInputEvent::kLeftButtonDown;
			blink::WebMouseEvent ev(blink::WebInputEvent::Type::kMouseMove, at, at,
						down_ ? blink::WebPointerProperties::Button::kLeft
						      : blink::WebPointerProperties::Button::kNoButton,
						0, m, now);
			ev.SetFrameScale(1);
			h.HandleMouseMoveEvent(ev, blink::Vector<blink::WebMouseEvent>(),
					       blink::Vector<blink::WebMouseEvent>());
		} else if ( type == BR_M_DOWN ) {
			/* a press on a player's bar of controls is the player's (the players are the first window's) */
			/* presses near each other in time and place count up (a double click) */
			if ( now - last_down_ < base::Milliseconds(kClickMs)
			     && std::abs(x - down_x_) < 4 && std::abs(y - down_y_) < 4 ) {
				clicks_++;
			} else {
				clicks_ = 1;
			}
			last_down_ = now;
			down_x_ = x;
			down_y_ = y;
			if ( button == 0 ) down_ = true;
			m |= ( button == 2 ) ? blink::WebInputEvent::kRightButtonDown
			     : ( button == 1 ) ? blink::WebInputEvent::kMiddleButtonDown
					       : blink::WebInputEvent::kLeftButtonDown;
			blink::WebMouseEvent ev(blink::WebInputEvent::Type::kMouseDown, at, at, b,
						clicks_, m, now);
			ev.SetFrameScale(1);
			h.HandleMousePressEvent(ev);
		} else {
			if ( button == 0 ) down_ = false;
			blink::WebMouseEvent ev(blink::WebInputEvent::Type::kMouseUp, at, at, b,
						clicks_, m, now);
			ev.SetFrameScale(1);
			h.HandleMouseReleaseEvent(ev);
		}
		run_tasks();
	}

	bool key( unsigned usage, unsigned mods, unsigned ch ) override
	{
		int vk = vk_of(usage);
		if ( vk == 0 && ch == 0 ) return false;
		update();
		blink::EventHandler	&h = p_.frame->GetEventHandler();
		base::TimeTicks		now = base::TimeTicks::Now();
		int			m = web_mods(mods);
		bool			took;

		blink::WebKeyboardEvent down(blink::WebInputEvent::Type::kRawKeyDown, m, now);
		fill_key(&down, vk, usage, ch);
		blink::WebInputEventResult r = h.KeyEvent(down);
		took = ( r != blink::WebInputEventResult::kNotHandled );
		/*
		 * The letter the key types, and Enter and Tab, follow as a
		 * character only when nothing took the key down: Tab has moved the
		 * focus by then, and its character would go into the next field
		 */
		bool types = ( ch >= 0x20 && ch != 0x7F ) || ch == '\r' || ch == '\t';
		if ( types && r == blink::WebInputEventResult::kNotHandled
		     && ( m & ( blink::WebInputEvent::kControlKey | blink::WebInputEvent::kAltKey ) ) == 0 ) {
			blink::WebKeyboardEvent c(blink::WebInputEvent::Type::kChar, m, now);
			fill_key(&c, vk, usage, ch);
			c.text[0] = (char16_t)ch;
			c.unmodified_text[0] = (char16_t)ch;
			if ( h.KeyEvent(c) != blink::WebInputEventResult::kNotHandled ) took = true;
		}
		blink::WebKeyboardEvent up(blink::WebInputEvent::Type::kKeyUp, m, now);
		fill_key(&up, vk, usage, ch);
		h.KeyEvent(up);
		run_tasks();
		return took;
	}

	/*
	 * The wheel, in notches across ('right', to the right positive) and up
	 * and down ('down', down positive): each notch kWheelPx pixels to the
	 * page, as a wheel event whose deltas are the content's move (negative
	 * when the view goes right or down)
	 */
	void wheel( int x, int y, int right, int down ) override
	{
		update();
		blink::EventHandler	&h = p_.frame->GetEventHandler();
		base::TimeTicks		now = base::TimeTicks::Now();
		gfx::PointF		at((float)x, (float)y);

		/* the page's wheel listeners first; what they leave scrolls */
		blink::WebMouseWheelEvent w(blink::WebInputEvent::Type::kMouseWheel, 0, now);
		w.SetPositionInWidget(at);
		w.SetPositionInScreen(at);
		w.SetFrameScale(1);
		w.delta_x = (float)( -right * kWheelPx );
		w.delta_y = (float)( -down * kWheelPx );
		w.wheel_ticks_x = (float)-right;
		w.wheel_ticks_y = (float)-down;
		w.delta_units = ui::ScrollGranularity::kScrollByPrecisePixel;
		w.dispatch_type = blink::WebInputEvent::DispatchType::kBlocking;
		blink::WebInputEventResult r = h.HandleWheelEvent(w);
		if ( r != blink::WebInputEventResult::kHandledApplication && ( right != 0 || down != 0 ) ) {
			/*
			 * The scroll a compositor would make of it, made on the
			 * main thread as the arrow keys' is: from the node under
			 * the point, each notch kWheelLines lines, passed on to
			 * the scrollers round it when one is at its end
			 */
			using Dir = blink::mojom::blink::ScrollDirection;
			blink::HitTestLocation loc(at);
			blink::Node *under = h.HitTestResultAtLocation(loc).InnerNode();
			const Dir across = right > 0 ? Dir::kScrollRightIgnoringWritingMode : Dir::kScrollLeftIgnoringWritingMode;
			const Dir updown = down > 0 ? Dir::kScrollDownIgnoringWritingMode : Dir::kScrollUpIgnoringWritingMode;
			for ( int i = 0; i < std::abs(right) * kWheelLines; i++ ) {
				if ( !h.BubblingScroll(across, ui::ScrollGranularity::kScrollByLine, under) ) break;
			}
			for ( int i = 0; i < std::abs(down) * kWheelLines; i++ ) {
				if ( !h.BubblingScroll(updown, ui::ScrollGranularity::kScrollByLine, under) ) break;
			}
		}
		run_tasks();
	}

	int scroll_top() override
	{
		blink::PaintLayerScrollableArea *sa = view()->LayoutViewport();
		scroll_ = ( sa != nullptr ) ? (int)sa->GetScrollOffset().y() : 0;
		return scroll_;
	}

	bool editable() override
	{
		return p_.frame->GetInputMethodController().TextInputType() != blink::kWebTextInputTypeNone;
	}

	void compose( const std::string &text, const std::vector<int> &clauses, int target,
		      int caret ) override
	{
		update();
		blink::InputMethodController &imc = p_.frame->GetInputMethodController();
		if ( text.empty() ) {
			if ( imc.HasComposition() ) imc.CancelComposition();
			run_tasks();
			return;
		}
		/* the clauses underlined, the one being converted thick */
		blink::Vector<blink::ImeTextSpan> spans;
		for ( size_t i = 0; i + 1 < clauses.size(); i++ ) {
			spans.push_back(blink::ImeTextSpan(
				blink::ImeTextSpan::Type::kComposition, u16_at(text, clauses[i]),
				u16_at(text, clauses[i + 1]), blink::Color::kBlack,
				(int)i == target ? ui::mojom::ImeTextSpanThickness::kThick
						 : ui::mojom::ImeTextSpanThickness::kThin,
				ui::mojom::ImeTextSpanUnderlineStyle::kSolid, blink::Color::kTransparent,
				blink::Color::kTransparent));
		}
		int c = (int)u16_at(text, caret);
		imc.SetComposition(blink::String::FromUtf8(text), spans, c, c);
		run_tasks();
	}

	void commit( const std::string &text ) override
	{
		update();
		p_.frame->GetInputMethodController().CommitText(blink::String::FromUtf8(text),
								 blink::Vector<blink::ImeTextSpan>(), 0);
		run_tasks();
	}

	std::string memory( bool collect ) override
	{
		v8::Isolate *iso = p_.frame->DomWindow() != nullptr ? p_.frame->DomWindow()->GetIsolate()
						      : v8::Isolate::GetCurrent();
		if ( collect ) {
			/* V8's heap and Blink's (one heap for both), all of it */
			iso->LowMemoryNotification();
			run_tasks();
		}
		v8::HeapStatistics hs;
		iso->GetHeapStatistics(&hs);
		size_t oilpan = 0;
		if ( iso->GetCppHeap() != nullptr ) {
			oilpan = iso->GetCppHeap()->CollectStatistics(
				cppgc::HeapStatistics::DetailLevel::kBrief).used_size_bytes;
		}
		struct mallinfo mi = mallinfo();
		using IC = blink::InstanceCounters;
		int nmem = 0;
		size_t memfd = ts_memfd_bytes(&nmem);
		char b[512];
		snprintf(b, sizeof(b), "malloc %zu KB in use (arena %zu KB), V8 %zu KB used of %zu KB "
			 "(external %zu KB), Oilpan %zu KB, partitions %zu KB, memory objects %d (%zu KB); "
			 "documents %d, frames %d, contexts %d, detached %d, array buffers %d, media keys %d",
			 (size_t)mi.uordblks / 1024, (size_t)mi.arena / 1024, hs.used_heap_size() / 1024,
			 hs.total_heap_size() / 1024, hs.external_memory() / 1024, oilpan / 1024,
			 blink::Partitions::TotalSizeOfCommittedPages() / 1024, nmem, memfd / 1024,
			 IC::CounterValue(IC::kDocumentCounter), IC::CounterValue(IC::kFrameCounter),
			 IC::CounterValue(IC::kV8PerContextDataCounter),
			 IC::CounterValue(IC::kDetachedScriptStateCounter),
			 IC::CounterValue(IC::kArrayBufferContentsCounter),
			 IC::CounterValue(IC::kMediaKeysCounter));
		return b;
	}

	void run_script( const std::string &url ) override
	{
		/* "js:<source>": the source itself, run at once */
		if ( url.compare(0, 3, "js:") == 0 ) {
			script_fetched(this, 1, 200, "js:", nullptr, nullptr, url.data() + 3, (unsigned)( url.size() - 3 ));
			return;
		}
		br_net_fetch_set_answer_on_main(1);
		int started = br_net_fetch_start(url.c_str(), "GET", nullptr, 0, "", &script_fetched, this);
		br_net_fetch_set_answer_on_main(0);
		if ( !started ) printf("browser: script %s not fetched\n", url.c_str());
	}

	static void script_fetched( void *ctx, int ok, int status, const char *url, const char *, const char *,
				    const char *data, unsigned len )
	{
		BlinkEngine *e = static_cast<BlinkEngine *>(ctx);
		if ( !ok || status != 200 ) {
			printf("browser: script %s not fetched (%d)\n", url != nullptr ? url : "", status);
			return;
		}
		blink::ScriptState *ss = blink::ToScriptStateForMainWorld(e->p_.frame.Get());
		if ( ss == nullptr || !ss->ContextIsValid() ) return;
		v8::Isolate		*iso = ss->GetIsolate();
		v8::HandleScope		hs(iso);
		v8::Local<v8::Context>	context = ss->GetContext();
		v8::Context::Scope	cs(context);
		v8::TryCatch		tc(iso);
		v8::Local<v8::Script>	script;
		v8::Local<v8::String>	src;
		if ( !v8::String::NewFromUtf8(iso, data, v8::NewStringType::kNormal, (int)len).ToLocal(&src) ) return;
		if ( !v8::Script::Compile(context, src).ToLocal(&script) || script->Run(context).IsEmpty() ) {
			v8::String::Utf8Value why(iso, tc.Exception());
			printf("browser: script failed: %s\n", *why != nullptr ? *why : "?");
			return;
		}
		printf("browser: script %s run\n", url != nullptr ? url : "");
	}

	bool caret( int *x, int *y, int *h ) override
	{
		update();
		gfx::Rect r = p_.frame->Selection().AbsoluteCaretBounds();
		if ( r.IsEmpty() && r.height() == 0 ) return false;
		*x = r.x();
		*y = r.y() - scroll_top();
		*h = r.height();
		return true;
	}

private:
	static constexpr int	kFrameMs = 100;		/* the frames while a page's parts still come */
	static constexpr int	kRefreshUs = 16667;	/* a refresh of the screen, at 60 Hz */
	static constexpr int	kCostShare = 2;		/* a frame's cost, times this: the least time to the next */
	static constexpr int	kEarlyUs = 2000;	/* the slack a frame due at a refresh is given */
	static constexpr uint64_t kBeginFrameSource = 1;	/* the source of the frames' numbers (begin_frame) */
	static constexpr int	kLoadWaitS = 30;	/* the most a page waits for its parts */
	static constexpr int	kClickMs = 500;		/* presses that make a double click */
	static constexpr int	kWheelPx = 120;		/* a notch of the wheel, to the page */
	static constexpr int	kWheelLines = 3;	/* and scrolled */

	/* What the raster invalidations are gathered into: rectangles of the view */
	struct DamageSink final : public blink::RasterInvalidator::Callback {
		std::vector<BrRect>	*out = nullptr;
		int			w = 0, h = 0;

		void InvalidateRect( const gfx::Rect &r ) override
		{
			gfx::Rect v = gfx::IntersectRects(r, gfx::Rect(w, h));
			if ( out == nullptr || v.IsEmpty() ) return;
			BrRect b;
			b.x = v.x();
			b.y = v.y();
			b.w = v.width();
			b.h = v.height();
			out->push_back(b);
		}
	};

	BrPage		p_;
	uint64_t	begin_frames_ = 0;			/* the frames begun, their numbers */
	std::string	nav_method_, nav_ctype_, nav_body_;	/* what went with the last navigation */
	bool		nav_left_asked_ = false;		/* and its beforeunload was asked */

	/*
	 * The joint session history: each entry's HistoryItem of the main
	 * frame (its address, state object, scroll) and of each iframe whose
	 * document is known in it, whether an iframe made the entry with no
	 * user activation (the window's back and forward pass over it), and
	 * the entry shown
	 */
	struct FrameItem {
		blink::WeakPersistent<blink::LocalFrame>	frame;
		blink::Persistent<blink::HistoryItem>		item;
	};
	static constexpr size_t	kHistMax = 50;
	static constexpr int	kLoadReplace = 100;		/* a load that takes the place of the entry shown */
	std::vector<blink::Persistent<blink::HistoryItem>>	hist_;
	std::vector<std::vector<FrameItem>>			subs_;
	std::vector<bool>					skip_;
	std::vector<int>	queued_go_;			/* the page's traversals waiting for the one under way */
	base::TimeTicks		pending_since_;			/* when the one under way began */
	int		at_ = -1;
	int		pending_ = -1;			/* the entry a traversal goes to, until it commits */
	int		load_as_ = BR_LOAD_PAGE;	/* how the application asked the next load to commit */
	int		page_load_ = BR_LOAD_NEW;	/* how the page asked for the navigation it began */
	bool		fs_ = false, fs_entering_ = false;	/* full screen, and a request for it waiting */
	int		resp_status_ = 0;		/* what the next load's bytes were answered with (response) */
	static constexpr size_t	kFindMax = 1000;	/* the matches a find counts, as Chrome's find bar does */
	int		find_at_ = -1;			/* the match gone to, from 0 */
	std::string	resp_headers_, resp_protocol_;
	static constexpr size_t	kChangedMax = 4096;	/* rectangles kept between two views */
	static inline std::vector<BlinkEngine *> all_;	/* every page's engine, which the paint hook tells */
	BlinkEngine	*opener_ = nullptr;		/* the engine of the page that opened this one's window */
	bool		pip_ = false;			/* a document's picture-in-picture window */
	std::vector<BrOpened>	opened_;		/* the windows opened, for the application */

	DamageSink	sink_;
	std::vector<BrRect>	changed_;		/* what changed since damage() was last asked */
	blink::Persistent<blink::RasterInvalidator>	inval_;
	int		seq_ = 1 << 20;		/* clearing the properties' changed flags */
	bool		fresh_ = false;		/* damage() brought the page up to date, nothing since */
	cc::PaintRecord	rec_;			/* the record of that state, once made */
	bool		rec_ok_ = false;
	int		w_ = 800, h_ = 600, scroll_ = 0;
	base::TimeTicks	last_frame_;
	base::TimeTicks	frame_began_;		/* the frame begun and not yet shown */
	base::TimeTicks	next_frame_;		/* the next frame no sooner than this */
	base::TimeTicks	refresh_origin_ = base::TimeTicks::Now();	/* the refreshes are counted from here */
	std::atomic<v8::Isolate *> iso_{nullptr};	/* the engine's thread's, for the window's thread */
	bool		down_ = false;			/* the left button is down */
	int		clicks_ = 0, down_x_ = 0, down_y_ = 0;
	base::TimeTicks	last_down_;

	blink::Document *doc() const
	{
		return p_.frame->DomWindow() != nullptr ? p_.frame->DomWindow()->document() : nullptr;
	}
	blink::LocalFrameView *view() const { return p_.frame->View(); }

	/*
	 * The page, and its main frame with its view. A window another page
	 * opened is in the opener's browsing context group, with the
	 * opener's agents, and its frame has the opener: the opener's
	 * script reaches it, and its initial empty document has the
	 * opener's origin.
	 */
	void make_page( BlinkEngine *opener, blink::LocalFrame *opener_frame )
	{
		p_.engine = this;
		p_.primary = ( opener == nullptr );
		if ( opener != nullptr ) {
			p_.agents = opener->p_.agents;
			p_.group = opener->p_.group;
		} else {
			p_.agents = blink::ThreadScheduler::Current()->ToMainThreadScheduler()
					->CreateAgentGroupScheduler();
			p_.group = base::UnguessableToken::Create();
		}
		p_.chrome = blink::MakeGarbageCollected<BrChromeClient>(&p_);
		p_.client = blink::MakeGarbageCollected<BrFrameClient>(&p_);
		/* before the first page's first request: their rules and scripts hold from there on */
		static bool ext_up = false;
		if ( !ext_up ) {
			ext_up = true;
			br_ext_start();
		}
		p_.page = blink::Page::CreateOrdinary(*p_.chrome, opener != nullptr ? opener->p_.page.Get() : nullptr,
						      *p_.agents, p_.group, nullptr);
		/* what modules add to a page, as a view adds it */
		blink::CoreInitializer::GetInstance().ProvideModulesToPage(*p_.page, std::string());
		blink::Settings &s = p_.page->GetSettings();
		s.SetScriptEnabled(true);
		s.SetImagesEnabled(true);
		s.SetLoadsImagesAutomatically(true);
		s.SetDefaultFontSize(16);
		/* navigator.languages as the requests' Accept-Language has them */
		s.SetAcceptLanguages("ja,en-US,en");
		/* Web Storage on, as Chrome's preferences have it (the storage access handle asks) */
		s.SetLocalStorageEnabled(true);
		/*
		 * No universal access for a file's page, as Chrome's preferences
		 * have it (Blink's own default gives it): a document then joins the
		 * agent cluster of its origin (window.originAgentCluster)
		 */
		s.SetAllowUniversalAccessFromFileURLs(false);
		s.SetAllowFileAccessFromFileURLs(false);
		/* plugins on, as in Chrome: the PDF viewer's MIME types are listed (navigator.plugins) */
		s.SetPluginsEnabled(true);
		/*
		 * A mouse: (pointer: fine) and (hover: hover), as a desktop's
		 * Chrome answers them; pages that see none take the machine for a
		 * touch screen
		 */
		s.SetPrimaryPointerType(blink::mojom::blink::PointerType::kPointerFineType);
		s.SetAvailablePointerTypes(ui::POINTER_TYPE_FINE);
		s.SetPrimaryHoverType(blink::mojom::blink::HoverType::kHoverHoverType);
		s.SetAvailableHoverTypes(ui::HOVER_TYPE_HOVER);
		s.SetDefaultFixedFontSize(13);
		/* a document that names no encoding is read as Chrome in Japanese reads it */
		s.SetDefaultTextEncodingName("Shift_JIS");
		s.SetMinimumFontSize(0);
		s.SetAcceleratedCompositingEnabled(false);
		/*
		 * a smooth scroll (scroll-behavior: smooth, a script's behavior
		 * "smooth", a key's or a wheel's step) is animated over the page's
		 * frames, on the main thread (there is no compositor thread)
		 */
		s.SetScrollAnimatorEnabled(true);
		/* the page as a whole is scrolled by the window's own bars */
		s.SetHideScrollbars(true);
		blink::GenericFontFamilySettings &f = s.GetGenericFontFamilySettings();
		blink::AtomicString fam(kFamily);
		f.UpdateStandard(fam);
		/* the serif face where the system has one (BrFontMgr) */
		f.UpdateSerif(g_have_serif ? blink::AtomicString(kSerifFamily) : fam);
		f.UpdateSansSerif(fam);
		/* the monospaced face where the system has one (BrFontMgr) */
		f.UpdateFixed(g_have_mono ? blink::AtomicString(kMonoFamily) : fam);
		f.UpdateCursive(fam);
		f.UpdateFantasy(fam);
		p_.page->UpdateColorProvidersForTest();
		blink::DocumentLoader::DisableCodeCacheForTesting();

		p_.frame = blink::MakeGarbageCollected<blink::LocalFrame>(
			p_.client.Get(), *p_.page, nullptr, nullptr, nullptr,
			blink::FrameInsertType::kInsertInConstructor, blink::LocalFrameToken(),
			opener_frame != nullptr ? &opener_frame->window_agent_factory() : nullptr, nullptr,
			mojo::NullRemote(), base::DefaultTickClock::GetInstance());
		gfx::Size size(w_, h_);
		p_.frame->SetView(blink::MakeGarbageCollected<blink::LocalFrameView>(*p_.frame, size));
		p_.frame->View()->GetPage()->GetVisualViewport().SetSize(size);
		p_.frame->Init(opener_frame, blink::DocumentToken(), nullptr,
			       opener_frame != nullptr && opener_frame->DomWindow() != nullptr
				       ? blink::StorageKey(opener_frame->DomWindow()->GetStorageKey()) : blink::StorageKey(),
			       ukm::kInvalidSourceId,
			       opener_frame != nullptr && opener_frame->GetDocument() != nullptr
				       ? opener_frame->GetDocument()->BaseURL() : blink::KURL());
		/*
		 * The page is the one the window shows: active and focused, so
		 * that a field with the focus shows its caret, blinking
		 */
		p_.page->GetFocusController().SetActive(true);
		p_.page->GetFocusController().SetFocused(true);
		/* the frames' long tasks, watched from now on (BrLongTasks) */
		p_.timing_client = std::make_unique<BrLongTasks>(&p_);
		p_.timing = blink::MakeGarbageCollected<blink::AnimationFrameTimingMonitor>(
			*p_.timing_client, p_.frame->GetProbeSink());
	}

	void resize()
	{
		gfx::Size size(w_, h_);
		view()->Resize(size);
		p_.page->GetVisualViewport().SetSize(size);
	}

	void scroll_to( int top )
	{
		blink::PaintLayerScrollableArea *sa = view()->LayoutViewport();
		if ( sa == nullptr ) return;
		/* across, the view stays where it is */
		sa->SetScrollOffset(blink::ScrollOffset(sa->GetScrollOffset().x(), (float)top),
				    blink::mojom::blink::ScrollType::kProgrammatic,
				    cc::ScrollSourceType::kNone);
		scroll_ = (int)sa->GetScrollOffset().y();
	}

	void update()
	{
		fresh_ = false;
		rec_ok_ = false;
		rec_ = cc::PaintRecord();
		run_tasks();
		/* the style and layout of a frame begun, for its timing (BrLongTasks) */
		if ( p_.timing != nullptr ) p_.timing->WillPerformStyleAndLayoutCalculation();
		FrameTime t(&g_turn.lifecycle);
		view()->UpdateAllLifecyclePhasesForTest();
	}
};

blink::Page *br_open_window( BrPage *opener, blink::LocalFrame *opener_frame, const blink::FrameLoadRequest &req,
			     const blink::WebWindowFeatures &f )
{
	BlinkEngine *from = BlinkEngine::of(opener);
	if ( from == nullptr ) return nullptr;
	int w = from->view_w(), h = from->view_h(), x = -1, y = -1;
	bool pip = false;
	if ( const auto &o = req.GetPictureInPictureWindowOptions() ) {
		/* a picture-in-picture window: as large as asked, else small */
		pip = true;
		w = o->width > 0 ? (int)o->width : 400;
		h = o->height > 0 ? (int)o->height : 300;
	} else {
		if ( f.width_set && f.width > 0 ) w = f.width;
		if ( f.height_set && f.height > 0 ) h = f.height;
		if ( f.x_set ) x = f.x;
		if ( f.y_set ) y = f.y;
	}
	auto *child = new BlinkEngine(from, opener_frame, w, h, pip);
	from->add_opened(child, x, y);
	printf("browser: a new window with its opener, %dx%d%s: %s\n", w, h, pip ? " (picture-in-picture)" : "",
	       req.GetResourceRequest().Url().GetString().Utf8().c_str());
	return child->page();
}

blink::LocalFrame *br_top_frame_of_engines()
{
	return BlinkEngine::top_frame();
}

void page_committed( BrPage *p, blink::HistoryItem *item, blink::WebHistoryCommitType type )
{
	if ( BlinkEngine *e = BlinkEngine::of(p) ) e->committed(item, type);
}

void page_frame_committed( BrPage *p, blink::LocalFrame *frame, blink::HistoryItem *item,
			   blink::WebHistoryCommitType type )
{
	if ( BlinkEngine *e = BlinkEngine::of(p) ) e->frame_committed(frame, item, type);
}

void page_frame_gone( BrPage *p, blink::LocalFrame *frame )
{
	/* the iframes of a document another takes the place of stay in the entries they made */
	if ( g_doc_replacing > 0 ) return;
	if ( BlinkEngine *e = BlinkEngine::of(p) ) e->frame_gone(frame);
}

bool page_history_go( BrPage *p, int offset )
{
	BlinkEngine *e = BlinkEngine::of(p);
	return e != nullptr && e->go_entry(offset, false) != BR_HIST_NONE;
}

unsigned page_history_length( BrPage *p )
{
	BlinkEngine *e = BlinkEngine::of(p);
	return e != nullptr ? e->history_length() : 1u;
}

}  // namespace

blink::LocalFrame *br_top_frame()
{
	return br_top_frame_of_engines();
}

std::string br_v8_flags;
std::string br_features;
std::string br_secure_origins;
std::string br_trace;

/*
 * What the browser says it is: Chrome 153's reduced user agent of a Linux
 * desktop, which says "X11; Linux x86_64" whatever the processor (as
 * navigator.platform does, "Linux x86_64"); the processor is said by the
 * client hints (architecture "arm", bitness "64")
 */
const char *br_user_agent()
{
	return "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
	       "Chrome/153.0.0.0 Safari/537.36";
}

/*
 * The same said as client hints (navigator.userAgentData and the
 * Sec-CH-UA headers): the engine, Chromium 153, with the made-up brand
 * every Chromium adds so that sites do not match the list as a whole
 * (its name, version and place in the list chosen from the major
 * version, 153: Chromium first), on Linux (as the user agent says) on a
 * 64-bit Arm. The full version is the engine's own (chrome/VERSION of
 * the sources built), which the reduced user agent leaves out but the
 * high-entropy hints give.
 */
blink::UserAgentMetadata br_user_agent_metadata()
{
	blink::UserAgentMetadata m;

	m.brand_version_list = { { "Chromium", "153" }, { "Not_A Brand", "8" } };
	m.brand_full_version_list = { { "Chromium", "153.0.8010.36" }, { "Not_A Brand", "8.0.0.0" } };
	m.full_version = "153.0.8010.36";
	m.platform = "Linux";
	m.architecture = "arm";
	m.bitness = "64";
	m.mobile = false;
	m.form_factors = { "Desktop" };
	return m;
}

/* The low-entropy hints a secure request carries, as "name: value" lines */
const std::string &br_client_hint_headers()
{
	static const std::string lines = [] {
		blink::UserAgentMetadata m = br_user_agent_metadata();
		return "Sec-CH-UA: " + m.SerializeBrandMajorVersionList() + "\r\nSec-CH-UA-Mobile: ?0\r\n"
		       "Sec-CH-UA-Platform: \"" + m.platform + "\"\r\n";
	}();
	return lines;
}

/*
 * The client hints a site asked for (Accept-CH on the response of a
 * secure document of a window), kept by origin for the session as a
 * browser keeps them: a document of the origin is committed with them on
 * (Blink then adds them to the document's requests where the permissions
 * policy lets it), and a later navigation to the origin carries the user
 * agent's ones
 */
namespace {

std::mutex g_hints_lock;
std::map<std::string, std::vector<network::mojom::WebClientHintsType>> g_hints;

std::string hints_origin( const GURL &u )
{
	url::Origin o = url::Origin::Create(u);
	return o.opaque() ? std::string() : o.Serialize();
}

/* A header's values, the lines of the same name joined by commas */
std::string header_values( const std::string &headers, const char *name )
{
	std::string lh = headers, out;
	for ( char &c : lh ) c = (char)tolower((unsigned char)c);
	size_t n = strlen(name), at = 0;
	while ( at < lh.size() ) {
		size_t eol = lh.find('\n', at);
		if ( eol == std::string::npos ) eol = lh.size();
		if ( lh.compare(at, n, name) == 0 && at + n < eol && lh[at + n] == ':' ) {
			std::string v = headers.substr(at + n + 1, eol - at - n - 1);
			while ( !v.empty() && ( v.back() == '\r' || v.back() == ' ' ) ) v.pop_back();
			v.erase(0, v.find_first_not_of(" \t"));
			if ( !out.empty() ) out += ", ";
			out += v;
		}
		at = eol + 1;
	}
	return out;
}

}  // namespace

void br_hints_learn( const std::string &url, const std::string &headers )
{
	GURL u(url);
	if ( !u.SchemeIsHTTPOrHTTPS() || !network::IsUrlPotentiallyTrustworthy(u) ) return;
	std::string v = header_values(headers, "accept-ch");
	if ( v.empty() ) return;
	std::optional<std::vector<network::mojom::WebClientHintsType>> parsed = network::ParseClientHintsHeader(v);
	if ( !parsed ) return;
	std::lock_guard<std::mutex> hold(g_hints_lock);
	g_hints[hints_origin(u)] = *parsed;
}

std::vector<network::mojom::WebClientHintsType> br_hints_for( const std::string &url )
{
	GURL u(url);
	std::lock_guard<std::mutex> hold(g_hints_lock);
	auto it = g_hints.find(hints_origin(u));
	return it == g_hints.end() ? std::vector<network::mojom::WebClientHintsType>() : it->second;
}

/*
 * When the window's navigation began (2), was sent (0) and answered (1),
 * for its document's timing (apply_nav_timing)
 */
void br_navigation_timing( int phase )
{
	( phase == 2 ? g_nav_began : phase == 0 ? g_nav_sent : g_nav_answered ) = base::TimeTicks::Now();
}

/*
 * What a browser puts on the request of a window's navigation besides
 * the user agent, the languages and the encodings the network stack
 * adds: the document's Accept, Upgrade-Insecure-Requests, the user
 * agent's client hints the origin asked for, the fetch metadata (from
 * the browser, "none" and a user's navigation; from the page shown, its
 * relation to the address), and the highest priority. `from` is the
 * address of the page that asked, empty when the user did.
 */
std::string br_navigation_headers( const std::string &url, const std::string &from )
{
	GURL u(url);
	std::string h = "Upgrade-Insecure-Requests: 1\r\n"
			"Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,"
			"image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.7\r\n";
	/*
	 * lines the network stack takes off: a main frame's navigation (its
	 * site for cookies follows its redirects), and the page that asked
	 */
	h += "X-TessronOS-Navigation: main\r\nX-TessronOS-Initiator: " + ( from.empty() ? std::string("none") : from ) + "\r\n";
	/* what the extensions' rules are told: from the origin of the page that asked, as a request's initiator is */
	if ( br_ext_any() ) {
		h += "X-TessronOS-Resource: main_frame";
		if ( !from.empty() ) h += " " + url::Origin::Create(GURL(from)).Serialize();
		h += "\r\n";
	}
	if ( !u.SchemeIsHTTPOrHTTPS() || !network::IsUrlPotentiallyTrustworthy(u) ) return h;
	std::vector<network::mojom::WebClientHintsType> want = br_hints_for(url);
	auto wanted = [&]( network::mojom::WebClientHintsType t ) {
		return std::find(want.begin(), want.end(), t) != want.end();
	};
	blink::UserAgentMetadata m = br_user_agent_metadata();
	using T = network::mojom::WebClientHintsType;
	if ( wanted(T::kUAArch) ) h += "Sec-CH-UA-Arch: \"" + m.architecture + "\"\r\n";
	if ( wanted(T::kUAPlatformVersion) ) h += "Sec-CH-UA-Platform-Version: \"" + m.platform_version + "\"\r\n";
	if ( wanted(T::kUAModel) ) h += "Sec-CH-UA-Model: \"" + m.model + "\"\r\n";
	if ( wanted(T::kUABitness) ) h += "Sec-CH-UA-Bitness: \"" + m.bitness + "\"\r\n";
	if ( wanted(T::kUAWoW64) ) h += std::string("Sec-CH-UA-WoW64: ") + ( m.wow64 ? "?1" : "?0" ) + "\r\n";
	if ( wanted(T::kUAFullVersionList) ) h += "Sec-CH-UA-Full-Version-List: " + m.SerializeBrandFullVersionList() + "\r\n";
	if ( wanted(T::kDpr) ) h += "Sec-CH-DPR: 1\r\n";
	if ( wanted(T::kDeviceMemory) ) {
		h += "Sec-CH-Device-Memory: " +
		     base::NumberToString(blink::ApproximatedDeviceMemory::GetApproximatedDeviceMemory()) + "\r\n";
	}
	if ( wanted(T::kPrefersColorScheme) ) h += "Sec-CH-Prefers-Color-Scheme: light\r\n";
	std::string site = "none";
	if ( !from.empty() ) {
		GURL f(from);
		url::Origin fo = url::Origin::Create(f);
		if ( fo.IsSameOriginWith(u) ) site = "same-origin";
		else if ( net::SchemefulSite(fo) == net::SchemefulSite(u) ) site = "same-site";
		else site = "cross-site";
	}
	h += "Sec-Fetch-Site: " + site + "\r\nSec-Fetch-Mode: navigate\r\n";
	if ( from.empty() ) h += "Sec-Fetch-User: ?1\r\n";
	h += "Sec-Fetch-Dest: document\r\n";
	h += "X-TessronOS-Priority: " + base::NumberToString((int)net::HIGHEST) + " i\r\n";
	return h;
}

void br_base_up()
{
	base_up();
}

void br_blink_up( sk_sp<SkData> face )
{
	static bool up;

	if ( !up ) {
		start_blink(std::move(face));
		up = true;
	}
}

BrEngine *br_blink_engine( sk_sp<SkData> face )
{
	br_blink_up(std::move(face));
	return new BlinkEngine;
}

std::string br_page_origin()
{
	std::lock_guard<std::mutex> hold(g_origin_lock);
	return g_origin;
}

/*
 * What a page fetched is, decided as Chrome's browser decides it when a
 * navigation's response comes, before anything is committed: a download
 * when it says it is an attachment (Content-Disposition), or when its
 * type is not one Blink makes a document of (blink::IsSupportedMimeType:
 * HTML, XML, text, the images and the media Blink shows; a PDF is shown
 * as the page PDFium draws). Committed, a body of another type would be
 * parsed as HTML: a zip of megabytes held the renderer in Blink's parser
 * for minutes. A type that says nothing of the bytes (none, text/plain,
 * application/octet-stream and the others net::ShouldSniffMimeType
 * names) is sniffed from their start first, as Chrome's MIME sniffing
 * does, unless the server forbids it (X-Content-Type-Options: nosniff);
 * the page is then loaded as the type found (text with no type is shown
 * as text, not as HTML). A download whose status is not a success is not
 * saved but answered with an error page (ERR_INVALID_RESPONSE), as Chrome
 * does. The browser's own pages (about:, data:, the extensions') with no
 * type stay HTML. With only the start of the body ('whole' false), -1
 * when the sniffing wants more of it than has come.
 */
static std::string mime_of( const std::string &type )
{
	std::string mime = type.substr(0, type.find(';'));
	mime.erase(0, mime.find_first_not_of(" \t"));
	while ( !mime.empty() && ( mime.back() == ' ' || mime.back() == '\t' ) ) mime.pop_back();
	for ( char &ch : mime ) ch = (char)tolower((unsigned char)ch);
	return mime;
}

static int br_nav_judge( BrResponse *r, bool whole )
{
	GURL u(r->url);
	bool http = u.SchemeIsHTTPOrHTTPS();
	std::string mime = mime_of(r->type);
	scoped_refptr<net::HttpResponseHeaders> h;
	if ( !r->headers.empty() ) {
		h = base::MakeRefCounted<net::HttpResponseHeaders>(
			net::HttpUtil::AssembleRawHeaders("HTTP/1.1 200 OK\r\n" + r->headers + "\r\n"));
	}
	if ( http && net::ShouldSniffMimeType(u, h.get(), mime) ) {
		if ( !whole && r->body.size() < (size_t)net::kMaxBytesToSniff ) return -1;
		std::string sniffed;
		size_t n = std::min(r->body.size(), (size_t)net::kMaxBytesToSniff);
		(void)net::SniffMimeType(std::string_view(r->body.data(), n), u, mime,
					 net::ForceSniffFileUrlsForHtml::kDisabled, &sniffed);
		if ( sniffed.empty() ) sniffed = "text/plain";
		if ( sniffed != mime ) {
			printf("browser: %.200s said %s: sniffed as %s\n", r->url.c_str(),
			       mime.empty() ? "no type" : mime.c_str(), sniffed.c_str());
			r->type = sniffed;
			mime = sniffed;
		}
	}
	bool attachment = !r->disposition.empty()
			  && net::HttpContentDisposition(r->disposition, std::string()).is_attachment();
	bool shown = mime == "application/pdf" || blink::IsSupportedMimeType(mime) || ( mime.empty() && !http );
	if ( shown && !attachment ) return BR_ANSWER_SHOW;
	if ( http && ( r->status < 200 || r->status >= 300 ) ) {
		printf("browser: %.200s is a download answered with status %d: not saved\n", r->url.c_str(), r->status);
		return BR_ANSWER_INVALID;
	}
	printf("browser: %.200s is %s: saved, not shown\n", r->url.c_str(),
	       attachment ? "an attachment" : ( "of a type no document is made of (" + mime + ")" ).c_str());
	return BR_ANSWER_SAVED;
}

/* The name a download is saved under: the server's (Content-Disposition), else from its address and type */
static std::string nav_save_name( const BrResponse &r )
{
	std::string given = r.disposition.empty() ? std::string() : disposition_name("Content-Disposition: " + r.disposition);
	return save_name(given, r.url, mime_of(r.type));
}

/* An answer had whole (a service worker's, the stack's waited for): its bytes saved when it is a download */
int br_nav_answer( BrResponse *r )
{
	int answer = br_nav_judge(r, true);
	if ( answer == BR_ANSWER_SAVED ) save_bytes(nav_save_name(*r), r->body);
	return answer;
}

/*
 * A window's navigation fetched as it comes, decided at its response
 * (br_nav_judge) as Chrome decides it there: a page waits for the rest of
 * its body (within 'timeout_ms' for the whole, as before); a download is
 * handed over at once (ns_hand_over) and the navigation ends, the page
 * shown staying, with no limit on how long the download takes but a
 * minute with no byte. The renderer holds nothing of it, nor does the
 * browser hold it whole.
 */
int br_nav_head_fetch( const std::string &url, const std::string &method, const std::string &body,
		       const std::string &hdrs, int timeout_ms, BrNavFetched *out )
{
	std::shared_ptr<NavStream> s = ns_start(url, method, body, hdrs);
	if ( s == nullptr ) return -1;
	std::unique_lock<std::mutex> hold(s->m);
	BrResponse r;
	int answer = -1;
	for ( long t0 = dl_now_ms();; ) {
		if ( s->head && answer < 0 ) {
			r.url = !s->url.empty() ? s->url : url;
			r.type = s->ctype;
			r.headers = s->headers;
			r.status = s->status;
			r.disposition = header_of(s->headers, "content-disposition");
			r.body = s->body;
			answer = br_nav_judge(&r, s->ended);
			if ( answer >= 0 && answer != BR_ANSWER_SHOW ) break;
		}
		if ( s->ended ) break;
		if ( br_net_fetch_cancel_asked() ) {
			hold.unlock();
			printf("browser: net fetch: %s given up (stopped)\n", url.c_str());
			ns_let_go(s);
			return 0;
		}
		if ( dl_now_ms() - t0 >= timeout_ms ) {
			hold.unlock();
			printf("browser: net fetch: %s not in after %d ms\n", url.c_str(), timeout_ms);
			ns_let_go(s);
			return 0;
		}
		s->cv.wait_for(hold, std::chrono::milliseconds(100));
	}
	if ( answer < 0 ) {
		/* ended: well with no response told (an extension's address), or failed */
		if ( !s->ok ) return 0;
		r.url = !s->url.empty() ? s->url : url;
		r.type = s->ctype;
		r.headers = s->headers;
		r.status = s->status;
		r.disposition = header_of(s->headers, "content-disposition");
		r.body = s->body;
		answer = br_nav_judge(&r, true);
	}
	if ( answer == BR_ANSWER_SHOW && !s->ok ) return 0;	/* a page whose body broke off */
	out->answer = answer;
	out->status = r.status;
	out->url = r.url;
	out->ctype = r.type;
	out->headers = r.headers;
	if ( answer == BR_ANSWER_SHOW ) {
		out->body = std::move(s->body);
		return 1;
	}
	hold.unlock();
	if ( answer == BR_ANSWER_INVALID ) {
		ns_let_go(s);
		return 1;
	}
	(void)ns_hand_over(s, nav_save_name(r), r.url, mime_of(r.type), length_of(r.headers), r.status);
	return 1;
}

/* What a renderer handed over (Host.KeepDownload), kept here */
void br_keep_download( const std::string &name, std::string bytes )
{
	save_bytes(name, std::move(bytes));
}

/* ---------------------------------------------------------------- downloads taken in as they come */

/*
 * One download: the pieces not yet taken in (each with what tells its
 * source it has been), read by the thread that makes the real object
 * (xfu_import, through br_obj_save_stream) as they come
 */
struct BrDownload {
	struct Piece {
		std::string		bytes;
		size_t			at = 0;
		std::function<void()>	taken;
	};
	std::string		name, url;
	long long		total = -1, got = 0;
	int			row = 0;
	std::function<void()>	stop;
	std::mutex		m;
	std::condition_variable	cv;
	std::deque<Piece>	q;
	bool			ended = false, ok = false, stopped = false, over = false;
	long			last_ms = 0;
};

namespace {

/* The downloads under way, for the browser closing to stop them and wait */
std::mutex				g_dl_live_lock;
std::condition_variable			g_dl_live_cv;
std::set<std::shared_ptr<BrDownload>>	g_dl_live;
bool					g_dl_closing;

/* The pieces dropped (stopped, failed): each source told they are taken, so that its pipe lets go of them */
void dl_drop( BrDownload &d, std::unique_lock<std::mutex> &hold )
{
	std::vector<std::function<void()>> told;
	for ( BrDownload::Piece &p : d.q ) {
		if ( p.taken ) told.push_back(std::move(p.taken));
	}
	d.q.clear();
	hold.unlock();
	for ( auto &t : told ) t();
	hold.lock();
}

/*
 * The bytes for the object: from the pieces as they come, 0 at the end;
 * below 0 when the download is stopped (中止 in the list, the browser
 * closing), its request failed, or no byte came for kDlIdleMs
 */
long dl_read( BrDownload &d, void *buf, size_t n )
{
	std::unique_lock<std::mutex> hold(d.m);
	for ( ;; ) {
		if ( d.stopped ) return -1;
		if ( !d.q.empty() ) {
			BrDownload::Piece &p = d.q.front();
			size_t k = std::min(n, p.bytes.size() - p.at);
			memcpy(buf, p.bytes.data() + p.at, k);
			p.at += k;
			if ( p.at == p.bytes.size() ) {
				std::function<void()> t = std::move(p.taken);
				d.q.pop_front();
				hold.unlock();
				if ( t ) t();
			}
			return (long)k;
		}
		if ( d.ended ) {
			if ( d.ok ) return 0;
			printf("browser: the download of %s broke off\n", d.name.c_str());
			return -2;
		}
		if ( dl_now_ms() - d.last_ms >= kDlIdleMs ) {
			printf("browser: the download of %s had no byte for %ld s: given up\n", d.name.c_str(), kDlIdleMs / 1000);
			return -3;
		}
		d.cv.wait_for(hold, std::chrono::milliseconds(500));
		/* 中止 from the list is asked through the row */
		long long got = d.got, total = d.total;
		hold.unlock();
		bool go = br_download_progress(d.row, got, total);
		hold.lock();
		if ( !go ) d.stopped = true;
	}
}

/* The thread of one download: the object made as the bytes come, the row ended, the message line told */
void dl_run( std::shared_ptr<BrDownload> d )
{
	unsigned char id[16];
	bool saved = br_obj_save_stream(d->name, [d]( void *buf, size_t n ) { return dl_read(*d, buf, n); }, id);
	bool let_go, stopped;
	long long got, total;
	{
		std::unique_lock<std::mutex> hold(d->m);
		d->over = true;
		let_go = !d->ended;
		stopped = d->stopped;
		got = d->got;
		total = d->total;
		dl_drop(*d, hold);
	}
	/* its request stopped where it comes from, when it is still coming */
	if ( !saved && let_go && d->stop ) d->stop();
	d->stop = nullptr;
	(void)br_download_progress(d->row, got, total);
	br_download_end(d->row, saved ? BR_DL_DONE : stopped ? BR_DL_STOPPED : BR_DL_FAILED);
	if ( !saved ) {
		printf("browser: the download of %s %s (%lld bytes came)\n", d->name.c_str(), stopped ? "stopped" : "failed", got);
		br_main_command(stopped ? "said:「" + d->name + "」の保存を中止しました" : "said:「" + d->name + "」を保存できませんでした");
	}
	std::lock_guard<std::mutex> hold(g_dl_live_lock);
	g_dl_live.erase(d);
	g_dl_live_cv.notify_all();
}

}  // namespace

std::shared_ptr<BrDownload> br_download_begin( const std::string &name, const std::string &url, long long total,
					       int status, std::function<void()> stop )
{
	if ( status != 0 && ( status < 200 || status >= 300 ) ) {
		printf("browser: %s answered with status %d: not saved\n", url.c_str(), status);
		dl_failed_row(name, url);
		return nullptr;
	}
	auto d = std::make_shared<BrDownload>();
	d->name = name.empty() ? std::string("download") : name;
	d->url = url;
	d->total = total;
	d->stop = std::move(stop);
	d->last_ms = dl_now_ms();
	{
		std::lock_guard<std::mutex> hold(g_dl_live_lock);
		if ( g_dl_closing ) return nullptr;
		g_dl_live.insert(d);
	}
	d->row = br_download_start(d->name, url);
	(void)br_download_progress(d->row, 0, total);
	/* the message line says it before the bytes come, after what the window said of the page (br_main.cc, "said:") */
	br_main_command("said:「" + d->name + "」を保存しています…");
	printf("browser: %s saved as it comes from %.160s (%lld bytes said)\n", d->name.c_str(), url.c_str(), total);
	std::thread(dl_run, d).detach();
	return d;
}

void br_download_bytes( const std::shared_ptr<BrDownload> &d, std::string piece, std::function<void()> taken )
{
	if ( d == nullptr || piece.empty() ) {
		if ( taken ) taken();
		return;
	}
	long long got;
	{
		std::lock_guard<std::mutex> hold(d->m);
		if ( d->over || d->stopped ) {
			got = -1;
		} else {
			got = d->got += (long long)piece.size();
			d->last_ms = dl_now_ms();
			BrDownload::Piece p;
			p.bytes = std::move(piece);
			p.taken = std::move(taken);
			d->q.push_back(std::move(p));
			d->cv.notify_all();
		}
	}
	if ( got < 0 ) {
		if ( taken ) taken();
		return;
	}
	if ( !br_download_progress(d->row, got, d->total) ) {
		std::lock_guard<std::mutex> hold(d->m);
		d->stopped = true;
		d->cv.notify_all();
	}
}

void br_download_finish( const std::shared_ptr<BrDownload> &d, bool ok )
{
	if ( d == nullptr ) return;
	std::lock_guard<std::mutex> hold(d->m);
	if ( d->ended ) return;
	d->ended = true;
	d->ok = ok;
	d->cv.notify_all();
}

bool br_download_over( const std::shared_ptr<BrDownload> &d )
{
	if ( d == nullptr ) return true;
	std::lock_guard<std::mutex> hold(d->m);
	return d->over || d->stopped;
}

/*
 * The browser closing: what is still coming is stopped as 中止 stops it
 * (Chrome cancels the downloads under way when it is closed), each
 * thread throwing its object away, and waited for so that none is left
 * half made
 */
void br_downloads_close( int wait_ms )
{
	std::unique_lock<std::mutex> hold(g_dl_live_lock);
	g_dl_closing = true;
	if ( g_dl_live.empty() ) return;
	printf("browser: closing: %zu downloads under way stopped\n", g_dl_live.size());
	for ( const std::shared_ptr<BrDownload> &d : g_dl_live ) {
		std::lock_guard<std::mutex> h(d->m);
		d->stopped = true;
		d->cv.notify_all();
	}
	if ( !g_dl_live_cv.wait_for(hold, std::chrono::milliseconds(wait_ms), [] { return g_dl_live.empty(); }) ) {
		printf("browser: closing: %zu downloads not ended in %d ms\n", g_dl_live.size(), wait_ms);
	}
}
