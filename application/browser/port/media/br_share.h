/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_share.h
 *	ブラウザ: what the person chose to share on the panel (getDisplayMedia,
 *	design 14.4.8): the service that asks (devices_tessronos.cc), the panel
 *	(br_main.cc), and the captures of the pictures and of the sound.
 */

#ifndef BR_SHARE_H
#define BR_SHARE_H

#include <string>

/* What a page may be offered, from its request */
struct BrShareAsk {
	bool	audio = false;		/* it asks for sound */
	bool	no_system_audio = false;	/* not the machine's whole sound */
	bool	own_audio_out = false;	/* the whole sound without this browser's */
	bool	no_self = false;	/* not the window the page is in */
	bool	no_screen = false;	/* not the whole screen */
	int	prefer = 0;		/* the kind to have chosen first: BR_SHARE_* */
};

#define BR_SHARE_SCREEN		1
#define BR_SHARE_WINDOW		2
#define BR_SHARE_BROWSER	3

/* The sound's capture: what the sound device's monitor takes (SDMon) */
struct BrShareChoice {
	int		kind = 0;	/* BR_SHARE_* */
	int		wid = 0;	/* the window, with its opening's serial */
	unsigned	serial = 0;
	std::string	name;
	bool		audio = false;	/* sound goes with it */
	int		mon_mode = 0;	/* SND_MON_* */
	int		mon_pid = 0;
	/*
	 * BR_SHARE_SCREEN: which screen (its number when chosen, 0 the main
	 * one), the name and the display's ident of the output that shows it
	 * ("" and 0 for the columns of one output), and its rectangle in the
	 * view when chosen
	 */
	int		screen = 0;
	std::string	out_name;
	unsigned	out_ident = 0;
	int		sx = 0, sy = 0, sw = 0, sh = 0;
};

/*
 * The rectangle in the view of the screen a share chose, as the screens
 * are now: the same output's screen where it has one (found by its name
 * and ident), else the screen of that number while the screens are not
 * shown by outputs of their own. False when it is gone (taken away, or
 * now mirroring the main screen): the share ends. From the browser's
 * process; a renderer asks it of the browser.
 */
bool br_share_screen_rect( const BrShareChoice &c, int *x, int *y, int *w, int *h );

/* The panel, from a service's thread: false when refused */
bool br_share_choose( const std::string &origin, const BrShareAsk &ask, BrShareChoice *out );

#endif
