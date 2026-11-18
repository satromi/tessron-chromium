/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	gl_angle.cc
 *	ブラウザ: WebGL 1 and 2 on ANGLE over the system's OpenGL ES (design
 *	14.6; make GL=1).
 *
 *	There is no GPU process and no command buffer: Blink's WebGL sits on
 *	a gpu::gles2::GLES2Interface, and this one gives each call straight
 *	to ANGLE in the same process. ANGLE (libGLESv2_thin_static: its GL_
 *	and EGL_ entry points, not the gl and egl names) runs its OpenGL ES
 *	backend on the EGL and OpenGL ES of TessronOS's Mesa, which draws with
 *	softpipe, or with the V3D when the machine has one. Each provider has
 *	a context of ANGLE made for WebGL (EGL_CONTEXT_WEBGL_COMPATIBILITY_
 *	ANGLE: ANGLE validates the calls and translates the shaders the way
 *	WebGL asks) on a pbuffer it never draws to.
 *
 *	The calls are forwarded by gl_angle_forward.inc (tools/browser/
 *	gen_gl_forward.py); what the command buffer's client adds of its own
 *	is here: the colour buffers of the drawing buffer as shared images,
 *	the extensions asked for by name, flushes and synchronisation tokens
 *	(every call has run when it returns, so there is nothing to wait for),
 *	and the queries of the command buffer's own targets (WebGL 2's fences,
 *	the completion of a link).
 *
 *	What WebGL drew reaches the page as pixels: compositing is on the CPU
 *	(br_blink.cc), so the drawing buffer reads its colour buffer back with
 *	ReadPixels into the canvas's bitmap, which Skia paints. A colour buffer
 *	is made in one place (ColorBuffer); giving it a BO of the GPU instead,
 *	for the window system to compose without reading it back, changes that
 *	place and the drawing buffer's way out, not the forwarding.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../base/counter_tessronos.h"
#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>

#include "libGLESv2/entry_points_egl_autogen.h"
#include "libGLESv2/entry_points_egl_ext_autogen.h"
#include "libGLESv2/entry_points_gles_2_0_autogen.h"
#include "libGLESv2/entry_points_gles_3_0_autogen.h"
#include "libGLESv2/entry_points_gles_3_1_autogen.h"
#include "libGLESv2/entry_points_gles_3_2_autogen.h"
#include "gl_angle_entry.inc"
#include "gpu_info_util/SystemInfo.h"

#include "gpu/command_buffer/client/gles2_interface_stub.h"
#include "gpu/command_buffer/client/shared_image_interface.h"
#include "gpu/command_buffer/client/client_shared_image.h"
#include "gpu/command_buffer/common/shared_image_capabilities.h"
#include "gpu/command_buffer/common/capabilities.h"
#include "gpu/command_buffer/common/mailbox.h"
#include "gpu/command_buffer/common/sync_token.h"
#include "gpu/config/gpu_feature_info.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/public/platform/web_graphics_context_3d_provider.h"
#include "ui/gfx/geometry/size.h"

/* GL_ANGLE_request_extension's string (ANGLE's gl2ext_angle.h wants ANGLE's own gl2.h) */
#ifndef GL_REQUESTABLE_EXTENSIONS_ANGLE
#define GL_REQUESTABLE_EXTENSIONS_ANGLE 0x93A8
#endif

/* The command buffer's own query targets (gl2extchromium.h) */
#ifndef GL_COMMANDS_ISSUED_CHROMIUM
#define GL_COMMANDS_ISSUED_CHROMIUM 0x6004
#endif
#ifndef GL_PROGRAM_COMPLETION_QUERY_CHROMIUM
#define GL_PROGRAM_COMPLETION_QUERY_CHROMIUM 0x6009
#endif
#ifndef GL_COMMANDS_COMPLETED_CHROMIUM
#define GL_COMMANDS_COMPLETED_CHROMIUM 0x84F7
#endif
#ifndef GL_READBACK_SHADOW_COPIES_UPDATED_CHROMIUM
#define GL_READBACK_SHADOW_COPIES_UPDATED_CHROMIUM 0x84F8
#endif

/* GL_KHR_debug (ANGLE's entry points of it; the engine's GLES2 headers stand in front of its gl2ext.h) */
#ifndef GL_DEBUG_OUTPUT_KHR
#define GL_DEBUG_OUTPUT_KHR 0x92E0
#endif
#ifndef GL_DEBUG_OUTPUT_SYNCHRONOUS_KHR
#define GL_DEBUG_OUTPUT_SYNCHRONOUS_KHR 0x8242
#endif
#ifndef GL_DEBUG_SOURCE_API_KHR
#define GL_DEBUG_SOURCE_API_KHR 0x8246
#endif
#ifndef GL_DEBUG_TYPE_ERROR_KHR
#define GL_DEBUG_TYPE_ERROR_KHR 0x824C
#endif
#ifndef GL_DONT_CARE
#define GL_DONT_CARE 0x1100
#endif
extern "C" {
typedef void(GL_APIENTRY* BrDebugProc)(GLenum source, GLenum type, GLuint id, GLenum severity,
                                        GLsizei length, const GLchar* message,
                                        const void* user);
void GL_APIENTRY GL_DebugMessageCallbackKHR(BrDebugProc callback, const void* user);
void GL_APIENTRY GL_DebugMessageControlKHR(GLenum source, GLenum type, GLenum severity,
                                           GLsizei count, const GLuint* ids, GLboolean enabled);
void GL_APIENTRY GL_DrawBuffersEXT(GLsizei n, const GLenum* bufs);
void GL_APIENTRY GL_BlitFramebufferANGLE(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                                         GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                                         GLbitfield mask, GLenum filter);
void GL_APIENTRY GL_RenderbufferStorageMultisampleANGLE(GLenum target, GLsizei samples,
                                                        GLenum internalformat, GLsizei width,
                                                        GLsizei height);
}

namespace {

/* ---------------------------------------------------------------- the display */

/*
 * ANGLE's display on the system's EGL: the OpenGL ES backend
 * (EGL_PLATFORM_ANGLE_TYPE_OPENGLES_ANGLE) on the native EGL as a device
 * (EGL_PLATFORM_ANGLE_DEVICE_TYPE_EGL_ANGLE), made once for the process.
 * ANGLE opens the native EGL by name; the program's dlopen gives it the
 * EGL linked in (port/compat/ts_linux.c). Its eglGetProcAddress is called
 * here first, which also has the linker take Mesa's EGL into the program.
 */
pthread_t g_page_thread = 0;  // the thread the display was made on, the page's

struct AngleDisplay {
  bool tried = false;
  EGLDisplay dpy = EGL_NO_DISPLAY;
  EGLConfig cfg = nullptr;
  std::string why;
};

AngleDisplay& Display() {
  static AngleDisplay d;
  if (d.tried)
    return d;
  d.tried = true;
  g_page_thread = pthread_self();
  if (eglGetProcAddress("eglInitialize") == nullptr) {
    d.why = "the system's EGL gives no eglInitialize";
    return d;
  }
  const EGLAttrib attrs[] = {
      EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_OPENGLES_ANGLE,
      EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_DEVICE_TYPE_EGL_ANGLE,
      EGL_NONE};
  EGLDisplay dpy = EGL_GetPlatformDisplay(EGL_PLATFORM_ANGLE_ANGLE,
                                          reinterpret_cast<void*>(EGL_DEFAULT_DISPLAY), attrs);
  EGLint major = 0, minor = 0;
  if (dpy == EGL_NO_DISPLAY || !EGL_Initialize(dpy, &major, &minor)) {
    char b[64];
    snprintf(b, sizeof(b), "ANGLE's display did not start (EGL error 0x%x)", EGL_GetError());
    d.why = b;
    return d;
  }
  // a config for OpenGL ES 3 and 2 (WebGL 2 and 1), or for 2 alone
  EGLint ca[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                 EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                 EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT_KHR, EGL_NONE};
  EGLint n = 0;
  if (!EGL_ChooseConfig(dpy, ca, &d.cfg, 1, &n) || n < 1) {
    ca[11] = EGL_OPENGL_ES2_BIT;
    if (!EGL_ChooseConfig(dpy, ca, &d.cfg, 1, &n) || n < 1) {
      d.why = "ANGLE has no config with a pbuffer";
      return d;
    }
  }
  d.dpy = dpy;
  printf("browser: webgl: ANGLE %s on %s\n",
         EGL_QueryString(dpy, EGL_VERSION), EGL_QueryString(dpy, EGL_VENDOR));
  return d;
}

/*
 * The extensions of ANGLE that the command buffer's client relies on for
 * itself (the drawing buffer's storage, copies, blits and readback),
 * asked for when a context is made, of those it can give
 */
const char* const kInternalExtensions[] = {
    "GL_ANGLE_framebuffer_blit",
    "GL_ANGLE_framebuffer_multisample",
    "GL_ANGLE_texture_usage",
    "GL_ANGLE_depth_texture",
    "GL_ANGLE_get_tex_level_parameter",
    "GL_ANGLE_instanced_arrays",
    "GL_ANGLE_pack_reverse_row_order",
    "GL_ANGLE_translated_shader_source",
    "GL_CHROMIUM_bind_uniform_location",
    "GL_CHROMIUM_copy_texture",
    "GL_CHROMIUM_sync_query",
    "GL_EXT_blend_minmax",
    "GL_EXT_debug_marker",
    "GL_EXT_discard_framebuffer",
    "GL_EXT_disjoint_timer_query",
    "GL_EXT_map_buffer_range",
    "GL_EXT_occlusion_query_boolean",
    "GL_EXT_sRGB",
    "GL_EXT_texture_format_BGRA8888",
    "GL_EXT_texture_norm16",
    "GL_EXT_texture_rg",
    "GL_EXT_texture_storage",
    "GL_EXT_unpack_subimage",
    "GL_KHR_debug",
    "GL_KHR_parallel_shader_compile",
    "GL_NV_fence",
    "GL_NV_pack_subimage",
    "GL_NV_pixel_buffer_object",
    "GL_OES_depth32",
    "GL_OES_mapbuffer",
    "GL_OES_packed_depth_stencil",
    "GL_OES_rgb8_rgba8",
    "GL_OES_vertex_array_object",
};

/* ---------------------------------------------------------------- the GLES2 interface */

/*
 * A context is used on the thread of the canvas it draws: the page's
 * thread, or a worker's for an OffscreenCanvas there. A worker's context
 * is made on the page's thread (Blink asks for it there and waits), so a
 * context is made current only while it is being made, and then given up
 * (Release), for the thread that uses it to make it its own; each thread
 * knows which context is current on it.
 *
 * A thread knows its current context by the context's serial number, not
 * its address: a context may be let go of on another thread than the one
 * it is current on, which cannot make it not current
 * there. ANGLE keeps such a context until that thread makes another one
 * current, and a new context made later at the same address must not be
 * taken for it.
 */
thread_local uint64_t g_current = 0;   // the serial number of the context current on this thread
std::atomic<uint64_t> g_serials{0};    // the last serial number given
std::atomic<long> g_frames{0};         // frames the drawing buffers took to be shown
thread_local uint64_t g_ticks = 0;     // the time of this thread's calls to ANGLE, in the counter's ticks

// The processor's counter, which a process reads without a call into the system
inline uint64_t TicksNow() {
  uint64_t c;
  BR_COUNTER_READ(c);
  return c;
}

/*
 * ANGLE's OpenGL ES backend runs the contexts of a virtualisation group
 * on one context of the native EGL, and a native context can be current
 * on one thread only. The contexts of the page's thread are in the
 * display's own group; each other thread that uses WebGL (a worker's
 * OffscreenCanvas) has a group, so a native context, of its own. In one
 * group, a worker's context and the page's would take the native context
 * from each other: the native eglMakeCurrent fails and the calls run on
 * another context's state.
 */
thread_local int g_group = -1;         // this thread's group (0: the display's own)
std::atomic<int> g_groups{0};          // the last group given to a thread

int ThreadGroup() {
  if (g_group < 0)
    g_group = pthread_equal(pthread_self(), g_page_thread) ? 0 : ++g_groups;
  return g_group;
}

/*
 * What each context did, summed for a line on the console every
 * kSummarySeconds while it is used (a page's or a worker's): its calls,
 * draws and clears, the frames the drawing buffer took, the reads of
 * pixels into memory and into a pixel pack buffer, the reads of a
 * buffer's data (and the first bytes read), the fences of the command
 * buffer's own queries and their answers, the errors given to the page,
 * and the colour buffer's size. The board's console then tells where a
 * page's WebGL stops, with no tool attached.
 */
enum CallKind { kCallOther, kCallDraw, kCallClear };
constexpr int kSummarySeconds = 5;

class BrAngleGLES2;
std::mutex& Registry() {
  static std::mutex m;
  return m;
}
std::set<BrAngleGLES2*>& Contexts() {
  static std::set<BrAngleGLES2*> s;
  return s;
}
std::atomic<uint64_t> g_next_summary{0};  // in the counter's ticks

inline uint64_t TicksPerSecond() {
  uint64_t f;
  BR_COUNTER_FREQ(f);
  return f;
}

void SummaryIfDue();

class BrAngleGLES2 final : public gpu::gles2::GLES2InterfaceStub {
 public:
  // mailbox name bytes -> the size its shared image was made with
  std::map<std::string, std::pair<int, int>> mailboxes_;

  BrAngleGLES2() : serial_(++g_serials) {
    std::lock_guard<std::mutex> l(Registry());
    Contexts().insert(this);
  }
  ~BrAngleGLES2() override {
    {
      std::lock_guard<std::mutex> l(Registry());
      Contexts().erase(this);
    }
    AngleDisplay& d = Display();
    if (g_current == serial_) {
      EGL_MakeCurrent(d.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      g_current = 0;
    } else if (current_on_ != 0 && current_on_ != pthread_self()) {
      static std::atomic<bool> told{false};
      if (!told.exchange(true))
        printf("browser: webgl: a context let go of on another thread than the one it is current on\n");
    }
    if (ctx_ != EGL_NO_CONTEXT)
      EGL_DestroyContext(d.dpy, ctx_);
    if (surf_ != EGL_NO_SURFACE)
      EGL_DestroySurface(d.dpy, surf_);
  }

  // A context for WebGL 2 (OpenGL ES 3.0) or WebGL 1 (OpenGL ES 2.0)
  bool Init(bool es3, std::string* why) {
    AngleDisplay& d = Display();
    if (d.dpy == EGL_NO_DISPLAY) {
      *why = d.why;
      return false;
    }
    es3_ = es3;
    const EGLint sa[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    surf_ = EGL_CreatePbufferSurface(d.dpy, d.cfg, sa);
    group_ = ThreadGroup();
    ctx_ = Make(group_);
    if (surf_ == EGL_NO_SURFACE || ctx_ == EGL_NO_CONTEXT) {
      char b[80];
      snprintf(b, sizeof(b), "ANGLE made no OpenGL ES %d context (EGL error 0x%x)",
               es3 ? 3 : 2, EGL_GetError());
      *why = b;
      return false;
    }
    Cur();
    Prepare();
    GL_GetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size_);
    return true;
  }

  // The context of ANGLE, in the virtualisation group 'group' (0: the display's own)
  EGLContext Make(int group) {
    EGLint xa[] = {
        EGL_CONTEXT_MAJOR_VERSION, es3_ ? 3 : 2,
        EGL_CONTEXT_MINOR_VERSION, 0,
        EGL_CONTEXT_WEBGL_COMPATIBILITY_ANGLE, EGL_TRUE,
        EGL_CONTEXT_OPENGL_BACKWARDS_COMPATIBLE_ANGLE, EGL_FALSE,
        EGL_CONTEXT_BIND_GENERATES_RESOURCE_CHROMIUM, EGL_FALSE,
        EGL_CONTEXT_CLIENT_ARRAYS_ENABLED_ANGLE, EGL_FALSE,
        EGL_ROBUST_RESOURCE_INITIALIZATION_ANGLE, EGL_TRUE,
        EGL_NONE, EGL_NONE,
        EGL_NONE};
    if (group > 0) {
      xa[14] = EGL_CONTEXT_VIRTUALIZATION_GROUP_ANGLE;
      xa[15] = group;
    }
    return EGL_CreateContext(Display().dpy, Display().cfg, EGL_NO_CONTEXT, xa);
  }

  // The extensions of ANGLE's own that the command buffer's client has on, asked for (current)
  void Prepare() {
    std::string req = Str(GL_REQUESTABLE_EXTENSIONS_ANGLE);
    std::set<std::string> can;
    for (size_t i = 0; i < req.size();) {
      size_t j = req.find(' ', i);
      if (j == std::string::npos)
        j = req.size();
      if (j > i)
        can.insert(req.substr(i, j - i));
      i = j + 1;
    }
    for (const char* e : kInternalExtensions) {
      if (can.count(e))
        GL_RequestExtensionANGLE(e);
    }
    /*
     * ANGLE's own account of each error it sets (which call, and why),
     * as the context's error messages: Blink prints them to the page's
     * console ("WebGL: ..."), so that a page's getError of an error
     * the page did not make itself can be traced to the call
     */
    if (Str(GL_EXTENSIONS).find("GL_KHR_debug") != std::string::npos) {
      GL_Enable(GL_DEBUG_OUTPUT_KHR);
      GL_Enable(GL_DEBUG_OUTPUT_SYNCHRONOUS_KHR);
      GL_DebugMessageControlKHR(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, GL_FALSE);
      GL_DebugMessageControlKHR(GL_DEBUG_SOURCE_API_KHR, GL_DEBUG_TYPE_ERROR_KHR, GL_DONT_CARE, 0,
                                nullptr, GL_TRUE);
      GL_DebugMessageCallbackKHR(&BrAngleGLES2::OnDebugMessage, this);
    }
  }

  void SetErrorMessageCallback(base::RepeatingCallback<void(const char*, int32_t)> cb) {
    on_error_ = std::move(cb);
  }

  GLint max_texture_size() const { return max_texture_size_; }

  // Given up by the thread it was made on, for the thread that uses it
  void Release() {
    if (g_current == serial_) {
      EGL_MakeCurrent(Display().dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      g_current = 0;
      current_on_ = 0;
    }
  }

  // A call to ANGLE: the context made current first, the call's time added to the thread's
  class Timed {
   public:
    explicit Timed(BrAngleGLES2* gl, CallKind kind = kCallOther) : start_(TicksNow()) {
      gl->Cur();
      gl->n_.calls++;
      if (kind == kCallDraw)
        gl->n_.draws++;
      else if (kind == kCallClear)
        gl->n_.clears++;
    }
    ~Timed() {
      uint64_t now = TicksNow();
      g_ticks += now - start_;
      if (now >= g_next_summary.load(std::memory_order_relaxed))
        SummaryIfDue();
    }

   private:
    uint64_t start_;
  };

  /* The counts since the last summary, and the summary's line of them; nothing when unused */
  struct Counts {
    std::atomic<uint32_t> calls{0}, draws{0}, clears{0}, frames{0}, rp_mem{0}, rp_pbo{0},
        gbsd{0}, fences{0}, avail_yes{0}, avail_no{0}, errors{0};
  };
  bool SummaryLine(std::string* out) {
    uint32_t calls = n_.calls.exchange(0);
    if (calls == 0)
      return false;
    char b[512];
    snprintf(b, sizeof(b),
             "browser: webgl: context %llu (WebGL %d, %s) in %d s: %u calls, %u draws, %u clears, "
             "%u frames, readPixels %u to memory [%02x %02x %02x %02x] %u to a buffer, "
             "getBufferSubData %u [%02x %02x %02x %02x %02x %02x %02x %02x], fences %u, "
             "answered done %u not yet %u, errors %u, colour buffer %dx%d\n",
             static_cast<unsigned long long>(serial_), es3_ ? 2 : 1,
             group_ == 0 ? "page" : "worker", kSummarySeconds, calls, n_.draws.exchange(0),
             n_.clears.exchange(0), n_.frames.exchange(0), n_.rp_mem.exchange(0), rp_first_[0],
             rp_first_[1], rp_first_[2], rp_first_[3], n_.rp_pbo.exchange(0), n_.gbsd.exchange(0),
             gbsd_first_[0], gbsd_first_[1], gbsd_first_[2], gbsd_first_[3], gbsd_first_[4],
             gbsd_first_[5], gbsd_first_[6], gbsd_first_[7], n_.fences.exchange(0),
             n_.avail_yes.exchange(0), n_.avail_no.exchange(0), n_.errors.exchange(0), last_w_,
             last_h_);
    *out += b;
    return true;
  }

  // ---- the forwarded calls ----
#include "gl_angle_forward.inc"

  // ---- what the command buffer's client answers itself ----
  GLenum GetError() override {
    Cur();
    GLenum e = GL_GetError();
    if (e != GL_NO_ERROR)
      n_.errors++;
    return e;
  }
  /*
   * The extensions: ANGLE's, and in an OpenGL ES 3 context
   * GL_OES_packed_depth_stencil, whose depth and stencil format is in the
   * core there and which WebGL asks for by name. The versions are those
   * of a browser's GPU command buffer, whatever the driver's
   * (getParameter(VERSION) is "WebGL 1.0 (OpenGL ES 2.0 Chromium)")
   */
  const GLubyte* GetString(GLenum name) override {
    if (name == GL_VERSION) {
      return reinterpret_cast<const GLubyte*>(es3_ ? "OpenGL ES 3.0 Chromium"
                                                   : "OpenGL ES 2.0 Chromium");
    }
    if (name == GL_SHADING_LANGUAGE_VERSION) {
      return reinterpret_cast<const GLubyte*>(es3_ ? "OpenGL ES GLSL ES 3.0 Chromium"
                                                   : "OpenGL ES GLSL ES 1.0 Chromium");
    }
    Cur();
    const GLubyte* s = GL_GetString(name);
    if (name != GL_EXTENSIONS || !es3_ || s == nullptr)
      return s;
    ext_ = reinterpret_cast<const char*>(s);
    if (ext_.find("GL_OES_packed_depth_stencil") == std::string::npos)
      ext_ += " GL_OES_packed_depth_stencil";
    return reinterpret_cast<const GLubyte*>(ext_.c_str());
  }
  void RequestExtensionCHROMIUM(const char* name) override {
    Cur();
    GL_RequestExtensionANGLE(name);
  }
  const GLchar* GetRequestableExtensionsCHROMIUM() override {
    Cur();
    return reinterpret_cast<const GLchar*>(GL_GetString(GL_REQUESTABLE_EXTENSIONS_ANGLE));
  }
  GLenum GetGraphicsResetStatusKHR() override { return GL_NO_ERROR; }
  void Flush() override {
    Timed t(this);
    GL_Flush();
  }
  void ShallowFlushCHROMIUM() override {
    Timed t(this);
    GL_Flush();
  }
  void OrderingBarrierCHROMIUM() override {}
  void Finish() override {
    Timed t(this);
    GL_Finish();
  }
  void ShallowFinishCHROMIUM() override {
    Timed t(this);
    GL_Finish();
  }
  GLuint GetMaxValueInBufferCHROMIUM(GLuint, GLsizei, GLenum, GLuint) override { return 0; }

  /*
   * The command buffer's names of functions that are in the core of
   * OpenGL ES 3: there the core function (WebGL 2's drawBuffers,
   * blitFramebuffer, renderbufferStorageMultisample), in an OpenGL ES 2
   * context the extension's (WEBGL_draw_buffers, the drawing buffer's
   * own blits and multisampling). ANGLE answers the extension's entry
   * point in an OpenGL ES 3 context with GL_INVALID_OPERATION when the
   * extension is not on (EXT_draw_buffers never is there), and checks a
   * blit by ANGLE_framebuffer_blit's narrower rules
   */
  void DrawBuffersEXT(GLsizei count, const GLenum* bufs) override {
    Timed t(this);
    if (es3_)
      GL_DrawBuffers(count, bufs);
    else
      GL_DrawBuffersEXT(count, bufs);
  }
  void BlitFramebufferCHROMIUM(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0,
                               GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask,
                               GLenum filter) override {
    Timed t(this);
    if (es3_)
      GL_BlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
    else
      GL_BlitFramebufferANGLE(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
  }
  void RenderbufferStorageMultisampleCHROMIUM(GLenum target, GLsizei samples,
                                              GLenum internalformat, GLsizei width,
                                              GLsizei height) override {
    Timed t(this);
    if (es3_)
      GL_RenderbufferStorageMultisample(target, samples, internalformat, width, height);
    else
      GL_RenderbufferStorageMultisampleANGLE(target, samples, internalformat, width, height);
  }

  // WebGL 2's getBufferSubData: the buffer mapped for reading and copied
  void GetBufferSubDataCHROMIUM(GLenum target, GLintptr offset, GLsizeiptr size,
                                void* data) override {
    Timed t(this);
    void* p = GL_MapBufferRange(target, offset, size, GL_MAP_READ_BIT);
    if (p == nullptr)
      return;
    memcpy(data, p, static_cast<size_t>(size));
    GL_UnmapBuffer(target);
    n_.gbsd++;
    memset(gbsd_first_, 0, sizeof(gbsd_first_));
    memcpy(gbsd_first_, data, std::min<size_t>(sizeof(gbsd_first_), static_cast<size_t>(size)));
  }

  void DeleteTextures(GLsizei n, const GLuint* textures) override {
    Cur();
    GL_DeleteTextures(n, textures);
  }

  /*
   * A read of a whole colour buffer from its origin is the drawing buffer
   * taking the frame WebGL drew, to be shown (compositing is on the CPU):
   * counted as a frame of WebGL
   */
  void ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type,
                  void* pixels) override {
    Timed t(this);
    bool frame = false;
    if (x == 0 && y == 0) {
      for (const auto& m : mailboxes_) {
        if (m.second.first == width && m.second.second == height) {
          g_frames++;
          n_.frames++;
          frame = true;
          last_w_ = width;
          last_h_ = height;
          break;
        }
      }
    }
    GLint pbo = 0;
    if (es3_)
      GL_GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pbo);
    GL_ReadPixels(x, y, width, height, format, type, pixels);
    if (pbo != 0) {
      n_.rp_pbo++;
    } else if (!frame && pixels != nullptr && width > 0 && height > 0) {
      n_.rp_mem++;
      memcpy(rp_first_, pixels, sizeof(rp_first_));
    }
  }

  /*
   * ---- the queries of the command buffer's own targets ----
   * WebGL 2's fenceSync is a query of GL_READBACK_SHADOW_COPIES_UPDATED_
   * CHROMIUM, and KHR_parallel_shader_compile's completion status a query
   * of GL_PROGRAM_COMPLETION_QUERY_CHROMIUM begun before linkProgram and
   * ended after it; Blink asks the query whether its result is available
   * (clientWaitSync, getSyncParameter, getProgramParameter's COMPLETION_
   * STATUS_KHR). ANGLE knows none of these targets: the query was never
   * begun, its result never available, a page's fence never signalled and
   * its program never done (and each ask left GL_INVALID_OPERATION for the
   * page's getError). Such a query is kept here: a fence's by a fence of
   * ANGLE put in at its end (OpenGL ES 3; in an OpenGL ES 2 context, where
   * WebGL has no fences, its result is there once the calls are flushed),
   * available when that fence has signalled; a link's by the program
   * linked within it, available once ended, with the link status as its
   * result (asking ANGLE for the link status waits for the link)
   */
  static bool OwnTarget(GLenum target) {
    return target == GL_READBACK_SHADOW_COPIES_UPDATED_CHROMIUM ||
           target == GL_COMMANDS_COMPLETED_CHROMIUM || target == GL_COMMANDS_ISSUED_CHROMIUM ||
           target == GL_PROGRAM_COMPLETION_QUERY_CHROMIUM;
  }

  void BeginQueryEXT(GLenum target, GLuint id) override {
    Timed t(this);
    if (!OwnTarget(target)) {
      if (es3_)
        GL_BeginQuery(target, id);
      else
        GL_BeginQueryEXT(target, id);
      return;
    }
    OwnQuery& q = own_[id];
    if (q.fence != nullptr)
      GL_DeleteSync(q.fence);
    q = OwnQuery();
    q.target = target;
    own_active_[target] = id;
  }

  void EndQueryEXT(GLenum target) override {
    Timed t(this);
    if (!OwnTarget(target)) {
      if (es3_)
        GL_EndQuery(target);
      else
        GL_EndQueryEXT(target);
      return;
    }
    auto a = own_active_.find(target);
    if (a == own_active_.end())
      return;
    auto it = own_.find(a->second);
    own_active_.erase(a);
    if (it == own_.end() || target == GL_PROGRAM_COMPLETION_QUERY_CHROMIUM)
      return;
    if (target != GL_COMMANDS_ISSUED_CHROMIUM && es3_)
      it->second.fence = GL_FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (target == GL_READBACK_SHADOW_COPIES_UPDATED_CHROMIUM)
      n_.fences++;
    GL_Flush();
  }

  void LinkProgram(GLuint program) override {
    Timed t(this);
    GL_LinkProgram(program);
    auto a = own_active_.find(GL_PROGRAM_COMPLETION_QUERY_CHROMIUM);
    if (a != own_active_.end()) {
      auto it = own_.find(a->second);
      if (it != own_.end())
        it->second.program = program;
    }
  }

  void GetQueryObjectuivEXT(GLuint id, GLenum pname, GLuint* params) override {
    Timed t(this);
    auto it = own_.find(id);
    if (it == own_.end()) {
      if (es3_)
        GL_GetQueryObjectuiv(id, pname, params);
      else
        GL_GetQueryObjectuivEXT(id, pname, params);
      return;
    }
    OwnQuery& q = it->second;
    if (q.fence != nullptr) {
      GLenum r = GL_ClientWaitSync(q.fence, 0, 0);
      if (r != GL_TIMEOUT_EXPIRED) {
        GL_DeleteSync(q.fence);
        q.fence = nullptr;
      }
    }
    auto a = own_active_.find(q.target);
    bool ended = a == own_active_.end() || a->second != id;
    if (pname == GL_QUERY_RESULT_AVAILABLE) {
      *params = ended && q.fence == nullptr ? GL_TRUE : GL_FALSE;
      if (q.target == GL_READBACK_SHADOW_COPIES_UPDATED_CHROMIUM) {
        if (*params)
          n_.avail_yes++;
        else
          n_.avail_no++;
      }
    } else if (pname == GL_QUERY_RESULT) {
      GLint result = GL_TRUE;
      if (q.target == GL_PROGRAM_COMPLETION_QUERY_CHROMIUM) {
        result = GL_FALSE;
        if (q.program != 0 && GL_IsProgram(q.program))
          GL_GetProgramiv(q.program, GL_LINK_STATUS, &result);
      }
      *params = static_cast<GLuint>(result);
    }
  }

  void DeleteQueriesEXT(GLsizei n, const GLuint* queries) override {
    Timed t(this);
    for (GLsizei i = 0; i < n; i++) {
      auto it = own_.find(queries[i]);
      if (it == own_.end())
        continue;
      if (it->second.fence != nullptr)
        GL_DeleteSync(it->second.fence);
      for (auto a = own_active_.begin(); a != own_active_.end();) {
        if (a->second == queries[i])
          a = own_active_.erase(a);
        else
          ++a;
      }
      own_.erase(it);
    }
    if (es3_)
      GL_DeleteQueries(n, queries);
    else
      GL_DeleteQueriesEXT(n, queries);
  }

  // ---- shared images: the drawing buffer's colour buffers ----
  GLuint CreateAndTexStorage2DSharedImageCHROMIUM(const GLbyte* mailbox) override {
    return ColorBuffer(mailbox);
  }
  void BeginSharedImageAccessDirectCHROMIUM(GLuint, GLenum) override {}
  void EndSharedImageAccessDirectCHROMIUM(GLuint) override {}

 private:
  void Cur() {
    if (g_current != serial_) {
      bool fresh = false;
      int group = ThreadGroup();
      if (group != group_ && !regrouped_) {
        /*
         * First used on another thread than the one it was made on (a
         * worker's OffscreenCanvas): made again in that thread's group,
         * before anything is drawn with it
         */
        regrouped_ = true;
        EGLContext c = Make(group);
        if (c != EGL_NO_CONTEXT) {
          EGL_DestroyContext(Display().dpy, ctx_);
          ctx_ = c;
          group_ = group;
          fresh = true;
        }
      }
      if (!EGL_MakeCurrent(Display().dpy, surf_, surf_, ctx_)) {
        // tried again at the next call; ANGLE answers the calls meanwhile as with no context
        static std::atomic<bool> told{false};
        if (!told.exchange(true))
          printf("browser: webgl: a context could not be made current (EGL error 0x%x)\n", EGL_GetError());
        // and none left current, so that no call goes to the context current before
        EGL_MakeCurrent(Display().dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        g_current = 0;
        return;
      }
      g_current = serial_;
      current_on_ = pthread_self();
      if (fresh)
        Prepare();
    }
  }

  const uint64_t serial_;     // told from every other context, even one made at its address
  Counts n_;
  uint8_t rp_first_[4] = {};     // the first bytes of the last readPixels into memory
  uint8_t gbsd_first_[8] = {};   // of the last getBufferSubData
  int last_w_ = 0, last_h_ = 0;  // the colour buffer the last frame was read from
  pthread_t current_on_ = 0;  // the thread it was last made current on (0: none)
  int group_ = 0;             // the virtualisation group it is in (ThreadGroup)
  bool regrouped_ = false;    // made again for the thread it is used on

  std::string Str(GLenum name) {
    const GLubyte* s = GL_GetString(name);
    return s ? std::string(reinterpret_cast<const char*>(s)) : std::string();
  }

  // The multi-draw calls' byte offsets as the pointers ANGLE takes
  std::vector<const void*> Ptrs(const GLsizei* offsets, GLsizei n) {
    std::vector<const void*> p(static_cast<size_t>(n > 0 ? n : 0));
    for (GLsizei i = 0; i < n; i++)
      p[i] = reinterpret_cast<const void*>(static_cast<uintptr_t>(offsets[i]));
    return p;
  }

  /*
   * An error ANGLE set, with its reason: given to the context's error
   * messages (as many as a context reports at most), and each different
   * one the first time to the console of the system too
   */
  static void GL_APIENTRY OnDebugMessage(GLenum source, GLenum type, GLuint id, GLenum,
                                         GLsizei, const GLchar* message, const void* user) {
    auto* self = static_cast<BrAngleGLES2*>(const_cast<void*>(user));
    if (self == nullptr || type != GL_DEBUG_TYPE_ERROR_KHR || source != GL_DEBUG_SOURCE_API_KHR)
      return;
    if (self->errors_told_ >= kMaxErrorsTold)
      return;
    self->errors_told_++;
    const char* name = id == GL_INVALID_ENUM                    ? "GL_INVALID_ENUM"
                       : id == GL_INVALID_VALUE                 ? "GL_INVALID_VALUE"
                       : id == GL_INVALID_OPERATION             ? "GL_INVALID_OPERATION"
                       : id == GL_OUT_OF_MEMORY                 ? "GL_OUT_OF_MEMORY"
                       : id == GL_INVALID_FRAMEBUFFER_OPERATION ? "GL_INVALID_FRAMEBUFFER_OPERATION"
                                                                : "GL error";
    std::string what = std::string(name) + ": " + (message ? message : "");
    std::string m = std::string("[.WebGL-") + std::to_string(self->serial_) + "]" + what;
    {
      static std::mutex mu;
      static std::set<std::string> told;
      std::lock_guard<std::mutex> l(mu);
      if (told.size() < 64 && told.insert(what).second)
        printf("browser: webgl: %s\n", m.c_str());
    }
    if (self->on_error_)
      self->on_error_.Run(m.c_str(), static_cast<int32_t>(id));
  }

  void Missing(const char* name) {
    static std::set<std::string> told;
    if (told.insert(name).second)
      printf("browser: webgl: %s is not given to ANGLE\n", name);
  }

  /*
   * The texture behind a shared image: RGBA8 storage of the size the
   * image was made with, the texture binding left as it was
   */
  GLuint ColorBuffer(const GLbyte* mailbox) {
    Cur();
    std::string key(reinterpret_cast<const char*>(mailbox), 16);
    int w = 1, h = 1;
    auto it = mailboxes_.find(key);
    if (it != mailboxes_.end()) {
      w = it->second.first;
      h = it->second.second;
    }
    GLint old = 0;
    GLuint id = 0;
    GL_GetIntegerv(GL_TEXTURE_BINDING_2D, &old);
    GL_GenTextures(1, &id);
    GL_BindTexture(GL_TEXTURE_2D, id);
    if (es3_)
      GL_TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
    else
      GL_TexStorage2DEXT(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
    GL_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    GL_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    GL_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    GL_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GL_BindTexture(GL_TEXTURE_2D, static_cast<GLuint>(old));
    return id;
  }

  EGLSurface surf_ = EGL_NO_SURFACE;
  EGLContext ctx_ = EGL_NO_CONTEXT;
  GLint max_texture_size_ = 2048;
  bool es3_ = false;
  std::string ext_;  // the extensions GetString answered last
  static constexpr int kMaxErrorsTold = 256;
  int errors_told_ = 0;  // ANGLE's errors given to the error messages
  base::RepeatingCallback<void(const char*, int32_t)> on_error_;

  // a query of the command buffer's own targets: its target, its fence, the program linked in it
  struct OwnQuery {
    GLenum target = 0;
    GLsync fence = nullptr;
    GLuint program = 0;
  };
  std::map<GLuint, OwnQuery> own_;          // by the query's name
  std::map<GLenum, GLuint> own_active_;     // the one begun and not yet ended, by target
};

/* The summaries of the contexts used since the last, when kSummarySeconds have gone by */
void SummaryIfDue() {
  uint64_t now = TicksNow(), due = g_next_summary.load(std::memory_order_relaxed);
  uint64_t next = now + TicksPerSecond() * kSummarySeconds;
  if (now < due || !g_next_summary.compare_exchange_strong(due, next))
    return;
  if (due == 0)
    return;  // the first call sets the clock
  std::string out;
  {
    std::lock_guard<std::mutex> l(Registry());
    for (BrAngleGLES2* c : Contexts())
      c->SummaryLine(&out);
  }
  if (!out.empty())
    fputs(out.c_str(), stdout);
}

/*
 * What a WebGL context of ANGLE offers here, once for WebGL 1 and once
 * for 2: its extensions and the limits a page chooses its way of drawing
 * by, so that the board's and QEMU's can be compared line by line
 */
void TellCaps(BrAngleGLES2* gl, bool es3) {
  static const struct {
    GLenum name;
    const char* what;
    bool es3;
  } kLimits[] = {
      {GL_MAX_TEXTURE_SIZE, "texture", false},
      {GL_MAX_CUBE_MAP_TEXTURE_SIZE, "cube", false},
      {GL_MAX_RENDERBUFFER_SIZE, "renderbuffer", false},
      {GL_MAX_VERTEX_ATTRIBS, "attribs", false},
      {GL_MAX_VERTEX_UNIFORM_VECTORS, "vertex uniforms", false},
      {GL_MAX_FRAGMENT_UNIFORM_VECTORS, "fragment uniforms", false},
      {GL_MAX_VARYING_VECTORS, "varyings", false},
      {GL_MAX_TEXTURE_IMAGE_UNITS, "fragment textures", false},
      {GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, "vertex textures", false},
      {GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, "textures", false},
      {GL_MAX_SAMPLES, "samples", true},
      {GL_MAX_DRAW_BUFFERS, "draw buffers", true},
      {GL_MAX_COLOR_ATTACHMENTS, "colour attachments", true},
      {GL_MAX_3D_TEXTURE_SIZE, "3d texture", true},
      {GL_MAX_ARRAY_TEXTURE_LAYERS, "array layers", true},
      {GL_MAX_UNIFORM_BLOCK_SIZE, "uniform block", true},
      {GL_MAX_UNIFORM_BUFFER_BINDINGS, "uniform buffers", true},
      {GL_MAX_VERTEX_UNIFORM_BLOCKS, "vertex uniform blocks", true},
      {GL_MAX_FRAGMENT_UNIFORM_BLOCKS, "fragment uniform blocks", true},
      {GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS, "feedback attribs", true},
  };
  std::string line = std::string("browser: webgl: limits of WebGL ") + (es3 ? "2:" : "1:");
  for (const auto& l : kLimits) {
    if (l.es3 && !es3)
      continue;
    GLint v = 0;
    gl->GetIntegerv(l.name, &v);
    line += " " + std::string(l.what) + " " + std::to_string(v) + ",";
  }
  GLint dims[2] = {0, 0}, range[2] = {0, 0}, prec = 0, irange[2] = {0, 0}, iprec = 0;
  gl->GetIntegerv(GL_MAX_VIEWPORT_DIMS, dims);
  gl->GetShaderPrecisionFormat(GL_FRAGMENT_SHADER, GL_HIGH_FLOAT, range, &prec);
  gl->GetShaderPrecisionFormat(GL_FRAGMENT_SHADER, GL_HIGH_INT, irange, &iprec);
  line += " viewport " + std::to_string(dims[0]) + "x" + std::to_string(dims[1]) +
          ", fragment highp float " + std::to_string(range[0]) + "/" + std::to_string(range[1]) +
          "/" + std::to_string(prec) + ", int " + std::to_string(irange[0]) + "/" +
          std::to_string(irange[1]) + "\n";
  fputs(line.c_str(), stdout);
  const GLubyte* e = gl->GetString(GL_EXTENSIONS);
  printf("browser: webgl: extensions of WebGL %d: %s\n", es3 ? 2 : 1,
         e ? reinterpret_cast<const char*>(e) : "?");
}

/* ---------------------------------------------------------------- shared images and the provider */

class BrAngleSharedImageInterface final : public gpu::SharedImageInterface {
 public:
  explicit BrAngleSharedImageInterface(BrAngleGLES2* gl) : gl_(gl) {}

  // A mailbox for every image made, and its size for ColorBuffer
  scoped_refptr<gpu::ClientSharedImage> Mint(const gpu::SharedImageInfo& si) {
    scoped_refptr<gpu::ClientSharedImage> img = gpu::ClientSharedImage::CreateForTesting(
        static_cast<const gpu::SharedImageMetadata&>(si), GL_TEXTURE_2D);
    gl_->mailboxes_[std::string(reinterpret_cast<const char*>(img->mailbox().name), 16)] = {
        si.size.width(), si.size.height()};
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
    gl_->mailboxes_.erase(std::string(reinterpret_cast<const char*>(mailbox.name), 16));
  }
  void DestroySharedImage(const gpu::SyncToken&,
                          scoped_refptr<gpu::ClientSharedImage> image) override {
    if (image) {
      gl_->mailboxes_.erase(
          std::string(reinterpret_cast<const char*>(image->mailbox().name), 16));
    }
  }
  scoped_refptr<gpu::ClientSharedImage> ImportSharedImage(gpu::ExportedSharedImage) override {
    return nullptr;
  }
  gpu::SyncToken GenUnverifiedSyncToken() override { return gpu::SyncToken(); }
  gpu::SyncToken GenVerifiedSyncToken() override { return gpu::SyncToken(); }
  void VerifySyncToken(gpu::SyncToken&) override {}
  // Every call has run when it returns: any token is verified and a flush
  // has nothing left to push
  bool CanVerifySyncToken(const gpu::SyncToken&) override { return true; }
  void VerifyFlush() override {}
  void WaitSyncToken(const gpu::SyncToken&) override {}
  const gpu::SharedImageCapabilities& GetCapabilities() override { return caps_; }

 private:
  BrAngleGLES2* gl_;
  gpu::SharedImageCapabilities caps_;
};

class BrAngleProvider final : public blink::WebGraphicsContext3DProvider {
 public:
  BrAngleProvider() : sii_(&gl_) { prefs_.anti_aliasing_mode = blink::kAntialiasingModeNone; }

  bool Init(bool es3, std::string* why) {
    if (!gl_.Init(es3, why))
      return false;
    caps_.max_texture_size = gl_.max_texture_size();
    return true;
  }

  void Release() { gl_.Release(); }

  gpu::InterfaceBase* InterfaceBase() override { return &gl_; }
  gpu::gles2::GLES2Interface* ContextGL() override { return &gl_; }
  gpu::raster::RasterInterface* RasterInterface() override { return nullptr; }
  gpu::ContextSupport* ContextSupport() override { return nullptr; }
  gpu::webgpu::WebGPUInterface* WebGPUInterface() override { return nullptr; }
  bool BindToCurrentSequence() override { return true; }
  const gpu::Capabilities& GetCapabilities() const override { return caps_; }
  const gpu::GpuFeatureInfo& GetGpuFeatureInfo() const override { return feat_; }
  const blink::WebglPreferences& GetWebglPreferences() const override { return prefs_; }
  // Rasterization runs on the CPU here, so there is no raster context
  viz::RasterContextProvider* RasterContextProvider() const override { return nullptr; }
  bool IsContextLost() override { return false; }
  void SetLostContextCallback(base::RepeatingClosure) override {}
  void SetErrorMessageCallback(
      base::RepeatingCallback<void(const char*, int32_t)> cb) override {
    gl_.SetErrorMessageCallback(std::move(cb));
  }
  cc::ImageDecodeCache* ImageDecodeCache(SkColorType) override { return nullptr; }
  gpu::SharedImageInterface* SharedImageInterface() override { return &sii_; }

 private:
  BrAngleGLES2 gl_;
  BrAngleSharedImageInterface sii_;
  gpu::Capabilities caps_;
  gpu::GpuFeatureInfo feat_;
  blink::WebglPreferences prefs_;
};

}  // namespace

/*
 * ANGLE's look at the PCI bus for the GPU (its libpci source is not
 * built): the GPU of a TessronOS machine is not on one
 */
namespace angle {
bool GetPCIDevicesWithLibPCI(std::vector<GPUDeviceInfo>*) {
  return false;
}
}  // namespace angle

/* The time of the calling thread's WebGL calls, in the counter's ticks (br_blink.cc's measure of a turn) */
uint64_t br_webgl_ticks() {
  return g_ticks;
}

/* WebGL's frames of the contexts in the GPU process (port/webgl/gl_cmdbuf.cc) */
long br_webgl_cmdbuf_frames() __attribute__((weak));

/* The frames of WebGL shown since the start, here and in the GPU process (the console's measure, br_main.cc) */
long br_webgl_frames() {
  return g_frames + (br_webgl_cmdbuf_frames != nullptr ? br_webgl_cmdbuf_frames() : 0);
}

/*
 * A WebGL context on ANGLE (es3: WebGL 2), or nullptr when ANGLE did not
 * start; the reason goes to the console the first time
 */
std::unique_ptr<blink::WebGraphicsContext3DProvider> BrCreateAngleGLProvider(
    bool es3, blink::Platform::WebGLContextInfo* info) {
  static bool told[2], failed[2];
  auto p = std::make_unique<BrAngleProvider>();
  std::string why;
  if (!p->Init(es3, &why)) {
    if (!failed[es3]) {
      printf("browser: webgl: WebGL %d not on ANGLE (%s)\n", es3 ? 2 : 1, why.c_str());
      failed[es3] = true;
    }
    return nullptr;
  }
  if (info != nullptr) {
    const GLubyte* r = p->ContextGL()->GetString(GL_RENDERER);
    const GLubyte* v = p->ContextGL()->GetString(GL_VENDOR);
    info->vendor_id = 0;
    info->device_id = 0;
    info->renderer_info = blink::WebString::FromUtf8(r ? reinterpret_cast<const char*>(r) : "ANGLE");
    info->vendor_info = blink::WebString::FromUtf8(v ? reinterpret_cast<const char*>(v) : "");
    info->driver_version = blink::WebString::FromUtf8("1.0");
    info->using_passthrough_command_decoder = true;
  }
  if (!told[es3]) {
    TellCaps(static_cast<BrAngleGLES2*>(p->ContextGL()), es3);
    const GLubyte* r = p->ContextGL()->GetString(GL_RENDERER);
    printf("browser: webgl: WebGL %d on ANGLE, %s\n", es3 ? 2 : 1,
           r ? reinterpret_cast<const char*>(r) : "?");
    told[es3] = true;
  }
  p->Release();
  return p;
}
