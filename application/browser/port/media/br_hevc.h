/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_hevc.h
 *	ブラウザ: the HEVC decoder of the machine (design 14.6.7), between the
 *	media library's side (hevc_tessronos.cc) and the system's (br_hevc.cc).
 *
 *	The structures are those of the device object "動画の復号器"
 *	(include/ts/hevc.h) in the types of the C library, so that the media
 *	library's side need not take the system's headers; br_hevc.cc checks
 *	that they are laid out the same.
 */

#ifndef BR_HEVC_H
#define BR_HEVC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BR_HEVC_MAX_WIDTH	1920
#define BR_HEVC_MAX_HEIGHT	1088
#define BR_HEVC_MAX_SLOTS	17
#define BR_HEVC_MAX_SLICES	64
#define BR_HEVC_MAX_DPB		16
#define BR_HEVC_JOB_MAX		( 4 * 1024 * 1024 )
#define BR_HEVC_JOB_MAGIC	0x48455631U

#define BR_HEVC_SPS_SEPARATE_COLOUR_PLANE	0x0001
#define BR_HEVC_SPS_SCALING_LIST		0x0002
#define BR_HEVC_SPS_AMP				0x0004
#define BR_HEVC_SPS_SAO				0x0008
#define BR_HEVC_SPS_PCM				0x0010
#define BR_HEVC_SPS_PCM_LOOP_FILTER_OFF		0x0020
#define BR_HEVC_SPS_TEMPORAL_MVP		0x0040
#define BR_HEVC_SPS_STRONG_INTRA_SMOOTHING	0x0080

#define BR_HEVC_PPS_SIGN_DATA_HIDING		0x0001
#define BR_HEVC_PPS_CONSTRAINED_INTRA		0x0002
#define BR_HEVC_PPS_TRANSFORM_SKIP		0x0004
#define BR_HEVC_PPS_CU_QP_DELTA			0x0008
#define BR_HEVC_PPS_WEIGHTED_PRED		0x0010
#define BR_HEVC_PPS_WEIGHTED_BIPRED		0x0020
#define BR_HEVC_PPS_TRANSQUANT_BYPASS		0x0040
#define BR_HEVC_PPS_TILES			0x0080
#define BR_HEVC_PPS_ENTROPY_SYNC		0x0100
#define BR_HEVC_PPS_LOOP_FILTER_TILES		0x0200

#define BR_HEVC_DPB_LONG_TERM			0x01

#define BR_HEVC_SL_SAO_LUMA			0x0001
#define BR_HEVC_SL_SAO_CHROMA			0x0002
#define BR_HEVC_SL_TEMPORAL_MVP			0x0004
#define BR_HEVC_SL_MVD_L1_ZERO			0x0008
#define BR_HEVC_SL_CABAC_INIT			0x0010
#define BR_HEVC_SL_COLLOCATED_FROM_L0		0x0020
#define BR_HEVC_SL_DEBLOCKING_OFF		0x0040
#define BR_HEVC_SL_LOOP_FILTER_SLICES		0x0080
#define BR_HEVC_SL_DEPENDENT			0x0100

typedef struct {
	uint8_t		slot, flags;
	uint16_t	rsv;
	int32_t		poc;
} BrHevcDpb;

typedef struct {
	uint16_t	width, height;
	uint8_t		bit_depth_luma, bit_depth_chroma;
	uint8_t		chroma_format_idc;
	uint8_t		log2_min_cb, log2_diff_max_min_cb;
	uint8_t		log2_min_tb, log2_diff_max_min_tb;
	uint8_t		max_th_depth_inter, max_th_depth_intra;
	uint8_t		pcm_bit_depth_luma, pcm_bit_depth_chroma;
	uint8_t		log2_min_pcm_cb, log2_diff_max_min_pcm_cb;
	uint8_t		max_sub_layers_minus1;
	uint32_t	flags;
} BrHevcSps;

typedef struct {
	int8_t		init_qp_minus26;
	uint8_t		diff_cu_qp_delta_depth;
	int8_t		cb_qp_offset, cr_qp_offset;
	uint8_t		num_tile_columns_minus1, num_tile_rows_minus1;
	uint8_t		column_width_minus1[20];
	uint8_t		row_height_minus1[22];
	uint8_t		log2_parallel_merge_level_minus2;
	uint8_t		rsv;
	uint32_t	flags;
} BrHevcPps;

typedef struct {
	uint8_t		s4x4[6][16];
	uint8_t		s8x8[6][64];
	uint8_t		s16x16[6][64];
	uint8_t		s32x32[2][64];
	uint8_t		dc16[6];
	uint8_t		dc32[2];
} BrHevcScale;

typedef struct {
	uint32_t	data_offset, data_size, header_bytes, segment_addr;
	uint8_t		type, nal_unit_type, temporal_id_plus1;
	uint8_t		num_ref_idx_l0_minus1, num_ref_idx_l1_minus1;
	uint8_t		collocated_ref_idx, five_minus_max_merge_cand;
	int8_t		qp_delta, cb_qp_offset, cr_qp_offset, beta_offset_div2, tc_offset_div2;
	uint8_t		ref_l0[BR_HEVC_MAX_DPB], ref_l1[BR_HEVC_MAX_DPB];
	uint8_t		luma_log2_weight_denom;
	int8_t		delta_chroma_log2_weight_denom;
	int8_t		delta_luma_weight[2][BR_HEVC_MAX_DPB];
	int8_t		luma_offset[2][BR_HEVC_MAX_DPB];
	int8_t		delta_chroma_weight[2][BR_HEVC_MAX_DPB][2];
	int8_t		chroma_offset[2][BR_HEVC_MAX_DPB][2];
	uint16_t	rsv;
	int32_t		poc;
	uint32_t	flags;
} BrHevcSlice;

typedef struct {
	uint32_t	magic, size, out_slot, nslice, ndpb, rsv;
	BrHevcDpb	dpb[BR_HEVC_MAX_DPB];
	BrHevcSps	sps;
	BrHevcPps	pps;
	BrHevcScale	scale;
} BrHevcJob;

/* 1 when the machine has the decoder (asked once) */
int br_hevc_present( void );

/* The decoder for one stream at a time: 0 when another has it or there is none */
int br_hevc_acquire( void );
void br_hevc_release( void );

/* The pictures' size, their bit depth (8 or 10) and the slots; >= 0 done */
int br_hevc_conf( unsigned width, unsigned height, unsigned bitdepth, unsigned nslots );

/* One picture: a BrHevcJob, its slices, their bytes; returns when it is decoded (>= 0) */
int br_hevc_decode( const void *job, size_t size );

/*
 * A slot's picture: NV12 for 8 bits (width × height of luma, then U and V
 * by turns), P010 for 10 (the same in samples of 16 bits, little endian,
 * the 10 bits at the top); the bytes read, or < 0
 */
long br_hevc_picture( unsigned slot, void *buf, size_t size );

#ifdef __cplusplus
}
#endif
#endif /* BR_HEVC_H */
