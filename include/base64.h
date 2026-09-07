#ifndef CWEB_BASE64_H
#define CWEB_BASE64_H

#include "strbuf.h"
#include "sv.h"

// standard base64 with padding, appended to sb
void base64_encode_into(Strbuf *sb, String_View data);

// decodes into sb. trailing '=' padding handled leniently, returns -1 on any
// character that is not in the base64 alphabet.
int base64_decode_into(Strbuf *sb, String_View text);

#endif