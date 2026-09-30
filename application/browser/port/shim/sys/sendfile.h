/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	sys/sendfile.h
 *	sendfile() fails with ENOSYS; callers copy with read and write
 *	instead (application/browser/port/compat/ts_linux.c).
 */

#ifndef __TS_SYS_SENDFILE_H__
#define __TS_SYS_SENDFILE_H__

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

ssize_t	sendfile( int out_fd, int in_fd, off_t *offset, size_t count );
#define sendfile64	sendfile

#ifdef __cplusplus
}
#endif

#endif /* __TS_SYS_SENDFILE_H__ */
