/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	linux/input.h
 *	The bounds of Linux's input event key codes, which the key code
 *	tables are checked against, and the codes they name. TessronOS's keys
 *	come as HID usages; no input device of Linux is read.
 */

#ifndef __TS_LINUX_INPUT_H__
#define __TS_LINUX_INPUT_H__

#define KEY_RESERVED	0
#define KEY_PLAYCD	200
#define KEY_PLAY	207
#define KEY_MAX		0x2ff
#define KEY_CNT		(KEY_MAX + 1)

#endif /* __TS_LINUX_INPUT_H__ */
