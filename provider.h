#ifndef FREECODE_PROVIDER_H
#define FREECODE_PROVIDER_H

#include <stddef.h>
#include <time.h>

#include "http.h"
#include "provider_kind.h"
#include "util.h"

typedef struct {
    ProviderKind kind;
    char *name;
    char *base_url;
    char *api_key;
    char *model;
    char *chat_path;
    char *models_path;
    int free_only;
    int timeout_ms;
    HttpTransport transport;
} Provider;

typedef struct {
    char *role;
    char *content;
} ChatMsg;

typedef struct {
    ChatMsg *v;
    size_t n;
    size_t cap;
} ChatMsgs;

void provider_init(Provider *p, ProviderKind kind, const char *name,
                   const char *base_url, const char *api_key, const char *model);
void provider_free(Provider *p);
void provider_set_model(Provider *p, const char *model);
char *provider_chat_url(const Provider *p);
char *provider_models_url(const Provider *p);
char *provider_chat_body(const Provider *p, const ChatMsgs *msgs,
                         int stream, size_t max_tokens);
void provider_add_headers(HttpReq *req, const Provider *p);

void chatmsgs_init(ChatMsgs *m);
void chatmsgs_add(ChatMsgs *m, const char *role, const char *content);
void chatmsgs_free(ChatMsgs *m);

typedef struct {
    Buf lines;
    Buf content;
    Buf reasoning;
    char *model;
    size_t prompt_tokens;
    size_t completion_tokens;
    double cost;
    size_t taken;
    int done;
    int error;
    char *error_msg;
} ChatParser;

void chat_parser_init(ChatParser *p);
void chat_parser_free(ChatParser *p);
void chat_parser_feed(ChatParser *p, const char *data, size_t len);
/* Returns newly parsed text since the previous call; caller owns it. */
char *chat_parser_take(ChatParser *p);
const char *chat_parser_text(const ChatParser *p);
int chat_parse_response(const char *body, ChatParser *p);
void chat_parser_mark_done(ChatParser *p);

typedef struct {
    char *model;
    size_t prompt;
    size_t completion;
    size_t requests;
    double cost;
    size_t context;
    size_t last_used;
    time_t last;
} UsageEntry;

typedef struct {
    UsageEntry *v;
    size_t n;
    size_t cap;
    int warn_pct;
    int compact_pct;
    size_t anonymous_requests;
    time_t hour_start;
    time_t last;
} Usage;

void usage_init(Usage *u, int warn_pct, int compact_pct);
void usage_free(Usage *u);
void usage_record(Usage *u, const char *model, size_t prompt, size_t completion,
                  double cost, size_t context, int anonymous);
UsageEntry *usage_get(Usage *u, const char *model);
size_t usage_context_used(const Usage *u, const char *model);
int usage_should_compact(const Usage *u, const char *model, size_t next_prompt_tokens);
int usage_near_anonymous_limit(const Usage *u);

#endif
