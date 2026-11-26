#ifndef FREECODE_CATALOG_H
#define FREECODE_CATALOG_H

#include <stddef.h>
#include <time.h>

typedef struct {
    char *id;
    char *name;
    size_t context;
    size_t max_out;
    int is_free;
    double prompt_cost;
    double completion_cost;
    double request_cost;
    int may_train;
    char **routes;
    size_t nroutes;
} CatalogModel;

typedef struct {
    CatalogModel *v;
    size_t n;
    size_t cap;
    char *err;
    time_t fetched;
} Catalog;

/* Fetch and parse an OpenAI-compatible /models catalog. */
int catalog_fetch(Catalog *out, const char *base_url, const char *api_key, int timeout_ms);
int catalog_is_free_anonymous(const CatalogModel *m);
long catalog_score(const CatalogModel *m);
void catalog_sort(Catalog *c);
/* Returns malloc'd, NUL-terminated array of model IDs. Caller frees each. */
char **catalog_select_ids(Catalog *c, size_t count, size_t *out_n);
void catalog_free(Catalog *c);

#endif
