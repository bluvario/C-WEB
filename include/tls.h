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
//
// besides loading a certificate pair from files (tls_server_ctx_new) the
// layer can mint its own self-signed pair on first use: tls_self_signed_pem
// generates fresh PEM strings, tls_server_ctx_from_pems feeds them straight
// into a server context, and tls_self_signed_paths materialises the pair on
// disk under a directory so a TOFU ("trust on first use") server keeps the
// same thumbprint across restarts.

#ifdef CWEB_OPENSSL
// when OpenSSL is available, tls.c provides the full implementations.

int tls_available(void);

void *tls_server_ctx_new(const char *cert, const char *key,
                         char *err, size_t errsz);

void tls_server_ctx_free(void *ctx);

// generate a self-signed X509v3 certificate and an RSA-2048 private key as
// PEM strings, each NUL-terminated and freshly malloc'd (caller frees with
// free()). common_name (NULL -> "localhost") and every extra_dns entry become
// DNS subjectAltNames; 127.0.0.1, ::1 and "localhost" are always added. the
// pair is valid for validity_days and forbids CA use. returns 0 on success.
int tls_self_signed_pem(const char *common_name, const char **extra_dns,
                        long validity_days, char *err, size_t errsz,
                        char **cert_pem, char **key_pem);

// build a server context from in-memory PEM strings instead of file paths,
// with the same tune-up tls_server_ctx_new applies (TLS1.2 min, no
// compression, strict ciphers, matching key).
void *tls_server_ctx_from_pems(const char *cert_pem, const char *key_pem,
                               char *err, size_t errsz);

// sha256 fingerprint (lowercase hex) of the DER form of a PEM certificate
// file. returns 0 on success.
int tls_cert_file_sha256(const char *cert_path, char *out, size_t outsz);

// TOFU entry point: make sure dir/name.pem + dir/name-key.pem exist (creating
// the pair with 0700/0600 permissions when missing) and hand the two paths
// back. *created is set to 1 when the pair was just generated, 0 when it was
// already there. returns 0 on success.
int tls_self_signed_paths(const char *dir, const char *name,
                          char *cert_out, size_t certsz,
                          char *key_out, size_t keysz,
                          int *created, char *err, size_t errsz);

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

static inline int tls_self_signed_pem(const char *common_name,
                                      const char **extra_dns, long validity_days,
                                      char *err, size_t errsz,
                                      char **cert_pem, char **key_pem)
{
    (void)common_name;
    (void)extra_dns;
    (void)validity_days;
    (void)cert_pem;
    (void)key_pem;
    if (err != NULL && errsz > 0) {
        snprintf(err, errsz, "TLS not compiled in (OpenSSL not found)");
    }
    return -1;
}

static inline void *tls_server_ctx_from_pems(const char *cert_pem,
                                             const char *key_pem, char *err,
                                             size_t errsz)
{
    (void)cert_pem;
    (void)key_pem;
    if (err != NULL && errsz > 0) {
        snprintf(err, errsz, "TLS not compiled in (OpenSSL not found)");
    }
    return NULL;
}

static inline int tls_cert_file_sha256(const char *cert_path, char *out,
                                       size_t outsz)
{
    (void)cert_path;
    (void)out;
    (void)outsz;
    return -1;
}

static inline int tls_self_signed_paths(const char *dir, const char *name,
                                        char *cert_out, size_t certsz,
                                        char *key_out, size_t keysz,
                                        int *created, char *err, size_t errsz)
{
    (void)dir;
    (void)name;
    (void)cert_out;
    (void)certsz;
    (void)key_out;
    (void)keysz;
    (void)created;
    if (err != NULL && errsz > 0) {
        snprintf(err, errsz, "TLS not compiled in (OpenSSL not found)");
    }
    return -1;
}

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
