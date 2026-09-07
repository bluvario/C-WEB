#ifndef CWEB_SERVER_H
#define CWEB_SERVER_H

#include "net.h"
#include "request.h"
#include "response.h"

// fills *res for a parsed request. the server owns memory management, the
// handler only writes into res.
typedef void (*Http_Handler_Fn)(Http_Request *req, Http_Response *res, void *user_data);

// serves exactly one connection: reads until a complete request, dispatches
// to the handler, writes the response, closes. malformed requests get a 400,
// oversized ones a 413.
void http_serve_connection(Socket_Handle client, Http_Handler_Fn handler, void *user_data);

// accept loop that never returns on its own. one request per connection for
// now (Connection: close), keep-alive is a TODO.
int http_serve(Socket_Handle listener, Http_Handler_Fn handler, void *user_data);

#endif