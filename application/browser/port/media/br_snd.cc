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
 *
 *	The players keep their clocks by what the ring and the device hold
 *	(br_snd_buffered): a ring that is not drained stops their time, and
 *	with it the picture. So when there is no sound device, or the device
 *	holds samples and its count has not moved for kStallMs (its hardware
 *	takes nothing), the feeder drains the ring itself at 48 kHz by the
 *	clock and lets the frames go unheard. It looks at the count still,
 *	and gives the device the ring again once the count moves.
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
#include <time.h>
#include <unistd.h>

namespace {

constexpr int kDevBufsz = 96000;		/* the channel's buffer: half a second */
constexpr int kPlayPrime = 2400 * 4;		/* bytes queued before playing starts */
constexpr int kRingFrames = 48000;		/* the ring: a second of stereo frames */
constexpr int kFeedBlock = 4800;		/* frames given to the device at a time */
constexpr int kDeviceLead = 7200;		/* frames the device may hold ahead */
constexpr int kRate = 48000;
constexpr long kStallMs = 400;			/* the device's count still this long while it holds samples */
constexpr long kStopLookMs = 100;		/* how often a stalled channel's state is read */

short		g_ring[kRingFrames * 2];
volatile int	g_rd, g_wr;			/* frame indices, wrapping at kRingFrames */
volatile int	g_run, g_stopped = 1;
volatile ID	g_key = 0;			/* the sound device, opened */
long		g_written;			/* bytes written since the start */
bool		g_playing;			/* PCM_PLAY given and not stopped */
volatile int	g_dev_frames;			/* frames the device held at the last look */
volatile bool	g_feeder_up;
long		g_played;			/* frames the device took since the program began */
volatile long	g_dropped;			/* frames drained unheard since the program began */
volatile int	g_mode;				/* BR_SND_* below */
volatile long	g_stalls;			/* times the device was found taking nothing */
long		g_restarts;			/* times a stalled channel was found stopped and written afresh */

enum { BR_SND_DEVICE = 0, BR_SND_NODEVICE = 1, BR_SND_STALLED = 2 };

long now_ms()
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

long now_us()
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

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

/*
 * Frames of the ring let go unheard at 48 kHz by the clock since the
 * last call; *last_us is where the clock stood. An empty ring keeps no
 * debt: the frames that come later are let go from when they came.
 */
void drain_by_clock( long *last_us )
{
	long now = now_us();
	if ( *last_us == 0 ) *last_us = now;
	long due = ( now - *last_us ) * kRate / 1000000;
	if ( due <= 0 ) return;
	int avail = ring_count();
	int n = due > avail ? avail : (int)due;
	int rd = g_rd + n;
	if ( rd >= kRingFrames ) rd -= kRingFrames;
	g_rd = rd;
	g_dropped += n;
	*last_us = ( n < due ) ? now : *last_us + (long)n * 1000000 / kRate;
}

void *feed_thread( void * )
{
	static short chunk[kFeedBlock * 2];
	UW	last_cnt = 0xFFFFFFFF;		/* the device's count at the last look */
	long	moved_at = now_ms();		/* when it last moved, or nothing was expected of it */
	long	clock_us = 0;			/* where the drain by the clock stands */
	long	stop_look_at = 0;		/* when the channel's state was last read while stalled */

	for ( ;; ) {
		if ( !g_run ) {
			/* asked to stop: the device stopped once, and the next start waited for */
			if ( !g_stopped ) {
				if ( g_key > 0 ) write_word(OB_SND_CTL, PCM_STOP);
				g_playing = false;
				g_written = 0;
				g_stopped = 1;
			}
			clock_us = 0;
			usleep(20000);
			continue;
		}
		if ( g_key <= 0 ) {
			/* no device: the ring drained by the clock */
			g_mode = BR_SND_NODEVICE;
			g_dev_frames = 0;
			drain_by_clock(&clock_us);
			usleep(5000);
			continue;
		}

		/* what the device still holds, and whether its count moves */
		int q;
		if ( g_playing ) {
			UW cnt = read_word(OB_SND_CNT);
			q = ( cnt == 0xFFFFFFFF ) ? -1 : (int)( ( g_written - (long)cnt ) / 4 );
			if ( cnt != last_cnt || q <= 0 ) {
				if ( g_mode == BR_SND_STALLED ) {
					printf("browser: snd: the sound device takes samples again; %ld frames went unheard\n",
					       (long)g_dropped);
				}
				if ( g_mode != BR_SND_DEVICE ) clock_us = 0;
				g_mode = BR_SND_DEVICE;
				last_cnt = cnt;
				moved_at = now_ms();
			}
		} else {
			q = (int)( g_written / 4 );
			last_cnt = 0xFFFFFFFF;
			moved_at = now_ms();
			if ( g_mode == BR_SND_NODEVICE ) g_mode = BR_SND_DEVICE;
		}
		if ( g_mode == BR_SND_DEVICE && g_playing && now_ms() - moved_at >= kStallMs ) {
			g_mode = BR_SND_STALLED;
			g_stalls++;
			printf("browser: snd: the sound device has taken nothing for %ld ms (it holds %d frames, "
			       "%ld taken); the ring is drained by the clock, unheard\n",
			       now_ms() - moved_at, q, g_played);
		}
		if ( g_mode == BR_SND_STALLED ) {
			/*
			 * A channel the device has stopped (its lower layer gone,
			 * or the samples run out under it) is written afresh: the
			 * counts start again from nothing, and the next write that
			 * fills the priming amount starts it. Looked at every
			 * kStopLookMs while the ring is drained by the clock.
			 */
			long t = now_ms();
			if ( t - stop_look_at >= kStopLookMs ) {
				stop_look_at = t;
				if ( read_word(OB_SND_CTL) == PCM_STOP ) {
					/* told the first few times: a channel that keeps stopping would fill the console */
					if ( ++g_restarts <= 5 ) {
						printf("browser: snd: the device's channel has stopped; written afresh "
						       "(%ld frames went unheard)\n", (long)g_dropped);
					}
					g_playing = false;
					g_written = 0;
					g_mode = BR_SND_DEVICE;
					last_cnt = 0xFFFFFFFF;
					moved_at = t;
					clock_us = 0;
					continue;
				}
			}
			g_dev_frames = 0;
			drain_by_clock(&clock_us);
			usleep(5000);
			continue;
		}

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
		g_played += a / 4;
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

/* The sound device opened, or false */
bool open_device()
{
	TS_UUID	dev;
	INT	cnt = 0;

	if ( ob_lst_obj(OB_T_DEVICE, OB_S_SOUND, NULL, &dev, 1, &cnt) < E_OK || cnt < 1 ) {
		printf("browser: snd: no sound device; the media's sound goes unheard, by the clock\n");
		return false;
	}
	ID key = ob_opn_obj(&dev, OB_OP_R | OB_OP_WRITE);
	if ( key <= 0 ) {
		printf("browser: snd: the sound device cannot be opened (%d); the sound goes unheard\n", (int)key);
		return false;
	}
	g_key = key;
	write_word(OB_SND_CTL, PCM_STOP);
	setup();
	g_playing = false;
	g_written = 0;
	printf("browser: snd: open (48 kHz, 16 bits, stereo)\n");
	return true;
}

}  // namespace

/*
 * The feeder running, and the device opened when there is one: 0, or -1
 * with no device (the ring is then drained by the clock, unheard)
 */
extern "C" int br_snd_start( void )
{
	if ( g_key > 0 && g_feeder_up ) {
		g_run = 1;
		g_stopped = 0;
		return 0;
	}
	if ( !g_feeder_up ) g_rd = g_wr = 0;
	bool ok = g_key > 0 || open_device();
	if ( ensure_feeder() != 0 ) return -1;
	/* running first, then the stopped mark cleared: the feeder acts on the mark only while not asked to run */
	g_run = 1;
	g_stopped = 0;
	return ok ? 0 : -1;
}

/* Frames queued, as many as fit; never waits */
extern "C" int br_snd_write( const short *frames, int nframes )
{
	if ( !g_feeder_up ) return 0;
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

/* Frames the device has taken since the program began, from the media elements and Web Audio */
extern "C" long br_snd_frames( void )
{
	return g_played;
}

/*
 * For the media's report: frames the device took and those let go
 * unheard, what the ring and the device hold, and how the ring is
 * drained ("device", "no device", "stalled"), with the stalls so far
 */
extern "C" void br_snd_state( long *taken, long *dropped, int *ring, int *device, const char **how, long *stalls )
{
	*taken = g_played;
	*dropped = g_dropped;
	*ring = ring_count();
	*device = g_dev_frames;
	*how = g_mode == BR_SND_NODEVICE ? "no device" : g_mode == BR_SND_STALLED ? "stalled" : "device";
	*stalls = g_stalls;
}

/* ---------------------------------------------------------------- the microphone */

/*
 * Recording goes through another unit of the sound device than playback
 * (a unit is a channel of its own: "snda0" while "snda" plays), so that
 * a page records while the media sound: the second of the device's
 * objects, or the first when it has one only. 48 kHz, 16 bits, stereo.
 */
namespace {

ID		g_mic_key = 0;
pthread_mutex_t	g_mic_lock = PTHREAD_MUTEX_INITIALIZER;

/* The device's objects, the one to record with */
bool mic_object( TS_UUID *dev )
{
	TS_UUID	list[8];
	INT	cnt = 0;

	if ( ob_lst_obj(OB_T_DEVICE, OB_S_SOUND, NULL, list, 8, &cnt) < E_OK || cnt < 1 ) return false;
	*dev = list[cnt > 1 ? 1 : 0];
	return true;
}

}  // namespace

/* Whether the machine has a sound device to play on (the default audio output) */
extern "C" int br_snd_available( void )
{
	TS_UUID	dev;
	INT	cnt = 0;

	return ob_lst_obj(OB_T_DEVICE, OB_S_SOUND, NULL, &dev, 1, &cnt) >= E_OK && cnt >= 1;
}

/* Whether the machine records at all: the device says it records at 48 kHz */
extern "C" int br_mic_available( void )
{
	TS_UUID		dev;
	SDPcmInfo	info;
	SZ		a = 0;

	if ( !mic_object(&dev) ) return 0;
	ID key = ob_opn_obj(&dev, OB_OP_R);
	if ( key <= 0 ) return 0;
	memset(&info, 0, sizeof(info));
	ER er = ob_rea_rec(key, OB_SND_INFO, 0, &info, sizeof(info), &a);
	ob_cls_obj(key);
	return er >= E_OK && ( info.spt[PCMST48K].srecord || info.spt[PCMST48K].mrecord );
}

/* Recording started: 0, or -1 when the device cannot record */
extern "C" int br_mic_open( void )
{
	TS_UUID		dev;
	SDPcmMode	m;
	SZ		a = 0;
	UW		v;

	pthread_mutex_lock(&g_mic_lock);
	if ( g_mic_key > 0 ) {
		pthread_mutex_unlock(&g_mic_lock);
		return 0;
	}
	ID key = mic_object(&dev) ? ob_opn_obj(&dev, OB_OP_R | OB_OP_WRITE) : -1;
	if ( key <= 0 ) {
		pthread_mutex_unlock(&g_mic_lock);
		printf("browser: mic: the sound device cannot be opened\n");
		return -1;
	}
	v = PCM_STOP;
	(void)ob_wri_rec(key, OB_SND_CTL, 0, &v, sizeof(v), &a);
	memset(&m, 0, sizeof(m));
	m.sampling = PCMST48K;
	m.datafmt = PCMFmt16;
	m.stereo = 1;
	(void)ob_wri_rec(key, OB_SND_MODE, 0, &m, sizeof(m), &a);
	v = kDevBufsz;
	(void)ob_wri_rec(key, OB_SND_BUFSZ, 0, &v, sizeof(v), &a);
	v = PCM_COUNT_NONE;
	(void)ob_wri_rec(key, OB_SND_ENDCNT, 0, &v, sizeof(v), &a);
	v = PCM_REC;
	ER er = ob_wri_rec(key, OB_SND_CTL, 0, &v, sizeof(v), &a);
	if ( er < E_OK ) {
		ob_cls_obj(key);
		pthread_mutex_unlock(&g_mic_lock);
		printf("browser: mic: the sound device does not record (%d)\n", (int)er);
		return -1;
	}
	g_mic_key = key;
	pthread_mutex_unlock(&g_mic_lock);
	printf("browser: mic: recording (48 kHz, 16 bits, stereo)\n");
	return 0;
}

/* Frames recorded since the last read, up to max; never waits */
extern "C" int br_mic_read( short *frames, int max )
{
	SZ a = 0;

	pthread_mutex_lock(&g_mic_lock);
	ER er = g_mic_key > 0 ? ob_rea_rec(g_mic_key, OB_SND_PCM, 0, frames, (SZ)max * 4, &a) : E_OBJ;
	pthread_mutex_unlock(&g_mic_lock);
	if ( er < E_OK ) return -1;
	return (int)( a / 4 );
}

/*
 * What is played, taken from the sound device's monitor (a share's sound).
 * A process has one channel for each unit: the first is the playing's, the
 * second the microphone's, and each monitor has one of the others. The
 * answer is a key, or less than 0 when none can be had.
 */
constexpr int	kMonUnits = 3;
static ID		g_mon_key[kMonUnits];
static pthread_mutex_t	g_mon_lock = PTHREAD_MUTEX_INITIALIZER;

extern "C" int br_mon_open( int mode, int pid )
{
	TS_UUID		list[8];
	INT		cnt = 0;
	SDPcmMode	m;
	SDMon		mo;
	SZ		a = 0;
	UW		v;
	int		u;

	if ( ob_lst_obj(OB_T_DEVICE, OB_S_SOUND, NULL, list, 8, &cnt) < E_OK ) return -1;
	pthread_mutex_lock(&g_mon_lock);
	for ( u = 0; u < kMonUnits && ( g_mon_key[u] > 0 || u + 2 >= cnt ); u++ ) ;
	if ( u >= kMonUnits ) {
		pthread_mutex_unlock(&g_mon_lock);
		printf("browser: share: no channel of the sound device left for its sound\n");
		return -1;
	}
	ID key = ob_opn_obj(&list[u + 2], OB_OP_R | OB_OP_WRITE);
	if ( key <= 0 ) {
		pthread_mutex_unlock(&g_mon_lock);
		return -1;
	}
	v = PCM_STOP;
	(void)ob_wri_rec(key, OB_SND_CTL, 0, &v, sizeof(v), &a);
	memset(&m, 0, sizeof(m));
	m.sampling = PCMST48K;
	m.datafmt = PCMFmt16;
	m.stereo = 1;
	(void)ob_wri_rec(key, OB_SND_MODE, 0, &m, sizeof(m), &a);
	v = kDevBufsz;
	(void)ob_wri_rec(key, OB_SND_BUFSZ, 0, &v, sizeof(v), &a);
	v = PCM_COUNT_NONE;
	(void)ob_wri_rec(key, OB_SND_ENDCNT, 0, &v, sizeof(v), &a);
	mo.mode = (UINT)mode;
	mo.pid = pid;
	ER er = ob_wri_rec(key, OB_SND_MONITOR, 0, &mo, sizeof(mo), &a);
	v = PCM_REC;
	if ( er >= E_OK ) er = ob_wri_rec(key, OB_SND_CTL, 0, &v, sizeof(v), &a);
	if ( er < E_OK ) {
		ob_cls_obj(key);
		pthread_mutex_unlock(&g_mon_lock);
		printf("browser: share: the sound device gives no monitor (%d)\n", (int)er);
		return -1;
	}
	g_mon_key[u] = key;
	pthread_mutex_unlock(&g_mon_lock);
	printf("browser: share: the sound is taken (mode %d, process %d)\n", mode, pid);
	return (int)key;
}

/* Frames taken since the last read, up to max; never waits */
extern "C" int br_mon_read( int key, short *frames, int max )
{
	SZ a = 0;
	ER er = ob_rea_rec((ID)key, OB_SND_PCM, 0, frames, (SZ)max * 4, &a);
	return er < E_OK ? -1 : (int)( a / 4 );
}

extern "C" void br_mon_close( int key )
{
	SZ a = 0;
	UW v = PCM_STOP;

	pthread_mutex_lock(&g_mon_lock);
	for ( int u = 0; u < kMonUnits; u++ ) {
		if ( g_mon_key[u] == (ID)key ) g_mon_key[u] = 0;
	}
	pthread_mutex_unlock(&g_mon_lock);
	(void)ob_wri_rec((ID)key, OB_SND_CTL, 0, &v, sizeof(v), &a);
	ob_cls_obj((ID)key);
}

extern "C" void br_mic_close( void )
{
	SZ a = 0;
	UW v = PCM_STOP;

	pthread_mutex_lock(&g_mic_lock);
	if ( g_mic_key > 0 ) {
		(void)ob_wri_rec(g_mic_key, OB_SND_CTL, 0, &v, sizeof(v), &a);
		ob_cls_obj(g_mic_key);
		g_mic_key = 0;
		printf("browser: mic: stopped\n");
	}
	pthread_mutex_unlock(&g_mic_lock);
}
