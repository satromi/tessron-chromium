// made by tools/browser/gen_gl_forward.py from gles2_interface_autogen.h and
// ANGLE's entry_points_gles_*_autogen.h: 283 forwarded, 27 without an entry point
void ActiveTexture(GLenum texture) override { Timed t_(this, kCallOther); GL_ActiveTexture(texture); }
void AttachShader(GLuint program, GLuint shader) override { Timed t_(this, kCallOther); GL_AttachShader(program, shader); }
void BindAttribLocation(GLuint program, GLuint index, const char* name) override { Timed t_(this, kCallOther); GL_BindAttribLocation(program, index, name); }
void BindBuffer(GLenum target, GLuint buffer) override { Timed t_(this, kCallOther); GL_BindBuffer(target, buffer); }
void BindBufferBase(GLenum target, GLuint index, GLuint buffer) override { Timed t_(this, kCallOther); GL_BindBufferBase(target, index, buffer); }
void BindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size) override { Timed t_(this, kCallOther); GL_BindBufferRange(target, index, buffer, offset, size); }
void BindFramebuffer(GLenum target, GLuint framebuffer) override { Timed t_(this, kCallOther); GL_BindFramebuffer(target, framebuffer); }
void BindRenderbuffer(GLenum target, GLuint renderbuffer) override { Timed t_(this, kCallOther); GL_BindRenderbuffer(target, renderbuffer); }
void BindSampler(GLuint unit, GLuint sampler) override { Timed t_(this, kCallOther); GL_BindSampler(unit, sampler); }
void BindTexture(GLenum target, GLuint texture) override { Timed t_(this, kCallOther); GL_BindTexture(target, texture); }
void BindTransformFeedback(GLenum target, GLuint transformfeedback) override { Timed t_(this, kCallOther); GL_BindTransformFeedback(target, transformfeedback); }
void BlendColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha) override { Timed t_(this, kCallOther); GL_BlendColor(red, green, blue, alpha); }
void BlendEquation(GLenum mode) override { Timed t_(this, kCallOther); GL_BlendEquation(mode); }
void BlendEquationSeparate(GLenum modeRGB, GLenum modeAlpha) override { Timed t_(this, kCallOther); GL_BlendEquationSeparate(modeRGB, modeAlpha); }
void BlendFunc(GLenum sfactor, GLenum dfactor) override { Timed t_(this, kCallOther); GL_BlendFunc(sfactor, dfactor); }
void BlendFuncSeparate(GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha) override { Timed t_(this, kCallOther); GL_BlendFuncSeparate(srcRGB, dstRGB, srcAlpha, dstAlpha); }
void BufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage) override { Timed t_(this, kCallOther); GL_BufferData(target, size, data, usage); }
void BufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) override { Timed t_(this, kCallOther); GL_BufferSubData(target, offset, size, data); }
GLenum CheckFramebufferStatus(GLenum target) override { Timed t_(this, kCallOther); return GL_CheckFramebufferStatus(target); }
void Clear(GLbitfield mask) override { Timed t_(this, kCallClear); GL_Clear(mask); }
void ClearBufferfi(GLenum buffer, GLint drawbuffers, GLfloat depth, GLint stencil) override { Timed t_(this, kCallClear); GL_ClearBufferfi(buffer, drawbuffers, depth, stencil); }
void ClearBufferfv(GLenum buffer, GLint drawbuffers, const GLfloat* value) override { Timed t_(this, kCallClear); GL_ClearBufferfv(buffer, drawbuffers, value); }
void ClearBufferiv(GLenum buffer, GLint drawbuffers, const GLint* value) override { Timed t_(this, kCallClear); GL_ClearBufferiv(buffer, drawbuffers, value); }
void ClearBufferuiv(GLenum buffer, GLint drawbuffers, const GLuint* value) override { Timed t_(this, kCallClear); GL_ClearBufferuiv(buffer, drawbuffers, value); }
void ClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha) override { Timed t_(this, kCallClear); GL_ClearColor(red, green, blue, alpha); }
void ClearDepthf(GLclampf depth) override { Timed t_(this, kCallClear); GL_ClearDepthf(depth); }
void ClearStencil(GLint s) override { Timed t_(this, kCallClear); GL_ClearStencil(s); }
GLenum ClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout) override { Timed t_(this, kCallOther); return GL_ClientWaitSync(sync, flags, timeout); }
void ColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha) override { Timed t_(this, kCallOther); GL_ColorMask(red, green, blue, alpha); }
void CompileShader(GLuint shader) override { Timed t_(this, kCallOther); GL_CompileShader(shader); }
void CompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void* data) override { Timed t_(this, kCallOther); GL_CompressedTexImage2D(target, level, internalformat, width, height, border, imageSize, data); }
void CompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, const void* data) override { Timed t_(this, kCallOther); GL_CompressedTexSubImage2D(target, level, xoffset, yoffset, width, height, format, imageSize, data); }
void CompressedTexImage3D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLsizei imageSize, const void* data) override { Timed t_(this, kCallOther); GL_CompressedTexImage3D(target, level, internalformat, width, height, depth, border, imageSize, data); }
void CompressedTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLsizei imageSize, const void* data) override { Timed t_(this, kCallOther); GL_CompressedTexSubImage3D(target, level, xoffset, yoffset, zoffset, width, height, depth, format, imageSize, data); }
void CopyBufferSubData(GLenum readtarget, GLenum writetarget, GLintptr readoffset, GLintptr writeoffset, GLsizeiptr size) override { Timed t_(this, kCallOther); GL_CopyBufferSubData(readtarget, writetarget, readoffset, writeoffset, size); }
void CopyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border) override { Timed t_(this, kCallOther); GL_CopyTexImage2D(target, level, internalformat, x, y, width, height, border); }
void CopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height) override { Timed t_(this, kCallOther); GL_CopyTexSubImage2D(target, level, xoffset, yoffset, x, y, width, height); }
void CopyTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLint x, GLint y, GLsizei width, GLsizei height) override { Timed t_(this, kCallOther); GL_CopyTexSubImage3D(target, level, xoffset, yoffset, zoffset, x, y, width, height); }
GLuint CreateProgram() override { Timed t_(this, kCallOther); return GL_CreateProgram(); }
GLuint CreateShader(GLenum type) override { Timed t_(this, kCallOther); return GL_CreateShader(type); }
void CullFace(GLenum mode) override { Timed t_(this, kCallOther); GL_CullFace(mode); }
void DeleteBuffers(GLsizei n, const GLuint* buffers) override { Timed t_(this, kCallOther); GL_DeleteBuffers(n, buffers); }
void DeleteFramebuffers(GLsizei n, const GLuint* framebuffers) override { Timed t_(this, kCallOther); GL_DeleteFramebuffers(n, framebuffers); }
void DeleteProgram(GLuint program) override { Timed t_(this, kCallOther); GL_DeleteProgram(program); }
void DeleteRenderbuffers(GLsizei n, const GLuint* renderbuffers) override { Timed t_(this, kCallOther); GL_DeleteRenderbuffers(n, renderbuffers); }
void DeleteSamplers(GLsizei n, const GLuint* samplers) override { Timed t_(this, kCallOther); GL_DeleteSamplers(n, samplers); }
void DeleteSync(GLsync sync) override { Timed t_(this, kCallOther); GL_DeleteSync(sync); }
void DeleteShader(GLuint shader) override { Timed t_(this, kCallOther); GL_DeleteShader(shader); }
void DeleteTransformFeedbacks(GLsizei n, const GLuint* ids) override { Timed t_(this, kCallOther); GL_DeleteTransformFeedbacks(n, ids); }
void DepthFunc(GLenum func) override { Timed t_(this, kCallOther); GL_DepthFunc(func); }
void DepthMask(GLboolean flag) override { Timed t_(this, kCallOther); GL_DepthMask(flag); }
void DepthRangef(GLclampf zNear, GLclampf zFar) override { Timed t_(this, kCallOther); GL_DepthRangef(zNear, zFar); }
void DetachShader(GLuint program, GLuint shader) override { Timed t_(this, kCallOther); GL_DetachShader(program, shader); }
void Disable(GLenum cap) override { Timed t_(this, kCallOther); GL_Disable(cap); }
void DisableVertexAttribArray(GLuint index) override { Timed t_(this, kCallOther); GL_DisableVertexAttribArray(index); }
void DrawArrays(GLenum mode, GLint first, GLsizei count) override { Timed t_(this, kCallDraw); GL_DrawArrays(mode, first, count); }
void DrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) override { Timed t_(this, kCallDraw); GL_DrawElements(mode, count, type, indices); }
void DrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices) override { Timed t_(this, kCallDraw); GL_DrawRangeElements(mode, start, end, count, type, indices); }
void Enable(GLenum cap) override { Timed t_(this, kCallOther); GL_Enable(cap); }
void EnableVertexAttribArray(GLuint index) override { Timed t_(this, kCallOther); GL_EnableVertexAttribArray(index); }
GLsync FenceSync(GLenum condition, GLbitfield flags) override { Timed t_(this, kCallOther); return GL_FenceSync(condition, flags); }
void FramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer) override { Timed t_(this, kCallOther); GL_FramebufferRenderbuffer(target, attachment, renderbuffertarget, renderbuffer); }
void FramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) override { Timed t_(this, kCallOther); GL_FramebufferTexture2D(target, attachment, textarget, texture, level); }
void FramebufferTextureLayer(GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer) override { Timed t_(this, kCallOther); GL_FramebufferTextureLayer(target, attachment, texture, level, layer); }
void FrontFace(GLenum mode) override { Timed t_(this, kCallOther); GL_FrontFace(mode); }
void GenBuffers(GLsizei n, GLuint* buffers) override { Timed t_(this, kCallOther); GL_GenBuffers(n, buffers); }
void GenerateMipmap(GLenum target) override { Timed t_(this, kCallOther); GL_GenerateMipmap(target); }
void GenFramebuffers(GLsizei n, GLuint* framebuffers) override { Timed t_(this, kCallOther); GL_GenFramebuffers(n, framebuffers); }
void GenRenderbuffers(GLsizei n, GLuint* renderbuffers) override { Timed t_(this, kCallOther); GL_GenRenderbuffers(n, renderbuffers); }
void GenSamplers(GLsizei n, GLuint* samplers) override { Timed t_(this, kCallOther); GL_GenSamplers(n, samplers); }
void GenTextures(GLsizei n, GLuint* textures) override { Timed t_(this, kCallOther); GL_GenTextures(n, textures); }
void GenTransformFeedbacks(GLsizei n, GLuint* ids) override { Timed t_(this, kCallOther); GL_GenTransformFeedbacks(n, ids); }
void GetActiveAttrib(GLuint program, GLuint index, GLsizei bufsize, GLsizei* length, GLint* size, GLenum* type, char* name) override { Timed t_(this, kCallOther); GL_GetActiveAttrib(program, index, bufsize, length, size, type, name); }
void GetActiveUniform(GLuint program, GLuint index, GLsizei bufsize, GLsizei* length, GLint* size, GLenum* type, char* name) override { Timed t_(this, kCallOther); GL_GetActiveUniform(program, index, bufsize, length, size, type, name); }
void GetActiveUniformBlockiv(GLuint program, GLuint index, GLenum pname, GLint* params) override { Timed t_(this, kCallOther); GL_GetActiveUniformBlockiv(program, index, pname, params); }
void GetActiveUniformBlockName(GLuint program, GLuint index, GLsizei bufsize, GLsizei* length, char* name) override { Timed t_(this, kCallOther); GL_GetActiveUniformBlockName(program, index, bufsize, length, name); }
void GetActiveUniformsiv(GLuint program, GLsizei count, const GLuint* indices, GLenum pname, GLint* params) override { Timed t_(this, kCallOther); GL_GetActiveUniformsiv(program, count, indices, pname, params); }
void GetAttachedShaders(GLuint program, GLsizei maxcount, GLsizei* count, GLuint* shaders) override { Timed t_(this, kCallOther); GL_GetAttachedShaders(program, maxcount, count, shaders); }
GLint GetAttribLocation(GLuint program, const char* name) override { Timed t_(this, kCallOther); return GL_GetAttribLocation(program, name); }
void GetBooleanv(GLenum pname, GLboolean* params) override { Timed t_(this, kCallOther); GL_GetBooleanv(pname, params); }
void GetBufferParameteri64v(GLenum target, GLenum pname, GLint64* params) override { Timed t_(this, kCallOther); GL_GetBufferParameteri64v(target, pname, params); }
void GetBufferParameteriv(GLenum target, GLenum pname, GLint* params) override { Timed t_(this, kCallOther); GL_GetBufferParameteriv(target, pname, params); }
void GetFloatv(GLenum pname, GLfloat* params) override { Timed t_(this, kCallOther); GL_GetFloatv(pname, params); }
GLint GetFragDataLocation(GLuint program, const char* name) override { Timed t_(this, kCallOther); return GL_GetFragDataLocation(program, name); }
void GetFramebufferAttachmentParameteriv(GLenum target, GLenum attachment, GLenum pname, GLint* params) override { Timed t_(this, kCallOther); GL_GetFramebufferAttachmentParameteriv(target, attachment, pname, params); }
void GetInteger64v(GLenum pname, GLint64* params) override { Timed t_(this, kCallOther); GL_GetInteger64v(pname, params); }
void GetIntegeri_v(GLenum pname, GLuint index, GLint* data) override { Timed t_(this, kCallOther); GL_GetIntegeri_v(pname, index, data); }
void GetInteger64i_v(GLenum pname, GLuint index, GLint64* data) override { Timed t_(this, kCallOther); GL_GetInteger64i_v(pname, index, data); }
void GetIntegerv(GLenum pname, GLint* params) override { Timed t_(this, kCallOther); GL_GetIntegerv(pname, params); }
void GetInternalformativ(GLenum target, GLenum format, GLenum pname, GLsizei bufSize, GLint* params) override { Timed t_(this, kCallOther); GL_GetInternalformativ(target, format, pname, bufSize, params); }
void GetProgramiv(GLuint program, GLenum pname, GLint* params) override { Timed t_(this, kCallOther); GL_GetProgramiv(program, pname, params); }
void GetProgramInfoLog(GLuint program, GLsizei bufsize, GLsizei* length, char* infolog) override { Timed t_(this, kCallOther); GL_GetProgramInfoLog(program, bufsize, length, infolog); }
void GetRenderbufferParameteriv(GLenum target, GLenum pname, GLint* params) override { Timed t_(this, kCallOther); GL_GetRenderbufferParameteriv(target, pname, params); }
void GetSamplerParameterfv(GLuint sampler, GLenum pname, GLfloat* params) override { Timed t_(this, kCallOther); GL_GetSamplerParameterfv(sampler, pname, params); }
void GetSamplerParameteriv(GLuint sampler, GLenum pname, GLint* params) override { Timed t_(this, kCallOther); GL_GetSamplerParameteriv(sampler, pname, params); }
void GetShaderiv(GLuint shader, GLenum pname, GLint* params) override { Timed t_(this, kCallOther); GL_GetShaderiv(shader, pname, params); }
void GetShaderInfoLog(GLuint shader, GLsizei bufsize, GLsizei* length, char* infolog) override { Timed t_(this, kCallOther); GL_GetShaderInfoLog(shader, bufsize, length, infolog); }
void GetShaderPrecisionFormat(GLenum shadertype, GLenum precisiontype, GLint* range, GLint* precision) override { Timed t_(this, kCallOther); GL_GetShaderPrecisionFormat(shadertype, precisiontype, range, precision); }
void GetShaderSource(GLuint shader, GLsizei bufsize, GLsizei* length, char* source) override { Timed t_(this, kCallOther); GL_GetShaderSource(shader, bufsize, length, source); }
const GLubyte* GetStringi(GLenum name, GLuint index) override { Timed t_(this, kCallOther); return GL_GetStringi(name, index); }
void GetSynciv(GLsync sync, GLenum pname, GLsizei bufsize, GLsizei* length, GLint* values) override { Timed t_(this, kCallOther); GL_GetSynciv(sync, pname, bufsize, length, values); }
void GetTexParameterfv(GLenum target, GLenum pname, GLfloat* params) override { Timed t_(this, kCallOther); GL_GetTexParameterfv(target, pname, params); }
void GetTexParameteriv(GLenum target, GLenum pname, GLint* params) override { Timed t_(this, kCallOther); GL_GetTexParameteriv(target, pname, params); }
void GetTransformFeedbackVarying(GLuint program, GLuint index, GLsizei bufsize, GLsizei* length, GLsizei* size, GLenum* type, char* name) override { Timed t_(this, kCallOther); GL_GetTransformFeedbackVarying(program, index, bufsize, length, size, type, name); }
GLuint GetUniformBlockIndex(GLuint program, const char* name) override { Timed t_(this, kCallOther); return GL_GetUniformBlockIndex(program, name); }
void GetUniformfv(GLuint program, GLint location, GLfloat* params) override { Timed t_(this, kCallOther); GL_GetUniformfv(program, location, params); }
void GetUniformiv(GLuint program, GLint location, GLint* params) override { Timed t_(this, kCallOther); GL_GetUniformiv(program, location, params); }
void GetUniformuiv(GLuint program, GLint location, GLuint* params) override { Timed t_(this, kCallOther); GL_GetUniformuiv(program, location, params); }
void GetUniformIndices(GLuint program, GLsizei count, const char* const* names, GLuint* indices) override { Timed t_(this, kCallOther); GL_GetUniformIndices(program, count, names, indices); }
GLint GetUniformLocation(GLuint program, const char* name) override { Timed t_(this, kCallOther); return GL_GetUniformLocation(program, name); }
void GetVertexAttribfv(GLuint index, GLenum pname, GLfloat* params) override { Timed t_(this, kCallOther); GL_GetVertexAttribfv(index, pname, params); }
void GetVertexAttribiv(GLuint index, GLenum pname, GLint* params) override { Timed t_(this, kCallOther); GL_GetVertexAttribiv(index, pname, params); }
void GetVertexAttribIiv(GLuint index, GLenum pname, GLint* params) override { Timed t_(this, kCallOther); GL_GetVertexAttribIiv(index, pname, params); }
void GetVertexAttribIuiv(GLuint index, GLenum pname, GLuint* params) override { Timed t_(this, kCallOther); GL_GetVertexAttribIuiv(index, pname, params); }
void GetVertexAttribPointerv(GLuint index, GLenum pname, void** pointer) override { Timed t_(this, kCallOther); GL_GetVertexAttribPointerv(index, pname, pointer); }
void Hint(GLenum target, GLenum mode) override { Timed t_(this, kCallOther); GL_Hint(target, mode); }
void InvalidateFramebuffer(GLenum target, GLsizei count, const GLenum* attachments) override { Timed t_(this, kCallOther); GL_InvalidateFramebuffer(target, count, attachments); }
void InvalidateSubFramebuffer(GLenum target, GLsizei count, const GLenum* attachments, GLint x, GLint y, GLsizei width, GLsizei height) override { Timed t_(this, kCallOther); GL_InvalidateSubFramebuffer(target, count, attachments, x, y, width, height); }
GLboolean IsBuffer(GLuint buffer) override { Timed t_(this, kCallOther); return GL_IsBuffer(buffer); }
GLboolean IsEnabled(GLenum cap) override { Timed t_(this, kCallOther); return GL_IsEnabled(cap); }
GLboolean IsFramebuffer(GLuint framebuffer) override { Timed t_(this, kCallOther); return GL_IsFramebuffer(framebuffer); }
GLboolean IsProgram(GLuint program) override { Timed t_(this, kCallOther); return GL_IsProgram(program); }
GLboolean IsRenderbuffer(GLuint renderbuffer) override { Timed t_(this, kCallOther); return GL_IsRenderbuffer(renderbuffer); }
GLboolean IsSampler(GLuint sampler) override { Timed t_(this, kCallOther); return GL_IsSampler(sampler); }
GLboolean IsShader(GLuint shader) override { Timed t_(this, kCallOther); return GL_IsShader(shader); }
GLboolean IsSync(GLsync sync) override { Timed t_(this, kCallOther); return GL_IsSync(sync); }
GLboolean IsTexture(GLuint texture) override { Timed t_(this, kCallOther); return GL_IsTexture(texture); }
GLboolean IsTransformFeedback(GLuint transformfeedback) override { Timed t_(this, kCallOther); return GL_IsTransformFeedback(transformfeedback); }
void LineWidth(GLfloat width) override { Timed t_(this, kCallOther); GL_LineWidth(width); }
void PauseTransformFeedback() override { Timed t_(this, kCallOther); GL_PauseTransformFeedback(); }
void PixelStorei(GLenum pname, GLint param) override { Timed t_(this, kCallOther); GL_PixelStorei(pname, param); }
void PolygonOffset(GLfloat factor, GLfloat units) override { Timed t_(this, kCallOther); GL_PolygonOffset(factor, units); }
void ReadBuffer(GLenum src) override { Timed t_(this, kCallOther); GL_ReadBuffer(src); }
void ReleaseShaderCompiler() override { Timed t_(this, kCallOther); GL_ReleaseShaderCompiler(); }
void RenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height) override { Timed t_(this, kCallOther); GL_RenderbufferStorage(target, internalformat, width, height); }
void ResumeTransformFeedback() override { Timed t_(this, kCallOther); GL_ResumeTransformFeedback(); }
void SampleCoverage(GLclampf value, GLboolean invert) override { Timed t_(this, kCallOther); GL_SampleCoverage(value, invert); }
void SamplerParameterf(GLuint sampler, GLenum pname, GLfloat param) override { Timed t_(this, kCallOther); GL_SamplerParameterf(sampler, pname, param); }
void SamplerParameterfv(GLuint sampler, GLenum pname, const GLfloat* params) override { Timed t_(this, kCallOther); GL_SamplerParameterfv(sampler, pname, params); }
void SamplerParameteri(GLuint sampler, GLenum pname, GLint param) override { Timed t_(this, kCallOther); GL_SamplerParameteri(sampler, pname, param); }
void SamplerParameteriv(GLuint sampler, GLenum pname, const GLint* params) override { Timed t_(this, kCallOther); GL_SamplerParameteriv(sampler, pname, params); }
void Scissor(GLint x, GLint y, GLsizei width, GLsizei height) override { Timed t_(this, kCallOther); GL_Scissor(x, y, width, height); }
void ShaderBinary(GLsizei n, const GLuint* shaders, GLenum binaryformat, const void* binary, GLsizei length) override { Timed t_(this, kCallOther); GL_ShaderBinary(n, shaders, binaryformat, binary, length); }
void ShaderSource(GLuint shader, GLsizei count, const GLchar* const* str, const GLint* length) override { Timed t_(this, kCallOther); GL_ShaderSource(shader, count, str, length); }
void MultiDrawArraysWEBGL(GLenum mode, const GLint* firsts, const GLsizei* counts, GLsizei drawcount) override { Timed t_(this, kCallDraw); GL_MultiDrawArraysANGLE(mode, firsts, counts, drawcount); }
void MultiDrawArraysInstancedWEBGL(GLenum mode, const GLint* firsts, const GLsizei* counts, const GLsizei* instance_counts, GLsizei drawcount) override { Timed t_(this, kCallDraw); GL_MultiDrawArraysInstancedANGLE(mode, firsts, counts, instance_counts, drawcount); }
void MultiDrawArraysInstancedBaseInstanceWEBGL(GLenum mode, const GLint* firsts, const GLsizei* counts, const GLsizei* instance_counts, const GLuint* baseinstances, GLsizei drawcount) override { Timed t_(this, kCallDraw); GL_MultiDrawArraysInstancedBaseInstanceANGLE(mode, firsts, counts, instance_counts, baseinstances, drawcount); }
void MultiDrawElementsWEBGL(GLenum mode, const GLsizei* counts, GLenum type, const GLsizei* offsets, GLsizei drawcount) override { Timed t_(this, kCallDraw); GL_MultiDrawElementsANGLE(mode, counts, type, Ptrs(offsets, drawcount).data(), drawcount); }
void MultiDrawElementsInstancedWEBGL(GLenum mode, const GLsizei* counts, GLenum type, const GLsizei* offsets, const GLsizei* instance_counts, GLsizei drawcount) override { Timed t_(this, kCallDraw); GL_MultiDrawElementsInstancedANGLE(mode, counts, type, Ptrs(offsets, drawcount).data(), instance_counts, drawcount); }
void MultiDrawElementsInstancedBaseVertexBaseInstanceWEBGL(GLenum mode, const GLsizei* counts, GLenum type, const GLsizei* offsets, const GLsizei* instance_counts, const GLint* basevertices, const GLuint* baseinstances, GLsizei drawcount) override { Timed t_(this, kCallDraw); GL_MultiDrawElementsInstancedBaseVertexBaseInstanceANGLE(mode, counts, type, Ptrs(offsets, drawcount).data(), instance_counts, basevertices, baseinstances, drawcount); }
void StencilFunc(GLenum func, GLint ref, GLuint mask) override { Timed t_(this, kCallOther); GL_StencilFunc(func, ref, mask); }
void StencilFuncSeparate(GLenum face, GLenum func, GLint ref, GLuint mask) override { Timed t_(this, kCallOther); GL_StencilFuncSeparate(face, func, ref, mask); }
void StencilMask(GLuint mask) override { Timed t_(this, kCallOther); GL_StencilMask(mask); }
void StencilMaskSeparate(GLenum face, GLuint mask) override { Timed t_(this, kCallOther); GL_StencilMaskSeparate(face, mask); }
void StencilOp(GLenum fail, GLenum zfail, GLenum zpass) override { Timed t_(this, kCallOther); GL_StencilOp(fail, zfail, zpass); }
void StencilOpSeparate(GLenum face, GLenum fail, GLenum zfail, GLenum zpass) override { Timed t_(this, kCallOther); GL_StencilOpSeparate(face, fail, zfail, zpass); }
void TexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void* pixels) override { Timed t_(this, kCallOther); GL_TexImage2D(target, level, internalformat, width, height, border, format, type, pixels); }
void TexImage3D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type, const void* pixels) override { Timed t_(this, kCallOther); GL_TexImage3D(target, level, internalformat, width, height, depth, border, format, type, pixels); }
void TexParameterf(GLenum target, GLenum pname, GLfloat param) override { Timed t_(this, kCallOther); GL_TexParameterf(target, pname, param); }
void TexParameterfv(GLenum target, GLenum pname, const GLfloat* params) override { Timed t_(this, kCallOther); GL_TexParameterfv(target, pname, params); }
void TexParameteri(GLenum target, GLenum pname, GLint param) override { Timed t_(this, kCallOther); GL_TexParameteri(target, pname, param); }
void TexParameteriv(GLenum target, GLenum pname, const GLint* params) override { Timed t_(this, kCallOther); GL_TexParameteriv(target, pname, params); }
void TexStorage3D(GLenum target, GLsizei levels, GLenum internalFormat, GLsizei width, GLsizei height, GLsizei depth) override { Timed t_(this, kCallOther); GL_TexStorage3D(target, levels, internalFormat, width, height, depth); }
void TexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void* pixels) override { Timed t_(this, kCallOther); GL_TexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels); }
void TexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* pixels) override { Timed t_(this, kCallOther); GL_TexSubImage3D(target, level, xoffset, yoffset, zoffset, width, height, depth, format, type, pixels); }
void TransformFeedbackVaryings(GLuint program, GLsizei count, const char* const* varyings, GLenum buffermode) override { Timed t_(this, kCallOther); GL_TransformFeedbackVaryings(program, count, varyings, buffermode); }
void Uniform1f(GLint location, GLfloat x) override { Timed t_(this, kCallOther); GL_Uniform1f(location, x); }
void Uniform1fv(GLint location, GLsizei count, const GLfloat* v) override { Timed t_(this, kCallOther); GL_Uniform1fv(location, count, v); }
void Uniform1i(GLint location, GLint x) override { Timed t_(this, kCallOther); GL_Uniform1i(location, x); }
void Uniform1iv(GLint location, GLsizei count, const GLint* v) override { Timed t_(this, kCallOther); GL_Uniform1iv(location, count, v); }
void Uniform1ui(GLint location, GLuint x) override { Timed t_(this, kCallOther); GL_Uniform1ui(location, x); }
void Uniform1uiv(GLint location, GLsizei count, const GLuint* v) override { Timed t_(this, kCallOther); GL_Uniform1uiv(location, count, v); }
void Uniform2f(GLint location, GLfloat x, GLfloat y) override { Timed t_(this, kCallOther); GL_Uniform2f(location, x, y); }
void Uniform2fv(GLint location, GLsizei count, const GLfloat* v) override { Timed t_(this, kCallOther); GL_Uniform2fv(location, count, v); }
void Uniform2i(GLint location, GLint x, GLint y) override { Timed t_(this, kCallOther); GL_Uniform2i(location, x, y); }
void Uniform2iv(GLint location, GLsizei count, const GLint* v) override { Timed t_(this, kCallOther); GL_Uniform2iv(location, count, v); }
void Uniform2ui(GLint location, GLuint x, GLuint y) override { Timed t_(this, kCallOther); GL_Uniform2ui(location, x, y); }
void Uniform2uiv(GLint location, GLsizei count, const GLuint* v) override { Timed t_(this, kCallOther); GL_Uniform2uiv(location, count, v); }
void Uniform3f(GLint location, GLfloat x, GLfloat y, GLfloat z) override { Timed t_(this, kCallOther); GL_Uniform3f(location, x, y, z); }
void Uniform3fv(GLint location, GLsizei count, const GLfloat* v) override { Timed t_(this, kCallOther); GL_Uniform3fv(location, count, v); }
void Uniform3i(GLint location, GLint x, GLint y, GLint z) override { Timed t_(this, kCallOther); GL_Uniform3i(location, x, y, z); }
void Uniform3iv(GLint location, GLsizei count, const GLint* v) override { Timed t_(this, kCallOther); GL_Uniform3iv(location, count, v); }
void Uniform3ui(GLint location, GLuint x, GLuint y, GLuint z) override { Timed t_(this, kCallOther); GL_Uniform3ui(location, x, y, z); }
void Uniform3uiv(GLint location, GLsizei count, const GLuint* v) override { Timed t_(this, kCallOther); GL_Uniform3uiv(location, count, v); }
void Uniform4f(GLint location, GLfloat x, GLfloat y, GLfloat z, GLfloat w) override { Timed t_(this, kCallOther); GL_Uniform4f(location, x, y, z, w); }
void Uniform4fv(GLint location, GLsizei count, const GLfloat* v) override { Timed t_(this, kCallOther); GL_Uniform4fv(location, count, v); }
void Uniform4i(GLint location, GLint x, GLint y, GLint z, GLint w) override { Timed t_(this, kCallOther); GL_Uniform4i(location, x, y, z, w); }
void Uniform4iv(GLint location, GLsizei count, const GLint* v) override { Timed t_(this, kCallOther); GL_Uniform4iv(location, count, v); }
void Uniform4ui(GLint location, GLuint x, GLuint y, GLuint z, GLuint w) override { Timed t_(this, kCallOther); GL_Uniform4ui(location, x, y, z, w); }
void Uniform4uiv(GLint location, GLsizei count, const GLuint* v) override { Timed t_(this, kCallOther); GL_Uniform4uiv(location, count, v); }
void UniformBlockBinding(GLuint program, GLuint index, GLuint binding) override { Timed t_(this, kCallOther); GL_UniformBlockBinding(program, index, binding); }
void UniformMatrix2fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) override { Timed t_(this, kCallOther); GL_UniformMatrix2fv(location, count, transpose, value); }
void UniformMatrix2x3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) override { Timed t_(this, kCallOther); GL_UniformMatrix2x3fv(location, count, transpose, value); }
void UniformMatrix2x4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) override { Timed t_(this, kCallOther); GL_UniformMatrix2x4fv(location, count, transpose, value); }
void UniformMatrix3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) override { Timed t_(this, kCallOther); GL_UniformMatrix3fv(location, count, transpose, value); }
void UniformMatrix3x2fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) override { Timed t_(this, kCallOther); GL_UniformMatrix3x2fv(location, count, transpose, value); }
void UniformMatrix3x4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) override { Timed t_(this, kCallOther); GL_UniformMatrix3x4fv(location, count, transpose, value); }
void UniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) override { Timed t_(this, kCallOther); GL_UniformMatrix4fv(location, count, transpose, value); }
void UniformMatrix4x2fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) override { Timed t_(this, kCallOther); GL_UniformMatrix4x2fv(location, count, transpose, value); }
void UniformMatrix4x3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) override { Timed t_(this, kCallOther); GL_UniformMatrix4x3fv(location, count, transpose, value); }
void UseProgram(GLuint program) override { Timed t_(this, kCallOther); GL_UseProgram(program); }
void ValidateProgram(GLuint program) override { Timed t_(this, kCallOther); GL_ValidateProgram(program); }
void VertexAttrib1f(GLuint indx, GLfloat x) override { Timed t_(this, kCallOther); GL_VertexAttrib1f(indx, x); }
void VertexAttrib1fv(GLuint indx, const GLfloat* values) override { Timed t_(this, kCallOther); GL_VertexAttrib1fv(indx, values); }
void VertexAttrib2f(GLuint indx, GLfloat x, GLfloat y) override { Timed t_(this, kCallOther); GL_VertexAttrib2f(indx, x, y); }
void VertexAttrib2fv(GLuint indx, const GLfloat* values) override { Timed t_(this, kCallOther); GL_VertexAttrib2fv(indx, values); }
void VertexAttrib3f(GLuint indx, GLfloat x, GLfloat y, GLfloat z) override { Timed t_(this, kCallOther); GL_VertexAttrib3f(indx, x, y, z); }
void VertexAttrib3fv(GLuint indx, const GLfloat* values) override { Timed t_(this, kCallOther); GL_VertexAttrib3fv(indx, values); }
void VertexAttrib4f(GLuint indx, GLfloat x, GLfloat y, GLfloat z, GLfloat w) override { Timed t_(this, kCallOther); GL_VertexAttrib4f(indx, x, y, z, w); }
void VertexAttrib4fv(GLuint indx, const GLfloat* values) override { Timed t_(this, kCallOther); GL_VertexAttrib4fv(indx, values); }
void VertexAttribI4i(GLuint indx, GLint x, GLint y, GLint z, GLint w) override { Timed t_(this, kCallOther); GL_VertexAttribI4i(indx, x, y, z, w); }
void VertexAttribI4iv(GLuint indx, const GLint* values) override { Timed t_(this, kCallOther); GL_VertexAttribI4iv(indx, values); }
void VertexAttribI4ui(GLuint indx, GLuint x, GLuint y, GLuint z, GLuint w) override { Timed t_(this, kCallOther); GL_VertexAttribI4ui(indx, x, y, z, w); }
void VertexAttribI4uiv(GLuint indx, const GLuint* values) override { Timed t_(this, kCallOther); GL_VertexAttribI4uiv(indx, values); }
void VertexAttribIPointer(GLuint indx, GLint size, GLenum type, GLsizei stride, const void* ptr) override { Timed t_(this, kCallOther); GL_VertexAttribIPointer(indx, size, type, stride, ptr); }
void VertexAttribPointer(GLuint indx, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* ptr) override { Timed t_(this, kCallOther); GL_VertexAttribPointer(indx, size, type, normalized, stride, ptr); }
void Viewport(GLint x, GLint y, GLsizei width, GLsizei height) override { Timed t_(this, kCallOther); GL_Viewport(x, y, width, height); }
void WaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout) override { Timed t_(this, kCallOther); GL_WaitSync(sync, flags, timeout); }
void RenderbufferStorageMultisampleAdvancedAMD(GLenum target, GLsizei samples, GLsizei storageSamples, GLenum internalformat, GLsizei width, GLsizei height) override { Missing("RenderbufferStorageMultisampleAdvancedAMD"); }
void RenderbufferStorageMultisampleEXT(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height) override { Timed t_(this, kCallOther); GL_RenderbufferStorageMultisampleEXT(target, samples, internalformat, width, height); }
void FramebufferTexture2DMultisampleEXT(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level, GLsizei samples) override { Timed t_(this, kCallOther); GL_FramebufferTexture2DMultisampleEXT(target, attachment, textarget, texture, level, samples); }
void TexStorage2DEXT(GLenum target, GLsizei levels, GLenum internalFormat, GLsizei width, GLsizei height) override { Timed t_(this, kCallOther); if (es3_) { GL_TexStorage2D(target, levels, internalFormat, width, height); } else { GL_TexStorage2DEXT(target, levels, internalFormat, width, height); } }
void GenQueriesEXT(GLsizei n, GLuint* queries) override { Timed t_(this, kCallOther); if (es3_) { GL_GenQueries(n, queries); } else { GL_GenQueriesEXT(n, queries); } }
void QueryCounterEXT(GLuint id, GLenum target) override { Timed t_(this, kCallOther); GL_QueryCounterEXT(id, target); }
GLboolean IsQueryEXT(GLuint id) override { Timed t_(this, kCallOther); if (es3_) { return GL_IsQuery(id); } else { return GL_IsQueryEXT(id); } }
void BeginTransformFeedback(GLenum primitivemode) override { Timed t_(this, kCallOther); GL_BeginTransformFeedback(primitivemode); }
void EndTransformFeedback() override { Timed t_(this, kCallOther); GL_EndTransformFeedback(); }
void GetQueryivEXT(GLenum target, GLenum pname, GLint* params) override { Timed t_(this, kCallOther); if (es3_) { GL_GetQueryiv(target, pname, params); } else { GL_GetQueryivEXT(target, pname, params); } }
void GetQueryObjectivEXT(GLuint id, GLenum pname, GLint* params) override { Timed t_(this, kCallOther); GL_GetQueryObjectivEXT(id, pname, params); }
void GetQueryObjecti64vEXT(GLuint id, GLenum pname, GLint64* params) override { Timed t_(this, kCallOther); GL_GetQueryObjecti64vEXT(id, pname, params); }
void GetQueryObjectui64vEXT(GLuint id, GLenum pname, GLuint64* params) override { Timed t_(this, kCallOther); GL_GetQueryObjectui64vEXT(id, pname, params); }
void SetDisjointValueSyncCHROMIUM() override { Missing("SetDisjointValueSyncCHROMIUM"); }
void InsertEventMarkerEXT(GLsizei length, const GLchar* marker) override { Timed t_(this, kCallOther); GL_InsertEventMarkerEXT(length, marker); }
void PushGroupMarkerEXT(GLsizei length, const GLchar* marker) override { Timed t_(this, kCallOther); GL_PushGroupMarkerEXT(length, marker); }
void PopGroupMarkerEXT() override { Timed t_(this, kCallOther); GL_PopGroupMarkerEXT(); }
void GenVertexArraysOES(GLsizei n, GLuint* arrays) override { Timed t_(this, kCallOther); if (es3_) { GL_GenVertexArrays(n, arrays); } else { GL_GenVertexArraysOES(n, arrays); } }
void DeleteVertexArraysOES(GLsizei n, const GLuint* arrays) override { Timed t_(this, kCallOther); if (es3_) { GL_DeleteVertexArrays(n, arrays); } else { GL_DeleteVertexArraysOES(n, arrays); } }
GLboolean IsVertexArrayOES(GLuint array) override { Timed t_(this, kCallOther); if (es3_) { return GL_IsVertexArray(array); } else { return GL_IsVertexArrayOES(array); } }
void BindVertexArrayOES(GLuint array) override { Timed t_(this, kCallOther); if (es3_) { GL_BindVertexArray(array); } else { GL_BindVertexArrayOES(array); } }
void FramebufferParameteri(GLenum target, GLenum pname, GLint param) override { Timed t_(this, kCallOther); GL_FramebufferParameteri(target, pname, param); }
void* MapBufferCHROMIUM(GLuint target, GLenum access) override { Timed t_(this, kCallOther); return GL_MapBufferOES(target, access); }
GLboolean UnmapBufferCHROMIUM(GLuint target) override { Timed t_(this, kCallOther); if (es3_) { return GL_UnmapBuffer(target); } else { return GL_UnmapBufferOES(target); } }
void* MapBufferSubDataCHROMIUM(GLuint target, GLintptr offset, GLsizeiptr size, GLenum access) override { Missing("MapBufferSubDataCHROMIUM"); return {}; }
void UnmapBufferSubDataCHROMIUM(const void* mem) override { Missing("UnmapBufferSubDataCHROMIUM"); }
void* MapTexSubImage2DCHROMIUM(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, GLenum access) override { Missing("MapTexSubImage2DCHROMIUM"); return {}; }
void UnmapTexSubImage2DCHROMIUM(const void* mem) override { Missing("UnmapTexSubImage2DCHROMIUM"); }
void GetProgramInfoCHROMIUM(GLuint program, GLsizei bufsize, GLsizei* size, void* info) override { Missing("GetProgramInfoCHROMIUM"); }
void GetUniformBlocksCHROMIUM(GLuint program, GLsizei bufsize, GLsizei* size, void* info) override { Missing("GetUniformBlocksCHROMIUM"); }
void GetTransformFeedbackVaryingsCHROMIUM(GLuint program, GLsizei bufsize, GLsizei* size, void* info) override { Missing("GetTransformFeedbackVaryingsCHROMIUM"); }
void GetUniformsES3CHROMIUM(GLuint program, GLsizei bufsize, GLsizei* size, void* info) override { Missing("GetUniformsES3CHROMIUM"); }
void DescheduleUntilFinishedCHROMIUM() override { Missing("DescheduleUntilFinishedCHROMIUM"); }
void GetTranslatedShaderSourceANGLE(GLuint shader, GLsizei bufsize, GLsizei* length, char* source) override { Timed t_(this, kCallOther); GL_GetTranslatedShaderSourceANGLE(shader, bufsize, length, source); }
void CopyTextureCHROMIUM(GLuint source_id, GLint source_level, GLenum dest_target, GLuint dest_id, GLint dest_level, GLint internalformat, GLenum dest_type, GLboolean unpack_flip_y, GLboolean unpack_premultiply_alpha, GLboolean unpack_unmultiply_alpha) override { Timed t_(this, kCallOther); GL_CopyTextureCHROMIUM(source_id, source_level, dest_target, dest_id, dest_level, internalformat, dest_type, unpack_flip_y, unpack_premultiply_alpha, unpack_unmultiply_alpha); }
void CopySubTextureCHROMIUM(GLuint source_id, GLint source_level, GLenum dest_target, GLuint dest_id, GLint dest_level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height, GLboolean unpack_flip_y, GLboolean unpack_premultiply_alpha, GLboolean unpack_unmultiply_alpha) override { Timed t_(this, kCallOther); GL_CopySubTextureCHROMIUM(source_id, source_level, dest_target, dest_id, dest_level, xoffset, yoffset, x, y, width, height, unpack_flip_y, unpack_premultiply_alpha, unpack_unmultiply_alpha); }
void DrawArraysInstancedANGLE(GLenum mode, GLint first, GLsizei count, GLsizei primcount) override { Timed t_(this, kCallDraw); if (es3_) { GL_DrawArraysInstanced(mode, first, count, primcount); } else { GL_DrawArraysInstancedANGLE(mode, first, count, primcount); } }
void DrawArraysInstancedBaseInstanceANGLE(GLenum mode, GLint first, GLsizei count, GLsizei primcount, GLuint baseinstance) override { Timed t_(this, kCallDraw); GL_DrawArraysInstancedBaseInstanceANGLE(mode, first, count, primcount, baseinstance); }
void DrawElementsInstancedANGLE(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei primcount) override { Timed t_(this, kCallDraw); if (es3_) { GL_DrawElementsInstanced(mode, count, type, indices, primcount); } else { GL_DrawElementsInstancedANGLE(mode, count, type, indices, primcount); } }
void DrawElementsInstancedBaseVertexBaseInstanceANGLE(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei primcount, GLint basevertex, GLuint baseinstance) override { Timed t_(this, kCallDraw); GL_DrawElementsInstancedBaseVertexBaseInstanceANGLE(mode, count, type, indices, primcount, basevertex, baseinstance); }
void VertexAttribDivisorANGLE(GLuint index, GLuint divisor) override { Timed t_(this, kCallOther); if (es3_) { GL_VertexAttribDivisor(index, divisor); } else { GL_VertexAttribDivisorANGLE(index, divisor); } }
void BindUniformLocationCHROMIUM(GLuint program, GLint location, const char* name) override { Timed t_(this, kCallOther); GL_BindUniformLocationCHROMIUM(program, location, name); }
void TraceBeginCHROMIUM(const char* category_name, const char* trace_name) override { Missing("TraceBeginCHROMIUM"); }
void TraceEndCHROMIUM() override { Missing("TraceEndCHROMIUM"); }
void DiscardFramebufferEXT(GLenum target, GLsizei count, const GLenum* attachments) override { Timed t_(this, kCallOther); GL_DiscardFramebufferEXT(target, count, attachments); }
void LoseContextCHROMIUM(GLenum current, GLenum other) override { Timed t_(this, kCallOther); GL_LoseContextCHROMIUM(current, other); }
void FlushDriverCachesCHROMIUM() override { Missing("FlushDriverCachesCHROMIUM"); }
GLuint GetLastFlushIdCHROMIUM() override { Missing("GetLastFlushIdCHROMIUM"); return {}; }
void SetActiveURLCHROMIUM(const char* url) override { Missing("SetActiveURLCHROMIUM"); }
void ContextVisibilityHintCHROMIUM(GLboolean visibility) override { Missing("ContextVisibilityHintCHROMIUM"); }
void BlendBarrierKHR() override { Timed t_(this, kCallOther); GL_BlendBarrierKHR(); }
void BindFragDataLocationIndexedEXT(GLuint program, GLuint colorNumber, GLuint index, const char* name) override { Timed t_(this, kCallOther); GL_BindFragDataLocationIndexedEXT(program, colorNumber, index, name); }
void BindFragDataLocationEXT(GLuint program, GLuint colorNumber, const char* name) override { Timed t_(this, kCallOther); GL_BindFragDataLocationEXT(program, colorNumber, name); }
GLint GetFragDataIndexEXT(GLuint program, const char* name) override { Timed t_(this, kCallOther); return GL_GetFragDataIndexEXT(program, name); }
void WindowRectanglesEXT(GLenum mode, GLsizei count, const GLint* box) override { Missing("WindowRectanglesEXT"); }
GLuint CreateGpuFenceCHROMIUM() override { Missing("CreateGpuFenceCHROMIUM"); return {}; }
GLuint CreateClientGpuFenceCHROMIUM(ClientGpuFence source) override { Missing("CreateClientGpuFenceCHROMIUM"); return {}; }
void WaitGpuFenceCHROMIUM(GLuint gpu_fence_id) override { Missing("WaitGpuFenceCHROMIUM"); }
void DestroyGpuFenceCHROMIUM(GLuint gpu_fence_id) override { Missing("DestroyGpuFenceCHROMIUM"); }
void InvalidateReadbackBufferShadowDataCHROMIUM(GLuint buffer_id) override { Missing("InvalidateReadbackBufferShadowDataCHROMIUM"); }
void FramebufferTextureMultiviewOVR(GLenum target, GLenum attachment, GLuint texture, GLint level, GLint baseViewIndex, GLsizei numViews) override { Timed t_(this, kCallOther); GL_FramebufferTextureMultiviewOVR(target, attachment, texture, level, baseViewIndex, numViews); }
void MaxShaderCompilerThreadsKHR(GLuint count) override { Timed t_(this, kCallOther); GL_MaxShaderCompilerThreadsKHR(count); }
void CopySharedImageINTERNAL(GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height, const GLbyte* mailboxes) override { Missing("CopySharedImageINTERNAL"); }
void CopySharedImageToTextureINTERNAL(GLuint texture, GLenum target, GLuint internal_format, GLenum type, GLint src_x, GLint src_y, GLsizei width, GLsizei height, GLboolean is_dst_origin_top_left, const GLbyte* src_mailbox) override { Missing("CopySharedImageToTextureINTERNAL"); }
GLboolean ReadbackARGBImagePixelsINTERNAL(const GLbyte* mailbox, const void* dst_color_space, GLuint dst_color_space_size, GLuint dst_size, GLuint dst_width, GLuint dst_height, GLuint dst_color_type, GLuint dst_alpha_type, GLuint dst_row_bytes, GLint src_x, GLint src_y, GLint plane_index, void* pixels) override { Missing("ReadbackARGBImagePixelsINTERNAL"); return {}; }
void WritePixelsYUVINTERNAL(const GLbyte* mailbox, GLuint src_size_plane1, GLuint src_size_plane2, GLuint src_size_plane3, GLuint src_size_plane4, GLuint src_width, GLuint src_height, GLuint src_plane_config, GLuint src_subsampling, GLuint src_datatype, GLuint src_row_bytes_plane1, GLuint src_row_bytes_plane2, GLuint src_row_bytes_plane3, GLuint src_row_bytes_plane4, const void* src_pixels_plane1, const void* src_pixels_plane2, const void* src_pixels_plane3, const void* src_pixels_plane4) override { Missing("WritePixelsYUVINTERNAL"); }
void EnableiOES(GLenum target, GLuint index) override { Timed t_(this, kCallOther); GL_EnableiOES(target, index); }
void DisableiOES(GLenum target, GLuint index) override { Timed t_(this, kCallOther); GL_DisableiOES(target, index); }
void BlendEquationiOES(GLuint buf, GLenum mode) override { Timed t_(this, kCallOther); GL_BlendEquationiOES(buf, mode); }
void BlendEquationSeparateiOES(GLuint buf, GLenum modeRGB, GLenum modeAlpha) override { Timed t_(this, kCallOther); GL_BlendEquationSeparateiOES(buf, modeRGB, modeAlpha); }
void BlendFunciOES(GLuint buf, GLenum src, GLenum dst) override { Timed t_(this, kCallOther); GL_BlendFunciOES(buf, src, dst); }
void BlendFuncSeparateiOES(GLuint buf, GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha) override { Timed t_(this, kCallOther); GL_BlendFuncSeparateiOES(buf, srcRGB, dstRGB, srcAlpha, dstAlpha); }
void ColorMaskiOES(GLuint buf, GLboolean r, GLboolean g, GLboolean b, GLboolean a) override { Timed t_(this, kCallOther); GL_ColorMaskiOES(buf, r, g, b, a); }
GLboolean IsEnablediOES(GLenum target, GLuint index) override { Timed t_(this, kCallOther); return GL_IsEnablediOES(target, index); }
void ProvokingVertexANGLE(GLenum provokeMode) override { Timed t_(this, kCallOther); GL_ProvokingVertexANGLE(provokeMode); }
void FramebufferMemorylessPixelLocalStorageANGLE(GLint plane, GLenum internalformat, GLbitfield usage) override { Timed t_(this, kCallOther); GL_FramebufferMemorylessPixelLocalStorageANGLE(plane, internalformat, usage); }
void FramebufferTexturePixelLocalStorageANGLE(GLint plane, GLuint backingtexture, GLint level, GLint layer, GLbitfield usage) override { Timed t_(this, kCallOther); GL_FramebufferTexturePixelLocalStorageANGLE(plane, backingtexture, level, layer, usage); }
void FramebufferPixelLocalClearValuefvANGLE(GLint plane, const GLfloat* value) override { Timed t_(this, kCallOther); GL_FramebufferPixelLocalClearValuefvANGLE(plane, value); }
void FramebufferPixelLocalClearValueivANGLE(GLint plane, const GLint* value) override { Timed t_(this, kCallOther); GL_FramebufferPixelLocalClearValueivANGLE(plane, value); }
void FramebufferPixelLocalClearValueuivANGLE(GLint plane, const GLuint* value) override { Timed t_(this, kCallOther); GL_FramebufferPixelLocalClearValueuivANGLE(plane, value); }
void BeginPixelLocalStorageANGLE(GLsizei count, const GLenum* loadops) override { Timed t_(this, kCallOther); GL_BeginPixelLocalStorageANGLE(count, loadops); }
void EndPixelLocalStorageANGLE(GLsizei count, const GLenum* storeops) override { Timed t_(this, kCallOther); GL_EndPixelLocalStorageANGLE(count, storeops); }
void EndPixelLocalStorageImplicitANGLE() override { Timed t_(this, kCallOther); GL_EndPixelLocalStorageImplicitANGLE(); }
void PixelLocalStorageBarrierANGLE() override { Timed t_(this, kCallOther); GL_PixelLocalStorageBarrierANGLE(); }
void FramebufferPixelLocalStorageInterruptANGLE() override { Timed t_(this, kCallOther); GL_FramebufferPixelLocalStorageInterruptANGLE(); }
void FramebufferPixelLocalStorageRestoreANGLE() override { Timed t_(this, kCallOther); GL_FramebufferPixelLocalStorageRestoreANGLE(); }
void GetFramebufferPixelLocalStorageParameterfvANGLE(GLint plane, GLenum pname, GLfloat* params) override { Timed t_(this, kCallOther); GL_GetFramebufferPixelLocalStorageParameterfvANGLE(plane, pname, params); }
void GetFramebufferPixelLocalStorageParameterivANGLE(GLint plane, GLenum pname, GLint* params) override { Timed t_(this, kCallOther); GL_GetFramebufferPixelLocalStorageParameterivANGLE(plane, pname, params); }
void GetFramebufferPixelLocalStorageParameteruivANGLE(GLint plane, GLenum pname, GLuint* params) override { Timed t_(this, kCallOther); GL_GetFramebufferPixelLocalStorageParameteruivANGLE(plane, pname, params); }
void ClipControlEXT(GLenum origin, GLenum depth) override { Timed t_(this, kCallOther); GL_ClipControlEXT(origin, depth); }
void PolygonModeANGLE(GLenum face, GLenum mode) override { Timed t_(this, kCallOther); GL_PolygonModeANGLE(face, mode); }
void PolygonOffsetClampEXT(GLfloat factor, GLfloat units, GLfloat clamp) override { Timed t_(this, kCallOther); GL_PolygonOffsetClampEXT(factor, units, clamp); }
