#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "util.h"

#define JSON_MAX_DEPTH 64   /* nesting guard against runaway recursion */

typedef struct {
    const char *start;   /* kept for error offsets */
    const char *p;
    const char *end;
    char *err;
    size_t errsz;
} json_parser;

static json_value *json_parse_value(json_parser *ps, int depth);

/* ---- Helpers ----------------------------------------------------------- */

static json_value *json_new(json_type type)
{
    json_value *v = xmalloc(sizeof(*v));
    memset(v, 0, sizeof(*v));
    v->type = type;
    return v;
}

static void json_append(json_value *parent, json_value *child)
{
    if (parent->last_child)
        parent->last_child->next = child;
    else
        parent->children = child;
    parent->last_child = child;
    parent->count++;
}

static void json_skip_space(json_parser *ps)
{
    while (ps->p < ps->end) {
        char c = *ps->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            ps->p++;
        else
            break;
    }
}

/* Reports an error, pointing at the offset where parsing stopped. */
static void json_fail(json_parser *ps, const char *what)
{
    err_set(ps->err, ps->errsz, "%s at offset %lu",
            what, (unsigned long)(ps->p - ps->start));
}

/* ---- Scalars ----------------------------------------------------------- */

/* Encodes one Unicode codepoint as UTF-8 into a buffer. */
static void json_utf8_encode(byte_buf *out, unsigned long cp)
{
    char tmp[4];

    if (cp <= 0x7F) {
        tmp[0] = (char)cp;
        buf_append(out, tmp, 1);
    } else if (cp <= 0x7FF) {
        tmp[0] = (char)(0xC0 | (cp >> 6));
        tmp[1] = (char)(0x80 | (cp & 0x3F));
        buf_append(out, tmp, 2);
    } else if (cp <= 0xFFFF) {
        tmp[0] = (char)(0xE0 | (cp >> 12));
        tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[2] = (char)(0x80 | (cp & 0x3F));
        buf_append(out, tmp, 3);
    } else {
        tmp[0] = (char)(0xF0 | (cp >> 18));
        tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[3] = (char)(0x80 | (cp & 0x3F));
        buf_append(out, tmp, 4);
    }
}

/* Reads the 4 hex digits of a \u escape; -1 on malformed input. */
static int json_read_hex4(json_parser *ps, unsigned *out)
{
    unsigned value = 0;
    int i;

    if (ps->end - ps->p < 4)
        return -1;
    for (i = 0; i < 4; i++) {
        char c = *ps->p++;
        value <<= 4;
        if (c >= '0' && c <= '9')      value |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') value |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') value |= (unsigned)(c - 'A' + 10);
        else return -1;
    }
    *out = value;
    return 0;
}

/* Parses a quoted string (cursor must sit on the opening quote). */
static char *json_parse_string(json_parser *ps)
{
    byte_buf out = {0};

    ps->p++;   /* opening quote */
    while (ps->p < ps->end && *ps->p != '"') {
        char c = *ps->p++;

        if (c != '\\') {
            buf_append(&out, &c, 1);
            continue;
        }
        if (ps->p >= ps->end)
            break;

        switch (*ps->p++) {
            case '"':  buf_append(&out, "\"", 1); break;
            case '\\': buf_append(&out, "\\", 1); break;
            case '/':  buf_append(&out, "/", 1);  break;
            case 'b':  buf_append(&out, "\b", 1); break;
            case 'f':  buf_append(&out, "\f", 1); break;
            case 'n':  buf_append(&out, "\n", 1); break;
            case 'r':  buf_append(&out, "\r", 1); break;
            case 't':  buf_append(&out, "\t", 1); break;
            case 'u': {
                unsigned cp;
                if (json_read_hex4(ps, &cp) != 0) {
                    err_set(ps->err, ps->errsz, "malformed \\u escape");
                    buf_free(&out);
                    return NULL;
                }
                /* Combine surrogate pairs into a single codepoint. */
                if (cp >= 0xD800 && cp <= 0xDBFF && ps->end - ps->p >= 6 &&
                    ps->p[0] == '\\' && ps->p[1] == 'u') {
                    unsigned low;
                    const char *save = ps->p;
                    ps->p += 2;
                    if (json_read_hex4(ps, &low) == 0 && low >= 0xDC00 && low <= 0xDFFF) {
                        json_utf8_encode(&out, 0x10000UL +
                                               (((unsigned long)cp - 0xD800) << 10) +
                                               ((unsigned long)low - 0xDC00));
                        break;
                    }
                    ps->p = save;   /* not a valid pair after all */
                }
                json_utf8_encode(&out, cp);
                break;
            }
            default:
                err_set(ps->err, ps->errsz, "unknown escape sequence");
                buf_free(&out);
                return NULL;
        }
    }

    if (ps->p >= ps->end) {
        err_set(ps->err, ps->errsz, "unterminated string");
        buf_free(&out);
        return NULL;
    }
    ps->p++;   /* closing quote */
    return buf_finish(&out, NULL);
}

/* ---- Composites -------------------------------------------------------- */

static json_value *json_parse_object(json_parser *ps, int depth)
{
    json_value *obj = json_new(JSON_OBJECT);

    ps->p++;   /* '{' */
    json_skip_space(ps);
    if (ps->p < ps->end && *ps->p == '}') {
        ps->p++;
        return obj;
    }

    for (;;) {
        json_value *member;
        char *key;

        json_skip_space(ps);
        if (ps->p >= ps->end || *ps->p != '"') {
            err_set(ps->err, ps->errsz, "expected a member name in object");
            json_free(obj);
            return NULL;
        }
        key = json_parse_string(ps);
        if (!key) {
            json_free(obj);
            return NULL;
        }

        json_skip_space(ps);
        if (ps->p >= ps->end || *ps->p != ':') {
            err_set(ps->err, ps->errsz, "expected ':' after member name '%s'", key);
            free(key);
            json_free(obj);
            return NULL;
        }
        ps->p++;

        member = json_parse_value(ps, depth + 1);
        if (!member) {
            free(key);
            json_free(obj);
            return NULL;
        }
        member->key = key;
        json_append(obj, member);

        json_skip_space(ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            continue;
        }
        if (ps->p < ps->end && *ps->p == '}') {
            ps->p++;
            return obj;
        }
        err_set(ps->err, ps->errsz, "expected ',' or '}' in object");
        json_free(obj);
        return NULL;
    }
}

static json_value *json_parse_array(json_parser *ps, int depth)
{
    json_value *arr = json_new(JSON_ARRAY);

    ps->p++;   /* '[' */
    json_skip_space(ps);
    if (ps->p < ps->end && *ps->p == ']') {
        ps->p++;
        return arr;
    }

    for (;;) {
        json_value *item = json_parse_value(ps, depth + 1);
        if (!item) {
            json_free(arr);
            return NULL;
        }
        json_append(arr, item);

        json_skip_space(ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            continue;
        }
        if (ps->p < ps->end && *ps->p == ']') {
            ps->p++;
            return arr;
        }
        err_set(ps->err, ps->errsz, "expected ',' or ']' in array");
        json_free(arr);
        return NULL;
    }
}

/* Matches a bare literal such as "true" and consumes it. */
static int json_match_literal(json_parser *ps, const char *literal)
{
    size_t len = strlen(literal);

    if ((size_t)(ps->end - ps->p) < len || memcmp(ps->p, literal, len) != 0)
        return 0;
    ps->p += len;
    return 1;
}

static json_value *json_parse_value(json_parser *ps, int depth)
{
    json_value *v;

    if (depth > JSON_MAX_DEPTH) {
        err_set(ps->err, ps->errsz, "nesting deeper than %d levels", JSON_MAX_DEPTH);
        return NULL;
    }

    json_skip_space(ps);
    if (ps->p >= ps->end) {
        err_set(ps->err, ps->errsz, "unexpected end of input");
        return NULL;
    }

    switch (*ps->p) {
        case '{': return json_parse_object(ps, depth);
        case '[': return json_parse_array(ps, depth);
        case '"': {
            char *s = json_parse_string(ps);
            if (!s)
                return NULL;
            v = json_new(JSON_STRING);
            v->string = s;
            return v;
        }
        default:
            break;
    }

    if (json_match_literal(ps, "true")) {
        v = json_new(JSON_BOOL);
        v->boolean = 1;
        return v;
    }
    if (json_match_literal(ps, "false")) {
        v = json_new(JSON_BOOL);
        v->boolean = 0;
        return v;
    }
    if (json_match_literal(ps, "null"))
        return json_new(JSON_NULL);

    /* Anything left must be a number; strtod validates it for us. */
    {
        char *number_end = NULL;
        double number = strtod(ps->p, &number_end);
        if (number_end && number_end != ps->p) {
            ps->p = number_end;
            v = json_new(JSON_NUMBER);
            v->number = number;
            return v;
        }
    }

    json_fail(ps, "unexpected character");
    return NULL;
}

/* ---- Public API -------------------------------------------------------- */

json_value *json_parse(const char *text, char *err, size_t errsz)
{
    json_parser ps;
    json_value *root;

    if (!text) {
        err_set(err, errsz, "no input");
        return NULL;
    }

    /* Tolerate a UTF-8 byte order mark. */
    if ((unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF)
        text += 3;

    ps.start = text;
    ps.p = text;
    ps.end = text + strlen(text);
    ps.err = err;
    ps.errsz = errsz;

    root = json_parse_value(&ps, 0);
    if (!root)
        return NULL;

    json_skip_space(&ps);
    if (ps.p != ps.end) {
        err_set(err, errsz, "trailing data after the top-level value");
        json_free(root);
        return NULL;
    }
    return root;
}

void json_free(json_value *value)
{
    while (value) {
        json_value *next = value->next;
        json_free(value->children);
        free(value->key);
        free(value->string);
        free(value);
        value = next;
    }
}

const json_value *json_get(const json_value *object, const char *key)
{
    const json_value *child;

    if (!object || object->type != JSON_OBJECT || !key)
        return NULL;
    for (child = object->children; child; child = child->next) {
        if (child->key && strcmp(child->key, key) == 0)
            return child;
    }
    return NULL;
}

const json_value *json_at(const json_value *array, size_t index)
{
    const json_value *child;

    if (!array || array->type != JSON_ARRAY)
        return NULL;
    for (child = array->children; child; child = child->next) {
        if (index-- == 0)
            return child;
    }
    return NULL;
}

size_t json_count(const json_value *value)
{
    if (!value || (value->type != JSON_ARRAY && value->type != JSON_OBJECT))
        return 0;
    return value->count;
}

const char *json_get_string(const json_value *object, const char *key, const char *fallback)
{
    const json_value *v = json_get(object, key);
    return (v && v->type == JSON_STRING) ? v->string : fallback;
}

long json_get_int(const json_value *object, const char *key, long fallback)
{
    const json_value *v = json_get(object, key);
    return (v && v->type == JSON_NUMBER) ? (long)v->number : fallback;
}

int json_get_bool(const json_value *object, const char *key, int fallback)
{
    const json_value *v = json_get(object, key);
    return (v && v->type == JSON_BOOL) ? v->boolean : fallback;
}

/* ---- Building ---------------------------------------------------------- */

json_value *json_new_object(void) { return json_new(JSON_OBJECT); }
json_value *json_new_array(void)  { return json_new(JSON_ARRAY); }
json_value *json_new_null(void)   { return json_new(JSON_NULL); }

json_value *json_new_string(const char *text)
{
    json_value *v = json_new(JSON_STRING);
    v->string = str_dup(text ? text : "");
    return v;
}

json_value *json_new_number(double number)
{
    json_value *v = json_new(JSON_NUMBER);
    v->number = number;
    return v;
}

json_value *json_new_bool(int boolean)
{
    json_value *v = json_new(JSON_BOOL);
    v->boolean = boolean ? 1 : 0;
    return v;
}

void json_object_set(json_value *object, const char *key, json_value *value)
{
    json_value *existing;

    if (!object || object->type != JSON_OBJECT || !value)
        return;

    existing = (json_value *)json_get(object, key);
    if (existing) {
        /* Swap the payload into the node already in place, so the member keeps
         * its position in the object and the file keeps its shape. */
        char *saved_key = existing->key;
        json_value *saved_next = existing->next;

        json_free(existing->children);
        free(existing->string);
        *existing = *value;
        existing->key = saved_key;
        existing->next = saved_next;
        free(value);                 /* the shell only: its payload moved */
        return;
    }

    value->key = str_dup(key);
    json_append(object, value);
}

void json_array_append(json_value *array, json_value *value)
{
    if (!array || array->type != JSON_ARRAY || !value)
        return;
    json_append(array, value);
}

/* ---- Writing ----------------------------------------------------------- */

/* Escapes a string into `out`, including the surrounding quotes. */
static void json_write_string(byte_buf *out, const char *text)
{
    const unsigned char *p = (const unsigned char *)(text ? text : "");

    buf_append(out, "\"", 1);
    for (; *p; p++) {
        switch (*p) {
            case '"':  buf_append(out, "\\\"", 2); break;
            case '\\': buf_append(out, "\\\\", 2); break;
            case '\b': buf_append(out, "\\b", 2);  break;
            case '\f': buf_append(out, "\\f", 2);  break;
            case '\n': buf_append(out, "\\n", 2);  break;
            case '\r': buf_append(out, "\\r", 2);  break;
            case '\t': buf_append(out, "\\t", 2);  break;
            default:
                if (*p < 0x20) {          /* other control characters */
                    char esc[7];
                    snprintf(esc, sizeof esc, "\\u%04x", *p);
                    buf_append(out, esc, 6);
                } else {
                    buf_append(out, p, 1);   /* UTF-8 passes through as is */
                }
                break;
        }
    }
    buf_append(out, "\"", 1);
}

/* Writes a number, keeping whole values free of a decimal point. */
static void json_write_number(byte_buf *out, double number)
{
    char text[40];

    if (number >= -9.0e15 && number <= 9.0e15 && number == (double)(long long)number)
        snprintf(text, sizeof text, "%lld", (long long)number);
    else
        snprintf(text, sizeof text, "%.17g", number);
    buf_append(out, text, strlen(text));
}

static void json_write_indent(byte_buf *out, int pretty, int depth)
{
    int i;

    if (!pretty)
        return;
    buf_append(out, "\n", 1);
    for (i = 0; i < depth; i++)
        buf_append(out, "  ", 2);
}

static void json_write(byte_buf *out, const json_value *value, int pretty, int depth)
{
    const json_value *child;

    switch (value->type) {
        case JSON_NULL:   buf_append(out, "null", 4); break;
        case JSON_BOOL:   if (value->boolean) buf_append(out, "true", 4);
                          else                buf_append(out, "false", 5);
                          break;
        case JSON_NUMBER: json_write_number(out, value->number); break;
        case JSON_STRING: json_write_string(out, value->string); break;

        case JSON_ARRAY:
        case JSON_OBJECT: {
            int is_object = value->type == JSON_OBJECT;

            buf_append(out, is_object ? "{" : "[", 1);
            for (child = value->children; child; child = child->next) {
                json_write_indent(out, pretty, depth + 1);
                if (is_object) {
                    json_write_string(out, child->key);
                    buf_append(out, pretty ? ": " : ":", pretty ? 2 : 1);
                }
                json_write(out, child, pretty, depth + 1);
                if (child->next)
                    buf_append(out, ",", 1);
            }
            if (value->children)
                json_write_indent(out, pretty, depth);
            buf_append(out, is_object ? "}" : "]", 1);
            break;
        }
    }
}

char *json_serialize(const json_value *value, int pretty)
{
    byte_buf out = {0};

    if (!value)
        return str_dup("null");
    json_write(&out, value, pretty, 0);
    if (pretty)
        buf_append(&out, "\n", 1);
    return buf_finish(&out, NULL);
}
