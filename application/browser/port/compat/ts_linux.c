/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	ts_linux.c
 *	The parts of the Linux surface the engine's sources call that are
 *	answered from what a TessronOS process is (application/browser/port/
 *	shim, design 17.19): the machine's features, resource limits, and
 *	dynamic loading, of which there is none.
 */

#include <stddef.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/auxv.h>
#include <sys/resource.h>
#include <dlfcn.h>
#include <link.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <sys/vfs.h>
#include <sys/timerfd.h>
#include <sys/inotify.h>
#include <sys/sysinfo.h>
#include <sys/random.h>
#include <sys/ioctl.h>
#include <sys/sendfile.h>
#include <sys/statvfs.h>
#include <syslog.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <linux/futex.h>
#include <sched.h>
#include <time.h>
#include <string.h>
#include <stdint.h>

/*
 * Both machines TessronOS runs on are Cortex-A76 (QEMU's -cpu cortex-a76,
 * the Raspberry Pi 5): Armv8.2-A with the cryptographic extension.
 */
#define A76_HWCAP	( HWCAP_FP | HWCAP_ASIMD | HWCAP_AES | HWCAP_PMULL | HWCAP_SHA1 \
			| HWCAP_SHA2 | HWCAP_CRC32 | HWCAP_ATOMICS | HWCAP_FPHP | HWCAP_ASIMDHP \
			| HWCAP_ASIMDRDM | HWCAP_LRCPC | HWCAP_DCPOP | HWCAP_ASIMDDP )

unsigned long getauxval( unsigned long type )
{
	switch ( type ) {
	case AT_HWCAP:	return A76_HWCAP;
	case AT_HWCAP2:	return 0;
	case AT_PAGESZ:	return 4096;
	case AT_CLKTCK:	return 100;
	default:
		errno = ENOENT;
		return 0;
	}
}

int sysinfo( struct sysinfo *info )
{
	struct timespec	t;

	memset(info, 0, sizeof(*info));
	clock_gettime(CLOCK_MONOTONIC, &t);
	info->uptime = (long)t.tv_sec;
	info->totalram = 1024UL * 1024 * 1024;
	info->freeram = 512UL * 1024 * 1024;
	info->procs = 1;
	info->mem_unit = 1;
	return 0;
}

int get_nprocs( void )
{
	return 2;
}

int get_nprocs_conf( void )
{
	return 2;
}

int getrusage( int who, struct rusage *ru )
{
	(void)who;
	memset(ru, 0, sizeof(*ru));
	return 0;
}

pid_t wait4( pid_t pid, int *status, int options, struct rusage *ru )
{
	(void)pid; (void)status; (void)options; (void)ru;
	errno = ECHILD;
	return -1;
}

/* What a file system holds: not told */
int statfs( const char *path, struct statfs *buf )
{
	(void)path; (void)buf;
	errno = ENOSYS;
	return -1;
}

int fstatfs( int fd, struct statfs *buf )
{
	(void)fd; (void)buf;
	errno = ENOSYS;
	return -1;
}

int timerfd_create( int clockid, int flags )
{
	(void)clockid; (void)flags;
	errno = ENOSYS;
	return -1;
}

int timerfd_settime( int fd, int flags, const struct itimerspec *nv, struct itimerspec *ov )
{
	(void)fd; (void)flags; (void)nv; (void)ov;
	errno = EBADF;
	return -1;
}

int timerfd_gettime( int fd, struct itimerspec *cur )
{
	(void)fd; (void)cur;
	errno = EBADF;
	return -1;
}

int inotify_init( void )
{
	errno = ENOSYS;
	return -1;
}

int inotify_init1( int flags )
{
	(void)flags;
	errno = ENOSYS;
	return -1;
}

int inotify_add_watch( int fd, const char *path, uint32_t mask )
{
	(void)fd; (void)path; (void)mask;
	errno = EBADF;
	return -1;
}

int inotify_rm_watch( int fd, int wd )
{
	(void)fd; (void)wd;
	errno = EBADF;
	return -1;
}

int getrlimit( int resource, struct rlimit *rl )
{
	(void)resource;
	rl->rlim_cur = RLIM_INFINITY;
	rl->rlim_max = RLIM_INFINITY;
	return 0;
}

int setrlimit( int resource, const struct rlimit *rl )
{
	(void)resource; (void)rl;
	return 0;
}

int getpriority( int which, int who )
{
	(void)which; (void)who;
	errno = 0;
	return 0;
}

int setpriority( int which, int who, int prio )
{
	(void)which; (void)who; (void)prio;
	return 0;
}

extern int tk_get_tid( void );
extern int ts_get_pid( void );

/*
 * getrandom and getentropy are the C++ runtime's (lib/libcxxrt of
 * TessronOS, ts_urandom.c): they read the system's random source object
 * 乱数, as /dev/urandom does there.
 */

/*
 * futex: waiting on a word and waking its waiters, for the locks that
 * sleep on one (PartitionAlloc's). A waiter sleeps on one of a few
 * condition variables, chosen by the word's address, under one mutex,
 * and looks at the word under that mutex: a wake that follows a change
 * of the word is never missed. A wake wakes every waiter of its
 * variable, and each looks at its word again, as futex waiters must.
 */
#define FUTEX_CVS	16

static pthread_mutex_t	futex_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t	futex_cv[FUTEX_CVS];

static pthread_cond_t *futex_cv_of( const volatile int *addr )
{
	return &futex_cv[( (unsigned long)addr >> 2 ) % FUTEX_CVS];
}

static long futex( volatile int *addr, int op, int val, const struct timespec *rel )
{
	int	cmd = op & ~( FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME );
	long	r = 0;

	switch ( cmd ) {
	case FUTEX_WAIT:
	case FUTEX_WAIT_BITSET:
		pthread_mutex_lock(&futex_mtx);
		if ( *addr != val ) {
			pthread_mutex_unlock(&futex_mtx);
			errno = EAGAIN;
			return -1;
		}
		if ( rel == NULL ) {
			pthread_cond_wait(futex_cv_of(addr), &futex_mtx);
		} else {
			struct timespec	until;

			/* WAIT takes a time to wait, WAIT_BITSET a time to wait until */
			if ( cmd == FUTEX_WAIT ) {
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
			if ( pthread_cond_clockwait(futex_cv_of(addr), &futex_mtx,
						   ( op & FUTEX_CLOCK_REALTIME ) ? CLOCK_REALTIME : CLOCK_MONOTONIC,
						   &until) == ETIMEDOUT ) {
				errno = ETIMEDOUT;
				r = -1;
			}
		}
		pthread_mutex_unlock(&futex_mtx);
		return r;

	case FUTEX_WAKE:
	case FUTEX_WAKE_BITSET:
		pthread_mutex_lock(&futex_mtx);
		pthread_cond_broadcast(futex_cv_of(addr));
		pthread_mutex_unlock(&futex_mtx);
		return ( val > 0 ) ? val : 0;	/* at most the number asked for */

	default:
		errno = ENOSYS;
		return -1;
	}
}

long syscall( long number, ... )
{
	va_list	ap;
	long	a[6];
	int	i;

	va_start(ap, number);
	for ( i = 0; i < 6; i++ ) a[i] = va_arg(ap, long);
	va_end(ap);

	switch ( number ) {
	case SYS_gettid:	return tk_get_tid();
	case SYS_getpid:	return ts_get_pid();
	case SYS_read:		return read((int)a[0], (void *)a[1], (size_t)a[2]);
	case SYS_write:		return write((int)a[0], (const void *)a[1], (size_t)a[2]);
	case SYS_mmap:		return (long)mmap((void *)a[0], (size_t)a[1], (int)a[2], (int)a[3],
						  (int)a[4], (off_t)a[5]);
	case SYS_munmap:	return munmap((void *)a[0], (size_t)a[1]);
	case SYS_getrandom:	return getrandom((void *)a[0], (size_t)a[1], (unsigned int)a[2]);
	case SYS_futex:		return futex((volatile int *)a[0], (int)a[1], (int)a[2],
					     (const struct timespec *)a[3]);
	default:
		errno = ENOSYS;
		return -1;
	}
}

int prctl( int option, ... )
{
	(void)option;
	return 0;
}

/* One image and no shared objects: nothing to report beyond the program */
int dl_iterate_phdr( int (*cb)( struct dl_phdr_info *info, size_t size, void *data ),
		     void *data )
{
	(void)cb; (void)data;
	return 0;
}

static char	dl_msg[] = "dynamic loading is not available";

void *dlopen( const char *file, int mode )
{
	(void)file; (void)mode;
	return NULL;
}

extern int	_close( int fd );

/*
 * Of the C library's functions, the one a program looks up to call
 * underneath its own definition (//base puts a close() in front of the C
 * library's and forwards to the next one): close is the file layer's
 * _close (lib/libcxxrt). Nothing else is found.
 */
void *dlsym( void *handle, const char *name )
{
	(void)handle;
	if ( strcmp(name, "close") == 0 ) return (void *)_close;
	return NULL;
}

int dlclose( void *handle )
{
	(void)handle;
	return 0;
}

char *dlerror( void )
{
	return dl_msg;
}

int dladdr( const void *addr, Dl_info *info )
{
	(void)addr;
	info->dli_fname = NULL;
	info->dli_fbase = NULL;
	info->dli_sname = NULL;
	info->dli_saddr = NULL;
	return 0;
}

/* The name the program was started by: TessronOS gives a process none */
char	*program_invocation_name = (char *)"browser";
char	*program_invocation_short_name = (char *)"browser";

/* No descriptor takes a device request */
int ioctl( int fd, unsigned long request, ... )
{
	(void)fd; (void)request;
	errno = ENOTTY;
	return -1;
}

ssize_t sendfile( int out_fd, int in_fd, off_t *offset, size_t count )
{
	(void)out_fd; (void)in_fd; (void)offset; (void)count;
	errno = ENOSYS;
	return -1;
}

int statvfs( const char *path, struct statvfs *buf )
{
	(void)path; (void)buf;
	errno = ENOSYS;
	return -1;
}

int fstatvfs( int fd, struct statvfs *buf )
{
	(void)fd; (void)buf;
	errno = ENOSYS;
	return -1;
}

/* No system log for a program */
void openlog( const char *ident, int option, int facility )
{
	(void)ident; (void)option; (void)facility;
}

void syslog( int priority, const char *format, ... )
{
	(void)priority; (void)format;
}

void closelog( void )
{
}

int fallocate( int fd, int mode, off_t offset, off_t len )
{
	(void)fd; (void)mode; (void)offset; (void)len;
	errno = ENOSYS;
	return -1;
}

int posix_fallocate( int fd, off_t offset, off_t len )
{
	(void)fd; (void)offset; (void)len;
	return ENOSYS;
}

/* A process does not divide itself */
int clone( int (*fn)( void *arg ), void *stack, int flags, void *arg, ... )
{
	(void)fn; (void)stack; (void)flags; (void)arg;
	errno = ENOSYS;
	return -1;
}

int unshare( int flags )
{
	(void)flags;
	errno = ENOSYS;
	return -1;
}

int posix_fadvise( int fd, off_t offset, off_t len, int advice )
{
	(void)fd; (void)offset; (void)len; (void)advice;
	return 0;
}

int __ts_cpu_count( const cpu_set_t *s )
{
	int	n = 0;
	size_t	i;

	for ( i = 0; i < sizeof(s->__bits) / sizeof(s->__bits[0]); i++ ) {
		n += __builtin_popcountl(s->__bits[i]);
	}
	return n;
}

int sched_getaffinity( int pid, size_t size, cpu_set_t *set )
{
	int	i, n = get_nprocs();

	(void)pid;
	if ( size < sizeof(cpu_set_t) ) {
		errno = EINVAL;
		return -1;
	}
	CPU_ZERO(set);
	for ( i = 0; i < n; i++ ) CPU_SET(i, set);
	return 0;
}

int sched_setaffinity( int pid, size_t size, const cpu_set_t *set )
{
	(void)pid; (void)size; (void)set;
	return 0;
}

int sched_getcpu( void )
{
	return 0;
}
