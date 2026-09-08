#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "file.h"
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

    // --- integration: compile a whole page and run it through the library ---
    const char *page =
        "<h1>Page</h1>\n"
        "<?c\n"
        "int x = 6 * 7;\n"
        "char numbuf[32];\n"
        "snprintf(numbuf, sizeof numbuf, \"%d\", x);\n"
        "?>\n"
        "you are <?c= numbuf ?>\n"
        "safe <?h= sv_from_cstr(\"<b>&</b>\") ?>\n";

    const char *tpl_dir = "/tmp/opencode";
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
        "                      \"safe &lt;b&gt;&amp;&lt;/b&gt;\\n\";\n"
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

    if (fails == 0) {
        printf("template ok\n");
        return 0;
    }
    fprintf(stderr, "%d FAILURES\n", fails);
    return 1;
}