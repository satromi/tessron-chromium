/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	widevine_ks_tessronos.cc
 *	ブラウザ: components/cdm's description of the Widevine key system
 *	(cdm::WidevineKeySystemInfo), for the CDM the user installs
 *	(eme_tessronos.cc, design 14.6.3).
 *
 *	The engine is built without a Widevine CDM of its own (enable_widevine
 *	is false: none is bundled), so the build flag says there is none and
 *	the source refuses to compile. The CDM comes later, from the user:
 *	the source is compiled here with the flag saying there is one. Only
 *	this unit sees the flag changed; the rest of the engine keeps it.
 */

#include "third_party/widevine/cdm/buildflags.h"

#undef BUILDFLAG_INTERNAL_ENABLE_WIDEVINE
#define BUILDFLAG_INTERNAL_ENABLE_WIDEVINE() (1)

#include "components/cdm/renderer/widevine_key_system_info.cc"
