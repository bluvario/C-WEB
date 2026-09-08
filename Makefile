CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O0 -g
PREFIX  ?= /usr/local

LIB_SRC := src/core/cweb.c src/utils/strbuf.c src/utils/xmem.c src/utils/file.c src/utils/sv.c src/utils/log.c src/utils/strmap.c src/utils/url.c src/utils/base64.c src/utils/buffer.c src/utils/thread.c src/utils/thread_pool.c src/utils/json.c src/utils/uri.c src/security/escape.c src/security/path.c src/security/headers.c src/http/http.c src/http/net.c src/http/request.c src/http/response.c src/http/server.c src/http/date.c src/http/params.c src/http/mime.c src/http/static.c src/http/negotiate.c src/http/cookie.c src/http/multipart.c src/http/auth.c src/http/sse.c src/http/http_client.c src/http/proxy.c src/http/middleware.c src/http/session.c src/http/rate_limit.c src/routing/route.c src/routing/router.c src/template/template.c

TESTS := test_version test_strbuf test_da test_file test_sv test_log test_strmap test_url test_base64 test_buffer test_escape test_path test_http test_request test_response test_date test_params test_mime test_route test_net test_server test_router test_static test_negotiate test_cookie test_multipart test_auth test_security test_thread test_pool test_stream test_shutdown test_sse test_json test_uri test_http_client test_proxy test_template test_middleware test_session test_rate_limit

all: build/libcweb.a

build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Iinclude -c $< -o $@

build/libcweb.a: $(LIB_SRC:%.c=build/%.o)
	$(AR) rcs $@ $^

build/tests/%: tests/%.c build/libcweb.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Iinclude -o $@ $< build/libcweb.a

test: $(TESTS:%=build/tests/%)
	@for t in $(TESTS); do ./build/tests/$$t || exit 1; done

clean:
	rm -rf build

.PHONY: all test clean