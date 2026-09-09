#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "file.h"
#include "response.h"
#include "strbuf.h"
#include "sv.h"
#include "template.h"
#include "xmem.h"

static int fails = 0;

static void check(const char *what, int cond)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        fails++;
    }
}

// emitted source is a C string, so search within its bytes for the needle
static bool has(const Strbuf *sb, const char *needle)
{
    if (sb->count < strlen(needle)) {
        return false;
    }
    size_t n = strlen(needle);
    for (size_t i = 0; i + n <= sb->count; i++) {
        if (memcmp(sb->items + i, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

int main(void)
{
    Strbuf out;
    Strbuf err;
    strbuf_init(&out);
    strbuf_init(&err);

    // --- unit: the three constructs + name ---
    const char *src1 = "hello <?c int x = 0; x++; ?>world";
    check("basic template compiles",
          cweb_template_to_c(sv_from_cstr(src1), "t1", &out, &err) == 0);
    check("function with the given name",
          has(&out, "void t1(Http_Request *req, Http_Response *res,"));
    check("static text emitted verbatim", has(&out, "\"hello \"") && has(&out, "\"world\""));
    check("raw C block spliced in", has(&out, "int x = 0; x++;"));
    check("content-type default set",
          has(&out, "\"text/html; charset=utf-8\""));
    check("no error text", err.count == 0);
    strbuf_free(&out);
    strbuf_init(&out);
    strbuf_free(&err);
    strbuf_init(&err);

    check("output directive generates a call",
          cweb_template_to_c(sv_from_cstr("x=<?c= num ?>end"), "t2", &out, &err) == 0);
    check("out line emitted", has(&out, "cweb_tpl_out(res,") && has(&out, "num"));
    strbuf_free(&out);
    strbuf_init(&out);
    strbuf_free(&err);
    strbuf_init(&err);

    check("escape directive generates a call",
          cweb_template_to_c(sv_from_cstr("<?h= sv_from_cstr(v) ?>"), "t3", &out, &err) == 0);
    check("escape line emitted", has(&out, "cweb_tpl_escape(res,") && has(&out, "sv_from_cstr(v)"));
    strbuf_free(&out);
    strbuf_init(&out);
    strbuf_free(&err);
    strbuf_init(&err);

    // quotes and backslashes must survive the C literal
    check("quoted text survives",
          cweb_template_to_c(sv_from_cstr("a\"b\\c"), "t4", &out, &err) == 0);
    check("escapes in literal",
          has(&out, "cweb_tpl_out(res, \"a\\\"b\\\\c\");"));
    check("no error for quotes", err.count == 0);
    strbuf_free(&out);
    strbuf_init(&out);
    strbuf_free(&err);
    strbuf_init(&err);

    // --- unit: malformed input ---
    const char *bad = "first line\n<?c boom";
    int rc = cweb_template_to_c(sv_from_cstr(bad), "t5", &out, &err);
    check("unterminated block rejected", rc == -1);
    check("error names the line",
          has(&err, "line 2") && has(&err, "unterminated"));
    strbuf_free(&out);
    strbuf_init(&out);
    strbuf_free(&err);
    strbuf_init(&err);

    check("bad identifier rejected",
          cweb_template_to_c(sv_from_cstr("x"), "1bad", &out, &err) == -1);
    strbuf_free(&out);
    strbuf_init(&out);
    strbuf_free(&err);
    strbuf_init(&err);

    // --- unit: the layout capture runtime ---
    // plain writes go straight into the response...
    Strbuf body;
    strbuf_init(&body);
    Http_Response res;
    http_response_init(&res);
    cweb_tpl_add(&res, "page ", 5);
    check("uncaptured write reaches the response",
          res.body.count == 5 && memcmp(res.body.items, "page ", 5) == 0);

    // ...but a capture holds them back for the wrapper template
    cweb_tpl_capture_begin();
    cweb_tpl_add(&res, "one ", 4);
    cweb_tpl_add(&res, "two", 3);
    cweb_tpl_capture_end();
    check("captured writes avoided the response",
          res.body.count == 5 && memcmp(res.body.items, "page ", 5) == 0);
    String_View cap = cweb_tpl_layout_body();
    check("layout body holds the captured markup",
          cap.count == 7 && memcmp(cap.data, "one two", 7) == 0);

    // reusing the capture discards the previous span
    cweb_tpl_capture_begin();
    cweb_tpl_add(&res, "fresh", 5);
    cweb_tpl_capture_end();
    cap = cweb_tpl_layout_body();
    check("next capture replaces the body",
          cap.count == 5 && memcmp(cap.data, "fresh", 5) == 0);

    // partials rendered mid-capture land in the capture too
    cweb_tpl_capture_begin();
    cweb_tpl_add(&res, "under ", 6);
    cweb_tpl_add(&res, "wrap", 4);
    cweb_tpl_capture_end();
    cap = cweb_tpl_layout_body();
    check("whole page flow is captured",
          cap.count == 10 && memcmp(cap.data, "under wrap", 10) == 0);

    // after the span closes, thread writes go to the response again
    cweb_tpl_add(&res, " end", 4);
    check("writes resume after capture",
          res.body.count == 9 && memcmp(res.body.items, "page  end", 9) == 0);
    http_response_free(&res);
    strbuf_free(&body);
    strbuf_free(&out);
    strbuf_init(&out);
    strbuf_free(&err);
    strbuf_init(&err);

    // --- unit: the date and time formatting helpers ---
    http_response_init(&res);
    cweb_tpl_date(&res, 0);
    check("http-date helper renders GMT epoch",
          res.body.count == 29 && strncmp(res.body.items, "Thu, 01 Jan 1970 00:00:00 GMT", 29) == 0);
    http_response_free(&res);

    // a fixed UTC clock makes the local stamps deterministic
    char saved_tz[128];
    const char *tz = getenv("TZ");
    if (tz != NULL) {
        snprintf(saved_tz, sizeof saved_tz, "%s", tz);
    } else {
        saved_tz[0] = '\0';
    }
    setenv("TZ", "UTC0", 1);
    tzset();
    http_response_init(&res);
    cweb_tpl_date_local(&res, 0);
    check("local stamp helper renders epoch",
          res.body.count == 19 && strncmp(res.body.items, "1970-01-01 00:00:00", 19) == 0);
    http_response_free(&res);
    http_response_init(&res);
    cweb_tpl_strftime(&res, "%a %b %Y", 0);
    check("strftime helper formats",
          strncmp(res.body.items, "Thu Jan 1970", sizeof("Thu Jan 1970") - 1) == 0 &&
          res.body.count == sizeof("Thu Jan 1970") - 1);
    http_response_free(&res);
    http_response_init(&res);
    cweb_tpl_now(&res);
    check("now helper yields a local stamp",
          res.body.count == 19 && res.body.items[4] == '-' && res.body.items[7] == '-' &&
          res.body.items[13] == ':' && res.body.items[16] == ':');
    http_response_free(&res);
    if (saved_tz[0] != '\0') {
        setenv("TZ", saved_tz, 1);
    } else {
        unsetenv("TZ");
    }
    tzset();
    strbuf_free(&out);
    strbuf_init(&out);
    strbuf_free(&err);
    strbuf_init(&err);

    // --- integration: compile a whole page and run it through the library ---
    const char *page =
        "<h1>Page</h1>\n"
        "<?c\n"
        "int x = 6 * 7;\n"
        "char numbuf[32];\n"
        "snprintf(numbuf, sizeof numbuf, \"%d\", x);\n"
        "?>\n"
        "you are <?c= numbuf ?>\n"
        "safe <?h= sv_from_cstr(\"<b>&</b>\") ?>\n"
        "<time><?c cweb_tpl_date(res, (time_t)0); ?></time>\n";

    char tpl_dir[64];
    strcpy(tpl_dir, "/tmp/cweb_template_test_XXXXXX");
    if (mkdtemp(tpl_dir) == NULL) {
        fprintf(stderr, "mkdtemp failed\n");
        return 1;
    }
    char tpl_path[512];
    snprintf(tpl_path, sizeof tpl_path, "%s/hello.c.html", tpl_dir);
    FILE *f = fopen(tpl_path, "w");
    check("page file written", f != NULL);
    if (f != NULL) {
        fwrite(page, 1, strlen(page), f);
        fclose(f);
    }

    check("file compile succeeds", cweb_template_compile(tpl_path, &out, &err) == 0);
    check("derived function name", has(&out, "void page_hello("));
    check("block kept its body content", has(&out, "int x = 6 * 7;"));
    check("escape helper emitted for <?h=", has(&out, "static void cweb_tpl_escape"));
    check("date helper call kept in the page", has(&out, "cweb_tpl_date(res, (time_t)0);"));
    check("no errors", err.count == 0);

    // the runner links the generated handler against the real library and the
    // canned main below; its exit code is the test verdict
    char gen_path[512], main_path[512], bin_path[512];
    snprintf(gen_path, sizeof gen_path, "%s/hello_gen.c", tpl_dir);
    snprintf(main_path, sizeof main_path, "%s/hello_main.c", tpl_dir);
    snprintf(bin_path, sizeof bin_path, "%s/hello_bin", tpl_dir);
    strbuf_null_terminate(&out);
    f = fopen(gen_path, "w");
    check("generated C written", f != NULL);
    if (f != NULL) {
        fwrite(out.items, 1, out.count, f);
        fclose(f);
    }

    const char *runner =
        "#include <stdio.h>\n"
        "#include <string.h>\n"
        "\n"
        "#include \"request.h\"\n"
        "#include \"response.h\"\n"
        "#include \"strmap.h\"\n"
        "\n"
        "void page_hello(Http_Request *req, Http_Response *res,\n"
        "                Str_Map *params, void *user_data);\n"
        "\n"
        "int main(void)\n"
        "{\n"
        "    Http_Request req;\n"
        "    memset(&req, 0, sizeof req);\n"
        "    Http_Response res;\n"
        "    http_response_init(&res);\n"
        "    page_hello(&req, &res, NULL, NULL);\n"
        "    const char *want = \"<h1>Page</h1>\\n\\nyou are 42\\n\"\n"
        "                      \"safe &lt;b&gt;&amp;&lt;/b&gt;\\n\"\n"
        "                      \"<time>Thu, 01 Jan 1970 00:00:00 GMT</time>\\n\";\n"
        "    int ok = res.body.count == strlen(want) &&\n"
        "             memcmp(res.body.items, want, res.body.count) == 0;\n"
        "    if (ok) {\n"
        "        printf(\"runner ok\\n\");\n"
        "    } else {\n"
        "        fprintf(stderr, \"runner: got %zu bytes\\n\", res.body.count);\n"
        "    }\n"
        "    http_response_free(&res);\n"
        "    return ok ? 0 : 1;\n"
        "}\n";
    f = fopen(main_path, "w");
    check("runner written", f != NULL);
    if (f != NULL) {
        fwrite(runner, 1, strlen(runner), f);
        fclose(f);
    }

    char cmd[4096];
    char warn_path[512];
    snprintf(warn_path, sizeof warn_path, "%s/template_warnings.txt", tpl_dir);
    snprintf(cmd, sizeof cmd,
             "cc -std=c11 -Wall -Wextra -Iinclude %s %s build/libcweb.a "
             "-o %s 2>%s",
             gen_path, main_path, bin_path, warn_path);
    check("generated code compiles clean", system(cmd) == 0);

    // a warning in the generated output is a framework bug, not a page bug
    char *warn = NULL;
    size_t warn_len = 0;
    if (file_read_all(warn_path, &warn, &warn_len) == 0) {
        check("no compiler warnings", warn_len == 0);
        xfree(warn);
    }

    snprintf(cmd, sizeof cmd, "%s > %s/hello_output.txt", bin_path, tpl_dir);
    check("generated handler runs", system(cmd) == 0);

    // the runner's own verdict (it prints "runner ok" and exits 0)
    snprintf(cmd, sizeof cmd, "test -f %s/hello_output.txt && grep -q 'runner ok' %s/hello_output.txt",
             tpl_dir, tpl_dir);
    check("rendered page matches", system(cmd) == 0);

    strbuf_free(&out);
    strbuf_free(&err);

    unlink(tpl_path);
    unlink(gen_path);
    unlink(main_path);
    unlink(bin_path);
    unlink(warn_path);
    char out_path[512];
    snprintf(out_path, sizeof out_path, "%s/hello_output.txt", tpl_dir);
    unlink(out_path);
    rmdir(tpl_dir);

    if (fails == 0) {
        printf("template ok\n");
        return 0;
    }
    fprintf(stderr, "%d FAILURES\n", fails);
    return 1;
}