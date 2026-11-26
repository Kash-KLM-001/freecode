#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void fc_die(const char *msg)
{
    fprintf(stderr, "freecode: %s: %s\n", msg, strerror(errno));
    exit(1);
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) fc_die("malloc");
    return p;
}

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) fc_die("realloc");
    return q;
}

char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

char *xstrndup_n(const char *s, size_t n)
{
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

void buf_reserve(Buf *b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap) return;
    size_t want = b->cap ? b->cap : 256;
    while (want < b->len + extra + 1) want *= 2;
    b->p = xrealloc(b->p, want);
    b->cap = want;
}

void buf_add(Buf *b, const char *s, size_t n)
{
    buf_reserve(b, n);
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

void buf_puts(Buf *b, const char *s)
{
    buf_add(b, s, strlen(s));
}

void buf_putc(Buf *b, char c)
{
    buf_add(b, &c, 1);
}

void buf_fmt(Buf *b, const char *fmt, ...)
{
    va_list ap, cp;
    va_start(ap, fmt);
    va_copy(cp, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(cp);
        return;
    }
    buf_reserve(b, (size_t)n);
    vsnprintf(b->p + b->len, b->cap - b->len, fmt, cp);
    va_end(cp);
    b->len += (size_t)n;
}

void buf_free(Buf *b)
{
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

char *buf_take(Buf *b)
{
    char *p = b->p ? b->p : xstrdup("");
    b->p = NULL;
    b->len = b->cap = 0;
    return p;
}

int fs_mkdir_parents(const char *path)
{
    char *copy = xstrdup(path ? path : "");
    char *slash = strrchr(copy, '/');
    if (!slash) {
        free(copy);
        return 0;
    }
    *slash = '\0';
    char *dir = copy;
    for (char *p = dir + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
                free(copy);
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        free(copy);
        return -1;
    }
    free(copy);
    return 0;
}

char *str_trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) e--;
    *e = '\0';
    return s;
}

int str_ends_with(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    if (b > a) return 0;
    return strcmp(s + a - b, suffix) == 0;
}

int str_contains_ci(const char *s, const char *needle)
{
    size_t n = strlen(needle);
    if (!n) return 1;
    for (const char *p = s; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return 1;
    }
    return 0;
}

char *json_quote(const char *s)
{
    Buf b = {0};
    buf_putc(&b, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  buf_puts(&b, "\\\""); break;
        case '\\': buf_puts(&b, "\\\\"); break;
        case '\b': buf_puts(&b, "\\b"); break;
        case '\f': buf_puts(&b, "\\f"); break;
        case '\n': buf_puts(&b, "\\n"); break;
        case '\r': buf_puts(&b, "\\r"); break;
        case '\t': buf_puts(&b, "\\t"); break;
        default:
            if (*p < 0x20) buf_fmt(&b, "\\u%04x", *p);
            else buf_putc(&b, (char)*p);
            break;
        }
    }
    buf_putc(&b, '"');
    return buf_take(&b);
}

size_t estimate_tokens(const char *s, size_t len)
{
    /* Fast, provider-agnostic approximation. Roughly 4 bytes per token for
     * English/code and 1-3 bytes per token for many UTF-8 scripts. */
    size_t non_ascii = 0;
    for (size_t i = 0; i < len; i++) {
        if (((unsigned char)s[i]) >= 0x80) non_ascii++;
    }
    size_t ascii = len - non_ascii;
    return (ascii + 3) / 4 + (non_ascii + 2) / 3;
}

size_t estimate_tokens_str(const char *s)
{
    return estimate_tokens(s, strlen(s));
}
