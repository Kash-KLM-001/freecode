#ifndef FREECODE_JSON_H
#define FREECODE_JSON_H

#include <stddef.h>

typedef enum {
    J_NULL = 0,
    J_BOOL,
    J_NUM,
    J_STR,
    J_ARR,
    J_OBJ
} JType;

typedef struct JVal JVal;

struct JVal {
    JType type;
    union {
        int b;
        double num;
        struct { char *s; size_t len; } str;
        struct { JVal **v; size_t n; } arr;
        struct { char **keys; JVal **vals; size_t n; } obj;
    } u;
};

JVal *json_parse(const char *s, size_t len, char **err);
void json_free(JVal *v);

/* Dot-separated path. Array indexes are decimal path components:
 * "data.0.id". Returns NULL when a component is missing. */
JVal *json_get(JVal *v, const char *path);
const char *json_str(JVal *v, const char *path);
double json_num(JVal *v, const char *path);
int json_bool(JVal *v, const char *path);
int json_is_null(JVal *v, const char *path);
size_t json_arr_len(JVal *v, const char *path);
JVal *json_arr_at(JVal *v, const char *path, size_t idx);

#endif
