/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	dawn_gles_flags.h
 *	ブラウザ: the build switch of Dawn's OpenGL ES backend for WebGPU's
 *	decoder and the EGL images' shared images in the GPU process, read
 *	before their sources (tools/browser/flags.txt).
 *
 *	The build's configuration has no Dawn of Chromium's, but the program
 *	has Dawn with its OpenGL ES backend when it is made with the system's
 *	OpenGL ES (make GL=1): the decoder then gives Dawn ANGLE's display and
 *	GL, and an EGL image's shared image is given to Dawn as that image.
 *	The switch is read here first, then turned on, and the header that
 *	defines it is not read again.
 */

#ifndef BR_DAWN_GLES_FLAGS_H
#define BR_DAWN_GLES_FLAGS_H

#include "ui/gl/buildflags.h"

#undef BUILDFLAG_INTERNAL_DAWN_ENABLE_BACKEND_OPENGLES
#define BUILDFLAG_INTERNAL_DAWN_ENABLE_BACKEND_OPENGLES() (1)

#endif /* BR_DAWN_GLES_FLAGS_H */
