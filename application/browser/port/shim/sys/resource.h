/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	sys/resource.h
 *	The C library's sys/resource.h and the resource limits a Linux
 *	program reads (getrlimit): none is set on TessronOS, every one is
 *	RLIM_INFINITY.
 */

#ifndef __TS_SYS_RESOURCE_H__
#define __TS_SYS_RESOURCE_H__

#define rusage		__ts_newlib_rusage
#define getrusage	__ts_newlib_getrusage
#include_next <sys/resource.h>
#undef rusage
#undef getrusage
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long rlim_t;

struct rlimit {
	rlim_t	rlim_cur;
	rlim_t	rlim_max;
};

#define RLIM_INFINITY	(~0UL)
#define RLIMIT_CPU	0
#define RLIMIT_FSIZE	1
#define RLIMIT_DATA	2
#define RLIMIT_STACK	3
#define RLIMIT_CORE	4
#define RLIMIT_RSS	5
#define RLIMIT_NPROC	6
#define RLIMIT_NOFILE	7
#define RLIMIT_MEMLOCK	8
#define RLIMIT_AS	9
#define RLIMIT_LOCKS	10
#define RLIMIT_SIGPENDING 11
#define RLIMIT_MSGQUEUE	12
#define RLIMIT_NICE	13
#define RLIMIT_RTPRIO	14
#define RLIMIT_RTTIME	15
#define RLIMIT_NLIMITS	16

/* the nice value of a task that has not asked for another */
#ifndef NZERO
#define NZERO		20
#endif

#define PRIO_PROCESS	0
#define PRIO_PGRP	1
#define PRIO_USER	2

struct rusage {
	struct timeval	ru_utime;
	struct timeval	ru_stime;
	long		ru_maxrss;
	long		ru_ixrss;
	long		ru_idrss;
	long		ru_isrss;
	long		ru_minflt;
	long		ru_majflt;
	long		ru_nswap;
	long		ru_inblock;
	long		ru_oublock;
	long		ru_msgsnd;
	long		ru_msgrcv;
	long		ru_nsignals;
	long		ru_nvcsw;
	long		ru_nivcsw;
};

#ifndef RUSAGE_SELF
#define RUSAGE_SELF	0
#define RUSAGE_CHILDREN	(-1)
#endif
#define RUSAGE_THREAD	1

int	getrusage( int who, struct rusage *ru );
int	getrlimit( int resource, struct rlimit *rl );
int	setrlimit( int resource, const struct rlimit *rl );
int	getpriority( int which, int who );
int	setpriority( int which, int who, int prio );

#ifdef __cplusplus
}
#endif

#endif /* __TS_SYS_RESOURCE_H__ */
