#ifndef CWEB_IP_H
#define CWEB_IP_H

#include "sv.h"

// an IPv4 or IPv6 network: an address image plus a prefix length. both
// families share one fixed 16-byte address image (IPv4 lives in the final
// four bytes), so IPv4 and IPv6 networks can mix freely in one trusted list.
typedef struct {
    unsigned char addr[16]; // network address, big-endian image
    unsigned char prefix;   // prefix length in bits
    int v6;                 // 1 for IPv6, 0 for IPv4
} Http_Cidr;

// parses a network in CIDR or bare-host form: "1.2.3.4", "1.2.3.0/24",
// "10.0.0.0/8", "fe80::1", "fe80::/10", "::1/128", "0.0.0.0/0". a bare
// address is a /32 (IPv4) or /128 (IPv6) host route, and a bare network
// without "/length" is illegal. returns 0 and fills *out on success, -1 on
// malformed text (bad address, bad prefix, out-of-range length).
int http_cidr_parse(Http_Cidr *out, const char *text);

// 1 when the address ip (an IPv4/IPv6 address string, e.g. req->remote or an
// X-Forwarded-For hop) sits inside the network, 0 when it is outside, -1
// when ip is not a parsable address (different family always answers 0).
int http_cidr_contains(const Http_Cidr *c, String_View ip);

#endif