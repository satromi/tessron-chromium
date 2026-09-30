/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_stream.cc
 *	ブラウザ: nonblocking TCP by address over the system's sockets, the
 *	lookup of names on threads of their own, and the network thread
 *	(design 17.19). See br_stream.h.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include "br_stream.h"

namespace {

void fill_sa( struct sockaddr_in *sa, const unsigned char *ip4, int port )
{
	memset(sa, 0, sizeof(*sa));
	sa->sin_family = AF_INET;
	sa->sin_port = htons((unsigned short)port);
	if ( ip4 != nullptr ) memcpy(&sa->sin_addr, ip4, 4);
}

void take_sa( const struct sockaddr_in *sa, unsigned char *ip4, int *port )
{
	if ( ip4 != nullptr ) memcpy(ip4, &sa->sin_addr, 4);
	if ( port != nullptr ) *port = ntohs(sa->sin_port);
}

int map_result( long rv )
{
	if ( rv >= 0 ) return (int)rv;
	if ( errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS ) return BR_STREAM_WOULDBLOCK;
	return BR_STREAM_ERROR;
}

int set_int_opt( int fd, int level, int name, int value )
{
	return ( setsockopt(fd, level, name, &value, sizeof(value)) < 0 ) ? BR_STREAM_ERROR : 0;
}

/*
 * Readiness is looked at with no wait: the network thread moves every
 * socket in turn and must not stop in one of them
 */
int poll_ready( int fd, short what )
{
	struct pollfd p;

	p.fd = fd;
	p.events = what;
	p.revents = 0;
	int rv = poll(&p, 1, 0);
	if ( rv < 0 ) return BR_STREAM_ERROR;
	if ( rv == 0 ) return 0;
	if ( ( p.revents & ( what | POLLHUP ) ) != 0 ) return 1;
	return ( ( p.revents & ( POLLERR | POLLNVAL ) ) != 0 ) ? BR_STREAM_ERROR : 0;
}

}  // namespace

extern "C" {

int br_stream_open( void )
{
	int s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if ( s < 0 ) {
		printf("browser: net: no socket (%d)\n", errno);
		return BR_STREAM_ERROR;
	}
	return s;
}

int br_stream_bind( int fd, const unsigned char *ip4, int port )
{
	struct sockaddr_in sa;

	fill_sa(&sa, ip4, port);
	return ( bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ) ? BR_STREAM_ERROR : 0;
}

int br_stream_connect( int fd, const unsigned char *ip4, int port )
{
	struct sockaddr_in sa;

	fill_sa(&sa, ip4, port);
	if ( connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0 || errno == EISCONN ) return 0;
	if ( errno == EINPROGRESS || errno == EAGAIN || errno == EWOULDBLOCK ) return BR_STREAM_WOULDBLOCK;
	printf("browser: net: connect %d.%d.%d.%d:%d: %d\n", ip4[0], ip4[1], ip4[2], ip4[3], port, errno);
	return BR_STREAM_ERROR;
}

/* Made when the socket can be written to and holds no error */
int br_stream_connected( int fd )
{
	int r = poll_ready(fd, POLLOUT);
	if ( r <= 0 ) return r;
	int err = 0;
	socklen_t len = sizeof(err);
	if ( getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0 ) {
		printf("browser: net: the connection failed (%d)\n", err);
		return BR_STREAM_ERROR;
	}
	return 1;
}

int br_stream_send( int fd, const void *buf, int len )
{
	return map_result((long)send(fd, buf, (size_t)len, MSG_DONTWAIT));
}

int br_stream_recv( int fd, void *buf, int len )
{
	return map_result((long)recv(fd, buf, (size_t)len, MSG_DONTWAIT));
}

int br_stream_local( int fd, unsigned char *ip4, int *port )
{
	struct sockaddr_in sa;
	socklen_t len = sizeof(sa);

	memset(&sa, 0, sizeof(sa));
	if ( getsockname(fd, (struct sockaddr *)&sa, &len) < 0 ) return BR_STREAM_ERROR;
	take_sa(&sa, ip4, port);
	return 0;
}

int br_stream_set_recv_buffer( int fd, int size )
{
	return set_int_opt(fd, SOL_SOCKET, SO_RCVBUF, size);
}

int br_stream_set_send_buffer( int fd, int size )
{
	return set_int_opt(fd, SOL_SOCKET, SO_SNDBUF, size);
}

int br_stream_set_nodelay( int fd, int on )
{
	return set_int_opt(fd, IPPROTO_TCP, TCP_NODELAY, on ? 1 : 0);
}

int br_stream_set_keepalive( int fd, int on, int idle_seconds )
{
	/* the time before the probes is not set here; whether they are made is */
	(void)idle_seconds;
	return set_int_opt(fd, SOL_SOCKET, SO_KEEPALIVE, on ? 1 : 0);
}

int br_stream_readable( int fd )
{
	return poll_ready(fd, POLLIN);
}

int br_stream_writable( int fd )
{
	return poll_ready(fd, POLLOUT);
}

void br_stream_close( int fd )
{
	if ( fd >= 0 ) close(fd);
}

}  /* extern "C" */

/* ---------------------------------------------------------------- lookups */

namespace {

/*
 * A lookup waits on the name server for as long as it takes: each is made
 * on a thread of its own, and the asker looks at its slot until the
 * answer is there
 */
enum { SLOT_FREE, SLOT_LOOKING, SLOT_DONE, SLOT_FAILED, SLOT_ABANDONED };

struct Lookup {
	volatile int	state = SLOT_FREE;
	char		host[256];
	unsigned char	ip4[4];
};

constexpr int kSlots = 32;
Lookup g_slots[kSlots];
pthread_mutex_t g_slot_lock = PTHREAD_MUTEX_INITIALIZER;

/* A dotted quad is its own address */
bool parse_quad( const char *host, unsigned char *ip4 )
{
	unsigned v[4];
	char tail;
	if ( sscanf(host, "%u.%u.%u.%u%c", &v[0], &v[1], &v[2], &v[3], &tail) != 4 ) return false;
	for ( int i = 0; i < 4; i++ ) {
		if ( v[i] > 255 ) return false;
		ip4[i] = (unsigned char)v[i];
	}
	return true;
}

bool system_lookup( const char *host, unsigned char *ip4 )
{
	struct addrinfo hints, *res = nullptr;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	if ( getaddrinfo(host, nullptr, &hints, &res) != 0 || res == nullptr ) return false;
	const struct sockaddr_in *sa = (const struct sockaddr_in *)res->ai_addr;
	memcpy(ip4, &sa->sin_addr, 4);
	freeaddrinfo(res);
	return true;
}

void *lookup_thread( void *arg )
{
	Lookup *l = (Lookup *)arg;
	unsigned char ip4[4];
	bool ok = system_lookup(l->host, ip4);

	pthread_mutex_lock(&g_slot_lock);
	if ( l->state == SLOT_ABANDONED ) {
		l->state = SLOT_FREE;
	} else {
		memcpy(l->ip4, ip4, 4);
		l->state = ok ? SLOT_DONE : SLOT_FAILED;
	}
	pthread_mutex_unlock(&g_slot_lock);
	return nullptr;
}

}  // namespace

extern "C" {

int br_stream_resolve_start( const char *host, unsigned char *ip4 )
{
	if ( parse_quad(host, ip4) ) return -2;
	pthread_mutex_lock(&g_slot_lock);
	int i;
	for ( i = 0; i < kSlots && g_slots[i].state != SLOT_FREE; i++ ) ;
	if ( i == kSlots ) {
		pthread_mutex_unlock(&g_slot_lock);
		/* every slot busy: looked up here */
		return system_lookup(host, ip4) ? -2 : -1;
	}
	Lookup *l = &g_slots[i];
	snprintf(l->host, sizeof(l->host), "%s", host);
	l->state = SLOT_LOOKING;
	pthread_mutex_unlock(&g_slot_lock);

	pthread_t th;
	pthread_attr_t a;
	pthread_attr_init(&a);
	pthread_attr_setstacksize(&a, 256 * 1024);
	pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
	int rc = pthread_create(&th, &a, lookup_thread, l);
	pthread_attr_destroy(&a);
	if ( rc != 0 ) {
		bool ok = system_lookup(host, ip4);
		pthread_mutex_lock(&g_slot_lock);
		l->state = SLOT_FREE;
		pthread_mutex_unlock(&g_slot_lock);
		return ok ? -2 : -1;
	}
	return i;
}

int br_stream_resolve_poll( int slot, unsigned char *ip4 )
{
	if ( slot < 0 || slot >= kSlots ) return -1;
	pthread_mutex_lock(&g_slot_lock);
	Lookup *l = &g_slots[slot];
	int r = 0;
	if ( l->state == SLOT_DONE ) {
		memcpy(ip4, l->ip4, 4);
		l->state = SLOT_FREE;
		r = 1;
	} else if ( l->state == SLOT_FAILED ) {
		l->state = SLOT_FREE;
		r = -1;
	}
	pthread_mutex_unlock(&g_slot_lock);
	return r;
}

void br_stream_resolve_cancel( int slot )
{
	if ( slot < 0 || slot >= kSlots ) return;
	pthread_mutex_lock(&g_slot_lock);
	Lookup *l = &g_slots[slot];
	if ( l->state == SLOT_LOOKING ) {
		l->state = SLOT_ABANDONED;	/* the thread lets it go */
	} else if ( l->state != SLOT_ABANDONED ) {
		l->state = SLOT_FREE;
	}
	pthread_mutex_unlock(&g_slot_lock);
}

}  /* extern "C" */

/* ---------------------------------------------------------------- the network thread */

/*
 * The network stack runs on a thread of its own: every ten milliseconds,
 * or at once when it is handed work, it moves the sockets and runs what
 * the stack has due. On the page's thread it would wait whenever the
 * page is busy (a script, a collection, a long layout), and every
 * connection with it.
 */

extern "C" void br_net_thread_turn( void );

namespace {

constexpr int kSems = 64;
sem_t *g_sems[kSems];
pthread_mutex_t g_sem_lock = PTHREAD_MUTEX_INITIALIZER;

int g_wake = -1;
volatile int g_net_up = 0;
pthread_t g_net_th;

struct timespec after_ms( int ms )
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	t.tv_sec += ms / 1000;
	t.tv_nsec += (long)( ms % 1000 ) * 1000000L;
	if ( t.tv_nsec >= 1000000000L ) {
		t.tv_sec++;
		t.tv_nsec -= 1000000000L;
	}
	return t;
}

void *net_thread( void * )
{
	for ( ;; ) {
		(void)br_sem_wait_ms(g_wake, 10);
		br_net_thread_turn();
	}
	return nullptr;
}

}  // namespace

extern "C" {

int br_net_thread_start( void )
{
	if ( g_net_up ) return 0;
	g_wake = br_sem_new();
	if ( g_wake < 0 ) return -1;
	pthread_attr_t a;
	pthread_attr_init(&a);
	pthread_attr_setstacksize(&a, 8 * 1024 * 1024);
	pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
	int rc = pthread_create(&g_net_th, &a, net_thread, nullptr);
	pthread_attr_destroy(&a);
	if ( rc != 0 ) {
		printf("browser: net: no network thread (%d)\n", rc);
		return -1;
	}
	g_net_up = 1;
	return 0;
}

int br_net_thread_up( void )
{
	return g_net_up;
}

int br_on_net_thread( void )
{
	return g_net_up && pthread_equal(pthread_self(), g_net_th);
}

void br_net_wake( void )
{
	if ( g_wake >= 0 ) br_sem_post(g_wake);
}

int br_sem_new( void )
{
	sem_t *s = (sem_t *)malloc(sizeof(sem_t));
	if ( s == nullptr || sem_init(s, 0, 0) != 0 ) {
		free(s);
		return -1;
	}
	pthread_mutex_lock(&g_sem_lock);
	for ( int i = 0; i < kSems; i++ ) {
		if ( g_sems[i] == nullptr ) {
			g_sems[i] = s;
			pthread_mutex_unlock(&g_sem_lock);
			return i;
		}
	}
	pthread_mutex_unlock(&g_sem_lock);
	sem_destroy(s);
	free(s);
	return -1;
}

void br_sem_wait( int sem )
{
	if ( sem < 0 || sem >= kSems || g_sems[sem] == nullptr ) return;
	while ( sem_wait(g_sems[sem]) != 0 && errno == EINTR ) ;
}

void br_sem_post( int sem )
{
	if ( sem < 0 || sem >= kSems || g_sems[sem] == nullptr ) return;
	sem_post(g_sems[sem]);
}

/* 1 when posted within 'ms' milliseconds, 0 otherwise */
int br_sem_wait_ms( int sem, int ms )
{
	if ( sem < 0 || sem >= kSems || g_sems[sem] == nullptr ) return 0;
	struct timespec t = after_ms(ms);
	return sem_clockwait(g_sems[sem], CLOCK_MONOTONIC, &t) == 0;
}

void br_sem_free( int sem )
{
	if ( sem < 0 || sem >= kSems ) return;
	pthread_mutex_lock(&g_sem_lock);
	sem_t *s = g_sems[sem];
	g_sems[sem] = nullptr;
	pthread_mutex_unlock(&g_sem_lock);
	if ( s != nullptr ) {
		sem_destroy(s);
		free(s);
	}
}

}  /* extern "C" */
