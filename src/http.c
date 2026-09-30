/* http.c — see http.h. */
#include "http.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

/* Hard caps. A request that exceeds them is answered 431 / 413 and the
 * connection closed; nothing here grows with the client's appetite. */
#define HDR_CAP (8u * 1024u)
#define BODY_CAP (1u * 1024u * 1024u)

static bool send_all(struct conn *c, size_t n, const char buf[static n]) {
    while (n > 0 && !c->broken) {
        ssize_t w = write(c->out, buf, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            c->broken = true;
            return false;
        }
        buf += w;
        n -= (size_t) w;
    }
    return !c->broken;
}

static bool send_str(struct conn *c, const char *s) {
    return send_all(c, strlen(s), s);
}

/* A non-stream request writes nothing until the end, so a client that hung
 * up would otherwise keep the model busy for the whole token budget. A
 * non-blocking peek tells: 0 = EOF (gone), EAGAIN = nothing to read (fine),
 * other errors = gone. Not poll(): macOS reports no event for a half-closed
 * loopback socket. Skipped on --stdio, where stdin closes after the request. */
bool client_gone(struct conn *c) {
    if (c->broken || c->stdio)
        return c->broken;
    char    b;
    ssize_t n = recv(c->in, &b, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n > 0 || (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)))
        return false;
    c->broken = true;
    fprintf(stderr, "geist-serve: client gone, aborting generation\n");
    return true;
}

int read_request(struct conn *c, struct req *r) {
    static char hdr[HDR_CAP + 1];
    size_t      got = 0;
    char       *end = nullptr;

    memset(r, 0, sizeof *r);
    for (;;) {
        if (got == HDR_CAP)
            return 431;
        ssize_t n = read(c->in, hdr + got, HDR_CAP - got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return got == 0 ? -1 : 400;
        got += (size_t) n;
        hdr[got] = '\0';
        end      = strstr(hdr, "\r\n\r\n");
        if (end != nullptr)
            break;
    }
    const size_t hdr_len = (size_t) (end + 4 - hdr);

    /* Request line: METHOD SP PATH[?query] SP HTTP/1.x */
    if (sscanf(hdr, "%7s %255s", r->method, r->path) != 2)
        return 400;
    char *q = strchr(r->path, '?');
    if (q != nullptr)
        *q = '\0';

    /* Headers we care about; the rest are ignored. */
    size_t content_length = 0;
    bool   expect_100     = false;
    for (char *line = strstr(hdr, "\r\n") + 2; line < end;) {
        char *eol = strstr(line, "\r\n");
        *eol      = '\0';
        if (strncasecmp(line, "Content-Length:", 15) == 0) {
            char *num            = line + 15;
            errno                = 0;
            unsigned long long v = strtoull(num, nullptr, 10);
            if (errno != 0)
                return 400;
            if (v > BODY_CAP)
                return 413;
            content_length = (size_t) v;
        } else if (strncasecmp(line, "Transfer-Encoding:", 18) == 0) {
            return 411; /* chunked request bodies are not supported */
        } else if (strncasecmp(line, "Expect:", 7) == 0 && strstr(line, "100-continue")) {
            expect_100 = true; /* curl sends this for bodies > 1 KiB and waits 1 s otherwise */
        }
        line = eol + 2;
    }

    if (content_length > 0) {
        r->body = malloc(content_length + 1);
        if (r->body == nullptr)
            return 500;
        size_t have = got - hdr_len;
        if (have > content_length)
            have = content_length;
        memcpy(r->body, hdr + hdr_len, have);
        if (expect_100 && have < content_length)
            send_str(c, "HTTP/1.1 100 Continue\r\n\r\n");
        while (have < content_length) {
            ssize_t n = read(c->in, r->body + have, content_length - have);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0) {
                free(r->body);
                r->body = nullptr;
                return 400;
            }
            have += (size_t) n;
        }
        r->body[content_length] = '\0';
        r->body_len             = content_length;
    }
    return 0;
}

const char *http_reason(int status) {
    switch (status) {
    case 200:
        return "OK";
    case 204:
        return "No Content";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 411:
        return "Length Required";
    case 413:
        return "Payload Too Large";
    case 431:
        return "Request Header Fields Too Large";
    case 500:
        return "Internal Server Error";
    case 501:
        return "Not Implemented";
    default:
        return "";
    }
}

/* CORS is permissive on purpose: browser clients (Open WebUI) talk to
 * 127.0.0.1 directly, and there is nothing to protect on a loopback
 * inference socket beyond what --host already decides. */
#define CORS_HEADERS                                       \
    "Access-Control-Allow-Origin: *\r\n"                   \
    "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n" \
    "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"

/* Fixed-length response: status line, headers, body, done. */
void respond(
        struct conn *c, int status, const char *content_type, size_t n, const char body[static n]) {
    char head[512];
    int  k = snprintf(head,
                      sizeof head,
                      "HTTP/1.1 %d %s\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %zu\r\n" CORS_HEADERS "Connection: close\r\n\r\n",
                      status,
                      http_reason(status),
                      content_type,
                      n);
    send_all(c, (size_t) k, head);
    if (n > 0 && !c->head)
        send_all(c, n, body);
}

void respond_json(struct conn *c, int status, const char *json) {
    respond(c, status, "application/json", strlen(json), json);
}

void respond_error(struct conn *c, int status, const char *msg) {
    char json[512];
    /* msg is ours, never client text — no escaping needed here. */
    snprintf(json,
             sizeof json,
             "{\"error\":{\"message\":\"%s\",\"type\":\"%s\"}}",
             msg,
             status >= 500 ? "server_error" : "invalid_request_error");
    respond_json(c, status, json);
}

bool stream_begin(struct conn *c, const char *content_type) {
    char head[512];
    int  k = snprintf(head,
                      sizeof head,
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: %s\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "Cache-Control: no-cache\r\n" CORS_HEADERS "Connection: close\r\n\r\n",
                      content_type);
    return send_all(c, (size_t) k, head);
}

bool stream_write(struct conn *c, size_t n, const char data[static n]) {
    char size[32];
    int  k = snprintf(size, sizeof size, "%zx\r\n", n);
    return send_all(c, (size_t) k, size) && send_all(c, n, data) && send_str(c, "\r\n");
}

bool stream_end(struct conn *c) {
    return send_str(c, "0\r\n\r\n");
}
