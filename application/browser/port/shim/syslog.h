/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	syslog.h
 *	The system log of Linux. TessronOS has none for a program; the calls
 *	are accepted and write nothing (application/browser/port/compat/
 *	ts_linux.c).
 */

#ifndef __TS_SYSLOG_H__
#define __TS_SYSLOG_H__

#ifdef __cplusplus
extern "C" {
#endif

#define LOG_EMERG	0
#define LOG_ALERT	1
#define LOG_CRIT	2
#define LOG_ERR		3
#define LOG_WARNING	4
#define LOG_NOTICE	5
#define LOG_INFO	6
#define LOG_DEBUG	7

#define LOG_KERN	(0 << 3)
#define LOG_USER	(1 << 3)
#define LOG_DAEMON	(3 << 3)
#define LOG_AUTH	(4 << 3)
#define LOG_LOCAL0	(16 << 3)

#define LOG_PID		0x01
#define LOG_CONS	0x02
#define LOG_NDELAY	0x08
#define LOG_NOWAIT	0x10

void	openlog( const char *ident, int option, int facility );
void	syslog( int priority, const char *format, ... );
void	closelog( void );

#ifdef __cplusplus
}
#endif

#endif /* __TS_SYSLOG_H__ */
