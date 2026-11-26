#ifndef FREECODE_CONFIG_H
#define FREECODE_CONFIG_H

#include "http.h"
#include "provider_kind.h"

typedef struct {
    char *provider;
    char *base_url;
    char *api_key;
    char *model;
    int free_only;
    int stub;
    int warn_pct;
    int compact_pct;
    int team_size;
    HttpTransport transport;
} Config;

char *config_default_path(void);
void config_init_defaults(Config *c);
int config_load(Config *c, const char *path);
int config_save(const Config *c, const char *path);
void config_free(Config *c);
ProviderKind config_provider_kind(const Config *c);

#endif
