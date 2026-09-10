#ifndef CWEB_WS_H
#define CWEB_WS_H

#include <stdbool.h>
#include <stdint.h>

#include "buffer.h"
#include "net.h"
#include "request.h"
#include "response.h"
#include "sv.h"

// SHA-1 (FIPS 180-1), exposed only because RFC 6455's handshake derives the
// Sec-WebSocket-Accept value from it; semantics match OpenSSL's SHA1(). the
// rest of the framework uses SHA-256 (hmac.h). writes 20 bytes into out.
void http_ws_sha1(const uint8_t *data, size_t len, uint8_t out[20]);

// server-side WebSocket (RFC 6455) support. a route handler turns a normal
// GET request into an upgrade with http_ws_upgrade(); the server replies 101
// and hands the raw socket to the app through Http_Ws_Conn.

// frame opcodes
#define WS_OP_CONT 0x0  // continuation of a fragmented message
#define WS_OP_TEXT 0x1  // utf-8 text message
#define WS_OP_BIN 0x2   // binary message
#define WS_OP_CLOSE 0x8 // close handshake; low 16 payload bits are a code
#define WS_OP_PING 0x9  // peer liveness probe, must answer PONG
#define WS_OP_PONG 0xa  // answer to PING

// one parsed frame from the wire: opcode, FIN flag, and up to payload_cap
// bytes of decoded (unmasked) payload copied into the caller's buffer.
typedef struct {
    bool fin;
    uint8_t opcode;
    uint64_t payload_len; // bytes delivered in payload[]
} Http_Ws_Frame;

// a live WebSocket connection: the raw client plus a receive buffer seeded
// with whatever the server had already buffered past the handshake request.
typedef struct {
    Socket_Handle client;
    Read_Buffer rb;
} Http_Ws_Conn;

// the on-open callback handed to http_ws_upgrade. it owns the connection:
// loop http_ws_read_frame, answer PING/PONG, respond to CLOSE, listen for
// errors, and return at will; the server closes the socket when it returns.
typedef void (*Http_Ws_On_Open)(Http_Ws_Conn *conn, void *user_data);

// turns a WebSocket handshake request (GET, Upgrade: websocket,
// Connection: upgrade, from a HTTP/1.1 peer with a Sec-WebSocket-Key and
// version 13) into a 101 Switching Protocols response wired to on_open /
// user_data. returns 0 when the response is ready to send and the server will
// follow with on_open, or -1 when the request was not a valid upgrade (the
// handler should reply 400).
int http_ws_upgrade(Http_Request *req, Http_Response *res,
                    Http_Ws_On_Open on_open, void *user_data);

// reads one frame. fills *frame and copies up to payload_cap decoded payload
// bytes into payload[]. returns 1 on a data/control frame (opcode TEXT, BIN,
// CONT, PING, PONG available for the caller to act on), 0 when the peer sent
// a CLOSE frame (the caller should answer with http_ws_send_close and return),
// and -1 on a closed/broken socket, a protocol violation, or a payload larger
// than payload_cap (a 1009 close should be sent before returning).
int http_ws_read_frame(Http_Ws_Conn *conn, Http_Ws_Frame *frame,
                       uint8_t *payload, size_t payload_cap);

// sends one server frame. payload is copied, the caller keeps it. note the
// server never masks frames; only clients do. returns 0 or -1.
int http_ws_send(Http_Ws_Conn *conn, uint8_t opcode,
                 const uint8_t *payload, size_t len, bool fin);

// convenience wrappers
int http_ws_send_text(Http_Ws_Conn *conn, const char *text);
int http_ws_send_close(Http_Ws_Conn *conn, uint16_t code); // 1000-4999
int http_ws_send_pong(Http_Ws_Conn *conn, const uint8_t *payload, size_t len);

// reads frames for the caller, answering PING with PONG automatically and
// with CLOSE sending an echoing close frame back. on_message returns 0 to
// keep listening or -1 to stop. returns 0 when the conversation ended
// cleanly (or the callback stopped it), -1 on a socket/protocol error.
typedef int (*Http_Ws_On_Message)(Http_Ws_Conn *conn, const Http_Ws_Frame *frame,
                                  const uint8_t *payload, void *user_data);
int http_ws_loop(Http_Ws_Conn *conn, Http_Ws_On_Message on_message,
                 void *user_data, uint8_t *buf, size_t buf_cap);

#endif