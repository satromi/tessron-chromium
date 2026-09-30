/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	sched.h
 *	newlib's, with the flags of Linux's clone. A TessronOS process does not
 *	fork or clone itself (syscall fails SYS_clone with ENOSYS); the names
 *	are what the sources that would compile.
 */

#ifndef __TS_SCHED_H__
#define __TS_SCHED_H__

#include_next <sched.h>

#define CSIGNAL			0x000000ff
#define CLONE_VM		0x00000100
#define CLONE_FS		0x00000200
#define CLONE_FILES		0x00000400
#define CLONE_SIGHAND		0x00000800
#define CLONE_PIDFD		0x00001000
#define CLONE_PTRACE		0x00002000
#define CLONE_VFORK		0x00004000
#define CLONE_PARENT		0x00008000
#define CLONE_THREAD		0x00010000
#define CLONE_NEWNS		0x00020000
#define CLONE_SYSVSEM		0x00040000
#define CLONE_SETTLS		0x00080000
#define CLONE_PARENT_SETTID	0x00100000
#define CLONE_CHILD_CLEARTID	0x00200000
#define CLONE_DETACHED		0x00400000
#define CLONE_UNTRACED		0x00800000
#define CLONE_CHILD_SETTID	0x01000000
#define CLONE_NEWCGROUP		0x02000000
#define CLONE_NEWUTS		0x04000000
#define CLONE_NEWIPC		0x08000000
#define CLONE_NEWUSER		0x10000000
#define CLONE_NEWPID		0x20000000
#define CLONE_NEWNET		0x40000000
#define CLONE_IO		0x80000000

#ifdef __cplusplus
extern "C" {
#endif

int	clone( int (*fn)( void *arg ), void *stack, int flags, void *arg, ... );
int	unshare( int flags );

/*
 * Sets of processors: a task runs where the kernel puts it; the affinity
 * a program reads is every processor of the machine, and a set one is
 * taken and not kept.
 */
#define CPU_SETSIZE	1024
typedef struct {
	unsigned long	__bits[CPU_SETSIZE / (8 * sizeof(unsigned long))];
} cpu_set_t;

#define __CPU_WORD(c)		((c) / (8 * sizeof(unsigned long)))
#define __CPU_BIT(c)		(1UL << ((c) % (8 * sizeof(unsigned long))))
#define CPU_ZERO(s)		__builtin_memset((s), 0, sizeof(cpu_set_t))
#define CPU_SET(c, s)		((void)((s)->__bits[__CPU_WORD(c)] |= __CPU_BIT(c)))
#define CPU_CLR(c, s)		((void)((s)->__bits[__CPU_WORD(c)] &= ~__CPU_BIT(c)))
#define CPU_ISSET(c, s)		(((s)->__bits[__CPU_WORD(c)] & __CPU_BIT(c)) != 0)
#define CPU_COUNT(s)		__ts_cpu_count(s)
#define CPU_EQUAL(a, b)		(__builtin_memcmp((a), (b), sizeof(cpu_set_t)) == 0)
#define CPU_ALLOC_SIZE(n)	sizeof(cpu_set_t)
#define CPU_ALLOC(n)		((cpu_set_t *)__builtin_calloc(1, sizeof(cpu_set_t)))
#define CPU_FREE(s)		__builtin_free(s)
#define CPU_ZERO_S(sz, s)	CPU_ZERO(s)
#define CPU_SET_S(c, sz, s)	CPU_SET(c, s)
#define CPU_ISSET_S(c, sz, s)	CPU_ISSET(c, s)
#define CPU_COUNT_S(sz, s)	CPU_COUNT(s)

int	__ts_cpu_count( const cpu_set_t *s );
int	sched_getaffinity( int pid, size_t size, cpu_set_t *set );
int	sched_setaffinity( int pid, size_t size, const cpu_set_t *set );
int	sched_getcpu( void );

/* A thread's processors are the process's */
#define pthread_getaffinity_np(th, size, set)	sched_getaffinity(0, (size), (set))

#ifdef __cplusplus
}
#endif

#endif /* __TS_SCHED_H__ */
