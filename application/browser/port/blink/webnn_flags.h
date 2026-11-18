/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	webnn_flags.h
 *	ブラウザ: the build switches of WebNN as the browser has it, read
 *	before every source of the service (tools/browser/flags.txt).
 *
 *	The build's configuration says LiteRT and TFLite with XNNPACK are
 *	there, but their sources are not in the checkout and the browser
 *	computes the graphs itself (port/blink/webnn_tessronos.cc): the
 *	switches are read here first, then turned off, and the headers that
 *	define them are not read again.
 */

#ifndef BR_WEBNN_FLAGS_H
#define BR_WEBNN_FLAGS_H

#include "services/webnn/buildflags.h"
#include "third_party/tflite/buildflags.h"

#undef BUILDFLAG_INTERNAL_WEBNN_USE_TFLITE
#define BUILDFLAG_INTERNAL_WEBNN_USE_TFLITE() (0)
#undef BUILDFLAG_INTERNAL_WEBNN_USE_LITERT
#define BUILDFLAG_INTERNAL_WEBNN_USE_LITERT() (0)
#undef BUILDFLAG_INTERNAL_BUILD_TFLITE_WITH_XNNPACK
#define BUILDFLAG_INTERNAL_BUILD_TFLITE_WITH_XNNPACK() (0)

#endif /* BR_WEBNN_FLAGS_H */
