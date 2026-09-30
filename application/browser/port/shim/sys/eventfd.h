/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	sys/eventfd.h
 *	Counters that wake a thread waiting in epoll_wait (eventfd), within
 *	the process (lib/libcxxrt/ts_evfd.c).
 */

#ifndef __TS_SYS_EVENTFD_H__
#define __TS_SYS_EVENTFD_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t eventfd_t;

#define EFD_SEMAPHORE	1
#define EFD_CLOEXEC	02000000
#define EFD_NONBLOCK	04000

int	eventfd( unsigned int initval, int flags );
int	eventfd_read( int fd, eventfd_t *value );
int	eventfd_write( int fd, eventfd_t value );

#ifdef __cplusplus
}
#endif

#endif /* __TS_SYS_EVENTFD_H__ */
