#ifndef FREECODE_HTTP_H
#define FREECODE_HTTP_H

#include <stddef.h>
#include <sys/types.h>

typedef enum {
    HTTP_AUTO = 0,
    HTTP_CURL = 1,
    HTTP_DIRECT = 2,
    HTTP_LIBCURL = 3
} HttpTransport;

typedef struct {
    char *key;
    char *value;
} HttpHeader;

typedef struct {
    char *method;
    char *url;
    HttpHeader *headers;
    size_t nheaders;
    char *body;
    int timeout_ms;
    HttpTransport transport;
} HttpReq;

typedef struct {
    int status;
    char *body;
    char *err;
} HttpResult;

typedef struct HttpStream HttpStream;

int http_request_sync(const HttpReq *req, HttpResult *out);
int http_stream_start(const HttpReq *req, HttpStream **out, char **err);
/* Returns body bytes: >0 bytes read, 0 EOF, -1 error. */
int http_stream_read(HttpStream *s, char *buf, size_t cap);
int http_stream_status(HttpStream *s);
int http_stream_fd(HttpStream *s);
int http_stream_eof(HttpStream *s);
const char *http_stream_error(HttpStream *s);
int http_stream_finish(HttpStream *s, int *status, char **err);
void http_stream_free(HttpStream *s);
void http_header_add(HttpReq *req, const char *key, const char *value);
void http_req_free(HttpReq *req);
void http_result_free(HttpResult *r);

#endif
