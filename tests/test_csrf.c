#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csrf.h"
#include "db.h"
#include "response.h"
#include "session.h"
#include "strmap.h"

static int check(const char *what, int cond)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        return 1;
    }
    return 0;
}

static int token_shape_ok(const char *tok)
{
    if (tok == NULL || strlen(tok) != 40) {
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

    Http_Session_Store s;
    http_session_store_init(&s, 0);
    char *token = http_session_create(&s, 0);
    fails += check("session created", token != NULL);
    Http_Session *sesh = http_session_open(&s, token);
    fails += check("session opened", sesh != NULL);

    // the token is generated lazily, shaped like the session tokens, and stays
    const char *tok1 = http_csrf_token(sesh);
    fails += check("token is 40 hex chars", token_shape_ok(tok1));
    fails += check("token is stable across calls", http_csrf_token(sesh) == tok1);

    // a second session gets its own, different token
    char *token2 = http_session_create(&s, 0);
    Http_Session *sesh2 = http_session_open(&s, token2);
    const char *tok2 = http_csrf_token(sesh2);
    fails += check("second session has its own token",
                   tok2 != NULL && strcmp(tok2, tok1) != 0);

    // the hidden input carries the session's token verbatim
    Http_Response res;
    http_response_init(&res);
    http_csrf_field(&res, sesh);
    strbuf_null_terminate(&res.body);
    char want[128];
    snprintf(want, sizeof want,
             "<input type=\"hidden\" name=\"csrf_token\" value=\"%s\">", tok1);
    fails += check("field emits a hidden input with the token",
                   strcmp(res.body.items, want) == 0);
    http_response_free(&res);

    // a matching submitted token verifies, everything else fails clean
    Str_Map params;
    strmap_init(&params);
    strmap_set(&params, sv_from_cstr("csrf_token"), sv_from_cstr(tok1));
    fails += check("matching token verifies", http_csrf_verify(&params, sesh) == 0);

    strmap_set(&params, sv_from_cstr("csrf_token"), sv_from_cstr("deadbeefdeadbeefdeadbeefdeadbeefdeadbeef"));
    fails += check("wrong token is refused", http_csrf_verify(&params, sesh) == -1);

    strmap_delete(&params, sv_from_cstr("csrf_token"));
    fails += check("missing token is refused", http_csrf_verify(&params, sesh) == -1);

    fails += check("no session means no verification", http_csrf_verify(&params, NULL) == -1);
    fails += check("token helper is null-safe", http_csrf_token(NULL) == NULL);

    // a stale session (no token ever served) cannot verify even a present field
    char *token3 = http_session_create(&s, 0);
    Http_Session *sesh3 = http_session_open(&s, token3);
    strmap_set(&params, sv_from_cstr("csrf_token"), sv_from_cstr("anything"));
    fails += check("session without a served token is refused",
                   http_csrf_verify(&params, sesh3) == -1);

    // rotation: the moment a token is replaced the old one stops verifying
    // and a fresh 40-hex token takes its place. the old token may be freed
    // by the store on replace, so hold our own copy.
    const char *before = http_csrf_token(sesh);
    char old_token[64];
    snprintf(old_token, sizeof old_token, "%s", before);
    const char *rotated = http_csrf_rotate(sesh);
    fails += check("rotate returns a fresh 40-hex token", token_shape_ok(rotated));
    fails += check("rotated token differs from the old one",
                   rotated != NULL && strcmp(rotated, old_token) != 0);
    strmap_set(&params, sv_from_cstr("csrf_token"), sv_from_cstr(old_token));
    fails += check("old token no longer verifies after rotation",
                   http_csrf_verify(&params, sesh) == -1);
    strmap_set(&params, sv_from_cstr("csrf_token"), sv_from_cstr(rotated));
    fails += check("rotated token verifies", http_csrf_verify(&params, sesh) == 0);
    fails += check("token is stable after rotation", http_csrf_token(sesh) == rotated);
    {
        const char *stable = http_csrf_token(sesh);
        char stable_copy[64];
        snprintf(stable_copy, sizeof stable_copy, "%s", stable);
        const char *fresh_again = http_csrf_rotate(sesh);
        fails += check("rotate the token twice keeps it fresh",
                       fresh_again != NULL && strcmp(fresh_again, stable_copy) != 0);
        fails += check("the double-rotated token also verifies",
                       http_csrf_token(sesh) == fresh_again);
    }
    fails += check("rotate is null-safe", http_csrf_rotate(NULL) == NULL);

    strmap_free(&params);

    // the reject answer is a 403 with an escaped retry link
    http_response_init(&res);
    http_csrf_reject(&res, "/?x=1&y=<b>");
    strbuf_null_terminate(&res.body);
    fails += check("reject answers 403", res.status == HTTP_403_FORBIDDEN);
    fails += check("reject escapes the retry link",
                   strstr(res.body.items, "href=\"/?x=1&amp;y=&lt;b&gt;\">try again</a>") != NULL);
    fails += check("reject carries html content-type",
                   http_response_has_header(&res, "Content-Type"));
    http_response_free(&res);

    // a rotation is a session write: on a db-backed store the fresh token is
    // mirrored right away, so the rotation survives a crash that never ran a
    // dump -- the fixed pre-login token cannot come back after a restart
    {
        char dir[] = "/tmp/cweb_csrf_db_XXXXXX";
        fails += check("mkdtemp for csrf db", mkdtemp(dir) != NULL);
        char dbpath[256];
        snprintf(dbpath, sizeof dbpath, "%s/csrf.db", dir);

        Cweb_Db db;
        fails += check("csrf db opens", cweb_db_open(&db, dbpath) == 0);
        Http_Session_Store st;
        http_session_store_init(&st, 0);
        http_session_store_set_db(&st, &db);
        char *tok = http_session_create(&st, 0);
        Http_Session *sesh = http_session_open(&st, tok);
        const char *minted = http_csrf_token(sesh);
        char minted_copy[64];
        snprintf(minted_copy, sizeof minted_copy, "%s", minted);
        const char *post_login = http_csrf_rotate(sesh);
        char rotated_copy[64];
        snprintf(rotated_copy, sizeof rotated_copy, "%s", post_login);
        fails += check("rotation on a db-backed store yields a new token",
                       post_login != NULL && strcmp(rotated_copy, minted_copy) != 0);
        char tok_copy[128];
        snprintf(tok_copy, sizeof tok_copy, "%s", tok);
        http_session_store_free(&st); // no dump: the crash
        cweb_db_close(&db);

        Cweb_Db db2;
        fails += check("csrf db reopens", cweb_db_open(&db2, dbpath) == 0);
        Http_Session_Store st2;
        http_session_store_init(&st2, 0);
        http_session_store_set_db(&st2, &db2);
        Http_Session *restored = http_session_open(&st2, tok_copy);
        const char *survivor = http_session_value(restored, "csrf-token");
        fails += check("rotated token survived the crash without a dump",
                       survivor != NULL && strcmp(survivor, rotated_copy) == 0);
        fails += check("pre-login token is gone from the reloaded session",
                       http_session_value(restored, "csrf-token") != NULL &&
                       strcmp(http_session_value(restored, "csrf-token"), minted_copy) != 0);
        // the fresh copy verifies right away after a restart
        Str_Map fresh;
        strmap_init(&fresh);
        strmap_set(&fresh, sv_from_cstr("csrf_token"), sv_from_cstr(survivor));
        fails += check("rotated token verifies after a restart",
                       http_csrf_verify(&fresh, restored) == 0);
        strmap_free(&fresh);

        http_session_store_free(&st2);
        cweb_db_close(&db2);
        unlink(dbpath);
        rmdir(dir);
    }

    http_session_store_free(&s);

    if (fails == 0) {
        printf("csrf ok\n");
    }
    return fails ? 1 : 0;
}