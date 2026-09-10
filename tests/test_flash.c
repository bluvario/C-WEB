#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "db.h"
#include "flash.h"
#include "response.h"
#include "session.h"
#include "sv.h"
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

    // flash consumption is mirrored to a backing db: another copy of the
    // session (e.g. after the server crashes and restarts) must not replay a
    // flash that was already read or rendered
    {
        char dir[] = "/tmp/cweb_flash_db_XXXXXX";
        fails += check("mkdtemp for flash db", mkdtemp(dir) != NULL);
        char dbpath[256];
        snprintf(dbpath, sizeof dbpath, "%s/flash.db", dir);

        Cweb_Db db;
        fails += check("db opens", cweb_db_open(&db, dbpath) == 0);

        Http_Session_Store s;
        http_session_store_init(&s, 0);
        http_session_store_set_db(&s, &db);
        char *tok = http_session_create(&s, 0);
        fails += check("db-backed session created", tok != NULL);
        Http_Session *sesh = http_session_open(&s, tok);

        // flash_get consumption must be durable
        http_flash_set(sesh, "read", "consumed by get");
        char *got = http_flash_get(sesh, "read");
        fails += check("flash read before the crash", got != NULL &&
                       strcmp(got, "consumed by get") == 0);
        xfree(got);
        char tok_copy[128];
        snprintf(tok_copy, sizeof tok_copy, "%s", tok);
        http_session_store_free(&s); // the crash: no dump

        Http_Session_Store s2;
        http_session_store_init(&s2, 0);
        http_session_store_set_db(&s2, &db);
        Http_Session *re = http_session_open(&s2, tok_copy);
        fails += check("session reloaded after the crash", re != NULL);
        got = http_flash_get(re, "read");
        fails += check("consumed flash did not replay after the crash",
            got == NULL);
        xfree(got);

        // render consumption is durable too
        http_flash_set(re, "shown", "consumed by render");
        Http_Response res;
        http_response_init(&res);
        size_t shown = http_flash_render(&res, re);
        strbuf_null_terminate(&res.body);
        fails += check("render shows the pending flash", shown == 1 &&
                       strstr(res.body.items, "consumed by render") != NULL);
        http_response_free(&res);
        http_session_store_free(&s2); // crash #2, again no dump

        Http_Session_Store s3;
        http_session_store_init(&s3, 0);
        http_session_store_set_db(&s3, &db);
        Http_Session *re3 = http_session_open(&s3, tok_copy);
        Http_Response res2;
        http_response_init(&res2);
        fails += check("rendered flash did not replay after the crash",
            re3 != NULL && http_flash_render(&res2, re3) == 0);
        http_response_free(&res2);
        http_session_store_free(&s3);
        cweb_db_close(&db);
        unlink(dbpath);
        rmdir(dir);
    }

    if (fails == 0) {
        printf("flash ok\n");
    }
    return fails ? 1 : 0;
}