/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	hevc_tessronos.cc
 *	ブラウザ: HEVC on the machine's decoder (design 14.6.7).
 *
 *	The media library's H265Decoder parses the stream (Annex B: the
 *	demuxer and WebCodecs make it so), keeps the decoded picture buffer
 *	and the order of output, and asks an accelerator for each picture:
 *	its parameters and references (SubmitFrameMetadata), its slices
 *	(SubmitSlice) and then the decoding (SubmitDecode). The accelerator
 *	here puts them together as one job of the device object
 *	"動画の復号器" (br_hevc.cc, include/ts/hevc.h) and writes it; the
 *	picture goes into one of the decoder's slots, the references are
 *	given by their slots. A picture output is read from its slot as NV12
 *	and made a frame of I420; a picture of 10 bits (Main 10) is read as
 *	P010 and made a frame of YUV420P10 (I420 of 16-bit samples holding
 *	10 bits), the format the media library's software decoders give for
 *	10 bits and WebCodecs shows as I420P10.
 *
 *	A picture holds its slot until the H265Decoder lets it go; the slots
 *	are the pictures the stream keeps for reference and the one being
 *	decoded. The decoder takes one stream at a time; a second stream
 *	finds it taken and goes to the software decoders, which have no
 *	HEVC.
 *
 *	The media pipeline and WebCodecs reach it through the decoder
 *	factory (patches/default_decoder_factory.patch); HEVC is said to be
 *	supported (canPlayType, isTypeSupported, isConfigSupported) only when
 *	the machine has the decoder (br_hevc_media_init).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <deque>
#include <memory>
#include <utility>
#include <vector>

#include "base/containers/flat_set.h"
#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "media/base/decoder_buffer.h"
#include "media/base/decoder_status.h"
#include "media/base/media_log.h"
#include "media/base/supported_types.h"
#include "media/base/video_aspect_ratio.h"
#include "media/base/video_codecs.h"
#include "media/base/video_decoder.h"
#include "media/base/video_decoder_config.h"
#include "media/base/video_frame.h"
#include "media/base/video_types.h"
#include "media/gpu/h265_decoder.h"
#include "media/gpu/h265_dpb.h"
#include "media/parsers/h265_parser.h"
#include "third_party/libyuv/include/libyuv/convert.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"

#include "br_hevc.h"

namespace media {

extern std::unique_ptr<VideoDecoder> (*g_platform_video_decoder)(scoped_refptr<base::SequencedTaskRunner>,
								   MediaLog *);

namespace {

/* The decoder's slots, and which the pictures hold; a configuration again makes them anew */
class Slots final : public base::RefCountedThreadSafe<Slots> {
public:
	void Reset( int n )
	{
		used_.assign(n, false);
		gen_++;
	}
	int Take()
	{
		for ( size_t i = 0; i < used_.size(); i++ ) {
			if ( !used_[i] ) {
				used_[i] = true;
				return (int)i;
			}
		}
		return -1;
	}
	void Give( int slot, unsigned gen )
	{
		if ( gen == gen_ && slot >= 0 && slot < (int)used_.size() ) used_[slot] = false;
	}
	unsigned gen() const { return gen_; }

private:
	friend class base::RefCountedThreadSafe<Slots>;
	~Slots() = default;

	std::vector<bool>	used_;
	unsigned		gen_ = 0;
};

class Picture final : public H265Picture {
public:
	Picture( scoped_refptr<Slots> slots, int slot ) : slots_(std::move(slots)), slot_(slot), gen_(slots_->gen()) {}
	int slot() const { return slot_; }

protected:
	~Picture() override { slots_->Give(slot_, gen_); }

private:
	scoped_refptr<Slots>	slots_;
	int			slot_;
	unsigned		gen_;
};

int clip( int v, int lo, int hi )
{
	return v < lo ? lo : v > hi ? hi : v;
}

class HevcDecoder;

/* One picture's parameters, slices and bytes made a job of the device */
class Accelerator final : public H265Decoder::H265Accelerator {
public:
	Accelerator( HevcDecoder *owner, scoped_refptr<Slots> slots ) : owner_(owner), slots_(std::move(slots)) {}

	scoped_refptr<H265Picture> CreateH265Picture() override
	{
		int s = slots_->Take();
		if ( s < 0 ) return nullptr;
		return base::MakeRefCounted<Picture>(slots_, s);
	}

	Status SubmitFrameMetadata( const H265SPS *sps, const H265PPS *pps, const H265SliceHeader *shdr,
				    const H265Picture::Vector &refs, const H265Picture::Vector &,
				    const H265Picture::Vector &, const H265Picture::Vector &,
				    scoped_refptr<H265Picture> pic ) override
	{
		drop_ = pic->no_rasl_output_flag_
			&& ( shdr->nal_unit_type == H265NALU::RASL_N || shdr->nal_unit_type == H265NALU::RASL_R );
		if ( drop_ ) return Status::kOk;
		memset(&job_, 0, sizeof(job_));
		slices_.clear();
		bytes_.clear();
		dpb_.clear();
		failed_ = false;
		job_.magic = BR_HEVC_JOB_MAGIC;
		job_.out_slot = (uint32_t)static_cast<Picture *>(pic.get())->slot();
		for ( const auto &r : refs ) {
			if ( !r || r->IsUnused() || dpb_.size() >= BR_HEVC_MAX_DPB ) continue;
			BrHevcDpb &d = job_.dpb[dpb_.size()];
			d.slot = (uint8_t)static_cast<Picture *>(r.get())->slot();
			d.flags = r->IsLongTermRef() ? BR_HEVC_DPB_LONG_TERM : 0;
			d.poc = r->pic_order_cnt_val_;
			dpb_.push_back(r.get());
		}
		job_.ndpb = (uint32_t)dpb_.size();

		BrHevcSps &s = job_.sps;
		s.width = (uint16_t)sps->pic_width_in_luma_samples;
		s.height = (uint16_t)sps->pic_height_in_luma_samples;
		s.bit_depth_luma = (uint8_t)( sps->bit_depth_luma_minus8 + 8 );
		s.bit_depth_chroma = (uint8_t)( sps->bit_depth_chroma_minus8 + 8 );
		s.chroma_format_idc = (uint8_t)sps->chroma_format_idc;
		s.log2_min_cb = (uint8_t)( sps->log2_min_luma_coding_block_size_minus3 + 3 );
		s.log2_diff_max_min_cb = (uint8_t)sps->log2_diff_max_min_luma_coding_block_size;
		s.log2_min_tb = (uint8_t)( sps->log2_min_luma_transform_block_size_minus2 + 2 );
		s.log2_diff_max_min_tb = (uint8_t)sps->log2_diff_max_min_luma_transform_block_size;
		s.max_th_depth_inter = (uint8_t)sps->max_transform_hierarchy_depth_inter;
		s.max_th_depth_intra = (uint8_t)sps->max_transform_hierarchy_depth_intra;
		if ( sps->pcm_enabled_flag ) {
			s.pcm_bit_depth_luma = (uint8_t)( sps->pcm_sample_bit_depth_luma_minus1 + 1 );
			s.pcm_bit_depth_chroma = (uint8_t)( sps->pcm_sample_bit_depth_chroma_minus1 + 1 );
			s.log2_min_pcm_cb = (uint8_t)( sps->log2_min_pcm_luma_coding_block_size_minus3 + 3 );
			s.log2_diff_max_min_pcm_cb = (uint8_t)sps->log2_diff_max_min_pcm_luma_coding_block_size;
		}
		s.max_sub_layers_minus1 = (uint8_t)sps->sps_max_sub_layers_minus1;
		s.flags = ( sps->separate_colour_plane_flag ? BR_HEVC_SPS_SEPARATE_COLOUR_PLANE : 0 )
			| ( sps->scaling_list_enabled_flag ? BR_HEVC_SPS_SCALING_LIST : 0 )
			| ( sps->amp_enabled_flag ? BR_HEVC_SPS_AMP : 0 )
			| ( sps->sample_adaptive_offset_enabled_flag ? BR_HEVC_SPS_SAO : 0 )
			| ( sps->pcm_enabled_flag ? BR_HEVC_SPS_PCM : 0 )
			| ( sps->pcm_loop_filter_disabled_flag ? BR_HEVC_SPS_PCM_LOOP_FILTER_OFF : 0 )
			| ( sps->sps_temporal_mvp_enabled_flag ? BR_HEVC_SPS_TEMPORAL_MVP : 0 )
			| ( sps->strong_intra_smoothing_enabled_flag ? BR_HEVC_SPS_STRONG_INTRA_SMOOTHING : 0 );

		BrHevcPps &p = job_.pps;
		p.init_qp_minus26 = (int8_t)pps->init_qp_minus26;
		p.diff_cu_qp_delta_depth = (uint8_t)pps->diff_cu_qp_delta_depth;
		p.cb_qp_offset = (int8_t)pps->pps_cb_qp_offset;
		p.cr_qp_offset = (int8_t)pps->pps_cr_qp_offset;
		if ( pps->tiles_enabled_flag ) {
			int cw = sps->pic_width_in_ctbs_y, ch = sps->pic_height_in_ctbs_y;
			int nc = pps->num_tile_columns_minus1 + 1, nr = pps->num_tile_rows_minus1 + 1;

			if ( nc > 20 || nr > 22 ) return Status::kNotSupported;
			p.num_tile_columns_minus1 = (uint8_t)( nc - 1 );
			p.num_tile_rows_minus1 = (uint8_t)( nr - 1 );
			for ( int i = 0; i < nc - 1; i++ ) {
				int w = pps->uniform_spacing_flag ? ( ( i + 1 ) * cw ) / nc - ( i * cw ) / nc - 1
								  : pps->column_width_minus1[i];
				p.column_width_minus1[i] = (uint8_t)w;
			}
			for ( int i = 0; i < nr - 1; i++ ) {
				int h = pps->uniform_spacing_flag ? ( ( i + 1 ) * ch ) / nr - ( i * ch ) / nr - 1
								  : pps->row_height_minus1[i];
				p.row_height_minus1[i] = (uint8_t)h;
			}
		}
		p.log2_parallel_merge_level_minus2 = (uint8_t)pps->log2_parallel_merge_level_minus2;
		p.flags = ( pps->sign_data_hiding_enabled_flag ? BR_HEVC_PPS_SIGN_DATA_HIDING : 0 )
			| ( pps->constrained_intra_pred_flag ? BR_HEVC_PPS_CONSTRAINED_INTRA : 0 )
			| ( pps->transform_skip_enabled_flag ? BR_HEVC_PPS_TRANSFORM_SKIP : 0 )
			| ( pps->cu_qp_delta_enabled_flag ? BR_HEVC_PPS_CU_QP_DELTA : 0 )
			| ( pps->weighted_pred_flag ? BR_HEVC_PPS_WEIGHTED_PRED : 0 )
			| ( pps->weighted_bipred_flag ? BR_HEVC_PPS_WEIGHTED_BIPRED : 0 )
			| ( pps->transquant_bypass_enabled_flag ? BR_HEVC_PPS_TRANSQUANT_BYPASS : 0 )
			| ( pps->tiles_enabled_flag ? BR_HEVC_PPS_TILES : 0 )
			| ( pps->entropy_coding_sync_enabled_flag ? BR_HEVC_PPS_ENTROPY_SYNC : 0 )
			| ( pps->loop_filter_across_tiles_enabled_flag ? BR_HEVC_PPS_LOOP_FILTER_TILES : 0 );

		if ( sps->scaling_list_enabled_flag ) {
			const H265ScalingListData &sl = pps->pps_scaling_list_data_present_flag ? pps->scaling_list_data
												 : sps->scaling_list_data;
			BrHevcScale &c = job_.scale;

			for ( size_t m = 0; m < 6; m++ ) {
				for ( size_t i = 0; i < 16; i++ ) c.s4x4[m][i] = sl.GetScalingList4x4EntryInRasterOrder(m, i);
				for ( size_t i = 0; i < 64; i++ ) {
					c.s8x8[m][i] = sl.GetScalingList8x8EntryInRasterOrder(m, i);
					c.s16x16[m][i] = sl.GetScalingList16x16EntryInRasterOrder(m, i);
				}
				c.dc16[m] = sl.scaling_list_dc_coef_16x16[m];
			}
			for ( size_t m = 0; m < 2; m++ ) {
				for ( size_t i = 0; i < 64; i++ ) {
					c.s32x32[m][i] = sl.GetScalingList32x32EntryInRasterOrder(m * 3, i);
				}
				c.dc32[m] = sl.scaling_list_dc_coef_32x32[m * 3];
			}
		}
		wp_half_c_ = sps->wp_offset_half_range_c;
		return Status::kOk;
	}

	Status SubmitSlice( const H265SPS *, const H265PPS *, const H265SliceHeader *shdr,
			    const H265Picture::Vector &l0, const H265Picture::Vector &l1,
			    const H265Picture::Vector &, const H265Picture::Vector &,
			    const H265Picture::Vector &, scoped_refptr<H265Picture> pic, const uint8_t *data,
			    size_t size, const std::vector<SubsampleEntry> & ) override
	{
		if ( drop_ ) return Status::kOk;
		if ( slices_.size() >= BR_HEVC_MAX_SLICES
		     || sizeof(BrHevcJob) + ( slices_.size() + 1 ) * sizeof(BrHevcSlice) + bytes_.size() + size
			> BR_HEVC_JOB_MAX
		     || l0.size() > BR_HEVC_MAX_DPB || l1.size() > BR_HEVC_MAX_DPB ) {
			failed_ = true;
			return Status::kNotSupported;
		}
		BrHevcSlice s;
		memset(&s, 0, sizeof(s));
		s.data_offset = (uint32_t)bytes_.size();
		s.data_size = (uint32_t)size;
		s.header_bytes = (uint32_t)( shdr->header_size + shdr->header_emulation_prevention_bytes );
		s.segment_addr = (uint32_t)shdr->slice_segment_address;
		s.type = (uint8_t)shdr->slice_type;
		s.nal_unit_type = (uint8_t)shdr->nal_unit_type;
		s.temporal_id_plus1 = (uint8_t)( shdr->temporal_id + 1 );
		s.num_ref_idx_l0_minus1 = (uint8_t)( l0.empty() ? 0 : l0.size() - 1 );
		s.num_ref_idx_l1_minus1 = (uint8_t)( l1.empty() ? 0 : l1.size() - 1 );
		s.collocated_ref_idx = (uint8_t)shdr->collocated_ref_idx;
		s.five_minus_max_merge_cand = (uint8_t)shdr->five_minus_max_num_merge_cand;
		s.qp_delta = (int8_t)shdr->slice_qp_delta;
		s.cb_qp_offset = (int8_t)shdr->slice_cb_qp_offset;
		s.cr_qp_offset = (int8_t)shdr->slice_cr_qp_offset;
		s.beta_offset_div2 = (int8_t)shdr->slice_beta_offset_div2;
		s.tc_offset_div2 = (int8_t)shdr->slice_tc_offset_div2;
		for ( size_t i = 0; i < l0.size(); i++ ) s.ref_l0[i] = index(l0[i].get());
		for ( size_t i = 0; i < l1.size(); i++ ) s.ref_l1[i] = index(l1[i].get());

		const H265PredWeightTable &w = shdr->pred_weight_table;
		int cd = w.chroma_log2_weight_denom;
		s.luma_log2_weight_denom = (uint8_t)w.luma_log2_weight_denom;
		s.delta_chroma_log2_weight_denom = (int8_t)w.delta_chroma_log2_weight_denom;
		for ( int l = 0; l < 2; l++ ) {
			size_t n = l ? l1.size() : l0.size();

			for ( size_t i = 0; i < n; i++ ) {
				s.delta_luma_weight[l][i] = (int8_t)( l ? w.delta_luma_weight_l1[i] : w.delta_luma_weight_l0[i] );
				s.luma_offset[l][i] = (int8_t)clip(l ? w.luma_offset_l1[i] : w.luma_offset_l0[i], -128, 127);
				for ( int j = 0; j < 2; j++ ) {
					int dw = l ? w.delta_chroma_weight_l1[i][j] : w.delta_chroma_weight_l0[i][j];
					int dofs = l ? w.delta_chroma_offset_l1[i][j] : w.delta_chroma_offset_l0[i][j];
					int cw = ( 1 << cd ) + dw;
					int co = clip(wp_half_c_ + dofs - ( ( wp_half_c_ * cw ) >> cd ), -wp_half_c_, wp_half_c_ - 1);

					s.delta_chroma_weight[l][i][j] = (int8_t)dw;
					s.chroma_offset[l][i][j] = (int8_t)clip(co, -128, 127);
				}
			}
		}
		s.poc = pic->pic_order_cnt_val_;
		s.flags = ( shdr->slice_sao_luma_flag ? BR_HEVC_SL_SAO_LUMA : 0 )
			| ( shdr->slice_sao_chroma_flag ? BR_HEVC_SL_SAO_CHROMA : 0 )
			| ( shdr->slice_temporal_mvp_enabled_flag ? BR_HEVC_SL_TEMPORAL_MVP : 0 )
			| ( shdr->mvd_l1_zero_flag ? BR_HEVC_SL_MVD_L1_ZERO : 0 )
			| ( shdr->cabac_init_flag ? BR_HEVC_SL_CABAC_INIT : 0 )
			| ( shdr->collocated_from_l0_flag ? BR_HEVC_SL_COLLOCATED_FROM_L0 : 0 )
			| ( shdr->slice_deblocking_filter_disabled_flag ? BR_HEVC_SL_DEBLOCKING_OFF : 0 )
			| ( shdr->slice_loop_filter_across_slices_enabled_flag ? BR_HEVC_SL_LOOP_FILTER_SLICES : 0 )
			| ( shdr->dependent_slice_segment_flag ? BR_HEVC_SL_DEPENDENT : 0 );
		slices_.push_back(s);
		bytes_.insert(bytes_.end(), data, data + size);
		return Status::kOk;
	}

	Status SubmitDecode( scoped_refptr<H265Picture> ) override;
	bool OutputPicture( scoped_refptr<H265Picture> pic ) override;

	void Reset() override
	{
		slices_.clear();
		bytes_.clear();
		dpb_.clear();
		drop_ = false;
	}

	bool IsChromaSamplingSupported( VideoChromaSampling format ) override
	{
		return format == VideoChromaSampling::k420;
	}

private:
	/* A reference as an index into the job's pictures; one missing is the first */
	uint8_t index( const H265Picture *r ) const
	{
		for ( size_t i = 0; i < dpb_.size(); i++ ) {
			if ( dpb_[i] == r ) return (uint8_t)i;
		}
		return 0;
	}

	HevcDecoder			*owner_;
	scoped_refptr<Slots>		slots_;
	BrHevcJob			job_;
	std::vector<BrHevcSlice>	slices_;
	std::vector<uint8_t>		bytes_;
	std::vector<const H265Picture *> dpb_;
	std::vector<uint8_t>		out_;
	int				wp_half_c_ = 128;
	bool				drop_ = false;
	bool				failed_ = false;
};

class HevcDecoder final : public VideoDecoder {
public:
	explicit HevcDecoder( MediaLog *log ) : log_(log), slots_(base::MakeRefCounted<Slots>()) {}

	~HevcDecoder() override
	{
		dec_.reset();
		if ( owned_ ) br_hevc_release();
	}

	VideoDecoderType GetDecoderType() const override { return VideoDecoderType::kUnknown; }
	bool IsPlatformDecoder() const override { return true; }
	bool NeedsBitstreamConversion() const override { return true; }
	bool CanReadWithoutStalling() const override { return true; }
	int GetMaxDecodeRequests() const override { return 1; }

	void Initialize( const VideoDecoderConfig &config, bool, CdmContext *, InitCB init_cb,
			 const OutputCB &output_cb, const WaitingCB & ) override
	{
		if ( config.codec() != VideoCodec::kHEVC
		     || ( config.profile() != HEVCPROFILE_MAIN && config.profile() != HEVCPROFILE_MAIN10
			  && config.profile() != HEVCPROFILE_MAIN_STILL_PICTURE )
		     || config.is_encrypted() || config.coded_size().width() > BR_HEVC_MAX_WIDTH
		     || config.coded_size().height() > BR_HEVC_MAX_HEIGHT ) {
			std::move(init_cb).Run(DecoderStatus::Codes::kUnsupportedConfig);
			return;
		}
		if ( !owned_ ) owned_ = br_hevc_acquire() != 0;
		if ( !owned_ ) {
			std::move(init_cb).Run(DecoderStatus::Codes::kUnsupportedConfig);
			return;
		}
		config_ = config;
		output_cb_ = output_cb;
		width_ = height_ = 0;
		dec_ = std::make_unique<H265Decoder>(std::make_unique<Accelerator>(this, slots_), config.profile(),
						     config.color_space_info());
		printf("browser: media: hevc on the machine's decoder, %dx%d\n", config.coded_size().width(),
		       config.coded_size().height());
		std::move(init_cb).Run(DecoderStatus::Codes::kOk);
	}

	void Decode( scoped_refptr<DecoderBuffer> buffer, DecodeCB decode_cb ) override
	{
		if ( !dec_ ) {
			std::move(decode_cb).Run(DecoderStatus::Codes::kNotInitialized);
			return;
		}
		if ( buffer->end_of_stream() ) {
			std::move(decode_cb).Run(dec_->Flush() ? DecoderStatus::Codes::kOk
							     : DecoderStatus::Codes::kPlatformDecodeFailure);
			return;
		}
		int32_t id = next_id_++;
		times_.push_back({ id, buffer->timestamp() });
		if ( times_.size() > 64 ) times_.pop_front();
		dec_->SetStream(id, buffer);
		for ( ;; ) {
			switch ( dec_->Decode() ) {
			case AcceleratedVideoDecoder::kConfigChange:
				if ( !Configure() ) {
					std::move(decode_cb).Run(DecoderStatus::Codes::kPlatformDecodeFailure);
					return;
				}
				continue;
			case AcceleratedVideoDecoder::kRanOutOfStreamData:
				std::move(decode_cb).Run(DecoderStatus::Codes::kOk);
				return;
			default:
				printf("browser: media: hevc: the stream is not decoded\n");
				std::move(decode_cb).Run(DecoderStatus::Codes::kPlatformDecodeFailure);
				return;
			}
		}
	}

	void Reset( base::OnceClosure closure ) override
	{
		if ( dec_ ) dec_->Reset();
		std::move(closure).Run();
	}

	/* A picture out of its slot, as a frame of I420 (8 bits) or YUV420P10 (10 bits) */
	bool Output( const Picture *pic )
	{
		size_t ysize = (size_t)width_ * height_;
		bool deep = depth_ == 10;

		if ( deep ) {
			p010_.resize(ysize * 3 / 2);
			if ( br_hevc_picture((unsigned)pic->slot(), p010_.data(), p010_.size() * 2) < (long)( p010_.size() * 2 ) ) {
				return false;
			}
		} else {
			nv12_.resize(ysize * 3 / 2);
			if ( br_hevc_picture((unsigned)pic->slot(), nv12_.data(), nv12_.size()) < (long)nv12_.size() ) {
				return false;
			}
		}
		gfx::Rect vis = pic->visible_rect();
		if ( vis.IsEmpty() ) vis = gfx::Rect(width_, height_);
		base::TimeDelta ts;
		for ( const auto &t : times_ ) {
			if ( t.first == pic->bitstream_id() ) ts = t.second;
		}
		scoped_refptr<VideoFrame> f = VideoFrame::CreateFrame(deep ? PIXEL_FORMAT_YUV420P10 : PIXEL_FORMAT_I420,
								      gfx::Size(width_, height_), vis,
								      config_.aspect_ratio().GetNaturalSize(vis), ts);
		if ( !f ) return false;
		if ( deep ) {
			/* the samples moved down to the bottom 10 bits, U and V apart */
			libyuv::P010ToI010(p010_.data(), width_, p010_.data() + ysize, width_,
					   reinterpret_cast<uint16_t *>(f->writable_data(VideoFrame::Plane::kY)),
					   f->stride(VideoFrame::Plane::kY) / 2,
					   reinterpret_cast<uint16_t *>(f->writable_data(VideoFrame::Plane::kU)),
					   f->stride(VideoFrame::Plane::kU) / 2,
					   reinterpret_cast<uint16_t *>(f->writable_data(VideoFrame::Plane::kV)),
					   f->stride(VideoFrame::Plane::kV) / 2, width_, height_);
		} else {
			libyuv::NV12ToI420(nv12_.data(), width_, nv12_.data() + ysize, width_,
					   f->writable_data(VideoFrame::Plane::kY), f->stride(VideoFrame::Plane::kY),
					   f->writable_data(VideoFrame::Plane::kU), f->stride(VideoFrame::Plane::kU),
					   f->writable_data(VideoFrame::Plane::kV), f->stride(VideoFrame::Plane::kV),
					   width_, height_);
		}
		VideoColorSpace cs = pic->get_colorspace();
		f->set_color_space(cs.IsSpecified() ? cs.ToGfxColorSpace() : config_.color_space_info().ToGfxColorSpace());
		output_cb_.Run(std::move(f));
		return true;
	}

private:
	/* The size and the slots the stream asks for */
	bool Configure()
	{
		gfx::Size sz = dec_->GetPicSize();
		size_t n = std::min<size_t>(BR_HEVC_MAX_SLOTS, dec_->GetNumReferenceFrames() + 2);

		int depth = dec_->GetBitDepth();

		if ( ( depth != 8 && depth != 10 ) || dec_->GetChromaSampling() != VideoChromaSampling::k420
		     || sz.width() > BR_HEVC_MAX_WIDTH || sz.height() > BR_HEVC_MAX_HEIGHT ) {
			printf("browser: media: hevc: %dx%d, %d bits, not for the machine's decoder\n", sz.width(),
			       sz.height(), depth);
			return false;
		}
		int er = br_hevc_conf((unsigned)sz.width(), (unsigned)sz.height(), (unsigned)depth, (unsigned)n);
		if ( er < 0 ) {
			printf("browser: media: hevc: %dx%d with %d slots not taken (%d)\n", sz.width(), sz.height(),
			       (int)n, er);
			return false;
		}
		slots_->Reset((int)n);
		width_ = sz.width();
		height_ = sz.height();
		depth_ = depth;
		return true;
	}

	MediaLog					*log_;
	scoped_refptr<Slots>				slots_;
	std::unique_ptr<H265Decoder>			dec_;
	VideoDecoderConfig				config_;
	OutputCB					output_cb_;
	std::deque<std::pair<int32_t, base::TimeDelta>>	times_;
	std::vector<uint8_t>				nv12_;
	std::vector<uint16_t>				p010_;
	int32_t						next_id_ = 0;
	int						width_ = 0, height_ = 0, depth_ = 8;
	bool						owned_ = false;
};

H265Decoder::H265Accelerator::Status Accelerator::SubmitDecode( scoped_refptr<H265Picture> )
{
	if ( drop_ ) return Status::kOk;
	if ( failed_ || slices_.empty() ) return Status::kFail;
	job_.nslice = (uint32_t)slices_.size();
	job_.size = (uint32_t)( sizeof(job_) + slices_.size() * sizeof(BrHevcSlice) + bytes_.size() );
	out_.resize(job_.size);
	memcpy(out_.data(), &job_, sizeof(job_));
	memcpy(out_.data() + sizeof(job_), slices_.data(), slices_.size() * sizeof(BrHevcSlice));
	memcpy(out_.data() + sizeof(job_) + slices_.size() * sizeof(BrHevcSlice), bytes_.data(), bytes_.size());
	int er = br_hevc_decode(out_.data(), out_.size());
	if ( er < 0 ) {
		printf("browser: media: hevc: a picture is not decoded (%d)\n", er);
		return Status::kFail;
	}
	return Status::kOk;
}

bool Accelerator::OutputPicture( scoped_refptr<H265Picture> pic )
{
	return owner_->Output(static_cast<Picture *>(pic.get()));
}

std::unique_ptr<VideoDecoder> make_decoder( scoped_refptr<base::SequencedTaskRunner>, MediaLog *log )
{
	if ( !br_hevc_present() ) return nullptr;
	return std::make_unique<HevcDecoder>(log);
}

}  // namespace

}  // namespace media

/* At the start: HEVC offered when the machine has the decoder */
extern "C" void br_hevc_media_init( void )
{
	if ( !br_hevc_present() ) return;
	media::g_platform_video_decoder = &media::make_decoder;
	media::UpdateDefaultDecoderSupportedVideoProfiles(
		{ media::HEVCPROFILE_MAIN, media::HEVCPROFILE_MAIN10, media::HEVCPROFILE_MAIN_STILL_PICTURE });
	printf("browser: media: HEVC on the machine's decoder\n");
}
