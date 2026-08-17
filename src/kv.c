#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "kv.h"
#include "platform.h"
#include "util.h"

#define KV_MAX_DEPTH 32   /* nesting guard for malformed or hostile input */

/* Token kinds produced by the lexer. */
typedef enum {
    KV_TOK_EOF,
    KV_TOK_OPEN,    /* {           */
    KV_TOK_CLOSE,   /* }           */
    KV_TOK_STR,     /* key or value, unescaped */
    KV_TOK_ERR
} kv_tok;

typedef struct {
    const char *p;      /* cursor */
    const char *end;
    char *err;
    size_t errsz;
} kv_lexer;

/* ---- Lexer ------------------------------------------------------------- */

static void kv_skip_space(kv_lexer *lx)
{
    while (lx->p < lx->end) {
        unsigned char c = (unsigned char)*lx->p;
        if (isspace(c)) {
            lx->p++;
        } else if (c == '/' && lx->p + 1 < lx->end && lx->p[1] == '/') {
            while (lx->p < lx->end && *lx->p != '\n')   /* line comment */
                lx->p++;
        } else {
            break;
        }
    }
}

/* Appends one char to a growing string buffer. */
static void kv_buf_push(char **buf, size_t *len, size_t *cap, char c)
{
    if (*len + 1 >= *cap) {
        *cap = *cap ? *cap * 2 : 32;
        *buf = xrealloc(*buf, *cap);
    }
    (*buf)[(*len)++] = c;
}

/* Reads the next token; for KV_TOK_STR, *out receives an owned string. */
static kv_tok kv_next(kv_lexer *lx, char **out)
{
    char *buf = NULL;
    size_t len = 0, cap = 0;

    *out = NULL;
    kv_skip_space(lx);
    if (lx->p >= lx->end)
        return KV_TOK_EOF;

    if (*lx->p == '{') { lx->p++; return KV_TOK_OPEN; }
    if (*lx->p == '}') { lx->p++; return KV_TOK_CLOSE; }

    if (*lx->p == '"') {
        lx->p++;
        while (lx->p < lx->end && *lx->p != '"') {
            char c = *lx->p++;
            if (c == '\\' && lx->p < lx->end) {   /* escape sequence */
                char e = *lx->p++;
                switch (e) {
                    case 'n':  c = '\n'; break;
                    case 't':  c = '\t'; break;
                    case 'r':  c = '\r'; break;
                    default:   c = e;    break;   /* covers \\ and \" */
                }
            }
            kv_buf_push(&buf, &len, &cap, c);
        }
        if (lx->p >= lx->end) {
            err_set(lx->err, lx->errsz, "unterminated quoted string");
            free(buf);
            return KV_TOK_ERR;
        }
        lx->p++;   /* closing quote */
    } else {
        /* Bare token: runs until whitespace or a structural character. */
        while (lx->p < lx->end && !isspace((unsigned char)*lx->p) &&
               *lx->p != '{' && *lx->p != '}' && *lx->p != '"')
            kv_buf_push(&buf, &len, &cap, *lx->p++);
    }

    kv_buf_push(&buf, &len, &cap, '\0');
    *out = buf;
    return KV_TOK_STR;
}

/*
 * Consumes an optional platform conditional such as [$WIN32] that may trail a
 * value. Leaves the cursor untouched when the next token is not one.
 */
static void kv_skip_conditional(kv_lexer *lx)
{
    const char *save = lx->p;
    char *tok = NULL;

    if (kv_next(lx, &tok) == KV_TOK_STR && tok && tok[0] == '[') {
        free(tok);
        return;
    }
    free(tok);
    lx->p = save;
}

/* ---- Tree building ----------------------------------------------------- */

static kv_node *kv_node_new(char *key)
{
    kv_node *n = xmalloc(sizeof(*n));
    n->key = key;
    n->value = NULL;
    n->children = n->last_child = n->next = NULL;
    return n;
}

static void kv_append(kv_node *parent, kv_node *child)
{
    if (parent->last_child)
        parent->last_child->next = child;
    else
        parent->children = child;
    parent->last_child = child;
}

/* Parses key/value pairs into `parent` until '}' (or EOF at depth 0). */
static int kv_parse_into(kv_lexer *lx, kv_node *parent, int depth)
{
    if (depth > KV_MAX_DEPTH) {
        err_set(lx->err, lx->errsz, "nesting deeper than %d levels", KV_MAX_DEPTH);
        return -1;
    }

    for (;;) {
        char *key = NULL, *val = NULL;
        kv_node *node;
        kv_tok t = kv_next(lx, &key);

        if (t == KV_TOK_EOF) {
            if (depth == 0)
                return 0;
            err_set(lx->err, lx->errsz, "unexpected end of file inside a block");
            return -1;
        }
        if (t == KV_TOK_CLOSE) {
            if (depth > 0)
                return 0;
            err_set(lx->err, lx->errsz, "unexpected '}' at top level");
            return -1;
        }
        if (t != KV_TOK_STR) {
            if (t == KV_TOK_OPEN)
                err_set(lx->err, lx->errsz, "unexpected '{' where a key was expected");
            return -1;
        }

        /* Preprocessor directives (#base, #include): skip them and their argument. */
        if (key[0] == '#') {
            free(key);
            if (kv_next(lx, &val) != KV_TOK_STR) {
                free(val);
                err_set(lx->err, lx->errsz, "malformed preprocessor directive");
                return -1;
            }
            free(val);
            continue;
        }

        node = kv_node_new(key);   /* takes ownership of key */
        kv_append(parent, node);

        t = kv_next(lx, &val);
        if (t == KV_TOK_OPEN) {
            if (kv_parse_into(lx, node, depth + 1) != 0)
                return -1;
        } else if (t == KV_TOK_STR) {
            node->value = val;     /* takes ownership of val */
            kv_skip_conditional(lx);
        } else {
            free(val);
            err_set(lx->err, lx->errsz, "key '%s' has no value", node->key);
            return -1;
        }
    }
}

/* ---- Public API -------------------------------------------------------- */

kv_node *kv_parse_string(const char *text, char *err, size_t errsz)
{
    kv_lexer lx;
    kv_node *root;

    if (!text) {
        err_set(err, errsz, "no input");
        return NULL;
    }

    /* Skip a UTF-8 byte order mark if present. */
    if ((unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF)
        text += 3;

    lx.p = text;
    lx.end = text + strlen(text);
    lx.err = err;
    lx.errsz = errsz;

    root = kv_node_new(NULL);
    if (kv_parse_into(&lx, root, 0) != 0) {
        kv_free(root);
        return NULL;
    }
    return root;
}

kv_node *kv_parse_file(const char *path, char *err, size_t errsz)
{
    char *text = plat_read_file(path, NULL);
    kv_node *root;

    if (!text) {
        err_set(err, errsz, "cannot read '%s'", path);
        return NULL;
    }
    root = kv_parse_string(text, err, errsz);
    free(text);
    return root;
}

void kv_free(kv_node *node)
{
    while (node) {
        kv_node *next = node->next;
        kv_free(node->children);
        free(node->key);
        free(node->value);
        free(node);
        node = next;
    }
}

const kv_node *kv_child(const kv_node *node, const char *key)
{
    const kv_node *c;

    if (!node)
        return NULL;
    for (c = node->children; c; c = c->next) {
        if (str_ieq(c->key, key))
            return c;
    }
    return NULL;
}

const char *kv_value(const kv_node *node, const char *key)
{
    const kv_node *c = kv_child(node, key);
    return c ? c->value : NULL;
}
