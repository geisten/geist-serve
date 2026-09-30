/* http.h — the HTTP/1.1 slice geist-serve speaks: one bounded request per
 * connection, fixed-length or chunked responses, Connection: close. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

struct conn {
    int  in, out;
    bool stdio;  /* fd 0/1: a closed stdin is normal, not a cancel */
    bool head;   /* HEAD request: headers only (the ollama CLI heartbeat) */
    bool broken; /* client went away: the decode loop must stop */
};

struct req {
    char   method[8];
    char   path[256];
    size_t body_len;
    char  *body; /* malloc'd, NUL-terminated; nullptr when body_len == 0 */
};

/* 0 on success, an HTTP status on a malformed or oversize request, or -1
 * on EOF before any byte (client closed). */
int         read_request(struct conn *c, struct req *r);
const char *http_reason(int status);
bool        client_gone(struct conn *c);

void respond(
        struct conn *c, int status, const char *content_type, size_t n, const char body[static n]);
void respond_json(struct conn *c, int status, const char *json);
void respond_error(struct conn *c, int status, const char *msg); /* OpenAI's error shape */

/* Streaming response: chunked transfer, one stream_write per event (SSE
 * for /v1, NDJSON for /api). */
bool stream_begin(struct conn *c, const char *content_type);
bool stream_write(struct conn *c, size_t n, const char data[static n]);
bool stream_end(struct conn *c);
