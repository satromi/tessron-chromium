/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_gpu_client.cc
 *	ブラウザ: the page and its canvases rastered in the GPU process, the
 *	renderer's side (design 14.13.10; make GL=1).
 *
 *	A renderer asks the browser for a channel to the GPU process
 *	(Host.GpuChannel) and makes one context of the raster's command
 *	buffer on it (viz::ContextProviderCommandBuffer, on the main thread).
 *	A view is rastered as the GPU process's work: the page's record is
 *	put in a display list, played into a shared image of the view's size
 *	(RasterCHROMIUM, one playback a rectangle that changed), and the
 *	rectangles are read back (ReadbackImagePixels, which waits for the
 *	work) into the page's memory, which the browser shows: one round trip
 *	a view, for their union when it is not much larger. The shared image
 *	keeps what the page's memory holds (a new one, or one after a view
 *	that failed, is rastered whole), so the union may be read. The images of
 *	the record are decoded here when the raster asks for them and carried
 *	in the transfer cache (cc::GpuImageDecodeCache); the letters' glyphs
 *	go as Skia's serialized strikes.
 *
 *	The canvases (2D) of the main thread are shared images of the same
 *	context, through the hooks of Blink's provider of a canvas's pixels
 *	(patch canvas_gpu.patch): their records are rastered into them as
 *	the page's are, putImageData writes into them (WritePixels), and a
 *	snapshot is an image backed by the shared image, which the page's
 *	raster names by its mailbox and draws in the GPU process without a
 *	copy; asked for its pixels (getImageData, toBlob, a canvas on the
 *	processor) it is read back. A snapshot keeps its shared image, and a
 *	canvas drawn again while one does is copied to a new one first.
 *
 *	The channel is watched: when it is lost (the GPU process ended, or
 *	was started again for a GPU that came after it), the context and the
 *	shared images go, the pages are painted on the processor, a new
 *	channel is asked for, and on it every page of the renderer is drawn
 *	again whole. What the canvases on the GPU held is lost with the
 *	process, as Chrome's are; they go on on the new context, or on the
 *	processor while there is none, and what they drew on the processor
 *	is written into a shared image when a context comes back.
 *
 *	While the browser has given a page the window's GPU buffers (two
 *	shared images of BOs the window is shown from, design 14.13.10),
 *	its views are rastered into them in turn instead (raster_into),
 *	nothing read back: the frame is told once the GPU process has taken
 *	the commands in (br_gpu_gsurf_after, a sync token signalled).
 *
 *	WebGL's and WebGPU's contexts are command buffers on the same
 *	channel (port/webgl/gl_cmdbuf.cc, port/webgpu/webgpu_cmdbuf.cc, which
 *	ask for it here); the pixels of a WebGPU canvas are read back with
 *	this raster context.
 *
 *	With the pages in the browser's process (process=single) the raster
 *	is the one of the renderer's own GPU thread, br_gpu.cc's.
 */

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/functional/function_ref.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/single_thread_task_runner.h"
#include "cc/paint/display_item_list.h"
#include "cc/paint/image_provider.h"
#include "cc/paint/paint_image.h"
#include "cc/paint/paint_image_builder.h"
#include "cc/paint/paint_op.h"
#include "cc/paint/paint_record.h"
#include "cc/paint/skia_paint_canvas.h"
#include "cc/paint/target_color_params.h"
#include "cc/paint/texture_backing.h"
#include "cc/raster/playback_image_provider.h"
#include "cc/tiles/gpu_image_decode_cache.h"
#include "components/viz/common/resources/shared_image_format.h"
#include "gpu/command_buffer/client/client_shared_image.h"
#include "gpu/command_buffer/client/context_support.h"
#include "gpu/command_buffer/client/raster_interface.h"
#include "gpu/command_buffer/client/shared_image_interface.h"
#include "gpu/command_buffer/client/shared_memory_limits.h"
#include "gpu/command_buffer/common/mailbox.h"
#include "gpu/command_buffer/common/shared_image_info.h"
#include "gpu/command_buffer/common/shared_image_usage.h"
#include "gpu/config/gpu_feature_info.h"
#include "gpu/config/gpu_info.h"
#include "gpu/ipc/client/gpu_channel_host.h"
#include "gpu/ipc/client/gpu_channel_observer.h"
#include "gpu/ipc/common/gpu_channel.mojom.h"
#include "gpu/ipc/common/gpu_feature_info.mojom.h"
#include "gpu/ipc/common/gpu_feature_info_mojom_traits.h"
#include "gpu/ipc/common/gpu_info.mojom.h"
#include "gpu/ipc/common/gpu_info_mojom_traits.h"
#include "gpu/ipc/common/shared_image_capabilities.mojom.h"
#include "gpu/ipc/common/shared_image_capabilities_mojom_traits.h"
#include "services/viz/public/cpp/gpu/command_buffer_metrics.h"
#include "services/viz/public/cpp/gpu/context_provider_command_buffer.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/skia/include/core/SkColorSpace.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "third_party/skia/include/core/SkPixmap.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "third_party/skia/include/core/SkSurfaceProps.h"
#include "ui/gfx/color_space.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/geometry/vector2d_f.h"
#include "ui/gfx/hdr_metadata.h"
#include "url/gurl.h"

#include "application/browser/mojom/br_process.mojom.h"

#include "../../br.h"
#include "../../br_proc.h"
#include "br_gpu.h"

/* br_proc.cc: the thread Mojo watches the channels on */
scoped_refptr<base::SingleThreadTaskRunner> br_proc_io_runner();

/* br_rproc.cc: a channel to the GPU process, asked of the browser (Host.GpuChannel), waited for or not */
bool br_rproc_gpu_channel( br::mojom::GpuChannelInfoPtr *out );
void br_rproc_gpu_channel_later( base::OnceCallback<void(br::mojom::GpuChannelInfoPtr)> cb );
/* br_rproc.cc: work for the renderer's main thread; every page of the renderer drawn again whole */
void br_rproc_post_main( base::OnceClosure work );
void br_rproc_redraw_all();

/*
 * The hooks of the provider of a canvas's pixels (patch canvas_gpu.patch,
 * in canvas_2d_bitmap_provider.cc), set here once the raster is in the
 * GPU process
 */
namespace blink {
extern void *(*g_canvas_2d_gpu_make)( int width, int height, SkColorType ct, SkAlphaType at,
				      sk_sp<SkColorSpace> cs, const SkSurfaceProps &props );
extern void (*g_canvas_2d_gpu_free)( void *surface );
extern void (*g_canvas_2d_gpu_raster)( void *surface, cc::PaintRecord record, cc::ImageProvider *images,
				       cc::PlaybackCallbacks::CustomDataRasterCallback callback );
extern bool (*g_canvas_2d_gpu_write)( void *surface, const SkImageInfo &info, const void *pixels,
				      size_t row_bytes, int x, int y );
extern cc::PaintImage (*g_canvas_2d_gpu_snapshot)( void *surface, cc::PaintImage::Id id,
						   const gfx::HDRMetadata &hdr );
}  // namespace blink

namespace {

/* What the decoded images the raster takes may hold, in the transfer cache */
const size_t kImageBytes = 64u << 20;

/* The calls timed for a round trip to the GPU process */
const int kTrips = 100;

/* A canvas smaller than this many pixels stays on the processor: a shared image and its commands cost more */
const long kCanvasMinPixels = 128L * 128L;

long now_us()
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000000L + t.tv_nsec / 1000;
}

class Watch final : public gpu::GpuChannelLostObserver {
public:
	void OnGpuChannelLost() override;
};

/* A wait for the GPU process to take in what was rastered into a window's buffer, from when the raster ended */
struct GsWait {
	unsigned			id = 0;
	long				since_us = 0;
	std::function<void( bool )>	done;
};

struct Client {
	bool						inproc = false;	/* br_gpu.cc's raster (one process) */
	int						mode = BR_GPU_AUTO;
	pthread_t					main;	/* the thread of Blink's main sequence */
	/*
	 * The context and what is made on it, used by the main thread and by
	 * a snapshot's readback on any other; the lock is taken again by a
	 * readback the main thread makes while it holds it
	 */
	std::recursive_mutex				mu;
	bool						on = false;	/* the pages are rastered in the GPU process */
	bool						bound = false;	/* the context was tried */
	std::atomic<unsigned>				gen{1};	/* the channel's: a shared image of another is gone */
	std::atomic<bool>				lost{false};	/* the channel was lost; a new one is asked for */
	bool						asking = false;	/* a new channel is asked for */
	unsigned					reset = 0;	/* the raster context's reset status, when it alone was lost */
	bool						none = false;	/* the browser has no GPU process to give */
	bool						webgl2 = false;	/* the GPU process's GL has OpenGL ES 3 */
	bool						webgpu = false;	/* the GPU process runs Dawn (OpenGL ES 3.1) */
	scoped_refptr<gpu::GpuChannelHost>		host;
	Watch						watch;
	scoped_refptr<viz::ContextProviderCommandBuffer> ctx;
	std::unique_ptr<cc::GpuImageDecodeCache>	images;
	scoped_refptr<gpu::ClientSharedImage>		view;	/* the view's pixels */
	gpu::SyncToken					view_token;	/* what the last access of it gave */
	bool						view_cleared = false;	/* cleared once, as a new one must be */
	bool						view_whole = true;	/* not all of it is what the page's memory holds */
	std::string					renderer;
	BrGpuStat					stat;
	/*
	 * The window's GPU buffers: the GPU process the channel goes to, the
	 * image the next raster goes into (null: the view's own, read back),
	 * the waits for the GPU process to take in what was rastered, and when
	 * the last raster into a buffer ended
	 */
	int						gpu_pid = 0;
	const unsigned char				*aim = nullptr;
	unsigned					gs_next = 0;
	std::vector<GsWait>				gs_waits;
	long						gs_end_us = 0;
};

Client	c;

void recover();

/* On the IO thread: the channel is gone; the main thread asks for a new one */
void Watch::OnGpuChannelLost()
{
	if ( c.lost.exchange(true) ) return;
	br_rproc_post_main(base::BindOnce(&recover));
}

/*
 * A channel the browser gave taken: the host made, the channel watched,
 * and whether the pages are rastered there; false when it is not one
 */
bool take( br::mojom::GpuChannelInfoPtr ch )
{
	c.renderer = ch->renderer;
	gpu::GPUInfo info;
	gpu::GpuFeatureInfo feature;
	gpu::SharedImageCapabilities caps;
	if ( !gpu::mojom::GpuInfo::Deserialize(ch->gpu_info, &info)
	     || !gpu::mojom::GpuFeatureInfo::Deserialize(ch->feature_info, &feature)
	     || !gpu::mojom::SharedImageCapabilities::Deserialize(ch->shared_image_caps, &caps) ) {
		printf("browser: gpu: the GPU process's channel came with what could not be read: "
		       "the page is rastered on the processor\n");
		return false;
	}
	c.host = gpu::GpuChannelHost::Create((int)getpid(), info, feature, caps, std::move(ch->channel),
					     br_proc_io_runner());
	c.lost = false;
	if ( !c.host->AddObserverIfNotAlreadyLost(&c.watch) ) {
		c.lost = true;
		br_rproc_post_main(base::BindOnce(&recover));
	}
	c.on = ch->raster && c.mode != BR_GPU_OFF;
	c.webgl2 = ch->webgl2;
	c.webgpu = ch->webgpu;
	c.gpu_pid = ch->gpu_pid;
	c.stat.proc = 1;
	return true;
}

/* The context and all that was made on it let go of (the lock held) */
void drop()
{
	c.images.reset();
	c.view = nullptr;
	c.ctx = nullptr;
	c.bound = false;
	c.view_cleared = false;
	c.view_whole = true;
	if ( c.host != nullptr ) {
		c.host->RemoveObserver(&c.watch);
		c.host = nullptr;
	}
	c.gpu_pid = 0;
	c.gen++;
	/* what was rastered into the window's buffers and not yet taken in is lost with the channel: told, in turn */
	std::vector<GsWait> waits;
	waits.swap(c.gs_waits);
	for ( GsWait &w : waits ) {
		br_rproc_post_main(base::BindOnce([]( std::function<void( bool )> done ) { done(false); }, std::move(w.done)));
	}
}

/* On the main thread: what was rastered into a window's buffer has been taken in by the GPU process */
void gs_taken( unsigned id )
{
	std::function<void( bool )> done;
	{
		std::lock_guard<std::recursive_mutex> hold(c.mu);
		for ( size_t i = 0; i < c.gs_waits.size(); i++ ) {
			if ( c.gs_waits[i].id != id ) continue;
			done = std::move(c.gs_waits[i].done);
			c.stat.gs_take_us += now_us() - c.gs_waits[i].since_us;
			c.gs_waits.erase(c.gs_waits.begin() + (long)i);
			break;
		}
	}
	if ( done ) done(true);
}

/* On the main thread: the answer to the ask for a new channel */
void again( br::mojom::GpuChannelInfoPtr ch )
{
	std::lock_guard<std::recursive_mutex> hold(c.mu);
	c.asking = false;
	if ( !ch ) {
		c.none = true;
		c.on = false;
		printf("browser: gpu: the browser has no GPU process to give: the pages are rastered on the processor\n");
	} else if ( take(std::move(ch)) ) {
		printf("browser: gpu: a new channel to the GPU process (%s): the pages are drawn again%s\n", c.renderer.c_str(),
		       c.on ? ", rastered there" : " on the processor");
	}
	br_rproc_redraw_all();
}

/*
 * On the main thread, when the channel or the context is lost: all of
 * it let go of, the pages painted on the processor, and a new channel
 * asked for without waiting (the browser may be starting the GPU
 * process again)
 */
void recover()
{
	std::lock_guard<std::recursive_mutex> hold(c.mu);
	if ( c.asking || c.none ) return;
	bool was = c.on || c.reset != 0;
	if ( c.reset != 0 ) {
		/* the context's loss, seen by the renderer or told by the GPU process; the channel may still be up */
		bool channel = c.host != nullptr && c.host->IsLost();
		printf("browser: gpu: the raster's context in the GPU process is lost (reset status 0x%x, the channel %s): "
		       "a new channel is asked for\n", c.reset, channel ? "lost too" : "still up");
		c.reset = 0;
	}
	c.on = false;
	drop();
	c.asking = true;
	printf("browser: gpu: the channel to the GPU process is lost: the pages are rastered on the processor%s "
	       "until a new one comes\n", was ? "" : " (as they were)");
	br_rproc_gpu_channel_later(base::BindOnce(&again));
}

/*
 * On the main thread (Blink's sequence there): the raster's context
 * bound; false when there is none, and the page is rastered on the
 * processor until the channel is made again
 */
bool bind()
{
	if ( c.bound ) return c.ctx != nullptr;
	if ( c.host == nullptr ) return false;
	c.bound = true;
	c.ctx = viz::ContextProviderCommandBuffer::CreateForRaster(
		c.host, /*stream_id=*/0, gpu::SchedulingPriority::kNormal, GURL("chrome://gpu/BrRaster"),
		/*automatic_flushes=*/false, /*support_locking=*/false, gpu::SharedMemoryLimits(),
		viz::command_buffer_metrics::ContextType::RENDERER_MAIN_THREAD);
	gpu::ContextResult r = c.ctx->BindToCurrentSequence();
	if ( r != gpu::ContextResult::kSuccess ) {
		printf("browser: gpu: the raster's context in the GPU process failed (%d): the page is rastered on the processor\n",
		       (int)r);
		c.ctx = nullptr;
		c.on = false;
		return false;
	}
	c.images = std::make_unique<cc::GpuImageDecodeCache>(c.ctx.get(), kN32_SkColorType, kImageBytes,
							     c.ctx->ContextCapabilities().max_texture_size,
							     /*dark_mode_filter=*/nullptr);
	return true;
}

/*
 * The context to draw with, on the main thread: bound, not lost; a lost
 * one starts the ask for a new channel and is not used
 */
gpu::raster::RasterInterface *usable()
{
	if ( !c.on || c.lost || !bind() ) return nullptr;
	gpu::raster::RasterInterface *ri = c.ctx->RasterInterface();
	GLenum reset = ri->GetGraphicsResetStatusKHR();
	if ( reset != 0 ) {
		c.reset = reset;
		br_rproc_post_main(base::BindOnce(&recover));
		c.on = false;
		return nullptr;
	}
	return ri;
}

/*
 * A round trip to the GPU process timed, once, when the first view has
 * gone to the browser: the calls hold the main thread, and the page is
 * shown first
 */
void time_trip()
{
	static bool done;

	if ( done ) return;
	done = true;
	long t0 = now_us();
	for ( int i = 0; i < kTrips; i++ ) c.host->GetGpuChannel().Flush();
	long us = now_us() - t0;
	c.stat.trip_us = us / kTrips;
	printf("browser: gpu: a round trip to the GPU process %ld us (%d calls, %ld.%03ld ms)\n", us / kTrips, kTrips,
	       us / 1000, us % 1000);
}

/* What one rectangle of the view gives the readback: the page's memory at the same place */
bool read_back( gpu::raster::RasterInterface *ri, const gfx::Rect &r, void *px, size_t pitch )
{
	SkImageInfo dst = SkImageInfo::MakeN32Premul(r.width(), r.height());
	c.stat.trips++;
	c.stat.read_px += (long)r.width() * r.height();
	return ri->ReadbackImagePixels(c.view->mailbox(), dst, (GLuint)pitch, r.x(), r.y(), /*plane_index=*/0,
				       static_cast<char *>(px) + (size_t)r.y() * pitch + (size_t)r.x() * 4);
}

/* ---------------------------------------------------------------- canvases (2D) */

/*
 * A canvas's pixels: a shared image of the context of generation 'gen',
 * or, while there is no context to make one on, a surface on the
 * processor (what it draws there is written into a shared image when a
 * context comes)
 */
struct Canvas {
	SkImageInfo				info;
	SkSurfaceProps				props;
	scoped_refptr<gpu::ClientSharedImage>	img;
	unsigned				gen = 0;
	gpu::SyncToken				token;	/* what the last access of img gave */
	sk_sp<SkSurface>			cpu;
	unsigned long				change = 0;	/* rasters and writes */
	unsigned long				snap_change = ~0UL;	/* the change the last snapshot saw */
	cc::PaintImage::ContentId		content = cc::PaintImage::kInvalidContentId;
};

/*
 * The format of the shared images the page and the canvases are rastered
 * into: the order of the processor's colours (N32, BGRA) where the GPU
 * process's GL makes BGRA textures, so that a readback into the page's
 * memory (N32) or a canvas of that order converts nothing; RGBA otherwise
 */
viz::SharedImageFormat raster_format( SkColorType ct )
{
	static bool told;
	bool bgra = c.ctx != nullptr && c.ctx->ContextCapabilities().texture_format_bgra8888;
	if ( !told ) {
		told = true;
		printf("browser: gpu: the shared images of the view and the canvases are %s\n",
		       bgra ? "BGRA8, as the page's memory" : "RGBA8 (the GPU process's GL makes no BGRA textures)");
	}
	return bgra && ct == kBGRA_8888_SkColorType ? viz::SinglePlaneFormat::kBGRA_8888 : viz::SinglePlaneFormat::kRGBA_8888;
}

gfx::ColorSpace space_of( const SkImageInfo &info )
{
	return info.colorSpace() != nullptr ? gfx::ColorSpace(*info.colorSpace()) : gfx::ColorSpace::CreateSRGB();
}

/* A shared image for the canvas, cleared to what an empty canvas is (the service draws into none that is not) */
scoped_refptr<gpu::ClientSharedImage> canvas_image( gpu::raster::RasterInterface *ri, const Canvas *cv,
						    gpu::SyncToken *token )
{
	gpu::SharedImageInterface *sii = c.ctx->SharedImageInterface();
	/* of the canvas's order where the GL makes it (raster_format): the raster, the writes and the readbacks convert others */
	gpu::SharedImageInfo si(raster_format(cv->info.colorType()), gfx::Size(cv->info.width(), cv->info.height()),
				space_of(cv->info),
				gpu::SharedImageUsageSet(gpu::SHARED_IMAGE_USAGE_RASTER_WRITE) |
					gpu::SHARED_IMAGE_USAGE_RASTER_READ,
				"BrCanvas");
	scoped_refptr<gpu::ClientSharedImage> img = sii->CreateSharedImage(si, gpu::kNullSurfaceHandle);
	if ( img == nullptr ) return nullptr;
	std::unique_ptr<gpu::RasterScopedAccess> access = img->BeginRasterAccess(ri, img->creation_sync_token(),
										 /*readonly=*/false);
	ri->BeginRasterCHROMIUM(cv->info.alphaType() == kOpaque_SkAlphaType ? SkColors::kBlack : SkColors::kTransparent,
				/*needs_clear=*/true, /*msaa_sample_count=*/0, gpu::raster::MsaaMode::kNoMSAA,
				/*can_use_lcd_text=*/false, /*visible=*/true, space_of(cv->info), /*hdr_headroom=*/1.0f,
				img->mailbox().name);
	ri->EndRasterCHROMIUM();
	*token = gpu::RasterScopedAccess::EndAccess(std::move(access));
	return img;
}

/*
 * The canvas made ready to draw into (the lock held): on a shared image
 * of the context there is now, which a snapshot does not hold (one that
 * does is copied first); on the processor while there is none. What it
 * held on a context that is gone is lost. 'ri' is the context, or null
 * for the processor.
 */
gpu::raster::RasterInterface *canvas_ready( Canvas *cv, bool write )
{
	gpu::raster::RasterInterface *ri = usable();
	if ( ri == nullptr ) {
		cv->img = nullptr;
		if ( cv->cpu == nullptr ) {
			cv->cpu = SkSurfaces::Raster(cv->info, &cv->props);
			cv->change++;
		}
		return nullptr;
	}
	if ( cv->img != nullptr && cv->gen == c.gen ) {
		if ( !write || cv->img->HasOneRef() ) return ri;
		/* a snapshot holds the shared image: the canvas goes on in a copy */
		gpu::SyncToken token;
		scoped_refptr<gpu::ClientSharedImage> copy = canvas_image(ri, cv, &token);
		if ( copy == nullptr ) return nullptr;
		ri->WaitSyncTokenCHROMIUM(cv->token.GetConstData());
		ri->CopySharedImage(cv->img->mailbox(), copy->mailbox(), 0, 0, 0, 0, cv->info.width(), cv->info.height());
		ri->GenUnverifiedSyncTokenCHROMIUM(token.GetData());
		cv->img = std::move(copy);
		cv->token = token;
		return ri;
	}
	cv->img = canvas_image(ri, cv, &cv->token);
	if ( cv->img == nullptr ) return nullptr;
	cv->gen = c.gen;
	cv->change++;
	if ( cv->cpu != nullptr ) {
		/* what was drawn on the processor while there was no context */
		SkPixmap pm;
		if ( cv->cpu->peekPixels(&pm) ) {
			ri->WaitSyncTokenCHROMIUM(cv->token.GetConstData());
			ri->WritePixels(cv->img->mailbox(), 0, 0, cv->img->GetTextureTarget(), pm);
			ri->GenUnverifiedSyncTokenCHROMIUM(cv->token.GetData());
		}
		cv->cpu = nullptr;
	}
	return ri;
}

/*
 * A snapshot of a canvas: its shared image, named by its mailbox in the
 * page's raster (drawn in the GPU process without a copy), read back when
 * its pixels are asked for (on any thread). One of a context that is
 * gone has no mailbox and no pixels: the page draws nothing there.
 */
class Backing final : public cc::TextureBacking {
public:
	Backing( scoped_refptr<gpu::ClientSharedImage> img, const SkImageInfo &info, unsigned gen )
		: img_(std::move(img)), info_(info), gen_(gen) {}

	/*
	 * The last of an image's holders: the image goes once the raster's
	 * work so far (a page's raster that drew it) is done
	 */
	~Backing() override
	{
		if ( !pthread_equal(pthread_self(), c.main) || !img_->HasOneRef() ) return;
		std::lock_guard<std::recursive_mutex> hold(c.mu);
		if ( gen_ != c.gen || c.lost || c.ctx == nullptr ) return;
		gpu::raster::RasterInterface *ri = c.ctx->RasterInterface();
		gpu::SyncToken token;
		ri->GenUnverifiedSyncTokenCHROMIUM(token.GetData());
		ri->ShallowFlushCHROMIUM();
		img_->UpdateDestructionSyncToken(token);
	}

	const SkImageInfo &GetSkImageInfo() override { return info_; }
	gpu::Mailbox GetMailbox() const override { return gen_ == c.gen ? img_->mailbox() : gpu::Mailbox(); }
	void Bind( scoped_refptr<cc::TextureBackingContext> ) override {}
	void Unbind() override {}

	sk_sp<SkImage> GetSkImageViaReadback() override
	{
		std::lock_guard<std::recursive_mutex> hold(c.mu);
		if ( raster_ != nullptr ) return raster_;
		SkBitmap bm;
		if ( !bm.tryAllocPixels(info_) || !read(info_, bm.getPixels(), bm.rowBytes(), 0, 0) ) return nullptr;
		bm.setImmutable();
		raster_ = SkImages::RasterFromBitmap(bm);
		return raster_;
	}

	bool readPixels( const SkImageInfo &dst_info, void *dst_pixels, size_t dst_row_bytes, int src_x,
			 int src_y ) override
	{
		std::lock_guard<std::recursive_mutex> hold(c.mu);
		return read(dst_info, dst_pixels, dst_row_bytes, src_x, src_y);
	}

private:
	/* The lock held */
	bool read( const SkImageInfo &dst, void *px, size_t rb, int x, int y )
	{
		if ( gen_ != c.gen || c.lost || c.ctx == nullptr ) return false;
		c.stat.trips++;
		return c.ctx->RasterInterface()->ReadbackImagePixels(img_->mailbox(), dst, (GLuint)rb, x, y,
								     /*plane_index=*/0, px);
	}

	scoped_refptr<gpu::ClientSharedImage>	img_;
	SkImageInfo				info_;
	unsigned				gen_;
	sk_sp<SkImage>				raster_;	/* its pixels read back, when asked for */
};

void *canvas_make( int width, int height, SkColorType ct, SkAlphaType at, sk_sp<SkColorSpace> cs,
		   const SkSurfaceProps &props )
{
	if ( !pthread_equal(pthread_self(), c.main) ) return nullptr;
	if ( (long)width * height < kCanvasMinPixels ) return nullptr;
	if ( ct != kN32_SkColorType && ct != kRGBA_8888_SkColorType && ct != kBGRA_8888_SkColorType ) return nullptr;
	std::lock_guard<std::recursive_mutex> hold(c.mu);
	if ( usable() == nullptr ) return nullptr;
	Canvas *cv = new Canvas;
	cv->info = SkImageInfo::Make(width, height, ct, at, std::move(cs));
	cv->props = props;
	if ( canvas_ready(cv, true) == nullptr ) {
		delete cv;
		return nullptr;
	}
	return cv;
}

void canvas_free( void *s )
{
	std::lock_guard<std::recursive_mutex> hold(c.mu);
	Canvas *cv = static_cast<Canvas *>(s);
	if ( cv->img != nullptr ) cv->img->UpdateDestructionSyncToken(cv->token);
	delete cv;
}

void canvas_raster( void *s, cc::PaintRecord record, cc::ImageProvider *images,
		    cc::PlaybackCallbacks::CustomDataRasterCallback callback )
{
	std::lock_guard<std::recursive_mutex> hold(c.mu);
	Canvas *cv = static_cast<Canvas *>(s);
	long t0 = now_us();
	gpu::raster::RasterInterface *ri = canvas_ready(cv, true);
	cv->change++;
	if ( ri == nullptr ) {
		cc::SkiaPaintCanvas pc(cv->cpu->getCanvas(), images);
		pc.drawPicture(std::move(record), callback);
		return;
	}
	auto list = base::MakeRefCounted<cc::DisplayItemList>();
	list->StartPaint();
	list->push<cc::DrawRecordOp>(std::move(record));
	list->EndPaintOfUnpaired(gfx::Rect(cv->info.width(), cv->info.height()));
	list->Finalize();

	/* the images of the record decoded here and carried in the transfer cache, as the page's are */
	cc::PlaybackImageProvider::Settings settings;
	settings.raster_mode = cc::PlaybackImageProvider::RasterMode::kGpu;
	cc::PlaybackImageProvider gpu_images(c.images.get(), cc::TargetColorParams(space_of(cv->info)),
					     std::move(settings));
	gfx::Size size(cv->info.width(), cv->info.height());
	std::unique_ptr<gpu::RasterScopedAccess> access = cv->img->BeginRasterAccess(ri, cv->token, /*readonly=*/false);
	ri->BeginRasterCHROMIUM(SkColors::kTransparent, /*needs_clear=*/false, /*msaa_sample_count=*/0,
				gpu::raster::MsaaMode::kNoMSAA,
				/*can_use_lcd_text=*/cv->info.alphaType() == kOpaque_SkAlphaType, /*visible=*/true,
				space_of(cv->info), /*hdr_headroom=*/1.0f, cv->img->mailbox().name);
	size_t hint = gpu::raster::RasterInterface::kDefaultMaxOpSizeHint;
	ri->RasterCHROMIUM(list.get(), &gpu_images, size, gfx::Rect(size), gfx::Rect(size), gfx::Vector2dF(),
			   gfx::Vector2dF(1.0f, 1.0f), /*requires_clear=*/false,
			   /*raster_inducing_scroll_offsets=*/nullptr, &hint, callback);
	ri->EndRasterCHROMIUM();
	cv->token = gpu::RasterScopedAccess::EndAccess(std::move(access));
	/* the GPU process starts on it while the page goes on */
	ri->ShallowFlushCHROMIUM();
	c.stat.canvases++;
	c.stat.canvas_us += now_us() - t0;
}

bool canvas_write( void *s, const SkImageInfo &info, const void *pixels, size_t row_bytes, int x, int y )
{
	std::lock_guard<std::recursive_mutex> hold(c.mu);
	Canvas *cv = static_cast<Canvas *>(s);
	gpu::raster::RasterInterface *ri = canvas_ready(cv, true);
	cv->change++;
	if ( ri == nullptr ) {
		cv->cpu->writePixels(SkPixmap(info, pixels, row_bytes), x, y);
		return true;
	}
	ri->WaitSyncTokenCHROMIUM(cv->token.GetConstData());
	ri->WritePixels(cv->img->mailbox(), x, y, cv->img->GetTextureTarget(), SkPixmap(info, pixels, row_bytes));
	ri->GenUnverifiedSyncTokenCHROMIUM(cv->token.GetData());
	return true;
}

cc::PaintImage canvas_snapshot( void *s, cc::PaintImage::Id id, const gfx::HDRMetadata &hdr )
{
	std::lock_guard<std::recursive_mutex> hold(c.mu);
	Canvas *cv = static_cast<Canvas *>(s);
	(void)canvas_ready(cv, false);
	/* a new content id only when the canvas changed, as the images the page caches go by it */
	if ( cv->change != cv->snap_change || cv->content == cc::PaintImage::kInvalidContentId ) {
		cv->content = cc::PaintImage::GetNextContentId();
		cv->snap_change = cv->change;
	}
	if ( cv->img == nullptr ) {
		sk_sp<SkImage> img = cv->cpu != nullptr ? cv->cpu->makeImageSnapshot() : nullptr;
		if ( img == nullptr ) return cc::PaintImage();
		return cc::PaintImageBuilder::WithDefault()
			.set_id(id)
			.set_image(std::move(img), cv->content)
			.set_hdr_metadata(hdr)
			.TakePaintImage();
	}
	return cc::PaintImageBuilder::WithDefault()
		.set_id(id)
		.set_texture_backing(sk_make_sp<Backing>(cv->img, cv->info, cv->gen), cv->content)
		.set_hdr_metadata(hdr)
		.TakePaintImage();
}

/* ---------------------------------------------------------------- the window's GPU buffers */

/*
 * The rectangles of the view rastered into the window's buffer c.aim (a
 * shared image of the GPU process, which the window is shown from), with
 * nothing read back (the lock held). The buffer keeps what was rastered
 * into it before: the caller asks for every rectangle it lacks (those of
 * the frame drawn into the other buffer meanwhile, or all of it).
 */
bool raster_into( gpu::raster::RasterInterface *ri, const cc::PaintRecord &rec, const std::vector<BrRect> &rects, int w,
		  int h, long t0 )
{
	gpu::Mailbox mb;
	mb.SetName(reinterpret_cast<const int8_t *>(c.aim));
	std::vector<gfx::Rect> play;
	for ( const BrRect &r : rects ) {
		gfx::Rect p = gfx::IntersectRects(gfx::Rect(r.x, r.y, r.w, r.h), gfx::Rect(0, 0, w, h));
		if ( !p.IsEmpty() ) play.push_back(p);
	}
	if ( play.empty() ) return true;

	/* the record in a display list, on white, as the view's own */
	auto list = base::MakeRefCounted<cc::DisplayItemList>();
	list->StartPaint();
	list->push<cc::DrawColorOp>(SkColors::kWhite, SkBlendMode::kSrc);
	list->push<cc::DrawRecordOp>(rec);
	list->EndPaintOfUnpaired(gfx::Rect(0, 0, w, h));
	list->Finalize();
	cc::PlaybackImageProvider::Settings settings;
	settings.raster_mode = cc::PlaybackImageProvider::RasterMode::kGpu;
	cc::PlaybackImageProvider images(c.images.get(), cc::TargetColorParams(gfx::ColorSpace::CreateSRGB()),
					 std::move(settings));

	/* the buffer's shared image is cleared when it is made (its BO starts zeroed) */
	ri->BeginRasterCHROMIUM(SkColors::kWhite, /*needs_clear=*/false, /*msaa_sample_count=*/0,
				gpu::raster::MsaaMode::kNoMSAA, /*can_use_lcd_text=*/false, /*visible=*/true,
				gfx::ColorSpace::CreateSRGB(), /*hdr_headroom=*/1.0f, mb.name);
	size_t hint = gpu::raster::RasterInterface::kDefaultMaxOpSizeHint;
	for ( const gfx::Rect &p : play ) {
		ri->RasterCHROMIUM(list.get(), &images, gfx::Size(w, h), gfx::Rect(0, 0, w, h), p, gfx::Vector2dF(),
				   gfx::Vector2dF(1.0f, 1.0f), /*requires_clear=*/false,
				   /*raster_inducing_scroll_offsets=*/nullptr, &hint, base::NullCallback());
	}
	ri->EndRasterCHROMIUM();
	long t1 = now_us();
	ri->ShallowFlushCHROMIUM();
	long t2 = now_us();
	c.gs_end_us = t2;
	c.stat.views++;
	c.stat.gs_views++;
	c.stat.play_us += t1 - t0;
	c.stat.flush_us += t2 - t1;
	if ( t2 - t0 > c.stat.max_us ) c.stat.max_us = t2 - t0;
	return true;
}

}  // namespace

bool br_gpu_start( int mode )
{
	static bool tried;

	if ( tried ) return c.on;
	tried = true;
	if ( !br_multi_process || !br_sys_gpuproc() ) {
		/* the raster of this process's own GPU thread (process=single, gpuproc=off) */
		c.inproc = true;
		c.on = br_gpu_inproc_start != nullptr && br_gpu_inproc_start(mode);
		return c.on;
	}
	if ( mode == BR_GPU_OFF ) {
		printf("browser: gpu: off (gpu=off), the page is rastered on the processor\n");
		return false;
	}
	c.mode = mode;
	c.main = pthread_self();
	br::mojom::GpuChannelInfoPtr ch;
	if ( !br_rproc_gpu_channel(&ch) || !ch ) {
		printf("browser: gpu: no channel to the GPU process, the page is rastered on the processor\n");
		return false;
	}
	std::lock_guard<std::recursive_mutex> hold(c.mu);
	if ( !take(std::move(ch)) ) return false;
	/*
	 * the canvases' hooks are set even while the pages are not rastered
	 * there: they take a canvas only when they are (a GPU that comes later)
	 */
	blink::g_canvas_2d_gpu_make = canvas_make;
	blink::g_canvas_2d_gpu_free = canvas_free;
	blink::g_canvas_2d_gpu_raster = canvas_raster;
	blink::g_canvas_2d_gpu_write = canvas_write;
	blink::g_canvas_2d_gpu_snapshot = canvas_snapshot;
	if ( !c.on ) {
		printf("browser: gpu: the GPU process is on %s: the page is rastered by Skia on the processor "
		       "(gpu=on rasters there)\n", c.renderer.c_str());
		return false;
	}
	printf("browser: gpu: the page and its canvases are rastered in the GPU process (Ganesh on %s)\n",
	       c.renderer.c_str());
	return true;
}

const char *br_gpu_renderer( void )
{
	if ( c.inproc ) return br_gpu_inproc_renderer != nullptr ? br_gpu_inproc_renderer() : "";
	return c.renderer.c_str();
}

bool br_gpu_raster( const cc::PaintRecord &rec, const std::vector<BrRect> &rects, int w, int h, void *px,
		    size_t pitch )
{
	if ( c.inproc ) return br_gpu_inproc_raster != nullptr && br_gpu_inproc_raster(rec, rects, w, h, px, pitch);
	if ( w <= 0 || h <= 0 ) return false;
	std::lock_guard<std::recursive_mutex> hold(c.mu);
	long t0 = now_us(), t1, t2, t3;
	gpu::raster::RasterInterface *ri = usable();
	if ( ri == nullptr ) return false;
	if ( c.aim != nullptr ) return raster_into(ri, rec, rects, w, h, t0);
	gpu::SharedImageInterface *sii = c.ctx->SharedImageInterface();
	if ( c.view == nullptr || c.view->size() != gfx::Size(w, h) ) {
		if ( c.view != nullptr ) sii->DestroySharedImage(c.view_token, std::move(c.view));
		gpu::SharedImageInfo si(raster_format(kN32_SkColorType), gfx::Size(w, h), gfx::ColorSpace::CreateSRGB(),
					gpu::SharedImageUsageSet(gpu::SHARED_IMAGE_USAGE_RASTER_WRITE) |
						gpu::SHARED_IMAGE_USAGE_RASTER_READ,
					"BrPageView");
		c.view = sii->CreateSharedImage(si, gpu::kNullSurfaceHandle);
		if ( c.view == nullptr ) return false;
		c.view_token = c.view->creation_sync_token();
		c.view_cleared = false;
		c.view_whole = true;
	}
	if ( c.stat.views == 1 ) time_trip();

	/*
	 * The rectangles that changed, clipped to the view; all of it while the
	 * shared image does not hold what the page's memory does (a new one, or
	 * a view that failed and was painted on the processor). From then on
	 * every pixel of the image is what was last read back into the page's
	 * memory, so a readback may take pixels of it outside the rectangles.
	 */
	std::vector<gfx::Rect> play;
	gfx::Rect all;
	long area = 0;
	if ( c.view_whole ) {
		play.push_back(gfx::Rect(0, 0, w, h));
	} else {
		for ( const BrRect &r : rects ) {
			gfx::Rect p = gfx::IntersectRects(gfx::Rect(r.x, r.y, r.w, r.h), gfx::Rect(0, 0, w, h));
			if ( !p.IsEmpty() ) play.push_back(p);
		}
	}
	for ( const gfx::Rect &p : play ) {
		all.Union(p);
		area += (long)p.width() * p.height();
	}
	if ( play.empty() ) return true;
	c.view_whole = true;		/* until this one is read back */

	/* the record in a display list, on white (the view is cleared to it where it is played) */
	auto list = base::MakeRefCounted<cc::DisplayItemList>();
	list->StartPaint();
	list->push<cc::DrawColorOp>(SkColors::kWhite, SkBlendMode::kSrc);
	list->push<cc::DrawRecordOp>(rec);
	list->EndPaintOfUnpaired(gfx::Rect(0, 0, w, h));
	list->Finalize();

	cc::PlaybackImageProvider::Settings settings;
	settings.raster_mode = cc::PlaybackImageProvider::RasterMode::kGpu;
	cc::PlaybackImageProvider images(c.images.get(), cc::TargetColorParams(gfx::ColorSpace::CreateSRGB()),
					 std::move(settings));

	std::unique_ptr<gpu::RasterScopedAccess> access = c.view->BeginRasterAccess(ri, c.view_token, /*readonly=*/false);
	/* a new shared image is cleared (to white) by its first raster: the service does not draw into one that is not */
	ri->BeginRasterCHROMIUM(SkColors::kWhite, /*needs_clear=*/!c.view_cleared, /*msaa_sample_count=*/0,
				gpu::raster::MsaaMode::kNoMSAA, /*can_use_lcd_text=*/false, /*visible=*/true,
				gfx::ColorSpace::CreateSRGB(), /*hdr_headroom=*/1.0f, c.view->mailbox().name);
	size_t hint = gpu::raster::RasterInterface::kDefaultMaxOpSizeHint;
	for ( const gfx::Rect &p : play ) {
		ri->RasterCHROMIUM(list.get(), &images, gfx::Size(w, h), gfx::Rect(0, 0, w, h), p, gfx::Vector2dF(),
				   gfx::Vector2dF(1.0f, 1.0f), /*requires_clear=*/false,
				   /*raster_inducing_scroll_offsets=*/nullptr, &hint, base::NullCallback());
	}
	ri->EndRasterCHROMIUM();
	c.view_cleared = true;
	c.view_token = gpu::RasterScopedAccess::EndAccess(std::move(access));
	t1 = now_us();
	ri->ShallowFlushCHROMIUM();
	t2 = now_us();

	bool ok = true;
	ri->WaitSyncTokenCHROMIUM(c.view_token.GetConstData());
	/*
	 * One readback (one round trip) for all the rectangles when their union
	 * is not much larger than they are; each alone otherwise
	 */
	if ( play.size() > 1 && (long)all.width() * all.height() <= 2 * area ) {
		ok = read_back(ri, all, px, pitch);
	} else {
		for ( const gfx::Rect &p : play ) {
			if ( !read_back(ri, p, px, pitch) ) ok = false;
		}
	}
	if ( ok ) c.view_whole = false;
	t3 = now_us();
	c.stat.views++;
	c.stat.play_us += t1 - t0;
	c.stat.flush_us += t2 - t1;
	c.stat.read_us += t3 - t2;
	if ( t3 - t0 > c.stat.max_us ) c.stat.max_us = t3 - t0;
	return ok;
}

/*
 * The channel WebGL's contexts are made on (port/webgl/gl_cmdbuf.cc), on
 * the main thread: null with the raster of this process's own GPU thread,
 * without a GPU process, or while a lost channel is replaced ('later')
 */
scoped_refptr<gpu::GpuChannelHost> br_gpu_webgl_channel( bool *later, bool *webgl2 )
{
	*later = false;
	*webgl2 = false;
	if ( c.inproc ) return nullptr;
	std::lock_guard<std::recursive_mutex> hold(c.mu);
	if ( c.none ) return nullptr;
	if ( c.host == nullptr || c.lost ) {
		*later = c.asking || c.lost;
		return nullptr;
	}
	*webgl2 = c.webgl2;
	return c.host;
}

/*
 * The channel WebGPU's contexts are made on (port/webgpu/webgpu_cmdbuf.cc),
 * as WebGL's; 'webgpu' whether the GPU process runs Dawn
 */
scoped_refptr<gpu::GpuChannelHost> br_gpu_webgpu_channel( bool *later, bool *webgpu )
{
	bool	webgl2;

	scoped_refptr<gpu::GpuChannelHost> host = br_gpu_webgl_channel(later, &webgl2);
	*webgpu = host != nullptr && c.webgpu;
	return host;
}

/*
 * A frame of WebGL (port/webgl/gl_cmdbuf.cc), on the main thread while the
 * pages are rastered in the GPU process: 'copy' copies the drawing
 * buffer's colour buffer into a new shared image of the raster's context
 * (of 'info', in the order of rows 'origin') and gives the copy's sync
 * token. The image the frame is given as is that shared image, which the
 * page's raster names by its mailbox and draws in the GPU process, after
 * the copy as it is on the same context; read back when its pixels are
 * asked for (a view rastered on the processor, a canvas on the processor
 * it is drawn into). An empty image when there is none.
 */
cc::PaintImage br_gpu_frame_image( const SkImageInfo &info, GrSurfaceOrigin origin,
				   base::FunctionRef<std::optional<gpu::SyncToken>( gpu::raster::RasterInterface *,
										    const scoped_refptr<gpu::ClientSharedImage> &,
										    const gpu::SyncToken & )> copy )
{
	if ( c.inproc || !pthread_equal(pthread_self(), c.main) ) return cc::PaintImage();
	std::lock_guard<std::recursive_mutex> hold(c.mu);
	gpu::raster::RasterInterface *ri = usable();
	if ( ri == nullptr ) return cc::PaintImage();
	gpu::SharedImageInfo si(viz::SinglePlaneFormat::kRGBA_8888, gfx::Size(info.width(), info.height()),
				gfx::ColorSpace::CreateSRGB(), origin, info.alphaType(),
				gpu::SharedImageUsageSet(gpu::SHARED_IMAGE_USAGE_RASTER_WRITE) |
					gpu::SHARED_IMAGE_USAGE_RASTER_READ,
				"BrWebGLFrame");
	scoped_refptr<gpu::ClientSharedImage> img = c.ctx->SharedImageInterface()->CreateSharedImage(si, gpu::kNullSurfaceHandle);
	if ( img == nullptr ) return cc::PaintImage();
	std::optional<gpu::SyncToken> token = copy(ri, img, img->creation_sync_token());
	if ( !token ) return cc::PaintImage();
	return cc::PaintImageBuilder::WithDefault()
		.set_id(cc::PaintImage::GetNextId())
		.set_texture_backing(sk_make_sp<Backing>(std::move(img), info, c.gen), cc::PaintImage::GetNextContentId())
		.TakePaintImage();
}

/*
 * The pixels of a shared image of the channel (a WebGPU canvas's copy,
 * port/webgpu/webgpu_cmdbuf.cc), once the work 'token' ends is done,
 * read back with the raster's context, on any thread; the context is
 * made on the main thread. False when there is none.
 */
bool br_gpu_read_image( const gpu::Mailbox &mailbox, const gpu::SyncToken &token, const SkImageInfo &info, void *px,
			size_t row_bytes )
{
	if ( c.inproc ) return false;
	std::lock_guard<std::recursive_mutex> hold(c.mu);
	if ( c.lost || c.host == nullptr ) return false;
	if ( !c.bound && !pthread_equal(pthread_self(), c.main) ) return false;
	if ( !bind() ) return false;
	gpu::raster::RasterInterface *ri = c.ctx->RasterInterface();
	ri->WaitSyncTokenCHROMIUM(token.GetConstData());
	c.stat.trips++;
	return ri->ReadbackImagePixels(mailbox, info, (GLuint)row_bytes, 0, 0, /*plane_index=*/0, px);
}

void br_gpu_stat( BrGpuStat *s )
{
	if ( c.inproc ) {
		if ( br_gpu_inproc_stat != nullptr ) br_gpu_inproc_stat(s);
		return;
	}
	/* not locked: the counts are only said, and a raster may hold the lock over a round trip */
	*s = c.stat;
}

/* ---------------------------------------------------------------- the window's GPU buffers (br_gpu.h) */

bool br_gpu_gsurf_able( int gpu_pid )
{
	if ( c.inproc || gpu_pid <= 0 || !pthread_equal(pthread_self(), c.main) ) return false;
	std::lock_guard<std::recursive_mutex> hold(c.mu);
	return c.gpu_pid == gpu_pid && usable() != nullptr;
}

void br_gpu_gsurf_aim( const unsigned char *mailbox )
{
	std::lock_guard<std::recursive_mutex> hold(c.mu);
	c.aim = mailbox;
}

void br_gpu_gsurf_after( std::function<void( bool )> done )
{
	std::unique_lock<std::recursive_mutex> hold(c.mu);
	if ( c.inproc || c.lost || c.ctx == nullptr ) {
		hold.unlock();
		done(false);
		return;
	}
	/*
	 * a sync token after the rasters: the GPU process releases it once it
	 * has run every command before it, which for a buffer the GPU draws
	 * into has given the GPU its work (the jobs the window waits for), and
	 * for one on the processor has copied the drawing into it
	 */
	gpu::raster::RasterInterface *ri = c.ctx->RasterInterface();
	gpu::SyncToken token;
	ri->GenUnverifiedSyncTokenCHROMIUM(token.GetData());
	ri->ShallowFlushCHROMIUM();
	GsWait w;
	w.id = ++c.gs_next;
	w.since_us = c.gs_end_us > 0 ? c.gs_end_us : now_us();
	w.done = std::move(done);
	unsigned id = w.id;
	c.gs_waits.push_back(std::move(w));
	c.ctx->ContextSupport()->SignalSyncToken(token, base::BindOnce(&gs_taken, id));
}

bool br_gpu_gsurf_read( const unsigned char *mailbox, int w, int h, void *px, size_t pitch )
{
	if ( c.inproc || w <= 0 || h <= 0 ) return false;
	std::lock_guard<std::recursive_mutex> hold(c.mu);
	if ( c.lost || c.ctx == nullptr ) return false;
	gpu::Mailbox mb;
	mb.SetName(reinterpret_cast<const int8_t *>(mailbox));
	c.stat.trips++;
	return c.ctx->RasterInterface()->ReadbackImagePixels(mb, SkImageInfo::MakeN32Premul(w, h), (GLuint)pitch, 0, 0,
							      /*plane_index=*/0, px);
}
