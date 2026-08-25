/*
 * kv.h - Parser for Valve's KeyValues format, used by Steam's .vdf and .acf
 * files (https://developer.valvesoftware.com/wiki/KeyValues).
 *
 * The format is a recursive list of key/value pairs where a value is either a
 * string or a nested block. Keys and values may be quoted or bare, lookups are
 * case-insensitive, "//" starts a line comment, and quoted strings support the
 * usual backslash escapes (notably "\\" for path separators on Windows).
 */
#ifndef TABBER_KV_H
#define TABBER_KV_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kv_node kv_node;
struct kv_node {
    char *key;            /* NULL only for the synthetic root node */
    char *value;          /* NULL when this node is a block (see children) */
    kv_node *children;    /* first child, NULL for plain values */
    kv_node *last_child;  /* tail pointer, used while building the tree */
    kv_node *next;        /* next sibling */
};

/*
 * Parse a document into a synthetic root node whose children are the
 * top-level keys. Returns NULL on failure, writing a reason into `err`.
 * The returned tree must be released with kv_free().
 */
kv_node *kv_parse_string(const char *text, char *err, size_t errsz);
kv_node *kv_parse_file(const char *path, char *err, size_t errsz);

void kv_free(kv_node *node);

/* Direct child of `node` whose key matches `key` (case-insensitive), or NULL. */
const kv_node *kv_child(const kv_node *node, const char *key);

/* String value of such a child, or NULL if missing or if it is a block. */
const char *kv_value(const kv_node *node, const char *key);

#ifdef __cplusplus
}
#endif

#endif /* TABBER_KV_H */
