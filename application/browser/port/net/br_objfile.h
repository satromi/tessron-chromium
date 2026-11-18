/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_objfile.h
 *	ブラウザ: real objects as the files of a page (br_objfile.cc): what
 *	a page is given as a file, what it saves, and the links in the tray.
 */

#ifndef BR_OBJFILE_H
#define BR_OBJFILE_H

#include <stdint.h>

#include <functional>
#include <string>
#include <vector>

/* A link: the real object's UUID and its name */
struct BrObjLink {
	unsigned char	id[16];
	std::string	name;
};

/* The links of the set in hand of the tray, in their order */
std::vector<BrObjLink> br_tray_links();

/* A real object's name (its metadata's), "" when it cannot be read */
std::string br_obj_name( const unsigned char id[16] );

/*
 * A real object as the one file it is given out as (include/ts/xfu.h):
 * a file taken in, unchanged, as it was; a document as its text; anything
 * else as its xmlTAD. False when it cannot be given out.
 */
bool br_obj_export( const unsigned char id[16], std::string *name, std::vector<uint8_t> *bytes );

/*
 * Bytes a page saves made a real object in the browser's box of what is
 * saved (ブラウザの保存箱), as a file of that name is taken in (a text as
 * a document, a picture as a figure, anything else kept as it is), and a
 * link to it put in the tray. The new object's UUID in id. False when it
 * could not be made.
 */
bool br_obj_save( const std::string &name, const std::vector<uint8_t> &bytes, unsigned char id[16] );

/*
 * The same for bytes read as they come (a download): 'read' fills up to
 * the size it is given and answers how many it put there, 0 at the end,
 * or a fault below 0 (stopped, failed), when nothing of the object is
 * kept. The object is linked into the box once it is whole, and the link
 * put in the tray. Nothing is said on the message line when it fails.
 */
bool br_obj_save_stream( const std::string &name, std::function<long( void *, size_t )> read, unsigned char id[16] );

/* An entry of a tree of files: its path below the top ("a/b.txt"), whether a directory, a file's bytes */
struct BrTreeEntry {
	std::string		path;
	bool			dir = false;
	std::vector<uint8_t>	bytes;
};

/*
 * A real object given out as a directory (a figure of links is the
 * directory of what it links to): the name of the top in name, every
 * entry below it in out. False when it cannot be given out, or is given
 * out as one file and not a directory.
 */
bool br_obj_export_tree( const unsigned char id[16], std::string *name, std::vector<BrTreeEntry> *out );

/*
 * A tree of files taken in as a new real object -- a figure of links to
 * what it holds -- in ブラウザの保存箱, with a link to it put in the
 * tray. Its UUID in id; false when it could not be made.
 */
bool br_obj_save_tree( const std::string &name, const std::vector<BrTreeEntry> &entries, unsigned char id[16] );

#endif /* BR_OBJFILE_H */
