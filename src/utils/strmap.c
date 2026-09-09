#include "strmap.h"

#include <stdint.h>
#include <string.h>

#include "xmem.h"

// FNV-1a, small and good enough for short header/param names
static uint64_t fnv1a(const char *s, size_t n)
{
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

void strmap_init(Str_Map *m)
{
    m->entries = NULL;
    m->count = 0;
    m->capacity = 0;
}

void strmap_free(Str_Map *m)
{
    for (size_t i = 0; i < m->capacity; i++) {
        if (m->entries[i].key) {
            xfree(m->entries[i].key);
            xfree(m->entries[i].value);
        }
    }
    xfree(m->entries);
    strmap_init(m);
}

static char *dup_own(const char *data, size_t len)
{
    char *copy = xmalloc(len + 1);
    memcpy(copy, data, len);
    copy[len] = '\0';
    return copy;
}

const char *strmap_get(Str_Map *m, String_View key)
{
    if (m->capacity == 0) {
        return NULL;
    }
    size_t idx = fnv1a(key.data, key.count) & (m->capacity - 1);
    for (size_t i = 0; i < m->capacity; i++) {
        Str_Map_Entry *e = &m->entries[(idx + i) & (m->capacity - 1)];
        if (!e->key) {
            return NULL;
        }
        if (strlen(e->key) == key.count && memcmp(e->key, key.data, key.count) == 0) {
            return e->value;
        }
    }
    return NULL; // should never get here with load factor kept under 0.7
}

const char *strmap_get_cstr(Str_Map *m, const char *key)
{
    return strmap_get(m, sv_from_cstr(key));
}

static int strmap_grow(Str_Map *m)
{
    size_t new_cap = m->capacity ? m->capacity * 2 : 16;
    Str_Map_Entry *entries = xcalloc(new_cap, sizeof(Str_Map_Entry));

    for (size_t i = 0; i < m->capacity; i++) {
        Str_Map_Entry *e = &m->entries[i];
        if (!e->key) {
            continue;
        }
        size_t idx = fnv1a(e->key, strlen(e->key)) & (new_cap - 1);
        while (entries[idx].key) {
            idx = (idx + 1) & (new_cap - 1);
        }
        entries[idx] = *e; // steals the allocated key/value pointers
    }

    xfree(m->entries);
    m->entries = entries;
    m->capacity = new_cap;
    return 0;
}

int strmap_set(Str_Map *m, String_View key, String_View value)
{
    if (m->capacity == 0 || m->count * 2 >= m->capacity) {
        if (strmap_grow(m) != 0) {
            return -1;
        }
    }
    size_t idx = fnv1a(key.data, key.count) & (m->capacity - 1);
    for (size_t i = 0; i < m->capacity; i++) {
        Str_Map_Entry *e = &m->entries[(idx + i) & (m->capacity - 1)];
        if (!e->key) {
            e->key = dup_own(key.data, key.count);
            e->value = dup_own(value.data, value.count);
            m->count++;
            return 0;
        }
        if (strlen(e->key) == key.count && memcmp(e->key, key.data, key.count) == 0) {
            xfree(e->value);
            e->value = dup_own(value.data, value.count);
            return 0;
        }
    }
    return -1; // table is full, should not happen
}

void strmap_delete(Str_Map *m, String_View key)
{
    if (m->capacity == 0) {
        return;
    }
    size_t mask = m->capacity - 1;
    size_t idx = fnv1a(key.data, key.count) & mask;
    while (m->entries[idx].key != NULL) {
        if (strlen(m->entries[idx].key) == key.count &&
            memcmp(m->entries[idx].key, key.data, key.count) == 0) {
            break;
        }
        idx = (idx + 1) & mask;
    }
    if (m->entries[idx].key == NULL) {
        return; // not present
    }
    xfree(m->entries[idx].key);
    xfree(m->entries[idx].value);
    m->entries[idx].key = NULL;
    m->entries[idx].value = NULL;
    m->count--;

    // every entry that probed across the freed slot is now unreachable: pull
    // the trailing cluster and re-insert it through strmap_set, which finds
    // the correct slot now that the gap exists
    Str_Map_Entry *held = xmalloc(m->capacity * sizeof(*held));
    size_t nheld = 0;
    size_t k = (idx + 1) & mask;
    while (m->entries[k].key != NULL) {
        held[nheld++] = m->entries[k];
        m->count--;
        m->entries[k].key = NULL;
        m->entries[k].value = NULL;
        k = (k + 1) & mask;
    }
    for (size_t i = 0; i < nheld; i++) {
        (void)strmap_set(m, sv_from_cstr(held[i].key), sv_from_cstr(held[i].value));
        xfree(held[i].key);
        xfree(held[i].value);
    }
    xfree(held);
}