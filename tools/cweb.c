// cweb: the C-WEB command-line driver. turns a directory of .c.html pages
// into a native server binary:
//
//   cweb build views/ out/    compile every views/**/*.c.html to C, generate
//                             out/main.c and link out/server against libcweb.a
//   cweb serve views/         build into a scratch dir and run it
//   cweb version
//
// ROOT (default ".") is the project root holding include/ and build/libcweb.a,
// so run the tool from there.

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

// plain pages answer their own path; xxx/index.c.html also answers "/"
static void emit_main(FILE *f, const Str_List *views)
{
    fputs(
        "/* generated by cweb build; hand edits are overwritten */\n"
        "#include <stdio.h>\n"
        "#include <stdlib.h>\n"
        "#include <string.h>\n"
        "\n"
        "#include \"http.h\"\n"
        "#include \"net.h\"\n"
        "#include \"router.h\"\n"
        "#include \"server.h\"\n"
        "#include \"strmap.h\"\n"
        "\n",
        f);

    // forward declarations come first so each generated page can live in its
    // own translation unit
    for (size_t i = 0; i < views->count; i++) {
        char name[128], route[512];
        cweb_template_page_name(views->items[i], name, sizeof name);
        route_of(views->items[i], route, sizeof route);
        fprintf(f, "void %s(Http_Request *req, Http_Response *res,\n"
                   "      Str_Map *params, void *user_data);\n", name);
    }
    fputs("\nstatic void list_routes(void)\n{\n", f);
    for (size_t i = 0; i < views->count; i++) {
        char name[128], route[512];
        cweb_template_page_name(views->items[i], name, sizeof name);
        route_of(views->items[i], route, sizeof route);
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
    fputs("}\n\n", f);

    fputs(
        "int main(int argc, char **argv)\n"
        "{\n"
        "    int port = 8080;\n"
        "    for (int i = 1; i < argc; i++) {\n"
        "        if (strcmp(argv[i], \"--port\") == 0 && i + 1 < argc) {\n"
        "            port = atoi(argv[++i]);\n"
        "        } else if (strcmp(argv[i], \"--routes\") == 0) {\n"
        "            list_routes();\n"
        "            return 0;\n"
        "        } else {\n"
        "            fprintf(stderr, \"cweb: unknown option %s\\n\", argv[i]);\n"
        "            return 2;\n"
        "        }\n"
        "    }\n"
        "    if (net_init() != 0) {\n"
        "        fprintf(stderr, \"cweb: net_init failed\\n\");\n"
        "        return 1;\n"
        "    }\n"
        "    Http_Router r;\n"
        "    router_init(&r);\n",
        f);

    for (size_t i = 0; i < views->count; i++) {
        char name[128], route[512];
        cweb_template_page_name(views->items[i], name, sizeof name);
        route_of(views->items[i], route, sizeof route);
        fprintf(f, "    router_add(&r, HTTP_GET, ");
        fputc('"', f);
        emit_esc(f, route);
        fputc('"', f);
        fprintf(f, ", %s, NULL);\n", name);
        if (ends_with_index(route)) {
            fprintf(f, "    router_add(&r, HTTP_GET, \"/\", %s, NULL);\n", name);
        }
    }

    fputs(
        "    printf(\"cweb server on http://0.0.0.0:%d\\n\", port);\n"
        "    Socket_Handle listener = net_listen(port);\n"
        "    if (listener < 0) {\n"
        "        fprintf(stderr, \"cweb: listen %d: %s\\n\", port,\n"
        "                net_error_string());\n"
        "        return 1;\n"
        "    }\n"
        "    int rc = http_serve(listener, router_dispatch, &r);\n"
        "    router_free(&r);\n"
        "    net_cleanup();\n"
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

static int cmd_build(int argc, char **argv)
{
    if (argc < 4) {
        return die("build needs VIEWS_DIR and OUT_DIR (cweb build views out [root])");
    }
    const char *views_arg = argv[2];
    const char *out_arg = argv[3];
    const char *root = argc >= 5 ? argv[4] : ".";

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
    char (*names)[128] = xmalloc(views.count * sizeof *names);
    char (*routes)[512] = xmalloc(views.count * sizeof *routes);
    for (size_t i = 0; i < views.count; i++) {
        cweb_template_page_name(views.items[i], names[i], sizeof names[i]);
        route_of(views.items[i], routes[i], sizeof routes[i]);
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

    mkdir(out_arg, 0755); // ignore EEXIST

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
    emit_main(f, &views);
    fclose(f);

    char bin_path[512];
    snprintf(bin_path, sizeof bin_path, "%s/server", out_arg);
    Strbuf cc;
    strbuf_init(&cc);
    strbuf_append_cstr(&cc, "cc -std=c11 -Wall -Wextra -I");
    strbuf_append_cstr(&cc, root);
    strbuf_append_cstr(&cc, "/include ");
    strbuf_append_cstr(&cc, main_path);
    for (size_t i = 0; i < views.count; i++) {
        snprintf(gen, sizeof gen, "%s/%s.c", out_arg, names[i]);
        strbuf_append_char(&cc, ' ');
        strbuf_append_cstr(&cc, gen);
    }
    strbuf_append_cstr(&cc, " ");
    strbuf_append_cstr(&cc, root);
    strbuf_append_cstr(&cc, "/build/libcweb.a -o ");
    strbuf_append_cstr(&cc, bin_path);
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
// serve / version
// ---------------------------------------------------------------------------

static int cmd_serve(int argc, char **argv)
{
    if (argc < 3) {
        return die("serve needs VIEWS_DIR (cweb serve views [port] [root])");
    }
    const char *views_arg = argv[2];
    const char *port = argc >= 4 ? argv[3] : "8080";
    const char *root = argc >= 5 ? argv[4] : ".";

    char scratch[] = "/tmp/cweb-serve-XXXXXX";
    if (mkdtemp(scratch) == NULL) {
        return die("mkdtemp failed");
    }
    // reuse the build pass, pointed at the scratch dir
    char *build_av[5];
    build_av[0] = argv[0];
    build_av[1] = (char *)"build";
    build_av[2] = (char *)views_arg;
    build_av[3] = scratch;
    build_av[4] = (char *)root;
    if (cmd_build(5, build_av) != 0) {
        return 1;
    }
    // exec replaces this process, the generated server takes over
    char bin_path[512];
    snprintf(bin_path, sizeof bin_path, "%s/server", scratch);
    execl(bin_path, "server", "--port", port, NULL);
    fprintf(stderr, "cweb: cannot exec %s\n", bin_path);
    return 1;
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
        "  build VIEWS_DIR OUT_DIR [ROOT]  compile every VIEWS_DIR/**/*.c.html\n"
        "                                  into C, emit OUT_DIR/main.c and link\n"
        "                                  OUT_DIR/server\n"
        "  serve VIEWS_DIR [PORT] [ROOT]   build into a scratch dir and run it\n"
        "  version                         print the framework version\n"
        "\n"
        "ROOT (default \".\") is the project root holding include/ and\n"
        "build/libcweb.a, so run the tool from there.\n");
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