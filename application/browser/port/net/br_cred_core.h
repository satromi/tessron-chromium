/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_cred_core.h
 *	ブラウザ: the passwords and passkeys kept, as operations (design
 *	14.4.7, 14.13.8).
 *
 *	What is kept ("creds", sealed: br_store.cc) is read, changed and
 *	written only by a process that opens the stores' objects: the browser
 *	of one process, the browser's own process, the network process. A
 *	renderer asks the network process for what one origin may have (the
 *	passwords of an origin, the passkeys of a relying party, a passkey
 *	made, a challenge signed) and is never given the store's bytes nor a
 *	passkey's private key. The network process answers only for an origin
 *	of the site the browser gave the renderer (store_service_tessronos.cc).
 */

#ifndef BR_CRED_CORE_H
#define BR_CRED_CORE_H

#include <string>
#include <vector>

/* A password kept for an origin */
struct BrSavedPassword {
	std::string	id, name, password;
};

/* A passkey of a relying party, as a page may know it: its ID and the user's name */
struct BrPasskeyFound {
	std::string	id;
	std::string	user_name;
};

/* A passkey made: its ID, the authenticator's data, the attestation object, the public key (SPKI) */
struct BrPasskeyMade {
	std::string	id, auth_data, attestation, spki;
};

/* An assertion: the authenticator's data, the signature, the user's handle */
struct BrPasskeySigned {
	std::string	auth_data, signature, user_handle;
};

enum {
	BR_CRED_OK = 0,
	BR_CRED_REFUSED = -1,		/* not for the asker: another site, or no store */
	BR_CRED_BAD_RP = -2,		/* the relying party is not the origin's host nor a domain it is in */
	BR_CRED_EXCLUDED = -3,		/* one of the excluded credentials is kept */
	BR_CRED_NONE = -4,		/* no such passkey */
	BR_CRED_FAILED = -5,		/* the store could not be read or written, or the key is broken */
};

/* A relying party's ID may be the origin's host or a domain it is in */
bool br_cred_rp_fits( const std::string &rp, const std::string &origin );
/* The site of an origin (the scheme and the registrable domain, net::SchemefulSite), "" for none */
std::string br_cred_site_of( const std::string &origin );

/*
 * The operations, wherever the process is: done here, or asked of the
 * network process in a renderer (br_store_remote()). 'origin' is the
 * asking document's.
 */
std::vector<BrSavedPassword> br_cred_passwords( const std::string &origin );
bool br_cred_password_keep( const std::string &origin, const BrSavedPassword &p );
std::vector<BrPasskeyFound> br_cred_passkeys( const std::string &origin, const std::string &rp );
int br_cred_passkey_make( const std::string &origin, const std::string &rp, const std::string &user_id,
			  const std::string &user_name, const std::vector<std::string> &exclude, BrPasskeyMade *out );
int br_cred_passkey_sign( const std::string &origin, const std::string &rp, const std::string &id,
			  const std::string &client_data, BrPasskeySigned *out );

/* The same, done in this process (one that opens the stores' objects) */
std::vector<BrSavedPassword> br_cred_here_passwords( const std::string &origin );
bool br_cred_here_password_keep( const std::string &origin, const BrSavedPassword &p );
std::vector<BrPasskeyFound> br_cred_here_passkeys( const std::string &origin, const std::string &rp );
int br_cred_here_passkey_make( const std::string &origin, const std::string &rp, const std::string &user_id,
			       const std::string &user_name, const std::vector<std::string> &exclude, BrPasskeyMade *out );
int br_cred_here_passkey_sign( const std::string &origin, const std::string &rp, const std::string &id,
			       const std::string &client_data, BrPasskeySigned *out );

/* In a renderer: the same asked of the network process (store_service_tessronos.cc) */
std::vector<BrSavedPassword> br_store_remote_passwords( const std::string &origin );
bool br_store_remote_password_keep( const std::string &origin, const BrSavedPassword &p );
std::vector<BrPasskeyFound> br_store_remote_passkeys( const std::string &origin, const std::string &rp );
int br_store_remote_passkey_make( const std::string &origin, const std::string &rp, const std::string &user_id,
				  const std::string &user_name, const std::vector<std::string> &exclude,
				  BrPasskeyMade *out );
int br_store_remote_passkey_sign( const std::string &origin, const std::string &rp, const std::string &id,
				  const std::string &client_data, BrPasskeySigned *out );

#endif /* BR_CRED_CORE_H */
