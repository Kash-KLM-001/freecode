#include "catalog.h"
#include "http.h"
#include "json.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *join_url(const char *base, const char *path)
{
    size_t n = strlen(base);
    while (n && base[n - 1] == '/') n--;
    char *out = xmalloc(n + strlen(path) + 2);
    memcpy(out, base, n);
    out[n] = '/';
    strcpy(out + n + 1, path);
    return out;
}

static double price_value(JVal *m, const char *key)
{
    const char *s = json_str(m, key);
    if (!s || !*s) return 0.0;
    return atof(s);
}

static int price_is_free(double d)
{
    return d == 0.0;
}

int catalog_is_free_anonymous(const CatalogModel *m)
{
    if (!m || !m->id) return 0;
    int tagged = m->is_free || str_ends_with(m->id, ":free") ||
                 strcmp(m->id, "kilo-auto/free") == 0 ||
                 strcmp(m->id, "openrouter/free") == 0 ||
                 str_ends_with(m->id, "-free");
    if (!tagged) return 0;
    /* Negative prices are router sentinels for paid auto modes; skip them. */
    if (!price_is_free(m->prompt_cost) || !price_is_free(m->completion_cost) ||
        !price_is_free(m->request_cost)) return 0;
    return 1;
}

long catalog_score(const CatalogModel *m)
{
    if (!m || !m->id) return -1000000;
    long score = 0;
    const char *id = m->id;
    const char *name = m->name ? m->name : "";

    if (strcmp(id, "kilo-auto/free") == 0) score += 900;
    if (str_contains_ci(id, "code") || str_contains_ci(name, "code")) score += 500;
    if (str_contains_ci(id, "coder")) score += 250;

    static const char *const good[] = {
        "glm", "kimi", "qwen", "gemini", "flash", "sonnet",
        "beam", "laguna", "north", "flint", "gpt", "deepseek"
    };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        if (str_contains_ci(id, good[i]) || str_contains_ci(name, good[i])) {
            score += 220;
            break;
        }
    }

    if (m->context >= 200000) score += 300;
    else if (m->context >= 100000) score += 200;
    else if (m->context >= 32000) score += 100;
    if (m->max_out >= 16384) score += 100;
    else if (m->max_out >= 8192) score += 50;
    if (m->may_train) score -= 80;
    if (str_contains_ci(id, "nvidia")) score -= 120;
    return score;
}

static int cmp_score(const void *a, const void *b)
{
    const CatalogModel *x = *(const CatalogModel *const *)a;
    const CatalogModel *y = *(const CatalogModel *const *)b;
    long sx = catalog_score(x), sy = catalog_score(y);
    if (sx != sy) return sy > sx ? 1 : -1;
    return strcmp(x->id ? x->id : "", y->id ? y->id : "");
}

void catalog_sort(Catalog *c)
{
    if (!c || c->n < 2) return;
    CatalogModel **tmp = xmalloc(c->n * sizeof(CatalogModel *));
    for (size_t i = 0; i < c->n; i++) tmp[i] = &c->v[i];
    /* qsort moves pointers, but callers only use catalog_select_ids after sorting.
     * Reorder the backing array from the sorted pointer view. */
    qsort(tmp, c->n, sizeof(CatalogModel *), cmp_score);
    CatalogModel *copy = xmalloc(c->n * sizeof(CatalogModel));
    for (size_t i = 0; i < c->n; i++) copy[i] = *tmp[i];
    free(c->v);
    c->v = copy;
    free(tmp);
}

char **catalog_select_ids(Catalog *c, size_t count, size_t *out_n)
{
    if (out_n) *out_n = 0;
    if (!c || !count) return NULL;
    catalog_sort(c);
    char **ids = xmalloc(count * sizeof(char *));
    size_t n = 0;
    for (size_t i = 0; i < c->n && n < count; i++) {
        if (!catalog_is_free_anonymous(&c->v[i])) continue;
        int dup = 0;
        for (size_t k = 0; k < n; k++) {
            if (strcmp(ids[k], c->v[i].id) == 0) { dup = 1; break; }
        }
        if (!dup) ids[n++] = xstrdup(c->v[i].id);
    }
    if (out_n) *out_n = n;
    if (!n) {
        free(ids);
        return NULL;
    }
    return ids;
}

static void catalog_item_free(CatalogModel *m)
{
    if (!m) return;
    free(m->id);
    free(m->name);
    for (size_t i = 0; i < m->nroutes; i++) free(m->routes[i]);
    free(m->routes);
    memset(m, 0, sizeof(*m));
}

void catalog_free(Catalog *c)
{
    if (!c) return;
    for (size_t i = 0; i < c->n; i++) catalog_item_free(&c->v[i]);
    free(c->v);
    free(c->err);
    memset(c, 0, sizeof(*c));
}

static void parse_model(Catalog *c, JVal *item)
{
    const char *id = json_str(item, "id");
    if (!id || !*id) return;
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 64;
        c->v = xrealloc(c->v, c->cap * sizeof(CatalogModel));
    }
    CatalogModel *m = &c->v[c->n++];
    memset(m, 0, sizeof(*m));
    m->id = xstrdup(id);
    const char *name = json_str(item, "name");
    m->name = xstrdup(name ? name : id);
    m->is_free = json_bool(item, "isFree");
    m->prompt_cost = price_value(item, "pricing.prompt");
    m->completion_cost = price_value(item, "pricing.completion");
    m->request_cost = price_value(item, "pricing.request");
    m->may_train = json_bool(item, "mayTrainOnYourPrompts");
    m->context = (size_t)json_num(item, "top_provider.context_length");
    if (!m->context) m->context = (size_t)json_num(item, "context_length");
    m->max_out = (size_t)json_num(item, "top_provider.max_completion_tokens");
    if (!m->max_out) m->max_out = (size_t)json_num(item, "max_completion_tokens");

    size_t nr = json_arr_len(item, "autoRouting.models");
    for (size_t i = 0; i < nr; i++) {
        JVal *route = json_arr_at(item, "autoRouting.models", i);
        const char *rid = route && route->type == J_STR ? route->u.str.s : NULL;
        if (!rid) continue;
        m->routes = xrealloc(m->routes, (m->nroutes + 1) * sizeof(char *));
        m->routes[m->nroutes++] = xstrdup(rid);
    }
}

int catalog_fetch(Catalog *out, const char *base_url, const char *api_key, int timeout_ms)
{
    if (!out || !base_url) return -1;
    memset(out, 0, sizeof(*out));
    char *url = join_url(base_url, "models");
    HttpReq req;
    memset(&req, 0, sizeof(req));
    req.method = xstrdup("GET");
    req.url = url;
    req.timeout_ms = timeout_ms ? timeout_ms : 8000;
    http_header_add(&req, "Accept", "application/json");
    if (api_key && *api_key) {
        char *auth = xmalloc(strlen(api_key) + 8);
        snprintf(auth, strlen(api_key) + 8, "Bearer %s", api_key);
        http_header_add(&req, "Authorization", auth);
        free(auth);
    }
    HttpResult res;
    int rc = http_request_sync(&req, &res);
    if (rc != 0 || res.status < 200 || res.status >= 300) {
        out->err = res.err ? res.err : xstrdup(res.body ? res.body : "model catalog request failed");
        res.err = NULL;
        http_result_free(&res);
        http_req_free(&req);
        return -1;
    }
    char *jerr = NULL;
    JVal *root = json_parse(res.body, strlen(res.body), &jerr);
    http_result_free(&res);
    http_req_free(&req);
    if (!root) {
        out->err = jerr ? jerr : xstrdup("invalid catalog JSON");
        return -1;
    }
    size_t n = json_arr_len(root, "data");
    for (size_t i = 0; i < n; i++) {
        JVal *item = json_arr_at(root, "data", i);
        if (item) parse_model(out, item);
    }
    json_free(root);
    out->fetched = time(NULL);
    return 0;
}
