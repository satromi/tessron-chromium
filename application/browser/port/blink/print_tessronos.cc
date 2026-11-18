/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	print_tessronos.cc
 *	ブラウザ: 印刷 of a page (design 14.7.5).
 *
 *	The page is laid out in Blink's print mode for the printable area of
 *	the paper the system prints on (A4 with margins of 20 mm, one CSS
 *	pixel a pixel of 96 dpi, as a figure's pixels are), and each page is
 *	painted as it is for a printer. Its letters are kept as letters: each
 *	run of them that is drawn upright, in one colour, becomes a piece of
 *	text of the page's figure (its letters, the face, size and colour,
 *	where it stands), which the desktop writes into the PDF as text of
 *	that face; the rest -- boxes, lines, pictures, letters drawn any
 *	other way -- is rastered into the picture under them. The
 *	pages go to the desktop as 印刷 from a process does
 *	(port/net/br_print.cc): the desktop's panels say how, and it writes
 *	the PDF. beforeprint and afterprint are told to the page around it.
 */

#include <stdio.h>

#include <atomic>
#include <string>
#include <vector>

#include "cc/paint/paint_record.h"
#include "third_party/blink/public/web/web_print_params.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/dom/events/event.h"
#include "third_party/blink/renderer/core/events/after_print_event.h"
#include "third_party/blink/renderer/core/events/before_print_event.h"
#include "third_party/blink/renderer/core/frame/frame.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/local_frame_view.h"
#include "third_party/blink/renderer/core/layout/layout_view.h"
#include "third_party/blink/renderer/core/page/print_context.h"
#include "third_party/blink/renderer/core/paint/paint_layer_scrollable_area.h"
#include "third_party/blink/renderer/platform/graphics/graphics_context.h"
#include "third_party/blink/renderer/platform/graphics/paint/cull_rect.h"
#include "third_party/blink/renderer/platform/graphics/paint/paint_record_builder.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkFont.h"
#include "third_party/skia/include/core/SkFontMetrics.h"
#include "third_party/skia/include/core/SkStream.h"
#include "third_party/skia/include/core/SkString.h"
#include "third_party/skia/include/core/SkTextBlob.h"
#include "third_party/skia/include/core/SkTypeface.h"
#include "third_party/skia/include/encode/SkPngEncoder.h"
#include "third_party/skia/src/core/SkFontPriv.h"

/*
 * The pages sent to the desktop to print (port/net/br_print.cc): each
 * page's picture, and the pieces of text that go over it (xmlTAD of the
 * page's figure, at the picture's place)
 */
bool br_print_send( const std::string &name, const std::vector<std::vector<uint8_t>> &pages,
		    const std::vector<std::string> &texts, int width, int height );

/* The runs of letters printed as letters, for what 印刷 came to (br_print_report) */
std::atomic<int> g_print_texts{0};

namespace {

/* A4 (595 by 842 points) less margins of 57 points, in pixels of 96 dpi */
constexpr int	kPageW = ( 595 - 2 * 57 ) * 96 / 72;
constexpr int	kPageH = ( 842 - 2 * 57 ) * 96 / 72;
constexpr int	kMarginPx = 57 * 4 / 3;		/* where the picture stands on the page's figure */

/* < > & and " written as xmlTAD wants them */
void escape( std::string &out, const std::string &s )
{
	for ( char c : s ) {
		switch ( c ) {
		case '<': out += "&lt;"; break;
		case '>': out += "&gt;"; break;
		case '&': out += "&amp;"; break;
		case '"': out += "&quot;"; break;
		default: out += c; break;
		}
	}
}

/*
 * A canvas the page is played back into: everything drawn into its
 * pixels except the runs of letters it can keep as letters, which become
 * pieces of text (xmlTAD, at x0, y0 on the figure). A run is kept when
 * it stands upright (only scaled and moved), is filled in one colour,
 * and every glyph of it is a letter of its face; else it is drawn.
 */
class TextTaker final : public SkCanvas {
public:
	TextTaker( const SkBitmap &bitmap, int x0, int y0 ) : SkCanvas(bitmap), x0_(x0), y0_(y0) {}

	const std::string &Texts() const { return xml_; }
	int Runs() const { return runs_; }

protected:
	void onDrawTextBlob( const SkTextBlob *blob, SkScalar x, SkScalar y, const SkPaint &paint ) override
	{
		if ( !Take(blob, x, y, paint) ) SkCanvas::onDrawTextBlob(blob, x, y, paint);
	}

private:
	bool Take( const SkTextBlob *blob, SkScalar x, SkScalar y, const SkPaint &paint )
	{
		const SkMatrix m = getTotalMatrix();
		if ( blob == nullptr || !m.isScaleTranslate() || m.getScaleX() <= 0 || m.getScaleY() <= 0
		     || paint.getShader() != nullptr || paint.getMaskFilter() != nullptr
		     || paint.getStyle() != SkPaint::kFill_Style ) {
			return false;
		}
		SkColor col = paint.getColor();
		if ( SkColorGetA(col) == 0 ) return true;	/* not seen: nothing to keep */
		SkIRect clip = getDeviceClipBounds();
		std::string out;
		int n = 0;
		SkTextBlob::Iter it(*blob);
		SkTextBlob::Iter::ExperimentalRun run;
		while ( it.experimentalNext(&run) ) {
			if ( run.count <= 0 ) continue;
			std::vector<SkUnichar> uni((size_t)run.count);
			SkFontPriv::GlyphsToUnichars(run.font, run.glyphs, run.count, uni.data());
			std::string text;
			for ( SkUnichar u : uni ) {
				if ( u <= 0 || u > 0x10FFFF ) return false;	/* a glyph that is no letter */
				SkString one;
				one.appendUnichar(u);
				text.append(one.c_str(), one.size());
			}
			SkScalar w = 0;
			run.font.getWidths(SkSpan<const SkGlyphID>(&run.glyphs[run.count - 1], 1), SkSpan<SkScalar>(&w, 1));
			SkFontMetrics fm;
			run.font.getMetrics(&fm);
			SkPoint a = m.mapPoint({ x + run.positions[0].x(), y + run.positions[0].y() + fm.fAscent });
			SkPoint b = m.mapPoint({ x + run.positions[run.count - 1].x() + w, y + run.positions[0].y() + fm.fDescent });
			if ( b.x() < clip.left() || a.x() > clip.right() || b.y() < clip.top() || a.y() > clip.bottom() ) {
				continue;					/* clipped away: not seen */
			}
			int size = (int)( run.font.getSize() * m.getScaleY() + 0.5f );
			if ( size <= 0 ) continue;
			int l = x0_ + (int)a.x(), t = y0_ + (int)a.y();
			int r = x0_ + (int)( b.x() + 0.5f ) + size / 2, bt = y0_ + (int)( b.y() + 0.5f ) + 1;
			SkString family;
			bool bold = false, italic = false;
			if ( SkTypeface *tf = run.font.getTypeface() ) {
				tf->getFamilyName(&family);
				bold = tf->fontStyle().weight() >= SkFontStyle::kSemiBold_Weight || run.font.isEmbolden();
				italic = tf->fontStyle().slant() != SkFontStyle::kUpright_Slant || run.font.getSkewX() != 0;
			}
			char num[200];
			snprintf(num, sizeof(num), "<document>\n<docView viewleft=\"%d\" viewtop=\"%d\" viewright=\"%d\" viewbottom=\"%d\"/>\n"
				 "<docDraw drawleft=\"%d\" drawtop=\"%d\" drawright=\"%d\" drawbottom=\"%d\"/>\n",
				 l, t, r, bt, l, t, r, bt);
			out += num;
			snprintf(num, sizeof(num), "<docScale hunit=\"-96\" vunit=\"-96\"/>\n<text lang=\"0\" bpat=\"0\"/>\n"
				 "<font size=\"%d\"/>\n<font color=\"#%06x\"/>\n", size, (unsigned)( col & 0xFFFFFF ));
			out += num;
			if ( family.size() > 0 ) {
				out += "<font face=\"";
				escape(out, std::string(family.c_str(), family.size()));
				out += "\"/>\n";
			}
			if ( bold ) out += "<bold>";
			if ( italic ) out += "<italic>";
			escape(out, text);
			if ( italic ) out += "</italic>";
			if ( bold ) out += "</bold>";
			out += "\n</document>\n";
			n++;
		}
		xml_ += out;
		runs_ += n;
		return true;
	}

	int		x0_, y0_;
	std::string	xml_;
	int		runs_ = 0;
};

class PagePrinter final : public blink::PrintContext {
public:
	explicit PagePrinter( blink::LocalFrame *frame ) : blink::PrintContext(frame) {}

	/*
	 * A page painted as for a printer, into a picture of the printable
	 * area and the pieces of text over it (*texts); empty when it cannot be
	 */
	std::vector<uint8_t> Page( blink::wtf_size_t index, std::string *texts )
	{
		std::vector<uint8_t> png;
		blink::LocalFrameView *view = GetFrame()->View();

		if ( !IsFrameValid() || view == nullptr || !view->UpdateLifecyclePhasesForPrinting()
		     || index >= PageCount() ) {
			return png;
		}
		gfx::Rect page_rect = PageRect(index);
		blink::PaintRecordBuilder outer;
		blink::GraphicsContext &context = outer.Context();
		context.SetPrinting(true);
		context.BeginRecording();
		/* the scroll offset of the screen is not the page's */
		gfx::Vector2d offset = view->LayoutViewport()->ScrollOffsetInt();
		context.Save();
		context.Translate((float)offset.x(), (float)offset.y());
		blink::PaintRecordBuilder builder(context);
		view->PrintPage(builder.Context(), index, blink::CullRect(page_rect));
		auto state = view->GetLayoutView()->FirstFragment().LocalBorderBoxProperties();
		context.DrawRecord(builder.EndRecording(state.Unalias()));
		context.Restore();
		cc::PaintRecord record = context.EndRecording();

		SkBitmap bitmap;
		if ( !bitmap.tryAllocN32Pixels(kPageW, kPageH, true) ) return png;
		TextTaker canvas(bitmap, kMarginPx, kMarginPx);
		canvas.clear(SK_ColorWHITE);
		record.Playback(&canvas);
		*texts = canvas.Texts();
		g_print_texts += canvas.Runs();
		SkDynamicMemoryWStream out;
		if ( !SkPngEncoder::Encode(&out, bitmap.pixmap(), {}) ) return png;
		png.resize(out.bytesWritten());
		out.copyTo(png.data());
		return png;
	}
};

/*
 * beforeprint or afterprint told to the frame and the frames within it:
 * events that reach the page's script while window.print() holds it
 */
void tell( blink::LocalFrame *top, bool before )
{
	blink::HeapVector<blink::Member<blink::Frame>> frames;
	for ( blink::Frame *f = top; f != nullptr; f = f->Tree().TraverseNext(top) ) frames.push_back(f);
	if ( before ) top->GetDocument()->SetPrinting(blink::Document::kBeforePrinting);
	for ( auto &f : frames ) {
		auto *lf = blink::DynamicTo<blink::LocalFrame>(f.Get());
		if ( lf == nullptr || lf->DomWindow() == nullptr || !lf->Tree().IsDescendantOf(top) ) continue;
		blink::Event *e = before ? static_cast<blink::Event *>(blink::MakeGarbageCollected<blink::BeforePrintEvent>())
					 : static_cast<blink::Event *>(blink::MakeGarbageCollected<blink::AfterPrintEvent>());
		lf->DomWindow()->DispatchEvent(*e);
	}
	if ( before && top->GetDocument() != nullptr ) top->GetDocument()->SetPrinting(blink::Document::kNotPrinting);
}

}  // namespace

/*
 * 印刷 of the page the frame shows: its pages to the desktop under the
 * page's title (or 'fallback'); false when there was nothing to print or
 * the desktop did not print it
 */
bool br_print_frame( blink::LocalFrame *frame, const std::string &fallback )
{
	if ( frame == nullptr || frame->GetDocument() == nullptr || frame->View() == nullptr ) return false;
	std::string name = frame->GetDocument()->title().Utf8();
	if ( name.empty() ) name = fallback;
	if ( name.empty() ) name = "ページ";

	tell(frame, true);
	auto *printer = blink::MakeGarbageCollected<PagePrinter>(frame);
	blink::WebPrintParams params(gfx::SizeF(kPageW, kPageH));
	printer->BeginPrintMode(params);
	std::vector<std::vector<uint8_t>> pages;
	std::vector<std::string> texts;
	blink::wtf_size_t n = printer->PageCount();
	for ( blink::wtf_size_t i = 0; i < n; i++ ) {
		std::string t;
		std::vector<uint8_t> png = printer->Page(i, &t);
		if ( png.empty() ) break;
		pages.push_back(std::move(png));
		texts.push_back(std::move(t));
	}
	printer->EndPrintMode();
	tell(frame, false);
	printf("browser: print: %zu of %u pages, %d by %d\n", pages.size(), (unsigned)n, kPageW, kPageH);
	if ( pages.empty() ) return false;
	return br_print_send(name, pages, texts, kPageW, kPageH);
}
