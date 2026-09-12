#ifndef CWEB_HPACK_H
#define CWEB_HPACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "strbuf.h"
#include "sv.h"

// HTTP/2 header compression per RFC 7541. one context per connection: it owns
// the HPACK decoder state (Huffman decode tree and the dynamic table), which
// the peer's encoder mutates across header blocks, so each side needs its own
// context that survives for the whole connection.
//
// responses never touch the dynamic table: the encoder always emits indexed
// :status and literal-without-indexing for everything else, so encoding needs
// no per-connection state and hpack_encode_* can run against any Strbuf.

#define HPACK_DEFAULT_MAX_TABLE_SIZE 4096u
#define HPACK_STATIC_TABLE_COUNT     61u
#define HPACK_MAX_STRING_LEN         (64u * 1024u)

typedef struct {
    String_View name;
    String_View value;
} Hpack_Field;

typedef struct Hpack_Huff_Node {
    struct Hpack_Huff_Node *child[2];
    int symbol; // -1 internal node; 256 is the EOS marker, rejected mid-stream
} Hpack_Huff_Node;

// full HPACK decode state for one connection
typedef struct {
    // dynamic table, newest entry first at index 0 (§4.1). name/value strings
    // are owned heap copies; eviction frees the oldest entries at the back.
    String_View *dyn_name;
    String_View *dyn_value;
    size_t dyn_count;
    size_t dyn_capacity; // array slots, not bytes
    size_t dyn_used;     // current table size in bytes (RFC 7541 §4.1)
    size_t dyn_max_capacity; // budget cap, SETTINGS_HEADER_TABLE_SIZE
    bool  huff_built;
    Hpack_Huff_Node huff_root; // decode trie, built once in hpack_init
} Hpack;

// output of hpack_decode_block: one owned buffer (bytes) holds every name and
// value; the items borrow from it. free the whole thing with
// hpack_decoded_free.
typedef struct {
    Hpack_Field *items;
    size_t count;
    size_t capacity;
    Strbuf bytes;
} Hpack_Decoded;

void hpack_init(Hpack *hp);
void hpack_free(Hpack *hp);
void hpack_decoded_free(Hpack_Decoded *d);

// RFC 7541 §4.2: the dynamic table may use at most size bytes; entries beyond
// the new budget are evicted from the back. the encoded peer signals this with
// a dynamic table size update at the start of a header block.
void hpack_set_max_table_size(Hpack *hp, size_t size);

// decodes one header block of len bytes. out must be zero-initialized on
// entry. returns 0 on success, -1 on a compression error: the caller must
// tear the connection down with COMPRESSION_ERROR.
int hpack_decode_block(Hpack *hp, const uint8_t *data, size_t len,
                       Hpack_Decoded *out);

// builds the byte streams for response headers. each returns 0 on success,
// -1 on allocation failure.
int hpack_encode_status(Strbuf *out, uint16_t status);
int hpack_encode_field(Strbuf *out, String_View name, String_View value);

#endif