/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_stream.h
 *	ブラウザ: nonblocking TCP by address, the lookup of names and the
 *	network thread the port's network stack runs on (design 17.19).
 *
 *	The stack does its own resolving and arrives with an address in
 *	hand: the operations here take an address (BrAddr: the bytes of an
 *	IPv4 or an IPv6 one) and a port in the host's order, so that the
 *	callers need no socket headers; br_stream.cc is the only unit that
 *	includes them.
 */

#ifndef BR_STREAM_H
#define BR_STREAM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BR_STREAM_WOULDBLOCK	(-1)
#define BR_STREAM_ERROR		(-2)

/* An address: 4 bytes of IPv4 or 16 of IPv6, in network order, with the zone of a link-local IPv6 one */
typedef struct {
	int		len;		/* 4 or 16 */
	unsigned char	addr[16];
	unsigned int	scope;		/* the interface's index, 0 for none */
} BrAddr;

/* A nonblocking stream socket of IPv4, or of IPv6 ('ipv6' 1), not yet connected: its descriptor, or BR_STREAM_ERROR */
int br_stream_open(int ipv6);

/* Bound to a local address ('a' NULL for IPv4's any, 'port' 0 for any): 0 or BR_STREAM_ERROR */
int br_stream_bind(int fd, const BrAddr *a, int port);

/* 0 connected at once, BR_STREAM_WOULDBLOCK while it is being made, or BR_STREAM_ERROR */
int br_stream_connect(int fd, const BrAddr *a, int port);

/* A connection being made: 1 made, 0 still trying, BR_STREAM_ERROR failed */
int br_stream_connected(int fd);

/* Bytes sent (>= 0), BR_STREAM_WOULDBLOCK or BR_STREAM_ERROR */
int br_stream_send(int fd, const void *buf, int len);

/* Bytes read (> 0), 0 at the end of the stream, BR_STREAM_WOULDBLOCK or BR_STREAM_ERROR */
int br_stream_recv(int fd, void *buf, int len);

/* The address the socket is bound to: 0 or BR_STREAM_ERROR */
int br_stream_local(int fd, BrAddr *a, int *port);

/* Options: 0 or BR_STREAM_ERROR */
int br_stream_set_recv_buffer(int fd, int size);
int br_stream_set_send_buffer(int fd, int size);
int br_stream_set_nodelay(int fd, int on);
int br_stream_set_keepalive(int fd, int on, int idle_seconds);

/* Whether data waits, or there is room to send: 1, 0 or BR_STREAM_ERROR */
int br_stream_readable(int fd);
int br_stream_writable(int fd);

void br_stream_close(int fd);

/*
 * Lookups on a thread of their own, of the addresses of a family
 * ('family' 4 or 6, or 0 for both, those the machine has no address of
 * its own to reach the world with left out), in the order to try them,
 * up to BR_RESOLVE_MAX into 'out' and how many into '*n'.
 * br_stream_resolve_start answers a slot to look at, -2 when answered at
 * once (a number: out filled) or -1 when it failed at once;
 * br_stream_resolve_poll answers 1 (out filled, the slot let go), 0
 * still looking, -1 failed (the slot let go); br_stream_resolve_cancel
 * gives a slot up.
 */
#define BR_RESOLVE_MAX	16

int br_stream_resolve_start(const char *host, int family, BrAddr *out, int *n);
int br_stream_resolve_poll(int slot, BrAddr *out, int *n);
void br_stream_resolve_cancel(int slot);

/*
 * A socket the network thread waits on between its turns: what for
 * (BR_WATCH_READ, BR_WATCH_WRITE; a connection being made waits to be
 * written to). The stack's stream sockets and datagram sockets list
 * theirs (br_tcp_watches, br_udp_watches), each up to 'max' into 'out',
 * how many; br_net_due_in_ms is how long until the stack's next timer,
 * -1 when it has none. They are asked on the network thread.
 */
#define BR_WATCH_READ	1
#define BR_WATCH_WRITE	2

typedef struct {
	int	fd;
	int	events;
} BrWatch;

int br_tcp_watches(BrWatch *out, int max);
int br_udp_watches(BrWatch *out, int max);
long br_net_due_in_ms(void);

/*
 * The network thread: started once, asked whether it runs and whether
 * the caller is it, and woken when work is handed to it. Semaphores to
 * hand work over and wait for the answer. 'stack': the network stack is
 * this process's, so that the thread is woken through a socket of the
 * loopback with the sockets it waits on; else (a renderer, whose network
 * is the network process's and which may not use sockets) it waits on a
 * semaphore.
 */
int br_net_thread_start(int stack);
int br_net_thread_up(void);
int br_on_net_thread(void);
void br_net_wake(void);
int br_sem_new(void);
void br_sem_wait(int sem);
void br_sem_post(int sem);
int br_sem_wait_ms(int sem, int ms);
void br_sem_free(int sem);

#ifdef __cplusplus
}
#endif

#endif /* BR_STREAM_H */
