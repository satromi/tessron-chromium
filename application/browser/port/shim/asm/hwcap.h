/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	asm/hwcap.h
 *	The feature bits of AT_HWCAP (sys/auxv.h).
 */

#ifndef __TS_ASM_HWCAP_H__
#define __TS_ASM_HWCAP_H__

#include <sys/auxv.h>

/* AT_HWCAP2 bits (arm64): none of these is present on the Cortex-A76 */
#define HWCAP2_DCPODP	(1UL << 0)
#define HWCAP2_SVE2	(1UL << 1)
#define HWCAP2_FRINT	(1UL << 8)
#define HWCAP2_MTE	(1UL << 18)
#define HWCAP2_BTI	(1UL << 17)

#endif /* __TS_ASM_HWCAP_H__ */
