/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	sys/statvfs.h
 *	statvfs(): what a file system has room for. TessronOS does not tell a
 *	program; both calls fail with ENOSYS
 *	(application/browser/port/compat/ts_linux.c).
 */

#ifndef __TS_SYS_STATVFS_H__
#define __TS_SYS_STATVFS_H__

#include <sys/types.h>		/* fsblkcnt_t, fsfilcnt_t */

#ifdef __cplusplus
extern "C" {
#endif

struct statvfs {
	unsigned long	f_bsize;
	unsigned long	f_frsize;
	fsblkcnt_t	f_blocks;
	fsblkcnt_t	f_bfree;
	fsblkcnt_t	f_bavail;
	fsfilcnt_t	f_files;
	fsfilcnt_t	f_ffree;
	fsfilcnt_t	f_favail;
	unsigned long	f_fsid;
	unsigned long	f_flag;
	unsigned long	f_namemax;
};

#define ST_RDONLY	1
#define ST_NOSUID	2

int	statvfs( const char *path, struct statvfs *buf );
int	fstatvfs( int fd, struct statvfs *buf );
#define statvfs64	statvfs
#define fstatvfs64	fstatvfs

#ifdef __cplusplus
}
#endif

#endif /* __TS_SYS_STATVFS_H__ */
