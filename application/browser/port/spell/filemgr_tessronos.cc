/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	filemgr_tessronos.cc
 *	ブラウザ: hunspell's reader of the dictionary's lines (filemgr_ts.hxx),
 *	over the bytes linked into the program (spell_tessronos.cc) in place
 *	of a file: the name hunspell is given ("en_US.aff", "en_US.dic")
 *	picks them. A line is what comes before its '\n', as std::getline
 *	gives it.
 */

#include <stdio.h>

#include <string>

#include "filemgr_ts.hxx"

/* The bytes of a dictionary's file by its name (spell_tessronos.cc); false when there is none */
bool br_spell_file( const char *name, const char **data, size_t *len );

FileMgr::FileMgr( const char *file, const char *key ) : data_(nullptr), len_(0), at_(0), linenum(0)
{
	(void)key;
	if ( file == nullptr || !br_spell_file(file, &data_, &len_) ) {
		data_ = nullptr;
		len_ = 0;
		fail("error: %s: cannot open\n", file ? file : "");
	}
}

FileMgr::~FileMgr()
{
}

int FileMgr::fail( const char *err, const char *par )
{
	fprintf(stderr, err, par);
	return -1;
}

bool FileMgr::getline( std::string &dest )
{
	if ( data_ == nullptr || at_ >= len_ ) return false;
	size_t e = at_;
	while ( e < len_ && data_[e] != '\n' ) e++;
	dest.assign(data_ + at_, e - at_);
	at_ = ( e < len_ ) ? e + 1 : len_;
	linenum++;
	return true;
}

int FileMgr::getlinenum()
{
	return linenum;
}
