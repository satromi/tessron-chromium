/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	sys/prctl.h
 *	prctl(): the names and flags a Linux program sets on itself; accepted
 *	and forgotten.
 */

#ifndef __TS_SYS_PRCTL_H__
#define __TS_SYS_PRCTL_H__

#ifdef __cplusplus
extern "C" {
#endif

#define PR_SET_PDEATHSIG	1
#define PR_GET_DUMPABLE		3
#define PR_SET_DUMPABLE		4
#define PR_SET_NAME		15
#define PR_GET_NAME		16
#define PR_SET_SECCOMP		22
#define PR_SET_TIMERSLACK	29
#define PR_SET_NO_NEW_PRIVS	38
#define PR_SET_VMA		0x53564d41
#define PR_SET_VMA_ANON_NAME	0
#define PR_SET_PTRACER		0x59616d61
#define PR_SET_PTRACER_ANY	((unsigned long)-1)

int	prctl( int option, ... );

#ifdef __cplusplus
}
#endif

#endif /* __TS_SYS_PRCTL_H__ */
