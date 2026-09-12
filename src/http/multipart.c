#include "multipart.h"

#include <ctype.h>
#include <string.h>

#include "da.h"
#include "file.h"
#include "strmap.h"
#include "sv.h"
#include "xmem.h"

int multipart_boundary(String_View header_value, char *out, size_t out_sz)
{
    String_View rest = sv_trim(header_value);
    String_View media = sv_chop_by_delim(&rest, ';');
    if (!sv_equal(sv_trim(media), sv_from_cstr("multipart/form-data"))) {
        return -1;
    }
    while (rest.count > 0) {
        String_View param = sv_trim(sv_chop_by_delim(&rest, ';'));
        String_View key = sv_chop_by_delim(&param, '=');
        if (sv_equal(sv_trim(key), sv_from_cstr("boundary"))) {
            String_View value = sv_trim(param);
            // quoted values have the quotes peeled here
            if (value.count >= 2 && value.data[0] == '"' &&
                value.data[value.count - 1] == '"') {
                value.data++;
                value.count -= 2;
            }
            if (value.count == 0 || value.count >= out_sz) {
                return -1;
            }
            memcpy(out, value.data, value.count);
            out[value.count] = '\0';
            return 0;
        }
    }
    return -1;
}

// where the next real delimiter sits: CRLF plus "--" plus boundary, followed
// by either CRLF (another part coming) or "--" (the closing one). a field's
// data may legitimately contain the boundary text, so the lookahead matters.
// returns the index of the "--" (the CRLF before it still belongs to the
// part's content), SIZE_MAX when no delimiter is found.
static size_t find_delimiter(String_View body, size_t start,
                             const char *boundary, size_t blen)
{
    for (size_t i = start; i + blen + 6 <= body.count; i++) {
        if (body.data[i] == '\r' && body.data[i + 1] == '\n' &&
            body.data[i + 2] == '-' && body.data[i + 3] == '-' &&
            memcmp(body.data + i + 4, boundary, blen) == 0) {
            char a = i + 4 + blen < body.count ? body.data[i + 4 + blen] : '\0';
            char b = i + 5 + blen < body.count ? body.data[i + 5 + blen] : '\0';
            if ((a == '\r' && b == '\n') || (a == '-' && b == '-')) {
                return i + 2;
            }
        }
    }
    return (size_t)-1;
}

// strips enclosing quotes from a quoted-string attribute value
static String_View unquote(String_View v)
{
    if (v.count >= 2 && v.data[0] == '"') {
        v.data++;
        v.count -= 2;
    }
    return v;
}

// "form-data; name=\"user\"; filename=\"a b.txt\"" -> name and filename
static void parse_disposition(String_View value, String_View *name,
                              String_View *filename)
{
    *name = (String_View){0};
    *filename = (String_View){0};
    String_View rest = value;
    String_View first = sv_trim(sv_chop_by_delim(&rest, ';'));
    (void)first; // "form-data", nothing to do with it for now
    while (rest.count > 0) {
        String_View param = sv_trim(sv_chop_by_delim(&rest, ';'));
        String_View key = sv_chop_by_delim(&param, '=');
        String_View val = unquote(sv_trim(param));
        if (sv_equal(sv_trim(key), sv_from_cstr("name"))) {
            *name = val;
        } else if (sv_equal(sv_trim(key), sv_from_cstr("filename"))) {
            *filename = val;
        }
    }
}

static int parse_part_headers(Multipart_Part *part, String_View block)
{
    String_View rest = block;
    while (rest.count > 0) {
        String_View line = sv_trim(sv_chop_by_delim(&rest, '\n'));
        if (line.count == 0) {
            continue;
        }
        size_t c = 0;
        while (c < line.count && line.data[c] != ':') {
            c++;
        }
        if (c >= line.count) {
            continue;
        }
        String_View key = sv_trim_right((String_View){line.data, c});
        String_View value = sv_trim((String_View){line.data + c + 1, line.count - c - 1});
        if (key.count == 0) {
            continue;
        }
        // headers are indexed under their lowercase name, HTTP folds case
        char lk[256];
        size_t kl = key.count < sizeof(lk) - 1 ? key.count : sizeof(lk) - 1;
        for (size_t i = 0; i < kl; i++) {
            lk[i] = (char)tolower((unsigned char)key.data[i]);
        }
        lk[kl] = '\0';
        if (strmap_set(&part->headers, sv_from_cstr(lk), value) != 0) {
            return -1;
        }
        if (sv_equal(sv_from_cstr(lk), sv_from_cstr("content-disposition"))) {
            parse_disposition(value, &part->name, &part->filename);
        }
    }
    return 0;
}

int multipart_parse(Multipart *m, String_View body, const char *boundary)
{
    m->items = NULL;
    m->count = 0;
    m->capacity = 0;

    size_t blen = strlen(boundary);
    if (blen == 0 || blen > MULTIPART_MAX_BOUNDARY) {
        return -1;
    }
    char delim[2 + MULTIPART_MAX_BOUNDARY + 1];
    memcpy(delim, "--", 2);
    memcpy(delim + 2, boundary, blen);
    delim[2 + blen] = '\0';
    size_t dd = 2 + blen;

    // the body opens with the first boundary
    if (body.count < dd || memcmp(body.data, delim, dd) != 0) {
        return -1;
    }
    size_t pos = dd;

    for (;;) {
        // "--boundary--" ends the stream (an optional trailing CRLF may stay)
        if (pos + 1 < body.count && body.data[pos] == '-' && body.data[pos + 1] == '-') {
            break;
        }
        if (pos + 1 >= body.count || body.data[pos] != '\r' || body.data[pos + 1] != '\n') {
            return -1;
        }
        pos += 2;

        // part headers run up to the blank line
        size_t h_start = pos;
        size_t h_end = (size_t)-1;
        for (size_t i = pos; i + 3 < body.count; i++) {
            if (body.data[i] == '\r' && body.data[i + 1] == '\n' &&
                body.data[i + 2] == '\r' && body.data[i + 3] == '\n') {
                h_end = i;
                break;
            }
        }
        if (h_end == (size_t)-1) {
            return -1;
        }

        Multipart_Part part;
        memset(&part, 0, sizeof(part));
        strmap_init(&part.headers);
        String_View head_block = (String_View){body.data + h_start, h_end - h_start};
        if (parse_part_headers(&part, head_block) != 0) {
            strmap_free(&part.headers);
            multipart_free(m);
            return -1;
        }

        size_t content_start = h_end + 4;
        size_t delim_pos = find_delimiter(body, content_start, boundary, blen);
        if (delim_pos == (size_t)-1) {
            // a part always ends at a real delimiter or the closing one
            strmap_free(&part.headers);
            multipart_free(m);
            return -1;
        }
        // content ends where the delimiter's leading CRLF begins; that CRLF is
        // framing, it does not belong to the part
        part.content = (String_View){body.data + content_start,
                                     (delim_pos - 2) - content_start};
        da_append(m, part);
        pos = delim_pos + 2 + blen;
    }
    return 0;
}

Multipart_Part *multipart_get(Multipart *m, const char *name)
{
    size_t n = strlen(name);
    for (size_t i = 0; i < m->count; i++) {
        if (m->items[i].name.count == n &&
            memcmp(m->items[i].name.data, name, n) == 0) {
            return &m->items[i];
        }
    }
    return NULL;
}

int multipart_save(const Multipart_Part *part, const char *dir,
                   char *out_path, size_t out_sz)
{
    // the filename is client-supplied, so the directory part is stripped to
    // its very last segment: "photos/../x.png" must arrive as plain x.png and
    // an absolute "/etc/passwd" as passwd, never a path the client chooses
    size_t last = part->filename.count;
    for (size_t i = 0; i < part->filename.count; i++) {
        if (part->filename.data[i] == '/' || part->filename.data[i] == '\\') {
            last = part->filename.count - i - 1;
        }
    }
    const char *base = part->filename.data + part->filename.count - last;
    if (last == 0 || (last == 1 && base[0] == '.') ||
        (last == 2 && base[0] == '.' && base[1] == '.')) {
        return -1;
    }

    size_t dlen = strlen(dir);
    size_t need = dlen + (dlen > 0 && dir[dlen - 1] != '/' ? 1 : 0) + last + 1;
    if (need > out_sz) {
        return -1;
    }
    memcpy(out_path, dir, dlen);
    size_t o = dlen;
    if (dlen > 0 && dir[dlen - 1] != '/') {
        out_path[o++] = '/';
    }
    memcpy(out_path + o, base, last);
    o += last;
    out_path[o] = '\0';

    return file_write(out_path, part->content.data, part->content.count);
}

void multipart_free(Multipart *m)
{
    for (size_t i = 0; i < m->count; i++) {
        strmap_free(&m->items[i].headers);
    }
    xfree(m->items);
    m->items = NULL;
    m->count = 0;
    m->capacity = 0;
}