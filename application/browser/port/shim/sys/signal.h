/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	sys/signal.h
 *	The C library's signals with the parts of POSIX a Linux program
 *	takes for granted: a sigaction that carries sa_sigaction and its
 *	flags, a siginfo_t with the address of a fault, sigaltstack. No
 *	signal is delivered to a process on TessronOS; handlers are kept and
 *	never called (application/browser/port/compat/ts_signal.c).
 */

#ifndef __TS_SYS_SIGNAL_H__
#define __TS_SYS_SIGNAL_H__

#define sigaction	__ts_newlib_sigaction
#define siginfo_t	__ts_newlib_siginfo_t
#include_next <sys/signal.h>
#undef sigaction
#undef siginfo_t

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	int		si_signo;
	int		si_errno;
	int		si_code;
	int		si_pid;
	int		si_uid;
	int		si_status;
	void		*si_addr;
	union sigval	si_value;
	long		si_band;
} siginfo_t;

struct sigaction {
	union {
		void	(*sa_handler)( int );
		void	(*sa_sigaction)( int, siginfo_t *, void * );
	};
	sigset_t	sa_mask;
	int		sa_flags;
	void		(*sa_restorer)( void );
};

#undef SA_NOCLDSTOP
#define SA_NOCLDSTOP	0x00000001
#define SA_NOCLDWAIT	0x00000002
#define SA_SIGINFO	0x00000004
#define SA_ONSTACK	0x08000000
#define SA_RESTART	0x10000000
#define SA_NODEFER	0x40000000
#define SA_RESETHAND	0x80000000

#ifndef ILL_ILLOPC
#define ILL_ILLOPC	1
#define ILL_ILLOPN	2
#define ILL_ILLADR	3
#define ILL_ILLTRP	4
#define ILL_PRVOPC	5
#define ILL_PRVREG	6
#define ILL_COPROC	7
#define ILL_BADSTK	8
#define FPE_INTDIV	1
#define FPE_INTOVF	2
#define FPE_FLTDIV	3
#define FPE_FLTOVF	4
#define FPE_FLTUND	5
#define FPE_FLTRES	6
#define FPE_FLTINV	7
#define FPE_FLTSUB	8
#define SEGV_MAPERR	1
#define SEGV_ACCERR	2
#define BUS_ADRALN	1
#define BUS_ADRERR	2
#define BUS_OBJERR	3
#define TRAP_BRKPT	1
#define TRAP_TRACE	2
#endif

#ifndef SIGSTKFLT
#define SIGSTKFLT	16
#endif

int	sigaction( int sig, const struct sigaction *act, struct sigaction *old );

#ifdef __cplusplus
}
#endif

#endif /* __TS_SYS_SIGNAL_H__ */
