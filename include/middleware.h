#ifndef CWEB_MIDDLEWARE_H
#define CWEB_MIDDLEWARE_H

#include <stdio.h>

#include "http.h"
#include "request.h"
#include "response.h"

// the handler that a middleware calls to pass control to the rest of the chain.
// this is the same shape as the top-level Http_Handler_Fn the server loop
// expects, which lets a fully built chain drop right into http_serve.
typedef void (*Http_Handler_Fn)(Http_Request *req, Http_Response *res,
                                void *user_data);

// middleware callback: runs *around* the handler, may inspect or modify the
// request, call next to continue, then inspect or modify the response.
typedef void (*Http_Middleware_Fn)(Http_Request *req, Http_Response *res,
                                   void *user_data,
                                   Http_Handler_Fn next, void *next_data);

typedef struct {
    Http_Middleware_Fn fn;
    void *user_data;
} Http_Middleware;

typedef struct {
    Http_Middleware *items;
    size_t count;
    size_t capacity;
} Http_Middleware_Chain;

void http_middleware_init(Http_Middleware_Chain *c);
void http_middleware_free(Http_Middleware_Chain *c);
int  http_middleware_add(Http_Middleware_Chain *c, Http_Middleware_Fn fn,
                         void *user_data);

// builds a dispatchable handler that runs every middleware in FIFO order,
// ending with final_handler(final_user_data). the returned handler is always
// invoked as fn(req, res, handler_data); *handler_data is an opaque array the
// caller must keep alive until dispatch is done and free with
// http_middleware_data_free. an empty chain still yields a trampoline so the
// calling convention stays uniform.
Http_Handler_Fn http_middleware_build(Http_Middleware_Chain *c,
                                      Http_Handler_Fn final_handler,
                                      void *final_user_data,
                                      void **handler_data);
void http_middleware_data_free(void *handler_data);

// -----------------------------------------------------------------------
// built-in middleware
// -----------------------------------------------------------------------

// options for the access logger; pass as user_data to http_access_log_middleware.
// file may be NULL (defaults to stderr).
typedef struct {
    FILE *file;
} Http_AccessLog_Opts;

// logs one CLF-style line per completed request:
//   - - [10/Oct/2000:13:55:36 +0300] "GET /path HTTP/1.1" 200 2326 5
// (client-ip and ident are placeholders; last field is elapsed ms)
void http_access_log_middleware(Http_Request *req, Http_Response *res,
                                void *user_data,
                                Http_Handler_Fn next, void *next_data);

#endif
