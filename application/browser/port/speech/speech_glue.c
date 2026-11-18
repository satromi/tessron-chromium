/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	speech_glue.c
 *	ブラウザ: the speech synthesizers and the recognizer behind a few
 *	calls (design 14.4.15), compiled into libspeech.a with them by
 *	tools/browser/speech.sh.
 *
 *	  - Japanese: Open JTalk (MeCab with naist-jdic, the NJD rules, the
 *	    full-context labels) and hts_engine with the voice of NIT, 48 kHz;
 *	  - English: Flite with the voice cmu_us_slt, 16 kHz;
 *	  - recognition: whisper.cpp with the tiny model, on 16 kHz mono.
 *
 *	The files they read (the dictionary, the voice, the model) are the
 *	browser's (br_speech_file, port/blink/speech_tessronos.cc). Each is
 *	made once, on the first call, and the calls are made one at a time.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mecab.h"
#include "njd.h"
#include "jpcommon.h"
#include "HTS_engine.h"
#include "text2mecab.h"
#include "mecab2njd.h"
#include "njd_set_pronunciation.h"
#include "njd_set_digit.h"
#include "njd_set_accent_phrase.h"
#include "njd_set_accent_type.h"
#include "njd_set_unvoiced_vowel.h"
#include "njd_set_long_vowel.h"
#include "njd2jpcommon.h"

#include "flite.h"

#include "whisper.h"

int br_speech_file( const char *name, const void **data, unsigned long *len );
cst_voice *register_cmu_us_slt( const char *voxdir );

/* ---------------------------------------------------------------- Japanese */

static int		ja_up;
static Mecab		ja_mecab;
static NJD		ja_njd;
static JPCommon		ja_jpcommon;
static HTS_Engine	ja_engine;

static int ja_load( void )
{
	char *voices[1] = { (char *)"mem:voice.htsvoice" };

	if ( ja_up ) return ja_up > 0;
	ja_up = -1;
	Mecab_initialize(&ja_mecab);
	NJD_initialize(&ja_njd);
	JPCommon_initialize(&ja_jpcommon);
	HTS_Engine_initialize(&ja_engine);
	if ( Mecab_load(&ja_mecab, "mem") != TRUE ) {
		fprintf(stderr, "speech: the dictionary is not read: %s\n", mecab_strerror(NULL));
		return 0;
	}
	if ( HTS_Engine_load(&ja_engine, voices, 1) != TRUE ) {
		fprintf(stderr, "speech: the voice is not read\n");
		return 0;
	}
	ja_up = 1;
	return 1;
}

/*
 * Japanese text as 16-bit samples at *rate (the voice's, 48 kHz): the
 * buffer from malloc, its samples in *n; 0 when nothing is said
 */
int br_tts_ja( const char *utf8, double speed, double pitch, double volume, short **pcm, int *n, int *rate )
{
	char	*buf;
	size_t	len = strlen(utf8), i, ns;
	int	ok = 0;

	*pcm = NULL;
	*n = 0;
	if ( !ja_load() ) return 0;
	buf = (char *)malloc(len * 4 + 16);
	if ( buf == NULL ) return 0;
	text2mecab(buf, utf8);
	Mecab_analysis(&ja_mecab, buf);
	mecab2njd(&ja_njd, Mecab_get_feature(&ja_mecab), Mecab_get_size(&ja_mecab));
	njd_set_pronunciation(&ja_njd);
	njd_set_digit(&ja_njd);
	njd_set_accent_phrase(&ja_njd);
	njd_set_accent_type(&ja_njd);
	njd_set_unvoiced_vowel(&ja_njd);
	njd_set_long_vowel(&ja_njd);
	njd2jpcommon(&ja_jpcommon, &ja_njd);
	JPCommon_make_label(&ja_jpcommon);
	HTS_Engine_set_speed(&ja_engine, speed > 0.1 ? speed : 1.0);
	HTS_Engine_set_volume(&ja_engine, volume > 0 ? 20.0 * log10(volume) : -100.0);
	HTS_Engine_add_half_tone(&ja_engine, 12.0 * log2(pitch > 0.05 ? pitch : 1.0));
	if ( JPCommon_get_label_size(&ja_jpcommon) > 2
	     && HTS_Engine_synthesize_from_strings(&ja_engine, JPCommon_get_label_feature(&ja_jpcommon),
						   JPCommon_get_label_size(&ja_jpcommon)) == TRUE ) {
		ns = HTS_Engine_get_nsamples(&ja_engine);
		*pcm = (short *)malloc(ns * sizeof(short) + 2);
		if ( *pcm != NULL ) {
			for ( i = 0; i < ns; i++ ) {
				double v = HTS_Engine_get_generated_speech(&ja_engine, i);
				(*pcm)[i] = (short)( v > 32767 ? 32767 : v < -32768 ? -32768 : v );
			}
			*n = (int)ns;
			*rate = (int)HTS_Engine_get_sampling_frequency(&ja_engine);
			ok = 1;
		}
		HTS_Engine_refresh(&ja_engine);
	}
	/* the pitch and speed back as the voice has them, for the next */
	HTS_Engine_add_half_tone(&ja_engine, -12.0 * log2(pitch > 0.05 ? pitch : 1.0));
	JPCommon_refresh(&ja_jpcommon);
	NJD_refresh(&ja_njd);
	Mecab_refresh(&ja_mecab);
	free(buf);
	return ok;
}

/* ---------------------------------------------------------------- English */

static cst_voice	*en_voice;

int br_tts_en( const char *utf8, double speed, double pitch, double volume, short **pcm, int *n, int *rate )
{
	cst_wave	*w;
	int		i;

	*pcm = NULL;
	*n = 0;
	if ( en_voice == NULL ) {
		flite_init();
		en_voice = register_cmu_us_slt(NULL);
		if ( en_voice == NULL ) return 0;
	}
	feat_set_float(en_voice->features, "duration_stretch", (float)( speed > 0.1 ? 1.0 / speed : 1.0 ));
	feat_set_float(en_voice->features, "int_f0_target_mean", (float)( 170.0 * ( pitch > 0.05 ? pitch : 1.0 ) ));
	w = flite_text_to_wave(utf8, en_voice);
	if ( w == NULL ) return 0;
	*pcm = (short *)malloc(w->num_samples * sizeof(short) + 2);
	if ( *pcm != NULL ) {
		for ( i = 0; i < w->num_samples; i++ ) {
			double v = w->samples[i] * ( volume >= 0 ? volume : 1.0 );
			(*pcm)[i] = (short)( v > 32767 ? 32767 : v < -32768 ? -32768 : v );
		}
		*n = w->num_samples;
		*rate = w->sample_rate;
	}
	delete_wave(w);
	return *pcm != NULL;
}

/* ---------------------------------------------------------------- recognition */

static struct whisper_context	*stt_ctx;

/*
 * What was said in 16 kHz mono samples, as UTF-8 in out (cap bytes), the
 * language "ja", "en", or "" to be told: 1, or 0 when nothing is heard
 */
int br_stt( const float *samples, int n, const char *lang, int threads, char *out, int cap )
{
	struct whisper_full_params	p;
	const void			*model;
	unsigned long			len;
	int				i, k, at = 0;

	out[0] = '\0';
	if ( stt_ctx == NULL ) {
		struct whisper_context_params cp = whisper_context_default_params();
		cp.use_gpu = 0;
		if ( !br_speech_file("whisper.bin", &model, &len) ) return 0;
		stt_ctx = whisper_init_from_buffer_with_params((void *)model, (size_t)len, cp);
		if ( stt_ctx == NULL ) return 0;
	}
	p = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
	p.n_threads = threads > 0 ? threads : 1;
	p.language = ( lang != NULL && lang[0] != '\0' ) ? lang : "auto";
	p.translate = 0;
	p.no_context = 1;
	p.single_segment = 0;
	p.print_progress = 0;
	p.print_realtime = 0;
	p.print_timestamps = 0;
	p.print_special = 0;
	/*
	 * The encoder made to look at the recording's length (20 ms a step)
	 * and a little more, not at its whole window of 30 seconds: a few
	 * seconds of speech are heard in a fraction of the time
	 */
	p.audio_ctx = n / 320 + 64;
	if ( p.audio_ctx > 1500 ) p.audio_ctx = 0;
	if ( whisper_full(stt_ctx, p, samples, n) != 0 ) return 0;
	k = whisper_full_n_segments(stt_ctx);
	for ( i = 0; i < k; i++ ) {
		const char *t = whisper_full_get_segment_text(stt_ctx, i);
		size_t l = strlen(t);
		if ( at + (int)l + 1 >= cap ) break;
		memcpy(out + at, t, l);
		at += (int)l;
	}
	out[at] = '\0';
	return at > 0;
}
