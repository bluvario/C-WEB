#include "mime.h"

#include <ctype.h>
#include <string.h>

#include "sv.h"

typedef struct {
    const char *ext;
    const char *mime;
} Mime_Entry;

static const Mime_Entry MIME_TABLE[] = {
    {"html", "text/html"},
    {"htm", "text/html"},
    {"css", "text/css"},
    {"js", "text/javascript"},
    {"json", "application/json"},
    {"xml", "application/xml"},
    {"txt", "text/plain"},
    {"csv", "text/csv"},
    {"png", "image/png"},
    {"jpg", "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"gif", "image/gif"},
    {"svg", "image/svg+xml"},
    {"webp", "image/webp"},
    {"avif", "image/avif"},
    {"ico", "image/x-icon"},
    {"woff2", "font/woff2"},
    {"ttf", "font/ttf"},
    {"mp4", "video/mp4"},
    {"webm", "video/webm"},
    {"mp3", "audio/mpeg"},
    {"pdf", "application/pdf"},
    {"zip", "application/zip"},
    {"wasm", "application/wasm"},
};

String_View mime_for_path(String_View path)
{
    const char *dot = NULL;
    for (size_t i = 0; i < path.count; i++) {
        if (path.data[i] == '.') {
            dot = &path.data[i];
        }
    }
    if (!dot) {
        return sv_from_cstr("application/octet-stream");
    }

    size_t ext_len = path.count - (size_t)(dot - path.data) - 1;
    const char *ext = dot + 1;
    for (size_t i = 0; i < sizeof(MIME_TABLE) / sizeof(MIME_TABLE[0]); i++) {
        const char *want = MIME_TABLE[i].ext;
        size_t n = strlen(want);
        if (ext_len == n) {
            bool same = true;
            for (size_t j = 0; j < n; j++) {
                if (tolower((unsigned char)ext[j]) != tolower((unsigned char)want[j])) {
                    same = false;
                    break;
                }
            }
            if (same) {
                return sv_from_cstr(MIME_TABLE[i].mime);
            }
        }
    }
    return sv_from_cstr("application/octet-stream");
}