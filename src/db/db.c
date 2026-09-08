#include "db.h"

#include <stdio.h>
#include <string.h>

#include "file.h"
#include "strbuf.h"
#include "sv.h"
#include "xmem.h"

// on-disk framing, version byte 1:
//   1 byte  version
//   per entry, little-endian:
//     u32 key_len, key bytes, u32 value_len, value bytes
// explicit lengths mean keys and values may hold any bytes, including NULs.

static void put_u32(char *p, uint32_t v)
{
    p[0] = (char)(v & 0xff);
    p[1] = (char)((v >> 8) & 0xff);
    p[2] = (char)((v >> 16) & 0xff);
    p[3] = (char)((v >> 24) & 0xff);
}

static uint32_t get_u32(const char *p)
{
    return (uint32_t)(unsigned char)p[0] |
           ((uint32_t)(unsigned char)p[1] << 8) |
           ((uint32_t)(unsigned char)p[2] << 16) |
           ((uint32_t)(unsigned char)p[3] << 24);
}

static char *db_strdup(const char *s)
{
    size_t n = strlen(s);
    char *out = xmalloc(n + 1);
    memcpy(out, s, n + 1);
    return out;
}

// how many bytes entry occupies on disk: two u32 lengths plus the payloads
static size_t entry_size(const Cweb_Db_Entry *e)
{
    return 8 + e->klen + e->vlen;
}

// linear search: pages hold a handful of keys, keeping a hash table and its
// collision rules out of a storage layer that wants to stay boring
static Cweb_Db_Entry *db_find(Cweb_Db *db, String_View key)
{
    for (size_t i = 0; i < db->count; i++) {
        if (db->entries[i].klen == key.count &&
            (key.count == 0 ||
             memcmp(db->entries[i].key, key.data, key.count) == 0)) {
            return &db->entries[i];
        }
    }
    return NULL;
}

// owned copy appended to the store; grows the backing array in place
static int db_append(Cweb_Db *db, const char *key, size_t klen,
                     const char *value, size_t vlen)
{
    if (db->count == db->capacity) {
        size_t nc = db->capacity ? db->capacity * 2 : 8;
        db->entries = xrealloc(db->entries, nc * sizeof(*db->entries));
        db->capacity = nc;
    }
    Cweb_Db_Entry *e = &db->entries[db->count++];
    e->key = xmalloc(klen + 1);
    if (klen > 0) {
        memcpy(e->key, key, klen);
    }
    e->key[klen] = '\0';
    e->value = xmalloc(vlen + 1);
    if (vlen > 0) {
        memcpy(e->value, value, vlen);
    }
    e->value[vlen] = '\0';
    e->klen = klen;
    e->vlen = vlen;
    return 0;
}

int cweb_db_open(Cweb_Db *db, const char *path)
{
    Cweb_Db tmp;
    memset(&tmp, 0, sizeof tmp);

    time_t mtime;
    size_t fsize;
    if (file_stat(path, &mtime, &fsize) != 0) {
        // no file yet: the store starts empty and is born on the first sync
        tmp.path = db_strdup(path);
    } else {
        char *data;
        size_t len;
        if (file_read_all(path, &data, &len) != 0) {
            return -1;
        }
        if (len == 0 || (unsigned char)data[0] != 1) {
            xfree(data);
            return -1; // not a store we framed
        }
        tmp.path = db_strdup(path);
        size_t pos = 1;
        int bad = 0;
        while (pos + 8 <= len) {
            uint32_t klen = get_u32(data + pos);
            pos += 4;
            uint32_t vlen;
            if (klen > len - pos) {
                bad = 1;
                break;
            }
            const char *key = data + pos;
            pos += klen;
            if (pos + 4 > len) {
                bad = 1;
                break;
            }
            vlen = get_u32(data + pos);
            pos += 4;
            if (vlen > len - pos) {
                bad = 1;
                break;
            }
            const char *value = data + pos;
            pos += vlen;
            db_append(&tmp, key, klen, value, vlen);
            tmp.total += 8 + (size_t)klen + (size_t)vlen;
        }
        if (bad || pos != len) {
            for (size_t i = 0; i < tmp.count; i++) {
                xfree(tmp.entries[i].key);
                xfree(tmp.entries[i].value);
            }
            xfree(tmp.entries);
            xfree(tmp.path);
            xfree(data);
            return -1; // trailing garbage or a truncated frame
        }
        xfree(data);
    }

    mutex_init(&tmp.mu);
    *db = tmp;
    return 0;
}

void cweb_db_close(Cweb_Db *db)
{
    if (db->path == NULL) {
        return; // already closed
    }
    if (db->dirty) {
        cweb_db_sync(db);
    }
    mutex_destroy(&db->mu);
    for (size_t i = 0; i < db->count; i++) {
        xfree(db->entries[i].key);
        xfree(db->entries[i].value);
    }
    xfree(db->entries);
    xfree(db->path);
    memset(db, 0, sizeof *db);
}

String_View cweb_db_get(Cweb_Db *db, String_View key)
{
    mutex_lock(&db->mu);
    Cweb_Db_Entry *e = db_find(db, key);
    String_View view = e ? (String_View){e->value, e->vlen} : (String_View){NULL, 0};
    mutex_unlock(&db->mu);
    return view;
}

int cweb_db_put(Cweb_Db *db, String_View key, String_View value)
{
    mutex_lock(&db->mu);
    Cweb_Db_Entry *e = db_find(db, key);
    if (e != NULL) {
        // replace in place: the slot keeps its position and only the payload
        // changes, so no array realloc can move the entry under us
        size_t old = entry_size(e);
        xfree(e->key);
        xfree(e->value);
        e->key = xmalloc(key.count + 1);
        if (key.count > 0) {
            memcpy(e->key, key.data, key.count);
        }
        e->key[key.count] = '\0';
        e->value = xmalloc(value.count + 1);
        if (value.count > 0) {
            memcpy(e->value, value.data, value.count);
        }
        e->value[value.count] = '\0';
        e->klen = key.count;
        e->vlen = value.count;
        db->total += (8 + key.count + value.count) - old;
    } else {
        db_append(db, key.data, key.count, value.data, value.count);
        db->total += 8 + key.count + value.count;
    }
    db->dirty = 1;
    mutex_unlock(&db->mu);
    return 0;
}

int cweb_db_delete(Cweb_Db *db, String_View key)
{
    mutex_lock(&db->mu);
    Cweb_Db_Entry *e = db_find(db, key);
    if (e == NULL) {
        mutex_unlock(&db->mu);
        return -1;
    }
    db->total -= entry_size(e);
    xfree(e->key);
    xfree(e->value);
    *e = db->entries[db->count - 1]; // swap-remove keeps the array dense
    db->count--;
    db->dirty = 1;
    mutex_unlock(&db->mu);
    return 0;
}

int cweb_db_sync(Cweb_Db *db)
{
    mutex_lock(&db->mu);
    int rc;
    if (db->count == 0) {
        // an empty store renders as no file, so a wiped db reads back fresh
        remove(db->path);
        db->dirty = 0;
        rc = 0;
    } else {
        Strbuf buf;
        strbuf_init(&buf);
        strbuf_append_char(&buf, (char)1);
        for (size_t i = 0; i < db->count; i++) {
            char head[4];
            put_u32(head, (uint32_t)db->entries[i].klen);
            strbuf_append(&buf, head, 4);
            strbuf_append(&buf, db->entries[i].key, db->entries[i].klen);
            put_u32(head, (uint32_t)db->entries[i].vlen);
            strbuf_append(&buf, head, 4);
            strbuf_append(&buf, db->entries[i].value, db->entries[i].vlen);
        }

        rc = -1;
        if (buf.count == db->total + 1) { // our byte accounting never drifts
            char tmp[1024];
            if (snprintf(tmp, sizeof tmp, "%s.tmp", db->path) <
                (int)sizeof tmp) {
                if (file_write(tmp, buf.items, buf.count) == 0) {
#ifdef _WIN32
                    remove(db->path); // rename cannot replace over it
#endif
                    if (rename(tmp, db->path) == 0) {
                        db->dirty = 0;
                        rc = 0;
                    } else {
                        remove(tmp);
                    }
                }
            }
            // a failed write leaves the old file in place and the change
            // pending, so sync can simply be retried
        } else {
            rc = -1; // total says one thing, the buffer holds another
        }
        strbuf_free(&buf);
    }
    mutex_unlock(&db->mu);
    return rc;
}