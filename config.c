#include "config.h"
#include "provider.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define KILO_BASE "https://ai-gateway.kilo.ai/api/v1"

char *config_default_path(void)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        size_t n = strlen(xdg) + strlen("/freecode/config") + 1;
        char *p = xmalloc(n);
        snprintf(p, n, "%s/freecode/config", xdg);
        return p;
    }
    const char *home = getenv("HOME");
    if (!home || !*home) home = ".";
    size_t n = strlen(home) + strlen("/.config/freecode/config") + 1;
    char *p = xmalloc(n);
    snprintf(p, n, "%s/.config/freecode/config", home);
    return p;
}

void config_init_defaults(Config *c)
{
    memset(c, 0, sizeof(*c));
    c->provider = xstrdup("kilo");
    c->base_url = xstrdup(KILO_BASE);
    c->api_key = NULL;
    c->model = xstrdup("kilo-auto/free");
    c->free_only = 1;
    c->stub = 0;
    c->warn_pct = 70;
    c->compact_pct = 88;
    c->team_size = 3;
    c->transport = HTTP_AUTO;
}

static void set_value(Config *c, const char *key, const char *value)
{
    if (strcmp(key, "provider") == 0 || strcmp(key, "api") == 0) {
        free(c->provider);
        c->provider = xstrdup(value);
    } else if (strcmp(key, "base_url") == 0 || strcmp(key, "route") == 0) {
        free(c->base_url);
        c->base_url = xstrdup(value);
    } else if (strcmp(key, "api_key") == 0 || strcmp(key, "key") == 0) {
        free(c->api_key);
        c->api_key = value && *value ? xstrdup(value) : NULL;
    } else if (strcmp(key, "model") == 0) {
        free(c->model);
        c->model = xstrdup(value);
    } else if (strcmp(key, "free_only") == 0) {
        c->free_only = atoi(value);
    } else if (strcmp(key, "stub") == 0) {
        c->stub = atoi(value);
    } else if (strcmp(key, "warn_pct") == 0) {
        c->warn_pct = atoi(value);
    } else if (strcmp(key, "compact_pct") == 0) {
        c->compact_pct = atoi(value);
    } else if (strcmp(key, "team_size") == 0) {
        c->team_size = atoi(value);
    } else if (strcmp(key, "transport") == 0) {
        if (strcmp(value, "curl") == 0) c->transport = HTTP_CURL;
        else if (strcmp(value, "direct") == 0) c->transport = HTTP_DIRECT;
        else if (strcmp(value, "libcurl") == 0) c->transport = HTTP_LIBCURL;
        else c->transport = HTTP_AUTO;
    }
}

static void apply_env(Config *c)
{
    const char *env[][2] = {
        {"FREECODE_PROVIDER", "provider"},
        {"FREECODE_BASE_URL", "base_url"},
        {"FREECODE_API_KEY", "api_key"},
        {"KILO_API_KEY", "api_key"},
        {"OPENAI_API_KEY", "api_key"},
        {"ANTHROPIC_API_KEY", "api_key"},
        {"FREECODE_MODEL", "model"},
        {"FREECODE_FREE_ONLY", "free_only"},
        {"FREECODE_STUB", "stub"},
        {"FREECODE_WARN_PCT", "warn_pct"},
        {"FREECODE_COMPACT_PCT", "compact_pct"},
        {"FREECODE_TEAM_SIZE", "team_size"},
        {"FREECODE_TRANSPORT", "transport"},
        {NULL, NULL}
    };
    for (int i = 0; env[i][0]; i++) {
        const char *v = getenv(env[i][0]);
        if (v && *v) set_value(c, env[i][1], v);
    }
}

int config_load(Config *c, const char *path)
{
    config_init_defaults(c);
    FILE *f = path ? fopen(path, "r") : NULL;
    if (f) {
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            char *cr = strchr(line, '\r');
            if (cr) *cr = '\0';
            char *s = str_trim(line);
            if (!*s || *s == '#' || *s == ';') continue;
            char *eq = strchr(s, '=');
            if (!eq) continue;
            *eq = '\0';
            char *key = str_trim(s);
            char *value = str_trim(eq + 1);
            set_value(c, key, value);
        }
        fclose(f);
    }
    apply_env(c);
    if (c->team_size < 1) c->team_size = 1;
    if (c->team_size > 8) c->team_size = 8;
    if (c->warn_pct < 1 || c->warn_pct >= 100) c->warn_pct = 70;
    if (c->compact_pct <= c->warn_pct || c->compact_pct > 100) c->compact_pct = 88;
    return 0;
}

int config_save(const Config *c, const char *path)
{
    if (!c || !path) return -1;
    if (fs_mkdir_parents(path) != 0) return -1;
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    chmod(path, 0600);
    fprintf(f, "# freecode config. API keys are stored locally; keep this file private.\n");
    fprintf(f, "provider=%s\n", c->provider ? c->provider : "kilo");
    fprintf(f, "base_url=%s\n", c->base_url ? c->base_url : "");
    fprintf(f, "api_key=%s\n", c->api_key ? c->api_key : "");
    fprintf(f, "model=%s\n", c->model ? c->model : "kilo-auto/free");
    fprintf(f, "free_only=%d\n", c->free_only);
    fprintf(f, "stub=%d\n", c->stub);
    fprintf(f, "warn_pct=%d\n", c->warn_pct);
    fprintf(f, "compact_pct=%d\n", c->compact_pct);
    fprintf(f, "team_size=%d\n", c->team_size);
    const char *transport = "auto";
    if (c->transport == HTTP_CURL) transport = "curl";
    else if (c->transport == HTTP_DIRECT) transport = "direct";
    else if (c->transport == HTTP_LIBCURL) transport = "libcurl";
    fprintf(f, "transport=%s\n", transport);
    fclose(f);
    return 0;
}

ProviderKind config_provider_kind(const Config *c)
{
    if (!c || !c->provider) return PROV_KILO;
    if (strcmp(c->provider, "local") == 0 || strcmp(c->provider, "ollama") == 0) return PROV_LOCAL;
    if (strcmp(c->provider, "openai") == 0) return PROV_OPENAI;
    if (strcmp(c->provider, "anthropic") == 0) return PROV_ANTHROPIC;
    if (strcmp(c->provider, "custom") == 0) return PROV_CUSTOM;
    return PROV_KILO;
}

void config_free(Config *c)
{
    if (!c) return;
    free(c->provider);
    free(c->base_url);
    free(c->api_key);
    free(c->model);
    memset(c, 0, sizeof(*c));
}
