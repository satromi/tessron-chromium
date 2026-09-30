/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	sys/ucontext.h
 *	The machine context of aarch64 Linux, which the sources read in a
 *	signal handler: never filled on TessronOS, where no signal is
 *	delivered.
 */

#ifndef __TS_SYS_UCONTEXT_H__
#define __TS_SYS_UCONTEXT_H__

#include <signal.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	unsigned long long	fault_address;
	unsigned long long	regs[31];
	unsigned long long	sp;
	unsigned long long	pc;
	unsigned long long	pstate;
	unsigned char		__reserved[4096] __attribute__((aligned(16)));
} mcontext_t;

typedef struct ucontext {
	unsigned long		uc_flags;
	struct ucontext		*uc_link;
	stack_t			uc_stack;
	sigset_t		uc_sigmask;
	mcontext_t		uc_mcontext;
} ucontext_t;

#ifdef __cplusplus
}
#endif

#endif /* __TS_SYS_UCONTEXT_H__ */
