#ifndef CWEB_SERVER_H
#define CWEB_SERVER_H

#include <stddef.h>

#include "net.h"
#include "request.h"
#include "response.h"

// fills *res for a parsed request. the server owns memory management, the
// handler only writes into res.
typedef void (*Http_Handler_Fn)(Http_Request *req, Http_Response *res, void *user_data);

// hardening knobs for the serving loop; every field zero keeps the library
// default. pass the struct to http_serve_config() / http_serve_connection_config().
typedef struct {
    size_t max_body;             // request read-buffer cap, oversized asks 413
    const char *body_dir;        // temp dir for oversized bodies, NULL = 413
    unsigned long io_timeout_ms; // per-read deadline, stalled clients get 408
    size_t workers;              // accept-loop worker threads, 0 = auto
} Http_Server_Config;

// serves exactly one connection: reads until a complete request (keep-alive
// and pipelining included), dispatches to the handler, writes the response,
// closes. malformed requests get a 400, oversized ones a 413, stalled clients
// a 408. cfg may be NULL for the defaults.
void http_serve_connection_config(Socket_Handle client, Http_Handler_Fn handler,
                                  void *user_data, const Http_Server_Config *cfg);

// http_serve_connection with the library defaults
void http_serve_connection(Socket_Handle client, Http_Handler_Fn handler, void *user_data);

// accept loop that runs until SIGINT/SIGTERM, then drains the in-flight
// connections and returns 0. malformed requests get a 400, oversized ones a
// 413, stalled clients a 408. cfg may be NULL for the defaults.
int http_serve_config(Socket_Handle listener, Http_Handler_Fn handler,
                      void *user_data, const Http_Server_Config *cfg);

// http_serve_config with the library defaults
int http_serve(Socket_Handle listener, Http_Handler_Fn handler, void *user_data);

#endif