/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_widevine.cc
 *	ブラウザ: installing the Widevine CDM for the user (design 14.6.3).
 *
 *	The CDM is not part of TessronOS or of the browser. It is fetched
 *	only when the user asks for it on the menu and agrees on the panel
 *	that says what it is, as it is distributed for the processor.
 *
 *	On aarch64, from the Raspberry Pi archive, the package libwidevinecdm0
 *	of Raspberry Pi OS:
 *
 *	  - the archive's index of arm64 packages
 *	    (<repo>/dists/<dist>/main/binary-arm64/Packages, over HTTPS) is
 *	    read for the package libwidevinecdm0 (its latest version, its
 *	    file, its size and its SHA-256)
 *	  - the package (a Debian archive) is fetched from <repo>/<file>,
 *	    and its size and SHA-256 checked against the index
 *	  - its data member (data.tar.xz) is unpacked (the XZ decoder of the
 *	    LZMA SDK), and the library (opt/WidevineCdm/.../
 *	    libwidevinecdm.so) and its manifest (opt/WidevineCdm/
 *	    manifest.json) taken out of the tar archive
 *
 *	On x86-64, from Google's component update service, the Widevine CDM
 *	component for Linux x64:
 *
 *	  - the service (<service>/service/update2/json, over HTTPS) is
 *	    asked, in its protocol 3.1, for the component of the Widevine
 *	    CDM (the application id that the component's key names) for
 *	    Linux x64 and this engine's version; it answers the version, the
 *	    addresses of the CRX3 file, its size and its SHA-256
 *	  - the file is fetched from an address over HTTPS, and its size and
 *	    SHA-256 checked against the answer
 *	  - the CRX3 file's signatures are checked (the CRX3 format of
 *	    components/crx_file/crx3.proto): every proof's signature over the
 *	    signed header and the archive must hold, one of its keys must be
 *	    the component's key (kWidevineKeyHash below) and one the key of
 *	    the id the signed header declares
 *	  - the library (_platform_specific/linux_x64/libwidevinecdm.so) and
 *	    manifest.json are taken out of the ZIP archive inside
 *
 *	Then on both:
 *
 *	  - the manifest must name versions of the CDM interfaces this
 *	    browser speaks
 *	  - both are kept in a new object of the user's, "Widevine CDM":
 *	    record 1 the library, record 2 the manifest, record 3 where they
 *	    came from, record 0 a text saying what the object is. It is
 *	    placed beside the browser's storage object, owned by the user
 *	    with the mode rw-------, and held by a fixed count, as no virtual
 *	    object points to it
 *	  - the user's entry in record 10 of the browser's storage object
 *	    ("widevine", br_store.cc), one line "<user> <object>" a user,
 *	    is made to point to the new object, and the object it pointed
 *	    to before is deleted
 *
 *	The fetching and unpacking run on a thread of their own; the object
 *	is made the user's on the engine's thread (br_wv_finish), where the
 *	other stores are written. Taking the CDM away deletes the object and
 *	the user's entry.
 *
 *	Nothing in the library is looked at or changed here: it is kept as
 *	it came, to be loaded by the browser's EME client.
 */

#include TS_CONFIG_H
#include <stdint.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/uapp.h>
#include <ts/sysdef.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "third_party/boringssl/src/include/openssl/sha.h"
#if defined(__x86_64__)
#include "base/json/json_reader.h"
#include "base/values.h"
#include "third_party/boringssl/src/include/openssl/bytestring.h"
#include "third_party/boringssl/src/include/openssl/digest.h"
#include "third_party/boringssl/src/include/openssl/evp.h"
#include "third_party/zlib/zlib.h"
#else
#include "third_party/lzma_sdk/src/C/7zCrc.h"
#include "third_party/lzma_sdk/src/C/Xz.h"
#include "third_party/lzma_sdk/src/C/XzCrc64.h"
#endif

#include "../net/br_store.h"
#include "br_widevine.h"

/* The network stack's fetch, waited for (net_fetch_tessronos.cc) */
extern "C" int br_net_fetch_sync( const char *url, const char *method, const void *body,
				  unsigned body_len, const char *req_headers, char **buf,
				  unsigned *len, char *ctype, unsigned ctype_cap, int *status,
				  char **headers, char *final_url, unsigned url_cap,
				  int timeout_ms ) __attribute__((weak));

namespace {

#if defined(__x86_64__)
/*
 * Google's component update service, and the Widevine CDM component:
 * the SHA-256 of its signing key, whose first 16 bytes written in the
 * letters a to p are its application id
 */
const char	kRepo[] = "https://update.googleapis.com";
const char	kUpdatePath[] = "/service/update2/json";
const uint8_t	kWidevineKeyHash[32] = {
	0xe8, 0xce, 0xcf, 0x42, 0x06, 0xd0, 0x93, 0x49, 0x6d, 0xd9, 0x89,
	0xe1, 0x41, 0x04, 0x86, 0x4a, 0x8f, 0xbd, 0x86, 0x12, 0xb9, 0x58,
	0x9b, 0xfb, 0x4f, 0xbb, 0x1b, 0xa9, 0xd3, 0x85, 0x37, 0xef };
/* the engine's version (the one br_blink.cc gives the pages): the service answers the CDM for it */
const char	kEngineVersion[] = "153.0.8010.36";
const char	kZipLibrary[] = "_platform_specific/linux_x64/libwidevinecdm.so";
const char	kZipManifest[] = "manifest.json";
#else
/* The archive, and its distributions looked in, the newest first */
const char	kRepo[] = "https://archive.raspberrypi.com/debian";
const char *const kDists[] = { "trixie", "bookworm" };
const char	kPackage[] = "libwidevinecdm0";
#endif

constexpr int	kFetchMs = 15 * 60 * 1000;
constexpr size_t kMaxPackage = 64u << 20;	/* the .deb, and what its data unpacks to */
constexpr size_t kMaxIndex = 32u << 20;
constexpr SZ	kChunk = 256 * 1024;

constexpr int	kRecLib = 1, kRecManifest = 2, kRecInfo = 3;

std::mutex		g_lock;
std::string		g_repo = kRepo;
std::atomic<int>	g_installed{-1};	/* -1 not yet known */
std::atomic<bool>	g_busy{false};

/* What the installing thread leaves for br_wv_finish */
struct Job {
	bool		ok = false;
	std::string	msg;
	TS_UUID		obj;
	ID		key = 0;	/* the new object, kept open until it is the user's */
	std::string	version;
};
Job	g_job;

/* A UUID as text (8-4-4-4-12 hexadecimal digits), its 16 bytes in the order written */
std::string uuid_text( const TS_UUID &u )
{
	static_assert(sizeof(TS_UUID) == 16, "a UUID is 16 bytes");
	const unsigned char	*b = (const unsigned char *)&u;
	std::string		t;

	for ( int i = 0; i < 16; i++ ) {
		static const char hex[] = "0123456789abcdef";
		if ( i == 4 || i == 6 || i == 8 || i == 10 ) t += '-';
		t += hex[b[i] >> 4];
		t += hex[b[i] & 15];
	}
	return t;
}

bool uuid_parse( const std::string &s, TS_UUID *u )
{
	unsigned char	b[16];
	int		n = 0;

	for ( char c : s ) {
		if ( c == '-' ) continue;
		int v = ( c >= '0' && c <= '9' ) ? c - '0' : ( c >= 'a' && c <= 'f' ) ? c - 'a' + 10
			: ( c >= 'A' && c <= 'F' ) ? c - 'A' + 10 : -1;
		if ( v < 0 || n >= 32 ) return false;
		if ( n % 2 == 0 ) b[n / 2] = (unsigned char)( v << 4 );
		else b[n / 2] |= (unsigned char)v;
		n++;
	}
	if ( n != 32 ) return false;
	memcpy(u, b, sizeof(b));
	return true;
}

/* The user the browser acts as, and that user's first group */
bool me( TS_UUID *user, TS_UUID *group )
{
	T_OBCRD *c = (T_OBCRD *)calloc(1, sizeof(T_OBCRD));
	if ( c == nullptr ) return false;
	bool ok = ob_get_crd(c) >= E_OK;
	if ( ok ) {
		*user = c->user;
		*group = ( c->ngrp > 0 ) ? c->grp[0] : c->user;
	}
	free(c);
	return ok;
}

/* ---------------------------------------------------------------- the users' entries (record 10) */

typedef std::vector<std::pair<std::string, std::string>> Entries;

Entries entries_load()
{
	Entries		e;
	unsigned char	*data = nullptr;
	int		len = 0;

	if ( br_store_load("widevine", &data, &len) < 0 ) return e;
	std::string t((const char *)data, (size_t)len);
	free(data);
	size_t p = 0;
	while ( p < t.size() ) {
		size_t nl = t.find('\n', p);
		std::string line = t.substr(p, nl == std::string::npos ? std::string::npos : nl - p);
		size_t sp = line.find(' ');
		if ( sp != std::string::npos ) e.emplace_back(line.substr(0, sp), line.substr(sp + 1));
		if ( nl == std::string::npos ) break;
		p = nl + 1;
	}
	return e;
}

bool entries_save( const Entries &e )
{
	std::string t;
	for ( const auto &x : e ) t += x.first + " " + x.second + "\n";
	return br_store_save("widevine", t.data(), (int)t.size()) == 0;
}

/* The user's object, when the user has an entry */
bool user_object( TS_UUID *obj )
{
	std::string user = br_wv_user();
	if ( user.empty() ) return false;
	for ( const auto &x : entries_load() ) {
		if ( x.first == user ) return uuid_parse(x.second, obj);
	}
	return false;
}

/* ---------------------------------------------------------------- records */

long rec_size( ID key, int rec )
{
	T_OBREC	r[16];
	INT	cnt = 0;

	if ( ob_lst_rec(key, r, 16, &cnt) < E_OK ) return -1;
	for ( INT i = 0; i < cnt && i < 16; i++ ) {
		if ( r[i].recno == rec ) return (long)r[i].size;
	}
	return -1;
}

bool rec_read( ID key, int rec, std::string *out )
{
	long size = rec_size(key, rec);
	if ( size < 0 ) return false;
	out->assign((size_t)size, '\0');
	long off = 0;
	while ( off < size ) {
		SZ want = ( size - off > kChunk ) ? kChunk : (SZ)( size - off ), got = 0;
		if ( ob_rea_rec(key, rec, (D)off, &( *out )[(size_t)off], want, &got) < E_OK || got <= 0 ) return false;
		off += got;
	}
	return true;
}

bool rec_add( ID key, UINT type, int want, const std::string &bytes )
{
	INT recno = -1;
	if ( ob_apd_rec(key, type, 0, &recno) < E_OK || recno != want ) return false;
	for ( size_t off = 0; off < bytes.size(); ) {
		SZ part = ( bytes.size() - off > (size_t)kChunk ) ? kChunk : (SZ)( bytes.size() - off ), asz = 0;
		if ( ob_wri_rec(key, recno, (D)off, bytes.data() + off, part, &asz) < E_OK || asz != part ) return false;
		off += (size_t)part;
	}
	return true;
}

std::string xml_text( const std::string &s )
{
	std::string o;
	for ( char c : s ) {
		if ( c == '&' ) o += "&amp;";
		else if ( c == '<' ) o += "&lt;";
		else if ( c == '>' ) o += "&gt;";
		else o += c;
	}
	return o;
}

/* ---------------------------------------------------------------- fetching */

bool fetch( const std::string &url, size_t max, std::string *body, std::string *err,
	    const std::string &post = std::string(), const char *headers = "" )
{
	if ( br_net_fetch_sync == nullptr ) {
		*err = "ネットワークを使えません";
		return false;
	}
	char		*buf = nullptr, *hdrs = nullptr;
	unsigned	len = 0;
	char		type[128] = "", final_url[2048] = "";
	int		status = 0;

	printf("browser: widevine: fetching %s\n", url.c_str());
	int ok = br_net_fetch_sync(url.c_str(), post.empty() ? "GET" : "POST", post.empty() ? nullptr : post.data(),
				   (unsigned)post.size(), headers, &buf, &len, type, sizeof(type), &status,
				   &hdrs, final_url, sizeof(final_url), kFetchMs);
	free(hdrs);
	if ( ok <= 0 ) {
		free(buf);
		*err = "取ってこられません: " + url;
		return false;
	}
	std::string base;
	{
		std::lock_guard<std::mutex> hold(g_lock);
		base = g_repo;
	}
	/* a redirect may not leave HTTPS (or the tests' own server) */
	if ( final_url[0] != 0 && strncmp(final_url, "https://", 8) != 0
	     && strncmp(final_url, base.c_str(), base.size()) != 0 ) {
		free(buf);
		*err = "HTTPSでない所へ転送されました: " + std::string(final_url);
		return false;
	}
	if ( status != 200 || len > max ) {
		free(buf);
		*err = ( status != 200 ) ? "取ってこられません(HTTP " + std::to_string(status) + "): " + url
					 : "大きすぎます: " + url;
		return false;
	}
	body->assign(buf != nullptr ? buf : "", len);
	free(buf);
	return true;
}

std::string sha256_hex( const std::string &data )
{
	unsigned char	d[SHA256_DIGEST_LENGTH];
	char		h[SHA256_DIGEST_LENGTH * 2 + 1];

	SHA256((const uint8_t *)data.data(), data.size(), d);
	for ( int i = 0; i < SHA256_DIGEST_LENGTH; i++ ) snprintf(h + i * 2, 3, "%02x", d[i]);
	return std::string(h, SHA256_DIGEST_LENGTH * 2);
}

#if !defined(__x86_64__)
/* ---------------------------------------------------------------- the archive's index */

/* Debian's ordering of versions (epoch:upstream-revision), as dpkg compares them */
int ver_order( char c )
{
	if ( isdigit((unsigned char)c) ) return 0;
	if ( isalpha((unsigned char)c) ) return c;
	if ( c == '~' ) return -1;
	if ( c != 0 ) return c + 256;
	return 0;
}

int ver_part( const char *a, const char *b )
{
	while ( *a != 0 || *b != 0 ) {
		int first = 0;
		while ( ( *a != 0 && !isdigit((unsigned char)*a) ) || ( *b != 0 && !isdigit((unsigned char)*b) ) ) {
			int ac = ver_order(*a), bc = ver_order(*b);
			if ( ac != bc ) return ac - bc;
			a++;
			b++;
		}
		while ( *a == '0' ) a++;
		while ( *b == '0' ) b++;
		while ( isdigit((unsigned char)*a) && isdigit((unsigned char)*b) ) {
			if ( first == 0 ) first = *a - *b;
			a++;
			b++;
		}
		if ( isdigit((unsigned char)*a) ) return 1;
		if ( isdigit((unsigned char)*b) ) return -1;
		if ( first != 0 ) return first;
	}
	return 0;
}

int ver_cmp( const std::string &x, const std::string &y )
{
	auto split = []( const std::string &v, long *epoch, std::string *up, std::string *rev ) {
		size_t c = v.find(':');
		*epoch = ( c == std::string::npos ) ? 0 : atol(v.substr(0, c).c_str());
		std::string rest = ( c == std::string::npos ) ? v : v.substr(c + 1);
		size_t d = rest.rfind('-');
		*up = ( d == std::string::npos ) ? rest : rest.substr(0, d);
		*rev = ( d == std::string::npos ) ? std::string() : rest.substr(d + 1);
	};
	long		ex, ey;
	std::string	ux, uy, rx, ry;

	split(x, &ex, &ux, &rx);
	split(y, &ey, &uy, &ry);
	if ( ex != ey ) return ex < ey ? -1 : 1;
	int r = ver_part(ux.c_str(), uy.c_str());
	return ( r != 0 ) ? r : ver_part(rx.c_str(), ry.c_str());
}

struct Pkg {
	std::string	version, file, sha256;
	size_t		size = 0;
};

/* The latest arm64 libwidevinecdm0 the index lists: false when it lists none */
bool find_package( const std::string &index, Pkg *best )
{
	bool	found = false;
	size_t	p = 0;

	while ( p < index.size() ) {
		size_t end = index.find("\n\n", p);
		std::string st = index.substr(p, end == std::string::npos ? std::string::npos : end - p);
		p = ( end == std::string::npos ) ? index.size() : end + 2;
		std::string name, arch;
		Pkg k;
		size_t q = 0;
		while ( q < st.size() ) {
			size_t nl = st.find('\n', q);
			std::string line = st.substr(q, nl == std::string::npos ? std::string::npos : nl - q);
			q = ( nl == std::string::npos ) ? st.size() : nl + 1;
			size_t colon = line.find(": ");
			if ( line.empty() || line[0] == ' ' || colon == std::string::npos ) continue;
			std::string f = line.substr(0, colon), v = line.substr(colon + 2);
			while ( !v.empty() && ( v.back() == '\r' || v.back() == ' ' ) ) v.pop_back();
			if ( f == "Package" ) name = v;
			else if ( f == "Architecture" ) arch = v;
			else if ( f == "Version" ) k.version = v;
			else if ( f == "Filename" ) k.file = v;
			else if ( f == "SHA256" ) k.sha256 = v;
			else if ( f == "Size" ) k.size = (size_t)strtoull(v.c_str(), nullptr, 10);
		}
		if ( name != kPackage || arch != "arm64" || k.file.empty() || k.sha256.size() != 64 || k.size == 0 ) continue;
		/* a file of the pool, named plainly */
		bool plain = k.file.compare(0, 5, "pool/") == 0 && k.file.find("..") == std::string::npos;
		for ( char c : k.file ) {
			if ( !isalnum((unsigned char)c) && strchr("/._+~-", c) == nullptr ) plain = false;
		}
		if ( !plain ) continue;
		if ( !found || ver_cmp(k.version, best->version) > 0 ) *best = k;
		found = true;
	}
	return found;
}

/* ---------------------------------------------------------------- the package */

/* A member of an ar archive (the .deb): false when it has none of that name */
bool ar_member( const std::string &ar, const std::string &want, std::string *out )
{
	if ( ar.size() < 8 || ar.compare(0, 8, "!<arch>\n") != 0 ) return false;
	size_t off = 8;
	while ( off + 60 <= ar.size() ) {
		const char *h = ar.data() + off;
		std::string name(h, 16);
		while ( !name.empty() && ( name.back() == ' ' || name.back() == '/' ) ) name.pop_back();
		size_t size = (size_t)strtoull(std::string(h + 48, 10).c_str(), nullptr, 10);
		if ( h[58] != '`' || h[59] != '\n' || size > ar.size() - off - 60 ) return false;
		if ( name == want ) {
			out->assign(ar, off + 60, size);
			return true;
		}
		off += 60 + size + ( size & 1 );
	}
	return false;
}

void *xz_alloc( ISzAllocPtr, size_t n ) { return malloc(n); }
void xz_free( ISzAllocPtr, void *p ) { free(p); }
const ISzAlloc kXzAlloc = { xz_alloc, xz_free };

/* An XZ stream unpacked whole */
bool unxz( const std::string &in, std::string *out, std::string *err )
{
	static std::once_flag	tables;
	CXzUnpacker		x;
	size_t			inpos = 0, outpos = 0;
	bool			ok = false;

	std::call_once(tables, [] {
		CrcGenerateTable();
		Crc64GenerateTable();
	});
	XzUnpacker_Construct(&x, &kXzAlloc);
	XzUnpacker_Init(&x);
	out->clear();
	for ( ;; ) {
		if ( out->size() - outpos < ( 1u << 20 ) ) out->resize(outpos + ( 4u << 20 ));
		SizeT dl = out->size() - outpos, sl = in.size() - inpos;
		ECoderStatus st;
		SRes r = XzUnpacker_Code(&x, (Byte *)&( *out )[outpos], &dl, (const Byte *)in.data() + inpos, &sl, 1,
					 CODER_FINISH_ANY, &st);
		inpos += sl;
		outpos += dl;
		if ( r != SZ_OK ) {
			*err = "パッケージのデータが壊れています(XZ " + std::to_string(r) + ")";
			break;
		}
		if ( outpos > kMaxPackage ) {
			*err = "パッケージのデータが大きすぎます";
			break;
		}
		if ( dl == 0 && sl == 0 ) {
			ok = XzUnpacker_IsStreamWasFinished(&x);
			if ( !ok ) *err = "パッケージのデータが途中で切れています";
			break;
		}
	}
	XzUnpacker_Free(&x);
	out->resize(ok ? outpos : 0);
	return ok;
}

struct TarFile {
	std::string	path;
	size_t		off, size;
};

size_t tar_num( const char *p, size_t n )
{
	size_t v = 0;
	if ( (unsigned char)p[0] & 0x80 ) {
		/* base-256, for the large */
		for ( size_t i = 1; i < n; i++ ) v = ( v << 8 ) | (unsigned char)p[i];
		return v;
	}
	for ( size_t i = 0; i < n && p[i] != 0; i++ ) {
		if ( p[i] >= '0' && p[i] <= '7' ) v = v * 8 + (size_t)( p[i] - '0' );
	}
	return v;
}

/* The regular files of a tar archive (ustar, with GNU's long names and pax's paths) */
std::vector<TarFile> tar_files( const std::string &t )
{
	std::vector<TarFile>	files;
	std::string		longname;
	size_t			off = 0;

	while ( off + 512 <= t.size() ) {
		const char *h = t.data() + off;
		bool zero = true;
		for ( int i = 0; i < 512 && zero; i++ ) zero = h[i] == 0;
		if ( zero ) break;
		size_t size = tar_num(h + 124, 12), data = off + 512;
		if ( size > t.size() - data ) break;
		char type = h[156];
		std::string name(h, strnlen(h, 100));
		if ( memcmp(h + 257, "ustar", 5) == 0 && h[345] != 0 ) {
			name = std::string(h + 345, strnlen(h + 345, 155)) + "/" + name;
		}
		if ( type == 'L' ) {
			longname.assign(t, data, size);
			longname.resize(strnlen(longname.c_str(), longname.size()));
		} else if ( type == 'x' ) {
			/* pax: "<len> <key>=<value>\n" records; the path is what is kept */
			size_t p = data;
			while ( p < data + size ) {
				size_t len = (size_t)strtoull(t.c_str() + p, nullptr, 10);
				if ( len == 0 || p + len > data + size ) break;
				std::string rec = t.substr(p, len);
				size_t sp = rec.find(' '), eq = rec.find('=');
				if ( sp != std::string::npos && eq != std::string::npos && rec.compare(sp + 1, eq - sp - 1, "path") == 0 ) {
					longname = rec.substr(eq + 1, rec.size() - eq - 2);
				}
				p += len;
			}
		} else if ( type != 'g' ) {
			if ( !longname.empty() ) name = longname;
			longname.clear();
			while ( name.compare(0, 2, "./") == 0 ) name.erase(0, 2);
			while ( !name.empty() && name[0] == '/' ) name.erase(0, 1);
			if ( type == '0' || type == 0 ) files.push_back({name, data, size});
		}
		off = data + ( ( size + 511 ) & ~(size_t)511 );
	}
	return files;
}

/*
 * The CDM out of the package: the library and the manifest, the
 * manifest checked by the EME client; 'version' the manifest's
 */
bool unpack( const std::string &deb, std::string *lib, std::string *manifest, std::string *version, std::string *err )
{
	std::string data, tar;

	if ( !ar_member(deb, "debian-binary", &data) || data.compare(0, 2, "2.") != 0 ) {
		*err = "Debianのパッケージではありません";
		return false;
	}
	if ( !ar_member(deb, "data.tar.xz", &data) ) {
		*err = "パッケージのデータが対応していない形です(data.tar.xzがありません)";
		return false;
	}
	if ( !unxz(data, &tar, err) ) return false;
	data.clear();
	const TarFile *l = nullptr, *m = nullptr;
	std::vector<TarFile> files = tar_files(tar);
	for ( const TarFile &f : files ) {
		if ( f.path.compare(0, 16, "opt/WidevineCdm/") != 0 ) continue;
		const std::string tail = "/libwidevinecdm.so";
		if ( f.path.size() > tail.size() && f.path.compare(f.path.size() - tail.size(), tail.size(), tail) == 0 ) {
			if ( l == nullptr ) l = &f;
		}
		if ( f.path == "opt/WidevineCdm/manifest.json" ) m = &f;
	}
	if ( l == nullptr || m == nullptr ) {
		*err = "パッケージにCDM(libwidevinecdm.soとmanifest.json)がありません";
		return false;
	}
	lib->assign(tar, l->off, l->size);
	manifest->assign(tar, m->off, m->size);
	printf("browser: widevine: %s (%lu bytes), %s\n", l->path.c_str(), (unsigned long)l->size, m->path.c_str());
	return br_eme_manifest_ok(*manifest, version, err);
}
#endif /* !__x86_64__ */

/* ---------------------------------------------------------------- the user's object */

/* A new object of the user's holding the CDM, open: false with 'err' */
bool keep( const std::string &lib, const std::string &manifest, const std::string &version,
	   const std::string &source, const std::string &sha, Job *job, std::string *err )
{
	static const char kJson[] = "{\"name\":\"Widevine CDM\"}";
	TS_UUID	user, group, store;
	T_OBPRT	prt;
	T_OBCRE	c;

	if ( !me(&user, &group) || !uuid_parse(SYSDEF_BROWSER_STORAGE, &store) ) {
		*err = "利用者が分かりません";
		return false;
	}
	memset(&prt, 0, sizeof(prt));
	prt.owner = user;
	prt.group = group;
	prt.mode = 0600;
	memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = (CONST UB *)kJson;
	c.jsonsz = (SZ)strlen(kJson);
	c.near = store;
	c.prt = &prt;
	if ( ob_cre_obj(&c, &job->obj) < E_OK ) {
		*err = "実身を作れません";
		return false;
	}
	ID k = ob_opn_obj(&job->obj, OB_OP_R | OB_OP_W | OB_O_EXCL);	/* filled record by record: not read half made */
	std::string tad =
		"<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"Widevine CDM\">\n<document>\n"
		"<p>Widevine CDM</p>\n"
		"<p>利用者がブラウザのメニューの〈Widevineを導入〉で入れた、GoogleのWidevine CDM(Googleの独自のソフトウェア)。"
		"TessronOSとブラウザには含まれていない。〈Widevineを外す〉でこの実身は消える。</p>\n"
		"<p>バージョン " + xml_text(version) + "、入手元 " + xml_text(source) + "、SHA-256 " + sha + "</p>\n"
		"</document>\n</tad>\n";
	std::string info = "version=" + version + "\nsource=" + source + "\nsha256=" + sha + "\n";
	bool ok = k > 0 && rec_add(k, OB_RT_TAD, 0, tad) && rec_add(k, OB_RT_SYSDATA, kRecLib, lib)
		  && rec_add(k, OB_RT_SYSDATA, kRecManifest, manifest) && rec_add(k, OB_RT_SYSDATA, kRecInfo, info);
	if ( !ok ) {
		if ( k > 0 ) ob_cls_obj(k);
		(void)ob_del_obj(&job->obj);
		*err = "実身に書けません";
		return false;
	}
	job->key = k;
	return true;
}

/* What was fetched: the library, the manifest, its version, where it came from, the file's SHA-256 */
struct Got {
	std::string	lib, manifest, version, url, sha;
};

#if defined(__x86_64__)
/* ---------------------------------------------------------------- the component update service (x86-64) */

/* The request of the update check (protocol 3.1) for the Widevine CDM component on Linux x64 */
std::string update_request()
{
	std::string id;
	for ( int i = 0; i < 16; i++ ) {
		id += (char)( 'a' + ( kWidevineKeyHash[i] >> 4 ) );
		id += (char)( 'a' + ( kWidevineKeyHash[i] & 15 ) );
	}
	return std::string("{\"request\":{\"@os\":\"linux\",\"@updater\":\"chromium\",\"acceptformat\":\"crx3\","
			   "\"app\":[{\"appid\":\"") + id + "\",\"updatecheck\":{},\"version\":\"0.0.0.0\"}],"
	       "\"arch\":\"x64\",\"nacl_arch\":\"x86-64\",\"os\":{\"arch\":\"x86_64\",\"platform\":\"Linux\"},"
	       "\"prodversion\":\"" + kEngineVersion + "\",\"protocol\":\"3.1\",\"updaterversion\":\"" + kEngineVersion + "\"}}";
}

struct Crx {
	std::string	version, name, sha256;
	std::vector<std::string> codebases;
	size_t		size = 0;
};

/* The service's answer: the component's version, its file's name, size and SHA-256, and the addresses */
bool parse_update( const std::string &body, Crx *c, std::string *err )
{
	/* the answer starts with a line that keeps it from being run as a script */
	size_t at = body.find('{');
	std::optional<base::DictValue> d =
		at == std::string::npos ? std::nullopt : base::JSONReader::ReadDict(body.substr(at), base::JSON_PARSE_RFC);
	const base::DictValue *resp = d ? d->FindDict("response") : nullptr;
	const base::ListValue *apps = resp ? resp->FindList("app") : nullptr;
	const base::DictValue *app = ( apps && !apps->empty() ) ? ( *apps )[0].GetIfDict() : nullptr;
	if ( app == nullptr ) {
		*err = "コンポーネントの更新のサービスの答えを読めません";
		return false;
	}
	const std::string *st = app->FindStringByDottedPath("updatecheck.status");
	if ( st == nullptr || *st != "ok" ) {
		*err = "コンポーネントの更新のサービスがWidevine CDMを答えません(" + std::string(st ? *st : "?") + ")";
		return false;
	}
	const base::ListValue *urls = app->FindListByDottedPath("updatecheck.urls.url");
	const base::ListValue *pkgs = app->FindListByDottedPath("updatecheck.manifest.packages.package");
	const std::string *ver = app->FindStringByDottedPath("updatecheck.manifest.version");
	const base::DictValue *pkg = ( pkgs && !pkgs->empty() ) ? ( *pkgs )[0].GetIfDict() : nullptr;
	const std::string *name = pkg ? pkg->FindString("name") : nullptr;
	const std::string *sha = pkg ? pkg->FindString("hash_sha256") : nullptr;
	std::optional<double> size = pkg ? pkg->FindDouble("size") : std::nullopt;
	if ( urls == nullptr || ver == nullptr || name == nullptr || sha == nullptr || sha->size() != 64 || !size
	     || *size <= 0 || *size > (double)kMaxPackage ) {
		*err = "コンポーネントの更新のサービスの答えにファイルがありません";
		return false;
	}
	/* a plain file name of the CRX3 */
	bool plain = name->size() > 5 && name->compare(name->size() - 5, 5, ".crx3") == 0;
	for ( char ch : *name ) {
		if ( !isalnum((unsigned char)ch) && strchr("._-", ch) == nullptr ) plain = false;
	}
	if ( !plain ) {
		*err = "コンポーネントの更新のサービスが答えたファイルの名前が不正です";
		return false;
	}
	for ( const base::Value &u : *urls ) {
		const base::DictValue *ud = u.GetIfDict();
		const std::string *cb = ud ? ud->FindString("codebase") : nullptr;
		if ( cb != nullptr && !cb->empty() && cb->back() == '/' ) c->codebases.push_back(*cb);
	}
	c->version = *ver;
	c->name = *name;
	c->sha256 = *sha;
	for ( char &ch : c->sha256 ) ch = (char)tolower((unsigned char)ch);
	c->size = (size_t)*size;
	return true;
}

/* One field of a protocol buffer: its number, and its bytes when it is length-delimited */
bool pb_field( CBS *in, uint64_t *num, CBS *val )
{
	uint64_t key = 0, len = 0;
	int shift = 0;
	uint8_t b;
	do {
		if ( !CBS_get_u8(in, &b) || shift > 63 ) return false;
		key |= (uint64_t)( b & 0x7f ) << shift;
		shift += 7;
	} while ( b & 0x80 );
	*num = key >> 3;
	switch ( key & 7 ) {
	case 0:		/* a varint, skipped */
		do {
			if ( !CBS_get_u8(in, &b) ) return false;
		} while ( b & 0x80 );
		CBS_init(val, nullptr, 0);
		return true;
	case 1:
		return CBS_get_bytes(in, val, 8);
	case 5:
		return CBS_get_bytes(in, val, 4);
	case 2:
		shift = 0;
		do {
			if ( !CBS_get_u8(in, &b) || shift > 63 ) return false;
			len |= (uint64_t)( b & 0x7f ) << shift;
			shift += 7;
		} while ( b & 0x80 );
		return len <= CBS_len(in) && CBS_get_bytes(in, val, (size_t)len);
	default:
		return false;
	}
}

bool has_token( const std::string &h, const char *t )
{
	return h.find(std::string(t, 4)) != std::string::npos;
}

/*
 * A CRX3 file verified with a required key: the header (CrxFileHeader of
 * crx3.proto) parsed, the signature of every proof (sha256_with_rsa,
 * sha256_with_ecdsa) over "CRX3 SignedData\0", the signed header's size
 * (32 bits, little-endian), the signed header and the archive holding,
 * the component's key among the proofs' keys, and the key of the id the
 * signed header declares among them too. 'zip' is the archive.
 */
bool crx3_verify( const std::string &f, std::string *zip, std::string *err )
{
	static const char kContext[] = "CRX3 SignedData";	/* and its terminating zero */
	*err = "Widevine CDMのファイルの署名が正しくありません";
	if ( f.size() < 12 || f.compare(0, 4, "Cr24") != 0 ) return false;
	const uint8_t *u = (const uint8_t *)f.data();
	uint32_t ver = u[4] | u[5] << 8 | u[6] << 16 | (uint32_t)u[7] << 24;
	uint32_t hsz = u[8] | u[9] << 8 | u[10] << 16 | (uint32_t)u[11] << 24;
	if ( ver != 3 || hsz > f.size() - 12 ) return false;
	std::string header = f.substr(12, hsz);
	/* a header with a ZIP's end of central directory in it could be read as the archive */
	if ( has_token(header, "PK\x05\x06") || has_token(header, "PK\x06\x07") || has_token(header, "PK\x06\x06") ) {
		return false;
	}
	const uint8_t *arch = u + 12 + hsz;
	size_t archsz = f.size() - 12 - hsz;

	struct Proof { CBS key, sig; bool rsa; };
	std::vector<Proof> proofs;
	CBS in, val, signed_header;
	uint64_t num;
	bool have_signed = false;
	CBS_init(&in, (const uint8_t *)header.data(), header.size());
	while ( CBS_len(&in) > 0 ) {
		if ( !pb_field(&in, &num, &val) ) return false;
		if ( num == 2 || num == 3 ) {
			Proof pr;
			CBS_init(&pr.key, nullptr, 0);
			CBS_init(&pr.sig, nullptr, 0);
			pr.rsa = num == 2;
			CBS p = val, v;
			uint64_t n;
			while ( CBS_len(&p) > 0 ) {
				if ( !pb_field(&p, &n, &v) ) return false;
				if ( n == 1 ) pr.key = v;
				if ( n == 2 ) pr.sig = v;
			}
			proofs.push_back(pr);
		} else if ( num == 10000 ) {
			signed_header = val;
			have_signed = true;
		}
	}
	if ( !have_signed || proofs.empty() ) return false;
	/* the id the signed header declares (SignedData's crx_id) */
	CBS sh = signed_header, v, crx_id;
	CBS_init(&crx_id, nullptr, 0);
	while ( CBS_len(&sh) > 0 ) {
		if ( !pb_field(&sh, &num, &v) ) return false;
		if ( num == 1 ) crx_id = v;
	}
	uint8_t sz[4] = { (uint8_t)CBS_len(&signed_header), (uint8_t)( CBS_len(&signed_header) >> 8 ),
			  (uint8_t)( CBS_len(&signed_header) >> 16 ), (uint8_t)( CBS_len(&signed_header) >> 24 ) };
	bool required = false, declared = false;
	for ( const Proof &pr : proofs ) {
		uint8_t h[SHA256_DIGEST_LENGTH];
		SHA256(CBS_data(&pr.key), CBS_len(&pr.key), h);
		if ( memcmp(h, kWidevineKeyHash, sizeof(h)) == 0 ) required = true;
		if ( CBS_len(&crx_id) == 16 && memcmp(h, CBS_data(&crx_id), 16) == 0 ) declared = true;
		CBS k = pr.key;
		EVP_PKEY *pk = EVP_parse_public_key(&k);
		bool ok = pk != nullptr && CBS_len(&k) == 0
			  && EVP_PKEY_id(pk) == ( pr.rsa ? EVP_PKEY_RSA : EVP_PKEY_EC );
		EVP_MD_CTX *ctx = ok ? EVP_MD_CTX_new() : nullptr;
		ok = ok && ctx != nullptr && EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, pk) == 1
		     && EVP_DigestVerifyUpdate(ctx, kContext, sizeof(kContext)) == 1
		     && EVP_DigestVerifyUpdate(ctx, sz, sizeof(sz)) == 1
		     && EVP_DigestVerifyUpdate(ctx, CBS_data(&signed_header), CBS_len(&signed_header)) == 1
		     && EVP_DigestVerifyUpdate(ctx, arch, archsz) == 1
		     && EVP_DigestVerifyFinal(ctx, CBS_data(&pr.sig), CBS_len(&pr.sig)) == 1;
		EVP_MD_CTX_free(ctx);
		EVP_PKEY_free(pk);
		if ( !ok ) return false;
	}
	if ( !required || !declared ) {
		*err = "Widevine CDMのファイルがWidevineのコンポーネントの鍵で署名されていません";
		return false;
	}
	printf("browser: widevine: the CRX3 file's %lu signatures hold; it is signed with the component's key\n",
	       (unsigned long)proofs.size());
	zip->assign((const char *)arch, archsz);
	err->clear();
	return true;
}

uint32_t le32( const std::string &s, size_t at )
{
	const unsigned char *p = (const unsigned char *)s.data() + at;
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

uint32_t le16( const std::string &s, size_t at )
{
	const unsigned char *p = (const unsigned char *)s.data() + at;
	return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

/* A file of the ZIP archive by its name, from its central directory (stored or deflated) */
bool zip_file( const std::string &z, const std::string &want, std::string *out )
{
	size_t end = std::string::npos;
	if ( z.size() < 22 ) return false;
	for ( size_t i = z.size() - 22; ; i-- ) {
		if ( le32(z, i) == 0x06054b50 ) {
			end = i;
			break;
		}
		if ( i == 0 || z.size() - i > 65557 ) break;
	}
	if ( end == std::string::npos ) return false;
	size_t count = le16(z, end + 10), at = le32(z, end + 16);
	for ( size_t i = 0; i < count; i++ ) {
		if ( at + 46 > z.size() || le32(z, at) != 0x02014b50 ) return false;
		uint32_t method = le16(z, at + 10), csize = le32(z, at + 20), usize = le32(z, at + 24);
		size_t nlen = le16(z, at + 28), xlen = le16(z, at + 30), clen = le16(z, at + 32), local = le32(z, at + 42);
		if ( at + 46 + nlen > z.size() ) return false;
		std::string name = z.substr(at + 46, nlen);
		at += 46 + nlen + xlen + clen;
		if ( name != want ) continue;
		if ( local + 30 > z.size() || le32(z, local) != 0x04034b50 || usize > kMaxPackage ) return false;
		size_t data = local + 30 + le16(z, local + 26) + le16(z, local + 28);
		if ( data > z.size() || csize > z.size() - data ) return false;
		if ( method == 0 ) {
			if ( csize != usize ) return false;
			out->assign(z, data, csize);
			return true;
		}
		if ( method != 8 ) return false;
		z_stream s;
		memset(&s, 0, sizeof(s));
		if ( inflateInit2(&s, -15) != Z_OK ) return false;
		out->resize(usize);
		s.next_in = (Bytef *)z.data() + data;
		s.avail_in = (uInt)csize;
		s.next_out = (Bytef *)&( *out )[0];
		s.avail_out = (uInt)usize;
		int e = inflate(&s, Z_FINISH);
		inflateEnd(&s);
		return e == Z_STREAM_END && s.total_out == usize;
	}
	return false;
}

bool fetch_cdm( const std::string &base, Got *g, std::string *err )
{
	std::string body;
	Crx c;

	if ( !fetch(base + kUpdatePath, kMaxIndex, &body, err, update_request(),
		    "Content-Type: application/json\r\n") || !parse_update(body, &c, err) ) {
		return false;
	}
	printf("browser: widevine: the component update service answers Widevine CDM %s, %s, %lu bytes, SHA-256 %s\n",
	       c.version.c_str(), c.name.c_str(), (unsigned long)c.size, c.sha256.c_str());
	/* the addresses over HTTPS (or the tests' server's), in the order the service gives them */
	std::string crx;
	bool got = false;
	for ( const std::string &cb : c.codebases ) {
		if ( cb.compare(0, 8, "https://") != 0 && cb.compare(0, base.size(), base) != 0 ) continue;
		g->url = cb + c.name;
		std::string e;
		if ( fetch(g->url, kMaxPackage, &crx, &e) ) {
			got = true;
			break;
		}
		*err = e;
	}
	if ( !got ) {
		if ( err->empty() ) *err = "Widevine CDMのファイルのHTTPSのアドレスがありません";
		return false;
	}
	g->sha = sha256_hex(crx);
	if ( crx.size() != c.size || g->sha != c.sha256 ) {
		printf("browser: widevine: %s: %lu bytes, SHA-256 %s; the service says %lu, %s\n", g->url.c_str(),
		       (unsigned long)crx.size(), g->sha.c_str(), (unsigned long)c.size, c.sha256.c_str());
		*err = "取ってきたファイルがコンポーネントの更新のサービスのSHA-256と合いません";
		return false;
	}
	std::string zip;
	if ( !crx3_verify(crx, &zip, err) ) return false;
	crx.clear();
	if ( !zip_file(zip, kZipLibrary, &g->lib) || !zip_file(zip, kZipManifest, &g->manifest) ) {
		*err = std::string("Widevine CDMのファイルにCDM(") + kZipLibrary + "と" + kZipManifest + ")がありません";
		return false;
	}
	printf("browser: widevine: %s (%lu bytes), %s\n", kZipLibrary, (unsigned long)g->lib.size(), kZipManifest);
	if ( !br_eme_manifest_ok(g->manifest, &g->version, err) ) return false;
	if ( g->version != c.version ) {
		*err = "マニフェストのバージョン(" + g->version + ")がサービスの答え(" + c.version + ")と違います";
		return false;
	}
	return true;
}
#else
/* ---------------------------------------------------------------- the Raspberry Pi archive (aarch64) */

bool fetch_cdm( const std::string &base, Got *g, std::string *err )
{
	std::string	index, deb;
	Pkg		pkg;
	bool		found = false;

	for ( const char *d : kDists ) {
		std::string e;
		if ( !fetch(base + "/dists/" + d + "/main/binary-arm64/Packages", kMaxIndex, &index, &e) ) {
			*err = e;
			continue;
		}
		if ( find_package(index, &pkg) ) {
			found = true;
			printf("browser: widevine: %s %s in %s, %lu bytes\n", kPackage, pkg.version.c_str(), d,
			       (unsigned long)pkg.size);
			break;
		}
		*err = std::string("リポジトリに") + kPackage + "(arm64)がありません";
	}
	index.clear();
	g->url = base + "/" + pkg.file;
	if ( !found || !fetch(g->url, kMaxPackage, &deb, err) ) return false;
	g->sha = sha256_hex(deb);
	if ( deb.size() != pkg.size || g->sha != pkg.sha256 ) {
		printf("browser: widevine: %s: %lu bytes, SHA-256 %s; the index says %lu, %s\n", g->url.c_str(),
		       (unsigned long)deb.size(), g->sha.c_str(), (unsigned long)pkg.size, pkg.sha256.c_str());
		*err = "取ってきたパッケージがリポジトリの索引のSHA-256と合いません";
		return false;
	}
	return unpack(deb, &g->lib, &g->manifest, &g->version, err);
}
#endif

void install( void (*done)( void ) )
{
	Job		job;
	std::string	base, err;
	Got		g;

	{
		std::lock_guard<std::mutex> hold(g_lock);
		base = g_repo;
	}
	if ( fetch_cdm(base, &g, &err) && keep(g.lib, g.manifest, g.version, g.url, g.sha, &job, &err) ) {
		job.version = g.version;
		job.ok = true;
	}
	job.msg = job.ok ? "Widevine " + job.version + "を取ってきました" : err;
	printf("browser: widevine: %s\n", job.msg.c_str());
	{
		std::lock_guard<std::mutex> hold(g_lock);
		g_job = job;
	}
	done();
}

}  // namespace

std::string br_wv_user()
{
	TS_UUID user, group;
	return me(&user, &group) ? uuid_text(user) : std::string();
}

bool br_wv_installed()
{
	int v = g_installed.load();
	if ( v < 0 ) {
		TS_UUID obj;
		v = user_object(&obj) ? 1 : 0;
		g_installed = v;
	}
	return v > 0;
}

bool br_wv_busy()
{
	return g_busy.load();
}

void br_wv_set_repo( const std::string &url )
{
	/* the archive over HTTPS, or a server of the tests on this machine */
	if ( url.compare(0, 8, "https://") != 0 && url.compare(0, 17, "http://127.0.0.1:") != 0 ) return;
	std::lock_guard<std::mutex> hold(g_lock);
	g_repo = url;
	while ( !g_repo.empty() && g_repo.back() == '/' ) g_repo.pop_back();
}

void br_wv_source( std::string *line1, std::string *line2 )
{
#if defined(__x86_64__)
	*line1 = "〈導入〉を選ぶと、Googleのコンポーネントの更新のサービス";
	*line2 = "(update.googleapis.com)からWidevine CDMを取ってきて、";
#else
	*line1 = "〈導入〉を選ぶと、Raspberry Piの公式のリポジトリ";
	*line2 = "(archive.raspberrypi.com)からlibwidevinecdm0を取ってきて、";
#endif
}

bool br_wv_install_start( void (*done)( void ) )
{
	bool was = false;
	if ( !g_busy.compare_exchange_strong(was, true) ) return false;
	std::thread(install, done).detach();
	return true;
}

bool br_wv_finish( std::string *msg )
{
	Job job;
	{
		std::lock_guard<std::mutex> hold(g_lock);
		job = g_job;
		g_job = Job();
	}
	g_busy = false;
	*msg = job.msg;
	if ( !job.ok ) return false;

	std::string user = br_wv_user();
	Entries e = entries_load();
	std::string old;
	bool ok = !user.empty() && ob_lnk_obj(&job.obj) >= E_OK;
	if ( ok ) {
		bool had = false;
		for ( auto &x : e ) {
			if ( x.first == user ) {
				old = x.second;
				x.second = uuid_text(job.obj);
				had = true;
			}
		}
		if ( !had ) e.emplace_back(user, uuid_text(job.obj));
		ok = entries_save(e);
		if ( !ok ) (void)ob_unl_obj(&job.obj);
	}
	ob_cls_obj(job.key);
	if ( !ok ) {
		(void)ob_del_obj(&job.obj);
		*msg = "導入したCDMを保存できません";
		return false;
	}
	TS_UUID o;
	if ( uuid_parse(old, &o) ) {
		(void)ob_unl_obj(&o);
		(void)ob_del_obj(&o);
	}
	g_installed = 1;
	printf("browser: widevine: %s installed for %s in %s\n", job.version.c_str(), user.c_str(),
	       uuid_text(job.obj).c_str());
	*msg = "Widevine " + job.version + "を導入しました";
	return true;
}

bool br_wv_remove( std::string *msg )
{
	std::string user = br_wv_user();
	Entries e = entries_load(), kept;
	std::string obj;

	for ( const auto &x : e ) {
		if ( x.first == user ) obj = x.second;
		else kept.push_back(x);
	}
	if ( user.empty() || obj.empty() ) {
		g_installed = 0;
		*msg = "Widevineは導入されていません";
		return false;
	}
	if ( !entries_save(kept) ) {
		*msg = "Widevineを外せません";
		return false;
	}
	TS_UUID o;
	if ( uuid_parse(obj, &o) ) {
		(void)ob_unl_obj(&o);
		(void)ob_del_obj(&o);
	}
	g_installed = 0;
	printf("browser: widevine: removed for %s (%s)\n", user.c_str(), obj.c_str());
	*msg = "Widevineを外しました";
	return true;
}

bool br_wv_read( std::string *manifest, std::string *library, std::string *err )
{
	TS_UUID obj;

	if ( !user_object(&obj) ) {
		*err = "not installed";
		return false;
	}
	ID k = ob_opn_obj(&obj, OB_OP_R);
	if ( k <= 0 ) {
		*err = "its object " + uuid_text(obj) + " cannot be opened";
		return false;
	}
	bool ok = rec_read(k, kRecManifest, manifest) && rec_read(k, kRecLib, library);
	ob_cls_obj(k);
	if ( !ok ) *err = "its object " + uuid_text(obj) + " cannot be read";
	return ok;
}
