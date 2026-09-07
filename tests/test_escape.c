#include <stdio.h>
#include <string.h>

#include "escape.h"
#include "strbuf.h"
#include "sv.h"

int main(void)
{
    Strbuf sb;
    strbuf_init(&sb);

    html_escape_into(&sb, sv_from_cstr("<script>alert(\"x\") & 'y'</script>"));
    if (strbuf_null_terminate(&sb) != 0) return 1;
    if (strcmp(sb.items, "&lt;script&gt;alert(&quot;x&quot;) &amp; &#39;y&#39;&lt;/script&gt;") != 0) {
        fprintf(stderr, "escape gave: %s\n", sb.items);
        return 1;
    }
    strbuf_free(&sb);

    // bytes outside ASCII have no entities, they must pass through untouched
    strbuf_init(&sb);
    html_escape_into(&sb, sv_from_cstr("héllo\xC3\xA9"));
    if (strbuf_null_terminate(&sb) != 0) return 1;
    if (strcmp(sb.items, "héllo\xC3\xA9") != 0) {
        fprintf(stderr, "unicode passthrough broke: %s\n", sb.items);
        return 1;
    }

    // empty input keeps the buffer empty
    if (sb.count != 8) {
        fprintf(stderr, "expected 8 chars, got %zu\n", sb.count);
        return 1;
    }

    strbuf_free(&sb);
    printf("escape ok\n");
    return 0;
}