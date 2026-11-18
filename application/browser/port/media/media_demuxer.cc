/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	media_demuxer.cc
 *	ブラウザ: the demuxer of a resource the player has whole (design
 *	14.6): FFmpeg's, made here with the flags of the media library, whose
 *	headers reach FFmpeg's own.
 */

#include <memory>

#include "base/functional/callback_helpers.h"
#include "media/base/media_tracks.h"
#include "media/filters/ffmpeg_demuxer.h"

/* FFmpeg's demuxer on a data source, its calls on the media thread */
std::unique_ptr<media::Demuxer> br_ffmpeg_demuxer( scoped_refptr<base::SequencedTaskRunner> task_runner,
						   media::DataSource *data,
						   media::Demuxer::EncryptedMediaInitDataCB encrypted_cb,
						   media::MediaLog *media_log )
{
	return std::make_unique<media::FFmpegDemuxer>(task_runner, data, encrypted_cb, base::DoNothing(), media_log,
						      true);
}
