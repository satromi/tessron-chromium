/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	unistd.h
 *	newlib's, with glibc's large file names, which are the same calls
 *	where off_t has 64 bits.
 */

#ifndef __TS_UNISTD_H__
#define __TS_UNISTD_H__

#include_next <unistd.h>

#define off64_t		off_t
#define lseek64		lseek
#define ftruncate64	ftruncate
#define pread64		pread
#define pwrite64	pwrite

#endif /* __TS_UNISTD_H__ */
