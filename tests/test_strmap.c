#include <stdio.h>
#include <string.h>

#include "strmap.h"
#include "sv.h"

int main(void)
{
    Str_Map m;
    strmap_init(&m);

    for (int i = 0; i < 1000; i++) {
        char key[32];
        char value[64];
        snprintf(key, sizeof(key), "param%d", i);
        snprintf(value, sizeof(value), "value-%d", i);
        if (strmap_set(&m, sv_from_cstr(key), sv_from_cstr(value)) != 0) {
            fprintf(stderr, "set failed for %s\n", key);
            return 1;
        }
    }
    if (m.count != 1000) {
        fprintf(stderr, "count=%zu, want 1000\n", m.count);
        return 1;
    }

    for (int i = 0; i < 1000; i++) {
        char key[32];
        char want[64];
        snprintf(key, sizeof(key), "param%d", i);
        snprintf(want, sizeof(want), "value-%d", i);
        const char *got = strmap_get_cstr(&m, key);
        if (!got || strcmp(got, want) != 0) {
            fprintf(stderr, "get for %s returned %s\n", key, got ? got : "(null)");
            return 1;
        }
    }

    // overwriting must keep one entry, not pile up duplicates
    if (strmap_set(&m, sv_from_cstr("dup"), sv_from_cstr("one")) != 0) {
        fprintf(stderr, "set dup failed\n");
        return 1;
    }
    if (strmap_set(&m, sv_from_cstr("dup"), sv_from_cstr("two")) != 0) {
        fprintf(stderr, "re-set dup failed\n");
        return 1;
    }
    if (m.count != 1001) {
        fprintf(stderr, "count after overwrite=%zu, want 1001\n", m.count);
        return 1;
    }
    const char *dupv = strmap_get_cstr(&m, "dup");
    if (!dupv || strcmp(dupv, "two") != 0) {
        fprintf(stderr, "dup value not replaced: %s\n", dupv ? dupv : "(null)");
        return 1;
    }

    if (strmap_get_cstr(&m, "no_such_key") != NULL) {
        fprintf(stderr, "got a value for a missing key\n");
        return 1;
    }

    strmap_free(&m);
    if (m.entries != NULL) {
        fprintf(stderr, "free left entries dangling\n");
        return 1;
    }

    printf("strmap ok\n");
    return 0;
}