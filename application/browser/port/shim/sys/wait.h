/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	sys/wait.h
 *	The C library's sys/wait.h and wait4: a process has no children it
 *	waits for this way.
 */

#ifndef __TS_SYS_WAIT_H__
#define __TS_SYS_WAIT_H__

#include_next <sys/wait.h>
#include <sys/resource.h>

#ifdef __cplusplus
extern "C" {
#endif

pid_t	wait4( pid_t pid, int *status, int options, struct rusage *ru );

#ifdef __cplusplus
}
#endif

#endif /* __TS_SYS_WAIT_H__ */
