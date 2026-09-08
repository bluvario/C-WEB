#define _POSIX_C_SOURCE 199309L

#include "server.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <ctype.h>
#include <signal.h>
#include <string.h>
#include <time.h>

#include "buffer.h"
#include "http.h"
#include "log.h"
#include "net.h"
#include "request.h"
#include "response.h"
#include "strbuf.h"
#include "sv.h"
#include "thread_pool.h"
#include "xmem.h"

// 64 KiB is plenty for the requests we serve; anything bigger is a 413
// TODO: stream the body to disk instead of hoarding it in RAM
static const size_t REQUEST_BUFFER_CAP = 64 * 1024;

// how long a client may dawdle between bytes without losing the connection
static const unsigned long REQUEST_TIMEOUT_MS = 5000;

// a SIGINT/SIGTERM flips this and the accept loop winds down instead of
// spinning; only the flag write happens in a signal context, which is just
// barely safe enough, and it is the only thing the handler does
static volatile sig_atomic_t g_shutdown = 0;

static void handle_signal(int sig)
{
    (void)sig;
    g_shutdown = 1;
}

static const char *const ERROR_BODY = "cweb error page (it hurts us too)\r\n";

// wall-clock milliseconds for request timing, monotonic so NTP jumps don't
// skew the numbers
static unsigned long long mono_ms(void)
{
#ifdef _WIN32
    return (unsigned long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000 + (unsigned long long)ts.tv_nsec / 1000000;
#endif
}

static void log_request(Http_Request *req, Http_Response *res, unsigned long long elapsed_ms)
{
    const char *method = http_method_name(req->method);
    if (method == NULL) {
        method = "???";
    }
    log_info("%s %.*s -> %d in %llu ms", method,
             (int)req->path.count, req->path.data,
             (int)res->status, elapsed_ms);
}

static void send_wire(Socket_Handle client, Http_Response *res)
{
    Strbuf wire;
    strbuf_init(&wire);
    http_response_serialize(res, &wire);
    if (wire.count > 0) {
        net_send_all(client, wire.items, wire.count);
    }
    strbuf_free(&wire);
}

// streams the response body in chunked framing: the head is serialized like
// any other response (with Transfer-Encoding: chunked), then the callback is
// drained into wire-sized frames until it reports the stream over. a raw
// "0\r\n\r\n" balance frame closes it. for HEAD the metadata is delivered but
// the chunked body is not, mirroring what a GET with a body would send.
static void send_stream(Socket_Handle client, Http_Response *res, bool send_body)
{
    send_wire(client, res); // head only: serialize skips the stream body
    if (!send_body) {
        return;
    }
    char chunk[8192];
    for (;;) {
        size_t n = res->stream_fn(chunk, sizeof(chunk), res->stream_user);
        if (n == 0) {
            break;
        }
        if (n > sizeof(chunk)) {
            break; // corrupt producer, stop rather than buffer forever
        }
        char size_line[32];
        int sl = snprintf(size_line, sizeof(size_line), "%zx\r\n", n);
        net_send_all(client, size_line, (size_t)sl);
        net_send_all(client, chunk, n);
        net_send_all(client, "\r\n", 2);
    }
    net_send_all(client, "0\r\n\r\n", 5);
}

static void send_error(Socket_Handle client, Http_Status status)
{
    Http_Response res;
    http_response_init(&res);
    http_response_set_status(&res, status);
    http_response_set_header(&res, "Content-Type", "text/plain; charset=utf-8");
    http_response_add_body_cstr(&res, ERROR_BODY);
    send_wire(client, &res);
    http_response_free(&res);
}

// pulls the next chunk of request bytes into the read buffer. returns 0 when
// bytes arrived, -1 when the connection is past saving: clean EOF and socket
// errors just close, but a client that stalls mid-request gets a 408 before
// the door shuts.
static int fill_more(Socket_Handle client, Read_Buffer *rb)
{
    String_View head = rb_write_head(rb);
    long n = net_recv(client, (void *)head.data, head.count);
    if (n == NET_READ_TIMEOUT) {
        if (rb->count > 0) {
            log_warn("client stalled mid-request, sending 408");
            send_error(client, HTTP_408_REQUEST_TIMEOUT);
        }
        return -1;
    }
    if (n <= 0) {
        // peer went away (0) or the socket is broken (-1), nothing to answer
        return -1;
    }
    rb_commit(rb, (size_t)n);
    return 0;
}

// does a comma/space separated header value mention this token, any case?
static bool header_has_token(Http_Request *req, const char *name, const char *token)
{
    const String_View *value = http_request_get_header(req, name);
    if (value == NULL) {
        return false;
    }
    size_t n = strlen(token);
    String_View rest = *value;
    while (rest.count > 0) {
        String_View part = sv_chop_by_delim(&rest, ',');
        part = sv_trim(part);
        if (part.count == n) {
            bool same = true;
            for (size_t i = 0; i < n; i++) {
                if (tolower((unsigned char)part.data[i]) != tolower((unsigned char)token[i])) {
                    same = false;
                    break;
                }
            }
            if (same) {
                return true;
            }
        }
    }
    return false;
}

// HTTP/1.1 keeps the connection by default, HTTP/1.0 drops it unless the
// client says keep-alive, and an explicit Connection: close always wins
static bool conn_keep_alive(Http_Request *req)
{
    bool close = header_has_token(req, "connection", "close");
    bool keep = header_has_token(req, "connection", "keep-alive");
    bool http11 = sv_equal(req->version, sv_from_cstr("HTTP/1.1"));
    return !close && (http11 || keep);
}

void http_serve_connection(Socket_Handle client, Http_Handler_Fn handler, void *user_data)
{
    Read_Buffer rb;
    rb_init(&rb, REQUEST_BUFFER_CAP);
    // a dawdling client must not pin a worker forever; this is also the
    // slowloris backstop
    net_set_timeout(client, REQUEST_TIMEOUT_MS);

    for (;;) {
        Request_Parse_Result pr;
        size_t consumed = 0;
        Http_Request req;

        // fill the buffer until a whole request is staged; pipelined bytes
        // already buffered just parse without another recv
        for (;;) {
            pr = http_request_parse_adv(&req, rb_view(&rb), &consumed);
            if (pr == REQ_INCOMPLETE) {
                if (rb.count >= rb.capacity) {
                    log_warn("request exceeds %zu bytes, sending 413", REQUEST_BUFFER_CAP);
                    send_error(client, HTTP_413_PAYLOAD_TOO_LARGE);
                    rb_free(&rb);
                    return;
                }
                if (fill_more(client, &rb) != 0) {
                    rb_free(&rb);
                    return;
                }
                continue;
            }
            if (pr == REQ_ERROR) {
                break;
            }
            // chunked bodies can still be incomplete even though the headers
            // parsed, so decode on every pass and keep reading when the
            // framing calls for bytes that have not arrived yet
            if (http_request_get_header(&req, "transfer-encoding") != NULL) {
                int dr = http_request_decode_chunked(&req, rb.data, rb.count, &consumed);
                if (dr > 0) {
                    http_request_free(&req);
                    if (rb.count >= rb.capacity) {
                        log_warn("request exceeds %zu bytes, sending 413", REQUEST_BUFFER_CAP);
                        send_error(client, HTTP_413_PAYLOAD_TOO_LARGE);
                        rb_free(&rb);
                        return;
                    }
                    if (fill_more(client, &rb) != 0) {
                        rb_free(&rb);
                        return;
                    }
                    continue;
                }
                if (dr < 0) {
                    log_warn("malformed chunked request from client, sending 400");
                    http_request_free(&req);
                    send_error(client, HTTP_400_BAD_REQUEST);
                    break;
                }
            }
            break; // REQ_OK, chunked payload fully unfolded
        }

        if (pr == REQ_ERROR) {
            log_warn("malformed request from client, sending 400");
            send_error(client, HTTP_400_BAD_REQUEST);
            break;
        }

        bool keep = conn_keep_alive(&req);
        bool is_head = req.method == HTTP_HEAD;
        Http_Response res;
        http_response_init(&res);
        unsigned long long t0 = mono_ms();
        handler(&req, &res, user_data);
        unsigned long long elapsed = mono_ms() - t0;
        res.keep_alive = keep;
        log_request(&req, &res, elapsed);
        http_request_free(&req);

        if (res.stream_fn != NULL) {
            send_stream(client, &res, !is_head);
        } else {
            send_wire(client, &res);
        }
        http_response_free(&res);

        rb_discard(&rb, consumed);
        if (!keep) {
            break;
        }
    }

    rb_free(&rb);
}

// one accept -> one job on the worker pool: a slow client must not stall
// everyone else behind it, and the bounded pool caps how many clients can be
// in flight at once. each job owns its socket and is freed by the worker.
typedef struct {
    Socket_Handle client;
    Http_Handler_Fn handler;
    void *user_data;
} Connection_Job;

static void connection_worker(void *arg)
{
    Connection_Job *job = arg;
    log_info("client connected");
    http_serve_connection(job->client, job->handler, job->user_data);
    net_close(job->client);
    log_info("client done");
    xfree(job);
}

int http_serve(Socket_Handle listener, Http_Handler_Fn handler, void *user_data)
{
#ifdef _WIN32
    // TODO: a graceful-stop flag on Windows too, via a console ctrl handler
#else
    // gain the loudest close signals so the loop can drain instead of dying
    // mid-request; the old dispositions are deliberately not restored while
    // the accept loop lives
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
#endif

    Thread_Pool pool;
    if (thread_pool_init(&pool, 0, connection_worker) != 0) {
        log_error("could not start the worker pool");
        return -1;
    }

    for (;;) {
        Socket_Handle client = net_accept(listener);
        if (client == -1) {
#ifndef _WIN32
            if (g_shutdown) {
                break;
            }
#endif
            // TODO: back off on EMFILE instead of spinning
            log_error("accept failed: %s", net_error_string());
            continue;
        }

        Connection_Job *job = xmalloc(sizeof *job);
        job->client = client;
        job->handler = handler;
        job->user_data = user_data;
        if (thread_pool_submit(&pool, job) != 0) {
            // only happens after shutdown, so this accept loop is leaving
            log_error("worker pool shut down, dropping connection");
            net_close(client);
            xfree(job);
            break;
        }
    }

    // stop taking new work is done above; now let in-flight requests finish
    log_info("shutting down: draining %zu queued jobs", pool.queued);
    thread_pool_wait(&pool);
    thread_pool_free(&pool);
    return 0;
}