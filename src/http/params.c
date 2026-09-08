#include "params.h"

#include "multipart.h"
#include "request.h"
#include "strmap.h"
#include "sv.h"
#include "url.h"
#include "xmem.h"

// query strings, application/x-www-form-urlencoded bodies and cookie lines
// all share this shape: &-separated key=value with percent escapes
int params_parse_into(Str_Map *m, String_View s)
{
    while (s.count > 0) {
        String_View pair = sv_chop_by_delim(&s, '&');
        if (pair.count == 0) {
            continue;
        }

        size_t eq = 0;
        while (eq < pair.count && pair.data[eq] != '=') {
            eq++;
        }
        String_View key = (String_View){pair.data, eq};
        String_View value = (String_View){
            eq < pair.count ? pair.data + eq + 1 : pair.data,
            eq < pair.count ? pair.count - eq - 1 : 0,
        };

        char *dk = url_decode_alloc(key);
        if (!dk) {
            return -1;
        }
        char *dv = url_decode_alloc(value);
        if (!dv) {
            xfree(dk);
            return -1;
        }

        int rc = strmap_set(m, sv_from_cstr(dk), sv_from_cstr(dv));
        xfree(dk);
        xfree(dv);
        if (rc != 0) {
            return -1;
        }
    }
    return 0;
}

int request_merge_params(Http_Request *req, Str_Map *out)
{
    if (req->query.count > 0 && params_parse_into(out, req->query) != 0) {
        return -1;
    }
    const String_View *content_type = http_request_get_header(req, "content-type");
    if (!content_type) {
        return 0;
    }
    if (sv_starts_with(*content_type, "application/x-www-form-urlencoded")) {
        if (params_parse_into(out, req->body) != 0) {
            return -1;
        }
        return 0;
    }
    if (sv_starts_with(*content_type, "multipart/form-data")) {
        // file parts carry no "value", so they are left to the handler to
        // fish out itself; plain fields land in out like any other param
        char boundary[MULTIPART_MAX_BOUNDARY];
        if (multipart_boundary(*content_type, boundary, sizeof(boundary)) != 0) {
            return -1;
        }
        Multipart mp;
        if (multipart_parse(&mp, req->body, boundary) != 0) {
            return -1;
        }
        for (size_t i = 0; i < mp.count; i++) {
            if (mp.items[i].filename.count == 0) {
                if (strmap_set(out, mp.items[i].name, mp.items[i].content) != 0) {
                    multipart_free(&mp);
                    return -1;
                }
            }
        }
        multipart_free(&mp);
    }
    return 0;
}