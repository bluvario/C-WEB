#ifndef CWEB_VALIDATE_H
#define CWEB_VALIDATE_H

#include <stddef.h>

#include "response.h"
#include "strmap.h"

// tiny form validation for pages. queue checks on the merged form params with
// http_form_*, then ask http_form_ok(); the FIRST failure's readable message
// is in http_form_error(), and no further checks run once a form has failed,
// so a page shows one problem at a time. missing or empty fields fail
// required and are passed over by the other checks -- call required first
// when a field is mandatory. labels make the messages human; NULL echoes the
// field name.

typedef struct {
    char error[192]; // first failure's message, empty while the form is ok
    int ok;          // stays 1 until a check fails, then 0
} Http_Form;

void http_form_init(Http_Form *f);

// the field must be present with at least one non-space character
void http_form_required(Http_Form *f, Str_Map *params,
                        const char *field, const char *label);

// a present value must be between min and max characters (UTF-8 aware); a max
// of 0 lifts the upper bound, a min of 0 the lower one
void http_form_length(Http_Form *f, Str_Map *params,
                      const char *field, const char *label,
                      size_t min, size_t max);

// a present value must look like a single e-mail address: a non-empty local
// part, exactly one '@', and a dotted domain label with no spaces
void http_form_email(Http_Form *f, Str_Map *params,
                     const char *field, const char *label);

// a present value must parse as a whole number inside [lo, hi]; when it does,
// the parsed value lands in *out (only when out is non-NULL)
void http_form_int(Http_Form *f, Str_Map *params,
                   const char *field, const char *label,
                   long lo, long hi, long *out);

// 1 while every registered check passed, 0 after the first failure
int http_form_ok(const Http_Form *f);

// the first failure's message, or NULL while the form is ok
const char *http_form_error(const Http_Form *f);

// echoes the input attribute preserving what the visitor typed:
// `value="<escaped current value>"` for field, or nothing when absent --
// glue so a failed form re-renders without losing the submission
void http_form_value(Http_Response *res, Str_Map *params, const char *field);

#endif