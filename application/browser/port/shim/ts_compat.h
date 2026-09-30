/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	ts_compat.h
 *	Read first by every source of the engine compiled for TessronOS
 *	(tools/browser/translate.py, design 17.19).
 *
 *	The sources are written for Linux with glibc; they are compiled with
 *	__linux__ defined, against newlib and the headers of this directory,
 *	which give the parts of the Linux and POSIX surface they reach for.
 *	What is declared here is what several unrelated headers take for
 *	granted before any of them is read.
 */

#ifndef __TS_COMPAT_H__
#define __TS_COMPAT_H__

#include <stddef.h>
#include <limits.h>

#ifndef SSIZE_MAX
#define SSIZE_MAX	LONG_MAX
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* The name the program was started by (glibc's, from <errno.h>) */
extern char	*program_invocation_name;
extern char	*program_invocation_short_name;

#ifdef __cplusplus
}
#endif

#endif /* __TS_COMPAT_H__ */
