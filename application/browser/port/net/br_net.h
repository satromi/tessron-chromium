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

/*
 * Told to the asker of a request whose body is handed on as it comes (an
 * event stream): first the response ('first' 1, with its status, address,
 * header lines and Content-Type), then each piece of the body as it
 * arrives; the end is told through the BrNetFetchDone with no body
 */
typedef void (*BrNetFetchPart)(void *ctx, int first, int status, const char *url,
			       const char *headers, const char *ctype,
			       const char *data, unsigned len);

/*
 * A request started as br_net_fetch_start starts one, whose body comes in
 * pieces through 'part'. It is not counted as outstanding once its
 * response has come: a stream that stays open does not hold the page.
 */
int br_net_fetch_stream(const char *url, const char *method, const void *body,
			unsigned body_len, const char *req_headers,
			BrNetFetchPart part, BrNetFetchDone done, void *ctx);

/*
 * A stream as br_net_fetch_stream starts one, paced by its asker: the
 * body is read no further while the pieces handed on and not yet taken
 * (br_net_fetch_taken) come to a window of their own, so that an asker
 * that writes them out as they come (a download) holds no more than
 * that. 'off_thread' 1: the response, the pieces and the end are told on
 * the network's own thread (in a renderer, the thread the network
 * process's answers come to), for an asker that waits on its own.
 */
int br_net_fetch_paced(const char *url, const char *method, const void *body,
		       unsigned body_len, const char *req_headers,
		       BrNetFetchPart part, BrNetFetchDone done, void *ctx,
		       int off_thread);

/* 'n' bytes of the paced stream of 'ctx' taken by its asker; any thread */
void br_net_fetch_taken(void *ctx, unsigned n);

/* A stream asked for with 'ctx' let go; its end is still told, through 'done' */
void br_net_fetch_cancel(void *ctx);

/*
 * In a renderer, whose requests the network process answers: the stream
 * of 'ctx' (br_net_fetch_paced, off its thread) taken out of this
 * process, so that the browser goes on with it (br_net_fetch_give). Its
 * callbacks are not called again, and every one called before it was
 * taken has returned. NULL when there is no network process, or when
 * the stream had already ended (its end has then been told).
 */
void *br_net_fetch_take(void *ctx);

/*
 * The download of a navigation or a link handed to the browser, which
 * takes it in as a real object as it comes (Host.Download): what was
 * taken (NULL when the stream had ended), the name it is saved under,
 * the address and the type, its length (-1 when not said), the status,
 * the bytes that came before, and whether the whole had come and well.
 * 1 when the browser has it.
 */
int br_net_fetch_give(void *taken, const char *name, const char *url, const char *type,
		      long long total, int status, const char *first, unsigned first_len,
		      int ended, int ok);

/* Whether this process asks a network process for its requests (a renderer of several processes) */
int br_net_fetch_remote(void);

/* Whether the waits are to give up now (br_net_fetch_cancel_sync) */
int br_net_fetch_cancel_asked(void);

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

/*
 * From any thread: 1 makes the waits of br_net_fetch_sync give up at
 * once (a wait that gave up answers 0), as 中止 and closing want; 0 lets
 * them wait again
 */
void br_net_fetch_cancel_sync(int on);

/*
 * Why the last request for 'url' failed: the network stack's error (a
 * negative net::Error, 0 when none is known), and its name
 * ("ERR_NAME_NOT_RESOLVED") into 'name'
 */
int br_net_fetch_error(const char *url, char *name, unsigned cap);

/* The protocol the last answer for 'url' came over ("h2"), into 'name': 1 when one is known */
int br_net_fetch_protocol(const char *url, char *name, unsigned cap);

/* How many requests have been started and not yet answered */
unsigned br_net_fetch_outstanding(void);

/* The most that were under way at once since the last call */
unsigned br_net_fetch_peak(void);

/*
 * How many responses came from the network over each protocol and over
 * which the first came, as text ("h3=N h2=N http/1.1=N other=N first=h3"
 * and a newline) into 'out'; its length
 */
int br_net_protocols(char *out, unsigned cap);

/*
 * Whether what earlier runs learnt of servers (which speak HTTP/3, their
 * QUIC sessions) is read back and this run's kept: 1 (the default) or 0.
 * Set before the network stack is built
 */
extern int br_net_keep_servers;
/* Start as a machine started again would: the session cookies kept are of another session (tests) */
extern int br_net_new_boot;
void br_net_cookies_flush(int ms);

/*
 * Whether names are looked up by the network stack's DNS client, with the
 * HTTPS records that tell of HTTP/3 (1, the default), or by the system's
 * resolver alone (0). Set before the network stack is built
 */
extern int br_net_dns_https;

/*
 * Whether HTTP/3 (QUIC) is used where a server offers it (1, the default)
 * or everything stays on TCP (0). Set before the network stack is built
 */
extern int br_net_quic;

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
