#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include <errno.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#include "catalog.h"
#include "config.h"
#include "http.h"
#include "json.h"
#include "provider.h"
#include "util.h"

/* freecode: small OpenCode-style terminal UI with a synchronized model team.
 * Default backend is the Kilo AI Gateway anonymous/free tier: no account,
 * no API key, and no paid models. Local and custom OpenAI-compatible routes
 * are optional and configurable.
 */

enum {
    ST_DEFAULT = 0,
    ST_DIM = 1,
    ST_HEADER = 2,
    ST_USER = 3,
    ST_ASSIST = 4,
    ST_STATUS = 5,
    ST_BOLD = 6,
    STYLE_COUNT = 7
};

static const char *const SGR[STYLE_COUNT] = {
    "\033[0m", "\033[2m", "\033[36m", "\033[32m",
    "\033[33m", "\033[31m", "\033[1;36m"
};

#define CELL_CONT ((wchar_t)0)
#define NO_SLOT ((size_t)-1)
#define MAX_LOG_MSGS 600
#define COMPACTION_KEEP 64

/* Chat memory is written automatically; these track the pending save. */
static int g_mem_dirty = 0;
static time_t g_mem_last_save = 0;
static char *g_mem_path = NULL;

static char *preview_text(const char *s, size_t len, size_t max);

/* ---------- utf8 / width ---------- */

static size_t utf8_next(const char *s, size_t len, size_t i, wchar_t *wc)
{
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) { *wc = (wchar_t)c; return 1; }
    size_t need;
    uint32_t cp;
    if ((c & 0xE0) == 0xC0) { need = 2; cp = c & 0x1Fu; }
    else if ((c & 0xF0) == 0xE0) { need = 3; cp = c & 0x0Fu; }
    else if ((c & 0xF8) == 0xF0) { need = 4; cp = c & 0x07u; }
    else { *wc = (wchar_t)c; return 1; }
    if (i + need > len) { *wc = (wchar_t)c; return 1; }
    for (size_t k = 1; k < need; k++) {
        unsigned char d = (unsigned char)s[i + k];
        if ((d & 0xC0) != 0x80) { *wc = (wchar_t)c; return 1; }
        cp = (cp << 6) | (d & 0x3Fu);
    }
    *wc = (wchar_t)cp;
    return need;
}

static size_t utf8_prev_start(const char *s, size_t len, size_t pos)
{
    (void)len;
    if (pos == 0) return 0;
    pos--;
    while (pos > 0 && (((unsigned char)s[pos]) & 0xC0) == 0x80) pos--;
    return pos;
}

static int wcw(wchar_t wc)
{
    int w = wcwidth(wc);
    if (w < 0) return (wc < 32 || wc == 0x7f) ? 0 : 1;
    return w;
}

static size_t text_width(const char *s, size_t len)
{
    size_t width = 0;
    for (size_t i = 0; i < len;) {
        wchar_t wc;
        i += utf8_next(s, len, i, &wc);
        int w = wcw(wc);
        if (w > 0) width += (size_t)w;
    }
    return width;
}

static size_t visible_input_offset(const char *s, size_t len, int cols)
{
    int avail = cols - 3;
    if (avail < 1) return len;
    int width = 0;
    size_t i = len;
    while (i > 0) {
        size_t p = utf8_prev_start(s, len, i);
        wchar_t wc;
        utf8_next(s, len, p, &wc);
        int w = wcw(wc);
        if (w == 0) { i = p; continue; }
        if (width + w > avail) break;
        width += w;
        i = p;
    }
    return i;
}

static void encode_wchar(Buf *b, wchar_t wc)
{
    uint32_t cp = (uint32_t)wc;
    if (cp < 0x80) {
        char c = (char)cp;
        buf_add(b, &c, 1);
    } else if (cp < 0x800) {
        char c[2] = {(char)(0xC0 | (cp >> 6)), (char)(0x80 | (cp & 0x3F))};
        buf_add(b, c, 2);
    } else if (cp < 0x10000) {
        char c[3] = {(char)(0xE0 | (cp >> 12)),
                     (char)(0x80 | ((cp >> 6) & 0x3F)),
                     (char)(0x80 | (cp & 0x3F))};
        buf_add(b, c, 3);
    } else {
        char c[4] = {(char)(0xF0 | (cp >> 18)),
                     (char)(0x80 | ((cp >> 12) & 0x3F)),
                     (char)(0x80 | ((cp >> 6) & 0x3F)),
                     (char)(0x80 | (cp & 0x3F))};
        buf_add(b, c, 4);
    }
}

/* ---------- screen ---------- */

typedef struct { wchar_t ch; uint8_t style; } Cell;

typedef struct {
    int rows, cols;
    Cell *cur, *prev;
    int last_style, cursor_x, cursor_y;
} Screen;

static void screen_force(Screen *s);

static void screen_init(Screen *s) { memset(s, 0, sizeof(*s)); }

static void screen_alloc(Screen *s, int rows, int cols)
{
    if (rows < 1) rows = 1;
    if (cols < 1) cols = 1;
    size_t n = (size_t)rows * (size_t)cols;
    s->cur = xrealloc(s->cur, n * sizeof(Cell));
    s->prev = xrealloc(s->prev, n * sizeof(Cell));
    s->rows = rows;
    s->cols = cols;
    screen_force(s);
}

static void screen_force(Screen *s)
{
    for (size_t i = 0; i < (size_t)s->rows * (size_t)s->cols; i++) {
        s->cur[i].ch = L' ';
        s->cur[i].style = ST_DEFAULT;
        s->prev[i].ch = L' ';
        s->prev[i].style = 255;
    }
    s->last_style = -1;
}

static void screen_put(Screen *s, int x, int y, wchar_t ch, uint8_t style)
{
    if (x < 0 || y < 0 || x >= s->cols || y >= s->rows || ch == CELL_CONT) return;
    size_t i = (size_t)y * (size_t)s->cols + (size_t)x;
    s->cur[i].ch = ch;
    s->cur[i].style = style;
    if (wcw(ch) == 2 && x + 1 < s->cols) {
        s->cur[i + 1].ch = CELL_CONT;
        s->cur[i + 1].style = style;
    }
}

static void screen_text(Screen *s, int x, int y, const char *str, size_t len, uint8_t style)
{
    if (y < 0 || y >= s->rows) return;
    for (size_t i = 0; i < len;) {
        wchar_t wc;
        size_t n = utf8_next(str, len, i, &wc);
        i += n;
        int w = wcw(wc);
        if (w == 0) continue;
        if (w == 2) {
            if (x < s->cols) screen_put(s, x, y, wc, style);
            if (x + 1 < s->cols) {
                size_t j = (size_t)y * (size_t)s->cols + (size_t)(x + 1);
                s->cur[j].ch = CELL_CONT;
                s->cur[j].style = style;
            }
            x += 2;
        } else {
            screen_put(s, x, y, wc, style);
            x += w;
        }
        if (x >= s->cols) break;
    }
}

static void write_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k > 0) { p += k; n -= (size_t)k; continue; }
        if (k < 0 && errno == EINTR) continue;
        fc_die("write");
    }
}

static void screen_flush(Screen *s)
{
    Buf out = {0};
    int last = s->last_style;
    for (int y = 0; y < s->rows; y++) {
        size_t base = (size_t)y * (size_t)s->cols;
        for (int x = 0; x < s->cols;) {
            size_t i = base + (size_t)x;
            if (s->cur[i].ch == s->prev[i].ch && s->cur[i].style == s->prev[i].style) {
                x++;
                continue;
            }
            uint8_t style = s->cur[i].style;
            int end = x + 1;
            while (end < s->cols) {
                size_t j = base + (size_t)end;
                if ((s->cur[j].ch != s->prev[j].ch || s->cur[j].style != s->prev[j].style) &&
                    s->cur[j].style == style) end++;
                else break;
            }
            buf_fmt(&out, "\033[%d;%dH", y + 1, x + 1);
            if (style != last && style < STYLE_COUNT) {
                buf_puts(&out, SGR[style]);
                last = (int)style;
            }
            for (int k = x; k < end; k++) {
                wchar_t ch = s->cur[base + (size_t)k].ch;
                if (ch != CELL_CONT) encode_wchar(&out, ch);
            }
            for (int k = x; k < end; k++) s->prev[base + (size_t)k] = s->cur[base + (size_t)k];
            x = end;
        }
    }
    buf_fmt(&out, "\033[%d;%dH", s->cursor_y + 1, s->cursor_x + 1);
    if (out.len) write_all(STDOUT_FILENO, out.p, out.len);
    s->last_style = last;
    buf_free(&out);
}

/* ---------- messages / sessions ---------- */

enum { MSG_USER = 0, MSG_ASSIST = 1, MSG_SYSTEM = 2 };

typedef struct { char *text; size_t len, cap; uint8_t kind; } Msg;

typedef struct {
    Msg *v;
    size_t n, cap;
    char *title;
} Session;

typedef struct {
    Session *v;
    size_t n, cap, cur, scroll;
} App;

typedef struct {
    int active;
    size_t session, message;
    char *target;
    size_t total, pos;
} Stream;

static void msg_append(Msg *m, const char *s, size_t n)
{
    if (m->len + n + 1 > m->cap) {
        size_t want = m->cap ? m->cap : 64;
        while (want < m->len + n + 1) want *= 2;
        m->text = xrealloc(m->text, want);
        m->cap = want;
    }
    memcpy(m->text + m->len, s, n);
    m->len += n;
    m->text[m->len] = '\0';
}

static void session_add(Session *s, const char *text, size_t len, uint8_t kind)
{
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 16;
        s->v = xrealloc(s->v, s->cap * sizeof(Msg));
    }
    Msg *m = &s->v[s->n++];
    memset(m, 0, sizeof(*m));
    m->kind = kind;
    msg_append(m, text, len);
    g_mem_dirty = 1;
}

static void app_new_session(App *a)
{
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 4;
        a->v = xrealloc(a->v, a->cap * sizeof(Session));
    }
    Session *s = &a->v[a->n];
    memset(s, 0, sizeof(*s));
    s->title = xstrdup("untitled");
    a->cur = a->n++;
    a->scroll = 0;
    g_mem_dirty = 1;
}

static void session_set_title(Session *s, const char *text, size_t len)
{
    if (strcmp(s->title, "untitled") != 0) return;
    char *p = preview_text(text, len, 28);
    free(s->title);
    s->title = p;
}

typedef void (*LineCb)(void *ctx, uint8_t kind, const char *p, size_t len, int first);

static void walk_lines(Session *s, int width, LineCb cb, void *ctx)
{
    int avail = width - 2;
    if (avail < 1) avail = 1;
    for (size_t mi = 0; mi < s->n; mi++) {
        Msg *m = &s->v[mi];
        if (m->len == 0) { cb(ctx, m->kind, "", 0, 1); continue; }
        size_t i = 0, line_start = 0;
        int width_used = 0, first = 1;
        while (i < m->len) {
            wchar_t wc;
            size_t n = utf8_next(m->text, m->len, i, &wc);
            int w = wcw(wc);
            if (w == 0) { i += n; continue; }
            if (width_used > 0 && width_used + w > avail) {
                cb(ctx, m->kind, m->text + line_start, i - line_start, first);
                first = 0;
                line_start = i;
                width_used = 0;
            }
            width_used += w;
            i += n;
        }
        cb(ctx, m->kind, m->text + line_start, m->len - line_start, first);
    }
}

static void count_cb(void *ctx, uint8_t kind, const char *p, size_t len, int first)
{
    (void)kind; (void)p; (void)len; (void)first;
    (*(size_t *)ctx)++;
}

typedef struct {
    Screen *screen;
    size_t line;
    int start, end, x0, y0, y1;
} LineCtx;

static void render_cb(void *ctx, uint8_t kind, const char *p, size_t len, int first)
{
    (void)first;
    LineCtx *c = ctx;
    size_t line = c->line++;
    if ((int)line < c->start || (int)line >= c->end) return;
    int y = c->y0 + (int)line - c->start;
    if (y < 0 || y >= c->y1 || y >= c->screen->rows) return;
    uint8_t style = ST_ASSIST;
    wchar_t marker = '*';
    if (kind == MSG_USER) { style = ST_USER; marker = '>'; }
    else if (kind == MSG_SYSTEM) { style = ST_DIM; marker = '#'; }
    screen_put(c->screen, c->x0, y, marker, style);
    screen_text(c->screen, c->x0 + 2, y, p, len, style);
}

static char *preview_text(const char *s, size_t len, size_t max)
{
    Buf b = {0};
    size_t i = 0, written = 0;
    while (i < len && written < max) {
        wchar_t wc;
        size_t n = utf8_next(s, len, i, &wc);
        unsigned char c = (wc < 32 || wc == 0x7f) ? (unsigned char)' ' : (unsigned char)wc;
        if (c >= 128) {
            for (size_t k = 0; k < n; k++) {
                char ch = s[i + k];
                buf_add(&b, &ch, 1);
            }
        } else {
            char ch = (char)c;
            buf_add(&b, &ch, 1);
        }
        i += n;
        written++;
    }
    char *out = xstrdup(b.p ? b.p : "");
    buf_free(&b);
    return out;
}

/* ---------- global runtime state ---------- */

typedef struct {
    char *name, *role, *model;
    int busy;
    size_t seen;
} ModelSlot;

typedef struct {
    ModelSlot *v;
    size_t n, cap, focus, scroll;
    int broadcast;
    Session log;
    char *summary;
} Team;

typedef struct {
    int active;
    size_t *slots;
    size_t n, pos;
    size_t message;
    HttpStream *req;
    ChatParser parser;
    int started;
    int stub;
    char *stub_target;
    size_t stub_total, stub_pos;
    size_t prompt_est;
} TeamRun;

typedef struct {
    int active;
    HttpStream *req;
    ChatParser parser;
    Buf body;
    char *reason;
    size_t prompt_est;
} CompactRun;

typedef struct {
    Catalog catalog;
    int discovered;
    char **candidates;
    size_t ncandidates;
} ModelPool;

static Config g_cfg;
static Provider g_provider;
static Usage g_usage;
static ModelPool g_pool;
static Team g_team;
static TeamRun g_run;
static CompactRun g_compact;
static char *g_pending = NULL;
static int g_compact_after_run = 0;
static int g_rate_warned = 0;
static char *g_input = NULL;
static size_t g_input_len = 0, g_input_cap = 0;

static void team_log_sys(Team *t, const char *msg);
static int compact_start(const char *reason);

static void input_clear(void)
{
    g_input_len = 0;
    if (g_input) g_input[0] = '\0';
}

/* ---------- model discovery and team ---------- */

static const char *const FALLBACK_FREE[] = {
    "kilo-auto/free",
    "reflection/beam-free",
    "poolside/laguna-s-2.1:free"
};

/* ---------- automatic chat memory ---------- */

static char *memory_default_path(void)
{
    const char *env = getenv("FREECODE_MEMORY_FILE");
    if (env && *env) return xstrdup(env);
    const char *state = getenv("XDG_STATE_HOME");
    if (state && *state) {
        size_t n = strlen(state) + strlen("/freecode/memory.jsonl") + 1;
        char *p = xmalloc(n);
        snprintf(p, n, "%s/freecode/memory.jsonl", state);
        return p;
    }
    const char *home = getenv("HOME");
    if (!home || !*home) home = ".";
    size_t n = strlen(home) + strlen("/.local/state/freecode/memory.jsonl") + 1;
    char *p = xmalloc(n);
    snprintf(p, n, "%s/.local/state/freecode/memory.jsonl", home);
    return p;
}

static void session_clear(Session *s)
{
    if (!s) return;
    for (size_t i = 0; i < s->n; i++) free(s->v[i].text);
    free(s->v);
    free(s->title);
    memset(s, 0, sizeof(*s));
}

static void app_clear(App *a)
{
    if (!a) return;
    for (size_t i = 0; i < a->n; i++) session_clear(&a->v[i]);
    free(a->v);
    memset(a, 0, sizeof(*a));
}

static void team_clear_state(Team *t)
{
    if (!t) return;
    for (size_t i = 0; i < t->n; i++) {
        free(t->v[i].name);
        free(t->v[i].role);
        free(t->v[i].model);
    }
    free(t->v);
    session_clear(&t->log);
    free(t->summary);
    memset(t, 0, sizeof(*t));
}

static void mem_add_slot(Team *t, const char *name, const char *role,
                         const char *model, size_t seen)
{
    if (!t || !name || !*name) return;
    t->v = xrealloc(t->v, (t->n + 1) * sizeof(ModelSlot));
    ModelSlot *s = &t->v[t->n++];
    memset(s, 0, sizeof(*s));
    s->name = xstrdup(name);
    s->role = xstrdup(role ? role : "worker");
    s->model = xstrdup(model ? model : "kilo-auto/free");
    s->seen = seen;
}

static void mem_write_messages(Buf *b, const Session *s)
{
    buf_putc(b, '[');
    for (size_t i = 0; i < s->n; i++) {
        if (i) buf_putc(b, ',');
        char *text = json_quote(s->v[i].text ? s->v[i].text : "");
        buf_fmt(b, "{\"kind\":%u,\"text\":%s}",
                (unsigned int)(s->v[i].kind <= 2 ? s->v[i].kind : MSG_SYSTEM), text);
        free(text);
    }
    buf_putc(b, ']');
}

static int memory_save_now(App *app, Team *t)
{
    if (!g_mem_path || !app || !t) return -1;
    if (fs_mkdir_parents(g_mem_path) != 0) return -1;

    size_t tmpcap = strlen(g_mem_path) + 8;
    char *tmp = xmalloc(tmpcap);
    snprintf(tmp, tmpcap, "%s.tmp", g_mem_path);

    FILE *f = fopen(tmp, "wb");
    if (!f) {
        free(tmp);
        return -1;
    }
    chmod(tmp, 0600);

    Buf b = {0};
    buf_fmt(&b, "{\"type\":\"meta\",\"version\":1,\"saved\":%ld,"
                "\"cur_session\":%zu,\"focus\":%zu,\"broadcast\":%s}\n",
            (long)time(NULL), app->cur, t->focus, t->broadcast ? "true" : "false");

    buf_puts(&b, "{\"type\":\"team\",\"summary\":");
    char *q = json_quote(t->summary ? t->summary : "");
    buf_puts(&b, q);
    free(q);
    buf_puts(&b, ",\"messages\":");
    mem_write_messages(&b, &t->log);
    buf_puts(&b, ",\"slots\":[");
    for (size_t i = 0; i < t->n; i++) {
        if (i) buf_putc(&b, ',');
        char *name = json_quote(t->v[i].name ? t->v[i].name : "");
        char *role = json_quote(t->v[i].role ? t->v[i].role : "");
        char *model = json_quote(t->v[i].model ? t->v[i].model : "");
        buf_fmt(&b, "{\"name\":%s,\"role\":%s,\"model\":%s,\"seen\":%zu}",
                name, role, model, t->v[i].seen);
        free(name);
        free(role);
        free(model);
    }
    buf_puts(&b, "]}\n");

    for (size_t i = 0; i < app->n; i++) {
        buf_puts(&b, "{\"type\":\"session\",\"title\":");
        q = json_quote(app->v[i].title ? app->v[i].title : "untitled");
        buf_puts(&b, q);
        free(q);
        buf_puts(&b, ",\"messages\":");
        mem_write_messages(&b, &app->v[i]);
        buf_puts(&b, "}\n");
    }

    int rc = 0;
    if (b.len && fwrite(b.p, 1, b.len, f) != b.len) rc = -1;
    if (fflush(f) != 0) rc = -1;
    if (fsync(fileno(f)) != 0 && errno != EINVAL) rc = -1;
    if (fclose(f) != 0) rc = -1;
    if (rc == 0 && rename(tmp, g_mem_path) != 0) rc = -1;
    if (rc != 0) unlink(tmp);

    buf_free(&b);
    free(tmp);
    return rc;
}

static void mem_read_messages(JVal *root, Session *s)
{
    size_t n = json_arr_len(root, "messages");
    for (size_t i = 0; i < n; i++) {
        JVal *item = json_arr_at(root, "messages", i);
        const char *text = json_str(item, "text");
        int kind = (int)json_num(item, "kind");
        if (kind < 0 || kind > 2) kind = MSG_SYSTEM;
        session_add(s, text ? text : "", text ? strlen(text) : 0, (uint8_t)kind);
    }
}

static int memory_load(const char *path, App *app, Team *t)
{
    if (!path || !app || !t) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return 1;

    Buf raw = {0};
    char chunk[8192];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) buf_add(&raw, chunk, n);
    fclose(f);
    if (!raw.len) {
        buf_free(&raw);
        return 1;
    }

    App loaded_app;
    Team loaded_team;
    memset(&loaded_app, 0, sizeof(loaded_app));
    memset(&loaded_team, 0, sizeof(loaded_team));

    int ok = 1;
    char *p = raw.p;
    char *end = raw.p + raw.len;
    while (p < end && ok) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        char *line_end = nl ? nl : end;
        size_t len = (size_t)(line_end - p);
        if (len) {
            char *line = xstrndup_n(p, len);
            char *jerr = NULL;
            JVal *root = json_parse(line, len, &jerr);
            if (!root) {
                free(jerr);
                ok = 0;
            } else {
                const char *type = json_str(root, "type");
                if (type && strcmp(type, "meta") == 0) {
                    loaded_app.cur = (size_t)json_num(root, "cur_session");
                    loaded_team.focus = (size_t)json_num(root, "focus");
                    loaded_team.broadcast = json_bool(root, "broadcast");
                } else if (type && strcmp(type, "team") == 0) {
                    const char *summary = json_str(root, "summary");
                    free(loaded_team.summary);
                    loaded_team.summary = xstrdup(summary ? summary : "");
                    mem_read_messages(root, &loaded_team.log);
                    size_t ns = json_arr_len(root, "slots");
                    for (size_t i = 0; i < ns; i++) {
                        JVal *item = json_arr_at(root, "slots", i);
                        size_t seen = (size_t)json_num(item, "seen");
                        if (seen > loaded_team.log.n) seen = loaded_team.log.n;
                        mem_add_slot(&loaded_team,
                                     json_str(item, "name"),
                                     json_str(item, "role"),
                                     json_str(item, "model"), seen);
                    }
                } else if (type && strcmp(type, "session") == 0) {
                    Session s;
                    memset(&s, 0, sizeof(s));
                    const char *title = json_str(root, "title");
                    s.title = xstrdup(title ? title : "untitled");
                    mem_read_messages(root, &s);
                    loaded_app.v = xrealloc(loaded_app.v, (loaded_app.n + 1) * sizeof(Session));
                    loaded_app.v[loaded_app.n++] = s;
                }
                json_free(root);
            }
            free(line);
        }
        p = nl ? nl + 1 : end;
    }
    buf_free(&raw);

    if (!ok) {
        app_clear(&loaded_app);
        team_clear_state(&loaded_team);
        return -1;
    }

    if (!loaded_app.n) app_new_session(&loaded_app);
    if (!loaded_team.n) {
        mem_add_slot(&loaded_team, "planner", "plan",
                     g_cfg.model ? g_cfg.model : FALLBACK_FREE[0], 0);
        loaded_team.broadcast = 1;
    }
    if (loaded_app.cur >= loaded_app.n) loaded_app.cur = loaded_app.n - 1;
    if (loaded_team.focus >= loaded_team.n) loaded_team.focus = loaded_team.n - 1;
    loaded_app.scroll = 0;
    loaded_team.scroll = 0;

    app_clear(app);
    team_clear_state(t);
    *app = loaded_app;
    *t = loaded_team;
    return 0;
}

static void memory_save_if_dirty(App *app, Team *t, int force)
{
    if (!g_mem_dirty) return;
    if (!force && g_mem_last_save && time(NULL) - g_mem_last_save < 2) return;
    if (memory_save_now(app, t) == 0) {
        g_mem_dirty = 0;
        g_mem_last_save = time(NULL);
    }
}

static void memory_clear(App *app, Team *t)
{
    if (g_mem_path) {
        unlink(g_mem_path);
        size_t n = strlen(g_mem_path) + 8;
        char *tmp = xmalloc(n);
        snprintf(tmp, n, "%s.tmp", g_mem_path);
        unlink(tmp);
        free(tmp);
    }

    app_clear(app);
    app_new_session(app);
    Session *s = &app->v[0];
    static const char welcome[] = "freecode: fresh chat memory started.";
    session_add(s, welcome, sizeof(welcome) - 1, MSG_SYSTEM);
    free(s->title);
    s->title = xstrdup("freecode");

    team_clear_state(t);
    static const char *const roles[] = {
        "plan", "review", "critic", "worker", "worker", "worker", "worker", "worker"
    };
    for (int i = 0; i < g_cfg.team_size; i++) {
        char name[32];
        snprintf(name, sizeof(name), i == 0 ? "planner" : i == 1 ? "reviewer" : "m%d", i + 1);
        const char *model = i == 0 && g_cfg.model && *g_cfg.model
            ? g_cfg.model : FALLBACK_FREE[(size_t)i % (sizeof(FALLBACK_FREE) / sizeof(FALLBACK_FREE[0]))];
        mem_add_slot(t, name, roles[i], model, 0);
    }
    t->focus = 0;
    t->broadcast = 1;
    g_mem_dirty = 1;
}

static void pool_init(void)
{
    memset(&g_pool, 0, sizeof(g_pool));
}

static void pool_free(void)
{
    catalog_free(&g_pool.catalog);
    for (size_t i = 0; i < g_pool.ncandidates; i++) free(g_pool.candidates[i]);
    free(g_pool.candidates);
    memset(&g_pool, 0, sizeof(g_pool));
}

static void pool_set(char **ids, size_t n)
{
    for (size_t i = 0; i < g_pool.ncandidates; i++) free(g_pool.candidates[i]);
    free(g_pool.candidates);
    g_pool.candidates = ids;
    g_pool.ncandidates = n;
}

static void discover_models(int quiet)
{
    /* The default team always comes from Kilo's anonymous free catalog. */
    static const char kilo_base[] = "https://ai-gateway.kilo.ai/api/v1";
    Catalog c;
    if (catalog_fetch(&c, kilo_base, NULL, 6000) != 0) {
        if (!quiet) {
            char msg[256];
            snprintf(msg, sizeof(msg), "discovery failed (%s); using free fallbacks",
                     c.err ? c.err : "network error");
            team_log_sys(&g_team, msg);
        }
        catalog_free(&c);
        size_t n = sizeof(FALLBACK_FREE) / sizeof(FALLBACK_FREE[0]);
        char **ids = xmalloc(n * sizeof(char *));
        for (size_t i = 0; i < n; i++) ids[i] = xstrdup(FALLBACK_FREE[i]);
        pool_set(ids, n);
        g_pool.discovered = 0;
        return;
    }
    catalog_free(&g_pool.catalog);
    g_pool.catalog = c;
    size_t n = 0;
    char **ids = catalog_select_ids(&g_pool.catalog, (size_t)g_cfg.team_size + 3, &n);
    if (!ids || !n) {
        free(ids);
        size_t k = 1;
        ids = xmalloc(k * sizeof(char *));
        ids[0] = xstrdup("kilo-auto/free");
        n = k;
    }
    pool_set(ids, n);
    g_pool.discovered = 1;
    if (!quiet) {
        char msg[256];
        snprintf(msg, sizeof(msg), "discovered %zu free anonymous models; best: %s",
                 n, ids[0]);
        team_log_sys(&g_team, msg);
    }
}

static const char *pool_model_at(size_t i)
{
    if (!g_pool.ncandidates) return "kilo-auto/free";
    return g_pool.candidates[i % g_pool.ncandidates];
}

static size_t team_find(Team *t, const char *name)
{
    if (!t || !name) return NO_SLOT;
    for (size_t i = 0; i < t->n; i++) {
        if (strcmp(t->v[i].name, name) == 0) return i;
    }
    return NO_SLOT;
}

static void team_add_slot(Team *t, const char *name, const char *role, const char *model)
{
    if (team_find(t, name) != NO_SLOT) return;
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 4;
        t->v = xrealloc(t->v, t->cap * sizeof(ModelSlot));
    }
    ModelSlot *s = &t->v[t->n];
    memset(s, 0, sizeof(*s));
    s->name = xstrdup(name);
    s->role = xstrdup(role ? role : "worker");
    s->model = xstrdup(model ? model : pool_model_at(t->n));
    s->seen = t->log.n;
    if (t->n == 0) t->focus = 0;
    t->n++;
    g_mem_dirty = 1;
}

static void team_remove_slot(Team *t, const char *name)
{
    size_t idx = team_find(t, name);
    if (idx == NO_SLOT) return;
    free(t->v[idx].name);
    free(t->v[idx].role);
    free(t->v[idx].model);
    for (size_t i = idx + 1; i < t->n; i++) t->v[i - 1] = t->v[i];
    t->n--;
    if (t->n == 0) t->focus = 0;
    else if (t->focus >= t->n) t->focus = t->n - 1;
    g_mem_dirty = 1;
}

static void team_log_sys(Team *t, const char *msg)
{
    if (!t || !msg) return;
    session_add(&t->log, msg, strlen(msg), MSG_SYSTEM);
    for (size_t i = 0; i < t->n; i++) t->v[i].seen = t->log.n;
}

static void team_init(void)
{
    memset(&g_team, 0, sizeof(g_team));
    static const char welcome[] =
        "Kilo free gateway team: no account, no API key, no paid models. All slots share one synchronized log. Chat memory is saved automatically.";
    session_add(&g_team.log, welcome, sizeof(welcome) - 1, MSG_SYSTEM);
    static const char *const roles[] = {"plan", "review", "critic", "worker", "worker", "worker", "worker", "worker"};
    for (int i = 0; i < g_cfg.team_size; i++) {
        char name[32];
        snprintf(name, sizeof(name), i == 0 ? "planner" : i == 1 ? "reviewer" : "m%d", i + 1);
        const char *model = i == 0 && g_cfg.model && *g_cfg.model
            ? g_cfg.model : pool_model_at((size_t)i);
        team_add_slot(&g_team, name, roles[i], model);
    }
    g_team.focus = 0;
    g_team.broadcast = 1;
}

static void team_cleanup(void)
{
    team_clear_state(&g_team);
}

static void team_sync_all(Team *t)
{
    for (size_t i = 0; i < t->n; i++) t->v[i].seen = t->log.n;
}

static char *team_serialize(Team *t, size_t upto, size_t max_chars)
{
    Buf b = {0};
    for (size_t i = 0; i < upto && i < t->log.n; i++) {
        const char *prefix = t->log.v[i].kind == MSG_USER ? "[you] "
                           : t->log.v[i].kind == MSG_SYSTEM ? "[system] " : "[assistant] ";
        buf_puts(&b, prefix);
        buf_add(&b, t->log.v[i].text, t->log.v[i].len);
        buf_putc(&b, '\n');
    }
    if (max_chars && b.len > max_chars) {
        size_t skip = b.len - max_chars;
        memmove(b.p, b.p + skip, max_chars);
        b.len = max_chars;
        b.p[b.len] = '\0';
    }
    return buf_take(&b);
}

static size_t team_prompt_tokens(Team *t, size_t upto, const char *extra)
{
    char *s = team_serialize(t, upto, 48000);
    size_t n = estimate_tokens_str(s);
    free(s);
    if (extra) n += estimate_tokens_str(extra);
    return n + 32;
}

static void team_build_messages(Team *t, size_t upto, ChatMsgs *out)
{
    chatmsgs_add(out, "system",
        "You are one slot in a synchronized model team. Read the shared log carefully. "
        "Build on earlier work instead of repeating it. Be concise and concrete.");
    for (size_t i = 0; i < upto && i < t->log.n; i++) {
        const char *role = t->log.v[i].kind == MSG_USER ? "user"
                         : t->log.v[i].kind == MSG_SYSTEM ? "system" : "assistant";
        chatmsgs_add(out, role, t->log.v[i].text);
    }
}

static void rotate_models(int log_it)
{
    if (!g_team.n || !g_pool.ncandidates) return;
    for (size_t i = 0; i < g_team.n; i++) {
        size_t idx = 0;
        for (size_t k = 0; k < g_pool.ncandidates; k++) {
            if (strcmp(g_pool.candidates[k], g_team.v[i].model) == 0) { idx = k; break; }
        }
        free(g_team.v[i].model);
        g_team.v[i].model = xstrdup(g_pool.candidates[(idx + 1 + i) % g_pool.ncandidates]);
    }
    provider_set_model(&g_provider, g_team.v[g_team.focus].model);
    g_mem_dirty = 1;
    if (log_it) team_log_sys(&g_team, "models rotated after compaction/error");
}

/* ---------- provider setup ---------- */

static void provider_reconfigure(void)
{
    provider_free(&g_provider);
    ProviderKind kind = config_provider_kind(&g_cfg);
    const char *base = g_cfg.base_url && *g_cfg.base_url
        ? g_cfg.base_url
        : (kind == PROV_LOCAL ? "http://127.0.0.1:11434/v1" : "https://ai-gateway.kilo.ai/api/v1");
    provider_init(&g_provider, kind, g_cfg.provider ? g_cfg.provider : "kilo",
                  base, g_cfg.api_key, g_team.n ? g_team.v[g_team.focus].model : g_cfg.model);
    g_provider.transport = g_cfg.transport;
    g_provider.timeout_ms = 45000;
    g_provider.free_only = g_cfg.free_only;
}

static void usage_pct_for(const char *model, size_t *used, size_t *total)
{
    UsageEntry *e = usage_get(&g_usage, model ? model : "kilo-auto/free");
    *total = e && e->context ? e->context : 128000;
    *used = e ? e->last_used : 0;
}

/* ---------- run / compaction ---------- */

static void run_reset(void)
{
    chat_parser_free(&g_run.parser);
    if (g_run.req) http_stream_free(g_run.req);
    free(g_run.slots);
    free(g_run.stub_target);
    memset(&g_run, 0, sizeof(g_run));
    chat_parser_init(&g_run.parser);
}

static void compact_reset(void)
{
    chat_parser_free(&g_compact.parser);
    if (g_compact.req) http_stream_free(g_compact.req);
    buf_free(&g_compact.body);
    free(g_compact.reason);
    memset(&g_compact, 0, sizeof(g_compact));
    chat_parser_init(&g_compact.parser);
}

static int start_http_job(HttpStream **req, ChatMsgs *msgs, int stream)
{
    char *url = provider_chat_url(&g_provider);
    char *body = provider_chat_body(&g_provider, msgs, stream, 4096);
    HttpReq hr;
    memset(&hr, 0, sizeof(hr));
    hr.method = xstrdup("POST");
    hr.url = url;
    hr.body = body;
    hr.timeout_ms = g_provider.timeout_ms;
    hr.transport = g_provider.transport;
    provider_add_headers(&hr, &g_provider);
    char *err = NULL;
    int rc = http_stream_start(&hr, req, &err);
    if (rc != 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "request failed: %s", err ? err : "unknown error");
        team_log_sys(&g_team, msg);
        free(err);
    }
    http_req_free(&hr);
    return rc;
}

static void team_start_job(void)
{
    TeamRun *r = &g_run;
    if (!r->active || r->pos >= r->n) return;
    size_t slot = r->slots[r->pos];
    if (slot >= g_team.n) { r->pos++; team_start_job(); return; }

    ModelSlot *ms = &g_team.v[slot];
    session_add(&g_team.log, "", 0, MSG_ASSIST);
    r->message = g_team.log.n - 1;
    msg_append(&g_team.log.v[r->message], ms->name, strlen(ms->name));
    msg_append(&g_team.log.v[r->message], ": ", 2);
    team_sync_all(&g_team);
    ms->busy = 1;

    if (g_cfg.stub) {
        char buf[512];
        snprintf(buf, sizeof(buf), "local stub for %s/%s: synchronized reply queued", ms->name, ms->model);
        r->stub_target = xstrdup(buf);
        r->stub_total = strlen(r->stub_target);
        r->stub_pos = 0;
        r->stub = 1;
        r->started = 1;
        return;
    }

    ChatMsgs msgs;
    chatmsgs_init(&msgs);
    team_build_messages(&g_team, r->message, &msgs);
    char *old = g_provider.model;
    g_provider.model = xstrdup(ms->model);
    chat_parser_free(&r->parser);
    chat_parser_init(&r->parser);
    int rc = start_http_job(&r->req, &msgs, 1);
    free(g_provider.model);
    g_provider.model = old;
    chatmsgs_free(&msgs);
    if (rc == 0) r->started = 1;
    else {
        msg_append(&g_team.log.v[r->message], "request failed", 14);
        ms->busy = 0;
        r->pos++;
        team_start_job();
    }
}

static void team_begin_run(void)
{
    if (!g_pending || !g_team.n) { free(g_pending); g_pending = NULL; return; }
    run_reset();
    size_t upto = g_team.log.n;
    size_t est = team_prompt_tokens(&g_team, upto, g_pending) + 16;
    const char *model = g_team.v[g_team.focus].model;

    if (usage_should_compact(&g_usage, model, est) ||
        (usage_near_anonymous_limit(&g_usage) && !g_rate_warned)) {
        g_run.prompt_est = est;
        compact_start("context/rate threshold");
        return;
    }

    char user[2048];
    int n = snprintf(user, sizeof(user), "[you] %s", g_pending);
    if (n < 0) n = 0;
    session_add(&g_team.log, user, (size_t)n, MSG_USER);
    team_sync_all(&g_team);
    free(g_pending);
    g_pending = NULL;

    size_t count = g_team.broadcast ? g_team.n : 1;
    g_run.active = 1;
    g_run.n = count;
    g_run.slots = xmalloc(count * sizeof(size_t));
    for (size_t i = 0; i < count; i++) g_run.slots[i] = g_team.broadcast ? i : g_team.focus;
    g_run.pos = 0;
    g_run.prompt_est = est;
    g_compact_after_run = 0;
    team_start_job();
}

static void team_send(const char *text, size_t len)
{
    if (g_run.active || g_compact.active) {
        team_log_sys(&g_team, "busy: wait for the current round to finish");
        return;
    }
    free(g_pending);
    g_pending = xstrndup_n(text, len);
    team_begin_run();
}

static int compact_start(const char *reason)
{
    if (g_compact.active || !g_team.n) return -1;
    compact_reset();
    g_compact.active = 1;
    g_compact.reason = xstrdup(reason ? reason : "manual compaction");
    g_compact.prompt_est = team_prompt_tokens(&g_team, g_team.log.n, NULL);

    if (g_cfg.stub) {
        static const char stub_summary[] =
            "Compaction summary (local stub): preserved team decisions and open tasks.";
        free(g_team.summary);
        g_team.summary = xstrdup(stub_summary);
        g_team.log.n = 0;
        session_add(&g_team.log, stub_summary, strlen(stub_summary), MSG_SYSTEM);
        team_sync_all(&g_team);
        char msg[256];
        snprintf(msg, sizeof(msg), "compacted (stub): %s", g_compact.reason);
        team_log_sys(&g_team, msg);
        rotate_models(1);
        compact_reset();
        if (g_pending) team_begin_run();
        return 0;
    }

    ChatMsgs msgs;
    chatmsgs_init(&msgs);
    chatmsgs_add(&msgs, "system",
        "Compress the shared team transcript into a dense continuation summary. "
        "Preserve decisions, file paths, constraints, open tasks, and the latest user request. "
        "Do not answer the task; only summarize it in under 500 words.");
    char *serialized = team_serialize(&g_team, g_team.log.n, 48000);
    chatmsgs_add(&msgs, "user", serialized);
    free(serialized);
    char *old = g_provider.model;
    g_provider.model = xstrdup(pool_model_at(0));
    int rc = start_http_job(&g_compact.req, &msgs, 0);
    free(g_provider.model);
    g_provider.model = old;
    chatmsgs_free(&msgs);
    if (rc != 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "compaction request failed: %s", g_compact.reason);
        team_log_sys(&g_team, msg);
        compact_reset();
        if (g_pending) team_begin_run();
        return -1;
    }
    return 0;
}

static void compact_finish(void)
{
    int status = http_stream_status(g_compact.req);
    char *err = NULL;
    http_stream_finish(g_compact.req, &status, &err);
    ChatParser *p = &g_compact.parser;
    if (chat_parse_response(g_compact.body.p ? g_compact.body.p : "", p) != 0 ||
        (!p->content.len && !p->reasoning.len) || status < 200 || status >= 300) {
        char msg[256];
        snprintf(msg, sizeof(msg), "compaction failed (status %d): %s", status,
                 p->error_msg ? p->error_msg : (err ? err : "empty summary"));
        team_log_sys(&g_team, msg);
        free(err);
        compact_reset();
        if (g_pending) team_begin_run();
        return;
    }
    free(err);

    const char *summary = chat_parser_text(p);
    free(g_team.summary);
    g_team.summary = xstrdup(summary);

    /* Replace the transcript with the summary plus the last few messages. */
    size_t keep = g_team.log.n > COMPACTION_KEEP ? COMPACTION_KEEP : 0;
    Msg *saved = NULL;
    if (keep) {
        saved = xmalloc(keep * sizeof(Msg));
        memcpy(saved, g_team.log.v + (g_team.log.n - keep), keep * sizeof(Msg));
    }
    for (size_t i = 0; i < g_team.log.n; i++) free(g_team.log.v[i].text);
    free(g_team.log.v);
    g_team.log.v = NULL;
    g_team.log.n = g_team.log.cap = 0;
    session_add(&g_team.log, g_team.summary, strlen(g_team.summary), MSG_SYSTEM);
    if (saved) {
        for (size_t i = 0; i < keep; i++) {
            session_add(&g_team.log, saved[i].text ? saved[i].text : "",
                        saved[i].len, saved[i].kind);
            free(saved[i].text);
        }
        free(saved);
    }
    team_sync_all(&g_team);
    char msg[512];
    snprintf(msg, sizeof(msg), "compacted: %s -> %s (%zu bytes summary)",
             g_compact.reason ? g_compact.reason : "manual",
             g_team.v[g_team.focus].model, strlen(g_team.summary));
    team_log_sys(&g_team, msg);
    rotate_models(1);
    compact_reset();
    if (g_pending) team_begin_run();
}

static void compact_tick(void)
{
    if (!g_compact.active || !g_compact.req) return;
    char buf[8192];
    int n = http_stream_read(g_compact.req, buf, sizeof(buf));
    if (n > 0) buf_add(&g_compact.body, buf, (size_t)n);
    if (n < 0 || http_stream_eof(g_compact.req)) compact_finish();
}

static void run_finish_job(void)
{
    TeamRun *r = &g_run;
    ModelSlot *ms = r->pos < r->n && r->slots[r->pos] < g_team.n
        ? &g_team.v[r->slots[r->pos]] : NULL;
    int status = r->req ? http_stream_status(r->req) : 200;
    char *err = NULL;
    if (r->req) http_stream_finish(r->req, &status, &err);

    const char *text = chat_parser_text(&r->parser);
    if ((status < 200 || status >= 300) || r->parser.error) {
        char e[256];
        snprintf(e, sizeof(e), " [error %d: %s]", status,
                 r->parser.error_msg ? r->parser.error_msg : (err ? err : "request failed"));
        msg_append(&g_team.log.v[r->message], e, strlen(e));
        if (status == 402 || status == 429 ||
            str_contains_ci(e, "rate") || str_contains_ci(e, "balance")) {
            g_rate_warned = 1;
            g_compact_after_run = 1;
        }
    }
    free(err);
    g_mem_dirty = 1;

    size_t prompt = r->parser.prompt_tokens ? r->parser.prompt_tokens : r->prompt_est;
    size_t completion = r->parser.completion_tokens ? r->parser.completion_tokens
                                                   : estimate_tokens_str(text ? text : "");
    size_t context = 128000;
    UsageEntry *ue = usage_get(&g_usage, ms ? ms->model : "kilo-auto/free");
    if (ue && ue->context) context = ue->context;
    usage_record(&g_usage, ms ? ms->model : "kilo-auto/free", prompt, completion,
                 r->parser.cost, context,
                 config_provider_kind(&g_cfg) == PROV_KILO && (!g_cfg.api_key || !*g_cfg.api_key));
    if (ms) ms->busy = 0;
    team_sync_all(&g_team);

    if (usage_should_compact(&g_usage, ms ? ms->model : "kilo-auto/free", 512))
        g_compact_after_run = 1;
    r->pos++;
    if (r->pos < r->n) {
        chat_parser_free(&r->parser);
        chat_parser_init(&r->parser);
        if (r->req) { http_stream_free(r->req); r->req = NULL; }
        free(r->stub_target); r->stub_target = NULL;
        r->stub = 0; r->started = 0; r->stub_pos = 0; r->stub_total = 0;
        team_start_job();
    } else {
        int should_compact = g_compact_after_run;
        run_reset();
        if (should_compact) compact_start("usage threshold");
    }
}

static void run_tick(void)
{
    TeamRun *r = &g_run;
    if (!r->active || !r->started) return;
    if (r->stub) {
        if (!r->stub_target || r->message >= g_team.log.n) { run_finish_job(); return; }
        size_t step = 2;
        if (r->stub_pos + step > r->stub_total) step = r->stub_total - r->stub_pos;
        msg_append(&g_team.log.v[r->message], r->stub_target + r->stub_pos, step);
        g_mem_dirty = 1;
        r->stub_pos += step;
        if (r->stub_pos >= r->stub_total) run_finish_job();
        return;
    }
    if (!r->req) { run_finish_job(); return; }
    char buf[8192];
    int n = http_stream_read(r->req, buf, sizeof(buf));
    if (n > 0) chat_parser_feed(&r->parser, buf, (size_t)n);
    char *delta = chat_parser_take(&r->parser);
    if (delta) {
        if (r->message < g_team.log.n) {
            msg_append(&g_team.log.v[r->message], delta, strlen(delta));
            g_mem_dirty = 1;
        }
        free(delta);
    }
    if (n < 0 || http_stream_eof(r->req)) {
        char *tail = chat_parser_take(&r->parser);
        if (tail) {
            if (r->message < g_team.log.n) {
                msg_append(&g_team.log.v[r->message], tail, strlen(tail));
                g_mem_dirty = 1;
            }
            free(tail);
        }
        run_finish_job();
    }
}

/* ---------- local stub session ---------- */

static void start_stream(App *a, Stream *st)
{
    if (!g_input_len) return;
    Session *s = &a->v[a->cur];
    session_add(s, g_input, g_input_len, MSG_USER);
    session_set_title(s, g_input, g_input_len);
    char *preview = preview_text(g_input, g_input_len, 72);
    char target[512];
    snprintf(target, sizeof(target), "local stub: received %zu bytes: %s", g_input_len, preview);
    free(preview);
    session_add(s, "", 0, MSG_ASSIST);
    free(st->target);
    st->target = xstrdup(target);
    st->total = strlen(st->target);
    st->pos = 0;
    st->session = a->cur;
    st->message = s->n - 1;
    st->active = 1;
    a->scroll = 0;
}

static void stream_tick(App *a, Stream *st)
{
    if (!st->active) return;
    if (st->session >= a->n || st->message >= a->v[st->session].n) { st->active = 0; return; }
    Msg *m = &a->v[st->session].v[st->message];
    size_t step = 2;
    if (st->pos + step > st->total) step = st->total - st->pos;
    msg_append(m, st->target + st->pos, step);
    g_mem_dirty = 1;
    st->pos += step;
    if (st->pos >= st->total) st->active = 0;
}

/* ---------- commands ---------- */

static const char *const PALETTE_ITEMS[] = {
    "New session", "Next session", "Clear prompt", "Force redraw",
    "Toggle team view", "Toggle broadcast", "Discover free models",
    "Compact now", "Show usage", "Save config", "Memory status",
    "Clear memory", "Quit"
};
#define PALETTE_COUNT ((int)(sizeof(PALETTE_ITEMS) / sizeof(PALETTE_ITEMS[0])))

static void cmd_split(const char *s, size_t len, char *name, size_t name_cap,
                      char *rest, size_t rest_cap)
{
    size_t i = 0;
    if (i < len && s[i] == '/') i++;
    size_t a = 0;
    while (i < len && s[i] != ' ' && a + 1 < name_cap) name[a++] = s[i++];
    name[a] = '\0';
    while (i < len && s[i] == ' ') i++;
    size_t b = 0;
    while (i < len && b + 1 < rest_cap) rest[b++] = s[i++];
    rest[b] = '\0';
}

static const char *next_token(char **p)
{
    while (**p == ' ') (*p)++;
    if (!**p) return NULL;
    char *start = *p;
    while (**p && **p != ' ') (*p)++;
    if (**p) { **p = '\0'; (*p)++; }
    return start;
}

static void cmd_models(void)
{
    team_log_sys(&g_team, "available free anonymous models:");
    for (size_t i = 0; i < g_pool.ncandidates && i < 12; i++) {
        char line[256];
        snprintf(line, sizeof(line), "  %zu. %s%s", i + 1, g_pool.candidates[i],
                 i == 0 ? "  (best)" : "");
        team_log_sys(&g_team, line);
    }
    for (size_t i = 0; i < g_team.n; i++) {
        char line[256];
        snprintf(line, sizeof(line), "  slot %s/%s -> %s", g_team.v[i].name,
                 g_team.v[i].role, g_team.v[i].model);
        team_log_sys(&g_team, line);
    }
}

static void cmd_usage(void)
{
    size_t used = 0, total = 0;
    usage_pct_for(g_team.n ? g_team.v[g_team.focus].model : "kilo-auto/free", &used, &total);
    char line[512];
    snprintf(line, sizeof(line),
             "usage: provider=%s model=%s ctx=%zu/%zu (%zu%%) requests-anon=%zu warn=%d%% compact=%d%%",
             g_cfg.provider, g_team.n ? g_team.v[g_team.focus].model : "-",
             used, total, total ? used * 100 / total : 0,
             g_usage.anonymous_requests, g_cfg.warn_pct, g_cfg.compact_pct);
    team_log_sys(&g_team, line);
}

static void cmd_memory(App *a)
{
    size_t local_msgs = 0;
    for (size_t i = 0; i < a->n; i++) local_msgs += a->v[i].n;
    char line[640];
    snprintf(line, sizeof(line),
             "memory: auto-save=on file=%s team_msgs=%zu local_msgs=%zu sessions=%zu summary=%zu bytes",
             g_mem_path ? g_mem_path : "-", g_team.log.n, local_msgs, a->n,
             g_team.summary ? strlen(g_team.summary) : 0);
    team_log_sys(&g_team, line);
    if (memory_save_now(a, &g_team) == 0) {
        g_mem_dirty = 0;
        g_mem_last_save = time(NULL);
        team_log_sys(&g_team, "memory flushed to disk");
    } else {
        team_log_sys(&g_team, "memory flush failed");
    }
}

static void cmd_clear_memory(App *a, int *team_mode)
{
    memory_clear(a, &g_team);
    *team_mode = 1;
    team_log_sys(&g_team, "chat memory cleared (auto-save remains on)");
}

static int run_command(App *a, Stream *st, int *team_mode, int *dirty,
                       int *force, int *running)
{
    if (!g_input_len || g_input[0] != '/') return 0;
    char cmd[32] = {0}, rest[512] = {0};
    cmd_split(g_input, g_input_len, cmd, sizeof(cmd), rest, sizeof(rest));
    char *p = rest;

    if (strcmp(cmd, "team") == 0) *team_mode = 1;
    else if (strcmp(cmd, "local") == 0 && !*p) *team_mode = 0;
    else if (strcmp(cmd, "add") == 0) {
        const char *name = next_token(&p);
        const char *role = next_token(&p);
        const char *model = next_token(&p);
        char n[64];
        if (!name) snprintf(n, sizeof(n), "m%zu", g_team.n + 1);
        else snprintf(n, sizeof(n), "%s", name);
        team_add_slot(&g_team, n, role, model);
        *team_mode = 1;
        char msg[256];
        snprintf(msg, sizeof(msg), "added slot %s (%s) -> %s", n,
                 role ? role : "worker", model ? model : g_team.v[g_team.n - 1].model);
        team_log_sys(&g_team, msg);
    } else if (strcmp(cmd, "rm") == 0 || strcmp(cmd, "remove") == 0) {
        const char *name = next_token(&p);
        if (name && team_find(&g_team, name) != NO_SLOT) {
            team_remove_slot(&g_team, name);
            char msg[160];
            snprintf(msg, sizeof(msg), "removed slot %s", name);
            team_log_sys(&g_team, msg);
        } else team_log_sys(&g_team, "slot not found");
    } else if (strcmp(cmd, "focus") == 0) {
        const char *name = next_token(&p);
        size_t idx = team_find(&g_team, name ? name : "");
        if (idx != NO_SLOT) {
            g_team.focus = idx;
            g_team.broadcast = 0;
            char msg[160];
            snprintf(msg, sizeof(msg), "focus: %s (broadcast off)", name);
            team_log_sys(&g_team, msg);
        } else team_log_sys(&g_team, "slot not found");
    } else if (strcmp(cmd, "sync") == 0) {
        g_team.broadcast = !g_team.broadcast;
        *team_mode = 1;
        team_log_sys(&g_team, g_team.broadcast ? "sync: broadcast to all" : "sync: focused slot only");
    } else if (strcmp(cmd, "broadcast") == 0) {
        g_team.broadcast = 1;
        *team_mode = 1;
        team_log_sys(&g_team, "sync: broadcast to all");
    } else if (strcmp(cmd, "models") == 0) {
        cmd_models();
    } else if (strcmp(cmd, "discover") == 0) {
        discover_models(0);
    } else if (strcmp(cmd, "model") == 0) {
        const char *model = next_token(&p);
        if (model && g_team.n) {
            free(g_team.v[g_team.focus].model);
            g_team.v[g_team.focus].model = xstrdup(model);
            provider_set_model(&g_provider, model);
            char msg[256];
            snprintf(msg, sizeof(msg), "model set: %s -> %s", g_team.v[g_team.focus].name, model);
            team_log_sys(&g_team, msg);
        } else team_log_sys(&g_team, "usage: /model ID");
    } else if (strcmp(cmd, "smodel") == 0) {
        const char *name = next_token(&p);
        const char *model = next_token(&p);
        size_t idx = team_find(&g_team, name ? name : "");
        if (idx != NO_SLOT && model) {
            free(g_team.v[idx].model);
            g_team.v[idx].model = xstrdup(model);
            char msg[256];
            snprintf(msg, sizeof(msg), "slot %s model=%s", name, model);
            team_log_sys(&g_team, msg);
        } else team_log_sys(&g_team, "usage: /smodel NAME MODEL");
    } else if (strcmp(cmd, "provider") == 0) {
        const char *name = next_token(&p);
        if (name) {
            free(g_cfg.provider);
            g_cfg.provider = xstrdup(name);
            provider_reconfigure();
            char msg[256];
            snprintf(msg, sizeof(msg), "provider=%s base=%s", name, g_cfg.base_url);
            team_log_sys(&g_team, msg);
        }
    } else if (strcmp(cmd, "route") == 0) {
        const char *url = next_token(&p);
        if (url) {
            free(g_cfg.base_url);
            g_cfg.base_url = xstrdup(url);
            provider_reconfigure();
            char msg[256];
            snprintf(msg, sizeof(msg), "route=%s", url);
            team_log_sys(&g_team, msg);
        } else team_log_sys(&g_team, "usage: /route URL");
    } else if (strcmp(cmd, "apikey") == 0) {
        const char *key = next_token(&p);
        free(g_cfg.api_key);
        g_cfg.api_key = key ? xstrdup(key) : NULL;
        provider_reconfigure();
        team_log_sys(&g_team, key ? "API key set (masked)" : "API key cleared");
    } else if (strcmp(cmd, "local") == 0 && *p) {
        const char *url = next_token(&p);
        const char *model = next_token(&p);
        free(g_cfg.provider); g_cfg.provider = xstrdup("local");
        if (url) { free(g_cfg.base_url); g_cfg.base_url = xstrdup(url); }
        if (model) { free(g_cfg.model); g_cfg.model = xstrdup(model); }
        provider_reconfigure();
        if (g_team.n) {
            free(g_team.v[g_team.focus].model);
            g_team.v[g_team.focus].model = xstrdup(model ? model : g_cfg.model);
        }
        char msg[256];
        snprintf(msg, sizeof(msg), "local route=%s model=%s",
                 g_cfg.base_url, model ? model : g_cfg.model);
        team_log_sys(&g_team, msg);
    } else if (strcmp(cmd, "usage") == 0) {
        cmd_usage();
    } else if (strcmp(cmd, "memory") == 0) {
        cmd_memory(a);
    } else if (strcmp(cmd, "clearmemory") == 0 || strcmp(cmd, "forget") == 0) {
        cmd_clear_memory(a, team_mode);
    } else if (strcmp(cmd, "compact") == 0) {
        compact_start(rest[0] ? rest : "manual /compact");
    } else if (strcmp(cmd, "rotate") == 0) {
        rotate_models(1);
    } else if (strcmp(cmd, "threshold") == 0) {
        const char *v = next_token(&p);
        if (v) {
            g_cfg.compact_pct = atoi(v);
            if (g_cfg.compact_pct <= g_cfg.warn_pct || g_cfg.compact_pct > 100)
                g_cfg.compact_pct = 88;
            usage_init(&g_usage, g_cfg.warn_pct, g_cfg.compact_pct);
            char msg[160];
            snprintf(msg, sizeof(msg), "compaction threshold=%d%%", g_cfg.compact_pct);
            team_log_sys(&g_team, msg);
        }
    } else if (strcmp(cmd, "stub") == 0) {
        g_cfg.stub = !g_cfg.stub;
        char msg[128];
        snprintf(msg, sizeof(msg), "local stub %s", g_cfg.stub ? "on" : "off");
        team_log_sys(&g_team, msg);
    } else if (strcmp(cmd, "save") == 0) {
        char *path = config_default_path();
        int rc = config_save(&g_cfg, path);
        team_log_sys(&g_team, rc == 0 ? "config saved (chmod 600)" : "config save failed");
        free(path);
    } else if (strcmp(cmd, "teamhelp") == 0) {
        team_log_sys(&g_team,
            "/team /local /add NAME ROLE MODEL /rm NAME /focus NAME /sync /broadcast | "
            "/models /discover /model ID /smodel NAME MODEL /provider NAME /route URL /apikey KEY | "
            "/local URL MODEL /usage /memory /clearmemory /compact /rotate /threshold PCT /stub /save");
    } else if (strcmp(cmd, "new") == 0) {
        app_new_session(a);
        st->active = 0;
        *team_mode = 0;
    } else if (strcmp(cmd, "next") == 0) {
        a->cur = (a->cur + 1) % a->n;
        a->scroll = 0;
        st->active = 0;
        *team_mode = 0;
    } else if (strcmp(cmd, "clear") == 0) {
        /* input is replaced by the command itself */
    } else if (strcmp(cmd, "redraw") == 0) {
        *force = 1;
    } else if (strcmp(cmd, "quit") == 0) {
        *running = 0;
    } else if (strcmp(cmd, "help") == 0) {
        session_add(&a->v[a->cur],
            "/new /next /clear /redraw /help /quit | /team /models /discover /usage /memory /clearmemory /compact /stub",
            strlen("/new /next /clear /redraw /help /quit | /team /models /discover /usage /memory /clearmemory /compact /stub"),
            MSG_SYSTEM);
    } else {
        char msg[160];
        snprintf(msg, sizeof(msg), "unknown command: /%s", cmd);
        if (*team_mode) team_log_sys(&g_team, msg);
        else session_add(&a->v[a->cur], msg, strlen(msg), MSG_SYSTEM);
    }
    input_clear();
    *dirty = 1;
    return 1;
}

static void palette_execute(int sel, App *a, Stream *st, int *team_mode,
                            int *dirty, int *force, int *running)
{
    switch (sel) {
    case 0: app_new_session(a); st->active = 0; *team_mode = 0; break;
    case 1: a->cur = (a->cur + 1) % a->n; a->scroll = 0; st->active = 0; *team_mode = 0; break;
    case 2: input_clear(); break;
    case 3: *force = 1; break;
    case 4: *team_mode = !*team_mode; break;
    case 5: g_team.broadcast = !g_team.broadcast; team_log_sys(&g_team,
                g_team.broadcast ? "sync: broadcast to all" : "sync: focused slot only"); break;
    case 6: discover_models(0); *team_mode = 1; break;
    case 7: compact_start("palette"); *team_mode = 1; break;
    case 8: cmd_usage(); *team_mode = 1; break;
    case 9: {
        char *path = config_default_path();
        config_save(&g_cfg, path);
        free(path);
        team_log_sys(&g_team, "config saved");
        break;
    }
    case 10: cmd_memory(a); *team_mode = 1; break;
    case 11: cmd_clear_memory(a, team_mode); break;
    case 12: *running = 0; break;
    default: break;
    }
    *dirty = 1;
}

/* ---------- input ---------- */

static void process_input(App *a, Stream *st, const char *buf, ssize_t n,
                          int *team_mode, int *dirty, int *force, int *running,
                          int *palette_open, size_t *pal_sel)
{
    for (ssize_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (*palette_open) {
            if (c == 27 && i + 2 < n && buf[i + 1] == '[') {
                if (buf[i + 2] == 'A') { if (*pal_sel > 0) (*pal_sel)--; i += 2; }
                else if (buf[i + 2] == 'B') { if (*pal_sel + 1 < PALETTE_COUNT) (*pal_sel)++; i += 2; }
                continue;
            }
            if (c == '\r' || c == '\n') {
                palette_execute((int)*pal_sel, a, st, team_mode, dirty, force, running);
                *palette_open = 0;
                *pal_sel = 0;
                continue;
            }
            if (c == 3 || c == 27) { *palette_open = 0; continue; }
            continue;
        }

        if (c == 3 || c == 4) { *running = 0; continue; }
        if (c == '\r' || c == '\n') {
            if (!run_command(a, st, team_mode, dirty, force, running)) {
                if (*team_mode) team_send(g_input, g_input_len);
                else start_stream(a, st);
                input_clear();
            }
            *dirty = 1;
            continue;
        }
        if (c == 127 || c == 8) {
            if (g_input_len) {
                size_t p = utf8_prev_start(g_input, g_input_len, g_input_len);
                g_input_len = p;
                g_input[p] = '\0';
            }
            *dirty = 1;
            continue;
        }
        if (c == 21) { input_clear(); *dirty = 1; continue; } /* Ctrl+U */
        if (c == 12) { *force = 1; *dirty = 1; continue; }   /* Ctrl+L */
        if (c == 2) {
            g_team.broadcast = !g_team.broadcast;
            *team_mode = 1;
            team_log_sys(&g_team, g_team.broadcast ? "sync: broadcast to all" : "sync: focused slot only");
            *dirty = 1;
            continue;
        }
        if (c == 14) { app_new_session(a); st->active = 0; *team_mode = 0; *dirty = 1; continue; }
        if (c == 20) { *team_mode = !*team_mode; *dirty = 1; continue; }
        if (c == 16) { *palette_open = 1; *pal_sel = 0; *dirty = 1; continue; }
        if (c == 27) {
            size_t *scrollp = *team_mode ? &g_team.scroll : &a->scroll;
            if (i + 2 >= n) continue;
            if (buf[i + 1] == '[') {
                if (buf[i + 2] == 'A') { (*scrollp)++; i += 2; }
                else if (buf[i + 2] == 'B') { if (*scrollp) (*scrollp)--; i += 2; }
                else if (buf[i + 2] == '5' && i + 3 < n && buf[i + 3] == '~') { *scrollp += 10; i += 3; }
                else if (buf[i + 2] == '6' && i + 3 < n && buf[i + 3] == '~') {
                    *scrollp = *scrollp > 10 ? *scrollp - 10 : 0;
                    i += 3;
                } else i += 2;
            } else i++;
            *dirty = 1;
            continue;
        }
        if (c < 32) continue;
        if (g_input_len + 4 >= g_input_cap) {
            g_input_cap = g_input_cap ? g_input_cap * 2 : 128;
            g_input = xrealloc(g_input, g_input_cap);
        }
        g_input[g_input_len++] = (char)c;
        g_input[g_input_len] = '\0';
        *dirty = 1;
    }
}

/* ---------- render ---------- */

static void render(App *a, Screen *s, Stream *st, int team_mode,
                   const char *input, size_t input_len,
                   int palette_open, size_t pal_sel, int force)
{
    (void)force;
    for (int y = 0; y < s->rows; y++) {
        for (int x = 0; x < s->cols; x++) {
            s->cur[(size_t)y * (size_t)s->cols + (size_t)x].ch = L' ';
            s->cur[(size_t)y * (size_t)s->cols + (size_t)x].style = ST_DEFAULT;
        }
    }

    char header[256];
    const char *model = g_team.n ? g_team.v[g_team.focus].model : "none";
    size_t used = 0, total = 0;
    usage_pct_for(model, &used, &total);
    if (team_mode) {
        snprintf(header, sizeof(header),
                 " ltui  kilo-free team %zu  %s  ctx %zu%%  sync %s  focus %s",
                 g_team.n, model, total ? used * 100 / total : 0,
                 g_team.broadcast ? "all" : "one",
                 g_team.n ? g_team.v[g_team.focus].name : "-");
    } else {
        snprintf(header, sizeof(header),
                 " ltui  %s/%s  ctx %zu%%  session %zu/%zu  [local]",
                 g_cfg.provider, model, total ? used * 100 / total : 0, a->cur + 1, a->n);
    }
    screen_text(s, 0, 0, header, strlen(header), ST_HEADER);
    if (s->rows > 1) {
        for (int x = 0; x < s->cols; x++) screen_put(s, x, 1, '-', ST_DIM);
    }

    int body_y0 = 2;
    int status_y = s->rows > 3 ? s->rows - 2 : 1;
    int prompt_y = s->rows > 1 ? s->rows - 1 : 0;
    int body_rows = status_y - body_y0;
    if (body_rows < 1) body_rows = 1;

    int sidebar_w = 0;
    if (team_mode) {
        if (s->cols >= 88) sidebar_w = 36;
        else if (s->cols >= 62) sidebar_w = 24;
    } else {
        if (s->cols >= 70) sidebar_w = 24;
        else if (s->cols >= 50) sidebar_w = 16;
    }
    if (sidebar_w > 0) {
        int list_h = body_rows - 1;
        if (team_mode) {
            screen_text(s, 1, body_y0, " MODELS (free/no-auth)", strlen(" MODELS (free/no-auth)"), ST_BOLD);
            size_t start = g_team.n > (size_t)list_h ? g_team.n - (size_t)list_h : 0;
            for (size_t i = start; i < g_team.n && list_h > 0; i++) {
                int y = body_y0 + 1 + (int)(i - start);
                if (y >= status_y) break;
                ModelSlot *ms = &g_team.v[i];
                uint8_t style = i == g_team.focus ? ST_BOLD : ST_DIM;
                screen_put(s, 1, y, i == g_team.focus ? '>' : ' ', style);
                char line[256];
                snprintf(line, sizeof(line), "%s/%s %s %zu/%zu %s", ms->name, ms->role,
                         ms->busy ? "busy" : "idle", ms->seen, g_team.log.n, ms->model);
                char *fit = preview_text(line, strlen(line), (size_t)(sidebar_w - 3));
                screen_text(s, 3, y, fit, strlen(fit), style);
                free(fit);
            }
        } else {
            screen_text(s, 1, body_y0, " SESSIONS", strlen(" SESSIONS"), ST_BOLD);
            size_t start = a->n > (size_t)list_h ? a->n - (size_t)list_h : 0;
            for (size_t i = start; i < a->n && list_h > 0; i++) {
                int y = body_y0 + 1 + (int)(i - start);
                if (y >= status_y) break;
                uint8_t style = i == a->cur ? ST_BOLD : ST_DIM;
                screen_put(s, 1, y, i == a->cur ? '>' : ' ', style);
                char *title = preview_text(a->v[i].title, strlen(a->v[i].title),
                                           (size_t)(sidebar_w - 3));
                screen_text(s, 3, y, title, strlen(title), style);
                free(title);
            }
        }
        for (int y = body_y0; y < status_y; y++) screen_put(s, sidebar_w - 1, y, '|', ST_DIM);
    }

    int x0 = sidebar_w;
    int width = s->cols - x0;
    if (width < 2) width = 2;
    Session *view = team_mode ? &g_team.log : &a->v[a->cur];
    size_t *scrollp = team_mode ? &g_team.scroll : &a->scroll;
    size_t total_lines = 0;
    walk_lines(view, width, count_cb, &total_lines);
    int maxscroll = total_lines > (size_t)body_rows ? (int)(total_lines - (size_t)body_rows) : 0;
    if ((int)*scrollp > maxscroll) *scrollp = (size_t)maxscroll;
    int start_line = total_lines > (size_t)body_rows
        ? (int)(total_lines - (size_t)body_rows - *scrollp) : 0;
    LineCtx lc = { s, 0, start_line, start_line + body_rows, x0, body_y0, status_y };
    walk_lines(view, width, render_cb, &lc);

    const char *status;
    if (g_compact.active) status = " compacting transcript | memory auto-save | feeding summary to next model | /teamhelp";
    else if (g_run.active) status = " team running | memory auto-save | synchronized round | Ctrl+T local | /teamhelp";
    else if (team_mode) status = " team | memory auto-save | /models /discover /usage /compact /stub | Ctrl+T local";
    else status = st->active
        ? " local-stub | memory auto-save | streaming | Ctrl+T team | Ctrl+P commands | /help"
        : " ready | memory auto-save | Ctrl+T team | Ctrl+P commands | /help";
    screen_text(s, 0, status_y, status, strlen(status), ST_STATUS);

    if (s->rows > 2) for (int x = 0; x < s->cols; x++) screen_put(s, x, status_y + 1, '-', ST_DIM);

    if (input_len == 0) {
        const char *hint = team_mode
            ? "Message the team (free Kilo models)... (/teamhelp)"
            : "Type a message... (/ for commands)";
        screen_text(s, 2, prompt_y, hint, strlen(hint), ST_DIM);
        s->cursor_x = 2;
    } else {
        size_t off = visible_input_offset(input, input_len, s->cols);
        size_t visible = input_len - off;
        s->cursor_x = (int)text_width(input + off, visible) + 2;
        if (s->cursor_x >= s->cols) s->cursor_x = s->cols - 1;
        screen_text(s, 2, prompt_y, input + off, visible, ST_DEFAULT);
    }

    if (palette_open) {
        int h = PALETTE_COUNT + 2;
        int w = 40;
        if (w > s->cols) w = s->cols;
        int py = prompt_y - h;
        if (py < body_y0) py = body_y0;
        int px = s->cols > w ? s->cols - w - 1 : 0;
        for (int y = py; y < py + h && y < s->rows; y++) {
            for (int x = px; x < px + w && x < s->cols; x++) screen_put(s, x, y, ' ', ST_DEFAULT);
        }
        screen_text(s, px + 1, py, " COMMANDS ", strlen(" COMMANDS "), ST_HEADER);
        for (int i = 0; i < PALETTE_COUNT && py + 1 + i < s->rows; i++) {
            uint8_t style = (size_t)i == pal_sel ? ST_BOLD : ST_DIM;
            screen_put(s, px + 1, py + 1 + i, (size_t)i == pal_sel ? '>' : ' ', style);
            screen_text(s, px + 3, py + 1 + i, PALETTE_ITEMS[i], strlen(PALETTE_ITEMS[i]), style);
        }
    }

    screen_flush(s);
}

/* ---------- terminal / main ---------- */

static volatile sig_atomic_t g_resize = 0;

static void on_winch(int sig)
{
    (void)sig;
    g_resize = 1;
}

int main(void)
{
    setlocale(LC_ALL, "");
    struct termios orig;
    int have_term = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &orig) == 0;
    if (have_term) {
        struct termios raw = orig;
        raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
        raw.c_oflag &= ~OPOST;
        raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }

    static const char enter_alt[] = "\033[?1049h\033[2J\033[H";
    write_all(STDOUT_FILENO, enter_alt, strlen(enter_alt));

    char *cfg_path = config_default_path();
    config_load(&g_cfg, cfg_path);
    free(cfg_path);

    pool_init();
    chat_parser_init(&g_run.parser);
    chat_parser_init(&g_compact.parser);
    usage_init(&g_usage, g_cfg.warn_pct, g_cfg.compact_pct);

    /* Default team comes from the live Kilo free catalog, with no key. */
    if (!getenv("FREECODE_NO_DISCOVER")) discover_models(1);
    else {
        size_t n = sizeof(FALLBACK_FREE) / sizeof(FALLBACK_FREE[0]);
        char **ids = xmalloc(n * sizeof(char *));
        for (size_t i = 0; i < n; i++) ids[i] = xstrdup(FALLBACK_FREE[i]);
        pool_set(ids, n);
    }
    team_init();

    App app;
    memset(&app, 0, sizeof(app));
    app_new_session(&app);
    Session *s0 = &app.v[0];
    session_add(s0,
        "freecode: Kilo free gateway team, synchronized rounds, auto-compaction, automatic chat memory.",
        strlen("freecode: Kilo free gateway team, synchronized rounds, auto-compaction, automatic chat memory."),
        MSG_SYSTEM);
    session_add(s0, "Type /team to join the team, /teamhelp for model/route/usage/memory commands.",
                strlen("Type /team to join the team, /teamhelp for model/route/usage/memory commands."),
                MSG_SYSTEM);
    free(s0->title);
    s0->title = xstrdup("freecode");

    /* Restore the previous conversation before configuring the provider so the
     * saved focus model and full transcript are used immediately. */
    g_mem_path = memory_default_path();
    int mem_rc = memory_load(g_mem_path, &app, &g_team);
    if (mem_rc < 0) team_log_sys(&g_team, "chat memory load failed; starting fresh");
    g_mem_dirty = mem_rc < 0;
    g_mem_last_save = time(NULL);
    provider_reconfigure();

    Stream stream;
    memset(&stream, 0, sizeof(stream));
    g_input = xmalloc(128);
    g_input_cap = 128;
    g_input[0] = '\0';

    Screen screen;
    screen_init(&screen);
    struct winsize ws;
    int rows = 24, cols = 100;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        rows = ws.ws_row;
        cols = ws.ws_col;
    }
    screen_alloc(&screen, rows, cols);

    signal(SIGWINCH, on_winch);
    int running = 1, dirty = 1, force = 1;
    int palette_open = 0;
    size_t pal_sel = 0;
    int team_mode = 1;

    while (running) {
        if (g_resize) {
            g_resize = 0;
            if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
                if (ws.ws_row != screen.rows || ws.ws_col != screen.cols) {
                    screen_alloc(&screen, ws.ws_row, ws.ws_col);
                    force = 1;
                    dirty = 1;
                }
            }
        }
        if (g_compact.active) { compact_tick(); dirty = 1; }
        else if (g_run.active) { run_tick(); dirty = 1; }
        else if (stream.active) { stream_tick(&app, &stream); dirty = 1; }

        struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
        int busy = g_compact.active || g_run.active || stream.active;
        memory_save_if_dirty(&app, &g_team, 0);
        int r = poll(&pfd, 1, busy ? 12 : 40);
        if (r > 0 && (pfd.revents & POLLIN)) {
            char buf[256];
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n == 0) running = 0;
            else if (n > 0) process_input(&app, &stream, buf, n, &team_mode,
                                           &dirty, &force, &running, &palette_open, &pal_sel);
            else if (errno != EINTR) fc_die("read");
        }
        if (dirty) {
            render(&app, &screen, &stream, team_mode, g_input, g_input_len,
                   palette_open, pal_sel, force);
            dirty = 0;
            force = 0;
        }
    }

    memory_save_if_dirty(&app, &g_team, 1);

    static const char leave_alt[] = "\033[0m\033[?1049l";
    write_all(STDOUT_FILENO, leave_alt, strlen(leave_alt));
    if (have_term) tcsetattr(STDIN_FILENO, TCSANOW, &orig);

    free(g_input);
    free(g_mem_path);
    free(stream.target);
    run_reset();
    compact_reset();
    free(g_pending);
    team_cleanup();
    pool_free();
    provider_free(&g_provider);
    usage_free(&g_usage);
    config_free(&g_cfg);
    for (size_t i = 0; i < app.n; i++) {
        for (size_t k = 0; k < app.v[i].n; k++) free(app.v[i].v[k].text);
        free(app.v[i].v);
        free(app.v[i].title);
    }
    free(app.v);
    free(screen.cur);
    free(screen.prev);
    return 0;
}
