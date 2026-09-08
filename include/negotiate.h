#ifndef CWEB_NEGOTIATE_H
#define CWEB_NEGOTIATE_H

#include <stddef.h>

#include "sv.h"

// one media range from an Accept header, e.g. "text/html" with q=1.0
typedef struct {
    String_View type; // full range as sent, "text/html", "text/*" or "*/*"
    double q;         // 0..1, 1.0 when the client sent no q
} Accept_Item;

// a parsed Accept header value
typedef struct {
    Accept_Item *items;
    size_t count;
    size_t capacity;
} Accept_List;

// parses an Accept header value ("text/html, application/json;q=0.8") into a
// list of media ranges. empty segments are skipped, unknown parameters are
// ignored, and a malformed q value fails the whole parse. returns 0 on
// success, -1 on malformed input.
int accept_parse(Accept_List *list, String_View header);

// the best match for available out of the client's accept list, or -1 when
// the client excludes every available representation. the most specific
// media range decides the quality for a type (RFC 9110), so a q=0 exclusion
// still kills an otherwise acceptable wildcard. an empty list accepts
// everything, so callers with a missing Accept header can skip negotiation.
// ties favour the earlier entry in available.
int accept_best(Accept_List *list, String_View *available, size_t count);

void accept_free(Accept_List *list);

#endif