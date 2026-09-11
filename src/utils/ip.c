#include "ip.h"

#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

// 0 for IPv4, 1 for IPv6; *img gets the big-endian address in the fixed
// 16-byte image (IPv4 occupies bytes 12..15). returns 0 on success.
static int parse_ip(String_View ip, unsigned char img[16], int *is_v6)
{
    if (ip.count > 45) {
        return -1;
    }
    char buf[46];
    memcpy(buf, ip.data, ip.count);
    buf[ip.count] = '\0';

    if (strchr(buf, '.') != NULL) {
        struct in_addr a;
        if (inet_pton(AF_INET, buf, &a) != 1) {
            return -1;
        }
        memset(img, 0, 16);
        memcpy(img + 12, &a, 4);
        *is_v6 = 0;
        return 0;
    }
    struct in6_addr a;
    if (inet_pton(AF_INET6, buf, &a) != 1) {
        return -1;
    }
    memcpy(img, &a, 16);
    *is_v6 = 1;
    return 0;
}

int http_cidr_parse(Http_Cidr *out, const char *text)
{
    // separate the optional "/N" suffix
    const char *slash = strchr(text, '/');
    char ipbuf[46];
    size_t iplen = slash != NULL ? (size_t)(slash - text) : strlen(text);
    if (iplen == 0 || iplen >= sizeof ipbuf) {
        return -1;
    }
    memcpy(ipbuf, text, iplen);
    ipbuf[iplen] = '\0';

    Http_Cidr c;
    if (parse_ip(sv_from_cstr(ipbuf), c.addr, &c.v6) != 0) {
        return -1;
    }

    if (slash != NULL) {
        char *endp = NULL;
        long p = strtol(slash + 1, &endp, 10);
        if (slash[1] == '\0' || endp == NULL || *endp != '\0') {
            return -1;
        }
        long maxp = c.v6 ? 128 : 32;
        if (p < 0 || p > maxp) {
            return -1;
        }
        c.prefix = (unsigned char)p;
    } else {
        // a bare address is a host route
        c.prefix = c.v6 ? 128 : 32;
    }

    *out = c;
    return 0;
}

int http_cidr_contains(const Http_Cidr *c, String_View ip)
{
    unsigned char a[16];
    int v6;
    if (parse_ip(ip, a, &v6) != 0) {
        return -1;
    }
    if (v6 != c->v6) {
        return 0; // an IPv4 address never sits inside an IPv6 network, and
                  // vice versa, no matter how the prefix overlaps
    }

    // IPv4 shares the image's tail, so all four kinds of prefix compare from
    // the same byte: 12 for IPv4, 0 for IPv6
    size_t base = v6 ? 0 : 12;
    int bits = c->prefix;
    for (int i = 0; i < bits / 8; i++) {
        if (a[base + i] != c->addr[base + i]) {
            return 0;
        }
    }
    int rem = bits % 8;
    if (rem != 0) {
        unsigned char mask = (unsigned char)(0xFFu << (8 - rem));
        if ((a[base + bits / 8] & mask) != (c->addr[base + bits / 8] & mask)) {
            return 0;
        }
    }
    return 1;
}