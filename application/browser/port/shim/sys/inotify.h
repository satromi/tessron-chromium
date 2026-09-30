/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	sys/inotify.h
 *	Watching files (inotify): none on TessronOS; inotify_init fails with
 *	ENOSYS.
 */

#ifndef __TS_SYS_INOTIFY_H__
#define __TS_SYS_INOTIFY_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct inotify_event {
	int		wd;
	uint32_t	mask;
	uint32_t	cookie;
	uint32_t	len;
	char		name[];
};

#define IN_ACCESS	0x001
#define IN_MODIFY	0x002
#define IN_ATTRIB	0x004
#define IN_CLOSE_WRITE	0x008
#define IN_CLOSE_NOWRITE 0x010
#define IN_OPEN		0x020
#define IN_MOVED_FROM	0x040
#define IN_MOVED_TO	0x080
#define IN_CREATE	0x100
#define IN_DELETE	0x200
#define IN_DELETE_SELF	0x400
#define IN_MOVE_SELF	0x800
#define IN_CLOSE	(IN_CLOSE_WRITE | IN_CLOSE_NOWRITE)
#define IN_MOVE		(IN_MOVED_FROM | IN_MOVED_TO)
#define IN_UNMOUNT	0x2000
#define IN_MASK_ADD	0x20000000
#define IN_EXCL_UNLINK	0x04000000
#define IN_ONLYDIR	0x01000000
#define IN_DONT_FOLLOW	0x02000000
#define IN_CLOEXEC	02000000
#define IN_NONBLOCK	04000
#define IN_IGNORED	0x8000
#define IN_Q_OVERFLOW	0x4000
#define IN_ISDIR	0x40000000

int	inotify_init( void );
int	inotify_init1( int flags );
int	inotify_add_watch( int fd, const char *path, uint32_t mask );
int	inotify_rm_watch( int fd, int wd );

#ifdef __cplusplus
}
#endif

#endif /* __TS_SYS_INOTIFY_H__ */
