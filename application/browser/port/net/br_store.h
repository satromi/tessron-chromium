/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_store.h
 *	ブラウザ: what the engine keeps between runs, in real objects
 *	(br_store.cc; design 14.8).
 */

#ifndef BR_STORE_H
#define BR_STORE_H

#ifdef __cplusplus
extern "C" {
#endif

/* br_store_load asking once, for a try again that must not wait */
int br_store_try_load( const char *name, unsigned char **data, int *len );
/* What was read makes no sense: it is left alone, as if it could not be read */
void br_store_mark_unread( const char *name );

/*
 * The store 'name' ("cookie", "storage", "cache", "indexeddb", "quic", "push", "sw",
 * "background", "caches", "perms", "opfs", "creds", "widevine", "cdmfiles"): 0 with a copy of its
 * bytes the caller frees ('len' may be 0), or a negative number when it
 * holds nothing or what it holds is damaged. "creds" is sealed: the
 * part of the user the browser acts as, decrypted (a record still kept
 * plain is read, and written again sealed at once). A store kept by
 * entries (br_store_register_keyed) answers its entry of the empty key,
 * which is what an older browser's whole record is read as.
 */
int br_store_load(const char *name, unsigned char **data, int *len);

/* The store 'name' made 'len' bytes ("creds": the user's part, encrypted), on the caller's thread: 0 or a negative number */
int br_store_save(const char *name, const void *data, int len);

/* A store's bytes now, as a buffer from malloc: 0, or not 0 to skip this save */
typedef int (*br_store_writer)(void *ctx, unsigned char **data, int *len);

/*
 * A store saved whole 'delay_ms' after it was first marked dirty: its
 * number for br_store_dirty, or -1. The writer is called on the
 * application's thread (br_store_tick) and only makes the bytes; the
 * writing is done by the store's own thread. Bytes the same as those
 * saved last are not written again.
 */
int br_store_register(const char *name, br_store_writer write, void *ctx, unsigned delay_ms);
void br_store_dirty(int id);

/*
 * Stores kept by entries: each entry a key and its bytes, and a save
 * writes only the entries put or deleted since the last (design 14.8).
 *
 * A delta is the changes of one save. br_store_submit hands it to the
 * store's thread to be written, which owns it from then on; it may be
 * called from any thread.
 */
typedef struct br_store_delta br_store_delta;
br_store_delta *br_store_delta_new(void);
void br_store_delta_put(br_store_delta *d, const void *key, int klen, const void *val, int vlen);
void br_store_delta_del(br_store_delta *d, const void *key, int klen);
int br_store_delta_count(const br_store_delta *d);
/* What is kept written afresh by this save, so that nothing deleted stays in the log */
void br_store_delta_afresh(br_store_delta *d);
void br_store_delta_free(br_store_delta *d);

/*
 * Asks a store kept by entries for its changes: called on the
 * application's thread 'delay_ms' after the store was marked dirty, and
 * when everything is flushed. It does not wait: the store makes the
 * delta where its entries live and answers with br_store_answer, once
 * for each time it is asked (with NULL when nothing changed).
 */
typedef void (*br_store_collect)(void *ctx);
int br_store_register_keyed(const char *name, br_store_collect collect, void *ctx, unsigned delay_ms);
void br_store_answer(int id, br_store_delta *d);

/*
 * A store whose saves must not wait for the application's loop: the store's
 * own thread sees when it is due, and its collect is called there.
 */
void br_store_on_own_thread(int id);

/*
 * A store other processes write too: on a thread of the store's, 'told' is called
 * when the kept entries are not what they were when last looked at, looked at
 * every 'every_ms'. It reads them with br_store_load_entries, which then asks
 * once and does not wait for another writer.
 */
typedef void (*br_store_told)(void *ctx);
void br_store_watch(int id, br_store_told told, void *ctx, unsigned every_ms);

/*
 * A store several browsers keep at once, its owner holding what it knows in
 * memory: an entry for each thing that is changed as one, so that two
 * browsers changing different things both keep theirs. Called on the
 * application's thread, as a whole store's writer is:
 *
 *	'snapshot' puts every entry there is now in 'all'; the store works out
 *	what changed since the last, and writes only that over what is kept. An
 *	entry the owner cannot say now is left as it is with br_store_delta_keep.
 *
 *	'take' is told of an entry another browser changed ('val' NULL: it
 *	deleted it). What this browser changed and has not written goes first,
 *	and wins over the other's for the same entry.
 *
 * Saved 'delay_ms' after br_store_dirty. The owner reads what is kept with
 * br_store_load_shared, the entry of the empty key being an older browser's
 * whole record, which the next save takes away.
 */
typedef void (*br_store_snapshot)(void *ctx, br_store_delta *all);
typedef void (*br_store_take)(void *ctx, const char *key, int klen, const unsigned char *val, int vlen);
int br_store_register_shared(const char *name, br_store_snapshot snapshot, br_store_take take, void *ctx, unsigned delay_ms);
void br_store_delta_keep(br_store_delta *d, const void *key, int klen);

/* Changes handed over unasked (the store made them on its own timer) */
void br_store_submit(int id, br_store_delta *d);

/*
 * The entries of a store kept by entries, each given to 'each' (its
 * key and bytes, valid during the call), in the order of their keys:
 * 0, or a negative number when there are none. An older browser's
 * whole record is given as one entry of the empty key.
 */
typedef void (*br_store_entry_fn)(void *ctx, const char *key, int klen, const unsigned char *val, int vlen);
int br_store_load_entries(const char *name, br_store_entry_fn each, void *ctx);

/* The entries of a shared store as br_store_load_entries gives them, known from now on as what is kept */
int br_store_load_shared(int id, br_store_entry_fn each, void *ctx);

/* From the application's loop: every store dirty for at least its delay saved, or asked for its changes */
void br_store_tick(void);

/*
 * Every dirty store saved, without waiting: the whole stores' bytes are
 * made here, the others asked for their changes. The number answered is
 * for br_store_wait.
 */
unsigned br_store_flush_async(void);

/* Until what a br_store_flush_async asked for is written, at most 'ms': true when it is */
int br_store_wait(unsigned ticket, int ms);

/*
 * The store 'id' alone saved, from any thread, for a store kept by entries
 * whose collect may be called anywhere: for br_store_wait.
 */
unsigned br_store_flush_one(int id);

/* At the end: every dirty store saved, and written, before it returns */
void br_store_flush_all(void);

#ifdef __cplusplus
}
#endif

#endif /* BR_STORE_H */
