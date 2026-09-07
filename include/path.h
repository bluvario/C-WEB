#ifndef CWEB_PATH_H
#define CWEB_PATH_H

#include "strbuf.h"
#include "sv.h"

// normalizes a URL or filesystem-ish path into *out (which should be empty):
// collapses duplicate slashes, drops "." segments, resolves ".." by popping.
// ".." that would climb above the root is a path traversal, returns -1 and
// leaves *out untouched. ".." within a relative path just pops.
int uri_path_normalize(String_View path, Strbuf *out);

#endif