/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	linux/kdev_t.h
 *	The kernel's old form of the parts of a device number.
 */

#ifndef __TS_LINUX_KDEV_T_H__
#define __TS_LINUX_KDEV_T_H__

#define MINORBITS	20
#define MINORMASK	((1U << MINORBITS) - 1)
#define MAJOR(dev)	((unsigned int)((dev) >> 8))
#define MINOR(dev)	((unsigned int)((dev) & 0xff))
#define MKDEV(ma, mi)	((ma) << 8 | (mi))

#endif /* __TS_LINUX_KDEV_T_H__ */
