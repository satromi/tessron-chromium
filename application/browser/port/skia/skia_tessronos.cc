/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	skia_tessronos.cc
 *	What Skia, as Chromium configures it (skia/config/SkUserConfig.h),
 *	expects of the program around it: the sinks of its messages and
 *	aborts, and the histograms it counts into, which are not kept here.
 *	Messages go to the console.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "include/core/SkTypes.h"
#include "skia/ext/skia_histogram.h"

void SkDebugf_FileLine(const char* file, int line, const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  vfprintf(stderr, format, ap);
  va_end(ap);
}

void SkLog_FileLine(const char* file, int line, SkLogPriority priority,
                    const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  vfprintf(stderr, format, ap);
  va_end(ap);
}

void SkAbort_FileLine(const char* file, int line, const char* format, ...) {
  va_list ap;
  fprintf(stderr, "skia: abort at %s:%d: ", file, line);
  va_start(ap, format);
  vfprintf(stderr, format, ap);
  va_end(ap);
  fputc('\n', stderr);
  abort();
}

namespace skia {

void HistogramBoolean(std::atomic_uintptr_t*, const char*, bool) {}
void HistogramEnumeration(std::atomic_uintptr_t*, const char*, int, int) {}
void HistogramExactLinear(std::atomic_uintptr_t*, const char*, int, int) {}
void HistogramCustomExactLinear(std::atomic_uintptr_t*, const char*, int, int,
                                int, size_t) {}
void HistogramMemoryKB(std::atomic_uintptr_t*, const char*, int) {}
void HistogramCustomCounts(std::atomic_uintptr_t*, const char*, int, int, int,
                           size_t) {}
void HistogramCustomMicrosecondsTimes(std::atomic_uintptr_t*, const char*,
                                      int64_t, unsigned, unsigned, size_t) {}

}  // namespace skia
