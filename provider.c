#include "provider.h"
#include "json.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_TIMEOUT_MS 45000

static char *join_base(const char *base, const char *path)
{
    if (!base) return xstrdup("");
    size_t n = strlen(base);
    while (n && base[n - 1] == '/') n--;
    const char *p = path ? path : "";
    while (*p == '/') p++;
    /* Allow a full route override in the "path" field. */
    if (strncmp(p, "http://", 7) == 0 || strncmp(p, "https://", 8) == 0)
        return xstrdup(p);
    char *out = xmalloc(n + strlen(p) + 2);
    memcpy(out, base, n);
    out[n] = '/';
    strcpy(out + n + 1, p);
    return out;
}

void provider_init(Provider *p, ProviderKind kind, const char *name,
                   const char *base_url, const char *api_key, const char *model)
{
    memset(p, 0, sizeof(*p));
    p->kind = kind;
    p->name = xstrdup(name ? name : "custom");
    p->base_url = xstrdup(base_url ? base_url : "");
    p->api_key = api_key && *api_key ? xstrdup(api_key) : NULL;
    p->model = xstrdup(model ? model : "kilo-auto/free");
    p->timeout_ms = DEFAULT_TIMEOUT_MS;
    p->free_only = kind == PROV_KILO;

    switch (kind) {
    case PROV_KILO:
        p->chat_path = xstrdup("chat/completions");
        p->models_path = xstrdup("models");
        break;
    case PROV_LOCAL:
        p->chat_path = xstrdup("v1/chat/completions");
        p->models_path = xstrdup("v1/models");
        break;
    case PROV_OPENAI:
        p->chat_path = xstrdup("chat/completions");
        p->models_path = xstrdup("models");
        break;
    case PROV_ANTHROPIC:
        p->chat_path = xstrdup("messages");
        p->models_path = xstrdup("models");
        break;
    default:
        p->chat_path = xstrdup("chat/completions");
        p->models_path = xstrdup("models");
        break;
    }
}

void provider_free(Provider *p)
{
    if (!p) return;
    free(p->name);
    free(p->base_url);
    free(p->api_key);
    free(p->model);
    free(p->chat_path);
    free(p->models_path);
    memset(p, 0, sizeof(*p));
}

void provider_set_model(Provider *p, const char *model)
{
    if (!p || !model) return;
    free(p->model);
    p->model = xstrdup(model);
}

char *provider_chat_url(const Provider *p)
{
    return p ? join_base(p->base_url, p->chat_path) : NULL;
}

char *provider_models_url(const Provider *p)
{
    return p ? join_base(p->base_url, p->models_path) : NULL;
}

void provider_add_headers(HttpReq *req, const Provider *p)
{
    if (!req || !p) return;
    http_header_add(req, "Content-Type", "application/json");
    http_header_add(req, "Accept", "application/json, text/event-stream");
    if (p->api_key && *p->api_key) {
        if (p->kind == PROV_ANTHROPIC) {
            char *v = xmalloc(strlen(p->api_key) + 16);
            snprintf(v, strlen(p->api_key) + 16, "%s", p->api_key);
            http_header_add(req, "x-api-key", v);
            free(v);
            http_header_add(req, "anthropic-version", "2023-06-01");
        } else {
            char *v = xmalloc(strlen(p->api_key) + 8);
            snprintf(v, strlen(p->api_key) + 8, "Bearer %s", p->api_key);
            http_header_add(req, "Authorization", v);
            free(v);
        }
    }
    if (p->kind == PROV_KILO)
        http_header_add(req, "X-KiloCode-Version", "freecode/1");
}

void chatmsgs_init(ChatMsgs *m)
{
    memset(m, 0, sizeof(*m));
}

void chatmsgs_add(ChatMsgs *m, const char *role, const char *content)
{
    if (!m || !role || !content) return;
    if (m->n == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 16;
        m->v = xrealloc(m->v, m->cap * sizeof(ChatMsg));
    }
    m->v[m->n].role = xstrdup(role);
    m->v[m->n].content = xstrdup(content);
    m->n++;
}

void chatmsgs_free(ChatMsgs *m)
{
    if (!m) return;
    for (size_t i = 0; i < m->n; i++) {
        free(m->v[i].role);
        free(m->v[i].content);
    }
    free(m->v);
    memset(m, 0, sizeof(*m));
}

char *provider_chat_body(const Provider *p, const ChatMsgs *msgs,
                         int stream, size_t max_tokens)
{
    if (!p || !msgs) return NULL;
    Buf b = {0};
    char *model = json_quote(p->model ? p->model : "");
    if (!max_tokens) max_tokens = 2048;

    if (p->kind == PROV_ANTHROPIC) {
        buf_fmt(&b, "{\"model\":%s,\"max_tokens\":%zu,\"stream\":%s",
                model, max_tokens, stream ? "true" : "false");
        for (size_t i = 0; i < msgs->n; i++) {
            if (strcmp(msgs->v[i].role, "system") == 0) {
                char *q = json_quote(msgs->v[i].content);
                buf_fmt(&b, ",\"system\":%s", q);
                free(q);
            }
        }
        buf_puts(&b, ",\"messages\":[");
        int first = 1;
        for (size_t i = 0; i < msgs->n; i++) {
            if (strcmp(msgs->v[i].role, "system") == 0) continue;
            const char *role = strcmp(msgs->v[i].role, "assistant") == 0
                ? "assistant" : "user";
            char *q = json_quote(msgs->v[i].content);
            if (!first) buf_putc(&b, ',');
            buf_fmt(&b, "{\"role\":\"%s\",\"content\":%s}", role, q);
            first = 0;
            free(q);
        }
        buf_puts(&b, "]}");
    } else {
        buf_fmt(&b, "{\"model\":%s,\"messages\":[", model);
        for (size_t i = 0; i < msgs->n; i++) {
            if (i) buf_putc(&b, ',');
            char *role = json_quote(msgs->v[i].role);
            char *content = json_quote(msgs->v[i].content);
            buf_fmt(&b, "{\"role\":%s,\"content\":%s}", role, content);
            free(role);
            free(content);
        }
        buf_fmt(&b, "],\"max_tokens\":%zu,\"stream\":%s", max_tokens,
                stream ? "true" : "false");
        if (stream && (p->kind == PROV_KILO || p->kind == PROV_OPENAI))
            buf_puts(&b, ",\"stream_options\":{\"include_usage\":true}");
        buf_putc(&b, '}');
    }
    free(model);
    return buf_take(&b);
}

void chat_parser_init(ChatParser *p)
{
    memset(p, 0, sizeof(*p));
}

void chat_parser_free(ChatParser *p)
{
    if (!p) return;
    buf_free(&p->lines);
    buf_free(&p->content);
    buf_free(&p->reasoning);
    free(p->model);
    free(p->error_msg);
    memset(p, 0, sizeof(*p));
}

void chat_parser_mark_done(ChatParser *p)
{
    if (p) p->done = 1;
}

static void set_error(ChatParser *p, const char *msg)
{
    if (!p || p->error) return;
    p->error = 1;
    free(p->error_msg);
    p->error_msg = xstrdup(msg ? msg : "model error");
}

static void parse_usage(ChatParser *p, JVal *root)
{
    double prompt = json_num(root, "usage.prompt_tokens");
    if (!prompt) prompt = json_num(root, "usage.input_tokens");
    double completion = json_num(root, "usage.completion_tokens");
    if (!completion) completion = json_num(root, "usage.output_tokens");
    if (prompt > 0) p->prompt_tokens = (size_t)prompt;
    if (completion > 0) p->completion_tokens = (size_t)completion;
    double cost = json_num(root, "usage.cost");
    if (cost > 0) p->cost = cost;
}

static void parse_openai_chunk(ChatParser *p, JVal *root)
{
    const char *model = json_str(root, "model");
    if (model && !p->model) p->model = xstrdup(model);

    JVal *err = json_get(root, "error");
    if (err && err->type == J_OBJ) {
        const char *msg = json_str(root, "error.message");
        set_error(p, msg ? msg : "model error");
        return;
    }

    JVal *delta = json_get(root, "choices.0.delta");
    if (delta && delta->type == J_OBJ) {
        const char *text = json_str(root, "choices.0.delta.content");
        const char *reasoning = json_str(root, "choices.0.delta.reasoning");
        if (text && *text) buf_add(&p->content, text, strlen(text));
        if (reasoning && *reasoning) buf_add(&p->reasoning, reasoning, strlen(reasoning));
        const char *finish = json_str(root, "choices.0.finish_reason");
        if (finish && *finish) p->done = 1;
    }
    parse_usage(p, root);
}

static void parse_anthropic_event(ChatParser *p, JVal *root)
{
    const char *type = json_str(root, "type");
    if (!type) return;
    if (strcmp(type, "content_block_delta") == 0) {
        const char *text = json_str(root, "delta.text");
        if (text && *text) buf_add(&p->content, text, strlen(text));
    } else if (strcmp(type, "message_start") == 0) {
        const char *model = json_str(root, "message.model");
        if (model && !p->model) p->model = xstrdup(model);
        parse_usage(p, root);
    } else if (strcmp(type, "message_delta") == 0) {
        parse_usage(p, root);
    } else if (strcmp(type, "message_stop") == 0) {
        p->done = 1;
    } else if (strcmp(type, "error") == 0) {
        set_error(p, json_str(root, "error.message"));
    }
}

static void process_line(ChatParser *p, char *line)
{
    while (*line == '\r' || *line == ' ' || *line == '\t') line++;
    if (!*line || *line == ':' || strncmp(line, "event:", 6) == 0) return;
    if (strncmp(line, "data:", 5) != 0) return;
    const char *data = line + 5;
    while (*data == ' ' || *data == '\t') data++;
    if (strcmp(data, "[DONE]") == 0) {
        p->done = 1;
        return;
    }
    char *jerr = NULL;
    JVal *root = json_parse(data, strlen(data), &jerr);
    if (!root) {
        free(jerr);
        return;
    }
    const char *type = json_str(root, "type");
    if (type && (strncmp(type, "message_", 8) == 0 ||
                 strcmp(type, "content_block_delta") == 0 ||
                 strcmp(type, "error") == 0)) {
        parse_anthropic_event(p, root);
    } else {
        parse_openai_chunk(p, root);
    }
    json_free(root);
}

void chat_parser_feed(ChatParser *p, const char *data, size_t len)
{
    if (!p || !data || !len) return;
    buf_add(&p->lines, data, len);
    for (;;) {
        char *nl = memchr(p->lines.p, '\n', p->lines.len);
        if (!nl) break;
        size_t linelen = (size_t)(nl - p->lines.p);
        char *line = xstrndup_n(p->lines.p, linelen);
        process_line(p, line);
        free(line);
        size_t used = linelen + 1;
        memmove(p->lines.p, p->lines.p + used, p->lines.len - used);
        p->lines.len -= used;
        p->lines.p[p->lines.len] = '\0';
    }
}

char *chat_parser_take(ChatParser *p)
{
    if (!p || p->taken >= p->content.len) return NULL;
    size_t n = p->content.len - p->taken;
    char *out = xstrndup_n(p->content.p + p->taken, n);
    p->taken = p->content.len;
    return out;
}

const char *chat_parser_text(const ChatParser *p)
{
    if (!p) return "";
    if (p->content.len) return p->content.p;
    return p->reasoning.p ? p->reasoning.p : "";
}

int chat_parse_response(const char *body, ChatParser *p)
{
    if (!body || !p) return -1;
    char *jerr = NULL;
    JVal *root = json_parse(body, strlen(body), &jerr);
    if (!root) {
        set_error(p, jerr ? jerr : "invalid model response");
        free(jerr);
        return -1;
    }
    JVal *err = json_get(root, "error");
    if (err && err->type == J_OBJ) {
        set_error(p, json_str(root, "error.message"));
        json_free(root);
        return -1;
    }
    const char *model = json_str(root, "model");
    if (model) p->model = xstrdup(model);
    const char *text = json_str(root, "choices.0.message.content");
    if (!text) text = json_str(root, "choices.0.message.reasoning");
    if (text) buf_add(&p->content, text, strlen(text));
    if (!text) {
        size_t n = json_arr_len(root, "content");
        for (size_t i = 0; i < n; i++) {
            const char *t = json_str(json_arr_at(root, "content", i), "text");
            if (t) buf_add(&p->content, t, strlen(t));
        }
    }
    parse_usage(p, root);
    p->done = 1;
    json_free(root);
    return 0;
}

void usage_init(Usage *u, int warn_pct, int compact_pct)
{
    memset(u, 0, sizeof(*u));
    u->warn_pct = warn_pct > 0 && warn_pct < 100 ? warn_pct : 70;
    u->compact_pct = compact_pct > 0 && compact_pct <= 100 ? compact_pct : 88;
}

void usage_free(Usage *u)
{
    if (!u) return;
    for (size_t i = 0; i < u->n; i++) {
        free(u->v[i].model);
    }
    free(u->v);
    memset(u, 0, sizeof(*u));
}

UsageEntry *usage_get(Usage *u, const char *model)
{
    if (!u || !model) return NULL;
    for (size_t i = 0; i < u->n; i++) {
        if (strcmp(u->v[i].model, model) == 0) return &u->v[i];
    }
    if (u->n == u->cap) {
        u->cap = u->cap ? u->cap * 2 : 8;
        u->v = xrealloc(u->v, u->cap * sizeof(UsageEntry));
    }
    UsageEntry *e = &u->v[u->n++];
    memset(e, 0, sizeof(*e));
    e->model = xstrdup(model);
    e->context = 128000;
    return e;
}

void usage_record(Usage *u, const char *model, size_t prompt, size_t completion,
                  double cost, size_t context, int anonymous)
{
    if (!u || !model) return;
    UsageEntry *e = usage_get(u, model);
    e->prompt += prompt;
    e->completion += completion;
    e->requests++;
    e->cost += cost;
    if (context) e->context = context;
    e->last_used = prompt + completion;
    e->last = time(NULL);
    u->last = e->last;
    if (anonymous) {
        if (!u->hour_start || u->last - u->hour_start >= 3600) {
            u->hour_start = u->last;
            u->anonymous_requests = 0;
        }
        u->anonymous_requests++;
    }
}

static const UsageEntry *usage_find(const Usage *u, const char *model)
{
    if (!u || !model) return NULL;
    for (size_t i = 0; i < u->n; i++) {
        if (strcmp(u->v[i].model, model) == 0) return &u->v[i];
    }
    return NULL;
}

size_t usage_context_used(const Usage *u, const char *model)
{
    const UsageEntry *e = usage_find(u, model);
    return e ? e->last_used : 0;
}

int usage_should_compact(const Usage *u, const char *model, size_t next_prompt_tokens)
{
    if (!u || !model) return 0;
    const UsageEntry *e = usage_find(u, model);
    if (!e) return 0;
    size_t context = e->context ? e->context : 128000;
    size_t threshold = context * (size_t)u->compact_pct / 100;
    return e->last_used + next_prompt_tokens >= threshold;
}

int usage_near_anonymous_limit(const Usage *u)
{
    if (!u || !u->hour_start) return 0;
    if (time(NULL) - u->hour_start >= 3600) return 0;
    return u->anonymous_requests >= 180;
}
