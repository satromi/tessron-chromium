/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	ifaddrs.h
 *	The list of the interfaces' addresses. The system cannot be asked
 *	for it: getifaddrs answers that it has none (the network stack then
 *	lets the system choose the source of a connection).
 */

#ifndef __TS_SHIM_IFADDRS_H__
#define __TS_SHIM_IFADDRS_H__

#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ifaddrs {
	struct ifaddrs	*ifa_next;
	char		*ifa_name;
	unsigned int	ifa_flags;
	struct sockaddr	*ifa_addr;
	struct sockaddr	*ifa_netmask;
	union {
		struct sockaddr	*ifu_broadaddr;
		struct sockaddr	*ifu_dstaddr;
	} ifa_ifu;
	void		*ifa_data;
};

#define ifa_broadaddr	ifa_ifu.ifu_broadaddr
#define ifa_dstaddr	ifa_ifu.ifu_dstaddr

int	getifaddrs( struct ifaddrs **ifap );
void	freeifaddrs( struct ifaddrs *ifa );

#ifdef __cplusplus
}
#endif

#endif /* __TS_SHIM_IFADDRS_H__ */
