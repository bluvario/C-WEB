#include "request_id.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "xmem.h"

// one whole-process counter rides on a per-process seed so consecutive ids in
// the process differ and each restart jumps to a fresh range; the seed is
// lazily minted on first use from the clock, the pid, and the address of a
// static that differs per execution and per ASLR placement
static _Atomic unsigned long long rid_seed = 0;
static _Atomic unsigned long long rid_counter = 0;

static unsigned long long rid_next(void)
{
    unsigned long long seed = atomic_load(&rid_seed);
    if (seed == 0) {
        // losing the compare-and-swap just means another worker seeded first;
        // any non-zero seed is a good one
        unsigned long long fresh =
            (unsigned long long)time(NULL) ^
            ((unsigned long long)(unsigned long)getpid() << 32) ^
            (unsigned long long)(uintptr_t)&rid_seed;
        atomic_compare_exchange_weak(&rid_seed, &seed, fresh);
        seed = atomic_load(&rid_seed);
    }
    return atomic_fetch_add_explicit(&rid_counter, 1, memory_order_relaxed) +
           seed;
}

// an inbound X-Request-Id is worth keeping only when it is printable ASCII
// (no whitespace, no control bytes) and short; anything else could be a forged
// log entry or a smuggling payload behind a misconfigured proxy
static int inbound_ok(String_View v)
{
    v = sv_trim(v);
    if (v.count == 0 || v.count > 128) {
        return 0;
    }
    for (size_t i = 0; i < v.count; i++) {
        unsigned char c = (unsigned char)v.data[i];
        if (c < 0x21 || c > 0x7e) {
            return 0;
        }
    }
    return 1;
}

void http_request_id_middleware(Http_Request *req, Http_Response *res,
                                void *user_data,
                                Http_Handler_Fn next, void *next_data)
{
    int honor = user_data != NULL &&
                ((Http_RequestId_Opts *)user_data)->honor_incoming;

    // decide the id before the handler runs so the application and any
    // downstream middleware see it on req->request_id; the copy below is what
    // survives the trampoline (http_request_free releases it)
    String_View chosen = {0};
    if (honor) {
        const String_View *in = http_request_get_header(req, "x-request-id");
        if (in != NULL && inbound_ok(*in)) {
            chosen = sv_trim(*in);
        }
    }
    char minted[17];
    if (chosen.count == 0) {
        snprintf(minted, sizeof minted, "%016llx", rid_next());
        chosen.data = minted;
        chosen.count = 16;
    }

    char *owned = xmalloc(chosen.count + 1);
    memcpy(owned, chosen.data, chosen.count);
    owned[chosen.count] = '\0';
    req->request_id.data = owned;
    req->request_id.count = chosen.count;

    next(req, res, next_data);

    // report the id we actually used, exactly once, no matter what the handler
    // wrote (set_header_replace drops any handler-set X-Request-Id)
    http_response_set_header_replace(res, "X-Request-Id", owned);
}