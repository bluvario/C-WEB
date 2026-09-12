#define _XOPEN_SOURCE 700 // realpath, snprintf("%s") in strict c11 mode

// cweb: the C-WEB command-line driver. turns a directory of .c.html pages
// into a native server binary:
//
//   cweb build views/ out/ [root [static/]]
//                             compile every views/**/*.c.html to C, generate
//                             out/main.c and link out/server against libcweb.a
//   cweb serve [--watch] views/ [port [root [static/]]]
//                             build into a scratch dir and run it; with --watch
//                             stay up, rebuild and restart on any file change
//   cweb new app/             scaffold a fresh project: pages, a 404 page, a
//                             shared partial and a stylesheet, ready to serve
//   cweb version
//
// every route answers GET and POST (the same handler, so pages branch on
// req->method) and every page gets a shared Http_Session_Store as user_data,
// so forms and login flows work out of the box. when a static/ dir is given
// it is mounted at "/*" after the pages, so any path no page claims is served
// from disk with proper MIME types.
//
// a views/404.c.html page is a convention: instead of a normal route it
// becomes the app's not-found page, reached after the pages (and the static
// dir, when mounted, gets first refusal) and answered with a 404 status.
//
// files under views/partials/ compile and are declared in the generated
// pages.h but never routed: they exist for pages to render as fragments with
// <?c page_name(req, res, params, user_data); ?>.
//
// ROOT (default ".") is the project root holding include/ and build/libcweb.a,
// so run the tool from there.

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cweb.h"
#include "strbuf.h"
#include "sv.h"
#include "template.h"
#include "xmem.h"

// ---------------------------------------------------------------------------
// small string list
// ---------------------------------------------------------------------------

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} Str_List;

static void sl_push(Str_List *l, const char *s)
{
    if (l->count == l->capacity) {
        size_t nc = l->capacity ? l->capacity * 2 : 16;
        l->items = xrealloc(l->items, nc * sizeof(*l->items));
        l->capacity = nc;
    }
    l->items[l->count++] = strdup(s);
}

static void sl_init(Str_List *l)
{
    l->items = NULL;
    l->count = 0;
    l->capacity = 0;
}

static void sl_free(Str_List *l)
{
    for (size_t i = 0; i < l->count; i++) {
        free(l->items[i]);
    }
    xfree(l->items);
    sl_init(l);
}

static int sl_cmp(const void *a, const void *b)
{
    const char *const *x = a;
    const char *const *y = b;
    return strcmp(*x, *y);
}

// ---------------------------------------------------------------------------
// views directory scan
// ---------------------------------------------------------------------------

static int has_suffix(const char *s, const char *suffix)
{
    size_t sn = strlen(suffix);
    size_t n = strlen(s);
    return n >= sn && strcmp(s + n - sn, suffix) == 0;
}

// dst = a + "/" + b when it fits; returns 0 or -1 on overflow (avoids the
// -Wformat-truncation noise that snprintf would raise on %s pairs)
static int path_join(char *dst, size_t dsize, const char *a, const char *b)
{
    size_t an = strlen(a);
    size_t bn = strlen(b);
    if (an + 1 + bn + 1 > dsize) {
        return -1;
    }
    memcpy(dst, a, an);
    dst[an] = '/';
    memcpy(dst + an + 1, b, bn + 1);
    return 0;
}

static int is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

// absolute directory holding argv[0], or "" when it was found via PATH (no
// slash in argv[0], so we cannot know where it lives)
static void absolute_tool_dir(const char *argv0, char *out, size_t out_size)
{
    out[0] = '\0';
    if (strchr(argv0, '/') == NULL) {
        return;
    }
    char abs[4096];
    if (realpath(argv0, abs) == NULL) {
        return;
    }
    char *slash = strrchr(abs, '/');
    if (slash == NULL || slash == abs) {
        return;
    }
    *slash = '\0';
    snprintf(out, out_size, "%s", abs);
}

// locates the cweb framework checkout for a build. tried in order:
//   1. the given root itself when it holds include/
//   2. the tool binary's own checkout (the binary normally lives at
//      <checkout>/build/cweb), which is what makes "cweb serve views 8080
//      . static" work from inside an app directory next to the checkout
//   3. the nearest ancestor of the given root that holds include/
// returns 1 with the absolute path in out, 0 when none exists (out
// untouched).
static int framework_root(const char *given, const char *argv0,
                          char *out, size_t out_size)
{
    char cur[4096];
    if (realpath(given, cur) == NULL) {
        // fall back to the literal string so the error message says what
        // the user typed even when the path does not exist
        snprintf(cur, sizeof cur, "%s", given);
    }

    char inc[4096];
    if (path_join(inc, sizeof inc, cur, "include") == 0 && is_dir(inc)) {
        snprintf(out, out_size, "%s", cur);
        return 1;
    }

    // the binary lives at <checkout>/build/cweb, so its parent holds include/
    char tdir[4096];
    absolute_tool_dir(argv0, tdir, sizeof tdir);
    if (tdir[0] != '\0') {
        char *slot = strrchr(tdir, '/');
        if (slot != NULL && slot != tdir) {
            *slot = '\0';
            if (path_join(inc, sizeof inc, tdir, "include") == 0 &&
                is_dir(inc)) {
                snprintf(out, out_size, "%s", tdir);
                return 1;
            }
        }
    }

    // walk the given root's ancestors, so a checkout one breadcrumb up from
    // the app dir is still found without an explicit ROOT
    for (;;) {
        char *split = strrchr(cur, '/');
        if (split == NULL || split == cur) {
            return 0;
        }
        *split = '\0';
        if (path_join(inc, sizeof inc, cur, "include") == 0 && is_dir(inc)) {
            snprintf(out, out_size, "%s", cur);
            return 1;
        }
    }
}

// appends s to sb as a shell single-quoted word, so a directory name cannot
// smuggle metacharacters into the system() invocation that links the server
static void strbuf_append_shell_quoted(Strbuf *sb, const char *s)
{
    strbuf_append_char(sb, '\'');
    for (const char *p = s; *p != '\0'; p++) {
        if (*p == '\'') {
            strbuf_append_cstr(sb, "'\\''");
        } else {
            strbuf_append_char(sb, *p);
        }
    }
    strbuf_append_char(sb, '\'');
}

// walks base + "/" + rel collecting every *.c.html as a path relative to base
static void scan_dir(const char *base, const char *rel, Str_List *out)
{
    char dir_path[4096];
    if (rel[0] == '\0') {
        snprintf(dir_path, sizeof dir_path, "%s", base);
    } else if (path_join(dir_path, sizeof dir_path, base, rel) != 0) {
        return;
    }
    DIR *d = opendir(dir_path);
    if (d == NULL) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *name = e->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }
        char child[4096];
        if (rel[0] == '\0') {
            snprintf(child, sizeof child, "%s", name);
        } else if (path_join(child, sizeof child, rel, name) != 0) {
            continue;
        }
        char full[4096];
        if (path_join(full, sizeof full, base, child) != 0) {
            continue;
        }
        struct stat st;
        if (stat(full, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            scan_dir(base, child, out);
        } else if (S_ISREG(st.st_mode) && has_suffix(child, ".c.html")) {
            sl_push(out, child);
        }
    }
    closedir(d);
}

// ---------------------------------------------------------------------------
// route/name handling
// ---------------------------------------------------------------------------

// "/login", "/sub/world": measured so the caller gets the stripped stem back
static void route_of(const char *rel, char *route, size_t n)
{
    size_t len = strlen(rel);
    if (len >= 7 && strcmp(rel + len - 7, ".c.html") == 0) {
        len -= 7;
    }
    if (len > n - 1) {
        len = n - 1;
    }
    snprintf(route, n, "/%.*s", (int)len, rel);
}

static int ends_with_index(const char *route)
{
    size_t n = strlen(route);
    return n >= 6 && strcmp(route + n - 6, "/index") == 0;
}

// files under a "partials/" subdir compile and are declared like pages, but
// are never routed: they exist for other pages to render as fragments
static int is_partial(const char *rel)
{
    return strncmp(rel, "partials/", 9) == 0;
}

// views/layout.c.html is the one-page convention: compiled like a page and
// declared in pages.h, but not routed -- when present it wraps every page
static int is_layout(const char *rel)
{
    return strcmp(rel, "layout.c.html") == 0;
}

// string content for a C literal, escaped so a filename cannot smuggle
// quotes, backslashes or newlines into the generated C
static void emit_esc(FILE *f, const char *s)
{
    for (const char *p = s; *p != '\0'; p++) {
        switch (*p) {
        case '"': fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f); break;
        default: fputc(*p, f); break;
        }
    }
}

// same but % is doubled, for text that becomes a printf() format string
static void emit_fmt(FILE *f, const char *s)
{
    for (const char *p = s; *p != '\0'; p++) {
        switch (*p) {
        case '"': fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '%': fputs("%%", f); break;
        case '\n': fputs("\\n", f); break;
        default: fputc(*p, f); break;
        }
    }
}

// ---------------------------------------------------------------------------
// code generation
// ---------------------------------------------------------------------------

// plain pages answer their own path; xxx/index.c.html also answers "/" and an
// optional static/ dir is served for anything a page does not claim. a page at
// exactly /404 skips the route table and is wired in as the not-found handler.
// partials/ files stay out of the route table; every handler is declared in
// the generated pages.h, which this file includes
static void emit_main(FILE *f, const Str_List *views, const char *static_root, int has_404, int has_layout)
{
    fputs(
        "/* generated by cweb build; hand edits are overwritten */\n"
        "#include <stdio.h>\n"
        "#include <stdlib.h>\n"
        "#include <string.h>\n"
        "\n"
        "#include \"http.h\"\n"
        "#include \"middleware.h\"\n"
        "#include \"net.h\"\n"
        "#include \"pages.h\"\n"
        "#include \"rate_limit.h\"\n"
        "#include \"response.h\"\n"
        "#include \"router.h\"\n"
        "#include \"security.h\"\n"
        "#include \"server.h\"\n"
        "#include \"session.h\"\n"
        "#include \"static.h\"\n"
        "#include \"strmap.h\"\n"
        "#include \"template.h\"\n"
        "#include \"db.h\"\n"
        "#include \"gzip.h\"\n"
        "#include \"request_sign.h\"\n"
        "#include \"thread.h\"\n"
        "#include \"cors.h\"\n"
        "#include \"etag.h\"\n"
        "#include \"request_id.h\"\n"
        "#include \"auth.h\"\n"
        "#include \"ip.h\"\n"
        "#include \"client_ip.h\"\n"
        "#include \"log.h\"\n"
        "\n"
        "// one server-side session store for the whole process, handed to every\n"
        "// page as user_data; sessions live for an hour of inactivity\n"
        "static Http_Session_Store cweb_sessions;\n"
        "\n"
        "// the store behind cweb_database(), opened from --db PATH. zeroed until\n"
        "// then, so pages that call the accessor before it is open get NULL\n"
        "static Cweb_Db cweb_database_impl;\n"
        "static int cweb_database_open = 0;\n"
        "Cweb_Db *cweb_database(void)\n"
        "{\n"
        "    return cweb_database_open ? &cweb_database_impl : NULL;\n"
        "}\n"
        "\n"
        "// Cache-Control max-age for the static root, from --static-cache\n"
        "static unsigned long cweb_static_cache = 0;\n"
        "\n"
        "// hardening knobs for http_serve_config(), from --max-body,\n"
        "// --io-timeout and --workers; zero keeps the library defaults\n"
        "static size_t cweb_max_body = 0;\n"
        "static size_t cweb_max_inflated = 0;\n"
        "static unsigned long cweb_io_timeout_ms = 0;\n"
        "static size_t cweb_workers = 0;\n"
        "\n"
        "// directory for spilling oversized request bodies to disk, from\n"
        "// --body-dir; NULL keeps the in-RAM cap (413) behaviour\n"
        "static const char *cweb_body_dir = NULL;\n"
        "\n"
        "// PEM chain cert and private key for HTTPS, from --tls-cert and\n"
        "// --tls-key; either NULL leaves the server plaintext\n"
        "static const char *cweb_tls_cert = NULL;\n"
        "static const char *cweb_tls_key = NULL;\n"
        "\n"
        "// shared secret for HMAC-signed requests, from --signature-secret;\n"
        "// NULL keeps request signing off entirely\n"
        "static const char *cweb_signature_secret = NULL;\n"
        "\n"
        "// Content-Security-Policy overrides from --csp and --csp-report-only;\n"
        "// NULL keeps the middleware's built-in default directive\n"
        "static const char *cweb_csp = NULL;\n"
        "static const char *cweb_csp_report_only = NULL;\n"
        "\n"
        "// cross-origin access from --cors; NULL keeps CORS headers off every\n"
        "// response, \"*\" opens the API to any origin without credentials\n"
        "static const char *cweb_cors_origin = NULL;\n"
        "\n"
        "// --etag stamps strong validators on dynamic responses and answers\n"
        "// If-None-Match with 304; zero keeps the middleware off\n"
        "static int cweb_etag = 0;\n"
        "\n"
        "// --request-id gives every response a unique trace id (and every\n"
        "// handler a req->request_id); --request-id-inherit additionally\n"
        "// echoes an inbound X-Request-Id from a trusted proxy\n"
        "static int cweb_request_id = 0;\n"
        "static int cweb_request_id_inherit = 0;\n"
        "\n"
        "// --basic-auth USER:PASS turns the whole app behind HTTP Basic auth\n"
        "// (401 + WWW-Authenticate for anything without matching credentials);\n"
        "// the pair is split into these buffers while parsing the flag\n"
        "static const char *cweb_basic_auth = NULL;\n"
        "static char cweb_basic_user[256];\n"
        "static char cweb_basic_pass[256];\n"
        "\n"
        "// reverse proxies trusted by --trusted-proxy; requests that arrive\n"
        "// directly from one of these CIDR networks have their X-Forwarded-For\n"
        "// header believed, so req->client_ip and the rate buckets see the real\n"
        "// client instead of the proxy's address\n"
        "static Http_Cidr cweb_trusted[8];\n"
        "static size_t cweb_trusted_count = 0;\n"
        "\n"
        "// unix socket listener from --socket PATH; NULL keeps the TCP listener\n"
        "// as the only way in (a reverse proxy can talk to the app over the\n"
        "// socket file, no IP stack involved)\n"
        "static const char *cweb_socket_path = NULL;\n"
        "\n"
        "// a second http_serve_config() runs in its own thread so the app can\n"
        "// listen on the TCP port and the unix socket at the same time; both\n"
        "// stop together when the shutdown signal arrives\n"
        "typedef struct {\n"
        "    Socket_Handle listener; // the unix listener this thread serves\n"
        "    Socket_Handle peer;     // the TCP listener, shut down on stop to wake it\n"
        "    Http_Handler_Fn handler;\n"
        "    void *user_data;\n"
        "    Http_Server_Config cfg;\n"
        "} Cweb_Listener_Job;\n"
        "\n"
        "static void cweb_unix_listener(void *arg)\n"
        "{\n"
        "    Cweb_Listener_Job *job = arg;\n"
        "    http_serve_config(job->listener, job->handler, job->user_data, &job->cfg);\n"
        "    // only one thread handles the shutdown signal, so the peer accept\n"
        "    // loop may still be asleep; shutdown() wakes it (close() would not\n"
        "    // reliably), and the loop then sees the shared g_shutdown flag\n"
        "    net_shutdown(job->peer);\n"
        "}\n"
        "\n"
        "// per-route timeout overrides from --route-timeout PATH MS\n"
        "#define CWEB_MAX_ROUTE_TIMEOUTS 32\n"
        "static const char *cweb_route_timeout_paths[CWEB_MAX_ROUTE_TIMEOUTS];\n"
        "static unsigned long cweb_route_timeout_ms[CWEB_MAX_ROUTE_TIMEOUTS];\n"
        "static int cweb_route_timeout_count = 0;\n"
        "\n"
        "// parses \"8192\", \"8k\", \"8K\", \"2m\", \"1g\" into bytes for\n"
        "// --max-body; 0 on an empty or non-numeric argument\n"
        "static size_t cweb_size_arg(const char *s)\n"
        "{\n"
        "    char *end = NULL;\n"
        "    size_t v = strtoul(s, &end, 10);\n"
        "    if (end == s) {\n"
        "        return 0;\n"
        "    }\n"
        "    switch (*end) {\n"
        "    case 'k': case 'K': return v * 1024;\n"
        "    case 'm': case 'M': return v * 1024 * 1024;\n"
        "    case 'g': case 'G': return v * 1024 * 1024 * 1024;\n"
        "    default: return v;\n"
        "    }\n"
        "}\n"
        "\n"
        "// the --rate budget's bucket key is identity-aware (see the library's\n"
        "// http_rate_limit_user_key): requests carrying Basic credentials are\n"
        "// billed to the account they name — even while the password is still\n"
        "// being checked, so guessing one account is throttled however many\n"
        "// caller IPs the attacker spreads across — and everything else is\n"
        "// billed to the caller's address\n"
        "static String_View cweb_rate_key(Http_Request *req, void *user_data)\n"
        "{\n"
        "    return http_rate_limit_user_key(req, user_data);\n"
        "}\n"
        "\n"
        "// disk root for files no page claims, empty when none was configured\n"
        "static const char cweb_static_root[] = \"",
        f);
    emit_esc(f, static_root);
    fputs("\";\n\n", f);
    fputs(
        "// every page's handler arrives from the generated pages.h, so any\n"
        "// page can call any other as a partial\n"
        "\n",
        f);

    fputs("static void list_routes(void)\n{\n", f);
    for (size_t i = 0; i < views->count; i++) {
        char name[128], route[512];
        cweb_template_page_name(views->items[i], name, sizeof name);
        route_of(views->items[i], route, sizeof route);
        if (strcmp(route, "/404") == 0 || is_partial(views->items[i]) ||
            is_layout(views->items[i])) {
            continue; // not routes: /404 and the layout become fallback/wrapper
        }
        // one printf() per route: the format string carries the page name and
        // the route verbatim so --routes lists them
        fputs("    printf(\"", f);
        fprintf(f, "%s: ", name);
        emit_fmt(f, route);
        fputs("\\n\");\n", f);
        if (ends_with_index(route)) {
            fputs("    printf(\"", f);
            fprintf(f, "%s: ", name);
            emit_fmt(f, "/");
            fputs("\\n\");\n", f);
        }
    }
    fputs("    if (cweb_static_root[0] != '\\0') "
          "printf(\"static: /* -> %s\\n\", cweb_static_root);\n",
          f);
    fputs("    if (cweb_static_cache > 0) "
          "printf(\"static-cache: %lus\\n\", cweb_static_cache);\n",
          f);
    fputs("    if (cweb_max_body > 0) "
          "printf(\"max-body: %zu bytes\\n\", cweb_max_body);\n",
          f);
    fputs("    if (cweb_max_inflated > 0) "
          "printf(\"max-inflated: %zu bytes\\n\", cweb_max_inflated);\n",
          f);
    fputs("    if (cweb_body_dir != NULL) "
          "printf(\"body-dir: %s\\n\", cweb_body_dir);\n",
          f);
    fputs("    if (cweb_tls_cert != NULL && cweb_tls_key != NULL) "
          "printf(\"tls-cert: %s\\n\", cweb_tls_cert);\n",
          f);
    fputs("    if (cweb_io_timeout_ms > 0) "
          "printf(\"io-timeout: %lums\\n\", cweb_io_timeout_ms);\n",
          f);
    fputs("    if (cweb_workers > 0) "
          "printf(\"workers: %zu\\n\", cweb_workers);\n",
          f);
    fputs("    if (cweb_signature_secret != NULL) "
          "printf(\"signature: on\\n\");\n",
          f);
    fputs("    if (cweb_csp != NULL) "
          "printf(\"csp: %s\\n\", cweb_csp);\n",
          f);
    fputs("    if (cweb_csp_report_only != NULL) "
          "printf(\"csp-report-only: %s\\n\", cweb_csp_report_only);\n",
          f);
    if (has_404) {
        fputs("    printf(\"404: * (fallback)\\n\");\n", f);
    }
    fputs("}\n\n", f);

    if (has_404) {
        fputs(
            "// last resort: let the static root claim first, and when that also\n"
            "// misses, drop its plain-text answer and render the app's 404 page.\n"
            "// a file the static root served is a raw asset, not a page: flag it\n"
            "// no_layout so a layout wrapper hands the bytes through untouched.\n"
            "static void cweb_fallback(Http_Request *req, Http_Response *res,\n"
            "                          Str_Map *params, void *user_data)\n"
            "{\n"
            "    if (cweb_static_root[0] != '\\0') {\n"
            "        http_serve_static(req, res, (void *)cweb_static_root);\n"
            "        if (res->status != HTTP_404_NOT_FOUND) {\n"
            "            res->no_layout = 1;\n"
            "            return;\n"
            "        }\n"
            "        res->status = HTTP_404_NOT_FOUND;\n"
            "        res->headers.count = 0;\n"
            "        res->body.count = 0;\n"
            "    } else {\n"
            "        http_response_set_status(res, HTTP_404_NOT_FOUND);\n"
            "    }\n"
            "    page_404(req, res, params, user_data);\n"
            "}\n\n",
            f);
    }

    if (has_layout) {
        // each routed page renders into a capture that views/layout.c.html
        // then embeds, so pages stay content-only and one file shapes the
        // whole site; the wrapper forwards what the router hands it
        for (size_t i = 0; i < views->count; i++) {
            char name[128], route[512];
            cweb_template_page_name(views->items[i], name, sizeof name);
            route_of(views->items[i], route, sizeof route);
            if (strcmp(route, "/404") == 0 || is_partial(views->items[i]) ||
                is_layout(views->items[i])) {
                continue;
            }
            fprintf(f, "static void cweb_wrap_%s(Http_Request *req, Http_Response *res,\n",
                    name + 5);
            fputs("                          Str_Map *params, void *user_data)\n"
                  "{\n"
                  "    cweb_tpl_capture_begin(res);\n", f);
            fprintf(f, "    %s(req, res, params, user_data);\n", name);
            fputs("    cweb_tpl_capture_end(res);\n"
                  // a response that never went through the capture (static
                  // files from the /* mount) is served as-is, not framed; a
                  // blank page is indistinguishable and stays unwrapped;
                  // no_layout lets a handler (e.g. a JSON endpoint) opt out
                  // of the layout while keeping its captured body
                  "    if (cweb_tpl_layout_body().count == 0) {\n"
                  "        return;\n"
                  "    }\n"
                  "    if (res->no_layout) {\n"
                  "        res->body.count = 0;\n"
                  "        http_response_add_body(res, cweb_tpl_layout_body());\n"
                  "        return;\n"
                  "    }\n"
                  "    res->body.count = 0;\n"
                  "    page_layout(req, res, params, user_data);\n"
                  "}\n\n", f);
        }
        if (has_404) {
            fputs(
                "static void cweb_wrap_fallback(Http_Request *req, Http_Response *res,\n"
                "                               Str_Map *params, void *user_data)\n"
                "{\n"
                "    cweb_tpl_capture_begin(res);\n"
                "    cweb_fallback(req, res, params, user_data);\n"
                "    cweb_tpl_capture_end(res);\n"
                "    if (cweb_tpl_layout_body().count == 0) {\n"
                "        return;\n"
                "    }\n"
                "    if (res->no_layout) {\n"
                "        res->body.count = 0;\n"
                "        http_response_add_body(res, cweb_tpl_layout_body());\n"
                "        return;\n"
                "    }\n"
                "    res->body.count = 0;\n"
                "    page_layout(req, res, params, user_data);\n"
                "}\n\n",
                f);
        }
    }

    fputs(
        "int main(int argc, char **argv)\n"
        "{\n"
        "    int port = 8080;\n"
        "    int rate_per_min = 0;\n"
        "    int secure = 0;\n"
        "    int gzip = 0;\n"
        "    const char *db_path = NULL;\n"
        "    const char *log_path = NULL;\n"
        "    for (int i = 1; i < argc; i++) {\n"
        "        if (strcmp(argv[i], \"--port\") == 0 && i + 1 < argc) {\n"
        "            port = atoi(argv[++i]);\n"
        "        } else if (strcmp(argv[i], \"--rate\") == 0 && i + 1 < argc) {\n"
        "            rate_per_min = atoi(argv[++i]);\n"
        "        } else if (strcmp(argv[i], \"--secure\") == 0) {\n"
        "            secure = 1;\n"
        "        } else if (strcmp(argv[i], \"--gzip\") == 0) {\n"
        "            gzip = 1;\n"
        "        } else if (strcmp(argv[i], \"--db\") == 0 && i + 1 < argc) {\n"
        "            db_path = argv[++i];\n"
        "        } else if (strcmp(argv[i], \"--log\") == 0 && i + 1 < argc) {\n"
        "            log_path = argv[++i];\n"
        "        } else if (strcmp(argv[i], \"--static-cache\") == 0 && i + 1 < argc) {\n"
        "            cweb_static_cache = strtoul(argv[++i], NULL, 10);\n"
"        } else if (strcmp(argv[i], \"--max-body\") == 0 && i + 1 < argc) {\n"
            "            cweb_max_body = cweb_size_arg(argv[++i]);\n"
            "        } else if (strcmp(argv[i], \"--max-inflated\") == 0 && i + 1 < argc) {\n"
            "            cweb_max_inflated = cweb_size_arg(argv[++i]);\n"
            "        } else if (strcmp(argv[i], \"--body-dir\") == 0 && i + 1 < argc) {\n"
            "            cweb_body_dir = argv[++i];\n"
            "        } else if (strcmp(argv[i], \"--tls-cert\") == 0 && i + 1 < argc) {\n"
            "            cweb_tls_cert = argv[++i];\n"
            "        } else if (strcmp(argv[i], \"--tls-key\") == 0 && i + 1 < argc) {\n"
            "            cweb_tls_key = argv[++i];\n"
            "        } else if (strcmp(argv[i], \"--io-timeout\") == 0 && i + 1 < argc) {\n"
        "            cweb_io_timeout_ms = strtoul(argv[++i], NULL, 10);\n"
"        } else if (strcmp(argv[i], \"--workers\") == 0 && i + 1 < argc) {\n"
            "            cweb_workers = strtoul(argv[++i], NULL, 10);\n"
            "        } else if (strcmp(argv[i], \"--signature-secret\") == 0 && i + 1 < argc) {\n"
"            cweb_signature_secret = argv[++i];\n"
             "        } else if (strcmp(argv[i], \"--csp\") == 0 && i + 1 < argc) {\n"
             "            cweb_csp = argv[++i];\n"
             "        } else if (strcmp(argv[i], \"--csp-report-only\") == 0 && i + 1 < argc) {\n"
             "            cweb_csp_report_only = argv[++i];\n"
             "        } else if (strcmp(argv[i], \"--route-timeout\") == 0 && i + 2 < argc) {\n"
             "            if (cweb_route_timeout_count < CWEB_MAX_ROUTE_TIMEOUTS) {\n"
             "                cweb_route_timeout_paths[cweb_route_timeout_count] = argv[++i];\n"
             "                cweb_route_timeout_ms[cweb_route_timeout_count] = strtoul(argv[++i], NULL, 10);\n"
             "                cweb_route_timeout_count++;\n"
             "            }\n"
"        } else if (strcmp(argv[i], \"--socket\") == 0 && i + 1 < argc) {\n"
        "            cweb_socket_path = argv[++i];\n"
        "        } else if (strcmp(argv[i], \"--trusted-proxy\") == 0 && i + 1 < argc) {\n"
        "            // comma-separated list of the reverse proxies allowed to\n"
        "            // stamp X-Forwarded-For; only a request that arrived\n"
        "            // directly from one of these has its header believed\n"
        "            const char *spec = argv[++i];\n"
        "            const char *p = spec;\n"
        "            for (;;) {\n"
        "                const char *comma = strchr(p, ',');\n"
        "                size_t len = comma != NULL ? (size_t)(comma - p)\n"
        "                                            : strlen(p);\n"
        "                if (len == 0 || len >= 64) {\n"
        "                    fprintf(stderr, \"cweb: invalid --trusted-proxy %s\\n\",\n"
        "                            spec);\n"
        "                    return 2;\n"
        "                }\n"
        "                char buf[64];\n"
        "                memcpy(buf, p, len);\n"
        "                buf[len] = '\\0';\n"
        "                if (cweb_trusted_count >= 8 ||\n"
        "                    http_cidr_parse(&cweb_trusted[cweb_trusted_count],\n"
        "                                    buf) != 0) {\n"
        "                    fprintf(stderr, \"cweb: invalid --trusted-proxy %s\\n\",\n"
        "                            spec);\n"
        "                    return 2;\n"
        "                }\n"
        "                cweb_trusted_count++;\n"
        "                if (comma == NULL) {\n"
        "                    break;\n"
        "                }\n"
        "                p = comma + 1;\n"
        "            }\n"
        "        } else if (strcmp(argv[i], \"--cors\") == 0 && i + 1 < argc) {\n"
        "            cweb_cors_origin = argv[++i];\n"
        "        } else if (strcmp(argv[i], \"--etag\") == 0) {\n"
        "            cweb_etag = 1;\n"
        "        } else if (strcmp(argv[i], \"--request-id\") == 0) {\n"
        "            cweb_request_id = 1;\n"
        "        } else if (strcmp(argv[i], \"--request-id-inherit\") == 0) {\n"
        "            // honoring an inbound id implies the middleware is on\n"
        "            cweb_request_id = 1;\n"
        "            cweb_request_id_inherit = 1;\n"
        "        } else if (strcmp(argv[i], \"--basic-auth\") == 0 && i + 1 < argc) {\n"
        "            // split USER:PASS at its first ':', keep the whole\n"
        "            // password (which may itself contain colons)\n"
        "            const char *spec = argv[++i];\n"
        "            const char *colon = strchr(spec, ':');\n"
        "            if (colon != NULL) {\n"
        "                size_t n = (size_t)(colon - spec);\n"
        "                if (n >= sizeof cweb_basic_user) {\n"
        "                    n = sizeof cweb_basic_user - 1;\n"
        "                }\n"
        "                memcpy(cweb_basic_user, spec, n);\n"
        "                cweb_basic_user[n] = '\\0';\n"
        "                snprintf(cweb_basic_pass, sizeof cweb_basic_pass,\n"
        "                         \"%s\", colon + 1);\n"
        "            } else {\n"
        "                snprintf(cweb_basic_user, sizeof cweb_basic_user,\n"
        "                         \"%s\", spec);\n"
        "                cweb_basic_pass[0] = '\\0';\n"
        "            }\n"
        "            cweb_basic_auth = cweb_basic_user;\n"
        "        } else if (strcmp(argv[i], \"--routes\") == 0) {\n"
        "            list_routes();\n"
        "            return 0;\n"
        "        } else {\n"
        "            fprintf(stderr, \"cweb: unknown option %s\\n\", argv[i]);\n"
        "            return 2;\n"
        "        }\n"
        "    }\n"
        "    if (db_path != NULL) {\n"
        "        if (cweb_db_open(&cweb_database_impl, db_path) != 0) {\n"
        "            fprintf(stderr, \"cweb: cannot open db %s\\n\", db_path);\n"
        "            return 1;\n"
        "        }\n"
        "        cweb_database_open = 1;\n"
        "    }\n"
        "    if (net_init() != 0) {\n"
        "        fprintf(stderr, \"cweb: net_init failed\\n\");\n"
        "        return 1;\n"
        "    }\n"
        "    Http_Router r;\n"
        "    router_init(&r);\n"
        "    http_session_store_init(&cweb_sessions, 3600);\n"
        "    // with --db the session store persists to it, so logins survive\n"
        "    // a restart as long as the same database file is passed again\n"
        "    if (cweb_database_open) {\n"
        "        http_session_store_set_db(&cweb_sessions, &cweb_database_impl);\n"
        "    }\n",
        f);

    for (size_t i = 0; i < views->count; i++) {
        char name[128], route[512], handler[160];
        cweb_template_page_name(views->items[i], name, sizeof name);
        route_of(views->items[i], route, sizeof route);
        if (strcmp(route, "/404") == 0 || is_partial(views->items[i]) ||
            is_layout(views->items[i])) {
            continue; // partials are fragments; /404 is the fallback; layout wraps
        }
        // a layout makes every route run inside a capture wrapper so the
        // page body can be embedded by views/layout.c.html; page names are
        // always "page_#name" so the wrapper borrows the stem
        if (has_layout) {
            snprintf(handler, sizeof handler, "cweb_wrap_%s", name + 5);
        } else {
            snprintf(handler, sizeof handler, "%s", name);
        }
        fprintf(f, "    router_add(&r, HTTP_GET, ");
        fputc('"', f);
        emit_esc(f, route);
        fputc('"', f);
        fprintf(f, ", %s, &cweb_sessions);\n", handler);
        fprintf(f, "    router_add(&r, HTTP_POST, ");
        fputc('"', f);
        emit_esc(f, route);
        fputc('"', f);
        fprintf(f, ", %s, &cweb_sessions);\n", handler);
        if (ends_with_index(route)) {
            fprintf(f, "    router_add(&r, HTTP_GET, \"/\", %s, &cweb_sessions);\n", handler);
            fprintf(f, "    router_add(&r, HTTP_POST, \"/\", %s, &cweb_sessions);\n", handler);
        }
    }

    if (has_404) {
        if (has_layout) {
            fputs(
                "    router_add(&r, HTTP_GET, \"*\", cweb_wrap_fallback, &cweb_sessions);\n"
                "    router_add(&r, HTTP_POST, \"*\", cweb_wrap_fallback, &cweb_sessions);\n",
                f);
        } else {
            fputs(
                "    router_add(&r, HTTP_GET, \"*\", cweb_fallback, &cweb_sessions);\n"
                "    router_add(&r, HTTP_POST, \"*\", cweb_fallback, &cweb_sessions);\n",
                f);
        }
    } else {
        fputs(
            "    if (cweb_static_root[0] != '\\0') {\n"
            "        http_static_mount(&r, \"\", cweb_static_root);\n"
            "    }\n",
            f);
    }
    fputs(
        "    if (cweb_static_cache > 0) {\n"
        "        http_static_set_cache(cweb_static_cache);\n"
        "    }\n"
        "    // apply per-route timeout overrides\n"
        "    for (int i = 0; i < cweb_route_timeout_count; i++) {\n"
        "        if (router_set_timeout(&r, HTTP_GET, cweb_route_timeout_paths[i], cweb_route_timeout_ms[i]) == 0)\n"
        "            router_set_timeout(&r, HTTP_POST, cweb_route_timeout_paths[i], cweb_route_timeout_ms[i]);\n"
        "    }\n",
        f);
    fputc('\n', f);

fputs(
        "    printf(\"cweb server on http://0.0.0.0:%d\\n\", port);\n"
        "    Socket_Handle listener = net_listen(port);\n"
        "    if (listener < 0) {\n"
        "        fprintf(stderr, \"cweb: listen %d: %s\\n\", port,\n"
        "                net_error_string());\n"
        "        return 1;\n"
        "    }\n"
        "    Socket_Handle unix_listener = -1;\n"
        "    if (cweb_socket_path != NULL) {\n"
        "        // --socket is served alongside the TCP port, so a reverse\n"
        "        // proxy can reach the app over a local socket file\n"
        "        unix_listener = net_listen_unix(cweb_socket_path);\n"
        "        if (unix_listener < 0) {\n"
        "            fprintf(stderr, \"cweb: unix listen %s: %s\\n\",\n"
        "                    cweb_socket_path, net_error_string());\n"
        "            net_close(listener);\n"
        "            return 1;\n"
        "        }\n"
        "        printf(\"cweb server on unix:%s\\n\", cweb_socket_path);\n"
        "    }\n"
        "    FILE *clf_file = NULL;\n"
        "    if (log_path != NULL) {\n"
        "        // --log appends Common Log Format access lines for every\n"
        "        // completed request; opened here so a failure is loud\n"
        "        clf_file = fopen(log_path, \"a\");\n"
        "        if (clf_file == NULL) {\n"
        "            fprintf(stderr, \"cweb: cannot open access log %s\\n\", log_path);\n"
        "            return 1;\n"
        "        }\n"
        "        log_set_clf(clf_file);\n"
        "    }\n"
        "    Http_Server_Config cfg = {0};\n"
        "    cfg.max_body = cweb_max_body;\n"
        "    cfg.max_inflated = cweb_max_inflated;\n"
        "    cfg.body_dir = cweb_body_dir;\n"
        "    cfg.tls_cert = cweb_tls_cert;\n"
        "    cfg.tls_key = cweb_tls_key;\n"
        "    cfg.io_timeout_ms = cweb_io_timeout_ms;\n"
        "    cfg.workers = cweb_workers;\n"
        "\n"
        "    // the dispatch and its state are shared by every listener\n"
        "    Http_Handler_Fn cweb_dispatch;\n"
        "    void *cweb_user;\n"
        "    Http_RateLimiter limiter;\n"
        "    int cweb_middleware = secure || gzip || rate_per_min > 0 ||\n"
        "                         cweb_signature_secret != NULL ||\n"
        "                         cweb_basic_auth != NULL ||\n"
        "                         cweb_cors_origin != NULL ||\n"
        "                         cweb_etag || cweb_request_id ||\n"
        "                         cweb_csp != NULL || cweb_csp_report_only != NULL;\n"
        "    void *cweb_chain_data = NULL;\n"
        "    if (cweb_middleware) {\n"
        "        // hardening headers, (optionally) a per-client request\n"
        "        // budget, gzip wrapping, a shared-secret signature, and a\n"
        "        // Content-Security-Policy run around every response\n"
        "        if (rate_per_min > 0) {\n"
        "            // N requests a minute per bucket: the bucket starts full\n"
        "            // so a momentary burst of N passes, beyond that the caller\n"
        "            // is answered with 429 until the bucket refills. buckets\n"
        "            // belong to who the request claims to be (see the\n"
        "            // identity-aware key above), falling back to the address\n"
        "            http_rate_limiter_init(&limiter, (double)rate_per_min / 60.0,\n"
        "                                   (double)rate_per_min);\n"
        "            limiter.key_fn = cweb_rate_key;\n"
        "        }\n"
        "        Http_Signature_Options sig_opts = {0};\n"
        "        if (cweb_signature_secret != NULL) {\n"
        "            // --signature-secret makes every request prove it came\n"
        "            // from a proxy holding the same secret, 403 otherwise\n"
        "            sig_opts.secret = cweb_signature_secret;\n"
        "        }\n"
        "        // --csp overrides the middleware's default directive;\n"
        "        // --csp-report-only adds a monitoring-only policy on top\n"
        "        Http_Security_Options sec_opts = {0};\n"
        "        if (cweb_csp != NULL || cweb_csp_report_only != NULL) {\n"
        "            sec_opts.csp = cweb_csp;\n"
        "            sec_opts.csp_report_only = cweb_csp_report_only;\n"
        "        }\n"
        "        Http_Cors_Options cors_opts = {0};\n"
        "        if (cweb_cors_origin != NULL) {\n"
        "            cors_opts.origin = cweb_cors_origin;\n"
        "        }\n"
        "        Http_Middleware_Chain chain;\n"
        "        http_middleware_init(&chain);\n"
        "        Http_ClientIp_Options ip_opts = {cweb_trusted, cweb_trusted_count};\n"
        "        if (cweb_trusted_count > 0) {\n"
        "            // resolves req->client_ip from X-Forwarded-For for requests\n"
        "            // that arrived straight from a trusted proxy. outermost, so\n"
        "            // access logs and the per-address rate buckets below name\n"
        "            // the real client, never the proxy\n"
        "            http_middleware_add(&chain, http_client_ip_middleware,\n"
        "                                &ip_opts);\n"
        "        }\n"
        "        if (cweb_request_id) {\n"
        "            // --request-id stamps every response (and every\n"
        "            // req->request_id) with a unique trace id; outermost,\n"
        "            // so even a short-circuited chain still carries it on\n"
        "            Http_RequestId_Opts rid_opts = {0};\n"
        "            rid_opts.honor_incoming = cweb_request_id_inherit;\n"
        "            http_middleware_add(&chain, http_request_id_middleware,\n"
        "                                &rid_opts);\n"
        "        }\n"
        "        if (secure || cweb_csp != NULL || cweb_csp_report_only != NULL) {\n"
        "            http_middleware_add(&chain, http_security_middleware,\n"
        "                                (cweb_csp != NULL || cweb_csp_report_only != NULL)\n"
        "                                    ? &sec_opts : NULL);\n"
        "        }\n"
        "        if (cweb_signature_secret != NULL) {\n"
        "            http_middleware_add(&chain, http_signature_middleware,\n"
        "                                &sig_opts);\n"
        "        }\n"
        "        if (rate_per_min > 0) {\n"
        "            // the budget sits outside the Basic-auth guard on purpose:\n"
        "            // the identity-aware key bills authenticated requests to\n"
        "            // the account they name, so a brute-forced password or a\n"
        "            // burst from one caller gets 429 before the guard can even\n"
        "            // answer 401, instead of rerolling the bucket on every try\n"
        "            http_middleware_add(&chain, http_rate_limit_middleware,\n"
        "                                &limiter);\n"
        "        }\n"
        "        Http_BasicAuth_Options basic_opts = {0};\n"
        "        if (cweb_basic_auth != NULL) {\n"
        "            // --basic-auth USER:PASS locks the whole app behind HTTP\n"
        "            // Basic auth; the pair was split into these buffers while\n"
        "            // parsing the flag, and anything without matching\n"
        "            // credentials is answered 401 + WWW-Authenticate\n"
        "            basic_opts.user = cweb_basic_user;\n"
        "            basic_opts.pass = cweb_basic_pass;\n"
        "            http_middleware_add(&chain, http_basic_auth_middleware,\n"
        "                                &basic_opts);\n"
        "        }\n"
        "        if (gzip) {\n"
        "            // --gzip compresses compressible bodies on the way out\n"
        "            http_middleware_add(&chain, http_gzip_middleware, NULL);\n"
        "        }\n"
        "        if (cweb_etag) {\n"
        "            // --etag hashes the uncompressed body for a stable\n"
        "            // validator regardless of Content-Encoding; gzip runs\n"
        "            // outermost so it still compresses the final bytes\n"
        "            http_middleware_add(&chain, http_etag_middleware, NULL);\n"
        "        }\n"
        "        if (cweb_cors_origin != NULL) {\n"
        "            // --cors answers browser preflights and stamps\n"
        "            // Access-Control-Allow-Origin on every matching request\n"
        "            http_middleware_add(&chain, http_cors_middleware,\n"
        "                                &cors_opts);\n"
        "        }\n"
        "        cweb_dispatch = http_middleware_build(&chain, router_dispatch,\n"
        "                                              &r, &cweb_chain_data);\n"
        "        http_middleware_free(&chain);\n"
        "        cweb_user = cweb_chain_data;\n"
        "    } else {\n"
        "        cweb_dispatch = router_dispatch;\n"
        "        cweb_user = &r;\n"
        "    }\n"
        "\n"
        "    int rc;\n"
        "    if (unix_listener >= 0) {\n"
        "        // the unix listener accepts in its own thread while the main\n"
        "        // thread keeps the TCP accept loop; both stop on the same\n"
        "        // shutdown signal and drain their in-flight requests together\n"
        "        Cweb_Listener_Job job = {unix_listener, listener, cweb_dispatch,\n"
        "                                cweb_user, cfg};\n"
        "        Thread unix_thread;\n"
        "        if (thread_init(&unix_thread, cweb_unix_listener, &job) != 0) {\n"
        "            fprintf(stderr, \"cweb: cannot start the unix listener\\n\");\n"
        "            net_close(listener);\n"
        "            net_close(unix_listener);\n"
        "            net_unix_unlink(cweb_socket_path);\n"
        "            return 1;\n"
        "        }\n"
        "        rc = http_serve_config(listener, cweb_dispatch, cweb_user, &cfg);\n"
        "        net_shutdown(unix_listener); // wake the peer loop if it is asleep\n"
        "        thread_join(&unix_thread);\n"
        "        net_close(unix_listener);\n"
        "        net_close(listener);\n"
        "        net_unix_unlink(cweb_socket_path);\n"
        "    } else {\n"
        "        rc = http_serve_config(listener, cweb_dispatch, cweb_user, &cfg);\n"
        "    }\n"
        "    if (cweb_middleware) {\n"
        "        http_middleware_data_free(cweb_chain_data);\n"
        "        if (rate_per_min > 0) {\n"
        "            http_rate_limiter_free(&limiter);\n"
        "        }\n"
        "    }\n"
        "    http_session_store_dump(&cweb_sessions);\n"
        "    http_session_store_free(&cweb_sessions);\n"
        "    if (clf_file != NULL) {\n"
        "        log_set_clf(NULL);\n"
        "        fclose(clf_file);\n"
        "    }\n"
        "    router_free(&r);\n"
        "    net_cleanup();\n"
        "    if (cweb_database_open) {\n"
        "        cweb_db_close(&cweb_database_impl);\n"
        "    }\n"
        "    return rc;\n"
        "}\n",
        f);
}

// ---------------------------------------------------------------------------
// build
// ---------------------------------------------------------------------------

static int die(const char *msg)
{
    fprintf(stderr, "cweb: %s\n", msg);
    return 1;
}

// like mkdir -p: creates every missing directory on the way
static void mkdir_p(const char *path)
{
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static int cmd_build(int argc, char **argv)
{
    if (argc < 4) {
        return die("build needs VIEWS_DIR and OUT_DIR "
                   "(cweb build views out [root [static]])");
    }
    const char *views_arg = argv[2];
    const char *out_arg = argv[3];
    const char *root_arg = argc >= 5 ? argv[4] : ".";
    // ROOT is only used to lay hands on ../include and ../build/libcweb.a, so
    // a relative "." from inside an app directory still finds the framework
    // checkout one level (or more) up
    char root[4096];
    if (!framework_root(root_arg, argv[0], root, sizeof root)) {
        char msg[512];
        snprintf(msg, sizeof msg,
                 "cannot find the cweb framework: no include/ under %s, next to "
                 "the tool binary, or in any ancestor",
                 root_arg);
        return die(msg);
    }
    const char *static_arg = argc >= 6 ? argv[5] : "";
    if (static_arg[0] != '\0') {
        struct stat st;
        if (stat(static_arg, &st) != 0 || !S_ISDIR(st.st_mode)) {
            return die("static dir does not exist");
        }
    }

    Str_List views;
    sl_init(&views);
    scan_dir(views_arg, "", &views);
    if (views.count == 0) {
        sl_free(&views);
        return die("no *.c.html pages found under the views dir");
    }
    qsort(views.items, views.count, sizeof(*views.items), sl_cmp);

    // resolve paths up front and check for the two ways two pages could fight
    // over one route or one function name
    int has_404 = 0;
    int has_layout = 0;
    char (*names)[128] = xmalloc(views.count * sizeof *names);
    char (*routes)[512] = xmalloc(views.count * sizeof *routes);
    for (size_t i = 0; i < views.count; i++) {
        cweb_template_page_name(views.items[i], names[i], sizeof names[i]);
        route_of(views.items[i], routes[i], sizeof routes[i]);
        if (strcmp(routes[i], "/404") == 0) {
            has_404 = 1;
        }
        if (is_layout(views.items[i])) {
            has_layout = 1;
        }
        if (!ends_with_index(routes[i])) {
            for (size_t j = 0; j < i; j++) {
                if (strcmp(routes[i], routes[j]) == 0) {
                    fprintf(stderr, "cweb: %s and %s map to the same route %s\n",
                            views.items[i], views.items[j], routes[i]);
                    return 1;
                }
            }
        }
        for (size_t j = 0; j < i; j++) {
            if (strcmp(names[i], names[j]) == 0) {
                fprintf(stderr, "cweb: %s and %s collide on handler %s\n",
                        views.items[i], views.items[j], names[i]);
                return 1;
            }
        }
    }

    mkdir_p(out_arg);

    // one generated .c per page, named after its handler for debuggability
    char gen[512];
    for (size_t i = 0; i < views.count; i++) {
        char full[512];
        snprintf(full, sizeof full, "%s/%s", views_arg, views.items[i]);
        Strbuf src, err;
        strbuf_init(&src);
        strbuf_init(&err);
        if (cweb_template_compile(full, &src, &err) != 0) {
            fprintf(stderr, "cweb: %s: %.*s\n", views.items[i],
                    (int)err.count, err.items);
            return 1;
        }
        snprintf(gen, sizeof gen, "%s/%s.c", out_arg, names[i]);
        FILE *f = fopen(gen, "w");
        if (f == NULL) {
            return die("cannot write generated C");
        }
        fwrite(src.items, 1, src.count, f);
        fclose(f);
        strbuf_free(&src);
        strbuf_free(&err);
    }

    // the routed main and then the whole thing linked against the library
    char main_path[512];
    snprintf(main_path, sizeof main_path, "%s/main.c", out_arg);
    FILE *f = fopen(main_path, "w");
    if (f == NULL) {
        return die("cannot write generated main");
    }
    emit_main(f, &views, static_arg, has_404, has_layout);
    fclose(f);

    // pages.h declares every page handler so any page can render another as a
    // partial; the build dir sits on every translation unit's include path
    char vh_path[512];
    snprintf(vh_path, sizeof vh_path, "%s/pages.h", out_arg);
    f = fopen(vh_path, "w");
    if (f == NULL) {
        return die("cannot write generated pages.h");
    }
    fputs(
        "/* generated by cweb build; hand edits are overwritten */\n"
        "#ifndef CWEB_PAGES_H\n"
        "#define CWEB_PAGES_H\n\n"
        "#include \"request.h\"\n"
        "#include \"response.h\"\n"
        "#include \"strmap.h\"\n"
        "#include \"db.h\"\n\n"
        "/* one declaration per page. call another page as a partial from\n"
        " * any view: <?c page_x(req, res, params, user_data); ?> */\n",
        f);
    for (size_t i = 0; i < views.count; i++) {
        fprintf(f, "void %s(Http_Request *req, Http_Response *res,\n"
                   "      Str_Map *params, void *user_data);\n",
                names[i]);
    }
    fputs(
        "\n"
        "/* the store opened from the server's --db PATH, or NULL when the\n"
        " * server ran without one. every page shares the same Cweb_Db, so\n"
        " * <?c Cweb_Db *db = cweb_database(); ?> reads and writes persist\n"
        " * between requests. check for NULL before touching it. */\n"
        "Cweb_Db *cweb_database(void);\n"
        "#endif\n",
        f);
    fclose(f);

    char bin_path[512];
    snprintf(bin_path, sizeof bin_path, "%s/server", out_arg);
    Strbuf cc;
    strbuf_init(&cc);
    strbuf_append_cstr(&cc, "cc -std=c11 -Wall -Wextra -Werror -I");
    strbuf_append_shell_quoted(&cc, root);
    strbuf_append_cstr(&cc, "/include -I");
    strbuf_append_shell_quoted(&cc, out_arg);
    strbuf_append_cstr(&cc, " ");
    strbuf_append_shell_quoted(&cc, main_path);
    for (size_t i = 0; i < views.count; i++) {
        snprintf(gen, sizeof gen, "%s/%s.c", out_arg, names[i]);
        strbuf_append_char(&cc, ' ');
        strbuf_append_shell_quoted(&cc, gen);
    }
    strbuf_append_cstr(&cc, " ");
    strbuf_append_shell_quoted(&cc, root);
    strbuf_append_cstr(&cc, "/build/libcweb.a -lz "
                           "$(pkg-config --libs openssl 2>/dev/null) -o ");
    strbuf_append_shell_quoted(&cc, bin_path);
    strbuf_null_terminate(&cc);
    int rc = system(cc.items);
    strbuf_free(&cc);
    if (rc != 0) {
        return die("cc could not link the server (build tree up to date? run make)");
    }

    printf("built %s (%zu pages)\n", bin_path, views.count);
    sl_free(&views);
    xfree(names);
    xfree(routes);
    return 0;
}

// ---------------------------------------------------------------------------
// new: scaffold a runnable app
// ---------------------------------------------------------------------------

static int write_file(const char *path, const char *data)
{
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        return -1;
    }
    fputs(data, f);
    fclose(f);
    return 0;
}

// true when dir exists and holds anything besides "." and ".."
static int dir_has_entries(const char *path)
{
    DIR *d = opendir(path);
    if (d == NULL) {
        return 0;
    }
    struct dirent *e;
    int has = 0;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) {
            has = 1;
            break;
        }
    }
    closedir(d);
    return has;
}

static int cmd_new(int argc, char **argv)
{
    if (argc < 3) {
        return die("new needs an app dir (cweb new APP_DIR)");
    }
    // refuse only when a non-empty dir occupies the spot; an empty dir is fine
    struct stat st;
    if (stat(argv[2], &st) == 0 && dir_has_entries(argv[2])) {
        return die("app dir exists and is not empty");
    }

    char app[4096], sub[512];
    mkdir_p(argv[2]);                     // creates the app dir
    path_join(sub, sizeof sub, argv[2], "views/partials");
    mkdir_p(sub);                         // creates views and views/partials
    path_join(sub, sizeof sub, argv[2], "static");
    mkdir_p(sub);

    path_join(app, sizeof app, argv[2], "views/layout.c.html");
    if (write_file(app,
        "<!DOCTYPE html>\n"
        "<html lang=\"en\">\n"
        "<head>\n"
        "  <meta charset=\"utf-8\">\n"
        "  <title>cweb app</title>\n"
        "  <link rel=\"stylesheet\" href=\"/style.css\">\n"
        "</head>\n"
        "<body>\n"
        "  <header><h1>cweb app</h1><p>a C-WEB site</p></header>\n"
        "  <main>\n"
        "    <?c cweb_tpl_layout_emit(res); ?>\n"
        "  </main>\n"
        "  <?c page_footer(req, res, params, user_data); ?>\n"
        "</body>\n"
        "</html>\n") != 0) {
        return die("cannot write views/layout.c.html");
    }
    path_join(app, sizeof app, argv[2], "views/index.c.html");
    if (write_file(app,
        "<p>Built by <strong>cweb build</strong>. Edit this page at\n"
        "  <code>views/index.c.html</code>.</p>\n"
        "  <p>Serve it again with <code>cweb serve views 8080 . static</code>.</p>\n") != 0) {
        return die("cannot write views/index.c.html");
    }
    path_join(app, sizeof app, argv[2], "views/404.c.html");
    if (write_file(app,
        "<h1>404 · no such page</h1>\n"
        "  <p>Nothing is served at this address.</p>\n"
        "  <p><a href=\"/\">Back to the start</a></p>\n") != 0) {
        return die("cannot write views/404.c.html");
    }
    path_join(app, sizeof app, argv[2], "views/partials/footer.c.html");
    if (write_file(app,
        "<footer>Powered by C-WEB</footer>\n") != 0) {
        return die("cannot write views/partials/footer.c.html");
    }
    path_join(app, sizeof app, argv[2], "static/style.css");
    if (write_file(app,
        "body {\n"
        "  font-family: system-ui, sans-serif;\n"
        "  max-width: 36rem;\n"
        "  margin: 3rem auto;\n"
        "  padding: 0 1rem;\n"
        "  color: #222;\n"
        "}\n"
        "\n"
        "h1 {\n"
        "  border-bottom: 2px solid #bcd;\n"
        "  padding-bottom: .4rem;\n"
        "}\n"
        "\n"
        "code, footer {\n"
        "  color: #556;\n"
        "}\n") != 0) {
        return die("cannot write static/style.css");
    }

    printf("created %s/\n"
           "  views/layout.c.html    document shell wrapping every page\n"
           "  views/index.c.html     home page (handles GET and POST)\n"
           "  views/404.c.html       custom not-found page\n"
           "  views/partials         reusable fragments, never routes\n"
           "  static/style.css       served from the /* mount\n"
           "\n"
           "next: cd %s && cweb serve views 8080 . static\n",
           argv[2], argv[2]);
    return 0;
}

// ---------------------------------------------------------------------------
// serve / watch / version
// ---------------------------------------------------------------------------

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

// newest mtime of any regular file under base/rel, folded into best; used to
// notice templates or static files changing while the watch server runs.
// sub-second precision keeps a quick edit within the same second from slipping
// past the comparison
static intmax_t file_mtime(const struct stat *st)
{
    return (intmax_t)st->st_mtim.tv_sec * 1000000000 + (intmax_t)st->st_mtim.tv_nsec;
}

static intmax_t tree_mtime(const char *base, const char *rel, intmax_t best)
{
    char dir_path[4096];
    if (rel[0] == '\0') {
        snprintf(dir_path, sizeof dir_path, "%s", base);
    } else if (path_join(dir_path, sizeof dir_path, base, rel) != 0) {
        return best;
    }
    DIR *d = opendir(dir_path);
    if (d == NULL) {
        return best;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *name = e->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }
        char child[4096];
        if (rel[0] == '\0') {
            snprintf(child, sizeof child, "%s", name);
        } else if (path_join(child, sizeof child, rel, name) != 0) {
            continue;
        }
        char full[4096];
        if (path_join(full, sizeof full, base, child) != 0) {
            continue;
        }
        struct stat st;
        if (stat(full, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            best = tree_mtime(base, child, best);
        } else if (S_ISREG(st.st_mode) && file_mtime(&st) > best) {
            best = file_mtime(&st);
        }
    }
    closedir(d);
    return best;
}

static intmax_t watch_snapshot(const char *views_arg, const char *static_arg)
{
    intmax_t best = tree_mtime(views_arg, "", 0);
    if (static_arg[0] != '\0') {
        best = tree_mtime(static_arg, "", best);
    }
    return best;
}

// the generated server's argv: fixed "--port" pairs plus whatever flags the
// user tacked on after the positional arguments, passed through verbatim so
// cweb serve and the built binary honor the same options
static void server_argv(char *av[12], const char *port, char **extra, int nextra)
{
    int n = 0;
    av[n++] = (char *)"server";
    av[n++] = (char *)"--port";
    av[n++] = (char *)port;
    for (int i = 0; i < nextra && n < 11; i++) {
        av[n++] = extra[i];
    }
    av[n] = NULL;
}

static pid_t spawn_server(const char *scratch, const char *port,
                          char **extra, int nextra)
{
    char bin_path[512];
    snprintf(bin_path, sizeof bin_path, "%s/server", scratch);
    char *av[12];
    server_argv(av, port, extra, nextra);
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        execv(bin_path, av);
        _exit(126);
    }
    return pid;
}

// flags after the positional arguments belong to the generated server: cap
// them so the argv array in server_argv never overflows
#define MAX_SERVER_FLAGS 8

static int cmd_serve(int argc, char **argv)
{
    int watch = 0;
    int ai = 2;
    if (argc > ai && strcmp(argv[ai], "--watch") == 0) {
        watch = 1;
        ai++;
    }
    if (argc <= ai) {
        return die("serve needs VIEWS_DIR "
                   "(cweb serve [--watch] views [port [root [static]]] [FLAGS...])");
    }
    const char *views_arg = argv[ai++];
    const char *port = argc > ai ? argv[ai++] : "8080";
    const char *root = argc > ai ? argv[ai++] : ".";
    const char *static_arg = argc > ai ? argv[ai++] : "";
    char **extra = argv + ai;
    int nextra = argc - ai;
    if (nextra > MAX_SERVER_FLAGS) {
        return die("too many server flags");
    }

    char scratch[] = "/tmp/cweb-serve-XXXXXX";
    if (mkdtemp(scratch) == NULL) {
        return die("mkdtemp failed");
    }
    // reuse the build pass, pointed at the scratch dir
    char *build_av[6];
    build_av[0] = argv[0];
    build_av[1] = (char *)"build";
    build_av[2] = (char *)views_arg;
    build_av[3] = scratch;
    build_av[4] = (char *)root;
    build_av[5] = (char *)static_arg;
    if (cmd_build(6, build_av) != 0) {
        return 1;
    }

    if (!watch) {
        // exec replaces this process, the generated server takes over
        char bin_path[512];
        snprintf(bin_path, sizeof bin_path, "%s/server", scratch);
        char *av[12];
        server_argv(av, port, extra, nextra);
        execv(bin_path, av);
        fprintf(stderr, "cweb: cannot exec %s\n", bin_path);
        return 1;
    }

    // watch mode keeps the CLI alive and restarts the server whenever a
    // template or static file changes; Ctrl-C stops both
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    // snapshot before the server is up: any edit made between now and the
    // handoff to the caller's first request still counts as a change
    struct timespec gap = {.tv_sec = 0, .tv_nsec = 200000000};
    intmax_t snap = watch_snapshot(views_arg, static_arg);
    printf("cweb: watching %s (Ctrl-C to stop)\n", views_arg);
    pid_t child = spawn_server(scratch, port, extra, nextra);
    if (child < 0) {
        return die("cannot spawn the server");
    }

    int rc = 0;
    while (!g_stop) {
        if (nanosleep(&gap, NULL) != 0) {
            break; // interrupted, almost certainly by the stop signal
        }
        intmax_t now = watch_snapshot(views_arg, static_arg);
        if (now <= snap) {
            continue;
        }
        snap = now;
        printf("cweb: change detected, rebuilding\n");
        if (cmd_build(6, build_av) != 0) {
            fprintf(stderr, "cweb: rebuild failed; keeping the current server\n");
            continue;
        }
        kill(child, SIGTERM);
        waitpid(child, NULL, 0);
        child = spawn_server(scratch, port, extra, nextra);
        if (child < 0) {
            fprintf(stderr, "cweb: cannot respawn the server\n");
            rc = 1;
            break;
        }
    }

    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    char cleanup[4096];
    snprintf(cleanup, sizeof cleanup, "rm -rf %s", scratch);
    system(cleanup);
    return rc;
}

static int cmd_version(void)
{
    printf("cweb %s\n", cweb_version());
    return 0;
}

static void usage(FILE *f)
{
    fprintf(f,
        "usage: cweb <command>\n"
        "\n"
        "  build VIEWS_DIR OUT_DIR [ROOT [STATIC]]  compile every\n"
        "                                  VIEWS_DIR/**/*.c.html into C, emit\n"
        "                                  OUT_DIR/main.c and link OUT_DIR/server\n"
        "  serve [--watch] VIEWS_DIR [PORT [ROOT [STATIC]]] [FLAGS...]\n"
        "                                  build into a scratch dir and run it;\n"
        "                                  --watch rebuilds and restarts on change;\n"
"                                  FLAGS pass through to the server\n"
         "                                  (e.g. --rate 30 --secure --log access.log)\n"
        "  new APP_DIR                     scaffold a runnable app skeleton\n"
        "  version                         print the framework version\n"
        "\n"
"ROOT (default \".\") is the cweb checkout holding include/ and\n"
         "build/libcweb.a; when the path given doesn't hold an include/ dir,\n"
         "the tool walks up to the nearest ancestor that does, so you can run\n"
         "\"cweb serve views 8080 . static\" from inside an app directory and\n"
         "the framework is still found. STATIC, when given, is a\n"
         "directory mounted at /* and served for any path a page does not claim.\n"
        "A views/404.c.html page becomes the app's custom not-found page, served\n"
        "with a 404 status for any path nothing else answers.\n"
        "Files under VIEWS_DIR/partials/ become reusable fragments: they are\n"
        "compiled and declared in OUT_DIR/pages.h but never routed, so a view\n"
        "can render one with <?c page_footer(req, res, params, user_data); ?>.\n"
        "A views/layout.c.html is the wrapper convention: compiled but never\n"
        "routed, it embeds the captured page body with\n"
        "<?c cweb_tpl_layout_emit(res); ?> and so shapes every\n"
        "page (including the 404 page).\n"
"The built OUT_DIR/server accepts --port N, --rate N (a per-unit\n"
         "budget of N requests a minute: the caller's address unless the\n"
         "request names an account — Basic creds map to that account even\n"
         "while the password is still being checked, throttling brute force\n"
         "across caller IPs), --secure (hardening headers on\n"
         "every response), --gzip (compress compressible bodies), --db PATH\n"
         "(a file-backed key-value store pages reach through cweb_database()),\n"
"--static-cache SECONDS (Cache-Control max-age on static files, with\n"
          "ETag and Last-Modified validators already answering 304), and\n"
"--etag (strong ETag from a SHA-256 of every dynamic response's\n"
          "body; a matching If-None-Match returns 304 with no payload),\n"
          "--request-id (stamp every response -- and every req->request_id --\n"
          "with a unique 16-hex trace id) and --request-id-inherit (additionally\n"
          "echo an inbound X-Request-Id from a trusted proxy),\n"
          "--basic-auth USER:PASS (lock the whole app behind HTTP Basic auth;\n"
          "anything without matching credentials gets 401 + WWW-Authenticate),\n"
          "--trusted-proxy CIDR[,CIDR...] (believe X-Forwarded-For only from\n"
          "reverse proxies in these networks, giving the real client instead\n"
          "of the proxy in logs and per-address rate buckets),\n"
          "--max-body BYTES (request size cap, 413 past it; accepts k/m/g\n"
          "suffixes like 4k or 2m) --body-dir DIR (with --max-body set, stream\n"
          "oversized bodies to temp files in DIR, mapped back and then\n"
          "unlinked, instead of answering 413) --max-inflated BYTES (ceiling\n"
          "a Content-Encoding: gzip request body may decompress to, 413 past\n"
          "it; accepts k/m/g suffixes, default 128m) --tls-cert FILE --tls-key FILE\n"
          "(PEM chain cert and private key; both turn the whole listener into\n"
          "HTTPS, generated servers reject plaintext to it) --io-timeout MS (per-read\n"
          "deadline, stalled\n"
          "clients get 408) and --workers N (accept-loop threads, default one\n"
"per core), and\n"
          "--signature-secret SECRET (fall back on a reverse proxy that stamps\n"
          "every request with HMAC-SHA256 of \"METHOD\\nPATH\\nUNIX-SECONDS\\nBODY\"\n"
          "in X-CWEB-Signature/X-CWEB-Date; anything else is answered 403), and\n"
          "--socket PATH (also serve the same app on a unix socket file at\n"
          "PATH, alongside the TCP port, for local reverse-proxy setups; the\n"
          "socket file is removed on shutdown), and\n"
          "--cors ORIGIN (answer browser CORS preflights and stamp\n"
          "Access-Control-Allow-Origin on matching requests; \"*\" opens the\n"
          "app to any origin without credentials), and\n"
          "--csp POLICY (Content-Security-Policy on every response, replacing\n"
          "the built-in default-src 'self' directive) and --csp-report-only\n"
          "POLICY (a monitoring-only policy alongside it), and\n"
          "--log FILE (append Common Log Format access lines to FILE), and\n"
         "--route-timeout PATH MS (set SO_RCVTIMEO to MS milliseconds for the\n"
         "next request on a keep-alive connection after serving PATH; a stall\n"
         "then produces 504 instead of the default 408; repeat for more routes).\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    if (strcmp(argv[1], "build") == 0) {
        return cmd_build(argc, argv);
    }
    if (strcmp(argv[1], "new") == 0) {
        return cmd_new(argc, argv);
    }
    if (strcmp(argv[1], "serve") == 0) {
        return cmd_serve(argc, argv);
    }
    if (strcmp(argv[1], "version") == 0) {
        return cmd_version();
    }
    fprintf(stderr, "cweb: unknown command %s\n\n", argv[1]);
    usage(stderr);
    return 2;
}