/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	webgl_webgpu_flags.h
 *	ブラウザ: WebGL on WebGPU's build switch of Dawn, read before its
 *	source (tools/browser/flags.txt).
 *
 *	The build's configuration has no Dawn of Chromium's (use_dawn =
 *	false), but the browser has Dawn's wire client and its own server
 *	when it is made with the system's OpenGL ES (make GL=1,
 *	port/webgpu/webgpu_tessronos.cc): the switch is read here first, then
 *	turned on, and the header that defines it is not read again.
 */

#ifndef BR_WEBGL_WEBGPU_FLAGS_H
#define BR_WEBGL_WEBGPU_FLAGS_H

#include "ui/gl/buildflags.h"

#undef BUILDFLAG_INTERNAL_USE_DAWN
#define BUILDFLAG_INTERNAL_USE_DAWN() (1)

#endif /* BR_WEBGL_WEBGPU_FLAGS_H */
