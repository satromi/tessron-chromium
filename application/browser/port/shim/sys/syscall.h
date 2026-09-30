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
 *	The numbers of the aarch64 Linux system calls the sources name, and
 *	syscall() (application/browser/port/compat/ts_linux.c), which
 *	answers the few that mean something on TessronOS (gettid, getpid,
 *	read, write, mmap, munmap) and fails the rest with ENOSYS.
 */

#ifndef __TS_SYS_SYSCALL_H__
#define __TS_SYS_SYSCALL_H__

#ifdef __cplusplus
extern "C" {
#endif

#define SYS_getdents64		61
#define SYS_read		63
#define SYS_write		64
#define SYS_exit		93
#define SYS_exit_group		94
#define SYS_futex		98
#define SYS_set_robust_list	99
#define SYS_nanosleep		101
#define SYS_clock_gettime	113
#define SYS_sched_yield		124
#define SYS_tgkill		131
#define SYS_rt_sigprocmask	135
#define SYS_prctl		167
#define SYS_getpid		172
#define SYS_gettid		178
#define SYS_munmap		215
#define SYS_clone		220
#define SYS_mmap		222
#define SYS_mprotect		226
#define SYS_madvise		233
#define SYS_rt_tgsigqueueinfo	240
#define SYS_perf_event_open	241
#define SYS_getrandom		278
#define SYS_memfd_create	279
#define SYS_membarrier		283
#define SYS_pkey_mprotect	288
#define SYS_pkey_alloc		289
#define SYS_pkey_free		290
#define SYS_rseq		293
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
