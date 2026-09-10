#include <stdio.h>
#include <string.h>

#include "base64.h"
#include "request.h"
#include "response.h"
#include "rate_limit.h"
#include "strbuf.h"
#include "sv.h"

static int check(const char *what, int cond)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        return 1;
    }
    return 0;
}

// a fake wall for the limiter's injectable clock: tests fast-forward time
// instead of sleeping
static unsigned long long fake_now;
static unsigned long long fake_clock(void)
{
    return fake_now;
}

typedef struct {
    int *ran;
} Fixture;

static void final_handler(Http_Request *req, Http_Response *res, void *user_data)
{
    (void)req;
    (void)res;
    Fixture *f = user_data;
    (*f->ran)++;
}

static String_View key_by_header(Http_Request *req, void *user_data)
{
    (void)user_data;
    const String_View *v = http_request_get_header(req, "x-api-key");
    return v != NULL ? *v : sv_from_cstr("anon");
}

int main(void)
{
    int fails = 0;

    // burst admits exactly burst instant requests, then slams the door
    {
        Http_RateLimiter rl;
        http_rate_limiter_init(&rl, 1.0, 3.0);
        fake_now = 1000;
        rl.clock_ms = fake_clock;
        String_View k = sv_from_cstr("/a");
        fails += check("burst request 1 allowed", http_rate_limiter_allow(&rl, k) == 0);
        fails += check("burst request 2 allowed", http_rate_limiter_allow(&rl, k) == 0);
        fails += check("burst request 3 allowed", http_rate_limiter_allow(&rl, k) == 0);
        fails += check("burst request 4 denied", http_rate_limiter_allow(&rl, k) == -1);
        fails += check("bucket was charged once", rl.count == 1);
        http_rate_limiter_free(&rl);
    }

    // tokens refill at the configured rate over time
    {
        Http_RateLimiter rl;
        http_rate_limiter_init(&rl, 2.0, 5.0);
        fake_now = 0;
        rl.clock_ms = fake_clock;
        String_View k = sv_from_cstr("/t");
        for (int i = 0; i < 5; i++) {
            fails += check("drain ok", http_rate_limiter_allow(&rl, k) == 0);
        }
        fails += check("drained bucket denies", http_rate_limiter_allow(&rl, k) == -1);

        fake_now = 1000; // one second of idle at 2/s
        fails += check("one second refills two tokens",
            http_rate_limiter_allow(&rl, k) == 0 &&
            http_rate_limiter_allow(&rl, k) == 0);
        fails += check("and no more", http_rate_limiter_allow(&rl, k) == -1);

        fake_now = 1500; // +.5s at 2/s = 1 token
        fails += check("half a second refills one token",
            http_rate_limiter_allow(&rl, k) == 0);
        fails += check("and no more", http_rate_limiter_allow(&rl, k) == -1);
        http_rate_limiter_free(&rl);
    }

    // buckets never exceed burst, even after a long holiday
    {
        Http_RateLimiter rl;
        http_rate_limiter_init(&rl, 1.0, 4.0);
        fake_now = 0;
        rl.clock_ms = fake_clock;
        String_View k = sv_from_cstr("/c");
        for (int i = 0; i < 4; i++) {
            http_rate_limiter_allow(&rl, k);
        }
        fake_now = 3 * 3600 * 1000ULL; // three hours later
        int allowed = 0;
        while (http_rate_limiter_allow(&rl, k) == 0 && allowed < 10) {
            allowed++;
        }
        fails += check("idle time caps at burst", allowed == 4);
        http_rate_limiter_free(&rl);
    }

    // keys are independent
    {
        Http_RateLimiter rl;
        http_rate_limiter_init(&rl, 1.0, 1.0);
        fake_now = 0;
        rl.clock_ms = fake_clock;
        String_View a = sv_from_cstr("/a");
        String_View b = sv_from_cstr("/b");
        fails += check("a spends its only token", http_rate_limiter_allow(&rl, a) == 0);
        fails += check("a is now dry", http_rate_limiter_allow(&rl, a) == -1);
        fails += check("b still has its own token", http_rate_limiter_allow(&rl, b) == 0);
        fails += check("keys tracked separately", rl.count == 2);
        http_rate_limiter_free(&rl);
    }

    // clear resets every bucket
    {
        Http_RateLimiter rl;
        http_rate_limiter_init(&rl, 1.0, 2.0);
        fake_now = 0;
        rl.clock_ms = fake_clock;
        String_View k = sv_from_cstr("/r");
        http_rate_limiter_allow(&rl, k);
        http_rate_limiter_allow(&rl, k);
        fails += check("drained before clear", http_rate_limiter_allow(&rl, k) == -1);
        http_rate_limiter_clear(&rl);
        fails += check("clear empties the table", rl.count == 0);
        fails += check("clear restores a full bucket",
            http_rate_limiter_allow(&rl, k) == 0);
        http_rate_limiter_free(&rl);
    }

    // the middleware: a 429 for a dry default-key bucket, Retry-After hint,
    // the rest of the chain skipped, then a refill lets the request through
    {
        Http_RateLimiter rl;
        http_rate_limiter_init(&rl, 2.0, 2.0);
        fake_now = 0;
        rl.clock_ms = fake_clock;

        Http_Middleware_Chain c;
        http_middleware_init(&c);
        http_middleware_add(&c, http_rate_limit_middleware, &rl);
        int ran = 0;
        Fixture f = {&ran};
        void *data = NULL;
        Http_Handler_Fn fn = http_middleware_build(&c, final_handler, &f, &data);

        Http_Request req;
        memset(&req, 0, sizeof req);
        req.path = sv_from_cstr("/x");
        Http_Response res;
        http_response_init(&res);

        fn(&req, &res, data);
        fails += check("first request passes", ran == 1);
        // on the first allowed request: bucket went from burst=2 → 1
        {
            Http_RateLimit_Status st;
            http_rate_limiter_status(&rl, sv_from_cstr("/x"), &st);
            fails += check("first: limit=2", st.limit == 2);
            fails += check("first: remaining=1", st.remaining == 1);
        }
        fn(&req, &res, data);
        fails += check("second request passes", ran == 2);
        // after second allow: bucket went from 1 → 0
        {
            Http_RateLimit_Status st;
            http_rate_limiter_status(&rl, sv_from_cstr("/x"), &st);
            fails += check("second: remaining=0", st.remaining == 0);
        }
        fn(&req, &res, data);
        fails += check("third request is cut off", ran == 2);
        fails += check("denied request is 429",
            res.status == HTTP_429_TOO_MANY_REQUESTS);
        fails += check("429 carries Retry-After",
            strstr(res.headers.items, "Retry-After: 1\r\n") != NULL);
        fails += check("429 carries X-RateLimit-Limit",
            strstr(res.headers.items, "X-RateLimit-Limit: 2\r\n") != NULL);
        fails += check("429 carries X-RateLimit-Remaining 0",
            strstr(res.headers.items, "X-RateLimit-Remaining: 0\r\n") != NULL);
        fails += check("429 carries X-RateLimit-Reset",
            strstr(res.headers.items, "X-RateLimit-Reset: 1\r\n") != NULL);

        http_response_free(&res);
        http_response_init(&res);
        fake_now = 500; // .5s at 2/s refills one token
        fn(&req, &res, data);
        fails += check("refilled request passes", ran == 3);
        fails += check("refilled response is untouched", res.status == HTTP_200_OK);
        fails += check("refilled carries X-RateLimit-Limit",
            strstr(res.headers.items, "X-RateLimit-Limit: 2\r\n") != NULL);
        fails += check("refilled carries X-RateLimit-Remaining 0",
            strstr(res.headers.items, "X-RateLimit-Remaining: 0\r\n") != NULL);
        fails += check("refilled carries X-RateLimit-Reset",
            strstr(res.headers.items, "X-RateLimit-Reset: 1\r\n") != NULL);

        http_response_free(&res);
        http_middleware_data_free(data);
        http_middleware_free(&c);
        http_rate_limiter_free(&rl);
    }

    // a custom key_fn charges calls per API key instead of per path
    {
        Http_RateLimiter rl;
        http_rate_limiter_init(&rl, 1.0, 1.0);
        fake_now = 0;
        rl.clock_ms = fake_clock;
        rl.key_fn = key_by_header;

        Http_Request req;
        fails += check("parse alice request",
            http_request_parse(&req, sv_from_cstr(
                "GET /a HTTP/1.1\r\nHost: x\r\nX-Api-Key: alice\r\n\r\n")) == REQ_OK);
        String_View k = key_by_header(&req, NULL);
        fails += check("key reads the header", sv_equal(k, sv_from_cstr("alice")));
        fails += check("alice allowed", http_rate_limiter_allow(&rl, k) == 0);
        fails += check("alice denied second time", http_rate_limiter_allow(&rl, k) == -1);
        fails += check("bob is a separate bucket",
            http_rate_limiter_allow(&rl, sv_from_cstr("bob")) == 0);
        http_request_free(&req);
        http_rate_limiter_free(&rl);
    }

    // http_rate_limit_user_key: an already-verified req->auth_user owns the
    // bucket (one account, however many addresses)
    {
        Http_RateLimiter rl;
        http_rate_limiter_init(&rl, 1.0, 2.0);
        fake_now = 0;
        rl.clock_ms = fake_clock;
        rl.key_fn = http_rate_limit_user_key;

        String_View u = sv_from_cstr("alice");
        fails += check("alice has a full bucket",
            http_rate_limiter_allow(&rl, u) == 0 &&
            http_rate_limiter_allow(&rl, u) == 0);
        fails += check("alice's budget is spent",
            http_rate_limiter_allow(&rl, u) == -1);

        String_View other = sv_from_cstr("bob");
        fails += check("bob is a separate bucket",
            http_rate_limiter_allow(&rl, other) == 0);
        http_rate_limiter_free(&rl);
    }

    // http_rate_limit_user_key: a claiming Basic header throttles the account
    // *before* the password is verified, so a wrong-password guess burns the
    // account's bucket even from a fresh caller address
    {
        Http_RateLimiter rl;
        http_rate_limiter_init(&rl, 1.0, 2.0);
        fake_now = 0;
        rl.clock_ms = fake_clock;
        rl.key_fn = http_rate_limit_user_key;

        // the wire credentials "alice:guess" (the password does not matter
        // here; the header alone names the account)
        char auth[128];
        {
            Strbuf s;
            strbuf_init(&s);
            base64_encode_into(&s, sv_from_cstr("alice:guess"));
            size_t n = s.count < sizeof auth - 1 ? s.count : sizeof auth - 1;
            memcpy(auth, s.items, n);
            auth[n] = '\0';
            strbuf_free(&s);
        }
        char alice_raw[512];
        snprintf(alice_raw, sizeof alice_raw,
                 "GET / HTTP/1.1\r\nHost: x\r\nAuthorization: Basic %s\r\n\r\n",
                 auth);

        Http_Request req;
        fails += check("parse alice request",
            http_request_parse(&req, sv_from_cstr(alice_raw)) == REQ_OK);
        req.remote = sv_from_cstr("10.0.0.1");

        String_View k = http_rate_limit_user_key(&req, NULL);
        fails += check("key is the claimed username, not the IP",
            sv_equal(k, sv_from_cstr("alice")));
        fails += check("guess 1 draws from the account bucket",
            http_rate_limiter_allow(&rl, k) == 0);
        fails += check("guess 2 draws from the account bucket",
            http_rate_limiter_allow(&rl, k) == 0);
        fails += check("guess 3 is throttled",
            http_rate_limiter_allow(&rl, k) == -1);

        // the same account from a different address still shares that bucket
        {
            Http_Request q;
            fails += check("parse alice request again",
                http_request_parse(&q, sv_from_cstr(alice_raw)) == REQ_OK);
            q.remote = sv_from_cstr("10.0.0.99");
            String_View k2 = http_rate_limit_user_key(&q, NULL);
            fails += check("key is still the account", sv_equal(k2, k));
            fails += check("fresh IP does not reroll alice's bucket",
                http_rate_limiter_allow(&rl, k2) == -1);
            http_request_free(&q);
        }

        // a different account's bucket stays full even on the same machine
        {
            char bob_raw[512];
            char bobby[128];
            Strbuf s;
            strbuf_init(&s);
            base64_encode_into(&s, sv_from_cstr("bob:guess"));
            size_t n = s.count < sizeof bobby - 1 ? s.count : sizeof bobby - 1;
            memcpy(bobby, s.items, n);
            bobby[n] = '\0';
            strbuf_free(&s);
            snprintf(bob_raw, sizeof bob_raw,
                     "GET / HTTP/1.1\r\nHost: x\r\nAuthorization: Basic %s\r\n\r\n",
                     bobby);

            Http_Request q;
            fails += check("parse bob request",
                http_request_parse(&q, sv_from_cstr(bob_raw)) == REQ_OK);
            q.remote = sv_from_cstr("10.0.0.1");
            String_View k3 = http_rate_limit_user_key(&q, NULL);
            fails += check("bob claims his own bucket",
                sv_equal(k3, sv_from_cstr("bob")));
            fails += check("bob is allowed while alice is dry",
                http_rate_limiter_allow(&rl, k3) == 0);
            http_request_free(&q);
        }

        http_request_free(&req);
        http_rate_limiter_free(&rl);
    }

    // anonymous requests without credentials fall back to the caller address
    {
        Http_RateLimiter rl;
        http_rate_limiter_init(&rl, 1.0, 1.0);
        fake_now = 0;
        rl.clock_ms = fake_clock;
        rl.key_fn = http_rate_limit_user_key;

        Http_Request a, b;
        fails += check("parse anon request a",
            http_request_parse(&a, sv_from_cstr(
                "GET / HTTP/1.1\r\nHost: x\r\n\r\n")) == REQ_OK);
        fails += check("parse anon request b",
            http_request_parse(&b, sv_from_cstr(
                "GET / HTTP/1.1\r\nHost: x\r\n\r\n")) == REQ_OK);
        a.remote = sv_from_cstr("10.1.1.1");
        b.remote = sv_from_cstr("10.1.1.2");
        fails += check("anon key is the caller address",
            sv_equal(http_rate_limit_user_key(&a, NULL),
                     sv_from_cstr("10.1.1.1")));
        fails += check("anon request a allowed",
            http_rate_limiter_allow(&rl, http_rate_limit_user_key(&a, NULL)) == 0);
        fails += check("anon request b has its own bucket",
            http_rate_limiter_allow(&rl, http_rate_limit_user_key(&b, NULL)) == 0);
        http_request_free(&a);
        http_request_free(&b);
        http_rate_limiter_free(&rl);
    }

    // status() reports the bucket state without consuming a token
    {
        Http_RateLimiter rl;
        http_rate_limiter_init(&rl, 2.0, 3.0);
        fake_now = 0;
        rl.clock_ms = fake_clock;
        String_View k = sv_from_cstr("/s");
        Http_RateLimit_Status st;

        http_rate_limiter_status(&rl, k, &st);
        fails += check("no bucket: remaining=burst", st.remaining == 3);
        fails += check("no bucket: reset=0", st.reset == 0);

        http_rate_limiter_allow(&rl, k); // 3→2
        http_rate_limiter_status(&rl, k, &st);
        fails += check("after allow: remaining=2", st.remaining == 2);
        fails += check("after allow: limit=3", st.limit == 3);
        fails += check("after allow: reset=1", st.reset == 1); // ceil((3-2)/2)=1

        http_rate_limiter_allow(&rl, k); // 2→1
        http_rate_limiter_status(&rl, k, &st);
        fails += check("second allow: remaining=1", st.remaining == 1);

        http_rate_limiter_allow(&rl, k); // 1→0
        http_rate_limiter_status(&rl, k, &st);
        fails += check("drained: remaining=0", st.remaining == 0);
        fails += check("drained: reset=2", st.reset == 2); // ceil((3-0)/2)=2

        // status() must not consume a token: three consecutive calls leave
        // the bucket untouched
        http_rate_limiter_status(&rl, k, &st);
        http_rate_limiter_status(&rl, k, &st);
        http_rate_limiter_status(&rl, k, &st);
        fails += check("status is idempotent", st.remaining == 0);

        http_rate_limiter_free(&rl);
    }

    if (fails == 0) {
        printf("rate_limit ok\n");
    }
    return fails != 0;
}