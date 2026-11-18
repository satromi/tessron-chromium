/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	sys/syscall.h
 *	The numbers of the x86-64 Linux system calls the sources name, and
 *	syscall() (application/browser/port/compat/ts_linux.c), which
 *	answers the few that mean something on TessronOS (gettid, getpid,
 *	read, write, mmap, munmap) and fails the rest with ENOSYS. They are
 *	the numbers of x86-64, not those of port/shim/sys/syscall.h, because
 *	the Rust code built for x86_64-unknown-linux-gnu passes them too.
 */

#ifndef __TS_SYS_SYSCALL_H__
#define __TS_SYS_SYSCALL_H__

#ifdef __cplusplus
extern "C" {
#endif

#define SYS_read		0
#define SYS_write		1
#define SYS_mmap		9
#define SYS_mprotect		10
#define SYS_munmap		11
#define SYS_rt_sigprocmask	14
#define SYS_sched_yield		24
#define SYS_madvise		28
#define SYS_nanosleep		35
#define SYS_getpid		39
#define SYS_clone		56
#define SYS_exit		60
#define SYS_prctl		157
#define SYS_gettid		186
#define SYS_futex		202
#define SYS_getdents64		217
#define SYS_clock_gettime	228
#define SYS_exit_group		231
#define SYS_tgkill		234
#define SYS_set_robust_list	273
#define SYS_rt_tgsigqueueinfo	297
#define SYS_perf_event_open	298
#define SYS_getrandom		318
#define SYS_memfd_create	319
#define SYS_membarrier		324
#define SYS_pkey_mprotect	329
#define SYS_pkey_alloc		330
#define SYS_pkey_free		331
#define SYS_rseq		334
#define SYS_mseal		462
#define __NR_futex		SYS_futex
#define __NR_getdents64		SYS_getdents64
#define __NR_gettid		SYS_gettid
#define __NR_getrandom		SYS_getrandom
#define __NR_membarrier		SYS_membarrier
#define __NR_memfd_create	SYS_memfd_create
#define __NR_mseal		SYS_mseal

long	syscall( long number, ... );

#ifdef __cplusplus
}
#endif

#endif /* __TS_SYS_SYSCALL_H__ */
