/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	webgpu_cmdbuf.cc
 *	ブラウザ: WebGPU in the GPU process (design 14.13.10; make GL=1,
 *	gpuproc=on).
 *
 *	Blink's WebGPU is given the WebGPU interface of a command buffer on
 *	the renderer's channel to the GPU process (viz::ContextProviderCommand-
 *	Buffer made for WebGPU): WebGPUImplementation is the client of Dawn's
 *	wire and puts what it serializes in the command buffer, and the GPU
 *	process's decoder (WebGPUDecoderImpl) is the wire's server there, on
 *	Dawn's OpenGL ES backend over ANGLE. The answers, the late work of the
 *	server (a buffer mapped, a queue's work done) and the loss of the
 *	context come back through the command buffer.
 *
 *	A context is bound on the thread that uses it. Its answers are run
 *	there, but for a context made to be waited on (WebGL on WebGPU: ANGLE
 *	asks for its device and blocks until it has it), whose answers are
 *	taken on the IO thread.
 *
 *	A canvas's textures are shared images of the GPU process. The page's
 *	paint asks for the pixels of the current one (patch
 *	gpu_canvas_context.patch): while it is the canvas's, Dawn holds it, so
 *	it is copied on the canvas's device into a shared image of the
 *	canvas's own, which the raster's context reads back (br_gpu_client.cc)
 *	once the copy's sync token has passed. With no current texture, the
 *	pixels last read are given.
 */

#include <stdint.h>
#include <stdio.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>

#include <dawn/dawn_proc.h>
#include <dawn/wire/WireClient.h>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/single_thread_task_runner.h"
#include "components/viz/common/gpu/context_lost_observer.h"
#include "gpu/command_buffer/client/client_shared_image.h"
#include "gpu/command_buffer/client/context_support.h"
#include "gpu/command_buffer/client/shared_image_interface.h"
#include "gpu/command_buffer/client/webgpu_interface.h"
#include "gpu/command_buffer/common/capabilities.h"
#include "gpu/command_buffer/common/context_result.h"
#include "gpu/command_buffer/common/mailbox.h"
#include "gpu/command_buffer/common/shared_image_info.h"
#include "gpu/command_buffer/common/shared_image_usage.h"
#include "gpu/command_buffer/common/sync_token.h"
#include "gpu/command_buffer/common/webgpu_cmd_enums.h"
#include "gpu/config/gpu_feature_info.h"
#include "gpu/ipc/client/gpu_channel_host.h"
#include "gpu/ipc/common/surface_handle.h"
#include "services/viz/public/cpp/gpu/command_buffer_metrics.h"
#include "services/viz/public/cpp/gpu/context_provider_command_buffer.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/public/platform/web_graphics_context_3d_provider.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/platform/web_url.h"
#include "third_party/blink/renderer/platform/graphics/gpu/webgpu_cpp.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkColorSpace.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "url/gurl.h"

/*
 * br_gpu_client.cc: the renderer's channel to the GPU process, null when
 * there is none ('later' true while a new one is asked for), 'webgpu'
 * whether the GPU process runs Dawn; a shared image of it read back
 */
scoped_refptr<gpu::GpuChannelHost> br_gpu_webgpu_channel(bool* later, bool* webgpu);
bool br_gpu_read_image(const gpu::Mailbox& mailbox, const gpu::SyncToken& token, const SkImageInfo& info,
                       void* px, size_t row_bytes);
/* webgpu_tessronos.cc: the canvas's hooks, which hand this file's canvases here */
void BrWebGPUCanvasHooks();

namespace {

class Provider;

/*
 * The WebGPU interfaces of the bound contexts, and per canvas the shared
 * image its texture is copied into and its pixels last read
 */
struct Copy {
  scoped_refptr<gpu::ClientSharedImage> img;
  gpu::SyncToken token;  // what the last access of img gave
  const gpu::webgpu::WebGPUInterface* wgi = nullptr;
};

std::mutex g_mu;
std::map<const gpu::webgpu::WebGPUInterface*, Provider*> g_ours;
std::map<const void*, Copy> g_copies;
std::map<const void*, sk_sp<SkImage>> g_last;

class Provider final : public blink::WebGraphicsContext3DProvider, public viz::ContextLostObserver {
 public:
  Provider(scoped_refptr<viz::ContextProviderCommandBuffer> ctx,
           scoped_refptr<base::SingleThreadTaskRunner> replies)
      : ctx_(std::move(ctx)), replies_(std::move(replies)) {}

  ~Provider() override {
    if (bound_) {
      std::lock_guard<std::mutex> l(g_mu);
      const gpu::webgpu::WebGPUInterface* wgi = ctx_->WebGPUInterface();
      g_ours.erase(wgi);
      for (auto it = g_copies.begin(); it != g_copies.end();) {
        if (it->second.wgi == wgi) {
          it->second.img->UpdateDestructionSyncToken(it->second.token);
          it = g_copies.erase(it);
        } else {
          ++it;
        }
      }
    }
    if (observing_)
      ctx_->RemoveObserver(this);
  }

  /* On the thread that will use the context: its loss is told there, and its answers too unless they go to the IO thread */
  bool BindToCurrentSequence() override {
    if (tried_)
      return bound_;
    tried_ = true;
    ctx_->AddObserver(this);
    observing_ = true;
    if (replies_)
      ctx_->SetReplyTaskRunner(replies_);
    gpu::ContextResult r = ctx_->BindToCurrentSequence();
    if (r != gpu::ContextResult::kSuccess) {
      static std::atomic<bool> told{false};
      if (!told.exchange(true))
        printf("browser: webgpu: a context in the GPU process could not be made (%d)\n", static_cast<int>(r));
      return false;
    }
    bound_ = true;
    std::lock_guard<std::mutex> l(g_mu);
    g_ours[ctx_->WebGPUInterface()] = this;
    return true;
  }

  gpu::InterfaceBase* InterfaceBase() override { return WebGPUInterface(); }
  gpu::gles2::GLES2Interface* ContextGL() override { return nullptr; }
  gpu::raster::RasterInterface* RasterInterface() override { return nullptr; }
  gpu::webgpu::WebGPUInterface* WebGPUInterface() override {
    return bound_ ? ctx_->WebGPUInterface() : nullptr;
  }
  gpu::ContextSupport* ContextSupport() override {
    return bound_ ? ctx_->ContextSupport() : nullptr;
  }
  bool IsContextLost() override { return lost_; }
  const gpu::Capabilities& GetCapabilities() const override {
    return bound_ ? ctx_->ContextCapabilities() : caps_;
  }
  const gpu::GpuFeatureInfo& GetGpuFeatureInfo() const override {
    return ctx_->GetGpuFeatureInfo();
  }
  const blink::WebglPreferences& GetWebglPreferences() const override { return prefs_; }
  void SetLostContextCallback(base::RepeatingClosure cb) override { on_lost_ = std::move(cb); }
  void SetErrorMessageCallback(base::RepeatingCallback<void(const char*, int32_t)> cb) override {
    if (bound_)
      ctx_->ContextSupport()->SetErrorMessageCallback(std::move(cb));
  }
  cc::ImageDecodeCache* ImageDecodeCache(SkColorType) override { return nullptr; }
  gpu::SharedImageInterface* SharedImageInterface() override {
    return ctx_->SharedImageInterface();
  }
  viz::RasterContextProvider* RasterContextProvider() const override { return nullptr; }

  /* viz::ContextLostObserver, on the bound thread: the GPU process ended, or the device was lost there */
  void OnContextLost() override {
    lost_ = true;
    static std::atomic<bool> told{false};
    if (!told.exchange(true))
      printf("browser: webgpu: a context in the GPU process is lost: the page is told (device.lost)\n");
    if (on_lost_)
      on_lost_.Run();
  }

  bool lost() const { return lost_; }

 private:
  scoped_refptr<viz::ContextProviderCommandBuffer> ctx_;
  scoped_refptr<base::SingleThreadTaskRunner> replies_;  // null: on the bound thread
  bool tried_ = false;
  bool bound_ = false;
  bool observing_ = false;
  std::atomic<bool> lost_{false};
  base::RepeatingClosure on_lost_;
  gpu::Capabilities caps_;  // what a context not bound has
  blink::WebglPreferences prefs_;
};

/*
 * The pixels of a canvas's texture (g_mu held): copied on its device into
 * the canvas's shared image, which is read back once the copy is done.
 * Null when it cannot be.
 */
sk_sp<SkImage> Read(const void* canvas, gpu::webgpu::WebGPUInterface* wgi, const gpu::ClientSharedImage* si,
                    WGPUDevice device, WGPUTexture texture, SkAlphaType alpha) {
  auto p = g_ours.find(wgi);
  if (p == g_ours.end() || p->second->lost() || si == nullptr || device == nullptr || texture == nullptr)
    return nullptr;
  gfx::Size size = si->size();
  if (size.IsEmpty())
    return nullptr;
  Copy& cp = g_copies[canvas];
  if (cp.img == nullptr || cp.wgi != wgi || cp.img->size() != size || cp.img->format() != si->format()) {
    if (cp.img != nullptr)
      cp.img->UpdateDestructionSyncToken(cp.token);
    gpu::SharedImageInfo info(si->format(), size, si->color_space(),
                              gpu::SharedImageUsageSet(gpu::SHARED_IMAGE_USAGE_WEBGPU_WRITE) |
                                  gpu::SHARED_IMAGE_USAGE_RASTER_READ,
                              "BrWebGPUCanvasCopy");
    cp.img = p->second->SharedImageInterface()->CreateSharedImage(info, gpu::kNullSurfaceHandle);
    cp.wgi = wgi;
    if (cp.img == nullptr) {
      g_copies.erase(canvas);
      static std::atomic<bool> told{false};
      if (!told.exchange(true))
        printf("browser: webgpu: no shared image to copy a canvas into in the GPU process\n");
      return nullptr;
    }
    cp.token = cp.img->creation_sync_token();
  }

  wgpu::Device dev(device);
  wgpu::TextureDescriptor desc;
  desc.usage = wgpu::TextureUsage::CopyDst | wgpu::TextureUsage::RenderAttachment;
  std::unique_ptr<gpu::WebGPUTextureScopedAccess> access = cp.img->BeginWebGPUTextureAccess(
      wgi, cp.token, dev, desc, /*usage=*/0, gpu::webgpu::WEBGPU_MAILBOX_NONE);
  if (access == nullptr)
    return nullptr;
  /* the canvas's texture is copied from with the usage the canvas gave it inside (CopySrc) */
  wgpu::DawnEncoderInternalUsageDescriptor internal;
  internal.useInternalUsages = true;
  wgpu::CommandEncoderDescriptor ed;
  ed.nextInChain = &internal;
  wgpu::CommandEncoder enc = dev.CreateCommandEncoder(&ed);
  wgpu::TexelCopyTextureInfo src;
  src.texture = wgpu::Texture(texture);
  wgpu::TexelCopyTextureInfo dst;
  dst.texture = access->texture();
  wgpu::Extent3D ext = {static_cast<uint32_t>(size.width()), static_cast<uint32_t>(size.height()), 1};
  enc.CopyTextureToTexture(&src, &dst, &ext);
  wgpu::CommandBuffer cmds = enc.Finish();
  dev.GetQueue().Submit(1, &cmds);
  cp.token = gpu::WebGPUTextureScopedAccess::EndAccess(std::move(access));
  wgi->FlushCommands();

  /* RGBA, as WebGL's uploads of a canvas take it (texImage2D) and the page's paint does too */
  SkImageInfo ii = SkImageInfo::Make(size.width(), size.height(), kRGBA_8888_SkColorType, alpha,
                                     SkColorSpace::MakeSRGB());
  SkBitmap bm;
  if (!bm.tryAllocPixels(ii))
    return nullptr;
  if (!br_gpu_read_image(cp.img->mailbox(), cp.token, ii, bm.getPixels(), bm.rowBytes())) {
    static std::atomic<bool> told{false};
    if (!told.exchange(true))
      printf("browser: webgpu: a canvas's texture was not read back from the GPU process\n");
    return nullptr;
  }
  bm.setImmutable();
  return SkImages::RasterFromBitmap(bm);
}

}  // namespace

/* Whether a WebGPU interface is one of a context here */
bool BrCmdBufWebGPUOwns(const gpu::webgpu::WebGPUInterface* wgi) {
  std::lock_guard<std::mutex> l(g_mu);
  return g_ours.count(wgi) != 0;
}

/* A canvas's pixels for the page's paint (webgpu_tessronos.cc's hook hands them here) */
sk_sp<SkImage> BrCmdBufWebGPUSnapshot(const void* canvas, gpu::webgpu::WebGPUInterface* wgi,
                                      const gpu::ClientSharedImage* si, WGPUDevice device,
                                      WGPUTexture texture, SkAlphaType alpha) {
  std::lock_guard<std::mutex> l(g_mu);
  sk_sp<SkImage> img = Read(canvas, wgi, si, device, texture, alpha);
  if (img != nullptr) {
    g_last[canvas] = img;
    return img;
  }
  auto it = g_last.find(canvas);
  return it == g_last.end() ? nullptr : it->second;
}

void BrCmdBufWebGPUForget(const void* canvas) {
  std::lock_guard<std::mutex> l(g_mu);
  g_last.erase(canvas);
  auto it = g_copies.find(canvas);
  if (it != g_copies.end()) {
    it->second.img->UpdateDestructionSyncToken(it->second.token);
    g_copies.erase(it);
  }
}

/*
 * A WebGPU context in the GPU process, on the renderer's main thread
 * (Blink asks there, for a worker's too); 'wait_on' for one whose user
 * blocks for its answers. Null when the renderer has no channel to it
 * ('later' true while a new one is asked for: the caller makes none
 * elsewhere), and when the GPU process runs no Dawn ('none' true: there
 * is no adapter).
 */
std::unique_ptr<blink::WebGraphicsContext3DProvider> BrCreateCmdBufWebGPUProvider(const blink::WebURL& url,
                                                                                  bool wait_on, bool* later,
                                                                                  bool* none) {
  bool webgpu = false;
  *later = false;
  *none = false;
  scoped_refptr<gpu::GpuChannelHost> host = br_gpu_webgpu_channel(later, &webgpu);
  if (host == nullptr)
    return nullptr;
  if (!webgpu) {
    *none = true;
    static std::atomic<bool> told{false};
    if (!told.exchange(true))
      printf("browser: webgpu: the GPU process runs no WebGPU (its ANGLE makes no OpenGL ES 3.1): no adapter\n");
    return nullptr;
  }
  static std::atomic<bool> procs{false};
  if (!procs.exchange(true)) {
    /* the calls of Blink and of ANGLE's WebGPU backend go to the wire's client */
    dawnProcSetProcs(&dawn::wire::client::GetProcs());
  }
  BrWebGPUCanvasHooks();
  scoped_refptr<base::SingleThreadTaskRunner> replies = wait_on ? host->io_task_runner() : nullptr;
  GURL page = url.IsNull() ? GURL() : GURL(url.GetString().Utf8(), url.GetParsed(), url.IsValid());
  auto ctx = viz::ContextProviderCommandBuffer::CreateForWebGPU(
      std::move(host), page, viz::command_buffer_metrics::ContextType::WEBGPU, /*buffer_mapper=*/nullptr);
  static std::atomic<bool> told[2];
  if (!told[wait_on].exchange(true))
    printf("browser: webgpu: a context in the GPU process%s\n", wait_on ? " (waited on, for WebGL on WebGPU)" : "");
  return std::make_unique<Provider>(std::move(ctx), std::move(replies));
}
