#ifndef CWEB_SV_H
#define CWEB_SV_H

#include <stdbool.h>
#include <stddef.h>

// non-owning view over a string, does not allocate. everything the parser
// touches is a String_View so we can slice requests and templates without
// copying bytes around.
typedef struct {
    const char *data;
    size_t count;
} String_View;

#define SV_Fmt "@.*s"
#define SV_Arg(sv) (int)(sv).count, (sv).data

String_View sv_from_cstr(const char *s);

String_View sv_trim_left(String_View sv);
String_View sv_trim_right(String_View sv);
String_View sv_trim(String_View sv);

bool sv_equal(String_View a, String_View b);
// does sv start with the given NUL-terminated prefix?
bool sv_starts_with(String_View sv, const char *prefix);
// if sv starts with prefix, chop it off and return true
bool sv_consume_prefix(String_View *sv, const char *prefix);

// cuts everything up to delim out of sv and advances sv past the delim.
// the last piece has no delim after it.
String_View sv_chop_by_delim(String_View *sv, char delim);

// parses a decimal integer, returns false on garbage input
bool sv_to_i64(String_View sv, long long *out);

// how many times a byte shows up in sv
size_t sv_count_char(String_View sv, char c);

#endif