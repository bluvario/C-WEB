#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "hpack.h"
#include "sv.h"

static int fails = 0;
static void check(const char *what, int cond)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        fails++;
    }
}

// hex string (spaces allowed) -> bytes; returns length
static size_t hex_bytes(const char *hex, uint8_t *out, size_t cap)
{
    size_t n = 0;
    int hi = -1;
    for (const char *p = hex; *p; p++) {
        int v;
        if (*p >= '0' && *p <= '9') {
            v = *p - '0';
        } else if (*p >= 'a' && *p <= 'f') {
            v = *p - 'a' + 10;
        } else if (*p >= 'A' && *p <= 'F') {
            v = *p - 'A' + 10;
        } else {
            continue;
        }
        if (hi < 0) {
            hi = v;
        } else {
            if (n < cap) {
                out[n++] = (uint8_t)((hi << 4) | v);
            }
            hi = -1;
        }
    }
    return n;
}

static String_View field_value(const Hpack_Decoded *d, const char *name)
{
    for (size_t i = 0; i < d->count; i++) {
        if (sv_equal(d->items[i].name, sv_from_cstr(name))) {
            return d->items[i].value;
        }
    }
    return (String_View){ 0, 0 };
}

static void check_field(const Hpack_Decoded *d, const char *name,
                        const char *value)
{
    static char buf[6 + 128 + 256];
    String_View v = field_value(d, name);
    int eq = sv_equal(v, sv_from_cstr(value));
    snprintf(buf, sizeof buf, "%s has value \"%s\"", name, value);
    check(buf, eq);
}

// shared decode helper that also checks the dynamic-table accounting
static int decode(Hpack *hp, const char *hex, Hpack_Decoded *out,
                  size_t exp_count, size_t exp_used)
{
    static char name_buf[256];
    uint8_t bytes[1024];
    size_t n = hex_bytes(hex, bytes, sizeof bytes);
    int rc = hpack_decode_block(hp, bytes, n, out);
    snprintf(name_buf, sizeof name_buf,
             "decode rc=%d count=%zu (want %zu) used=%zu (want %zu)",
             rc, rc == 0 ? out->count : 0, exp_count,
             rc == 0 ? hp->dyn_used : 0, exp_used);
    check(name_buf,
          rc == 0 && out->count == exp_count && hp->dyn_used == exp_used);
    return rc;
}

int main(void)
{
    // ---- RFC 7541 C.4: request sequence (HTTP/2-like decoder context) ----
    {
        Hpack hp;
        hpack_init(&hp);
        Hpack_Decoded d = { 0 };

        check("C.4.1 decode ok",
              decode(&hp,
                     "828684418cf1e3c2e5f23a6ba0ab90f4ff",
                     &d, 4, 57) == 0);
        check_field(&d, ":method", "GET");
        check_field(&d, ":scheme", "http");
        check_field(&d, ":path", "/");
        check_field(&d, ":authority", "www.example.com");
        check("C.4.1 table has one entry", hp.dyn_count == 1);
        hpack_decoded_free(&d);

        check("C.4.2 decode ok",
              decode(&hp, "828684be5886a8eb10649cbf", &d, 5, 110) == 0);
        check_field(&d, ":method", "GET");
        check_field(&d, ":authority", "www.example.com");
        check_field(&d, "cache-control", "no-cache");
        check("C.4.2 table has two entries", hp.dyn_count == 2);
        hpack_decoded_free(&d);

        check("C.4.3 decode ok",
              decode(&hp,
                     "828785bf408825a849e95ba97d7f8925a849e95bb8e8b4bf",
                     &d, 5, 164) == 0);
        check_field(&d, ":scheme", "https");
        check_field(&d, ":path", "/index.html");
        check_field(&d, "custom-key", "custom-value");
        check("C.4.3 table has three entries", hp.dyn_count == 3);
        hpack_decoded_free(&d);

        hpack_free(&hp);
    }

    // ---- RFC 7541 C.5: response sequence without Huffman ----
    {
        Hpack hp;
        hpack_init(&hp);
        hpack_set_max_table_size(&hp, 256);
        Hpack_Decoded d = { 0 };

        check("C.5.1 decode ok",
              decode(&hp,
                     "4803333032580770726976617465611d4d6f6e2c203231204f6374"
                     "20323031332032303a31333a323120474d546e1768747470733a2f2f"
                     "7777772e6578616d706c652e636f6d",
                     &d, 4, 222) == 0);
        check_field(&d, ":status", "302");
        check_field(&d, "cache-control", "private");
        check_field(&d, "date", "Mon, 21 Oct 2013 20:13:21 GMT");
        check_field(&d, "location", "https://www.example.com");
        check("C.5.1 table has four entries", hp.dyn_count == 4);
        hpack_decoded_free(&d);

        check("C.5.2 decode ok",
              decode(&hp, "4803333037c1c0bf", &d, 4, 222) == 0);
        check_field(&d, ":status", "307");
        check_field(&d, "date", "Mon, 21 Oct 2013 20:13:21 GMT");
        check_field(&d, "location", "https://www.example.com");
        hpack_decoded_free(&d);

        check("C.5.3 decode ok",
              decode(&hp,
                     "88c1611d4d6f6e2c203231204f637420323031332032303a31333a"
                     "323220474d54c05a04677a69707738666f6f3d4153444a4b48514b"
                     "425a584f5157454f50495541585157454f49553b206d61782d6167"
                     "653d333630303b2076657273696f6e3d31",
                     &d, 6, 215) == 0);
        check_field(&d, ":status", "200");
        check_field(&d, "cache-control", "private");
        check_field(&d, "date", "Mon, 21 Oct 2013 20:13:22 GMT");
        check_field(&d, "location", "https://www.example.com");
        check_field(&d, "content-encoding", "gzip");
        check_field(&d, "set-cookie",
                    "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1");
        check("C.5.3 table has three entries", hp.dyn_count == 3);
        hpack_decoded_free(&d);

        hpack_free(&hp);
    }

    // ---- RFC 7541 C.6: response sequence with Huffman coding ----
    {
        Hpack hp;
        hpack_init(&hp);
        hpack_set_max_table_size(&hp, 256);
        Hpack_Decoded d = { 0 };

        check("C.6.1 decode ok",
              decode(&hp,
                     "488264025885aec3771a4b6196d07abe941054d444a8200595040b"
                     "8166e082a62d1bff6e919d29ad171863c78f0b97c8e9ae82ae43d3",
                     &d, 4, 222) == 0);
        check_field(&d, ":status", "302");
        check_field(&d, "cache-control", "private");
        check_field(&d, "date", "Mon, 21 Oct 2013 20:13:21 GMT");
        check_field(&d, "location", "https://www.example.com");
        check("C.6.1 table has four entries", hp.dyn_count == 4);
        hpack_decoded_free(&d);

        check("C.6.2 decode ok",
              decode(&hp, "4883640effc1c0bf", &d, 4, 222) == 0);
        check_field(&d, ":status", "307");
        check_field(&d, "date", "Mon, 21 Oct 2013 20:13:21 GMT");
        check_field(&d, "location", "https://www.example.com");
        hpack_decoded_free(&d);

        check("C.6.3 decode ok",
              decode(&hp,
                     "88c16196d07abe941054d444a8200595040b8166e084a62d1bffc0"
                     "5a839bd9ab77ad94e7821dd7f2e6c7b335dfdfcd5b3960d5af2708"
                     "7f3672c1ab270fb5291f9587316065c003ed4ee5b1063d5007",
                     &d, 6, 215) == 0);
        check_field(&d, ":status", "200");
        check_field(&d, "cache-control", "private");
        check_field(&d, "date", "Mon, 21 Oct 2013 20:13:22 GMT");
        check_field(&d, "location", "https://www.example.com");
        check_field(&d, "content-encoding", "gzip");
        check_field(&d, "set-cookie",
                    "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1");
        check("C.6.3 table has three entries", hp.dyn_count == 3);
        hpack_decoded_free(&d);

        hpack_free(&hp);
    }

    // ---- encoder: every static status emits its one-byte indexed answer ----
    {
        Hpack hp;
        hpack_init(&hp);
        Hpack_Decoded d = { 0 };
        Strbuf out;
        strbuf_init(&out);

        struct {
            int status;
            const char *hex;
        } statuses[] = {
            { 200, "88" }, { 204, "89" }, { 206, "8a" }, { 304, "8b" },
            { 400, "8c" }, { 404, "8d" }, { 500, "8e" },
        };
        for (size_t i = 0; i < sizeof statuses / sizeof *statuses; i++) {
            strbuf_free(&out);
            strbuf_init(&out);
            check("status encode ok",
                  hpack_encode_status(&out, (uint16_t)statuses[i].status) == 0);
            check("status encode byte matches",
                  out.count == 1 &&
                  (unsigned char)out.items[0] ==
                      (unsigned char)strtoul(statuses[i].hex, NULL, 16));
            // decode back and confirm the value round-trips
            hpack_decoded_free(&d);
            memset(&d, 0, sizeof d);
            check("status decodes back",
                  hpack_decode_block(&hp, (const uint8_t *)out.items,
                                     out.count, &d) == 0 &&
                  d.count == 1);
            {
                char dec[8];
                snprintf(dec, sizeof dec, "%d", statuses[i].status);
                static char nm[64];
                snprintf(nm, sizeof nm, "status %d round-trips",
                         statuses[i].status);
                check(nm, sv_equal(d.items[0].name, sv_from_cstr(":status")) &&
                              sv_equal(d.items[0].value, sv_from_cstr(dec)));
            }
        }
        strbuf_free(&out);
        hpack_decoded_free(&d);
        hpack_free(&hp);
    }

    // ---- encoder: fields (static and free-form names) round-trip ----
    {
        Hpack hp;
        hpack_init(&hp);
        Hpack_Decoded d = { 0 };
        Strbuf out;
        strbuf_init(&out);

        struct {
            const char *name;
            const char *value;
        } fields[] = {
            { "content-type", "text/plain" },
            { "x-custom-flag", "1" },
            { "set-cookie", "a=b; Path=/" },
        };
        for (size_t i = 0; i < sizeof fields / sizeof *fields; i++) {
            strbuf_free(&out);
            strbuf_init(&out);
            check("field encode ok",
                  hpack_encode_field(&out, sv_from_cstr(fields[i].name),
                                     sv_from_cstr(fields[i].value)) == 0);
            hpack_decoded_free(&d);
            memset(&d, 0, sizeof d);
            check("field decodes back",
                  hpack_decode_block(&hp, (const uint8_t *)out.items,
                                     out.count, &d) == 0 &&
                  d.count == 1);
            {
                static char nm[64];
                snprintf(nm, sizeof nm, "field %s round-trips",
                         fields[i].name);
                check(nm,
                      sv_equal(d.items[0].name,
                               sv_from_cstr(fields[i].name)) &&
                      sv_equal(d.items[0].value,
                               sv_from_cstr(fields[i].value)));
            }
        }
        strbuf_free(&out);
        hpack_decoded_free(&d);
        hpack_free(&hp);
    }

    // ---- protocol edge cases ----
    {
        Hpack hp;
        hpack_init(&hp);
        Hpack_Decoded d = { 0 };

        // size update 256 at block start, then indexed :status 200
        {
            uint8_t b[] = { 0x3f, 0xe1, 0x01, 0x88 };
            check("size-update prefix accepted",
                  hpack_decode_block(&hp, b, sizeof b, &d) == 0 &&
                  d.count == 1 && hp.dyn_max_capacity == 256);
        }
        hpack_decoded_free(&d);

        // size update mid-block is a compression error
        {
            Hpack hp2;
            hpack_init(&hp2);
            uint8_t b[] = { 0x88, 0x3f, 0xe1, 0x01 };
            Hpack_Decoded d2 = { 0 };
            check("mid-block size update rejected",
                  hpack_decode_block(&hp2, b, sizeof b, &d2) != 0);
            hpack_decoded_free(&d2);
            hpack_free(&hp2);
        }

        // size update above the 4096 decode cap is rejected
        {
            Hpack hp2;
            hpack_init(&hp2);
            uint8_t b[] = { 0x3f, 0x41, 0x1f }; // 4096 exceeds cap
            Hpack_Decoded d2 = { 0 };
            check("oversized size update rejected",
                  hpack_decode_block(&hp2, b, sizeof b, &d2) != 0);
            hpack_decoded_free(&d2);
            hpack_free(&hp2);
        }

        // EOS as Huffman data is a decode error
        {
            Hpack hp2;
            hpack_init(&hp2);
            uint8_t b[] = { 0x00, 0x82 };
            Hpack_Decoded d2 = { 0 };
            check("EOS sequence rejected",
                  hpack_decode_block(&hp2, b, sizeof b, &d2) != 0);
            hpack_decoded_free(&d2);
            hpack_free(&hp2);
        }

        // bad padding: all-ones tail without the required floor rounding
        {
            Hpack hp2;
            hpack_init(&hp2);
            uint8_t b[] = { 0x11, 0x07, 'c', 'u', 's', 't', 'o', 'm', 'k',
                            0x02, 0xff, 0x00 };
            Hpack_Decoded d2 = { 0 };
            check("bad Huffman padding rejected",
                  hpack_decode_block(&hp2, b, sizeof b, &d2) != 0);
            hpack_decoded_free(&d2);
            hpack_free(&hp2);
        }

        // never-indexed literals decode but add no table entry
        {
            Hpack hp2;
            hpack_init(&hp2);
            uint8_t b[] = { 0x10, 0x07, 'c', 'u', 's', 't', 'o', 'm', 'k',
                            0x07, 'c', 'u', 's', 't', 'o', 'm', 'v' };
            Hpack_Decoded d2 = { 0 };
            check("never-indexed decodes",
                  hpack_decode_block(&hp2, b, sizeof b, &d2) == 0 &&
                  d2.count == 1 &&
                  sv_equal(d2.items[0].name, sv_from_cstr("customk")) &&
                  sv_equal(d2.items[0].value, sv_from_cstr("customv")));
            check("never-indexed adds nothing", hp2.dyn_count == 0);
            hpack_decoded_free(&d2);
            hpack_free(&hp2);
        }

        hpack_free(&hp);
    }

    if (fails == 0) {
        printf("hpack ok\n");
    }
    return fails != 0;
}