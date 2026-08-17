/*
 * json.h - Minimal recursive-descent JSON parser (RFC 8259).
 *
 * Produces a read-only DOM: objects and arrays hold a linked list of children,
 * object members carry their name in `key`. Strings are decoded to UTF-8,
 * escapes and surrogate pairs included. Enough for the mappack digest, with no
 * external dependencies.
 */
#ifndef TABBER_JSON_H
#define TABBER_JSON_H

#include <stddef.h>

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} json_type;

typedef struct json_value json_value;
struct json_value {
    json_type type;
    char *key;             /* member name, NULL outside of objects */
    char *string;          /* JSON_STRING */
    double number;         /* JSON_NUMBER */
    int boolean;           /* JSON_BOOL */
    json_value *children;  /* JSON_ARRAY / JSON_OBJECT: first element */
    json_value *last_child;
    json_value *next;      /* next sibling */
    size_t count;          /* number of children */
};

/* Parses a document; returns NULL and fills `err` on failure. */
json_value *json_parse(const char *text, char *err, size_t errsz);

void json_free(json_value *value);

/* Member of an object by name, or NULL. Names are matched case-sensitively. */
const json_value *json_get(const json_value *object, const char *key);

/* Element of an array by index, or NULL. */
const json_value *json_at(const json_value *array, size_t index);

/* Number of children of an array or object; 0 for anything else. */
size_t json_count(const json_value *value);

/* Typed accessors on a member; each falls back when absent or mistyped. */
const char *json_get_string(const json_value *object, const char *key, const char *fallback);
long        json_get_int(const json_value *object, const char *key, long fallback);
int         json_get_bool(const json_value *object, const char *key, int fallback);

#endif /* TABBER_JSON_H */
