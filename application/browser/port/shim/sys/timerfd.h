/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	sys/timerfd.h
 *	Timers read as descriptors (timerfd): none on TessronOS; timerfd_create
 *	fails with ENOSYS.
 */

#ifndef __TS_SYS_TIMERFD_H__
#define __TS_SYS_TIMERFD_H__

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TFD_CLOEXEC		02000000
#define TFD_NONBLOCK		04000
#define TFD_TIMER_ABSTIME	1

struct itimerspec;

int	timerfd_create( int clockid, int flags );
int	timerfd_settime( int fd, int flags, const struct itimerspec *nv, struct itimerspec *ov );
int	timerfd_gettime( int fd, struct itimerspec *cur );

#ifdef __cplusplus
}
#endif

#endif /* __TS_SYS_TIMERFD_H__ */
