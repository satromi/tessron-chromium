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
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <algorithm>
#include <vector>

#include "br_stream.h"

namespace {

/* The socket address of 'a' (NULL: IPv4's any) and 'port'; its length */
socklen_t fill_sa( struct sockaddr_storage *ss, const BrAddr *a, int port )
{
	memset(ss, 0, sizeof(*ss));
	if ( a != nullptr && a->len == 16 ) {
		struct sockaddr_in6 *sa = (struct sockaddr_in6 *)ss;

		sa->sin6_family = AF_INET6;
		sa->sin6_port = htons((unsigned short)port);
		memcpy(&sa->sin6_addr, a->addr, 16);
		sa->sin6_scope_id = a->scope;
		return sizeof(*sa);
	}
	struct sockaddr_in *sa = (struct sockaddr_in *)ss;

	sa->sin_family = AF_INET;
	sa->sin_port = htons((unsigned short)port);
	if ( a != nullptr ) memcpy(&sa->sin_addr, a->addr, 4);
	return sizeof(*sa);
}

void take_sa( const struct sockaddr_storage *ss, BrAddr *a, int *port )
{
	if ( ss->ss_family == AF_INET6 ) {
		const struct sockaddr_in6 *sa = (const struct sockaddr_in6 *)ss;

		if ( a != nullptr ) {
			a->len = 16;
			memcpy(a->addr, &sa->sin6_addr, 16);
			a->scope = sa->sin6_scope_id;
		}
		if ( port != nullptr ) *port = ntohs(sa->sin6_port);
		return;
	}
	const struct sockaddr_in *sa = (const struct sockaddr_in *)ss;

	if ( a != nullptr ) {
		memset(a, 0, sizeof(*a));
		a->len = 4;
		memcpy(a->addr, &sa->sin_addr, 4);
	}
	if ( port != nullptr ) *port = ntohs(sa->sin_port);
}

/* An address as text, for what is said on the console */
const char *addr_text( const BrAddr *a, char *t, socklen_t len )
{
	if ( inet_ntop(( a->len == 16 ) ? AF_INET6 : AF_INET, a->addr, t, len) == nullptr ) {
		snprintf(t, len, "?");
	}
	return t;
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
 * What the network thread's wait found (wait_for_work), for the turn that
 * follows: the sockets it waited on and what came of each. A look at one
 * of them during the turn is answered from here rather than by asking the
 * system again; that a socket is ready is answered once (the read or the
 * write it lets through may use it up), and a socket not waited on is
 * asked. The list is in the order of the descriptors, and as long as the
 * sockets waited on: it grows with them, the system's limit being the
 * process's own (so_getlimit).
 */
struct Seen {
	int	fd;
	short	asked;
	short	got;
};

std::vector<Seen>	g_seen;			/* the network thread's alone */
size_t			g_nseen = 0;

/*
 * Readiness is looked at with no wait: the network thread moves every
 * socket in turn and must not stop in one of them
 */
int poll_ready( int fd, short what )
{
	if ( g_nseen > 0 && br_on_net_thread() ) {
		auto end = g_seen.begin() + (long)g_nseen;
		auto it = std::lower_bound(g_seen.begin(), end, fd,
					   []( const Seen &e, int f ) { return e.fd < f; });
		if ( it != end && it->fd == fd && ( it->asked & what ) != 0 ) {
			Seen &e = *it;
			int r = ( ( e.got & ( what | POLLHUP ) ) != 0 ) ? 1
				: ( ( e.got & ( POLLERR | POLLNVAL ) ) != 0 ) ? BR_STREAM_ERROR : 0;
			if ( r != 0 ) e.asked = (short)( e.asked & ~what );
			return r;
		}
	}
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

int br_stream_open( int ipv6 )
{
	int s = socket(ipv6 ? AF_INET6 : AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if ( s < 0 ) {
		printf("browser: net: no socket (%d)\n", errno);
		return BR_STREAM_ERROR;
	}
	if ( ipv6 ) {
		/* an IPv6 socket reaches IPv6 addresses only: IPv4 ones have sockets of their own */
		int on = 1;
		(void)setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on));
	}
	return s;
}

int br_stream_bind( int fd, const BrAddr *a, int port )
{
	struct sockaddr_storage ss;
	socklen_t len = fill_sa(&ss, a, port);

	return ( bind(fd, (struct sockaddr *)&ss, len) < 0 ) ? BR_STREAM_ERROR : 0;
}

int br_stream_connect( int fd, const BrAddr *a, int port )
{
	struct sockaddr_storage ss;
	socklen_t len = fill_sa(&ss, a, port);
	char t[INET6_ADDRSTRLEN];

	if ( connect(fd, (struct sockaddr *)&ss, len) == 0 || errno == EISCONN ) return 0;
	if ( errno == EINPROGRESS || errno == EAGAIN || errno == EWOULDBLOCK ) return BR_STREAM_WOULDBLOCK;
	printf("browser: net: connect %s%s%s:%d: %d\n", ( a->len == 16 ) ? "[" : "", addr_text(a, t, sizeof(t)),
	       ( a->len == 16 ) ? "]" : "", port, errno);
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

int br_stream_local( int fd, BrAddr *a, int *port )
{
	struct sockaddr_storage ss;
	socklen_t len = sizeof(ss);

	memset(&ss, 0, sizeof(ss));
	if ( getsockname(fd, (struct sockaddr *)&ss, &len) < 0 ) return BR_STREAM_ERROR;
	take_sa(&ss, a, port);
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
	int		family;
	BrAddr		out[BR_RESOLVE_MAX];
	int		n;
};

constexpr int kSlots = 32;
Lookup g_slots[kSlots];
pthread_mutex_t g_slot_lock = PTHREAD_MUTEX_INITIALIZER;

/* A number is its own address: a dotted quad, or IPv6 (with or without its brackets) */
bool parse_number( const char *host, int family, BrAddr *a )
{
	char t[64];
	size_t n = strlen(host);

	memset(a, 0, sizeof(*a));
	if ( family != 6 && inet_pton(AF_INET, host, a->addr) == 1 ) {
		a->len = 4;
		return true;
	}
	if ( n >= 2 && host[0] == '[' && host[n - 1] == ']' && n - 2 < sizeof(t) ) {
		memcpy(t, host + 1, n - 2);
		t[n - 2] = 0;
		host = t;
	}
	if ( family != 4 && strchr(host, '%') == nullptr && inet_pton(AF_INET6, host, a->addr) == 1 ) {
		a->len = 16;
		return true;
	}
	return false;
}

/* The addresses of a name from the system, in its order; how many, 0 for none */
int system_lookup( const char *host, int family, BrAddr *out )
{
	struct addrinfo hints, *res = nullptr, *ai;
	int n = 0;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = ( family == 4 ) ? AF_INET : ( family == 6 ) ? AF_INET6 : AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if ( family == 0 ) hints.ai_flags = AI_ADDRCONFIG;
	if ( getaddrinfo(host, nullptr, &hints, &res) != 0 || res == nullptr ) return 0;
	for ( ai = res; ai != nullptr && n < BR_RESOLVE_MAX; ai = ai->ai_next ) {
		if ( ai->ai_family != AF_INET && ai->ai_family != AF_INET6 ) continue;
		take_sa((const struct sockaddr_storage *)ai->ai_addr, &out[n], nullptr);
		n++;
	}
	freeaddrinfo(res);
	return n;
}

void *lookup_thread( void *arg )
{
	Lookup *l = (Lookup *)arg;
	BrAddr out[BR_RESOLVE_MAX];
	int n = system_lookup(l->host, l->family, out);

	pthread_mutex_lock(&g_slot_lock);
	if ( l->state == SLOT_ABANDONED ) {
		l->state = SLOT_FREE;
	} else {
		memcpy(l->out, out, sizeof(BrAddr) * (size_t)n);
		l->n = n;
		l->state = ( n > 0 ) ? SLOT_DONE : SLOT_FAILED;
	}
	pthread_mutex_unlock(&g_slot_lock);
	return nullptr;
}

}  // namespace

extern "C" {

int br_stream_resolve_start( const char *host, int family, BrAddr *out, int *n )
{
	if ( parse_number(host, family, &out[0]) ) {
		*n = 1;
		return -2;
	}
	pthread_mutex_lock(&g_slot_lock);
	int i;
	for ( i = 0; i < kSlots && g_slots[i].state != SLOT_FREE; i++ ) ;
	if ( i == kSlots ) {
		pthread_mutex_unlock(&g_slot_lock);
		/* every slot busy: looked up here */
		*n = system_lookup(host, family, out);
		return ( *n > 0 ) ? -2 : -1;
	}
	Lookup *l = &g_slots[i];
	snprintf(l->host, sizeof(l->host), "%s", host);
	l->family = family;
	l->n = 0;
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
		*n = system_lookup(host, family, out);
		pthread_mutex_lock(&g_slot_lock);
		l->state = SLOT_FREE;
		pthread_mutex_unlock(&g_slot_lock);
		return ( *n > 0 ) ? -2 : -1;
	}
	return i;
}

int br_stream_resolve_poll( int slot, BrAddr *out, int *n )
{
	if ( slot < 0 || slot >= kSlots ) return -1;
	pthread_mutex_lock(&g_slot_lock);
	Lookup *l = &g_slots[slot];
	int r = 0;
	if ( l->state == SLOT_DONE ) {
		memcpy(out, l->out, sizeof(BrAddr) * (size_t)l->n);
		*n = l->n;
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
 * The network stack runs on a thread of its own. Between its turns it
 * waits in one poll on every socket the stack has parked a read or a
 * write on and on a datagram socket of its own on the loopback, which
 * another thread writes to when it hands work over; the wait ends no
 * later than the stack's next timer. A datagram that arrives, a
 * connection that is made or work that is handed over is taken up at
 * once, not at the next tick of a clock. On the page's thread the stack
 * would wait whenever the page is busy (a script, a collection, a long
 * layout), and every connection with it.
 */

extern "C" void br_net_thread_turn( void );

namespace {

constexpr int kSems = 64;
sem_t *g_sems[kSems];
pthread_mutex_t g_sem_lock = PTHREAD_MUTEX_INITIALIZER;

int g_wake = -1;
volatile int g_net_up = 0;
pthread_t g_net_th;

/* The socket the network thread is woken through, and its own address */
int g_wake_fd = -1;
struct sockaddr_in g_wake_to;
/* Set from a wake until the network thread has taken it: one datagram for many hand-overs */
volatile int g_wake_sent = 0;

/* The longest wait: what nothing wakes the thread for is looked at this often */
constexpr int kWaitMaxMs = 100;

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

/* The socket to be woken through: bound to the loopback at a port of the system's choosing */
void make_wake_socket( void )
{
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
	if ( fd < 0 ) return;
	struct sockaddr_in a;
	socklen_t len = sizeof(a);
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if ( bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0
	     || getsockname(fd, (struct sockaddr *)&a, &len) < 0 ) {
		close(fd);
		return;
	}
	g_wake_to = a;
	g_wake_fd = fd;
}

/*
 * The watches of the TCP streams and the datagram sockets into w, all of
 * them: a list they fill is made twice as long and filled again, as the
 * two say no more than how many went in
 */
constexpr size_t kWatchFirst = 64;

size_t take_watches( std::vector<BrWatch> &w )
{
	if ( w.size() < kWatchFirst ) w.resize(kWatchFirst);
	for ( ;; ) {
		const int room = (int)w.size();
		const int t = br_tcp_watches(w.data(), room);
		if ( t < room ) {
			const int u = br_udp_watches(w.data() + t, room - t);
			if ( t + u < room ) return (size_t)( t + u );
		}
		w.resize(w.size() * 2);
	}
}

/*
 * Waits until a parked socket is ready, work is handed over or the
 * stack's next timer is due, and keeps what the wait found for the turn
 * that follows (g_seen). Every socket watched is in the one wait, once,
 * with what each of its watches asks for.
 */
void wait_for_work( void )
{
	static std::vector<BrWatch>		w;
	static std::vector<struct pollfd>	p;
	size_t	n = take_watches(w), m = 0;

	long due = br_net_due_in_ms();
	int ms = ( due < 0 || due > kWaitMaxMs ) ? kWaitMaxMs : (int)due;
	g_nseen = 0;
	if ( g_wake_fd < 0 ) {
		/*
		 * No socket to be woken through: the semaphore br_net_wake posts.
		 * Sockets it cannot be waited for with are looked at every ten
		 * milliseconds; with none, the wait is until the work or the
		 * timer due.
		 */
		if ( n > 0 && ms > 10 ) ms = 10;
		if ( ms > 0 ) (void)br_sem_wait_ms(g_wake, ms);
		return;
	}
	std::sort(w.begin(), w.begin() + (long)n,
		  []( const BrWatch &a, const BrWatch &b ) { return a.fd < b.fd; });
	if ( p.size() < n + 1 ) p.resize(n + 1);
	p[m].fd = g_wake_fd;
	p[m].events = POLLIN;
	p[m].revents = 0;
	m++;
	for ( size_t i = 0; i < n; i++ ) {
		short ev = (short)( ( ( w[i].events & BR_WATCH_READ ) ? POLLIN : 0 )
				  | ( ( w[i].events & BR_WATCH_WRITE ) ? POLLOUT : 0 ) );
		if ( m > 1 && p[m - 1].fd == w[i].fd ) {
			p[m - 1].events |= ev;
			continue;
		}
		p[m].fd = w[i].fd;
		p[m].events = ev;
		p[m].revents = 0;
		m++;
	}
	__sync_synchronize();
	if ( g_wake_sent ) ms = 0;
	int rv = poll(p.data(), (nfds_t)m, ms);
	if ( rv < 0 ) {
		/* the wait refused: a short wait on the wake alone, the sockets looked at in the turn */
		rv = poll(p.data(), 1, ( ms > 10 ) ? 10 : ms);
		m = 1;
	}
	if ( rv > 0 && ( p[0].revents & POLLIN ) != 0 ) {
		char b[16];
		while ( recv(g_wake_fd, b, sizeof(b), MSG_DONTWAIT) > 0 ) ;
	}
	/* work handed over from here on wakes the next wait; what came before runs in the turn that follows */
	__sync_lock_release(&g_wake_sent);
	if ( rv < 0 ) return;
	if ( g_seen.size() < m ) g_seen.resize(m);
	for ( size_t j = 1; j < m; j++ ) {
		g_seen[g_nseen].fd = p[j].fd;
		g_seen[g_nseen].asked = p[j].events;
		g_seen[g_nseen].got = p[j].revents;
		g_nseen++;
	}
}

void *net_thread( void * )
{
	for ( ;; ) {
		wait_for_work();
		br_net_thread_turn();
		g_nseen = 0;
	}
	return nullptr;
}

}  // namespace

extern "C" {

int br_net_thread_start( int stack )
{
	if ( g_net_up ) return 0;
	g_wake = br_sem_new();
	if ( g_wake < 0 ) return -1;
	if ( stack ) {
		make_wake_socket();
		if ( g_wake_fd < 0 ) printf("browser: net: no socket to wake the network thread; it looks every 10 ms\n");
	}
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
	if ( g_wake_fd >= 0 ) {
		if ( __sync_bool_compare_and_swap(&g_wake_sent, 0, 1) ) {
			(void)sendto(g_wake_fd, "w", 1, MSG_DONTWAIT, (struct sockaddr *)&g_wake_to, sizeof(g_wake_to));
		}
		return;
	}
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
