/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	asm-generic/socket.h
 *	The socket options' names, as sys/socket.h gives them, and the names
 *	of Linux's options the system's stack has not (path MTU discovery,
 *	the traffic class received, the time stamp of a datagram): a socket
 *	refuses them, and a program that asks goes on without.
 */

#ifndef __TS_ASM_GENERIC_SOCKET_H__
#define __TS_ASM_GENERIC_SOCKET_H__

#include <sys/socket.h>
#include <netinet/in.h>

#ifndef IP_MTU_DISCOVER
#define IP_MTU_DISCOVER		10
#endif
#ifndef IP_PMTUDISC_DONT
#define IP_PMTUDISC_DONT	0
#endif
#ifndef IP_PMTUDISC_DO
#define IP_PMTUDISC_DO		2
#endif
#ifndef IP_RECVTOS
#define IP_RECVTOS		13
#endif
#ifndef IPV6_RECVTCLASS
#define IPV6_RECVTCLASS		66
#endif
#ifndef SO_TIMESTAMP
#define SO_TIMESTAMP		29
#endif
#ifndef SCM_TIMESTAMP
#define SCM_TIMESTAMP		29
#endif

#endif /* __TS_ASM_GENERIC_SOCKET_H__ */
