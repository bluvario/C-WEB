#include <math.h>
#include <stdio.h>
#include <string.h>

#include "json.h"
#include "strbuf.h"
#include "sv.h"
#include "xmem.h"

static int check(const char *what, int cond)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        return 1;
    }
    return 0;
}

// a whole document that parses: everything, in one go
static int parse_ok(const char *text)
{
    Json_Parse_Result pr = json_parse(sv_from_cstr(text));
    int ok = pr.error == NULL;
    if (pr.error == NULL) {
        json_free_value(&pr.root);
    } else {
        fprintf(stderr, "unexpected error for '%s': %s\n", text, pr.error);
    }
    xfree(pr.error);
    return ok;
}

static int parse_fails(const char *text)
{
    Json_Parse_Result pr = json_parse(sv_from_cstr(text));
    int ok = pr.error != NULL;
    if (pr.error == NULL) {
        json_free_value(&pr.root);
    }
    xfree(pr.error);
    return ok;
}

static int json_equal(const Json_Value *a, const Json_Value *b)
{
    if (a->type != b->type) {
        return 0;
    }
    switch (a->type) {
        case JSON_NULL:
            return 1;
        case JSON_BOOL:
            return a->as.boolean == b->as.boolean;
        case JSON_NUMBER:
            return a->as.number == b->as.number;
        case JSON_STRING:
            return strcmp(a->as.string, b->as.string) == 0;
        case JSON_ARRAY:
            if (a->as.array.count != b->as.array.count) {
                return 0;
            }
            for (size_t i = 0; i < a->as.array.count; i++) {
                if (!json_equal(&a->as.array.items[i], &b->as.array.items[i])) {
                    return 0;
                }
            }
            return 1;
        case JSON_OBJECT:
            if (a->as.object.count != b->as.object.count) {
                return 0;
            }
            for (size_t i = 0; i < a->as.object.count; i++) {
                const Json_Value *other = json_get(b, a->as.object.items[i].key);
                if (other == NULL ||
                    !json_equal(&a->as.object.items[i].value, other)) {
                    return 0;
                }
            }
            return 1;
    }
    return 0;
}

int main(void)
{
    int fails = 0;

    Json_Parse_Result pr;

    pr = json_parse(sv_from_cstr("{"
                                 "  \"name\": \"C-WEB\","
                                 "  \"score\": 4.5,"
                                 "  \"stars\": 7,"
                                 "  \"signed\": -3,"
                                 "  \"big\": 2.5e3,"
                                 "  \"ok\": true,"
                                 "  \"no\": false,"
                                 "  \"empty\": null,"
                                 "  \"tags\": [\"c\", \"http\", 1, 2],"
                                 "  \"nested\": {\"a\": {\"b\": [1, [2, [3]]]}}"
                                 "}"));
    if (pr.error) {
        fprintf(stderr, "big document failed: %s\n", pr.error);
        xfree(pr.error);
        return 1;
    }
    fails += check("name string", strcmp(json_get_string(&pr.root, "name"), "C-WEB") == 0);
    fails += check("decimal", json_get_number(&pr.root, "score") == 4.5);
    fails += check("integer", json_get_number(&pr.root, "stars") == 7.0);
    fails += check("negative", json_get_number(&pr.root, "signed") == -3.0);
    fails += check("exponent", json_get_number(&pr.root, "big") == 2500.0);
    fails += check("true", json_get_bool(&pr.root, "ok"));
    fails += check("false not true", !json_get_bool(&pr.root, "no"));

    const Json_Value *tags = json_get(&pr.root, "tags");
    fails += check("array length", json_array_length(tags) == 4);
    json_free_value(&pr.root);

    // escapes and unicode
    pr = json_parse(sv_from_cstr("\"quote \\\" back \\\\ slash \\/ nl \\n tab \\t\\r\\b\\f \\u0041\\u00e9\""));
    if (pr.error) {
        fprintf(stderr, "escape string failed: %s\n", pr.error);
        xfree(pr.error);
        return 1;
    }
    const char *got = pr.root.as.string;
    fails += check("string decoded", pr.root.type == JSON_STRING &&
                   strcmp(got, "quote \" back \\ slash / nl \n tab \t\r\b\f Aé") == 0);
    json_free_value(&pr.root);

    // surrogate pair -> U+1F600 😀
    pr = json_parse(sv_from_cstr("\"\\ud83d\\ude00\""));
    if (pr.error) {
        fprintf(stderr, "surrogate failed: %s\n", pr.error);
        xfree(pr.error);
        return 1;
    }
    const unsigned char *smile = (const unsigned char *)pr.root.as.string;
    fails += check("surrogate pair encodes",
                   pr.root.type == JSON_STRING &&
                   smile[0] == 0xF0 && smile[1] == 0x9F && smile[2] == 0x98 && smile[3] == 0x80 &&
                   smile[4] == 0);
    json_free_value(&pr.root);

    // whitespace tolerance
    fails += check("whitespace tolerated", parse_ok("\n\t[ 1 , 2 ] \r\n"));

    // malformed documents all have to fail
    fails += check("empty input fails", parse_fails(""));
    fails += check("garbage fails", parse_fails("hello"));
    fails += check("trailing junk fails", parse_fails("{} junk"));
    fails += check("unterminated string fails", parse_fails("\"abc"));
    fails += check("bad escape fails", parse_fails("\"\\x\""));
    fails += check("unterminated array fails", parse_fails("[1,2"));
    fails += check("trailing comma fails", parse_fails("[1,]"));
    fails += check("object without colon fails", parse_fails("{\"a\" 1}"));
    fails += check("unquoted key fails", parse_fails("{a:1}"));
    fails += check("bare number junk fails", parse_fails("1e"));
    fails += check("dot without digits fails", parse_fails("1."));
    fails += check("missing fraction digits fails", parse_fails("-"));
    fails += check("control char in string fails", parse_fails("\"a\x01b\""));
    fails += check("lone high surrogate fails", parse_fails("\"\\ud800\""));
    fails += check("duplicate keys still parse", parse_ok("{\"a\":1,\"a\":2}"));

    // builders compose into a tree and survive a round-trip through get()
    Json_Value user = json_make_object();
    json_object_set(&user, "id", json_make_number(7));
    json_object_set(&user, "name", json_make_string("alice"));
    Json_Value list = json_make_array();
    json_array_append(&list, json_make_bool(true));
    json_array_append(&list, json_make_null());
    json_object_set(&user, "flags", list);
    fails += check("builder number", json_get_number(&user, "id") == 7.0);
    fails += check("builder string", strcmp(json_get_string(&user, "name"), "alice") == 0);
    const Json_Value *uf = json_get(&user, "flags");
    fails += check("builder array len", json_array_length(uf) == 2);
    fails += check("builder null element", json_array_get(uf, 1)->type == JSON_NULL);
    // overwriting a key frees the old value
    json_object_set(&user, "id", json_make_number(9));
    fails += check("builder overwrite", json_get_number(&user, "id") == 9.0);
    json_free_value(&user);

    // serialization: compact bytes are pinned exactly
    Json_Value doc = json_make_object();
    json_object_set(&doc, "name", json_make_string("a\"b\\c\nd e"));
    Json_Value nums = json_make_array();
    json_array_append(&nums, json_make_number(0));
    json_array_append(&nums, json_make_number(4.5));
    json_array_append(&nums, json_make_number(-7));
    json_array_append(&nums, json_make_number(0.25));
    json_object_set(&doc, "nums", nums);
    json_object_set(&doc, "on", json_make_bool(true));
    json_object_set(&doc, "off", json_make_bool(false));
    json_object_set(&doc, "nil", json_make_null());
    json_object_set(&doc, "empty", json_make_array());

    Strbuf wire;
    strbuf_init(&wire);
    json_serialize(&doc, &wire);
    strbuf_null_terminate(&wire);
    const char *want = "{\"name\":\"a\\\"b\\\\c\\nd e\","
                       "\"nums\":[0,4.5,-7,0.25],"
                       "\"on\":true,\"off\":false,\"nil\":null,\"empty\":[]}";
    fails += check("compact serialize pinned", strcmp(wire.items, want) == 0);
    if (strcmp(wire.items, want) != 0) {
        fprintf(stderr, "got  %s\nwant %s\n", wire.items, want);
    }

    // pretty serialize stays the same tree when re-parsed
    strbuf_free(&wire);
    strbuf_init(&wire);
    json_serialize_pretty(&doc, &wire, 2);
    strbuf_null_terminate(&wire);
    fails += check("pretty serialize prints newlines", strstr(wire.items, "\n  \"name\":") != NULL);
    if (strstr(wire.items, "\n  \"name\":") == NULL) {
        fprintf(stderr, "pretty output:\n%s\n", wire.items);
    }
    Json_Parse_Result reparsed = json_parse(sv_from_cstr(wire.items));
    fails += check("pretty output reparses", reparsed.error == NULL);
    if (reparsed.error == NULL) {
        fails += check("reparse equals original", json_equal(&doc, &reparsed.root));
        json_free_value(&reparsed.root);
    } else {
        xfree(reparsed.error);
    }
    strbuf_free(&wire);
    json_free_value(&doc);

    // a parsed document serializes back to equivalent text
    Json_Parse_Result rt = json_parse(sv_from_cstr("{\"x\":[1,2.5],\"y\":\"\\n\"}"));
    strbuf_init(&wire);
    json_serialize(&rt.root, &wire);
    strbuf_null_terminate(&wire);
    fails += check("parse->serialize round trip",
                   strcmp(wire.items, "{\"x\":[1,2.5],\"y\":\"\\n\"}") == 0);
    if (strcmp(wire.items, "{\"x\":[1,2.5],\"y\":\"\\n\"}") != 0) {
        fprintf(stderr, "round trip got %s\n", wire.items);
    }
    strbuf_free(&wire);
    json_free_value(&rt.root);

    if (fails == 0) {
        printf("json ok\n");
    }
    return fails != 0;
}