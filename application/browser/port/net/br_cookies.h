/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_cookies.h
 *	ブラウザ: the network stack's cookie jar as scripts' cookieStore sees
 *	it (net_fetch_tessronos.cc).
 */

#ifndef BR_COOKIES_H
#define BR_COOKIES_H

#include <memory>

#include "base/functional/callback.h"
#include "net/cookies/canonical_cookie.h"
#include "net/cookies/cookie_change_dispatcher.h"
#include "url/gurl.h"

/* The cookies of url a script may see (no HttpOnly one), as the jar holds them; waits for the jar */
net::CookieList br_net_cookies( const GURL &url );

/* A cookie a script sets (or deletes, expired) for url: whether the jar took it; waits for the jar */
bool br_net_cookie_put( std::unique_ptr<net::CanonicalCookie> cookie, const GURL &url );

/*
 * The changes of url's cookies, told to 'told' on the network's thread
 * until br_net_cookie_unwatch: a number for the watch, 0 when there is
 * no jar.
 */
int br_net_cookie_watch( const GURL &url, base::RepeatingCallback<void( const net::CookieChangeInfo & )> told );
void br_net_cookie_unwatch( int watch );

#endif /* BR_COOKIES_H */
