/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_speech_rec.cc
 *	ブラウザ: the files the speech synthesizers and the recognizer read
 *	(the dictionary, the voice, the model), from record 2 of the browser's
 *	program object (design 14.4.15).
 *
 *	They are not in the program: a program is read whole into memory when
 *	it starts, and a browser that never speaks would carry 57 MB more each
 *	time it opens a window of its own. The record is made by
 *	tools/browser/speech.sh:
 *
 *	  "BRSPCH01", the number of files (u32), then for each a name of 32
 *	  bytes, its offset and size in the record (u64 each), whether it is
 *	  zlib compressed (u32) and 4 bytes of nothing; the files follow.
 *
 *	A file is read from the record the first time it is asked for and
 *	kept as long as the browser runs.
 */

#include TS_CONFIG_H
#include <stdint.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/uapp.h>
#include <ts/sysdef.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace {

/* The program object of the browser */
const TS_UUID kProgram = {{0x01, 0xa0, 0xd8, 0xc4, 0x5b, 0x20, 0x7a, 0x10,
			       0x9b, 0x31, 0x6c, 0x2e, 0x8f, 0x4d, 0x1a, 0x57}};
constexpr int kRecord = 2;

struct Entry {
	uint64_t	off = 0, size = 0;
	bool		zipped = false;
};

std::mutex				g_lock;
bool					g_read_index = false;
std::map<std::string, Entry>		g_index;
std::map<std::string, std::vector<unsigned char>>	g_files;

/* len bytes at off of the record, in pieces of a megabyte */
bool read_at( ID key, uint64_t off, void *buf, uint64_t len )
{
	uint64_t at = 0;
	while ( at < len ) {
		SZ want = ( len - at > ( 1u << 20 ) ) ? (SZ)( 1u << 20 ) : (SZ)( len - at ), got = 0;
		if ( ob_rea_rec(key, kRecord, (D)( off + at ), (char *)buf + at, want, &got) < E_OK || got <= 0 ) return false;
		at += (uint64_t)got;
	}
	return true;
}

void read_index( ID key )
{
	unsigned char head[12];
	if ( !read_at(key, 0, head, sizeof(head)) || memcmp(head, "BRSPCH01", 8) != 0 ) {
		printf("browser: speech: no files of speech in the program (record %d)\n", kRecord);
		return;
	}
	uint32_t n;
	memcpy(&n, head + 8, 4);
	std::vector<unsigned char> ents((size_t)n * 56);
	if ( !read_at(key, 12, ents.data(), ents.size()) ) return;
	for ( uint32_t i = 0; i < n; i++ ) {
		const unsigned char *e = &ents[(size_t)i * 56];
		char name[33];
		memcpy(name, e, 32);
		name[32] = '\0';
		Entry en;
		uint32_t z;
		memcpy(&en.off, e + 32, 8);
		memcpy(&en.size, e + 40, 8);
		memcpy(&z, e + 48, 4);
		en.zipped = z != 0;
		g_index[name] = en;
	}
}

}  // namespace

/*
 * A file of speech by its name: its bytes (as they are in the record,
 * compressed when *zipped says so), kept for the browser's run; 0 when
 * there is no such file
 */
extern "C" int br_speech_rec( const char *name, const unsigned char **data, unsigned long *len, int *zipped )
{
	std::lock_guard<std::mutex> hold(g_lock);
	auto have = g_files.find(name);
	if ( have == g_files.end() ) {
		ID key = ob_opn_obj(&kProgram, OB_OP_R);
		if ( key <= 0 ) {
			printf("browser: speech: the program object is not opened (%d)\n", (int)key);
			return 0;
		}
		if ( !g_read_index ) {
			read_index(key);
			g_read_index = true;
		}
		auto it = g_index.find(name);
		if ( it == g_index.end() ) {
			ob_cls_obj(key);
			return 0;
		}
		std::vector<unsigned char> buf(it->second.size);
		bool ok = read_at(key, it->second.off, buf.data(), buf.size());
		ob_cls_obj(key);
		if ( !ok ) {
			printf("browser: speech: %s is not read from the program\n", name);
			return 0;
		}
		have = g_files.emplace(name, std::move(buf)).first;
	}
	*data = have->second.data();
	*len = (unsigned long)have->second.size();
	*zipped = g_index[name].zipped ? 1 : 0;
	return 1;
}
