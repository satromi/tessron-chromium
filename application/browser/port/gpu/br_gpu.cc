/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_gpu.cc
 *	ブラウザ: Skia's GPU raster on the system's OpenGL ES in the process of
 *	the pages, with the pages in the browser's process (process=single;
 *	design 14.7.2; make GL=1). A renderer's pages are rastered in the GPU
 *	process instead (br_gpu_client.cc, br_gpu_main.cc).
 *
 *	There is no compositor here: the page's record (the
 *	paint artifact of Blink's lifecycle, a cc::PaintRecord) is played back
 *	by Skia's Ganesh into a render target of OpenGL ES, and the rectangles
 *	that changed are read back into the surface of the window's work
 *	area, which the window system shows. The context is made on the
 *	EGL and OpenGL ES of TessronOS's Mesa (v3d on the V3D of the Raspberry
 *	Pi 5, softpipe elsewhere) and lives on a thread of its own, the GPU's
 *	thread: every Ganesh object is made, used and let go of there. The
 *	main thread hands it a job and waits for it, so what
 *	the job reads of the main thread's (the record, the images, the
 *	canvases' records) does not change under it; what is let go of
 *	elsewhere (a snapshot of a canvas dropped with the page's image) goes
 *	to the thread as a job of its own, run between the others.
 *
 *	WebGL keeps its contexts of ANGLE on the main thread (port/webgl/
 *	gl_angle.cc). They are other contexts of the same Mesa display, and
 *	a context is current per thread, so the two never take the other's.
 *
 *	The images of the record are drawn by Ganesh as it draws images:
 *	one decoded on the processor is uploaded as a texture, and one whose
 *	generator gives YUV planes -- the frames of a video, a JPEG -- is
 *	uploaded as its planes and made RGB by a shader on the GPU
 *	(port/media/media_player.cc).
 *
 *	The canvases (2D) of the main thread are rastered here too, when the
 *	page is: Blink's provider of a canvas's pixels (canvas_2d_bitmap_
 *	provider.cc, patch canvas_gpu.patch) asks the hooks below for a
 *	surface on the GPU, plays the canvas's records into it and takes
 *	snapshots of it. A snapshot is a PaintImage backed by a texture of
 *	this thread (BrGpuBacking): played back here it is that texture, drawn
 *	without a copy; asked for its pixels anywhere else (getImageData,
 *	toBlob, a canvas on the processor) it is read back.
 */

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "cc/paint/image_provider.h"
#include "cc/paint/paint_image.h"
#include "cc/paint/paint_image_builder.h"
#include "cc/paint/paint_record.h"
#include "cc/paint/skia_paint_canvas.h"
#include "cc/paint/texture_backing.h"
#include "gpu/command_buffer/common/mailbox.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkColorSpace.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkPixmap.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "third_party/skia/include/core/SkSurfaceProps.h"
#include "third_party/skia/include/gpu/ganesh/GrContextOptions.h"
#include "third_party/skia/include/gpu/ganesh/GrDirectContext.h"
#include "third_party/skia/include/gpu/ganesh/SkSurfaceGanesh.h"
#include "third_party/skia/include/gpu/ganesh/gl/GrGLAssembleInterface.h"
#include "third_party/skia/include/gpu/ganesh/gl/GrGLDirectContext.h"
#include "third_party/skia/include/gpu/ganesh/gl/GrGLInterface.h"
#include "ui/gfx/hdr_metadata.h"

#include "../../br.h"
#include "br_gpu.h"

/*
 * The hooks of the provider of a canvas's pixels (patch canvas_gpu.patch,
 * in canvas_2d_bitmap_provider.cc), set here once the raster is on the GPU
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

const unsigned kGlVendor = 0x1F00, kGlRenderer = 0x1F01, kGlVersion = 0x1F02;

/* The textures Ganesh keeps for what it drew before (images, glyphs, canvases) */
const size_t kCacheBytes = 96u << 20;

/* A canvas smaller than this many pixels stays on the processor: a texture and a flush cost more */
const long kCanvasMinPixels = 128L * 128L;

long now_us()
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000000L + t.tv_nsec / 1000;
}

/* ---------------------------------------------------------------- the GPU's thread */

struct Gpu {
	pthread_t			th;
	pthread_t			main;		/* the thread that brought it up (Blink's) */
	pthread_mutex_t			mu = PTHREAD_MUTEX_INITIALIZER;
	pthread_cond_t			cv = PTHREAD_COND_INITIALIZER;
	pthread_mutex_t			callers = PTHREAD_MUTEX_INITIALIZER;
	const std::function<void()>	*job = nullptr;	/* the job handed over, until it is done */
	std::vector<std::function<void()>> later;	/* what was let go of elsewhere */

	EGLDisplay			dpy = EGL_NO_DISPLAY;
	EGLContext			ctx = EGL_NO_CONTEXT;
	EGLSurface			pb = EGL_NO_SURFACE;
	sk_sp<GrDirectContext>		gr;
	sk_sp<SkSurface>		view;		/* the view's render target */
	std::string			renderer, version, why;
	bool				software = false;	/* the OpenGL ES draws on the processor */
	bool				up = false;		/* the thread runs and Ganesh is there */
	bool				on = false;		/* the page is rastered here */
	BrGpuStat			stat;
};

Gpu	g;

bool on_gpu()
{
	return g.up && pthread_equal(pthread_self(), g.th);
}

void *gpu_thread( void * )
{
	pthread_mutex_lock(&g.mu);
	for ( ;; ) {
		while ( g.job == nullptr && g.later.empty() ) pthread_cond_wait(&g.cv, &g.mu);
		std::vector<std::function<void()>> later;
		later.swap(g.later);
		const std::function<void()> *job = g.job;
		pthread_mutex_unlock(&g.mu);
		for ( std::function<void()> &f : later ) f();
		later.clear();
		if ( job != nullptr ) (*job)();
		pthread_mutex_lock(&g.mu);
		if ( job != nullptr ) {
			g.job = nullptr;
			pthread_cond_broadcast(&g.cv);
		}
	}
	return nullptr;
}

/* A job run on the GPU's thread, waited for (run in place when on it already) */
void run( const std::function<void()> &f )
{
	if ( on_gpu() ) {
		f();
		return;
	}
	pthread_mutex_lock(&g.callers);
	pthread_mutex_lock(&g.mu);
	g.job = &f;
	pthread_cond_broadcast(&g.cv);
	while ( g.job != nullptr ) pthread_cond_wait(&g.cv, &g.mu);
	pthread_mutex_unlock(&g.mu);
	pthread_mutex_unlock(&g.callers);
}

/* Something of the GPU's let go of later on its thread (in place when on it) */
void later( std::function<void()> f )
{
	if ( on_gpu() ) {
		f();
		return;
	}
	pthread_mutex_lock(&g.mu);
	g.later.push_back(std::move(f));
	pthread_cond_broadcast(&g.cv);
	pthread_mutex_unlock(&g.mu);
}

GrGLFuncPtr gl_proc( void *, const char name[] )
{
	return reinterpret_cast<GrGLFuncPtr>(eglGetProcAddress(name));
}

/*
 * On the GPU's thread: the display (the platform tessronos, the one ANGLE
 * also stands on), a context of OpenGL ES 3 (or 2) on a pbuffer it never
 * draws to (Ganesh draws into its own render targets), and Ganesh on it
 */
void bring_up()
{
	EGLint major = 0, minor = 0, n = 0;
	EGLConfig cfg = nullptr;

	g.dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if ( g.dpy == EGL_NO_DISPLAY || !eglInitialize(g.dpy, &major, &minor) ) {
		g.why = "the EGL display did not start";
		return;
	}
	eglBindAPI(EGL_OPENGL_ES_API);
	EGLint ca[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
			EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_NONE };
	int es = 3;
	if ( !eglChooseConfig(g.dpy, ca, &cfg, 1, &n) || n < 1 ) {
		ca[11] = EGL_OPENGL_ES2_BIT;
		es = 2;
		if ( !eglChooseConfig(g.dpy, ca, &cfg, 1, &n) || n < 1 ) {
			g.why = "no EGL config with a pbuffer";
			return;
		}
	}
	const EGLint xa[] = { EGL_CONTEXT_MAJOR_VERSION, es, EGL_NONE };
	g.ctx = eglCreateContext(g.dpy, cfg, EGL_NO_CONTEXT, xa);
	const EGLint sa[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
	g.pb = eglCreatePbufferSurface(g.dpy, cfg, sa);
	if ( g.ctx == EGL_NO_CONTEXT || g.pb == EGL_NO_SURFACE || !eglMakeCurrent(g.dpy, g.pb, g.pb, g.ctx) ) {
		char b[80];
		snprintf(b, sizeof(b), "no OpenGL ES %d context (EGL error 0x%x)", es, eglGetError());
		g.why = b;
		return;
	}
	typedef const unsigned char *( *GetString )( unsigned );
	GetString gs = reinterpret_cast<GetString>(eglGetProcAddress("glGetString"));
	if ( gs != nullptr ) {
		const char *r = reinterpret_cast<const char *>(gs(kGlRenderer));
		const char *v = reinterpret_cast<const char *>(gs(kGlVersion));
		const char *vd = reinterpret_cast<const char *>(gs(kGlVendor));
		g.renderer = r ? r : "?";
		g.version = v ? v : "?";
		if ( vd != nullptr && g.renderer.find(vd) == std::string::npos ) g.renderer += std::string(" (") + vd + ")";
	}
	g.software = g.renderer.find("softpipe") != std::string::npos
		  || g.renderer.find("llvmpipe") != std::string::npos
		  || g.renderer.find("oftware") != std::string::npos;

	sk_sp<const GrGLInterface> gl = GrGLMakeAssembledInterface(nullptr, gl_proc);
	if ( gl == nullptr ) {
		g.why = "Skia could not assemble the OpenGL ES functions";
		return;
	}
	GrContextOptions opt;
	g.gr = GrDirectContexts::MakeGL(std::move(gl), opt);
	if ( g.gr == nullptr ) {
		g.why = "Skia made no context on the OpenGL ES";
		return;
	}
	g.gr->setResourceCacheLimit(kCacheBytes);
}

/* ---------------------------------------------------------------- canvases (2D) */

/* A canvas's pixels on the GPU, made, used and let go of on the GPU's thread */
struct BrGpuCanvas {
	sk_sp<SkSurface>		surf;
	SkImageInfo			info;
	unsigned long			gen = 0;		/* changes: rasters and writes */
	unsigned long			snap_gen = ~0UL;	/* the change the last snapshot saw */
	cc::PaintImage::ContentId	content = cc::PaintImage::kInvalidContentId;
};

/*
 * A snapshot of a canvas: an image of the GPU's thread. Played back there
 * (the page's raster, another canvas's) it is the texture itself; asked
 * for its pixels on another thread it is read back, once.
 */
class BrGpuBacking final : public cc::TextureBacking {
public:
	BrGpuBacking( sk_sp<SkImage> image, const SkImageInfo &info ) : image_(std::move(image)), info_(info)
	{
		pthread_mutex_init(&mu_, nullptr);
	}
	~BrGpuBacking() override
	{
		sk_sp<SkImage> *img = new sk_sp<SkImage>(std::move(image_));
		later([img] { delete img; });
		pthread_mutex_destroy(&mu_);
	}

	const SkImageInfo &GetSkImageInfo() override { return info_; }
	gpu::Mailbox GetMailbox() const override { return gpu::Mailbox(); }
	void Bind( scoped_refptr<cc::TextureBackingContext> ) override {}
	void Unbind() override {}

	sk_sp<SkImage> GetSkImageViaReadback() override
	{
		if ( on_gpu() ) return image_;
		pthread_mutex_lock(&mu_);
		if ( raster_ == nullptr ) {
			run([this] { raster_ = image_->makeRasterImage(g.gr.get(), SkImage::kDisallow_CachingHint); });
		}
		sk_sp<SkImage> r = raster_;
		pthread_mutex_unlock(&mu_);
		return r;
	}

	bool readPixels( const SkImageInfo &dst_info, void *dst_pixels, size_t dst_row_bytes, int src_x,
			 int src_y ) override
	{
		bool ok = false;
		run([&] { ok = image_->readPixels(g.gr.get(), dst_info, dst_pixels, dst_row_bytes, src_x, src_y); });
		return ok;
	}

private:
	sk_sp<SkImage>	image_;		/* the GPU's thread's */
	SkImageInfo	info_;
	pthread_mutex_t	mu_;
	sk_sp<SkImage>	raster_;	/* its pixels read back, when asked for */
};

void *canvas_make( int width, int height, SkColorType ct, SkAlphaType at, sk_sp<SkColorSpace> cs,
		   const SkSurfaceProps &props )
{
	if ( !g.on || !pthread_equal(pthread_self(), g.main) ) return nullptr;
	if ( (long)width * height < kCanvasMinPixels ) return nullptr;
	if ( ct != kN32_SkColorType && ct != kRGBA_8888_SkColorType && ct != kBGRA_8888_SkColorType ) return nullptr;
	BrGpuCanvas *c = new BrGpuCanvas;
	c->info = SkImageInfo::Make(width, height, ct, at, std::move(cs));
	run([&] {
		c->surf = SkSurfaces::RenderTarget(g.gr.get(), skgpu::Budgeted::kYes, c->info, 0,
						   kTopLeft_GrSurfaceOrigin, &props);
	});
	if ( c->surf == nullptr ) {
		delete c;
		return nullptr;
	}
	return c;
}

void canvas_free( void *s )
{
	BrGpuCanvas *c = static_cast<BrGpuCanvas *>(s);
	later([c] { delete c; });
}

void canvas_raster( void *s, cc::PaintRecord record, cc::ImageProvider *images,
		    cc::PlaybackCallbacks::CustomDataRasterCallback callback )
{
	BrGpuCanvas *c = static_cast<BrGpuCanvas *>(s);
	run([&] {
		long t0 = now_us();
		cc::SkiaPaintCanvas pc(c->surf->getCanvas(), images);
		pc.drawPicture(std::move(record), callback);
		g.gr->flushAndSubmit(c->surf.get());
		c->gen++;
		g.stat.canvases++;
		g.stat.canvas_us += now_us() - t0;
	});
}

bool canvas_write( void *s, const SkImageInfo &info, const void *pixels, size_t row_bytes, int x, int y )
{
	BrGpuCanvas *c = static_cast<BrGpuCanvas *>(s);
	bool ok = false;
	run([&] {
		c->surf->writePixels(SkPixmap(info, pixels, row_bytes), x, y);
		c->gen++;
		ok = true;
	});
	return ok;
}

cc::PaintImage canvas_snapshot( void *s, cc::PaintImage::Id id, const gfx::HDRMetadata &hdr )
{
	BrGpuCanvas *c = static_cast<BrGpuCanvas *>(s);
	sk_sp<cc::TextureBacking> backing;
	run([&] {
		sk_sp<SkImage> img = c->surf->makeImageSnapshot();
		if ( img != nullptr ) backing = sk_make_sp<BrGpuBacking>(std::move(img), c->info);
	});
	if ( backing == nullptr ) return cc::PaintImage();
	/* a new content id only when the canvas changed, as the images the page caches go by it */
	if ( c->gen != c->snap_gen || c->content == cc::PaintImage::kInvalidContentId ) {
		c->content = cc::PaintImage::GetNextContentId();
		c->snap_gen = c->gen;
	}
	return cc::PaintImageBuilder::WithDefault()
		.set_id(id)
		.set_texture_backing(std::move(backing), c->content)
		.set_hdr_metadata(hdr)
		.TakePaintImage();
}

}  // namespace

bool br_gpu_inproc_start( int mode )
{
	static bool tried;

	if ( tried ) return g.on;
	tried = true;
	if ( mode == BR_GPU_OFF ) {
		printf("browser: gpu: off (gpu=off), the page is rastered on the processor\n");
		return false;
	}
	g.main = pthread_self();
	pthread_attr_t at;
	pthread_attr_init(&at);
	/* Mesa's GLSL compiler and Skia's SkSL recurse through a shader */
	pthread_attr_setstacksize(&at, 4u << 20);
	int rc = pthread_create(&g.th, &at, gpu_thread, nullptr);
	pthread_attr_destroy(&at);
	if ( rc != 0 ) {
		printf("browser: gpu: no thread (%d), the page is rastered on the processor\n", rc);
		return false;
	}
	g.up = true;
	run(bring_up);
	if ( g.gr == nullptr ) {
		printf("browser: gpu: none (%s), the page is rastered on the processor\n", g.why.c_str());
		return false;
	}
	if ( g.software && mode != BR_GPU_ON ) {
		printf("browser: gpu: %s draws on the processor, the page is rastered by Skia on the processor "
		       "(gpu=on rasters on it)\n", g.renderer.c_str());
		return false;
	}
	g.on = true;
	blink::g_canvas_2d_gpu_make = canvas_make;
	blink::g_canvas_2d_gpu_free = canvas_free;
	blink::g_canvas_2d_gpu_raster = canvas_raster;
	blink::g_canvas_2d_gpu_write = canvas_write;
	blink::g_canvas_2d_gpu_snapshot = canvas_snapshot;
	printf("browser: gpu: Skia's GPU raster (Ganesh) on %s, %s\n", g.renderer.c_str(), g.version.c_str());
	return true;
}

const char *br_gpu_inproc_renderer( void )
{
	return g.renderer.c_str();
}

bool br_gpu_inproc_raster( const cc::PaintRecord &rec, const std::vector<BrRect> &rects, int w, int h, void *px,
		    size_t pitch )
{
	bool ok = false;

	if ( !g.on || w <= 0 || h <= 0 ) return false;
	run([&] {
		long t0 = now_us(), t1, t2, t3;
		if ( g.view == nullptr || g.view->width() != w || g.view->height() != h ) {
			g.view = SkSurfaces::RenderTarget(g.gr.get(), skgpu::Budgeted::kNo, SkImageInfo::MakeN32Premul(w, h),
							  0, kTopLeft_GrSurfaceOrigin, nullptr);
			if ( g.view == nullptr ) return;
		}
		SkCanvas *c = g.view->getCanvas();
		for ( const BrRect &r : rects ) {
			c->save();
			c->clipRect(SkRect::MakeXYWH((float)r.x, (float)r.y, (float)r.w, (float)r.h));
			c->clear(SK_ColorWHITE);
			rec.Playback(c);
			c->restore();
		}
		t1 = now_us();
		g.gr->flushAndSubmit(g.view.get());
		t2 = now_us();
		ok = true;
		for ( const BrRect &r : rects ) {
			int x = r.x < 0 ? 0 : r.x, y = r.y < 0 ? 0 : r.y;
			int x1 = r.x + r.w > w ? w : r.x + r.w, y1 = r.y + r.h > h ? h : r.y + r.h;
			if ( x1 <= x || y1 <= y ) continue;
			SkPixmap dst(SkImageInfo::MakeN32Premul(x1 - x, y1 - y),
				     static_cast<char *>(px) + (size_t)y * pitch + (size_t)x * 4, pitch);
			if ( !g.view->readPixels(dst, x, y) ) ok = false;
			g.stat.read_px += (long)( x1 - x ) * ( y1 - y );
		}
		t3 = now_us();
		g.stat.views++;
		g.stat.play_us += t1 - t0;
		g.stat.flush_us += t2 - t1;
		g.stat.read_us += t3 - t2;
		if ( t3 - t0 > g.stat.max_us ) g.stat.max_us = t3 - t0;
	});
	return ok;
}

void br_gpu_inproc_stat( BrGpuStat *s )
{
	*s = g.stat;
}
