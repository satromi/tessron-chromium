/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_sysnet.cc
 *	ブラウザ: the network's state as the system's real object shows it
 *	in its record 7 (design 9.3.4): the IPv6 name servers ("DNS6", in the
 *	order they are asked: the settings' first, then those the routers
 *	named), the IPv6 addresses of the card's interface ("ADDRESS6"), the
 *	stack's host name ("HOSTNAME") and the interface's number
 *	("IFINDEX"). The state is read there, not through calls of its own.
 */

#include TS_CONFIG_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/ob.h>
#include <ts/sysdef.h>
#include <ts/ui.h>

namespace {

int hexval( char c )
{
	if ( c >= '0' && c <= '9' ) return c - '0';
	if ( c >= 'a' && c <= 'f' ) return c - 'a' + 10;
	if ( c >= 'A' && c <= 'F' ) return c - 'A' + 10;
	return -1;
}

/* An IPv6 address's text ("2001:db8::1", a scope after % left off) as its 16 bytes */
bool parse6( const char *s, unsigned char out[16] )
{
	uint16_t head[8], tail[8];
	int nh = 0, nt = 0;
	bool gap = false;
	const char *p = s;

	if ( p[0] == ':' && p[1] == ':' ) {
		gap = true;
		p += 2;
	}
	while ( *p != '\0' && *p != '%' ) {
		unsigned v = 0;
		int k = 0;
		while ( hexval(*p) >= 0 && k < 4 ) {
			v = v * 16 + (unsigned)hexval(*p++);
			k++;
		}
		if ( k == 0 ) return false;
		if ( gap ) {
			if ( nt >= 8 ) return false;
			tail[nt++] = (uint16_t)v;
		} else {
			if ( nh >= 8 ) return false;
			head[nh++] = (uint16_t)v;
		}
		if ( *p == ':' ) {
			p++;
			if ( *p == ':' ) {
				if ( gap ) return false;
				gap = true;
				p++;
			}
		} else if ( *p != '\0' && *p != '%' ) {
			return false;
		}
	}
	if ( nh + nt > 8 || ( !gap && nh != 8 ) ) return false;
	memset(out, 0, 16);
	for ( int i = 0; i < nh; i++ ) {
		out[2 * i] = (unsigned char)( head[i] >> 8 );
		out[2 * i + 1] = (unsigned char)head[i];
	}
	for ( int i = 0; i < nt; i++ ) {
		int at = 8 - nt + i;
		out[2 * at] = (unsigned char)( tail[i] >> 8 );
		out[2 * at + 1] = (unsigned char)tail[i];
	}
	return true;
}

}  // namespace

/*
 * Record 7 of the system's real object, read afresh: its lines are a
 * key, a tab (or a space) and a value
 */
struct Rec7 {
	UB	text[4096];
	INT	len = 0;

	bool read()
	{
		TS_UUID	u;
		SZ	asz = 0;

		if ( ui_uuid(SYSDEF_SYSTEM, &u) < E_OK ) return false;
		ID key = ob_opn_obj(&u, OB_OP_R);
		if ( key < E_OK ) return false;
		ER er = ob_rea_rec(key, OB_SYS_NET, 0, text, sizeof(text), &asz);
		ob_cls_obj(key);
		if ( er < E_OK ) return false;
		len = (INT)( asz > (SZ)sizeof(text) ? (SZ)sizeof(text) : asz );
		return true;
	}

	/* the value of the nth line (0 up) of a key into v; false when there is none */
	bool get( const char *k, int nth, char *v, int cap ) const
	{
		INT kl = (INT)strlen(k);
		for ( INT at = 0; at < len; ) {
			INT end = at;
			while ( end < len && text[end] != '\n' ) end++;
			if ( end - at > kl && memcmp(text + at, k, (size_t)kl) == 0
			     && ( text[at + kl] == '\t' || text[at + kl] == ' ' ) && nth-- == 0 ) {
				INT n = end - at - kl - 1;
				if ( n > cap - 1 ) n = cap - 1;
				memcpy(v, text + at + kl + 1, (size_t)n);
				while ( n > 0 && ( v[n - 1] == '\r' || v[n - 1] == ' ' ) ) n--;
				v[n] = '\0';
				return true;
			}
			at = end + 1;
		}
		return false;
	}
};

/*
 * The IPv6 name servers, up to max of them, 16 bytes each; how many. Read
 * on the network thread and on the DNS settings' sequence alike, so the
 * record is read into a buffer of the caller's own
 */
extern "C" int br_sys_dns6( unsigned char (*out)[16], int max )
{
	Rec7		r;
	char		v[64];
	int		n = 0;

	if ( !r.read() ) return 0;
	for ( int i = 0; n < max && r.get("DNS6", i, v, sizeof(v)); i++ ) {
		if ( parse6(v, out[n]) ) n++;
	}
	return n;
}

/*
 * The IPv6 addresses of the card's interface that are in use, up to max,
 * 16 bytes each and their prefix lengths ("ADDRESS6 <address>/<length>
 * <kind> <lifetimes> [tentative] [duplicated] [deprecated]": one still
 * being checked or that another machine has is left out); how many
 */
extern "C" int br_sys_addr6( unsigned char (*out)[16], int *plen, int max )
{
	Rec7		r;
	char		v[160];
	int		n = 0;

	if ( !r.read() ) return 0;
	for ( int i = 0; n < max && r.get("ADDRESS6", i, v, sizeof(v)); i++ ) {
		if ( strstr(v, "tentative") != nullptr || strstr(v, "duplicated") != nullptr ) continue;
		char *slash = strchr(v, '/');
		if ( slash == nullptr ) continue;
		*slash = '\0';
		if ( !parse6(v, out[n]) ) continue;
		plen[n] = atoi(slash + 1);
		n++;
	}
	return n;
}

/* The network stack's host name into name; its length, or 0 when it is not told */
extern "C" int br_sys_hostname( char *name, int cap )
{
	static Rec7	r;

	if ( cap <= 0 || !r.read() || !r.get("HOSTNAME", 0, name, cap) ) return 0;
	return (int)strlen(name);
}

/* The number of the card's interface (the scope of its link-local addresses); 2 when not told */
extern "C" unsigned br_sys_ifindex( void )
{
	static Rec7	r;
	char		v[16];

	if ( !r.read() || !r.get("IFINDEX", 0, v, sizeof(v)) || atoi(v) <= 0 ) return 2;
	return (unsigned)atoi(v);
}
