#ifndef CWEB_PARAMS_H
#define CWEB_PARAMS_H

#include "strmap.h"
#include "sv.h"

// parses url-encoded key=value pairs ("a=1&b=two%20words", the '+' form too)
// into m, overwriting earlier values on duplicate keys. keys and values are
// percent-decoded. returns 0 on success, -1 on malformed input.
int params_parse_into(Str_Map *m, String_View encoded);

#endif