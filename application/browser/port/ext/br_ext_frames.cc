/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_ext_frames.cc
 *	ブラウザ: an extension's content scripts in a frame (design 14.12):
 *	each extension has one isolated world, which sees the frame's DOM and
 *	none of the page's script, and whose chrome object is made when the
 *	world's context is made in the frame. An extension's own page has its
 *	chrome object in its main world. What its worker asks to run in the
 *	page (chrome.scripting) comes here on the page's thread.
 */

#include <stdio.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/functional/bind.h"
#include "base/task/single_thread_task_runner.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/renderer/bindings/core/v8/script_evaluation_result.h"
#include "third_party/blink/renderer/bindings/core/v8/to_v8_traits.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/frame/frame.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/script/classic_script.h"
#include "third_party/blink/renderer/platform/bindings/dom_wrapper_world.h"
#include "third_party/blink/renderer/platform/bindings/script_state.h"
#include "third_party/blink/renderer/platform/weborigin/scheme_registry.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "v8/include/v8.h"

#include "br_ext.h"
#include "br_ext_shim.inc"

struct BrExtWorker {
	std::string	scope, script;
	bool		module;
};
std::vector<BrExtRun> br_ext_scripts_for( const std::string &url, bool main_frame, int when );
std::string br_ext_api_world( int world, const std::string &op, const std::string &arg, bool page );
std::string br_ext_api_world_later( int world, const std::string &op, const std::string &arg, bool page,
				    std::function<void( BrExtAns )> later, bool *deferred );
std::string br_ext_id_of_world( int world );
std::string br_ext_manifest_of_world( int world );
std::string br_ext_info_of_world( int world, long long frame );
int br_ext_world_of_page( const std::string &url );
std::vector<BrExtWorker> br_ext_workers();
void br_ext_read_all();
void br_sw_start_extension( const std::string &scope, const std::string &script, bool module );	/* port/blink/service_worker_tessronos.cc */
blink::LocalFrame *br_top_frame();	/* br_blink.cc: the frame of the page the window shows */

namespace {

constexpr int kFirstWorld = 1000;
constexpr int kPageBit = 1 << 20;	/* in the native function's data: the caller is an extension's page */

scoped_refptr<base::SingleThreadTaskRunner> g_page_runner;

/* The frame as the API numbers it: 0 the top */
long long frame_id( blink::LocalFrame *f )
{
	if ( f->IsOutermostMainFrame() ) return 0;
	return (long long)( f->GetLocalFrameToken().value().GetLowForSerialization() & 0x7fffffff );
}

/* A call answered later: the promise its context was given, settled on the page's thread */
struct LaterCall {
	v8::Isolate				*iso;
	v8::Global<v8::Context>			context;
	v8::Global<v8::Promise::Resolver>	resolver;
};

/* On the page's thread: the promise resolved with the answer's text, the call's handles let go */
void settle_later( LaterCall *c, std::string body )
{
	{
		v8::HandleScope hs(c->iso);
		v8::Local<v8::Context> context = c->context.Get(c->iso);
		v8::Context::Scope cs(context);
		/* the promise's reactions run when this scope ends, as after a task of the page's */
		v8::MicrotasksScope ms(context, v8::MicrotasksScope::kRunMicrotasks);
		v8::Local<v8::String> s;
		if ( v8::String::NewFromUtf8(c->iso, body.data(), v8::NewStringType::kNormal, (int)body.size()).ToLocal(&s) ) {
			(void)c->resolver.Get(c->iso)->Resolve(context, s);
		}
	}
	delete c;
}

/*
 * chrome.*'s one call, in the world whose number is its data. A question
 * that waits for an event (msg.poll, msg.answer) is answered with a
 * promise when nothing has come yet, settled when it comes (or after a
 * while): the context asks again only after something came, as the
 * worker's fetch does, instead of asking every so often
 */
void native_call( const v8::FunctionCallbackInfo<v8::Value> &a )
{
	v8::Isolate *iso = a.GetIsolate();
	int d = a.Data().As<v8::Int32>()->Value();
	v8::String::Utf8Value op(iso, a[0]), arg(iso, a[1]);
	std::string name = *op != nullptr ? *op : "", text = *arg != nullptr ? *arg : "";
	int world = d & ~kPageBit;
	bool page = ( d & kPageBit ) != 0;
	std::string out;
	v8::Local<v8::Context> context = iso->GetCurrentContext();
	v8::Local<v8::Promise::Resolver> r;
	if ( ( name == "msg.poll" || name == "msg.answer" ) && g_page_runner != nullptr
	     && v8::Promise::Resolver::New(context).ToLocal(&r) ) {
		LaterCall *c = new LaterCall{ iso, v8::Global<v8::Context>(iso, context), v8::Global<v8::Promise::Resolver>(iso, r) };
		/* answered once; what holds 'later' may be let go on another thread, so it holds no handle */
		auto once = std::make_shared<std::atomic<LaterCall *>>(c);
		bool deferred = false;
		out = br_ext_api_world_later(world, name, text, page, [once]( BrExtAns ans ) {
			LaterCall *mine = once->exchange(nullptr);
			if ( mine != nullptr ) g_page_runner->PostTask(FROM_HERE, base::BindOnce(&settle_later, mine, std::move(ans.body)));
		}, &deferred);
		if ( deferred ) {
			a.GetReturnValue().Set(r->GetPromise());
			return;
		}
		delete once->exchange(nullptr);
	} else {
		out = br_ext_api_world(world, name, text, page);
	}
	a.GetReturnValue().Set(v8::String::NewFromUtf8(iso, out.data(), v8::NewStringType::kNormal, (int)out.size()).ToLocalChecked());
}

/* The chrome object in a context: of a world's content scripts, or of an extension's page */
void make_chrome( v8::Local<v8::Context> context, int world, bool page, long long frame )
{
	std::string id = br_ext_id_of_world(world);
	if ( id.empty() ) return;
	v8::Isolate *iso = v8::Isolate::GetCurrent();
	v8::HandleScope hs(iso);
	v8::Context::Scope cs(context);
	v8::TryCatch tc(iso);
	v8::Local<v8::String> src = v8::String::NewFromUtf8(iso, kExtContentShim).ToLocalChecked();
	v8::Local<v8::Script> script;
	v8::Local<v8::Value> maker, native;
	if ( !v8::Script::Compile(context, src).ToLocal(&script) || !script->Run(context).ToLocal(&maker) || !maker->IsFunction()
	     || !v8::Function::New(context, native_call, v8::Int32::New(iso, world | ( page ? kPageBit : 0 ))).ToLocal(&native) ) {
		printf("browser: extension %s: its chrome object could not be made\n", id.c_str());
		return;
	}
	std::string man = br_ext_manifest_of_world(world), info = br_ext_info_of_world(world, frame);
	v8::Local<v8::Value> args[5] = {
		native,
		v8::String::NewFromUtf8(iso, id.c_str()).ToLocalChecked(),
		v8::String::NewFromUtf8(iso, man.data(), v8::NewStringType::kNormal, (int)man.size()).ToLocalChecked(),
		v8::String::NewFromUtf8(iso, info.data(), v8::NewStringType::kNormal, (int)info.size()).ToLocalChecked(),
		v8::String::NewFromUtf8(iso, page ? "page" : "content").ToLocalChecked(),
	};
	if ( maker.As<v8::Function>()->Call(context, context->Global(), 5, args).IsEmpty() ) {
		v8::String::Utf8Value why(iso, tc.Exception());
		printf("browser: extension %s: its chrome object could not be made: %s\n", id.c_str(), *why != nullptr ? *why : "");
	}
}

/* The results of a script in the frames it ran in, some of them promises still */
struct Injected {
	int				world, msg;
	std::vector<std::string>	results;
	int				waiting = 0;

	void Done()
	{
		if ( --waiting > 0 ) return;
		std::string s = "[";
		for ( size_t i = 0; i < results.size(); i++ ) s += ( i ? "," : "" ) + results[i];
		br_ext_inject_done(world, msg, s + "]");
		delete this;
	}
};

struct Slot {
	Injected	*in;
	size_t		at;
	long long	frame;
};

std::string result_of( v8::Local<v8::Context> context, long long frame, v8::Local<v8::Value> v )
{
	v8::Isolate *iso = v8::Isolate::GetCurrent();
	std::string text = "null";
	v8::Local<v8::String> s;
	if ( !v.IsEmpty() && !v->IsUndefined() && v8::JSON::Stringify(context, v).ToLocal(&s) ) {
		v8::String::Utf8Value u(iso, s);
		if ( *u != nullptr ) text = *u;
	}
	return "{\"frameId\":" + std::to_string(frame) + ",\"result\":" + text + "}";
}

void promise_settled( const v8::FunctionCallbackInfo<v8::Value> &a )
{
	std::unique_ptr<Slot> slot(static_cast<Slot *>(a.Data().As<v8::External>()->Value(v8::kExternalPointerTypeTagDefault)));
	slot->in->results[slot->at] = result_of(a.GetIsolate()->GetCurrentContext(), slot->frame, a[0]);
	slot->in->Done();
}

void run_one( blink::LocalFrame *f, const BrExtInject &what, Injected *in )
{
	if ( f->DomWindow() == nullptr ) return;
	long long id = frame_id(f);
	blink::Document *doc = f->GetDocument();
	if ( what.code.empty() ) {
		blink::WebDocument wd(doc);
		blink::WebStyleSheetKey key = blink::WebString::FromUtf8(what.key);
		blink::WebCssOrigin origin = what.user_origin ? blink::WebCssOrigin::kUser : blink::WebCssOrigin::kAuthor;
		if ( what.remove ) wd.RemoveInsertedStyleSheet(key, origin);
		else wd.InsertStyleSheet(blink::WebString::FromUtf8(what.css), &key, origin);
		return;
	}
	v8::Isolate *iso = v8::Isolate::GetCurrent();
	v8::HandleScope hs(iso);
	blink::ClassicScript *script = blink::ClassicScript::CreateUnspecifiedScript(blink::String::FromUtf8(what.code));
	blink::ScriptEvaluationResult r = what.main_world
		? script->RunScriptAndReturnValue(f->DomWindow())
		: script->RunScriptInIsolatedWorldAndReturnValue(f->DomWindow(), what.world);
	if ( r.GetResultType() != blink::ScriptEvaluationResult::ResultType::kSuccess || f->DomWindow() == nullptr ) {
		in->results.push_back("{\"frameId\":" + std::to_string(id) + ",\"result\":null}");
		return;
	}
	v8::Local<v8::Value> v = r.GetSuccessValueOrEmpty();
	blink::DOMWrapperWorld *world = what.main_world ? &blink::DOMWrapperWorld::MainWorld(iso)
							: blink::DOMWrapperWorld::EnsureIsolatedWorld(iso, what.world);
	v8::Local<v8::Context> context = blink::ToScriptState(f, *world)->GetContext();
	v8::Context::Scope cs(context);
	if ( !v.IsEmpty() && v->IsPromise() ) {
		/* the frame's answer is what the promise comes to */
		in->results.emplace_back();
		in->waiting++;
		auto *slot = new Slot{ in, in->results.size() - 1, id };
		v8::Local<v8::External> data = v8::External::New(iso, slot, v8::kExternalPointerTypeTagDefault);
		v8::Local<v8::Function> settled;
		if ( v8::Function::New(context, promise_settled, data).ToLocal(&settled)
		     && !v.As<v8::Promise>()->Then(context, settled, settled).IsEmpty() ) return;
		delete slot;
		in->waiting--;
		in->results.back() = "{\"frameId\":" + std::to_string(id) + ",\"result\":null}";
		return;
	}
	in->results.push_back(result_of(context, id, v));
}

/* On the page's thread: in the frames the call names */
void run_inject( const BrExtInject &what )
{
	auto *in = new Injected{ what.world, what.msg, {}, 1 };
	blink::LocalFrame *top = br_top_frame();
	if ( top != nullptr ) {
		if ( !what.all_frames && what.frames.empty() ) {
			run_one(top, what, in);
		} else {
			for ( blink::Frame *f = top; f != nullptr; f = f->Tree().TraverseNext() ) {
				blink::LocalFrame *l = blink::DynamicTo<blink::LocalFrame>(f);
				if ( l == nullptr ) continue;
				bool named = what.all_frames;
				for ( long long want : what.frames ) named = named || want == frame_id(l);
				if ( named ) run_one(l, what, in);
			}
		}
	}
	in->Done();
}

}  // namespace

void br_ext_inject( const BrExtInject &what )
{
	if ( g_page_runner == nullptr ) {
		br_ext_inject_done(what.world, what.msg, "[]");
		return;
	}
	g_page_runner->PostTask(FROM_HERE, base::BindOnce(&run_inject, what));
}

/* An extension's world made in a frame: its chrome object */
void br_ext_world_context( v8::Local<v8::Context> context, int world )
{
	if ( world < kFirstWorld ) return;
	blink::LocalDOMWindow *w = blink::ToLocalDOMWindow(context);
	make_chrome(context, world, false, w != nullptr && w->GetFrame() != nullptr ? frame_id(w->GetFrame()) : 0);
}

/* A frame's main world: an extension's page has its chrome object there */
void br_ext_page_context( v8::Local<v8::Context> context )
{
	blink::LocalDOMWindow *w = blink::ToLocalDOMWindow(context);
	if ( w == nullptr || w->GetFrame() == nullptr ) return;
	int world = br_ext_world_of_page(w->Url().GetString().Utf8());
	if ( world != 0 ) make_chrome(context, world, true, frame_id(w->GetFrame()));
}

/*
 * A moment of a frame's document: 0 the document's start, 1 its end, 2 idle.
 * The engine says it is idle more than once: a script runs once in a document.
 */
void br_ext_moment( blink::LocalFrame *frame, int when )
{
	if ( frame == nullptr || frame->DomWindow() == nullptr || frame->GetDocument() == nullptr ) return;
	blink::Document *doc = frame->GetDocument();
	static std::map<std::string, int> seen;
	int &done = seen[doc->Token().ToString()];
	if ( done & ( 1 << when ) ) return;
	done |= 1 << when;
	if ( seen.size() > 1024 ) seen.clear();		/* of documents long gone; one still open may at worst run again */
	std::string url = doc->Url().GetString().Utf8();
	std::vector<BrExtRun> runs = br_ext_scripts_for(url, frame->IsOutermostMainFrame(), when);
	for ( const BrExtRun &r : runs ) {
		printf("browser: extension %s: its content script in %s (moment %d)\n", r.id.c_str(), url.c_str(), when);
		if ( !r.css.empty() ) {
			blink::WebDocument(doc).InsertStyleSheet(blink::WebString::FromUtf8(r.css));
		}
		if ( r.code.empty() ) continue;
		v8::HandleScope hs(v8::Isolate::GetCurrent());
		blink::ClassicScript *script = blink::ClassicScript::CreateUnspecifiedScript(blink::String::FromUtf8(r.code));
		if ( r.main_world ) (void)script->RunScriptAndReturnValue(frame->DomWindow());
		else (void)script->RunScriptInIsolatedWorldAndReturnValue(frame->DomWindow(), r.world);
		if ( frame->DomWindow() == nullptr ) return;	/* the script took the frame away */
	}
}

/* What the browser has when it starts: the extensions' objects read, their workers started */
void br_ext_start()
{
	g_page_runner = base::SingleThreadTaskRunner::GetCurrentDefault();
	/* the worker reaches its files and the API by fetch */
	blink::SchemeRegistry::RegisterURLSchemeAsSupportingFetchAPI("chrome-extension");
	br_ext_read_all();
	for ( const BrExtWorker &w : br_ext_workers() ) br_sw_start_extension(w.scope, w.script, w.module);
}
