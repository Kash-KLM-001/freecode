#include "json.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *p;
    size_t len;
    size_t pos;
    char *err;
} Parser;

static void set_err(Parser *p, const char *msg)
{
    if (!p->err) p->err = xstrdup(msg);
}

static void skip_ws(Parser *p)
{
    while (p->pos < p->len && isspace((unsigned char)p->p[p->pos])) p->pos++;
}

static JVal *parse_value(Parser *p);

static void append_utf8(Buf *b, uint32_t cp)
{
    if (cp < 0x80) {
        char c = (char)cp;
        buf_add(b, &c, 1);
    } else if (cp < 0x800) {
        char c[2] = {(char)(0xC0 | (cp >> 6)), (char)(0x80 | (cp & 0x3F))};
        buf_add(b, c, 2);
    } else if (cp < 0x10000) {
        char c[3] = {(char)(0xE0 | (cp >> 12)),
                     (char)(0x80 | ((cp >> 6) & 0x3F)),
                     (char)(0x80 | (cp & 0x3F))};
        buf_add(b, c, 3);
    } else {
        char c[4] = {(char)(0xF0 | (cp >> 18)),
                     (char)(0x80 | ((cp >> 12) & 0x3F)),
                     (char)(0x80 | ((cp >> 6) & 0x3F)),
                     (char)(0x80 | (cp & 0x3F))};
        buf_add(b, c, 4);
    }
}

static int hex4(Parser *p, uint32_t *out)
{
    if (p->pos + 4 > p->len) return 0;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p->p[p->pos++];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

static JVal *parse_string(Parser *p)
{
    if (p->pos >= p->len || p->p[p->pos] != '"') {
        set_err(p, "expected string");
        return NULL;
    }
    p->pos++;
    Buf b = {0};
    while (p->pos < p->len) {
        unsigned char c = (unsigned char)p->p[p->pos++];
        if (c == '"') {
            JVal *v = xmalloc(sizeof(*v));
            memset(v, 0, sizeof(*v));
            v->type = J_STR;
            v->u.str.s = buf_take(&b);
            v->u.str.len = strlen(v->u.str.s);
            return v;
        }
        if (c != '\\') {
            buf_putc(&b, (char)c);
            continue;
        }
        if (p->pos >= p->len) break;
        char e = p->p[p->pos++];
        switch (e) {
        case '"': buf_putc(&b, '"'); break;
        case '\\': buf_putc(&b, '\\'); break;
        case '/': buf_putc(&b, '/'); break;
        case 'b': buf_putc(&b, '\b'); break;
        case 'f': buf_putc(&b, '\f'); break;
        case 'n': buf_putc(&b, '\n'); break;
        case 'r': buf_putc(&b, '\r'); break;
        case 't': buf_putc(&b, '\t'); break;
        case 'u': {
            uint32_t cp;
            if (!hex4(p, &cp)) {
                set_err(p, "bad unicode escape");
                goto fail;
            }
            if (cp >= 0xD800 && cp <= 0xDBFF && p->pos + 1 < p->len &&
                p->p[p->pos] == '\\' && p->p[p->pos + 1] == 'u') {
                p->pos += 2;
                uint32_t lo;
                if (hex4(p, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else {
                    cp = 0xFFFD;
                }
            }
            append_utf8(&b, cp);
            break;
        }
        default:
            set_err(p, "bad string escape");
            goto fail;
        }
    }
    set_err(p, "unterminated string");
fail:
    buf_free(&b);
    return NULL;
}

static JVal *parse_number(Parser *p)
{
    size_t start = p->pos;
    if (p->pos < p->len && (p->p[p->pos] == '-' || p->p[p->pos] == '+')) p->pos++;
    while (p->pos < p->len &&
           (isdigit((unsigned char)p->p[p->pos]) || p->p[p->pos] == '.' ||
            p->p[p->pos] == 'e' || p->p[p->pos] == 'E' ||
            p->p[p->pos] == '-' || p->p[p->pos] == '+')) p->pos++;
    if (p->pos == start) {
        set_err(p, "expected number");
        return NULL;
    }
    char *tmp = xstrndup_n(p->p + start, p->pos - start);
    char *end = NULL;
    double d = strtod(tmp, &end);
    free(tmp);
    if (end && *end) {
        set_err(p, "bad number");
        return NULL;
    }
    JVal *v = xmalloc(sizeof(*v));
    memset(v, 0, sizeof(*v));
    v->type = J_NUM;
    v->u.num = d;
    return v;
}

static JVal *parse_array(Parser *p)
{
    p->pos++; /* [ */
    JVal *v = xmalloc(sizeof(*v));
    memset(v, 0, sizeof(*v));
    v->type = J_ARR;
    skip_ws(p);
    if (p->pos < p->len && p->p[p->pos] == ']') {
        p->pos++;
        return v;
    }
    for (;;) {
        skip_ws(p);
        JVal *item = parse_value(p);
        if (!item) {
            json_free(v);
            return NULL;
        }
        v->u.arr.v = xrealloc(v->u.arr.v, (v->u.arr.n + 1) * sizeof(JVal *));
        v->u.arr.v[v->u.arr.n++] = item;
        skip_ws(p);
        if (p->pos >= p->len) {
            set_err(p, "unterminated array");
            json_free(v);
            return NULL;
        }
        if (p->p[p->pos] == ']') {
            p->pos++;
            return v;
        }
        if (p->p[p->pos] != ',') {
            set_err(p, "expected comma in array");
            json_free(v);
            return NULL;
        }
        p->pos++;
    }
}

static JVal *parse_object(Parser *p)
{
    p->pos++; /* { */
    JVal *v = xmalloc(sizeof(*v));
    memset(v, 0, sizeof(*v));
    v->type = J_OBJ;
    skip_ws(p);
    if (p->pos < p->len && p->p[p->pos] == '}') {
        p->pos++;
        return v;
    }
    for (;;) {
        skip_ws(p);
        JVal *key = parse_string(p);
        if (!key) {
            json_free(v);
            return NULL;
        }
        skip_ws(p);
        if (p->pos >= p->len || p->p[p->pos] != ':') {
            set_err(p, "expected colon");
            json_free(key);
            json_free(v);
            return NULL;
        }
        p->pos++;
        skip_ws(p);
        JVal *item = parse_value(p);
        if (!item) {
            json_free(key);
            json_free(v);
            return NULL;
        }
        v->u.obj.keys = xrealloc(v->u.obj.keys, (v->u.obj.n + 1) * sizeof(char *));
        v->u.obj.vals = xrealloc(v->u.obj.vals, (v->u.obj.n + 1) * sizeof(JVal *));
        v->u.obj.keys[v->u.obj.n] = key->u.str.s;
        v->u.obj.vals[v->u.obj.n] = item;
        v->u.obj.n++;
        key->u.str.s = NULL;
        json_free(key);
        skip_ws(p);
        if (p->pos >= p->len) {
            set_err(p, "unterminated object");
            json_free(v);
            return NULL;
        }
        if (p->p[p->pos] == '}') {
            p->pos++;
            return v;
        }
        if (p->p[p->pos] != ',') {
            set_err(p, "expected comma in object");
            json_free(v);
            return NULL;
        }
        p->pos++;
    }
}

static JVal *parse_value(Parser *p)
{
    skip_ws(p);
    if (p->pos >= p->len) {
        set_err(p, "unexpected end of JSON");
        return NULL;
    }
    char c = p->p[p->pos];
    if (c == '"') return parse_string(p);
    if (c == '{') return parse_object(p);
    if (c == '[') return parse_array(p);
    if (c == 't' && p->pos + 4 <= p->len && memcmp(p->p + p->pos, "true", 4) == 0) {
        p->pos += 4;
        JVal *v = xmalloc(sizeof(*v));
        memset(v, 0, sizeof(*v));
        v->type = J_BOOL;
        v->u.b = 1;
        return v;
    }
    if (c == 'f' && p->pos + 5 <= p->len && memcmp(p->p + p->pos, "false", 5) == 0) {
        p->pos += 5;
        JVal *v = xmalloc(sizeof(*v));
        memset(v, 0, sizeof(*v));
        v->type = J_BOOL;
        v->u.b = 0;
        return v;
    }
    if (c == 'n' && p->pos + 4 <= p->len && memcmp(p->p + p->pos, "null", 4) == 0) {
        p->pos += 4;
        JVal *v = xmalloc(sizeof(*v));
        memset(v, 0, sizeof(*v));
        v->type = J_NULL;
        return v;
    }
    if (c == '-' || isdigit((unsigned char)c)) return parse_number(p);
    set_err(p, "invalid JSON value");
    return NULL;
}

JVal *json_parse(const char *s, size_t len, char **err)
{
    Parser p = { s, len, 0, NULL };
    JVal *v = parse_value(&p);
    skip_ws(&p);
    if (v && p.pos < p.len) {
        set_err(&p, "trailing data after JSON");
        json_free(v);
        v = NULL;
    }
    if (err) *err = p.err;
    else free(p.err);
    return v;
}

void json_free(JVal *v)
{
    if (!v) return;
    switch (v->type) {
    case J_STR:
        free(v->u.str.s);
        break;
    case J_ARR:
        for (size_t i = 0; i < v->u.arr.n; i++) json_free(v->u.arr.v[i]);
        free(v->u.arr.v);
        break;
    case J_OBJ:
        for (size_t i = 0; i < v->u.obj.n; i++) {
            free(v->u.obj.keys[i]);
            json_free(v->u.obj.vals[i]);
        }
        free(v->u.obj.keys);
        free(v->u.obj.vals);
        break;
    default:
        break;
    }
    free(v);
}

static JVal *child_by_key(JVal *v, const char *key, size_t keylen)
{
    if (!v || v->type != J_OBJ) return NULL;
    for (size_t i = 0; i < v->u.obj.n; i++) {
        if (strlen(v->u.obj.keys[i]) == keylen &&
            memcmp(v->u.obj.keys[i], key, keylen) == 0) return v->u.obj.vals[i];
    }
    return NULL;
}

JVal *json_get(JVal *v, const char *path)
{
    if (!v || !path || !*path) return v;
    JVal *cur = v;
    const char *p = path;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t n = dot ? (size_t)(dot - p) : strlen(p);
        if (!n) return NULL;
        if (cur->type == J_OBJ) {
            cur = child_by_key(cur, p, n);
        } else if (cur->type == J_ARR) {
            char *tmp = xstrndup_n(p, n);
            char *end = NULL;
            long idx = strtol(tmp, &end, 10);
            free(tmp);
            if (end && *end) return NULL;
            if (idx < 0 || (size_t)idx >= cur->u.arr.n) return NULL;
            cur = cur->u.arr.v[idx];
        } else {
            return NULL;
        }
        if (!cur) return NULL;
        if (!dot) break;
        p = dot + 1;
    }
    return cur;
}

const char *json_str(JVal *v, const char *path)
{
    JVal *x = json_get(v, path);
    return (x && x->type == J_STR) ? x->u.str.s : NULL;
}

double json_num(JVal *v, const char *path)
{
    JVal *x = json_get(v, path);
    return (x && x->type == J_NUM) ? x->u.num : 0.0;
}

int json_bool(JVal *v, const char *path)
{
    JVal *x = json_get(v, path);
    return (x && x->type == J_BOOL) ? x->u.b : 0;
}

int json_is_null(JVal *v, const char *path)
{
    JVal *x = json_get(v, path);
    return !x || x->type == J_NULL;
}

size_t json_arr_len(JVal *v, const char *path)
{
    JVal *x = json_get(v, path);
    return (x && x->type == J_ARR) ? x->u.arr.n : 0;
}

JVal *json_arr_at(JVal *v, const char *path, size_t idx)
{
    JVal *x = json_get(v, path);
    if (!x || x->type != J_ARR || idx >= x->u.arr.n) return NULL;
    return x->u.arr.v[idx];
}
