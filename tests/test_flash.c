#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flash.h"
#include "response.h"
#include "session.h"
#include "xmem.h"

static int check(const char *what, int cond)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        return 1;
    }
    return 0;
}

// counts how many times needle appears in hay
static size_t count_of(const char *hay, const char *needle)
{
    size_t n = 0;
    const char *at = hay;
    while ((at = strstr(at, needle)) != NULL) {
        n++;
        at += strlen(needle);
    }
    return n;
}

int main(void)
{
    int fails = 0;

    Http_Session_Store s;
    http_session_store_init(&s, 0);
    char *token = http_session_create(&s, 0);
    fails += check("session created", token != NULL);
    Http_Session *sesh = http_session_open(&s, token);
    fails += check("session opened", sesh != NULL);

    // a stored message is handed out exactly once
    http_flash_set(sesh, "saved", "note pinned!");
    char *got = http_flash_get(sesh, "saved");
    fails += check("flash get returns the message",
                   got != NULL && strcmp(got, "note pinned!") == 0);
    xfree(got);
    got = http_flash_get(sesh, "saved");
    fails += check("flash get consumes the message on read", got == NULL);
    xfree(got);

    // re-setting the same key before it is read replaces it in place
    http_flash_set(sesh, "error", "old");
    http_flash_set(sesh, "error", "new");
    got = http_flash_get(sesh, "error");
    fails += check("second set replaces the pending message",
                   got != NULL && strcmp(got, "new") == 0);
    xfree(got);

    // flash keys live under a reserved prefix, so session data of the same
    // name is untouched
    http_session_set(sesh, "user", "alice");
    http_flash_set(sesh, "user", "flash-user");
    const char *plain = http_session_value(sesh, "user");
    fails += check("flash key does not collide with session data",
                   plain != NULL && strcmp(plain, "alice") == 0);
    got = http_flash_get(sesh, "user");
    fails += check("flash get reads its own reserved key",
                   got != NULL && strcmp(got, "flash-user") == 0);
    xfree(got);
    plain = http_session_value(sesh, "user");
    fails += check("session value survives the flash consume",
                   plain != NULL && strcmp(plain, "alice") == 0);

    // render drains everything queued, escaping each message
    http_flash_set(sesh, "one", "first");
    http_flash_set(sesh, "two", "a<b>&c</b>");
    Http_Response res;
    http_response_init(&res);
    size_t shown = http_flash_render(&res, sesh);
    strbuf_null_terminate(&res.body);
    fails += check("render returns how many were shown", shown == 2);
    fails += check("render emits a div per message",
                   count_of(res.body.items, "<div class=\"flash\">") == 2 &&
                   strstr(res.body.items, "<div class=\"flash\">first</div>\n") != NULL);
    fails += check("render escapes message markup",
                   strstr(res.body.items,
                          "<div class=\"flash\">a&lt;b&gt;&amp;c&lt;/b&gt;</div>\n") != NULL);
    shown = http_flash_render(&res, sesh);
    fails += check("second render has nothing left to show", shown == 0);
    fails += check("render consumed every queued flash", http_flash_get(sesh, "one") == NULL);
    http_response_free(&res);

    // an unread flash carries over to a later read instead of vanishing
    http_flash_set(sesh, "later", "still here");
    http_session_set(sesh, "other", "x");
    got = http_flash_get(sesh, "later");
    fails += check("unread flash survives unrelated session writes",
                   got != NULL && strcmp(got, "still here") == 0);
    xfree(got);

    http_session_store_free(&s);

    if (fails == 0) {
        printf("flash ok\n");
    }
    return fails ? 1 : 0;
}