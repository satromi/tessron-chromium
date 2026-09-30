/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	sys/sysmacros.h
 *	The parts of a Linux device number. TessronOS's files have none (their
 *	st_dev is 0), so these only take apart what they are given.
 */

#ifndef __TS_SYS_SYSMACROS_H__
#define __TS_SYS_SYSMACROS_H__

#define major(dev)	((unsigned int)((((dev) >> 32) & 0xfffff000) | (((dev) >> 8) & 0x00000fff)))
#define minor(dev)	((unsigned int)((((dev) >> 12) & 0xffffff00) | ((dev) & 0x000000ff)))
#define makedev(ma, mi)	((((unsigned long)(ma) & 0x00000fffUL) << 8) | \
			 (((unsigned long)(ma) & 0xfffff000UL) << 32) | \
			 ((unsigned long)(mi) & 0x000000ffUL) | \
			 (((unsigned long)(mi) & 0xffffff00UL) << 12))

#endif /* __TS_SYS_SYSMACROS_H__ */
