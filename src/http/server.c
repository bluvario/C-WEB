#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "server.h"

#include <ctype.h>
#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buffer.h"
#include "date.h"
#include "gzip.h"
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

// copies one request field into an access-log line, \xHH-encoding bytes that
// would break a single-line record: the quote that delimits the request
// field, the backslash, and any control character a hostile path might carry
static void clf_append_escaped(Strbuf *line, String_View s)
{
    for (size_t i = 0; i < s.count; i++) {
        unsigned char c = (unsigned char)s.data[i];
        if (c < 0x20 || c == 0x7f || c == '"' || c == '\\') {
            char esc[8];
            snprintf(esc, sizeof esc, "\\x%02X", c);
            strbuf_append_cstr(line, esc);
        } else {
            strbuf_append_char(line, (char)c);
        }
    }
}

// one Common Log Format record per completed request: host ident authuser
// [date] "METHOD PATH VERSION" status bytes. nothing is written unless a
// sink was configured with log_set_clf (the --log flag on generated servers).
// bytes is the entity size actually sent; 0 or negative becomes "-".
static void log_access(Http_Request *req, Http_Response *res, long long bytes)
{
    char date[40];
    http_date_clf(time(NULL), date, sizeof date);
    // the host field names the real caller: the trusted-proxy-resolved client
    // when one is known, else the peer address
    String_View who = req->client_ip.count > 0 ? req->client_ip : req->remote;
    if (who.count == 0) {
        who = sv_from_cstr("0.0.0.0");
    }
    Strbuf line;
    strbuf_init(&line);
    strbuf_append(&line, who.data, who.count);
    strbuf_append_cstr(&line, " - - [");
    strbuf_append_cstr(&line, date);
    strbuf_append_cstr(&line, "] \"");
    const char *method = http_method_name(req->method);
    strbuf_append_cstr(&line, method != NULL ? method : "???");
    strbuf_append_char(&line, ' ');
    clf_append_escaped(&line, req->path);
    strbuf_append_char(&line, ' ');
    clf_append_escaped(&line, req->version);
    strbuf_append_cstr(&line, "\" ");
    snprintf(date, sizeof date, "%d", (int)res->status);
    strbuf_append_cstr(&line, date);
    strbuf_append_char(&line, ' ');
    if (bytes > 0) {
        snprintf(date, sizeof date, "%lld", bytes);
        strbuf_append_cstr(&line, date);
    } else {
        strbuf_append_char(&line, '-');
    }
    log_clf_line(line.items, line.count);
    strbuf_free(&line);
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
// any other response (with Transfer-Encoding: chunked), then pushed chunks
// are drained and the callback is fed into wire-sized frames until it reports
// the stream over. a raw "0\r\n\r\n" balance frame closes it. for HEAD the
// metadata is delivered but the chunked body is not, mirroring what a GET
// with a body would send. returns the number of streamed entity bytes (0 for
// HEAD).
static long long send_stream(Socket_Handle client, Http_Response *res, bool send_body)
{
    send_wire(client, res); // head only: serialize skips the stream body
    if (!send_body) {
        return 0;
    }
    long long sent = 0;
    for (size_t i = 0; i < res->stream_chunk_count; i++) {
        Http_Stream_Chunk *c = &res->stream_chunks[i];
        if (c->len == 0) {
            continue;
        }
        sent += (long long)c->len;
        char size_line[32];
        int sl = snprintf(size_line, sizeof(size_line), "%zx\r\n", c->len);
        net_send_all(client, size_line, (size_t)sl);
        net_send_all(client, c->data, c->len);
        net_send_all(client, "\r\n", 2);
    }
    if (res->stream_fn != NULL) {
        char chunk[8192];
        for (;;) {
            size_t n = res->stream_fn(chunk, sizeof(chunk), res->stream_user);
            if (n == 0) {
                break;
            }
            if (n > sizeof(chunk)) {
                break; // corrupt producer, stop rather than buffer forever
            }
            sent += (long long)n;
            char size_line[32];
            int sl = snprintf(size_line, sizeof(size_line), "%zx\r\n", n);
            net_send_all(client, size_line, (size_t)sl);
            net_send_all(client, chunk, n);
            net_send_all(client, "\r\n", 2);
        }
    }
    net_send_all(client, "0\r\n\r\n", 5);
    return sent;
}

// hands the connection over to a raw protocol handler after the response head
// is out: anything the client pipelined past the request (e.g. the first
// WebSocket frame) is dangling in the read buffer, so it moves to the handoff
// as leftover bytes the protocol layer must consume before reading fresh ones.
// the connection carries only this one request: no keep-alive after an upgrade.
static void handle_upgrade(Socket_Handle client, Http_Response *res,
                           Read_Buffer *rb, size_t consumed)
{
    send_wire(client, res);
    String_View buffered = rb_view(rb);
    String_View leftover;
    leftover.data = buffered.data + consumed;
    leftover.count = buffered.count - consumed;
    if (res->upgrade_fn != NULL) {
        res->upgrade_fn(client, leftover, res->upgrade_user);
    }
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
// errors just close, but a client that stalls mid-request gets a 408 (or 504
// when a per-route timeout is active) before the door shuts.
static int fill_more(Socket_Handle client, Read_Buffer *rb,
                     bool route_timeout_active)
{
    String_View head = rb_write_head(rb);
    long n = net_recv(client, (void *)head.data, head.count);
    if (n == NET_READ_TIMEOUT) {
        if (rb->count > 0) {
            Http_Status timeout_status = route_timeout_active
                                             ? HTTP_504_GATEWAY_TIMEOUT
                                             : HTTP_408_REQUEST_TIMEOUT;
            log_warn("client stalled mid-request, sending %d",
                     (int)timeout_status);
            send_error(client, timeout_status);
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

static bool sv_ieq(const char *a, size_t an, const char *b, size_t bn)
{
    if (an != bn) {
        return false;
    }
    for (size_t i = 0; i < an; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) {
            return false;
        }
    }
    return true;
}

// RFC 9110 wants a request that says "Expect: 100-continue" answered with
// an interim 100 Continue before the body is sent, and an unsupported
// expectation with 417 Expectation Failed. the request parser frees a
// request's parsed headers whenever the declared body is still short, so
// this reads the raw header block out of the staging buffer (the range
// req->body_offset covers) rather than the live header array. returns 1
// when the client is holding its body for a 100, -1 when Expect is present
// but names something we do not know, and 0 when there is no Expect header.
static int expect_interest(const char *head, size_t len)
{
    static const char name[] = "Expect:";
    static const char want[] = "100-continue";
    size_t name_len = strlen(name);
    String_View blk = {head, len};
    sv_chop_by_delim(&blk, '\n'); // skip the request line, not a header
    while (blk.count > 0) {
        String_View line = sv_chop_by_delim(&blk, '\n');
        if (line.count >= 2 && line.data[line.count - 2] == '\r') {
            line.count -= 2;
        }
        if (sv_trim(line).count == 0) {
            continue; // blank line: header block is over
        }
        if (line.count < name_len ||
            !sv_ieq(line.data, name_len, name, name_len)) {
            continue;
        }
        String_View value = sv_trim(
            (String_View){line.data + name_len, line.count - name_len});
        if (sv_ieq(value.data, value.count, want, sizeof(want) - 1)) {
            return 1;
        }
        return -1;
    }
    return 0;
}

static void send_continue(Socket_Handle client)
{
    static const char interim[] = "HTTP/1.1 100 Continue\r\n\r\n";
    net_send_all(client, interim, sizeof interim - 1);
}

#ifndef _WIN32
// creates a fresh temp file inside dir for spilling an oversized request
// body and hands back its fd and (heap) path, or -1 when the directory is
// missing/unwritable. the body bytes stream in on the socket side as the
// connection recv()s, so only the return path of http_serve_connection
// "uploads" them.
static int spill_open(const char *dir, char **path_out)
{
    char tmpl[4096];
    int n = snprintf(tmpl, sizeof tmpl, "%s/cweb-body-XXXXXX", dir);
    if (n < 0 || (size_t)n >= sizeof tmpl) {
        return -1;
    }
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        return -1;
    }
    *path_out = strdup(tmpl);
    return fd;
}

// writes every byte to fd, retrying short writes; -1 on error
static int spill_write_all(int fd, const void *data, size_t len)
{
    const char *p = data;
    while (len > 0) {
        long n = write(fd, p, len);
        if (n < 0) {
            return -1;
        }
        if (n == 0) {
            return -1; // shouldn't happen for a regular file
        }
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}
#endif

// caller's address as text, handed to every request on this connection via
// req->remote; "0.0.0.0" stands in when the peer address cannot be resolved
static String_View peer_ip(Socket_Handle client, char buf[INET6_ADDRSTRLEN])
{
#ifdef _WIN32
    (void)client;
    (void)buf;
    return sv_from_cstr("0.0.0.0");
#else
    struct sockaddr_storage ss;
    socklen_t len = sizeof ss;
    if (getpeername((int)client, (struct sockaddr *)&ss, &len) != 0) {
        return sv_from_cstr("0.0.0.0");
    }
    const void *addr = NULL;
    if (ss.ss_family == AF_INET) {
        addr = &((struct sockaddr_in *)&ss)->sin_addr;
    } else if (ss.ss_family == AF_INET6) {
        addr = &((struct sockaddr_in6 *)&ss)->sin6_addr;
    } else {
        return sv_from_cstr("0.0.0.0");
    }
    if (inet_ntop(ss.ss_family, addr, buf, INET6_ADDRSTRLEN) == NULL) {
        return sv_from_cstr("0.0.0.0");
    }
    return sv_from_cstr(buf);
#endif
}

void http_serve_connection_config(Socket_Handle client, Http_Handler_Fn handler,
                                  void *user_data, const Http_Server_Config *cfg)
{
    // hardening knobs, zero meaning "library default"
    size_t max_body = cfg != NULL && cfg->max_body > 0 ? cfg->max_body
                                                       : REQUEST_BUFFER_CAP;
    size_t max_inflated = cfg != NULL && cfg->max_inflated > 0
                              ? cfg->max_inflated
                              : HTTP_GZIP_MAX_INFLATED_DEFAULT;
    unsigned long timeout_ms = cfg != NULL && cfg->io_timeout_ms > 0
                               ? cfg->io_timeout_ms : REQUEST_TIMEOUT_MS;

    Read_Buffer rb;
    rb_init(&rb, max_body);
    // a dawdling client must not pin a worker forever; this is also the
    // slowloris backstop
    net_set_timeout(client, timeout_ms);

    // POSIX-only: request bodies larger than the in-RAM cap spill to a temp
// file in cfg->body_dir and are mapped back, so oversized uploads stream
// through a worker without hoarding RAM. Windows keeps the 413. chunked
// bodies (no Content-Length to budget against) and oversized header blocks
// also keep the 413.
#ifndef _WIN32
    int spill_fd = -1;
    char *spill_path = NULL;
    size_t spill_written = 0;
#endif

    char peer_buf[INET6_ADDRSTRLEN];
    String_View remote = peer_ip(client, peer_buf);
    bool route_timeout_active = false;

    for (;;) {
        Request_Parse_Result pr;
        size_t consumed = 0;
        Http_Request req;
        // a 100 Continue goes out at most once per request, only after the
        // header block parsed and only when the client asked for it
        bool continue_sent = false;

        // fill the buffer until a whole request is staged; pipelined bytes
        // already buffered just parse without another recv
        for (;;) {
#ifndef _WIN32
            // the declared body is streaming into the spill file: keep
            // draining the socket until Content-Length bytes are on disk,
            // then map the file back so req->body reads exactly as usual
            if (spill_fd >= 0) {
                char stage[65536];
                long n = net_recv(client, stage, sizeof stage);
                if (n == NET_READ_TIMEOUT) {
                    if (rb.count > 0) {
                        Http_Status st = route_timeout_active
                                             ? HTTP_504_GATEWAY_TIMEOUT
                                             : HTTP_408_REQUEST_TIMEOUT;
                        log_warn("client stalled mid-body, sending %d",
                                 (int)st);
                        send_error(client, st);
                    }
                    close(spill_fd);
                    unlink(spill_path);
                    xfree(spill_path);
                    http_request_free(&req);
                    rb_free(&rb);
                    return;
                }
                if (n <= 0) {
                    // peer went away mid-body, nothing to answer
                    close(spill_fd);
                    unlink(spill_path);
                    xfree(spill_path);
                    http_request_free(&req);
                    rb_free(&rb);
                    return;
                }
                size_t declared = (size_t)req.content_length;
                size_t take = 0;
                if (spill_written < declared) {
                    take = (size_t)n < declared - spill_written
                               ? (size_t)n : declared - spill_written;
                    if (take > 0) {
                        if (spill_write_all(spill_fd, stage, take) != 0) {
                            log_warn("cannot write spilled body, 500");
                            close(spill_fd);
                            unlink(spill_path);
                            xfree(spill_path);
                            send_error(client, HTTP_500_INTERNAL_SERVER_ERROR);
                            http_request_free(&req);
                            rb_free(&rb);
                            return;
                        }
                        spill_written += take;
                    }
                }
                if (spill_written >= declared) {
                    // bytes past Content-Length belong to the next request
                    size_t surplus = (size_t)n - take;
                    if (surplus > 0) {
                        // body complete: park pipelined bytes for the next
                        // request, right after this request's header block
                        memmove(rb.data + req.body_offset,
                                stage + (size_t)n - surplus, surplus);
                        rb.count = req.body_offset + surplus;
                    }
                    // map the spilled bytes back into req->body
                    void *map = mmap(NULL, spill_written, PROT_READ,
                                     MAP_PRIVATE, spill_fd, 0);
                    close(spill_fd);
                    spill_fd = -1;
                    if (map == MAP_FAILED) {
                        log_warn("cannot map spilled body, 500");
                        unlink(spill_path);
                        xfree(spill_path);
                        send_error(client, HTTP_500_INTERNAL_SERVER_ERROR);
                        http_request_free(&req);
                        rb_free(&rb);
                        return;
                    }
                    req.body = (String_View){map, spill_written};
                    req.body_mmap = map;
                    req.body_mmap_len = spill_written;
                    req.body_file_path = spill_path;
                    spill_path = NULL;
                    consumed = req.body_offset;
                    break; // body mapped, ready to dispatch
                }
                continue; // keep draining the declared body
            }
#endif

            pr = http_request_parse_adv(&req, rb_view(&rb), &consumed);
            if (pr == REQ_INCOMPLETE) {
                // req->body_offset is nonzero once the header block parsed,
                // so the Expect handshake with the client can begin. a client
                // holding its body for the 100 is unblocked the moment this
                // request is accepted; one announcing an expectation this
                // implementation does not support is turned away with 417.
                if (!continue_sent && req.body_offset > 0) {
                    int want = expect_interest(rb.data, req.body_offset);
                    if (want < 0) {
                        log_warn("unsupported Expect header, sending 417");
                        send_error(client, HTTP_417_EXPECTATION_FAILED);
                        http_request_free(&req);
                        rb_free(&rb);
                        return;
                    }
                    if (want > 0) {
                        // a declared body this server would reject anyway is
                        // answered with the final 413 right now, before the
                        // client transmits a single body byte; otherwise the
                        // interim 100 unblocks the sender to proceed
                        if (cfg->body_dir == NULL &&
                            req.content_length > (long long)max_body) {
                            log_warn("request exceeds %zu bytes, sending 413",
                                     max_body);
                            send_error(client, HTTP_413_PAYLOAD_TOO_LARGE);
                            http_request_free(&req);
                            rb_free(&rb);
                            return;
                        }
                        send_continue(client);
                        continue_sent = true;
                    }
                }
                if (rb.count >= rb.capacity) {
#ifndef _WIN32
                    // a declared length past the in-RAM cap starts a spill
                    // when --body-dir is configured; the header block (and
                    // the views borrowed from it) stays put in RAM while the
                    // body streams past it to disk
                    if (cfg->body_dir != NULL &&
                        req.content_length > (long long)max_body &&
                        req.body_offset > 0 && rb.count > req.body_offset) {
                        // the parser released this request's headers when it
                        // saw the body was still short; put them back before
                        // the body streams out to disk so dispatch still sees
                        // Content-Type and friends
                        http_request_parse_header_block(
                            &req, (String_View){rb.data, req.body_offset});
                        spill_fd = spill_open(cfg->body_dir, &spill_path);
                        if (spill_fd < 0) {
                            log_warn("cannot open spill dir %s, 500",
                                     cfg->body_dir);
                            send_error(client, HTTP_500_INTERNAL_SERVER_ERROR);
                            http_request_free(&req);
                            rb_free(&rb);
                            return;
                        }
                        if (spill_write_all(spill_fd,
                                            rb.data + req.body_offset,
                                            rb.count - req.body_offset) != 0) {
                            log_warn("cannot write spilled body, 500");
                            close(spill_fd);
                            unlink(spill_path);
                            xfree(spill_path);
                            send_error(client, HTTP_500_INTERNAL_SERVER_ERROR);
                            http_request_free(&req);
                            rb_free(&rb);
                            return;
                        }
                        spill_written = rb.count - req.body_offset;
                        rb.count = req.body_offset; // headers only in RAM
                        continue; // loop back into the drain arm
                    }
#endif
                    log_warn("request exceeds %zu bytes, sending 413", max_body);
                    send_error(client, HTTP_413_PAYLOAD_TOO_LARGE);
                    http_request_free(&req);
                    rb_free(&rb);
                    return;
                }
                if (fill_more(client, &rb, route_timeout_active) != 0) {
                    http_request_free(&req);
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
                        log_warn("request exceeds %zu bytes, sending 413", max_body);
                        send_error(client, HTTP_413_PAYLOAD_TOO_LARGE);
                        rb_free(&rb);
                        return;
                    }
                    if (fill_more(client, &rb, route_timeout_active) != 0) {
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

        // a body that arrived Content-Encoding: gzip is inflated before the
        // handler sees it, so routes read the real bytes as always. a stream
        // that is not valid gzip is a 400; one that would decompress past the
        // ceiling is a 413 (decompression-bomb backstop).
        if (header_has_token(&req, "content-encoding", "gzip") ||
            header_has_token(&req, "content-encoding", "x-gzip")) {
            unsigned char *infl;
            size_t infl_len;
            int drc = http_gzip_decompress(req.body.data, req.body.count,
                                           max_inflated, &infl, &infl_len);
            if (drc == -1) {
                log_warn("malformed gzip request body, sending 400");
                http_request_free(&req);
                send_error(client, HTTP_400_BAD_REQUEST);
                break;
            }
            if (drc == -2) {
                log_warn("decompressed body exceeds %zu bytes, sending 413",
                         max_inflated);
                http_request_free(&req);
                send_error(client, HTTP_413_PAYLOAD_TOO_LARGE);
                break;
            }
            req.body_heap = infl;
            req.body = (String_View){(const char *)infl, infl_len};
        }

        bool keep = conn_keep_alive(&req);
        bool is_head = req.method == HTTP_HEAD;
        Http_Response res;
        http_response_init(&res);
        req.remote = remote;
        unsigned long long t0 = time_mono_ms();
        handler(&req, &res, user_data);
        unsigned long long elapsed = time_mono_ms() - t0;
        res.keep_alive = keep;
        log_request(&req, &res, elapsed);

        // a raw upgrade (WebSocket etc.): send the 101 head, hand the socket
        // and leftover buffered bytes to the app, then close when it returns.
        // this connection serves no further HTTP requests after the handoff.
        if (res.upgrade_fn != NULL) {
            handle_upgrade(client, &res, &rb, consumed);
            http_request_free(&req);
            http_response_free(&res);
            rb_free(&rb);
            return;
        }

        long long wire_bytes;
        if (res.stream_fn != NULL || res.stream_chunk_count > 0) {
            wire_bytes = send_stream(client, &res, !is_head);
        } else {
            send_wire(client, &res);
            wire_bytes = is_head ? 0 : (long long)res.body.count;
        }
        log_access(&req, &res, wire_bytes);

        // apply the matched route's timeout for the next request on this
        // keep-alive connection so a slow client hits 504 instead of 408
        if (req.route_timeout_ms > 0) {
            net_set_timeout(client, req.route_timeout_ms);
            route_timeout_active = true;
        } else {
            net_set_timeout(client, timeout_ms);
            route_timeout_active = false;
        }

        http_request_free(&req);
        http_response_free(&res);

        rb_discard(&rb, consumed);
        if (!keep) {
            break;
        }
    }

    rb_free(&rb);
}

void http_serve_connection(Socket_Handle client, Http_Handler_Fn handler, void *user_data)
{
    http_serve_connection_config(client, handler, user_data, NULL);
}

// one accept -> one job on the worker pool: a slow client must not stall
// everyone else behind it, and the bounded pool caps how many clients can be
// in flight at once. each job owns its socket and is freed by the worker.
typedef struct {
    Socket_Handle client;
    Http_Handler_Fn handler;
    void *user_data;
    Http_Server_Config cfg; // copied per job so the worker applies it
} Connection_Job;

static void connection_worker(void *arg)
{
    Connection_Job *job = arg;
    log_info("client connected");
    http_serve_connection_config(job->client, job->handler, job->user_data, &job->cfg);
    net_close(job->client);
    log_info("client done");
    xfree(job);
}

int http_serve_config(Socket_Handle listener, Http_Handler_Fn handler,
                      void *user_data, const Http_Server_Config *cfg)
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

    size_t workers = cfg != NULL ? cfg->workers : 0; // 0 = one per core
    Thread_Pool pool;
    if (thread_pool_init(&pool, workers, connection_worker) != 0) {
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
            // fd exhaustion drops the pending connection and the kernel
            // retries the accept; spinning here would peg a core and flood
            // the log, so pause and give a worker time to close a socket
            // before trying again
            if (net_exhausted_fds()) {
                net_pause_ms(100);
            }
            log_error("accept failed: %s", net_error_string());
            continue;
        }

        Connection_Job *job = xmalloc(sizeof *job);
        job->client = client;
        job->handler = handler;
        job->user_data = user_data;
        job->cfg = cfg != NULL ? *cfg : (Http_Server_Config){0};
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

int http_serve(Socket_Handle listener, Http_Handler_Fn handler, void *user_data)
{
    return http_serve_config(listener, handler, user_data, NULL);
}