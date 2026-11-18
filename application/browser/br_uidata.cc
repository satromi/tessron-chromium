/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_uidata.cc
 *	ブラウザ: what the browser's windows read and change of what the
 *	engine's side keeps, with one process or with renderers (design
 *	14.13; br_uidata.h).
 *
 *	With renderers each call is an operation by name with its argument
 *	and its answer written as JSON (the Data pipe, br_process.mojom): the browser's side
 *	writes the argument and reads the answer, the renderer's side
 *	(br_rproc_data) reads the argument, calls the engine's function and
 *	writes the answer.
 */

#include <stdint.h>
#include <stdlib.h>
#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/values.h"

#include "br_proc.h"
#include "br_uidata.h"

namespace {

/* With renderers, from the browser's process: carried to them */
bool remote()
{
	return br_multi_process && !br_is_renderer;
}

std::string write( const base::Value &v )
{
	std::string out;
	(void)base::JSONWriter::Write(v, &out);
	return out;
}

base::Value read( const std::string &s )
{
	std::optional<base::Value> v = base::JSONReader::Read(s, base::JSON_PARSE_RFC);
	return v ? std::move(*v) : base::Value();
}

const std::string &str_at( const base::ListValue &l, size_t i )
{
	static const std::string none;
	return ( i < l.size() && l[i].is_string() ) ? l[i].GetString() : none;
}

bool bool_at( const base::ListValue &l, size_t i )
{
	return i < l.size() && l[i].is_bool() && l[i].GetBool();
}

int int_at( const base::ListValue &l, size_t i )
{
	return ( i < l.size() && l[i].is_int() ) ? l[i].GetInt() : 0;
}

/* A number of 64 bits, carried as text (JSON's numbers are not as wide) */
long long ll_at( const base::ListValue &l, size_t i )
{
	return atoll(str_at(l, i).c_str());
}

base::ListValue strings( const std::vector<std::string> &v )
{
	base::ListValue l;
	for ( const std::string &s : v ) l.Append(s);
	return l;
}

std::vector<std::string> strings_of( const base::Value &v )
{
	std::vector<std::string> out;
	if ( !v.is_list() ) return out;
	for ( const base::Value &e : v.GetList() ) {
		if ( e.is_string() ) out.push_back(e.GetString());
	}
	return out;
}

base::ListValue ext_list( const BrExtInfo &e )
{
	base::ListValue l;
	l.Append(e.id);
	l.Append(e.uuid);
	l.Append(e.name);
	l.Append(e.version);
	l.Append(e.enabled);
	l.Append(e.error);
	l.Append(strings(e.wants));
	return l;
}

BrExtInfo ext_of( const base::ListValue &l )
{
	BrExtInfo e;
	e.id = str_at(l, 0);
	e.uuid = str_at(l, 1);
	e.name = str_at(l, 2);
	e.version = str_at(l, 3);
	e.enabled = bool_at(l, 4);
	e.error = str_at(l, 5);
	if ( l.size() > 6 ) e.wants = strings_of(l[6]);
	return e;
}

/* An operation of the renderer of the window's page, its answer as a value */
base::Value ask( const std::string &op, const base::Value &arg, bool all = false )
{
	return read(br_proc_data(op, write(arg), all));
}

bool ask_bool( const std::string &op, const base::Value &arg, bool all = false )
{
	base::Value v = ask(op, arg, all);
	return v.is_bool() && v.GetBool();
}

base::Value list_of( std::initializer_list<std::string> items )
{
	base::ListValue l;
	for ( const std::string &s : items ) l.Append(s);
	return base::Value(std::move(l));
}

}  // namespace

/* ---------------------------------------------------------------- the browser's side */

/*
 * The passwords and passkeys are no renderer's: the browser's process
 * reads and changes what is kept itself (br_cred_core.cc), as the network
 * process does for the pages. Each operation reads the store anew, so a
 * change here is what the next page is given.
 */
std::vector<BrPasswordRow> br_ui_passwords()
{
	return br_passwords_list();
}

bool br_ui_password_change( const std::string &origin, const std::string &id, const std::string &password )
{
	return br_password_change(origin, id, password);
}

bool br_ui_password_remove( const std::string &origin, const std::string &id )
{
	return br_password_remove(origin, id);
}

std::vector<BrPasskeyRow> br_ui_passkeys()
{
	return br_passkeys_list();
}

bool br_ui_passkey_remove( const std::string &id )
{
	return br_passkey_remove(id);
}

std::vector<BrExtInfo> br_ui_exts()
{
	if ( !remote() ) return br_ext_list();
	std::vector<BrExtInfo> out;
	base::Value v = ask("exts", base::Value());
	if ( !v.is_list() ) return out;
	for ( const base::Value &e : v.GetList() ) {
		if ( e.is_list() ) out.push_back(ext_of(e.GetList()));
	}
	return out;
}

/* Every renderer keeps the extensions of its own: switched on or off, or taken away, in each */
bool br_ui_ext_enable( const std::string &id, bool on )
{
	if ( !remote() ) return br_ext_enable(id, on);
	base::ListValue l;
	l.Append(id);
	l.Append(on);
	return ask_bool("extenable", base::Value(std::move(l)), true);
}

bool br_ui_ext_remove( const std::string &id )
{
	if ( !remote() ) return br_ext_remove(id);
	return ask_bool("extremove", base::Value(id), true);
}

bool br_ui_ext_preview( const std::string &uuid, BrExtInfo *out )
{
	if ( !remote() ) return br_ext_preview(uuid, out);
	/* the object the user chose: the renderer reads it through the network process */
	br_proc_ext_allow(uuid);
	base::Value v = ask("extpreview", base::Value(uuid));
	if ( !v.is_list() ) return false;
	const base::ListValue &l = v.GetList();
	if ( l.size() > 1 && l[1].is_list() ) *out = ext_of(l[1].GetList());
	return bool_at(l, 0);
}

bool br_ui_ext_add( const std::string &uuid, std::string *why )
{
	if ( !remote() ) return br_ext_add(uuid, why);
	br_proc_ext_allow(uuid);
	base::Value v = ask("extadd", base::Value(uuid), true);
	if ( !v.is_list() ) return false;
	*why = str_at(v.GetList(), 1);
	return bool_at(v.GetList(), 0);
}

bool br_ui_ext_perm_words( const std::string &id, int msg, std::string *name, std::vector<std::string> *wants )
{
	if ( !remote() ) return br_ext_perm_words(id, msg, name, wants);
	base::ListValue a;
	a.Append(id);
	a.Append(msg);
	base::Value v = ask("extpermwords", base::Value(std::move(a)));
	if ( !v.is_list() ) return false;
	const base::ListValue &l = v.GetList();
	*name = str_at(l, 1);
	if ( l.size() > 2 ) *wants = strings_of(l[2]);
	return bool_at(l, 0);
}

void br_ui_ext_perm_answer( const std::string &id, int msg, bool yes )
{
	if ( !remote() ) {
		br_ext_perm_answer(id, msg, yes);
		return;
	}
	base::ListValue a;
	a.Append(id);
	a.Append(msg);
	a.Append(yes);
	(void)ask("extpermanswer", base::Value(std::move(a)));
}

std::vector<std::string> br_ui_sites()
{
	if ( !remote() ) return br_data_sites();
	return strings_of(ask("sites", base::Value()));
}

/*
 * The sites' data cleared: by every renderer, each of which keeps its
 * own (cookies, storage, the cache), and here, where the history is
 */
void br_ui_clear( const BrClear &c )
{
	if ( !remote() ) {
		br_data_clear(c);
		return;
	}
	br_data_clear(c);
	base::ListValue a;
	a.Append((int)c.kinds);
	a.Append(std::to_string(c.since_us));
	a.Append(c.site);
	(void)ask("clear", base::Value(std::move(a)), true);
}

/*
 * The downloads: what the browser keeps itself (what the pages save, handed
 * over by the renderers: Host.KeepDownload, Host.Download, taken in here as
 * it comes), and what the renderer of the window's page still has of its
 * own. The rows kept are in the one store
 * both read, each row once.
 */
std::vector<BrDownloadRow> br_ui_downloads()
{
	if ( !remote() ) return br_downloads_list();
	std::vector<BrDownloadRow> out;
	std::set<std::string> keys;
	for ( BrDownloadRow &r : br_downloads_list() ) {
		/* one going here (a download handed over by a renderer): stopped here, not by a renderer */
		if ( r.key.compare(0, 4, "now:") == 0 ) r.key = "browser:" + r.key;
		keys.insert(r.key);
		out.push_back(std::move(r));
	}
	base::Value v = ask("downloads", base::Value());
	if ( !v.is_list() ) return out;
	for ( const base::Value &e : v.GetList() ) {
		if ( !e.is_list() ) continue;
		const base::ListValue &l = e.GetList();
		BrDownloadRow r;
		r.key = str_at(l, 0);
		r.name = str_at(l, 1);
		r.url = str_at(l, 2);
		r.when_us = ll_at(l, 3);
		r.total = ll_at(l, 4);
		r.got = ll_at(l, 5);
		r.state = int_at(l, 6);
		if ( keys.insert(r.key).second ) out.push_back(r);
	}
	/* what is going first, then the newest */
	std::stable_sort(out.begin(), out.end(), []( const BrDownloadRow &a, const BrDownloadRow &b ) {
		bool ga = a.state == BR_DL_GOING, gb = b.state == BR_DL_GOING;
		return ga != gb ? ga : a.when_us > b.when_us;
	});
	return out;
}

/* A download the browser takes in is stopped here; one a renderer fetches is asked of each */
bool br_ui_download_cancel( const std::string &key )
{
	if ( !remote() ) return br_download_cancel(key);
	if ( key.compare(0, 8, "browser:") == 0 ) return br_download_cancel(key.substr(8));
	return ask_bool("dlcancel", base::Value(key), true);
}

void br_ui_download_forget( const std::string &key )
{
	if ( !remote() ) {
		br_download_forget(key);
		return;
	}
	/* a row kept is in the store the browser and the renderers write: off it here and in each */
	br_download_forget(key);
	(void)ask("dlforget", base::Value(key), true);
}

bool br_ui_hsts_known( const std::string &host )
{
	if ( !remote() ) return br_net_hsts_known(host);
	return ask_bool("hsts", base::Value(host));
}

/* ---------------------------------------------------------------- the renderer's side */

std::string br_rproc_data( const std::string &op, const std::string &arg )
{
	base::Value a = read(arg);
	static const base::ListValue empty;
	const base::ListValue &l = a.is_list() ? a.GetList() : empty;
	const std::string s = a.is_string() ? a.GetString() : std::string();

	if ( op == "passwords" ) {
		base::ListValue out;
		for ( const BrPasswordRow &r : br_passwords_list() ) {
			base::ListValue e;
			e.Append(r.origin);
			e.Append(r.id);
			e.Append(r.password);
			out.Append(std::move(e));
		}
		return write(base::Value(std::move(out)));
	}
	if ( op == "pwchange" ) return write(base::Value(br_password_change(str_at(l, 0), str_at(l, 1), str_at(l, 2))));
	if ( op == "pwremove" ) return write(base::Value(br_password_remove(str_at(l, 0), str_at(l, 1))));
	if ( op == "passkeys" ) {
		base::ListValue out;
		for ( const BrPasskeyRow &r : br_passkeys_list() ) {
			base::ListValue e;
			e.Append(r.rp);
			e.Append(r.name);
			e.Append(r.id);
			out.Append(std::move(e));
		}
		return write(base::Value(std::move(out)));
	}
	if ( op == "pkremove" ) return write(base::Value(br_passkey_remove(s)));
	if ( op == "exts" ) {
		base::ListValue out;
		for ( const BrExtInfo &e : br_ext_list() ) out.Append(ext_list(e));
		return write(base::Value(std::move(out)));
	}
	if ( op == "extenable" ) return write(base::Value(br_ext_enable(str_at(l, 0), bool_at(l, 1))));
	if ( op == "extremove" ) return write(base::Value(br_ext_remove(s)));
	if ( op == "extpreview" ) {
		BrExtInfo e;
		bool ok = br_ext_preview(s, &e);
		base::ListValue out;
		out.Append(ok);
		out.Append(ext_list(e));
		return write(base::Value(std::move(out)));
	}
	if ( op == "extadd" ) {
		std::string why;
		bool ok = br_ext_add(s, &why);
		base::ListValue out;
		out.Append(ok);
		out.Append(why);
		return write(base::Value(std::move(out)));
	}
	if ( op == "extpermwords" ) {
		std::string name;
		std::vector<std::string> wants;
		bool ok = br_ext_perm_words(str_at(l, 0), int_at(l, 1), &name, &wants);
		base::ListValue out;
		out.Append(ok);
		out.Append(name);
		out.Append(strings(wants));
		return write(base::Value(std::move(out)));
	}
	if ( op == "extpermanswer" ) {
		br_ext_perm_answer(str_at(l, 0), int_at(l, 1), bool_at(l, 2));
		return write(base::Value(true));
	}
	if ( op == "sites" ) return write(base::Value(strings(br_data_sites())));
	if ( op == "clear" ) {
		BrClear c;
		c.kinds = (unsigned)int_at(l, 0);
		c.since_us = ll_at(l, 1);
		c.site = str_at(l, 2);
		br_data_clear(c);
		return write(base::Value(true));
	}
	if ( op == "downloads" ) {
		base::ListValue out;
		for ( const BrDownloadRow &r : br_downloads_list() ) {
			base::ListValue e;
			e.Append(r.key);
			e.Append(r.name);
			e.Append(r.url);
			e.Append(std::to_string(r.when_us));
			e.Append(std::to_string(r.total));
			e.Append(std::to_string(r.got));
			e.Append(r.state);
			out.Append(std::move(e));
		}
		return write(base::Value(std::move(out)));
	}
	if ( op == "dlcancel" ) return write(base::Value(br_download_cancel(s)));
	if ( op == "dlforget" ) {
		br_download_forget(s);
		return write(base::Value(true));
	}
	if ( op == "hsts" ) return write(base::Value(br_net_hsts_known(s)));
	return write(base::Value());
}
