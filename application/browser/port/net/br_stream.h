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
 *	hand: the operations here take four address bytes and a port in the
 *	host's order, so that the callers need no socket headers; br_stream.c
 *	is the only unit that includes them.
 */

#ifndef BR_STREAM_H
#define BR_STREAM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BR_STREAM_WOULDBLOCK	(-1)
#define BR_STREAM_ERROR		(-2)

/* A nonblocking IPv4 stream socket, not yet connected: its descriptor, or BR_STREAM_ERROR */
int br_stream_open(void);

/* Bound to a local address ('ip4' NULL for any, 'port' 0 for any): 0 or BR_STREAM_ERROR */
int br_stream_bind(int fd, const unsigned char *ip4, int port);

/* 0 connected at once, BR_STREAM_WOULDBLOCK while it is being made, or BR_STREAM_ERROR */
int br_stream_connect(int fd, const unsigned char *ip4, int port);

/* A connection being made: 1 made, 0 still trying, BR_STREAM_ERROR failed */
int br_stream_connected(int fd);

/* Bytes sent (>= 0), BR_STREAM_WOULDBLOCK or BR_STREAM_ERROR */
int br_stream_send(int fd, const void *buf, int len);

/* Bytes read (> 0), 0 at the end of the stream, BR_STREAM_WOULDBLOCK or BR_STREAM_ERROR */
int br_stream_recv(int fd, void *buf, int len);

/* The address the socket is bound to: 0 or BR_STREAM_ERROR */
int br_stream_local(int fd, unsigned char *ip4, int *port);

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
 * Lookups on a thread of their own: br_stream_resolve_start answers a
 * slot to look at, -2 when answered at once (ip4 filled) or -1 when it
 * failed at once; br_stream_resolve_poll answers 1 (ip4 filled, the slot
 * let go), 0 still looking, -1 failed (the slot let go);
 * br_stream_resolve_cancel gives a slot up.
 */
int br_stream_resolve_start(const char *host, unsigned char *ip4);
int br_stream_resolve_poll(int slot, unsigned char *ip4);
void br_stream_resolve_cancel(int slot);

/*
 * The network thread: started once, asked whether it runs and whether
 * the caller is it, and woken when work is handed to it. Semaphores to
 * hand work over and wait for the answer.
 */
int br_net_thread_start(void);
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
