#include "hpack.h"

#include <stdio.h>
#include <string.h>

#include "da.h"
#include "hpack_huff_tab.h"
#include "hpack_static_tab.h"
#include "xmem.h"

// ---------------------------------------------------------------------------
// internal helpers
// ---------------------------------------------------------------------------

static String_View sv_dup(String_View s)
{
    String_View r;
    r.data = xmalloc(s.count ? s.count : 1);
    if (s.count) {
        memcpy((void *)r.data, s.data, s.count);
    }
    r.count = s.count;
    return r;
}

// builds the Huffman decode trie. the code values in HPACK_HUFFMAN are stored
// as their transmitted bit pattern (MSB of the len-bit field first), so
// insertion walks bit (len-1) down to bit 0 and matches the byte packing the
// decoder uses (most significant bit of each byte first).
static void hp_huff_build(Hpack *hp)
{
    hp->huff_root.symbol = -1;
    hp->huff_root.child[0] = NULL;
    hp->huff_root.child[1] = NULL;
    for (int sym = 0; sym < 257; sym++) {
        uint32_t code = HPACK_HUFFMAN[sym].code;
        int len = HPACK_HUFFMAN[sym].len;
        Hpack_Huff_Node *n = &hp->huff_root;
        for (int b = len - 1; b >= 0; b--) {
            unsigned bit = (code >> (unsigned)b) & 1u;
            if (!n->child[bit]) {
                n->child[bit] = xcalloc(1, sizeof(Hpack_Huff_Node));
                n->child[bit]->symbol = -1;
            }
            n = n->child[bit];
        }
        n->symbol = sym;
    }
    hp->huff_built = true;
}

static void hp_tree_free(Hpack_Huff_Node *n)
{
    if (!n) {
        return;
    }
    hp_tree_free(n->child[0]);
    hp_tree_free(n->child[1]);
    xfree(n);
}

// RFC 7541 §5.1. reads an integer with an nbits prefix out of the stream,
// consuming continuation octets. returns 0 or -1.
static int hp_int(const uint8_t **p, const uint8_t *end, int nbits,
                  uint32_t *out)
{
    if (*p >= end) {
        return -1;
    }
    uint32_t maxp = (nbits >= 32) ? 0xffffffffu : ((1u << nbits) - 1u);
    uint32_t v = **p & maxp;
    (*p)++;
    if (v < maxp) {
        *out = v;
        return 0;
    }
    *out = maxp;
    uint64_t mult = 1;
    for (;;) {
        if (*p >= end) {
            return -1;
        }
        uint8_t b = **p;
        (*p)++;
        uint64_t add = (uint64_t)(b & 0x7f) * mult;
        if ((uint64_t)*out + add > 0xffffffffu) {
            return -1;
        }
        *out = (uint32_t)((uint64_t)*out + add);
        if (b & 0x80) {
            mult <<= 7;
            if (mult == 0) {
                return -1;
            }
        } else {
            return 0;
        }
    }
}

// RFC 7541 §5.2. decodes a string literal into `into` (appended) and points
// *out at the tail. huffman strings are inflated here.
static int hp_string(Hpack *hp, const uint8_t **p, const uint8_t *end,
                     Strbuf *into, String_View *out)
{
    if (*p >= end) {
        return -1;
    }
    uint8_t first = **p;
    bool huff = first & 0x80;
    uint32_t lenu = 0;
    if (hp_int(p, end, 7, &lenu) < 0) {
        return -1;
    }
    if (lenu > HPACK_MAX_STRING_LEN) {
        return -1;
    }
    size_t len = lenu;
    if ((size_t)(end - *p) < len) {
        return -1;
    }
    const uint8_t *raw = *p;
    *p += len;

    size_t start = into->count;
    if (huff && !hp->huff_built) {
        hp_huff_build(hp);
    }
    if (huff) {
        size_t cap = len * 8 + 1;
        char *tmp = xmalloc(cap);
        size_t o = 0;
        bool ok = false;
        if (len) {
            const Hpack_Huff_Node *n = &hp->huff_root;
            int pending = 0;
            bool pad_ones = true;
            ok = true;
            for (size_t i = 0; i < len; i++) {
                uint8_t byte = raw[i];
                for (int bit = 7; bit >= 0; bit--) {
                    unsigned b = (byte >> bit) & 1u;
                    pending++;
                    if (b == 0) {
                        pad_ones = false;
                    }
                    n = n->child[b];
                    if (!n) {
                        ok = false;
                        break;
                    }
                    if (n->symbol >= 0) {
                        if (n->symbol == 256) { // EOS
                            ok = false;
                            break;
                        }
                        if (o + 1 > cap) {
                            ok = false;
                            break;
                        }
                        tmp[o++] = (char)n->symbol;
                        n = &hp->huff_root;
                        pending = 0;
                        pad_ones = true;
                    }
                }
                if (!ok) {
                    break;
                }
            }
            if (ok && n != &hp->huff_root) {
                // trailing bits must be the EOS code prefix, all ones,
                // and no more than 7 of them (§5.2)
                ok = pending <= 7 && pad_ones;
            }
        } else {
            ok = true;
        }
        if (!ok || strbuf_append(into, tmp, o) < 0) {
            xfree(tmp);
            return -1;
        }
        xfree(tmp);
    } else if (strbuf_append(into, (const char *)raw, len) < 0) {
        return -1;
    }
    out->data = into->items + start;
    out->count = into->count - start;
    return 0;
}

// combined 1-based index (static then dynamic, newest first) into a field.
static int hp_lookup(const Hpack *hp, uint32_t idx, Hpack_Field *e)
{
    if (idx >= 1 && idx <= HPACK_STATIC_TABLE_COUNT) {
        const Hpack_Static_Entry *s = &HPACK_STATIC_TABLE[idx - 1u];
        e->name = s->name;
        e->value = s->value;
        return 0;
    }
    if (idx >= 62) {
        uint32_t d = idx - 62u;
        if (d < hp->dyn_count) {
            e->name = hp->dyn_name[d];
            e->value = hp->dyn_value[d];
            return 0;
        }
    }
    return -1;
}

// a decoded field described by offsets into out->bytes. out->bytes grows as
// more fields are decoded, which can move the buffer, so offsets are kept
// during the block and resolved into String_View at the end.
typedef struct {
    size_t n_off, n_len, v_off, v_len;
} Hpack_Off;

typedef struct {
    Hpack_Off *items;
    size_t count;
    size_t capacity;
} Hpack_Off_List;

// appends a string that never aliases out->bytes (static or dynamic table
// data), returning its offset
static int hp_copy_in(Hpack_Decoded *out, String_View text, size_t *off)
{
    *off = out->bytes.count;
    return strbuf_append(&out->bytes, text.data, text.count);
}

static void hp_finish(Hpack_Decoded *out, Hpack_Off_List *ol)
{
    if (ol->count == 0) {
        return;
    }
    out->items = xmalloc(ol->count * sizeof(*out->items));
    for (size_t k = 0; k < ol->count; k++) {
        Hpack_Off *o = &ol->items[k];
        out->items[k].name = (String_View){ out->bytes.items + o->n_off, o->n_len };
        out->items[k].value = (String_View){ out->bytes.items + o->v_off, o->v_len };
    }
    out->capacity = ol->count;
    out->count = ol->count;
}

static void hp_dyn_insert(Hpack *hp, String_View name, String_View value)
{
    size_t entry = name.count + value.count + 32u;
    if (hp->dyn_max_capacity && entry > hp->dyn_max_capacity) {
        return; // §4.4: an entry larger than the whole table is not added
    }
    while (hp->dyn_count && hp->dyn_used + entry > hp->dyn_max_capacity) {
        hp->dyn_count--;
        String_View *n = &hp->dyn_name[hp->dyn_count];
        String_View *v = &hp->dyn_value[hp->dyn_count];
        hp->dyn_used -= n->count + v->count + 32u;
        xfree((void *)n->data);
        xfree((void *)v->data);
    }
    if (hp->dyn_count == hp->dyn_capacity) {
        hp->dyn_capacity = hp->dyn_capacity ? hp->dyn_capacity * 2 : 16;
        hp->dyn_name = xrealloc(hp->dyn_name,
                                hp->dyn_capacity * sizeof(*hp->dyn_name));
        hp->dyn_value = xrealloc(hp->dyn_value,
                                 hp->dyn_capacity * sizeof(*hp->dyn_value));
    }
    memmove(hp->dyn_name + 1, hp->dyn_name,
            hp->dyn_count * sizeof(*hp->dyn_name));
    memmove(hp->dyn_value + 1, hp->dyn_value,
            hp->dyn_count * sizeof(*hp->dyn_value));
    hp->dyn_name[0] = sv_dup(name);
    hp->dyn_value[0] = sv_dup(value);
    hp->dyn_used += entry;
    hp->dyn_count++;
}

static void hp_shrink_to(Hpack *hp, size_t size)
{
    hp->dyn_max_capacity = size;
    while (hp->dyn_count && hp->dyn_used > size) {
        hp->dyn_count--;
        String_View *n = &hp->dyn_name[hp->dyn_count];
        String_View *v = &hp->dyn_value[hp->dyn_count];
        hp->dyn_used -= n->count + v->count + 32u;
        xfree((void *)n->data);
        xfree((void *)v->data);
    }
}

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

void hpack_init(Hpack *hp)
{
    memset(hp, 0, sizeof(*hp));
    hp_huff_build(hp);
    hp->dyn_max_capacity = HPACK_DEFAULT_MAX_TABLE_SIZE;
}

void hpack_free(Hpack *hp)
{
    for (size_t i = 0; i < hp->dyn_count; i++) {
        xfree((void *)hp->dyn_name[i].data);
        xfree((void *)hp->dyn_value[i].data);
    }
    xfree(hp->dyn_name);
    xfree(hp->dyn_value);
    hp_tree_free(hp->huff_root.child[0]);
    hp_tree_free(hp->huff_root.child[1]);
}

void hpack_decoded_free(Hpack_Decoded *d)
{
    da_free(d);
    strbuf_free(&d->bytes);
}

void hpack_set_max_table_size(Hpack *hp, size_t size)
{
    hp_shrink_to(hp, size);
}

int hpack_decode_block(Hpack *hp, const uint8_t *data, size_t len,
                       Hpack_Decoded *out)
{
    const uint8_t *p = data;
    const uint8_t *end = data + len;
    bool any_field = false;
    Hpack_Off_List ol = { 0 };

    while (p < end) {
        uint8_t b = *p;
        if ((b & 0xe0) == 0x20) {
            // dynamic table size update (§6.3), only before any field
            if (any_field) {
                da_free(&ol);
                return -1;
            }
            uint32_t sz = 0;
            if (hp_int(&p, end, 5, &sz) < 0) {
                da_free(&ol);
                return -1;
            }
            if (sz > hp->dyn_max_capacity) {
                da_free(&ol);
                return -1;
            }
            hp_shrink_to(hp, (size_t)sz);
            continue;
        }

        any_field = true;

        if (b & 0x80) {
            // indexed header field (§6.1)
            uint32_t idx = 0;
            Hpack_Field e;
            size_t n_off = 0, v_off = 0;
            if (hp_int(&p, end, 7, &idx) < 0 || idx == 0) {
                goto fail;
            }
            if (hp_lookup(hp, idx, &e) < 0) {
                goto fail;
            }
            if (hp_copy_in(out, e.name, &n_off) < 0 ||
                hp_copy_in(out, e.value, &v_off) < 0) {
                goto fail;
            }
            da_append(&ol, ((Hpack_Off){ n_off, e.name.count, v_off, e.value.count }));
        } else if ((b & 0xc0) == 0x40) {
            // literal with incremental indexing (§6.2.1)
            uint32_t idx = 0;
            Hpack_Field e;
            String_View name = { NULL, 0 };
            String_View value = { NULL, 0 };
            String_View lit_name = { NULL, 0 };
            size_t n_off = 0, n_len = 0, v_off = 0;
            if (hp_int(&p, end, 6, &idx) < 0) {
                goto fail;
            }
            if (idx == 0) {
                if (hp_string(hp, &p, end, &out->bytes, &name) < 0) {
                    goto fail;
                }
                n_off = out->bytes.count - name.count;
                n_len = name.count;
                // the value read below can move out->bytes, so snapshot the
                // literal name into its own allocation now
                lit_name = sv_dup(name);
            } else {
                if (hp_lookup(hp, idx, &e) < 0) {
                    goto fail;
                }
                if (hp_copy_in(out, e.name, &n_off) < 0) {
                    goto fail;
                }
                n_len = e.name.count;
            }
            if (hp_string(hp, &p, end, &out->bytes, &value) < 0) {
                xfree((void *)lit_name.data);
                goto fail;
            }
            v_off = out->bytes.count - value.count;
            da_append(&ol, ((Hpack_Off){ n_off, n_len, v_off, value.count }));
            if (idx == 0) {
                hp_dyn_insert(hp, lit_name, value);
                xfree((void *)lit_name.data);
            } else {
                hp_dyn_insert(hp, e.name, value);
            }
        } else {
            // literal without indexing (0000) and never indexed (0001)
            uint32_t idx = 0;
            Hpack_Field e;
            String_View name = { NULL, 0 };
            String_View value = { NULL, 0 };
            size_t n_off = 0, n_len = 0, v_off = 0;
            if (hp_int(&p, end, 4, &idx) < 0) {
                goto fail;
            }
            if (idx == 0) {
                if (hp_string(hp, &p, end, &out->bytes, &name) < 0) {
                    goto fail;
                }
                n_off = out->bytes.count - name.count;
                n_len = name.count;
            } else {
                if (hp_lookup(hp, idx, &e) < 0) {
                    goto fail;
                }
                if (hp_copy_in(out, e.name, &n_off) < 0) {
                    goto fail;
                }
                n_len = e.name.count;
            }
            if (hp_string(hp, &p, end, &out->bytes, &value) < 0) {
                goto fail;
            }
            v_off = out->bytes.count - value.count;
            da_append(&ol, ((Hpack_Off){ n_off, n_len, v_off, value.count }));
        }
    }
    hp_finish(out, &ol);
    da_free(&ol);
    return 0;

fail:
    da_free(&ol);
    return -1;
}

// ---------------------------------------------------------------------------
// encoding
// ---------------------------------------------------------------------------

// RFC 7541 §5.1, inverse. writes an integer with an nbits prefix; `pattern`
// carries the representation bits for the first octet.
static int hp_enc_int(Strbuf *out, uint32_t value, int nbits, uint8_t pattern)
{
    uint32_t maxp = (1u << nbits) - 1u;
    char tmp[8];
    int n = 0;
    if (value < maxp) {
        tmp[0] = (char)(pattern | (uint8_t)value);
        n = 1;
    } else {
        tmp[n++] = (char)(pattern | (uint8_t)maxp);
        uint32_t r = value - maxp;
        uint8_t groups[6];
        int gn = 0;
        do {
            groups[gn++] = (uint8_t)(r & 0x7f);
            r >>= 7;
        } while (r);
        for (int i = 0; i < gn; i++) {
            tmp[n++] = (char)(groups[i] | (i == gn - 1 ? 0x00 : 0x80));
        }
    }
    return strbuf_append(out, tmp, (size_t)n);
}

static int hp_enc_str(Strbuf *out, String_View s)
{
    if (hp_enc_int(out, (uint32_t)s.count, 7, 0x00) < 0) {
        return -1;
    }
    return strbuf_append(out, s.data, s.count);
}

static int hp_static_index(String_View name)
{
    for (int i = 0; i < (int)HPACK_STATIC_TABLE_COUNT; i++) {
        if (sv_equal(HPACK_STATIC_TABLE[i].name, name)) {
            return i + 1;
        }
    }
    return 0;
}

int hpack_encode_status(Strbuf *out, uint16_t status)
{
    switch (status) {
    case 200: return strbuf_append_char(out, (char)0x88);
    case 204: return strbuf_append_char(out, (char)0x89);
    case 206: return strbuf_append_char(out, (char)0x8a);
    case 304: return strbuf_append_char(out, (char)0x8b);
    case 400: return strbuf_append_char(out, (char)0x8c);
    case 404: return strbuf_append_char(out, (char)0x8d);
    case 500: return strbuf_append_char(out, (char)0x8e);
    default:
        break;
    }
    char dec[8];
    int n = snprintf(dec, sizeof dec, "%u", (unsigned)status);
    if (n <= 0) {
        return -1;
    }
    if (hp_enc_int(out, 0, 4, 0x00) < 0 ||
        hp_enc_str(out, sv_from_cstr(":status")) < 0) {
        return -1;
    }
    return hp_enc_str(out, (String_View){ dec, (size_t)n });
}

int hpack_encode_field(Strbuf *out, String_View name, String_View value)
{
    // literal without indexing (§6.2.2), indexed name when it is static so
    // responses stay small without ever touching the dynamic table
    int idx = hp_static_index(name);
    if (idx == 0) {
        if (hp_enc_int(out, 0, 4, 0x00) < 0) {
            return -1;
        }
        if (hp_enc_str(out, name) < 0) {
            return -1;
        }
    } else if (hp_enc_int(out, (uint32_t)idx, 4, 0x00) < 0) {
        return -1;
    }
    return hp_enc_str(out, value);
}