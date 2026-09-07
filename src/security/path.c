#include "path.h"

#include "strbuf.h"
#include "sv.h"

int uri_path_normalize(String_View path, Strbuf *out)
{
    bool absolute = path.count > 0 && path.data[0] == '/';

    // segments are built here without the leading slash, so popping a ".."
    // is just truncating back to the previous separator
    Strbuf parts;
    strbuf_init(&parts);
    size_t segs = 0;

    String_View rest = path;
    while (rest.count > 0) {
        String_View seg = sv_chop_by_delim(&rest, '/');
        if (seg.count == 0 || sv_equal(seg, sv_from_cstr("."))) {
            continue;
        }
        if (sv_equal(seg, sv_from_cstr(".."))) {
            if (segs == 0) {
                strbuf_free(&parts);
                return -1; // climbing above the root is not allowed
            }
            size_t i = parts.count - 1;
            while (i > 0 && parts.items[i] != '/') {
                i--;
            }
            parts.count = i;
            segs--;
            continue;
        }
        if (segs > 0) {
            strbuf_append_char(&parts, '/');
        }
        strbuf_append(&parts, seg.data, seg.count);
        segs++;
    }

    if (parts.count == 0) {
        strbuf_append_char(out, '/'); // root, absolute or not, is "whatever root"
    } else if (absolute) {
        strbuf_append_char(out, '/');
        strbuf_append(out, parts.items, parts.count);
    } else {
        strbuf_append(out, parts.items, parts.count);
    }

    strbuf_free(&parts);
    return 0;
}