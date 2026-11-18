/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	webgpu_tessronos.cc
 *	ブラウザ: WebGPU on Dawn over the system's OpenGL ES (design 14.6.5;
 *	make GL=1).
 *
 *	There is no GPU process and no command buffer: Blink's WebGPU speaks
 *	Dawn's wire as a client, and this gives it a wire server in the same
 *	process. What the client serializes is handed to the server when the
 *	client flushes; what the server answers is handed back to the client
 *	in a task of its own on the thread the context is bound to, so that
 *	the page's callbacks run on a fresh stack. The server runs Dawn's
 *	native implementation with its OpenGL ES backend on the EGL and
 *	OpenGL ES of TessronOS's Mesa (softpipe, or the V3D), given Mesa's
 *	eglGetProcAddress instead of a library to open. Dawn's OpenGL ES
 *	backend gives adapters of the compatibility level only, so an adapter
 *	asked for is always one of those. The server's work that finishes later (mapping a
 *	buffer, a queue's work done) is polled for while there is some.
 *
 *	A context made to be waited on (WebGL on WebGPU: ANGLE asks for its
 *	device and blocks until it has it) gets the server's answers at once,
 *	inside the flush, and each flush also runs the server's late work to
 *	its end: the thread that would take a task of answers is the one that
 *	waits.
 *
 *	A canvas's textures are shared images: the shared image interface
 *	here keeps each image's pixels in memory, and associating a mailbox
 *	makes a texture of the native device for it and injects it into the
 *	server under the client's reservation. Compositing is on the CPU
 *	(br_blink.cc): when the page is painted, the canvas asks for the
 *	pixels of its current texture (patch gpu_canvas_context.patch), which
 *	are copied into a buffer, mapped and read here.
 */

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <dawn/dawn_proc.h>
#include <dawn/native/DawnNative.h>
#include <dawn/native/OpenGLBackend.h>
#include <dawn/wire/WireClient.h>
#include <dawn/wire/WireServer.h>

#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "gpu/command_buffer/client/client_shared_image.h"
#include "gpu/command_buffer/client/shared_image_interface.h"
#include "gpu/command_buffer/client/webgpu_interface.h"
#include "gpu/command_buffer/common/capabilities.h"
#include "gpu/command_buffer/common/mailbox.h"
#include "gpu/command_buffer/common/shared_image_capabilities.h"
#include "gpu/command_buffer/common/sync_token.h"
#include "gpu/config/gpu_feature_info.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/public/platform/web_graphics_context_3d_provider.h"
#include "third_party/skia/include/core/SkColorSpace.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "third_party/skia/include/core/SkPixmap.h"
#include "ui/gfx/geometry/size.h"

/* Mesa's EGL, linked in with the program (make GL=1) */
extern "C" void (*eglGetProcAddress(const char* name))(void);

namespace {

/*
 * The EGL context current on the thread before Dawn's work, current again
 * after it. Dawn's OpenGL ES backend makes its own context current on the
 * thread it runs on and leaves it so; WebGL on ANGLE runs on the same
 * thread and Mesa's EGL, and expects its context to stay current between
 * its calls (a canvas of WebGPU read back in the middle of texImage2D).
 */
class KeepGL {
 public:
  KeepGL() {
    Load();
    if (!get_ctx_)
      return;
    dpy_ = get_dpy_();
    ctx_ = get_ctx_();
    draw_ = get_surf_(0x3059);  // EGL_DRAW
    read_ = get_surf_(0x305A);  // EGL_READ
  }
  ~KeepGL() {
    if (!get_ctx_ || ctx_ == nullptr || get_ctx_() == ctx_)
      return;
    make_(dpy_, draw_, read_, ctx_);
  }

 private:
  using GetPtr = void* (*)();
  using GetSurf = void* (*)(int);
  using Make = unsigned (*)(void*, void*, void*, void*);
  static void Load() {
    if (loaded_)
      return;
    loaded_ = true;
    get_ctx_ = reinterpret_cast<GetPtr>(eglGetProcAddress("eglGetCurrentContext"));
    get_dpy_ = reinterpret_cast<GetPtr>(eglGetProcAddress("eglGetCurrentDisplay"));
    get_surf_ = reinterpret_cast<GetSurf>(eglGetProcAddress("eglGetCurrentSurface"));
    make_ = reinterpret_cast<Make>(eglGetProcAddress("eglMakeCurrent"));
    if (!get_dpy_ || !get_surf_ || !make_)
      get_ctx_ = nullptr;
  }
  static inline bool loaded_ = false;
  static inline GetPtr get_ctx_ = nullptr;
  static inline GetPtr get_dpy_ = nullptr;
  static inline GetSurf get_surf_ = nullptr;
  static inline Make make_ = nullptr;
  void* dpy_ = nullptr;
  void* ctx_ = nullptr;
  void* draw_ = nullptr;
  void* read_ = nullptr;
};

/*
 * Whether the system's EGL makes a context of OpenGL ES 3.1, the version
 * Dawn's OpenGL ES backend asks for. Tried once, on a thread of its own
 * that has no context current. Mesa refuses a context of a version its
 * driver does not give (the V3D gives 3.0: EGL_BAD_MATCH) only after
 * making the half made context current and tearing it down, and puts
 * back the context that was current at its own level alone. On the
 * thread of a page or a worker, whose context of ANGLE is current there,
 * WebGL then drew no more. Where the version is not there, Dawn is not
 * asked: there is no adapter, and WebGL is left as it was.
 */
void* GlesProbe(void* out) {
  using GetDisplay = void* (*)(void*);
  using Initialize = unsigned (*)(void*, int*, int*);
  using BindAPI = unsigned (*)(unsigned);
  using ChooseConfig = unsigned (*)(void*, const int*, void**, int, int*);
  using CreateContext = void* (*)(void*, void*, void*, const int*);
  using DestroyContext = unsigned (*)(void*, void*);
  using ReleaseThread = unsigned (*)();
  auto get_display = reinterpret_cast<GetDisplay>(eglGetProcAddress("eglGetDisplay"));
  auto initialize = reinterpret_cast<Initialize>(eglGetProcAddress("eglInitialize"));
  auto bind_api = reinterpret_cast<BindAPI>(eglGetProcAddress("eglBindAPI"));
  auto choose = reinterpret_cast<ChooseConfig>(eglGetProcAddress("eglChooseConfig"));
  auto create = reinterpret_cast<CreateContext>(eglGetProcAddress("eglCreateContext"));
  auto destroy = reinterpret_cast<DestroyContext>(eglGetProcAddress("eglDestroyContext"));
  auto release = reinterpret_cast<ReleaseThread>(eglGetProcAddress("eglReleaseThread"));
  bool* has = static_cast<bool*>(out);
  *has = false;
  if (!get_display || !initialize || !bind_api || !choose || !create || !destroy)
    return nullptr;
  void* dpy = get_display(nullptr);  // EGL_DEFAULT_DISPLAY
  int major = 0, minor = 0;
  if (dpy == nullptr || !initialize(dpy, &major, &minor))
    return nullptr;
  const int ca[] = {0x3040, 0x40,  // EGL_RENDERABLE_TYPE: EGL_OPENGL_ES3_BIT
                    0x3033, 0x1,   // EGL_SURFACE_TYPE: EGL_PBUFFER_BIT
                    0x3038};       // EGL_NONE
  void* cfg = nullptr;
  int n = 0;
  if (!bind_api(0x30A0) || !choose(dpy, ca, &cfg, 1, &n) || n < 1)  // EGL_OPENGL_ES_API
    return nullptr;
  const int xa[] = {0x3098, 3,  // EGL_CONTEXT_MAJOR_VERSION
                    0x30FB, 1,  // EGL_CONTEXT_MINOR_VERSION
                    0x3038};
  void* ctx = create(dpy, cfg, nullptr, xa);
  if (ctx != nullptr) {
    *has = true;
    destroy(dpy, ctx);
  }
  if (release)
    release();
  return nullptr;
}

bool SystemHasGles31() {
  static const bool has = [] {
    bool r = false;
    pthread_t t;
    if (pthread_create(&t, nullptr, GlesProbe, &r) != 0)
      return false;
    pthread_join(t, nullptr);
    if (!r)
      printf("browser: webgpu: the system's OpenGL ES makes no context of 3.1\n");
    return r;
  }();
  return has;
}

}  // namespace

namespace {

const DawnProcTable& Native() {
  return dawn::native::GetProcs();
}

std::string Key(const gpu::Mailbox& m) {
  return std::string(reinterpret_cast<const char*>(m.name), sizeof(m.name));
}

WGPUStringView View(const char* s) {
  return WGPUStringView{s, WGPU_STRLEN};
}

/* ---------------------------------------------------------------- the serializers */

/*
 * Commands kept until the side that wrote them flushes; then the whole
 * run is handed over. Space is given only from what is reserved, so a
 * span given out stays where it is until the flush.
 */
class Serializer final : public dawn::wire::CommandSerializer {
 public:
  using Sink = base::RepeatingCallback<void(std::vector<std::byte>)>;

  explicit Serializer(Sink sink) : sink_(std::move(sink)) { buf_.reserve(kChunk); }

  size_t GetMaximumAllocationSize() const override { return kChunk; }

  std::optional<std::span<volatile std::byte>> GetCommandSpace(size_t size) override {
    if (buf_.size() + size > buf_.capacity()) {
      Flush();
      if (size > buf_.capacity())
        buf_.reserve(size);
    }
    size_t at = buf_.size();
    buf_.resize(at + size);
    return std::span<volatile std::byte>(buf_.data() + at, size);
  }

  bool Flush() override {
    if (buf_.empty())
      return true;
    std::vector<std::byte> run;
    run.reserve(kChunk);
    std::swap(run, buf_);
    sink_.Run(std::move(run));
    return true;
  }

 private:
  static constexpr size_t kChunk = 1 << 20;
  Sink sink_;
  std::vector<std::byte> buf_;
};

/* ---------------------------------------------------------------- shared images */

/* A shared image's pixels, as the canvas's swap buffers make them */
struct Image {
  int w = 0;
  int h = 0;
  bool bgra = true;
  bool read = false;  // the pixels have been read from a texture
  std::vector<uint8_t> px;
};

using Images = std::map<std::string, Image>;

class SharedImages final : public gpu::SharedImageInterface {
 public:
  explicit SharedImages(Images* images) : images_(images) {}

  scoped_refptr<gpu::ClientSharedImage> Mint(const gpu::SharedImageInfo& si) {
    scoped_refptr<gpu::ClientSharedImage> img = gpu::ClientSharedImage::CreateForTesting(
        static_cast<const gpu::SharedImageMetadata&>(si), GL_TEXTURE_2D);
    Image& m = (*images_)[Key(img->mailbox())];
    m.w = si.size.width();
    m.h = si.size.height();
    m.bgra = si.format != viz::SinglePlaneFormat::kRGBA_8888;
    m.px.assign(static_cast<size_t>(m.w) * m.h * 4, 0);
    return img;
  }

  scoped_refptr<gpu::ClientSharedImage> CreateSharedImage(
      const gpu::SharedImageInfo& si, gpu::SurfaceHandle,
      std::optional<gpu::SharedImagePoolId>) override {
    return Mint(si);
  }
  scoped_refptr<gpu::ClientSharedImage> CreateSharedImage(const gpu::SharedImageInfo& si,
                                                          base::span<const uint8_t>) override {
    return Mint(si);
  }
  scoped_refptr<gpu::ClientSharedImage> CreateSharedImage(const gpu::SharedImageInfo& si,
                                                          gpu::SurfaceHandle, gfx::BufferUsage,
                                                          gfx::GpuMemoryBufferHandle) override {
    return Mint(si);
  }
  scoped_refptr<gpu::ClientSharedImage> CreateSharedImage(const gpu::SharedImageInfo& si,
                                                          gfx::GpuMemoryBufferHandle) override {
    return Mint(si);
  }
  scoped_refptr<gpu::ClientSharedImage> CreateSharedImageForMLTensor(
      std::string, viz::SharedImageFormat, const gfx::Size&, gpu::SharedImageUsageSet) override {
    return nullptr;
  }
  scoped_refptr<gpu::ClientSharedImage> CreateSharedImageForSoftwareCompositor(
      const gpu::SharedImageInfo& si) override {
    return Mint(si);
  }
  void UpdateSharedImage(const gpu::SyncToken&, const gpu::Mailbox&) override {}
  void UpdateSharedImage(const gpu::SyncToken&, std::unique_ptr<gfx::GpuFence>,
                         const gpu::Mailbox&) override {}
  void DestroySharedImage(const gpu::SyncToken&, const gpu::Mailbox& mailbox) override {
    images_->erase(Key(mailbox));
  }
  void DestroySharedImage(const gpu::SyncToken&,
                          scoped_refptr<gpu::ClientSharedImage> image) override {
    if (image)
      images_->erase(Key(image->mailbox()));
  }
  scoped_refptr<gpu::ClientSharedImage> ImportSharedImage(gpu::ExportedSharedImage) override {
    return nullptr;
  }
  gpu::SyncToken GenUnverifiedSyncToken() override { return gpu::SyncToken(); }
  gpu::SyncToken GenVerifiedSyncToken() override { return gpu::SyncToken(); }
  void VerifySyncToken(gpu::SyncToken&) override {}
  // The server runs what it is given before the client goes on: any
  // token is verified and there is nothing to wait for
  bool CanVerifySyncToken(const gpu::SyncToken&) override { return true; }
  void VerifyFlush() override {}
  void WaitSyncToken(const gpu::SyncToken&) override {}
  const gpu::SharedImageCapabilities& GetCapabilities() override { return caps_; }
  // The swap buffers' pool keeps its images itself; there is no GPU
  // channel to lose
  void CreateSharedImagePool(
      const gpu::SharedImagePoolId&,
      mojo::PendingRemote<gpu::mojom::SharedImagePoolClientInterface>) override {}
  void DestroySharedImagePool(const gpu::SharedImagePoolId&) override {}
  bool IsLost() const override { return false; }
  bool AddGpuChannelLostObserver(gpu::GpuChannelLostObserver*) override { return true; }
  void RemoveGpuChannelLostObserver(gpu::GpuChannelLostObserver*) override {}

 private:
  Images* images_;
  gpu::SharedImageCapabilities caps_;
};

/* ---------------------------------------------------------------- the wire */

class WebGPU;

/* The client's end, which Blink keeps while it uses the device */
class Channel final : public gpu::webgpu::APIChannel {
 public:
  explicit Channel(dawn::wire::CommandSerializer* to_server)
      : client_(dawn::wire::WireClientDescriptor{to_server, nullptr}) {
    std::vector<WGPUInstanceFeatureName> features = {WGPUInstanceFeatureName_TimedWaitAny};
    WGPUInstanceDescriptor desc = WGPU_INSTANCE_DESCRIPTOR_INIT;
    desc.requiredFeatureCount = features.size();
    desc.requiredFeatures = features.data();
    reserved_ = client_.ReserveInstance(&desc);
  }

  WGPUInstance GetWGPUInstance() const override { return reserved_.instance; }
  void Disconnect() override {
    if (disconnected_)
      return;
    disconnected_ = true;
    client_.Disconnect();
  }

  bool disconnected() const { return disconnected_; }
  dawn::wire::WireClient& client() { return client_; }
  const dawn::wire::Handle& handle() const { return reserved_.handle; }

 private:
  ~Channel() override {
    if (reserved_.instance)
      dawn::wire::client::GetProcs().instanceRelease(reserved_.instance);
  }

  dawn::wire::WireClient client_;
  dawn::wire::ReservedInstance reserved_{};
  bool disconnected_ = false;
};

/* The interface whose request the server is answering (the native procs' override) */
thread_local WebGPU* g_serving = nullptr;

/*
 * Blink's WebGPU interface: the wire's client and its server, the native
 * instance, and the textures of the canvas's mailboxes
 */
class WebGPU final : public gpu::webgpu::WebGPUInterface {
 public:
  WebGPU()
      : to_server_(base::BindRepeating(&WebGPU::ToServer, base::Unretained(this))),
        to_client_(base::BindRepeating(&WebGPU::ToClient, base::Unretained(this))) {}

  ~WebGPU() override {
    KeepGL keep;
    for (auto& kv : textures_)
      Native().textureRelease(kv.second.texture);
    textures_.clear();
    if (channel_)
      channel_->Disconnect();
    server_.reset();
  }

  /* The native instance, the server and the client; false if Dawn's procs are not there */
  bool Init() {
    KeepGL keep;
    static bool procs_set = false;
    if (!procs_set) {
      // Blink's calls go to the wire's client
      dawnProcSetProcs(&dawn::wire::client::GetProcs());
      procs_set = true;
    }
    std::vector<WGPUInstanceFeatureName> features = {WGPUInstanceFeatureName_TimedWaitAny};
    WGPUInstanceDescriptor desc = WGPU_INSTANCE_DESCRIPTOR_INIT;
    desc.requiredFeatureCount = features.size();
    desc.requiredFeatures = features.data();
    native_ = std::make_unique<dawn::native::Instance>(&desc);

    procs_ = Native();
    procs_.createInstance = [](const WGPUInstanceDescriptor*) -> WGPUInstance { return nullptr; };
    procs_.instanceRequestAdapter = [](WGPUInstance instance, const WGPURequestAdapterOptions* o,
                                       WGPURequestAdapterCallbackInfo cb) -> WGPUFuture {
      return g_serving->RequestAdapter(o, cb);
    };
    procs_.adapterRequestDevice = [](WGPUAdapter adapter, const WGPUDeviceDescriptor* d,
                                     WGPURequestDeviceCallbackInfo cb) -> WGPUFuture {
      return RequestDevice(adapter, d, cb);
    };
    server_ = std::make_unique<dawn::wire::WireServer>(
        dawn::wire::WireServerDescriptor{&procs_, &to_client_, nullptr});
    channel_ = base::MakeRefCounted<Channel>(&to_server_);
    server_->InjectInstance(native_->Get(), channel_->handle());
    return true;
  }

  /* The thread the context is used on: the answers are handed back there */
  void Bind() { runner_ = base::SingleThreadTaskRunner::GetCurrentDefault(); }

  /* The answers at once, in the flush (a context its user blocks on) */
  void SetAnswerAtOnce() { at_once_ = true; }

  Images* images() { return &images_; }

  // gpu::InterfaceBase
  void GenSyncTokenCHROMIUM(GLbyte* sync_token) override { Token(sync_token); }
  void GenUnverifiedSyncTokenCHROMIUM(GLbyte* sync_token) override { Token(sync_token); }
  void VerifySyncTokensCHROMIUM(GLbyte**, GLsizei) override {}
  void WaitSyncTokenCHROMIUM(const GLbyte*) override {}
  void ShallowFlushCHROMIUM() override { FlushCommands(); }

  // gpu::webgpu::WebGPUInterface
  void FlushCommands() override {
    to_server_.Flush();
    if (at_once_)
      Pump();
  }

  /* The server's late work run to its end, its answers given meanwhile */
  void Pump() {
    KeepGL keep;
    for (int i = 0; i < 1000; i++) {
      WebGPU* was = g_serving;
      g_serving = this;
      bool more = dawn::native::InstanceProcessEvents(native_->Get());
      g_serving = was;
      to_client_.Flush();
      if (!more)
        return;
      base::PlatformThread::Sleep(base::Milliseconds(1));
    }
  }
  bool EnsureAwaitingFlush() override {
    if (awaiting_flush_)
      return false;
    awaiting_flush_ = true;
    return true;
  }
  void FlushAwaitingCommands() override {
    awaiting_flush_ = false;
    to_server_.Flush();
  }
  scoped_refptr<gpu::webgpu::APIChannel> GetAPIChannel() const override { return channel_; }

  gpu::webgpu::ReservedBuffer ReserveBuffer(WGPUDevice device,
                                            const WGPUBufferDescriptor* desc) override {
    // what freed an id must reach the server before the id is given again
    to_server_.Flush();
    WGPUBufferDescriptor none = {};
    auto r = channel_->client().ReserveBuffer(device, desc ? desc : &none);
    return {r.buffer, r.handle.id, r.handle.generation, r.deviceHandle.id,
            r.deviceHandle.generation};
  }
  gpu::webgpu::ReservedTexture ReserveTexture(WGPUDevice device,
                                              const WGPUTextureDescriptor* desc) override {
    to_server_.Flush();
    WGPUTextureDescriptor none = {};
    auto r = channel_->client().ReserveTexture(device, desc ? desc : &none);
    return {r.texture, r.handle.id, r.handle.generation, r.deviceHandle.id,
            r.deviceHandle.generation};
  }
  WGPUDevice DeprecatedEnsureDefaultDeviceSync() override { return nullptr; }

  void AssociateMailbox(GLuint device_id, GLuint device_generation, GLuint id, GLuint generation,
                        uint64_t usage, uint64_t internal_usage,
                        const WGPUTextureFormat* view_formats, GLuint view_format_count,
                        gpu::webgpu::MailboxFlags, const gpu::Mailbox& mailbox) override {
    KeepGL keep;
    to_server_.Flush();
    auto img = images_.find(Key(mailbox));
    WGPUDevice device = server_->GetDevice(device_id, device_generation);
    if (img == images_.end() || device == nullptr) {
      printf("browser: webgpu: no %s to associate a mailbox with\n",
             device == nullptr ? "device" : "shared image");
      return;
    }
    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    desc.size = {static_cast<uint32_t>(img->second.w), static_cast<uint32_t>(img->second.h), 1};
    desc.format = img->second.bgra ? WGPUTextureFormat_BGRA8Unorm : WGPUTextureFormat_RGBA8Unorm;
    // read back for the page's paint, and written from the image when kept
    desc.usage = usage | internal_usage | WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst;
    desc.viewFormatCount = view_format_count;
    desc.viewFormats = view_formats;
    WGPUTexture texture = Native().deviceCreateTexture(device, &desc);
    if (!server_->InjectTexture(texture, {id, generation}, {device_id, device_generation})) {
      printf("browser: webgpu: the texture of a mailbox was not taken\n");
      Native().textureRelease(texture);
      return;
    }
    Native().deviceAddRef(device);
    textures_[{id, generation}] = {texture, device, Key(mailbox)};
  }
  void AssociateMailboxForBuffer(GLuint, GLuint, GLuint, GLuint, uint64_t,
                                 const gpu::Mailbox&) override {
    printf("browser: webgpu: buffers of shared images are not kept\n");
  }
  void DissociateMailbox(GLuint id, GLuint generation) override { Dissociate(id, generation); }
  void DissociateMailboxForBuffer(GLuint, GLuint) override {}
  void DissociateMailboxForPresent(GLuint, GLuint, GLuint id, GLuint generation) override {
    Dissociate(id, generation);
  }
  void SetWebGPUExecutionContextToken(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}

  /*
   * The pixels of the image of a mailbox: those of the texture associated
   * with it now, read back, or those last read. Null if there are none.
   */
  sk_sp<SkImage> Snapshot(const gpu::Mailbox& mailbox, SkAlphaType alpha) {
    to_server_.Flush();
    std::string key = Key(mailbox);
    auto img = images_.find(key);
    if (img == images_.end())
      return nullptr;
    for (auto& kv : textures_) {
      if (kv.second.mailbox == key) {
        ReadBack(kv.second, &img->second);
        break;
      }
    }
    if (!img->second.read)
      return nullptr;
    const Image& m = img->second;
    // RGBA, as WebGL's uploads of a canvas take it (texImage2D) and the
    // page's paint does too
    std::vector<uint8_t> px(m.px);
    if (m.bgra) {
      for (size_t i = 0; i + 3 < px.size(); i += 4)
        std::swap(px[i], px[i + 2]);
    }
    SkImageInfo info = SkImageInfo::Make(m.w, m.h, kRGBA_8888_SkColorType, alpha, SkColorSpace::MakeSRGB());
    return SkImages::RasterFromPixmapCopy(SkPixmap(info, px.data(), m.w * 4));
  }

 private:
  struct Assoc {
    WGPUTexture texture = nullptr;
    WGPUDevice device = nullptr;
    std::string mailbox;
  };

  static void Token(GLbyte* sync_token) {
    gpu::SyncToken t;
    memcpy(sync_token, &t, sizeof(t));
  }

  /* The client's run of commands, given to the server now */
  void ToServer(std::vector<std::byte> run) {
    KeepGL keep;
    if (channel_ && channel_->disconnected())
      return;
    WebGPU* was = g_serving;
    g_serving = this;
    if (!server_->HandleCommands(run))
      printf("browser: webgpu: the server did not take the client's commands\n");
    g_serving = was;
    to_client_.Flush();
    Poll();
  }

  /* The server's answers, for the client in a task of their own */
  void ToClient(std::vector<std::byte> run) {
    if (at_once_) {
      Answer(std::move(run));
      return;
    }
    if (!runner_)
      Bind();
    runner_->PostTask(FROM_HERE, base::BindOnce(&WebGPU::Answer, weak_.GetWeakPtr(),
                                                std::move(run)));
  }

  void Answer(std::vector<std::byte> run) {
    if (channel_->disconnected())
      return;
    if (!channel_->client().HandleCommands(run))
      printf("browser: webgpu: the client did not take the server's answers\n");
    dawn::wire::client::GetProcs().instanceProcessEvents(channel_->GetWGPUInstance());
  }

  /* The server's work that finishes later, looked at while there is some */
  void Poll() {
    KeepGL keep;
    WebGPU* was = g_serving;
    g_serving = this;
    bool more = dawn::native::InstanceProcessEvents(native_->Get());
    g_serving = was;
    to_client_.Flush();
    if (!more || poll_pending_ || at_once_)
      return;
    if (!runner_)
      Bind();
    poll_pending_ = true;
    runner_->PostDelayedTask(FROM_HERE,
                             base::BindOnce(
                                 [](base::WeakPtr<WebGPU> self) {
                                   if (!self)
                                     return;
                                   self->poll_pending_ = false;
                                   self->Poll();
                                 },
                                 weak_.GetWeakPtr()),
                             base::Milliseconds(2));
  }

  void Dissociate(GLuint id, GLuint generation) {
    KeepGL keep;
    to_server_.Flush();
    auto it = textures_.find({id, generation});
    if (it == textures_.end())
      return;
    // what was drawn stays the image's: the next paint without a texture shows it
    auto img = images_.find(it->second.mailbox);
    if (img != images_.end())
      ReadBack(it->second, &img->second);
    Native().textureRelease(it->second.texture);
    Native().deviceRelease(it->second.device);
    textures_.erase(it);
  }

  /*
   * A texture's pixels into its image: copied into a buffer the CPU can
   * map, and waited for (the server's own events, here on this thread)
   */
  void ReadBack(const Assoc& a, Image* img) {
    KeepGL keep;
    const DawnProcTable& n = Native();
    uint32_t w = static_cast<uint32_t>(img->w), h = static_cast<uint32_t>(img->h);
    if (w == 0 || h == 0)
      return;
    uint32_t row = (w * 4 + 255) & ~255u;
    WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
    bd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    bd.size = static_cast<uint64_t>(row) * h;
    WGPUBuffer buf = n.deviceCreateBuffer(a.device, &bd);
    WGPUCommandEncoder enc = n.deviceCreateCommandEncoder(a.device, nullptr);
    WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    src.texture = a.texture;
    WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    dst.buffer = buf;
    dst.layout.bytesPerRow = row;
    dst.layout.rowsPerImage = h;
    WGPUExtent3D size = {w, h, 1};
    n.commandEncoderCopyTextureToBuffer(enc, &src, &dst, &size);
    WGPUCommandBuffer cmds = n.commandEncoderFinish(enc, nullptr);
    WGPUQueue queue = n.deviceGetQueue(a.device);
    n.queueSubmit(queue, 1, &cmds);

    struct Mapped {
      bool done = false;
      WGPUMapAsyncStatus status = WGPUMapAsyncStatus_Error;
    } mapped;
    WGPUBufferMapCallbackInfo cb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    cb.mode = WGPUCallbackMode_AllowProcessEvents;
    cb.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* u1, void*) {
      auto* m = static_cast<Mapped*>(u1);
      m->status = status;
      m->done = true;
    };
    cb.userdata1 = &mapped;
    n.bufferMapAsync(buf, WGPUMapMode_Read, 0, bd.size, cb);
    base::TimeTicks give_up = base::TimeTicks::Now() + base::Seconds(10);
    while (!mapped.done && base::TimeTicks::Now() < give_up) {
      dawn::native::InstanceProcessEvents(native_->Get());
      if (!mapped.done)
        sched_yield();
    }
    if (mapped.done && mapped.status == WGPUMapAsyncStatus_Success) {
      const uint8_t* p = static_cast<const uint8_t*>(n.bufferGetConstMappedRange(buf, 0, bd.size));
      if (p != nullptr) {
        for (uint32_t y = 0; y < h; ++y)
          memcpy(img->px.data() + static_cast<size_t>(y) * w * 4, p + static_cast<size_t>(y) * row,
                 w * 4);
        img->read = true;
      }
      n.bufferUnmap(buf);
    } else {
      printf("browser: webgpu: a canvas's texture was not read back\n");
    }
    n.commandBufferRelease(cmds);
    n.commandEncoderRelease(enc);
    n.queueRelease(queue);
    n.bufferDestroy(buf);
    n.bufferRelease(buf);
    to_client_.Flush();
  }

  /*
   * An adapter of Dawn's OpenGL ES backend on Mesa's EGL. Its level is
   * compatibility whatever the page asked for: the backend has no other.
   */
  WGPUFuture RequestAdapter(const WGPURequestAdapterOptions* o, WGPURequestAdapterCallbackInfo cb) {
    dawn::native::opengl::RequestAdapterOptionsGetGLProc gl;
    gl.getProc = reinterpret_cast<dawn::native::opengl::EGLGetProcProc>(&eglGetProcAddress);
    gl.display = nullptr;  // EGL_NO_DISPLAY: the default display, Mesa's platform tessronos
    WGPURequestAdapterOptions opts = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
    if (o != nullptr)
      opts.powerPreference = o->powerPreference;
    opts.backendType = WGPUBackendType_OpenGLES;
    opts.featureLevel = WGPUFeatureLevel_Compatibility;
    opts.nextInChain = reinterpret_cast<WGPUChainedStruct*>(static_cast<wgpu::ChainedStruct*>(&gl));
    std::vector<dawn::native::Adapter> found;
    if (SystemHasGles31())
      found = native_->EnumerateAdapters(&opts);
    if (found.empty()) {
      static bool told;
      if (!told) {
        printf("browser: webgpu: no adapter on OpenGL ES\n");
        told = true;
      }
      cb.callback(WGPURequestAdapterStatus_Unavailable, nullptr,
                  View("There is no adapter on OpenGL ES."), cb.userdata1, cb.userdata2);
      return {};
    }
    WGPUAdapter adapter = found[0].Get();
    Native().adapterAddRef(adapter);
    static bool said;
    if (!said) {
      WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
      Native().adapterGetInfo(adapter, &info);
      printf("browser: webgpu: adapter %.*s (%.*s)\n", static_cast<int>(info.device.length),
             info.device.data, static_cast<int>(info.description.length), info.description.data);
      Native().adapterInfoFreeMembers(info);
      said = true;
    }
    cb.callback(WGPURequestAdapterStatus_Success, adapter, View(""), cb.userdata1, cb.userdata2);
    return {};
  }

  /*
   * A device as the page asked for it, with what a browser adds: the
   * internal usages (the canvas's textures are read back with a usage
   * the page did not give), no SPIR-V (only WGSL is checked for safety),
   * timestamps quantized, and no cache of compiled pipelines on disk
   */
  static WGPUFuture RequestDevice(WGPUAdapter adapter, const WGPUDeviceDescriptor* d,
                                  WGPURequestDeviceCallbackInfo cb) {
    WGPUDeviceDescriptor desc = WGPU_DEVICE_DESCRIPTOR_INIT;
    if (d != nullptr)
      desc = *d;
    std::vector<WGPUFeatureName> features(desc.requiredFeatures,
                                          desc.requiredFeatures + desc.requiredFeatureCount);
    features.push_back(WGPUFeatureName_DawnInternalUsages);
    desc.requiredFeatures = features.data();
    desc.requiredFeatureCount = features.size();
    static const char* const kEnabled[] = {"disallow_spirv", "timestamp_quantization",
                                           "disable_blob_cache"};
    WGPUDawnTogglesDescriptor toggles = WGPU_DAWN_TOGGLES_DESCRIPTOR_INIT;
    toggles.enabledToggleCount = std::size(kEnabled);
    toggles.enabledToggles = kEnabled;
    toggles.chain.next = desc.nextInChain;
    desc.nextInChain = &toggles.chain;
    return Native().adapterRequestDevice(adapter, &desc, cb);
  }

  Serializer to_server_;
  Serializer to_client_;
  std::unique_ptr<dawn::native::Instance> native_;
  DawnProcTable procs_{};
  std::unique_ptr<dawn::wire::WireServer> server_;
  scoped_refptr<Channel> channel_;
  scoped_refptr<base::SingleThreadTaskRunner> runner_;
  Images images_;
  std::map<std::pair<GLuint, GLuint>, Assoc> textures_;
  bool awaiting_flush_ = false;
  bool poll_pending_ = false;
  bool at_once_ = false;
  base::WeakPtrFactory<WebGPU> weak_{this};
};

class Provider final : public blink::WebGraphicsContext3DProvider {
 public:
  Provider() : sii_(webgpu_.images()) {}

  bool Init() { return webgpu_.Init(); }
  void SetAnswerAtOnce() { webgpu_.SetAnswerAtOnce(); }
  WebGPU* webgpu() { return &webgpu_; }

  gpu::InterfaceBase* InterfaceBase() override { return &webgpu_; }
  gpu::gles2::GLES2Interface* ContextGL() override { return nullptr; }
  gpu::raster::RasterInterface* RasterInterface() override { return nullptr; }
  gpu::ContextSupport* ContextSupport() override { return nullptr; }
  gpu::webgpu::WebGPUInterface* WebGPUInterface() override { return &webgpu_; }
  bool BindToCurrentSequence() override {
    webgpu_.Bind();
    return true;
  }
  const gpu::Capabilities& GetCapabilities() const override { return caps_; }
  const gpu::GpuFeatureInfo& GetGpuFeatureInfo() const override { return feat_; }
  const blink::WebglPreferences& GetWebglPreferences() const override { return prefs_; }
  viz::RasterContextProvider* RasterContextProvider() const override { return nullptr; }
  bool IsContextLost() override { return false; }
  void SetLostContextCallback(base::RepeatingClosure) override {}
  void SetErrorMessageCallback(base::RepeatingCallback<void(const char*, int32_t)>) override {}
  cc::ImageDecodeCache* ImageDecodeCache(SkColorType) override { return nullptr; }
  gpu::SharedImageInterface* SharedImageInterface() override { return &sii_; }

 private:
  WebGPU webgpu_;
  SharedImages sii_;
  gpu::Capabilities caps_;
  gpu::GpuFeatureInfo feat_;
  blink::WebglPreferences prefs_;
};

/* The canvases' last pixels, for a paint between two textures */
std::map<const void*, sk_sp<SkImage>>& Last() {
  static auto* last = new std::map<const void*, sk_sp<SkImage>>();
  return *last;
}

}  // namespace

namespace blink {
/* The canvas's hooks for its pixels (patch gpu_canvas_context.patch) */
extern sk_sp<SkImage> (*g_webgpu_canvas_snapshot)(const void* canvas,
                                                  gpu::webgpu::WebGPUInterface* wgi,
                                                  const gpu::ClientSharedImage* si,
                                                  WGPUDevice device, WGPUTexture texture,
                                                  SkAlphaType alpha);
extern void (*g_webgpu_canvas_forget)(const void* canvas);
}  // namespace blink

/*
 * webgpu_cmdbuf.cc: whether a WebGPU interface is one of a command buffer
 * of the GPU process, and the pixels of such a canvas's texture
 */
bool BrCmdBufWebGPUOwns(const gpu::webgpu::WebGPUInterface* wgi);
sk_sp<SkImage> BrCmdBufWebGPUSnapshot(const void* canvas, gpu::webgpu::WebGPUInterface* wgi,
                                      const gpu::ClientSharedImage* si, WGPUDevice device,
                                      WGPUTexture texture, SkAlphaType alpha);
void BrCmdBufWebGPUForget(const void* canvas);

namespace {

/*
 * A WebGPU canvas's pixels for the page's paint: those of the shared
 * image of its current texture, read back now, or the canvas's last ones.
 * A canvas of a context in the GPU process is read there.
 */
sk_sp<SkImage> CanvasSnapshot(const void* canvas, gpu::webgpu::WebGPUInterface* wgi,
                              const gpu::ClientSharedImage* si, WGPUDevice device,
                              WGPUTexture texture, SkAlphaType alpha) {
  if (wgi != nullptr && BrCmdBufWebGPUOwns(wgi))
    return BrCmdBufWebGPUSnapshot(canvas, wgi, si, device, texture, alpha);
  if (wgi != nullptr && si != nullptr) {
    if (sk_sp<SkImage> img = static_cast<WebGPU*>(wgi)->Snapshot(si->mailbox(), alpha)) {
      Last()[canvas] = img;
      return img;
    }
  }
  auto it = Last().find(canvas);
  return it == Last().end() ? nullptr : it->second;
}

void CanvasForget(const void* canvas) {
  Last().erase(canvas);
  BrCmdBufWebGPUForget(canvas);
}

}  // namespace

/*
 * The canvas's hooks, for the contexts here and those of the GPU process
 * (webgpu_cmdbuf.cc) alike
 */
void BrWebGPUCanvasHooks() {
  blink::g_webgpu_canvas_snapshot = &CanvasSnapshot;
  blink::g_webgpu_canvas_forget = &CanvasForget;
}

/* A WebGPU context on Dawn (br_blink.cc, CreateWebGPUGraphicsContext3DProviderAsync) */
std::unique_ptr<blink::WebGraphicsContext3DProvider> BrCreateWebGPUProvider() {
  BrWebGPUCanvasHooks();
  auto p = std::make_unique<Provider>();
  if (!p->Init())
    return nullptr;
  return p;
}

/* The same, answering in the flush (br_blink.cc, CreateWebGPUGraphicsContext3DProvider) */
std::unique_ptr<blink::WebGraphicsContext3DProvider> BrCreateWebGPUProviderToWaitOn() {
  BrWebGPUCanvasHooks();
  auto p = std::make_unique<Provider>();
  if (!p->Init())
    return nullptr;
  p->SetAnswerAtOnce();
  return p;
}
