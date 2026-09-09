#define _POSIX_C_SOURCE 200809L

#include "template.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "date.h"
#include "file.h"
#include "response.h"
#include "strbuf.h"
#include "sv.h"
#include "xmem.h"

// one capture buffer per worker thread. while a page renders into it, the
// thread's response body IS that buffer: capture_begin swaps res->body with
// the thread buffer and capture_end swaps it back, so every body write --
// markup, helpers, flash, csrf fields -- is collected instead of going to
// the response, and the wrapping layout reads the bytes back. the buffer
// outlives each capture so the layout can read the bytes after end() and the
// next begin() reuses its storage.
static _Thread_local Strbuf cap_buf;
static _Thread_local Strbuf page_buf; // the response's real body, parked here while capturing

void cweb_tpl_add(Http_Response *res, const void *data, size_t len)
{
    http_response_add_body(res, (String_View){(const char *)data, len});
}

void cweb_tpl_capture_begin(Http_Response *res)
{
    if (cap_buf.items == NULL) {
        strbuf_init(&cap_buf);
    }
    if (page_buf.items == NULL) {
        strbuf_init(&page_buf);
    }
    page_buf = res->body;   // park the request's body buffer safely
    res->body = cap_buf;    // the capture buffer is the active body now
    res->body.count = 0;    // start a fresh capture
}

void cweb_tpl_capture_end(Http_Response *res)
{
    cap_buf = res->body;    // harvest the captured bytes
    res->body = page_buf;   // restore the response's real body
}

String_View cweb_tpl_layout_body(void)
{
    return (String_View){cap_buf.items, cap_buf.count};
}

void cweb_tpl_layout_emit(Http_Response *res)
{
    cweb_tpl_add(res, cap_buf.items, cap_buf.count);
}

// broken-down local time, portable across the windows/unix localtime split
static void tpl_localtime(time_t t, struct tm *tm)
{
#ifdef _WIN32
    localtime_s(tm, &t);
#else
    localtime_r(&t, tm);
#endif
}

void cweb_tpl_date(Http_Response *res, time_t t)
{
    char buf[32] = "";
    http_date_rfc7231(t, buf, sizeof buf);
    cweb_tpl_add(res, buf, strlen(buf));
}

void cweb_tpl_date_local(Http_Response *res, time_t t)
{
    struct tm tm;
    tpl_localtime(t, &tm);
    char buf[32];
    buf[0] = '\0';
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
    cweb_tpl_add(res, buf, strlen(buf));
}

void cweb_tpl_strftime(Http_Response *res, const char *fmt, time_t t)
{
    struct tm tm;
    tpl_localtime(t, &tm);
    char buf[128];
    buf[0] = '\0';
    strftime(buf, sizeof buf, fmt, &tm);
    cweb_tpl_add(res, buf, strlen(buf));
}

void cweb_tpl_now(Http_Response *res)
{
    cweb_tpl_date_local(res, time(NULL));
}

// a page's static text rides inside cweb_tpl_out() calls, escaped so it
// survives the trip through a C string literal unharmed
static void emit_c_literal(String_View s, Strbuf *out)
{
    char oct[8];
    strbuf_append_char(out, '"');
    for (size_t i = 0; i < s.count; i++) {
        unsigned char c = (unsigned char)s.data[i];
        switch (c) {
        case '"': strbuf_append_cstr(out, "\\\""); break;
        case '\\': strbuf_append_cstr(out, "\\\\"); break;
        case '\n': strbuf_append_cstr(out, "\\n"); break;
        case '\r': strbuf_append_cstr(out, "\\r"); break;
        case '\t': strbuf_append_cstr(out, "\\t"); break;
        default:
            if (c < 0x20 || c == 0x7f) {
                // three-digit octal so a following digit cannot merge into it
                snprintf(oct, sizeof(oct), "\\%03o", c);
                strbuf_append_cstr(out, oct);
            } else {
                strbuf_append_char(out, (char)c);
            }
        }
    }
    strbuf_append_char(out, '"');
}

static size_t find_in(String_View src, size_t from, const char *needle)
{
    size_t n = strlen(needle);
    if (from > src.count) {
        return SIZE_MAX;
    }
    for (size_t i = from; i + n <= src.count; i++) {
        if (memcmp(src.data + i, needle, n) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

static void bump_lines(String_View region, size_t *line)
{
    for (size_t i = 0; i < region.count; i++) {
        if (region.data[i] == '\n') {
            (*line)++;
        }
    }
}

static bool valid_fn_name(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    for (size_t i = 0; name[i] != '\0'; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
              (c >= '0' && c <= '9' && i != 0))) {
            return false;
        }
    }
    return true;
}

int cweb_template_to_c(String_View source, const char *fn_name,
                       Strbuf *out, Strbuf *err)
{
    if (!valid_fn_name(fn_name)) {
        if (err) {
            strbuf_append_cstr(err, "invalid generated function name");
        }
        return -1;
    }

    // the body is parsed into its own buffer first so the runtime helpers are
    // only emitted when a directive actually uses them (an unused static would
    // trip -Wunused-function)
    Strbuf body;
    strbuf_init(&body);
    bool used_out = false;
    bool used_escape = false;
    size_t line = 1;
    size_t pos = 0;
    int rc = 0;

    while (pos < source.count) {
        // a directive opens with either tag; take whichever comes first
        size_t open_c = find_in(source, pos, "<?c");
        size_t open_esc = find_in(source, pos, "<?h=");
        bool is_esc_open = false;
        size_t open;
        if (open_c == SIZE_MAX && open_esc == SIZE_MAX) {
            strbuf_append_cstr(&body, "    cweb_tpl_out(res, ");
            emit_c_literal((String_View){source.data + pos, source.count - pos}, &body);
            strbuf_append_cstr(&body, ");\n");
            used_out = true;
            break;
        }
        if (open_esc != SIZE_MAX && (open_c == SIZE_MAX || open_esc < open_c)) {
            open = open_esc;
            is_esc_open = true;
        } else {
            open = open_c;
        }

        // the text before the tag becomes an append call of its own
        strbuf_append_cstr(&body, "    cweb_tpl_out(res, ");
        emit_c_literal((String_View){source.data + pos, open - pos}, &body);
        strbuf_append_cstr(&body, ");\n");
        used_out = true;
        bump_lines((String_View){source.data + pos, open - pos}, &line);

        size_t p = open + (is_esc_open ? 4 : 3); // past the opener
        bool esc = false;
        bool out = false;
        if (is_esc_open) {
            esc = true;
        } else if (p < source.count && source.data[p] == '=') {
            out = true;
            p++;
        }

        size_t close = find_in(source, p, "?>");
        if (close == SIZE_MAX) {
            char msg[128];
            snprintf(msg, sizeof msg,
                     "template line %zu: unterminated <?c (missing ?>)", line);
            if (err) {
                strbuf_append_cstr(err, msg);
            }
            rc = -1;
            break;
        }

        String_View code = {source.data + p, close - p};
        if (esc) {
            strbuf_append_cstr(&body, "    cweb_tpl_escape(res, ");
            strbuf_append(&body, code.data, code.count);
            strbuf_append_cstr(&body, ");\n");
            used_escape = true;
        } else if (out) {
            strbuf_append_cstr(&body, "    cweb_tpl_out(res, ");
            strbuf_append(&body, code.data, code.count);
            strbuf_append_cstr(&body, ");\n");
            used_out = true;
        } else {
            strbuf_append(&body, code.data, code.count);
            strbuf_append_char(&body, '\n');
        }
        bump_lines((String_View){source.data + open, close + 2 - open}, &line);
        pos = close + 2;
    }

    if (rc == 0) {
        // the template author needs the classics plus the framework's parts;
        // extra includes cost nothing if a page does not use them
        strbuf_append_cstr(out,
            "#include <inttypes.h>\n"
            "#include <stdint.h>\n"
            "#include <stdio.h>\n"
            "#include <stdlib.h>\n"
            "#include <string.h>\n"
            "#include <time.h>\n\n"
            "#include \"escape.h\"\n"
            "#include \"validate.h\"\n"
            "#include \"csrf.h\"\n"
            "#include \"flash.h\"\n"
            "#include \"http.h\"\n"
            "#include \"request.h\"\n"
            "#include \"response.h\"\n"
            "#include \"params.h\"\n"
            "#include \"session.h\"\n"
            "#include \"strmap.h\"\n"
            "#include \"strbuf.h\"\n"
            "#include \"sv.h\"\n"
            "#include \"template.h\"\n"
            "#include \"db.h\"\n"
            "\n"
            "// cweb build emits out/pages.h declaring every page, so a page\n"
            "// can render another as a partial: <?c page_x(req,res,params,user_data); ?>\n"
            "#if __has_include(\"pages.h\")\n"
            "#include \"pages.h\"\n"
            "#endif\n\n");

        if (used_out) {
            strbuf_append_cstr(out,
                "static void cweb_tpl_out(Http_Response *res, const char *s)\n"
                "{\n"
                "    cweb_tpl_add(res, s, strlen(s));\n"
                "}\n\n");
        }
        if (used_escape) {
            strbuf_append_cstr(out,
                "static void cweb_tpl_escape(Http_Response *res, String_View s)\n"
                "{\n"
                "    Strbuf tmp;\n"
                "    strbuf_init(&tmp);\n"
                "    html_escape_into(&tmp, s);\n"
                "    cweb_tpl_add(res, tmp.items, tmp.count);\n"
                "    strbuf_free(&tmp);\n"
                "}\n\n");
        }

        strbuf_append_cstr(out, "void ");
        strbuf_append_cstr(out, fn_name);
        strbuf_append_cstr(out,
            "(Http_Request *req, Http_Response *res,\n"
            "      Str_Map *params, void *user_data)\n"
            "{\n"
            "    (void)req;\n"
            "    (void)params;\n"
            "    (void)user_data;\n"
            "    http_response_set_header(res, \"Content-Type\", \"text/html; charset=utf-8\");\n");
        strbuf_append(out, body.items, body.count);
        strbuf_append_cstr(out, "}\n");
    }

    strbuf_free(&body);
    return rc;
}

void cweb_template_page_name(const char *path, char *out, size_t out_size)
{
    // page_hello from "views/hello.c.html": strip any directory part and the
    // suffix, then slug the stem into an identifier
    const char *base = strrchr(path, '/');
    base = (base == NULL) ? path : base + 1;
    size_t sl = strlen(base);
    if (sl > 7 && strcmp(base + sl - 7, ".c.html") == 0) {
        sl -= 7;
    } else if (sl > 5 && strcmp(base + sl - 5, ".html") == 0) {
        sl -= 5;
    }

    // the page_ prefix keeps the identifier valid even when the stem starts
    // with a digit or is empty
    char stem[96];
    size_t n;
    if (sl == 0) {
        n = 4;
        strcpy(stem, "page");
    } else {
        n = sl > 95 ? 95 : sl;
        memcpy(stem, base, n);
    }
    stem[n] = '\0';
    for (size_t i = 0; stem[i] != '\0'; i++) {
        char c = stem[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
              (c >= '0' && c <= '9'))) {
            stem[i] = '_';
        }
    }

    snprintf(out, out_size, "page_%s", stem);
}

int cweb_template_compile(const char *path, Strbuf *out, Strbuf *err)
{
    char *data;
    size_t len;
    if (file_read_all(path, &data, &len) != 0) {
        if (err) {
            strbuf_append_cstr(err, "cannot read template");
        }
        return -1;
    }

    char name[104];
    cweb_template_page_name(path, name, sizeof name);

    int rc = cweb_template_to_c((String_View){data, len}, name, out, err);
    xfree(data);
    return rc;
}