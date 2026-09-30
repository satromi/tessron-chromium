/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	sys/random.h
 *	Random bytes from the system's random source object 乱数, read by
 *	the C++ runtime of TessronOS (lib/libcxxrt/ts_urandom.c).
 */

#ifndef __TS_SYS_RANDOM_H__
#define __TS_SYS_RANDOM_H__

#include <stddef.h>
#include <sys/types.h>

#define GRND_NONBLOCK	0x0001
#define GRND_RANDOM	0x0002
#define GRND_INSECURE	0x0004

#ifdef __cplusplus
extern "C" {
#endif

ssize_t	getrandom( void *buf, size_t len, unsigned int flags );
int	getentropy( void *buf, size_t len );

#ifdef __cplusplus
}
#endif

#endif /* __TS_SYS_RANDOM_H__ */
