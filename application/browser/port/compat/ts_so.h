/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	ts_so.h
 *	Linux shared objects in the browser's process (design 14.6.3): an
 *	aarch64 ELF shared object made for the GNU C library, loaded from
 *	memory, its relocations applied, its symbols bound to the GNU C
 *	library's calls the process has (ts_glibc.c), its thread-local
 *	storage given to each thread as it first asks, and its initialising
 *	functions run. dlopen, dlsym and dlclose (ts_linux.c) reach what is
 *	loaded by the name it was loaded under.
 */

#ifndef __TS_SO_H__
#define __TS_SO_H__

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The shared object 'image' (size bytes, the file as it is) loaded under
 * 'name': its handle, or NULL with what went wrong in err. The image is
 * not used after the call. A name already loaded gives that object again
 * (one more reference).
 */
void	*ts_so_load( const char *name, const void *image, size_t size, char *err, size_t errsz );

/* An object loaded under 'name', one more reference to it; NULL when there is none */
void	*ts_so_find( const char *name );

/* Whether 'h' is the handle of an object loaded here */
int	ts_so_is_handle( const void *h );

/* A symbol the object exports (a function or data it defines), NULL when it has none */
void	*ts_so_sym( void *h, const char *sym );

/*
 * One reference let go; with the last, the object's ending functions
 * are run and its memory given back. 0, or -1 when 'h' is no handle.
 */
int	ts_so_close( void *h );

/* What the last failure of these calls was, NULL when none */
const char *ts_so_error( void );

/* The object an address lies in: its name and where it starts, 0 when in none */
int	ts_so_addr( const void *addr, const char **name, void **base );

/*
 * The objects loaded, each told to cb as the GNU C library's
 * dl_iterate_phdr does (its struct dl_phdr_info)
 */
int	ts_so_iterate( int (*cb)( void *info, size_t size, void *data ), void *data );

/* ---------------------------------------------------------------- the GNU C library's calls (ts_glibc.c) */

/*
 * What a symbol of the GNU C library an object asks for is bound to:
 * the address, or NULL when the process has no such thing. 'version' is
 * the version the object asks it in ("GLIBC_2.17"), NULL when none.
 */
void	*ts_glibc_sym( const char *name, const char *version );

/* Whether an object may ask for this version of the GNU C library's symbols */
int	ts_glibc_version_ok( const char *version );

/* Whether an object may need this library (one the GNU C library is made of) */
int	ts_glibc_library_ok( const char *soname );

/* The functions __cxa_atexit registered with 'dso', run and forgotten (an object let go) */
void	ts_glibc_finalize( void *dso );

#ifdef __cplusplus
}
#endif

#endif /* __TS_SO_H__ */
