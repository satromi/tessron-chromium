/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	media_decode.c
 *	ブラウザ: the decoding of audio and video for the media elements
 *	(design 17.19): container bytes in, I420 frames and sound out, through
 *	FFmpeg (tools/browser/ffmpeg.sh).
 *
 *	The whole resource is fetched first, demuxed from memory with an
 *	AVIOContext and decoded forward only: frame_at(t) decodes until the
 *	stream reaches t and keeps the last frame, and a seek backwards opens
 *	the stream again from its start. An audio stream is resampled to 48 kHz
 *	16-bit stereo into a ring the sound feeder drains; a file with audio
 *	and no video plays as audio only. A media source (MSE) appends its
 *	bytes to a buffer that grows as they come.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <stddef.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavutil/pixdesc.h>
#include <libavutil/dict.h>
/* The mov demuxer's own state, read and nudged by brm_mov_resume below. */
#include "libavformat/isom.h"

#include <libavutil/cpu.h>

void br_media_reset_to(int id, double t);

/* Pages keep players around: a site that moves between its pages keeps
 * the earlier ones, and their previews and idle players, in the tree.
 * With four slots the real player found none. */
#define BRM_MAX 16

struct brm {
	int used;
	const unsigned char *data;   /* caller-owned container bytes */
	unsigned len;
	unsigned pos;

	/* Common Encryption (CENC, AES-CTR): the 16-byte content key as 32 hex
	 * digits, handed to the mov demuxer's decryption_key option so encrypted
	 * samples are deciphered before decode. Empty when the media is clear. */
	char deckey[33];

	AVFormatContext *fmt;
	AVIOContext *avio;
	AVCodecContext *dec;
	int stream;
	AVFrame *frame;              /* last decoded frame */
	/* Where the decoder writes. A receive that finds nothing to return
	 * empties the frame it was given, so decoding straight into |frame|
	 * blanked the picture on screen whenever a live stream ran short of
	 * data; a frame is moved into |frame| only once one has arrived. */
	AVFrame *next;
	AVPacket *pkt;
	double frame_t;              /* presentation time of |frame| */
	int eof;

	/* audio: decoded and resampled to 48kHz S16 stereo into a ring */
	int astream;
	AVCodecContext *adec;
	AVFrame *aframe;
	struct SwrContext *swr;
#define BRM_ARING 96000          /* two seconds of stereo frames */
	short *aring;
	int ard, awr;                /* frame indices into aring */

	int w, h;
	double duration;

	/* chroma normalization: decoders that emit anything other than I420
	 * (4:2:2 and 4:4:4 from mjpeg/prores, NV12, GRAY8 from mono codecs)
	 * are resampled into these so callers always receive I420 planes.
	 * swscale is not in this ffmpeg build. */
	unsigned char *cu, *cv;
	unsigned ccap;               /* bytes allocated per chroma plane */

	/* live (MSE) mode: bytes arrive through br_media_append into an owned
	 * growing buffer; running out of bytes is an underrun, not the end of
	 * the stream, until end-of-stream is marked. */
	int live;
	unsigned char *lbuf;
	unsigned lcap;
	int opened;                  /* demuxer open has succeeded */
	int eos;                     /* site marked endOfStream */

	/* Fragmented MP4 (ISO BMFF) in live mode: the demuxer is shown the
	 * buffer only up to the end of the last complete top-level box, and is
	 * told it cannot seek. ffmpeg's mov demuxer, given a seekable input,
	 * takes whatever fragments it found at open to be the whole file and
	 * answers end-of-file once their samples run out, however much is
	 * appended afterwards -- the video then never shows a frame. Told the
	 * input is a stream, it notes where the next fragment starts and moves
	 * on to it when the bytes are there; a box cut in half by an append
	 * boundary would be parsed as garbage, hence the cut at box ends. Real
	 * seeks still go through the seek callback. */
	int bmff;
	unsigned avail;              /* end of complete boxes in lbuf */
	int saw_moov, saw_mdat;      /* an mdat after the moov is complete */
	/* The header has been read for what the stream holds (tracks, size,
	 * duration) before the demuxer proper could open: the page is told
	 * as soon as the header is in. */
	int info_ready;
	/* A second header appended after media: the stream changes there (a
	 * player switching quality appends the new rendition's header and then
	 * its fragments). The demuxer is shown the buffer only up to it; once
	 * it has read everything before, the old part is dropped and demuxer
	 * and decoders open again on the new header. 0 when there is none. */
	unsigned reinit_off;
	unsigned last_box_off;       /* where the last complete box began */
	int last_box_ftyp;           /* ... and whether it was an ftyp */

	/* scanner: a second demux context that parses appended bytes ahead of
	 * playback so buffered-range ends are known without decoding */
	AVFormatContext *sfmt;
	AVIOContext *savio;
	AVPacket *spkt;
	unsigned spos;
	double smax;                 /* highest packet end time seen */
	double smin;                 /* lowest packet start time, -1 unknown */

	/* Fragments of a live MP4 stream in buffer order: where each one's moof
	 * starts and the presentation span the scanner found for its samples.
	 * Removing buffered time drops whole fragments from the front. */
	struct brm_frag {
		unsigned off;
		double t0, t1;
	} *frags;
	int nfrags, capfrags;
	/* The last packet each decoder was handed. When the buffer is rebuilt
	 * the demuxer starts over at the first fragment kept, part of which the
	 * decoders already have; those packets are passed over (|v_skip|/
	 * |a_skip|) so the decoders carry on where they were, with nothing
	 * decoded twice. */
	int64_t v_dts, a_dts;
	int v_skip, a_skip;
};

/* Past this much held, a live buffer gives up time already played. */
#define BRM_LIVE_CAP (32u * 1024u * 1024u)

static struct brm g_brm[BRM_MAX];

static int brm_mov_resume(struct brm *m, AVFormatContext *fmt);
static int brm_reinit_switch(struct brm *m);

/* How far the demuxers may read: all of it, except for a live MP4 buffer,
 * which ends at the last complete top-level box. */
static unsigned brm_end(const struct brm *m)
{
	if (!m->bmff)
		return m->len;
	return m->reinit_off != 0 ? m->reinit_off : m->avail;
}

/* The scanner reads all of it, new headers included: the times it finds
 * are what the page is told is buffered. */
static unsigned brm_send(const struct brm *m)
{
	return m->bmff ? m->avail : m->len;
}

static unsigned brm_be32(const unsigned char *p);

/* A fragment's moof has arrived complete at |off|. */
static void brm_add_frag(struct brm *m, unsigned off)
{
	if (m->nfrags == m->capfrags) {
		int cap = m->capfrags ? m->capfrags * 2 : 64;
		struct brm_frag *nf = realloc(m->frags, (size_t)cap * sizeof(*nf));
		if (nf == NULL)
			return;
		m->frags = nf;
		m->capfrags = cap;
	}
	m->frags[m->nfrags].off = off;
	m->frags[m->nfrags].t0 = 1e30;
	m->frags[m->nfrags].t1 = -1.0;
	m->nfrags++;
}

/* A packet at byte |pos| spans |t0|..|t1|: widen its fragment's span. */
static void brm_frag_time(struct brm *m, int64_t pos, double t0, double t1)
{
	int lo = 0, hi = m->nfrags - 1, at = -1;

	if (pos < 0)
		return;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if ((int64_t)m->frags[mid].off <= pos) {
			at = mid;
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	}
	if (at < 0)
		return;
	if (t0 < m->frags[at].t0)
		m->frags[at].t0 = t0;
	if (t1 > m->frags[at].t1)
		m->frags[at].t1 = t1;
}

/* Whether the packet just read was already given to its decoder before the
 * buffer was rebuilt, and so is to be passed over. Otherwise it is noted as
 * the stream's latest. */
static int brm_skip_sent(struct brm *m)
{
	AVPacket *p = m->pkt;
	int64_t ts = p->dts != AV_NOPTS_VALUE ? p->dts : p->pts;

	if (p->stream_index == m->stream) {
		if (m->v_skip && ts != AV_NOPTS_VALUE && ts <= m->v_dts)
			return 1;
		m->v_skip = 0;
		if (ts != AV_NOPTS_VALUE)
			m->v_dts = ts;
	} else if (p->stream_index == m->astream) {
		if (m->a_skip && ts != AV_NOPTS_VALUE && ts <= m->a_dts)
			return 1;
		m->a_skip = 0;
		if (ts != AV_NOPTS_VALUE)
			m->a_dts = ts;
	}
	return 0;
}

static int brm_read(void *opaque, uint8_t *buf, int n)
{
	struct brm *m = opaque;
	unsigned left = brm_end(m) > m->pos ? brm_end(m) - m->pos : 0;
	if (left == 0)
		return AVERROR_EOF;
	if ((unsigned)n > left)
		n = (int)left;
	memcpy(buf, m->data + m->pos, (size_t)n);
	m->pos += (unsigned)n;
	return n;
}

static int64_t brm_seek(void *opaque, int64_t off, int whence)
{
	struct brm *m = opaque;
	if (whence == AVSEEK_SIZE)
		return (int64_t)brm_end(m);
	whence &= ~AVSEEK_FORCE;
	if (whence == SEEK_CUR)
		off += (int64_t)m->pos;
	else if (whence == SEEK_END)
		off += (int64_t)brm_end(m);
	if (off < 0 || off > (int64_t)brm_end(m))
		return -1;
	m->pos = (unsigned)off;
	return off;
}

static int brm_sread(void *opaque, uint8_t *buf, int n)
{
	struct brm *m = opaque;
	unsigned left = brm_send(m) > m->spos ? brm_send(m) - m->spos : 0;
	if (left == 0)
		return AVERROR_EOF;
	if ((unsigned)n > left)
		n = (int)left;
	memcpy(buf, m->data + m->spos, (size_t)n);
	m->spos += (unsigned)n;
	return n;
}

static int64_t brm_sseek(void *opaque, int64_t off, int whence)
{
	struct brm *m = opaque;
	if (whence == AVSEEK_SIZE)
		return (int64_t)brm_send(m);
	whence &= ~AVSEEK_FORCE;
	if (whence == SEEK_CUR)
		off += (int64_t)m->spos;
	else if (whence == SEEK_END)
		off += (int64_t)brm_send(m);
	if (off < 0 || off > (int64_t)brm_send(m))
		return -1;
	m->spos = (unsigned)off;
	return off;
}

static void brm_scan_close(struct brm *m)
{
	if (m->sfmt != NULL)
		avformat_close_input(&m->sfmt);
	if (m->savio != NULL) {
		av_freep(&m->savio->buffer);
		avio_context_free(&m->savio);
	}
	if (m->spkt != NULL)
		av_packet_free(&m->spkt);
	m->spos = 0;
}

/* Parse (never decode) whatever bytes the buffer holds beyond the last scan
 * position, tracking the highest packet end time. This is what buffered
 * ranges report while playback lags behind the appended data. */
static void brm_scan_pump(struct brm *m)
{
	if (m->sfmt == NULL) {
		unsigned char *iobuf = av_malloc(65536);
		if (iobuf == NULL)
			return;
		m->spos = 0;
		m->savio = avio_alloc_context(iobuf, 65536, 0, m, brm_sread,
		                              NULL, brm_sseek);
		if (m->savio == NULL) {
			av_free(iobuf);
			return;
		}
		if (m->bmff)
			m->savio->seekable = 0;
		m->sfmt = avformat_alloc_context();
		if (m->sfmt == NULL)
			return;
		m->sfmt->pb = m->savio;
		if (avformat_open_input(&m->sfmt, NULL, NULL, NULL) < 0) {
			brm_scan_close(m);
			return;
		}
		m->spkt = av_packet_alloc();
		if (m->spkt == NULL)
			return;
	}
	for (;;) {
		int rv = av_read_frame(m->sfmt, m->spkt);
		AVRational tb;
		int64_t ts;
		double end;
		if (rv < 0) {
			if (m->savio != NULL)
				m->savio->eof_reached = 0;
			if (brm_mov_resume(m, m->sfmt))
				continue;
			break;
		}
		tb = m->sfmt->streams[m->spkt->stream_index]->time_base;
		ts = m->spkt->pts != AV_NOPTS_VALUE ? m->spkt->pts
		                                    : m->spkt->dts;
		if (ts != AV_NOPTS_VALUE) {
			double start = (double)ts * tb.num / tb.den;
			end = (double)(ts + (m->spkt->duration > 0
			                         ? m->spkt->duration
			                         : 0)) *
			      tb.num / tb.den;
			if (end > m->smax)
				m->smax = end;
			if (m->smin < 0.0 || start < m->smin)
				m->smin = start;
			if (m->bmff)
				brm_frag_time(m, m->spkt->pos, start, end);
		}
		av_packet_unref(m->spkt);
	}
}

static void brm_teardown(struct brm *m)
{
	if (m->dec != NULL)
		avcodec_free_context(&m->dec);
	if (m->adec != NULL)
		avcodec_free_context(&m->adec);
	if (m->swr != NULL)
		swr_free(&m->swr);
	if (m->fmt != NULL)
		avformat_close_input(&m->fmt);
	if (m->avio != NULL) {
		av_freep(&m->avio->buffer);
		avio_context_free(&m->avio);
	}
	if (m->frame != NULL)
		av_frame_free(&m->frame);
	if (m->next != NULL)
		av_frame_free(&m->next);
	if (m->aframe != NULL)
		av_frame_free(&m->aframe);
	if (m->pkt != NULL)
		av_packet_free(&m->pkt);
	free(m->aring);
	m->aring = NULL;
	brm_scan_close(m);
}

static int brm_setup(struct brm *m)
{
	const AVCodec *codec;
	unsigned char *iobuf;
	int rv, i;

	m->pos = 0;
	m->eof = 0;
	m->frame_t = -1.0;

	iobuf = av_malloc(65536);
	if (iobuf == NULL)
		return -1;

	m->avio = avio_alloc_context(iobuf, 65536, 0, m, brm_read, NULL,
	                             brm_seek);
	if (m->avio == NULL) {
		av_free(iobuf);
		return -1;
	}
	if (m->bmff)
		m->avio->seekable = 0;
	m->fmt = avformat_alloc_context();
	if (m->fmt == NULL)
		return -1;
	m->fmt->pb = m->avio;
	{
		/* The CENC key, when present, must be set before open: the sample
		 * encryption boxes are parsed during header read. */
		AVDictionary *opts = NULL;
		if (m->deckey[0] != 0)
			av_dict_set(&opts, "decryption_key", m->deckey, 0);
		rv = avformat_open_input(&m->fmt, NULL, NULL, &opts);
		av_dict_free(&opts);
	}
	if (rv < 0) {
		printf("browser: brm: open_input %d\n", rv);
		return -1;
	}
	rv = avformat_find_stream_info(m->fmt, NULL);
	if (rv < 0)
		return -1;

	m->stream = -1;
	for (i = 0; i < (int)m->fmt->nb_streams; i++) {
		if (m->fmt->streams[i]->codecpar->codec_type ==
		    AVMEDIA_TYPE_VIDEO) {
			m->stream = i;
			break;
		}
	}

	if (m->stream >= 0) {
		codec = avcodec_find_decoder(
		    m->fmt->streams[m->stream]->codecpar->codec_id);
		if (codec == NULL) {
			printf("browser: brm: no decoder for codec %d\n",
			        (int)m->fmt->streams[m->stream]
			            ->codecpar->codec_id);
			return -1;
		}
		m->dec = avcodec_alloc_context3(codec);
		if (m->dec == NULL)
			return -1;
		avcodec_parameters_to_context(
		    m->dec, m->fmt->streams[m->stream]->codecpar);
		m->dec->thread_count = 1;
		rv = avcodec_open2(m->dec, codec, NULL);
		if (rv < 0) {
			printf("browser: brm: avcodec_open2 %d\n", rv);
			return -1;
		}
		m->w = m->dec->width;
		m->h = m->dec->height;
	} else {
		m->w = 0;
		m->h = 0;
	}
	m->duration = m->fmt->duration > 0
	                  ? (double)m->fmt->duration / AV_TIME_BASE
	                  : 0.0;
	/* A file written as it was recorded (MediaRecorder's WebM) carries no
	 * duration. The whole file is in memory, so one parse of its packets
	 * finds where the last one ends. */
	if (m->duration <= 0.0 && !m->live) {
		m->smax = 0.0;
		brm_scan_pump(m);
		m->duration = m->smax;
		brm_scan_close(m);
	}

	/* Optional audio track: decoded to 48kHz S16 stereo. Failure to set it
	 * up just leaves the clip silent. */
	m->astream = -1;
	for (i = 0; i < (int)m->fmt->nb_streams; i++) {
		if (m->fmt->streams[i]->codecpar->codec_type ==
		    AVMEDIA_TYPE_AUDIO) {
			m->astream = i;
			break;
		}
	}
	if (m->astream >= 0) {
		const AVCodec *acodec = avcodec_find_decoder(
		    m->fmt->streams[m->astream]->codecpar->codec_id);
		if (acodec != NULL) {
			m->adec = avcodec_alloc_context3(acodec);
			if (m->adec != NULL) {
				avcodec_parameters_to_context(
				    m->adec,
				    m->fmt->streams[m->astream]->codecpar);
				m->adec->thread_count = 1;
				if (avcodec_open2(m->adec, acodec, NULL) < 0)
					avcodec_free_context(&m->adec);
			}
		}
		if (m->adec != NULL) {
			int64_t layout = m->adec->channel_layout != 0
			                     ? (int64_t)m->adec->channel_layout
			                     : av_get_default_channel_layout(
			                           m->adec->channels);
			m->swr = swr_alloc_set_opts(
			    NULL, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, 48000,
			    layout, m->adec->sample_fmt, m->adec->sample_rate,
			    0, NULL);
			if (m->swr == NULL || swr_init(m->swr) < 0) {
				if (m->swr != NULL)
					swr_free(&m->swr);
				avcodec_free_context(&m->adec);
			}
		}
		if (m->adec == NULL)
			m->astream = -1;
	}
	if (m->astream >= 0 && m->aring == NULL) {
		m->aring = malloc(BRM_ARING * 2 * sizeof(short));
		if (m->aring == NULL)
			m->astream = -1;
	}
	m->ard = m->awr = 0;
	if (m->stream < 0 && m->astream < 0) {
		printf("browser: brm: no decodable stream\n");
		return -1;
	}

	if (m->frame == NULL)
		m->frame = av_frame_alloc();
	if (m->next == NULL)
		m->next = av_frame_alloc();
	if (m->aframe == NULL)
		m->aframe = av_frame_alloc();
	if (m->pkt == NULL)
		m->pkt = av_packet_alloc();
	return (m->frame != NULL && m->next != NULL && m->pkt != NULL) ? 0
	                                                                : -1;
}

static int brm_aring_count(const struct brm *m)
{
	int d = m->awr - m->ard;
	return d < 0 ? d + BRM_ARING : d;
}

/* Resample one decoded audio frame into the ring (drops when full). */
static void brm_push_audio(struct brm *m)
{
	short buf[4096 * 2];
	const uint8_t *in[8];
	uint8_t *out[1];
	int got, i;

	if (m->swr == NULL || m->aring == NULL)
		return;
	for (i = 0; i < 8; i++)
		in[i] = i < AV_NUM_DATA_POINTERS ? m->aframe->data[i] : NULL;
	out[0] = (uint8_t *)buf;
	got = swr_convert(m->swr, out, 4096, in, m->aframe->nb_samples);
	for (i = 0; i < got; i++) {
		int nxt = m->awr + 1;
		if (nxt >= BRM_ARING)
			nxt -= BRM_ARING;
		if (nxt == m->ard)
			break;               /* ring full: drop the tail */
		m->aring[m->awr * 2] = buf[i * 2];
		m->aring[m->awr * 2 + 1] = buf[i * 2 + 1];
		m->awr = nxt;
	}
}

/* Drain whatever the audio decoder currently holds. */
static void brm_pump_adec(struct brm *m)
{
	if (m->adec == NULL)
		return;
	while (avcodec_receive_frame(m->adec, m->aframe) == 0)
		brm_push_audio(m);
}

int br_media_open_key(const void *data, unsigned len, const char *keyhex)
{
	int i;
	struct brm *m = NULL;

	for (i = 0; i < BRM_MAX; i++) {
		if (!g_brm[i].used) {
			m = &g_brm[i];
			break;
		}
	}
	if (m == NULL)
		return -1;
	memset(m, 0, sizeof(*m));
	m->data = data;
	m->len = len;
	if (keyhex != NULL && keyhex[0] != 0) {
		unsigned k = 0;
		while (keyhex[k] != 0 && k < sizeof(m->deckey) - 1) {
			m->deckey[k] = keyhex[k];
			k++;
		}
		m->deckey[k] = 0;
	}
	if (brm_setup(m) < 0) {
		brm_teardown(m);
		memset(m, 0, sizeof(*m));
		return -1;
	}
	m->used = 1;
	printf("browser: brm: open %dx%d dur=%d ms stream=%d astream=%d%s\n", m->w, m->h,
	        (int)(m->duration * 1000), m->stream, m->astream,
	        m->deckey[0] ? " (clearkey)" : "");
	return i;
}

int br_media_open(const void *data, unsigned len)
{
	return br_media_open_key(data, len, NULL);
}

int br_media_info(int id, int *w, int *h, double *duration)
{
	struct brm *m = &g_brm[id];
	if (!m->used)
		return -1;
	*w = m->w;
	*h = m->h;
	*duration = m->duration;
	return 0;
}

static double brm_frame_time(const struct brm *m)
{
	AVRational tb = m->fmt->streams[m->stream]->time_base;
	int64_t pts = m->frame->best_effort_timestamp;
	if (pts == AV_NOPTS_VALUE)
		pts = m->frame->pts;
	if (pts == AV_NOPTS_VALUE)
		return m->frame_t < 0 ? 0.0 : m->frame_t + 0.04;
	return (double)pts * tb.num / tb.den;
}

/* Takes the decoder's next frame, if it has one, as the current frame. */
static int brm_take_frame(struct brm *m)
{
	int rv = avcodec_receive_frame(m->dec, m->next);
	if (rv == 0) {
		av_frame_unref(m->frame);
		av_frame_move_ref(m->frame, m->next);
		m->frame_t = brm_frame_time(m);
	}
	return rv;
}

/* A live MP4 stream that ran out of data at the end of a fragment. The mov
 * demuxer moves to the next fragment by seeking to where it starts; when
 * that is exactly the end of the data so far, the seek succeeds, there is
 * nothing to parse, and the demuxer forgets where the next fragment was to
 * begin -- every later read then reports the end of the file, however much
 * is appended. So once it is in that state, point it at the first complete
 * fragment after the last one it parsed, if one has arrived since.
 * Returns 1 when it did, and a read can be tried again at once. */
static int brm_mov_resume(struct brm *m, AVFormatContext *fmt)
{
	MOVContext *mov;
	unsigned off = 0;

	if (!m->bmff || fmt == NULL || fmt->iformat == NULL ||
	    strncmp(fmt->iformat->name, "mov", 3) != 0)
		return 0;
	unsigned end = fmt == m->sfmt ? brm_send(m) : brm_end(m);

	mov = fmt->priv_data;
	if (mov == NULL || mov->next_root_atom != 0)
		return 0;
	while (end - off >= 8 && off < end) {
		const unsigned char *b = m->lbuf + off;
		unsigned long long size = ((unsigned long long)b[0] << 24) |
		                          ((unsigned long long)b[1] << 16) |
		                          ((unsigned long long)b[2] << 8) | b[3];
		if (size == 1) {
			int i;
			if (end - off < 16)
				return 0;
			size = 0;
			for (i = 8; i < 16; i++)
				size = (size << 8) | b[i];
		}
		if (size < 8 || size > (unsigned long long)(end - off))
			return 0;
		if ((int64_t)off > mov->fragment.moof_offset &&
		    memcmp(b + 4, "moof", 4) == 0) {
			mov->next_root_atom = off;
			return 1;
		}
		off += (unsigned)size;
	}
	return 0;
}

/* Decode forward until the stream time reaches |t| seconds. In live mode,
 * running out of appended bytes leaves the pipeline waiting (the last frame
 * stays up); only a marked end-of-stream flushes the decoder for real. */
/* The clock at which each handle last jumped ahead. */
static double g_jumped_at[BRM_MAX];

static void brm_advance_to(struct brm *m, double t)
{
	if (m->dec == NULL)
		return;
	/* Decoding is paced by the clock, so a stall (a collection of the
	 * whole heap, a burst of layout) leaves the picture behind the sound
	 * and every later call has to decode all the frames in between at
	 * once, on this thread, which only puts it further behind. While the
	 * picture lags, the decoder is told to drop what nothing else depends
	 * on -- the non-reference frames, and beyond a long lag everything
	 * but key frames -- and given the full stream again once it has
	 * caught up. */
	if (m->frame_t >= 0) {
		double lag = t - m->frame_t;
		if (lag > 2.0 && m->fmt != NULL &&
		    t - g_jumped_at[m - g_brm] > 3.0) {
			/* Too far to decode through: seek to the key frame at or
			 * after the clock and go on from there. Seeking to the one
			 * before it meant decoding a whole group of pictures to get
			 * back to the clock, which took longer than the clock moved
			 * on, and the seek repeated every few frames. Decoding all
			 * but key frames instead left the picture standing on one
			 * frame for seconds at a time. */
			int rv = av_seek_frame(m->fmt, -1,
			                       (int64_t)(t * AV_TIME_BASE), 0);
			if (rv >= 0) {
				avcodec_flush_buffers(m->dec);
				if (m->adec != NULL)
					avcodec_flush_buffers(m->adec);
				m->frame_t = -1.0;
				m->eof = 0;
				g_jumped_at[m - g_brm] = t;
				printf("browser: brm: picture %d ms behind, jumped ahead\n",
				        (int)(lag * 1000));
				lag = 0.0;
			}
		}
		{
			int want = lag > 0.3 ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
			if (want != m->dec->skip_frame) {
				m->dec->skip_frame = want;
				if (want != AVDISCARD_DEFAULT)
					printf("browser: brm: picture %d ms behind, dropping \n"
					        "non-reference frames", (int)(lag * 1000));
			}
		}
	}
	while (!m->eof && m->frame_t < t) {
		int rv = brm_take_frame(m);
		if (rv == 0)
			continue;
		if (rv != AVERROR(EAGAIN)) {
			m->eof = 1;
			return;
		}
		for (;;) {
			rv = av_read_frame(m->fmt, m->pkt);
			if (rv < 0) {
				if (m->live && !m->eos) {
					if (m->avio != NULL)
						m->avio->eof_reached = 0;
					if (brm_mov_resume(m, m->fmt))
						continue;
					if (brm_reinit_switch(m))
						continue;
					return;
				}
				avcodec_send_packet(m->dec, NULL);
				break;
			}
			if (brm_skip_sent(m)) {
				av_packet_unref(m->pkt);
				continue;
			}
			if (m->pkt->stream_index == m->stream) {
				avcodec_send_packet(m->dec, m->pkt);
				av_packet_unref(m->pkt);
				break;
			}
			if (m->pkt->stream_index == m->astream &&
			    m->adec != NULL) {
				avcodec_send_packet(m->adec, m->pkt);
				brm_pump_adec(m);
			}
			av_packet_unref(m->pkt);
		}
	}
}

/* Fill |m->cu| / |m->cv| with I420-shaped chroma taken from the decoded
 * frame, whatever its subsampling. Returns 0 when the format is not an
 * 8-bit planar or semi-planar YUV (packed RGB, >8-bit) and so cannot be
 * served through the three-plane contract. */
static int brm_make_i420_chroma(struct brm *m)
{
	const AVPixFmtDescriptor *d;
	AVFrame *f = m->frame;
	int cw, ch, sw, sh, x, yy;
	unsigned need;
	int fmt = f->format;

	if (fmt == AV_PIX_FMT_YUV420P || fmt == AV_PIX_FMT_YUVJ420P)
		return 1;                        /* already I420 */

	d = av_pix_fmt_desc_get((enum AVPixelFormat)fmt);
	if (d == NULL || d->comp[0].depth != 8)
		return 0;

	cw = (f->width + 1) / 2;
	ch = (f->height + 1) / 2;
	need = (unsigned)cw * (unsigned)ch;
	if (m->ccap < need) {
		unsigned char *nu = realloc(m->cu, need);
		unsigned char *nv = realloc(m->cv, need);
		if (nu != NULL)
			m->cu = nu;
		if (nv != NULL)
			m->cv = nv;
		if (nu == NULL || nv == NULL)
			return 0;
		m->ccap = need;
	}

	/* Monochrome: no chroma planes at all, so paint neutral. */
	if (d->nb_components < 3) {
		memset(m->cu, 128, need);
		memset(m->cv, 128, need);
		return 1;
	}

	/* Source chroma plane dimensions from the format's subsampling. */
	sw = AV_CEIL_RSHIFT(f->width, d->log2_chroma_w);
	sh = AV_CEIL_RSHIFT(f->height, d->log2_chroma_h);
	if (sw <= 0 || sh <= 0 || f->data[1] == NULL)
		return 0;

	if ((d->flags & AV_PIX_FMT_FLAG_PLANAR) == 0 || f->data[2] == NULL) {
		/* Semi-planar (NV12/NV21): one plane of interleaved pairs. */
		int u_off = (d->comp[1].offset != 0) ? 1 : 0;
		int v_off = u_off ^ 1;
		if (f->data[1] == NULL)
			return 0;
		for (yy = 0; yy < ch; yy++) {
			int sy = (sh == ch) ? yy : (int)((long)yy * sh / ch);
			const unsigned char *row = f->data[1] +
			                           (long)sy * f->linesize[1];
			unsigned char *du = m->cu + (long)yy * cw;
			unsigned char *dv = m->cv + (long)yy * cw;
			for (x = 0; x < cw; x++) {
				int sx = (sw == cw) ? x : (int)((long)x * sw / cw);
				du[x] = row[sx * 2 + u_off];
				dv[x] = row[sx * 2 + v_off];
			}
		}
		return 1;
	}

	/* Planar: nearest-sample each chroma plane onto the I420 grid. */
	for (yy = 0; yy < ch; yy++) {
		int sy = (sh == ch) ? yy : (int)((long)yy * sh / ch);
		const unsigned char *ru = f->data[1] + (long)sy * f->linesize[1];
		const unsigned char *rv = f->data[2] + (long)sy * f->linesize[2];
		unsigned char *du = m->cu + (long)yy * cw;
		unsigned char *dv = m->cv + (long)yy * cw;
		if (sw == cw) {
			memcpy(du, ru, (size_t)cw);
			memcpy(dv, rv, (size_t)cw);
			continue;
		}
		for (x = 0; x < cw; x++) {
			int sx = (int)((long)x * sw / cw);
			du[x] = ru[sx];
			dv[x] = rv[sx];
		}
	}
	return 1;
}

int br_media_frame_at(int id, double t, const unsigned char **y,
                      const unsigned char **u, const unsigned char **v,
                      int *ystride, int *uvstride)
{
	struct brm *m = &g_brm[id];
	if (!m->used)
		return 0;
	if (t < m->frame_t - 0.05) {
		/* backwards: restart the forward-only pipeline */
		if (m->live) {
			br_media_reset_to(id, t);
		} else {
			brm_teardown(m);
			if (brm_setup(m) < 0) {
				memset(m, 0, sizeof(*m));
				return 0;
			}
		}
	}
	brm_advance_to(m, t);
	if (m->frame_t < 0 || m->frame->data[0] == NULL)
		return 0;
	if (!brm_make_i420_chroma(m))
		return 0;
	*y = m->frame->data[0];
	*ystride = m->frame->linesize[0];
	if (m->frame->format == AV_PIX_FMT_YUV420P ||
	    m->frame->format == AV_PIX_FMT_YUVJ420P) {
		*u = m->frame->data[1];
		*v = m->frame->data[2];
		*uvstride = m->frame->linesize[1];
	} else {
		*u = m->cu;
		*v = m->cv;
		*uvstride = (m->frame->width + 1) / 2;
	}
	return 1;
}

/* The presentation time of the frame br_media_frame_at last returned, or
 * -1: a caller that shows frames as they come skips the ones it has shown. */
double br_media_frame_time(int id)
{
	struct brm *m = &g_brm[id];
	return m->used ? m->frame_t : -1.0;
}

int br_media_ended(int id)
{
	struct brm *m = &g_brm[id];
	return m->used ? m->eof : 1;
}

int br_media_has_audio(int id)
{
	struct brm *m = &g_brm[id];
	return m->used && m->astream >= 0;
}

/* Pull up to |max_frames| stereo frames of 48kHz S16 PCM. When the ring
 * runs dry the demux loop advances (which may also move the video decoder
 * forward -- video and audio consume at the same wall rate, so the drift
 * stays within the ring). */
int br_media_pull_audio(int id, short *out, int max_frames)
{
	struct brm *m = &g_brm[id];
	int got = 0;

	if (!m->used || m->astream < 0 || m->fmt == NULL)
		return 0;
	while (got < max_frames) {
		int avail = brm_aring_count(m);
		int rv;
		if (avail == 0) {
			if (m->eof)
				break;
			/* demux more; audio packets land in the ring */
			rv = av_read_frame(m->fmt, m->pkt);
			if (rv < 0) {
				if (m->live && !m->eos) {
					/* underrun, not the end: resume after
					 * the next append */
					if (m->avio != NULL)
						m->avio->eof_reached = 0;
					if (brm_mov_resume(m, m->fmt))
						continue;
					if (brm_reinit_switch(m))
						continue;
					break;
				}
				if (m->adec != NULL) {
					avcodec_send_packet(m->adec, NULL);
					brm_pump_adec(m);
				}
				if (brm_aring_count(m) == 0)
					break;
				continue;
			}
			if (brm_skip_sent(m)) {
				av_packet_unref(m->pkt);
				continue;
			}
			if (m->pkt->stream_index == m->astream &&
			    m->adec != NULL) {
				avcodec_send_packet(m->adec, m->pkt);
				brm_pump_adec(m);
			} else if (m->pkt->stream_index == m->stream) {
				avcodec_send_packet(m->dec, m->pkt);
				brm_take_frame(m);
			}
			av_packet_unref(m->pkt);
			continue;
		}
		out[got * 2] = m->aring[m->ard * 2];
		out[got * 2 + 1] = m->aring[m->ard * 2 + 1];
		m->ard = m->ard + 1 >= BRM_ARING ? 0 : m->ard + 1;
		++got;
	}
	return got;
}

void br_media_close(int id)
{
	struct brm *m = &g_brm[id];
	if (!m->used)
		return;
	brm_teardown(m);
	free(m->lbuf);
	free(m->cu);
	free(m->cv);
	free(m->frags);
	memset(m, 0, sizeof(*m));
}

/* ---- live (MSE) handles -------------------------------------------------- */

int br_media_open_live(void)
{
	int i;
	for (i = 0; i < BRM_MAX; i++) {
		if (!g_brm[i].used) {
			memset(&g_brm[i], 0, sizeof(g_brm[i]));
			g_brm[i].used = 1;
			g_brm[i].live = 1;
			g_brm[i].smin = -1.0;
			return i;
		}
	}
	return -1;
}

/* Close only the demux/decoder side, keeping the byte buffer and scan
 * results; a later open retries over the grown buffer. */
static void brm_close_demux(struct brm *m)
{
	if (m->dec != NULL)
		avcodec_free_context(&m->dec);
	if (m->adec != NULL)
		avcodec_free_context(&m->adec);
	if (m->swr != NULL)
		swr_free(&m->swr);
	if (m->fmt != NULL)
		avformat_close_input(&m->fmt);
	if (m->avio != NULL) {
		av_freep(&m->avio->buffer);
		avio_context_free(&m->avio);
	}
}

/* Advance |avail| over every top-level box now complete in the buffer. The
 * first append decides whether this is ISO BMFF at all: a box type at bytes
 * 4..7 that such a stream starts with. Anything else (WebM) is read up to
 * the end of what arrived, as before. */
static void brm_walk_boxes(struct brm *m)
{
	if (m->avail == 0 && !m->bmff) {
		if (m->len < 8)
			return;
		if (memcmp(m->lbuf + 4, "ftyp", 4) != 0 &&
		    memcmp(m->lbuf + 4, "styp", 4) != 0 &&
		    memcmp(m->lbuf + 4, "moov", 4) != 0)
			return;
		m->bmff = 1;
	}
	if (!m->bmff)
		return;
	while (m->len - m->avail >= 8) {
		const unsigned char *b = m->lbuf + m->avail;
		unsigned long long size = ((unsigned long long)b[0] << 24) |
		                          ((unsigned long long)b[1] << 16) |
		                          ((unsigned long long)b[2] << 8) | b[3];
		if (size == 1) {
			/* 64-bit size after the type */
			int i;
			if (m->len - m->avail < 16)
				break;
			size = 0;
			for (i = 8; i < 16; i++)
				size = (size << 8) | b[i];
		} else if (size == 0) {
			/* "to the end of the file": complete only at the end */
			if (!m->eos)
				break;
			size = m->len - m->avail;
		}
		if (size < 8 || size > (unsigned long long)(m->len - m->avail))
			break;
		if (memcmp(b + 4, "moov", 4) == 0) {
			if (m->saw_moov && m->saw_mdat && m->reinit_off == 0) {
				/* from its ftyp, when one leads it */
				m->reinit_off = m->last_box_ftyp ? m->last_box_off
				                                 : m->avail;
				printf("browser: brm: new header at %u after media\n",
				        m->reinit_off);
			}
			m->saw_moov = 1;
		} else if (memcmp(b + 4, "mdat", 4) == 0 && m->saw_moov) {
			m->saw_mdat = 1;
		} else if (memcmp(b + 4, "moof", 4) == 0) {
			brm_add_frag(m, m->avail);
		}
		m->last_box_ftyp = memcmp(b + 4, "ftyp", 4) == 0;
		m->last_box_off = m->avail;
		m->avail += (unsigned)size;
	}
}

/* After a switch the buffer starts with the new header: look for yet
 * another one further on. */
static void brm_find_reinit(struct brm *m)
{
	unsigned off = 0, prev = 0;
	int moovs = 0, mdat = 0, prev_ftyp = 0;

	m->reinit_off = 0;
	while (m->avail - off >= 8) {
		unsigned size = brm_be32(m->lbuf + off);
		const unsigned char *t = m->lbuf + off + 4;
		if (size < 8 || size > m->avail - off)
			break;
		if (memcmp(t, "moov", 4) == 0) {
			if (moovs > 0 && mdat) {
				m->reinit_off = prev_ftyp ? prev : off;
				return;
			}
			moovs++;
		} else if (memcmp(t, "mdat", 4) == 0 && moovs > 0) {
			mdat = 1;
		}
		prev_ftyp = memcmp(t, "ftyp", 4) == 0;
		prev = off;
		off += size;
	}
}

/* The demuxer has read everything before a new header: drop that, and read
 * from the new header with new decoders. What was decoded stays: the frame
 * on screen until the next arrives, and the audio already in the ring.
 * Returns 1 when it switched. */
static int brm_reinit_switch(struct brm *m)
{
	unsigned from = m->reinit_off;
	unsigned char *nb;
	int i, j, ard, awr;

	if (from == 0 || !m->live || !m->bmff || from > m->len)
		return 0;
	memmove(m->lbuf, m->lbuf + from, m->len - from);
	m->len -= from;
	m->avail -= from;
	nb = malloc(m->len + 262144u);
	if (nb != NULL) {
		memcpy(nb, m->lbuf, m->len);
		free(m->lbuf);
		m->lbuf = nb;
		m->lcap = m->len + 262144u;
	}
	m->data = m->lbuf;
	for (i = 0, j = 0; i < m->nfrags; i++) {
		if (m->frags[i].off < from)
			continue;
		m->frags[j] = m->frags[i];
		m->frags[j].off -= from;
		j++;
	}
	m->nfrags = j;
	if (m->nfrags > 0 && m->frags[0].t0 < 1e29)
		m->smin = m->frags[0].t0;
	m->last_box_off = m->last_box_off >= from ? m->last_box_off - from : 0;
	brm_find_reinit(m);

	ard = m->ard;
	awr = m->awr;
	brm_close_demux(m);
	brm_scan_close(m);
	if (brm_setup(m) != 0) {
		/* the next append tries again from the header */
		brm_close_demux(m);
		m->opened = 0;
		printf("browser: brm: reading the new header failed\n");
		return 0;
	}
	m->ard = ard;
	m->awr = awr;
	m->v_skip = m->a_skip = 0;
	printf("browser: brm: switched to the new header: %dx%d, %u KB held\n",
	        m->w, m->h, m->len / 1024);
	return 1;
}

int br_media_append(int id, const void *bytes, unsigned n)
{
	struct brm *m = &g_brm[id];
	if (!m->used || !m->live)
		return -1;
	if (m->len + n > m->lcap) {
		unsigned cap = m->lcap == 0 ? 262144 : m->lcap;
		unsigned char *nb;
		while (cap < m->len + n)
			cap *= 2;
		nb = realloc(m->lbuf, cap);
		if (nb == NULL)
			return -1;
		m->lbuf = nb;
		m->lcap = cap;
	}
	memcpy(m->lbuf + m->len, bytes, n);
	m->len += n;
	m->data = m->lbuf;
	brm_walk_boxes(m);

	/* An MP4 stream opens once a media fragment is complete behind its
	 * header: opened on the header alone, the demuxer would not know
	 * where the first fragment starts. What the stream holds is read from
	 * the header as soon as it is in, though: a player may append the
	 * header alone and wait to hear its tracks before sending media. */
	if (!m->opened && m->bmff && !(m->saw_moov && m->saw_mdat)) {
		if (m->saw_moov && !m->info_ready) {
			if (brm_setup(m) == 0) {
				m->info_ready = 1;
				printf("browser: brm: header in: %dx%d dur=%d ms v=%d a=%d\n",
				        m->w, m->h, (int)(m->duration * 1000),
				        m->stream, m->astream);
			}
			brm_close_demux(m);
		}
		return 0;
	}
	if (!m->opened) {
		if (brm_setup(m) == 0) {
			m->opened = 1;
			printf("browser: brm: live open %dx%d dur=%d ms v=%d a=%d\n",
			        m->w, m->h, (int)(m->duration * 1000),
			        m->stream, m->astream);
		} else {
			/* The container head may simply not be complete yet;
			 * keep the bytes and retry on the next append. */
			brm_close_demux(m);
			if (m->len > 16u * 1024 * 1024) {
				printf("browser: brm: live open gave up at %u bytes\n",
				        m->len);
				return -1;
			}
		}
	}
	if (m->opened) {
		if (m->avio != NULL)
			m->avio->eof_reached = 0;
		brm_scan_pump(m);
	}
	return 0;
}

int br_media_live_ready(int id)
{
	struct brm *m = &g_brm[id];
	return m->used && m->live && (m->opened || m->info_ready);
}

double br_media_buffered_end(int id)
{
	struct brm *m = &g_brm[id];
	if (!m->used)
		return 0.0;
	if (m->live && m->opened && m->sfmt == NULL)
		brm_scan_pump(m);
	return m->smax;
}

void br_media_mark_eos(int id)
{
	struct brm *m = &g_brm[id];
	if (m->used) {
		m->eos = 1;
		if (m->live)
			brm_walk_boxes(m);
	}
}

int br_media_has_video(int id)
{
	struct brm *m = &g_brm[id];
	return m->used && m->stream >= 0;
}

/* Reposition a live pipeline to |t|: try the demuxer's own seek (works once
 * the byte buffer holds the target), fall back to a fresh open. The video
 * side then advances on the next frame_at; the audio side skips packets
 * until the target time so the ring restarts near |t|. */
void br_media_reset_to(int id, double t)
{
	struct brm *m = &g_brm[id];
	int rv;
	if (!m->used || !m->opened || m->fmt == NULL)
		return;
	rv = av_seek_frame(m->fmt, -1, (int64_t)(t * AV_TIME_BASE),
	                   AVSEEK_FLAG_BACKWARD);
	if (rv < 0) {
		brm_close_demux(m);
		if (brm_setup(m) != 0) {
			m->opened = 0;
			return;
		}
	} else {
		if (m->dec != NULL)
			avcodec_flush_buffers(m->dec);
		if (m->adec != NULL)
			avcodec_flush_buffers(m->adec);
	}
	m->frame_t = -1.0;
	m->eof = 0;
	m->ard = m->awr = 0;
	m->v_skip = m->a_skip = 0;
	if (m->stream < 0 && m->astream >= 0 && t > 0.2) {
		/* audio-only: drop packets below the target so the next pull
		 * starts near |t| instead of replaying from the front */
		while (av_read_frame(m->fmt, m->pkt) == 0) {
			AVRational tb =
			    m->fmt->streams[m->pkt->stream_index]->time_base;
			int64_t ts = m->pkt->pts != AV_NOPTS_VALUE
			                 ? m->pkt->pts
			                 : m->pkt->dts;
			double pt = ts != AV_NOPTS_VALUE
			                ? (double)ts * tb.num / tb.den
			                : 0.0;
			if (m->pkt->stream_index == m->astream &&
			    pt >= t - 0.2) {
				if (m->adec != NULL) {
					avcodec_send_packet(m->adec, m->pkt);
					brm_pump_adec(m);
				}
				av_packet_unref(m->pkt);
				break;
			}
			av_packet_unref(m->pkt);
		}
	}
}

/* ---- removing buffered time from a live MP4 stream ---------------------- */

/* Reads the stream again from the start of the (rebuilt) buffer, keeping the
 * decoders: only the demuxer's byte positions refer to the old layout. */
static int brm_reopen_demux(struct brm *m)
{
	unsigned char *iobuf;
	AVDictionary *opts = NULL;
	int rv;

	if (m->fmt != NULL)
		avformat_close_input(&m->fmt);
	if (m->avio != NULL) {
		av_freep(&m->avio->buffer);
		avio_context_free(&m->avio);
	}
	m->pos = 0;
	iobuf = av_malloc(65536);
	if (iobuf == NULL)
		return -1;

	m->avio = avio_alloc_context(iobuf, 65536, 0, m, brm_read, NULL,
	                             brm_seek);
	if (m->avio == NULL) {
		av_free(iobuf);
		return -1;
	}
	m->avio->seekable = 0;
	m->fmt = avformat_alloc_context();
	if (m->fmt == NULL)
		return -1;
	m->fmt->pb = m->avio;
	if (m->deckey[0] != 0)
		av_dict_set(&opts, "decryption_key", m->deckey, 0);
	rv = avformat_open_input(&m->fmt, NULL, NULL, &opts);
	av_dict_free(&opts);
	if (rv < 0)
		return -1;
	return avformat_find_stream_info(m->fmt, NULL) < 0 ? -1 : 0;
}

/* Drops the fragments that end at or before |before| seconds, always keeping
 * the last one -- read or not: removed media is gone whether it was played
 * (a track nobody pulls, muted audio say, is never read at all), and the
 * packets of kept fragments the decoders already had are passed over after
 * the rebuild. The buffer is
 * rebuilt as the stream's header boxes (without the segment index, whose
 * offsets describe the old layout) followed by the fragments kept, and the
 * memory goes back to the allocator, which does not shrink a block in
 * place. Returns the bytes released. */
static unsigned brm_trim(struct brm *m, double before)
{
	int cut = 0, i;
	unsigned head, from, w = 0, off = 0, drop;
	unsigned char *nb;

	if (!m->used || !m->live || !m->bmff || !m->opened || m->nfrags < 2)
		return 0;
	while (cut < m->nfrags - 1 && m->frags[cut].t1 >= 0.0 &&
	       m->frags[cut].t1 <= before &&
	       (m->reinit_off == 0 || m->frags[cut + 1].off <= m->reinit_off))
		cut++;
	if (cut == 0)
		return 0;
	head = m->frags[0].off;
	from = m->frags[cut].off;

	while (head - off >= 8) {
		unsigned size = brm_be32(m->lbuf + off);
		if (size < 8 || size > head - off)
			break;
		if (memcmp(m->lbuf + off + 4, "sidx", 4) != 0) {
			if (w != off)
				memmove(m->lbuf + w, m->lbuf + off, size);
			w += size;
		}
		off += size;
	}
	if (off < head) {
		/* a header box this cannot size: keep the rest as it is */
		memmove(m->lbuf + w, m->lbuf + off, head - off);
		w += head - off;
	}
	drop = from - w;
	memmove(m->lbuf + w, m->lbuf + from, m->len - from);
	m->len -= drop;
	m->avail -= drop;
	if (m->reinit_off != 0)
		m->reinit_off -= drop;
	if (m->last_box_off >= from)
		m->last_box_off -= drop;
	nb = malloc(m->len + 262144u);
	if (nb != NULL) {
		memcpy(nb, m->lbuf, m->len);
		free(m->lbuf);
		m->lbuf = nb;
		m->lcap = m->len + 262144u;
	}
	m->data = m->lbuf;
	for (i = cut; i < m->nfrags; i++) {
		m->frags[i - cut] = m->frags[i];
		m->frags[i - cut].off -= drop;
	}
	m->nfrags -= cut;
	m->smin = m->frags[0].t0 < 1e29 ? m->frags[0].t0 : before;

	brm_scan_close(m);
	if (brm_reopen_demux(m) != 0) {
		/* start over from the header on the next append */
		printf("browser: brm: reading the trimmed buffer failed, reopening\n");
		brm_close_demux(m);
		m->opened = 0;
		return drop;
	}
	m->eof = 0;
	m->v_skip = m->a_skip = 1;
	printf("browser: brm: dropped %u KB (%d fragment(s)) before %d ms, %u KB held\n",
	        drop / 1024, cut, (int)(before * 1000), m->len / 1024);
	return drop;
}

double br_media_buffered_start(int id)
{
	struct brm *m = &g_brm[id];
	if (!m->used || m->smin < 0.0)
		return 0.0;
	return m->smin;
}

/* SourceBuffer.remove(): only a range starting at the front of what is held
 * can go (the kind a player issues to drop what it has played), and never
 * what is playing now. Anything else stays held. */
void br_media_remove(int id, double start, double end, double now)
{
	struct brm *m = &g_brm[id];
	double before;

	if (!m->used)
		return;
	if (start > br_media_buffered_start(id) + 0.5)
		return;
	before = end < now - 1.0 ? end : now - 1.0;
	brm_trim(m, before);
}

/* Room for |incoming| more bytes: past the cap, time more than a few
 * seconds behind |now| is given up, the way a browser evicts before an
 * append. */
void br_media_evict(int id, double now, unsigned incoming)
{
	struct brm *m = &g_brm[id];

	if (!m->used || m->len + incoming <= BRM_LIVE_CAP)
		return;
	brm_trim(m, now - 5.0);
}

/* ---- Common Encryption (CENC) / EME ClearKey support ------------------- */
/* A small keyID -> content key store filled by the EME layer (session
 * update), and an MP4 probe that reports whether a container is encrypted,
 * with the track's default key ID and the concatenated pssh init data the
 * 'encrypted' event needs. Decryption itself is the mov demuxer's, driven by
 * the decryption_key option set from a matching key (see br_media_open_key). */

static int brm_hexval(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

#define BRM_EME_MAX 8
static struct {
	char kid[33];   /* 32 hex digits + NUL */
	char key[33];
} g_eme_keys[BRM_EME_MAX];
static int g_eme_nkeys;

/* Register a content key for a key ID (both 32-hex-digit strings). A repeat
 * key ID overwrites. */
void br_eme_add_key(const char *kidhex, const char *keyhex)
{
	int i;
	if (kidhex == NULL || keyhex == NULL)
		return;
	for (i = 0; i < g_eme_nkeys; i++) {
		if (strncmp(g_eme_keys[i].kid, kidhex, 32) == 0)
			break;
	}
	if (i == g_eme_nkeys) {
		if (g_eme_nkeys >= BRM_EME_MAX)
			return;
		g_eme_nkeys++;
	}
	strncpy(g_eme_keys[i].kid, kidhex, 32); g_eme_keys[i].kid[32] = 0;
	strncpy(g_eme_keys[i].key, keyhex, 32); g_eme_keys[i].key[32] = 0;
	printf("browser: brm: eme key added for kid %s\n", g_eme_keys[i].kid);
}

/* The content key for a key ID, or NULL. */
const char *br_eme_find_key(const char *kidhex)
{
	int i;
	if (kidhex == NULL)
		return NULL;
	for (i = 0; i < g_eme_nkeys; i++) {
		if (strncmp(g_eme_keys[i].kid, kidhex, 32) == 0)
			return g_eme_keys[i].key;
	}
	return NULL;
}

/* Big-endian u32 at p. */
static unsigned brm_be32(const unsigned char *p)
{
	return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
	       ((unsigned)p[2] << 8) | p[3];
}

static void brm_tohex(const unsigned char *b, int n, char *out)
{
	static const char h[] = "0123456789abcdef";
	int i;
	for (i = 0; i < n; i++) {
		out[i * 2] = h[(b[i] >> 4) & 0xf];
		out[i * 2 + 1] = h[b[i] & 0xf];
	}
	out[n * 2] = 0;
}

/* Recursively walk MP4 boxes in [p, p+len), collecting the first 'tenc'
 * default key ID and appending every 'pssh' box (whole box) to initdata. */
static void brm_scan_boxes(const unsigned char *p, unsigned len,
                           char *kid_out, int *have_kid,
                           unsigned char *init, unsigned *init_len,
                           unsigned init_cap)
{
	unsigned off = 0;
	while (off + 8 <= len) {
		unsigned bsz = brm_be32(p + off);
		const unsigned char *type = p + off + 4;
		unsigned hdr = 8;
		unsigned long long real = bsz;
		if (bsz == 1) {           /* 64-bit largesize */
			if (off + 16 > len) break;
			real = ((unsigned long long)brm_be32(p + off + 8) << 32) |
			       brm_be32(p + off + 12);
			hdr = 16;
		} else if (bsz == 0) {
			real = len - off;     /* extends to end */
		}
		if (real < hdr || off + real > len)
			break;
		if (memcmp(type, "pssh", 4) == 0) {
			unsigned n = (unsigned)real;
			if (*init_len + n <= init_cap) {
				memcpy(init + *init_len, p + off, n);
				*init_len += n;
			}
		} else if (memcmp(type, "tenc", 4) == 0) {
			/* fullbox(4) reserved(1) default_crypt/skip(1)
			 * default_isProtected(1) default_Per_Sample_IV_Size(1)
			 * default_KID(16) */
			const unsigned char *b = p + off + hdr;
			if (real >= hdr + 8 + 16 && !*have_kid) {
				brm_tohex(b + 8, 16, kid_out);
				*have_kid = 1;
			}
		} else {
			/* Descend into container boxes to reach the sinf/tenc that
			 * carries the key ID. Sample entries (encv/enca) hold their
			 * child boxes after a fixed VisualSampleEntry/AudioSampleEntry
			 * prefix; stsd after its fullbox+count header. */
			unsigned inner = 0;
			if (memcmp(type, "moov", 4) == 0 ||
			    memcmp(type, "trak", 4) == 0 ||
			    memcmp(type, "mdia", 4) == 0 ||
			    memcmp(type, "minf", 4) == 0 ||
			    memcmp(type, "stbl", 4) == 0 ||
			    memcmp(type, "sinf", 4) == 0 ||
			    memcmp(type, "schi", 4) == 0 ||
			    memcmp(type, "moof", 4) == 0 ||
			    memcmp(type, "traf", 4) == 0)
				inner = hdr;
			else if (memcmp(type, "stsd", 4) == 0)
				inner = hdr + 8;
			else if (memcmp(type, "encv", 4) == 0)
				inner = hdr + 78;   /* 8 SampleEntry + 70 VisualSampleEntry */
			else if (memcmp(type, "enca", 4) == 0)
				inner = hdr + 28;   /* 8 SampleEntry + 20 AudioSampleEntry */
			if (inner != 0 && (unsigned long long)inner < real)
				brm_scan_boxes(p + off + inner,
				               (unsigned)(real - inner), kid_out,
				               have_kid, init, init_len, init_cap);
		}
		off += (unsigned)real;
	}
}

/* Probe an MP4 for CENC: 1 if encrypted (kid_out and initdata filled), else 0.
 * kid_out holds 32 hex digits; initdata/initlen receive the concatenated pssh
 * boxes (EME 'cenc' init data). */
int br_mp4_encryption_info(const void *data, unsigned len, char *kid_out,
                           unsigned char *initdata, unsigned *initdata_len,
                           unsigned initdata_cap)
{
	int have_kid = 0;
	kid_out[0] = 0;
	*initdata_len = 0;
	brm_scan_boxes((const unsigned char *)data, len, kid_out, &have_kid,
	               initdata, initdata_len, initdata_cap);
	return have_kid;
}
