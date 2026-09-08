#include "negotiate.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "da.h"
#include "xmem.h"

static char sv_fold(char c)
{
    return (char)tolower((unsigned char)c);
}

static bool sv_equal_ci(String_View a, String_View b)
{
    if (a.count != b.count) {
        return false;
    }
    for (size_t i = 0; i < a.count; i++) {
        if (sv_fold(a.data[i]) != sv_fold(b.data[i])) {
            return false;
        }
    }
    return true;
}

static int parse_q(String_View s, double *out)
{
    s = sv_trim(s);
    // q = "0" | ( "0" "." 0*3DIGIT ) | ( "1" "." 0*3("0") ), so 5 bytes is
    // already the longest legal spelling ("1.000")
    if (s.count == 0 || s.count >= 8) {
        return -1;
    }
    char tmp[8];
    memcpy(tmp, s.data, s.count);
    tmp[s.count] = '\0';
    char *end;
    double v = strtod(tmp, &end);
    if (end == tmp || *end != '\0') {
        return -1;
    }
    if (v < 0.0) {
        v = 0.0;
    }
    if (v > 1.0) {
        v = 1.0;
    }
    *out = v;
    return 0;
}

int accept_parse(Accept_List *list, String_View header)
{
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;

    String_View rest = header;
    while (rest.count > 0) {
        String_View chunk = sv_chop_by_delim(&rest, ',');
        chunk = sv_trim(chunk);
        if (chunk.count == 0) {
            continue; // ",," and trailing commas are harmless in practice
        }

        // the media range is everything before the first ';', the params
        // after it. only q= is meaningful to us, the rest is ignored.
        String_View media = sv_chop_by_delim(&chunk, ';');
        media = sv_trim(media);
        if (media.count == 0 || media.data[0] == '/') {
            accept_free(list);
            return -1;
        }

        double q = 1.0;
        String_View params = chunk;
        while (params.count > 0) {
            String_View param = sv_chop_by_delim(&params, ';');
            String_View key = sv_chop_by_delim(&param, '=');
            key = sv_trim(key);
            String_View value = sv_trim(param);
            if (sv_equal_ci(key, sv_from_cstr("q")) && value.count > 0) {
                if (parse_q(value, &q) != 0) {
                    accept_free(list);
                    return -1;
                }
            }
        }

        Accept_Item item = {media, q};
        da_append(list, item);
    }

    return 0;
}

// rank 0 = no match, 1 = "*/*", 2 = "type/*", 3 = exact
static int match_rank(String_View range, String_View type)
{
    String_View r_type = sv_chop_by_delim(&range, '/');
    String_View r_sub = range;
    String_View t_type = sv_chop_by_delim(&type, '/');
    String_View t_sub = type;

    if (sv_equal_ci(r_type, sv_from_cstr("*"))) {
        return sv_equal_ci(r_sub, sv_from_cstr("*")) ? 1 : 0;
    }
    if (sv_equal_ci(r_type, t_type)) {
        if (sv_equal_ci(r_sub, sv_from_cstr("*"))) {
            return 2;
        }
        return sv_equal_ci(r_sub, t_sub) ? 3 : 0;
    }
    return 0;
}

int accept_best(Accept_List *list, String_View *available, size_t count)
{
    if (count == 0) {
        return -1;
    }
    if (list->count == 0) {
        return 0; // no constraints from the client: the first serves fine
    }

    int best = -1;
    double best_q = -1.0;
    for (size_t i = 0; i < count; i++) {
        int rank = 0;
        double q = 0.0;
        for (size_t j = 0; j < list->count; j++) {
            int r = match_rank(list->items[j].type, available[i]);
            if (r > rank) {
                rank = r;
                q = list->items[j].q;
            }
        }
        if (rank > 0 && (best < 0 || q > best_q)) {
            best = (int)i;
            best_q = q;
        }
    }
    // best_q == 0 means the client excluded every candidate
    return best >= 0 && best_q > 0.0 ? best : -1;
}

void accept_free(Accept_List *list)
{
    xfree(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}