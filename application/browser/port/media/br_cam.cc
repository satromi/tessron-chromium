/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_cam.cc
 *	ブラウザ: the cameras, for getUserMedia's video (design 17.19): the
 *	camera device's objects (design 18.7), one a unit with a camera
 *	("cama0" .. "cama3"; "cama" is the device as a whole and is passed
 *	over). Record 2 says what it is, 3 its modes; a mode written to record
 *	4 and CAM_START to record 5 start it; record 1 gives the newest frame
 *	the program has not had, or nothing, without waiting.
 */

#include TS_CONFIG_H
#include <stdint.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/uapp.h>
#include <ts/ob.h>
#include <ts/cam.h>
#include <ts/ui.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "br_cam.h"

namespace {

pthread_mutex_t	g_lock = PTHREAD_MUTEX_INITIALIZER;
ID		g_key = 0;
int		g_frame_max = 0;

/* The units with a camera, in the order of their numbers */
int units( TS_UUID *out, T_CAMINFO *info, int max )
{
	TS_UUID	*list = nullptr;
	int	n = 0;
	/* every camera object, a page at a time: those that are not units are passed over */
	INT	cnt = ui_lst_obj(OB_T_DEVICE, OB_S_CAMERA, &list);

	if ( cnt < 0 ) return 0;
	for ( int i = 0; i < cnt && n < max; i++ ) {
		T_CAMINFO in;
		SZ	a = 0;

		/* the name whole: a unit's ends in its number */
		UB *nm = ui_obj_name(&list[i]);
		if ( nm == nullptr ) continue;
		size_t len = strlen((const char *)nm);
		bool unit = ( len > 0 && nm[len - 1] >= '0' && nm[len - 1] <= '9' );
		free(nm);
		if ( !unit ) continue;
		ID key = ob_opn_obj(&list[i], OB_OP_R);
		if ( key <= 0 ) continue;
		memset(&in, 0, sizeof(in));
		ER er = ob_rea_rec(key, OB_CAM_INFO, 0, &in, sizeof(in), &a);
		ob_cls_obj(key);
		if ( er < E_OK || in.state == CAMST_NODEV || in.state == CAMST_ERROR ) continue;
		out[n] = list[i];
		if ( info != NULL ) info[n] = in;
		n++;
	}
	free(list);
	return n;
}

}  // namespace

extern "C" int br_cam_list( char (*names)[64], unsigned char (*ids)[16], int max )
{
	TS_UUID		u[BR_CAM_MAX];
	T_CAMINFO	in[BR_CAM_MAX];
	int		n = units(u, in, max < BR_CAM_MAX ? max : BR_CAM_MAX);

	for ( int i = 0; i < n; i++ ) {
		snprintf(names[i], 64, "%.63s", (const char *)in[i].product);
		if ( ids != nullptr ) memcpy(ids[i], u[i].b, 16);
	}
	return n;
}

extern "C" int br_cam_modes( int cam, BrCamMode *modes, int max )
{
	TS_UUID		u[BR_CAM_MAX];
	static T_CAMMODE m[CAM_MAXMODES];
	SZ		a = 0;
	int		n = units(u, nullptr, BR_CAM_MAX), k = 0;

	if ( cam < 0 || cam >= n ) return 0;
	ID key = ob_opn_obj(&u[cam], OB_OP_R);
	if ( key <= 0 ) return 0;
	pthread_mutex_lock(&g_lock);
	ER er = ob_rea_rec(key, OB_CAM_MODES, 0, m, sizeof(m), &a);
	if ( er >= E_OK ) {
		for ( int i = 0; i < (int)( a / sizeof(T_CAMMODE) ) && k < max; i++, k++ ) {
			modes[k].format = m[i].format;
			modes[k].width = m[i].width;
			modes[k].height = m[i].height;
			modes[k].interval = m[i].defInterval;
			modes[k].fastest = m[i].defInterval;
			if ( m[i].nintv == 0 && m[i].interval[0] > 0 ) {
				modes[k].fastest = m[i].interval[0];
			}
			for ( int j = 0; j < m[i].nintv; j++ ) {
				if ( m[i].interval[j] > 0 && m[i].interval[j] < modes[k].fastest ) {
					modes[k].fastest = m[i].interval[j];
				}
			}
		}
	}
	pthread_mutex_unlock(&g_lock);
	ob_cls_obj(key);
	return k;
}

extern "C" int br_cam_open( int cam, int mode, unsigned interval )
{
	TS_UUID		u[BR_CAM_MAX];
	T_CAMSETMODE	m;
	SZ		a = 0;
	W		v;
	int		n = units(u, nullptr, BR_CAM_MAX);

	if ( cam < 0 || cam >= n ) return -1;
	pthread_mutex_lock(&g_lock);
	if ( g_key > 0 ) {
		v = CAM_STOP;
		(void)ob_wri_rec(g_key, OB_CAM_STREAM, 0, &v, sizeof(v), &a);
		ob_cls_obj(g_key);
		g_key = 0;
	}
	ID key = ob_opn_obj(&u[cam], OB_OP_R | OB_OP_WRITE);
	if ( key <= 0 ) {
		pthread_mutex_unlock(&g_lock);
		printf("browser: camera: the device cannot be opened\n");
		return -1;
	}
	memset(&m, 0, sizeof(m));
	m.mode = mode;
	m.interval = interval;
	ER er = ob_wri_rec(key, OB_CAM_MODE, 0, &m, sizeof(m), &a);
	if ( er >= E_OK ) {
		(void)ob_rea_rec(key, OB_CAM_MODE, 0, &m, sizeof(m), &a);
		v = CAM_START;
		er = ob_wri_rec(key, OB_CAM_STREAM, 0, &v, sizeof(v), &a);
	}
	if ( er < E_OK ) {
		ob_cls_obj(key);
		pthread_mutex_unlock(&g_lock);
		printf("browser: camera: mode %d does not start (%d)\n", mode, (int)er);
		return -1;
	}
	g_key = key;
	g_frame_max = (int)m.maxFrame;
	pthread_mutex_unlock(&g_lock);
	printf("browser: camera: streaming mode %d\n", mode);
	return 0;
}

extern "C" int br_cam_frame_max( void )
{
	return g_frame_max;
}

extern "C" int br_cam_read( unsigned char *buf, int max, BrCamFrame *hdr )
{
	T_CAMFRAME	h;
	SZ		a = 0;

	if ( max <= (int)sizeof(h) ) return -1;
	pthread_mutex_lock(&g_lock);
	ER er = g_key > 0 ? ob_rea_rec(g_key, OB_CAM_FRAME, 0, buf, (SZ)max, &a) : E_OBJ;
	pthread_mutex_unlock(&g_lock);
	if ( er < E_OK ) return -1;
	if ( a < (SZ)sizeof(h) ) return 0;
	memcpy(&h, buf, sizeof(h));
	if ( h.flags & CAMFR_TRUNC ) return 0;
	hdr->seq = h.seq;
	hdr->format = h.format;
	hdr->width = h.width;
	hdr->height = h.height;
	hdr->bytes = h.bytes;
	hdr->msec = h.msec;
	memmove(buf, buf + sizeof(h), (size_t)a - sizeof(h));
	return (int)( a - (SZ)sizeof(h) );
}

extern "C" void br_cam_close( void )
{
	SZ	a = 0;
	W	v = CAM_STOP;

	pthread_mutex_lock(&g_lock);
	if ( g_key > 0 ) {
		(void)ob_wri_rec(g_key, OB_CAM_STREAM, 0, &v, sizeof(v), &a);
		ob_cls_obj(g_key);
		g_key = 0;
		printf("browser: camera: stopped\n");
	}
	pthread_mutex_unlock(&g_lock);
}
