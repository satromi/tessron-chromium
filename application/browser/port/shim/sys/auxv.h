/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	sys/auxv.h
 *	The auxiliary vector a Linux program asks about its machine with.
 *	There is none on TessronOS; getauxval answers from what the port knows:
 *	the page size, and the features of the Cortex-A76 both machines have
 *	(application/browser/port/compat/ts_linux.c).
 */

#ifndef __TS_SYS_AUXV_H__
#define __TS_SYS_AUXV_H__

#include <linux/auxvec.h>

#ifdef __cplusplus
extern "C" {
#endif

/* AT_HWCAP bits (arm64) */
#define HWCAP_FP	(1UL << 0)
#define HWCAP_ASIMD	(1UL << 1)
#define HWCAP_EVTSTRM	(1UL << 2)
#define HWCAP_AES	(1UL << 3)
#define HWCAP_PMULL	(1UL << 4)
#define HWCAP_SHA1	(1UL << 5)
#define HWCAP_SHA2	(1UL << 6)
#define HWCAP_CRC32	(1UL << 7)
#define HWCAP_ATOMICS	(1UL << 8)
#define HWCAP_FPHP	(1UL << 9)
#define HWCAP_ASIMDHP	(1UL << 10)
#define HWCAP_CPUID	(1UL << 11)
#define HWCAP_ASIMDRDM	(1UL << 12)
#define HWCAP_JSCVT	(1UL << 13)
#define HWCAP_FCMA	(1UL << 14)
#define HWCAP_LRCPC	(1UL << 15)
#define HWCAP_DCPOP	(1UL << 16)
#define HWCAP_SHA3	(1UL << 17)
#define HWCAP_SM3	(1UL << 18)
#define HWCAP_SM4	(1UL << 19)
#define HWCAP_ASIMDDP	(1UL << 20)
#define HWCAP_SHA512	(1UL << 21)
#define HWCAP_SVE	(1UL << 22)
#define HWCAP_ASIMDFHM	(1UL << 23)
#define HWCAP_DIT	(1UL << 24)
#define HWCAP_USCAT	(1UL << 25)
#define HWCAP_ILRCPC	(1UL << 26)
#define HWCAP_FLAGM	(1UL << 27)
#define HWCAP_SSBS	(1UL << 28)
#define HWCAP_SB	(1UL << 29)
#define HWCAP_PACA	(1UL << 30)
#define HWCAP_PACG	(1UL << 31)

unsigned long getauxval( unsigned long type );

#ifdef __cplusplus
}
#endif

#endif /* __TS_SYS_AUXV_H__ */
