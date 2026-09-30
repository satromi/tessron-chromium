/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_glsl.h
 *	ブラウザ: a GLSL ES 1.00 pipeline on the CPU for WebGL (design 17.19):
 *	a vertex and fragment shader pair compiled into an interpretable form,
 *	and draw calls rasterized against a plain RGBA8 target. The subset
 *	covers what hand-written WebGL 1.0 shaders use; a shader outside it
 *	fails to compile and draws nothing.
 */

#ifndef BR_GLSL_H_
#define BR_GLSL_H_

#include <string>
#include <vector>

namespace brgl {

enum ValType : unsigned char {
  T_VOID, T_FLOAT, T_INT, T_BOOL,
  T_VEC2, T_VEC3, T_VEC4,
  T_MAT2, T_MAT3, T_MAT4,
  T_SAMPLER2D, T_SAMPLERCUBE
};

inline int Components(ValType t) {
  switch (t) {
    case T_FLOAT: case T_INT: case T_BOOL:
    case T_SAMPLER2D: case T_SAMPLERCUBE: return 1;
    case T_VEC2: return 2;
    case T_VEC3: return 3;
    case T_VEC4: return 4;
    case T_MAT2: return 4;
    case T_MAT3: return 9;
    case T_MAT4: return 16;
    default: return 0;
  }
}

struct Val {
  ValType type = T_FLOAT;
  float f[16] = {0};
};

struct VarInfo {
  std::string name;
  ValType type = T_FLOAT;
};

class Program;

// Parses and links both stages; null with |log| filled on failure.
Program* Compile(const std::string& vs_src, const std::string& fs_src,
                 std::string* log);
void Release(Program* p);

const std::vector<VarInfo>& Attributes(const Program* p);
const std::vector<VarInfo>& Uniforms(const Program* p);

struct TexBinding {
  const unsigned char* px = nullptr;  // RGBA8
  int w = 0;
  int h = 0;
  bool linear = true;
  bool repeat_s = true;
  bool repeat_t = true;
};

struct AttribStream {
  bool enabled = false;
  const unsigned char* data = nullptr;  // resolved buffer pointer + offset
  int size = 4;                         // components 1..4
  int gltype = 0x1406;                  // GL_FLOAT / GL_UNSIGNED_BYTE / ...
  bool normalized = false;
  int stride = 0;                       // bytes; 0 = tight
  float const_val[4] = {0, 0, 0, 1};    // used when not enabled
};

struct DrawTarget {
  unsigned char* color = nullptr;  // RGBA8, row 0 at top
  int w = 0;
  int h = 0;
  float* depth = nullptr;          // optional, w*h
};

struct DrawParams {
  int mode = 0x0004;  // GL_TRIANGLES
  int first = 0;
  int count = 0;
  const unsigned char* indices = nullptr;  // null = sequential
  int index_gltype = 0;                    // UNSIGNED_BYTE/SHORT
  const AttribStream* attribs = nullptr;   // indexed by program attrib slot
  int attrib_count = 0;
  const Val* uniforms = nullptr;           // indexed by program uniform slot
  int uniform_count = 0;
  TexBinding textures[8];
  int viewport[4] = {0, 0, 0, 0};
  bool scissor = false;
  int scissor_box[4] = {0, 0, 0, 0};
  bool depth_test = false;
  int depth_func = 0x0201;  // GL_LESS
  bool depth_write = true;
  bool blend = false;
  int blend_src = 1;  // GL_ONE
  int blend_dst = 0;  // GL_ZERO
  bool cull = false;
  int cull_face = 0x0405;  // GL_BACK
  bool front_ccw = true;
};

bool Draw(Program* p, const DrawTarget& target, const DrawParams& dp);

}  // namespace brgl

#endif  // BR_GLSL_H_
