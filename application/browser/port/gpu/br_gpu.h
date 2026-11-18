/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_gpu.h
 *	ブラウザ: Skia's GPU raster on the system's OpenGL ES (design 14.7.2;
 *	make GL=1).
 *
 *	The raster is the GPU process's for a renderer (br_gpu_client.cc,
 *	br_gpu_main.cc), and that of a GPU thread of the browser's process with
 *	the pages there (br_gpu.cc). They are linked only with the system's
 *	OpenGL ES (make GL=1): the names are weak, and a program without it
 *	finds them null and draws on the processor.
 */

#ifndef __BR_GPU_H__
#define __BR_GPU_H__

#include <stddef.h>
#include <functional>
#include <vector>

struct BrRect;

namespace cc {
class PaintRecord;
}

/* How br_gpu_start is asked: on a GPU only, on whatever OpenGL ES there is, or not at all */
enum { BR_GPU_AUTO = 0, BR_GPU_ON = 1, BR_GPU_OFF = -1 };

/*
 * The GPU's thread and its context brought up, once: true when the
 * page is to be rastered on the GPU. BR_GPU_AUTO takes it only when the
 * OpenGL ES draws on a GPU (not softpipe, a renderer on the processor);
 * BR_GPU_ON takes any. What was chosen and why goes to the console.
 */
bool		br_gpu_start( int mode ) __attribute__((weak));

/* The OpenGL ES the raster runs on ("" before br_gpu_start, or without it) */
const char	*br_gpu_renderer( void ) __attribute__((weak));

/*
 * The rectangles of a view 'w' x 'h' rastered on the GPU from 'rec' (each
 * cleared to white, then the record played back clipped to it), and read
 * back into the pixels at 'px' (N32, 'pitch' bytes a row) at the same
 * places. The GPU process's raster may write pixels between the
 * rectangles too, with what they already hold there. False when the GPU
 * could not: the rectangles are then painted on the processor.
 */
bool		br_gpu_raster( const cc::PaintRecord &rec, const std::vector<BrRect> &rects, int w, int h,
			       void *px, size_t pitch ) __attribute__((weak));

/* What the raster has taken since the start, in microseconds, for the console */
struct BrGpuStat {
	long	views = 0;		/* br_gpu_raster calls that drew */
	long	play_us = 0;		/* the records played back into the GPU's commands */
	long	flush_us = 0;		/* the commands given to the driver */
	long	read_us = 0;		/* the rectangles read back, the GPU's work waited for */
	long	max_us = 0;		/* the longest view */
	long	read_px = 0;		/* pixels read back */
	long	canvases = 0;		/* canvases of the page rastered on the GPU (2D) */
	long	canvas_us = 0;		/* their records played and flushed */
	long	trips = 0;		/* calls that waited for an answer of the GPU process */
	long	proc = 0;		/* 1: the raster is the GPU process's (the counts above are the renderer's side) */
	long	trip_us = 0;		/* a round trip to the GPU process, as timed once (0 before) */
	long	gs_views = 0;		/* of the views, those rastered into the window's GPU buffers (not read back) */
	long	gs_take_us = 0;		/* from the end of each of those to the GPU process having taken it in */
};

void		br_gpu_stat( BrGpuStat *s ) __attribute__((weak));

/*
 * The window's GPU buffers (design 14.13.10), in a renderer. The views
 * are rastered into one of two shared images of the GPU process
 * 'gpu_pid', named by their mailboxes (16 bytes), instead of the view's
 * own image read back into the page's memory:
 *
 * br_gpu_gsurf_able: whether they can be, now (the page is rastered in
 * the GPU process, on a channel to that very process).
 * br_gpu_gsurf_aim: the next br_gpu_raster goes into the image 'mailbox'
 * (null: back to the readback); its 'px' is not written.
 * br_gpu_gsurf_after: 'done' called on the main thread once the GPU
 * process has taken in what was rastered so far (true), or the channel
 * was lost first (false).
 * br_gpu_gsurf_read: an image read back into 'px' ('pitch' bytes a row),
 * for the comparison with the processor ("gpucheck=").
 */
bool		br_gpu_gsurf_able( int gpu_pid ) __attribute__((weak));
void		br_gpu_gsurf_aim( const unsigned char *mailbox ) __attribute__((weak));
void		br_gpu_gsurf_after( std::function<void( bool )> done ) __attribute__((weak));
bool		br_gpu_gsurf_read( const unsigned char *mailbox, int w, int h, void *px, size_t pitch )
		__attribute__((weak));

/*
 * The same raster on a GPU thread of the process of the pages (br_gpu.cc),
 * which br_gpu_client.cc's names above take with the pages in the
 * browser's process (process=single)
 */
bool		br_gpu_inproc_start( int mode ) __attribute__((weak));
const char	*br_gpu_inproc_renderer( void ) __attribute__((weak));
bool		br_gpu_inproc_raster( const cc::PaintRecord &rec, const std::vector<BrRect> &rects, int w, int h,
				      void *px, size_t pitch ) __attribute__((weak));
void		br_gpu_inproc_stat( BrGpuStat *s ) __attribute__((weak));

/* The GPU process's life (br_gpu_main.cc): it never returns */
void		br_gpu_main( void ) __attribute__((weak));

#endif /* __BR_GPU_H__ */
