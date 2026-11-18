/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	printing_tessronos.cc
 *	ブラウザ: Web Printing (navigator.printing) for the origins the user
 *	lets act as isolated web apps (design 14.4.14), on the browser's 印刷.
 *
 *	There is one printer, the desktop's 印刷: its attributes are those
 *	of the pages the browser prints (A4, one copy, colour, portrait or
 *	landscape, 96 dots an inch). A job's document, a PDF Blob, is read
 *	whole, its pages drawn by PDFium to the printed area of a page
 *	(pdf_tessronos.cc) and sent to the desktop as 印刷 sends a page's
 *	(port/net/br_print.cc); the desktop's panels say how it is printed.
 *	The job's observer is told it is processing, then completed, or
 *	aborted when the desktop did not print it.
 */

#include <stdio.h>

#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/wait.h"
#include "third_party/blink/public/mojom/blob/blob.mojom-blink.h"
#include "third_party/blink/public/mojom/printing/web_printing.mojom-blink.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

/* The services' thread (blob_tessronos.cc) */
void br_on_service_thread( base::OnceClosure task );

/* PDFium (pdf_tessronos.cc) and 印刷 (port/net/br_print.cc) */
int br_pdf_pages( const std::string &bytes );
std::vector<std::vector<uint8_t>> br_pdf_print_pages( const std::string &bytes, int w, int h, bool landscape );
bool br_print_send( const std::string &name, const std::vector<std::vector<uint8_t>> &pages,
		    const std::vector<std::string> &texts, int width, int height );

namespace {

namespace bm = blink::mojom::blink;

/* the printed area of an A4 page at 96 dots an inch, its margins of 57 points left off */
constexpr int	kPageW = ( 595 - 2 * 57 ) * 96 / 72;
constexpr int	kPageH = ( 842 - 2 * 57 ) * 96 / 72;
constexpr char	kPrinterId[] = "tessronos-print";

bm::WebPrintingMediaCollectionPtr a4()
{
	auto m = bm::WebPrintingMediaCollection::New();
	m->media_size = bm::WebPrintingMediaSize::New();
	m->media_size->x_dimension = bm::WebPrintingMediaSizeDimension::NewValue(21000);	/* hundredths of a millimetre */
	m->media_size->y_dimension = bm::WebPrintingMediaSizeDimension::NewValue(29700);
	m->media_size_name = blink::String("iso_a4_210x297mm");
	return m;
}

class Job final : public bm::WebPrintJobController {
public:
	void Cancel() override { cancelled_ = true; }
	bool cancelled_ = false;
};

/* A Blob's bytes, read whole on the calling thread (not the services') */
std::string read_blob( mojo::PendingRemote<bm::Blob> blob )
{
	mojo::ScopedDataPipeProducerHandle producer;
	mojo::ScopedDataPipeConsumerHandle consumer;
	if ( mojo::CreateDataPipe(64 * 1024, producer, consumer) != MOJO_RESULT_OK ) return std::string();
	br_on_service_thread(base::BindOnce([]( mojo::PendingRemote<bm::Blob> blob, mojo::ScopedDataPipeProducerHandle p ) {
		mojo::Remote<bm::Blob> b(std::move(blob));
		b->ReadAll(std::move(p), mojo::NullRemote());
	}, std::move(blob), std::move(producer)));
	std::string out;
	std::vector<uint8_t> buf(16384);
	for ( ;; ) {
		size_t got = 0;
		MojoResult r = consumer->ReadData(MOJO_READ_DATA_FLAG_NONE, base::span<uint8_t>(buf), got);
		if ( r == MOJO_RESULT_SHOULD_WAIT ) {
			mojo::Wait(consumer.get(), MOJO_HANDLE_SIGNAL_READABLE);
			continue;
		}
		if ( r != MOJO_RESULT_OK ) break;
		out.append((const char *)buf.data(), got);
	}
	return out;
}

class Printer final : public bm::WebPrinter {
public:
	void FetchAttributes( FetchAttributesCallback cb ) override
	{
		auto a = bm::WebPrinterAttributes::New();
		a->copies_default = 1;
		a->copies_supported = bm::WebPrintingRange::New(1, 1);
		a->media_col_default = a4();
		a->media_col_database.push_back(a4());
		a->multiple_document_handling_default = bm::WebPrintingMultipleDocumentHandling::kSeparateDocumentsCollatedCopies;
		a->multiple_document_handling_supported.push_back(
			bm::WebPrintingMultipleDocumentHandling::kSeparateDocumentsCollatedCopies);
		a->orientation_requested_default = bm::WebPrintingOrientationRequested::kPortrait;
		a->orientation_requested_supported.push_back(bm::WebPrintingOrientationRequested::kPortrait);
		a->orientation_requested_supported.push_back(bm::WebPrintingOrientationRequested::kLandscape);
		a->printer_resolution_default = gfx::Size(96, 96);
		a->printer_resolution_supported.push_back(gfx::Size(96, 96));
		a->print_color_mode_default = bm::WebPrintColorMode::kColor;
		a->print_color_mode_supported.push_back(bm::WebPrintColorMode::kColor);
		a->printer_state = bm::WebPrinterState::kIdle;
		a->printer_state_message = blink::String("");
		a->printer_state_reasons.push_back(bm::WebPrinterStateReason::kNone);
		a->sides_default = bm::WebPrintingSides::kOneSided;
		a->sides_supported.push_back(bm::WebPrintingSides::kOneSided);
		a->printer_id = blink::String(kPrinterId);
		std::move(cb).Run(std::move(a));
	}

	void Print( mojo::PendingRemote<bm::Blob> document, bm::WebPrintJobTemplateAttributesPtr attributes,
		    PrintCallback cb ) override
	{
		std::string name = attributes->job_name.Utf8();
		bool landscape = attributes->orientation_requested
				 && *attributes->orientation_requested == bm::WebPrintingOrientationRequested::kLandscape;
		std::thread([name, landscape]( mojo::PendingRemote<bm::Blob> document, PrintCallback cb ) {
			std::string pdf = read_blob(std::move(document));
			int pages = br_pdf_pages(pdf);
			printf("browser: printing: job \"%s\", %zu bytes, %d pages\n", name.c_str(), pdf.size(), pages);
			br_on_service_thread(base::BindOnce([]( std::string name, std::string pdf, int pages, bool landscape,
								PrintCallback cb ) {
				if ( pages <= 0 ) {
					std::move(cb).Run(base::unexpected(bm::WebPrintError::kDocumentMalformed));
					return;
				}
				auto info = bm::WebPrintJobInfo::New();
				info->job_name = blink::String::FromUtf8(name);
				info->job_pages = (uint32_t)pages;
				mojo::PendingRemote<bm::WebPrintJobStateObserver> observer;
				info->observer = observer.InitWithNewPipeAndPassReceiver();
				auto job = std::make_unique<Job>();
				Job *jp = job.get();
				mojo::MakeSelfOwnedReceiver(std::move(job), info->controller.InitWithNewPipeAndPassReceiver());
				std::move(cb).Run(std::move(info));
				auto obs = std::make_shared<mojo::Remote<bm::WebPrintJobStateObserver>>(std::move(observer));
				( *obs )->OnWebPrintJobUpdate(bm::WebPrintJobUpdate::New(bm::WebPrintJobState::kProcessing, 0));
				(void)jp;
				std::thread([name, pdf, pages, landscape, obs]() {
					std::vector<std::vector<uint8_t>> pics = br_pdf_print_pages(pdf, landscape ? kPageH : kPageW,
												    landscape ? kPageW : kPageH, landscape);
					bool ok = !pics.empty() && br_print_send(name.empty() ? std::string("印刷") : name, pics,
										 std::vector<std::string>(pics.size()),
										 landscape ? kPageH : kPageW,
										 landscape ? kPageW : kPageH);
					printf("browser: printing: \"%s\" %s\n", name.c_str(), ok ? "printed" : "not printed");
					br_on_service_thread(base::BindOnce([]( std::shared_ptr<mojo::Remote<bm::WebPrintJobStateObserver>> obs,
										bool ok, int pages ) {
						( *obs )->OnWebPrintJobUpdate(bm::WebPrintJobUpdate::New(
							ok ? bm::WebPrintJobState::kCompleted : bm::WebPrintJobState::kAborted,
							ok ? (uint32_t)pages : 0u));
					}, obs, ok, pages));
				}).detach();
			}, name, std::move(pdf), pages, landscape, std::move(cb)));
		}, std::move(document), std::move(cb)).detach();
	}
};

class Printing final : public bm::WebPrintingService {
public:
	void Bind( mojo::PendingReceiver<bm::WebPrintingService> r ) { receivers_.Add(this, std::move(r)); }

	void GetPrinters( GetPrintersCallback cb ) override
	{
		blink::Vector<bm::WebPrinterInfoPtr> list;
		auto p = bm::WebPrinterInfo::New();
		p->printer_name = blink::String::FromUtf8("TessronOSの印刷");
		p->printer_id = blink::String(kPrinterId);
		mojo::MakeSelfOwnedReceiver(std::make_unique<Printer>(), p->printer_remote.InitWithNewPipeAndPassReceiver());
		list.push_back(std::move(p));
		std::move(cb).Run(std::move(list));
	}

private:
	mojo::ReceiverSet<bm::WebPrintingService>	receivers_;
};

Printing	*g_printing;

}  // namespace

/* The binder of the frames' broker, once when the engine is up */
void br_printing_init()
{
	blink::GetEmptyBrowserInterfaceBroker().SetBinderForTesting(
		bm::WebPrintingService::Name_, base::BindRepeating([]( mojo::ScopedMessagePipeHandle h ) {
			br_on_service_thread(base::BindOnce([]( mojo::PendingReceiver<bm::WebPrintingService> r ) {
				if ( g_printing == nullptr ) g_printing = new Printing();
				g_printing->Bind(std::move(r));
			}, mojo::PendingReceiver<bm::WebPrintingService>(std::move(h))));
		}));
}
