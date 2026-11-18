/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	ts_glibc.c
 *	The GNU C library's calls, for a Linux shared object loaded into the
 *	browser's process (ts_so.c, design 14.6.3).
 *
 *	A shared object made for Linux calls the GNU C library by name and
 *	sees its types as that library lays them out on the processor
 *	(aarch64, or x86-64). The calls
 *	here are what such an object is bound to: most go to the process's
 *	own C library (newlib, lib/libcxxrt and lib/libpthread of TessronOS)
 *	with the GNU C library's numbers and layouts put into the process's
 *	and back:
 *
 *	  - errno: a word of each thread's own that __errno_location gives,
 *	    set with Linux's numbers after a call that failed
 *	  - the flags of open and fcntl, the clocks, sysconf's names and the
 *	    system calls of syscall() are Linux's numbers
 *	  - struct tm, struct utsname, struct dl_phdr_info as glibc has them
 *	  - mutexes, condition variables, read-write locks and semaphores
 *	    live in the object's memory in glibc's sizes (a mutex 48 bytes
 *	    on aarch64 and 40 on x86-64, the others 48, 56 and 32): their
 *	    first word points to the process's own, made the first time
 *	    the object uses one (a mutex glibc's initialiser left all zero
 *	    is ready to use, as in glibc); pthread_once and the
 *	    attributes are kept in glibc's words themselves
 *	  - the character classes of the C locale (__ctype_b_loc) in glibc's
 *	    table and bits; the locale calls of the C++ library answer for
 *	    the C locale
 *	  - setjmp and longjmp keep the registers in glibc's jmp_buf
 *	  - the files Linux shows the processors in (/proc/cpuinfo,
 *	    /sys/devices/system/cpu/present) are made from the processors
 *	    the process has (on x86-64, from what CPUID says of them)
 *
 *	GCC's unwinder (libgcc_s.so.1), which an object may name for its
 *	exceptions, is the process's own (libgcc's _Unwind_ calls); ts_so.c
 *	makes the object's frames known to it.
 *
 *	Every symbol an object asks for is looked up by name in the table at
 *	the end (ts_glibc_sym); what is not there is not bound.
 */

#define _GNU_SOURCE	/* vasprintf, memfd_create */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <wchar.h>
#include <locale.h>
#include <sched.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/random.h>
#include <signal.h>
#include <wchar.h>

#include "ts_so.h"

#if defined(__aarch64__) || defined(__x86_64__)

/* What differs between the processors in glibc's layouts and Linux's numbers */
#if defined(__aarch64__)
#define G_MACHINE	"aarch64"
#define G_MUTEX_SIZE	48		/* sizeof(pthread_mutex_t) */
#define G_ATTR_SIZE	64		/* sizeof(pthread_attr_t) */
#define L_O_DIRECTORY	00040000
#define SYS_READ	63
#define SYS_WRITE	64
#define SYS_GETPID	172
#define SYS_GETTID	178
#define SYS_SCHED_YIELD	124
#define SYS_CLOCK_GETTIME 113
#define SYS_FUTEX	98
#define SYS_PRCTL	167
#define SYS_GETRANDOM	278
#define G_DYNLINKER	"ld-linux-aarch64.so.1"
#else
#define G_MACHINE	"x86_64"
#define G_MUTEX_SIZE	40
#define G_ATTR_SIZE	56
#define L_O_DIRECTORY	00200000
#define SYS_READ	0
#define SYS_WRITE	1
#define SYS_GETPID	39
#define SYS_GETTID	186
#define SYS_SCHED_YIELD	24
#define SYS_CLOCK_GETTIME 228
#define SYS_FUTEX	202
#define SYS_PRCTL	157
#define SYS_GETRANDOM	318
#define G_DYNLINKER	"ld-linux-x86-64.so.2"
#endif

extern int	tk_get_tid( void );
extern unsigned long getauxval( unsigned long type );

/* ---------------------------------------------------------------- errno */

static __thread int	g_errno;

/* The process's error number as Linux numbers it */
static int to_linux_errno( int e )
{
	switch ( e ) {
	case EDEADLK:		return 35;
	case ENAMETOOLONG:	return 36;
	case ENOSYS:		return 38;
	case ENOTEMPTY:		return 39;
	case EILSEQ:		return 84;
	case EOVERFLOW:		return 75;
	case ETIMEDOUT:		return 110;
	case ENOTSUP:		return 95;
	default:		return e;	/* 1 to 34 are the same */
	}
}

static int *g___errno_location( void )
{
	return &g_errno;
}

/* The result of a call that says -1 on failure, errno taken over */
static long ret_errno( long r )
{
	if ( r < 0 ) g_errno = to_linux_errno(errno);
	return r;
}

static void note( const char *what, const char *name, long n )
{
	printf("browser: glibc: %s %s %ld\n", what, name != NULL ? name : "", n);
}

/* ---------------------------------------------------------------- memory and strings */

static void *g___libc_memalign( size_t align, size_t size )
{
	void *p = NULL;
	if ( align < sizeof(void *) ) align = sizeof(void *);
	if ( posix_memalign(&p, align, size) != 0 ) {
		g_errno = 12;
		return NULL;
	}
	return p;
}

static void chk_fail( const char *what )
{
	printf("browser: glibc: %s: a buffer overflow was detected\n", what);
	abort();
}

static void *g___memcpy_chk( void *d, const void *s, size_t n, size_t dn )
{
	if ( n > dn ) chk_fail("__memcpy_chk");
	return memcpy(d, s, n);
}

static void *g___memset_chk( void *d, int c, size_t n, size_t dn )
{
	if ( n > dn ) chk_fail("__memset_chk");
	return memset(d, c, n);
}

static int g___vsnprintf_chk( char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list ap )
{
	(void)flag;
	if ( maxlen > slen ) chk_fail("__vsnprintf_chk");
	return vsnprintf(s, maxlen, fmt, ap);
}

static int g___vasprintf_chk( char **strp, int flag, const char *fmt, va_list ap )
{
	(void)flag;
	return vasprintf(strp, fmt, ap);
}

static int g___vfprintf_chk( FILE *fp, int flag, const char *fmt, va_list ap )
{
	(void)flag;
	return vfprintf(fp, fmt, ap);
}

static char *g___fgets_chk( char *buf, size_t size, int n, FILE *fp )
{
	if ( (size_t)n > size ) chk_fail("__fgets_chk");
	return fgets(buf, n, fp);
}

static void g___stack_chk_fail( void )
{
	printf("browser: glibc: the stack of a loaded object was overwritten\n");
	abort();
}

static uintptr_t	g___stack_chk_guard;

static void g_abort( void )
{
	printf("browser: glibc: a loaded object called abort\n");
	abort();
}

static void g_perror( const char *s )
{
	printf("browser: glibc: %s%s%s\n", s ? s : "", s ? ": " : "", strerror(errno));
}

/* strerror_r as glibc's GNU variant: the text, maybe not in buf */
static char *g_strerror_r( int e, char *buf, size_t n )
{
	snprintf(buf, n, "error %d", e);
	return buf;
}

static long g_strtol( const char *s, char **end, int base )
{
	long r;
	errno = 0;
	r = strtol(s, end, base);
	if ( errno != 0 ) g_errno = to_linux_errno(errno);
	return r;
}

static long long g_strtoll_l( const char *s, char **end, int base, void *loc )
{
	long long r;
	(void)loc;
	errno = 0;
	r = strtoll(s, end, base);
	if ( errno != 0 ) g_errno = to_linux_errno(errno);
	return r;
}

static unsigned long long g_strtoull_l( const char *s, char **end, int base, void *loc )
{
	unsigned long long r;
	(void)loc;
	errno = 0;
	r = strtoull(s, end, base);
	if ( errno != 0 ) g_errno = to_linux_errno(errno);
	return r;
}

static double g_strtod_l( const char *s, char **end, void *loc )
{
	double r;
	(void)loc;
	errno = 0;
	r = strtod(s, end);
	if ( errno != 0 ) g_errno = to_linux_errno(errno);
	return r;
}

static float g_strtof_l( const char *s, char **end, void *loc )
{
	float r;
	(void)loc;
	errno = 0;
	r = strtof(s, end);
	if ( errno != 0 ) g_errno = to_linux_errno(errno);
	return r;
}

static long double g_strtold_l( const char *s, char **end, void *loc )
{
	long double r;
	(void)loc;
	errno = 0;
	r = strtold(s, end);
	if ( errno != 0 ) g_errno = to_linux_errno(errno);
	return r;
}

static unsigned long g_strtoul( const char *s, char **end, int base )
{
	unsigned long r;
	errno = 0;
	r = strtoul(s, end, base);
	if ( errno != 0 ) g_errno = to_linux_errno(errno);
	return r;
}

static unsigned long long g_strtoull( const char *s, char **end, int base )
{
	unsigned long long r;
	errno = 0;
	r = strtoull(s, end, base);
	if ( errno != 0 ) g_errno = to_linux_errno(errno);
	return r;
}

static char *g___strncat_chk( char *d, const char *src, size_t n, size_t dn )
{
	size_t dl = strnlen(d, dn), sl = strnlen(src, n);
	if ( dl + sl + 1 > dn ) chk_fail("__strncat_chk");
	return strncat(d, src, n);
}

static int g_vasprintf( char **strp, const char *fmt, va_list ap )
{
	int r = vasprintf(strp, fmt, ap);
	if ( r < 0 ) g_errno = 12;
	return r;
}

static size_t g___mbrlen( const char *s, size_t n, mbstate_t *ps )
{
	return mbrlen(s, n, ps);
}

static int g___isoc99_vsscanf( const char *s, const char *fmt, va_list ap )
{
	return vsscanf(s, fmt, ap);
}

static int g___isoc99_sscanf( const char *s, const char *fmt, ... )
{
	va_list ap;
	int r;
	va_start(ap, fmt);
	r = vsscanf(s, fmt, ap);
	va_end(ap);
	return r;
}

static char *g_getenv( const char *name )
{
	(void)name;
	return NULL;
}

/* ---------------------------------------------------------------- locales and character classes */

/* glibc's bits of a character's classes (little-endian) */
#define G_UPPER		0x0100
#define G_LOWER		0x0200
#define G_ALPHA		0x0400
#define G_DIGIT		0x0800
#define G_XDIGIT	0x1000
#define G_SPACE		0x2000
#define G_PRINT		0x4000
#define G_GRAPH		0x8000
#define G_BLANK		0x0001
#define G_CNTRL		0x0002
#define G_PUNCT		0x0004
#define G_ALNUM		0x0008

static unsigned short		ctype_tab[384];		/* -128 to 255 */
static const unsigned short	*ctype_ptr;
static int32_t			lower_tab[384], upper_tab[384];	/* -128 to 255: the C locale's case maps */
static const int32_t		*lower_ptr, *upper_ptr;

static void ctype_make( void )
{
	int c;

	for ( c = 0; c < 128; c++ ) {
		unsigned short b = 0;
		int up = c >= 'A' && c <= 'Z', lo = c >= 'a' && c <= 'z', dg = c >= '0' && c <= '9';
		if ( up ) b |= G_UPPER;
		if ( lo ) b |= G_LOWER;
		if ( up || lo ) b |= G_ALPHA | G_ALNUM;
		if ( dg ) b |= G_DIGIT | G_ALNUM | G_XDIGIT;
		if ( ( c >= 'a' && c <= 'f' ) || ( c >= 'A' && c <= 'F' ) ) b |= G_XDIGIT;
		if ( c == ' ' || ( c >= '\t' && c <= '\r' ) ) b |= G_SPACE;
		if ( c >= 0x20 && c < 0x7f ) b |= G_PRINT;
		if ( c > 0x20 && c < 0x7f ) b |= G_GRAPH;
		if ( c == ' ' || c == '\t' ) b |= G_BLANK;
		if ( c < 0x20 || c == 0x7f ) b |= G_CNTRL;
		if ( c > 0x20 && c < 0x7f && !up && !lo && !dg ) b |= G_PUNCT;
		ctype_tab[128 + c] = b;
	}
	ctype_ptr = ctype_tab + 128;
	for ( c = -128; c < 256; c++ ) {
		lower_tab[128 + c] = ( c >= 'A' && c <= 'Z' ) ? c + 32 : c;
		upper_tab[128 + c] = ( c >= 'a' && c <= 'z' ) ? c - 32 : c;
	}
	lower_ptr = lower_tab + 128;
	upper_ptr = upper_tab + 128;
}

static const int32_t **g___ctype_tolower_loc( void )
{
	return &lower_ptr;
}

static const int32_t **g___ctype_toupper_loc( void )
{
	return &upper_ptr;
}

static const unsigned short **g___ctype_b_loc( void )
{
	return &ctype_ptr;
}

static size_t g___ctype_get_mb_cur_max( void )
{
	return 1;
}

/* A locale: the C locale, whatever is asked */
static struct { int dummy; }	c_locale;

static void *g_newlocale( int mask, const char *name, void *base )
{
	(void)mask;
	(void)name;
	(void)base;
	return &c_locale;
}

static void g_freelocale( void *loc )
{
	(void)loc;
}

static void *g_uselocale( void *loc )
{
	(void)loc;
	return (void *)-1L;	/* LC_GLOBAL_LOCALE */
}

static void *g_catopen( const char *name, int flag )
{
	(void)name;
	(void)flag;
	g_errno = 2;
	return (void *)-1L;
}

static char *g_catgets( void *cat, int set, int num, const char *s )
{
	(void)cat;
	(void)set;
	(void)num;
	return (char *)s;
}

static int g_catclose( void *cat )
{
	(void)cat;
	return 0;
}

/* ---------------------------------------------------------------- time */

/* struct tm as glibc has it */
struct g_tm {
	int		tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
	long		tm_gmtoff;
	const char	*tm_zone;
};

static struct g_tm *to_g_tm( const struct tm *t, struct g_tm *g, long off, const char *zone )
{
	g->tm_sec = t->tm_sec;
	g->tm_min = t->tm_min;
	g->tm_hour = t->tm_hour;
	g->tm_mday = t->tm_mday;
	g->tm_mon = t->tm_mon;
	g->tm_year = t->tm_year;
	g->tm_wday = t->tm_wday;
	g->tm_yday = t->tm_yday;
	g->tm_isdst = t->tm_isdst;
	g->tm_gmtoff = off;
	g->tm_zone = zone;
	return g;
}

static void from_g_tm( const struct g_tm *g, struct tm *t )
{
	memset(t, 0, sizeof(*t));
	t->tm_sec = g->tm_sec;
	t->tm_min = g->tm_min;
	t->tm_hour = g->tm_hour;
	t->tm_mday = g->tm_mday;
	t->tm_mon = g->tm_mon;
	t->tm_year = g->tm_year;
	t->tm_wday = g->tm_wday;
	t->tm_yday = g->tm_yday;
	t->tm_isdst = g->tm_isdst;
}

static struct g_tm *g_gmtime_r( const time_t *tp, struct g_tm *g )
{
	struct tm t;
	if ( gmtime_r(tp, &t) == NULL ) return NULL;
	return to_g_tm(&t, g, 0, "GMT");
}

static struct g_tm *g_localtime_r( const time_t *tp, struct g_tm *g )
{
	struct tm t, u;
	long off;
	if ( localtime_r(tp, &t) == NULL || gmtime_r(tp, &u) == NULL ) return NULL;
	off = (long)( mktime(&t) - mktime(&u) );
	return to_g_tm(&t, g, off, "");
}

static time_t g_mktime( struct g_tm *g )
{
	struct tm	t;
	time_t		r;

	from_g_tm(g, &t);
	r = mktime(&t);
	if ( r != (time_t)-1 ) {
		struct tm u;
		long off = ( gmtime_r(&r, &u) != NULL ) ? (long)( r - timegm(&u) ) : 0;
		to_g_tm(&t, g, off, "");
	}
	return r;
}

static time_t g_timegm( struct g_tm *g )
{
	struct tm	t;
	time_t		r;

	from_g_tm(g, &t);
	r = timegm(&t);
	if ( r != (time_t)-1 ) to_g_tm(&t, g, 0, "GMT");
	return r;
}

static size_t g_strftime_l( char *s, size_t max, const char *fmt, const struct g_tm *g, void *loc )
{
	struct tm t;
	(void)loc;
	from_g_tm(g, &t);
	return strftime(s, max, fmt, &t);
}

static clockid_t clock_of( int linux_clock )
{
	switch ( linux_clock ) {
	case 0:	case 5:		return CLOCK_REALTIME;
	case 2:			return CLOCK_PROCESS_CPUTIME_ID;
	case 3:			return CLOCK_THREAD_CPUTIME_ID;
	default:		return CLOCK_MONOTONIC;		/* 1, 4, 6, 7 */
	}
}

static int g_clock_gettime( int clk, struct timespec *ts )
{
	return (int)ret_errno(clock_gettime(clock_of(clk), ts));
}

static int g_gettimeofday( struct timeval *tv, void *tz )
{
	(void)tz;
	return (int)ret_errno(gettimeofday(tv, NULL));
}

static int g_nanosleep( const struct timespec *req, struct timespec *rem )
{
	return (int)ret_errno(nanosleep(req, rem));
}

static int g_usleep( unsigned us )
{
	return (int)ret_errno(usleep(us));
}

/* ---------------------------------------------------------------- the processors */

static int ncpus( void )
{
	long n = sysconf(_SC_NPROCESSORS_ONLN);
	return ( n >= 1 && n <= 64 ) ? (int)n : 1;
}

static long g_sysconf( int name )
{
	switch ( name ) {
	case 2:		return 100;			/* _SC_CLK_TCK */
	case 4:		return 1024;			/* _SC_OPEN_MAX */
	case 30:	return 4096;			/* _SC_PAGESIZE */
	case 69: case 70: return 1024;			/* _SC_GETGR_R_SIZE_MAX, _SC_GETPW_R_SIZE_MAX */
	case 83: case 84: return ncpus();		/* _SC_NPROCESSORS_CONF, _ONLN */
	case 85: case 86: {				/* _SC_PHYS_PAGES, _SC_AVPHYS_PAGES */
		long n = sysconf(_SC_PHYS_PAGES);
		return ( n > 0 ) ? n : ( 1L << 20 );
	}
	default:
		note("sysconf of", "", name);
		g_errno = 22;
		return -1;
	}
}

static unsigned long g_getauxval( unsigned long type )
{
	switch ( type ) {
	case 6:		return 4096;		/* AT_PAGESZ */
	case 16:	return getauxval(16);	/* AT_HWCAP */
	case 26:	return getauxval(26);	/* AT_HWCAP2 */
	case 17:	return 100;		/* AT_CLKTCK */
	case 23:	return 0;		/* AT_SECURE */
	default:
		g_errno = 2;
		return 0;
	}
}

struct g_utsname {
	char	sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
};

static int g_uname( struct g_utsname *u )
{
	memset(u, 0, sizeof(*u));
	strcpy(u->sysname, "Linux");
	strcpy(u->nodename, "tessronos");
	strcpy(u->release, "6.6.0");
	strcpy(u->version, "#1 SMP TessronOS");
	strcpy(u->machine, G_MACHINE);
	return 0;
}

static int g_getpid( void )
{
	return (int)getpid();
}

static unsigned g_geteuid( void )
{
	return (unsigned)getuid();
}

static int g_getpagesize( void )
{
	return 4096;
}

/* The sockets the process may hold, soft and hard (include/ts/soapp.h); what getrlimit reads (ts_linux.c) */
extern int	so_getlimit( int *p_cur, int *p_max );

/* glibc's struct rlimit (two 64-bit words); RLIM_INFINITY for what the system does not limit */
static int g_getrlimit64( int res, uint64_t *rl )
{
	int	cur, max;

	switch ( res ) {
	case 3:		/* RLIMIT_STACK: the main thread's stack */
		rl[0] = rl[1] = 8u << 20;
		return 0;
	case 7:		/* RLIMIT_NOFILE: the limit the process's own getrlimit tells */
		if ( so_getlimit(&cur, &max) >= 0 ) {
			rl[0] = (uint64_t)cur;
			rl[1] = (uint64_t)max;
		} else {
			rl[0] = rl[1] = 1024;
		}
		return 0;
	default:
		rl[0] = rl[1] = ~(uint64_t)0;
		return 0;
	}
}

static int g_setpriority( int which, unsigned who, int prio )
{
	(void)which;
	(void)who;
	(void)prio;
	return 0;	/* the process's tasks keep their priorities */
}

static int g_sched_setscheduler( int pid, int policy, const void *param )
{
	(void)pid;
	(void)param;
	if ( policy == 0 ) return 0;	/* SCHED_OTHER, as it is */
	g_errno = 1;			/* EPERM: no real-time policy for a loaded object */
	return -1;
}

/* glibc's cpu_set_t: a bit a processor; every processor the process has */
static int cpus_fill( size_t size, unsigned char *set )
{
	int i;
	memset(set, 0, size);
	for ( i = 0; i < ncpus() && (size_t)( i / 8 ) < size; i++ ) set[i / 8] |= (unsigned char)( 1u << ( i % 8 ) );
	return 0;
}

static int g_sched_getaffinity( int pid, size_t size, unsigned char *set )
{
	(void)pid;
	return cpus_fill(size, set);
}

static int g_pthread_getaffinity_np( unsigned long th, size_t size, unsigned char *set )
{
	(void)th;
	return cpus_fill(size, set);
}

static int g___sched_cpucount( size_t size, const unsigned char *set )
{
	int n = 0;
	for ( size_t i = 0; i < size; i++ ) n += __builtin_popcount(set[i]);
	return n;
}

static void g__exit( int code )
{
	printf("browser: glibc: a loaded object called _exit(%d)\n", code);
	_exit(code);
}

static int g_prctl( int option, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5 )
{
	(void)option;
	(void)a2;
	(void)a3;
	(void)a4;
	(void)a5;
	return 0;
}

static int g___register_atfork( void (*prepare)( void ), void (*parent)( void ), void (*child)( void ), void *dso )
{
	(void)prepare;
	(void)parent;
	(void)child;
	(void)dso;
	return 0;	/* a TessronOS process never forks */
}

/* ---------------------------------------------------------------- files */

/* Linux's flags of open and their place in the process's */
#define L_O_ACCMODE	00000003
#define L_O_CREAT	00000100
#define L_O_EXCL	00000200
#define L_O_TRUNC	00001000
#define L_O_APPEND	00002000
#define L_O_NONBLOCK	00004000
#define L_O_CLOEXEC	02000000

static int open_flags( int f )
{
	int o = f & L_O_ACCMODE;
	if ( f & L_O_CREAT ) o |= O_CREAT;
	if ( f & L_O_EXCL ) o |= O_EXCL;
	if ( f & L_O_TRUNC ) o |= O_TRUNC;
	if ( f & L_O_APPEND ) o |= O_APPEND;
	if ( f & L_O_NONBLOCK ) o |= O_NONBLOCK;
	if ( f & L_O_DIRECTORY ) o |= O_DIRECTORY;
	if ( f & L_O_CLOEXEC ) o |= O_CLOEXEC;
	return o;
}

#if defined(__x86_64__)
static void cpuid( unsigned leaf, unsigned sub, unsigned r[4] )
{
	__asm__ volatile ( "cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub) );
}

/*
 * /proc/cpuinfo as Linux writes it on x86-64: the vendor, the family,
 * model and stepping, the brand string and the flags of the features
 * CPUID reports (AVX and AVX2 only when the system has their registers
 * enabled in XCR0), one block a processor
 */
static size_t x86_cpuinfo( char *text, size_t cap )
{
	static const struct { unsigned char leaf, reg, bit; const char *name; } fl[] = {
		{ 1, 3, 0, "fpu" }, { 1, 3, 4, "tsc" }, { 1, 3, 8, "cx8" }, { 1, 3, 15, "cmov" },
		{ 1, 3, 19, "clflush" }, { 1, 3, 23, "mmx" }, { 1, 3, 24, "fxsr" }, { 1, 3, 25, "sse" },
		{ 1, 3, 26, "sse2" }, { 1, 3, 28, "ht" }, { 1, 2, 0, "pni" }, { 1, 2, 1, "pclmulqdq" },
		{ 1, 2, 9, "ssse3" }, { 1, 2, 12, "fma" }, { 1, 2, 13, "cx16" }, { 1, 2, 19, "sse4_1" },
		{ 1, 2, 20, "sse4_2" }, { 1, 2, 22, "movbe" }, { 1, 2, 23, "popcnt" }, { 1, 2, 25, "aes" },
		{ 1, 2, 26, "xsave" }, { 1, 2, 28, "avx" }, { 1, 2, 29, "f16c" }, { 1, 2, 30, "rdrand" },
		{ 7, 1, 3, "bmi1" }, { 7, 1, 5, "avx2" }, { 7, 1, 8, "bmi2" }, { 7, 1, 9, "erms" },
		{ 7, 1, 18, "rdseed" }, { 7, 1, 19, "adx" }, { 7, 1, 29, "sha_ni" },
	};
	unsigned	r0[4], r1[4], r7[4] = { 0, 0, 0, 0 }, b[12];
	char		vendor[13], brand[49], flags[512];
	size_t		n = 0, f = 0, k;
	int		i, family, model, avx;

	cpuid(0, 0, r0);
	memcpy(vendor, &r0[1], 4);
	memcpy(vendor + 4, &r0[3], 4);
	memcpy(vendor + 8, &r0[2], 4);
	vendor[12] = 0;
	cpuid(1, 0, r1);
	if ( r0[0] >= 7 ) cpuid(7, 0, r7);
	family = (int)( ( r1[0] >> 8 ) & 15 );
	model = (int)( ( r1[0] >> 4 ) & 15 );
	if ( family == 15 ) family += (int)( ( r1[0] >> 20 ) & 0xff );
	if ( family >= 6 ) model |= (int)( ( r1[0] >> 12 ) & 0xf0 );
	brand[0] = 0;
	{
		unsigned e[4];
		cpuid(0x80000000u, 0, e);
		if ( e[0] >= 0x80000004u ) {
			cpuid(0x80000002u, 0, b);
			cpuid(0x80000003u, 0, b + 4);
			cpuid(0x80000004u, 0, b + 8);
			memcpy(brand, b, 48);
			brand[48] = 0;
		}
	}
	/* AVX's registers usable: OSXSAVE, and XCR0 with the SSE and AVX state */
	avx = 0;
	if ( r1[2] & ( 1u << 27 ) ) {
		unsigned lo, hi;
		__asm__ volatile ( "xgetbv" : "=a"(lo), "=d"(hi) : "c"(0) );
		avx = ( lo & 6 ) == 6;
	}
	flags[0] = 0;
	for ( k = 0; k < sizeof(fl) / sizeof(fl[0]); k++ ) {
		unsigned v = ( fl[k].leaf == 1 ) ? r1[fl[k].reg] : r7[fl[k].reg];
		if ( !( v & ( 1u << fl[k].bit ) ) ) continue;
		if ( !avx && ( strcmp(fl[k].name, "avx") == 0 || strcmp(fl[k].name, "avx2") == 0
			       || strcmp(fl[k].name, "fma") == 0 || strcmp(fl[k].name, "f16c") == 0 ) ) continue;
		f += (size_t)snprintf(flags + f, sizeof(flags) - f, "%s%s", f ? " " : "", fl[k].name);
		if ( f >= sizeof(flags) ) break;
	}
	for ( i = 0; i < ncpus() && n + 800 < cap; i++ ) {
		n += (size_t)snprintf(text + n, cap - n,
			"processor\t: %d\nvendor_id\t: %s\ncpu family\t: %d\nmodel\t\t: %d\nmodel name\t: %s\n"
			"stepping\t: %u\nphysical id\t: 0\ncore id\t\t: %d\ncpu cores\t: %d\nflags\t\t: %s\n\n",
			i, vendor, family, model, brand, r1[0] & 15, i, ncpus(), flags);
	}
	return n;
}
#endif

/* What Linux shows of the processors: the file made from the processors there are, -1 for another path */
static int virtual_file( const char *path )
{
	char	text[4096];
	size_t	n = 0;
	int	fd, i;

	if ( strcmp(path, "/proc/cpuinfo") == 0 ) {
#if defined(__x86_64__)
		(void)i;
		n = x86_cpuinfo(text, sizeof(text));
#else
		for ( i = 0; i < ncpus() && n < sizeof(text) - 400; i++ ) {
			n += (size_t)snprintf(text + n, sizeof(text) - n,
				"processor\t: %d\nBogoMIPS\t: 108.00\n"
				"Features\t: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid "
				"asimdrdm lrcpc dcpop asimddp\nCPU implementer\t: 0x41\nCPU architecture: 8\n"
				"CPU variant\t: 0x4\nCPU part\t: 0xd0b\nCPU revision\t: 1\n\n", i);
		}
#endif
	} else if ( strcmp(path, "/sys/devices/system/cpu/present") == 0
		    || strcmp(path, "/sys/devices/system/cpu/possible") == 0
		    || strcmp(path, "/sys/devices/system/cpu/online") == 0 ) {
		n = ( ncpus() > 1 ) ? (size_t)snprintf(text, sizeof(text), "0-%d\n", ncpus() - 1)
				    : (size_t)snprintf(text, sizeof(text), "0\n");
	} else {
		return -1;
	}
	fd = memfd_create("glibc", 0);
	if ( fd < 0 ) return -2;
	if ( write(fd, text, n) != (ssize_t)n || lseek(fd, 0, SEEK_SET) != 0 ) {
		close(fd);
		return -2;
	}
	return fd;
}

static int g___open64_2( const char *path, int flags )
{
	int fd = virtual_file(path);
	if ( fd >= 0 ) return fd;
	note("open", path, flags);
	return (int)ret_errno(open(path, open_flags(flags), 0));
}

static int g_open64( const char *path, int flags, ... )
{
	va_list		ap;
	unsigned	mode = 0;
	int		fd = virtual_file(path);

	if ( fd >= 0 ) return fd;
	if ( flags & L_O_CREAT ) {
		va_start(ap, flags);
		mode = va_arg(ap, unsigned);
		va_end(ap);
	}
	note("open", path, flags);
	return (int)ret_errno(open(path, open_flags(flags), (mode_t)mode));
}

static int g_creat64( const char *path, unsigned mode )
{
	note("creat", path, (long)mode);
	return (int)ret_errno(open(path, O_CREAT | O_WRONLY | O_TRUNC, (mode_t)mode));
}

static long g_pread64( int fd, void *buf, size_t n, long off )
{
	return ret_errno(pread(fd, buf, n, (off_t)off));
}

/* stat64 and fstat64 with Linux's struct stat (ts_libc.c) */
extern int	l_stat64( const char *path, void *st ) __asm__("stat64");
extern int	l_fstat64( int fd, void *st ) __asm__("fstat64");

static int g___xstat64( int ver, const char *path, void *st )
{
	(void)ver;
	return (int)ret_errno(l_stat64(path, st));
}

static int g___fxstat64( int ver, int fd, void *st )
{
	(void)ver;
	return (int)ret_errno(l_fstat64(fd, st));
}

static FILE *g_fopen64( const char *path, const char *mode )
{
	int	fd = virtual_file(path);
	FILE	*f;

	if ( fd >= 0 ) {
		f = fdopen(fd, "r");
		if ( f == NULL ) close(fd);
		return f;
	}
	note("fopen", path, 0);
	f = fopen(path, mode);
	if ( f == NULL ) g_errno = to_linux_errno(errno);
	return f;
}

static long g_read( int fd, void *buf, size_t n )
{
	return ret_errno(read(fd, buf, n));
}

static long g_write( int fd, const void *buf, size_t n )
{
	return ret_errno(write(fd, buf, n));
}

static int g_fcntl64( int fd, int cmd, long arg )
{
	int r;

	switch ( cmd ) {
	case 1:		/* F_GETFD */
		return (int)ret_errno(fcntl(fd, F_GETFD));
	case 2:		/* F_SETFD */
		return (int)ret_errno(fcntl(fd, F_SETFD, (int)arg));
	case 3:		/* F_GETFL */
		r = fcntl(fd, F_GETFL);
		if ( r < 0 ) return (int)ret_errno(r);
		return ( r & O_ACCMODE ) | ( ( r & O_NONBLOCK ) ? L_O_NONBLOCK : 0 ) | ( ( r & O_APPEND ) ? L_O_APPEND : 0 );
	case 4:		/* F_SETFL */
		return (int)ret_errno(fcntl(fd, F_SETFL, open_flags((int)arg) & ~( O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC )));
	default:
		note("fcntl", "", cmd);
		g_errno = 22;
		return -1;
	}
}

static void *g_mmap64( void *addr, size_t len, int prot, int flags, int fd, long off )
{
	void *p = mmap(addr, len, prot, flags, fd, (off_t)off);
	if ( p == MAP_FAILED ) g_errno = to_linux_errno(errno);
	return p;
}

static int g_mprotect( void *addr, size_t len, int prot )
{
	return (int)ret_errno(mprotect(addr, len, prot));	/* PROT_READ, WRITE and EXEC are Linux's numbers */
}

static int g_munmap( void *addr, size_t len )
{
	return (int)ret_errno(munmap(addr, len));
}

static int g_madvise( void *addr, size_t len, int advice )
{
	return (int)ret_errno(madvise(addr, len, advice));
}

static FILE	*g_stdin, *g_stdout, *g_stderr;

/* The dynamic linker's variable: the top of the main thread's stack (lib/libpthread's main thread) */
static void	*g___libc_stack_end;
extern void	*pth_main( void );

static void stack_end_make( void )
{
	pthread_attr_t	a;
	void		*lo = NULL;
	size_t		sz = 0;

	if ( pthread_getattr_np((pthread_t)pth_main(), &a) == 0 && pthread_attr_getstack(&a, &lo, &sz) == 0 ) {
		g___libc_stack_end = (char *)lo + sz;
	}
}
static char	*g_program_invocation_short_name = (char *)"browser";

/* ---------------------------------------------------------------- threads */

/*
 * glibc's synchronising objects hold, in their first word, the process's
 * own object, made the first time it is used
 */
static void *lazy_obj( void **slot, size_t size, void (*make)( void *obj, const void *g ),
		       void (*unmake)( void *obj ) )
{
	void *p = __atomic_load_n(slot, __ATOMIC_ACQUIRE), *have = NULL;

	if ( p != NULL ) return p;
	p = calloc(1, size);
	if ( p == NULL ) {
		printf("browser: glibc: no memory for a lock\n");
		abort();
	}
	make(p, slot);
	if ( __atomic_compare_exchange_n(slot, &have, p, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) ) return p;
	unmake(p);
	free(p);
	return have;
}

static void mutex_make( void *obj, const void *g )
{
	/* glibc's kind of mutex is its fifth word: 1 recursive, 2 checking errors */
	int kind = ((const int *)g)[4] & 3;
	pthread_mutexattr_t a;
	pthread_mutexattr_init(&a);
	if ( kind == 1 ) pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
	if ( kind == 2 ) pthread_mutexattr_settype(&a, PTHREAD_MUTEX_ERRORCHECK);
	pthread_mutex_init((pthread_mutex_t *)obj, &a);
	pthread_mutexattr_destroy(&a);
}

static void mutex_unmake( void *obj )
{
	pthread_mutex_destroy((pthread_mutex_t *)obj);
}

static pthread_mutex_t *mutex_of( void *g )
{
	return (pthread_mutex_t *)lazy_obj((void **)g, sizeof(pthread_mutex_t), mutex_make, mutex_unmake);
}

static int g_pthread_mutexattr_init( int *a )
{
	*a = 0;
	return 0;
}

static int g_pthread_mutexattr_destroy( int *a )
{
	(void)a;
	return 0;
}

static int g_pthread_mutex_init( void *g, const int *attr )
{
	memset(g, 0, G_MUTEX_SIZE);
	((int *)g)[4] = ( attr != NULL ) ? ( *attr & 3 ) : 0;
	(void)mutex_of(g);
	return 0;
}

static int g_pthread_mutex_destroy( void *g )
{
	void *p = __atomic_exchange_n((void **)g, NULL, __ATOMIC_ACQ_REL);
	if ( p != NULL ) {
		mutex_unmake(p);
		free(p);
	}
	return 0;
}

static int g_pthread_mutex_lock( void *g )
{
	return to_linux_errno(pthread_mutex_lock(mutex_of(g)));
}

static int g_pthread_mutex_trylock( void *g )
{
	return to_linux_errno(pthread_mutex_trylock(mutex_of(g)));
}

static int g_pthread_mutex_unlock( void *g )
{
	return to_linux_errno(pthread_mutex_unlock(mutex_of(g)));
}

/*
 * The clock a condition's waits are timed with: glibc's word after the
 * pointer holds it (1 + Linux's clock), 0 for the default (CLOCK_REALTIME)
 */
static clockid_t cond_clock( const void *g )
{
	return ( ((const int *)g)[2] == 2 ) ? CLOCK_MONOTONIC : CLOCK_REALTIME;
}

static void cond_make( void *obj, const void *g )
{
	(void)g;
	pthread_cond_init((pthread_cond_t *)obj, NULL);
}

static void cond_unmake( void *obj )
{
	pthread_cond_destroy((pthread_cond_t *)obj);
}

static pthread_cond_t *cond_of( void *g )
{
	return (pthread_cond_t *)lazy_obj((void **)g, sizeof(pthread_cond_t), cond_make, cond_unmake);
}

/* glibc's pthread_condattr_t: one int; here the clock's Linux number plus 1 */
static int g_pthread_condattr_init( int *a )
{
	*a = 0;
	return 0;
}

static int g_pthread_condattr_destroy( int *a )
{
	(void)a;
	return 0;
}

static int g_pthread_condattr_setclock( int *a, int clk )
{
	if ( clk != 0 && clk != 1 ) return 22;
	*a = clk + 1;
	return 0;
}

static int g_pthread_cond_init( void *g, const int *attr )
{
	memset(g, 0, 48);
	((int *)g)[2] = ( attr != NULL ) ? *attr : 0;
	(void)cond_of(g);
	return 0;
}

static int g_pthread_cond_timedwait( void *c, void *m, const struct timespec *abstime )
{
	return to_linux_errno(pthread_cond_clockwait(cond_of(c), mutex_of(m), cond_clock(c), abstime));
}

static int g_pthread_cond_destroy( void *g )
{
	void *p = __atomic_exchange_n((void **)g, NULL, __ATOMIC_ACQ_REL);
	if ( p != NULL ) {
		cond_unmake(p);
		free(p);
	}
	return 0;
}

static int g_pthread_cond_wait( void *c, void *m )
{
	return to_linux_errno(pthread_cond_wait(cond_of(c), mutex_of(m)));
}

static int g_pthread_cond_signal( void *c )
{
	return to_linux_errno(pthread_cond_signal(cond_of(c)));
}

static int g_pthread_cond_broadcast( void *c )
{
	return to_linux_errno(pthread_cond_broadcast(cond_of(c)));
}

static void rw_make( void *obj, const void *g )
{
	(void)g;
	pthread_rwlock_init((pthread_rwlock_t *)obj, NULL);
}

static void rw_unmake( void *obj )
{
	pthread_rwlock_destroy((pthread_rwlock_t *)obj);
}

static pthread_rwlock_t *rw_of( void *g )
{
	return (pthread_rwlock_t *)lazy_obj((void **)g, sizeof(pthread_rwlock_t), rw_make, rw_unmake);
}

static int g_pthread_rwlock_init( void *g, const void *attr )
{
	(void)attr;
	memset(g, 0, 56);
	(void)rw_of(g);
	return 0;
}

static int g_pthread_rwlock_destroy( void *g )
{
	void *p = __atomic_exchange_n((void **)g, NULL, __ATOMIC_ACQ_REL);
	if ( p != NULL ) {
		rw_unmake(p);
		free(p);
	}
	return 0;
}

static int g_pthread_rwlock_rdlock( void *g )
{
	return to_linux_errno(pthread_rwlock_rdlock(rw_of(g)));
}

static int g_pthread_rwlock_wrlock( void *g )
{
	return to_linux_errno(pthread_rwlock_wrlock(rw_of(g)));
}

static int g_pthread_rwlock_unlock( void *g )
{
	return to_linux_errno(pthread_rwlock_unlock(rw_of(g)));
}

/* glibc's semaphore: the process's own made by sem_init */
static int g_sem_init( void *g, int pshared, unsigned value )
{
	sem_t *s = (sem_t *)calloc(1, sizeof(sem_t));
	(void)pshared;
	if ( s == NULL || sem_init(s, 0, value) != 0 ) {
		free(s);
		g_errno = 12;
		return -1;
	}
	memset(g, 0, 32);
	*(sem_t **)g = s;
	return 0;
}

static int g_sem_destroy( void *g )
{
	sem_t *s = *(sem_t **)g;
	if ( s != NULL ) {
		sem_destroy(s);
		free(s);
		*(sem_t **)g = NULL;
	}
	return 0;
}

static int g_sem_wait( void *g )
{
	return (int)ret_errno(sem_wait(*(sem_t **)g));
}

static int g_sem_trywait( void *g )
{
	return (int)ret_errno(sem_trywait(*(sem_t **)g));
}

static int g_sem_post( void *g )
{
	return (int)ret_errno(sem_post(*(sem_t **)g));
}

/* pthread_once on glibc's word: 0 not yet, 1 running, 2 done */
static int g_pthread_once( int *once, void (*fn)( void ) )
{
	int want = 0;

	if ( __atomic_load_n(once, __ATOMIC_ACQUIRE) == 2 ) return 0;
	if ( __atomic_compare_exchange_n(once, &want, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) ) {
		fn();
		__atomic_store_n(once, 2, __ATOMIC_RELEASE);
		return 0;
	}
	while ( __atomic_load_n(once, __ATOMIC_ACQUIRE) != 2 ) sched_yield();
	return 0;
}

static int g_pthread_key_create( unsigned *key, void (*dtor)( void * ) )
{
	pthread_key_t k;
	int r = pthread_key_create(&k, dtor);
	if ( r == 0 ) *key = (unsigned)k;
	return to_linux_errno(r);
}

static void *g_pthread_getspecific( unsigned key )
{
	return pthread_getspecific((pthread_key_t)key);
}

static int g_pthread_setspecific( unsigned key, const void *v )
{
	return to_linux_errno(pthread_setspecific((pthread_key_t)key, v));
}

/* glibc's attributes of a thread: the stack's size in its first word, a mark in the second */
#define ATTR_MARK	0x47415454UL
#define DEF_STACK	( 1024UL * 1024 )

static int g_pthread_attr_init( unsigned long *a )
{
	memset(a, 0, G_ATTR_SIZE);
	a[0] = DEF_STACK;
	a[1] = ATTR_MARK;
	return 0;
}

static int g_pthread_attr_destroy( unsigned long *a )
{
	(void)a;
	return 0;
}

/* the detach state in the attributes' third word */
static int g_pthread_attr_setdetachstate( unsigned long *a, int st )
{
	if ( st != 0 && st != 1 ) return 22;	/* PTHREAD_CREATE_JOINABLE, PTHREAD_CREATE_DETACHED */
	a[2] = (unsigned long)st;
	return 0;
}

static int g_pthread_attr_setstacksize( unsigned long *a, size_t size )
{
	if ( size < 16384 ) return 22;
	a[0] = size;
	a[1] = ATTR_MARK;
	return 0;
}

static int g_pthread_create( unsigned long *th, const unsigned long *ga, void *(*fn)( void * ), void *arg )
{
	pthread_attr_t	a;
	pthread_t	t;
	size_t		stack = ( ga != NULL && ga[1] == ATTR_MARK ) ? ga[0] : DEF_STACK;
	int		r;

	pthread_attr_init(&a);
	pthread_attr_setstacksize(&a, stack < DEF_STACK ? DEF_STACK : stack);
	if ( ga != NULL && ga[1] == ATTR_MARK && ga[2] == 1 ) pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
	r = pthread_create(&t, &a, fn, arg);
	pthread_attr_destroy(&a);
	if ( r != 0 ) return to_linux_errno(r);
	*th = 0;
	memcpy(th, &t, sizeof(t) < sizeof(*th) ? sizeof(t) : sizeof(*th));
	return 0;
}

static int g_pthread_join( unsigned long th, void **ret )
{
	pthread_t t;
	memset(&t, 0, sizeof(t));
	memcpy(&t, &th, sizeof(t) < sizeof(th) ? sizeof(t) : sizeof(th));
	return to_linux_errno(pthread_join(t, ret));
}

static unsigned long g_pthread_self( void )
{
	pthread_t t = pthread_self();
	unsigned long v = 0;
	memcpy(&v, &t, sizeof(t) < sizeof(v) ? sizeof(t) : sizeof(v));
	return v;
}

static int g_pthread_getname_np( unsigned long th, char *name, size_t len )
{
	pthread_t t;
	memset(&t, 0, sizeof(t));
	memcpy(&t, &th, sizeof(t) < sizeof(th) ? sizeof(t) : sizeof(th));
	return to_linux_errno(pthread_getname_np(t, name, len));
}

static int g_pthread_setname_np( unsigned long th, const char *name )
{
	pthread_t t;
	memset(&t, 0, sizeof(t));
	memcpy(&t, &th, sizeof(t) < sizeof(th) ? sizeof(t) : sizeof(th));
	return to_linux_errno(pthread_setname_np(t, name));
}

/* SCHED_OTHER at priority 0: the policy every thread of the process has, as Linux reports it */
static int g_pthread_getschedparam( unsigned long th, int *policy, int *param )
{
	(void)th;
	*policy = 0;
	*param = 0;
	return 0;
}

/* glibc's sigset_t (1024 bits); no signal is ever sent to the process's threads */
static int g_sigfillset( unsigned char *set )
{
	memset(set, 0xff, 128);
	return 0;
}

static int g_pthread_sigmask( int how, const void *set, unsigned char *old )
{
	(void)how;
	(void)set;
	if ( old != NULL ) memset(old, 0, 128);
	return 0;
}

static int g_sched_yield( void )
{
	return sched_yield();
}

/* ---------------------------------------------------------------- futex and syscall */

#define FUTEX_CVS	16

static pthread_mutex_t	fx_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t	fx_cv[FUTEX_CVS];
static int		fx_ready;

static long fx_call( volatile int *addr, int op, int val, const struct timespec *rel )
{
	int	cmd = op & 0x7f;	/* without FUTEX_PRIVATE_FLAG, FUTEX_CLOCK_REALTIME */
	pthread_cond_t *cv;
	long	r = 0;

	pthread_mutex_lock(&fx_mtx);
	if ( !fx_ready ) {
		for ( int i = 0; i < FUTEX_CVS; i++ ) pthread_cond_init(&fx_cv[i], NULL);
		fx_ready = 1;
	}
	cv = &fx_cv[( (uintptr_t)addr >> 2 ) % FUTEX_CVS];
	switch ( cmd ) {
	case 0:		/* FUTEX_WAIT */
	case 9:		/* FUTEX_WAIT_BITSET */
		if ( *addr != val ) {
			g_errno = 11;
			r = -1;
		} else if ( rel == NULL ) {
			pthread_cond_wait(cv, &fx_mtx);
		} else {
			struct timespec until;
			if ( cmd == 0 ) {
				clock_gettime(CLOCK_MONOTONIC, &until);
				until.tv_sec += rel->tv_sec;
				until.tv_nsec += rel->tv_nsec;
				if ( until.tv_nsec >= 1000000000L ) {
					until.tv_sec++;
					until.tv_nsec -= 1000000000L;
				}
			} else {
				until = *rel;
			}
			if ( pthread_cond_clockwait(cv, &fx_mtx, ( op & 256 ) ? CLOCK_REALTIME : CLOCK_MONOTONIC,
						   &until) == ETIMEDOUT ) {
				g_errno = 110;
				r = -1;
			}
		}
		break;
	case 1:		/* FUTEX_WAKE */
	case 10:	/* FUTEX_WAKE_BITSET */
		pthread_cond_broadcast(cv);
		r = ( val > 0 ) ? val : 0;
		break;
	default:
		note("futex", "op", op);
		g_errno = 38;
		r = -1;
		break;
	}
	pthread_mutex_unlock(&fx_mtx);
	return r;
}

static long g_syscall( long number, long a0, long a1, long a2, long a3, long a4, long a5 )
{
	(void)a4;
	(void)a5;
	switch ( number ) {
	case SYS_GETRANDOM:	return ret_errno(getrandom((void *)a0, (size_t)a1, (unsigned)a2));
	case SYS_FUTEX:		return fx_call((volatile int *)a0, (int)a1, (int)a2, (const struct timespec *)a3);
	case SYS_GETTID:	return tk_get_tid();
	case SYS_GETPID:	return getpid();
	case SYS_SCHED_YIELD:	return sched_yield();
	case SYS_CLOCK_GETTIME:	return g_clock_gettime((int)a0, (struct timespec *)a1);
	case SYS_READ:		return g_read((int)a0, (void *)a1, (size_t)a2);
	case SYS_WRITE:		return g_write((int)a0, (const void *)a1, (size_t)a2);
	case SYS_PRCTL:		return 0;
	default:
		note("syscall", "", number);
		g_errno = 38;
		return -1;
	}
}

static long g_getrandom( void *buf, size_t n, unsigned flags )
{
	return ret_errno(getrandom(buf, n, flags));
}

/* ---------------------------------------------------------------- dynamic linking */

typedef struct atx {
	struct atx	*next;
	void		(*fn)( void * );
	void		*arg;
	void		*dso;
} ATX;

static pthread_mutex_t	atx_lock = PTHREAD_MUTEX_INITIALIZER;
static ATX		*atx_list;

static int g___cxa_atexit( void (*fn)( void * ), void *arg, void *dso )
{
	ATX *a = (ATX *)malloc(sizeof(*a));
	if ( a == NULL ) return -1;
	a->fn = fn;
	a->arg = arg;
	a->dso = dso;
	pthread_mutex_lock(&atx_lock);
	a->next = atx_list;
	atx_list = a;
	pthread_mutex_unlock(&atx_lock);
	return 0;
}

/* Whether two addresses lie in the same loaded object */
static int same_object( void *a, void *b )
{
	const char	*n1, *n2;
	void		*b1, *b2;

	return ts_so_addr(a, &n1, &b1) && ts_so_addr(b, &n2, &b2) && b1 == b2;
}

/*
 * Run (newest first) and forget the functions registered for 'dso', or
 * ('within') for any handle inside the same object as 'dso'
 */
static void finalize( void *dso, int within )
{
	for ( ;; ) {
		ATX *a, **pp;
		pthread_mutex_lock(&atx_lock);
		for ( pp = &atx_list; ( a = *pp ) != NULL; pp = &a->next ) {
			if ( a->dso == dso || ( within && same_object(a->dso, dso) ) ) break;
		}
		if ( a != NULL ) *pp = a->next;
		pthread_mutex_unlock(&atx_lock);
		if ( a == NULL ) return;
		a->fn(a->arg);
		free(a);
	}
}

static void g___cxa_finalize( void *dso )
{
	if ( dso != NULL ) finalize(dso, 0);
}

void ts_glibc_finalize( void *dso )
{
	finalize(dso, 1);
}

static int g_dl_iterate_phdr( int (*cb)( void *info, size_t size, void *data ), void *data )
{
	return ts_so_iterate(cb, data);
}

static void *g_dlsym( void *handle, const char *name )
{
	void *p;
	if ( ts_so_is_handle(handle) ) return ts_so_sym(handle, name);
	p = ts_glibc_sym(name, NULL);
	note("dlsym", name, p != NULL);
	return p;
}

static int g_dlclose( void *handle )
{
	if ( ts_so_is_handle(handle) ) return ts_so_close(handle);
	return 0;
}

/* ---------------------------------------------------------------- setjmp, atomics */

extern int g__setjmp( void *env );
extern void g___longjmp_chk( void *env, int val );
#if defined(__aarch64__)
/*
 * glibc's jmp_buf on aarch64: x19 to x30, sp and d8 to d15 in its first
 * 21 words, "mask was saved" after its 22nd
 */
__asm__(
	"	.text\n"
	"	.p2align 2\n"
	"	.type g__setjmp, %function\n"
	"g__setjmp:\n"
	"	stp x19, x20, [x0, #0]\n"
	"	stp x21, x22, [x0, #16]\n"
	"	stp x23, x24, [x0, #32]\n"
	"	stp x25, x26, [x0, #48]\n"
	"	stp x27, x28, [x0, #64]\n"
	"	stp x29, x30, [x0, #80]\n"
	"	mov x2, sp\n"
	"	str x2, [x0, #96]\n"
	"	stp d8, d9, [x0, #104]\n"
	"	stp d10, d11, [x0, #120]\n"
	"	stp d12, d13, [x0, #136]\n"
	"	stp d14, d15, [x0, #152]\n"
	"	str wzr, [x0, #176]\n"
	"	mov w0, #0\n"
	"	ret\n"
	"	.size g__setjmp, . - g__setjmp\n"
	"	.p2align 2\n"
	"	.type g___longjmp_chk, %function\n"
	"g___longjmp_chk:\n"
	"	ldp x19, x20, [x0, #0]\n"
	"	ldp x21, x22, [x0, #16]\n"
	"	ldp x23, x24, [x0, #32]\n"
	"	ldp x25, x26, [x0, #48]\n"
	"	ldp x27, x28, [x0, #64]\n"
	"	ldp x29, x30, [x0, #80]\n"
	"	ldr x2, [x0, #96]\n"
	"	mov sp, x2\n"
	"	ldp d8, d9, [x0, #104]\n"
	"	ldp d10, d11, [x0, #120]\n"
	"	ldp d12, d13, [x0, #136]\n"
	"	ldp d14, d15, [x0, #152]\n"
	"	cmp w1, #0\n"
	"	csinc w0, w1, wzr, ne\n"
	"	br x30\n"
	"	.size g___longjmp_chk, . - g___longjmp_chk\n"
	/*
	 * The atomic helpers a compiler calls when it does not know whether
	 * the processor has the Large System Extensions: the processors
	 * TessronOS runs on have them
	 */
	"	.p2align 2\n"
	"	.type g___aarch64_ldadd4_acq_rel, %function\n"
	"g___aarch64_ldadd4_acq_rel:\n"
	"	ldaddal w0, w0, [x1]\n"
	"	ret\n"
	"	.size g___aarch64_ldadd4_acq_rel, . - g___aarch64_ldadd4_acq_rel\n"
	"	.p2align 2\n"
	"	.type g___aarch64_swp4_acq_rel, %function\n"
	"g___aarch64_swp4_acq_rel:\n"
	"	swpal w0, w0, [x1]\n"
	"	ret\n"
	"	.size g___aarch64_swp4_acq_rel, . - g___aarch64_swp4_acq_rel\n"
);
extern void g___aarch64_ldadd4_acq_rel( void );
extern void g___aarch64_swp4_acq_rel( void );
#else
/*
 * glibc's jmp_buf on x86-64: rbx, rbp, r12 to r15, the stack pointer
 * after the return and the address returned to in its first 8 words,
 * "mask was saved" after them. Only these two read and write it, so the
 * pointers are kept as they are (glibc's own mangles them)
 */
__asm__(
	"	.text\n"
	"	.p2align 4\n"
	"	.type g__setjmp, @function\n"
	"g__setjmp:\n"
	"	movq %rbx, 0(%rdi)\n"
	"	movq %rbp, 8(%rdi)\n"
	"	movq %r12, 16(%rdi)\n"
	"	movq %r13, 24(%rdi)\n"
	"	movq %r14, 32(%rdi)\n"
	"	movq %r15, 40(%rdi)\n"
	"	leaq 8(%rsp), %rdx\n"
	"	movq %rdx, 48(%rdi)\n"
	"	movq (%rsp), %rdx\n"
	"	movq %rdx, 56(%rdi)\n"
	"	movl $0, 64(%rdi)\n"
	"	xorl %eax, %eax\n"
	"	ret\n"
	"	.size g__setjmp, . - g__setjmp\n"
	"	.p2align 4\n"
	"	.type g___longjmp_chk, @function\n"
	"g___longjmp_chk:\n"
	"	movl %esi, %eax\n"
	"	testl %eax, %eax\n"
	"	jnz 1f\n"
	"	incl %eax\n"
	"1:\n"
	"	movq 0(%rdi), %rbx\n"
	"	movq 8(%rdi), %rbp\n"
	"	movq 16(%rdi), %r12\n"
	"	movq 24(%rdi), %r13\n"
	"	movq 32(%rdi), %r14\n"
	"	movq 40(%rdi), %r15\n"
	"	movq 48(%rdi), %rsp\n"
	"	jmpq *56(%rdi)\n"
	"	.size g___longjmp_chk, . - g___longjmp_chk\n"
);
#endif

/* ---------------------------------------------------------------- the table */

typedef struct {
	const char	*name;
	void		*addr;
} GSym;

#define F(n)	{ #n, (void *)n }
#define G(n)	{ #n, (void *)g_##n }
#define V(n)	{ #n, (void *)&g_##n }	/* a variable: its address */

extern void *ts_so_tls_get_addr( void *ti );

/* libgcc's 128-bit division, which libgcc_s.so.1 exports too */
extern void	__udivti3( void );

/* GCC's unwinder in the process (libgcc's unwind-dw2.c); only their addresses are taken */
extern void	_Unwind_Backtrace( void ), _Unwind_DeleteException( void ), _Unwind_FindEnclosingFunction( void ),
		_Unwind_ForcedUnwind( void ), _Unwind_GetCFA( void ), _Unwind_GetDataRelBase( void ),
		_Unwind_GetGR( void ), _Unwind_GetIP( void ), _Unwind_GetIPInfo( void ),
		_Unwind_GetLanguageSpecificData( void ), _Unwind_GetRegionStart( void ),
		_Unwind_GetTextRelBase( void ), _Unwind_RaiseException( void ), _Unwind_Resume( void ),
		_Unwind_Resume_or_Rethrow( void ), _Unwind_SetGR( void ), _Unwind_SetIP( void );

static const GSym gsyms[] = {
	/* memory */
	{ "__libc_malloc", (void *)malloc }, { "__libc_free", (void *)free },
	{ "__libc_calloc", (void *)calloc }, { "__libc_realloc", (void *)realloc },
	G(__libc_memalign), F(malloc), F(free), F(calloc), F(realloc),
	{ "memalign", (void *)g___libc_memalign }, F(posix_memalign),
	/* strings */
	F(memcmp), F(memcpy), F(memmove), F(memset), F(strchr), F(strcmp), F(strlen), F(strncmp),
	{ "bcmp", (void *)memcmp }, F(wcslen), F(qsort), F(rand), G(strtol), G(strerror_r), G(getenv),
	G(__memcpy_chk), G(__memset_chk), G(__vsnprintf_chk), G(__vasprintf_chk), G(__vfprintf_chk),
	G(__fgets_chk), G(__stack_chk_fail), V(__stack_chk_guard), G(abort), G(perror),
	G(__isoc99_sscanf), G(__isoc99_vsscanf),
	F(memchr), F(strncpy), F(strrchr), F(snprintf), F(vsnprintf), F(fprintf), F(vfprintf), F(fputc), F(fgets),
	F(strerror), F(wmemchr), F(getwc), F(ungetwc), F(fputwc), G(strtoul), G(strtoull), G(__strncat_chk),
	G(vasprintf), G(__mbrlen),
	F(ceil), F(ceilf), F(floor), F(rint), F(round), F(log2), F(ldexp), F(frexp),
	/* locales */
	G(__ctype_b_loc), G(__ctype_tolower_loc), G(__ctype_toupper_loc), G(__ctype_get_mb_cur_max), G(newlocale), G(freelocale), G(uselocale),
	G(strtod_l), G(strtof_l), G(strtold_l), G(strtoll_l), G(strtoull_l), G(strftime_l),
	G(catopen), G(catgets), G(catclose),
	F(mbrlen), F(mbrtowc), F(mbsnrtowcs), F(mbsrtowcs), F(mbtowc), F(wcrtomb), F(wcsnrtombs),
	/* time */
	G(clock_gettime), G(gettimeofday), G(gmtime_r), G(localtime_r), G(nanosleep), G(usleep), G(mktime), G(timegm),
	G(sched_yield),
	/* the process */
	G(getpid), G(sysconf), G(getauxval), G(uname), G(prctl), G(__register_atfork), G(syscall),
	G(geteuid), G(getpagesize), G(getrlimit64), G(setpriority), G(sched_setscheduler), G(sched_getaffinity),
	G(__sched_cpucount), G(_exit),
	G(getrandom), G(__errno_location), V(program_invocation_short_name), F(exp),
	V(__libc_stack_end),
	/* files */
	G(__open64_2), G(fopen64), G(read), G(write), G(fcntl64), G(mmap64), G(munmap), G(madvise),
	G(open64), G(creat64), G(pread64), G(__xstat64), G(__fxstat64), G(mprotect), { "fcntl", (void *)g_fcntl64 },
	F(fclose), F(fflush), F(fwrite), F(getc), F(ungetc), V(stdin), V(stdout), V(stderr),
	/* threads */
	G(pthread_mutexattr_init), G(pthread_mutexattr_destroy), G(pthread_mutex_init),
	G(pthread_mutex_destroy), G(pthread_mutex_lock), G(pthread_mutex_trylock), G(pthread_mutex_unlock),
	G(pthread_cond_init), G(pthread_cond_destroy), G(pthread_cond_wait), G(pthread_cond_signal),
	G(pthread_cond_broadcast), G(pthread_cond_timedwait), G(pthread_condattr_init), G(pthread_condattr_destroy),
	G(pthread_condattr_setclock), G(pthread_rwlock_init), G(pthread_rwlock_destroy),
	G(pthread_rwlock_rdlock), G(pthread_rwlock_wrlock), G(pthread_rwlock_unlock),
	G(sem_init), G(sem_destroy), G(sem_wait), G(sem_trywait), G(sem_post),
	G(pthread_once), G(pthread_key_create), G(pthread_getspecific), G(pthread_setspecific),
	G(pthread_attr_init), G(pthread_attr_destroy), G(pthread_attr_setstacksize), G(pthread_attr_setdetachstate),
	G(pthread_create), G(pthread_join), G(pthread_self), G(pthread_getname_np), G(pthread_setname_np),
	G(pthread_getschedparam), G(pthread_getaffinity_np), G(sigfillset), G(pthread_sigmask),
	/* dynamic linking, unwinding, setjmp */
	G(__cxa_atexit), G(__cxa_finalize), G(dl_iterate_phdr), G(dlsym), G(dlclose),
	{ "__tls_get_addr", (void *)ts_so_tls_get_addr },
	/* GCC's unwinder (libgcc_s.so.1) */
	F(_Unwind_Backtrace), F(_Unwind_DeleteException), F(_Unwind_FindEnclosingFunction), F(_Unwind_ForcedUnwind),
	F(_Unwind_GetCFA), F(_Unwind_GetDataRelBase), F(_Unwind_GetGR), F(_Unwind_GetIP), F(_Unwind_GetIPInfo),
	F(_Unwind_GetLanguageSpecificData), F(_Unwind_GetRegionStart), F(_Unwind_GetTextRelBase),
	F(_Unwind_RaiseException), F(_Unwind_Resume), F(_Unwind_Resume_or_Rethrow), F(_Unwind_SetGR),
	F(_Unwind_SetIP),
	G(_setjmp), G(__longjmp_chk), { "longjmp", (void *)g___longjmp_chk }, F(__udivti3),
#if defined(__aarch64__)
	G(__aarch64_ldadd4_acq_rel), G(__aarch64_swp4_acq_rel),
#endif
};

void *ts_glibc_sym( const char *name, const char *version )
{
	size_t i;

	(void)version;
	if ( ctype_ptr == NULL ) {
		ctype_make();
		g_stdin = stdin;
		g_stdout = stdout;
		g_stderr = stderr;
		stack_end_make();
	}
	if ( g___stack_chk_guard == 0 ) {
		if ( getrandom(&g___stack_chk_guard, sizeof(g___stack_chk_guard), 0) != sizeof(g___stack_chk_guard) ) {
			g___stack_chk_guard = (uintptr_t)0x5a17c0de2b9e4d61ULL;
		}
		g___stack_chk_guard &= ~(uintptr_t)0xff;	/* a zero byte first, as glibc's canary has */
	}
	for ( i = 0; i < sizeof(gsyms) / sizeof(gsyms[0]); i++ ) {
		if ( strcmp(gsyms[i].name, name) == 0 ) return gsyms[i].addr;
	}
	return NULL;
}

int ts_glibc_version_ok( const char *version )
{
	int minor;

	if ( strcmp(version, "GLIBC_ABI_DT_RELR") == 0 ) return 1;
	/* the versions of GCC's unwinder (GCC_3.0, GCC_3.3, GCC_4.2.0 ...) */
	if ( strncmp(version, "GCC_", 4) == 0 && version[4] >= '3' && version[4] <= '9' ) return 1;
	if ( strncmp(version, "GLIBC_2.", 8) != 0 ) return 0;
	minor = atoi(version + 8);
	return minor >= 0 && minor <= 39;
}

int ts_glibc_library_ok( const char *soname )
{
	static const char *const glibc[] = {
		"libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1",
		G_DYNLINKER, "libgcc_s.so.1",
	};
	/*
	 * Libraries an object may name without taking anything from them:
	 * whatever it asks for must still be among the GNU C library's calls
	 * here, or the object is refused when its symbols are bound
	 */
	static const char *const unused[] = { "libnspr4.so", "libplc4.so", "libplds4.so" };
	size_t i;

	for ( i = 0; i < sizeof(glibc) / sizeof(glibc[0]); i++ ) {
		if ( strcmp(soname, glibc[i]) == 0 ) return 1;
	}
	for ( i = 0; i < sizeof(unused) / sizeof(unused[0]); i++ ) {
		if ( strcmp(soname, unused[i]) == 0 ) {
			printf("browser: so: %s is named; nothing of it is bound\n", soname);
			return 1;
		}
	}
	return 0;
}

#endif /* __aarch64__ || __x86_64__ */
