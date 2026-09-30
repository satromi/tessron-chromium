/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_net.h
 *	ブラウザ: fetching through the network stack on the network thread
 *	(net_fetch_tessronos.cc; design 17.19).
 */

#ifndef BR_NET_H
#define BR_NET_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Told to the asker when a request ends, well or not: the status, the
 * address it came from after redirects, the response's header lines
 * ("Name: value" parted by CRLF), its Content-Type, and 'len' bytes of
 * body that stay valid only for the length of the call
 */
typedef void (*BrNetFetchDone)(void *ctx, int ok, int status, const char *url,
			       const char *headers, const char *ctype,
			       const char *data, unsigned len);

/*
 * The stack built and the network thread started; called once on the
 * page's thread after the engine is up
 */
void br_net_prepare(void);

/*
 * A request started: 1 when it was taken, and 'done' is called once,
 * later, on the asker's thread; 0 when it could not be started, and
 * 'done' is never called. 'req_headers' are "Name: value" lines parted
 * by CRLF.
 */
int br_net_fetch_start(const char *url, const char *method, const void *body,
		       unsigned body_len, const char *req_headers,
		       BrNetFetchDone done, void *ctx);

/* While set, the answers of requests started on the page's thread go to its main queue */
void br_net_fetch_set_answer_on_main(int on);

/*
 * A request waited for: 1 with the answer (a body from malloc, a
 * header block from strdup, both the caller's), 0 when it failed, -1
 * when there is no network thread to ask
 */
int br_net_fetch_sync(const char *url, const char *method, const void *body,
		      unsigned body_len, const char *req_headers, char **buf,
		      unsigned *len, char *ctype, unsigned ctype_cap, int *status,
		      char **headers, char *final_url, unsigned url_cap,
		      int timeout_ms);

/* How many requests have been started and not yet answered */
unsigned br_net_fetch_outstanding(void);

/* The most that were under way at once since the last call */
unsigned br_net_fetch_peak(void);

/*
 * The cookie jar: the Cookie header's value for an address ('for_script'
 * leaves out the HttpOnly ones, as document.cookie must), and a
 * Set-Cookie line (or a script's assignment) taken in
 */
int br_net_cookie_line(const char *url, int for_script, char *out, int cap);
void br_net_cookie_set(const char *url, const char *line, int from_script);

#ifdef __cplusplus
}
#endif

#endif /* BR_NET_H */
