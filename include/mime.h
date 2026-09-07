#ifndef CWEB_MIME_H
#define CWEB_MIME_H

#include "sv.h"

// mime type for a path, guessed by its extension (case-insensitive).
// "application/octet-stream" when nothing matches.
String_View mime_for_path(String_View path);

#endif