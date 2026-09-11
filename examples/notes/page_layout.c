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

void page_layout(Http_Request *req, Http_Response *res,
      Str_Map *params, void *user_data)
{
    (void)req;
    (void)params;
    (void)user_data;
    http_response_set_header(res, "Content-Type", "text/html; charset=utf-8");
    cweb_tpl_out(res, "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n  <meta charset=\"utf-8\">\n  <title>cweb notes</title>\n  <link rel=\"stylesheet\" href=\"/style.css\">\n</head>\n<body>\n  <header><h1>cweb notes</h1><p>persisted by C-WEB</p></header>\n  <main>\n    ");
 cweb_tpl_layout_emit(res); 
    cweb_tpl_out(res, "\n  </main>\n  ");
 page_footer(req, res, params, user_data); 
    cweb_tpl_out(res, "\n</body>\n</html>");
}
