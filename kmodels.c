#include "catalog.h"
#include "http.h"
#include "json.h"
#include "provider.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *BASE = "https://ai-gateway.kilo.ai/api/v1";

static void usage(void)
{
    fprintf(stderr,
        "usage: kmodels [options]\n"
        "  --list N       print N best free anonymous model IDs (default: 1)\n"
        "  --json         print selected models as JSON\n"
        "  --catalog      print the full catalog with free/auth flags\n"
        "  --probe        verify the first selection with a tiny chat request\n"
        "  --base-url URL override the models endpoint base URL\n"
        "  --timeout MS   request timeout (default: 8000)\n"
        "  --help         show this help\n");
}

static void print_json_models(Catalog *c, size_t count)
{
    catalog_sort(c);
    printf("[");
    size_t printed = 0;
    for (size_t i = 0; i < c->n && printed < count; i++) {
        if (!catalog_is_free_anonymous(&c->v[i])) continue;
        CatalogModel *m = &c->v[i];
        char *id = json_quote(m->id);
        char *name = json_quote(m->name ? m->name : m->id);
        if (printed) printf(",");
        printf("{\"id\":%s,\"name\":%s,\"score\":%ld,\"context\":%zu,\"may_train\":%s}",
               id, name, catalog_score(m), m->context, m->may_train ? "true" : "false");
        printed++;
        free(id);
        free(name);
    }
    printf("]\n");
}

static int probe_model(Provider *p, const char *model, int timeout_ms, char **err)
{
    Provider probe;
    char *base = xstrdup(p->base_url);
    provider_init(&probe, PROV_KILO, "probe", base, p->api_key, model);
    probe.timeout_ms = timeout_ms ? timeout_ms : 30000;
    free(base);

    ChatMsgs msgs;
    chatmsgs_init(&msgs);
    chatmsgs_add(&msgs, "system", "Reply with the single word OK.");
    chatmsgs_add(&msgs, "user", "ping");

    char *url = provider_chat_url(&probe);
    char *body = provider_chat_body(&probe, &msgs, 0, 256);
    HttpReq req;
    memset(&req, 0, sizeof(req));
    req.method = xstrdup("POST");
    req.url = url;
    req.body = body;
    req.timeout_ms = probe.timeout_ms;
    provider_add_headers(&req, &probe);

    HttpResult res;
    int rc = http_request_sync(&req, &res);
    int ok = rc == 0 && res.status >= 200 && res.status < 300;
    if (ok) {
        ChatParser parser;
        chat_parser_init(&parser);
        if (chat_parse_response(res.body, &parser) == 0 && !parser.error) ok = 1;
        else ok = 0;
        if (!ok && err) *err = parser.error_msg ? xstrdup(parser.error_msg)
                                                : xstrdup("empty model response");
        chat_parser_free(&parser);
    } else if (err) {
        *err = res.err ? xstrdup(res.err)
                       : xstrdup(res.body ? res.body : "probe request failed");
    }
    http_result_free(&res);
    http_req_free(&req);
    chatmsgs_free(&msgs);
    provider_free(&probe);
    return ok ? 0 : -1;
}

int main(int argc, char **argv)
{
    const char *base = BASE;
    int count = 1;
    int json = 0;
    int catalog_dump = 0;
    int probe = 0;
    int timeout = 8000;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--list") == 0 && i + 1 < argc) count = atoi(argv[++i]);
        else if (strcmp(argv[i], "--json") == 0) json = 1;
        else if (strcmp(argv[i], "--catalog") == 0) catalog_dump = 1;
        else if (strcmp(argv[i], "--probe") == 0) probe = 1;
        else if (strcmp(argv[i], "--base-url") == 0 && i + 1 < argc) base = argv[++i];
        else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) timeout = atoi(argv[++i]);
        else if (strcmp(argv[i], "--help") == 0) { usage(); return 0; }
        else {
            fprintf(stderr, "kmodels: unknown option %s\n", argv[i]);
            usage();
            return 2;
        }
    }
    if (count < 1) count = 1;
    if (count > 32) count = 32;

    Catalog c;
    if (catalog_fetch(&c, base, NULL, timeout) != 0) {
        fprintf(stderr, "kmodels: %s\n", c.err ? c.err : "catalog fetch failed");
        catalog_free(&c);
        return 1;
    }

    if (catalog_dump) {
        catalog_sort(&c);
        printf("%-46s %-8s %-10s %7s %s\n", "ID", "FREE", "ANON", "SCORE", "NAME");
        for (size_t i = 0; i < c.n; i++) {
            CatalogModel *m = &c.v[i];
            printf("%-46s %-8s %-10s %7ld %s\n", m->id,
                   m->is_free ? "yes" : "no",
                   catalog_is_free_anonymous(m) ? "yes" : "no",
                   catalog_score(m), m->name ? m->name : "");
        }
        catalog_free(&c);
        return 0;
    }

    size_t n = 0;
    char **ids = catalog_select_ids(&c, (size_t)count, &n);
    if (!ids || !n) {
        fprintf(stderr, "kmodels: no free anonymous models found\n");
        free(ids);
        catalog_free(&c);
        return 1;
    }

    if (json && !probe) {
        print_json_models(&c, n);
    } else {
        if (probe) {
            Provider p;
            provider_init(&p, PROV_KILO, "kilo", base, NULL, ids[0]);
            char *err = NULL;
            if (probe_model(&p, ids[0], 30000, &err) != 0) {
                fprintf(stderr, "kmodels: probe failed for %s: %s\n",
                        ids[0], err ? err : "unknown error");
                free(err);
                provider_free(&p);
                for (size_t i = 0; i < n; i++) free(ids[i]);
                free(ids);
                catalog_free(&c);
                return 1;
            }
            fprintf(stderr, "kmodels: probe OK: %s\n", ids[0]);
            provider_free(&p);
        }
        for (size_t i = 0; i < n; i++) printf("%s\n", ids[i]);
    }

    for (size_t i = 0; i < n; i++) free(ids[i]);
    free(ids);
    catalog_free(&c);
    return 0;
}
