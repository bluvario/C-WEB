#ifndef CWEB_STRMAP_H
#define CWEB_STRMAP_H

#include <stddef.h>

#include "sv.h"

// owned string -> owned string hash map (open addressing, linear probing).
// used for headers, query params, session data.
typedef struct {
    char *key;   // owned, NULL marks an empty slot
    char *value; // owned
} Str_Map_Entry;

typedef struct {
    Str_Map_Entry *entries;
    size_t count;
    size_t capacity; // power of two
} Str_Map;

void strmap_init(Str_Map *m);
void strmap_free(Str_Map *m);

const char *strmap_get(Str_Map *m, String_View key); // NULL if absent
const char *strmap_get_cstr(Str_Map *m, const char *key);

// inserts or replaces. copies both key and value, returns -1 on allocation failure
int strmap_set(Str_Map *m, String_View key, String_View value);

// removes a key/value pair; unknown keys are a no-op. the cluster that
// probed past the freed slot is re-inserted so lookups stay correct.
void strmap_delete(Str_Map *m, String_View key);

#endif