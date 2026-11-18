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
 *	The names modules give their events, their event targets and
 *	IndexedDB's values (the strings an interface answers with) are made
 *	here, after core's.
 *
 *	The registry of the object URLs of MediaSource (URL.createObjectURL)
 *	is made here too: a media element looks the URL it is given up in it.
 *
 *	Values copied for another context or kept (postMessage,
 *	structuredClone, history states, IndexedDB's records) are written and
 *	read with the types modules add to the format (CryptoKey, VideoFrame,
 *	AudioData, MediaStreamTrack, MediaSourceHandle, the file system's
 *	handles, ...) as well as core's.
 *
 *	The context snapshot is not used (V8's own snapshot is linked in): a
 *	context is always made from the templates, and there is no table of
 *	external references. The snapshot's hooks (V8ContextSnapshotImpl,
 *	patch context_snapshot_emulation.patch) build the main thread's main
 *	world as if it came from a snapshot all the same, so that the global
 *	object lists its properties in the order a browser with a snapshot
 *	gives: the templates of the types a snapshot holds are made with every
 *	runtime-enabled feature off, and each context gets those types'
 *	runtime-enabled members afterwards.
 */

#include TS_CONFIG_H

#include "third_party/blink/renderer/bindings/core/v8/serialization/serialized_script_value_factory.h"
#include "third_party/blink/renderer/bindings/modules/v8/init_idl_interfaces.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_context_snapshot_impl.h"
#include "third_party/blink/renderer/bindings/modules/v8/serialization/serialized_script_value_for_modules_factory.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/modules/event_interface_modules_names.h"
#include "third_party/blink/renderer/modules/event_modules_factory.h"
#include "third_party/blink/renderer/modules/event_target_modules_names.h"
#include "third_party/blink/renderer/modules/indexed_db_names.h"
#include "third_party/blink/renderer/modules/mediasource/media_source_registry_impl.h"
#include "third_party/blink/renderer/platform/bindings/origin_trial_features.h"
#include "third_party/blink/renderer/platform/bindings/script_state.h"
#include "v8/include/v8.h"

#include "br.h"

namespace {

/* Features enabled by an origin trial: none are, so none are installed */
void no_trial_features( blink::ScriptState *, blink::mojom::blink::OriginTrialFeature ) {}

}  // namespace

void br_blink_bindings_init()
{
	blink::event_interface_names::InitModules();
	blink::event_target_names::InitModules();
	blink::indexed_db_names::Init();
	/* the modules' events document.createEvent() makes (StorageEvent, DeviceMotionEvent, ...) */
	blink::Document::RegisterEventFactory(blink::EventModulesFactory::Create());
	blink::MediaSourceRegistryImpl::Init();
	blink::SerializedScriptValueFactory::Initialize(new blink::SerializedScriptValueForModulesFactory);
	blink::bindings::InitIDLInterfaces();
	blink::SetInstallPropertiesPerFeatureFunc(no_trial_features);
	blink::V8ContextSnapshotImpl::Init();
}

/*
 * The factory of the modules' events for document.createEvent(), made by
 * the build from the modules' event names (it is not in the engine's
 * library, which takes the modules' sources only)
 */
#include "third_party/blink/renderer/modules/event_modules_factory.cc"
