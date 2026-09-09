#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "response.h"
#include "strmap.h"
#include "validate.h"

static int check(const char *what, int cond)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        return 1;
    }
    return 0;
}

static int err_is(Http_Form *f, const char *want)
{
    const char *got = http_form_error(f);
    return got != NULL && strcmp(got, want) == 0;
}

int main(void)
{
    int fails = 0;
    Str_Map p;
    strmap_init(&p);
    Http_Form f;

    // --- required ---
    http_form_init(&f);
    http_form_required(&f, &p, "user", "username");
    fails += check("empty form fails required",
                   !http_form_ok(&f) && err_is(&f, "username is required."));

    strmap_set(&p, sv_from_cstr("user"), sv_from_cstr("  "));
    strmap_set(&p, sv_from_cstr("other"), sv_from_cstr("x"));
    http_form_init(&f);
    http_form_required(&f, &p, "user", "username");
    fails += check("blank value fails required",
                   !http_form_ok(&f) && err_is(&f, "username is required."));

    strmap_set(&p, sv_from_cstr("user"), sv_from_cstr("  bob  "));
    http_form_init(&f);
    http_form_required(&f, &p, "user", "username");
    fails += check("trimmed value passes required", http_form_ok(&f));

    strmap_set(&p, sv_from_cstr("user"), sv_from_cstr("  "));
    http_form_init(&f);
    http_form_required(&f, &p, "user", NULL);
    fails += check("label defaults to the field name",
                   !http_form_ok(&f) && err_is(&f, "user is required."));

    strmap_set(&p, sv_from_cstr("user"), sv_from_cstr("alice"));
    http_form_init(&f);
    http_form_required(&f, &p, "user", "username");
    fails += check("present value passes required", http_form_ok(&f));

    // --- length (UTF-8 aware) ---
    strmap_set(&p, sv_from_cstr("nick"), sv_from_cstr("alice"));
    http_form_init(&f);
    http_form_length(&f, &p, "nick", "nick", 3, 8);
    fails += check("value inside range passes length",
                   http_form_ok(&f) && strcmp(strmap_get_cstr(&p, "nick"), "alice") == 0);

    strmap_set(&p, sv_from_cstr("nick"), sv_from_cstr("ab"));
    http_form_init(&f);
    http_form_length(&f, &p, "nick", "nick", 3, 8);
    fails += check("too-short fails length",
                   !http_form_ok(&f) && err_is(&f, "nick must be between 3 and 8 characters."));

    strmap_set(&p, sv_from_cstr("nick"), sv_from_cstr("abcdefghi"));
    http_form_init(&f);
    http_form_length(&f, &p, "nick", "nick", 3, 8);
    fails += check("too-long fails length",
                   !http_form_ok(&f) && err_is(&f, "nick must be between 3 and 8 characters."));

    // four multibyte characters count as four, not eight bytes
    strmap_set(&p, sv_from_cstr("nick"), sv_from_cstr("\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9"));
    http_form_init(&f);
    http_form_length(&f, &p, "nick", "nick", 3, 8);
    fails += check("utf-8 characters are counted as characters, not bytes",
                   http_form_ok(&f));

    strmap_set(&p, sv_from_cstr("nick"), sv_from_cstr("abc"));
    http_form_init(&f);
    http_form_length(&f, &p, "nick", "nick", 5, 0);
    fails += check("min-only message",
                   !http_form_ok(&f) && err_is(&f, "nick must be at least 5 characters."));

    strmap_set(&p, sv_from_cstr("nick"), sv_from_cstr("abcd"));
    http_form_init(&f);
    http_form_length(&f, &p, "nick", "nick", 0, 3);
    fails += check("max-only message",
                   !http_form_ok(&f) && err_is(&f, "nick must be at most 3 characters."));

    strmap_delete(&p, sv_from_cstr("nick"));
    http_form_init(&f);
    http_form_length(&f, &p, "nick", "nick", 1, 3);
    fails += check("absent value passes length (required catches it)", http_form_ok(&f));

    // --- email ---
    strmap_set(&p, sv_from_cstr("mail"), sv_from_cstr("a@b.co"));
    http_form_init(&f);
    http_form_email(&f, &p, "mail", "email");
    fails += check("simple address passes email", http_form_ok(&f));

    strmap_set(&p, sv_from_cstr("mail"), sv_from_cstr("user.name+tag@example-domain.com"));
    http_form_init(&f);
    http_form_email(&f, &p, "mail", "email");
    fails += check("dotted local part and hyphenated domain pass email",
                   http_form_ok(&f));

    strmap_set(&p, sv_from_cstr("mail"), sv_from_cstr("@b.co"));
    http_form_init(&f);
    http_form_email(&f, &p, "mail", "email");
    fails += check("empty local part fails email",
                   !http_form_ok(&f) && err_is(&f, "email is not a valid e-mail address."));

    strmap_set(&p, sv_from_cstr("mail"), sv_from_cstr("a@b"));
    http_form_init(&f);
    http_form_email(&f, &p, "mail", "email");
    fails += check("undotted domain fails email", !http_form_ok(&f));

    strmap_set(&p, sv_from_cstr("mail"), sv_from_cstr("a@@b.co"));
    http_form_init(&f);
    http_form_email(&f, &p, "mail", "email");
    fails += check("double at-sign fails email", !http_form_ok(&f));

    strmap_set(&p, sv_from_cstr("mail"), sv_from_cstr("a b@c.co"));
    http_form_init(&f);
    http_form_email(&f, &p, "mail", "email");
    fails += check("space in the address fails email", !http_form_ok(&f));

    strmap_set(&p, sv_from_cstr("mail"), sv_from_cstr("a@b..co"));
    http_form_init(&f);
    http_form_email(&f, &p, "mail", "email");
    fails += check("doubled domain dots fail email", !http_form_ok(&f));

    strmap_set(&p, sv_from_cstr("mail"), sv_from_cstr("a@.b.co"));
    http_form_init(&f);
    http_form_email(&f, &p, "mail", "email");
    fails += check("leading domain dot fails email", !http_form_ok(&f));

    strmap_delete(&p, sv_from_cstr("mail"));
    http_form_init(&f);
    http_form_email(&f, &p, "mail", "email");
    fails += check("absent value passes email", http_form_ok(&f));

    // --- int ---
    strmap_set(&p, sv_from_cstr("age"), sv_from_cstr("42"));
    long got = 0;
    http_form_init(&f);
    http_form_int(&f, &p, "age", "age", 1, 150, &got);
    fails += check("parseable value in range passes int",
                   http_form_ok(&f) && got == 42);

    strmap_set(&p, sv_from_cstr("age"), sv_from_cstr(" -7 "));
    http_form_init(&f);
    http_form_int(&f, &p, "age", "age", -10, -1, &got);
    fails += check("negative value scales with the range",
                   http_form_ok(&f) && got == -7);

    strmap_set(&p, sv_from_cstr("age"), sv_from_cstr("200"));
    http_form_init(&f);
    http_form_int(&f, &p, "age", "age", 1, 150, &got);
    fails += check("out-of-range fails int",
                   !http_form_ok(&f) && err_is(&f, "age must be between 1 and 150."));

    strmap_set(&p, sv_from_cstr("age"), sv_from_cstr("x"));
    http_form_init(&f);
    http_form_int(&f, &p, "age", "age", 1, 150, &got);
    fails += check("garbage fails int as not-a-number",
                   !http_form_ok(&f) && err_is(&f, "age must be a whole number."));

    strmap_set(&p, sv_from_cstr("age"), sv_from_cstr("-"));
    http_form_init(&f);
    http_form_int(&f, &p, "age", "age", 0, 150, &got);
    fails += check("bare sign fails int", !http_form_ok(&f));

    strmap_set(&p, sv_from_cstr("age"), sv_from_cstr("12abc"));
    http_form_init(&f);
    http_form_int(&f, &p, "age", "age", 0, 150, &got);
    fails += check("trailing junk fails int", !http_form_ok(&f));

    strmap_set(&p, sv_from_cstr("age"), sv_from_cstr("99999999999999999999"));
    http_form_init(&f);
    http_form_int(&f, &p, "age", "age", 0, 150, &got);
    fails += check("overflowing value fails int", !http_form_ok(&f));

    strmap_delete(&p, sv_from_cstr("age"));
    got = 7;
    http_form_init(&f);
    http_form_int(&f, &p, "age", "age", 0, 150, &got);
    fails += check("absent value passes int and leaves *out alone",
                   http_form_ok(&f) && got == 7);

    // --- first failure wins, later checks are skipped ---
    strmap_delete(&p, sv_from_cstr("user"));
    http_form_init(&f);
    http_form_required(&f, &p, "user", "username");
    if (http_form_ok(&f)) {
        http_form_email(&f, &p, "mail", "email");
    }
    fails += check("required fires before other checks",
                   !http_form_ok(&f) && err_is(&f, "username is required."));

    strmap_set(&p, sv_from_cstr("user"), sv_from_cstr("alice"));
    strmap_set(&p, sv_from_cstr("age"), sv_from_cstr("12"));
    strmap_set(&p, sv_from_cstr("mail"), sv_from_cstr("b@c.co"));
    http_form_init(&f);
    http_form_required(&f, &p, "user", "username");
    if (http_form_ok(&f)) {
        http_form_email(&f, &p, "mail", "email");
    }
    if (http_form_ok(&f)) {
        http_form_int(&f, &p, "age", "age", 13, 150, NULL);
    }
    fails += check("a failed age stops the queue",
                   !http_form_ok(&f) && err_is(&f, "age must be between 13 and 150."));

    strmap_set(&p, sv_from_cstr("age"), sv_from_cstr("25"));
    http_form_init(&f);
    http_form_required(&f, &p, "user", "username");
    http_form_email(&f, &p, "mail", "email");
    http_form_int(&f, &p, "age", "age", 13, 150, &got);
    fails += check("a fully valid form stays ok",
                   http_form_ok(&f) && http_form_error(&f) == NULL && got == 25);

    // --- the re-render echo keeps what the visitor typed, escaped ---
    strmap_set(&p, sv_from_cstr("user"), sv_from_cstr("a&b\"c"));
    Http_Response res;
    http_response_init(&res);
    http_form_value(&res, &p, "user");
    strbuf_null_terminate(&res.body);
    fails += check("echo renders an escaped value attribute",
                   strcmp(res.body.items,
                          "value=\"a&amp;b&quot;c\"") == 0);
    http_response_free(&res);

    http_response_init(&res);
    http_form_value(&res, &p, "nope");
    strbuf_null_terminate(&res.body);
    fails += check("echo of an absent field emits nothing",
                   res.body.count == 0);
    http_response_free(&res);

    strmap_free(&p);

    if (fails == 0) {
        printf("validate ok\n");
    }
    return fails ? 1 : 0;
}