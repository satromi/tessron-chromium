/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	png_tessronos.cc
 *	PNG for the engine with libpng (design 17.19).
 *
 *	The engine reads and writes PNG through the names of Skia's Rust
 *	codec (SkPngRustDecoder, SkPngRustEncoder): the image decoder of
 *	Blink, Skia's list of decoders, the canvas's encoder. The Rust side of
 *	that codec is not built for TessronOS; the same names are defined here
 *	on Skia's libpng codec (SkPngCodec, SkPngEncoderImpl), which the
 *	program links in their place.
 */

#include "include/codec/SkPngDecoder.h"
#include "include/codec/SkPngRustDecoder.h"
#include "include/core/SkData.h"
#include "include/core/SkStream.h"
#include "include/encode/SkPngEncoder.h"
#include "include/encode/SkPngRustEncoder.h"

namespace {

SkPngEncoder::Options png_options( const SkPngRustEncoder::Options &o )
{
	SkPngEncoder::Options	p;

	/* zlib's levels: fast, its default, its best */
	switch ( o.fCompressionLevel ) {
	case SkPngRustEncoder::CompressionLevel::kLow:		p.fZLibLevel = 1; break;
	case SkPngRustEncoder::CompressionLevel::kMedium:	p.fZLibLevel = 6; break;
	case SkPngRustEncoder::CompressionLevel::kHigh:		p.fZLibLevel = 9; break;
	}
	p.fComments = o.fComments;
	p.fGainmap = o.fGainmap;
	p.fGainmapInfo = o.fGainmapInfo;
	return p;
}

}  // namespace

namespace SkPngRustDecoder {

bool IsPng( const void *data, size_t size )
{
	return SkPngDecoder::IsPng(data, size);
}

std::unique_ptr<SkCodec> Decode( std::unique_ptr<SkStream> stream, SkCodec::Result *result,
				 SkCodecs::DecodeContext ctx )
{
	return SkPngDecoder::Decode(std::move(stream), result, ctx);
}

}  // namespace SkPngRustDecoder

namespace SkPngRustEncoder {

bool Encode( SkWStream *dst, const SkPixmap &src, const Options &options )
{
	return SkPngEncoder::Encode(dst, src, png_options(options));
}

sk_sp<SkData> Encode( const SkPixmap &src, const Options &options )
{
	return SkPngEncoder::Encode(src, png_options(options));
}

sk_sp<SkData> Encode( GrDirectContext *ctx, const SkImage *img, const Options &options )
{
	return SkPngEncoder::Encode(ctx, img, png_options(options));
}

std::unique_ptr<SkEncoder> Make( SkWStream *dst, const SkPixmap &src, const Options &options )
{
	return SkPngEncoder::Make(dst, src, png_options(options));
}

}  // namespace SkPngRustEncoder
