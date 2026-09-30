/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	sys/ifunc.h
 *	The argument a Linux loader gives the resolver of an indirect
 *	function on arm64. A TessronOS program is linked statically and nothing
 *	resolves indirect functions, so the sources are compiled where they
 *	choose an implementation without one (memory tagging and branch
 *	targets are off); only the names are needed.
 */

#ifndef __TS_SYS_IFUNC_H__
#define __TS_SYS_IFUNC_H__

#define _IFUNC_ARG_HWCAP	(1ULL << 62)

typedef struct __ifunc_arg_t {
	unsigned long	_size;
	unsigned long	_hwcap;
	unsigned long	_hwcap2;
} __ifunc_arg_t;

#endif /* __TS_SYS_IFUNC_H__ */
