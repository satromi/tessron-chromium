/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_widevine.h
 *	ブラウザ: the Widevine CDM a user installs (design 14.6.3).
 *
 *	The CDM is Google's own software and is not part of TessronOS or of
 *	the browser. When the user chooses 〈Widevine を導入〉 and agrees on
 *	the panel, br_widevine.cc fetches it over HTTPS as it is distributed
 *	for the processor: on aarch64 the package libwidevinecdm0 for arm64
 *	from the Raspberry Pi archive, checked against the SHA-256 the
 *	archive's index gives; on x86-64 the Widevine CDM component for
 *	Linux x64 from Google's component update service, checked against
 *	the SHA-256 the service gives and the component's signing key. The
 *	library and its manifest are kept in an object of the user's own,
 *	and the EME client (eme_tessronos.cc) loads what is kept there.
 */

#ifndef BR_WIDEVINE_H
#define BR_WIDEVINE_H

#include <string>

/* Whether the user the browser acts as has the CDM installed */
bool br_wv_installed();

/* Whether an installation is under way */
bool br_wv_busy();

/*
 * The installation started on a thread of its own: false when one is
 * already under way. 'done' is called on that thread when it ends,
 * whatever the outcome; br_wv_finish then takes it over on the
 * engine's thread.
 */
bool br_wv_install_start( void (*done)( void ) );

/*
 * On the engine's thread, after 'done': what was fetched made the
 * user's (the object kept, the user's entry pointing to it, the one it
 * replaces deleted). True when installed; 'msg' says what happened.
 */
bool br_wv_finish( std::string *msg );

/* The user's CDM taken away (its object deleted): true when it was there */
bool br_wv_remove( std::string *msg );

/*
 * The installed CDM's manifest (JSON) and library, read from the
 * user's object: false with 'err' when there is none or it cannot be read
 */
bool br_wv_read( std::string *manifest, std::string *library, std::string *err );

/* Where the packages are fetched from (tests: an http address on 127.0.0.1) */
void br_wv_set_repo( const std::string &url );

/* The two lines of the installing panel that say where the CDM comes from */
void br_wv_source( std::string *line1, std::string *line2 );

/* The UUID of the user the browser acts as, as text; "" when unknown */
std::string br_wv_user();

/* ---------------------------------------------------------------- the EME client (eme_tessronos.cc) */

/* The CDM installed or taken away: the key systems the pages are offered, made again */
void br_eme_widevine_changed();

/*
 * Whether a CDM's manifest names versions of the CDM interfaces this
 * browser speaks: its version, or false with what is wrong in 'why'
 */
bool br_eme_manifest_ok( const std::string &json, std::string *version, std::string *why );

/* ---------------------------------------------------------------- the engine (br_blink.cc) */

/* The origin of the main frame's document, "" when it has none */
std::string br_page_origin();

#endif /* BR_WIDEVINE_H */
