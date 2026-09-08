#ifndef CWEB_MULTIPART_H
#define CWEB_MULTIPART_H

#include "strmap.h"
#include "sv.h"

// one section of a multipart/form-data body
typedef struct {
    Str_Map headers;   // part headers, keys lowercased, values owned
    String_View name;     // the form field name (never empty)
    String_View filename; // file parts have this set, plain fields empty
    String_View content;  // the part's bytes
} Multipart_Part;

typedef struct {
    Multipart_Part *items;
    size_t count;
    size_t capacity;
} Multipart;

// boundaries longer than this cannot be represented in a stack buffer
#define MULTIPART_MAX_BOUNDARY 200

// pulls the boundary token out of a Content-Type header value like
// "multipart/form-data; boundary=------------------------abc". fills *out
// (NUL-terminated), returns 0, or -1 when the value is not multipart or has
// no boundary.
int multipart_boundary(String_View content_type_value, char *out, size_t out_sz);

// splits body on boundary into parts. part->headers are owned but name,
// filename and content borrow from body, so body must outlive the list.
// returns 0 on success, -1 on malformed framing.
int multipart_parse(Multipart *m, String_View body, const char *boundary);

// the first part with the given field name, NULL when absent
Multipart_Part *multipart_get(Multipart *m, const char *name);

void multipart_free(Multipart *m);

#endif