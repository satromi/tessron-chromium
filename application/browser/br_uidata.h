/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_uidata.h
 *	ブラウザ: what the browser's windows (〈設定〉, the extensions'
 *	panels) read and change of what the engine's side keeps
 *	(br_uidata.cc, design 14.13).
 *
 *	With one process these are the engine's own functions. With
 *	renderers, the passwords, the passkeys, the extensions, the sites'
 *	data and the downloads are kept by the renderers, and each call is
 *	carried to the renderer of the window's page (the Data pipe, br_process.mojom); a
 *	change that every renderer keeps a copy of (clearing the sites'
 *	data, an extension switched on or off or taken away, a download
 *	stopped) is carried to every renderer. The history is the browser's
 *	own and is kept here.
 */

#ifndef __BR_UIDATA_H__
#define __BR_UIDATA_H__

#include <string>
#include <vector>

#include "port/net/br_data.h"
#include "port/ext/br_ext.h"

std::vector<BrPasswordRow>	br_ui_passwords();
bool				br_ui_password_change( const std::string &origin, const std::string &id,
						       const std::string &password );
bool				br_ui_password_remove( const std::string &origin, const std::string &id );
std::vector<BrPasskeyRow>	br_ui_passkeys();
bool				br_ui_passkey_remove( const std::string &id );

std::vector<BrExtInfo>		br_ui_exts();
bool				br_ui_ext_enable( const std::string &id, bool on );
bool				br_ui_ext_remove( const std::string &id );
bool				br_ui_ext_preview( const std::string &uuid, BrExtInfo *out );
bool				br_ui_ext_add( const std::string &uuid, std::string *why );
bool				br_ui_ext_perm_words( const std::string &id, int msg, std::string *name,
						      std::vector<std::string> *wants );
void				br_ui_ext_perm_answer( const std::string &id, int msg, bool yes );

std::vector<std::string>	br_ui_sites();
void				br_ui_clear( const BrClear &c );
std::vector<BrDownloadRow>	br_ui_downloads();
bool				br_ui_download_cancel( const std::string &key );
void				br_ui_download_forget( const std::string &key );
bool				br_ui_hsts_known( const std::string &host );

/* A renderer's side: an operation of the Data pipe carried out, its answer as JSON */
std::string			br_rproc_data( const std::string &op, const std::string &arg );

/*
 * The browser's side (br_proc.cc): an operation carried to the renderer
 * of the window's page, or to every renderer ('all'; the answer is the
 * window's page's), from any thread of the browser's: the call is made on a
 * thread of its own (the data calls'), never on the engine thread
 */
std::string			br_proc_data( const std::string &op, const std::string &arg, bool all );

#endif /* __BR_UIDATA_H__ */
