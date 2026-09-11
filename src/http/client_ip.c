#include "client_ip.h"

#include <string.h>

int http_client_ip_resolve(String_View xff, String_View peer,
                           const Http_Cidr *trusted, size_t trusted_count,
                           String_View *out)
{
    // nothing to resolve against: whoever is on the other end is the client
    if (trusted == NULL || trusted_count == 0 || xff.count == 0) {
        *out = peer;
        return 0;
    }

    int peer_trusted = 0;
    for (size_t i = 0; i < trusted_count; i++) {
        if (http_cidr_contains(&trusted[i], peer) == 1) {
            peer_trusted = 1;
            break;
        }
    }
    // an untrusted peer is the client itself: taking its header at face value
    // would let anyone claim any address in our logs and rate buckets
    if (!peer_trusted) {
        *out = peer;
        return 0;
    }

    // split the header into hop addresses, trimming the usual spaces around
    // the commas; proxies usually append to the right, and the rightmost hop
    // a proxy can vouch for is the one everyone else saw first
    String_View hops[32];
    size_t nhops = 0;
    size_t i = 0;
    while (i <= xff.count && nhops < 32) {
        size_t j = i;
        while (j < xff.count && xff.data[j] != ',') {
            j++;
        }
        String_View e = {xff.data + i, j - i};
        while (e.count > 0 && (e.data[0] == ' ' || e.data[0] == '\t')) {
            e.data++;
            e.count--;
        }
        while (e.count > 0 && (e.data[e.count - 1] == ' ' ||
                               e.data[e.count - 1] == '\t')) {
            e.count--;
        }
        if (e.count > 0) {
            hops[nhops++] = e;
        }
        i = j + 1;
    }

    // skip over hops that are themselves our proxies (the hop chain), and
    // take the rightmost one that is not: that is the first address that
    // entered the trusted chain and so the real client. an unparsable hop
    // cannot name anyone, so it is skipped rather than trusted or chosen
    for (size_t k = nhops; k > 0; k--) {
        int saw_parsable = 0;
        int hop_trusted = 0;
        for (size_t c = 0; c < trusted_count; c++) {
            int r = http_cidr_contains(&trusted[c], hops[k - 1]);
            if (r == 1) {
                hop_trusted = 1;
                saw_parsable = 1;
                break;
            }
            if (r == 0) {
                saw_parsable = 1;
            }
        }
        if (!saw_parsable) {
            continue; // junk, not an IP; move to the next hop left
        }
        if (!hop_trusted) {
            *out = hops[k - 1];
            return 0;
        }
    }

    // a chain of nothing but trusted hops (or garbage) cannot name a client
    *out = peer;
    return 0;
}

void http_client_ip_middleware(Http_Request *req, Http_Response *res,
                               void *user_data,
                               Http_Handler_Fn next, void *next_data)
{
    Http_ClientIp_Options *opts = user_data;
    const Http_Cidr *trusted = opts != NULL ? opts->trusted : NULL;
    size_t count = opts != NULL ? opts->trusted_count : 0;

    const String_View *xff = http_request_get_header(req, "x-forwarded-for");
    String_View xffv = xff != NULL ? *xff : (String_View){0};
    http_client_ip_resolve(xffv, req->remote, trusted, count, &req->client_ip);

    next(req, res, next_data);
}