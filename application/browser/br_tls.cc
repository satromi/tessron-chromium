/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_tls.cc
 *	TLS for ブラウザ (design 17.19): the br_tls_* calls that
 *	br_http.cc makes for an address of https, over BoringSSL.
 *
 *	TLS 1.2 and 1.3. The server's chain is checked against the root
 *	certificates built into the program (br_roots, the PEM blocks of the
 *	engine's root store, assembled in by tools/browser/build.sh) and its
 *	name against the host asked for. The socket is reached through a BIO
 *	of its own that calls send and recv, so the receive time-out of the
 *	socket holds for the handshake too.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include <string>

#include "openssl/bio.h"
#include "openssl/err.h"
#include "openssl/pem.h"
#include "openssl/ssl.h"
#include "openssl/x509.h"

extern "C" const char br_roots[];
extern "C" const char br_roots_end[];

namespace {

SSL_CTX		*g_ctx;
BIO_METHOD	*g_meth;
std::string	g_err;
int		g_roots;

int fd_of( BIO *b )
{
	return (int)(intptr_t)BIO_get_data(b);
}

int sock_write( BIO *b, const char *p, int n )
{
	BIO_clear_retry_flags(b);
	return (int)send(fd_of(b), p, (size_t)n, 0);
}

int sock_read( BIO *b, char *p, int n )
{
	BIO_clear_retry_flags(b);
	return (int)recv(fd_of(b), p, (size_t)n, 0);
}

long sock_ctrl( BIO *b, int cmd, long num, void *ptr )
{
	(void)b;
	(void)num;
	(void)ptr;
	return ( cmd == BIO_CTRL_FLUSH ) ? 1 : 0;
}

/* The roots, once: every PEM block of br_roots into the context's store */
int load_roots( SSL_CTX *ctx )
{
	X509_STORE	*st = SSL_CTX_get_cert_store(ctx);
	BIO		*m = BIO_new_mem_buf(br_roots, (ossl_ssize_t)( br_roots_end - br_roots ));
	X509		*x;
	int		n = 0;

	if ( m == nullptr ) return 0;
	while ( ( x = PEM_read_bio_X509(m, nullptr, nullptr, nullptr) ) != nullptr ) {
		if ( X509_STORE_add_cert(st, x) ) n++;
		X509_free(x);
	}
	ERR_clear_error();		/* the end of the blocks is reported as an error */
	BIO_free(m);
	return n;
}

bool setup()
{
	static const unsigned char alpn[] = { 8, 'h', 't', 't', 'p', '/', '1', '.', '1' };

	if ( g_ctx != nullptr ) return true;
	g_meth = BIO_meth_new(BIO_TYPE_SOCKET, "tessronos socket");
	if ( g_meth == nullptr ) return false;
	BIO_meth_set_write(g_meth, sock_write);
	BIO_meth_set_read(g_meth, sock_read);
	BIO_meth_set_ctrl(g_meth, sock_ctrl);
	g_ctx = SSL_CTX_new(TLS_method());
	if ( g_ctx == nullptr ) return false;
	SSL_CTX_set_min_proto_version(g_ctx, TLS1_2_VERSION);
	SSL_CTX_set_verify(g_ctx, SSL_VERIFY_PEER, nullptr);
	SSL_CTX_set_alpn_protos(g_ctx, alpn, sizeof(alpn));
	g_roots = load_roots(g_ctx);
	printf("browser: TLS with %d root certificates\n", g_roots);
	return true;
}

std::string reason( SSL *s, int r )
{
	long v = SSL_get_verify_result(s);
	if ( v != X509_V_OK ) return std::string("証明書: ") + X509_verify_cert_error_string(v);

	int e = SSL_get_error(s, r);
	unsigned long q = ERR_get_error();
	if ( q != 0 ) {
		char buf[160];
		ERR_error_string_n(q, buf, sizeof(buf));
		return buf;
	}
	if ( e == SSL_ERROR_SYSCALL ) return std::string("接続: ") + strerror(errno);
	return "TLS の誤り " + std::to_string(e);
}

}  // namespace

extern "C" {

void *br_tls_open( int fd, const char *host )
{
	g_err.clear();
	if ( !setup() ) {
		g_err = "TLS を用意できません";
		return nullptr;
	}
	SSL *s = SSL_new(g_ctx);
	BIO *b = BIO_new(g_meth);
	if ( s == nullptr || b == nullptr ) {
		if ( b != nullptr ) BIO_free(b);
		if ( s != nullptr ) SSL_free(s);
		g_err = "記憶が足りません";
		return nullptr;
	}
	BIO_set_data(b, (void *)(intptr_t)fd);
	BIO_set_init(b, 1);
	SSL_set_bio(s, b, b);
	SSL_set_tlsext_host_name(s, host);
	X509_VERIFY_PARAM_set1_host(SSL_get0_param(s), host, strlen(host));

	int r = SSL_connect(s);
	if ( r != 1 ) {
		g_err = reason(s, r);
		printf("browser: TLS to %s: %s\n", host, g_err.c_str());
		ERR_clear_error();
		SSL_free(s);
		return nullptr;
	}
	printf("browser: TLS to %s: %s %s\n", host, SSL_get_version(s),
	       SSL_CIPHER_get_name(SSL_get_current_cipher(s)));
	return s;
}

int br_tls_write( void *p, const void *buf, int n )
{
	int r = SSL_write((SSL *)p, buf, n);
	if ( r <= 0 ) {
		ERR_clear_error();
		return -1;
	}
	return r;
}

/* The bytes that came; 0 at the end, also when the peer closed without
   saying so (the length of the body is known from HTTP) */
int br_tls_read( void *p, void *buf, int n )
{
	SSL	*s = (SSL *)p;
	int	r = SSL_read(s, buf, n);

	if ( r > 0 ) return r;
	int e = SSL_get_error(s, r);
	ERR_clear_error();
	if ( e == SSL_ERROR_ZERO_RETURN ) return 0;
	if ( e == SSL_ERROR_SYSCALL && r == 0 ) return 0;
	return -1;
}

void br_tls_close( void *p )
{
	SSL_free((SSL *)p);
}

const char *br_tls_error( void )
{
	return g_err.c_str();
}

}  // extern "C"
