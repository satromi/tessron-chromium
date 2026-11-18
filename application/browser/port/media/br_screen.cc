/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_screen.cc
 *	ブラウザ: the screen as a page shares it (getDisplayMedia, design
 *	14.4.8): its size, and its pixels copied from the screen the window
 *	manager last built (wm_grab_screen, a call of the system's), with the
 *	pointer as it lies on it. Only the capture of a share the person
 *	allowed on the browser's panel calls it (port/blink/screen_tessronos.cc).
 */

#include TS_CONFIG_H
#include <stdint.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/uapp.h>
#include <ts/wm.h>

/* The size of the screen in pixels: false while there is none */
extern "C" int br_screen_size( int *w, int *h )
{
	INT sw = 0, sh = 0;
	wm_screen(&sw, &sh);
	*w = sw;
	*h = sh;
	return sw > 0 && sh > 0;
}

/*
 * The w by h pixels of the view from (x, y), a screen's rectangle (ARGB,
 * 'pitch' pixels to a row), into dst, with the pointer where it lies on
 * them: 0, or less than 0 when they could not be read
 */
extern "C" int br_screen_grab( uint32_t *dst, int pitch, int x, int y, int w, int h )
{
	T_DPRECT r;
	r.left = x;
	r.top = y;
	r.right = x + w;
	r.bottom = y + h;
	ER er = wm_grab_screen((UW *)dst, pitch, &r, TRUE);
	return er < E_OK ? (int)er : 0;
}

/*
 * A window's work area (ARGB, 'pitch' pixels to a row), cut to maxw by
 * maxh, into dst, whatever covers it on the screen: 0 and its size, 1 when
 * the window has been closed, less than 0 when there is no picture this time
 */
extern "C" int br_window_grab( int wid, unsigned serial, uint32_t *dst, int pitch, int maxw, int maxh, int *w, int *h )
{
	T_WMGRAB g;
	g.wid = wid;
	g.serial = serial;
	g.dst = (UW *)dst;
	g.pitch = pitch;
	g.maxw = maxw;
	g.maxh = maxh;
	g.w = g.h = 0;
	ER er = wm_grab_window(&g);
	if ( er == E_NOEXS || er == E_ID ) return 1;
	if ( er < E_OK ) return (int)er;
	*w = g.w;
	*h = g.h;
	return 0;
}
