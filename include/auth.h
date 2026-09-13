#ifndef CWEB_AUTH_H
#define CWEB_AUTH_H

#include "middleware.h"
#include "response.h"
#include "session.h"
#include "sv.h"

// decodes the credentials in an "Authorization: Basic <base64>" header value.
// the base64 payload is "user:pass", split here into the caller's buffers
// (truncated to fit, always NUL-terminated). returns 0 for well-formed
// credentials, -1 when the scheme, encoding or shape is wrong. an empty
// password is legal, an empty user or a missing colon is not.
int http_basic_auth_parse(String_View authorization,
                          char *user, size_t user_size,
                          char *pass, size_t pass_size);

// answers a request with 401 and a WWW-Authenticate challenge for the realm,
// so clients pop a credentials dialog. realm is echoed back, escaped for the
// header's quoted-string grammar.
void http_response_require_basic_auth(Http_Response *res, const char *realm);

// options for the Basic-auth middleware; pass a pointer as user_data.
typedef struct {
    // the one identity that unlocks the chain. user must be non-empty; pass
    // may be NULL (an empty password is legal per http_basic_auth_parse).
    const char *user;
    const char *pass;
    // realm echoed in the WWW-Authenticate challenge so the browser labels
    // its stored credentials; NULL defaults to "cweb".
    const char *realm;
} Http_BasicAuth_Options;

// Basic-auth guard middleware: verifies the Authorization header before
// letting the chain run, so one call guards an entire app or route group.
//   * missing, malformed, or wrong credentials -> 401 with a WWW-Authenticate
//     challenge, and the rest of the chain is skipped;
//   * matching credentials -> the authenticated username is copied into
//     req->auth_user (owned by the request object, like request_id), so
//     handlers can greet or log who is talking, then next() runs;
//   * opts NULL, or an empty opts->user -> disabled, everything passes; handy
//     for a dev build that mounts the same server unguarded.
//
// the comparison is done on SHA-256 digests of the whole "user:pass" wire
// payload vs the expected identity, compared in constant time, so a timing
// side channel cannot reveal how much of a guess was right.
void http_basic_auth_middleware(Http_Request *req, Http_Response *res,
                                void *user_data,
                                Http_Handler_Fn next, void *next_data);

// ---------------------------------------------------------------------------
// session-based authentication and authorization
// ---------------------------------------------------------------------------
//
// the pair of middlewares below turns the server-side session store into an
// authentication source, so an app's own login form can gate the whole chain
// instead of relying on HTTP Basic. together they replace — for most apps —
// hand-writing "load session, check the user field" at the top of every page:
//
//   Http_SessionAuth_Options sa = { .store = &store, .cookie_name = "sesh" };
//   http_middleware_add(&chain, http_session_auth_middleware, &sa);
//   Http_RequireAuth_Options ra = { .roles = roles, .roles_count = n };
//   http_middleware_add(&chain, http_require_auth_middleware, &ra);
//
// the session-auth middleware is expected to run early in the chain; the
// require-auth guard runs after it and blocks before the handler.

// options for http_session_auth_middleware; pass a pointer as user_data.
// a NULL store (or a NULL/empty cookie_name) disables the middleware and
// everything passes through untouched.
typedef struct {
    // the shared session store the cookie token resolves against; required.
    Http_Session_Store *store;
    // the cookie name holding the session token. NULL defaults to "cweb_session".
    const char *cookie_name;
    // which session key holds the username and the comma-separated role list;
    // NULL defaults to "auth_user" and "auth_roles".
    const char *session_key;
    const char *roles_key;
} Http_SessionAuth_Options;

// resolves the request's session cookie and copies its identity into the
// request for later middleware and handlers:
//   * the session key's value becomes req->auth_user (owned by the request,
//     freed with the request, empty when no session or no user is stored);
//   * the roles key's value becomes req->auth_roles, a comma-separated list
//     with the same ownership rules (empty when absent);
//   * no session, or no stored identity, leaves both views empty and never
//     blocks. this middleware only annotates; gating is require-auth's job.
void http_session_auth_middleware(Http_Request *req, Http_Response *res,
                                  void *user_data,
                                  Http_Handler_Fn next, void *next_data);

// options for http_require_auth_middleware; pass a pointer as user_data.
typedef struct {
    // roles the caller must hold; the caller needs only one of them ("any-of").
    // NULL with roles_count == 0 simply requires any authenticated user.
    const char **roles;
    size_t roles_count;
    // when an unauthenticated request arrives, redirect here with 303 instead
    // of answering 401. NULL (the default) answers 401 + a plain-body hint.
    const char *login_path;
    // when an authenticated caller lacks every required role, redirect here
    // with 303 instead of answering 403. NULL answers 403.
    const char *forbidden_path;
} Http_RequireAuth_Options;

// authorization guard: refuses the request unless req->auth_user names someone
// (set by http_session_auth_middleware earlier in the chain) holding one of
// the required roles. blocking answers are 401-with-login-hint (or a 303 to
// opts->login_path) when unauthenticated and 403 (or a 303 to
// opts->forbidden_path) when authenticated but role-less, and the rest of the
// chain — including the handler — is skipped. opts NULL disables the guard
// and lets everything through.
void http_require_auth_middleware(Http_Request *req, Http_Response *res,
                                  void *user_data,
                                  Http_Handler_Fn next, void *next_data);

#endif