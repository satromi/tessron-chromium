/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	byteswap.h
 *	Byte order reversal of 16, 32 and 64 bit values, by the compiler.
 */

#ifndef __TS_BYTESWAP_H__
#define __TS_BYTESWAP_H__

#define bswap_16(x)	__builtin_bswap16(x)
#define bswap_32(x)	__builtin_bswap32(x)
#define bswap_64(x)	__builtin_bswap64(x)

#endif /* __TS_BYTESWAP_H__ */
