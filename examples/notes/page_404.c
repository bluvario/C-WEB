#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "escape.h"
#include "validate.h"
#include "csrf.h"
#include "flash.h"
#include "http.h"
#include "request.h"
#include "response.h"
#include "params.h"
#include "session.h"
#include "strmap.h"
#include "strbuf.h"
#include "sv.h"
#include "template.h"
#include "db.h"

// cweb build emits out/pages.h declaring every page, so a page
// can render another as a partial: <?c page_x(req,res,params,user_data); ?>
#if __has_include("pages.h")
#include "pages.h"
#endif

static void cweb_tpl_out(Http_Response *res, const char *s)
{
    cweb_tpl_add(res, s, strlen(s));
}

void page_404(Http_Request *req, Http_Response *res,
      Str_Map *params, void *user_data)
{
    (void)req;
    (void)params;
    (void)user_data;
    http_response_set_header(res, "Content-Type", "text/html; charset=utf-8");
    cweb_tpl_out(res, "<h1>404 · no such note</h1>\n<p>Nothing is pinned here. <a href=\"/\">Back to the board</a>.</p>");
}
