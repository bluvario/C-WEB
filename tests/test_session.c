#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "db.h"
#include "request.h"
#include "response.h"
#include "session.h"
#include "sv.h"

static int check(const char *what, int cond)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        return 1;
    }
    return 0;
}

// tokens are the hex of 20 random bytes, guaranteed safe for a cookie value
static int token_shape_ok(const char *tok)
{
    if (strlen(tok) != 40) {
        return 0;
    }
    for (size_t i = 0; i < 40; i++) {
        char c = tok[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

int main(void)
{
    int fails = 0;

    // create + open round trip
    {
        Http_Session_Store s;
        http_session_store_init(&s, 0);
        char *t1 = http_session_create(&s, 0);
        fails += check("create hands back a token", t1 != NULL);
        fails += check("token is 40 hex chars", token_shape_ok(t1));
        Http_Session *a = http_session_open(&s, t1);
        fails += check("open resolves the token", a != NULL);
        char *t2 = http_session_create(&s, 0);
        fails += check("tokens are unique", t1 != NULL && t2 != NULL && strcmp(t1, t2) != 0);
        fails += check("two sessions tracked", s.count == 2);
        fails += check("unknown token is NULL",
            http_session_open(&s, "deadbeefdeadbeefdeadbeefdeadbeefdeadbeef") == NULL);
        http_session_store_free(&s);
    }

    // data survives in the store, values are copies, sessions are isolated
    {
        Http_Session_Store s;
        http_session_store_init(&s, 0);
        char *t1 = http_session_create(&s, 0);
        char *t2 = http_session_create(&s, 0);
        Http_Session *a = http_session_open(&s, t1);
        Http_Session *b = http_session_open(&s, t2);
        http_session_set(a, "user", "alice");
        http_session_set(a, "theme", "dark");
        char scratch[32];
        strcpy(scratch, "replaced");
        http_session_set(a, "theme", scratch);
        strcpy(scratch, "mutated-outside");
        fails += check("value is a stored copy", http_session_value(a, "user") &&
            strcmp(http_session_value(a, "user"), "alice") == 0);
        fails += check("later set overwrites",
            strcmp(http_session_value(a, "theme"), "replaced") == 0);
        fails += check("other session does not leak alice's data",
            http_session_value(b, "user") == NULL);
        fails += check("missing key is NULL", http_session_value(a, "nope") == NULL);

        // now the real isolation proof: mutate b, a must stay pristine
        http_session_set(b, "user", "bob");
        fails += check("sessions stay isolated",
            strcmp(http_session_value(a, "user"), "alice") == 0);
        fails += check("bob's session sees its own data",
            strcmp(http_session_value(b, "user"), "bob") == 0);
        http_session_store_free(&s);
    }

    // destroy, swap-remove reuse, and a growth+shrink stress loop
    {
        Http_Session_Store s;
        http_session_store_init(&s, 0);
        char *kept[20];
        char *gone[4];
        for (int i = 0; i < 20; i++) {
            kept[i] = http_session_create(&s, 0);
        }
        fails += check("store grew past the initial capacity", s.count == 20);

        gone[0] = kept[4];
        gone[1] = kept[9];
        gone[2] = kept[15];
        gone[3] = kept[19];
        char gone_copy[4][64];
        for (int i = 0; i < 4; i++) {
            strcpy(gone_copy[i], gone[i]);
            http_session_destroy(&s, gone[i]);
        }
        fails += check("destroy removed exactly those four", s.count == 16);
        for (int i = 0; i < 4; i++) {
            fails += check("destroyed token no longer opens",
                http_session_open(&s, gone_copy[i]) == NULL);
        }
        for (int i = 0; i < 20; i++) {
            if (kept[i] != gone[0] && kept[i] != gone[1] &&
                kept[i] != gone[2] && kept[i] != gone[3]) {
                fails += check("surviving token still opens",
                    http_session_open(&s, kept[i]) != NULL);
            }
        }

        // destroy an unknown token is a no-op
        http_session_destroy(&s, "nonexistenttoken");
        fails += check("destroy of unknown token is a no-op", s.count == 16);

        // hammer create/destroy so slots recycle through the swap-remove path
        for (int i = 0; i < 60; i++) {
            char *t = http_session_create(&s, 0);
            http_session_destroy(&s, t);
        }
        fails += check("stress loop keeps the survivors only", s.count == 16);
        char *again = http_session_create(&s, 0);
        fails += check("store still usable after the churn",
            again != NULL && http_session_open(&s, again) != NULL);
        http_session_store_free(&s);
    }

    // expiry: lazy drop on contact, and reap sweeps in bulk
    {
        Http_Session_Store s;
        http_session_store_init(&s, 0);
        char *t_soon = http_session_create(&s, 1);
        char *t_kept = http_session_create(&s, 3600);
        fails += check("fresh session opens", http_session_open(&s, t_soon) != NULL);

        // white-box: move both deadlines into the past without sleeping
        Http_Session *a = http_session_open(&s, t_soon);
        a->expires = 1;
        fails += check("expired token refuses to open", http_session_open(&s, t_soon) == NULL);
        fails += check("contact with a stale token drops it", s.count == 1);

        char *t_soon2 = http_session_create(&s, 1);
        Http_Session *x = http_session_open(&s, t_soon2);
        x->expires = 1;
        http_session_reap(&s);
        fails += check("reap swept the stale one", s.count == 1);
        fails += check("live session survived the sweep",
            http_session_open(&s, t_kept) != NULL);
        http_session_store_free(&s);
    }

    // cookie glue: load from the request header
    {
        Http_Session_Store s;
        http_session_store_init(&s, 0);
        char *tok = http_session_create(&s, 0);

        Http_Request req;
        char raw[256];
        snprintf(raw, sizeof raw,
                 "GET / HTTP/1.1\r\nHost: x\r\nCookie: sid=%s; theme=dark\r\n\r\n", tok);
        fails += check("request parses",
            http_request_parse(&req, sv_from_cstr(raw)) == REQ_OK);
        Http_Session *sesh = http_session_from_cookie(&s, &req, "sid");
        fails += check("session loads from its cookie", sesh != NULL);
        http_session_set(sesh, "user", "alice");
        fails += check("loaded session carries data",
            strcmp(http_session_value(sesh, "user"), "alice") == 0);

        fails += check("a different cookie name resolves to nothing",
            http_session_from_cookie(&s, &req, "theme") == NULL);
        fails += check("an unknown cookie name is NULL",
            http_session_from_cookie(&s, &req, "nope") == NULL);
        http_request_free(&req);

        Http_Request bare;
        fails += check("no cookie header parses",
            http_request_parse(&bare, sv_from_cstr("GET / HTTP/1.1\r\nHost: x\r\n\r\n")) == REQ_OK);
        fails += check("missing cookie header is NULL",
            http_session_from_cookie(&s, &bare, "sid") == NULL);
        http_request_free(&bare);

        http_session_store_free(&s);
    }

    // cookie glue: write the Set-Cookie back out
    {
        Http_Response res;
        http_response_init(&res);
        http_session_issue_cookie(&res, "sid", "abc123", NULL);
        fails += check("default cookie is HttpOnly under /",
            strstr(res.headers.items, "Set-Cookie: sid=abc123; Path=/; HttpOnly\r\n") != NULL);

        http_response_free(&res);
        http_response_init(&res);
        Cookie_Attrs attrs = {.path = "/app", .max_age = 3600, .secure = true};
        http_session_issue_cookie(&res, "sid", "abc123", &attrs);
        fails += check("custom attrs land on the wire",
            strstr(res.headers.items,
                   "Set-Cookie: sid=abc123; Path=/app; Max-Age=3600; Secure\r\n") != NULL);
        http_response_free(&res);
    }

    // persistent store: a store's sessions survive a store free and a second
    // store loading the same db file, and destroying removes the record
    {
        char dir[] = "/tmp/cweb_session_db_XXXXXX";
        fails += check("mkdtemp for session db", mkdtemp(dir) != NULL);
        char dbpath[256];
        snprintf(dbpath, sizeof dbpath, "%s/sessions.db", dir);

        Cweb_Db db;
        fails += check("db opens", cweb_db_open(&db, dbpath) == 0);

        Http_Session_Store s;
        http_session_store_init(&s, 0);
        http_session_store_set_db(&s, &db);
        char *tok = http_session_create(&s, 0);
        fails += check("created session with backing db", tok != NULL);
        Http_Session *sesh = http_session_open(&s, tok);
        http_session_set(sesh, "user", "alice");
        http_session_store_dump(&s);

        // the record is on disk under the session- prefixed key
        char tok_copy[128];
        snprintf(tok_copy, sizeof tok_copy, "%s", tok);
        char key[160];
        snprintf(key, sizeof key, "session-%s", tok_copy);
        fails += check("record mirrored to db",
            cweb_db_get(&db, sv_from_cstr(key)).data != NULL);

        // a fresh store on the same db finds the session again
        http_session_store_free(&s);
        Http_Session_Store s2;
        http_session_store_init(&s2, 0);
        http_session_store_set_db(&s2, &db);
        Http_Session *loaded = http_session_open(&s2, tok_copy);
        fails += check("persisted session loads into a fresh store", loaded != NULL);
        fails += check("persisted data is restored",
            http_session_value(loaded, "user") != NULL &&
            strcmp(http_session_value(loaded, "user"), "alice") == 0);

        // destroying removes the record so it cannot be resurrected
        http_session_destroy(&s2, tok_copy);
        fails += check("destroyed session's record is deleted",
            cweb_db_get(&db, sv_from_cstr(key)).data == NULL);

        http_session_store_free(&s2);
        cweb_db_close(&db);
        unlink(dbpath);
        rmdir(dir);
    }

    if (fails == 0) {
        printf("session ok\n");
    }
    return fails != 0;
}