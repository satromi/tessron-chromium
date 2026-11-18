/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_cam.h
 *	ブラウザ: the cameras of the machine, through the camera device's
 *	objects (port/media/br_cam.cc). One camera streams at a time.
 */

#ifndef BR_CAM_H
#define BR_CAM_H

#ifdef __cplusplus
extern "C" {
#endif

/* The formats a camera gives its frames in (the camera device's CAMFMT_*) */
#define BR_CAM_YUY2	1
#define BR_CAM_NV12	2
#define BR_CAM_MJPEG	3
#define BR_CAM_UYVY	4

#define BR_CAM_MAX	4
#define BR_CAM_MODES	64

typedef struct {
	int		format;		/* BR_CAM_* */
	int		width, height;
	unsigned	interval;	/* the frame interval it takes by default, 100 ns */
	unsigned	fastest;	/* the shortest it takes, 100 ns */
} BrCamMode;

typedef struct {
	unsigned	seq;
	int		format;
	int		width, height;
	unsigned	bytes;
	unsigned	msec;
} BrCamFrame;

/*
 * The cameras there are: their names (UTF-8) into names[i], and the
 * UUIDs of their units' objects (UUIDv7, kept by the system for each
 * unit) into ids[i] when ids is not NULL; how many
 */
int br_cam_list( char (*names)[64], unsigned char (*ids)[16], int max );

/* A camera's modes, how many */
int br_cam_modes( int cam, BrCamMode *modes, int max );

/* The camera streams in the mode: 0, or -1 */
int br_cam_open( int cam, int mode, unsigned interval );

/* The newest frame not had yet, never waiting: its bytes, 0 none, -1 the camera is gone */
int br_cam_read( unsigned char *buf, int max, BrCamFrame *hdr );

/* The largest frame of the mode streaming */
int br_cam_frame_max( void );

void br_cam_close( void );

#ifdef __cplusplus
}
#endif

#endif /* BR_CAM_H */
