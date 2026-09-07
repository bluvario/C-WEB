#include "server.h"

#include "buffer.h"
#include "http.h"
#include "log.h"
#include "net.h"
#include "request.h"
#include "response.h"
#include "strbuf.h"
#include "sv.h"

// 64 KiB is plenty for the requests we serve; anything bigger is a 413
// TODO: stream the body to disk instead of hoarding it in RAM
static const size_t REQUEST_BUFFER_CAP = 64 * 1024;

static const char *const ERROR_BODY = "cweb error page (it hurts us too)\r\n";

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

void http_serve_connection(Socket_Handle client, Http_Handler_Fn handler, void *user_data)
{
    Read_Buffer rb;
    rb_init(&rb, REQUEST_BUFFER_CAP);

    for (;;) {
        String_View head = rb_write_head(&rb);
        long n = net_recv(client, (void *)head.data, head.count);
        if (n <= 0) {
            break; // peer closed or errored, nothing more to say
        }
        rb_commit(&rb, (size_t)n);

        Http_Request req;
        Request_Parse_Result pr = http_request_parse(&req, rb_view(&rb));
        if (pr == REQ_ERROR) {
            log_warn("malformed request from client, sending 400");
            send_error(client, HTTP_400_BAD_REQUEST);
            break;
        }
        if (pr == REQ_INCOMPLETE) {
            if (rb.count >= rb.capacity) {
                log_warn("request exceeds %zu bytes, sending 413", REQUEST_BUFFER_CAP);
                send_error(client, HTTP_413_PAYLOAD_TOO_LARGE);
                break;
            }
            continue; // wait for the rest of the request
        }

        Http_Response res;
        http_response_init(&res);
        handler(&req, &res, user_data);
        http_request_free(&req);

        send_wire(client, &res);
        http_response_free(&res);
        break; // Connection: close, one request per connection
    }

    rb_free(&rb);
}

int http_serve(Socket_Handle listener, Http_Handler_Fn handler, void *user_data)
{
    for (;;) {
        Socket_Handle client = net_accept(listener);
        if (client == -1) {
            // TODO: back off on EMFILE instead of spinning
            log_error("accept failed: %s", net_error_string());
            continue;
        }
        log_info("client connected");
        http_serve_connection(client, handler, user_data);
        net_close(client);
        log_info("client done");
    }
    return 0; // unreachable
}