/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	fcntl.h
 *	newlib's, with the names glibc adds: O_NDELAY, and the large file
 *	names, which are the same calls where off_t has 64 bits.
 */

#ifndef __TS_FCNTL_H__
#define __TS_FCNTL_H__

#include_next <fcntl.h>

#ifndef O_NDELAY
#define O_NDELAY	O_NONBLOCK
#endif
#ifndef O_LARGEFILE
#define O_LARGEFILE	0
#endif
#define open64		open
#define openat64	openat

/* Sealing a memory file: none can be made (memfd_create fails) */
#ifndef F_ADD_SEALS
#define F_ADD_SEALS	1033
#define F_GET_SEALS	1034
#define F_SEAL_SEAL	0x0001
#define F_SEAL_SHRINK	0x0002
#define F_SEAL_GROW	0x0004
#define F_SEAL_WRITE	0x0008
#endif

#define POSIX_FADV_NORMAL	0
#define POSIX_FADV_RANDOM	1
#define POSIX_FADV_SEQUENTIAL	2
#define POSIX_FADV_WILLNEED	3
#define POSIX_FADV_DONTNEED	4
#define POSIX_FADV_NOREUSE	5

#define FALLOC_FL_KEEP_SIZE	0x01
#define FALLOC_FL_PUNCH_HOLE	0x02

#ifdef __cplusplus
extern "C" {
#endif

/* Space given to a file ahead of its writes: fails with ENOSYS, callers write */
int	fallocate( int fd, int mode, off_t offset, off_t len );
int	posix_fallocate( int fd, off_t offset, off_t len );
/* Advice on how a file will be read: taken and not followed */
int	posix_fadvise( int fd, off_t offset, off_t len, int advice );
#define fallocate64		fallocate
#define posix_fallocate64	posix_fallocate

#ifdef __cplusplus
}
#endif

#endif /* __TS_FCNTL_H__ */
