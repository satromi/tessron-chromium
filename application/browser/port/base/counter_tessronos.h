/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	counter_tessronos.h
 *	The processor's counter, which a process reads without a call into
 *	the system: the times the browser sums of its turns, its tasks and
 *	its calls to ANGLE (br_blink.cc, br_main.cc, port/webgl/gl_angle.cc).
 *
 *	  BR_COUNTER_READ(c)	the counter into c (uint64_t)
 *	  BR_COUNTER_FREQ(f)	its ticks in a second into f (uint64_t)
 *
 *	aarch64: the generic timer's virtual count and its frequency.
 *	x86-64: the time stamp counter, whose rate is measured once against
 *	the monotonic clock (br_counter_hz, port/compat/ts_linux.c).
 */

#ifndef __COUNTER_TESSRONOS_H__
#define __COUNTER_TESSRONOS_H__

#include <stdint.h>

#if defined(__x86_64__)

#ifdef __cplusplus
extern "C" {
#endif
uint64_t	br_counter_hz( void );
#ifdef __cplusplus
}
#endif

#define BR_COUNTER_READ(c)	do { \
		uint32_t lo_, hi_; \
		asm volatile("lfence; rdtsc" : "=a"(lo_), "=d"(hi_) : : "memory"); \
		(c) = ( (uint64_t)hi_ << 32 ) | lo_; \
	} while ( 0 )
#define BR_COUNTER_FREQ(f)	( (f) = br_counter_hz() )

#else

#define BR_COUNTER_READ(c)	asm volatile("isb; mrs %0, cntvct_el0" : "=r"(c) : : "memory")
#define BR_COUNTER_FREQ(f)	asm volatile("mrs %0, cntfrq_el0" : "=r"(f))

#endif

#endif /* __COUNTER_TESSRONOS_H__ */
