#ifndef CWEB_CSRF_H
#define CWEB_CSRF_H

#include "response.h"
#include "session.h"
#include "strmap.h"

// per-session cross-site request forgery protection. the token is a random
// secret kept on the server in the session (`csrf-token` key); a form carries
// it back as a hidden "csrf_token" input, and a POST handler verifies it
// before acting. an attacker's page cannot read the token -- it is only ever
// served to the same origin -- so a forged cross-site POST never carries the
// right value and is refused with 403.
//
// a visitor with no session yet has no token to check against, so every call
// is a safe no-op or a clean failure when sesh is NULL (and http_csrf_field()
// simply emits nothing until the page creates a session and its cookie).

// the session's token, generating one on first use. borrowed from the session
// store, valid for the rest of the session; NULL when sesh is NULL.
const char *http_csrf_token(Http_Session *sesh);

// appends the standard form hidden input for the session's token:
//   <input type="hidden" name="csrf_token" value="<token>">
// nothing is emitted without a session.
void http_csrf_field(Http_Response *res, Http_Session *sesh);

// verifies that the submitted form's "csrf_token" equals the session's token.
// returns 0 when the form is genuine, -1 when the field is missing, the
// session has no token yet, or the values differ. a -1 answer should become
// a 403 (http_csrf_reject() renders one) and the form must be re-fetched.
int http_csrf_verify(Str_Map *params, Http_Session *sesh);

// answers 403 with a short note and a link back to retry_url, for when
// http_csrf_verify() failed.
void http_csrf_reject(Http_Response *res, const char *retry_url);

#endif