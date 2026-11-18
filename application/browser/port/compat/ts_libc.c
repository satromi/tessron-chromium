/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	ts_libc.c
 *	The C library's calls the engine's sources reach that the system's
 *	library does not have (design 17.19):
 *
 *	  - select, on poll (WebRTC's socket server when it cannot use epoll)
 *	  - fnmatch (base's FileEnumerator with a pattern)
 *	  - gethostname: the network stack's host name, as the system's
 *	    real object shows it (record 7, "HOSTNAME"; br_sysnet.cc)
 *	  - getifaddrs and freeifaddrs: the loopback interface and the
 *	    card's, with its IPv4 address and mask and its IPv6 addresses
 *	    (record 7, "ADDRESS6"), for WebRTC's own addresses
 *	  - the calls of Rust's standard library built for Linux (the
 *	    decoders written in Rust): __errno_location, open64, stat64,
 *	    fstat64, lseek64, mmap64, with Linux's numbers for the flags of
 *	    open and the layout of struct stat converted to and from the
 *	    system's.
 *	  - the conversions of floating numbers to and from text (dtoa,
 *	    strtod and their kin), wrapped to run one at a time, as the C
 *	    library's work space for them is shared by the threads.
 *
 *	The system's parts are reached through weak names, as these objects
 *	are linked into programs that do not have them all.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <locale.h>
#include <pthread.h>

#undef open64
#undef lseek64

/* the system's interface and record 7 (application/browser/port/net/br_sysnet.cc, libts) */
extern int	so_getifaddr( unsigned *p_addr, unsigned *p_mask, unsigned *p_gw ) __attribute__((weak));
extern int	br_sys_addr6( unsigned char (*addr)[16], int *plen, int max ) __attribute__((weak));
extern int	br_sys_hostname( char *name, int cap ) __attribute__((weak));
extern unsigned	br_sys_ifindex( void ) __attribute__((weak));

/* ---------------------------------------------------------------- select */

int select( int nfds, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *tv )
{
	struct pollfd	p[FD_SETSIZE];
	int		n = 0, fd, r, ms, got = 0;

	if ( nfds < 0 || nfds > FD_SETSIZE ) {
		errno = EINVAL;
		return -1;
	}
	for ( fd = 0; fd < nfds; fd++ ) {
		short ev = 0;
		if ( rd != NULL && FD_ISSET(fd, rd) ) ev |= POLLIN;
		if ( wr != NULL && FD_ISSET(fd, wr) ) ev |= POLLOUT;
		if ( ex != NULL && FD_ISSET(fd, ex) ) ev |= POLLPRI;
		if ( ev == 0 ) continue;
		p[n].fd = fd;
		p[n].events = ev;
		p[n].revents = 0;
		n++;
	}
	ms = ( tv == NULL ) ? -1 : (int)( tv->tv_sec * 1000 + ( tv->tv_usec + 999 ) / 1000 );
	r = poll(p, (nfds_t)n, ms);
	if ( r < 0 ) return -1;
	if ( rd != NULL ) FD_ZERO(rd);
	if ( wr != NULL ) FD_ZERO(wr);
	if ( ex != NULL ) FD_ZERO(ex);
	for ( fd = 0; fd < n; fd++ ) {
		short rv = p[fd].revents;
		if ( rd != NULL && ( rv & ( POLLIN | POLLHUP | POLLERR ) ) && ( p[fd].events & POLLIN ) ) {
			FD_SET(p[fd].fd, rd);
			got++;
		}
		if ( wr != NULL && ( rv & ( POLLOUT | POLLERR ) ) && ( p[fd].events & POLLOUT ) ) {
			FD_SET(p[fd].fd, wr);
			got++;
		}
		if ( ex != NULL && ( rv & POLLPRI ) ) {
			FD_SET(p[fd].fd, ex);
			got++;
		}
	}
	if ( tv != NULL && r == 0 ) tv->tv_sec = tv->tv_usec = 0;
	return got;
}

/* ---------------------------------------------------------------- fnmatch */

#define FNM_NOMATCH	1
#define FNM_NOESCAPE	0x01
#define FNM_PATHNAME	0x02
#define FNM_PERIOD	0x04

/* A bracket expression at p against c; where it ends, or NULL when it is not one */
static const char *bracket( const char *p, char c, int flags, int *hit )
{
	int	neg = 0, ok = 0;
	char	lo, hi;

	if ( *p == '!' || *p == '^' ) {
		neg = 1;
		p++;
	}
	if ( *p == ']' ) {
		ok = ( c == ']' );
		p++;
	}
	while ( *p != ']' ) {
		if ( *p == '\0' ) return NULL;
		lo = *p++;
		if ( lo == '\\' && !( flags & FNM_NOESCAPE ) && *p != '\0' ) lo = *p++;
		if ( *p == '-' && p[1] != ']' && p[1] != '\0' ) {
			hi = p[1];
			p += 2;
			if ( hi == '\\' && !( flags & FNM_NOESCAPE ) && *p != '\0' ) hi = *p++;
			if ( c >= lo && c <= hi ) ok = 1;
		} else if ( c == lo ) {
			ok = 1;
		}
	}
	*hit = ok ^ neg;
	return p + 1;
}

static int match( const char *p, const char *s, const char *start, int flags )
{
	for ( ;; ) {
		char c = *p++;
		switch ( c ) {
		case '\0':
			return *s == '\0' ? 0 : FNM_NOMATCH;
		case '?':
			if ( *s == '\0' ) return FNM_NOMATCH;
			if ( *s == '/' && ( flags & FNM_PATHNAME ) ) return FNM_NOMATCH;
			if ( *s == '.' && ( flags & FNM_PERIOD ) &&
			     ( s == start || ( ( flags & FNM_PATHNAME ) && s[-1] == '/' ) ) ) return FNM_NOMATCH;
			s++;
			break;
		case '*':
			while ( *p == '*' ) p++;
			if ( *s == '.' && ( flags & FNM_PERIOD ) &&
			     ( s == start || ( ( flags & FNM_PATHNAME ) && s[-1] == '/' ) ) ) return FNM_NOMATCH;
			if ( *p == '\0' ) {
				if ( flags & FNM_PATHNAME ) return strchr(s, '/') == NULL ? 0 : FNM_NOMATCH;
				return 0;
			}
			for ( ; *s != '\0'; s++ ) {
				if ( match(p, s, start, flags & ~FNM_PERIOD) == 0 ) return 0;
				if ( *s == '/' && ( flags & FNM_PATHNAME ) ) return FNM_NOMATCH;
			}
			return match(p, s, start, flags & ~FNM_PERIOD);
		case '[': {
			int		hit = 0;
			const char	*q;
			if ( *s == '\0' ) return FNM_NOMATCH;
			if ( *s == '/' && ( flags & FNM_PATHNAME ) ) return FNM_NOMATCH;
			q = bracket(p, *s, flags, &hit);
			if ( q == NULL ) {
				if ( *s != '[' ) return FNM_NOMATCH;
			} else {
				if ( !hit ) return FNM_NOMATCH;
				p = q;
			}
			s++;
			break;
		}
		case '\\':
			if ( !( flags & FNM_NOESCAPE ) && *p != '\0' ) c = *p++;
			/* fall through */
		default:
			if ( c != *s ) return FNM_NOMATCH;
			s++;
			break;
		}
	}
}

int fnmatch( const char *pattern, const char *string, int flags )
{
	return match(pattern, string, string, flags);
}

/* ---------------------------------------------------------------- gethostname */

int gethostname( char *name, size_t len )
{
	char	h[64];
	size_t	n;

	if ( name == NULL ) {
		errno = EFAULT;
		return -1;
	}
	if ( br_sys_hostname == NULL || br_sys_hostname(h, (int)sizeof(h)) <= 0 ) strcpy(h, "tessronos");
	n = strlen(h);
	if ( n + 1 > len ) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memcpy(name, h, n + 1);
	return 0;
}

/* ---------------------------------------------------------------- getifaddrs */

/* One entry and the addresses it points to, in one block (freeifaddrs frees each) */
struct ifa_block {
	struct ifaddrs		ifa;
	struct sockaddr_in6	addr, mask;
	char			name[8];
};

static struct ifaddrs *add_entry( struct ifaddrs **head, struct ifaddrs **tail, const char *name,
				  unsigned flags, int family, const unsigned char *addr, int plen )
{
	struct ifa_block	*b = calloc(1, sizeof(*b));
	int			i;

	if ( b == NULL ) return NULL;
	strncpy(b->name, name, sizeof(b->name) - 1);
	b->ifa.ifa_name = b->name;
	b->ifa.ifa_flags = flags;
	b->ifa.ifa_addr = (struct sockaddr *)&b->addr;
	b->ifa.ifa_netmask = (struct sockaddr *)&b->mask;
	if ( family == AF_INET ) {
		struct sockaddr_in *a = (struct sockaddr_in *)&b->addr, *m = (struct sockaddr_in *)&b->mask;
		uint32_t bits = plen <= 0 ? 0 : plen >= 32 ? 0xffffffffu : ~( 0xffffffffu >> plen );
		a->sin_family = m->sin_family = AF_INET;
		memcpy(&a->sin_addr, addr, 4);
		m->sin_addr.s_addr = htonl(bits);
	} else {
		b->addr.sin6_family = b->mask.sin6_family = AF_INET6;
		memcpy(&b->addr.sin6_addr, addr, 16);
		for ( i = 0; i < 16; i++ ) {
			int k = plen - 8 * i;
			((unsigned char *)&b->mask.sin6_addr)[i] =
				k >= 8 ? 0xff : k <= 0 ? 0 : (unsigned char)( 0xff << ( 8 - k ) );
		}
		/* a link-local address names its interface */
		if ( addr[0] == 0xfe && ( addr[1] & 0xc0 ) == 0x80 ) {
			b->addr.sin6_scope_id = ( flags & IFF_LOOPBACK ) ? 1
						: ( br_sys_ifindex != NULL ? br_sys_ifindex() : 2 );
		}
	}
	if ( *tail != NULL ) ( *tail )->ifa_next = &b->ifa;
	else *head = &b->ifa;
	*tail = &b->ifa;
	return &b->ifa;
}

int getifaddrs( struct ifaddrs **ifap )
{
	static const unsigned char	lo4[4] = { 127, 0, 0, 1 };
	static const unsigned char	lo6[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
	struct ifaddrs			*head = NULL, *tail = NULL;
	unsigned			addr = 0, mask = 0, gw = 0;
	unsigned char			a6[8][16];
	int				pl6[8], n6 = 0, i;
	const unsigned			up = IFF_UP | IFF_RUNNING;

	if ( ifap == NULL ) {
		errno = EFAULT;
		return -1;
	}
	add_entry(&head, &tail, "lo0", up | IFF_LOOPBACK, AF_INET, lo4, 8);
	add_entry(&head, &tail, "lo0", up | IFF_LOOPBACK, AF_INET6, lo6, 128);
	if ( so_getifaddr != NULL && so_getifaddr(&addr, &mask, &gw) >= 0 && addr != 0 ) {
		add_entry(&head, &tail, "neta0", up | IFF_BROADCAST | IFF_MULTICAST, AF_INET,
			  (const unsigned char *)&addr, __builtin_popcount(mask));
	}
	if ( br_sys_addr6 != NULL ) n6 = br_sys_addr6(a6, pl6, 8);
	for ( i = 0; i < n6; i++ ) {
		add_entry(&head, &tail, "neta0", up | IFF_MULTICAST, AF_INET6, a6[i], pl6[i]);
	}
	*ifap = head;
	return 0;
}

void freeifaddrs( struct ifaddrs *ifa )
{
	while ( ifa != NULL ) {
		struct ifaddrs *next = ifa->ifa_next;
		free(ifa);
		ifa = next;
	}
}

/* ---------------------------------------------------------------- Rust's standard library */

int *__errno_location( void )
{
	return &errno;
}

#if defined(__x86_64__)
/* Linux's numbers for the flags of open (x86-64) */
#define L_O_ACCMODE	00000003
#define L_O_CREAT	00000100
#define L_O_EXCL	00000200
#define L_O_NOCTTY	00000400
#define L_O_TRUNC	00001000
#define L_O_APPEND	00002000
#define L_O_NONBLOCK	00004000
#define L_O_SYNC	04010000
#define L_O_DIRECTORY	00200000
#define L_O_NOFOLLOW	00400000
#define L_O_CLOEXEC	02000000
#else
/* Linux's numbers for the flags of open (aarch64) */
#define L_O_ACCMODE	00000003
#define L_O_CREAT	00000100
#define L_O_EXCL	00000200
#define L_O_NOCTTY	00000400
#define L_O_TRUNC	00001000
#define L_O_APPEND	00002000
#define L_O_NONBLOCK	00004000
#define L_O_SYNC	04010000
#define L_O_DIRECTORY	00040000
#define L_O_NOFOLLOW	00100000
#define L_O_CLOEXEC	02000000
#endif

static int open_flags( int f )
{
	int o = f & L_O_ACCMODE;

	if ( f & L_O_CREAT ) o |= O_CREAT;
	if ( f & L_O_EXCL ) o |= O_EXCL;
	if ( f & L_O_NOCTTY ) o |= O_NOCTTY;
	if ( f & L_O_TRUNC ) o |= O_TRUNC;
	if ( f & L_O_APPEND ) o |= O_APPEND;
	if ( f & L_O_NONBLOCK ) o |= O_NONBLOCK;
	if ( ( f & L_O_SYNC ) == L_O_SYNC ) o |= O_SYNC;
	if ( f & L_O_DIRECTORY ) o |= O_DIRECTORY;
	if ( f & L_O_NOFOLLOW ) o |= O_NOFOLLOW;
	if ( f & L_O_CLOEXEC ) o |= O_CLOEXEC;
	return o;
}

int open64( const char *path, int flags, ... )
{
	va_list		ap;
	unsigned	mode = 0;

	if ( flags & L_O_CREAT ) {
		va_start(ap, flags);
		mode = va_arg(ap, unsigned);
		va_end(ap);
	}
	return open(path, open_flags(flags), (mode_t)mode);
}

#if defined(__x86_64__)
/* struct stat as Linux has it on x86-64 */
struct linux_stat {
	uint64_t	st_dev;
	uint64_t	st_ino;
	uint64_t	st_nlink;
	uint32_t	st_mode;
	uint32_t	st_uid;
	uint32_t	st_gid;
	int32_t		pad0;
	uint64_t	st_rdev;
	int64_t		st_size;
	int64_t		st_blksize;
	int64_t		st_blocks;
	int64_t		st_atime_sec, st_atime_nsec;
	int64_t		st_mtime_sec, st_mtime_nsec;
	int64_t		st_ctime_sec, st_ctime_nsec;
	int64_t		unused[3];
};
#else
/* struct stat as Linux has it on aarch64 (the generic layout) */
struct linux_stat {
	uint64_t	st_dev;
	uint64_t	st_ino;
	uint32_t	st_mode;
	uint32_t	st_nlink;
	uint32_t	st_uid;
	uint32_t	st_gid;
	uint64_t	st_rdev;
	uint64_t	pad1;
	int64_t		st_size;
	int32_t		st_blksize;
	int32_t		pad2;
	int64_t		st_blocks;
	int64_t		st_atime_sec, st_atime_nsec;
	int64_t		st_mtime_sec, st_mtime_nsec;
	int64_t		st_ctime_sec, st_ctime_nsec;
	uint32_t	unused[2];
};
#endif

static void to_linux( const struct stat *s, struct linux_stat *l )
{
	memset(l, 0, sizeof(*l));
	l->st_dev = (uint64_t)s->st_dev;
	l->st_ino = (uint64_t)s->st_ino;
	l->st_mode = (uint32_t)s->st_mode;
	l->st_nlink = (uint32_t)s->st_nlink;
	l->st_uid = (uint32_t)s->st_uid;
	l->st_gid = (uint32_t)s->st_gid;
	l->st_rdev = (uint64_t)s->st_rdev;
	l->st_size = (int64_t)s->st_size;
	l->st_blksize = s->st_blksize;
	l->st_blocks = (int64_t)s->st_blocks;
	l->st_atime_sec = (int64_t)s->st_atim.tv_sec;
	l->st_atime_nsec = (int64_t)s->st_atim.tv_nsec;
	l->st_mtime_sec = (int64_t)s->st_mtim.tv_sec;
	l->st_mtime_nsec = (int64_t)s->st_mtim.tv_nsec;
	l->st_ctime_sec = (int64_t)s->st_ctim.tv_sec;
	l->st_ctime_nsec = (int64_t)s->st_ctim.tv_nsec;
}

int stat64( const char *path, struct linux_stat *out )
{
	struct stat s;

	if ( stat(path, &s) < 0 ) return -1;
	to_linux(&s, out);
	return 0;
}

int fstat64( int fd, struct linux_stat *out )
{
	struct stat s;

	if ( fstat(fd, &s) < 0 ) return -1;
	to_linux(&s, out);
	return 0;
}

off_t lseek64( int fd, off_t offset, int whence )
{
	return lseek(fd, offset, whence);
}

/* the system's numbers for the protections and kinds of a mapping are Linux's (sys/mman.h) */
void *mmap64( void *addr, size_t len, int prot, int flags, int fd, off_t offset )
{
	return mmap(addr, len, prot, flags, fd, offset);
}

/* ---------------------------------------------------------------- the conversions of floating numbers */

/*
 * The C library keeps one reentrancy record for the whole program (it is
 * built without per-thread records), and its conversions between floating
 * numbers and text work on big integers taken from a free list in that
 * record, the last result of dtoa kept there too. Two threads converting
 * at once (printf of a double, strtod, sscanf) break the list and the
 * program faults in _Balloc. The program is linked with these calls
 * wrapped (-Wl,--wrap, tools/browser/build.sh): each runs under one lock,
 * and dtoa's result is copied to a buffer of the calling thread before
 * the lock is let go, as the next conversion on another thread frees it.
 * A program linked without the wrapping does not reach these, and the
 * link drops them with their references to the real calls (each is a
 * section of its own, -ffunction-sections and --gc-sections).
 */
struct _reent;
extern char	*__real__dtoa_r( struct _reent *r, double d, int mode, int ndigits, int *decpt, int *sign,
				 char **rve );
extern char	*__real__ldtoa_r( struct _reent *r, long double d, int mode, int ndigits, int *decpt,
				  int *sign, char **rve );
extern double	__real__strtod_r( struct _reent *r, const char *s, char **end );
extern double	__real__strtod_l( struct _reent *r, const char *s, char **end, locale_t loc );
extern double	__real_strtod( const char *s, char **end );
extern double	__real_strtod_l( const char *s, char **end, locale_t loc );
extern float	__real_strtof( const char *s, char **end );
extern float	__real_strtof_l( const char *s, char **end, locale_t loc );
extern int	__real__strtodg_l( struct _reent *r, const char *s, char **end, void *fpi, long *exp,
				   unsigned long *bits, locale_t loc );

static pthread_mutex_t	g_fp_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread char	*g_fp_buf;	/* this thread's copy of the last result */
static __thread size_t	g_fp_cap;

/*
 * dtoa's result (and its end) moved to the calling thread's buffer, under
 * the lock. The buffer is as large as dtoa's own would be: printf pads the
 * digits with zeros in place up to the precision asked for ('ndigits'
 * digits, after the decimal point in mode 3), past the end of the string.
 */
static char *fp_keep( char *s, char **rve, int ndigits, int decpt )
{
	size_t n, end, need;

	if ( s == NULL ) return NULL;
	n = strlen(s) + 1;
	end = ( rve != NULL && *rve != NULL && *rve >= s ) ? (size_t)( *rve - s ) : n - 1;
	need = n + 32;
	if ( ndigits > 0 ) need += (size_t)ndigits;
	if ( decpt > 0 && decpt < 9999 ) need += (size_t)decpt;
	if ( need > g_fp_cap ) {
		char *b = (char *)realloc(g_fp_buf, need);
		if ( b == NULL ) return s;
		g_fp_buf = b;
		g_fp_cap = need;
	}
	memcpy(g_fp_buf, s, n);
	if ( rve != NULL ) *rve = g_fp_buf + end;
	return g_fp_buf;
}

char *__wrap__dtoa_r( struct _reent *r, double d, int mode, int ndigits, int *decpt, int *sign, char **rve )
{
	char *s;

	pthread_mutex_lock(&g_fp_lock);
	s = __real__dtoa_r(r, d, mode, ndigits, decpt, sign, rve);
	s = fp_keep(s, rve, ndigits, *decpt);
	pthread_mutex_unlock(&g_fp_lock);
	return s;
}

char *__wrap__ldtoa_r( struct _reent *r, long double d, int mode, int ndigits, int *decpt, int *sign,
		       char **rve )
{
	char *s;

	pthread_mutex_lock(&g_fp_lock);
	s = __real__ldtoa_r(r, d, mode, ndigits, decpt, sign, rve);
	s = fp_keep(s, rve, ndigits, *decpt);
	pthread_mutex_unlock(&g_fp_lock);
	return s;
}

#define FP_LOCKED(type, call)	do { type v_; pthread_mutex_lock(&g_fp_lock); v_ = (call); \
				     pthread_mutex_unlock(&g_fp_lock); return v_; } while ( 0 )

double __wrap__strtod_r( struct _reent *r, const char *s, char **end )
{
	FP_LOCKED(double, __real__strtod_r(r, s, end));
}

double __wrap__strtod_l( struct _reent *r, const char *s, char **end, locale_t loc )
{
	FP_LOCKED(double, __real__strtod_l(r, s, end, loc));
}

double __wrap_strtod( const char *s, char **end )
{
	FP_LOCKED(double, __real_strtod(s, end));
}

double __wrap_strtod_l( const char *s, char **end, locale_t loc )
{
	FP_LOCKED(double, __real_strtod_l(s, end, loc));
}

float __wrap_strtof( const char *s, char **end )
{
	FP_LOCKED(float, __real_strtof(s, end));
}

float __wrap_strtof_l( const char *s, char **end, locale_t loc )
{
	FP_LOCKED(float, __real_strtof_l(s, end, loc));
}

int __wrap__strtodg_l( struct _reent *r, const char *s, char **end, void *fpi, long *exp, unsigned long *bits,
		       locale_t loc )
{
	FP_LOCKED(int, __real__strtodg_l(r, s, end, fpi, exp, bits, loc));
}
