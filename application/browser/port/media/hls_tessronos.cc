/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	hls_tessronos.cc
 *	ブラウザ: HLS presentations for the media elements (design 14.6).
 *
 *	A media element whose resource is an HLS playlist (#EXTM3U) is
 *	played by the media library's HLS demuxer (HlsManifestDemuxerEngine
 *	inside a ManifestDemuxer): it reads the playlists, picks the
 *	renditions, and feeds the segments (MPEG-2 TS, fragmented MP4, packed
 *	audio) to the pipeline through the stream parsers Media Source
 *	Extensions use. What the demuxer reads comes through the data sources
 *	made here: each playlist or segment is fetched whole through the
 *	network stack, on the page's thread, and read from memory.
 */

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/sequence_bound.h"
#include "media/base/demuxer.h"
#include "media/base/media_log.h"
#include "media/base/media_track.h"
#include "media/media_buildflags.h"
#include "url/gurl.h"
#include "url/origin.h"

#if BUILDFLAG(ENABLE_HLS_DEMUXER)
#include "media/base/cross_origin_data_source.h"
#include "media/filters/hls_data_source_provider_impl.h"
#include "media/filters/hls_manifest_demuxer_engine.h"
#include "media/filters/manifest_demuxer.h"
#endif

#include "../net/br_net.h"

#if BUILDFLAG(ENABLE_HLS_DEMUXER)
namespace {

/* One playlist or segment, fetched whole when the demuxer starts to read it */
class BrHlsDataSource final : public media::CrossOriginDataSource {
public:
	explicit BrHlsDataSource( const GURL &url ) : url_(url), final_url_(url) {}

	void Initialize( base::OnceCallback<void( bool )> init_cb ) override
	{
		init_cb_ = std::move(init_cb);
		auto *ctx = new base::WeakPtr<BrHlsDataSource>(weak_factory_.GetWeakPtr());
		if ( !br_net_fetch_start(url_.spec().c_str(), "GET", nullptr, 0, "", &Fetched, ctx) ) {
			delete ctx;
			std::move(init_cb_).Run(false);
		}
	}

	const std::string &GetMimeType() const override { return mime_; }

	void Read( int64_t position, base::span<uint8_t> data, ReadCB read_cb ) override
	{
		if ( stopped_ || position < 0 ) {
			std::move(read_cb).Run(kReadError);
			return;
		}
		const int64_t have = static_cast<int64_t>(bytes_.size());
		const int n = position >= have ? 0
				: static_cast<int>(std::min<int64_t>(static_cast<int64_t>(data.size()), have - position));
		if ( n > 0 ) memcpy(data.data(), bytes_.data() + position, static_cast<size_t>(n));
		std::move(read_cb).Run(n);
	}

	void Stop() override { stopped_ = true; }
	void Abort() override {}
	bool GetSize( int64_t *size_out ) override
	{
		*size_out = static_cast<int64_t>(bytes_.size());
		return true;
	}
	void SetBitrate( int ) override {}
	bool PassedTimingAllowOriginCheck() override { return true; }
	bool AssumeFullyBuffered() const override { return true; }
	bool DidRedirect() const override { return final_url_ != url_; }
	GURL GetUrlAfterRedirects() const override { return final_url_; }
	int64_t GetMemoryUsage() override { return static_cast<int64_t>(bytes_.size()); }
	bool WouldTaintOrigin() const override { return false; }
	bool IsStreaming() const override { return false; }

private:
	static void Fetched( void *ctx, int ok, int status, const char *url, const char *,
			     const char *ctype, const char *data, unsigned len )
	{
		std::unique_ptr<base::WeakPtr<BrHlsDataSource>> self(static_cast<base::WeakPtr<BrHlsDataSource> *>(ctx));
		BrHlsDataSource *s = self->get();
		if ( s == nullptr || !s->init_cb_ ) return;
		const bool good = ok && status >= 200 && status < 300;
		if ( good ) {
			s->bytes_.assign(data != nullptr ? data : "", len);
			if ( ctype != nullptr ) {
				s->mime_ = ctype;
				s->mime_ = s->mime_.substr(0, s->mime_.find(';'));
			}
			if ( url != nullptr && url[0] != 0 ) s->final_url_ = GURL(url);
		} else {
			printf("browser: media: HLS %s not fetched (%d, status %d)\n", s->url_.spec().c_str(), ok, status);
		}
		std::move(s->init_cb_).Run(good);
	}

	GURL					url_;
	GURL					final_url_;
	std::string				mime_;
	std::string				bytes_;
	bool					stopped_ = false;
	base::OnceCallback<void( bool )>	init_cb_;
	base::WeakPtrFactory<BrHlsDataSource>	weak_factory_{this};
};

/* Makes the data sources, on the page's thread where the provider lives */
class BrHlsDataSourceFactory final : public media::CrossOriginDataSource::Factory {
public:
	void Create( const GURL &uri, media::DataSource::CacheMode, media::DataSource::EncodingMode,
		     base::OnceCallback<void( std::unique_ptr<media::CrossOriginDataSource> )> cb ) override
	{
		std::move(cb).Run(std::make_unique<BrHlsDataSource>(uri));
	}
};

}  // namespace
#endif  // BUILDFLAG(ENABLE_HLS_DEMUXER)

/* Whether the resource fetched is an HLS playlist: it starts with #EXTM3U (after a byte order mark) */
bool br_is_hls_playlist( const std::string &bytes )
{
	size_t at = bytes.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
	return bytes.compare(at, 7, "#EXTM3U") == 0;
}

/*
 * The demuxer of an HLS presentation whose root playlist is at 'url':
 * none when the engine is built without HLS
 */
std::unique_ptr<media::Demuxer> br_hls_demuxer( scoped_refptr<base::SingleThreadTaskRunner> main_task_runner,
						scoped_refptr<base::SequencedTaskRunner> media_task_runner,
						const GURL &url, const url::Origin &origin,
						media::MediaLog *media_log,
						base::RepeatingCallback<void( base::TimeDelta )> request_seek,
						std::unique_ptr<media::TrackManager> tracks )
{
#if BUILDFLAG(ENABLE_HLS_DEMUXER)
	base::SequenceBound<media::HlsDataSourceProvider> provider =
		base::SequenceBound<media::HlsDataSourceProviderImpl>(
			main_task_runner, std::make_unique<BrHlsDataSourceFactory>());
	auto engine = std::make_unique<media::HlsManifestDemuxerEngine>(
		std::move(provider), media_task_runner, std::move(tracks), false, origin, url, media_log);
	return std::make_unique<media::ManifestDemuxer>(media_task_runner, std::move(request_seek),
							 std::move(engine), media_log);
#else
	return nullptr;
#endif
}
