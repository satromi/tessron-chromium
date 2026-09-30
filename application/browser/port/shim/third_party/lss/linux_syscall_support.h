/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	third_party/lss/linux_syscall_support.h
 *	Read in place of the library of inline Linux system calls: those trap
 *	into the kernel with Linux's numbers, which TessronOS's kernel does not
 *	take. The sources that include it call syscall() with the numbers of
 *	sys/syscall.h, which the port answers (application/browser/port/
 *	compat/ts_linux.c).
 */

#ifndef __TS_LINUX_SYSCALL_SUPPORT_H__
#define __TS_LINUX_SYSCALL_SUPPORT_H__

#include <sys/syscall.h>
#include <unistd.h>

#endif /* __TS_LINUX_SYSCALL_SUPPORT_H__ */
