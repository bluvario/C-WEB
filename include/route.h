#ifndef CWEB_ROUTE_H
#define CWEB_ROUTE_H

#include "http.h"
#include "strmap.h"
#include "sv.h"

typedef struct {
    Http_Method method;
    const char *pattern; // borrowed, e.g. "/users/<id>/posts"
} Route_Def;

// matches just the path portion of a pattern. single segments of the form
// <name> capture the corresponding path segment into *params (raw, not
// percent-decoded). *params may be NULL to skip captures. literal segments
// compare byte for byte. a terminal "*" segment is a catch-all prefix: it
// matches the remainder of the path, so "/a/*" also matches plain "/a".
bool route_path_matches(const char *pattern, String_View path, Str_Map *params);

// matches a pattern against a request method and path, i.e. a method check
// in front of route_path_matches
bool route_match(Route_Def route, Http_Method method, String_View path, Str_Map *params);

#endif