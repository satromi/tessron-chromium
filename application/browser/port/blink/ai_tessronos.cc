/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	ai_tessronos.cc
 *	ブラウザ: what the built-in AI of a page asks of a browser's process
 *	(design 14.4.13).
 *
 *	  - LanguageDetector: the language detection driver says the model is
 *	    there (kReadily) and hands no file; the model in this process is
 *	    CLD3 (third_party/cld_3, the neural network whose weights are
 *	    compiled in), in place of the TFLite model a browser downloads
 *	    (cld3_predict, given to blink::LanguageDetectionModel).
 *	  - LanguageModel, Summarizer, Writer, Rewriter, Proofreader,
 *	    SemanticEmbedder: there is no model on the machine and none can
 *	    be fetched, so each answers as Chrome does on a machine that is
 *	    not eligible for one: availability() "unavailable", create()
 *	    refused, params() null.
 *	  - Translator: no language pack, and none to download: every pair
 *	    is "unavailable", create() refused as not supported.
 *
 *	The services are on the services' thread; the detection runs on the
 *	task runner the page gives it.
 */

#include <stdio.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"
#include "components/language_detection/content/common/language_detection.mojom-blink.h"
#include "components/language_detection/core/language_detection_model.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "third_party/blink/public/mojom/ai/ai_language_model.mojom-blink.h"
#include "third_party/blink/public/mojom/ai/ai_manager.mojom-blink.h"
#include "third_party/blink/public/mojom/on_device_translation/translation_manager.mojom-blink.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "third_party/cld_3/src/src/nnet_language_identifier.h"

/* The services' thread (blob_tessronos.cc) */
void br_on_service_thread( base::OnceClosure task );

namespace {

namespace bm = blink::mojom::blink;
namespace ld = language_detection::mojom::blink;

/* What a machine with no model is told of every model of Chrome's */
constexpr bm::ModelAvailabilityCheckResult kNoModel = bm::ModelAvailabilityCheckResult::kUnavailableModelNotEligible;

class AIManager : public bm::AIManager {
public:
	void Bind( mojo::PendingReceiver<bm::AIManager> r ) { receivers_.Add(this, std::move(r)); }

	void CanCreateLanguageModel( bm::AILanguageModelCreateOptionsPtr, CanCreateLanguageModelCallback cb ) override
	{
		std::move(cb).Run(kNoModel);
	}

	void CreateLanguageModel( mojo::PendingRemote<bm::AIManagerCreateLanguageModelClient> client,
				  bm::AILanguageModelCreateOptionsPtr,
				  mojo::PendingRemote<on_device_model::mojom::blink::DownloadObserver> ) override
	{
		mojo::Remote<bm::AIManagerCreateLanguageModelClient> c(std::move(client));
		c->OnError(bm::AIManagerCreateClientError::kUnableToCreateSession, nullptr);
	}

	void CanCreateSummarizer( bm::AISummarizerCreateOptionsPtr, CanCreateSummarizerCallback cb ) override
	{
		std::move(cb).Run(kNoModel);
	}

	void CreateSummarizer( mojo::PendingRemote<bm::AIManagerCreateSummarizerClient> client,
			       bm::AISummarizerCreateOptionsPtr,
			       mojo::PendingRemote<on_device_model::mojom::blink::DownloadObserver> ) override
	{
		mojo::Remote<bm::AIManagerCreateSummarizerClient> c(std::move(client));
		c->OnError(bm::AIManagerCreateClientError::kUnableToCreateSession, nullptr);
	}

	void GetLanguageModelParams( GetLanguageModelParamsCallback cb ) override
	{
		std::move(cb).Run(nullptr);
	}

	void CanCreateWriter( bm::AIWriterCreateOptionsPtr, CanCreateWriterCallback cb ) override
	{
		std::move(cb).Run(kNoModel);
	}

	void CreateWriter( mojo::PendingRemote<bm::AIManagerCreateWriterClient> client, bm::AIWriterCreateOptionsPtr,
			   mojo::PendingRemote<on_device_model::mojom::blink::DownloadObserver> ) override
	{
		mojo::Remote<bm::AIManagerCreateWriterClient> c(std::move(client));
		c->OnError(bm::AIManagerCreateClientError::kUnableToCreateSession, nullptr);
	}

	void CanCreateRewriter( bm::AIRewriterCreateOptionsPtr, CanCreateRewriterCallback cb ) override
	{
		std::move(cb).Run(kNoModel);
	}

	void CreateRewriter( mojo::PendingRemote<bm::AIManagerCreateRewriterClient> client, bm::AIRewriterCreateOptionsPtr,
			     mojo::PendingRemote<on_device_model::mojom::blink::DownloadObserver> ) override
	{
		mojo::Remote<bm::AIManagerCreateRewriterClient> c(std::move(client));
		c->OnError(bm::AIManagerCreateClientError::kUnableToCreateSession, nullptr);
	}

	void CanCreateProofreader( bm::AIProofreaderCreateOptionsPtr, CanCreateProofreaderCallback cb ) override
	{
		std::move(cb).Run(kNoModel);
	}

	void CreateProofreader( mojo::PendingRemote<bm::AIManagerCreateProofreaderClient> client,
				bm::AIProofreaderCreateOptionsPtr,
				mojo::PendingRemote<on_device_model::mojom::blink::DownloadObserver> ) override
	{
		mojo::Remote<bm::AIManagerCreateProofreaderClient> c(std::move(client));
		c->OnError(bm::AIManagerCreateClientError::kUnableToCreateSession, nullptr);
	}

	void CanCreateSemanticEmbedder( CanCreateSemanticEmbedderCallback cb ) override
	{
		std::move(cb).Run(kNoModel);
	}

	void CreateSemanticEmbedder( mojo::PendingRemote<bm::AIManagerCreateSemanticEmbedderClient> client,
				     mojo::PendingRemote<on_device_model::mojom::blink::DownloadObserver> ) override
	{
		mojo::Remote<bm::AIManagerCreateSemanticEmbedderClient> c(std::move(client));
		c->OnError(bm::AIManagerCreateClientError::kUnableToCreateSession);
	}

private:
	mojo::ReceiverSet<bm::AIManager>	receivers_;
};

/* No language pack is installed and none can be fetched */
class Translations : public bm::TranslationManager {
public:
	void Bind( mojo::PendingReceiver<bm::TranslationManager> r ) { receivers_.Add(this, std::move(r)); }

	void CreateTranslator( mojo::PendingRemote<bm::TranslationManagerCreateTranslatorClient> client,
			       bm::TranslatorCreateOptionsPtr ) override
	{
		mojo::Remote<bm::TranslationManagerCreateTranslatorClient> c(std::move(client));
		c->OnResult(bm::CreateTranslatorResult::NewError(bm::CreateTranslatorError::kNotSupportedLanguage),
			    nullptr, nullptr);
	}

	void TranslationAvailable( bm::TranslatorLanguageCodePtr, bm::TranslatorLanguageCodePtr,
				   TranslationAvailableCallback cb ) override
	{
		std::move(cb).Run(bm::CanCreateTranslatorResult::kNoNotSupportedLanguage);
	}

private:
	mojo::ReceiverSet<bm::TranslationManager>	receivers_;
};

/* The detection model is in the process: it is there, and there is no file to hand */
class Detection : public ld::ContentLanguageDetectionDriver {
public:
	void Bind( mojo::PendingReceiver<ld::ContentLanguageDetectionDriver> r ) { receivers_.Add(this, std::move(r)); }

	void GetLanguageDetectionModel( GetLanguageDetectionModelCallback cb ) override
	{
		std::move(cb).Run(base::File());
	}

	void GetLanguageDetectionModelStatus( GetLanguageDetectionModelStatusCallback cb ) override
	{
		std::move(cb).Run(ld::LanguageDetectionModelStatus::kReadily);
	}

private:
	mojo::ReceiverSet<ld::ContentLanguageDetectionDriver>	receivers_;
};

AIManager	*g_ai;
Translations	*g_translations;
Detection	*g_detection;

template <typename Interface, typename Service>
void Binder( Service **svc )
{
	blink::GetEmptyBrowserInterfaceBroker().SetBinderForTesting(
		Interface::Name_, base::BindRepeating([]( Service **svc, mojo::ScopedMessagePipeHandle h ) {
			br_on_service_thread(base::BindOnce([]( Service **svc, mojo::PendingReceiver<Interface> r ) {
				if ( *svc == nullptr ) *svc = new Service();
				( *svc )->Bind(std::move(r));
			}, svc, mojo::PendingReceiver<Interface>(std::move(h))));
		}, svc));
}

/*
 * CLD3's code for a language as the BCP 47 tag the LanguageDetector
 * gives: the old codes it keeps for Hebrew, Javanese and Filipino made
 * the current ones
 */
std::string bcp47_of( const std::string &code )
{
	if ( code == "iw" ) return "he";
	if ( code == "jw" ) return "jv";
	if ( code == "tl" ) return "fil";
	return code;
}

}  // namespace

/* ---------------------------------------------------------------- the detection model */

namespace blink {
/* The embedder's model of the LanguageDetector (the patch language_detection_model.patch) */
extern std::vector<language_detection::Prediction> ( *g_detect_language_hook )( const std::string &text );
}

namespace {

/*
 * The languages of a text as the model of a browser gives them: the
 * likeliest first, each with the share of the text CLD3 found in it
 * times how sure it is of that part (up to three), and "unknown" with
 * what is left; an empty text is "und" alone
 */
std::vector<language_detection::Prediction> cld3_predict( const std::string &utf8 )
{
	static std::mutex lock;
	static chrome_lang_id::NNetLanguageIdentifier *cld;
	std::vector<language_detection::Prediction> out;
	if ( utf8.find_first_not_of(" \t\r\n") == std::string::npos ) {
		out.emplace_back("und", 1.0f);
		return out;
	}
	float sum = 0;
	{
		std::lock_guard<std::mutex> hold(lock);
		if ( cld == nullptr ) cld = new chrome_lang_id::NNetLanguageIdentifier(0, 1000);
		for ( const chrome_lang_id::NNetLanguageIdentifier::Result &r : cld->FindTopNMostFreqLangs(utf8, 3) ) {
			if ( r.language == chrome_lang_id::NNetLanguageIdentifier::kUnknown ) continue;
			float score = r.probability * ( r.proportion > 0 ? r.proportion : 1.0f );
			score = std::min(score, 1.0f - sum);
			if ( score <= 0 ) break;
			out.emplace_back(bcp47_of(r.language), score);
			sum += score;
		}
	}
	std::stable_sort(out.begin(), out.end(), []( const language_detection::Prediction &a,
						    const language_detection::Prediction &b ) { return a.score > b.score; });
	if ( out.empty() ) out.emplace_back("und", 0.0f);
	out.emplace_back("unknown", std::max(0.0f, 1.0f - sum));
	return out;
}

}  // namespace

/* The binders of the frames' broker, once when the engine is up */
void br_ai_init()
{
	Binder<bm::AIManager, AIManager>(&g_ai);
	Binder<bm::TranslationManager, Translations>(&g_translations);
	Binder<ld::ContentLanguageDetectionDriver, Detection>(&g_detection);
	blink::g_detect_language_hook = &cld3_predict;
}
