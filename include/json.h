#ifndef CWEB_JSON_H
#define CWEB_JSON_H

#include <stdbool.h>
#include <stddef.h>

#include "strbuf.h"
#include "sv.h"

// JSON value tree. nodes own their memory: a string owns its bytes, arrays
// and objects own their elements. copies of a Json_Value share that memory,
// so transfer nodes into a container and then leave them alone.
typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT,
} Json_Type;

typedef struct Json_Value Json_Value;
typedef struct Json_Pair Json_Pair;

struct Json_Value {
    Json_Type type;
    union {
        bool boolean;
        double number;
        char *string; // owned, NUL-terminated copy
        struct {
            Json_Value *items;
            size_t count;
            size_t capacity;
        } array;
        struct {
            Json_Pair *items;
            size_t count;
            size_t capacity;
        } object;
    } as;
};

struct Json_Pair {
    char *key; // owned, NUL-terminated
    Json_Value value;
};

typedef struct {
    Json_Value root; // only valid when error == NULL
    char *error;     // owned message, NULL on success
} Json_Parse_Result;

// parses a whole document, trailing whitespace aside. root is freed with
// json_parse_result_free, which no-ops on a failed parse.
Json_Parse_Result json_parse(String_View src);
void json_parse_result_free(Json_Parse_Result *pr);

// object field lookups: NULL / 0 / false when missing or the wrong type
const Json_Value *json_get(const Json_Value *obj, const char *name);
const char *json_get_string(const Json_Value *obj, const char *name);
double json_get_number(const Json_Value *obj, const char *name);
bool json_get_bool(const Json_Value *obj, const char *name);
size_t json_array_length(const Json_Value *arr);
const Json_Value *json_array_get(const Json_Value *arr, size_t index);

// builders. append/set transfer ownership into the container; the submitted
// node must not be freed or reused afterwards. json_free_value frees a tree.
void json_free_value(Json_Value *v);
Json_Value json_make_null(void);
Json_Value json_make_bool(bool b);
Json_Value json_make_number(double n);
Json_Value json_make_string(const char *s);
Json_Value json_make_array(void);
Json_Value json_make_object(void);
void json_array_append(Json_Value *arr, Json_Value child);
void json_object_set(Json_Value *obj, const char *key, Json_Value child);

// render a tree back to JSON text. compact for the wire, pretty with the
// given indent width for humans. escapes quotes, backslashes and control
// characters; numbers go out with enough digits to survive a round trip.
void json_serialize(const Json_Value *v, Strbuf *out);
void json_serialize_pretty(const Json_Value *v, Strbuf *out, int spaces);

#endif