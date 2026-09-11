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

static void cweb_tpl_escape(Http_Response *res, String_View s)
{
    Strbuf tmp;
    strbuf_init(&tmp);
    html_escape_into(&tmp, s);
    cweb_tpl_add(res, tmp.items, tmp.count);
    strbuf_free(&tmp);
}

void page_index(Http_Request *req, Http_Response *res,
      Str_Map *params, void *user_data)
{
    (void)req;
    (void)params;
    (void)user_data;
    http_response_set_header(res, "Content-Type", "text/html; charset=utf-8");
    cweb_tpl_out(res, "");

  Cweb_Db *db = cweb_database();
  if (db == NULL) {

    cweb_tpl_out(res, "\n  <p>Start this server with <code>--db notes.db</code> and your notes will\n  survive restarts.</p>\n");

      return;
  }
  if (req->method == HTTP_POST) {
      const char *note = strmap_get_cstr(params, "note");
      if (note != NULL && note[0] != '\0') {
          String_View count = cweb_db_get(db, sv_from_cstr("count"));
          long long n = 0;
          if (count.data) {
              sv_to_i64(count, &n);
          }
          char key[32], val[32];
          snprintf(key, sizeof key, "note-%lld", n);
          snprintf(val, sizeof val, "%lld", n + 1);
          cweb_db_put(db, sv_from_cstr(key), sv_from_cstr(note));
          cweb_db_put(db, sv_from_cstr("count"), sv_from_cstr(val));
          http_response_redirect(res, HTTP_303_SEE_OTHER, "/");
          return;
      }
  }
  String_View count = cweb_db_get(db, sv_from_cstr("count"));
  long long n = 0;
  if (count.data) {
      sv_to_i64(count, &n);
  }
  char numbuf[32];
  snprintf(numbuf, sizeof numbuf, "%lld", n);

    cweb_tpl_out(res, "\n  <form method=\"post\" action=\"/\">\n    <label>Note <input type=\"text\" name=\"note\" autofocus></label>\n    <button>Pin it</button>\n  </form>\n  <p>you have ");
    cweb_tpl_out(res,  numbuf );
    cweb_tpl_out(res, " notes</p>\n");
 for (long long i = 0; i < n; i++) { 
    cweb_tpl_out(res, "\n  ");

    char kbuf[32];
    snprintf(kbuf, sizeof kbuf, "note-%lld", i);
    String_View text = cweb_db_get(db, sv_from_cstr(kbuf));
  
    cweb_tpl_out(res, "\n  <p class=\"note\">");
    cweb_tpl_escape(res,  text );
    cweb_tpl_out(res, "</p>\n");
 } 
}
