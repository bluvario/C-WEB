#ifndef CWEB_TLS_H
#define CWEB_TLS_H

#include <stddef.h>
#include "net.h"

// Transport-security layer for the HTTP server. when OpenSSL is compiled in
// (the Makefile adds -DCWEB_OPENSSL when it finds it) a listener configured
// with tls_cert/tls_key wraps every accepted socket in a TLS session before
// any HTTP byte crosses the wire. every function here takes an opaque void *
// so the rest of the library stays OpenSSL-free; with the transport compiled
// out the stubs below politely return failure and the server serves plaintext
// only.

#ifdef CWEB_OPENSSL
// when OpenSSL is available, tls.c provides the full implementations.

int tls_available(void);

void *tls_server_ctx_new(const char *cert, const char *key,
                         char *err, size_t errsz);

void tls_server_ctx_free(void *ctx);

void *tls_accept_client(void *ctx, int fd);

long tls_recv(void *ssl, void *buf, size_t len);

long tls_send_all(void *ssl, const void *buf, size_t len);

void tls_close(void *ssl, int fd);

#else // !CWEB_OPENSSL -- static inline stubs so the library links without -lssl

#include <stdio.h>

static inline int tls_available(void) { return 0; }

static inline void *tls_server_ctx_new(const char *cert, const char *key,
                                       char *err, size_t errsz)
{
    (void)cert;
    (void)key;
    if (err != NULL && errsz > 0) {
        snprintf(err, errsz, "TLS not compiled in (OpenSSL not found)");
    }
    return NULL;
}

static inline void tls_server_ctx_free(void *ctx) { (void)ctx; }

static inline void *tls_accept_client(void *ctx, int fd)
{
    (void)ctx;
    (void)fd;
    return NULL;
}

static inline long tls_recv(void *ssl, void *buf, size_t len)
{
    (void)ssl;
    (void)buf;
    (void)len;
    return -1;
}

static inline long tls_send_all(void *ssl, const void *buf, size_t len)
{
    (void)ssl;
    (void)buf;
    (void)len;
    return -1;
}

static inline void tls_close(void *ssl, int fd)
{
    (void)ssl;
    net_close(fd);
}

#endif // CWEB_OPENSSL

#endif // CWEB_TLS_H
