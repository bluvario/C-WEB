#include <stdio.h>
#include <string.h>

#include "strbuf.h"

int main(void)
{
    Strbuf sb;
    strbuf_init(&sb);

    if (strbuf_append_cstr(&sb, "cweb") != 0) {
        fprintf(stderr, "append_cstr failed\n");
        return 1;
    }
    if (strbuf_null_terminate(&sb) != 0) {
        fprintf(stderr, "null_terminate failed\n");
        return 1;
    }
    if (sb.count != 4 || strcmp(sb.items, "cweb") != 0) {
        fprintf(stderr, "bad content after append_cstr (count=%zu)\n", sb.count);
        return 1;
    }

    for (int i = 0; i < 100000; i++) {
        if (strbuf_append_char(&sb, 'x') != 0) {
            fprintf(stderr, "append_char failed at %d (cap=%zu)\n", i, sb.capacity);
            return 1;
        }
    }
    if (sb.count != 4 + 100000) {
        fprintf(stderr, "count=%zu, want %d\n", sb.count, 4 + 100000);
        return 1;
    }
    if (strbuf_null_terminate(&sb) != 0) {
        fprintf(stderr, "null_terminate failed after growth\n");
        return 1;
    }
    // prefix must survive the realloc churn
    if (memcmp(sb.items, "cweb", 4) != 0) {
        fprintf(stderr, "prefix clobbered after growth\n");
        return 1;
    }

    strbuf_free(&sb);
    if (sb.items != NULL) {
        fprintf(stderr, "free left items dangling\n");
        return 1;
    }

    printf("strbuf ok\n");
    return 0;
}