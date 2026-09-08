#ifndef CWEB_PARAMS_H
#define CWEB_PARAMS_H

#include "request.h"
#include "strmap.h"
#include "sv.h"

// parses url-encoded key=value pairs ("a=1&b=two%20words", the '+' form too)
// into m, overwriting earlier values on duplicate keys. keys and values are
// percent-decoded. returns 0 on success, -1 on malformed input.
int params_parse_into(Str_Map *m, String_View encoded);

// merges the request's query string and, when the body claims to be
// application/x-www-form-urlencoded, the form fields, all into out. the query
// goes first so later sources win. returns -1 on malformed percent encoding.
int request_merge_params(Http_Request *req, Str_Map *out);

#endif