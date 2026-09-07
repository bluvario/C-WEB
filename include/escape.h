#ifndef CWEB_ESCAPE_H
#define CWEB_ESCAPE_H

#include "strbuf.h"
#include "sv.h"

// appends src to sb with &, <, >, " and ' replaced by HTML entities.
// used everywhere we drop user input into a page.
void html_escape_into(Strbuf *sb, String_View src);

#endif