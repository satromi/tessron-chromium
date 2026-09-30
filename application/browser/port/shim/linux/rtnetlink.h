/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	linux/rtnetlink.h
 *	The message of Linux's routing socket that tells of an interface
 *	address, as the network service's types carry it. TessronOS has no
 *	such socket; the type is only passed through.
 */

#ifndef __TS_LINUX_RTNETLINK_H__
#define __TS_LINUX_RTNETLINK_H__

#include <stdint.h>

struct ifaddrmsg {
	uint8_t		ifa_family;
	uint8_t		ifa_prefixlen;
	uint8_t		ifa_flags;
	uint8_t		ifa_scope;
	uint32_t	ifa_index;
};

#define RT_SCOPE_UNIVERSE	0
#define RT_SCOPE_SITE		200
#define RT_SCOPE_LINK		253
#define RT_SCOPE_HOST		254
#define RT_SCOPE_NOWHERE	255

#define IFA_F_SECONDARY		0x01
#define IFA_F_TEMPORARY		IFA_F_SECONDARY
#define IFA_F_NODAD		0x02
#define IFA_F_OPTIMISTIC	0x04
#define IFA_F_DADFAILED		0x08
#define IFA_F_HOMEADDRESS	0x10
#define IFA_F_DEPRECATED	0x20
#define IFA_F_TENTATIVE		0x40
#define IFA_F_PERMANENT		0x80

#endif /* __TS_LINUX_RTNETLINK_H__ */
