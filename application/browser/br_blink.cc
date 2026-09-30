/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_blink.cc
 *	ブラウザ: the page laid out, painted and run by Blink (design 17.19),
 *	behind the engine interface of br.h.
 *
 *	Blink is brought up without the layers a browser normally wraps it
 *	in (the content layer, other processes, a compositor): the process
 *	sets up //base (the exit manager, the command line, the features,
 *	the pool of worker threads) and Mojo within the process, gives Blink
 *	a Platform of its own (resources from the embedded blink_resources.pak,
 *	the font the application draws in, the user agent), the main thread
 *	scheduler, the names and bindings of core, and V8's isolate. A page
 *	is a Page with one LocalFrame and its view, made with the empty
 *	clients of core and a few overrides: the chrome client notes that a
 *	new frame is wanted and prints the console, the frame client hands
 *	navigations to the application (a form sent with POST with its body)
 *	and loads subresources through the network stack on its own thread
 *	(br_loader.cc, port/net).
 *
 *	A load commits a navigation with the fetched bytes as the response.
 *	The view is as wide as the window's page area and as high as it; the
 *	application's scroll position is the layout viewport's, and a paint
 *	runs the document's lifecycle and plays its paint record into the
 *	application's canvas.
 */

#include TS_CONFIG_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <memory>
#include <string>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/feature_list.h"
#include "base/memory/discardable_memory.h"
#include "base/memory/discardable_memory_allocator.h"
#include "base/memory/ref_counted_memory.h"
#include "base/memory_coordinator/dummy_memory_consumer_registry.h"
#include "base/memory_coordinator/memory_consumer_registry.h"
#include "base/message_loop/message_pump.h"
#include "base/message_loop/message_pump_type.h"
#include "base/run_loop.h"
#include "base/time/default_tick_clock.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "base/unguessable_token.h"
#include "cc/paint/paint_record.h"
#include "mojo/core/embedder/embedder.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "services/metrics/public/cpp/ukm_source_id.h"
#include "skia/ext/font_utils.h"
#include "third_party/blink/public/common/storage_key/storage_key.h"
#include "third_party/blink/public/common/tokens/tokens.h"
#include "third_party/blink/public/platform/linux/web_sandbox_support.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/public/platform/scheduler/web_thread_scheduler.h"
#include "third_party/blink/public/platform/web_crypto.h"
#include "third_party/blink/public/platform/web_crypto_algorithm.h"
#include "third_party/blink/public/platform/web_data.h"
#include "third_party/blink/public/platform/web_font_render_style.h"
#include "third_party/blink/public/platform/web_media_player.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/web/web_navigation_params.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_context_snapshot.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_initializer.h"
#include "third_party/blink/renderer/core/core_initializer.h"
#include "third_party/blink/renderer/core/scheduler/task_attribution_tracker_impl.h"
#include "third_party/blink/renderer/platform/bindings/v8_per_isolate_data.h"
#include "third_party/blink/renderer/platform/crypto.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/dom/element.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/local_frame_view.h"
#include "third_party/blink/renderer/core/frame/settings.h"
#include "third_party/blink/renderer/core/frame/visual_viewport.h"
#include "third_party/blink/renderer/core/input/event_handler.h"
#include "third_party/blink/renderer/core/html/canvas/html_canvas_element.h"
#include "third_party/blink/renderer/core/html/html_anchor_element.h"
#include "third_party/blink/renderer/modules/canvas/canvas2d/canvas_rendering_context_2d.h"
#include "third_party/blink/renderer/modules/webgl/webgl_context_factory.h"
#include "third_party/blink/renderer/platform/network/encoded_form_data.h"
#include "third_party/blink/public/platform/web_media_player.h"
#include "third_party/blink/renderer/core/html/media/html_media_element.h"
#include "third_party/blink/public/platform/web_graphics_context_3d_provider.h"
#include "third_party/blink/renderer/core/html/html_collection.h"
#include "third_party/blink/public/mojom/input/scroll_direction.mojom-blink.h"
#include "third_party/blink/public/common/input/web_keyboard_event.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "third_party/blink/public/common/input/web_mouse_wheel_event.h"
#include "third_party/blink/public/platform/web_text_input_type.h"
#include "third_party/blink/renderer/core/editing/frame_selection.h"
#include "third_party/blink/renderer/core/editing/ime/ime_text_span.h"
#include "third_party/blink/renderer/core/editing/ime/input_method_controller.h"
#include "third_party/blink/renderer/platform/graphics/color.h"
#include "ui/events/keycodes/dom/dom_key.h"
#include "ui/events/types/scroll_types.h"
#include "third_party/blink/renderer/core/layout/hit_test_location.h"
#include "third_party/blink/renderer/core/layout/hit_test_request.h"
#include "third_party/blink/renderer/core/layout/hit_test_result.h"
#include "third_party/blink/renderer/core/layout/layout_view.h"
#include "third_party/blink/renderer/core/loader/document_loader.h"
#include "third_party/blink/renderer/core/loader/empty_clients.h"
#include "third_party/blink/renderer/core/loader/frame_loader.h"
#include "third_party/blink/renderer/core/page/page.h"
#include "third_party/blink/renderer/core/paint/paint_layer_scrollable_area.h"
#include "third_party/blink/renderer/platform/fonts/generic_font_family_settings.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/heap/persistent.h"
#include "third_party/blink/renderer/platform/scheduler/public/agent_group_scheduler.h"
#include "third_party/blink/renderer/platform/scheduler/public/main_thread.h"
#include "third_party/blink/renderer/platform/scheduler/public/main_thread_scheduler.h"
#include "third_party/blink/renderer/platform/scheduler/public/thread.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/wtf/allocator/partitions.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkData.h"
#include "third_party/skia/include/core/SkFontMgr.h"
#include "third_party/skia/include/core/SkFontStyle.h"
#include "third_party/skia/include/core/SkStream.h"
#include "third_party/skia/include/core/SkTypeface.h"
#include "third_party/skia/include/ports/SkFontMgr_data.h"
#include "v8/include/cppgc/heap-statistics.h"
#include "v8/include/v8-cppgc.h"
#include "v8/include/v8.h"

#include <malloc.h>

#include "br.h"
#include "port/net/br_net.h"

/* The subresource loader (br_loader.cc) and the bindings (br_bindings.cc) */
std::unique_ptr<blink::URLLoader> br_blink_url_loader();
void br_blink_bindings_init();

/* localStorage read back from its object (port/blink/storage_tessronos.cc) */
void br_storage_restore();

/* The players of the media elements (port/media/media_player.cc) */
std::unique_ptr<blink::WebMediaPlayer> br_media_player( blink::HTMLMediaElement &element,
							blink::WebMediaPlayerClient *client );
void br_media_tick();
bool br_media_click( int x, int y );

/* WebGL's GLES2 on the CPU (port/webgl/gl_tessronos.cc) */
std::unique_ptr<blink::WebGraphicsContext3DProvider> BrCreateGLProvider(
	blink::Platform::WebGLContextInfo *info );

/* Brotli's decoder (third_party/brotli), for the compressed resources */
extern "C" int BrotliDecoderDecompress( size_t encoded_size, const uint8_t *encoded,
					size_t *decoded_size, uint8_t *decoded );
#define BROTLI_DECODER_RESULT_SUCCESS	1

/* The resources of Blink (style sheets, scripts of its controls), linked in */
extern "C" const unsigned char br_blink_pak[];
extern "C" const unsigned char br_blink_pak_end[];

namespace {

const char kFamily[] = "Noto Sans JP";

/* ---------------------------------------------------------------- resources */

/*
 * blink_resources.pak (version 5): a header, a table of (id, offset)
 * sorted by id with one entry more than there are resources, and a table
 * of aliases (id, index into the first). A resource compressed with
 * Brotli starts with 0x1e 0x9b and the size of its text in six bytes.
 */
class Pak {
public:
	bool find( int id, const uint8_t **p, size_t *n ) const
	{
		const uint8_t	*d = br_blink_pak;
		size_t		size = (size_t)( br_blink_pak_end - br_blink_pak );

		if ( size < 12 || rd32(d) != 5 ) return false;
		unsigned cnt = rd16(d + 8), aliases = rd16(d + 10);
		const uint8_t *ent = d + 12, *al = ent + 6 * ( cnt + 1 );
		int index = lookup(ent, 6, cnt, id);
		if ( index < 0 ) {
			int a = lookup(al, 4, aliases, id);
			if ( a < 0 ) return false;
			index = rd16(al + 4 * a + 2);
		}
		uint32_t off = rd32(ent + 6 * index + 2), end = rd32(ent + 6 * ( index + 1 ) + 2);
		if ( end < off || end > size ) return false;
		*p = d + off;
		*n = end - off;
		return true;
	}

	/* The text of a resource, decompressed */
	std::string text( int id ) const
	{
		const uint8_t	*p;
		size_t		n;

		if ( !find(id, &p, &n) ) return std::string();
		if ( n >= 8 && p[0] == 0x1e && p[1] == 0x9b ) {
			size_t want = 0;
			for ( int i = 0; i < 6; i++ ) want |= (size_t)p[2 + i] << ( 8 * i );
			std::string out(want, '\0');
			size_t got = want;
			if ( BrotliDecoderDecompress(n - 8, p + 8, &got, (uint8_t *)&out[0])
			     != BROTLI_DECODER_RESULT_SUCCESS ) {
				printf("browser: resource %d does not decompress\n", id);
				return std::string();
			}
			out.resize(got);
			return out;
		}
		return std::string((const char *)p, n);
	}

private:
	static unsigned rd16( const uint8_t *p ) { return p[0] | ( p[1] << 8 ); }
	static uint32_t rd32( const uint8_t *p )
	{
		return p[0] | ( p[1] << 8 ) | ( p[2] << 16 ) | ( (uint32_t)p[3] << 24 );
	}
	static int lookup( const uint8_t *t, int stride, unsigned cnt, int id )
	{
		int lo = 0, hi = (int)cnt - 1;
		while ( lo <= hi ) {
			int mid = ( lo + hi ) / 2, v = (int)rd16(t + stride * mid);
			if ( v == id ) return mid;
			if ( v < id ) lo = mid + 1; else hi = mid - 1;
		}
		return -1;
	}
};

Pak	g_pak;

/* ---------------------------------------------------------------- fonts */

/*
 * The one face the system has for pages: every family a page names, and
 * every generic family, is that face; synthetic bold and italic make the
 * other styles.
 */
class BrFontMgr final : public SkFontMgr {
public:
	explicit BrFontMgr( sk_sp<SkData> data )
		: inner_(SkFontMgr_New_Custom_Data(SkSpan<sk_sp<SkData>>(&data, 1))),
		  face_(inner_->makeFromData(data)) {}

protected:
	int onCountFamilies() const override { return 1; }
	void onGetFamilyName( int, SkString *name ) const override { name->set(kFamily); }
	sk_sp<SkFontStyleSet> onCreateStyleSet( int ) const override
	{
		return inner_->createStyleSet(0);
	}
	sk_sp<SkFontStyleSet> onMatchFamily( const char * ) const override
	{
		return inner_->createStyleSet(0);
	}
	sk_sp<SkTypeface> onMatchFamilyStyle( const char *family, const SkFontStyle & ) const override
	{
		/*
		 * The icon font the scrollbar's arrows are drawn from is not there:
		 * with no face the theme draws them as triangles, where the one
		 * face would give its private-use code points as missing glyphs.
		 */
		if ( family != nullptr && strcmp(family, "Segoe Fluent Icons") == 0 ) return nullptr;
		return face_;
	}
	sk_sp<SkTypeface> onMatchFamilyStyleCharacter( const char *, const SkFontStyle &,
						       const char *[], int, SkUnichar c ) const override
	{
		return ( face_ != nullptr && face_->unicharToGlyph(c) != 0 ) ? face_ : nullptr;
	}
	sk_sp<SkTypeface> onMakeFromData( sk_sp<SkData> d, int ttc ) const override
	{
		return inner_->makeFromData(std::move(d), ttc);
	}
	sk_sp<SkTypeface> onMakeFromStreamIndex( std::unique_ptr<SkStreamAsset> s, int ttc ) const override
	{
		return inner_->makeFromStream(std::move(s), ttc);
	}
	sk_sp<SkTypeface> onMakeFromStreamArgs( std::unique_ptr<SkStreamAsset> s,
						const SkFontArguments &a ) const override
	{
		return inner_->makeFromStream(std::move(s), a);
	}
	sk_sp<SkTypeface> onMakeFromFile( const char *path, int ttc ) const override
	{
		return inner_->makeFromFile(path, ttc);
	}
	sk_sp<SkTypeface> onLegacyMakeTypeface( const char *, SkFontStyle ) const override
	{
		return face_;
	}

private:
	sk_sp<SkFontMgr>	inner_;
	sk_sp<SkTypeface>	face_;
};

/* How glyphs are drawn: grey antialiasing and slight hinting */
class BrSandbox final : public blink::WebSandboxSupport {
public:
	bool GetFallbackFontForCharacter( blink::WebUChar32, const char *,
					  gfx::FallbackFontData * ) override
	{
		return false;
	}
	bool MatchFontByPostscriptNameOrFullFontName( const char *, gfx::FallbackFontData * ) override
	{
		return false;
	}
	void GetWebFontRenderStyleForStrike( const char *, int, bool, bool, float,
					     blink::WebFontRenderStyle *out ) override
	{
		out->use_bitmaps = 0;
		out->use_auto_hint = 0;
		out->use_hinting = 1;
		out->hint_style = 1;
		out->use_anti_alias = 1;
		out->use_subpixel_rendering = 0;
		out->use_subpixel_positioning = 0;
	}
};

/* ---------------------------------------------------------------- discardable memory */

/*
 * The decoded images the engine keeps (cc's decode cache, the frames of
 * an image) are in discardable memory, which a browser shares with its
 * other processes and takes back under pressure. Here it is memory of
 * the heap that is never taken back while it is held: locking it always
 * succeeds, and it goes when its owner lets it go.
 */
class BrDiscardableMemory final : public base::DiscardableMemory {
public:
	BrDiscardableMemory( size_t size, size_t *total )
		: data_(new uint8_t[size != 0 ? size : 1]), size_(size), total_(total)
	{
		*total_ += size_;
	}
	~BrDiscardableMemory() override
	{
		*total_ -= size_;
		delete[] data_;
	}
	bool Lock() override { return true; }
	void Unlock() override {}
	void *data() const override { return data_; }
	void DiscardForTesting() override {}
	base::trace_event::MemoryAllocatorDump *CreateMemoryAllocatorDump(
		const char *, base::trace_event::ProcessMemoryDump * ) const override
	{
		return nullptr;
	}

private:
	uint8_t	*data_;
	size_t	size_;
	size_t	*total_;
};

class BrDiscardableAllocator final : public base::DiscardableMemoryAllocator {
public:
	std::unique_ptr<base::DiscardableMemory> AllocateLockedDiscardableMemory( size_t size ) override
	{
		return std::make_unique<BrDiscardableMemory>(size, &total_);
	}
	size_t GetBytesAllocated() const override { return total_; }
	void ReleaseFreeMemory() override {}

private:
	size_t	total_ = 0;
};

/* ---------------------------------------------------------------- the platform */

/*
 * crypto.subtle's operations. The binding calls the platform's WebCrypto
 * without a check, and the base platform has none. Digests (SHA-256,
 * SHA-384, SHA-512) are computed here; every other operation answers
 * "not supported", which the binding turns into a rejected promise.
 */
class BrWebCrypto final : public blink::WebCrypto {
public:
	void Digest( const blink::WebCryptoAlgorithm &algorithm, std::vector<unsigned char> data,
		     blink::WebCryptoResult result, scoped_refptr<base::SingleThreadTaskRunner> ) override
	{
		blink::HashAlgorithm hash;
		switch ( algorithm.Id() ) {
		case blink::kWebCryptoAlgorithmIdSha256: hash = blink::kHashAlgorithmSha256; break;
		case blink::kWebCryptoAlgorithmIdSha384: hash = blink::kHashAlgorithmSha384; break;
		case blink::kWebCryptoAlgorithmIdSha512: hash = blink::kHashAlgorithmSha512; break;
		default:
			result.CompleteWithError(blink::kWebCryptoErrorTypeNotSupported, "");
			return;
		}
		blink::DigestValue digest;
		if ( !blink::ComputeDigest(hash, base::span<const uint8_t>(data), digest) ) {
			result.CompleteWithError(blink::kWebCryptoErrorTypeOperation, "");
			return;
		}
		result.CompleteWithBuffer(base::span<const uint8_t>(digest.data(), digest.size()));
	}
};

class BrPlatform final : public blink::Platform {
public:
	blink::WebCrypto *Crypto() override { return &crypto_; }

	blink::WebSandboxSupport *GetSandboxSupport() override { return &sandbox_; }

	blink::WebString DefaultLocale() override { return blink::WebString::FromAscii("ja"); }

	blink::WebString UserAgent() override
	{
		return blink::WebString::FromAscii(br_user_agent());
	}

	bool HasDataResource( int id ) const override
	{
		const uint8_t	*p;
		size_t		n;
		return g_pak.find(id, &p, &n);
	}

	blink::WebData GetDataResource( int id, ui::ResourceScaleFactor ) override
	{
		const uint8_t	*p;
		size_t		n;
		if ( !g_pak.find(id, &p, &n) ) return blink::WebData();
		return blink::WebData(base::span<const uint8_t>(p, n));
	}

	std::string GetDataResourceString( int id ) override { return g_pak.text(id); }

	scoped_refptr<base::RefCountedMemory> GetDataResourceBytes( int id ) override
	{
		std::string s = g_pak.text(id);
		return base::MakeRefCounted<base::RefCountedString>(std::move(s));
	}

	/*
	 * Compositing is on the CPU: the drawing buffer of WebGL then keeps
	 * its colour buffers readable, which the canvas's paint reads back
	 */
	bool IsGpuCompositingDisabled() const override { return true; }

	/* WebGL's GLES2, on the CPU (port/webgl/gl_tessronos.cc) */
	std::unique_ptr<blink::WebGraphicsContext3DProvider> CreateWebGLGraphicsContextProvider(
		bool, bool, WebGLContextType, const blink::WebURL &, WebGLContextInfo *info ) override
	{
		return BrCreateGLProvider(info);
	}

	std::unique_ptr<blink::WebGraphicsContext3DProvider>
	CreateSharedOffscreenGraphicsContext3DProvider() override
	{
		WebGLContextInfo info;
		return BrCreateGLProvider(&info);
	}

private:
	BrSandbox	sandbox_;
	BrWebCrypto	crypto_;
};

/* ---------------------------------------------------------------- core without modules */

/*
 * What modules would add to core: nothing here. Media, storage, the
 * inspector and the other module features are not built in.
 */
class BrCoreInitializer final : public blink::CoreInitializer {
public:
	void RegisterInterfaces( mojo::BinderMap & ) override {}
	void InitLocalFrame( blink::LocalFrame & ) const override {}
	void InitServiceWorkerGlobalScope( blink::ServiceWorkerGlobalScope & ) const override {}
	void InstallSupplements( blink::LocalFrame & ) const override {}
	blink::MediaControls *CreateMediaControls( blink::HTMLMediaElement &,
						   blink::ShadowRoot & ) const override
	{
		return nullptr;
	}
	blink::PictureInPictureController *CreatePictureInPictureController(
		blink::Document & ) const override
	{
		return nullptr;
	}
	void InitInspectorAgentSession( blink::DevToolsSession *, blink::InspectorDOMAgent *,
					blink::InspectedFrames *, blink::Page * ) const override {}
	void InitWorkerInspectorAgentSession( blink::DevToolsSession *,
					      blink::WorkerGlobalScope * ) const override {}
	void OnClearWindowObjectInMainWorld( blink::Document &, const blink::Settings & ) const override {}
	std::unique_ptr<blink::WebMediaPlayer> CreateWebMediaPlayer(
		blink::WebLocalFrameClient *, blink::HTMLMediaElement &,
		const blink::WebMediaPlayerSource &, blink::WebMediaPlayerClient * ) const override
	{
		return nullptr;
	}
	blink::RemotePlaybackClient *CreateRemotePlaybackClient( blink::HTMLMediaElement & ) const override
	{
		return nullptr;
	}
	void ProvideModulesToPage( blink::Page &, const blink::SessionStorageNamespaceId & ) const override {}
	void ForceNextWebGLContextCreationToFail() const override {}
	void CollectAllGarbageForAnimationAndPaintWorkletForTesting() const override {}
	void CloneSessionStorage( blink::Page *, const blink::SessionStorageNamespaceId & ) override {}
	void EvictSessionStorageCachedData( blink::Page * ) override {}
	void DidChangeManifest( blink::LocalFrame & ) override {}
	void NotifyOrientationChanged( blink::LocalFrame & ) override {}
	void DidUpdateScreens( blink::LocalFrame &, const display::ScreenInfos & ) override {}
	void SetLocalStorageArea( blink::LocalFrame &,
				  mojo::PendingRemote<blink::mojom::blink::StorageArea> ) override {}
	void SetSessionStorageArea( blink::LocalFrame &,
				    mojo::PendingRemote<blink::mojom::blink::StorageArea> ) override {}
	blink::mojom::blink::FileSystemManager &GetFileSystemManager( blink::ExecutionContext * ) override
	{
		NOTREACHED();
	}
};

/* ---------------------------------------------------------------- the page's clients */

struct BrPage;

class BrChromeClient final : public blink::EmptyChromeClient {
public:
	explicit BrChromeClient( BrPage *p ) : page_(p) {}

	void ScheduleAnimation( const blink::LocalFrameView *, cc::BeginMainFrameReason,
				base::TimeDelta, bool ) override;

	bool ShouldReportDetailedMessageForSourceAndSeverity(
		blink::LocalFrame &, blink::mojom::blink::ConsoleMessageLevel,
		const blink::String & ) override
	{
		return true;
	}

	void AddMessageToConsole( blink::LocalFrame *, blink::mojom::ConsoleMessageSource,
				  blink::mojom::ConsoleMessageLevel, const blink::String &message,
				  unsigned line, const blink::String &source,
				  const blink::String & ) override
	{
		printf("browser: console: %s (%s:%u)\n", message.Utf8().c_str(),
		       source.Utf8().c_str(), line);
	}

private:
	BrPage	*page_;
};

class BrFrameClient final : public blink::EmptyLocalFrameClient {
public:
	explicit BrFrameClient( BrPage *p ) : page_(p) {}

	blink::String UserAgent() override { return blink::String(br_user_agent()); }

	void BeginNavigation(
		const blink::ResourceRequest &request, const blink::KURL &,
		blink::mojom::RequestContextFrameType, blink::LocalDOMWindow *,
		blink::DocumentLoader *, blink::WebNavigationType, blink::NavigationPolicy,
		blink::WebFrameLoadType, blink::mojom::blink::ForceHistoryPush, bool, bool,
		blink::mojom::blink::TriggeringEventInfo, blink::HTMLFormElement *,
		network::mojom::CSPDisposition, mojo::PendingRemote<blink::mojom::blink::BlobURLToken>,
		base::TimeTicks, base::TimeTicks, const blink::String &,
		const blink::LocalFrameToken *, const base::UnguessableToken &,
		const blink::DocumentToken &, blink::SourceLocation *,
		mojo::PendingRemote<blink::mojom::blink::NavigationStateKeepAliveHandle>,
		bool, bool,
		mojo::PendingReceiver<blink::mojom::blink::NavigationResumeDeferredCommitListener>,
		std::optional<base::UnguessableToken> ) override;

	/* the player of <video> and <audio> (port/media) */
	std::unique_ptr<blink::WebMediaPlayer> CreateWebMediaPlayer(
		blink::HTMLMediaElement &element, const blink::WebMediaPlayerSource &,
		blink::WebMediaPlayerClient *client ) override
	{
		return br_media_player(element, client);
	}

	std::unique_ptr<blink::URLLoader> CreateURLLoaderForTesting() override
	{
		return br_blink_url_loader();
	}

private:
	BrPage	*page_;
};

/* A page: its Page, its frame and what the application is to be told */
struct BrPage {
	blink::Persistent<blink::Page>		page;
	blink::Persistent<blink::LocalFrame>	frame;
	blink::Persistent<blink::AgentGroupScheduler>	agents;
	blink::Persistent<BrChromeClient>	chrome;
	blink::Persistent<BrFrameClient>	client;
	bool			wants_frame = false;	/* something to paint anew */
	std::string		go_to;			/* a navigation the page asked for */
	std::string		go_method, go_ctype, go_body;	/* and a form's, sent with POST */
};

void BrChromeClient::ScheduleAnimation( const blink::LocalFrameView *, cc::BeginMainFrameReason,
					base::TimeDelta, bool )
{
	page_->wants_frame = true;
}

void BrFrameClient::BeginNavigation(
	const blink::ResourceRequest &request, const blink::KURL &,
	blink::mojom::RequestContextFrameType, blink::LocalDOMWindow *,
	blink::DocumentLoader *, blink::WebNavigationType, blink::NavigationPolicy,
	blink::WebFrameLoadType, blink::mojom::blink::ForceHistoryPush, bool, bool,
	blink::mojom::blink::TriggeringEventInfo, blink::HTMLFormElement *,
	network::mojom::CSPDisposition, mojo::PendingRemote<blink::mojom::blink::BlobURLToken>,
	base::TimeTicks, base::TimeTicks, const blink::String &,
	const blink::LocalFrameToken *, const base::UnguessableToken &,
	const blink::DocumentToken &, blink::SourceLocation *,
	mojo::PendingRemote<blink::mojom::blink::NavigationStateKeepAliveHandle>,
	bool, bool,
	mojo::PendingReceiver<blink::mojom::blink::NavigationResumeDeferredCommitListener>,
	std::optional<base::UnguessableToken> )
{
	/* the application fetches it and loads it as it does an address */
	page_->go_to = request.Url().GetString().Utf8();
	/* a form sent with POST carries its method, its body and the body's type */
	page_->go_method = request.HttpMethod().GetString().Utf8();
	page_->go_ctype.clear();
	page_->go_body.clear();
	if ( request.HttpBody() != nullptr ) {
		page_->go_body = request.HttpBody()->FlattenToString().Utf8();
		page_->go_ctype = request.HttpContentType().GetString().Utf8();
	}
}

/* ---------------------------------------------------------------- bringing Blink up */

std::unique_ptr<blink::scheduler::WebThreadScheduler>	g_scheduler;
BrPlatform						*g_platform;

void start_blink( sk_sp<SkData> face )
{
	static base::AtExitManager exit_manager;

	base::CommandLine::Init(0, nullptr);
	{
		/*
		 * Of the image decoders written in Rust, those of JPEG XL and
		 * AVIF are built (tools/browser/rust.sh); BMP and ICO are read
		 * by the decoders in C++, as JPEG and PNG are. JPEG XL is off by
		 * default in the engine and is turned on.
		 */
		auto features = std::make_unique<base::FeatureList>();
		features->InitFromCommandLine("JXLImageFormat",
					      "RustyBmpFeature,RustyIcoFeature,RustyJpegFeature");
		base::FeatureList::SetInstance(std::move(features));
	}
	/* the memory the engine's parts give back when asked: nobody asks here */
	static base::ScopedMemoryConsumerRegistry<base::DummyMemoryConsumerRegistry> consumers;
	base::ThreadPoolInstance::CreateAndStartWithDefaultParams("browser");
	static BrDiscardableAllocator discardable;
	base::DiscardableMemoryAllocator::SetInstance(&discardable);
	mojo::core::Init();

	skia::OverrideDefaultSkFontMgr(sk_make_sp<BrFontMgr>(face));

	g_platform = new BrPlatform;
	blink::Platform::InitializeBlink();
	g_scheduler = blink::scheduler::WebThreadScheduler::CreateMainThreadScheduler(
		base::MessagePump::Create(base::MessagePumpType::DEFAULT));
	blink::Platform::InitializeMainThread(g_platform, g_scheduler.get());

	static BrCoreInitializer core;
	core.Initialize();
	br_blink_bindings_init();
	/* the contexts of modules a canvas gets here: 2D, drawn by Skia on the CPU */
	blink::HTMLCanvasElement::RegisterRenderingContextFactory(
		std::make_unique<blink::CanvasRenderingContext2D::Factory>());
	/* and WebGL 1, drawn on the CPU */
	blink::HTMLCanvasElement::RegisterRenderingContextFactory(blink::WebGLContextFactory::MakeWebGL1());
	blink::V8PerIsolateData::SetTaskAttributionTrackerFactory(
		&blink::scheduler::TaskAttributionTrackerImpl::Create);

	/* no executable pages for a process: V8 interprets */
	blink::V8Initializer::InitializeIsolateHolder(
		blink::V8ContextSnapshot::GetReferenceTable(), "--jitless");
	(void)blink::V8Initializer::InitializeMainThread();
	blink::Partitions::InitializeArrayBufferPartition();
	blink::V8Initializer::InitializeInSandboxAllocator();

	/* the network stack, on its thread, and what it keeps between runs */
	br_net_prepare();
	br_storage_restore();
}

/* Everything the main thread has to do now */
void run_tasks()
{
	base::RunLoop().RunUntilIdle();
}

/* ---------------------------------------------------------------- keys */

/* The modifiers of the system's keyboard (HID_MOD_*) as Blink's */
int web_mods( unsigned mods )
{
	int m = 0;

	if ( mods & 0x22 ) m |= blink::WebInputEvent::kShiftKey;
	if ( mods & 0x11 ) m |= blink::WebInputEvent::kControlKey;
	if ( mods & 0x44 ) m |= blink::WebInputEvent::kAltKey;
	if ( mods & 0x88 ) m |= blink::WebInputEvent::kMetaKey;
	return m;
}

/* A key's HID usage as the virtual key code pages see (keyCode); 0 for none */
int vk_of( unsigned u )
{
	static const unsigned char punct[] = {		/* 0x2D - 0x38 */
		0xBD, 0xBB, 0xDB, 0xDD, 0xDC, 0xDC, 0xBA, 0xDE, 0xC0, 0xBC, 0xBE, 0xBF
	};
	static const unsigned char nav[] = {		/* 0x49 - 0x52 */
		0x2D, 0x24, 0x21, 0x2E, 0x23, 0x22, 0x27, 0x25, 0x28, 0x26
	};

	if ( u >= 0x04 && u <= 0x1D ) return 'A' + (int)( u - 0x04 );
	if ( u >= 0x1E && u <= 0x26 ) return '1' + (int)( u - 0x1E );
	switch ( u ) {
	case 0x27: return '0';
	case 0x28: case 0x58: return 0x0D;
	case 0x29: return 0x1B;
	case 0x2A: return 0x08;
	case 0x2B: return 0x09;
	case 0x2C: return 0x20;
	case 0x62: return 0x60;
	case 0x87: return 0xE2;
	case 0x89: return 0xDC;
	}
	if ( u >= 0x2D && u <= 0x38 ) return punct[u - 0x2D];
	if ( u >= 0x3A && u <= 0x45 ) return 0x70 + (int)( u - 0x3A );
	if ( u >= 0x49 && u <= 0x52 ) return nav[u - 0x49];
	if ( u >= 0x59 && u <= 0x61 ) return 0x61 + (int)( u - 0x59 );
	return 0;
}

/* What the key is to KeyboardEvent.key: its letter, or its name */
ui::DomKey dom_key_of( unsigned u, unsigned ch )
{
	static const ui::DomKey fkeys[] = {
		ui::DomKey::F1, ui::DomKey::F2, ui::DomKey::F3, ui::DomKey::F4,
		ui::DomKey::F5, ui::DomKey::F6, ui::DomKey::F7, ui::DomKey::F8,
		ui::DomKey::F9, ui::DomKey::F10, ui::DomKey::F11, ui::DomKey::F12
	};

	switch ( u ) {
	case 0x28: case 0x58: return ui::DomKey::ENTER;
	case 0x29: return ui::DomKey::ESCAPE;
	case 0x2A: return ui::DomKey::BACKSPACE;
	case 0x2B: return ui::DomKey::TAB;
	case 0x49: return ui::DomKey::INSERT;
	case 0x4A: return ui::DomKey::HOME;
	case 0x4B: return ui::DomKey::PAGE_UP;
	case 0x4C: return ui::DomKey::DEL;
	case 0x4D: return ui::DomKey::END;
	case 0x4E: return ui::DomKey::PAGE_DOWN;
	case 0x4F: return ui::DomKey::ARROW_RIGHT;
	case 0x50: return ui::DomKey::ARROW_LEFT;
	case 0x51: return ui::DomKey::ARROW_DOWN;
	case 0x52: return ui::DomKey::ARROW_UP;
	}
	if ( u >= 0x3A && u <= 0x45 ) return fkeys[u - 0x3A];
	if ( ch >= 0x20 && ch != 0x7F ) return ui::DomKey::FromCharacter(ch);
	return ui::DomKey::UNIDENTIFIED;
}

void fill_key( blink::WebKeyboardEvent *e, int vk, unsigned usage, unsigned ch )
{
	e->windows_key_code = vk;
	e->native_key_code = (int)usage;
	e->dom_code = (int)( 0x070000 | usage );	/* ui::DomCode is the USB usage */
	e->dom_key = dom_key_of(usage, ch);
}

/* The UTF-16 length of the first 'bytes' bytes of UTF-8 text */
unsigned u16_at( const std::string &text, int bytes )
{
	size_t n = ( bytes < 0 ) ? 0 : ( (size_t)bytes > text.size() ) ? text.size() : (size_t)bytes;
	return blink::String::FromUtf8(std::string_view(text.data(), n)).length();
}

/* ---------------------------------------------------------------- the engine */

class BlinkEngine final : public BrEngine {
public:
	BlinkEngine() { make_page(); }

	void load( const std::string &url, const std::string &body,
		   const std::string &charset ) override
	{
		auto params = std::make_unique<blink::WebNavigationParams>();
		params->url = blink::KURL(blink::String::FromUtf8(url));
		params->navigation_timings.navigation_start = base::TimeTicks::Now();
		blink::WebNavigationParams::FillStaticResponse(
			params.get(), blink::WebString::FromAscii("text/html"),
			blink::WebString::FromAscii(charset.empty() ? std::string() : charset),
			base::span<const char>(body.data(), body.size()));
		p_.go_to.clear();
		(void)br_net_fetch_peak();
		base::TimeTicks start = base::TimeTicks::Now(), until = start + base::Seconds(kLoadWaitS);
		p_.frame->Loader().CommitNavigation(std::move(params), nullptr);
		run_tasks();
		/*
		 * The page's style sheets, scripts and images come in on the
		 * network thread all at once; the page is shown when they have
		 * (or after kLoadWaitS, when a server is slow to answer)
		 */
		while ( br_net_fetch_outstanding() != 0 && base::TimeTicks::Now() < until ) {
			if ( br_stop_pushed != nullptr && br_stop_pushed() ) {
				/* 停止: the loads cancelled, the page shown with what has come */
				p_.frame->Loader().StopAllLoaders(false);
				break;
			}
			usleep(5000);
			run_tasks();
		}
		run_tasks();
		unsigned most = br_net_fetch_peak();
		printf("browser: the page's parts in %d ms, %u at most at once\n",
		       (int)( base::TimeTicks::Now() - start ).InMilliseconds(), most);
		scroll_ = 0;
	}

	int layout( int width ) override
	{
		if ( width != w_ ) {
			w_ = width;
			resize();
		}
		update();
		blink::LayoutView *lv = doc() != nullptr ? doc()->GetLayoutView() : nullptr;
		if ( lv == nullptr ) return h_;
		int dh = lv->DocumentRect().Height().ToInt();
		return dh > h_ ? dh : h_;
	}

	int doc_width() override
	{
		blink::LayoutView *lv = doc() != nullptr ? doc()->GetLayoutView() : nullptr;
		return ( lv != nullptr ) ? lv->DocumentRect().Width().ToInt() : 0;
	}

	int scroll_left() override
	{
		blink::PaintLayerScrollableArea *sa = view()->LayoutViewport();
		return ( sa != nullptr ) ? (int)sa->GetScrollOffset().x() : 0;
	}

	void set_scroll_left( int x ) override
	{
		blink::PaintLayerScrollableArea *sa = view()->LayoutViewport();
		if ( sa == nullptr ) return;
		sa->SetScrollOffset(blink::ScrollOffset((float)x, sa->GetScrollOffset().y()),
				    blink::mojom::blink::ScrollType::kProgrammatic,
				    cc::ScrollSourceType::kNone);
	}

	void paint( SkCanvas *c, int top, int height ) override
	{
		if ( height != h_ ) {
			h_ = height;
			resize();
		}
		scroll_to(top);
		update();
		c->save();
		c->clear(SK_ColorWHITE);
		cc::PaintRecord rec = view()->GetPaintRecord();
		rec.Playback(c);
		c->restore();
		p_.wants_frame = false;
	}

	std::string link_at( int x, int y ) override
	{
		if ( y < scroll_ || y >= scroll_ + h_ ) {
			/* the point brought into the view */
			scroll_to(y - h_ / 2 > 0 ? y - h_ / 2 : 0);
		}
		update();
		int vy = y - scroll_;
		blink::HitTestLocation loc(gfx::PointF((float)x, (float)vy));
		blink::HitTestResult r = p_.frame->GetEventHandler().HitTestResultAtLocation(loc);
		blink::KURL u = r.AbsoluteLinkURL();
		return u.IsValid() ? std::string(u.GetString().Utf8()) : std::string();
	}

	bool click( int x, int y ) override
	{
		if ( y < scroll_ || y >= scroll_ + h_ ) {
			scroll_to(y - h_ / 2 > 0 ? y - h_ / 2 : 0);
		}
		update();
		gfx::PointF at((float)x, (float)( y - scroll_ ));
		blink::WebMouseEvent down(blink::WebInputEvent::Type::kMouseDown, at, at,
					  blink::WebPointerProperties::Button::kLeft, 1,
					  blink::WebInputEvent::kLeftButtonDown, base::TimeTicks::Now());
		down.SetFrameScale(1);
		p_.frame->GetEventHandler().HandleMousePressEvent(down);
		blink::WebMouseEvent up(blink::WebInputEvent::Type::kMouseUp, at, at,
					blink::WebPointerProperties::Button::kLeft, 1,
					blink::WebInputEvent::kNoModifiers, base::TimeTicks::Now());
		up.SetFrameScale(1);
		p_.frame->GetEventHandler().HandleMouseReleaseEvent(up);
		run_tasks();
		return true;
	}

	std::string first_link() override
	{
		if ( doc() == nullptr ) return std::string();
		blink::HTMLCollection *links = doc()->links();
		for ( unsigned i = 0; links != nullptr && i < links->length(); i++ ) {
			auto *a = blink::DynamicTo<blink::HTMLAnchorElement>(links->item(i));
			if ( a != nullptr && a->Href().IsValid() ) return a->Href().GetString().Utf8();
		}
		return std::string();
	}

	std::string title() override
	{
		return doc() != nullptr ? std::string(doc()->title().Utf8()) : std::string();
	}

	bool pump() override
	{
		run_tasks();
		br_media_tick();
		/*
		 * A frame the page asked for is painted at most every
		 * kFrameMs: a page that animates would otherwise keep the
		 * window busy
		 */
		base::TimeTicks now = base::TimeTicks::Now();
		if ( !p_.wants_frame || now - last_frame_ < base::Milliseconds(kFrameMs) ) return false;
		last_frame_ = now;
		return true;
	}

	std::string navigation() override
	{
		std::string to;
		to.swap(p_.go_to);
		if ( !to.empty() ) {
			nav_method_ = std::move(p_.go_method);
			nav_ctype_ = std::move(p_.go_ctype);
			nav_body_ = std::move(p_.go_body);
		}
		p_.go_method.clear();
		p_.go_ctype.clear();
		p_.go_body.clear();
		return to;
	}

	bool navigation_body( std::string *method, std::string *ctype, std::string *body ) override
	{
		if ( nav_method_ != "POST" ) return false;
		*method = nav_method_;
		*ctype = nav_ctype_;
		*body = nav_body_;
		return true;
	}

	/* ------------------------------------------------ input given to the page */

	bool input() override { return true; }

	void mouse( int type, int x, int y, int button, unsigned mods ) override
	{
		update();
		gfx::PointF		at((float)x, (float)y);
		base::TimeTicks		now = base::TimeTicks::Now();
		blink::EventHandler	&h = p_.frame->GetEventHandler();
		int			m = web_mods(mods);
		blink::WebPointerProperties::Button b =
			button == 2 ? blink::WebPointerProperties::Button::kRight
			: button == 1 ? blink::WebPointerProperties::Button::kMiddle
			: blink::WebPointerProperties::Button::kLeft;

		if ( type == BR_M_MOVE ) {
			if ( down_ ) m |= blink::WebInputEvent::kLeftButtonDown;
			blink::WebMouseEvent ev(blink::WebInputEvent::Type::kMouseMove, at, at,
						down_ ? blink::WebPointerProperties::Button::kLeft
						      : blink::WebPointerProperties::Button::kNoButton,
						0, m, now);
			ev.SetFrameScale(1);
			h.HandleMouseMoveEvent(ev, blink::Vector<blink::WebMouseEvent>(),
					       blink::Vector<blink::WebMouseEvent>());
		} else if ( type == BR_M_DOWN ) {
			/* a press on a player's bar of controls is the player's */
			if ( button == 0 && br_media_click(x, y + scroll_top()) ) return;
			/* presses near each other in time and place count up (a double click) */
			if ( now - last_down_ < base::Milliseconds(kClickMs)
			     && std::abs(x - down_x_) < 4 && std::abs(y - down_y_) < 4 ) {
				clicks_++;
			} else {
				clicks_ = 1;
			}
			last_down_ = now;
			down_x_ = x;
			down_y_ = y;
			if ( button == 0 ) down_ = true;
			m |= ( button == 2 ) ? blink::WebInputEvent::kRightButtonDown
					     : blink::WebInputEvent::kLeftButtonDown;
			blink::WebMouseEvent ev(blink::WebInputEvent::Type::kMouseDown, at, at, b,
						clicks_, m, now);
			ev.SetFrameScale(1);
			h.HandleMousePressEvent(ev);
		} else {
			if ( button == 0 ) down_ = false;
			blink::WebMouseEvent ev(blink::WebInputEvent::Type::kMouseUp, at, at, b,
						clicks_, m, now);
			ev.SetFrameScale(1);
			h.HandleMouseReleaseEvent(ev);
		}
		run_tasks();
	}

	bool key( unsigned usage, unsigned mods, unsigned ch ) override
	{
		int vk = vk_of(usage);
		if ( vk == 0 && ch == 0 ) return false;
		update();
		blink::EventHandler	&h = p_.frame->GetEventHandler();
		base::TimeTicks		now = base::TimeTicks::Now();
		int			m = web_mods(mods);
		bool			took;

		blink::WebKeyboardEvent down(blink::WebInputEvent::Type::kRawKeyDown, m, now);
		fill_key(&down, vk, usage, ch);
		blink::WebInputEventResult r = h.KeyEvent(down);
		took = ( r != blink::WebInputEventResult::kNotHandled );
		/*
		 * The letter the key types, and Enter and Tab, follow as a
		 * character unless the page kept the key for itself
		 */
		bool types = ( ch >= 0x20 && ch != 0x7F ) || ch == '\r' || ch == '\t';
		if ( types && r != blink::WebInputEventResult::kHandledApplication
		     && ( m & ( blink::WebInputEvent::kControlKey | blink::WebInputEvent::kAltKey ) ) == 0 ) {
			blink::WebKeyboardEvent c(blink::WebInputEvent::Type::kChar, m, now);
			fill_key(&c, vk, usage, ch);
			c.text[0] = (char16_t)ch;
			c.unmodified_text[0] = (char16_t)ch;
			if ( h.KeyEvent(c) != blink::WebInputEventResult::kNotHandled ) took = true;
		}
		blink::WebKeyboardEvent up(blink::WebInputEvent::Type::kKeyUp, m, now);
		fill_key(&up, vk, usage, ch);
		h.KeyEvent(up);
		run_tasks();
		return took;
	}

	void wheel( int x, int y, int notches ) override
	{
		update();
		blink::EventHandler	&h = p_.frame->GetEventHandler();
		base::TimeTicks		now = base::TimeTicks::Now();
		gfx::PointF		at((float)x, (float)y);
		float			dy = (float)( notches * kWheelPx );

		/* the page's wheel listeners first; what they leave scrolls */
		blink::WebMouseWheelEvent w(blink::WebInputEvent::Type::kMouseWheel, 0, now);
		w.SetPositionInWidget(at);
		w.SetPositionInScreen(at);
		w.SetFrameScale(1);
		w.delta_y = -dy;
		w.wheel_ticks_y = (float)-notches;
		w.delta_units = ui::ScrollGranularity::kScrollByPrecisePixel;
		w.dispatch_type = blink::WebInputEvent::DispatchType::kBlocking;
		blink::WebInputEventResult r = h.HandleWheelEvent(w);
		if ( r != blink::WebInputEventResult::kHandledApplication && notches != 0 ) {
			/*
			 * The scroll a compositor would make of it, made on the
			 * main thread as the arrow keys' is: from the node under
			 * the point, each notch kWheelLines lines, passed on to
			 * the scrollers round it when one is at its end
			 */
			blink::HitTestLocation loc(at);
			blink::Node *under = h.HitTestResultAtLocation(loc).InnerNode();
			blink::mojom::blink::ScrollDirection dir =
				notches > 0 ? blink::mojom::blink::ScrollDirection::kScrollDownIgnoringWritingMode
					    : blink::mojom::blink::ScrollDirection::kScrollUpIgnoringWritingMode;
			for ( int i = 0; i < std::abs(notches) * kWheelLines; i++ ) {
				if ( !h.BubblingScroll(dir, ui::ScrollGranularity::kScrollByLine, under) ) break;
			}
		}
		run_tasks();
	}

	int scroll_top() override
	{
		blink::PaintLayerScrollableArea *sa = view()->LayoutViewport();
		scroll_ = ( sa != nullptr ) ? (int)sa->GetScrollOffset().y() : 0;
		return scroll_;
	}

	bool editable() override
	{
		return p_.frame->GetInputMethodController().TextInputType() != blink::kWebTextInputTypeNone;
	}

	void compose( const std::string &text, const std::vector<int> &clauses, int target,
		      int caret ) override
	{
		update();
		blink::InputMethodController &imc = p_.frame->GetInputMethodController();
		if ( text.empty() ) {
			if ( imc.HasComposition() ) imc.CancelComposition();
			run_tasks();
			return;
		}
		/* the clauses underlined, the one being converted thick */
		blink::Vector<blink::ImeTextSpan> spans;
		for ( size_t i = 0; i + 1 < clauses.size(); i++ ) {
			spans.push_back(blink::ImeTextSpan(
				blink::ImeTextSpan::Type::kComposition, u16_at(text, clauses[i]),
				u16_at(text, clauses[i + 1]), blink::Color::kBlack,
				(int)i == target ? ui::mojom::ImeTextSpanThickness::kThick
						 : ui::mojom::ImeTextSpanThickness::kThin,
				ui::mojom::ImeTextSpanUnderlineStyle::kSolid, blink::Color::kTransparent,
				blink::Color::kTransparent));
		}
		int c = (int)u16_at(text, caret);
		imc.SetComposition(blink::String::FromUtf8(text), spans, c, c);
		run_tasks();
	}

	void commit( const std::string &text ) override
	{
		update();
		p_.frame->GetInputMethodController().CommitText(blink::String::FromUtf8(text),
								 blink::Vector<blink::ImeTextSpan>(), 0);
		run_tasks();
	}

	std::string memory( bool collect ) override
	{
		v8::Isolate *iso = p_.frame->DomWindow() != nullptr ? p_.frame->DomWindow()->GetIsolate()
						      : v8::Isolate::GetCurrent();
		if ( collect ) {
			/* V8's heap and Blink's (one heap for both), all of it */
			iso->LowMemoryNotification();
			run_tasks();
		}
		v8::HeapStatistics hs;
		iso->GetHeapStatistics(&hs);
		size_t oilpan = 0;
		if ( iso->GetCppHeap() != nullptr ) {
			oilpan = iso->GetCppHeap()->CollectStatistics(
				cppgc::HeapStatistics::DetailLevel::kBrief).used_size_bytes;
		}
		struct mallinfo mi = mallinfo();
		char b[256];
		snprintf(b, sizeof(b), "malloc %zu KB in use (arena %zu KB), V8 %zu KB used of %zu KB, "
			 "Oilpan %zu KB, partitions %zu KB",
			 (size_t)mi.uordblks / 1024, (size_t)mi.arena / 1024, hs.used_heap_size() / 1024,
			 hs.total_heap_size() / 1024, oilpan / 1024,
			 blink::Partitions::TotalSizeOfCommittedPages() / 1024);
		return b;
	}

	bool caret( int *x, int *y, int *h ) override
	{
		update();
		gfx::Rect r = p_.frame->Selection().AbsoluteCaretBounds();
		if ( r.IsEmpty() && r.height() == 0 ) return false;
		*x = r.x();
		*y = r.y() - scroll_top();
		*h = r.height();
		return true;
	}

private:
	static constexpr int	kFrameMs = 100;
	static constexpr int	kLoadWaitS = 30;	/* the most a page waits for its parts */
	static constexpr int	kClickMs = 500;		/* presses that make a double click */
	static constexpr int	kWheelPx = 120;		/* a notch of the wheel, to the page */
	static constexpr int	kWheelLines = 3;	/* and scrolled */

	BrPage		p_;
	std::string	nav_method_, nav_ctype_, nav_body_;	/* what went with the last navigation */
	int		w_ = 800, h_ = 600, scroll_ = 0;
	base::TimeTicks	last_frame_;
	bool		down_ = false;			/* the left button is down */
	int		clicks_ = 0, down_x_ = 0, down_y_ = 0;
	base::TimeTicks	last_down_;

	blink::Document *doc() const
	{
		return p_.frame->DomWindow() != nullptr ? p_.frame->DomWindow()->document() : nullptr;
	}
	blink::LocalFrameView *view() const { return p_.frame->View(); }

	void make_page()
	{
		p_.agents = blink::ThreadScheduler::Current()->ToMainThreadScheduler()
				->CreateAgentGroupScheduler();
		p_.chrome = blink::MakeGarbageCollected<BrChromeClient>(&p_);
		p_.client = blink::MakeGarbageCollected<BrFrameClient>(&p_);
		p_.page = blink::Page::CreateOrdinary(*p_.chrome, nullptr, *p_.agents,
						      base::UnguessableToken::Create(), nullptr);
		blink::Settings &s = p_.page->GetSettings();
		s.SetScriptEnabled(true);
		s.SetImagesEnabled(true);
		s.SetLoadsImagesAutomatically(true);
		s.SetDefaultFontSize(16);
		s.SetDefaultFixedFontSize(13);
		s.SetMinimumFontSize(0);
		s.SetAcceleratedCompositingEnabled(false);
		/* a scroll is made at once: no frames come to animate it */
		s.SetScrollAnimatorEnabled(false);
		/* the page as a whole is scrolled by the window's own bars */
		s.SetHideScrollbars(true);
		blink::GenericFontFamilySettings &f = s.GetGenericFontFamilySettings();
		blink::AtomicString fam(kFamily);
		f.UpdateStandard(fam);
		f.UpdateSerif(fam);
		f.UpdateSansSerif(fam);
		f.UpdateFixed(fam);
		f.UpdateCursive(fam);
		f.UpdateFantasy(fam);
		p_.page->UpdateColorProvidersForTest();
		blink::DocumentLoader::DisableCodeCacheForTesting();

		p_.frame = blink::MakeGarbageCollected<blink::LocalFrame>(
			p_.client.Get(), *p_.page, nullptr, nullptr, nullptr,
			blink::FrameInsertType::kInsertInConstructor, blink::LocalFrameToken(),
			nullptr, nullptr, mojo::NullRemote(), base::DefaultTickClock::GetInstance());
		gfx::Size size(w_, h_);
		p_.frame->SetView(blink::MakeGarbageCollected<blink::LocalFrameView>(*p_.frame, size));
		p_.frame->View()->GetPage()->GetVisualViewport().SetSize(size);
		p_.frame->Init(nullptr, blink::DocumentToken(), nullptr, blink::StorageKey(),
			       ukm::kInvalidSourceId, blink::KURL());
	}

	void resize()
	{
		gfx::Size size(w_, h_);
		view()->Resize(size);
		p_.page->GetVisualViewport().SetSize(size);
	}

	void scroll_to( int top )
	{
		blink::PaintLayerScrollableArea *sa = view()->LayoutViewport();
		if ( sa == nullptr ) return;
		/* across, the view stays where it is */
		sa->SetScrollOffset(blink::ScrollOffset(sa->GetScrollOffset().x(), (float)top),
				    blink::mojom::blink::ScrollType::kProgrammatic,
				    cc::ScrollSourceType::kNone);
		scroll_ = (int)sa->GetScrollOffset().y();
	}

	void update()
	{
		run_tasks();
		view()->UpdateAllLifecyclePhasesForTest();
	}
};

}  // namespace

const char *br_user_agent()
{
	return "Mozilla/5.0 (X11; TessronOS aarch64) AppleWebKit/537.36 (KHTML, like Gecko) "
	       "Chrome/153.0.0.0 Safari/537.36";
}

BrEngine *br_blink_engine( sk_sp<SkData> face )
{
	static bool up;

	if ( !up ) {
		start_blink(std::move(face));
		up = true;
	}
	return new BlinkEngine;
}
