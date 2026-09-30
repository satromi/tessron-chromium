/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	sys/ioctl.h
 *	ioctl(): TessronOS's descriptors (files and sockets of the port) take no
 *	device requests; every request fails with ENOTTY
 *	(application/browser/port/compat/ts_linux.c).
 */

#ifndef __TS_SYS_IOCTL_H__
#define __TS_SYS_IOCTL_H__

#ifdef __cplusplus
extern "C" {
#endif

#define TCGETS		0x5401
#define TCSETS		0x5402
#define TIOCGWINSZ	0x5413
#define TIOCSWINSZ	0x5414
#define FIONREAD	0x541B
#define TIOCINQ		FIONREAD
#define FIONBIO		0x5421
#define FIOCLEX		0x5451
#define FIONCLEX	0x5450
#define SIOCGIFCONF	0x8912
#define SIOCGIFFLAGS	0x8913
#define SIOCGIFADDR	0x8915
#define SIOCGIFHWADDR	0x8927
#define SIOCGIFINDEX	0x8933

struct winsize {
	unsigned short	ws_row;
	unsigned short	ws_col;
	unsigned short	ws_xpixel;
	unsigned short	ws_ypixel;
};

int	ioctl( int fd, unsigned long request, ... );

#ifdef __cplusplus
}
#endif

#endif /* __TS_SYS_IOCTL_H__ */
