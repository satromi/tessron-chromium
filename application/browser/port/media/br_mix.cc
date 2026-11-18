/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_mix.cc
 *	ブラウザ: the mixing of the browser's sounds (design 14.6): every
 *	source of sound in the process (a media element's pipeline, a
 *	MediaStream's track, the sound of WebRTC's peers, an AudioContext,
 *	the speech synthesizer) is a stream of its own here, and one thread
 *	sums them into the sound ring of br_snd.cc, its only writer.
 *
 *	The ring is one channel of the sound device and plays what is written
 *	to it in order, so two sources written to it straight would follow
 *	each other a buffer at a time instead of being heard together.
 *
 *	A stream is a queue of 48 kHz, 16-bit stereo frames. Its writer keeps
 *	it filled as it did the ring, by what is still to be heard of it
 *	(br_mix_buffered: its queue and the ring). The mixer moves a block of
 *	10 ms whenever the ring holds less than kRingAhead: a stream adds what
 *	it holds of the block, and one that holds part of a block is waited
 *	for while the ring has more than kRingLow still to play, so that a
 *	writer a little late is not heard as a gap. A stream that holds
 *	nothing (paused, ended, or not begun) adds silence. When no stream
 *	holds anything, nothing is written, and the ring runs out and stops
 *	the channel as before.
 */

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>

/* The ring the sound device is fed from (br_snd.cc) */
extern "C" int br_snd_start( void );
extern "C" int br_snd_write( const short *frames, int nframes );
extern "C" int br_snd_buffered( void );

namespace {

constexpr int kBlock = 480;		/* frames mixed at a time: 10 ms */
constexpr int kRingAhead = 2400;	/* the ring is filled to 50 ms */
constexpr int kRingLow = 960;		/* below 20 ms in the ring a late stream is not waited for */
constexpr int kQueueMax = 48000;	/* a stream's queue: a second */

struct Stream {
	std::deque<short>	frames;		/* interleaved, two samples a frame */
};

std::mutex				g_lock;
std::map<int, std::unique_ptr<Stream>>	g_streams;
int					g_next = 1;
bool					g_running;
std::atomic<long>			g_mixed{0};	/* frames written to the ring */

short Clip( int v )
{
	return (short)( v > 32767 ? 32767 : v < -32768 ? -32768 : v );
}

void *Mixer( void * )
{
	int sum[kBlock * 2];
	short out[kBlock * 2];
	for ( ;; ) {
		if ( br_snd_buffered() >= kRingAhead ) {
			usleep(2000);
			continue;
		}
		bool any = false, partial = false;
		{
			std::lock_guard<std::mutex> hold(g_lock);
			for ( auto &s : g_streams ) {
				size_t have = s.second->frames.size() / 2;
				if ( have > 0 ) any = true;
				if ( have > 0 && have < (size_t)kBlock ) partial = true;
			}
		}
		if ( !any ) {
			usleep(5000);
			continue;
		}
		if ( partial && br_snd_buffered() > kRingLow ) {
			usleep(2000);
			continue;
		}
		memset(sum, 0, sizeof(sum));
		{
			std::lock_guard<std::mutex> hold(g_lock);
			for ( auto &s : g_streams ) {
				std::deque<short> &q = s.second->frames;
				size_t n = std::min(q.size(), (size_t)kBlock * 2);
				for ( size_t i = 0; i < n; i++ ) sum[i] += q[i];
				q.erase(q.begin(), q.begin() + n);
			}
		}
		for ( int i = 0; i < kBlock * 2; i++ ) out[i] = Clip(sum[i]);
		int done = 0;
		while ( done < kBlock ) {
			int n = br_snd_write(out + done * 2, kBlock - done);
			if ( n <= 0 ) {
				usleep(2000);
				continue;
			}
			done += n;
		}
		g_mixed += done;
	}
	return nullptr;
}

}  // namespace

/*
 * A stream of sound into the mix: its number, or -1. Whether the sound
 * device is there is told by br_snd_start, as before
 */
extern "C" int br_mix_open( void )
{
	std::lock_guard<std::mutex> hold(g_lock);
	if ( !g_running ) {
		pthread_t t;
		pthread_attr_t at;
		pthread_attr_init(&at);
		pthread_attr_setstacksize(&at, 64 * 1024);
		if ( pthread_create(&t, &at, Mixer, nullptr) != 0 ) {
			pthread_attr_destroy(&at);
			printf("browser: sound: no thread to mix on\n");
			return -1;
		}
		pthread_attr_destroy(&at);
		pthread_detach(t);
		g_running = true;
	}
	int h = g_next++;
	g_streams[h] = std::make_unique<Stream>();
	return h;
}

extern "C" void br_mix_close( int h )
{
	std::lock_guard<std::mutex> hold(g_lock);
	g_streams.erase(h);
}

/* Frames of 48 kHz stereo queued on the stream: as many as fit, the number taken */
extern "C" int br_mix_write( int h, const short *frames, int nframes )
{
	std::lock_guard<std::mutex> hold(g_lock);
	auto it = g_streams.find(h);
	if ( it == g_streams.end() || nframes <= 0 ) return 0;
	std::deque<short> &q = it->second->frames;
	int room = kQueueMax - (int)( q.size() / 2 );
	int n = nframes < room ? nframes : room;
	if ( n > 0 ) q.insert(q.end(), frames, frames + n * 2);
	return n < 0 ? 0 : n;
}

/* Frames of the stream still to be heard: its queue and the ring after it */
extern "C" int br_mix_buffered( int h )
{
	int queued = 0;
	{
		std::lock_guard<std::mutex> hold(g_lock);
		auto it = g_streams.find(h);
		if ( it != g_streams.end() ) queued = (int)( it->second->frames.size() / 2 );
	}
	return queued + br_snd_buffered();
}

/* Frames of the stream not yet mixed */
extern "C" int br_mix_queued( int h )
{
	std::lock_guard<std::mutex> hold(g_lock);
	auto it = g_streams.find(h);
	return it == g_streams.end() ? 0 : (int)( it->second->frames.size() / 2 );
}
