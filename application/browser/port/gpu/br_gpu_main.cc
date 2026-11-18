/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_gpu_main.cc
 *	ブラウザ: the GPU process (design 14.13.10; make GL=1).
 *
 *	The program again, started by the browser with the first line "gpu"
 *	(br_proc.cc's gpu_start). It holds the GPU's work of every renderer:
 *	Chromium's service of the command buffers (gpu::GpuChannelManager,
 *	its channels and their stubs, the scheduler, the sync points and the
 *	shared images), on ui/gl's GL, which is ANGLE on its OpenGL ES
 *	backend over TessronOS's Mesa (port/gpu/gl_init_tessronos.cc). Skia's
 *	Ganesh, which the raster's decoder plays the pages' records with, is
 *	on that GL too, and so is Dawn's OpenGL ES backend, which WebGPU's
 *	decoder (the server of Dawn's wire) runs WebGPU on, where ANGLE
 *	makes contexts of OpenGL ES 3.1.
 *
 *	A process started on a machine with no GPU registered yet (the GL is
 *	then Mesa's softpipe) waits for one on a thread of its own: the GPU's
 *	device object tells it when one is registered, and the browser is
 *	told (GpuHost.GpuArrived), which starts another process on the GPU.
 *
 *	It makes the windows' GPU buffers too (GpuService.MakeGsurf, design
 *	14.13.10): two BOs a page's window is shown from, as shared images
 *	the page's raster draws into, handed to the browser as keys to their
 *	objects.
 *
 *	The browser's pipe (GpuService) is bound on the main thread, where
 *	the channel manager lives, as the channels' work is run there; Mojo
 *	watches the channels on its IO thread. The process brings up neither
 *	Blink, V8, the faces nor the network.
 *
 *	There is no windowing platform and no sandbox helper of Chromium's
 *	here, so gpu::GpuInit is not used: the GL is brought up as it does
 *	(the static bindings, the display, the graphics information, the
 *	extension settings, the default offscreen surface), and what it would
 *	compute of the GPU's features is set here: every feature is taken,
 *	with no driver's workaround from the lists, the drivers being known
 *	(Mesa's v3d, iris and softpipe) and ANGLE keeping its own.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <map>
#include <memory>
#include <string>

#include <dawn/dawn_proc.h>
#include <dawn/native/DawnNative.h>

#include "base/base_paths.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/path_service.h"
#include "base/run_loop.h"
#include "base/task/single_thread_task_executor.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "base/unguessable_token.h"
#include "base/files/scoped_file.h"
#include "components/viz/common/resources/shared_image_format.h"
#include "gpu/command_buffer/common/mailbox.h"
#include "gpu/command_buffer/common/shared_image_info.h"
#include "gpu/command_buffer/common/shared_image_usage.h"
#include "gpu/command_buffer/service/memory_tracking.h"
#include "gpu/command_buffer/service/scheduler.h"
#include "gpu/command_buffer/service/shared_context_state.h"
#include "gpu/command_buffer/service/shared_image/gl_texture_holder.h"
#include "gpu/command_buffer/service/shared_image/shared_image_backing.h"
#include "gpu/command_buffer/service/shared_image/shared_image_factory.h"
#include "gpu/command_buffer/service/shared_image/shared_image_format_service_utils.h"
#include "gpu/command_buffer/service/shared_image/shared_image_gl_utils.h"
#include "gpu/command_buffer/service/shared_image/shared_image_manager.h"
#include "gpu/command_buffer/service/shared_image/shared_image_representation.h"
#include "gpu/command_buffer/service/shared_image/skia_gl_image_representation.h"
#include "gpu/command_buffer/service/sync_point_manager.h"
#include "gpu/command_buffer/service/texture_manager.h"
#include "gpu/config/gpu_feature_info.h"
#include "gpu/config/gpu_finch_features.h"
#include "gpu/config/gpu_info.h"
#include "gpu/config/gpu_info_collector.h"
#include "gpu/config/gpu_preferences.h"
#include "gpu/ipc/common/gpu_feature_info.mojom.h"
#include "gpu/ipc/common/gpu_feature_info_mojom_traits.h"
#include "gpu/ipc/common/gpu_info.mojom.h"
#include "gpu/ipc/common/gpu_info_mojom_traits.h"
#include "gpu/ipc/common/shared_image_capabilities.mojom.h"
#include "gpu/ipc/common/shared_image_capabilities_mojom_traits.h"
#include "gpu/ipc/service/gpu_channel.h"
#include "gpu/ipc/service/gpu_channel_manager.h"
#include "gpu/ipc/service/gpu_channel_manager_delegate.h"
#include "gpu/ipc/service/shared_image_stub.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/platform/platform_channel_endpoint.h"
#include "mojo/public/cpp/platform/platform_handle.h"
#include "mojo/public/cpp/system/invitation.h"
#include "mojo/public/cpp/system/message_pipe.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "third_party/skia/include/core/SkPixmap.h"
#include "ui/gfx/gpu_extra_info.h"
#include "ui/gl/gl_bindings.h"
#include "ui/gl/gl_context.h"
#include "ui/gl/gl_display.h"
#include "ui/gl/gl_implementation.h"
#include "ui/gl/gl_surface.h"
#include "ui/gl/scoped_binders.h"
#include "ui/gl/scoped_egl_image.h"
#include "ui/gl/init/gl_factory.h"
#include "ui/base/ime/input_method.h"
#include "ui/display/types/native_display_delegate.h"
#include "ui/ozone/public/ozone_platform.h"
#include "ui/ozone/public/platform_screen.h"
#include "ui/ozone/public/system_input_injector.h"
#include "ui/platform_window/platform_window_init_properties.h"
#include "url/gurl.h"

#include "application/browser/mojom/br_process.mojom.h"

#include "../../br.h"
#include "../../br_proc.h"
#include "br_gpu.h"

/* br_proc.cc: the thread Mojo watches the channels on */
scoped_refptr<base::SingleThreadTaskRunner> br_proc_io_runner();
/* br_main.cc: a step of the start noted, for the console */
void br_start_mark( const char *what );

/*
 * What the raster's decoder took in this process (patch
 * raster_decoder_time.patch): the rasters ended, the microseconds of
 * their records played and of their flushes; the readbacks, the waits for
 * the drawing to finish before them and their copies
 */
namespace gpu::raster {
extern std::atomic<int64_t> g_raster_work[6];
/*
 * The readbacks: their number, their pixels, the microseconds of
 * glReadPixels in them (gl_init_tessronos.cc adds them), those from BGRA
 * and those into BGRA
 */
extern std::atomic<int64_t> g_readback_work[5];
}
namespace gpu {
/* The transfer buffers registered (patch raster_decoder_time.patch, transfer_buffer_manager.cc) */
extern std::atomic<int64_t> g_transfer_buffers_made;
}

namespace mj = br::mojom;

namespace {

long mono_ms()
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/*
 * Every 5 seconds with readbacks in them, a line of what they took: Skia's
 * readPixels whole, the GL's glReadPixels in it and the rest (the colours'
 * conversion), and the transfer buffers registered meanwhile
 */
void say_readbacks()
{
	static int64_t last[8];
	int64_t now[8] = {
		gpu::raster::g_readback_work[0].load(), gpu::raster::g_readback_work[1].load(),
		gpu::raster::g_readback_work[2].load(), gpu::raster::g_readback_work[3].load(),
		gpu::raster::g_readback_work[4].load(), gpu::raster::g_raster_work[5].load(),
		gpu::g_transfer_buffers_made.load(), 0
	};
	int64_t n = now[0] - last[0];
	if ( n > 0 ) {
		int64_t from_bgra = now[3] - last[3], to_bgra = now[4] - last[4];
		int64_t skia = now[5] - last[5], gl = now[2] - last[2];
		printf("browser: gpu readback: %lld reads, %lld kpx; source %s, dest %s; skia %lld us (gl %lld us, convert %lld us);"
		       " new transfer buffers %lld\n",
		       (long long)n, (long long)( ( now[1] - last[1] ) / 1000 ),
		       from_bgra == n ? "BGRA" : from_bgra == 0 ? "RGBA" : "mixed",
		       to_bgra == n ? "BGRA" : to_bgra == 0 ? "RGBA" : "mixed", (long long)skia, (long long)gl,
		       (long long)( skia - gl ), (long long)( now[6] - last[6] ));
	}
	for ( int i = 0; i < 8; i++ ) last[i] = now[i];
}

void watch_readbacks()
{
	say_readbacks();
	base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(FROM_HERE, base::BindOnce(&watch_readbacks),
									  base::Seconds(5));
}

/* The channel manager's delegate: the process keeps no disk cache and is not restarted from here */
class Delegate final : public gpu::GpuChannelManagerDelegate {
public:
	void LoseAllContexts() override {}
	void DidCreateContextSuccessfully() override {}
	void DidCreateOffscreenContext( const GURL & ) override {}
	void DidDestroyChannel( int client_id ) override
	{
		printf("browser: gpu process: the channel of %d is closed\n", client_id);
	}
	void DidDestroyAllChannels() override {}
	void DidDestroyOffscreenContext( const GURL & ) override {}
	void DidLoseContext( gpu::error::ContextLostReason reason, const GURL & ) override
	{
		printf("browser: gpu process: a context was lost (reason %d)\n", (int)reason);
	}
	void StoreBlobToDisk( const gpu::GpuDiskCacheHandle &, const std::string &, const std::string & ) override {}
	void GetIsolationKey( int, const blink::WebGPUExecutionContextToken &, GetIsolationKeyCallback cb ) override
	{
		std::move(cb).Run(std::string());
	}
	void MaybeExitOnContextLost( gpu::error::ContextLostReason ) override {}
	bool IsExiting() const override { return false; }
	gpu::Scheduler *GetGpuScheduler() override;
};

/*
 * The windowing platform the service asks of (it is built for Chromium's
 * Ozone): the process has no windows, no input and no native pixmaps,
 * so the shared images are those of GL textures only
 */
class NoWindows final : public ui::OzonePlatform {
public:
	ui::SurfaceFactoryOzone *GetSurfaceFactoryOzone() override { return nullptr; }
	ui::OverlayManagerOzone *GetOverlayManager() override { return nullptr; }
	ui::CursorFactory *GetCursorFactory() override { return nullptr; }
	ui::InputController *GetInputController() override { return nullptr; }
	ui::GpuPlatformSupportHost *GetGpuPlatformSupportHost() override { return nullptr; }
	std::unique_ptr<ui::SystemInputInjector> CreateSystemInputInjector() override { return nullptr; }
	std::unique_ptr<ui::PlatformWindow> CreatePlatformWindow( ui::PlatformWindowDelegate *,
								  ui::PlatformWindowInitProperties ) override
	{
		return nullptr;
	}
	std::unique_ptr<display::NativeDisplayDelegate> CreateNativeDisplayDelegate() override { return nullptr; }
	std::unique_ptr<ui::PlatformScreen> CreateScreen() override { return nullptr; }
	void InitScreen( ui::PlatformScreen * ) override {}
	std::unique_ptr<ui::InputMethod> CreateInputMethod( ui::ImeKeyEventDispatcher *, gfx::AcceleratedWidget ) override
	{
		return nullptr;
	}
	bool IsWindowCompositingSupported() const override { return false; }
	bool InitializeUI( const InitParams & ) override { return false; }
	void InitializeGPU( const InitParams & ) override {}
};

struct Gpu {
	gl::GLDisplay				*display = nullptr;
	scoped_refptr<gl::GLSurface>		surface;	/* the default offscreen surface */
	gpu::GPUInfo				info;
	gpu::GpuFeatureInfo			feature;
	gpu::GpuPreferences			prefs;
	std::unique_ptr<gpu::SyncPointManager>	sync;
	std::unique_ptr<gpu::Scheduler>		scheduler;
	std::unique_ptr<gpu::SharedImageManager> images;
	std::unique_ptr<gpu::GpuChannelManager>	manager;
	Delegate				delegate;
	scoped_refptr<base::SingleThreadTaskRunner> main;
	bool					started = false;
	bool					ok = false;
	bool					on_gpu = false;	/* the GL draws on a GPU, not on the processor */
	bool					raster = false;	/* the pages are rastered here */
	bool					webgl2 = false;	/* the GL has OpenGL ES 3, for WebGL 2's contexts */
	bool					webgpu = false;	/* ANGLE makes contexts of OpenGL ES 3.1, for Dawn */
	std::string				renderer, why;
	int					channels = 0;
	mojo::Remote<mj::GpuHost>		host;		/* the browser, told of a GPU that comes */
	/*
	 * The windows' GPU buffers (design 14.13.10): how they are filled
	 * ("drawn" by the GPU into the BOs, "copied" into them as each raster
	 * on the processor's GL ends, "" none and why)
	 */
	std::string				gs_how, gs_why;
};

Gpu	g;

gpu::Scheduler *Delegate::GetGpuScheduler()
{
	return g.scheduler.get();
}

/*
 * The paths of the program and its directory, which Dawn's instance asks
 * for to look for libraries in (it loads none here): the program is an
 * object of the system's, not a file of a directory, so there are none.
 * Answered here, before base's provider, which would read a link of
 * /proc that the system does not have and stop the process.
 */
bool no_program_path( int key, base::FilePath *path )
{
	(void)path;
	return key == base::FILE_EXE || key == base::FILE_MODULE || key == base::DIR_EXE || key == base::DIR_MODULE;
}

/* A long list of names put out in lines of about 200 characters, for the console */
void say_list( const char *what, const std::string &names )
{
	size_t at = 0, n = 0;
	for ( size_t i = 0; i <= names.size(); i++ ) {
		if ( i < names.size() && names[i] != ' ' ) continue;
		if ( i > at ) n++;
		at = i + 1;
	}
	printf("browser: gpu process: %s (%zu):\n", what, n);
	std::string line;
	at = 0;
	for ( size_t i = 0; i <= names.size(); i++ ) {
		if ( i < names.size() && names[i] != ' ' ) continue;
		if ( i > at ) {
			if ( !line.empty() && line.size() + ( i - at ) > 200 ) {
				printf("browser: gpu process:   %s\n", line.c_str());
				line.clear();
			}
			if ( !line.empty() ) line += ' ';
			line.append(names, at, i - at);
		}
		at = i + 1;
	}
	if ( !line.empty() ) printf("browser: gpu process:   %s\n", line.c_str());
}

/*
 * Whether ANGLE's display makes a context of OpenGL ES 3.1, the version
 * Dawn's OpenGL ES backend asks for there. ANGLE gives no more than its
 * backend can carry out on the system's driver (V3D's v3d gives 3.0 when
 * the kernel takes no compute job); a context of 3.1 is made once and
 * let go of, before any context of the service is current.
 */
bool es31()
{
	gl::GLContextAttribs a;
	a.client_major_es_version = 3;
	a.client_minor_es_version = 1;
	a.allow_es_version_fallback = false;
	scoped_refptr<gl::GLContext> ctx = gl::init::CreateGLContext(nullptr, g.surface.get(), a);
	return ctx != nullptr;
}

void gs_choose();

/*
 * The GL brought up (ANGLE on Mesa), what it is told of, and the service
 * on it; 'g.why' says what failed
 */
void bring_up( int mode )
{
	long t0 = mono_ms();
	static NoWindows *platform = new NoWindows;	/* the one instance, for the process's life */
	(void)platform;
	if ( !gl::init::InitializeStaticGLBindingsOneOff() ) {
		g.why = "the GL's bindings did not start";
		return;
	}
	g.display = gl::init::InitializeGLNoExtensionsOneOff(/*init_bindings=*/false, gl::GpuPreference::kDefault);
	if ( g.display == nullptr ) {
		g.why = "ANGLE's display did not start";
		return;
	}
	if ( !gpu::CollectGraphicsInfoGL(&g.info, g.display) ) {
		g.why = "the GL's strings could not be read";
		return;
	}
	if ( !gl::init::InitializeExtensionSettingsOneOffPlatform(g.display) ) {
		g.why = "the EGL's extensions could not be set";
		return;
	}
	g.surface = gl::init::CreateOffscreenGLSurface(g.display, gfx::Size());
	if ( g.surface == nullptr ) {
		g.why = "no offscreen surface";
		return;
	}
	g.renderer = g.info.gl_renderer;
	g.on_gpu = g.renderer.find("softpipe") == std::string::npos && g.renderer.find("llvmpipe") == std::string::npos
		   && g.renderer.find("oftware") == std::string::npos;

	/*
	 * the passthrough decoder is the only one, on ANGLE: the GLES2 stubs of
	 * WebGL's contexts (GLES2DecoderPassthroughImpl) as the raster's; ANGLE
	 * makes their contexts with WebGL's attributes and checks and
	 * translates for WebGL there. Skia's backend is GL (Ganesh).
	 */
	g.prefs.use_passthrough_cmd_decoder = true;
	size_t es = g.info.gl_version.find("OpenGL ES ");
	g.webgl2 = es != std::string::npos && es + 10 < g.info.gl_version.size() && g.info.gl_version[es + 10] >= '3' &&
		   g.info.gl_version[es + 10] <= '9';
	g.prefs.gr_context_type = gpu::GrContextType::kGL;
	g.info.passthrough_cmd_decoder = true;
	for ( int i = 0; i < gpu::NUMBER_OF_GPU_FEATURE_TYPES; i++ ) {
		g.feature.status_values[i] = gpu::kGpuFeatureStatusEnabled;
	}

	/*
	 * WebGPU: the stubs of WebGPU's command buffers (WebGPUDecoderImpl, the
	 * server of Dawn's wire) run Dawn's OpenGL ES backend on ANGLE's display
	 * and its GL, as the shared images' representations for Dawn are made
	 * on ANGLE's EGL images. That backend gives adapters of the
	 * compatibility level only, so the level is always that one. The
	 * adapters are not checked against Chromium's list of adapters kept
	 * from WebGPU, which leaves out those on the processor (softpipe).
	 */
	g.webgpu = es31();
	if ( g.webgpu ) {
		dawnProcSetProcs(&dawn::native::GetProcs());
		g.prefs.enable_webgpu = true;
		g.prefs.use_webgpu_adapter = gpu::WebGPUAdapterName::kOpenGLES;
		g.prefs.force_webgpu_compat = true;
		g.prefs.disabled_dawn_features_list.push_back("adapter_blocklist");
	}

	g.sync = std::make_unique<gpu::SyncPointManager>();
	g.scheduler = std::make_unique<gpu::Scheduler>(g.sync.get());
	g.images = std::make_unique<gpu::SharedImageManager>(/*thread_safe=*/true);
	g.manager = std::make_unique<gpu::GpuChannelManager>(
		g.prefs, &g.delegate, /*watchdog=*/nullptr, g.main, br_proc_io_runner(), g.scheduler.get(), g.sync.get(),
		g.images.get(), g.feature, /*use_shader_cache_shm_count=*/nullptr, g.surface);
	g.ok = true;
	g.raster = mode == BR_GPU_ON || ( mode == BR_GPU_AUTO && g.on_gpu );
	/* the windows' GPU buffers (design 14.13.10): the word "gsurf=" on the line says how they are filled */
	gs_choose();
	printf("browser: gpu process %d: the command buffers' service on %s, %s, in %ld ms; the pages %s; WebGL %s%s here; "
	       "gsurf=%s\n",
	       (int)getpid(), g.renderer.c_str(), g.info.gl_version.c_str(), mono_ms() - t0,
	       g.raster ? "rastered here (Ganesh)" : mode == BR_GPU_OFF ? "on the processor (gpu=off)"
					       : "on the processor (softpipe; gpu=on rasters here)",
	       g.webgl2 ? "1 and 2" : "1", g.webgpu ? " and WebGPU (Dawn on ANGLE, OpenGL ES 3.1)" : "",
	       g.gs_how.empty() ? "none" : g.gs_how.c_str());
	if ( g.gs_how == "drawn" ) {
		printf("browser: gpu process %d: window buffers: drawn by the GPU (64x64 drawn and read back the same)\n",
		       (int)getpid());
	} else if ( g.gs_how.empty() ) {
		printf("browser: gpu process %d: window buffers: none (%s): each view is read back\n", (int)getpid(),
		       g.gs_why.c_str());
	}
	if ( !g.webgpu ) printf("browser: gpu process %d: no WebGPU: ANGLE makes no context of OpenGL ES 3.1\n", (int)getpid());
	say_list("EGL extensions", g.info.gl_ws_extensions);
	say_list("GL extensions", g.info.gl_extensions);
	/* glReadPixels in the readbacks is timed by the GL's bindings (gl_init_tessronos.cc) */
	g.main->PostDelayedTask(FROM_HERE, base::BindOnce(&watch_readbacks), base::Seconds(5));
}

/*
 * On the thread that watches the GPU's object: a GPU was registered after
 * this process started without one. The browser is told on the main
 * thread, and starts another process on it.
 */
void arrived( const char *what )
{
	std::string w = what;
	g.main->PostTask(FROM_HERE, base::BindOnce([]( std::string w ) {
		printf("browser: gpu process %d: the machine's GPU has come (%s): the browser is told\n", (int)getpid(),
		       w.c_str());
		if ( g.host.is_bound() ) g.host->GpuArrived(w);
	}, std::move(w)));
}

/* ---------------------------------------------------------------- the windows' GPU buffers */

/*
 * A window's buffer (design 14.13.10): a BO of the process's own context
 * of the GPU, linear, 0x00RRGGBB in a word (DRM_FORMAT_XRGB8888), which
 * the window manager shows the window's work area from, as a shared image
 * the renderers' raster draws a page's frames into.
 *
 * Where the GL draws on the GPU, the BO is the texture itself: ANGLE's EGL
 * takes it in as a DMA-BUF (EGL_EXT_image_dma_buf_import) and the GL
 * texture is its sibling, so the raster draws into the BO and the window
 * reads it with nothing copied; each write's end flushes the GL, so that
 * the GPU's jobs on it are in the kernel when the renderer is told the
 * frame was taken in, and the window shows it once they are over. Where
 * the GL draws on the processor (Mesa's softpipe, which takes in no
 * DMA-BUF), the texture is the GL's own and each write's end copies it
 * into the BO, mapped into this process.
 */
const uint32_t kXrgb8888 = 0x34325258;		/* DRM_FORMAT_XRGB8888, 'XR24' */

class GsBacking;

/* The texture of a window's buffer, to the passthrough decoder and to Skia (SkiaGLImageRepresentation) */
class GsTexture final : public gpu::GLTexturePassthroughImageRepresentation {
public:
	GsTexture( gpu::SharedImageManager *manager, GsBacking *backing, gpu::MemoryTypeTracker *tracker,
		   scoped_refptr<gpu::gles2::TexturePassthrough> tex );
	~GsTexture() override;

	const scoped_refptr<gpu::gles2::TexturePassthrough> &GetTexturePassthrough( size_t plane_index ) override
	{
		(void)plane_index;
		return tex_;
	}
	bool BeginAccess( GLenum mode ) override
	{
		writing_ = mode == GL_SHARED_IMAGE_ACCESS_MODE_READWRITE_CHROMIUM;
		return true;
	}
	void EndAccess() override;

private:
	GsBacking					*gs_;
	scoped_refptr<gpu::gles2::TexturePassthrough>	tex_;
	bool						writing_ = false;
};

class GsBacking final : public gpu::ClearTrackingSharedImageBacking {
public:
	/*
	 * 'image' the BO taken in by the EGL (the texture its sibling), or
	 * 'copy' the texture held for its copy into the BO mapped at 'map'
	 */
	GsBacking( const gpu::Mailbox &mailbox, const gpu::SharedImageInfo &info, size_t bytes,
		   scoped_refptr<gpu::SharedContextState> st, scoped_refptr<gpu::gles2::TexturePassthrough> tex,
		   gl::ScopedEGLImage image, scoped_refptr<gpu::GLTextureHolder> copy, void *map, size_t pitch )
		: gpu::ClearTrackingSharedImageBacking(mailbox, info, bytes, /*is_thread_safe=*/false), st_(std::move(st)),
		  tex_(std::move(tex)), image_(std::move(image)), copy_(std::move(copy)), map_(map), pitch_(pitch) {}

	~GsBacking() override
	{
		if ( !have_context() ) {
			tex_->MarkContextLost();
			if ( copy_ != nullptr ) copy_->SetContextLost();
		}
		copy_ = nullptr;
		tex_ = nullptr;
	}

	gpu::SharedImageBackingType GetType() const override
	{
		return copy_ != nullptr ? gpu::SharedImageBackingType::kGLTexture : gpu::SharedImageBackingType::kEGLImage;
	}

	void Update( std::unique_ptr<gfx::GpuFence> in_fence ) override { (void)in_fence; }

	/* A write ended, on the context it was made on: the GPU given its work, or the drawing copied into the BO */
	void written()
	{
		gl::GLApi *api = gl::g_current_gl_context;
		if ( copy_ == nullptr ) {
			api->glFlushFn();
			return;
		}
		SkPixmap pm(SkImageInfo::Make(size().width(), size().height(), kBGRA_8888_SkColorType, kPremul_SkAlphaType),
			    map_, pitch_);
		(void)copy_->ReadbackToMemory(pm);
		/* Skia's view of the GL's state is not what the copy left */
		st_->PessimisticallyResetGrContext();
	}

protected:
	std::unique_ptr<gpu::GLTexturePassthroughImageRepresentation>
	ProduceGLTexturePassthrough( gpu::SharedImageManager *manager, gpu::MemoryTypeTracker *tracker ) override
	{
		return std::make_unique<GsTexture>(manager, this, tracker, tex_);
	}

	std::unique_ptr<gpu::SkiaGaneshImageRepresentation>
	ProduceSkiaGanesh( gpu::SharedImageManager *manager, gpu::MemoryTypeTracker *tracker,
			   scoped_refptr<gpu::SharedContextState> context_state ) override
	{
		return gpu::SkiaGLImageRepresentation::Create(ProduceGLTexturePassthrough(manager, tracker),
							      std::move(context_state), manager, this, tracker);
	}

private:
	scoped_refptr<gpu::SharedContextState>		st_;
	scoped_refptr<gpu::gles2::TexturePassthrough>	tex_;
	gl::ScopedEGLImage				image_;
	scoped_refptr<gpu::GLTextureHolder>		copy_;
	void						*map_;
	size_t						pitch_;
};

GsTexture::GsTexture( gpu::SharedImageManager *manager, GsBacking *backing, gpu::MemoryTypeTracker *tracker,
		      scoped_refptr<gpu::gles2::TexturePassthrough> tex )
	: gpu::GLTexturePassthroughImageRepresentation(manager, backing, tracker), gs_(backing), tex_(std::move(tex)) {}

GsTexture::~GsTexture()
{
	if ( !has_context() ) tex_->MarkContextLost();
}

void GsTexture::EndAccess()
{
	if ( writing_ ) gs_->written();
	writing_ = false;
}

/* The buffers made for a window: their shared images (the manager's references) and their BOs */
struct GsPair {
	std::unique_ptr<gpu::SharedImageRepresentationFactoryRef>	ref[2];
	BrGsurfBo							bo[2];
};

std::map<int, GsPair>			g_gs_sets;
int					g_gs_next = 0;
gpu::MemoryTypeTracker			*g_gs_tracker;

/* The shared context the raster draws on made current; null when there is none */
scoped_refptr<gpu::SharedContextState> gs_context( std::string *why )
{
	gpu::ContextResult r = gpu::ContextResult::kSuccess;
	scoped_refptr<gpu::SharedContextState> st = g.manager->GetSharedContextState(&r);
	if ( st == nullptr || !st->MakeCurrent(nullptr, /*needs_gl=*/true) ) {
		*why = "no shared context to draw on (" + std::to_string((int)r) + ")";
		return nullptr;
	}
	return st;
}

/* A BO taken in by the EGL as a DMA-BUF: linear, 'pitch' bytes a row */
gl::ScopedEGLImage gs_import( const BrGsurfBo &bo, int w, int h, size_t pitch )
{
	int fd = br_sys_gsurf_bo_fd(&bo);
	if ( fd < 0 ) return gl::ScopedEGLImage();
	bool mods = g.info.gl_ws_extensions.find("EGL_EXT_image_dma_buf_import_modifiers") != std::string::npos;
	EGLint a[32];
	int n = 0;
	a[n++] = EGL_WIDTH;
	a[n++] = w;
	a[n++] = EGL_HEIGHT;
	a[n++] = h;
	a[n++] = EGL_LINUX_DRM_FOURCC_EXT;
	a[n++] = (EGLint)kXrgb8888;
	a[n++] = EGL_DMA_BUF_PLANE0_FD_EXT;
	a[n++] = fd;
	a[n++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
	a[n++] = 0;
	a[n++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
	a[n++] = (EGLint)pitch;
	if ( mods ) {
		/* linear (DRM_FORMAT_MOD_LINEAR, 0): the window manager reads the rows as they lie */
		a[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
		a[n++] = 0;
		a[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
		a[n++] = 0;
	}
	a[n++] = EGL_NONE;
	gl::ScopedEGLImage image = gl::MakeScopedEGLImage(EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, a);
	/* the EGL keeps the BO taken in: the descriptor (and the key it holds) is let go */
	close(fd);
	return image;
}

/* A texture the EGL image is the storage of */
scoped_refptr<gpu::gles2::TexturePassthrough> gs_texture_of( EGLImageKHR image )
{
	gl::GLApi *api = gl::g_current_gl_context;
	GLuint id = 0;
	api->glGenTexturesFn(1, &id);
	gl::ScopedTextureBinder bind(GL_TEXTURE_2D, id);
	api->glTexParameteriFn(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	api->glTexParameteriFn(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	api->glTexParameteriFn(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	api->glTexParameteriFn(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	while ( api->glGetErrorFn() != GL_NO_ERROR ) {
	}
	api->glEGLImageTargetTexture2DOESFn(GL_TEXTURE_2D, image);
	if ( api->glGetErrorFn() != GL_NO_ERROR ) {
		api->glDeleteTexturesFn(1, &id);
		return nullptr;
	}
	return base::MakeRefCounted<gpu::gles2::TexturePassthrough>(id, GL_TEXTURE_2D);
}

/*
 * Whether the GPU draws into a BO taken in as above, once, as the process
 * starts: a BO of 64 x 64 cleared to one colour, its first 8 rows of the
 * texture to another, read where the window manager reads it. The rows
 * must lie as the window shows them: the texture's first row (the top of
 * what Skia draws for a surface of kTopLeft_GrSurfaceOrigin) first.
 */
bool gs_probe( std::string *why )
{
	if ( g.info.gl_ws_extensions.find("EGL_EXT_image_dma_buf_import") == std::string::npos ) {
		*why = "ANGLE's EGL takes in no DMA-BUF (no EGL_EXT_image_dma_buf_import)";
		return false;
	}
	scoped_refptr<gpu::SharedContextState> st = gs_context(why);
	if ( st == nullptr ) return false;
	const int w = 64, h = 64;
	const size_t pitch = (size_t)w * 4;
	BrGsurfBo bo;
	if ( !br_sys_gsurf_bo(pitch * h, true, &bo, why) ) return false;
	bool ok = false;
	{
		gl::ScopedEGLImage image = gs_import(bo, w, h, pitch);
		scoped_refptr<gpu::gles2::TexturePassthrough> tex = image.is_valid() ? gs_texture_of(image.get()) : nullptr;
		if ( !image.is_valid() ) {
			*why = "ANGLE's EGL did not take the BO in";
		} else if ( tex == nullptr ) {
			*why = "the GL made no texture of the BO";
		} else {
			gl::GLApi *api = gl::g_current_gl_context;
			GLuint fb = 0;
			api->glGenFramebuffersEXTFn(1, &fb);
			{
				gl::ScopedFramebufferBinder bind(fb);
				api->glFramebufferTexture2DEXTFn(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
								 tex->service_id(), 0);
				if ( api->glCheckFramebufferStatusEXTFn(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ) {
					*why = "the GL draws into no texture of the BO";
				} else {
					api->glDisableFn(GL_SCISSOR_TEST);
					api->glViewportFn(0, 0, w, h);
					api->glClearColorFn(32 / 255.0f, 64 / 255.0f, 96 / 255.0f, 1.0f);
					api->glClearFn(GL_COLOR_BUFFER_BIT);
					api->glEnableFn(GL_SCISSOR_TEST);
					api->glScissorFn(0, 0, w, 8);
					api->glClearColorFn(160 / 255.0f, 128 / 255.0f, 16 / 255.0f, 1.0f);
					api->glClearFn(GL_COLOR_BUFFER_BIT);
					api->glDisableFn(GL_SCISSOR_TEST);
					api->glFinishFn();
					const uint32_t *px = static_cast<const uint32_t *>(bo.map);
					uint32_t first = px[0] & 0xFFFFFF, last = px[( h - 1 ) * w + w - 1] & 0xFFFFFF;
					ok = first == 0xA08010 && last == 0x204060;
					if ( !ok ) {
						char b[128];
						snprintf(b, sizeof(b), "64x64 drawn, read back other than drawn (0x%06x first, 0x%06x last)",
							 (unsigned)first, (unsigned)last);
						*why = b;
					}
				}
				api->glFramebufferTexture2DEXTFn(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
			}
			api->glDeleteFramebuffersEXTFn(1, &fb);
		}
		tex = nullptr;
	}
	st->PessimisticallyResetGrContext();
	br_sys_gsurf_bo_free(&bo);
	return ok;
}

/* How the windows' buffers are to be filled here, found once as the GL comes up */
void gs_choose()
{
	if ( !g.raster ) {
		g.gs_why = "the pages are not rastered here";
	} else if ( !g.on_gpu ) {
		/* the GL draws on the processor: each raster's end copies the drawing into the BO */
		g.gs_how = "copied";
	} else if ( gs_probe(&g.gs_why) ) {
		g.gs_how = "drawn";
	}
}

/* One buffer of a window, 'w' x 'h' (rows of 'pitch' bytes), made a shared image; false and 'why' when it cannot be */
bool gs_one( const scoped_refptr<gpu::SharedContextState> &st, int w, int h, size_t pitch, BrGsurfBo *bo,
	     std::unique_ptr<gpu::SharedImageRepresentationFactoryRef> *ref, std::string *why )
{
	bool drawn = g.gs_how == "drawn";
	if ( !br_sys_gsurf_bo(pitch * (size_t)h, drawn, bo, why) ) return false;
	gl::ScopedEGLImage image;
	scoped_refptr<gpu::gles2::TexturePassthrough> tex;
	scoped_refptr<gpu::GLTextureHolder> copy;
	viz::SharedImageFormat format = viz::SinglePlaneFormat::kBGRA_8888;
	gfx::Size size(w, h);
	if ( drawn ) {
		image = gs_import(*bo, w, h, pitch);
		if ( !image.is_valid() ) {
			*why = "ANGLE's EGL did not take the BO in";
			return false;
		}
		tex = gs_texture_of(image.get());
		if ( tex == nullptr ) {
			*why = "the GL made no texture of the BO";
			return false;
		}
	} else {
		gpu::GLFormatDesc fd = st->GetGLFormatCaps().ToGLFormatDesc(format, /*plane_index=*/0);
		gl::GLApi *api = gl::g_current_gl_context;
		gpu::MakeTextureAndSetParameters(GL_TEXTURE_2D, /*framebuffer_attachment_angle=*/false, &tex, nullptr);
		gl::ScopedTextureBinder bind(GL_TEXTURE_2D, tex->service_id());
		while ( api->glGetErrorFn() != GL_NO_ERROR ) {
		}
		if ( gpu::IsTexStorage2DAvailable() ) {
			api->glTexStorage2DEXTFn(GL_TEXTURE_2D, 1, fd.storage_internal_format, w, h);
		} else {
			api->glTexImage2DFn(GL_TEXTURE_2D, 0, fd.image_internal_format, w, h, 0, fd.data_format, fd.data_type,
					    nullptr);
		}
		if ( api->glGetErrorFn() != GL_NO_ERROR ) {
			*why = "the GL made no texture of " + std::to_string(w) + "x" + std::to_string(h);
			return false;
		}
		copy = base::MakeRefCounted<gpu::GLTextureHolder>(format, size, /*is_passthrough=*/true, nullptr);
		copy->InitializeWithTexture(fd, tex);
	}
	gpu::SharedImageInfo info(format, size, gfx::ColorSpace::CreateSRGB(), kTopLeft_GrSurfaceOrigin, kPremul_SkAlphaType,
				  gpu::SharedImageUsageSet(gpu::SHARED_IMAGE_USAGE_RASTER_WRITE) |
					  gpu::SHARED_IMAGE_USAGE_RASTER_READ,
				  "BrWindowBuffer");
	auto backing = std::make_unique<GsBacking>(gpu::Mailbox::Generate(), info, pitch * (size_t)h, st, std::move(tex),
						   std::move(image), std::move(copy), bo->map, pitch);
	/* the BO starts zeroed and the texture is defined: no clear is asked before the first raster */
	backing->SetCleared();
	if ( g_gs_tracker == nullptr ) g_gs_tracker = new gpu::MemoryTypeTracker(nullptr);
	*ref = g.images->Register(std::move(backing), g_gs_tracker);
	if ( *ref == nullptr ) {
		*why = "the shared image was not registered";
		return false;
	}
	return true;
}

/* The buffers 'id' let go of, the shared context current */
void gs_free( GsPair &p )
{
	for ( int i = 0; i < 2; i++ ) {
		p.ref[i].reset();
		br_sys_gsurf_bo_free(&p.bo[i]);
	}
}

/* ---------------------------------------------------------------- the browser's pipe */

class ServiceImpl final : public mj::GpuService {
public:
	void Start( int32_t mode, StartCallback cb ) override
	{
		if ( !g.started ) {
			g.started = true;
			/*
			 * a GPU registered later is watched for before the GL looks for
			 * one, so that none is missed between the two
			 */
			br_sys_gpu_fake_begin();
			bool had = br_sys_gpu_watch(&arrived);
			bring_up(mode);
			br_sys_gpu_fake_end();
			if ( !g.ok ) printf("browser: gpu process %d: none (%s)\n", (int)getpid(), g.why.c_str());
			if ( !had ) printf("browser: gpu process %d: the machine has no GPU yet: one registered later is waited for\n",
					   (int)getpid());
		}
		std::move(cb).Run(g.ok, g.on_gpu, g.renderer, g.why);
	}

	void EstablishChannel( int32_t client_id, uint64_t tracing_id, EstablishChannelCallback cb ) override
	{
		if ( !g.ok ) {
			std::move(cb).Run(nullptr);
			return;
		}
		gpu::GpuChannel *ch = g.manager->EstablishChannel(base::UnguessableToken::Create(), client_id, tracing_id,
								  /*is_gpu_host=*/false,
								  /*enable_extra_handles_validation=*/false,
								  gfx::GpuExtraInfo(), g.info, g.feature);
		if ( ch == nullptr ) {
			printf("browser: gpu process: no channel for %d\n", client_id);
			std::move(cb).Run(nullptr);
			return;
		}
		mojo::MessagePipe pipe;
		if ( features::IsLegacyIpcDisabled() ) {
			ch->Start(std::move(pipe.handle0));
		} else {
			ch->Init(pipe.handle0.release());
		}
		gpu::SharedImageCapabilities caps = ch->shared_image_stub()->factory()->MakeCapabilities();
		auto info = mj::GpuChannelInfo::New();
		info->channel = std::move(pipe.handle1);
		info->gpu_info = gpu::mojom::GpuInfo::Serialize(&g.info);
		info->feature_info = gpu::mojom::GpuFeatureInfo::Serialize(&g.feature);
		info->shared_image_caps = gpu::mojom::SharedImageCapabilities::Serialize(&caps);
		info->raster = g.raster;
		info->webgl2 = g.webgl2;
		info->webgpu = g.webgpu;
		info->renderer = g.renderer;
		info->generation = 1;
		info->gpu_pid = (int32_t)getpid();
		g.channels++;
		printf("browser: gpu process: a channel for %d\n", client_id);
		std::move(cb).Run(std::move(info));
	}

	void CloseChannel( int32_t client_id ) override
	{
		if ( g.manager != nullptr ) g.manager->RemoveChannel(client_id);
	}

	void Query( const std::string &what, QueryCallback cb ) override
	{
		if ( what == "channels" ) {
			std::move(cb).Run(std::to_string(g.channels));
			return;
		}
		if ( what == "rasterstat" ) {
			char n[160];
			snprintf(n, sizeof(n), "%lld %lld %lld %lld %lld %lld", (long long)gpu::raster::g_raster_work[0].load(),
				 (long long)gpu::raster::g_raster_work[1].load(), (long long)gpu::raster::g_raster_work[2].load(),
				 (long long)gpu::raster::g_raster_work[3].load(), (long long)gpu::raster::g_raster_work[4].load(),
				 (long long)gpu::raster::g_raster_work[5].load());
			std::move(cb).Run(n);
			return;
		}
		std::move(cb).Run(std::string());
	}

	void Closing() override
	{
		fflush(stdout);
		_exit(0);
	}

	void MakeGsurf( int32_t w, int32_t h, MakeGsurfCallback cb ) override
	{
		std::vector<mj::GsurfBufPtr> bufs;
		std::string why;
		/* rows of a multiple of 256 bytes, as the window takes them (OB_GS_PITCH) */
		size_t pitch = ( (size_t)( w > 0 ? w : 0 ) * 4 + 255 ) / 256 * 256;
		if ( !g.ok || g.gs_how.empty() ) {
			std::move(cb).Run(0, 0, std::move(bufs), std::string(), g.ok ? g.gs_why : g.why);
			return;
		}
		if ( w <= 0 || h <= 0 || w > 16384 || h > 16384 ) {
			std::move(cb).Run(0, 0, std::move(bufs), std::string(),
					  "no buffers of " + std::to_string(w) + "x" + std::to_string(h));
			return;
		}
		scoped_refptr<gpu::SharedContextState> st = gs_context(&why);
		GsPair p;
		bool ok = st != nullptr;
		for ( int i = 0; ok && i < 2; i++ ) ok = gs_one(st, w, h, pitch, &p.bo[i], &p.ref[i], &why);
		for ( int i = 0; ok && i < 2; i++ ) {
			int fd = br_sys_gsurf_bo_fd(&p.bo[i]);
			if ( fd < 0 ) {
				why = "no descriptor of the BO to hand on";
				ok = false;
				break;
			}
			auto b = mj::GsurfBuf::New();
			b->bo = mojo::PlatformHandle(base::ScopedFD(fd));
			b->image = mj::GsurfImage::New();
			const gpu::Mailbox mb = p.ref[i]->mailbox();
			b->image->mailbox.assign(reinterpret_cast<const uint8_t *>(mb.name),
						 reinterpret_cast<const uint8_t *>(mb.name) + sizeof(mb.name));
			bufs.push_back(std::move(b));
		}
		if ( st != nullptr ) st->PessimisticallyResetGrContext();
		if ( !ok ) {
			gs_free(p);
			printf("browser: gpu process %d: no window buffers of %dx%d: %s\n", (int)getpid(), w, h, why.c_str());
			std::move(cb).Run(0, 0, std::vector<mj::GsurfBufPtr>(), std::string(), why);
			return;
		}
		int id = ++g_gs_next;
		g_gs_sets[id] = std::move(p);
		std::move(cb).Run(id, (uint32_t)pitch, std::move(bufs), g.gs_how, std::string());
	}

	void DropGsurf( int32_t id ) override
	{
		auto it = g_gs_sets.find(id);
		if ( it == g_gs_sets.end() ) return;
		std::string why;
		/* the textures go on the context they were made on */
		(void)gs_context(&why);
		gs_free(it->second);
		g_gs_sets.erase(it);
	}
};

}  // namespace

void br_gpu_main()
{
	br_base_up();
	base::PathService::RegisterProvider(&no_program_path, base::PATH_START, base::PATH_END);
	br_start_mark("the base and Mojo");
	int fd = br_sys_take(30000);
	if ( fd < 0 ) {
		printf("browser: gpu process: no channel was handed to it\n");
		_exit(1);
	}
	static base::SingleThreadTaskExecutor main_executor;
	g.main = main_executor.task_runner();
	mojo::IncomingInvitation inv = mojo::IncomingInvitation::Accept(
		mojo::PlatformChannelEndpoint(mojo::PlatformHandle(base::ScopedFD(fd))));
	mojo::ScopedMessagePipeHandle sp = inv.ExtractMessagePipe(mj::kGpuServicePipe);
	mojo::ScopedMessagePipeHandle hp = inv.ExtractMessagePipe(mj::kGpuHostPipe);
	mojo::Receiver<mj::GpuService> recv(new ServiceImpl, mojo::PendingReceiver<mj::GpuService>(std::move(sp)));
	recv.set_disconnect_handler(base::BindOnce([] {
		printf("browser: gpu process: the browser has gone\n");
		fflush(stdout);
		_exit(0);
	}));
	g.host.Bind(mojo::PendingRemote<mj::GpuHost>(std::move(hp), 0));
	printf("browser: gpu process %d: ready\n", (int)getpid());
	base::RunLoop().Run();
	_exit(0);
}
