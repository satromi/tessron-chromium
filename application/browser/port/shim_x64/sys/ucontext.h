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
 *	The machine context of x86-64 Linux, which the sources read in a
 *	signal handler. On TessronOS it is filled only for a fault of the
 *	process's own handed back to it (port/compat/ts_signal.c, from the
 *	picture of include/ts/proc.h's T_TSFLT): V8's trap handler for
 *	WebAssembly reads and moves REG_RIP and sets REG_R10.
 *
 *	The x86-64 build reads this directory before port/shim
 *	(tools/browser/translate.py, TS_ARCH=x86_64).
 */

#ifndef __TS_SYS_UCONTEXT_H__
#define __TS_SYS_UCONTEXT_H__

#include <signal.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef long long	greg_t;

#define NGREG	23
typedef greg_t		gregset_t[NGREG];

/* The general registers' places in gregs, as glibc numbers them */
enum {
	REG_R8 = 0,
#define REG_R8		REG_R8
	REG_R9,
#define REG_R9		REG_R9
	REG_R10,
#define REG_R10		REG_R10
	REG_R11,
#define REG_R11		REG_R11
	REG_R12,
#define REG_R12		REG_R12
	REG_R13,
#define REG_R13		REG_R13
	REG_R14,
#define REG_R14		REG_R14
	REG_R15,
#define REG_R15		REG_R15
	REG_RDI,
#define REG_RDI		REG_RDI
	REG_RSI,
#define REG_RSI		REG_RSI
	REG_RBP,
#define REG_RBP		REG_RBP
	REG_RBX,
#define REG_RBX		REG_RBX
	REG_RDX,
#define REG_RDX		REG_RDX
	REG_RAX,
#define REG_RAX		REG_RAX
	REG_RCX,
#define REG_RCX		REG_RCX
	REG_RSP,
#define REG_RSP		REG_RSP
	REG_RIP,
#define REG_RIP		REG_RIP
	REG_EFL,
#define REG_EFL		REG_EFL
	REG_CSGSFS,
#define REG_CSGSFS	REG_CSGSFS
	REG_ERR,
#define REG_ERR		REG_ERR
	REG_TRAPNO,
#define REG_TRAPNO	REG_TRAPNO
	REG_OLDMASK,
#define REG_OLDMASK	REG_OLDMASK
	REG_CR2
#define REG_CR2		REG_CR2
};

struct _libc_fpxreg {
	unsigned short		significand[4];
	unsigned short		exponent;
	unsigned short		__glibc_reserved1[3];
};

struct _libc_xmmreg {
	uint32_t		element[4];
};

/* The FXSAVE image */
struct _libc_fpstate {
	uint16_t		cwd;
	uint16_t		swd;
	uint16_t		ftw;
	uint16_t		fop;
	uint64_t		rip;
	uint64_t		rdp;
	uint32_t		mxcsr;
	uint32_t		mxcr_mask;
	struct _libc_fpxreg	_st[8];
	struct _libc_xmmreg	_xmm[16];
	uint32_t		__glibc_reserved1[24];
};

typedef struct _libc_fpstate	*fpregset_t;

typedef struct {
	gregset_t		gregs;
	fpregset_t		fpregs;
	unsigned long long	__reserved1[8];
} mcontext_t;

typedef struct ucontext_t {
	unsigned long		uc_flags;
	struct ucontext_t	*uc_link;
	stack_t			uc_stack;
	mcontext_t		uc_mcontext;
	sigset_t		uc_sigmask;
	struct _libc_fpstate	__fpregs_mem;
	unsigned long long	__ssp[4];
} ucontext_t;

#ifdef __cplusplus
}
#endif

#endif /* __TS_SYS_UCONTEXT_H__ */
