#ifndef FREECODE_UTIL_H
#define FREECODE_UTIL_H

#include <stddef.h>
#include <stdio.h>

void fc_die(const char *msg);
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup_n(const char *s, size_t n);

typedef struct {
    char *p;
    size_t len;
    size_t cap;
} Buf;

void buf_reserve(Buf *b, size_t extra);
void buf_add(Buf *b, const char *s, size_t n);
void buf_puts(Buf *b, const char *s);
void buf_putc(Buf *b, char c);
void buf_fmt(Buf *b, const char *fmt, ...);
void buf_free(Buf *b);
char *buf_take(Buf *b);

char *str_trim(char *s);
int str_ends_with(const char *s, const char *suffix);
int str_contains_ci(const char *s, const char *needle);
char *json_quote(const char *s);

size_t estimate_tokens(const char *s, size_t len);
size_t estimate_tokens_str(const char *s);

#endif
