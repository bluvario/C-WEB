#ifndef CWEB_ROUTE_H
#define CWEB_ROUTE_H

#include "http.h"
#include "strmap.h"
#include "sv.h"

typedef struct {
    Http_Method method;
    const char *pattern; // borrowed, e.g. "/users/<id>/posts"
} Route_Def;

// matches a pattern against a request method and path. single segments of the
// form <name> capture the corresponding path segment into *params (raw, not
// percent-decoded). returns true on a match. *params may be NULL to skip
// captures. literal segments compare byte for byte.
bool route_match(Route_Def route, Http_Method method, String_View path, Str_Map *params);

#endif