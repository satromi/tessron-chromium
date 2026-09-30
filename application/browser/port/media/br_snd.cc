/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_snd.cc
 *	ブラウザ: the sound of the media elements (design 17.19): a ring of
 *	frames the players fill, drained by a feeder thread into a channel of
 *	the sound device (the device object, design 18.7).
 *
 *	The channel's buffer is a FIFO of the program's samples: a write
 *	queues what fits, PCM_PLAY starts it, and a channel that runs short
 *	of samples for a while stops by itself; the next write starts it
 *	again. The count of bytes played since the start (OB_SND_CNT) against
 *	what was written tells what the channel still holds.
 *
 *	The feeder is the only one that writes PCM and the only one that
 *	starts or stops the channel, so the counts it keeps agree with the
 *	device's. The format is fixed: 48 kHz, 16-bit, stereo, interleaved;
 *	the decoder resamples into it.
 */

#include TS_CONFIG_H
#include <stdint.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/uapp.h>
#include <ts/snd.h>

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

namespace {

constexpr int kDevBufsz = 96000;		/* the channel's buffer: half a second */
constexpr int kPlayPrime = 2400 * 4;		/* bytes queued before playing starts */
constexpr int kRingFrames = 48000;		/* the ring: a second of stereo frames */
constexpr int kFeedBlock = 4800;		/* frames given to the device at a time */
constexpr int kDeviceLead = 7200;		/* frames the device may hold ahead */

short		g_ring[kRingFrames * 2];
volatile int	g_rd, g_wr;			/* frame indices, wrapping at kRingFrames */
volatile int	g_run, g_stopped = 1;
ID		g_key = 0;			/* the sound device, opened */
long		g_written;			/* bytes written since the start */
bool		g_playing;			/* PCM_PLAY given and not stopped */
volatile int	g_dev_frames;			/* frames the device held at the last look */
volatile bool	g_feeder_up;

int ring_count()
{
	int d = g_wr - g_rd;
	return d < 0 ? d + kRingFrames : d;
}

UW read_word( INT rec )
{
	UW v = 0;
	SZ a = 0;
	if ( ob_rea_rec(g_key, rec, 0, &v, sizeof(v), &a) < E_OK || a < (SZ)sizeof(v) ) return 0xFFFFFFFF;
	return v;
}

void write_word( INT rec, UW v )
{
	SZ a = 0;
	(void)ob_wri_rec(g_key, rec, 0, &v, sizeof(v), &a);
}

/* 48 kHz, 16 bits, stereo, a half-second buffer and no stop count */
void setup()
{
	SDPcmMode m;
	SZ a = 0;

	memset(&m, 0, sizeof(m));
	m.sampling = PCMST48K;
	m.datafmt = PCMFmt16;
	m.stereo = 1;
	(void)ob_wri_rec(g_key, OB_SND_MODE, 0, &m, sizeof(m), &a);
	write_word(OB_SND_BUFSZ, kDevBufsz);
	write_word(OB_SND_ENDCNT, PCM_COUNT_NONE);
}

/* Frames written to the device and not yet played; -1 when it will not say */
int device_queued()
{
	if ( !g_playing ) return (int)( g_written / 4 );
	UW cnt = read_word(OB_SND_CNT);
	if ( cnt == 0xFFFFFFFF ) return -1;
	return (int)( ( g_written - (long)cnt ) / 4 );
}

/* Bytes the device took, or an error; starts playing once kPlayPrime bytes are in, and after an underrun */
long play_write( const void *p, SZ n )
{
	SZ a = 0;

	if ( g_playing && read_word(OB_SND_CTL) == PCM_STOP ) {
		g_playing = false;		/* stopped by the device: it ran short */
		g_written = 0;
	}
	ER er = ob_wri_rec(g_key, OB_SND_PCM, 0, p, n, &a);
	if ( er < E_OK ) return er;
	g_written += a;
	if ( !g_playing && g_written >= kPlayPrime ) {
		write_word(OB_SND_CTL, PCM_PLAY);
		g_playing = true;
	}
	return a;
}

void *feed_thread( void * )
{
	static short chunk[kFeedBlock * 2];

	for ( ;; ) {
		if ( !g_run ) {
			/* asked to stop: the device stopped once, and the next start waited for */
			if ( !g_stopped ) {
				write_word(OB_SND_CTL, PCM_STOP);
				g_playing = false;
				g_written = 0;
				g_stopped = 1;
			}
			usleep(20000);
			continue;
		}
		int q = device_queued();
		g_dev_frames = q > 0 ? q : 0;
		int avail = ring_count();
		if ( avail <= 0 ) {
			usleep(10000);
			continue;
		}
		int n = avail > kFeedBlock ? kFeedBlock : avail;
		if ( q >= 0 ) {
			if ( q >= kDeviceLead ) {
				usleep(10000);
				continue;
			}
			if ( n > kDeviceLead - q ) n = kDeviceLead - q;
		}
		int rd = g_rd;
		for ( int i = 0; i < n; i++ ) {
			int idx = rd + i;
			if ( idx >= kRingFrames ) idx -= kRingFrames;
			chunk[i * 2] = g_ring[idx * 2];
			chunk[i * 2 + 1] = g_ring[idx * 2 + 1];
		}
		long a = play_write(chunk, (SZ)n * 4);
		if ( a < 0 ) {
			usleep(20000);
			continue;
		}
		rd += (int)( a / 4 );
		if ( rd >= kRingFrames ) rd -= kRingFrames;
		g_rd = rd;
		usleep(( a < n * 4 ) ? 8000 : 1000);
	}
	return nullptr;
}

int ensure_feeder()
{
	if ( g_feeder_up ) return 0;
	pthread_t th;
	pthread_attr_t at;
	pthread_attr_init(&at);
	pthread_attr_setstacksize(&at, 256 * 1024);
	pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	int rc = pthread_create(&th, &at, feed_thread, nullptr);
	pthread_attr_destroy(&at);
	if ( rc != 0 ) {
		printf("browser: snd: no feeder (%d)\n", rc);
		return -1;
	}
	g_feeder_up = true;
	return 0;
}

}  // namespace

/* The device opened and the feeder running: 0, or -1 with no sound */
extern "C" int br_snd_start( void )
{
	TS_UUID	dev;
	INT	cnt = 0;

	if ( g_key > 0 ) return 0;
	if ( ob_lst_obj(OB_T_DEVICE, OB_S_SOUND, NULL, &dev, 1, &cnt) < E_OK || cnt < 1 ) {
		printf("browser: snd: no sound device\n");
		return -1;
	}
	g_key = ob_opn_obj(&dev, OB_OP_R | OB_OP_WRITE);
	if ( g_key <= 0 ) {
		printf("browser: snd: the sound device cannot be opened (%d)\n", (int)g_key);
		g_key = 0;
		return -1;
	}
	write_word(OB_SND_CTL, PCM_STOP);
	setup();
	g_playing = false;
	g_written = 0;
	g_rd = g_wr = 0;
	if ( ensure_feeder() != 0 ) {
		ob_cls_obj(g_key);
		g_key = 0;
		return -1;
	}
	/* running first, then the stopped mark cleared: the feeder acts on the mark only while not asked to run */
	g_run = 1;
	g_stopped = 0;
	printf("browser: snd: open (48 kHz, 16 bits, stereo)\n");
	return 0;
}

/* Frames queued, as many as fit; never waits */
extern "C" int br_snd_write( const short *frames, int nframes )
{
	if ( g_key <= 0 ) return 0;
	int free_frames = kRingFrames - 1 - ring_count();
	int n = nframes > free_frames ? free_frames : nframes;
	int wr = g_wr;
	for ( int i = 0; i < n; i++ ) {
		int idx = wr + i;
		if ( idx >= kRingFrames ) idx -= kRingFrames;
		g_ring[idx * 2] = frames[i * 2];
		g_ring[idx * 2 + 1] = frames[i * 2 + 1];
	}
	wr += n;
	if ( wr >= kRingFrames ) wr -= kRingFrames;
	g_wr = wr;
	return n;
}

/* Frames waiting in the ring and in the device */
extern "C" int br_snd_buffered( void )
{
	return ring_count() + g_dev_frames;
}

/* How long what is written now waits before it is heard, in milliseconds */
extern "C" int br_snd_latency_ms( void )
{
	return br_snd_buffered() / 48;
}

extern "C" void br_snd_stop( void )
{
	g_run = 0;
}
