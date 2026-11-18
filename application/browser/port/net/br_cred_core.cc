/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_cred_core.cc
 *	ブラウザ: the passwords and passkeys kept, read and changed as
 *	operations by a process that opens the stores' objects (design 14.4.7,
 *	14.13.8; br_cred_core.h).
 *
 *	Nothing of the store stays in memory between operations: each one
 *	reads the sealed store, does what it is for and, when it changed
 *	something, writes the store back at once, under one lock of this
 *	process. Another process that changed the store (the network process
 *	of another browser, the settings window of this one) is so seen by the
 *	next operation, with no notice between them. The passkeys' private
 *	keys are used here to sign and never handed out.
 *
 *	The form of the store is the one the browser has always written:
 *	{"passkeys": [{"id", "key", "rp", "user", "name", "count"}],
 *	 "passwords": [{"origin", "id", "name", "password"}]}, the bytes in
 *	base64url.
 */

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <array>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "base/base64url.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/values.h"
#include "crypto/hash.h"
#include "crypto/keypair.h"
#include "crypto/random.h"
#include "crypto/sign.h"
#include "net/base/schemeful_site.h"
#include "third_party/boringssl/src/include/openssl/mem.h"
#include "url/gurl.h"

#include "br_cred_core.h"
#include "br_data.h"
#include "br_store_core.h"

namespace {

/* ---------------------------------------------------------------- CBOR, as WebAuthn writes it */

void cbor_head( std::string &o, int major, uint64_t v )
{
	unsigned char m = (unsigned char)( major << 5 );
	if ( v < 24 ) {
		o += (char)( m | v );
	} else if ( v < 0x100 ) {
		o += (char)( m | 24 );
		o += (char)v;
	} else if ( v < 0x10000 ) {
		o += (char)( m | 25 );
		o += (char)( v >> 8 );
		o += (char)v;
	} else {
		o += (char)( m | 26 );
		for ( int s = 24; s >= 0; s -= 8 ) o += (char)( v >> s );
	}
}

void cbor_int( std::string &o, int64_t v )
{
	if ( v >= 0 ) cbor_head(o, 0, (uint64_t)v);
	else cbor_head(o, 1, (uint64_t)( -1 - v ));
}

void cbor_bytes( std::string &o, const std::string &b )
{
	cbor_head(o, 2, b.size());
	o += b;
}

void cbor_text( std::string &o, const char *t )
{
	cbor_head(o, 3, strlen(t));
	o += t;
}

std::string b64( const std::string &s )
{
	std::string o;
	base::Base64UrlEncode(s, base::Base64UrlEncodePolicy::OMIT_PADDING, &o);
	return o;
}

std::string unb64( const std::string *s )
{
	std::string o;
	if ( s == nullptr || !base::Base64UrlDecode(*s, base::Base64UrlDecodePolicy::IGNORE_PADDING, &o) ) return std::string();
	return o;
}

void wipe( std::string *s )
{
	if ( !s->empty() ) OPENSSL_cleanse(&( *s )[0], s->size());
}

/* ---------------------------------------------------------------- what is kept */

struct Passkey {
	std::string		id;		/* the credential's ID, 16 random bytes */
	std::string		pkcs8;		/* its private key */
	std::string		rp;
	std::string		user_id, user_name;
	uint32_t		count = 0;
};

struct Password {
	std::string	origin, id, name, password;
};

struct Creds {
	std::vector<Passkey>	keys;
	std::vector<Password>	pws;

	~Creds()
	{
		for ( Passkey &k : keys ) wipe(&k.pkcs8);
		for ( Password &p : pws ) wipe(&p.password);
	}
};

std::mutex	g_lock;		/* one operation at a time in this process */

/* The store read: false when it is there and could not be read (nothing may then be written over it) */
bool read_creds( Creds *c )
{
	std::string json;
	uint64_t live = 0;
	int rc = br_store_core_load("creds", false, &json, &live);
	if ( rc == -3 ) return true;
	if ( rc != 0 ) {
		printf("browser: credentials: the store not read (%d)\n", rc);
		return false;
	}
	std::optional<base::Value> v = base::JSONReader::Read(json, base::JSON_PARSE_RFC);
	wipe(&json);
	if ( !v || !v->is_dict() ) {
		printf("browser: credentials: the store makes no sense: left alone\n");
		return false;
	}
	if ( const base::ListValue *keys = v->GetDict().FindList("passkeys") ) {
		for ( const base::Value &e : *keys ) {
			if ( !e.is_dict() ) continue;
			const base::DictValue &d = e.GetDict();
			Passkey k;
			k.id = unb64(d.FindString("id"));
			k.pkcs8 = unb64(d.FindString("key"));
			const std::string *rp = d.FindString("rp"), *name = d.FindString("name");
			k.rp = rp != nullptr ? *rp : std::string();
			k.user_id = unb64(d.FindString("user"));
			k.user_name = name != nullptr ? *name : std::string();
			k.count = (uint32_t)d.FindInt("count").value_or(0);
			if ( !k.id.empty() && !k.pkcs8.empty() && !k.rp.empty() ) c->keys.push_back(std::move(k));
		}
	}
	if ( const base::ListValue *pws = v->GetDict().FindList("passwords") ) {
		for ( const base::Value &e : *pws ) {
			if ( !e.is_dict() ) continue;
			const base::DictValue &d = e.GetDict();
			const std::string *o = d.FindString("origin"), *id = d.FindString("id");
			const std::string *n = d.FindString("name"), *pw = d.FindString("password");
			if ( o == nullptr || id == nullptr || pw == nullptr ) continue;
			c->pws.push_back({ *o, *id, n != nullptr ? *n : std::string(), *pw });
		}
	}
	return true;
}

bool write_creds( const Creds &c )
{
	base::ListValue keys, pws;
	for ( const Passkey &k : c.keys ) {
		base::DictValue e;
		e.Set("id", b64(k.id));
		e.Set("key", b64(k.pkcs8));
		e.Set("rp", k.rp);
		e.Set("user", b64(k.user_id));
		e.Set("name", k.user_name);
		e.Set("count", (int)k.count);
		keys.Append(std::move(e));
	}
	for ( const Password &p : c.pws ) {
		base::DictValue e;
		e.Set("origin", p.origin);
		e.Set("id", p.id);
		e.Set("name", p.name);
		e.Set("password", p.password);
		pws.Append(std::move(e));
	}
	base::DictValue all;
	all.Set("passkeys", std::move(keys));
	all.Set("passwords", std::move(pws));
	std::string json;
	(void)base::JSONWriter::Write(all, &json);
	int rc = br_store_core_save("creds", json);
	wipe(&json);
	if ( rc != 0 ) printf("browser: credentials: the store not written (%d)\n", rc);
	return rc == 0;
}

/* The authenticator's data: the relying party's hash, the flags, the count, and (made) the credential */
std::string auth_data( const std::string &rp, uint8_t flags, uint32_t count, const std::string *made_id,
		       const std::vector<uint8_t> &point )
{
	std::array<uint8_t, 32> h = crypto::hash::Sha256(rp);
	std::string d(h.begin(), h.end());
	d += (char)flags;
	for ( int s = 24; s >= 0; s -= 8 ) d += (char)( count >> s );
	if ( made_id != nullptr ) {
		d += std::string(16, '\0');			/* the AAGUID: none said */
		d += (char)( made_id->size() >> 8 );
		d += (char)made_id->size();
		d += *made_id;
		/* the public key as a COSE key: EC2, ES256, P-256, x, y */
		std::string k;
		cbor_head(k, 5, 5);
		cbor_int(k, 1); cbor_int(k, 2);
		cbor_int(k, 3); cbor_int(k, -7);
		cbor_int(k, -1); cbor_int(k, 1);
		cbor_int(k, -2); cbor_bytes(k, std::string(point.begin() + 1, point.begin() + 33));
		cbor_int(k, -3); cbor_bytes(k, std::string(point.begin() + 33, point.begin() + 65));
		d += k;
	}
	return d;
}

constexpr uint8_t	kUP = 0x01, kUV = 0x04, kAT = 0x40;

/* What clientDataJSON says it is for: its type and origin as the asker wrote them */
bool client_data_for( const std::string &client_data, const char *type, const std::string &origin )
{
	std::optional<base::DictValue> d = base::JSONReader::ReadDict(client_data, base::JSON_PARSE_RFC);
	if ( !d ) return false;
	const std::string *t = d->FindString("type"), *o = d->FindString("origin");
	return t != nullptr && o != nullptr && *t == type && *o == origin;
}

}  // namespace

std::string br_cred_site_of( const std::string &origin )
{
	GURL u(origin);
	if ( !u.is_valid() || !u.SchemeIsHTTPOrHTTPS() ) return std::string();
	return net::SchemefulSite(u).Serialize();
}

bool br_cred_rp_fits( const std::string &rp, const std::string &origin )
{
	std::string host(GURL(origin).host());
	if ( rp.empty() || host.empty() ) return false;
	if ( rp == host ) return true;
	return host.size() > rp.size() && host.compare(host.size() - rp.size(), rp.size(), rp) == 0
	       && host[host.size() - rp.size() - 1] == '.';
}

/* ---------------------------------------------------------------- the operations, here */

std::vector<BrSavedPassword> br_cred_here_passwords( const std::string &origin )
{
	std::lock_guard<std::mutex> hold(g_lock);
	std::vector<BrSavedPassword> out;
	Creds c;
	if ( origin.empty() || !read_creds(&c) ) return out;
	for ( const Password &p : c.pws ) {
		if ( p.origin == origin ) out.push_back({ p.id, p.name, p.password });
	}
	return out;
}

bool br_cred_here_password_keep( const std::string &origin, const BrSavedPassword &p )
{
	std::lock_guard<std::mutex> hold(g_lock);
	Creds c;
	if ( origin.empty() || !read_creds(&c) ) return false;
	c.pws.erase(std::remove_if(c.pws.begin(), c.pws.end(),
				   [&]( const Password &q ) { return q.origin == origin && q.id == p.id; }),
		    c.pws.end());
	c.pws.push_back({ origin, p.id, p.name, p.password });
	bool ok = write_creds(c);
	if ( ok ) printf("browser: credentials: the password of %s for %s kept\n", p.id.c_str(), origin.c_str());
	return ok;
}

std::vector<BrPasskeyFound> br_cred_here_passkeys( const std::string &origin, const std::string &rp )
{
	std::lock_guard<std::mutex> hold(g_lock);
	std::vector<BrPasskeyFound> out;
	Creds c;
	if ( !br_cred_rp_fits(rp, origin) || !read_creds(&c) ) return out;
	for ( const Passkey &k : c.keys ) {
		if ( k.rp == rp ) out.push_back({ k.id, k.user_name });
	}
	return out;
}

int br_cred_here_passkey_make( const std::string &origin, const std::string &rp, const std::string &user_id,
			       const std::string &user_name, const std::vector<std::string> &exclude, BrPasskeyMade *out )
{
	std::lock_guard<std::mutex> hold(g_lock);
	if ( !br_cred_rp_fits(rp, origin) ) return BR_CRED_BAD_RP;
	Creds c;
	if ( !read_creds(&c) ) return BR_CRED_FAILED;
	for ( const Passkey &k : c.keys ) {
		if ( k.rp == rp && std::find(exclude.begin(), exclude.end(), k.id) != exclude.end() ) return BR_CRED_EXCLUDED;
	}
	crypto::keypair::PrivateKey key = crypto::keypair::PrivateKey::GenerateEcP256();
	crypto::keypair::PublicKey pub = crypto::keypair::PublicKey::FromPrivateKey(key);
	std::vector<uint8_t> point = pub.ToUncompressedX962Point();
	std::vector<uint8_t> pkcs8 = key.ToPrivateKeyInfo();
	Passkey made;
	made.rp = rp;
	made.user_id = user_id;
	made.user_name = user_name;
	made.pkcs8.assign(pkcs8.begin(), pkcs8.end());
	OPENSSL_cleanse(pkcs8.data(), pkcs8.size());
	made.id.resize(16);
	crypto::RandBytes(base::as_writable_byte_span(made.id));
	out->id = made.id;
	out->auth_data = auth_data(rp, kUP | kUV | kAT, 0, &made.id, point);
	/* the attestation object: {"fmt": "none", "attStmt": {}, "authData": ...} */
	out->attestation.clear();
	cbor_head(out->attestation, 5, 3);
	cbor_text(out->attestation, "fmt"); cbor_text(out->attestation, "none");
	cbor_text(out->attestation, "attStmt"); cbor_head(out->attestation, 5, 0);
	cbor_text(out->attestation, "authData"); cbor_bytes(out->attestation, out->auth_data);
	std::vector<uint8_t> spki = pub.ToSubjectPublicKeyInfo();
	out->spki.assign(spki.begin(), spki.end());
	c.keys.push_back(std::move(made));
	if ( !write_creds(c) ) return BR_CRED_FAILED;
	printf("browser: webauthn: a passkey for %s (%s) made\n", rp.c_str(), user_name.c_str());
	return BR_CRED_OK;
}

int br_cred_here_passkey_sign( const std::string &origin, const std::string &rp, const std::string &id,
			       const std::string &client_data, BrPasskeySigned *out )
{
	std::lock_guard<std::mutex> hold(g_lock);
	if ( !br_cred_rp_fits(rp, origin) ) return BR_CRED_BAD_RP;
	if ( !client_data_for(client_data, "webauthn.get", origin) ) {
		printf("browser: webauthn: the client data is not of %s: not signed\n", origin.c_str());
		return BR_CRED_REFUSED;
	}
	Creds c;
	if ( !read_creds(&c) ) return BR_CRED_FAILED;
	Passkey *k = nullptr;
	for ( Passkey &x : c.keys ) {
		if ( x.rp == rp && x.id == id ) k = &x;
	}
	if ( k == nullptr ) return BR_CRED_NONE;
	std::optional<crypto::keypair::PrivateKey> key = crypto::keypair::PrivateKey::FromPrivateKeyInfo(
		base::as_byte_span(k->pkcs8));
	if ( !key ) return BR_CRED_FAILED;
	k->count++;
	if ( !write_creds(c) ) return BR_CRED_FAILED;
	out->auth_data = auth_data(k->rp, kUP | kUV, k->count, nullptr, {});
	std::array<uint8_t, 32> ch = crypto::hash::Sha256(client_data);
	std::string signed_over = out->auth_data + std::string(ch.begin(), ch.end());
	std::vector<uint8_t> sig = crypto::sign::Sign(crypto::sign::ECDSA_SHA256, *key, base::as_byte_span(signed_over));
	out->signature.assign(sig.begin(), sig.end());
	out->user_handle = k->user_id;
	printf("browser: webauthn: signed in to %s as %s\n", k->rp.c_str(), k->user_name.c_str());
	return BR_CRED_OK;
}

/* ---------------------------------------------------------------- the operations, wherever the process is */

std::vector<BrSavedPassword> br_cred_passwords( const std::string &origin )
{
	return br_store_remote() ? br_store_remote_passwords(origin) : br_cred_here_passwords(origin);
}

bool br_cred_password_keep( const std::string &origin, const BrSavedPassword &p )
{
	return br_store_remote() ? br_store_remote_password_keep(origin, p) : br_cred_here_password_keep(origin, p);
}

std::vector<BrPasskeyFound> br_cred_passkeys( const std::string &origin, const std::string &rp )
{
	return br_store_remote() ? br_store_remote_passkeys(origin, rp) : br_cred_here_passkeys(origin, rp);
}

int br_cred_passkey_make( const std::string &origin, const std::string &rp, const std::string &user_id,
			  const std::string &user_name, const std::vector<std::string> &exclude, BrPasskeyMade *out )
{
	return br_store_remote() ? br_store_remote_passkey_make(origin, rp, user_id, user_name, exclude, out)
				 : br_cred_here_passkey_make(origin, rp, user_id, user_name, exclude, out);
}

int br_cred_passkey_sign( const std::string &origin, const std::string &rp, const std::string &id,
			  const std::string &client_data, BrPasskeySigned *out )
{
	return br_store_remote() ? br_store_remote_passkey_sign(origin, rp, id, client_data, out)
				 : br_cred_here_passkey_sign(origin, rp, id, client_data, out);
}

/* ---------------------------------------------------------------- the settings window (br_data.h) */

/*
 * Called in the process that has the window: the browser of one process
 * or the browser's own. A renderer has nothing of them to say.
 */

std::vector<BrPasswordRow> br_passwords_list()
{
	std::lock_guard<std::mutex> hold(g_lock);
	std::vector<BrPasswordRow> out;
	Creds c;
	if ( br_store_remote() || !read_creds(&c) ) return out;
	for ( const Password &p : c.pws ) out.push_back({ p.origin, p.id, p.password });
	return out;
}

bool br_password_change( const std::string &origin, const std::string &id, const std::string &password )
{
	std::lock_guard<std::mutex> hold(g_lock);
	Creds c;
	if ( br_store_remote() || !read_creds(&c) ) return false;
	bool found = false;
	for ( Password &p : c.pws ) {
		if ( p.origin == origin && p.id == id ) {
			wipe(&p.password);
			p.password = password;
			found = true;
		}
	}
	return found && write_creds(c);
}

bool br_password_remove( const std::string &origin, const std::string &id )
{
	std::lock_guard<std::mutex> hold(g_lock);
	Creds c;
	if ( br_store_remote() || !read_creds(&c) ) return false;
	auto gone = std::remove_if(c.pws.begin(), c.pws.end(),
				   [&]( const Password &p ) { return p.origin == origin && p.id == id; });
	if ( gone == c.pws.end() ) return false;
	for ( auto it = gone; it != c.pws.end(); ++it ) wipe(&it->password);
	c.pws.erase(gone, c.pws.end());
	return write_creds(c);
}

std::vector<BrPasskeyRow> br_passkeys_list()
{
	std::lock_guard<std::mutex> hold(g_lock);
	std::vector<BrPasskeyRow> out;
	Creds c;
	if ( br_store_remote() || !read_creds(&c) ) return out;
	for ( const Passkey &k : c.keys ) out.push_back({ k.rp, k.user_name, b64(k.id) });
	return out;
}

bool br_passkey_remove( const std::string &id )
{
	std::lock_guard<std::mutex> hold(g_lock);
	Creds c;
	if ( br_store_remote() || !read_creds(&c) ) return false;
	auto gone = std::remove_if(c.keys.begin(), c.keys.end(), [&]( const Passkey &k ) { return b64(k.id) == id; });
	if ( gone == c.keys.end() ) return false;
	for ( auto it = gone; it != c.keys.end(); ++it ) wipe(&it->pkcs8);
	c.keys.erase(gone, c.keys.end());
	return write_creds(c);
}
