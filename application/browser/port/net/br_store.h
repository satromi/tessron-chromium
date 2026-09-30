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
 *	ブラウザ: what the engine keeps between runs, whole, in real objects
 *	(br_store.cc; design 17.19).
 */

#ifndef BR_STORE_H
#define BR_STORE_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The store 'name' ("cookie", "storage", "cache"): 0 with a copy of its
 * bytes the caller frees ('len' may be 0), or a negative number when it
 * holds nothing or what it holds is damaged
 */
int br_store_load(const char *name, unsigned char **data, int *len);

/* The store 'name' made 'len' bytes: 0 or a negative number */
int br_store_save(const char *name, const void *data, int len);

/* A store's bytes now, as a buffer from malloc: 0, or not 0 to skip this save */
typedef int (*br_store_writer)(void *ctx, unsigned char **data, int *len);

/* A store saved 'delay_ms' after it was first marked dirty: its number for br_store_dirty, or -1 */
int br_store_register(const char *name, br_store_writer write, void *ctx, unsigned delay_ms);
void br_store_dirty(int id);

/* From the application's loop: every store dirty for at least its delay saved */
void br_store_tick(void);

/* At the end: every dirty store saved now */
void br_store_flush_all(void);

#ifdef __cplusplus
}
#endif

#endif /* BR_STORE_H */
