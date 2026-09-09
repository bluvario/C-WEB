#ifndef CWEB_FLASH_H
#define CWEB_FLASH_H

#include <stddef.h>

#include "response.h"
#include "session.h"

// one-shot notices carried in a session: a page that handles a POST stores a
// message with http_flash_set() and redirects (303), and the next GET surfaces
// it once. flash values live under reserved "flash:" keys, so they can never
// collide with session data a page sets itself, and an unread flash simply
// carries over to the next render instead of being lost.
//
// the session's backing db already mirrors everything, so a flash that was
// stored and then never shown survives a restart just long enough to be
// rendered once -- the "one-shot" part still holds.
//
// every call is a safe no-op when sesh is NULL (a visitor with no session yet,
// e.g. http_flash_render() from a page any anonymous visitor can hit).

// queues (or replaces, if the same key is still unread) a one-shot message.
void http_flash_set(Http_Session *sesh, const char *key, const char *message);

// returns the message queued under key and consumes it, so the next call
// yields NULL. NULL when nothing is pending. the returned buffer is the
// caller's to xfree.
char *http_flash_get(Http_Session *sesh, const char *key);

// renders every queued message into the response as
//   <div class="flash">message</div>
// with the messages HTML-escaped, then clears them all. returns how many were
// shown (so a page can skip an empty container). a no-op when none are queued.
size_t http_flash_render(Http_Response *res, Http_Session *sesh);

#endif