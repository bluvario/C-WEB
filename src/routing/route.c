#include "route.h"

#include "http.h"
#include "sv.h"

// walk a path one slash-delimited segment at a time, ignoring leading and
// repeated slashes. returns an empty view once there is nothing left.
static String_View next_seg(String_View *s)
{
    while (s->count > 0 && s->data[0] == '/') {
        s->data++;
        s->count--;
    }
    if (s->count == 0) {
        return (String_View){0};
    }
    return sv_chop_by_delim(s, '/');
}

bool route_path_matches(const char *pattern, String_View path, Str_Map *params)
{
    String_View p = sv_from_cstr(pattern);
    String_View q = path;
    for (;;) {
        String_View pseg = next_seg(&p);
        String_View qseg = next_seg(&q);
        if (pseg.count == 0 || qseg.count == 0) {
            return pseg.count == 0 && qseg.count == 0;
        }

        if (pseg.count >= 2 && pseg.data[0] == '<' && pseg.data[pseg.count - 1] == '>') {
            String_View name = (String_View){pseg.data + 1, pseg.count - 2};
            if (name.count == 0) {
                return false; // "<>" captures nothing, refuse to match
            }
            if (params && strmap_set(params, name, qseg) != 0) {
                return false;
            }
        } else if (!sv_equal(pseg, qseg)) {
            return false;
        }
    }
}

bool route_match(Route_Def route, Http_Method method, String_View path, Str_Map *params)
{
    if (route.method != method) {
        return false;
    }
    return route_path_matches(route.pattern, path, params);
}