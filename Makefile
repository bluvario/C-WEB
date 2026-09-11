CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -O0 -g
PREFIX  ?= /usr/local

LIB_SRC := src/core/cweb.c src/utils/strbuf.c src/utils/xmem.c src/utils/file.c src/utils/sv.c src/utils/log.c src/utils/strmap.c src/utils/url.c src/utils/base64.c src/utils/buffer.c src/utils/thread.c src/utils/thread_pool.c src/utils/json.c src/utils/uri.c src/security/escape.c src/security/path.c src/security/headers.c src/http/http.c src/http/net.c src/http/request.c src/http/response.c src/http/server.c src/http/date.c src/http/params.c src/http/mime.c src/http/static.c src/http/negotiate.c src/http/cookie.c src/http/multipart.c src/http/auth.c src/http/sse.c src/http/http_client.c src/http/proxy.c src/http/middleware.c src/http/session.c src/http/rate_limit.c src/http/gzip.c src/http/cors.c src/http/etag.c src/http/flash.c src/http/csrf.c src/http/validate.c src/http/request_sign.c src/http/ws.c src/http/request_id.c src/utils/ip.c src/http/client_ip.c src/routing/route.c src/routing/router.c src/template/template.c src/db/db.c src/security/hmac.c

LIBS := -lz

TESTS := test_version test_strbuf test_da test_file test_sv test_log test_strmap test_url test_base64 test_buffer test_escape test_path test_http test_request test_response test_date test_params test_mime test_route test_net test_server test_router test_static test_negotiate test_cookie test_multipart test_auth test_security test_thread test_pool test_stream test_shutdown test_sse test_json test_uri test_http_client test_proxy test_template test_middleware test_session test_rate_limit test_gzip test_cors test_etag test_db test_notes test_unix test_cli test_login test_flash test_csrf test_validate test_hmac test_sign test_ws test_request_id test_basic_auth test_ip test_client_ip

DEPS := $(shell find include src -name '*.h')

all: build/libcweb.a build/cweb

build/%.o: %.c $(DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Iinclude -c $< -o $@

build/libcweb.a: $(LIB_SRC:%.c=build/%.o)
	$(AR) rcs $@ $^

build/tests/%: tests/%.c build/libcweb.a $(DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Iinclude -o $@ $< build/libcweb.a $(LIBS)

build/cweb: tools/cweb.c build/libcweb.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Iinclude -o $@ tools/cweb.c build/libcweb.a $(LIBS)

# the login example, compiled and linked entirely through the cweb CLI
build/examples/login/server: build/cweb $(wildcard examples/login/views/*.c.html) $(wildcard examples/login/views/partials/*.c.html) $(wildcard examples/login/static/*)
	./build/cweb build examples/login/views build/examples/login . examples/login/static

# the notes example rides every server feature: layout wrapper, persistent
# --db store, --secure hardening and a static mount
build/examples/notes/server: build/cweb $(wildcard examples/notes/views/*.c.html) $(wildcard examples/notes/views/partials/*.c.html) $(wildcard examples/notes/static/*)
	./build/cweb build examples/notes/views build/examples/notes . examples/notes/static

examples: build/examples/login/server build/examples/notes/server
	@./build/examples/login/server --routes
	@echo "run it with:       ./build/examples/login/server --port 8080"
	@./build/examples/notes/server --routes
	@echo "run it with:       ./build/examples/notes/server --port 8080 --db notes.db --secure"

test: build/cweb $(TESTS:%=build/tests/%)
	@for t in $(TESTS); do ./build/tests/$$t || exit 1; done

clean:
	rm -rf build

.PHONY: all test clean examples