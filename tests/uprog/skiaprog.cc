/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	skiaprog.cc
 *	Skia's raster in a window of a TessronOS process (design 17.19): a
 *	gradient, a rectangle, an anti-aliased circle and path, and letters
 *	in the system's Japanese face (read from the face's object, rendered
 *	by Skia's FreeType) are drawn into a Skia surface, the pixels put in
 *	the window with dp_put_argb and shown. The program checks what it
 *	drew; its exit code is 0 when the colours are where they must be
 *	(the test reads the window on the screen itself). The start-up
 *	argument, when there is one, is a SKARG: how long the window stays
 *	up (ms), and a file to make once it is shown, which the test waits
 *	for before it takes a picture of the screen. Built by
 *	tools/browser/build.sh (make BROWSER=1), run by tests/ktest/ktest_v8.c.
 */

#include TS_CONFIG_H	/* the system's, ahead of any other config.h */
#include <stdint.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/uapp.h>
#include <ts/face.h>

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <vector>

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkData.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkRRect.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTypeface.h"
#include "include/effects/SkGradient.h"
#include "include/ports/SkFontMgr_data.h"

/* The system's sans-serif Japanese face (etc/def, the font box) */
static const char kFaceName[] = "NotoSansJP-Regular";

typedef struct {
	UW	stay;
	char	ready[60];
} SKARG;

static const char kWinJson[] = "{\"rect\":[80,60,620,460],\"attr\":7}";

static long ms_since( const struct timespec *t0 )
{
	struct timespec t1;

	clock_gettime(CLOCK_MONOTONIC, &t1);
	return ( t1.tv_sec - t0->tv_sec ) * 1000 + ( t1.tv_nsec - t0->tv_nsec ) / 1000000;
}

/* Record 1 of the face's object, whole */
static sk_sp<SkData> read_face()
{
	std::vector<unsigned char>	buf;
	const SZ			piece = 256 * 1024;
	SZ				got = 0, asz = 0;
	TS_UUID				face;
	ID				k = E_NOEXS;

	if ( ts_face_find(kFaceName, &face) >= E_OK ) {
		k = ob_opn_obj(&face, OB_OP_READ);
	}
	if ( k <= 0 ) {
		printf("skiaprog: no face object (%d)\n", (int)k);
		return nullptr;
	}
	for ( ;; ) {
		buf.resize((size_t)( got + piece ));
		asz = 0;
		if ( ob_rea_rec(k, 1, (D)got, buf.data() + got, piece, &asz) < E_OK ) break;
		got += asz;
		if ( asz < piece ) break;
	}
	ob_cls_obj(k);
	if ( got <= 0 ) return nullptr;
	return SkData::MakeWithCopy(buf.data(), (size_t)got);
}

static void draw( SkCanvas *c, int w, int h, sk_sp<SkTypeface> face )
{
	SkPaint	p;

	c->clear(SK_ColorWHITE);

	/* a gradient band across the top */
	SkPoint		pts[2] = { { 0, 0 }, { (SkScalar)w, 0 } };
	SkColor4f	cols[3] = { SkColor4f::FromColor(0xFF2060C0), SkColor4f::FromColor(0xFF20C0A0),
				    SkColor4f::FromColor(0xFFF0C040) };
	SkGradient	grad(SkGradient::Colors(SkSpan<const SkColor4f>(cols, 3), SkTileMode::kClamp),
			     SkGradient::Interpolation());
	p.setShader(SkShaders::LinearGradient(pts, grad));
	c->drawRect(SkRect::MakeXYWH(0, 0, (SkScalar)w, 40), p);
	p.setShader(nullptr);

	/* a solid red rectangle: its middle is checked */
	p.setAntiAlias(false);
	p.setColor(SK_ColorRED);
	c->drawRect(SkRect::MakeXYWH(20, 60, 120, 80), p);

	/* an anti-aliased blue disc and a rounded green frame */
	p.setAntiAlias(true);
	p.setColor(SK_ColorBLUE);
	c->drawCircle(220, 100, 40, p);
	p.setStyle(SkPaint::kStroke_Style);
	p.setStrokeWidth(4);
	p.setColor(0xFF008000);
	c->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(290, 60, 180, 80), 16, 16), p);

	/* a star as a path */
	SkPathBuilder	b;
	for ( int i = 0; i < 5; i++ ) {
		double	a = -3.14159265 / 2 + i * 4 * 3.14159265 / 5;
		SkPoint	q = { (SkScalar)( 80 + 50 * cos(a) ), (SkScalar)( 230 + 50 * sin(a) ) };
		if ( i == 0 ) b.moveTo(q); else b.lineTo(q);
	}
	b.close();
	p.setStyle(SkPaint::kFill_Style);
	p.setColor(0xFFE08000);
	c->drawPath(b.detach(), p);

	/* letters */
	if ( face != nullptr ) {
		SkFont	f(face, 28);
		p.setColor(SK_ColorBLACK);
		f.setEdging(SkFont::Edging::kAntiAlias);
		c->drawString("TessronOS ブラウザ", 150, 220, f, p);
		f.setSize(18);
		c->drawString("Skia の CPU ラスタで描いた窓", 150, 260, f, p);
	}
	(void)h;
}

int main()
{
	T_OBCRE		c;
	T_OBWPOS	pos;
	TS_UUID		win;
	struct timespec	t0;
	SZ		asz = 0;
	SKARG		arg;
	INT		gid;
	ID		kw;
	int		rc = 0, w, h;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	memset(&arg, 0, sizeof(arg));
	arg.stay = 3000;
	if ( ts_get_arg(nullptr, 0) >= (SZ)sizeof(UW) ) {
		(void)ts_get_arg(&arg, sizeof(arg));
		arg.ready[sizeof(arg.ready) - 1] = 0;
	}

	memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (CONST UB *)"Skia";
	c.json = (CONST UB *)kWinJson;
	c.jsonsz = (SZ)strlen(kWinJson);
	if ( ob_cre_obj(&c, &win) < E_OK ) {
		printf("skiaprog: no window\n");
		return 1;
	}
	kw = ob_opn_obj(&win, OB_OP_ALL);
	gid = ( kw > 0 ) ? wm_obj_gid(kw) : E_ID;
	memset(&pos, 0, sizeof(pos));
	if ( gid < 0 || ob_rea_rec(kw, OB_WR_PLACE, 0, &pos, sizeof(pos), &asz) < E_OK ) {
		printf("skiaprog: the window cannot be drawn in\n");
		(void)ob_del_obj(&win);
		return 2;
	}
	w = pos.wright - pos.wleft;
	h = pos.wbottom - pos.wtop;
	if ( w > 520 ) w = 520;
	if ( h > 360 ) h = 360;
	printf("skiaprog: window %dx%d\n", w, h);

	sk_sp<SkData>		data = read_face();
	sk_sp<SkFontMgr>	mgr;
	sk_sp<SkTypeface>	face;
	if ( data != nullptr ) {
		mgr = SkFontMgr_New_Custom_Data(SkSpan<sk_sp<SkData>>(&data, 1));
		face = mgr->makeFromData(data);
		printf("skiaprog: face %zu bytes, %s\n", data->size(), face ? "loaded" : "not loaded");
	}

	SkImageInfo		info = SkImageInfo::MakeN32Premul(w, h);
	sk_sp<SkSurface>	surf = SkSurfaces::Raster(info);
	struct timespec		t1;

	clock_gettime(CLOCK_MONOTONIC, &t1);
	draw(surf->getCanvas(), w, h, face);
	printf("skiaprog: drawn in %ld ms\n", ms_since(&t1));

	SkPixmap pm;
	if ( !surf->peekPixels(&pm) ) {
		printf("skiaprog: no pixels\n");
		(void)ob_del_obj(&win);
		return 3;
	}

	/* N32 here is BGRA in memory: as a 32 bit word 0xAARRGGBB, the window's 0x00RRGGBB */
	std::vector<UW>	px((size_t)w * h);
	for ( int y = 0; y < h; y++ ) {
		const uint32_t *row = pm.addr32(0, y);
		for ( int x = 0; x < w; x++ ) px[(size_t)y * w + x] = row[x] & 0x00FFFFFF;
	}
	(void)dp_put_argb(gid, 0, 0, px.data(), w, w, h, 0xFF000000);
	(void)wm_obj_flush(kw, nullptr);

	/* what was put in the window */
	UW	back[3];
	int	dark = 0;
	back[0] = px[(size_t)100 * w + 70];		/* in the red rectangle */
	back[1] = px[(size_t)100 * w + 220];		/* the disc's centre */
	back[2] = px[(size_t)20 * w + 2];		/* the gradient's left */
	for ( int y = 196; y < 226; y++ ) {
		for ( int x = 150; x < 400 && x < w; x++ ) {
			if ( ( px[(size_t)y * w + x] & 0xFF ) < 0x40 ) dark++;
		}
	}
	printf("skiaprog: red %06x blue %06x gradient %06x, %d dark pixels of letters\n",
	       (unsigned)back[0], (unsigned)back[1], (unsigned)back[2], dark);
	if ( back[0] != 0xFF0000 ) rc = 4;
	else if ( back[1] != 0x0000FF ) rc = 5;
	else if ( ( back[2] & 0xFF ) < 0x80 ) rc = 6;
	else if ( face != nullptr && dark < 200 ) rc = 7;

	if ( arg.ready[0] != 0 ) {
		/* the file layer's write, create and truncate (include/ts/fs.h) */
		INT fd = fs_open(arg.ready, 0x0001 | 0x0100 | 0x0400);
		if ( fd >= 0 ) fs_close(fd);
	}
	tk_dly_tsk(arg.stay);
	ob_cls_obj(kw);
	(void)ob_del_obj(&win);
	printf("skiaprog: done %d in %ld ms\n", rc, ms_since(&t0));
	return rc;
}
