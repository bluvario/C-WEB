#ifndef CWEB_ROUTER_H
#define CWEB_ROUTER_H

#include "http.h"
#include "request.h"
#include "response.h"
#include "strmap.h"

typedef void (*Http_Route_Handler)(Http_Request *req, Http_Response *res,
                                   Str_Map *params, void *user_data);

// *params holds captured <name> segments, owned by the dispatcher. it is
// freed right after the handler returns, keep no pointers into it.
typedef struct {
    Http_Method method;
    char *pattern; // owned copy, "<id>" style captures allowed
    Http_Route_Handler handler;
    void *user_data;
    unsigned long timeout_ms; // per-route SO_RCVTIMEO override; 0 = default
} Http_Route;

typedef struct {
    Http_Route *items;
    size_t count;
    size_t capacity;
} Http_Router;

void router_init(Http_Router *r);
void router_free(Http_Router *r);

// router takes ownership of a copy of pattern. returns 0 or -1 on OOM.
int router_add(Http_Router *r, Http_Method method, const char *pattern,
               Http_Route_Handler handler, void *user_data);

// like router_add but with a per-route SO_RCVTIMEO override in milliseconds.
// when set the server resets the socket read timeout to this value after each
// response on a keep-alive connection; a stall during the next request then
// produces a 504 instead of the default 408.
int router_add_timeout(Http_Router *r, Http_Method method, const char *pattern,
                       Http_Route_Handler handler, void *user_data,
                       unsigned long timeout_ms);

// sets the timeout on an already-registered route found by pattern and method.
// returns 0 on success, -1 if no matching route exists.
int router_set_timeout(Http_Router *r, Http_Method method, const char *pattern,
                       unsigned long timeout_ms);

// removes the first route matching method+pattern. the route's owned pattern is
// freed; its user_data is returned through *user_data_out (which may be NULL)
// so the caller can release anything it owns. returns 0 on success, -1 if no
// match.
int router_remove(Http_Router *r, Http_Method method, const char *pattern,
                  void **user_data_out);

// dispatches a parsed request: first matching route wins and its handler runs
// with the captured params. no match produces a 404 response. designed to be
// passed straight to http_serve as the top-level handler with the router as
// user_data.
void router_dispatch(Http_Request *req, Http_Response *res, void *user_data);

#endif