#include <stdio.h>
#include <string.h>

#include "buffer.h"
#include "sv.h"

static int check(const char *what, int cond)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        return 1;
    }
    return 0;
}

int main(void)
{
    int fails = 0;
    Read_Buffer rb;
    rb_init(&rb, 16);

    // "read" the first chunk
    String_View head = rb_write_head(&rb);
    if (head.count != 16) {
        fprintf(stderr, "expected 16 free bytes, got %zu\n", head.count);
        return 1;
    }
    memcpy((void *)head.data, "hello", 5);
    rb_commit(&rb, 5);
    fails += check("view after first read", sv_equal(rb_view(&rb), sv_from_cstr("hello")));

    // second chunk lands right after the first
    String_View head2 = rb_write_head(&rb);
    fails += check("write head advances", head2.data == head.data + 5 && head2.count == 11);
    memcpy((void *)head2.data, " world", 6);
    rb_commit(&rb, 6);
    fails += check("view after second read", sv_equal(rb_view(&rb), sv_from_cstr("hello world")));

    // dropping bytes compacts without losing the rest
    rb_discard(&rb, 6);
    fails += check("discard compacts", sv_equal(rb_view(&rb), sv_from_cstr("world")));
    String_View head3 = rb_write_head(&rb);
    fails += check("compacted tail is reusable", head3.count >= 5);
    memcpy((void *)head3.data, "!", 1);
    rb_commit(&rb, 1);
    fails += check("data intact after mixing", sv_equal(rb_view(&rb), sv_from_cstr("world!")));

    rb_free(&rb);
    fails += check("free nulls the buffer", rb.data == NULL && rb.capacity == 0);

    if (fails == 0) {
        printf("buffer ok\n");
    }
    return fails != 0;
}