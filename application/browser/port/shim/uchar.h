/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	uchar.h
 *	The C11 types of UTF-16 and UTF-32 code units, which newlib does not
 *	have. (C++ has them built in.)
 */

#ifndef __TS_UCHAR_H__
#define __TS_UCHAR_H__

#include <stdint.h>

#ifndef __cplusplus
typedef uint_least16_t	char16_t;
typedef uint_least32_t	char32_t;
#endif

#endif /* __TS_UCHAR_H__ */
