/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	ts_signal.c
 *	Signals. A process on TessronOS is sent none but a fault of its own
 *	(design 9.11.1): a data abort its memory does not put right goes, when
 *	the process has named an entry for it (ts_def_flt), to that entry with
 *	a picture of where the task was. Here that is SIGSEGV (SIGBUS for an
 *	alignment fault): the handler sigaction installed for it is called
 *	with a siginfo_t and a ucontext_t made from the picture, and what it
 *	leaves in the context is where the task goes back to (ts_flt_ret).
 *	V8's trap handler for WebAssembly is such a handler: a load or a store
 *	of a module's out of its memory lands on the guard pages around it,
 *	and the handler turns it into the module's trap.
 *
 *	A fault with no handler, or with the default one, takes the entry
 *	away and goes back: the access, made again, ends the process as
 *	before. The other signals' handlers are kept, so that the engine reads
 *	back what it set, and are never called.
 *
 *	The registers a function keeps (x19 to x28; rbx and r12 to r15 on
 *	x86-64) are not in the picture the system makes: the entry is to
 *	keep them. The entry (ts_flt_entry) is a few instructions that put
 *	them into the picture before anything changes them and call the C
 *	part (ts_flt_handle); that ends in ts_flt_back, which loads them from
 *	the picture again just before ts_flt_ret. The task goes back with
 *	them as they were, or as the handler set them in the context. The C
 *	part never returns, so without this its own use of them would be
 *	what the task found on going back.
 *
 *	On x86-64 the picture holds rax to r15 (but rsp), rsp, rip, the
 *	arithmetic flags, the address met (CR2) and the vector and error
 *	code of the page fault; they are put into the general registers of
 *	the context (gregs, port/shim_x64/sys/ucontext.h) and taken back from
 *	it. Only a page fault comes here: the fault is SEGV_MAPERR when the
 *	page was not there (the error code's P bit clear), SEGV_ACCERR when
 *	the access was not allowed. V8's trap handler moves REG_RIP to its
 *	landing pad and puts the place of the fault in REG_R10.
 */

#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <ucontext.h>

#define NSIG_TF		64

/* The picture of where a task met its fault (include/ts/proc.h: T_TSFLT) */
#if defined(__x86_64__)
struct ts_flt {
	unsigned long long	regs[16];	/* rax, rbx, rcx, rdx, rsi, rdi, rbp, r8-r15, 0 */
	unsigned long long	sp, pc, pstate;
	unsigned long long	far, esr;	/* CR2; the vector << 32 | the error code */
	unsigned long long	fpsr, fpcr;
	unsigned long long	vregs[64] __attribute__((aligned(16)));
};
#else
struct ts_flt {
	unsigned long long	regs[31];
	unsigned long long	sp, pc, pstate;
	unsigned long long	far, esr;
	unsigned long long	fpsr, fpcr;
	unsigned long long	vregs[64] __attribute__((aligned(16)));
};
#endif

extern int ts_def_flt( void (*f)( struct ts_flt *flt ) );
extern int ts_flt_ret( const struct ts_flt *flt );
/* The browser's console ring put out at once (br_main.cc); absent in the other programs */
extern void br_console_flush_now( void ) __attribute__((weak));

static struct sigaction	acts[NSIG_TF];
static int		entry_set;

void ts_flt_entry( struct ts_flt *c );
void ts_flt_handle( struct ts_flt *c ) __attribute__((used, noreturn));
void ts_flt_back( const struct ts_flt *c ) __attribute__((noreturn));

/*
 * ts_flt_entry: the kept registers into the picture (still the task's
 * here), and the C part called with it; the stack is below the picture,
 * as for a function just called. ts_flt_back: the kept registers loaded
 * from the picture, and back to where it says (ts_flt_ret keeps them as
 * a function does).
 */
#if defined(__x86_64__)
_Static_assert(offsetof(struct ts_flt, regs[1]) == 8 && offsetof(struct ts_flt, regs[11]) == 88,
	       "rbx and r12 in the picture");
__asm__(
	".text\n"
	".p2align 4\n"
	".globl ts_flt_entry\n"
	".hidden ts_flt_entry\n"
	".type ts_flt_entry, @function\n"
"ts_flt_entry:\n"
	"movq %rbx, 8(%rdi)\n"
	"movq %r12, 88(%rdi)\n"
	"movq %r13, 96(%rdi)\n"
	"movq %r14, 104(%rdi)\n"
	"movq %r15, 112(%rdi)\n"
	"subq $8, %rsp\n"
	"call ts_flt_handle\n"
	"ud2\n"
	".size ts_flt_entry, .-ts_flt_entry\n"
	".p2align 4\n"
	".globl ts_flt_back\n"
	".hidden ts_flt_back\n"
	".type ts_flt_back, @function\n"
"ts_flt_back:\n"
	"movq 88(%rdi), %r12\n"
	"movq 96(%rdi), %r13\n"
	"movq 104(%rdi), %r14\n"
	"movq 112(%rdi), %r15\n"
	"movq 8(%rdi), %rbx\n"
	"subq $8, %rsp\n"
	"call ts_flt_ret\n"
	"1: jmp 1b\n"
	".size ts_flt_back, .-ts_flt_back\n"
);
#else
_Static_assert(offsetof(struct ts_flt, regs[19]) == 152, "x19 in the picture");
__asm__(
	".text\n"
	".p2align 2\n"
	".globl ts_flt_entry\n"
	".hidden ts_flt_entry\n"
	".type ts_flt_entry, %function\n"
"ts_flt_entry:\n"
	"stp x19, x20, [x0, #152]\n"
	"stp x21, x22, [x0, #168]\n"
	"stp x23, x24, [x0, #184]\n"
	"stp x25, x26, [x0, #200]\n"
	"stp x27, x28, [x0, #216]\n"
	"bl ts_flt_handle\n"
	"brk #0\n"
	".size ts_flt_entry, .-ts_flt_entry\n"
	".p2align 2\n"
	".globl ts_flt_back\n"
	".hidden ts_flt_back\n"
	".type ts_flt_back, %function\n"
"ts_flt_back:\n"
	"ldp x21, x22, [x0, #168]\n"
	"ldp x23, x24, [x0, #184]\n"
	"ldp x25, x26, [x0, #200]\n"
	"ldp x27, x28, [x0, #216]\n"
	"ldp x19, x20, [x0, #152]\n"
	"bl ts_flt_ret\n"
	"1: b 1b\n"
	".size ts_flt_back, .-ts_flt_back\n"
);
#endif

#if defined(__x86_64__)
/* The error code's P bit: the page was there and the access was not allowed */
#define PF_PRESENT(esr)	( ( (esr) & 1 ) != 0 )

/* The picture's registers and the places of the context's gregs they go to */
static const int greg_of[15] = {
	REG_RAX, REG_RBX, REG_RCX, REG_RDX, REG_RSI, REG_RDI, REG_RBP,
	REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15
};

void ts_flt_handle( struct ts_flt *c )
{
	int		sig = SIGSEGV;
	struct sigaction a = acts[sig];

	if ( ( a.sa_flags & SA_SIGINFO ) != 0 && a.sa_sigaction != NULL ) {
		siginfo_t	si;
		ucontext_t	uc;
		struct _libc_fpstate fp;
		int		i;

		memset(&si, 0, sizeof(si));
		si.si_signo = sig;
		si.si_code = PF_PRESENT(c->esr) ? SEGV_ACCERR : SEGV_MAPERR;
		si.si_addr = (void *)(unsigned long)c->far;
		memset(&uc, 0, sizeof(uc));
		for ( i = 0; i < 15; i++ ) uc.uc_mcontext.gregs[greg_of[i]] = (greg_t)c->regs[i];
		uc.uc_mcontext.gregs[REG_RSP] = (greg_t)c->sp;
		uc.uc_mcontext.gregs[REG_RIP] = (greg_t)c->pc;
		uc.uc_mcontext.gregs[REG_EFL] = (greg_t)c->pstate;
		uc.uc_mcontext.gregs[REG_ERR] = (greg_t)( c->esr & 0xffffffffULL );
		uc.uc_mcontext.gregs[REG_TRAPNO] = (greg_t)( c->esr >> 32 );
		uc.uc_mcontext.gregs[REG_CR2] = (greg_t)c->far;
		memcpy(&fp, c->vregs, sizeof(fp));
		uc.uc_mcontext.fpregs = &fp;
		if ( ( a.sa_flags & SA_RESETHAND ) != 0 ) {
			memset(&acts[sig], 0, sizeof(acts[sig]));
		}
		a.sa_sigaction(sig, &si, &uc);
		for ( i = 0; i < 15; i++ ) c->regs[i] = (unsigned long long)uc.uc_mcontext.gregs[greg_of[i]];
		c->sp = (unsigned long long)uc.uc_mcontext.gregs[REG_RSP];
		c->pc = (unsigned long long)uc.uc_mcontext.gregs[REG_RIP];
		c->pstate = (unsigned long long)uc.uc_mcontext.gregs[REG_EFL];
	}
#else
/* The data fault status: 0x21 an alignment fault; translation faults are of nothing there */
#define FSC(esr)	( (int)( (esr) & 0x3f ) )
#define FSC_ALIGN	0x21
#define FSC_TRANS(f)	( (f) >= 0x04 && (f) <= 0x07 )

void ts_flt_handle( struct ts_flt *c )
{
	int		sig = ( FSC(c->esr) == FSC_ALIGN ) ? SIGBUS : SIGSEGV;
	struct sigaction a = acts[sig];

	if ( ( a.sa_flags & SA_SIGINFO ) != 0 && a.sa_sigaction != NULL ) {
		siginfo_t	si;
		ucontext_t	uc;
		int		i;

		memset(&si, 0, sizeof(si));
		si.si_signo = sig;
		si.si_code = ( sig == SIGBUS ) ? BUS_ADRALN : FSC_TRANS(FSC(c->esr)) ? SEGV_MAPERR : SEGV_ACCERR;
		si.si_addr = (void *)(unsigned long)c->far;
		memset(&uc, 0, sizeof(uc));
		uc.uc_mcontext.fault_address = c->far;
		for ( i = 0; i < 31; i++ ) uc.uc_mcontext.regs[i] = c->regs[i];
		uc.uc_mcontext.sp = c->sp;
		uc.uc_mcontext.pc = c->pc;
		uc.uc_mcontext.pstate = c->pstate;
		if ( ( a.sa_flags & SA_RESETHAND ) != 0 ) {
			memset(&acts[sig], 0, sizeof(acts[sig]));
		}
		a.sa_sigaction(sig, &si, &uc);
		for ( i = 0; i < 31; i++ ) c->regs[i] = uc.uc_mcontext.regs[i];
		c->sp = uc.uc_mcontext.sp;
		c->pc = uc.uc_mcontext.pc;
		c->pstate = uc.uc_mcontext.pstate;
	}
#endif
	else if ( ( a.sa_flags & SA_SIGINFO ) == 0 && a.sa_handler != SIG_DFL && a.sa_handler != SIG_IGN
		    && a.sa_handler != NULL ) {
		a.sa_handler(sig);
	} else {
		/* the lines the browser keeps for its console put out first (br_main.cc) */
		if ( br_console_flush_now != NULL ) br_console_flush_now();
		(void)ts_def_flt(NULL);		/* the default: the access again ends the process */
		entry_set = 0;
	}
	ts_flt_back(c);
}

int sigaction( int sig, const struct sigaction *act, struct sigaction *old )
{
	if ( sig <= 0 || sig >= NSIG_TF ) {
		errno = EINVAL;
		return -1;
	}
	if ( old != NULL ) {
		*old = acts[sig];
	}
	if ( act != NULL ) {
		acts[sig] = *act;
		if ( ( sig == SIGSEGV || sig == SIGBUS ) && !entry_set && act->sa_handler != SIG_DFL
		     && act->sa_handler != SIG_IGN ) {
			if ( ts_def_flt(ts_flt_entry) == 0 ) entry_set = 1;
		}
	}
	return 0;
}

int sigaltstack( const stack_t *ss, stack_t *old )
{
	(void)ss;
	if ( old != NULL ) {
		memset(old, 0, sizeof(*old));
		old->ss_flags = SS_DISABLE;
	}
	return 0;
}
