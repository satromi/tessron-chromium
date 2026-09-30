/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	sys/vfs.h
 *	What a file system holds (statfs): not told on TessronOS; statfs
 *	fails with ENOSYS (application/browser/port/compat/ts_linux.c).
 */

#ifndef __TS_SYS_VFS_H__
#define __TS_SYS_VFS_H__

#include <stdint.h>

typedef struct { int __val[2]; } fsid_t;

struct statfs {
	long		f_type;
	long		f_bsize;
	uint64_t	f_blocks;
	uint64_t	f_bfree;
	uint64_t	f_bavail;
	uint64_t	f_files;
	uint64_t	f_ffree;
	fsid_t		f_fsid;
	long		f_namelen;
	long		f_frsize;
	long		f_flags;
	long		f_spare[4];
};

#ifdef __cplusplus
extern "C" {
#endif

int	statfs( const char *path, struct statfs *buf );
int	fstatfs( int fd, struct statfs *buf );

#ifdef __cplusplus
}
#endif

#endif /* __TS_SYS_VFS_H__ */
