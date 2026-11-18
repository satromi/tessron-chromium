/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_objfile.cc
 *	ブラウザ: real objects as the files of a page (design 14.7.4).
 *
 *	A page that asks for a file (<input type="file">) is given a real
 *	object, and a file a page saves (a download) becomes one. The rules
 *	are those ファイル変換 follows (lib/libxfu, include/ts/xfu.h): a real
 *	object is given out as the file it was taken in from, a document as
 *	its text, anything else as its xmlTAD; a file is taken in as a
 *	document when it is a text, a figure when it is a picture, and kept
 *	as it is otherwise. Here the tree of files the library reads and
 *	writes is in memory: one file, the one given out or taken in.
 *
 *	What is saved goes into ブラウザの保存箱 (SYSDEF_BROWSER_SAVE, in the
 *	system box), and a link to it into the tray, so that it can be put
 *	wherever it is wanted.
 */

#include TS_CONFIG_H
#include <stdint.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/uapp.h>
#include <ts/sysdef.h>
#include <ts/tray.h>
#include <ts/uuid.h>
#include <ts/ui.h>
#include <ts/wm.h>
#include <ts/xfu.h>

#include <stdio.h>
#include "br_data.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "br_objfile.h"
#include "../../br.h"

namespace {

/* ---------------------------------------------------------------- a tree of one file in memory */

struct MemTree {
	std::string		name;		/* the file's name, "" until one is written */
	std::vector<uint8_t>	bytes;
	size_t			read_at = 0;
	bool			reading = false;	/* a tree to read (taken in), or to write */
};

INT mt_open( void *ctx, CONST UB *path, UINT mode )
{
	MemTree *t = (MemTree *)ctx;
	const char *p = (const char *)path;

	if ( ( mode & XFU_O_WRITE ) != 0 ) {
		if ( t->reading || !t->name.empty() ) return E_LIMIT;	/* one file, the first */
		const char *slash = strrchr(p, '/');
		t->name = ( slash != nullptr ) ? slash + 1 : p;
		t->bytes.clear();
		return 0;
	}
	if ( !t->reading || t->name != p ) return E_NOEXS;
	t->read_at = 0;
	return 0;
}

INT mt_read( void *ctx, INT, void *buf, SZ n )
{
	MemTree *t = (MemTree *)ctx;
	size_t left = t->bytes.size() - t->read_at;
	size_t k = ( (size_t)n < left ) ? (size_t)n : left;
	if ( k > 0 ) memcpy(buf, t->bytes.data() + t->read_at, k);
	t->read_at += k;
	return (INT)k;
}

INT mt_write( void *ctx, INT, CONST void *buf, SZ n )
{
	MemTree *t = (MemTree *)ctx;
	const uint8_t *b = (const uint8_t *)buf;
	t->bytes.insert(t->bytes.end(), b, b + n);
	return (INT)n;
}

ER mt_close( void *, INT )
{
	return E_OK;
}

ER mt_list( void *ctx, CONST UB *path, XFU_ENTCB cb, void *arg )
{
	MemTree *t = (MemTree *)ctx;
	T_XFUENT e;

	if ( path != nullptr && path[0] != 0 ) return E_NOEXS;
	if ( t->name.empty() ) return E_OK;
	memset(&e, 0, sizeof(e));
	snprintf((char *)e.name, sizeof(e.name), "%s", t->name.c_str());
	e.kind = XFU_K_FILE;
	e.size = (D)t->bytes.size();
	(void)cb(arg, &e);
	return E_OK;
}

ER mt_stat( void *ctx, CONST UB *path, T_XFUENT *e )
{
	MemTree *t = (MemTree *)ctx;
	const char *p = (const char *)path;

	memset(e, 0, sizeof(*e));
	if ( p == nullptr || p[0] == 0 ) {
		e->kind = XFU_K_DIR;
		e->size = -1;
		return E_OK;
	}
	if ( t->name != p ) return E_NOEXS;
	snprintf((char *)e->name, sizeof(e->name), "%s", t->name.c_str());
	e->kind = XFU_K_FILE;
	e->size = (D)t->bytes.size();
	return E_OK;
}

ER mt_mkdir( void *, CONST UB * )
{
	return E_LIMIT;			/* one file: a figure of links is not given out as a directory */
}

T_XFUTREE tree_of( MemTree *t )
{
	T_XFUTREE tr;

	memset(&tr, 0, sizeof(tr));
	tr.ctx = t;
	tr.open = mt_open;
	tr.read = mt_read;
	tr.write = mt_write;
	tr.close = mt_close;
	tr.list = mt_list;
	tr.stat = mt_stat;
	tr.mkdir = mt_mkdir;
	return tr;
}

/* ---------------------------------------------------------------- a tree of several files */

/* Files and directories by their paths from the tree's root ("top/a/b") */
struct DirTree {
	std::map<std::string, BrTreeEntry>	e;
	std::vector<std::string>		open;		/* the paths of the handles open */
	size_t					read_at = 0;
};

std::string parent_of( const std::string &p )
{
	size_t s = p.rfind('/');
	return s == std::string::npos ? std::string() : p.substr(0, s);
}

std::string base_of( const std::string &p )
{
	size_t s = p.rfind('/');
	return s == std::string::npos ? p : p.substr(s + 1);
}

INT dt_open( void *ctx, CONST UB *path, UINT mode )
{
	DirTree *t = (DirTree *)ctx;
	std::string p = (const char *)path;

	if ( ( mode & XFU_O_WRITE ) != 0 ) {
		BrTreeEntry &f = t->e[p];
		f.path = p;
		f.dir = false;
		f.bytes.clear();
	} else {
		auto it = t->e.find(p);
		if ( it == t->e.end() || it->second.dir ) return E_NOEXS;
		t->read_at = 0;
	}
	t->open.push_back(p);
	return (INT)t->open.size() - 1;
}

INT dt_read( void *ctx, INT h, void *buf, SZ n )
{
	DirTree *t = (DirTree *)ctx;
	if ( h < 0 || (size_t)h >= t->open.size() ) return E_PAR;
	const std::vector<uint8_t> &b = t->e[t->open[(size_t)h]].bytes;
	size_t left = b.size() - std::min(t->read_at, b.size());
	size_t k = ( (size_t)n < left ) ? (size_t)n : left;
	if ( k > 0 ) memcpy(buf, b.data() + t->read_at, k);
	t->read_at += k;
	return (INT)k;
}

INT dt_write( void *ctx, INT h, CONST void *buf, SZ n )
{
	DirTree *t = (DirTree *)ctx;
	if ( h < 0 || (size_t)h >= t->open.size() ) return E_PAR;
	std::vector<uint8_t> &b = t->e[t->open[(size_t)h]].bytes;
	const uint8_t *p = (const uint8_t *)buf;
	b.insert(b.end(), p, p + n);
	return (INT)n;
}

ER dt_list( void *ctx, CONST UB *path, XFU_ENTCB cb, void *arg )
{
	DirTree *t = (DirTree *)ctx;
	std::string p = path != nullptr ? (const char *)path : "";
	T_XFUENT e;

	for ( auto &kv : t->e ) {
		if ( parent_of(kv.first) != p ) continue;
		memset(&e, 0, sizeof(e));
		snprintf((char *)e.name, sizeof(e.name), "%s", base_of(kv.first).c_str());
		e.kind = kv.second.dir ? XFU_K_DIR : XFU_K_FILE;
		e.size = kv.second.dir ? -1 : (D)kv.second.bytes.size();
		if ( cb(arg, &e) < 0 ) break;
	}
	return E_OK;
}

ER dt_stat( void *ctx, CONST UB *path, T_XFUENT *e )
{
	DirTree *t = (DirTree *)ctx;
	std::string p = path != nullptr ? (const char *)path : "";

	memset(e, 0, sizeof(*e));
	if ( p.empty() ) {
		e->kind = XFU_K_DIR;
		e->size = -1;
		return E_OK;
	}
	auto it = t->e.find(p);
	if ( it == t->e.end() ) return E_NOEXS;
	snprintf((char *)e->name, sizeof(e->name), "%s", base_of(p).c_str());
	e->kind = it->second.dir ? XFU_K_DIR : XFU_K_FILE;
	e->size = it->second.dir ? -1 : (D)it->second.bytes.size();
	return E_OK;
}

ER dt_mkdir( void *ctx, CONST UB *path )
{
	DirTree *t = (DirTree *)ctx;
	std::string p = (const char *)path;
	BrTreeEntry &d = t->e[p];
	d.path = p;
	d.dir = true;
	return E_OK;
}

T_XFUTREE dir_tree_of( DirTree *t )
{
	T_XFUTREE tr;

	memset(&tr, 0, sizeof(tr));
	tr.ctx = t;
	tr.open = dt_open;
	tr.read = dt_read;
	tr.write = dt_write;
	tr.close = mt_close;
	tr.list = dt_list;
	tr.stat = dt_stat;
	tr.mkdir = dt_mkdir;
	return tr;
}

/* ---------------------------------------------------------------- the tray's links */

std::string attr_of( const std::string &tag, const char *name )
{
	std::string key = std::string(" ") + name + "=\"";
	size_t at = tag.find(key);
	if ( at == std::string::npos ) return std::string();
	at += key.size();
	size_t end = tag.find('"', at);
	if ( end == std::string::npos ) return std::string();
	std::string v = tag.substr(at, end - at);
	/* the entities a name may hold */
	std::string out;
	for ( size_t i = 0; i < v.size(); ) {
		if ( v.compare(i, 4, "&lt;") == 0 ) { out += '<'; i += 4; }
		else if ( v.compare(i, 4, "&gt;") == 0 ) { out += '>'; i += 4; }
		else if ( v.compare(i, 5, "&amp;") == 0 ) { out += '&'; i += 5; }
		else if ( v.compare(i, 6, "&quot;") == 0 ) { out += '"'; i += 6; }
		else if ( v.compare(i, 6, "&apos;") == 0 ) { out += '\''; i += 6; }
		else out += v[i++];
	}
	return out;
}

std::string xml_escape( const std::string &s )
{
	std::string o;
	for ( char c : s ) {
		switch ( c ) {
		case '<': o += "&lt;"; break;
		case '>': o += "&gt;"; break;
		case '&': o += "&amp;"; break;
		case '"': o += "&quot;"; break;
		default: o += c; break;
		}
	}
	return o;
}

/* The box what is saved goes into */
bool save_box( TS_UUID *box )
{
	return ui_uuid(SYSDEF_BROWSER_SAVE, box) >= E_OK;
}

}  // namespace

std::vector<BrObjLink> br_tray_links()
{
	std::vector<BrObjLink>	out;
	T_TRINF			inf;
	SZ			asz = 0;

	for ( INT i = 0; i < TR_REC_MAX; i++ ) {
		if ( tr_ref_rec(TR_HAND, i, &inf) < E_OK ) break;
		if ( inf.kind != TR_TAD || inf.size <= 0 || inf.size > 4 * 1024 * 1024 ) continue;
		std::string text((size_t)inf.size, '\0');
		if ( tr_rea_rec(TR_HAND, i, &text[0], inf.size, &asz) < E_OK ) continue;
		text.resize((size_t)asz);
		for ( size_t at = text.find("<link"); at != std::string::npos; at = text.find("<link", at + 5) ) {
			size_t end = text.find('>', at);
			if ( end == std::string::npos ) break;
			std::string tag = text.substr(at, end - at);
			std::string id = attr_of(tag, "id");
			if ( id.size() < 36 ) continue;
			TS_UUID u;
			if ( ui_uuid(id.substr(0, 36).c_str(), &u) < E_OK ) continue;
			BrObjLink l;
			memcpy(l.id, u.b, 16);
			l.name = br_obj_name(l.id);
			if ( l.name.empty() ) l.name = attr_of(tag, "name");
			out.push_back(l);
		}
	}
	return out;
}

std::string br_obj_name( const unsigned char id[16] )
{
	BrObject o;
	if ( !br_obj_open(id, &o) ) return std::string();
	return o.name;
}

bool br_obj_export( const unsigned char id[16], std::string *name, std::vector<uint8_t> *bytes )
{
	MemTree		t;
	T_XFUTREE	tr = tree_of(&t);
	T_XFUOPT	opt;
	TS_UUID		u;
	UB		got[XFU_NAME_MAX];

	memcpy(u.b, id, 16);
	memset(&opt, 0, sizeof(opt));
	opt.flags = XFU_F_ONE;
	got[0] = 0;
	ER er = xfu_export(&u, &tr, (CONST UB *)"", &opt, got, (INT)sizeof(got));
	if ( er < E_OK || t.name.empty() ) {
		printf("browser: %s cannot be given out as a file (%d)\n", br_uuid_str(id).c_str(), (int)er);
		return false;
	}
	*name = t.name;
	bytes->swap(t.bytes);
	return true;
}

static void tray_link( const unsigned char id[16], const std::string &fallback );

/*
 * Saves come from threads of their own (a page's downloads, a Blob, a
 * file the page writes) and may end at once; each puts its link into
 * the one save box, which is open to one writer at a time (the others
 * are refused with E_BUSY), so they go one after another.
 */
static std::mutex g_save_lock;

bool br_obj_save( const std::string &name, const std::vector<uint8_t> &bytes, unsigned char id[16] )
{
	std::lock_guard<std::mutex> hold(g_save_lock);
	MemTree		t;
	T_XFUTREE	tr = tree_of(&t);
	T_XFUOPT	opt;
	TS_UUID		box, made;

	if ( !save_box(&box) ) return false;
	t.reading = true;
	t.name = name.empty() ? std::string("download") : name;
	for ( char &c : t.name ) {
		if ( c == '/' || (unsigned char)c < 0x20 ) c = '_';
	}
	t.bytes = bytes;
	memset(&opt, 0, sizeof(opt));
	opt.as = XFU_AS_AUTO;
	opt.source = "web";
	ER er = xfu_import(&tr, (CONST UB *)t.name.c_str(), &opt, &box, &made);
	if ( er < E_OK ) {
		printf("browser: %s could not be made a real object (%d)\n", t.name.c_str(), (int)er);
		br_main_command("said:「" + t.name + "」を保存できませんでした");
		return false;
	}
	memcpy(id, made.b, 16);
	tray_link(id, t.name);
	printf("browser: saved %s (%zu bytes) as %s\n", t.name.c_str(), bytes.size(), br_uuid_str(id).c_str());
	return true;
}

namespace {

/* A tree of one file whose bytes are read as they come (br_obj_save_stream) */
struct StreamTree {
	std::string				name;
	std::function<long( void *, size_t )>	read;
};

INT st_open( void *ctx, CONST UB *path, UINT mode )
{
	StreamTree *t = (StreamTree *)ctx;
	if ( ( mode & XFU_O_WRITE ) != 0 || t->name != (const char *)path ) return E_NOEXS;
	return 0;
}

INT st_read( void *ctx, INT, void *buf, SZ n )
{
	return (INT)( (StreamTree *)ctx )->read(buf, (size_t)n );
}

INT st_write( void *, INT, CONST void *, SZ )
{
	return E_NOSPT;
}

ER st_close( void *, INT )
{
	return E_OK;
}

ER st_list( void *ctx, CONST UB *path, XFU_ENTCB cb, void *arg )
{
	StreamTree *t = (StreamTree *)ctx;
	T_XFUENT e;

	if ( path != nullptr && path[0] != 0 ) return E_NOEXS;
	memset(&e, 0, sizeof(e));
	snprintf((char *)e.name, sizeof(e.name), "%s", t->name.c_str());
	e.kind = XFU_K_FILE;
	e.size = -1;
	(void)cb(arg, &e);
	return E_OK;
}

ER st_stat( void *ctx, CONST UB *path, T_XFUENT *e )
{
	StreamTree *t = (StreamTree *)ctx;
	const char *p = (const char *)path;

	memset(e, 0, sizeof(*e));
	if ( p == nullptr || p[0] == 0 ) {
		e->kind = XFU_K_DIR;
		e->size = -1;
		return E_OK;
	}
	if ( t->name != p ) return E_NOEXS;
	snprintf((char *)e->name, sizeof(e->name), "%s", t->name.c_str());
	e->kind = XFU_K_FILE;
	e->size = -1;
	return E_OK;
}

ER st_mkdir( void *, CONST UB * )
{
	return E_NOSPT;
}

}  // namespace

bool br_obj_save_stream( const std::string &name, std::function<long( void *, size_t )> read, unsigned char id[16] )
{
	StreamTree	t;
	T_XFUTREE	tr;
	T_XFUOPT	opt;
	TS_UUID		box, made;

	if ( !save_box(&box) ) return false;
	t.name = name.empty() ? std::string("download") : name;
	for ( char &c : t.name ) {
		if ( c == '/' || (unsigned char)c < 0x20 ) c = '_';
	}
	t.read = std::move(read);
	memset(&tr, 0, sizeof(tr));
	tr.ctx = &t;
	tr.open = st_open;
	tr.read = st_read;
	tr.write = st_write;
	tr.close = st_close;
	tr.list = st_list;
	tr.stat = st_stat;
	tr.mkdir = st_mkdir;
	memset(&opt, 0, sizeof(opt));
	opt.as = XFU_AS_AUTO;
	opt.source = "web";
	opt.near = &box;
	/*
	 * Made on the box's volume with nothing linking to it while its bytes
	 * come (it is thrown away again when they fail); linked into the box
	 * once it is whole, one save at a time, the box being open to one
	 * writer at once
	 */
	ER er = xfu_import(&tr, (CONST UB *)t.name.c_str(), &opt, nullptr, &made);
	if ( er < E_OK ) {
		printf("browser: %s could not be made a real object as it came (%d)\n", t.name.c_str(), (int)er);
		return false;
	}
	{
		std::lock_guard<std::mutex> hold(g_save_lock);
		er = xfu_box_link(&box, &made);
	}
	if ( er < E_OK ) {
		printf("browser: %s could not be put in the save box (%d)\n", t.name.c_str(), (int)er);
		(void)ob_del_obj(&made);
		return false;
	}
	memcpy(id, made.b, 16);
	tray_link(id, t.name);
	printf("browser: saved %s as it came as %s\n", t.name.c_str(), br_uuid_str(id).c_str());
	return true;
}

/* A link to a real object put in the tray, and the message line told of it */
static void tray_link( const unsigned char id[16], const std::string &fallback )
{
	std::string uuid = br_uuid_str(id);
	const char *txt = uuid.c_str();
	std::string shown = br_obj_name(id);
	if ( shown.empty() ) shown = fallback;
	std::string frag = "<tad version=\"1.0\" encoding=\"UTF-8\"><figure><link id=\"" + std::string(txt)
			   + "_0.xtad\" name=\"" + xml_escape(shown) + "\" vobjleft=\"8\" vobjtop=\"8\""
			   " vobjright=\"248\" vobjbottom=\"33\" height=\"25\" chsz=\"14\" frcol=\"#000000\""
			   " chcol=\"#000000\" tbcol=\"#ffffff\" bgcol=\"#ffffff\" pictdisp=\"true\""
			   " namedisp=\"true\" framedisp=\"true\"/></figure></tad>";
	T_TRREC r;
	memset(&r, 0, sizeof(r));
	r.kind = TR_TAD;
	r.data = frag.data();
	r.size = (SZ)frag.size();
	std::string set = shown;
	if ( set.size() >= TR_NAME_MAX ) {
		/* cut before the letter that does not fit whole */
		size_t k = TR_NAME_MAX - 1;
		while ( k > 0 && ( (unsigned char)set[k] & 0xC0 ) == 0x80 ) k--;
		set.resize(k);
	}
	(void)tr_psh_dat(&r, 1, (CONST UB *)set.c_str());
	br_main_command("said:「" + shown + "」をブラウザの保存箱に入れ、トレーに仮身を置きました");
}

bool br_obj_export_tree( const unsigned char id[16], std::string *name, std::vector<BrTreeEntry> *out )
{
	DirTree		t;
	T_XFUTREE	tr = dir_tree_of(&t);
	T_XFUOPT	opt;
	TS_UUID		u;
	UB		got[XFU_NAME_MAX];

	memcpy(u.b, id, 16);
	memset(&opt, 0, sizeof(opt));
	got[0] = 0;
	ER er = xfu_export(&u, &tr, (CONST UB *)"", &opt, got, (INT)sizeof(got));
	std::string top = (const char *)got;
	auto it = t.e.find(top);
	if ( er < E_OK || top.empty() || it == t.e.end() || !it->second.dir ) {
		printf("browser: %s is not given out as a directory (%d)\n", br_uuid_str(id).c_str(), (int)er);
		return false;
	}
	*name = top;
	out->clear();
	for ( auto &kv : t.e ) {
		if ( kv.first.compare(0, top.size() + 1, top + "/") != 0 ) continue;
		BrTreeEntry e = kv.second;
		e.path = kv.first.substr(top.size() + 1);
		out->push_back(std::move(e));
	}
	return true;
}

bool br_obj_save_tree( const std::string &name, const std::vector<BrTreeEntry> &entries, unsigned char id[16] )
{
	DirTree		t;
	T_XFUTREE	tr = dir_tree_of(&t);
	T_XFUOPT	opt;
	TS_UUID		box, made;
	std::string	top = name.empty() ? std::string("folder") : name;

	if ( !save_box(&box) ) return false;
	for ( char &c : top ) {
		if ( c == '/' || (unsigned char)c < 0x20 ) c = '_';
	}
	BrTreeEntry &root = t.e[top];
	root.path = top;
	root.dir = true;
	for ( const BrTreeEntry &e : entries ) {
		BrTreeEntry c = e;
		c.path = top + "/" + e.path;
		t.e[c.path] = std::move(c);
	}
	memset(&opt, 0, sizeof(opt));
	opt.as = XFU_AS_AUTO;
	opt.source = "web";
	ER er = xfu_import(&tr, (CONST UB *)top.c_str(), &opt, &box, &made);
	if ( er < E_OK ) {
		printf("browser: the folder %s could not be made a real object (%d)\n", top.c_str(), (int)er);
		(void)wm_msg_put(( "「" + top + "」を保存できませんでした" ).c_str());
		return false;
	}
	memcpy(id, made.b, 16);
	tray_link(id, top);
	printf("browser: saved the folder %s (%zu entries) as %s\n", top.c_str(), entries.size(), br_uuid_str(id).c_str());
	return true;
}
