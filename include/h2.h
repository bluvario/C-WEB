#ifndef CWEB_H2_H
#define CWEB_H2_H

#include <stdbool.h>
#include <stddef.h>

#include "buffer.h"
#include "server.h"

// the 24-byte client connection preface wire bytes
#define H2_MAGIC "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
#define H2_MAGIC_LEN 24

// how many concurrently open client streams this server services before
// refusing new ones with RST_STREAM(REFUSED_STREAM)
#define H2_MAX_CONCURRENT_STREAMS 100u

// serves an HTTP/2 cleartext (h2c) connection on fd. two entry paths:
//  - prior knowledge: rb already holds the client preface and any frames that
//    arrived with it; consumed must be H2_MAGIC_LEN (the preface itself), and
//    upgrade_req is NULL.
//  - upgrade: the HTTP/1.1 request was parsed out of rb by the caller;
//    consumed is how many bytes of rb that request occupied, and upgrade_req
//    is that parsed request, served as stream 1. the caller must keep its rb
//    and upgrade_req alive until h2_serve returns and frees them itself.
// the caller owns the socket and closes it after this returns.
void h2_serve(Socket_Handle fd, Http_Handler_Fn handler, void *user_data,
              const Http_Server_Config *cfg, Read_Buffer *rb, size_t consumed,
              Http_Request *upgrade_req, String_View remote,
              unsigned long default_timeout_ms);

// true when req is an HTTP/1.1 request announcing an h2c upgrade
// (RFC 7540 section 3.2.1): Connection lists both Upgrade and
// HTTP2-Settings, the Upgrade header names h2c, and there is exactly one
// HTTP2-Settings header field.
bool h2_is_upgrade_request(const Http_Request *req);

#endif