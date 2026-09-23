/* geist-serve — an Ollama- and OpenAI-compatible HTTP front for the geist
 * engine. One model, one process, one request at a time.
 *
 * Process model: a new-style foreground daemon. No fork, no pid file; logs
 * to stderr, exits on SIGTERM/SIGINT after the in-flight response. The
 * listening socket comes from systemd (LISTEN_FDS) or inetd wait-mode when
 * one is handed in, otherwise we bind --host:--port ourselves. --stdio
 * serves exactly one connection on fd 0/1, which is inetd accept-mode and
 * the test harness in one flag.
 *
 * Requests are served serially. ponytail: serial by design — a second
 * concurrent inference is a thermal problem on a Pi 5, not a throughput
 * gain; add a worker only when a client measurably blocks on it. */
#include <geist.h>
#include <geist_util.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* ====================================================================== */
/* Arguments                                                               */
/* ====================================================================== */

struct args {
    const char *model;
    const char *host;
    int         port;
    bool        stdio;
};

static int usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s <model.gguf> [--host ADDR] [--port N] [--stdio]\n"
            "  --host ADDR   bind address (default 127.0.0.1)\n"
            "  --port N      TCP port (default 11434, the Ollama port)\n"
            "  --stdio       serve one connection on stdin/stdout, then exit\n"
            "A listening socket passed via LISTEN_FDS (systemd) or on fd 0\n"
            "(inetd wait-mode) is used as-is.\n",
            argv0);
    return 2;
}

static bool parse_args(int argc, char **argv, struct args *a) {
    *a = (struct args) {.host = "127.0.0.1", .port = 11434};
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--stdio") == 0) {
            a->stdio = true;
        } else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            a->host = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            a->port = atoi(argv[++i]);
            if (a->port <= 0 || a->port > 65535)
                return false;
        } else if (argv[i][0] == '-') {
            return false;
        } else if (a->model == nullptr) {
            a->model = argv[i];
        } else {
            return false;
        }
    }
    return a->model != nullptr;
}

/* ====================================================================== */
/* HTTP/1.1 — bounded reader, chunked writer                               */
/* ====================================================================== */

/* Hard caps. A request that exceeds them is answered 431 / 413 and the
 * connection closed; nothing here grows with the client's appetite. */
#define HDR_CAP (8u * 1024u)
#define BODY_CAP (1u * 1024u * 1024u)

struct conn {
    int  in, out;
    bool broken; /* client went away: the decode loop must stop */
};

struct req {
    char        method[8];
    char        path[256];
    size_t      body_len;
    char       *body; /* malloc'd, NUL-terminated; nullptr when body_len == 0 */
    const char *content_type;
};

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

/* Reads one request. Returns 0 on success, an HTTP status on a malformed or
 * oversize request, or -1 on EOF before any byte (client closed). */
static int read_request(struct conn *c, struct req *r) {
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

static const char *reason(int status) {
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
static void
respond(struct conn *c, int status, const char *content_type, size_t n, const char body[static n]) {
    char head[512];
    int  k = snprintf(head,
                      sizeof head,
                      "HTTP/1.1 %d %s\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %zu\r\n" CORS_HEADERS "Connection: close\r\n\r\n",
                      status,
                      reason(status),
                      content_type,
                      n);
    send_all(c, (size_t) k, head);
    if (n > 0)
        send_all(c, n, body);
}

static void respond_json(struct conn *c, int status, const char *json) {
    respond(c, status, "application/json", strlen(json), json);
}

static void respond_error(struct conn *c, int status, const char *msg) {
    char json[512];
    /* msg is ours, never client text — no escaping needed here. */
    snprintf(json,
             sizeof json,
             "{\"error\":{\"message\":\"%s\",\"type\":\"%s\"}}",
             msg,
             status >= 500 ? "server_error" : "invalid_request_error");
    respond_json(c, status, json);
}

/* Streaming response: chunked transfer, one stream_write per event. The
 * endpoint issues (#5, #6) use these for SSE and NDJSON; maybe_unused
 * until then. */
[[maybe_unused]] static bool stream_begin(struct conn *c, const char *content_type) {
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

[[maybe_unused]] static bool stream_write(struct conn *c, size_t n, const char data[static n]) {
    char size[32];
    int  k = snprintf(size, sizeof size, "%zx\r\n", n);
    return send_all(c, (size_t) k, size) && send_all(c, n, data) && send_str(c, "\r\n");
}

[[maybe_unused]] static bool stream_end(struct conn *c) {
    return send_str(c, "0\r\n\r\n");
}

/* ====================================================================== */
/* Routing                                                                 */
/* ====================================================================== */

struct server {
    struct geist_backend *be;
    struct geist_model   *m;
};

static void handle(struct server *sv, struct conn *c, struct req *r) {
    (void) sv;
    if (strcmp(r->method, "OPTIONS") == 0) {
        respond(c, 204, "text/plain", 0, "");
        return;
    }
    if (strcmp(r->path, "/health") == 0) {
        respond_json(c, 200, "{\"status\":\"ok\"}");
        return;
    }
    if (strcmp(r->path, "/") == 0) {
        respond(c, 200, "text/plain", 17, "Ollama is running");
        return;
    }
    /* ponytail: the /v1 (#5) and /api (#6) routes plug in here. */
    respond_error(c, 404, "no such endpoint");
}

/* One connection, one request; Connection: close after every response is
 * the v1 contract (keep-alive buys nothing on a serial server). */
static void serve_conn(struct server *sv, int in, int out) {
    struct conn c = {.in = in, .out = out};
    struct req  r;
    int         st = read_request(&c, &r);
    if (st == -1)
        return;
    if (st != 0) {
        respond_error(&c, st, reason(st));
    } else {
        handle(sv, &c, &r);
    }
    free(r.body);
}

/* ====================================================================== */
/* Listener acquisition                                                    */
/* ====================================================================== */

static volatile sig_atomic_t stop_requested = 0;

static void on_stop(int sig) {
    (void) sig;
    stop_requested = 1;
}

/* SO_ACCEPTCONN is unreliable on macOS, so probe the way that works
 * everywhere: listen() on a listening socket succeeds (it only resets the
 * backlog); on a pipe, tty or connected socket it fails. */
static bool is_listener(int fd) {
    struct sockaddr_storage ss;
    socklen_t               l = sizeof ss;
    return getsockname(fd, (struct sockaddr *) &ss, &l) == 0 && listen(fd, 16) == 0;
}

/* systemd passes listeners starting at fd 3; inetd wait-mode passes the
 * listener as fd 0. Both are "use as-is". */
static int inherited_listener(void) {
    const char *n = getenv("LISTEN_FDS");
    if (n != nullptr && atoi(n) >= 1 && is_listener(3))
        return 3;
    if (is_listener(0))
        return 0;
    return -1;
}

static int bind_listener(const char *host, int port) {
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t) port)};
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        fprintf(stderr, "geist-serve: --host %s: not an IPv4 address\n", host);
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(fd, (struct sockaddr *) &sa, sizeof sa) != 0 || listen(fd, 16) != 0) {
        fprintf(stderr, "geist-serve: bind %s:%d: %s\n", host, port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static void accept_loop(struct server *sv, int lfd) {
    while (!stop_requested) {
        int fd = accept(lfd, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "geist-serve: accept: %s\n", strerror(errno));
            break;
        }
        /* A stalled client must not hold the one serving thread forever. */
        struct timeval tv = {.tv_sec = 30};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        serve_conn(sv, fd, fd);
        close(fd);
    }
}

/* ====================================================================== */
/* main                                                                    */
/* ====================================================================== */

int main(int argc, char **argv) {
    struct args a;
    if (!parse_args(argc, argv, &a))
        return usage(argv[0]);

    /* Decide the transport before the multi-second model load, so a bad
     * --host or a busy port fails in milliseconds. */
    int lfd = -1;
    if (!a.stdio) {
        lfd = inherited_listener();
        if (lfd < 0)
            lfd = bind_listener(a.host, a.port);
        if (lfd < 0)
            return 1;
    }

    struct server sv = {};
    if (geist_backend_create("auto", nullptr, nullptr, &sv.be) != GEIST_OK) {
        fprintf(stderr, "backend: %s\n", sv.be ? geist_backend_errmsg(sv.be) : "create failed");
        return 1;
    }
    if (geist_model_load(a.model, sv.be, &sv.m) != GEIST_OK) {
        fprintf(stderr, "model: %s\n", sv.m ? geist_model_errmsg(sv.m) : "load failed");
        geist_backend_destroy(sv.be);
        return 1;
    }
    fprintf(stderr,
            "geist-serve: loaded %s (%s) on %s\n",
            a.model,
            geist_model_arch(sv.m),
            geist_backend_name(sv.be));

    /* EPIPE reaches us as a write error (conn.broken), not a signal. No
     * SA_RESTART: accept() must return EINTR so the loop sees the stop. */
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa = {.sa_handler = on_stop};
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);

    if (a.stdio) {
        serve_conn(&sv, STDIN_FILENO, STDOUT_FILENO);
    } else {
        fprintf(stderr,
                "geist-serve: listening (%s)\n",
                lfd == 3 || lfd == 0 ? "inherited socket" : "own socket");
        accept_loop(&sv, lfd);
        close(lfd);
        fprintf(stderr, "geist-serve: stopped\n");
    }

    geist_model_destroy(sv.m);
    geist_backend_destroy(sv.be);
    return 0;
}
