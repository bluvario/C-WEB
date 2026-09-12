#include "tls.h"

#ifdef CWEB_OPENSSL

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "file.h"

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

// shared construction: base server method, TLS1.2 floor, no compression,
// auto-retry on blocking sockets, and no weak or NULL ciphers. both loaders
// below rely on it.
static SSL_CTX *tls_ctx_base(char *err, size_t errsz);

void *tls_server_ctx_new(const char *cert, const char *key,
                         char *err, size_t errsz)
{
    SSL_CTX *ctx = tls_ctx_base(err, errsz);
    if (ctx == NULL) {
        return NULL;
    }
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
    return ctx;
}

static SSL_CTX *tls_ctx_base(char *err, size_t errsz)
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
    if (SSL_CTX_set_cipher_list(ctx, "HIGH:!aNULL:!MD5:!3DES") != 1) {
        if (err != NULL && errsz > 0) {
            snprintf(err, errsz, "cipher list is empty on this OpenSSL");
        }
        SSL_CTX_free(ctx);
        return NULL;
    }
    return ctx;
}

void *tls_server_ctx_from_pems(const char *cert_pem, const char *key_pem,
                               char *err, size_t errsz)
{
    SSL_CTX *ctx = tls_ctx_base(err, errsz);
    if (ctx == NULL) {
        return NULL;
    }
    BIO *cbio = BIO_new_mem_buf(cert_pem, -1);
    X509 *cert = cbio != NULL ? PEM_read_bio_X509(cbio, NULL, NULL, NULL)
                              : NULL;
    BIO_free(cbio);
    if (cert == NULL ||
        SSL_CTX_use_certificate(ctx, cert) != 1) {
        if (err != NULL && errsz > 0) {
            snprintf(err, errsz, "cannot load PEM certificate");
        }
        X509_free(cert);
        SSL_CTX_free(ctx);
        return NULL;
    }
    BIO *kbio = BIO_new_mem_buf(key_pem, -1);
    EVP_PKEY *pkey = kbio != NULL ? PEM_read_bio_PrivateKey(kbio, NULL, NULL,
                                                           NULL)
                                  : NULL;
    BIO_free(kbio);
    if (pkey == NULL ||
        SSL_CTX_use_PrivateKey(ctx, pkey) != 1) {
        if (err != NULL && errsz > 0) {
            snprintf(err, errsz, "cannot load PEM private key");
        }
        EVP_PKEY_free(pkey);
        X509_free(cert);
        SSL_CTX_free(ctx);
        return NULL;
    }
    if (SSL_CTX_check_private_key(ctx) != 1) {
        if (err != NULL && errsz > 0) {
            snprintf(err, errsz, "private key does not match certificate");
        }
        EVP_PKEY_free(pkey);
        X509_free(cert);
        SSL_CTX_free(ctx);
        return NULL;
    }
    EVP_PKEY_free(pkey);
    X509_free(cert);
    return ctx;
}

void tls_server_ctx_free(void *ctx)
{
    if (ctx != NULL) {
        SSL_CTX_free((SSL_CTX *)ctx);
    }
}

// returns a malloc'd NUL-terminated copy of everything a memory BIO holds
static char *bio_to_cstr(BIO *bio)
{
    long len = BIO_get_mem_data(bio, NULL);
    if (len < 0) {
        return NULL;
    }
    const char *data = NULL;
    BIO_get_mem_data(bio, &data);
    char *s = malloc((size_t)len + 1);
    if (s == NULL) {
        return NULL;
    }
    if (len > 0) {
        memcpy(s, data, (size_t)len);
    }
    s[len] = '\0';
    return s;
}

// pushes one DNS name into the SAN stack; returns 0 on failure
static int san_add_dns(GENERAL_NAMES *sans, const char *dns)
{
    ASN1_STRING *as = ASN1_IA5STRING_new();
    if (as == NULL || ASN1_STRING_set(as, dns, -1) != 1) {
        ASN1_STRING_free(as);
        return 0;
    }
    GENERAL_NAME *name = GENERAL_NAME_new();
    if (name == NULL) {
        ASN1_STRING_free(as);
        return 0;
    }
    GENERAL_NAME_set0_value(name, GEN_DNS, as); // takes ownership of as
    if (sk_GENERAL_NAME_push(sans, name) == 0) {
        GENERAL_NAME_free(name);
        return 0;
    }
    return 1;
}

// pushes one raw IP address into the SAN stack; returns 0 on failure
static int san_add_ip(GENERAL_NAMES *sans, const unsigned char *octets,
                      int nbytes)
{
    ASN1_OCTET_STRING *os = ASN1_OCTET_STRING_new();
    if (os == NULL || ASN1_OCTET_STRING_set(os, octets, nbytes) != 1) {
        ASN1_OCTET_STRING_free(os);
        return 0;
    }
    GENERAL_NAME *name = GENERAL_NAME_new();
    if (name == NULL) {
        ASN1_OCTET_STRING_free(os);
        return 0;
    }
    GENERAL_NAME_set0_value(name, GEN_IPADD, os); // takes ownership of os
    if (sk_GENERAL_NAME_push(sans, name) == 0) {
        GENERAL_NAME_free(name);
        return 0;
    }
    return 1;
}

int tls_self_signed_pem(const char *common_name, const char **extra_dns,
                        long validity_days, char *err, size_t errsz,
                        char **cert_pem, char **key_pem)
{
    if (common_name == NULL) {
        common_name = "localhost";
    }
    if (cert_pem == NULL || key_pem == NULL) {
        return -1;
    }
    *cert_pem = NULL;
    *key_pem = NULL;
    tls_available();

    EVP_PKEY *pkey = NULL;
    EVP_PKEY_CTX *kgen = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
    if (kgen == NULL || EVP_PKEY_keygen_init(kgen) != 1 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(kgen, 2048) != 1 ||
        EVP_PKEY_keygen(kgen, &pkey) != 1) {
        EVP_PKEY_CTX_free(kgen);
        if (err != NULL && errsz > 0) {
            snprintf(err, errsz, "RSA key generation failed");
        }
        return -1;
    }
    EVP_PKEY_CTX_free(kgen);

    X509 *x = NULL;
    X509_NAME *nm = NULL;
    int ok = 0;
    if ((x = X509_new()) == NULL ||
        X509_set_version(x, 2) != 1 || // X509v3
        ASN1_INTEGER_set(X509_get_serialNumber(x),
                         (long)((time(NULL) & 0x7fffffff) ^
                                (rand() & 0x7fffff))) != 1 ||
        X509_gmtime_adj(X509_getm_notBefore(x), 0) == NULL ||
        X509_gmtime_adj(X509_getm_notAfter(x),
                        60L * 60 * 24 * validity_days) == NULL ||
        X509_set_pubkey(x, pkey) != 1) {
        goto cleanup;
    }
    nm = X509_get_subject_name(x);
    if (X509_NAME_add_entry_by_txt(nm, "O", MBSTRING_ASC,
                                   (const unsigned char *)"cweb",
                                   -1, -1, 0) != 1 ||
        X509_NAME_add_entry_by_txt(nm, "CN", MBSTRING_ASC,
                                   (const unsigned char *)common_name,
                                   -1, -1, 0) != 1 ||
        X509_set_issuer_name(x, nm) != 1) { // self-signed
        goto cleanup;
    }

    // subjectAltName: DNS common_name + localhost + extra_dns, and the two
    // loopback IP families so "https://localhost" and "https://127.0.0.1"
    // both verify against the name instead of failing on the default check
    GENERAL_NAMES *sans = sk_GENERAL_NAME_new_null();
    if (sans == NULL) {
        goto cleanup;
    }
    ok = san_add_dns(sans, common_name);
    if (ok) {
        ok = san_add_dns(sans, "localhost");
    }
    if (ok && extra_dns != NULL) {
        for (size_t i = 0; extra_dns[i] != NULL; i++) {
            if (!san_add_dns(sans, extra_dns[i])) {
                ok = 0;
                break;
            }
        }
    }
    static const unsigned char v4[4] = {127, 0, 0, 1};
    static const unsigned char v6[16] =
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    if (ok) {
        ok = san_add_ip(sans, v4, 4);
    }
    if (ok) {
        ok = san_add_ip(sans, v6, 16);
    }
    if (ok) {
        X509_EXTENSION *ext = X509V3_EXT_i2d(NID_subject_alt_name, 0, sans);
        if (ext != NULL) {
            ok = X509_add_ext(x, ext, -1) == 1;
            X509_EXTENSION_free(ext);
        } else {
            ok = 0;
        }
    }
    sk_GENERAL_NAME_pop_free(sans, GENERAL_NAME_free);
    if (!ok) {
        goto cleanup;
    }

    // leaf, not a CA: forbid CA use and claim only server-auth
    BASIC_CONSTRAINTS *bc = BASIC_CONSTRAINTS_new();
    X509_EXTENSION *bext = bc != NULL
                               ? X509V3_EXT_i2d(NID_basic_constraints, 0, bc)
                               : NULL;
    BASIC_CONSTRAINTS_free(bc);
    ok = bext != NULL && X509_add_ext(x, bext, -1) == 1;
    X509_EXTENSION_free(bext);
    if (!ok) {
        goto cleanup;
    }
    EXTENDED_KEY_USAGE *eku = sk_ASN1_OBJECT_new_null();
    if (eku == NULL ||
        sk_ASN1_OBJECT_push(eku, OBJ_nid2obj(NID_server_auth)) == 0) {
        sk_ASN1_OBJECT_free(eku);
        goto cleanup;
    }
    X509_EXTENSION *kext = X509V3_EXT_i2d(NID_ext_key_usage, 0, eku);
    sk_ASN1_OBJECT_free(eku);
    ok = kext != NULL && X509_add_ext(x, kext, -1) == 1;
    X509_EXTENSION_free(kext);
    if (!ok) {
        goto cleanup;
    }

    if (X509_sign(x, pkey, EVP_sha256()) <= 0) {
        goto cleanup;
    }

    BIO *cb = BIO_new(BIO_s_mem());
    BIO *kb = BIO_new(BIO_s_mem());
    if (cb == NULL || kb == NULL) {
        BIO_free(cb);
        BIO_free(kb);
        goto cleanup;
    }
    if (PEM_write_bio_X509(cb, x) != 1 ||
        PEM_write_bio_PrivateKey(kb, pkey, NULL, NULL, 0, NULL, NULL) != 1) {
        BIO_free(cb);
        BIO_free(kb);
        goto cleanup;
    }
    *cert_pem = bio_to_cstr(cb);
    *key_pem = bio_to_cstr(kb);
    BIO_free(cb);
    BIO_free(kb);
    if (*cert_pem == NULL || *key_pem == NULL) {
        goto cleanup;
    }
    ok = 1;
    goto cleanup;

cleanup:
    X509_free(x);
    EVP_PKEY_free(pkey);
    if (!ok) {
        free(*cert_pem);
        free(*key_pem);
        *cert_pem = NULL;
        *key_pem = NULL;
        if (err != NULL && errsz > 0) {
            snprintf(err, errsz, "self-signed certificate generation failed");
        }
    }
    return ok ? 0 : -1;
}

int tls_cert_file_sha256(const char *cert_path, char *out, size_t outsz)
{
    FILE *f = fopen(cert_path, "rb");
    if (f == NULL) {
        return -1;
    }
    X509 *x = PEM_read_X509(f, NULL, NULL, NULL);
    fclose(f);
    if (x == NULL) {
        return -1;
    }
    int dlen = i2d_X509(x, NULL);
    unsigned char *der = malloc(dlen > 0 ? (size_t)dlen : 1);
    int rc = -1;
    if (der != NULL) {
        unsigned char *p = der;
        if (i2d_X509(x, &p) == dlen) {
            unsigned char md[EVP_MAX_MD_SIZE];
            unsigned int mdlen = 0;
            if (EVP_Digest(der, (size_t)dlen, md, &mdlen, EVP_sha256(),
                           NULL) == 1 &&
                outsz >= (size_t)mdlen * 2 + 1) {
                for (unsigned int i = 0; i < mdlen; i++) {
                    snprintf(out + (size_t)i * 2, 3, "%02x", md[i]);
                }
                rc = 0;
            }
        }
        free(der);
    }
    X509_free(x);
    return rc;
}

int tls_self_signed_paths(const char *dir, const char *name,
                          char *cert_out, size_t certsz,
                          char *key_out, size_t keysz,
                          int *created, char *err, size_t errsz)
{
    if (dir == NULL || dir[0] == '\0' ||
        name == NULL || name[0] == '\0' ||
        cert_out == NULL || key_out == NULL) {
        if (err != NULL && errsz > 0) {
            snprintf(err, errsz, "invalid tls path arguments");
        }
        return -1;
    }
    if (created != NULL) {
        *created = 0;
    }
    snprintf(cert_out, certsz, "%s/%s.pem", dir, name);
    snprintf(key_out, keysz, "%s/%s-key.pem", dir, name);
    struct stat st;
    if (stat(cert_out, &st) == 0 && stat(key_out, &st) == 0) {
        return 0; // already minted on a previous run: keep the thumbprint
    }
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        if (err != NULL && errsz > 0) {
            snprintf(err, errsz, "mkdir %s failed: %s", dir, strerror(errno));
        }
        return -1;
    }
    char gen_err[256];
    char *cpem = NULL;
    char *kpem = NULL;
    if (tls_self_signed_pem("localhost", NULL, 3650, gen_err, sizeof gen_err,
                            &cpem, &kpem) != 0) {
        if (err != NULL && errsz > 0) {
            snprintf(err, errsz, "self-signed generation failed: %s", gen_err);
        }
        return -1;
    }
    int rc = -1;
    if (file_write(cert_out, cpem, strlen(cpem)) == 0 &&
        file_write(key_out, kpem, strlen(kpem)) == 0) {
        rc = 0;
    }
    chmod(cert_out, 0644);
    chmod(key_out, 0600);
    free(cpem);
    free(kpem);
    if (rc == 0 && created != NULL) {
        *created = 1;
    }
    return rc;
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