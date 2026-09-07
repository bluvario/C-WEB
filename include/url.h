#ifndef CWEB_URL_H
#define CWEB_URL_H

#include "strbuf.h"
#include "sv.h"

// percent-decodes src into sb (appended to). decodes %XX and the '+' form
// used in query strings. returns 0 on success, -1 on a malformed %xx.
int url_decode_into(Strbuf *sb, String_View src);
// percent-encodes every byte outside RFC 3986 unreserved into sb (appended to)
void url_encode_into(Strbuf *sb, String_View src);

// malloc'd NUL-terminated decoded copy, or NULL when src is malformed.
// caller owns the result.
char *url_decode_alloc(String_View src);

#endif