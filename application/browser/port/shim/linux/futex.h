/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	linux/futex.h
 *	The futex operations' names. syscall(SYS_futex) waits and wakes
 *	within the process (port/compat/ts_linux.c, on lib/libpthread).
 */

#ifndef __TS_LINUX_FUTEX_H__
#define __TS_LINUX_FUTEX_H__

#define FUTEX_WAIT		0
#define FUTEX_WAKE		1
#define FUTEX_WAIT_BITSET	9
#define FUTEX_WAKE_BITSET	10
#define FUTEX_PRIVATE_FLAG	128
#define FUTEX_CLOCK_REALTIME	256
#define FUTEX_BITSET_MATCH_ANY	0xffffffff
#define FUTEX_WAIT_PRIVATE	( FUTEX_WAIT | FUTEX_PRIVATE_FLAG )
#define FUTEX_WAKE_PRIVATE	( FUTEX_WAKE | FUTEX_PRIVATE_FLAG )

#endif /* __TS_LINUX_FUTEX_H__ */
