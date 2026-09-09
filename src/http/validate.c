#include "validate.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "escape.h"
#include "strbuf.h"
#include "sv.h"

static const char *label_for(const char *field, const char *label)
{
    return label != NULL ? label : field;
}

static void fail(Http_Form *f, const char *fmt, const char *label)
{
    f->ok = 0;
    snprintf(f->error, sizeof f->error, fmt, label);
}

// finds the trimmed value for field; returns 1 and sets *start/*len when the
// trimmed value holds at least one character, 0 when it is absent or blank
static int trimmed_value(Str_Map *params, const char *field,
                         const char **start, size_t *len)
{
    if (params == NULL || field == NULL) {
        return 0;
    }
    const char *v = strmap_get_cstr(params, field);
    if (v == NULL) {
        return 0;
    }
    size_t n = strlen(v);
    while (n > 0 && isspace((unsigned char)v[n - 1])) {
        n--;
    }
    size_t off = 0;
    while (off < n && isspace((unsigned char)v[off])) {
        off++;
    }
    *start = v + off;
    *len = n - off;
    return *len > 0;
}

void http_form_init(Http_Form *f)
{
    f->error[0] = '\0';
    f->ok = 1;
}

void http_form_required(Http_Form *f, Str_Map *params,
                        const char *field, const char *label)
{
    if (!f->ok) {
        return;
    }
    const char *s;
    size_t n;
    if (!trimmed_value(params, field, &s, &n)) {
        fail(f, "%s is required.", label_for(field, label));
    }
}

// counts characters (UTF-8 lead bytes) in the first n bytes
static size_t utf8_len(const char *s, size_t n)
{
    size_t chars = 0;
    for (size_t i = 0; i < n; i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) {
            chars++;
        }
    }
    return chars;
}

void http_form_length(Http_Form *f, Str_Map *params,
                      const char *field, const char *label,
                      size_t min, size_t max)
{
    if (!f->ok) {
        return;
    }
    const char *s;
    size_t n;
    if (!trimmed_value(params, field, &s, &n)) {
        return;
    }
    size_t chars = utf8_len(s, n);
    const char *lab = label_for(field, label);
    if (chars < min) {
        f->ok = 0;
        if (max != 0) {
            snprintf(f->error, sizeof f->error,
                     "%s must be between %zu and %zu characters.", lab, min, max);
        } else {
            snprintf(f->error, sizeof f->error,
                     "%s must be at least %zu characters.", lab, min);
        }
    } else if (max != 0 && chars > max) {
        f->ok = 0;
        if (min != 0) {
            snprintf(f->error, sizeof f->error,
                     "%s must be between %zu and %zu characters.", lab, min, max);
        } else {
            snprintf(f->error, sizeof f->error,
                     "%s must be at most %zu characters.", lab, max);
        }
    }
}

static int local_char_ok(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '_' ||
           c == '+' || c == '-';
}

// domain must be dotted: at least one '.' that is not leading, trailing or
// doubled, letters/digits/hyphens only otherwise
static int domain_part_ok(const char *d, size_t n)
{
    if (n == 0) {
        return 0;
    }
    int dot_seen = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)d[i];
        if (c == '.') {
            if (i == 0 || i + 1 == n || d[i - 1] == '.') {
                return 0;
            }
            dot_seen = 1;
        } else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') || c == '-')) {
            return 0;
        }
    }
    return dot_seen;
}

void http_form_email(Http_Form *f, Str_Map *params,
                     const char *field, const char *label)
{
    if (!f->ok) {
        return;
    }
    const char *s;
    size_t n;
    if (!trimmed_value(params, field, &s, &n)) {
        return;
    }
    const char *at = memchr(s, '@', n);
    int ok_email = at != NULL;
    if (ok_email) {
        size_t local = (size_t)(at - s);
        const char *dom = at + 1;
        size_t domlen = n - local - 1;
        ok_email = local > 0 && local <= 64 &&
                   memchr(dom, '@', domlen) == NULL &&
                   domain_part_ok(dom, domlen);
        for (size_t i = 0; ok_email && i < local; i++) {
            ok_email = local_char_ok((unsigned char)s[i]);
        }
    }
    if (!ok_email) {
        fail(f, "%s is not a valid e-mail address.", label_for(field, label));
    }
}

void http_form_int(Http_Form *f, Str_Map *params,
                   const char *field, const char *label,
                   long lo, long hi, long *out)
{
    if (!f->ok) {
        return;
    }
    const char *s;
    size_t n;
    if (!trimmed_value(params, field, &s, &n)) {
        return;
    }
    // the value must start with a digit (after an optional sign), so "-", "+"
    // and plain garbage are refused by strtol without ambiguity
    size_t sign = (s[0] == '+' || s[0] == '-');
    if (n <= sign || !isdigit((unsigned char)s[sign])) {
        fail(f, "%s must be a whole number.", label_for(field, label));
        return;
    }
    char buf[32];
    if (n >= sizeof buf) {
        fail(f, "%s must be a whole number.", label_for(field, label));
        return;
    }
    memcpy(buf, s, n);
    buf[n] = '\0';
    char *end = NULL;
    errno = 0;
    long v = strtol(buf, &end, 10);
    if (end == buf || *end != '\0' || errno != 0) {
        fail(f, "%s must be a whole number.", label_for(field, label));
        return;
    }
    const char *lab = label_for(field, label);
    if (v < lo || v > hi) {
        f->ok = 0;
        snprintf(f->error, sizeof f->error,
                 "%s must be between %ld and %ld.", lab, lo, hi);
        return;
    }
    if (out != NULL) {
        *out = v;
    }
}

int http_form_ok(const Http_Form *f)
{
    return f->ok;
}

const char *http_form_error(const Http_Form *f)
{
    return f->ok ? NULL : f->error;
}

void http_form_value(Http_Response *res, Str_Map *params, const char *field)
{
    if (params == NULL || field == NULL) {
        return;
    }
    const char *v = strmap_get_cstr(params, field);
    if (v == NULL) {
        return;
    }
    Strbuf esc;
    strbuf_init(&esc);
    html_escape_into(&esc, sv_from_cstr(v));
    http_response_add_body_cstr(res, "value=\"");
    http_response_add_body(res, (String_View){esc.items, esc.count});
    http_response_add_body_cstr(res, "\"");
    strbuf_free(&esc);
}