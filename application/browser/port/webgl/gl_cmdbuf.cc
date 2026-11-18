/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	gl_cmdbuf.cc
 *	ブラウザ: WebGL 1 and 2 in the GPU process (design 14.13.10; make GL=1,
 *	gpuproc=on).
 *
 *	Blink's WebGL is given the GLES2 interface of a command buffer on the
 *	renderer's channel to the GPU process (viz::ContextProviderCommandBuffer
 *	made for WebGL): the calls are put in the command buffer by
 *	GLES2Implementation, and the GPU process's passthrough decoder gives
 *	them to ANGLE there, on a context made with WebGL's attributes. The
 *	fences, the queries of the command buffer's own targets, the shared
 *	images and the sync tokens are answered by the client and the decoder
 *	as they are made to.
 *
 *	A provider is made on the main thread (Blink asks there for a worker's
 *	OffscreenCanvas too) and bound on the thread that uses it, which then
 *	runs its replies, its error messages and the loss of its context.
 *
 *	What WebGL drew reaches the page without a readback while the pages
 *	are rastered in the GPU process: the canvas's snapshot for the page's
 *	paint (patch webgl_snapshot.patch) is the drawing buffer's colour
 *	buffer copied there into a shared image of the raster's context
 *	(br_gpu_client.cc), an image the page's record names by its mailbox
 *	and the page's raster draws in the GPU process. Otherwise (the pages
 *	rastered on the processor, a worker's OffscreenCanvas) the drawing
 *	buffer reads its colour buffer back (ReadPixels, one round trip to the
 *	GPU process) into the canvas's bitmap, as compositing is on the CPU
 *	(br_blink.cc). Both are counted as frames of WebGL for the console.
 *
 *	The decoder's messages of the errors ANGLE set come back to the
 *	context (CommandBufferClient::OnConsoleMessage) and go to Blink's error
 *	message callback, which puts them on the page's console; each different
 *	one is also put once on the system's console.
 *
 *	When the GPU process ends, the context is lost: Blink is told and gives
 *	the page webglcontextlost, and asks for a new provider when the page
 *	wants the context restored; that one is made on the new channel.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/functional/function_ref.h"
#include "base/memory/scoped_refptr.h"
#include "cc/paint/paint_image.h"
#include "components/viz/common/gpu/context_lost_observer.h"
#include "gpu/command_buffer/client/client_shared_image.h"
#include "gpu/command_buffer/client/context_support.h"
#include "gpu/command_buffer/client/gles2_interface.h"
#include "gpu/command_buffer/client/gles2_trace_implementation.h"
#include "gpu/command_buffer/common/capabilities.h"
#include "gpu/command_buffer/common/context_result.h"
#include "gpu/command_buffer/common/sync_token.h"
#include "gpu/config/gpu_feature_info.h"
#include "gpu/config/gpu_info.h"
#include "gpu/ipc/client/gpu_channel_host.h"
#include "services/viz/public/cpp/gpu/context_provider_command_buffer.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/public/platform/web_graphics_context_3d_provider.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/platform/web_url.h"
#include "third_party/blink/renderer/platform/graphics/gpu/drawing_buffer.h"
#include "third_party/blink/renderer/platform/graphics/static_bitmap_image.h"
#include "third_party/blink/renderer/platform/graphics/unaccelerated_static_bitmap_image.h"
#include "third_party/skia/include/core/SkColorSpace.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "url/gurl.h"

/*
 * br_gpu_client.cc: the renderer's channel to the GPU process, for WebGL;
 * null when there is none, with 'later' true while a new one is asked for
 * (the GPU process is being started again), and 'webgl2' whether its GL
 * has OpenGL ES 3
 */
scoped_refptr<gpu::GpuChannelHost> br_gpu_webgl_channel(bool* later, bool* webgl2);
/* br_gpu_client.cc: a frame copied into a shared image of the page's raster, or an empty image */
cc::PaintImage br_gpu_frame_image(
    const SkImageInfo& info, GrSurfaceOrigin origin,
    base::FunctionRef<std::optional<gpu::SyncToken>(gpu::raster::RasterInterface*,
                                                    const scoped_refptr<gpu::ClientSharedImage>&,
                                                    const gpu::SyncToken&)>
        copy);

namespace blink {
/* The snapshot of a WebGL canvas for the page's paint (patch webgl_snapshot.patch) */
extern scoped_refptr<StaticBitmapImage> (*g_webgl_gpu_snapshot)(DrawingBuffer* drawing_buffer,
                                                                SourceDrawingBuffer source_buffer,
                                                                SkAlphaType alpha_type);
}  // namespace blink

namespace {

std::atomic<long> g_frames{0};  // frames shown: whole colour buffers read back, and copies in the GPU process
std::atomic<long> g_copies{0};  // of them, the copies in the GPU process

/* The providers made here, which the canvas's snapshot takes from */
std::mutex g_mu;
std::set<const blink::WebGraphicsContext3DProvider*> g_ours;

/*
 * The GLES2 interface Blink is given: every call goes on to the command
 * buffer's, and a readback of the whole drawing buffer for the canvas is
 * counted as a frame. The drawing buffer sets the pack alignment to 1
 * right before it reads its framebuffer from the origin.
 */
class Counting final : public gpu::gles2::GLES2TraceImplementation {
 public:
  explicit Counting(gpu::gles2::GLES2Interface* gl) : GLES2TraceImplementation(gl) {}

  void PixelStorei(GLenum pname, GLint param) override {
    if (pname == GL_PACK_ALIGNMENT)
      pack_one_ = param == 1;
    GLES2TraceImplementation::PixelStorei(pname, param);
  }

  void ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type,
                  void* pixels) override {
    if (pack_one_ && x == 0 && y == 0 && width > 0 && height > 0 && format == GL_RGBA &&
        pixels != nullptr) {
      g_frames++;
      static std::atomic<bool> told{false};
      if (!told.exchange(true))
        printf("browser: webgl: a frame of the GPU process was read back to be shown\n");
    }
    GLES2TraceImplementation::ReadPixels(x, y, width, height, format, type, pixels);
  }

 private:
  bool pack_one_ = false;
};

/* An error message of a context: put once on the system's console, each different one */
void Said(const base::RepeatingCallback<void(const char*, int32_t)>& to, const char* message,
          int32_t id) {
  {
    static std::mutex mu;
    static std::set<std::string> told;
    std::lock_guard<std::mutex> l(mu);
    /* the same message of another context differs only in the context's name in brackets before it */
    const char* what = message;
    if (what[0] == '[') {
      const char* end = strchr(what, ']');
      if (end != nullptr)
        what = end + 1;
    }
    while (*what == ' ')
      what++;
    if (told.size() < 64 && told.insert(what).second)
      printf("browser: webgl: %s\n", message);
  }
  if (to)
    to.Run(message, id);
}

class Provider final : public blink::WebGraphicsContext3DProvider, public viz::ContextLostObserver {
 public:
  Provider(scoped_refptr<viz::ContextProviderCommandBuffer> ctx, bool es3)
      : ctx_(std::move(ctx)), es3_(es3) {
    prefs_.anti_aliasing_mode = blink::kAntialiasingModeNone;
    std::lock_guard<std::mutex> l(g_mu);
    g_ours.insert(this);
  }

  ~Provider() override {
    {
      std::lock_guard<std::mutex> l(g_mu);
      g_ours.erase(this);
    }
    gl_.reset();
    if (observing_)
      ctx_->RemoveObserver(this);
  }

  /* On the thread that will use the context: its replies and its loss are run there */
  bool BindToCurrentSequence() override {
    if (tried_)
      return bound_;
    tried_ = true;
    ctx_->AddObserver(this);
    observing_ = true;
    gpu::ContextResult r = ctx_->BindToCurrentSequence();
    if (r != gpu::ContextResult::kSuccess) {
      static std::atomic<bool> told{false};
      if (!told.exchange(true))
        printf("browser: webgl: WebGL %d in the GPU process could not be made (%d)\n", es3_ ? 2 : 1,
               static_cast<int>(r));
      return false;
    }
    bound_ = true;
    gl_ = std::make_unique<Counting>(ctx_->ContextGL());
    if (on_error_)
      ctx_->ContextSupport()->SetErrorMessageCallback(base::BindRepeating(&Said, on_error_));
    return true;
  }

  gpu::InterfaceBase* InterfaceBase() override { return ContextGL(); }
  gpu::gles2::GLES2Interface* ContextGL() override { return gl_.get(); }
  /* The pages are not rastered with the WebGL context: there is no raster interface on it */
  gpu::raster::RasterInterface* RasterInterface() override { return nullptr; }
  gpu::webgpu::WebGPUInterface* WebGPUInterface() override { return nullptr; }
  gpu::ContextSupport* ContextSupport() override {
    return bound_ ? ctx_->ContextSupport() : nullptr;
  }
  bool IsContextLost() override {
    return bound_ && ctx_->ContextGL()->GetGraphicsResetStatusKHR() != GL_NO_ERROR;
  }
  const gpu::Capabilities& GetCapabilities() const override {
    return bound_ ? ctx_->ContextCapabilities() : caps_;
  }
  const gpu::GpuFeatureInfo& GetGpuFeatureInfo() const override {
    return ctx_->GetGpuFeatureInfo();
  }
  const blink::WebglPreferences& GetWebglPreferences() const override { return prefs_; }
  void SetLostContextCallback(base::RepeatingClosure cb) override { on_lost_ = std::move(cb); }
  void SetErrorMessageCallback(base::RepeatingCallback<void(const char*, int32_t)> cb) override {
    on_error_ = std::move(cb);
    if (bound_)
      ctx_->ContextSupport()->SetErrorMessageCallback(base::BindRepeating(&Said, on_error_));
  }
  /*
   * As with the contexts in the renderer, the images the canvases take are
   * decoded and drawn on the processor (compositing is on the CPU)
   */
  cc::ImageDecodeCache* ImageDecodeCache(SkColorType) override { return nullptr; }
  gpu::SharedImageInterface* SharedImageInterface() override {
    return ctx_->SharedImageInterface();
  }
  viz::RasterContextProvider* RasterContextProvider() const override { return nullptr; }

  /* viz::ContextLostObserver, on the bound thread: the GPU process ended, or the context was lost there */
  void OnContextLost() override {
    static std::atomic<bool> told{false};
    if (!told.exchange(true))
      printf("browser: webgl: a context in the GPU process is lost: the page is told (webglcontextlost)\n");
    if (on_lost_)
      on_lost_.Run();
  }

 private:
  scoped_refptr<viz::ContextProviderCommandBuffer> ctx_;
  std::unique_ptr<Counting> gl_;  // made on binding; let go of before the context
  bool es3_;
  bool tried_ = false;
  bool bound_ = false;
  bool observing_ = false;
  base::RepeatingClosure on_lost_;
  base::RepeatingCallback<void(const char*, int32_t)> on_error_;
  gpu::Capabilities caps_;  // what a context not bound has
  blink::WebglPreferences prefs_;
};

/*
 * The snapshot of a canvas of a context here for the page's paint: the
 * drawing buffer's colour buffer (bottom up, as GL's) drawn in the GPU
 * process into a shared image of the page's raster, top down (Skia
 * writes into shared images of that order only), so the copy turns it.
 * Null when the pages are not rastered there, on another thread, or for
 * another context: the snapshot is then read back.
 */
scoped_refptr<blink::StaticBitmapImage> GpuSnapshot(blink::DrawingBuffer* db, blink::SourceDrawingBuffer src,
                                                    SkAlphaType alpha) {
  if (db == nullptr)
    return nullptr;
  {
    std::lock_guard<std::mutex> l(g_mu);
    if (g_ours.count(db->ContextProvider()) == 0)
      return nullptr;
  }
  gfx::Size size = db->Size();
  if (size.IsEmpty())
    return nullptr;
  SkImageInfo info = SkImageInfo::Make(size.width(), size.height(), kRGBA_8888_SkColorType,
                                       alpha == kUnknown_SkAlphaType ? kPremul_SkAlphaType : alpha,
                                       SkColorSpace::MakeSRGB());
  cc::PaintImage image = br_gpu_frame_image(
      info, kTopLeft_GrSurfaceOrigin,
      [&](gpu::raster::RasterInterface* ri, const scoped_refptr<gpu::ClientSharedImage>& dst,
          const gpu::SyncToken& token) { return db->CopyToPlatformSharedImage(ri, dst, token, src); });
  if (!image)
    return nullptr;
  g_frames++;
  if (g_copies++ == 0)
    printf("browser: webgl: the frames are shown from shared images of the GPU process, with no readback\n");
  return blink::UnacceleratedStaticBitmapImage::Create(std::move(image));
}

}  // namespace

/* The frames of WebGL in the GPU process shown since the start (gl_angle.cc's br_webgl_frames adds them) */
long br_webgl_cmdbuf_frames() {
  return g_frames;
}

/*
 * A WebGL context in the GPU process (es3: WebGL 2), on the main thread.
 * Null when the renderer has no channel to it, or its GL has no OpenGL
 * ES 3 for WebGL 2; 'later' is then true while a new channel is asked for
 * (the caller makes no context elsewhere: Blink tries again to restore a
 * lost one).
 */
std::unique_ptr<blink::WebGraphicsContext3DProvider> BrCreateCmdBufGLProvider(
    bool es3, bool prefer_low_power_gpu, bool fail_if_major_performance_caveat,
    const blink::WebURL& url, blink::Platform::WebGLContextInfo* info, bool* later) {
  static std::atomic<bool> told[2];
  bool webgl2 = false;
  *later = false;
  scoped_refptr<gpu::GpuChannelHost> host = br_gpu_webgl_channel(later, &webgl2);
  if (host == nullptr) {
    if (*later) {
      static std::atomic<bool> waited{false};
      if (!waited.exchange(true))
        printf("browser: webgl: no context while the GPU process is started again\n");
    }
    return nullptr;
  }
  if (es3 && !webgl2) {
    static std::atomic<bool> no2{false};
    if (!no2.exchange(true))
      printf("browser: webgl: no WebGL 2 in the GPU process (its GL has no OpenGL ES 3)\n");
    return nullptr;
  }
  const gpu::GPUInfo& gi = host->gpu_info();
  if (info != nullptr) {
    const gpu::GPUInfo::GPUDevice& dev = gi.active_gpu();
    info->vendor_id = dev.vendor_id;
    info->device_id = dev.device_id;
    info->renderer_info = blink::WebString::FromUtf8(gi.gl_renderer);
    info->vendor_info = blink::WebString::FromUtf8(gi.gl_vendor);
    info->driver_version = blink::WebString::FromUtf8(gi.gl_version);
    info->reset_notification_strategy = gi.gl_reset_notification_strategy;
    info->using_gpu_compositing = false;
    info->using_passthrough_command_decoder = gi.passthrough_cmd_decoder;
    info->angle_implementation = gi.gl_implementation_parts.angle;
  }
  /* the page's address names the context in the GPU process's messages */
  GURL page = url.IsNull() ? GURL() : GURL(url.GetString().Utf8(), url.GetParsed(), url.IsValid());
  auto ctx = viz::ContextProviderCommandBuffer::CreateForWebGL(
      std::move(host), page, es3 ? viz::WebGLContextType::kWebGL2 : viz::WebGLContextType::kWebGL1,
      prefer_low_power_gpu, fail_if_major_performance_caveat);
  if (!told[es3].exchange(true))
    printf("browser: webgl: WebGL %d in the GPU process, %s\n", es3 ? 2 : 1, gi.gl_renderer.c_str());
  blink::g_webgl_gpu_snapshot = &GpuSnapshot;
  return std::make_unique<Provider>(std::move(ctx), es3);
}
