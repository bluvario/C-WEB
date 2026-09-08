#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "da.h"
#include "strbuf.h"
#include "sv.h"
#include "xmem.h"

// nested values deeper than this are a stack-overflow waiting to happen; the
// RFC suggests implementations set a limit, so this one is ours
static const int JSON_MAX_DEPTH = 512;

typedef struct {
    const char *p;
    const char *end;
    const char *error; // static reason, "" when fine
} Parser;

static void fail(Parser *ps, const char *why)
{
    if (ps->error == NULL) {
        ps->error = why;
    }
}

static void skip_ws(Parser *ps)
{
    while (ps->p < ps->end) {
        char c = *ps->p;
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            break;
        }
        ps->p++;
    }
}

// --- string ---------------------------------------------------------------

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static bool parse_hex4(Parser *ps, unsigned *out)
{
    if (ps->end - ps->p < 4) {
        fail(ps, "truncated \\u escape");
        return false;
    }
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        int d = hex_val(ps->p[i]);
        if (d < 0) {
            fail(ps, "bad \\u escape");
            return false;
        }
        v = (v << 4) | (unsigned)d;
    }
    ps->p += 4;
    *out = v;
    return true;
}

// UTF-8 encode one code point (surrogate pairs already merged)
static void utf8_append(Strbuf *out, unsigned cp)
{
    if (cp < 0x80) {
        strbuf_append_char(out, (char)cp);
    } else if (cp < 0x800) {
        strbuf_append_char(out, (char)(0xC0 | (cp >> 6)));
        strbuf_append_char(out, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        strbuf_append_char(out, (char)(0xE0 | (cp >> 12)));
        strbuf_append_char(out, (char)(0x80 | ((cp >> 6) & 0x3F)));
        strbuf_append_char(out, (char)(0x80 | (cp & 0x3F)));
    } else {
        strbuf_append_char(out, (char)(0xF0 | (cp >> 18)));
        strbuf_append_char(out, (char)(0x80 | ((cp >> 12) & 0x3F)));
        strbuf_append_char(out, (char)(0x80 | ((cp >> 6) & 0x3F)));
        strbuf_append_char(out, (char)(0x80 | (cp & 0x3F)));
    }
}

static bool parse_string_raw(Parser *ps, Strbuf *out)
{
    if (ps->p >= ps->end || *ps->p != '"') {
        fail(ps, "expected a string");
        return false;
    }
    ps->p++; // opening quote

    while (ps->p < ps->end) {
        char c = *ps->p++;
        if (c == '"') {
            return true;
        }
        if (c == '\\') {
            if (ps->p >= ps->end) {
                fail(ps, "truncated escape");
                return false;
            }
            char e = *ps->p++;
            switch (e) {
                case '"':
                case '\\':
                case '/':
                    strbuf_append_char(out, e);
                    break;
                case 'b':
                    strbuf_append_char(out, '\b');
                    break;
                case 'f':
                    strbuf_append_char(out, '\f');
                    break;
                case 'n':
                    strbuf_append_char(out, '\n');
                    break;
                case 'r':
                    strbuf_append_char(out, '\r');
                    break;
                case 't':
                    strbuf_append_char(out, '\t');
                    break;
                case 'u': {
                    unsigned cp;
                    if (!parse_hex4(ps, &cp)) {
                        return false;
                    }
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        // a high surrogate must be followed by a low one
                        if (ps->end - ps->p >= 2 && ps->p[0] == '\\' && ps->p[1] == 'u') {
                            unsigned lo;
                            ps->p += 2;
                            if (!parse_hex4(ps, &lo)) {
                                return false;
                            }
                            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            } else {
                                fail(ps, "lone high surrogate");
                                return false;
                            }
                        } else {
                            fail(ps, "lone high surrogate");
                            return false;
                        }
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        fail(ps, "lone low surrogate");
                        return false;
                    }
                    utf8_append(out, cp);
                    break;
                }
                default:
                    fail(ps, "unknown escape");
                    return false;
            }
        } else if ((unsigned char)c < 0x20) {
            fail(ps, "control character in string");
            return false;
        } else {
            strbuf_append_char(out, c);
        }
    }
    fail(ps, "unterminated string");
    return false;
}

static bool parse_string(Parser *ps, Json_Value *out)
{
    Strbuf buf;
    strbuf_init(&buf);
    if (!parse_string_raw(ps, &buf)) {
        strbuf_free(&buf);
        memset(out, 0, sizeof *out);
        return false;
    }
    strbuf_append_char(&buf, '\0'); // NUL terminator the node owns
    out->type = JSON_STRING;
    out->as.string = buf.items;
    return true;
}

// --- numbers --------------------------------------------------------------

static bool parse_number(Parser *ps, Json_Value *out)
{
    const char *p = ps->p;
    if (p < ps->end && *p == '-') {
        p++;
    }
    if (p >= ps->end || *p < '0' || *p > '9') {
        fail(ps, "malformed number");
        memset(out, 0, sizeof *out);
        return false;
    }
    while (p < ps->end && *p >= '0' && *p <= '9') {
        p++;
    }
    if (p < ps->end && *p == '.') {
        p++;
        if (p >= ps->end || *p < '0' || *p > '9') {
            fail(ps, "malformed number");
            memset(out, 0, sizeof *out);
            return false;
        }
        while (p < ps->end && *p >= '0' && *p <= '9') {
            p++;
        }
    }
    if (p < ps->end && (*p == 'e' || *p == 'E')) {
        p++;
        if (p < ps->end && (*p == '+' || *p == '-')) {
            p++;
        }
        if (p >= ps->end || *p < '0' || *p > '9') {
            fail(ps, "malformed exponent");
            memset(out, 0, sizeof *out);
            return false;
        }
        while (p < ps->end && *p >= '0' && *p <= '9') {
            p++;
        }
    }

    size_t len = (size_t)(p - ps->p);
    char buf[64];
    if (len >= sizeof(buf)) {
        fail(ps, "number too long");
        memset(out, 0, sizeof *out);
        return false;
    }
    memcpy(buf, ps->p, len);
    buf[len] = '\0';
    ps->p = p;
    out->type = JSON_NUMBER;
    out->as.number = strtod(buf, NULL);
    return true;
}

// --- containers -----------------------------------------------------------

static bool parse_value(Parser *ps, Json_Value *out, int depth);

static bool parse_array(Parser *ps, Json_Value *out, int depth)
{
    ps->p++; // '['
    Json_Value arr = json_make_array();
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == ']') {
        ps->p++;
        *out = arr;
        return true;
    }
    for (;;) {
        Json_Value item;
        if (!parse_value(ps, &item, depth)) {
            json_free_value(&arr);
            memset(out, 0, sizeof *out);
            return false;
        }
        json_array_append(&arr, item);
        skip_ws(ps);
        if (ps->p >= ps->end) {
            fail(ps, "unterminated array");
            json_free_value(&arr);
            memset(out, 0, sizeof *out);
            return false;
        }
        char c = *ps->p++;
        if (c == ',') {
            continue;
        }
        if (c == ']') {
            *out = arr;
            return true;
        }
        fail(ps, "expected , or ] in array");
        json_free_value(&arr);
        memset(out, 0, sizeof *out);
        return false;
    }
}

static bool parse_object(Parser *ps, Json_Value *out, int depth)
{
    ps->p++; // '{'
    Json_Value obj = json_make_object();
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == '}') {
        ps->p++;
        *out = obj;
        return true;
    }
    for (;;) {
        skip_ws(ps);
        Strbuf key;
        strbuf_init(&key);
        if (ps->p >= ps->end || *ps->p != '"') {
            fail(ps, "object keys must be strings");
            strbuf_free(&key);
            json_free_value(&obj);
            memset(out, 0, sizeof *out);
            return false;
        }
        if (!parse_string_raw(ps, &key)) {
            strbuf_free(&key);
            json_free_value(&obj);
            memset(out, 0, sizeof *out);
            return false;
        }
        strbuf_append_char(&key, '\0');

        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != ':') {
            fail(ps, "expected : after object key");
            xfree(key.items);
            json_free_value(&obj);
            memset(out, 0, sizeof *out);
            return false;
        }
        ps->p++;

        Json_Value val;
        if (!parse_value(ps, &val, depth)) {
            xfree(key.items);
            json_free_value(&obj);
            memset(out, 0, sizeof *out);
            return false;
        }
        json_object_set(&obj, key.items, val);
        xfree(key.items);

        skip_ws(ps);
        if (ps->p >= ps->end) {
            fail(ps, "unterminated object");
            json_free_value(&obj);
            memset(out, 0, sizeof *out);
            return false;
        }
        char c = *ps->p++;
        if (c == ',') {
            continue;
        }
        if (c == '}') {
            *out = obj;
            return true;
        }
        fail(ps, "expected , or } in object");
        json_free_value(&obj);
        memset(out, 0, sizeof *out);
        return false;
    }
}

static bool parse_value(Parser *ps, Json_Value *out, int depth)
{
    if (depth > JSON_MAX_DEPTH) {
        fail(ps, "nesting too deep");
        memset(out, 0, sizeof *out);
        return false;
    }
    skip_ws(ps);
    if (ps->p >= ps->end) {
        fail(ps, "unexpected end of input");
        memset(out, 0, sizeof *out);
        return false;
    }

    switch (*ps->p) {
        case '{':
            return parse_object(ps, out, depth + 1);
        case '[':
            return parse_array(ps, out, depth + 1);
        case '"':
            return parse_string(ps, out);
        case 'n':
            if (ps->end - ps->p >= 4 && memcmp(ps->p, "null", 4) == 0) {
                ps->p += 4;
                *out = json_make_null();
                return true;
            }
            break;
        case 't':
            if (ps->end - ps->p >= 4 && memcmp(ps->p, "true", 4) == 0) {
                ps->p += 4;
                *out = json_make_bool(true);
                return true;
            }
            break;
        case 'f':
            if (ps->end - ps->p >= 5 && memcmp(ps->p, "false", 5) == 0) {
                ps->p += 5;
                *out = json_make_bool(false);
                return true;
            }
            break;
        case '-':
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
            return parse_number(ps, out);
        default:
            break;
    }
    fail(ps, "expected a JSON value");
    memset(out, 0, sizeof *out);
    return false;
}

Json_Parse_Result json_parse(String_View src)
{
    Json_Parse_Result pr;
    memset(&pr, 0, sizeof pr);
    Parser ps = {src.data, src.data + src.count, NULL};

    skip_ws(&ps);
    if (!parse_value(&ps, &pr.root, 0)) {
        pr.error = xmalloc(strlen(ps.error) + 1);
        strcpy(pr.error, ps.error);
        return pr;
    }
    skip_ws(&ps);
    if (ps.p != ps.end) {
        pr.error = xmalloc(strlen("trailing characters after value") + 1);
        strcpy(pr.error, "trailing characters after value");
        json_free_value(&pr.root);
        memset(&pr.root, 0, sizeof pr.root);
        return pr;
    }
    return pr;
}

void json_parse_result_free(Json_Parse_Result *pr)
{
    if (pr->error == NULL) {
        json_free_value(&pr->root);
    }
    xfree(pr->error);
    pr->error = NULL;
}

// --- lookups --------------------------------------------------------------

const Json_Value *json_get(const Json_Value *obj, const char *name)
{
    if (obj == NULL || obj->type != JSON_OBJECT || name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < obj->as.object.count; i++) {
        if (strcmp(obj->as.object.items[i].key, name) == 0) {
            return &obj->as.object.items[i].value;
        }
    }
    return NULL;
}

const char *json_get_string(const Json_Value *obj, const char *name)
{
    const Json_Value *v = json_get(obj, name);
    if (v == NULL || v->type != JSON_STRING) {
        return NULL;
    }
    return v->as.string;
}

double json_get_number(const Json_Value *obj, const char *name)
{
    const Json_Value *v = json_get(obj, name);
    if (v == NULL || v->type != JSON_NUMBER) {
        return 0.0;
    }
    return v->as.number;
}

bool json_get_bool(const Json_Value *obj, const char *name)
{
    const Json_Value *v = json_get(obj, name);
    if (v == NULL || v->type != JSON_BOOL) {
        return false;
    }
    return v->as.boolean;
}

size_t json_array_length(const Json_Value *arr)
{
    if (arr == NULL || arr->type != JSON_ARRAY) {
        return 0;
    }
    return arr->as.array.count;
}

const Json_Value *json_array_get(const Json_Value *arr, size_t index)
{
    if (arr == NULL || arr->type != JSON_ARRAY || index >= arr->as.array.count) {
        return NULL;
    }
    return &arr->as.array.items[index];
}

// --- builders -------------------------------------------------------------

void json_free_value(Json_Value *v)
{
    switch (v->type) {
        case JSON_STRING:
            xfree(v->as.string);
            break;
        case JSON_ARRAY:
            for (size_t i = 0; i < v->as.array.count; i++) {
                json_free_value(&v->as.array.items[i]);
            }
            xfree(v->as.array.items);
            break;
        case JSON_OBJECT:
            for (size_t i = 0; i < v->as.object.count; i++) {
                xfree(v->as.object.items[i].key);
                json_free_value(&v->as.object.items[i].value);
            }
            xfree(v->as.object.items);
            break;
        default:
            break;
    }
    memset(v, 0, sizeof *v);
}

Json_Value json_make_null(void)
{
    Json_Value v;
    memset(&v, 0, sizeof v);
    v.type = JSON_NULL;
    return v;
}

Json_Value json_make_bool(bool b)
{
    Json_Value v;
    memset(&v, 0, sizeof v);
    v.type = JSON_BOOL;
    v.as.boolean = b;
    return v;
}

Json_Value json_make_number(double n)
{
    Json_Value v;
    memset(&v, 0, sizeof v);
    v.type = JSON_NUMBER;
    v.as.number = n;
    return v;
}

Json_Value json_make_string(const char *s)
{
    Json_Value v;
    memset(&v, 0, sizeof v);
    v.type = JSON_STRING;
    v.as.string = xmalloc(strlen(s) + 1);
    strcpy(v.as.string, s);
    return v;
}

Json_Value json_make_array(void)
{
    Json_Value v;
    memset(&v, 0, sizeof v);
    v.type = JSON_ARRAY;
    return v;
}

Json_Value json_make_object(void)
{
    Json_Value v;
    memset(&v, 0, sizeof v);
    v.type = JSON_OBJECT;
    return v;
}

void json_array_append(Json_Value *arr, Json_Value child)
{
    da_append(&arr->as.array, child);
}

void json_object_set(Json_Value *obj, const char *key, Json_Value child)
{
    Json_Pair pair;
    pair.key = xmalloc(strlen(key) + 1);
    strcpy(pair.key, key);
    pair.value = child;
    // a duplicate key is replaced, letting the old value go
    for (size_t i = 0; i < obj->as.object.count; i++) {
        if (strcmp(obj->as.object.items[i].key, key) == 0) {
            json_free_value(&obj->as.object.items[i].value);
            obj->as.object.items[i].value = child;
            xfree(pair.key);
            return;
        }
    }
    da_append(&obj->as.object, pair);
}