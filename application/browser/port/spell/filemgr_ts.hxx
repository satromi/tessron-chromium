/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	filemgr_ts.hxx
 *	ブラウザ: hunspell's reader of a dictionary's lines, as the build of
 *	it for the spelling has it (port/spell/filemgr_tessronos.cc). Read
 *	before every source of hunspell (tools/browser/flags.txt), it stands
 *	for hunspell's filemgr.hxx, whose include guard it takes: the reader
 *	reads the bytes linked into the program and keeps no stream of the
 *	C++ library (the program does not set the library's streams up).
 */

#ifndef FILEMGR_HXX_
#define FILEMGR_HXX_

#include <cstddef>
#include <string>

class FileMgr {
protected:
	const char	*data_;
	size_t		len_, at_;
	int		linenum;
	int fail( const char *err, const char *par );

public:
	FileMgr( const char *filename, const char *key = NULL );
	FileMgr( const FileMgr & ) = delete;
	FileMgr &operator=( const FileMgr & ) = delete;
	~FileMgr();
	bool getline( std::string &dest );
	int getlinenum();
};

#endif
