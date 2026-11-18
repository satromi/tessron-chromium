/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_store_core.h
 *	ブラウザ: the reading and writing of the stores' objects, below what
 *	a process keeps of its stores in memory (br_store.cc; design 14.8,
 *	14.13.8).
 *
 *	A renderer does not open the objects: it asks the network process,
 *	which opens, reads and writes them for it (store_service_tessronos.cc).
 *	What the renderer keeps -- the stores registered, what is dirty, what
 *	it last wrote and took, what could not be read -- stays with it; what
 *	goes over the pipe is the bytes kept and the changes to write.
 */

#ifndef BR_STORE_CORE_H
#define BR_STORE_CORE_H

#include <stdint.h>

#include <map>
#include <string>
#include <vector>

/* An entry put (1) or deleted (2), as a save's delta holds it */
struct BrStoreOp {
	unsigned char	kind;
	std::string	key, val;
};

typedef std::map<std::string, std::string> BrStoreEntries;

/*
 * In the network process: the stores a renderer may ask for, the pages'
 * (the network's own -- cookie, cache, quic, hsts -- are not among them)
 */
bool br_store_for_pages( const char *name );

/*
 * The lower layer, which opens the objects (the numbers are br_store_load's).
 * 'once': asked once, not waiting for another writer. 'live' is what the
 * entries hold, for when the log is written afresh.
 */
int br_store_core_load( const char *name, bool once, std::string *bytes, uint64_t *live );
int br_store_core_entries( const char *name, bool once, BrStoreEntries *out, uint64_t *live );
bool br_store_core_version( const char *name, uint64_t *version );
/* A store kept sealed or in the older form made 'bytes' */
int br_store_core_save( const char *name, const std::string &bytes );
/* A save of the changes 'ops' of a store kept by entries: the bytes written, or a negative number */
long br_store_core_apply( const char *name, std::vector<BrStoreOp> *ops, bool whole, bool afresh,
			  uint64_t *live, bool *fresh );

/* In a renderer: the store's objects are the network process's, and asked there */
bool br_store_remote( void );
int br_store_remote_load( const char *name, bool once, std::string *bytes, uint64_t *live );
int br_store_remote_entries( const char *name, bool once, BrStoreEntries *out, uint64_t *live );
bool br_store_remote_version( const char *name, uint64_t *version );
int br_store_remote_save( const char *name, const std::string &bytes );
long br_store_remote_apply( const char *name, const std::vector<BrStoreOp> &ops, bool whole, bool afresh,
			    uint64_t *live, bool *fresh );

/* The extensions' objects (br_ext_os.cc), asked of the network process */
bool br_store_remote_ext_list( std::vector<std::string> *uuids );
bool br_store_remote_ext_read( const std::string &uuid, std::string *bytes );
bool br_store_remote_ext_state( std::string *json );
bool br_store_remote_ext_state_write( const std::string &json );
bool br_store_remote_ext_link( const std::string &uuid, const std::string &name );
bool br_store_remote_ext_unlink( const std::string &uuid );

#endif /* BR_STORE_CORE_H */
