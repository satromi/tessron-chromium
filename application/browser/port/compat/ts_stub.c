/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	ts_stub.c
 *	What a stand-in of tools/browser/blink_stubs.py says when it is
 *	called: the name of the function the port does not build, once for
 *	each (up to 256 names; after that, nothing more is said).
 */

#include <stdio.h>

void br_stub_note( const char *name )
{
	static const char	*seen[256];
	static int		n;
	int			i;

	for ( i = 0; i < n; i++ ) {
		if ( seen[i] == name ) return;
	}
	if ( n < 256 ) {
		seen[n++] = name;
		printf("browser: stand-in called: %s\n", name);
	}
}
