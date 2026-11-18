/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	gl_init_tessronos.cc
 *	ブラウザ: the platform part of ui/gl's one-off initialisation
 *	(gl::init) in the GPU process (design 14.13; make GL=1).
 *
 *	The process has no windowing platform of Chromium's: the GL of the
 *	command buffer's service is ANGLE, linked into the program, on its
 *	OpenGL ES backend over the EGL and OpenGL ES of TessronOS's Mesa
 *	(a display of the EGL device type, EGL_PLATFORM_ANGLE_DEVICE_TYPE_
 *	EGL_ANGLE: ui/gl's ANGLE_OPENGLES_EGL). The functions here are those
 *	ui/gl/init's common part (gl_factory.cc) leaves to the platform.
 *
 *	The names ui/gl's bindings ask for ("glActiveTexture", "eglCreate-
 *	Context") are looked up with ANGLE's own eglGetProcAddress
 *	(EGL_GetProcAddress, set by InitializeStaticANGLEEGL): its table,
 *	generated with ANGLE from the registries of GL and EGL, gives ANGLE's
 *	entry points (GL_ActiveTexture, EGL_CreateContext). Those are named
 *	apart from Mesa's gl and egl functions, which ANGLE's backend calls,
 *	so the two never stand for each other. No native library is loaded.
 *
 *	The process draws into no window: a view surface is not made, and an
 *	offscreen surface is a surfaceless one where the display allows it
 *	and no size is asked for, a pbuffer otherwise.
 *
 *	glReadPixels is given as a function that times ANGLE's while a
 *	readback of the raster's decoder runs on the thread (patch
 *	raster_decoder_time.patch), for the console of the GPU process: every
 *	context's table of functions, and Skia's interface made from it, take
 *	the function from here.
 */

#include <EGL/egl.h>
#include <string.h>
#include <time.h>

#include <atomic>
#include <cstdint>
#include <vector>

#include "base/logging.h"
#include "base/memory/scoped_refptr.h"
#include "ui/gl/gl_bindings.h"
#include "ui/gl/gl_context.h"
#include "ui/gl/gl_context_egl.h"
#include "ui/gl/gl_display.h"
#include "ui/gl/gl_egl_api_implementation.h"
#include "ui/gl/gl_gl_api_implementation.h"
#include "ui/gl/gl_implementation.h"
#include "ui/gl/gl_share_group.h"
#include "ui/gl/gl_surface.h"
#include "ui/gl/gl_surface_egl.h"
#include "ui/gl/gl_utils.h"
#include "ui/gl/init/gl_factory.h"
#include "ui/gl/init/gl_initializer.h"
#include "ui/gl/presenter.h"

extern "C" {
/* ANGLE's own eglGetProcAddress */
EGLAPI __eglMustCastToProperFunctionPointerType EGLAPIENTRY EGL_GetProcAddress(const char* procname);
}

namespace gpu::raster {
/* the readbacks' counts (patch raster_decoder_time.patch): [2] is glReadPixels's microseconds */
extern std::atomic<int64_t> g_readback_work[5];
extern thread_local bool g_in_readback;
}  // namespace gpu::raster

namespace {

using ReadPixels = void(GL_BINDING_CALL*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
ReadPixels g_read_pixels = nullptr;

int64_t now_us()
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

void GL_BINDING_CALL timed_read_pixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type,
				       void* pixels)
{
	if (!gpu::raster::g_in_readback) {
		g_read_pixels(x, y, w, h, format, type, pixels);
		return;
	}
	int64_t t0 = now_us();
	g_read_pixels(x, y, w, h, format, type, pixels);
	gpu::raster::g_readback_work[2] += now_us() - t0;
}

/* ANGLE's functions by their names, glReadPixels timed */
gl::GLFunctionPointerType GL_BINDING_CALL get_proc(const char* name)
{
	gl::GLFunctionPointerType f = reinterpret_cast<gl::GLFunctionPointerType>(EGL_GetProcAddress(name));
	if (f != nullptr && strcmp(name, "glReadPixels") == 0) {
		g_read_pixels = reinterpret_cast<ReadPixels>(f);
		return reinterpret_cast<gl::GLFunctionPointerType>(&timed_read_pixels);
	}
	return f;
}

}  // namespace

namespace gl {
namespace init {

/* the one implementation: ANGLE on its OpenGL ES backend */
std::vector<GLImplementationParts> GetAllowedGLImplementations()
{
	return {GLImplementationParts(ANGLEImplementation::kOpenGLES)};
}

bool InitializeStaticGLBindings(GLImplementationParts implementation)
{
	if (implementation.gl != kGLImplementationEGLANGLE) {
		LOG(ERROR) << "gl_init: GL implementation " << implementation.ToString()
			   << " is not there (ANGLE only)";
		return false;
	}
	if (!InitializeStaticANGLEEGL()) {
		return false;
	}
	SetGLGetProcAddressProc(&get_proc);
	SetGLImplementationParts(implementation);
	InitializeStaticGLBindingsGL();
	InitializeStaticGLBindingsEGL();
	return true;
}

/* ANGLE's display on the EGL device of the native (Mesa's) default display */
GLDisplay* InitializeGLOneOffPlatform(gl::GpuPreference gpu_preference)
{
	GLDisplayEGL* display = GetDisplayEGL(gpu_preference);
	std::vector<DisplayType> init_displays = {ANGLE_OPENGLES_EGL};
	if (!display->Initialize(/*supports_angle=*/true, init_displays,
				 EGLDisplayPlatform(EGL_DEFAULT_DISPLAY))) {
		LOG(ERROR) << "gl_init: ANGLE's display on the native EGL failed";
		return nullptr;
	}
	return display;
}

void ShutdownGLPlatform(GLDisplay* display)
{
	if (display) {
		display->Shutdown();
	}
	ClearBindingsGL();
	ClearBindingsEGL();
}

bool GetGLWindowSystemBindingInfo(const GLVersionInfo& gl_info,
				  GLWindowSystemBindingInfo* info)
{
	return GetGLWindowSystemBindingInfoEGL(info);
}

scoped_refptr<GLContext> CreateGLContext(GLShareGroup* share_group,
					 GLSurface* compatible_surface,
					 const GLContextAttribs& attribs)
{
	return InitializeGLContext(new GLContextEGL(share_group),
				   compatible_surface, attribs);
}

scoped_refptr<GLSurface> CreateViewGLSurface(GLDisplay* display,
					     gfx::AcceleratedWidget window)
{
	return nullptr;
}

scoped_refptr<Presenter> CreateSurfacelessViewGLSurface(GLDisplay* display,
							gfx::AcceleratedWidget window)
{
	return nullptr;
}

scoped_refptr<GLSurface> CreateOffscreenGLSurface(GLDisplay* display,
						  const gfx::Size& size)
{
	GLDisplayEGL* egl = display->GetAs<GLDisplayEGL>();
	if (egl->IsEGLSurfacelessContextSupported() && size.width() == 0 &&
	    size.height() == 0) {
		return InitializeGLSurface(base::MakeRefCounted<SurfacelessEGL>(egl, size));
	}
	return InitializeGLSurface(base::MakeRefCounted<PbufferGLSurfaceEGL>(egl, size));
}

void SetDisabledExtensionsPlatform(const std::string& disabled_extensions)
{
	SetDisabledExtensionsEGL(disabled_extensions);
}

bool InitializeExtensionSettingsOneOffPlatform(GLDisplay* display)
{
	return InitializeExtensionSettingsOneOffEGL(static_cast<GLDisplayEGL*>(display));
}

}  // namespace init
}  // namespace gl
