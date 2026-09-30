/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	linux/memfd.h
 *	The flags of memfd_create, which fails on TessronOS (there is no file
 *	made of anonymous memory; lib/libcxxrt/ts_mman.c).
 */

#ifndef __TS_LINUX_MEMFD_H__
#define __TS_LINUX_MEMFD_H__

#define MFD_CLOEXEC		0x0001U
#define MFD_ALLOW_SEALING	0x0002U
#define MFD_HUGETLB		0x0004U
#define MFD_NOEXEC_SEAL		0x0008U
#define MFD_EXEC		0x0010U

#endif /* __TS_LINUX_MEMFD_H__ */
