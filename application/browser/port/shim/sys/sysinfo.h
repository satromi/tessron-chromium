/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	sys/sysinfo.h
 *	The system's figures (sysinfo, get_nprocs): the memory is what the
 *	kernel says a process may have, the processors those a process's
 *	threads are given.
 */

#ifndef __TS_SYS_SYSINFO_H__
#define __TS_SYS_SYSINFO_H__

#ifdef __cplusplus
extern "C" {
#endif

struct sysinfo {
	long		uptime;
	unsigned long	loads[3];
	unsigned long	totalram;
	unsigned long	freeram;
	unsigned long	sharedram;
	unsigned long	bufferram;
	unsigned long	totalswap;
	unsigned long	freeswap;
	unsigned short	procs;
	unsigned long	totalhigh;
	unsigned long	freehigh;
	unsigned int	mem_unit;
};

int	sysinfo( struct sysinfo *info );
int	get_nprocs( void );
int	get_nprocs_conf( void );

#ifdef __cplusplus
}
#endif

#endif /* __TS_SYS_SYSINFO_H__ */
