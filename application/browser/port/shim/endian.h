/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	endian.h
 *	glibc's byte order names and conversions. The machine is little
 *	endian (aarch64 as TessronOS runs it).
 */

#ifndef __TS_ENDIAN_H__
#define __TS_ENDIAN_H__

#include <stdint.h>

#ifndef __LITTLE_ENDIAN
#define __LITTLE_ENDIAN	1234
#define __BIG_ENDIAN	4321
#define __PDP_ENDIAN	3412
#define __BYTE_ORDER	__LITTLE_ENDIAN
#endif
#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN	__LITTLE_ENDIAN
#define BIG_ENDIAN	__BIG_ENDIAN
#define PDP_ENDIAN	__PDP_ENDIAN
#define BYTE_ORDER	__BYTE_ORDER
#endif

#define htobe16(x)	__builtin_bswap16((uint16_t)(x))
#define htole16(x)	((uint16_t)(x))
#define be16toh(x)	__builtin_bswap16((uint16_t)(x))
#define le16toh(x)	((uint16_t)(x))
#define htobe32(x)	__builtin_bswap32((uint32_t)(x))
#define htole32(x)	((uint32_t)(x))
#define be32toh(x)	__builtin_bswap32((uint32_t)(x))
#define le32toh(x)	((uint32_t)(x))
#define htobe64(x)	__builtin_bswap64((uint64_t)(x))
#define htole64(x)	((uint64_t)(x))
#define be64toh(x)	__builtin_bswap64((uint64_t)(x))
#define le64toh(x)	((uint64_t)(x))

#endif /* __TS_ENDIAN_H__ */
