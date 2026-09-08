#ifndef CWEB_DB_H
#define CWEB_DB_H

#include <stddef.h>

#include "sv.h"
#include "thread.h"

// tiny file-backed key-value store for pages: the whole file is slurped into
// memory at open, mutated there, and rewritten atomically (a tmp file renamed
// over the path) on sync. no daemon, no schema, no query language -- enough to
// persist counters, settings, or whatever else a page wants between requests.
// every operation serializes on an internal mutex, so pages that run on a
// thread each get a coherent view.

typedef struct {
    char *key;   // owned
    size_t klen;
    char *value; // owned
    size_t vlen;
} Cweb_Db_Entry;

typedef struct {
    char *path; // owned; NULL once closed
    Cweb_Db_Entry *entries;
    size_t count;
    size_t capacity;
    size_t total; // bytes a sync would write; cheap sanity check on load
    Mutex mu;
    int dirty;
} Cweb_Db;

// loads path into memory, or starts empty when the file does not exist yet.
// a corrupt file is refused with -1 and *db is left untouched. callable on a
// zeroed Cweb_Db. no other db call may race this one.
int cweb_db_open(Cweb_Db *db, const char *path);

// writes pending changes back to the file and frees everything. after this
// the db handle is zeroed; call open again to reuse it.
void cweb_db_close(Cweb_Db *db);

// value held for key, or {NULL, 0}. the view is owned by the db and stays
// valid until the next mutating call (put/delete/sync/close).
String_View cweb_db_get(Cweb_Db *db, String_View key);

// inserts or replaces key. the key and value bytes are copied. returns 0.
int cweb_db_put(Cweb_Db *db, String_View key, String_View value);

// removes key, returning 0 if it was present or -1 if it was not.
int cweb_db_delete(Cweb_Db *db, String_View key);

// writes the in-memory state to disk now, atomically. returns 0 on success;
// on failure the change stays pending and dirty is still set. an empty db
// removes the file so an all-deleted store reads back as a fresh one.
int cweb_db_sync(Cweb_Db *db);

#endif