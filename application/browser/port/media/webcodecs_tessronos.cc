/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	webcodecs_tessronos.cc
 *	ブラウザ: what WebCodecs asks of a browser's process (design 14.6.4).
 *
 *	A VideoEncoder tells a browser how its encoding went (the codec, the
 *	size, the frames encoded, an error) through a provider of metrics the
 *	browser's process keeps. There is no such process here and nothing
 *	keeps metrics: the factory gives a provider that is told and keeps
 *	nothing, and the pipe it was given is let go.
 */

#include <memory>

#include "media/base/encoder_status.h"
#include "media/base/svc_scalability_mode.h"
#include "media/base/video_codecs.h"
#include "media/base/video_encoder_metrics_provider.h"
#include "media/mojo/clients/mojo_video_encoder_metrics_provider.h"
#include "ui/gfx/geometry/size.h"

namespace media {

namespace {

class NoMetrics final : public VideoEncoderMetricsProvider {
public:
	void Initialize( VideoCodecProfile, const gfx::Size &, bool, SVCScalabilityMode ) override {}
	void IncrementEncodedFrameCount() override {}
	void SetError( const EncoderStatus & ) override {}
};

}  // namespace

MojoVideoEncoderMetricsProviderFactory::MojoVideoEncoderMetricsProviderFactory(
	mojom::VideoEncoderUseCase use_case, mojo::PendingRemote<mojom::VideoEncoderMetricsProvider> )
	: use_case_(use_case)
{
}

MojoVideoEncoderMetricsProviderFactory::MojoVideoEncoderMetricsProviderFactory( mojom::VideoEncoderUseCase use_case )
	: use_case_(use_case)
{
}

MojoVideoEncoderMetricsProviderFactory::~MojoVideoEncoderMetricsProviderFactory() = default;

std::unique_ptr<VideoEncoderMetricsProvider> MojoVideoEncoderMetricsProviderFactory::CreateVideoEncoderMetricsProvider()
{
	return std::make_unique<NoMetrics>();
}

}  // namespace media
