#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "http.h"
#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef USE_LIBCURL
#include <curl/curl.h>
#endif

typedef struct {
    char scheme[16];
    char host[256];
    char path[1024];
    int port;
} Url;

struct HttpStream {
    int fd;
    int errfd;
    pid_t pid;
    Buf pending;
    Buf errbuf;
    char *body_path;
    int header_done;
    int status;
    int eof;
    int finished;
};

static int url_parse(const char *url, Url *u)
{
    memset(u, 0, sizeof(*u));
    const char *p = url;
    const char *sep = strstr(p, "://");
    if (!sep) return -1;
    size_t sl = (size_t)(sep - p);
    if (sl >= sizeof(u->scheme)) return -1;
    memcpy(u->scheme, p, sl);
    p = sep + 3;
    const char *end = p + strlen(p);
    const char *slash = strchr(p, '/');
    const char *hostend = slash ? slash : end;
    size_t hl = (size_t)(hostend - p);
    if (hl >= sizeof(u->host)) return -1;
    memcpy(u->host, p, hl);
    char *colon = memchr(u->host, ':', hl);
    if (colon) {
        *colon = '\0';
        u->port = atoi(colon + 1);
    } else {
        u->port = strcmp(u->scheme, "https") == 0 ? 443 : 80;
    }
    if (slash) {
        size_t pl = strlen(slash);
        if (pl >= sizeof(u->path)) return -1;
        memcpy(u->path, slash, pl);
    } else {
        u->path[0] = '/';
    }
    if (!u->port) u->port = strcmp(u->scheme, "https") == 0 ? 443 : 80;
    return 0;
}

static void write_fd_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k > 0) {
            p += k;
            n -= (size_t)k;
        } else if (k < 0 && errno == EINTR) {
            continue;
        } else {
            _exit(3);
        }
    }
}

static int has_header(const char *hdrs, const char *name)
{
    return strcasestr(hdrs, name) != NULL;
}

static void child_direct(const HttpReq *req)
{
    Url u;
    if (url_parse(req->url, &u) != 0 || strcmp(u.scheme, "http") != 0) {
        dprintf(STDERR_FILENO, "freecode: direct transport supports http:// only\n");
        _exit(2);
    }

    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", u.port);
    struct addrinfo hints, *ai = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(u.host, portstr, &hints, &ai);
    if (rc != 0 || !ai) {
        dprintf(STDERR_FILENO, "freecode: getaddrinfo: %s\n", gai_strerror(rc));
        _exit(2);
    }

    int fd = -1;
    for (struct addrinfo *a = ai; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(ai);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "freecode: connect failed\n");
        _exit(2);
    }

    Buf reqbuf = {0};
    buf_fmt(&reqbuf, "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: freecode/1\r\nConnection: close\r\n",
            req->method ? req->method : "GET", u.path, u.host);
    if (req->body && *req->body) {
        if (!has_header(reqbuf.p, "Content-Type:"))
            buf_puts(&reqbuf, "Content-Type: application/json\r\n");
        buf_fmt(&reqbuf, "Content-Length: %zu\r\n", strlen(req->body));
    }
    for (size_t i = 0; i < req->nheaders; i++) {
        if (req->headers[i].key && req->headers[i].value)
            buf_fmt(&reqbuf, "%s: %s\r\n", req->headers[i].key, req->headers[i].value);
    }
    buf_puts(&reqbuf, "\r\n");
    if (req->body && *req->body) buf_puts(&reqbuf, req->body);
    write_fd_all(fd, reqbuf.p, reqbuf.len);
    buf_free(&reqbuf);

    Buf raw = {0};
    char tmp[8192];
    for (;;) {
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n > 0) {
            buf_add(&raw, tmp, (size_t)n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    close(fd);

    char *hdr_end = strstr(raw.p, "\r\n\r\n");
    size_t body_off;
    if (hdr_end) body_off = (size_t)(hdr_end + 4 - raw.p);
    else if ((hdr_end = strstr(raw.p, "\n\n"))) body_off = (size_t)(hdr_end + 2 - raw.p);
    else body_off = 0;

    int status = 200;
    if (body_off) {
        char *line_end = strchr(raw.p, '\n');
        if (line_end) {
            char *line = xstrndup_n(raw.p, (size_t)(line_end - raw.p));
            char *sp = strchr(line, ' ');
            if (sp) status = atoi(sp + 1);
            free(line);
        }
    }
    char head[64];
    snprintf(head, sizeof(head), "HTTP/1.1 %d OK\r\n\r\n", status);
    write_fd_all(STDOUT_FILENO, head, strlen(head));

    if (!body_off) {
        if (raw.len) write_fd_all(STDOUT_FILENO, raw.p, raw.len);
        buf_free(&raw);
        _exit(0);
    }

    char *headers = xstrndup_n(raw.p, body_off);
    const char *body = raw.p + body_off;
    if (has_header(headers, "Transfer-Encoding: chunked") ||
        has_header(headers, "transfer-encoding: chunked")) {
        size_t i = 0;
        while (i < raw.len - body_off) {
            char *nl = memchr(body + i, '\n', raw.len - body_off - i);
            if (!nl) break;
            size_t linelen = (size_t)(nl - (body + i));
            char *size_line = xstrndup_n(body + i, linelen);
            char *semi = strchr(size_line, ';');
            if (semi) *semi = '\0';
            char *trimmed = str_trim(size_line);
            char *end = NULL;
            long chunk = strtol(trimmed, &end, 16);
            free(size_line);
            i += linelen + 1;
            if (chunk <= 0 || (size_t)chunk > raw.len - body_off - i) break;
            write_fd_all(STDOUT_FILENO, body + i, (size_t)chunk);
            i += (size_t)chunk;
            if (i + 2 <= raw.len - body_off && body[i] == '\r' && body[i + 1] == '\n') i += 2;
            else if (i < raw.len - body_off && body[i] == '\n') i++;
        }
    } else {
        write_fd_all(STDOUT_FILENO, body, raw.len - body_off);
    }
    free(headers);
    buf_free(&raw);
    _exit(0);
}

static void child_curl(const HttpReq *req, const char *body_path)
{
    char **argv = xmalloc(32 * sizeof(char *));
    size_t n = 0;
    char secs[32];
    snprintf(secs, sizeof(secs), "%d", (req->timeout_ms + 999) / 1000);
    argv[n++] = (char *)"curl";
    argv[n++] = (char *)"-sS";
    argv[n++] = (char *)"-i";
    argv[n++] = (char *)"-N";
    argv[n++] = (char *)"--http1.1";
    argv[n++] = (char *)"--compressed";
    argv[n++] = (char *)"--max-time";
    argv[n++] = secs;
    argv[n++] = (char *)"-X";
    argv[n++] = (char *)(req->method ? req->method : "GET");
    for (size_t i = 0; i < req->nheaders && n + 3 < 32; i++) {
        if (!req->headers[i].key || !req->headers[i].value) continue;
        char *h = xmalloc(strlen(req->headers[i].key) + strlen(req->headers[i].value) + 4);
        snprintf(h, strlen(req->headers[i].key) + strlen(req->headers[i].value) + 4,
                 "%s: %s", req->headers[i].key, req->headers[i].value);
        argv[n++] = (char *)"-H";
        argv[n++] = h;
    }
    if (body_path && n + 3 < 32) {
        char *spec = xmalloc(strlen(body_path) + 3);
        snprintf(spec, strlen(body_path) + 3, "@%s", body_path);
        argv[n++] = (char *)"--data-binary";
        argv[n++] = spec;
    }
    argv[n++] = req->url;
    argv[n++] = NULL;
    execvp("curl", argv);
    dprintf(STDERR_FILENO, "freecode: exec curl: %s\n", strerror(errno));
    _exit(127);
}

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

int http_stream_start(const HttpReq *req, HttpStream **out, char **err)
{
    if (!req || !req->url) {
        if (err) *err = xstrdup("missing URL");
        return -1;
    }
    HttpStream *s = xmalloc(sizeof(*s));
    memset(s, 0, sizeof(*s));
    s->fd = -1;
    s->errfd = -1;
    s->pid = -1;

    int outp[2], errp[2];
    if (pipe(outp) != 0 || pipe(errp) != 0) {
        if (err) *err = xstrdup("pipe failed");
        http_stream_free(s);
        return -1;
    }

    if (req->body && *req->body) {
        char tmpl[] = "/tmp/freecode-body-XXXXXX";
        int fd = mkstemp(tmpl);
        if (fd < 0) {
            if (err) *err = xstrdup("mkstemp failed");
            close(outp[0]); close(outp[1]); close(errp[0]); close(errp[1]);
            http_stream_free(s);
            return -1;
        }
        const char *p = req->body;
        size_t left = strlen(req->body);
        while (left) {
            ssize_t k = write(fd, p, left);
            if (k > 0) { p += k; left -= (size_t)k; }
            else if (k < 0 && errno == EINTR) continue;
            else break;
        }
        close(fd);
        s->body_path = xstrdup(tmpl);
    }

    pid_t pid = fork();
    if (pid < 0) {
        if (err) *err = xstrdup("fork failed");
        close(outp[0]); close(outp[1]); close(errp[0]); close(errp[1]);
        http_stream_free(s);
        return -1;
    }
    if (pid == 0) {
        dup2(outp[1], STDOUT_FILENO);
        dup2(errp[1], STDERR_FILENO);
        close(outp[0]); close(outp[1]); close(errp[0]); close(errp[1]);
        HttpTransport t = req->transport;
        if (t == HTTP_AUTO) {
            t = strncmp(req->url, "http://", 7) == 0 ? HTTP_DIRECT : HTTP_CURL;
        }
        if (t == HTTP_DIRECT) child_direct(req);
        else child_curl(req, s->body_path);
        _exit(2);
    }

    close(outp[1]);
    close(errp[1]);
    s->fd = outp[0];
    s->errfd = errp[0];
    s->pid = pid;
    set_nonblock(s->fd);
    set_nonblock(s->errfd);
    *out = s;
    return 0;
}

static void drain_err(HttpStream *s)
{
    char tmp[4096];
    for (;;) {
        ssize_t n = read(s->errfd, tmp, sizeof(tmp));
        if (n > 0) {
            buf_add(&s->errbuf, tmp, (size_t)n);
            continue;
        }
        if (n == 0) {
            close(s->errfd);
            s->errfd = -1;
            break;
        }
        if (errno == EINTR) continue;
        break;
    }
}

static void feed_bytes(HttpStream *s, const char *p, size_t n)
{
    buf_add(&s->pending, p, n);
    if (s->header_done) return;

    char *end = strstr(s->pending.p, "\r\n\r\n");
    size_t skip = end ? (size_t)(end + 4 - s->pending.p) : 0;
    if (!end) {
        end = strstr(s->pending.p, "\n\n");
        skip = end ? (size_t)(end + 2 - s->pending.p) : 0;
    }
    if (!end) return;

    char *nl = strchr(s->pending.p, '\n');
    if (nl) {
        char *line = xstrndup_n(s->pending.p, (size_t)(nl - s->pending.p));
        char *sp = strchr(line, ' ');
        if (sp) s->status = atoi(sp + 1);
        free(line);
    }
    if (skip < s->pending.len) {
        memmove(s->pending.p, s->pending.p + skip, s->pending.len - skip);
        s->pending.len -= skip;
        s->pending.p[s->pending.len] = '\0';
    } else {
        s->pending.len = 0;
        if (s->pending.p) s->pending.p[0] = '\0';
    }
    s->header_done = 1;
}

int http_stream_read(HttpStream *s, char *buf, size_t cap)
{
    if (!s || !buf || cap == 0) return -1;
    for (;;) {
        if (s->pending.len) {
            size_t n = s->pending.len < cap ? s->pending.len : cap;
            memcpy(buf, s->pending.p, n);
            memmove(s->pending.p, s->pending.p + n, s->pending.len - n);
            s->pending.len -= n;
            s->pending.p[s->pending.len] = '\0';
            return (int)n;
        }
        if (s->eof) return 0;

        char tmp[8192];
        ssize_t n = read(s->fd, tmp, sizeof(tmp));
        if (n > 0) {
            feed_bytes(s, tmp, (size_t)n);
            continue;
        }
        if (n == 0) {
            s->eof = 1;
            close(s->fd);
            s->fd = -1;
            continue;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        drain_err(s);
        return -1;
    }
}

int http_stream_status(HttpStream *s)
{
    return s ? s->status : 0;
}

int http_stream_fd(HttpStream *s)
{
    return s ? s->fd : -1;
}

int http_stream_eof(HttpStream *s)
{
    return s ? s->eof : 1;
}

const char *http_stream_error(HttpStream *s)
{
    if (!s) return NULL;
    return s->errbuf.p ? s->errbuf.p : NULL;
}

int http_stream_finish(HttpStream *s, int *status, char **err)
{
    if (!s) return -1;
    char tmp[8192];
    while (!s->eof) {
        ssize_t n = read(s->fd, tmp, sizeof(tmp));
        if (n > 0) feed_bytes(s, tmp, (size_t)n);
        else if (n == 0) {
            s->eof = 1;
            close(s->fd);
            s->fd = -1;
        } else if (errno == EINTR) continue;
        else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* Child may still be running; block briefly via wait below. */
            break;
        } else break;
    }
    drain_err(s);

    int wstatus = 0;
    while (waitpid(s->pid, &wstatus, 0) < 0 && errno == EINTR) {}
    s->finished = 1;
    if (status) *status = s->status;
    if (err && s->errbuf.len) *err = xstrdup(s->errbuf.p);
    if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) != 0 && !s->status) {
        if (err && !*err) *err = xstrdup("HTTP transport failed");
    }
    return 0;
}

void http_stream_free(HttpStream *s)
{
    if (!s) return;
    if (s->fd >= 0) close(s->fd);
    if (s->errfd >= 0) close(s->errfd);
    if (s->pid > 0 && !s->finished) {
        int st;
        while (waitpid(s->pid, &st, 0) < 0 && errno == EINTR) {}
    }
    if (s->body_path) {
        unlink(s->body_path);
        free(s->body_path);
    }
    buf_free(&s->pending);
    buf_free(&s->errbuf);
    free(s);
}

void http_header_add(HttpReq *req, const char *key, const char *value)
{
    if (!req || !key || !value) return;
    req->headers = xrealloc(req->headers, (req->nheaders + 1) * sizeof(HttpHeader));
    req->headers[req->nheaders].key = xstrdup(key);
    req->headers[req->nheaders].value = xstrdup(value);
    req->nheaders++;
}

void http_req_free(HttpReq *req)
{
    if (!req) return;
    free(req->method);
    free(req->url);
    for (size_t i = 0; i < req->nheaders; i++) {
        free(req->headers[i].key);
        free(req->headers[i].value);
    }
    free(req->headers);
    free(req->body);
    memset(req, 0, sizeof(*req));
}

void http_result_free(HttpResult *r)
{
    if (!r) return;
    free(r->body);
    free(r->err);
    memset(r, 0, sizeof(*r));
}

#ifdef USE_LIBCURL
static size_t curl_write(void *p, size_t size, size_t nmemb, void *ctx)
{
    size_t n = size * nmemb;
    Buf *b = ctx;
    buf_add(b, p, n);
    return n;
}

static int curl_sync(const HttpReq *req, HttpResult *out)
{
    CURL *c = curl_easy_init();
    if (!c) return -1;
    Buf body = {0};
    struct curl_slist *headers = NULL;
    char errbuf[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(c, CURLOPT_URL, req->url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS,
                     (long)(req->timeout_ms ? req->timeout_ms : 30000));
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
    if (req->body) {
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, req->body);
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)strlen(req->body));
    }
    if (req->method && strcmp(req->method, "GET") != 0)
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, req->method);
    for (size_t i = 0; i < req->nheaders; i++) {
        char *h = xmalloc(strlen(req->headers[i].key) + strlen(req->headers[i].value) + 4);
        snprintf(h, strlen(req->headers[i].key) + strlen(req->headers[i].value) + 4,
                 "%s: %s", req->headers[i].key, req->headers[i].value);
        headers = curl_slist_append(headers, h);
        free(h);
    }
    if (headers) curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) {
        out->err = xstrdup(errbuf[0] ? errbuf : curl_easy_strerror(rc));
        buf_free(&body);
        return -1;
    }
    out->status = (int)code;
    out->body = buf_take(&body);
    return 0;
}
#endif

int http_request_sync(const HttpReq *req, HttpResult *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
#ifdef USE_LIBCURL
    if (req->transport == HTTP_AUTO || req->transport == HTTP_LIBCURL)
        return curl_sync(req, out);
#endif
    HttpStream *s = NULL;
    char *err = NULL;
    if (http_stream_start(req, &s, &err) != 0) {
        out->err = err ? err : xstrdup("request failed");
        return -1;
    }
    Buf b = {0};
    char tmp[8192];
    for (;;) {
        int n = http_stream_read(s, tmp, sizeof(tmp));
        if (n > 0) {
            buf_add(&b, tmp, (size_t)n);
            continue;
        }
        if (n < 0) break;
        if (http_stream_eof(s)) break;
        int fd = http_stream_fd(s);
        if (fd < 0) continue;
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 100);
        if (pr < 0 && errno != EINTR) break;
    }
    int status = 0;
    if (http_stream_finish(s, &status, &err) == 0) {
        out->status = status;
        out->body = buf_take(&b);
        out->err = err;
    } else {
        buf_free(&b);
        out->err = err ? err : xstrdup("request failed");
    }
    http_stream_free(s);
    return out->status >= 200 && out->status < 300 ? 0 : -1;
}
