/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	pdf_tessronos.cc
 *	ブラウザ: PDF documents shown in the page (design 14.6.6).
 *
 *	A response of type application/pdf (the main frame's or an iframe's)
 *	is drawn by PDFium, page after page, at 96 dots an inch, and the
 *	document committed in its place is an HTML page of those pictures on
 *	a grey ground, with the PDF's title. 印刷 prints that page as it
 *	prints any other. PDFium is the library Chrome's PDF viewer draws
 *	with; its JavaScript and forms are not run.
 *
 *	The browser says it can show PDF (navigator.pdfViewerEnabled and the
 *	plugins and MIME types Chrome lists): the plugin registry answers
 *	one plugin that takes application/pdf and text/pdf.
 */

#include <stdio.h>
#include <string.h>

#include <mutex>
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "third_party/blink/public/mojom/plugins/plugin_registry.mojom-blink.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "third_party/pdfium/public/fpdf_doc.h"
#include "third_party/pdfium/public/fpdfview.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkData.h"
#include "third_party/skia/include/core/SkStream.h"
#include "third_party/skia/include/encode/SkPngEncoder.h"

void br_on_service_thread( base::OnceClosure task );

namespace {

namespace bm = blink::mojom::blink;

/* the pages drawn at most, and their width at 96 dots an inch: a point is 1/72 inch */
constexpr int		kPagesMax = 60;
constexpr double	kScale = 96.0 / 72.0;

std::once_flag	g_pdfium_once;

void pdfium_up()
{
	std::call_once(g_pdfium_once, [] {
		FPDF_LIBRARY_CONFIG c;
		memset(&c, 0, sizeof(c));
		c.version = 2;
		FPDF_InitLibraryWithConfig(&c);
	});
}

std::string html_text( const std::string &s )
{
	std::string o;
	for ( char c : s ) {
		if ( c == '<' ) o += "&lt;";
		else if ( c == '>' ) o += "&gt;";
		else if ( c == '&' ) o += "&amp;";
		else if ( c == '"' ) o += "&quot;";
		else o += c;
	}
	return o;
}

/* The document's title from its information dictionary, UTF-8; "" when it has none */
std::string pdf_title( FPDF_DOCUMENT doc )
{
	unsigned long n = FPDF_GetMetaText(doc, "Title", nullptr, 0);
	if ( n <= 2 ) return std::string();
	std::vector<unsigned short> w(n / 2 + 1, 0);
	FPDF_GetMetaText(doc, "Title", w.data(), n);
	std::u16string u(w.begin(), w.begin() + (long)( n / 2 - 1 ));
	return base::UTF16ToUTF8(u);
}

/* One page drawn on white and made a PNG; "" when it could not be */
std::string page_png( FPDF_DOCUMENT doc, int i, int *w_out, int *h_out )
{
	FPDF_PAGE page = FPDF_LoadPage(doc, i);
	if ( page == nullptr ) return std::string();
	int w = (int)( FPDF_GetPageWidthF(page) * kScale + 0.5 ), h = (int)( FPDF_GetPageHeightF(page) * kScale + 0.5 );
	std::string png;
	if ( w > 0 && h > 0 && w <= 8000 && h <= 8000 ) {
		FPDF_BITMAP bmp = FPDFBitmap_Create(w, h, 0);
		if ( bmp != nullptr ) {
			FPDFBitmap_FillRect(bmp, 0, 0, w, h, 0xFFFFFFFF);
			FPDF_RenderPageBitmap(bmp, page, 0, 0, w, h, 0, FPDF_ANNOT);
			SkBitmap sk;
			sk.installPixels(SkImageInfo::Make(w, h, kBGRA_8888_SkColorType, kOpaque_SkAlphaType),
					 FPDFBitmap_GetBuffer(bmp), (size_t)FPDFBitmap_GetStride(bmp));
			SkDynamicMemoryWStream out;
			SkPixmap pm;
			if ( sk.peekPixels(&pm) && SkPngEncoder::Encode(&out, pm, SkPngEncoder::Options()) ) {
				sk_sp<SkData> d = out.detachAsData();
				png.assign((const char *)d->data(), d->size());
			}
			FPDFBitmap_Destroy(bmp);
		}
	}
	FPDF_ClosePage(page);
	*w_out = w;
	*h_out = h;
	return png;
}

/* navigator.plugins: the one plugin a browser that shows PDF has */
class Plugins final : public bm::PluginRegistry {
public:
	void Bind( mojo::PendingReceiver<bm::PluginRegistry> r ) { receivers_.Add(this, std::move(r)); }

	void GetPlugins( GetPluginsCallback cb ) override
	{
		blink::Vector<bm::PluginInfoPtr> list;
		auto p = bm::PluginInfo::New();
		p->name = blink::String("Chromium PDF Viewer");
		p->description = blink::String("Portable Document Format");
		p->filename = base::FilePath(FILE_PATH_LITERAL("internal-pdf-viewer"));
		p->background_color = 0xFF525659;
		for ( const char *t : { "application/pdf", "text/pdf" } ) {
			auto m = bm::PluginMimeType::New();
			m->mime_type = blink::String(t);
			m->description = blink::String("Portable Document Format");
			m->file_extensions.push_back(blink::String("pdf"));
			p->mime_types.push_back(std::move(m));
		}
		list.push_back(std::move(p));
		std::move(cb).Run(std::move(list));
	}

private:
	mojo::ReceiverSet<bm::PluginRegistry>	receivers_;
};

Plugins	*g_plugins;

}  // namespace

/*
 * The HTML page a PDF is shown as (its pages as pictures, its title), ""
 * when PDFium does not read the bytes as a PDF
 */
std::string br_pdf_page( const std::string &url, const std::string &bytes )
{
	pdfium_up();
	FPDF_DOCUMENT doc = FPDF_LoadMemDocument64(bytes.data(), bytes.size(), nullptr);
	if ( doc == nullptr ) {
		printf("browser: pdf: %s is not read (%lu)\n", url.c_str(), FPDF_GetLastError());
		return std::string();
	}
	int n = FPDF_GetPageCount(doc);
	std::string title = pdf_title(doc);
	if ( title.empty() ) {
		std::string path = url.substr(0, url.find_first_of("?#"));
		title = path.substr(path.rfind('/') + 1);
	}
	std::string h = "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>" + html_text(title)
			+ "</title><style>html{background:#525659}body{margin:0;padding:8px 0}"
			  "img{display:block;margin:0 auto 8px;background:#fff;box-shadow:0 1px 4px rgba(0,0,0,.5);"
			  "max-width:calc(100% - 16px);height:auto}"
			  "p{color:#fff;font-family:sans-serif;text-align:center}</style></head><body>";
	int shown = 0;
	for ( int i = 0; i < n && i < kPagesMax; i++ ) {
		int w = 0, hh = 0;
		std::string png = page_png(doc, i, &w, &hh);
		if ( png.empty() ) continue;
		h += "<img alt=\"" + std::to_string(i + 1) + "\" width=\"" + std::to_string(w) + "\" height=\""
		     + std::to_string(hh) + "\" src=\"data:image/png;base64," + base::Base64Encode(png) + "\">";
		shown++;
	}
	if ( n > kPagesMax ) {
		h += "<p>" + std::to_string(kPagesMax) + " / " + std::to_string(n) + " ページを表示しています</p>";
	}
	h += "</body></html>";
	FPDF_CloseDocument(doc);
	printf("browser: pdf: %s, %d pages, %d shown\n", url.c_str(), n, shown);
	return h;
}

/* The plugin registry, asked for through the platform's broker (blob_tessronos.cc), bound on the services' thread */
void br_pdf_plugins_bind( mojo::ScopedMessagePipeHandle h )
{
	br_on_service_thread(base::BindOnce([]( mojo::PendingReceiver<bm::PluginRegistry> r ) {
		if ( g_plugins == nullptr ) g_plugins = new Plugins();
		g_plugins->Bind(std::move(r));
	}, mojo::PendingReceiver<bm::PluginRegistry>(std::move(h))));
}

/* The pages of a PDF: how many, -1 when PDFium does not read it */
int br_pdf_pages( const std::string &bytes )
{
	pdfium_up();
	FPDF_DOCUMENT doc = FPDF_LoadMemDocument64(bytes.data(), bytes.size(), nullptr);
	if ( doc == nullptr ) return -1;
	int n = FPDF_GetPageCount(doc);
	FPDF_CloseDocument(doc);
	return n;
}

/*
 * A PDF's pages as pictures of 'w' by 'h' (the printed area of a page),
 * each page made as large as fits and put in the middle, for 印刷
 */
std::vector<std::vector<uint8_t>> br_pdf_print_pages( const std::string &bytes, int w, int h, bool landscape )
{
	std::vector<std::vector<uint8_t>> out;
	pdfium_up();
	FPDF_DOCUMENT doc = FPDF_LoadMemDocument64(bytes.data(), bytes.size(), nullptr);
	if ( doc == nullptr ) return out;
	int n = FPDF_GetPageCount(doc);
	for ( int i = 0; i < n && i < kPagesMax; i++ ) {
		FPDF_PAGE page = FPDF_LoadPage(doc, i);
		if ( page == nullptr ) break;
		double pw = FPDF_GetPageWidthF(page), ph = FPDF_GetPageHeightF(page);
		int rot = ( landscape && ph > pw ) || ( !landscape && pw > ph ) ? 1 : 0;
		double fw = rot ? ph : pw, fh = rot ? pw : ph;
		double s = std::min(w / fw, h / fh);
		int dw = (int)( fw * s ), dh = (int)( fh * s );
		FPDF_BITMAP bmp = FPDFBitmap_Create(w, h, 0);
		if ( bmp != nullptr ) {
			FPDFBitmap_FillRect(bmp, 0, 0, w, h, 0xFFFFFFFF);
			FPDF_RenderPageBitmap(bmp, page, ( w - dw ) / 2, ( h - dh ) / 2, dw, dh, rot, FPDF_ANNOT | FPDF_PRINTING);
			SkBitmap sk;
			sk.installPixels(SkImageInfo::Make(w, h, kBGRA_8888_SkColorType, kOpaque_SkAlphaType),
					 FPDFBitmap_GetBuffer(bmp), (size_t)FPDFBitmap_GetStride(bmp));
			SkDynamicMemoryWStream png;
			SkPixmap pm;
			if ( sk.peekPixels(&pm) && SkPngEncoder::Encode(&png, pm, SkPngEncoder::Options()) ) {
				sk_sp<SkData> d = png.detachAsData();
				out.emplace_back((const uint8_t *)d->data(), (const uint8_t *)d->data() + d->size());
			}
			FPDFBitmap_Destroy(bmp);
		}
		FPDF_ClosePage(page);
	}
	FPDF_CloseDocument(doc);
	return out;
}
