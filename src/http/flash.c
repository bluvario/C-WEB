#include "flash.h"

#include <stdio.h>
#include <string.h>

#include "escape.h"
#include "sv.h"
#include "xmem.h"

// everything under this prefix is flash traffic, invisible to http_session_value
#define FLASH_PREFIX "flash:"
#define FLASH_PREFIX_LEN (sizeof(FLASH_PREFIX) - 1)

static void flash_key(char *buf, size_t bufsz, const char *key)
{
    snprintf(buf, bufsz, FLASH_PREFIX "%s", key);
}

void http_flash_set(Http_Session *sesh, const char *key, const char *message)
{
    // no session yet means there is nowhere to hang the notice; the caller
    // creates one (and issues its cookie) before storing anything
    if (sesh == NULL) {
        return;
    }
    char kbuf[256];
    flash_key(kbuf, sizeof kbuf, key);
    http_session_set(sesh, kbuf, message);
}

char *http_flash_get(Http_Session *sesh, const char *key)
{
    if (sesh == NULL) {
        return NULL;
    }
    char kbuf[256];
    flash_key(kbuf, sizeof kbuf, key);
    const char *value = http_session_value(sesh, kbuf);
    if (value == NULL) {
        return NULL;
    }
    size_t n = strlen(value);
    char *owned = xmalloc(n + 1);
    memcpy(owned, value, n);
    owned[n] = '\0';
    strmap_delete(&sesh->data, sv_from_cstr(kbuf));
    return owned;
}

// a "flash:" entry has been rendered or discarded; drop it and shift the
// probe cluster so the session map stays consistent for everyone else
static void clear_one(Http_Session *sesh, const char *key)
{
    strmap_delete(&sesh->data, sv_from_cstr(key));
}

size_t http_flash_render(Http_Response *res, Http_Session *sesh)
{
    if (sesh == NULL) {
        return 0; // no session, nothing could have been queued
    }
    size_t shown = 0;
    for (size_t i = 0; i < sesh->data.capacity; i++) {
        Str_Map_Entry *e = &sesh->data.entries[i];
        if (e->key == NULL || strncmp(e->key, FLASH_PREFIX, FLASH_PREFIX_LEN) != 0) {
            continue;
        }
        // render first: clears happen afterwards so a failed read never loses
        // a message the page meant to show
        http_response_add_body_cstr(res, "<div class=\"flash\">");
        html_escape_into(&res->body, sv_from_cstr(e->value));
        http_response_add_body_cstr(res, "</div>\n");
        shown++;
    }
    if (shown > 0) {
        // drain the reserved keys now the render pass is done; deleting
        // rehashes the map, so re-scan after each removal
        while (true) {
            const char *hit = NULL;
            for (size_t i = 0; i < sesh->data.capacity; i++) {
                Str_Map_Entry *e = &sesh->data.entries[i];
                if (e->key != NULL && strncmp(e->key, FLASH_PREFIX, FLASH_PREFIX_LEN) == 0) {
                    hit = e->key;
                    break;
                }
            }
            if (hit == NULL) {
                break;
            }
            clear_one(sesh, hit);
        }
    }
    return shown;
}