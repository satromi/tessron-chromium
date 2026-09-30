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
 *	Signals, which a process on TessronOS is never sent: a fault ends the
 *	process (design 9.11). The handlers the engine installs are kept, so
 *	that it reads back what it set, and are never called.
 */

#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <signal.h>

#define NSIG_TF		64

static struct sigaction	acts[NSIG_TF];

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
