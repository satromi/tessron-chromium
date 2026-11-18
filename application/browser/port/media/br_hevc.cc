/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_hevc.cc
 *	ブラウザ: the system's side of the HEVC decoder (design 14.6.7): the
 *	device object "動画の復号器" (include/ts/hevc.h, design 6.7), found
 *	by its subtype OB_S_VIDEODEC. Record 3 takes the size, the bit depth
 *	and the slots, a write of record 1 decodes one picture, record 2
 *	gives a slot's picture (NV12, or P010 for 10 bits) with the slot in
 *	the upper 32 bits of the offset.
 */

#include TS_CONFIG_H
#include <stdint.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/uapp.h>
#include <ts/ob.h>
#include <ts/hevc.h>

#include <pthread.h>
#include <stddef.h>
#include <string.h>

#include "br_hevc.h"

/* The same layout as the object's */
static_assert(sizeof(BrHevcDpb) == sizeof(T_HEVCDPB), "T_HEVCDPB");
static_assert(sizeof(BrHevcSps) == sizeof(T_HEVCSPS), "T_HEVCSPS");
static_assert(sizeof(BrHevcPps) == sizeof(T_HEVCPPS), "T_HEVCPPS");
static_assert(sizeof(BrHevcScale) == sizeof(T_HEVCSCALE), "T_HEVCSCALE");
static_assert(sizeof(BrHevcSlice) == sizeof(T_HEVCSLICE), "T_HEVCSLICE");
static_assert(sizeof(BrHevcJob) == sizeof(T_HEVCJOB), "T_HEVCJOB");
static_assert(offsetof(BrHevcSps, flags) == offsetof(T_HEVCSPS, flags), "T_HEVCSPS.flags");
static_assert(offsetof(BrHevcPps, flags) == offsetof(T_HEVCPPS, flags), "T_HEVCPPS.flags");
static_assert(offsetof(BrHevcSlice, ref_l0) == offsetof(T_HEVCSLICE, ref_l0), "T_HEVCSLICE.ref_l0");
static_assert(offsetof(BrHevcSlice, chroma_offset) == offsetof(T_HEVCSLICE, chroma_offset), "T_HEVCSLICE.chroma_offset");
static_assert(offsetof(BrHevcSlice, poc) == offsetof(T_HEVCSLICE, poc), "T_HEVCSLICE.poc");
static_assert(offsetof(BrHevcJob, sps) == offsetof(T_HEVCJOB, sps), "T_HEVCJOB.sps");
static_assert(offsetof(BrHevcJob, scale) == offsetof(T_HEVCJOB, scale), "T_HEVCJOB.scale");
static_assert(BR_HEVC_JOB_MAGIC == HEVC_JOB_MAGIC && BR_HEVC_JOB_MAX == HEVC_JOB_MAX
	      && BR_HEVC_MAX_SLOTS == HEVC_MAX_SLOTS && BR_HEVC_MAX_SLICES == HEVC_MAX_SLICES
	      && BR_HEVC_MAX_WIDTH == HEVC_MAX_WIDTH && BR_HEVC_MAX_HEIGHT == HEVC_MAX_HEIGHT, "limits");
static_assert(BR_HEVC_SPS_STRONG_INTRA_SMOOTHING == HEVC_SPS_STRONG_INTRA_SMOOTHING
	      && BR_HEVC_PPS_LOOP_FILTER_TILES == HEVC_PPS_LOOP_FILTER_TILES
	      && BR_HEVC_SL_DEPENDENT == HEVC_SL_DEPENDENT && BR_HEVC_DPB_LONG_TERM == HEVC_DPB_LONG_TERM, "flags");

namespace {

pthread_mutex_t	g_lock = PTHREAD_MUTEX_INITIALIZER;
int		g_present = -1;		/* not asked yet */
TS_UUID		g_uuid;
ID		g_key = 0;
int		g_owned = 0;

int find( void )
{
	TS_UUID	list[4];
	INT	cnt = 0;

	if ( g_present < 0 ) {
		g_present = ( ob_lst_obj(OB_T_DEVICE, OB_S_VIDEODEC, NULL, list, 4, &cnt) >= E_OK && cnt > 0 ) ? 1 : 0;
		if ( g_present ) g_uuid = list[0];
	}
	return g_present;
}

}  // namespace

extern "C" int br_hevc_present( void )
{
	pthread_mutex_lock(&g_lock);
	int p = find();
	pthread_mutex_unlock(&g_lock);
	return p;
}

extern "C" int br_hevc_acquire( void )
{
	int ok = 0;

	pthread_mutex_lock(&g_lock);
	if ( find() && !g_owned ) {
		if ( g_key <= 0 ) g_key = ob_opn_obj(&g_uuid, OB_OP_READ | OB_OP_WRITE);
		if ( g_key > 0 ) ok = g_owned = 1;
	}
	pthread_mutex_unlock(&g_lock);
	return ok;
}

extern "C" void br_hevc_release( void )
{
	pthread_mutex_lock(&g_lock);
	g_owned = 0;
	if ( g_key > 0 ) ob_cls_obj(g_key);
	g_key = 0;
	pthread_mutex_unlock(&g_lock);
}

extern "C" int br_hevc_conf( unsigned width, unsigned height, unsigned bitdepth, unsigned nslots )
{
	T_HEVCCONF	c;
	SZ		a = 0;

	memset(&c, 0, sizeof(c));
	c.width = width;
	c.height = height;
	c.bitdepth = bitdepth;
	c.nslots = nslots;
	return ( g_key > 0 ) ? ob_wri_rec(g_key, HEVC_REC_CONF, 0, &c, sizeof(c), &a) : E_NOEXS;
}

extern "C" int br_hevc_decode( const void *job, size_t size )
{
	SZ	a = 0;

	return ( g_key > 0 ) ? ob_wri_rec(g_key, HEVC_REC_JOB, 0, job, (SZ)size, &a) : E_NOEXS;
}

extern "C" long br_hevc_picture( unsigned slot, void *buf, size_t size )
{
	SZ	a = 0;

	if ( g_key <= 0 ) return E_NOEXS;
	ER er = ob_rea_rec(g_key, HEVC_REC_PICTURE, (D)( (UD)slot << 32 ), buf, (SZ)size, &a);
	return ( er < E_OK ) ? (long)er : (long)a;
}
