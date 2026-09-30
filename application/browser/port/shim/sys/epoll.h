/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	sys/epoll.h
 *	Readiness of many descriptors at once (epoll), within the process
 *	(lib/libcxxrt/ts_evfd.c): an epoll reports the eventfds it watches;
 *	sockets and files watched by it are never reported ready.
 */

#ifndef __TS_SYS_EPOLL_H__
#define __TS_SYS_EPOLL_H__

#include <stdint.h>
#include <signal.h>

#define EPOLLIN		0x001
#define EPOLLPRI	0x002
#define EPOLLOUT	0x004
#define EPOLLERR	0x008
#define EPOLLHUP	0x010
#define EPOLLRDNORM	0x040
#define EPOLLRDBAND	0x080
#define EPOLLWRNORM	0x100
#define EPOLLWRBAND	0x200
#define EPOLLMSG	0x400
#define EPOLLRDHUP	0x2000
#define EPOLLEXCLUSIVE	(1U << 26)
#define EPOLLWAKEUP	(1U << 29)
#define EPOLLONESHOT	(1U << 30)
#define EPOLLET		(1U << 31)

#define EPOLL_CTL_ADD	1
#define EPOLL_CTL_DEL	2
#define EPOLL_CTL_MOD	3

#define EPOLL_CLOEXEC	02000000

typedef union epoll_data {
	void		*ptr;
	int		fd;
	uint32_t	u32;
	uint64_t	u64;
} epoll_data_t;

struct epoll_event {
	uint32_t	events;
	epoll_data_t	data;
};

#ifdef __cplusplus
extern "C" {
#endif

int	epoll_create( int size );
int	epoll_create1( int flags );
int	epoll_ctl( int epfd, int op, int fd, struct epoll_event *event );
int	epoll_wait( int epfd, struct epoll_event *events, int maxevents, int timeout );
int	epoll_pwait( int epfd, struct epoll_event *events, int maxevents, int timeout,
		     const sigset_t *sigmask );

#ifdef __cplusplus
}
#endif

#endif /* __TS_SYS_EPOLL_H__ */
