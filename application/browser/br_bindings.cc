/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_bindings.cc
 *	ブラウザ: the JavaScript bindings made ready (design 17.19).
 *
 *	An interface of core that modules extend (Window, Document,
 *	Navigator, HTMLElement and about thirty more) has the code that
 *	builds its V8 templates generated among the bindings of modules, and
 *	reached from core through function pointers that
 *	bindings::InitIDLInterfaces sets. Those generated files are built
 *	with the program; the members that modules add to the interfaces
 *	answer as the port's stand-ins (tools/browser/blink_stubs.py) do.
 *
 *	The context snapshot is not used (V8's own snapshot is linked in): a
 *	context is always made from the templates, nothing is installed
 *	from a snapshot, and there is no table of external references.
 */

#include TS_CONFIG_H

#include "third_party/blink/renderer/bindings/core/v8/v8_context_snapshot.h"
#include "third_party/blink/renderer/bindings/modules/v8/init_idl_interfaces.h"
#include "third_party/blink/renderer/platform/bindings/origin_trial_features.h"
#include "third_party/blink/renderer/platform/bindings/script_state.h"
#include "v8/include/v8.h"

#include "br.h"

namespace {

v8::Local<v8::Context> no_snapshot_context( v8::Isolate *, const blink::DOMWrapperWorld &,
					    v8::Local<v8::Object>, blink::Document * )
{
	return v8::Local<v8::Context>();
}

void no_snapshot_props( blink::ScriptState * ) {}

void no_snapshot_templates( v8::Isolate * ) {}

v8::StartupData no_snapshot_take( v8::Isolate * )
{
	return v8::StartupData{ nullptr, 0 };
}

const intptr_t *no_reference_table()
{
	return nullptr;
}

/* Features enabled by an origin trial: none are, so none are installed */
void no_trial_features( blink::ScriptState *, blink::mojom::blink::OriginTrialFeature ) {}

}  // namespace

void br_blink_bindings_init()
{
	blink::bindings::InitIDLInterfaces();
	blink::SetInstallPropertiesPerFeatureFunc(no_trial_features);
	blink::V8ContextSnapshot::SetCreateContextFromSnapshotFunc(no_snapshot_context);
	blink::V8ContextSnapshot::SetInstallContextIndependentPropsFunc(no_snapshot_props);
	blink::V8ContextSnapshot::SetEnsureInterfaceTemplatesFunc(no_snapshot_templates);
	blink::V8ContextSnapshot::SetTakeSnapshotFunc(no_snapshot_take);
	blink::V8ContextSnapshot::SetGetReferenceTableFunc(no_reference_table);
}
