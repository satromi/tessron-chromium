/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	gl_tessronos.cc
 *	ブラウザ: WebGL's GLES2 on the CPU (design 17.19).
 *
 *	There is no GPU process and no command buffer: Blink's WebGL sits on
 *	a gpu::gles2::GLES2Interface, and this implements it as a small
 *	state machine on the CPU -- ids, the storage of textures and
 *	renderbuffers, framebuffer completeness, clear and readback, and the
 *	shaders and programs, which br_glsl.cc compiles and runs to draw.
 *	The drawing buffer's colour buffers come through shared images, so a
 *	small SharedImageInterface maps mailboxes to the textures behind
 *	them.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <thread>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "br_glsl.h"
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
#include "ui/gfx/color_space.h"
#include "ui/gfx/geometry/size.h"


namespace {

struct BrTex {
  int w = 0;
  int h = 0;
  std::vector<unsigned char> px;  // RGBA8
  GLenum min_filter = 0x2702;     // NEAREST_MIPMAP_LINEAR
  GLenum mag_filter = 0x2601;     // LINEAR
  GLenum wrap_s = 0x2901;         // REPEAT
  GLenum wrap_t = 0x2901;
};

struct BrVtxAttrib {
  bool enabled = false;
  GLint size = 4;
  GLenum gltype = 0x1406;
  bool normalized = false;
  GLsizei stride = 0;
  GLuint buffer = 0;
  uintptr_t offset = 0;
  float const_val[4] = {0, 0, 0, 1};
};

struct BrRb {
  GLenum fmt = 0;
  int w = 0;
  int h = 0;
  std::vector<unsigned char> px;  // only for color formats
};

struct BrFb {
  GLuint color_tex = 0;
  GLuint color_rb = 0;
  GLuint depth_rb = 0;
  GLuint stencil_rb = 0;
};

struct BrBuf {
  std::vector<unsigned char> data;
};

struct BrShader {
  GLenum type = 0;
  std::string src;
};

struct BrVar {
  std::string name;
  GLenum type = 0;
  int size = 1;
};

struct BrProgram {
  std::vector<GLuint> shaders;
  std::vector<BrVar> attrs;
  std::vector<BrVar> unis;
  bool linked = false;
  brgl::Program* engine = nullptr;      // set when the GLSL pipeline compiled
  std::vector<brgl::Val> uniform_vals;  // by engine uniform slot
};

GLenum GlTypeOf(brgl::ValType t) {
  switch (t) {
    case brgl::T_FLOAT: return 0x1406;
    case brgl::T_INT: return 0x1404;
    case brgl::T_BOOL: return 0x8B56;
    case brgl::T_VEC2: return 0x8B50;
    case brgl::T_VEC3: return 0x8B51;
    case brgl::T_VEC4: return 0x8B52;
    case brgl::T_MAT2: return 0x8B5A;
    case brgl::T_MAT3: return 0x8B5B;
    case brgl::T_MAT4: return 0x8B5C;
    case brgl::T_SAMPLER2D: return 0x8B5E;
    case brgl::T_SAMPLERCUBE: return 0x8B60;
    default: return 0x8B52;
  }
}

bool IsColorRenderbufferFormat(GLenum fmt) {
  return fmt == 0x8058 /* RGBA8_OES */ || fmt == 0x8051 /* RGB8_OES */ ||
         fmt == 0x8056 /* RGBA4 */ || fmt == 0x8057 /* RGB5_A1 */ ||
         fmt == 0x8D62 /* RGB565 */;
}

// Pulls "attribute/uniform <type> <name>;" declarations out of GLSL source.
// This is bookkeeping for location queries, not compilation: the token
// after the qualifier is the type, the next is the name (arrays keep their
// base name, precision qualifiers are skipped).
void ParseGlslVars(const std::string& src, const char* qualifier,
                   std::vector<BrVar>* out) {
  size_t at = 0;
  size_t qlen = strlen(qualifier);
  while ((at = src.find(qualifier, at)) != std::string::npos) {
    size_t p = at + qlen;
    at = p;
    // Must be a whole word at statement position.
    if (at > qlen && (isalnum((unsigned char)src[at - qlen - 1]) ||
                      src[at - qlen - 1] == '_'))
      continue;
    std::vector<std::string> toks;
    std::string cur;
    while (p < src.size() && src[p] != ';' && toks.size() < 4) {
      char c = src[p++];
      if (isalnum((unsigned char)c) || c == '_') {
        cur += c;
      } else {
        if (!cur.empty()) {
          toks.push_back(cur);
          cur.clear();
        }
        if (c == '[')
          break;
      }
    }
    if (!cur.empty())
      toks.push_back(cur);
    // Drop precision qualifiers.
    while (toks.size() > 2 &&
           (toks[0] == "highp" || toks[0] == "mediump" || toks[0] == "lowp"))
      toks.erase(toks.begin());
    if (toks.size() < 2)
      continue;
    BrVar v;
    v.name = toks[1];
    const std::string& t = toks[0];
    if (t == "float") v.type = 0x1406;
    else if (t == "vec2") v.type = 0x8B50;
    else if (t == "vec3") v.type = 0x8B51;
    else if (t == "vec4") v.type = 0x8B52;
    else if (t == "int") v.type = 0x1404;
    else if (t == "ivec2") v.type = 0x8B53;
    else if (t == "ivec3") v.type = 0x8B54;
    else if (t == "ivec4") v.type = 0x8B55;
    else if (t == "bool") v.type = 0x8B56;
    else if (t == "mat2") v.type = 0x8B5A;
    else if (t == "mat3") v.type = 0x8B5B;
    else if (t == "mat4") v.type = 0x8B5C;
    else if (t == "sampler2D") v.type = 0x8B5E;
    else if (t == "samplerCube") v.type = 0x8B60;
    else v.type = 0x8B52;
    bool dup = false;
    for (const BrVar& e : *out)
      if (e.name == v.name)
        dup = true;
    if (!dup)
      out->push_back(v);
  }
}

class BrGLES2 final : public gpu::gles2::GLES2InterfaceStub {
 public:
  // ---- ids and object stores ----
  std::map<GLuint, BrTex> tex_;
  std::map<GLuint, BrRb> rb_;
  std::map<GLuint, BrFb> fb_;
  std::map<GLuint, BrBuf> buf_;
  std::map<GLuint, BrShader> sh_;
  std::map<GLuint, BrProgram> prog_;
  std::map<std::string, GLuint> mailbox_dims_done_;
  GLuint next_id_ = 1;

  // mailbox name bytes -> pending size for CreateAndTexStorage2D
  std::map<std::string, std::pair<int, int>> mailboxes_;

  // ---- state ----
  GLenum error_ = 0;  // GL_NO_ERROR
  GLuint bound_fb_ = 0;
  GLuint bound_rb_ = 0;
  GLuint bound_array_buf_ = 0;
  GLuint bound_element_buf_ = 0;
  GLuint current_program_ = 0;
  int active_unit_ = 0;
  GLuint bound_tex2d_[64] = {0};
  float clear_color_[4] = {0, 0, 0, 0};
  bool scissor_on_ = false;
  int scissor_[4] = {0, 0, 0, 0};
  int viewport_[4] = {0, 0, 0, 0};
  int pack_alignment_ = 4;
  int unpack_alignment_ = 4;
  unsigned draw_calls_ = 0;
  BrVtxAttrib va_[16];
  bool depth_test_ = false;
  bool blend_on_ = false;
  bool cull_on_ = false;
  GLenum depth_func_ = 0x0201;  // LESS
  bool depth_mask_ = true;
  GLenum blend_src_ = 1, blend_dst_ = 0;
  GLenum cull_face_ = 0x0405;  // BACK
  bool front_ccw_ = true;
  float clear_depth_ = 1.0f;
  std::map<GLuint, std::vector<float>> depth_;  // per framebuffer

  float* DepthFor(int w, int h) {
    std::vector<float>& d = depth_[bound_fb_];
    if ((int)d.size() != w * h)
      d.assign((size_t)w * h, 1.0f);
    return d.data();
  }

  void SetError(GLenum e) {
    if (error_ == 0)
      error_ = e;
  }

  BrTex* BoundTex2D() {
    GLuint id = bound_tex2d_[active_unit_];
    if (id == 0)
      return nullptr;
    return &tex_[id];
  }

  // Resolves the color storage behind the currently bound framebuffer.
  // The default framebuffer of an offscreen context is never rendered to
  // by blink (DrawingBuffer always binds its own), so id 0 maps to a tiny
  // placeholder.
  bool ColorStore(unsigned char** px, int* w, int* h) {
    static BrTex fb0;
    if (bound_fb_ == 0) {
      if (fb0.px.empty()) {
        fb0.w = 1;
        fb0.h = 1;
        fb0.px.resize(4, 0);
      }
      *px = fb0.px.data();
      *w = fb0.w;
      *h = fb0.h;
      return true;
    }
    BrFb& f = fb_[bound_fb_];
    if (f.color_tex != 0) {
      BrTex& t = tex_[f.color_tex];
      if (t.px.empty())
        return false;
      *px = t.px.data();
      *w = t.w;
      *h = t.h;
      return true;
    }
    if (f.color_rb != 0) {
      BrRb& r = rb_[f.color_rb];
      if (r.px.empty())
        return false;
      *px = r.px.data();
      *w = r.w;
      *h = r.h;
      return true;
    }
    return false;
  }

  // ---- object lifecycle ----
  void GenTextures(GLsizei n, GLuint* ids) override {
    for (GLsizei i = 0; i < n; ++i) {
      ids[i] = next_id_++;
      tex_[ids[i]] = BrTex();
    }
  }
  void GenBuffers(GLsizei n, GLuint* ids) override {
    for (GLsizei i = 0; i < n; ++i) {
      ids[i] = next_id_++;
      buf_[ids[i]] = BrBuf();
    }
  }
  void GenFramebuffers(GLsizei n, GLuint* ids) override {
    for (GLsizei i = 0; i < n; ++i) {
      ids[i] = next_id_++;
      fb_[ids[i]] = BrFb();
    }
  }
  void GenRenderbuffers(GLsizei n, GLuint* ids) override {
    for (GLsizei i = 0; i < n; ++i) {
      ids[i] = next_id_++;
      rb_[ids[i]] = BrRb();
    }
  }
  void DeleteTextures(GLsizei n, const GLuint* ids) override {
    for (GLsizei i = 0; i < n; ++i)
      tex_.erase(ids[i]);
  }
  void DeleteBuffers(GLsizei n, const GLuint* ids) override {
    for (GLsizei i = 0; i < n; ++i)
      buf_.erase(ids[i]);
  }
  void DeleteFramebuffers(GLsizei n, const GLuint* ids) override {
    for (GLsizei i = 0; i < n; ++i) {
      if (bound_fb_ == ids[i])
        bound_fb_ = 0;
      fb_.erase(ids[i]);
    }
  }
  void DeleteRenderbuffers(GLsizei n, const GLuint* ids) override {
    for (GLsizei i = 0; i < n; ++i)
      rb_.erase(ids[i]);
  }

  // ---- bindings ----
  void ActiveTexture(GLenum texture) override {
    int unit = (int)(texture - 0x84C0 /* GL_TEXTURE0 */);
    if (unit >= 0 && unit < 64)
      active_unit_ = unit;
  }
  void BindTexture(GLenum target, GLuint texture) override {
    if (target == 0x0DE1 /* GL_TEXTURE_2D */)
      bound_tex2d_[active_unit_] = texture;
  }
  void BindBuffer(GLenum target, GLuint buffer) override {
    if (target == 0x8892 /* ARRAY_BUFFER */)
      bound_array_buf_ = buffer;
    else if (target == 0x8893 /* ELEMENT_ARRAY_BUFFER */)
      bound_element_buf_ = buffer;
  }
  void BindFramebuffer(GLenum, GLuint framebuffer) override {
    bound_fb_ = framebuffer;
    if (framebuffer != 0 && fb_.find(framebuffer) == fb_.end())
      fb_[framebuffer] = BrFb();
  }
  void BindRenderbuffer(GLenum, GLuint renderbuffer) override {
    bound_rb_ = renderbuffer;
    if (renderbuffer != 0 && rb_.find(renderbuffer) == rb_.end())
      rb_[renderbuffer] = BrRb();
  }

  // ---- texture and renderbuffer storage ----
  void TexImage2D(GLenum target, GLint level, GLint internalformat,
                  GLsizei width, GLsizei height, GLint border, GLenum format,
                  GLenum type, const void* pixels) override {
    static int calls = 0;
    if ((++calls & 63) == 1)
      printf("browser: brgl: teximage #%d %dx%d (textures=%d)\n", calls, (int)width,
              (int)height, (int)tex_.size());
    if (target != 0x0DE1 || level != 0)
      return;
    BrTex* t = BoundTex2D();
    if (t == nullptr)
      return;
    t->w = width;
    t->h = height;
    t->px.assign((size_t)width * height * 4, 0);
    if (pixels != nullptr && type == 0x1401 /* UNSIGNED_BYTE */) {
      const unsigned char* src = (const unsigned char*)pixels;
      size_t n = (size_t)width * height;
      if (format == 0x1908 /* RGBA */) {
        memcpy(t->px.data(), src, n * 4);
      } else if (format == 0x1907 /* RGB */) {
        for (size_t i = 0; i < n; ++i) {
          t->px[i * 4] = src[i * 3];
          t->px[i * 4 + 1] = src[i * 3 + 1];
          t->px[i * 4 + 2] = src[i * 3 + 2];
          t->px[i * 4 + 3] = 255;
        }
      }
    }
  }
  void TexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                     GLsizei width, GLsizei height, GLenum format, GLenum type,
                     const void* pixels) override {
    if (target != 0x0DE1 || level != 0 || pixels == nullptr ||
        type != 0x1401 || format != 0x1908)
      return;
    BrTex* t = BoundTex2D();
    if (t == nullptr || t->px.empty())
      return;
    const unsigned char* src = (const unsigned char*)pixels;
    for (GLsizei row = 0; row < height; ++row) {
      int dy = yoffset + row;
      if (dy < 0 || dy >= t->h)
        continue;
      for (GLsizei col = 0; col < width; ++col) {
        int dx = xoffset + col;
        if (dx < 0 || dx >= t->w)
          continue;
        memcpy(&t->px[((size_t)dy * t->w + dx) * 4],
               &src[((size_t)row * width + col) * 4], 4);
      }
    }
  }
  void RenderbufferStorage(GLenum, GLenum internalformat, GLsizei width,
                           GLsizei height) override {
    if (bound_rb_ == 0)
      return;
    BrRb& r = rb_[bound_rb_];
    r.fmt = internalformat;
    r.w = width;
    r.h = height;
    if (IsColorRenderbufferFormat(internalformat))
      r.px.assign((size_t)width * height * 4, 0);
    else
      r.px.clear();
  }

  // ---- framebuffer wiring ----
  void FramebufferTexture2D(GLenum, GLenum attachment, GLenum, GLuint texture,
                            GLint) override {
    if (bound_fb_ == 0)
      return;
    BrFb& f = fb_[bound_fb_];
    if (attachment == 0x8CE0 /* COLOR_ATTACHMENT0 */) {
      f.color_tex = texture;
      f.color_rb = 0;
    }
  }
  void FramebufferRenderbuffer(GLenum, GLenum attachment, GLenum,
                               GLuint renderbuffer) override {
    if (bound_fb_ == 0)
      return;
    BrFb& f = fb_[bound_fb_];
    switch (attachment) {
      case 0x8CE0: /* COLOR_ATTACHMENT0 */
        f.color_rb = renderbuffer;
        f.color_tex = 0;
        break;
      case 0x8D00: /* DEPTH_ATTACHMENT */
        f.depth_rb = renderbuffer;
        break;
      case 0x8D20: /* STENCIL_ATTACHMENT */
        f.stencil_rb = renderbuffer;
        break;
      case 0x821A: /* DEPTH_STENCIL_ATTACHMENT */
        f.depth_rb = renderbuffer;
        f.stencil_rb = renderbuffer;
        break;
    }
  }
  GLenum CheckFramebufferStatus(GLenum) override {
    return 0x8CD5; /* FRAMEBUFFER_COMPLETE */
  }

  // ---- raster state and clear ----
  void Viewport(GLint x, GLint y, GLsizei w, GLsizei h) override {
    viewport_[0] = x;
    viewport_[1] = y;
    viewport_[2] = w;
    viewport_[3] = h;
  }
  void Scissor(GLint x, GLint y, GLsizei w, GLsizei h) override {
    scissor_[0] = x;
    scissor_[1] = y;
    scissor_[2] = w;
    scissor_[3] = h;
  }
  void Enable(GLenum cap) override {
    if (cap == 0x0C11 /* SCISSOR_TEST */) scissor_on_ = true;
    else if (cap == 0x0B71 /* DEPTH_TEST */) depth_test_ = true;
    else if (cap == 0x0BE2 /* BLEND */) blend_on_ = true;
    else if (cap == 0x0B44 /* CULL_FACE */) cull_on_ = true;
  }
  void Disable(GLenum cap) override {
    if (cap == 0x0C11) scissor_on_ = false;
    else if (cap == 0x0B71) depth_test_ = false;
    else if (cap == 0x0BE2) blend_on_ = false;
    else if (cap == 0x0B44) cull_on_ = false;
  }
  void DepthFunc(GLenum func) override { depth_func_ = func; }
  void DepthMask(GLboolean flag) override { depth_mask_ = flag != 0; }
  void BlendFunc(GLenum sfactor, GLenum dfactor) override {
    blend_src_ = sfactor;
    blend_dst_ = dfactor;
  }
  void BlendFuncSeparate(GLenum src_rgb, GLenum dst_rgb, GLenum,
                         GLenum) override {
    blend_src_ = src_rgb;
    blend_dst_ = dst_rgb;
  }
  void CullFace(GLenum mode) override { cull_face_ = mode; }
  void FrontFace(GLenum mode) override { front_ccw_ = mode == 0x0901; }
  void ClearDepthf(GLclampf d) override { clear_depth_ = d; }
  void TexParameteri(GLenum target, GLenum pname, GLint param) override {
    if (target != 0x0DE1)
      return;
    BrTex* t = BoundTex2D();
    if (t == nullptr)
      return;
    switch (pname) {
      case 0x2801: t->min_filter = (GLenum)param; break;  // MIN_FILTER
      case 0x2800: t->mag_filter = (GLenum)param; break;  // MAG_FILTER
      case 0x2802: t->wrap_s = (GLenum)param; break;      // WRAP_S
      case 0x2803: t->wrap_t = (GLenum)param; break;      // WRAP_T
    }
  }

  // ---- vertex attribute state ----
  void VertexAttribPointer(GLuint indx, GLint size, GLenum type,
                           GLboolean normalized, GLsizei stride,
                           const void* ptr) override {
    if (indx >= 16)
      return;
    BrVtxAttrib& a = va_[indx];
    a.size = size;
    a.gltype = type;
    a.normalized = normalized != 0;
    a.stride = stride;
    a.buffer = bound_array_buf_;
    a.offset = (uintptr_t)ptr;
  }
  void EnableVertexAttribArray(GLuint indx) override {
    if (indx < 16)
      va_[indx].enabled = true;
  }
  void DisableVertexAttribArray(GLuint indx) override {
    if (indx < 16)
      va_[indx].enabled = false;
  }
  void VertexAttrib1f(GLuint indx, GLfloat x) override {
    if (indx < 16) {
      va_[indx].const_val[0] = x;
      va_[indx].const_val[1] = 0;
      va_[indx].const_val[2] = 0;
      va_[indx].const_val[3] = 1;
    }
  }
  void VertexAttrib2f(GLuint indx, GLfloat x, GLfloat y) override {
    if (indx < 16) {
      va_[indx].const_val[0] = x;
      va_[indx].const_val[1] = y;
      va_[indx].const_val[2] = 0;
      va_[indx].const_val[3] = 1;
    }
  }
  void VertexAttrib3f(GLuint indx, GLfloat x, GLfloat y, GLfloat z) override {
    if (indx < 16) {
      va_[indx].const_val[0] = x;
      va_[indx].const_val[1] = y;
      va_[indx].const_val[2] = z;
      va_[indx].const_val[3] = 1;
    }
  }
  void VertexAttrib4f(GLuint indx, GLfloat x, GLfloat y, GLfloat z,
                      GLfloat w) override {
    if (indx < 16) {
      va_[indx].const_val[0] = x;
      va_[indx].const_val[1] = y;
      va_[indx].const_val[2] = z;
      va_[indx].const_val[3] = w;
    }
  }

  // ---- uniforms ----
  brgl::Val* UniformSlot(GLint location) {
    if (current_program_ == 0)
      return nullptr;
    BrProgram& p = prog_[current_program_];
    if (location < 0 || (size_t)location >= p.uniform_vals.size())
      return nullptr;
    return &p.uniform_vals[location];
  }
  void SetUniformF(GLint loc, brgl::ValType t, const float* v, int n) {
    brgl::Val* u = UniformSlot(loc);
    if (u == nullptr)
      return;
    u->type = t;
    for (int i = 0; i < n && i < 16; ++i)
      u->f[i] = v[i];
  }
  void Uniform1f(GLint l, GLfloat x) override {
    SetUniformF(l, brgl::T_FLOAT, &x, 1);
  }
  void Uniform2f(GLint l, GLfloat x, GLfloat y) override {
    float v[2] = {x, y};
    SetUniformF(l, brgl::T_VEC2, v, 2);
  }
  void Uniform3f(GLint l, GLfloat x, GLfloat y, GLfloat z) override {
    float v[3] = {x, y, z};
    SetUniformF(l, brgl::T_VEC3, v, 3);
  }
  void Uniform4f(GLint l, GLfloat x, GLfloat y, GLfloat z,
                 GLfloat w) override {
    float v[4] = {x, y, z, w};
    SetUniformF(l, brgl::T_VEC4, v, 4);
  }
  void Uniform1i(GLint l, GLint x) override {
    float v = (float)x;
    SetUniformF(l, brgl::T_INT, &v, 1);
  }
  void Uniform1fv(GLint l, GLsizei, const GLfloat* v) override {
    SetUniformF(l, brgl::T_FLOAT, v, 1);
  }
  void Uniform2fv(GLint l, GLsizei, const GLfloat* v) override {
    SetUniformF(l, brgl::T_VEC2, v, 2);
  }
  void Uniform3fv(GLint l, GLsizei, const GLfloat* v) override {
    SetUniformF(l, brgl::T_VEC3, v, 3);
  }
  void Uniform4fv(GLint l, GLsizei, const GLfloat* v) override {
    SetUniformF(l, brgl::T_VEC4, v, 4);
  }
  void UniformMatrix2fv(GLint l, GLsizei, GLboolean, const GLfloat* v)
      override {
    SetUniformF(l, brgl::T_MAT2, v, 4);
  }
  void UniformMatrix3fv(GLint l, GLsizei, GLboolean, const GLfloat* v)
      override {
    SetUniformF(l, brgl::T_MAT3, v, 9);
  }
  void UniformMatrix4fv(GLint l, GLsizei, GLboolean, const GLfloat* v)
      override {
    SetUniformF(l, brgl::T_MAT4, v, 16);
  }
  void ClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) override {
    clear_color_[0] = r;
    clear_color_[1] = g;
    clear_color_[2] = b;
    clear_color_[3] = a;
  }
  void Clear(GLbitfield mask) override {
    unsigned char* px;
    int w, h;
    if (!ColorStore(&px, &w, &h))
      return;
    if ((mask & 0x00000100 /* DEPTH_BUFFER_BIT */) != 0) {
      float* d = DepthFor(w, h);
      for (int i = 0; i < w * h; ++i)
        d[i] = clear_depth_;
    }
    if ((mask & 0x00004000 /* COLOR_BUFFER_BIT */) == 0)
      return;
    unsigned char c[4];
    for (int i = 0; i < 4; ++i) {
      float v = clear_color_[i];
      if (v < 0) v = 0;
      if (v > 1) v = 1;
      c[i] = (unsigned char)(v * 255.0f + 0.5f);
    }
    int x0 = 0, y0 = 0, x1 = w, y1 = h;
    if (scissor_on_) {
      x0 = scissor_[0] > 0 ? scissor_[0] : 0;
      y0 = scissor_[1] > 0 ? scissor_[1] : 0;
      x1 = scissor_[0] + scissor_[2] < w ? scissor_[0] + scissor_[2] : w;
      y1 = scissor_[1] + scissor_[3] < h ? scissor_[1] + scissor_[3] : h;
    }
    for (int y = y0; y < y1; ++y) {
      unsigned char* row = px + ((size_t)y * w + x0) * 4;
      for (int x = x0; x < x1; ++x) {
        memcpy(row, c, 4);
        row += 4;
      }
    }
  }
  void ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                  GLenum format, GLenum type, void* pixels) override {
    if (pixels == nullptr || type != 0x1401 /* UNSIGNED_BYTE */)
      return;
    if (format != 0x1908 /* RGBA */)
      return;
    unsigned char* px;
    int w, h;
    if (!ColorStore(&px, &w, &h))
      return;
    unsigned char* out = (unsigned char*)pixels;
    for (GLsizei row = 0; row < height; ++row) {
      for (GLsizei col = 0; col < width; ++col) {
        // GL counts rows from the bottom; row 0 of the store is the top
        int sx = x + col;
        int sy = h - 1 - (y + row);
        unsigned char* dst = out + ((size_t)row * width + col) * 4;
        if (sx < 0 || sx >= w || sy < 0 || sy >= h) {
          memset(dst, 0, 4);
        } else {
          memcpy(dst, px + ((size_t)sy * w + sx) * 4, 4);
        }
      }
    }
  }

  // ---- buffers ----
  void BufferData(GLenum target, GLsizeiptr size, const void* data,
                  GLenum) override {
    GLuint id =
        target == 0x8893 ? bound_element_buf_ : bound_array_buf_;
    if (id == 0)
      return;
    BrBuf& b = buf_[id];
    b.data.assign((size_t)size, 0);
    if (data != nullptr)
      memcpy(b.data.data(), data, (size_t)size);
  }
  void BufferSubData(GLenum target, GLintptr offset, GLsizeiptr size,
                     const void* data) override {
    GLuint id =
        target == 0x8893 ? bound_element_buf_ : bound_array_buf_;
    if (id == 0 || data == nullptr)
      return;
    BrBuf& b = buf_[id];
    if ((size_t)(offset + size) > b.data.size())
      return;
    memcpy(b.data.data() + offset, data, (size_t)size);
  }

  // ---- shaders and programs (bookkeeping only) ----
  GLuint CreateShader(GLenum type) override {
    GLuint id = next_id_++;
    sh_[id].type = type;
    return id;
  }
  void DeleteShader(GLuint shader) override { sh_.erase(shader); }
  void ShaderSource(GLuint shader, GLsizei count, const char* const* str,
                    const GLint* length) override {
    std::string src;
    for (GLsizei i = 0; i < count; ++i) {
      if (str[i] == nullptr)
        continue;
      if (length != nullptr && length[i] >= 0)
        src.append(str[i], (size_t)length[i]);
      else
        src.append(str[i]);
    }
    sh_[shader].src = src;
  }
  void CompileShader(GLuint) override {}
  void GetShaderiv(GLuint shader, GLenum pname, GLint* params) override {
    switch (pname) {
      case 0x8B81: /* COMPILE_STATUS */
        *params = 1;
        break;
      case 0x8B84: /* INFO_LOG_LENGTH */
        *params = 0;
        break;
      case 0x8B88: /* SHADER_SOURCE_LENGTH */
        *params = (GLint)sh_[shader].src.size() + 1;
        break;
      case 0x8B4F: /* SHADER_TYPE */
        *params = (GLint)sh_[shader].type;
        break;
      case 0x8B80: /* DELETE_STATUS */
        *params = 0;
        break;
      default:
        *params = 0;
    }
  }
  void GetShaderInfoLog(GLuint, GLsizei bufsize, GLsizei* length,
                        char* infolog) override {
    if (bufsize > 0 && infolog != nullptr)
      infolog[0] = 0;
    if (length != nullptr)
      *length = 0;
  }
  void GetShaderPrecisionFormat(GLenum, GLenum precisiontype, GLint* range,
                                GLint* precision) override {
    bool is_int = precisiontype == 0x8DF4 /* LOW_INT */ ||
                  precisiontype == 0x8DF5 || precisiontype == 0x8DF6;
    if (range != nullptr) {
      range[0] = is_int ? 31 : 127;
      range[1] = is_int ? 30 : 127;
    }
    if (precision != nullptr)
      *precision = is_int ? 0 : 23;
  }
  GLuint CreateProgram() override {
    GLuint id = next_id_++;
    prog_[id] = BrProgram();
    return id;
  }
  void DeleteProgram(GLuint program) override {
    auto it = prog_.find(program);
    if (it != prog_.end() && it->second.engine != nullptr)
      brgl::Release(it->second.engine);
    prog_.erase(program);
  }
  void AttachShader(GLuint program, GLuint shader) override {
    prog_[program].shaders.push_back(shader);
  }
  void LinkProgram(GLuint program) override {
    BrProgram& p = prog_[program];
    p.attrs.clear();
    p.unis.clear();
    if (p.engine != nullptr) {
      brgl::Release(p.engine);
      p.engine = nullptr;
    }
    std::string vs, fs;
    for (GLuint sid : p.shaders) {
      auto it = sh_.find(sid);
      if (it == sh_.end())
        continue;
      if (it->second.type == 0x8B31 /* VERTEX_SHADER */)
        vs = it->second.src;
      else
        fs = it->second.src;
    }
    std::string log;
    p.engine = brgl::Compile(vs, fs, &log);
    if (p.engine != nullptr) {
      for (const brgl::VarInfo& v : brgl::Attributes(p.engine))
        p.attrs.push_back({v.name, GlTypeOf(v.type), 1});
      for (const brgl::VarInfo& v : brgl::Uniforms(p.engine))
        p.unis.push_back({v.name, GlTypeOf(v.type), 1});
      p.uniform_vals.assign(p.unis.size(), brgl::Val());
      for (size_t i = 0; i < p.unis.size(); ++i)
        p.uniform_vals[i].type = brgl::Uniforms(p.engine)[i].type;
    } else {
      // Outside the interpreter's subset: fall back to declaration parsing
      // so location queries stay coherent; draws stay no-ops.
      printf("browser: brgl: shader outside subset (%s)\n", log.c_str());
      for (GLuint sid : p.shaders) {
        auto it = sh_.find(sid);
        if (it == sh_.end())
          continue;
        if (it->second.type == 0x8B31)
          ParseGlslVars(it->second.src, "attribute", &p.attrs);
        ParseGlslVars(it->second.src, "uniform", &p.unis);
      }
    }
    p.linked = true;
  }
  void UseProgram(GLuint program) override { current_program_ = program; }
  void GetProgramiv(GLuint program, GLenum pname, GLint* params) override {
    BrProgram& p = prog_[program];
    switch (pname) {
      case 0x8B82: /* LINK_STATUS */
        *params = p.linked ? 1 : 0;
        break;
      case 0x8B80: /* DELETE_STATUS */
        *params = 0;
        break;
      case 0x8B83: /* VALIDATE_STATUS */
        *params = 1;
        break;
      case 0x8B84: /* INFO_LOG_LENGTH */
        *params = 0;
        break;
      case 0x8B85: /* ATTACHED_SHADERS */
        *params = (GLint)p.shaders.size();
        break;
      case 0x8B89: /* ACTIVE_ATTRIBUTES */
        *params = (GLint)p.attrs.size();
        break;
      case 0x8B8A: /* ACTIVE_ATTRIBUTE_MAX_LENGTH */
      case 0x8B87: /* ACTIVE_UNIFORM_MAX_LENGTH */ {
        size_t m = 1;
        const std::vector<BrVar>& vars =
            pname == 0x8B8A ? p.attrs : p.unis;
        for (const BrVar& v : vars)
          if (v.name.size() + 1 > m)
            m = v.name.size() + 1;
        *params = (GLint)m;
        break;
      }
      case 0x8B86: /* ACTIVE_UNIFORMS */
        *params = (GLint)p.unis.size();
        break;
      default:
        *params = 0;
    }
  }
  void GetProgramInfoLog(GLuint, GLsizei bufsize, GLsizei* length,
                         char* infolog) override {
    if (bufsize > 0 && infolog != nullptr)
      infolog[0] = 0;
    if (length != nullptr)
      *length = 0;
  }
  GLint GetAttribLocation(GLuint program, const char* name) override {
    BrProgram& p = prog_[program];
    for (size_t i = 0; i < p.attrs.size(); ++i)
      if (p.attrs[i].name == name)
        return (GLint)i;
    return -1;
  }
  GLint GetUniformLocation(GLuint program, const char* name) override {
    BrProgram& p = prog_[program];
    std::string base(name);
    size_t br = base.find('[');
    if (br != std::string::npos)
      base = base.substr(0, br);
    for (size_t i = 0; i < p.unis.size(); ++i)
      if (p.unis[i].name == base)
        return (GLint)i;
    return -1;
  }
  void GetActiveAttrib(GLuint program, GLuint index, GLsizei bufsize,
                       GLsizei* length, GLint* size, GLenum* type,
                       char* name) override {
    BrProgram& p = prog_[program];
    if (index >= p.attrs.size())
      return;
    const BrVar& v = p.attrs[index];
    if (size != nullptr)
      *size = v.size;
    if (type != nullptr)
      *type = v.type;
    GLsizei n = (GLsizei)v.name.size();
    if (bufsize > 0 && name != nullptr) {
      if (n > bufsize - 1)
        n = bufsize - 1;
      memcpy(name, v.name.data(), (size_t)n);
      name[n] = 0;
    }
    if (length != nullptr)
      *length = n;
  }
  void GetActiveUniform(GLuint program, GLuint index, GLsizei bufsize,
                        GLsizei* length, GLint* size, GLenum* type,
                        char* name) override {
    BrProgram& p = prog_[program];
    if (index >= p.unis.size())
      return;
    const BrVar& v = p.unis[index];
    if (size != nullptr)
      *size = v.size;
    if (type != nullptr)
      *type = v.type;
    GLsizei n = (GLsizei)v.name.size();
    if (bufsize > 0 && name != nullptr) {
      if (n > bufsize - 1)
        n = bufsize - 1;
      memcpy(name, v.name.data(), (size_t)n);
      name[n] = 0;
    }
    if (length != nullptr)
      *length = n;
  }

  // ---- draws: run the interpreted pipeline ----
  void RunDraw(GLenum mode, GLint first, GLsizei count,
               const unsigned char* indices, GLenum index_type) {
    ++draw_calls_;
    if (current_program_ == 0)
      return;
    BrProgram& p = prog_[current_program_];
    if (p.engine == nullptr)
      return;
    brgl::DrawTarget tgt;
    if (!ColorStore(&tgt.color, &tgt.w, &tgt.h))
      return;
    if (depth_test_)
      tgt.depth = DepthFor(tgt.w, tgt.h);
    brgl::DrawParams dp;
    dp.mode = (int)mode;
    dp.first = first;
    dp.count = count;
    dp.indices = indices;
    dp.index_gltype = (int)index_type;
    // attribute slots follow the engine's attribute table order, which is
    // also what GetAttribLocation reported to the page
    std::vector<brgl::AttribStream> streams(p.attrs.size());
    for (size_t i = 0; i < p.attrs.size() && i < 16; ++i) {
      const BrVtxAttrib& a = va_[i];
      brgl::AttribStream& s = streams[i];
      s.enabled = a.enabled;
      s.size = a.size;
      s.gltype = (int)a.gltype;
      s.normalized = a.normalized;
      s.stride = a.stride;
      memcpy(s.const_val, a.const_val, sizeof(s.const_val));
      if (a.enabled) {
        auto it = buf_.find(a.buffer);
        if (it == buf_.end() || a.offset >= it->second.data.size()) {
          s.enabled = false;
        } else {
          s.data = it->second.data.data() + a.offset;
        }
      }
    }
    dp.attribs = streams.data();
    dp.attrib_count = (int)streams.size();
    dp.uniforms = p.uniform_vals.data();
    dp.uniform_count = (int)p.uniform_vals.size();
    for (int unit = 0; unit < 8; ++unit) {
      GLuint tid = bound_tex2d_[unit];
      // sampler uniforms carry the unit; resolve every unit that has a
      // texture with storage
      if (tid == 0)
        continue;
      auto it = tex_.find(tid);
      if (it == tex_.end() || it->second.px.empty())
        continue;
      brgl::TexBinding& tb = dp.textures[unit];
      tb.px = it->second.px.data();
      tb.w = it->second.w;
      tb.h = it->second.h;
      tb.linear = it->second.mag_filter != 0x2600 /* NEAREST */;
      tb.repeat_s = it->second.wrap_s == 0x2901;
      tb.repeat_t = it->second.wrap_t == 0x2901;
    }
    memcpy(dp.viewport, viewport_, sizeof(dp.viewport));
    dp.scissor = scissor_on_;
    memcpy(dp.scissor_box, scissor_, sizeof(dp.scissor_box));
    dp.depth_test = depth_test_;
    dp.depth_func = (int)depth_func_;
    dp.depth_write = depth_mask_;
    dp.blend = blend_on_;
    dp.blend_src = (int)blend_src_;
    dp.blend_dst = (int)blend_dst_;
    dp.cull = cull_on_;
    dp.cull_face = (int)cull_face_;
    dp.front_ccw = front_ccw_;
    brgl::Draw(p.engine, tgt, dp);
  }
  void DrawArrays(GLenum mode, GLint first, GLsizei count) override {
    RunDraw(mode, first, count, nullptr, 0);
  }
  void DrawElements(GLenum mode, GLsizei count, GLenum type,
                    const void* indices) override {
    auto it = buf_.find(bound_element_buf_);
    if (it == buf_.end()) {
      ++draw_calls_;
      return;
    }
    uintptr_t off = (uintptr_t)indices;
    if (off >= it->second.data.size()) {
      ++draw_calls_;
      return;
    }
    RunDraw(mode, 0, count, it->second.data.data() + off, type);
  }

  // ---- queries ----
  GLenum GetError() override {
    GLenum e = error_;
    error_ = 0;
    return e;
  }
  const GLubyte* GetString(GLenum name) override {
    switch (name) {
      case 0x1F00: /* VENDOR */
        return (const GLubyte*)"TessronOS";
      case 0x1F01: /* RENDERER */
        return (const GLubyte*)"TessronOS software GL";
      case 0x1F02: /* VERSION */
        return (const GLubyte*)"OpenGL ES 2.0 (TessronOS)";
      case 0x8B8C: /* SHADING_LANGUAGE_VERSION */
        return (const GLubyte*)"OpenGL ES GLSL ES 1.0 (TessronOS)";
      case 0x1F03: /* EXTENSIONS */
        return (const GLubyte*)
            "GL_OES_packed_depth_stencil GL_OES_rgb8_rgba8 GL_OES_depth24";
      default:
        return (const GLubyte*)"";
    }
  }
  void GetIntegerv(GLenum pname, GLint* params) override {
    switch (pname) {
      case 0x0D33: /* MAX_TEXTURE_SIZE */
      case 0x851C: /* MAX_CUBE_MAP_TEXTURE_SIZE */
      case 0x84E8: /* MAX_RENDERBUFFER_SIZE */
        *params = 2048;
        break;
      case 0x0D3A: /* MAX_VIEWPORT_DIMS */
        params[0] = 2048;
        params[1] = 2048;
        break;
      case 0x8869: /* MAX_VERTEX_ATTRIBS */
        *params = 16;
        break;
      case 0x8DFB: /* MAX_VERTEX_UNIFORM_VECTORS */
        *params = 256;
        break;
      case 0x8DFD: /* MAX_FRAGMENT_UNIFORM_VECTORS */
        *params = 224;
        break;
      case 0x8DFC: /* MAX_VARYING_VECTORS */
        *params = 15;
        break;
      case 0x8872: /* MAX_TEXTURE_IMAGE_UNITS */
        *params = 16;
        break;
      case 0x8B4C: /* MAX_VERTEX_TEXTURE_IMAGE_UNITS */
        *params = 16;
        break;
      case 0x8B4D: /* MAX_COMBINED_TEXTURE_IMAGE_UNITS */
        *params = 32;
        break;
      case 0x0D50: /* SUBPIXEL_BITS */
        *params = 4;
        break;
      case 0x0D52: /* RED_BITS */
      case 0x0D53: /* GREEN_BITS */
      case 0x0D54: /* BLUE_BITS */
      case 0x0D55: /* ALPHA_BITS */
        *params = 8;
        break;
      case 0x0D56: /* DEPTH_BITS */
        *params = 24;
        break;
      case 0x0D57: /* STENCIL_BITS */
        *params = 8;
        break;
      case 0x80A8: /* SAMPLE_BUFFERS */
      case 0x80A9: /* SAMPLES */
        *params = 0;
        break;
      case 0x8B9A: /* IMPLEMENTATION_COLOR_READ_TYPE */
        *params = 0x1401; /* UNSIGNED_BYTE */
        break;
      case 0x8B9B: /* IMPLEMENTATION_COLOR_READ_FORMAT */
        *params = 0x1908; /* RGBA */
        break;
      case 0x86A2: /* NUM_COMPRESSED_TEXTURE_FORMATS */
      case 0x8DF9: /* NUM_SHADER_BINARY_FORMATS */
        *params = 0;
        break;
      case 0x0D05: /* PACK_ALIGNMENT */
        *params = pack_alignment_;
        break;
      case 0x0CF5: /* UNPACK_ALIGNMENT */
        *params = unpack_alignment_;
        break;
      case 0x0BA2: /* VIEWPORT */
        memcpy(params, viewport_, sizeof(viewport_));
        break;
      case 0x0C10: /* SCISSOR_BOX */
        memcpy(params, scissor_, sizeof(scissor_));
        break;
      case 0x8CA6: /* FRAMEBUFFER_BINDING */
        *params = (GLint)bound_fb_;
        break;
      case 0x8CA7: /* RENDERBUFFER_BINDING */
        *params = (GLint)bound_rb_;
        break;
      case 0x8894: /* ARRAY_BUFFER_BINDING */
        *params = (GLint)bound_array_buf_;
        break;
      case 0x8895: /* ELEMENT_ARRAY_BUFFER_BINDING */
        *params = (GLint)bound_element_buf_;
        break;
      case 0x8069: /* TEXTURE_BINDING_2D */
        *params = (GLint)bound_tex2d_[active_unit_];
        break;
      case 0x84E0: /* ACTIVE_TEXTURE */
        *params = 0x84C0 + active_unit_;
        break;
      case 0x8B8D: /* CURRENT_PROGRAM */
        *params = (GLint)current_program_;
        break;
      default:
        *params = 0;
    }
  }
  void GetFloatv(GLenum pname, GLfloat* params) override {
    switch (pname) {
      case 0x846D: /* ALIASED_POINT_SIZE_RANGE */
        params[0] = 1;
        params[1] = 1024;
        break;
      case 0x846E: /* ALIASED_LINE_WIDTH_RANGE */
        params[0] = 1;
        params[1] = 1;
        break;
      case 0x0B70: /* DEPTH_RANGE */
        params[0] = 0;
        params[1] = 1;
        break;
      case 0x0C22: /* COLOR_CLEAR_VALUE */
        memcpy(params, clear_color_, sizeof(clear_color_));
        break;
      default:
        *params = 0;
    }
  }
  void PixelStorei(GLenum pname, GLint param) override {
    if (pname == 0x0D05)
      pack_alignment_ = param;
    else if (pname == 0x0CF5)
      unpack_alignment_ = param;
  }

  // ---- shared image hand-off: the mailbox was minted by the provider's
  // SharedImageInterface with a size; realize it here as a texture with
  // real storage so DrawingBuffer's FBO has bytes behind it. ----
  GLuint CreateAndTexStorage2DSharedImageCHROMIUM(
      const GLbyte* mailbox) override {
    static int calls = 0;
    if ((++calls & 63) == 1)
      printf("browser: brgl: shared-image tex #%d (textures=%d)\n", calls,
              (int)tex_.size());
    std::string key((const char*)mailbox, 16);
    GLuint id = next_id_++;
    BrTex& t = tex_[id];
    auto it = mailboxes_.find(key);
    if (it != mailboxes_.end()) {
      t.w = it->second.first;
      t.h = it->second.second;
    } else {
      t.w = 1;
      t.h = 1;
    }
    t.px.assign((size_t)t.w * t.h * 4, 0);
    return id;
  }
};

}  // namespace



namespace {

class BrSharedImageInterface final : public gpu::SharedImageInterface {
 public:
  explicit BrSharedImageInterface(BrGLES2* gl) : gl_(gl) {}

  // Every creation lands here: mint a ClientSharedImage, and remember the
  // mailbox -> size mapping that CreateAndTexStorage2DSharedImageCHROMIUM
  // later resolves against.
  scoped_refptr<gpu::ClientSharedImage> Mint(const gpu::SharedImageInfo& si) {
    scoped_refptr<gpu::ClientSharedImage> img =
        gpu::ClientSharedImage::CreateForTesting(
            static_cast<const gpu::SharedImageMetadata&>(si), GL_TEXTURE_2D);
    gl_->mailboxes_[std::string((const char*)img->mailbox().name, 16)] = {
        si.size.width(), si.size.height()};
    return img;
  }

  scoped_refptr<gpu::ClientSharedImage> CreateSharedImage(
      const gpu::SharedImageInfo& si_info,
      gpu::SurfaceHandle,
      std::optional<gpu::SharedImagePoolId>) override {
    return Mint(si_info);
  }
  scoped_refptr<gpu::ClientSharedImage> CreateSharedImage(
      const gpu::SharedImageInfo& si_info,
      base::span<const uint8_t>) override {
    return Mint(si_info);
  }
  scoped_refptr<gpu::ClientSharedImage> CreateSharedImage(
      const gpu::SharedImageInfo& si_info,
      gpu::SurfaceHandle,
      gfx::BufferUsage,
      gfx::GpuMemoryBufferHandle) override {
    return Mint(si_info);
  }
  scoped_refptr<gpu::ClientSharedImage> CreateSharedImage(
      const gpu::SharedImageInfo& si_info,
      gfx::GpuMemoryBufferHandle) override {
    return Mint(si_info);
  }
  scoped_refptr<gpu::ClientSharedImage> CreateSharedImageForMLTensor(
      std::string,
      viz::SharedImageFormat,
      const gfx::Size&,
      gpu::SharedImageUsageSet) override {
    return nullptr;
  }
  scoped_refptr<gpu::ClientSharedImage> CreateSharedImageForSoftwareCompositor(
      const gpu::SharedImageInfo& si_info) override {
    return Mint(si_info);
  }
  void UpdateSharedImage(const gpu::SyncToken&, const gpu::Mailbox&) override {
  }
  void UpdateSharedImage(const gpu::SyncToken&,
                         std::unique_ptr<gfx::GpuFence>,
                         const gpu::Mailbox&) override {}
  void DestroySharedImage(const gpu::SyncToken&,
                          const gpu::Mailbox& mailbox) override {
    gl_->mailboxes_.erase(std::string((const char*)mailbox.name, 16));
  }
  void DestroySharedImage(
      const gpu::SyncToken&,
      scoped_refptr<gpu::ClientSharedImage> image) override {
    if (image) {
      gl_->mailboxes_.erase(
          std::string((const char*)image->mailbox().name, 16));
    }
  }
  scoped_refptr<gpu::ClientSharedImage> ImportSharedImage(
      gpu::ExportedSharedImage) override {
    return nullptr;
  }
  gpu::SyncToken GenUnverifiedSyncToken() override { return gpu::SyncToken(); }
  gpu::SyncToken GenVerifiedSyncToken() override { return gpu::SyncToken(); }
  void VerifySyncToken(gpu::SyncToken&) override {}
  // Every command completes synchronously on this thread, so any token can be
  // marked verified and a flush has nothing left to push.
  bool CanVerifySyncToken(const gpu::SyncToken&) override { return true; }
  void VerifyFlush() override {}
  void WaitSyncToken(const gpu::SyncToken&) override {}
  const gpu::SharedImageCapabilities& GetCapabilities() override {
    return shared_image_caps_;
  }

 private:
  BrGLES2* gl_;
  gpu::SharedImageCapabilities shared_image_caps_;
};


class BrGLProvider final : public blink::WebGraphicsContext3DProvider {
 public:
  BrGLProvider() : sii_(&gl_) {
    caps_.max_texture_size = 2048;
    prefs_.anti_aliasing_mode = blink::kAntialiasingModeNone;
  }

  gpu::InterfaceBase* InterfaceBase() override { return &gl_; }
  gpu::gles2::GLES2Interface* ContextGL() override { return &gl_; }
  gpu::raster::RasterInterface* RasterInterface() override { return nullptr; }
  gpu::ContextSupport* ContextSupport() override { return nullptr; }
  gpu::webgpu::WebGPUInterface* WebGPUInterface() override { return nullptr; }
  bool BindToCurrentSequence() override { return true; }
  const gpu::Capabilities& GetCapabilities() const override { return caps_; }
  const gpu::GpuFeatureInfo& GetGpuFeatureInfo() const override {
    return feat_;
  }
  const blink::WebglPreferences& GetWebglPreferences() const override {
    return prefs_;
  }
  // Rasterization runs on the CPU here, so there is no raster context.
  viz::RasterContextProvider* RasterContextProvider() const override {
    return nullptr;
  }
  bool IsContextLost() override { return false; }
  void SetLostContextCallback(base::RepeatingClosure) override {}
  void SetErrorMessageCallback(
      base::RepeatingCallback<void(const char*, int32_t)>) override {}
  cc::ImageDecodeCache* ImageDecodeCache(SkColorType) override {
    return nullptr;
  }
  gpu::SharedImageInterface* SharedImageInterface() override { return &sii_; }

 private:
  BrGLES2 gl_;
  BrSharedImageInterface sii_;
  gpu::Capabilities caps_;
  gpu::GpuFeatureInfo feat_;
  blink::WebglPreferences prefs_;
};

}  // namespace

std::unique_ptr<blink::WebGraphicsContext3DProvider> BrCreateGLProvider(
    blink::Platform::WebGLContextInfo* info) {
  if (info != nullptr) {
    info->vendor_id = 0;
    info->device_id = 0;
    info->renderer_info = blink::WebString::FromUtf8("TessronOS software GL");
    info->vendor_info = blink::WebString::FromUtf8("TessronOS");
    info->driver_version = blink::WebString::FromUtf8("1.0");
  }
  printf("browser: bv: gl provider created\n");
  return std::make_unique<BrGLProvider>();
}
