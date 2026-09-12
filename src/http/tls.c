#include "tls.h"

#ifdef CWEB_OPENSSL

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

int tls_available(void)
{
    static int once = 0;
    if (!once) {
        once = 1;
        OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS |
                             OPENSSL_INIT_LOAD_CRYPTO_STRINGS,
                         NULL);
        // a peer that resets mid-write would otherwise SIGPIPE the server
        // out of existence instead of surfacing as a send error
        signal(SIGPIPE, SIG_IGN);
    }
    return 1;
}

void *tls_server_ctx_new(const char *cert, const char *key,
                         char *err, size_t errsz)
{
    tls_available();
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (ctx == NULL) {
        if (err != NULL && errsz > 0) {
            snprintf(err, errsz, "TLS_server_method failed");
        }
        return NULL;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION);
    SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);
    if (SSL_CTX_use_certificate_chain_file(ctx, cert) != 1) {
        if (err != NULL && errsz > 0) {
            snprintf(err, errsz, "cannot load certificate %s", cert);
        }
        SSL_CTX_free(ctx);
        return NULL;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) != 1) {
        if (err != NULL && errsz > 0) {
            snprintf(err, errsz, "cannot load private key %s", key);
        }
        SSL_CTX_free(ctx);
        return NULL;
    }
    if (SSL_CTX_check_private_key(ctx) != 1) {
        if (err != NULL && errsz > 0) {
            snprintf(err, errsz, "private key does not match certificate");
        }
        SSL_CTX_free(ctx);
        return NULL;
    }
    SSL_CTX_set_cipher_list(ctx, "HIGH:!aNULL:!MD5:!3DES");
    return ctx;
}

void tls_server_ctx_free(void *ctx)
{
    if (ctx != NULL) {
        SSL_CTX_free((SSL_CTX *)ctx);
    }
}

void *tls_accept_client(void *ctx, int fd)
{
    SSL_CTX *cctx = (SSL_CTX *)ctx;
    SSL *ssl = SSL_new(cctx);
    if (ssl == NULL) {
        return NULL;
    }
    SSL_set_fd(ssl, fd);
    int r = SSL_accept(ssl);
    if (r != 1) {
        SSL_free(ssl);
        return NULL;
    }
    return ssl;
}

long tls_recv(void *sslh, void *buf, size_t len)
{
    SSL *ssl = (SSL *)sslh;
    int r = SSL_read(ssl, buf, (int)(len > INT_MAX ? INT_MAX : len));
    if (r > 0) {
        return r;
    }
    switch (SSL_get_error(ssl, r)) {
    case SSL_ERROR_ZERO_RETURN:
        return 0;
    case SSL_ERROR_SYSCALL:
        if (r == 0) {
            return 0; // bare EOF from a dead peer
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return NET_READ_TIMEOUT;
        }
        return -1;
    case SSL_ERROR_WANT_READ:
    case SSL_ERROR_WANT_WRITE:
        return NET_READ_TIMEOUT;
    default:
        return -1;
    }
}

long tls_send_all(void *sslh, const void *buf, size_t len)
{
    SSL *ssl = (SSL *)sslh;
    const char *p = buf;
    size_t sent = 0;
    while (sent < len) {
        int chunk = (int)(len - sent > INT_MAX ? INT_MAX : len - sent);
        int n = SSL_write(ssl, p + sent, chunk);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        int e = SSL_get_error(ssl, n);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            continue; // blocking fd: retry is the right move
        }
        if (e == SSL_ERROR_SYSCALL &&
            (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        return -1;
    }
    return (long)sent;
}

void tls_close(void *sslh, int fd)
{
    if (sslh != NULL) {
        SSL *ssl = (SSL *)sslh;
        SSL_shutdown(ssl); // best-effort close_notify, ignore failure
        SSL_free(ssl);
    }
    net_close(fd);
}

#else // !CWEB_OPENSSL

int tls_available(void)
{
    return 0;
}

void *tls_server_ctx_new(const char *cert, const char *key,
                         char *err, size_t errsz)
{
    (void)cert;
    (void)key;
    if (err != NULL && errsz > 0) {
        snprintf(err, errsz, "TLS not compiled in (OpenSSL not found)");
    }
    return NULL;
}

void tls_server_ctx_free(void *ctx)
{
    (void)ctx;
}

void *tls_accept_client(void *ctx, int fd)
{
    (void)ctx;
    (void)fd;
    return NULL;
}

long tls_recv(void *ssl, void *buf, size_t len)
{
    (void)ssl;
    (void)buf;
    (void)len;
    return -1;
}

long tls_send_all(void *ssl, const void *buf, size_t len)
{
    (void)ssl;
    (void)buf;
    (void)len;
    return -1;
}

void tls_close(void *ssl, int fd)
{
    (void)ssl;
    net_close(fd);
}

#endif // CWEB_OPENSSL