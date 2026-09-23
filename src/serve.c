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

#define JSMN_STATIC
#define JSMN_STRICT
#define JSMN_PARENT_LINKS
#include "jsmn.h" /* MIT, Serge Zaitsev — vendored, same copy as geistshell */
#include "template.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <libgen.h>
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
    bool stdio;  /* fd 0/1: a closed stdin is normal, not a cancel */
    bool head;   /* HEAD request: headers only (the ollama CLI heartbeat) */
    bool broken; /* client went away: the decode loop must stop */
};

struct req {
    char        method[8];
    char        path[256];
    size_t      body_len;
    char       *body; /* malloc'd, NUL-terminated; nullptr when body_len == 0 */
    const char *content_type;
};

struct server {
    struct geist_backend *be;
    struct geist_model   *m;
    char                  name[128]; /* GGUF basename without .gguf */
    geist_token_t         eos;
    geist_token_t         eot[6]; /* end-of-turn tokens by family */
    int                   n_eot;
    enum chat_family      family;
    struct gguf_meta      meta; /* template, arch, size label, file type */
    const char           *path; /* the GGUF as given on the command line */
    off_t                 file_size;
    time_t                file_mtime;
    bool                  add_bos; /* tokenizer prepends BOS in set_prompt */
    struct geist_session *tok;     /* tiny session kept for tokenize-only counting */
    time_t                loaded_at;
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

static void iso_time(time_t t, char out[static 40]);

static bool send_str(struct conn *c, const char *s) {
    return send_all(c, strlen(s), s);
}

/* A non-stream request writes nothing until the end, so a client that hung
 * up would otherwise keep the model busy for the whole token budget. A
 * non-blocking peek tells: 0 = EOF (gone), EAGAIN = nothing to read (fine),
 * other errors = gone. Not poll(): macOS reports no event for a half-closed
 * loopback socket. Skipped on --stdio, where stdin closes after the request. */
static bool client_gone(struct conn *c) {
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
    if (n > 0 && !c->head)
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

/* Streaming response: chunked transfer, one stream_write per event (SSE
 * for /v1, NDJSON for /api). */
static bool stream_begin(struct conn *c, const char *content_type) {
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

static bool stream_write(struct conn *c, size_t n, const char data[static n]) {
    char size[32];
    int  k = snprintf(size, sizeof size, "%zx\r\n", n);
    return send_all(c, (size_t) k, size) && send_all(c, n, data) && send_str(c, "\r\n");
}

static bool stream_end(struct conn *c) {
    return send_str(c, "0\r\n\r\n");
}

/* ====================================================================== */
/* Small string buffer + JSON writer                                       */
/* ====================================================================== */

struct sb {
    char  *p;
    size_t len, cap;
};

static void sb_put(struct sb *b, size_t n, const char s[static n]) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n + 1)
            cap *= 2;
        char *np = realloc(b->p, cap);
        if (np == nullptr)
            return; /* dropped write; response ends short */
        b->p   = np;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void sb_puts(struct sb *b, const char *s) {
    sb_put(b, strlen(s), s);
}

static void sb_printf(struct sb *b, const char *fmt, ...) {
    char    tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (k > 0)
        sb_put(b, (size_t) k < sizeof tmp ? (size_t) k : sizeof tmp - 1, tmp);
}

/* JSON string literal, quotes included. Bytes are emitted as-is except the
 * JSON escapes; the generation loop only hands us complete UTF-8. */
static void sb_json_str(struct sb *b, size_t n, const char s[static n]) {
    sb_put(b, 1, "\"");
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char) s[i];
        switch (c) {
        case '"':
            sb_puts(b, "\\\"");
            break;
        case '\\':
            sb_puts(b, "\\\\");
            break;
        case '\n':
            sb_puts(b, "\\n");
            break;
        case '\r':
            sb_puts(b, "\\r");
            break;
        case '\t':
            sb_puts(b, "\\t");
            break;
        default:
            if (c < 0x20)
                sb_printf(b, "\\u%04x", c);
            else
                sb_put(b, 1, (const char *) &c);
        }
    }
    sb_put(b, 1, "\"");
}

static void sb_free(struct sb *b) {
    free(b->p);
    *b = (struct sb) {};
}

/* ====================================================================== */
/* JSON reader (jsmn) — lookup helpers over a parsed request body           */
/* ====================================================================== */

#define JSON_TOK_CAP 4096

struct json {
    const char *src;
    jsmntok_t   tok[JSON_TOK_CAP];
    int         n;
};

/* Returns the token count, or -1 on malformed / too large. */
static int json_parse(struct json *j, size_t n, const char src[static n]) {
    jsmn_parser p;
    jsmn_init(&p);
    j->src = src;
    j->n   = jsmn_parse(&p, src, n, j->tok, JSON_TOK_CAP);
    if (j->n < 1 || j->tok[0].type != JSMN_OBJECT)
        j->n = -1;
    /* Anything after the root object ("{...}xxx") is a second top-level
     * token with no parent: refuse it rather than run the request. */
    for (int i = 1; i < j->n; i++)
        if (j->tok[i].parent < 0)
            j->n = -1;
    return j->n;
}

/* Direct child `key` of object `obj`; -1 when absent. Only direct children
 * match (parent links), so "options.stop" needs two hops. */
static int json_get(const struct json *j, int obj, const char *key) {
    if (obj < 0 || j->tok[obj].type != JSMN_OBJECT)
        return -1;
    size_t kl = strlen(key);
    for (int i = obj + 1; i < j->n; i++) {
        const jsmntok_t *t = &j->tok[i];
        if (t->parent != obj)
            continue;
        if (t->type == JSMN_STRING && (size_t) (t->end - t->start) == kl &&
            memcmp(j->src + t->start, key, kl) == 0 && i + 1 < j->n)
            return i + 1;
    }
    return -1;
}

static bool json_is_str(const struct json *j, int t) {
    return t >= 0 && j->tok[t].type == JSMN_STRING;
}

/* Unescape a JSON string token into a fresh malloc'd buffer. \uXXXX is
 * decoded to UTF-8 (surrogate pairs included); unknown escapes are copied. */
static char *json_strdup(const struct json *j, int t) {
    if (!json_is_str(j, t))
        return nullptr;
    const char *s   = j->src + j->tok[t].start;
    size_t      n   = (size_t) (j->tok[t].end - j->tok[t].start);
    char       *out = malloc(n + 1);
    if (out == nullptr)
        return nullptr;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '\\' || i + 1 >= n) {
            out[o++] = s[i];
            continue;
        }
        char e = s[++i];
        switch (e) {
        case 'n':
            out[o++] = '\n';
            break;
        case 't':
            out[o++] = '\t';
            break;
        case 'r':
            out[o++] = '\r';
            break;
        case 'b':
            out[o++] = '\b';
            break;
        case 'f':
            out[o++] = '\f';
            break;
        case 'u': {
            if (i + 4 >= n)
                break;
            unsigned cp = (unsigned) strtoul(
                    (char[]) {s[i + 1], s[i + 2], s[i + 3], s[i + 4], 0}, nullptr, 16);
            i += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < n && s[i + 1] == '\\' && s[i + 2] == 'u') {
                unsigned lo = (unsigned) strtoul(
                        (char[]) {s[i + 3], s[i + 4], s[i + 5], s[i + 6], 0}, nullptr, 16);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 6;
                }
            }
            if (cp < 0x80) {
                out[o++] = (char) cp;
            } else if (cp < 0x800) {
                out[o++] = (char) (0xC0 | (cp >> 6));
                out[o++] = (char) (0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                out[o++] = (char) (0xE0 | (cp >> 12));
                out[o++] = (char) (0x80 | ((cp >> 6) & 0x3F));
                out[o++] = (char) (0x80 | (cp & 0x3F));
            } else {
                out[o++] = (char) (0xF0 | (cp >> 18));
                out[o++] = (char) (0x80 | ((cp >> 12) & 0x3F));
                out[o++] = (char) (0x80 | ((cp >> 6) & 0x3F));
                out[o++] = (char) (0x80 | (cp & 0x3F));
            }
            break;
        }
        default:
            out[o++] = e; /* \" \\ \/ */
        }
    }
    out[o] = '\0';
    return out;
}

static double json_num(const struct json *j, int t, double dflt) {
    if (t < 0 || j->tok[t].type != JSMN_PRIMITIVE)
        return dflt;
    char c = j->src[j->tok[t].start];
    if (c != '-' && (c < '0' || c > '9'))
        return dflt;
    return strtod(j->src + j->tok[t].start, nullptr);
}

/* Number clamped to [lo, hi]; absent or NaN → dflt. A present field that is
 * not a number sets *bad: ignoring a string max_tokens would silently turn
 * a client bug into a 4096-token run, so the request is refused instead. */
static double
json_clamp(const struct json *j, int t, double dflt, double lo, double hi, bool *bad) {
    if (t >= 0 && (j->tok[t].type != JSMN_PRIMITIVE || j->src[j->tok[t].start] == 't' ||
                   j->src[j->tok[t].start] == 'f'))
        *bad = true;
    double v = json_num(j, t, dflt);
    if (isnan(v))
        v = dflt;
    return v < lo ? lo : v > hi ? hi : v;
}

static bool json_bool(const struct json *j, int t, bool dflt) {
    if (t < 0 || j->tok[t].type != JSMN_PRIMITIVE)
        return dflt;
    return j->src[j->tok[t].start] == 't';
}

/* ====================================================================== */
/* Generation — the one place that talks to libgeist                       */
/* ====================================================================== */

/* The engine caps every session at 4096 tokens today (geistlib#428). */
#define CTX_CAP 4096
/* Bytes of prompt text worth tokenizing at all: 4096 tokens of any script
 * fit in 32 KiB, and tokenizer time on a 1 MiB "word" is a DoS, not a
 * request (measured: 32 KB single-word = 5 s just to be refused). */
#define PROMPT_BYTES_CAP (32u * 1024u)
#define STOP_MAX 8
#define STOP_LEN 64

struct gen_opts {
    float    temperature, top_p;
    int      top_k;
    uint64_t seed;
    int      max_tokens; /* <= 0: fill the context */
    int      n_stop;
    char     stop[STOP_MAX][STOP_LEN];
};

/* Ollama's defaults; every editor client that sends nothing gets a
 * non-greedy chat. Engine note: with both top_k > 1 and top_p < 1 set,
 * the engine applies top_k and ignores top_p. */
static struct gen_opts gen_opts_default(void) {
    return (struct gen_opts) {.temperature = 0.7f, .top_p = 0.9f, .top_k = 40, .seed = 0};
}

struct gen_result {
    int         prompt_tokens, completion_tokens;
    const char *finish_reason; /* "stop" | "length" */
    uint64_t    prefill_ns, decode_ns;
};

/* Called with complete UTF-8 text as it is decoded; return false to stop
 * (the client hung up). */
typedef bool (*emit_fn)(void *ctx, size_t n, const char text[static n]);

/* Text pending emission: held back for the longest partial stop-string
 * match and for an incomplete trailing UTF-8 sequence. */
struct holdback {
    char   buf[1024];
    size_t len;
};

/* Longest prefix of buf[0..n) that ends on a complete UTF-8 sequence. A
 * byte-fallback token is itself a fragment of one, so "back up to a lead
 * byte" is not enough: the lead byte's sequence must also be complete. */
static size_t utf8_complete(size_t n, const unsigned char buf[static n]) {
    size_t p = n;
    while (p > 0 && n - p < 4 && (buf[p - 1] & 0xC0) == 0x80)
        p--; /* skip continuations */
    if (p == 0)
        return 0;
    unsigned char lead = buf[p - 1];
    size_t        need = lead < 0x80             ? 1
                         : (lead & 0xE0) == 0xC0 ? 2
                         : (lead & 0xF0) == 0xE0 ? 3
                         : (lead & 0xF8) == 0xF0 ? 4
                                                 : 1;
    return (p - 1) + need <= n ? n : p - 1;
}

static bool hb_flush(struct holdback *h, size_t keep, emit_fn emit, void *ctx) {
    if (h->len <= keep)
        return true;
    size_t cut = utf8_complete(h->len - keep, (const unsigned char *) h->buf);
    if (cut == 0)
        return true;
    if (!emit(ctx, cut, h->buf))
        return false;
    memmove(h->buf, h->buf + cut, h->len - cut);
    h->len -= cut;
    return true;
}

/* Returns 0, or an HTTP status: 400 prompt does not fit, 500 engine error. */
static int generate(struct server         *sv,
                    struct conn           *c,
                    const struct gen_opts *o,
                    const char            *prompt,
                    emit_fn                emit,
                    void                  *ctx,
                    struct gen_result     *res,
                    char                   err[static 256]) {
    *res = (struct gen_result) {.finish_reason = "stop"};

    struct geist_session_opts so = {
            .max_seq_len = CTX_CAP,
            .temperature = o->temperature,
            .top_p       = o->top_p,
            .top_k       = o->top_k,
            .random_seed = o->seed ? o->seed : (uint64_t) time(nullptr) ^ (uint64_t) clock(),
    };
    struct geist_session *s = nullptr;
    if (geist_session_create(sv->m, sv->be, &so, &s) != GEIST_OK) {
        snprintf(err, 256, "session: %s", s ? geist_session_errmsg(s) : "create failed");
        if (s)
            geist_session_destroy(s);
        return 500;
    }

    if (prompt[0] == '\0') {
        snprintf(err, 256, "prompt is empty");
        geist_session_destroy(s);
        return 400;
    }
    if (strlen(prompt) > PROMPT_BYTES_CAP) {
        snprintf(err, 256, "prompt exceeds %u bytes", PROMPT_BYTES_CAP);
        geist_session_destroy(s);
        return 400;
    }
    /* Token budget: tokenize() reports content tokens; set_prompt adds BOS
     * when the tokenizer says so. */
    static geist_token_t ids[CTX_CAP];
    size_t               n_ids = 0;
    if (geist_session_tokenize(s, prompt, CTX_CAP, ids, &n_ids) != GEIST_OK ||
        n_ids + 1 >= CTX_CAP) {
        snprintf(err, 256, "prompt does not fit the %d-token context", CTX_CAP);
        geist_session_destroy(s);
        return 400;
    }
    res->prompt_tokens = (int) n_ids + (sv->add_bos ? 1 : 0);
    int budget         = CTX_CAP - res->prompt_tokens;
    int max_tokens     = (o->max_tokens > 0 && o->max_tokens < budget) ? o->max_tokens : budget;

    if (geist_session_set_prompt(s, prompt) != GEIST_OK) {
        snprintf(err, 256, "prefill: %s", geist_session_errmsg(s));
        geist_session_destroy(s);
        return 500;
    }

    size_t keep = 0;
    for (int i = 0; i < o->n_stop; i++) {
        size_t l = strlen(o->stop[i]);
        if (l > 0 && l - 1 > keep)
            keep = l - 1;
    }

    struct holdback hb   = {};
    bool            more = true;
    for (int n = 0; n < max_tokens && more; n++) {
        if ((n & 7) == 7 && client_gone(c))
            break;
        geist_token_t t;
        if (geist_session_decode_step(s, &t) != GEIST_OK) {
            snprintf(err, 256, "decode: %s", geist_session_errmsg(s));
            geist_session_destroy(s);
            return 500;
        }
        if (t == sv->eos)
            break;
        bool eot = false;
        for (int k = 0; k < sv->n_eot; k++)
            eot |= (t == sv->eot[k]);
        if (eot)
            break;

        res->completion_tokens++;
        const char *piece = geist_session_token_to_str(s, t);
        if (piece == nullptr)
            continue; /* control token */
        size_t pl = strlen(piece);
        if (hb.len + pl >= sizeof hb.buf)
            more = hb_flush(&hb, 0, emit, ctx);
        if (pl >= sizeof hb.buf)
            continue; /* absurd token; drop it */
        memcpy(hb.buf + hb.len, piece, pl);
        hb.len += pl;
        hb.buf[hb.len] = '\0';

        for (int k = 0; k < o->n_stop; k++) {
            char *at = o->stop[k][0] ? strstr(hb.buf, o->stop[k]) : nullptr;
            if (at != nullptr) {
                hb.len = (size_t) (at - hb.buf);
                more   = false;
                keep   = 0;
                break;
            }
        }
        if (more)
            more = hb_flush(&hb, keep, emit, ctx);
        if (n + 1 == max_tokens)
            res->finish_reason = "length";
    }
    hb_flush(&hb, 0, emit, ctx);

    struct geist_session_stats st = {};
    geist_session_get_stats(s, &st);
    res->prefill_ns = st.total_prefill_ns;
    res->decode_ns  = st.total_decode_ns;
    geist_session_destroy(s);
    return 0;
}

/* Sampling fields shared by the OpenAI and Ollama request shapes: `obj` is
 * the object that carries them (the request itself, or Ollama's options). */
static bool gen_opts_from_json(struct gen_opts *o, const struct json *j, int obj) {
    bool bad = false;
    o->temperature =
            (float) json_clamp(j, json_get(j, obj, "temperature"), o->temperature, 0, 2, &bad);
    o->top_p = (float) json_clamp(j, json_get(j, obj, "top_p"), o->top_p, 0.01, 1, &bad);
    o->top_k = (int) json_clamp(j, json_get(j, obj, "top_k"), o->top_k, 0, 1000, &bad);
    o->seed  = (uint64_t) json_clamp(
            j, json_get(j, obj, "seed"), (double) o->seed, 0, 9007199254740992.0, &bad);
    int stop = json_get(j, obj, "stop");
    if (json_is_str(j, stop)) {
        char *s = json_strdup(j, stop);
        if (s)
            snprintf(o->stop[o->n_stop++], STOP_LEN, "%s", s);
        free(s);
    } else if (stop >= 0 && j->tok[stop].type == JSMN_ARRAY) {
        for (int i = stop + 1; i < j->n && o->n_stop < STOP_MAX; i++) {
            if (j->tok[i].parent != stop)
                continue;
            char *s = json_strdup(j, i);
            if (s && s[0])
                snprintf(o->stop[o->n_stop++], STOP_LEN, "%s", s);
            free(s);
        }
    } else if (stop >= 0 && j->tok[stop].type != JSMN_PRIMITIVE) {
        bad = true; /* null is fine, an object is not */
    }
    return !bad;
}

/* ====================================================================== */
/* Routing                                                                 */
/* ====================================================================== */

/* ---- OpenAI /v1/completions — the first consumer of generate() ---------- */

struct sse_ctx {
    struct conn *c;
    const char  *model;
    const char  *id;
    time_t       created;
    struct sb   *text; /* non-stream: collect; stream: nullptr */
    enum emit_kind { EMIT_OAI_TEXT, EMIT_OAI_CHAT, EMIT_OLLAMA_GEN, EMIT_OLLAMA_CHAT } kind;
};

static bool emit_completion(void *vctx, size_t n, const char text[static n]) {
    struct sse_ctx *x = vctx;
    if (x->text != nullptr) {
        sb_put(x->text, n, text);
        return true;
    }
    struct sb ev = {};
    if (x->kind == EMIT_OLLAMA_GEN || x->kind == EMIT_OLLAMA_CHAT) {
        char now[40];
        iso_time(time(nullptr), now);
        sb_printf(&ev, "{\"model\":\"%s\",\"created_at\":\"%s\",", x->model, now);
        sb_puts(&ev,
                x->kind == EMIT_OLLAMA_CHAT ? "\"message\":{\"role\":\"assistant\",\"content\":"
                                            : "\"response\":");
        sb_json_str(&ev, n, text);
        sb_puts(&ev, x->kind == EMIT_OLLAMA_CHAT ? "},\"done\":false}\n" : ",\"done\":false}\n");
    } else {
        bool chat = x->kind == EMIT_OAI_CHAT;
        sb_printf(&ev,
                  "data: "
                  "{\"id\":\"%s\",\"object\":\"%s\",\"created\":%lld,\"model\":\"%s\",\"choices\":["
                  "{\"index\":0,",
                  x->id,
                  chat ? "chat.completion.chunk" : "text_completion",
                  (long long) x->created,
                  x->model);
        sb_puts(&ev, chat ? "\"delta\":{\"content\":" : "\"text\":");
        sb_json_str(&ev, n, text);
        sb_puts(&ev, chat ? "},\"finish_reason\":null}]}\n\n" : ",\"finish_reason\":null}]}\n\n");
    }
    bool ok = stream_write(x->c, ev.len, ev.p);
    sb_free(&ev);
    return ok;
}

/* The served model's name, with or without Ollama's ":latest"; absent is
 * fine (Cursor sends whatever was typed into its model box). */
static bool model_name_ok(const struct server *sv, const struct json *j, int obj) {
    int t = json_get(j, obj, "model");
    if (t < 0)
        return true;
    char *m = json_strdup(j, t);
    if (m == nullptr)
        return false;
    char *colon = strstr(m, ":latest");
    if (colon != nullptr && colon[7] == '\0')
        *colon = '\0';
    bool ok = strcmp(m, sv->name) == 0;
    free(m);
    return ok;
}

/* Token count of a rendered prompt for chat_render_fit, via the session
 * kept for that purpose. Over the byte cap counts as "does not fit". */
static size_t count_tokens(void *vsv, const char *text) {
    struct server *sv = vsv;
    if (strlen(text) > PROMPT_BYTES_CAP)
        return CTX_CAP + 1;
    static geist_token_t ids[CTX_CAP];
    size_t               n = 0;
    if (geist_session_tokenize(sv->tok, text, CTX_CAP, ids, &n) != GEIST_OK)
        return CTX_CAP + 1;
    return n + (sv->add_bos ? 1 : 0);
}

/* messages[] → chat_msg[]. content is a string, or an array of parts of
 * which the {"type":"text"} ones are concatenated (image parts ignored:
 * no vision in v1). All strings are malloc'd; free with free_messages. */
static int parse_messages(
        const struct json *j, int arr, size_t cap, struct chat_msg out[static cap], size_t *n_out) {
    *n_out = 0;
    if (arr < 0 || j->tok[arr].type != JSMN_ARRAY)
        return 400;
    for (int i = arr + 1; i < j->n; i++) {
        if (j->tok[i].parent != arr)
            continue;
        if (j->tok[i].type != JSMN_OBJECT || *n_out == cap)
            return 400;
        char *role    = json_strdup(j, json_get(j, i, "role"));
        int   ct      = json_get(j, i, "content");
        char *content = nullptr;
        if (json_is_str(j, ct)) {
            content = json_strdup(j, ct);
        } else if (ct >= 0 && j->tok[ct].type == JSMN_ARRAY) {
            struct sb b = {};
            for (int k = ct + 1; k < j->n; k++) {
                if (j->tok[k].parent != ct)
                    continue;
                char *txt = json_strdup(j, json_get(j, k, "text"));
                if (txt)
                    sb_puts(&b, txt);
                free(txt);
            }
            content = b.p ? b.p : strdup("");
        } else if (ct >= 0 && j->tok[ct].type == JSMN_PRIMITIVE &&
                   j->src[j->tok[ct].start] == 'n') {
            content = strdup(""); /* null content (tool-call turns) */
        }
        if (role == nullptr || content == nullptr) {
            free(role);
            free(content);
            return 400;
        }
        out[(*n_out)++] = (struct chat_msg) {.role = role, .content = content};
    }
    return 0;
}

static void free_messages(size_t n, const struct chat_msg msgs[]) {
    for (size_t i = 0; i < n; i++) {
        free((char *) msgs[i].role);
        free((char *) msgs[i].content);
    }
}

#define MSG_CAP 256

/* ---- OpenAI /v1/chat/completions ------------------------------------------ */

static void route_v1_chat(struct server *sv, struct conn *c, struct req *r) {
    struct json j;
    if (r->body_len == 0 || json_parse(&j, r->body_len, r->body) < 0) {
        respond_error(c, 400, "body is not a JSON object");
        return;
    }
    if (!model_name_ok(sv, &j, 0)) {
        respond_error(c, 404, "model not found; GET /v1/models lists the one served");
        return;
    }
    if (sv->family == CHAT_UNKNOWN) {
        respond_error(c, 501, "no chat template known for this model; use /v1/completions");
        return;
    }
    struct chat_msg msgs[MSG_CAP];
    size_t          n_msgs = 0;
    if (parse_messages(&j, json_get(&j, 0, "messages"), MSG_CAP, msgs, &n_msgs) != 0 ||
        n_msgs == 0) {
        free_messages(n_msgs, msgs);
        respond_error(c, 400, "messages must be a non-empty array of {role, content}");
        return;
    }
    struct gen_opts o   = gen_opts_default();
    bool            ok  = gen_opts_from_json(&o, &j, 0);
    bool            bad = false;
    int             mt  = json_get(&j, 0, "max_completion_tokens");
    if (mt < 0)
        mt = json_get(&j, 0, "max_tokens");
    o.max_tokens = (int) json_clamp(&j, mt, 0, 0, CTX_CAP, &bad);
    bool stream  = json_bool(&j, json_get(&j, 0, "stream"), false);
    if (!ok || bad) {
        free_messages(n_msgs, msgs);
        respond_error(c, 400, "a numeric field has the wrong type");
        return;
    }

    /* Fit: keep room for the requested reply length, else a 512-token
     * reserve — enough for an answer, small enough to keep context. */
    size_t reserve = o.max_tokens > 0 ? (size_t) o.max_tokens : 512;
    if (reserve > CTX_CAP / 2)
        reserve = CTX_CAP / 2;
    size_t prompt_tokens = 0;
    char  *prompt        = chat_render_fit(
            sv->family, n_msgs, msgs, CTX_CAP, reserve, count_tokens, sv, &prompt_tokens);
    free_messages(n_msgs, msgs);
    if (prompt == nullptr) {
        respond_error(c, 400, "the last message alone does not fit the context");
        return;
    }

    char id[40];
    snprintf(id,
             sizeof id,
             "chatcmpl-%llx",
             (unsigned long long) time(nullptr) ^ (unsigned long long) clock());
    struct sb      text = {};
    struct sse_ctx x    = {.c       = c,
                           .model   = sv->name,
                           .id      = id,
                           .created = time(nullptr),
                           .text    = stream ? nullptr : &text,
                           .kind    = EMIT_OAI_CHAT};
    if (stream) {
        if (!stream_begin(c, "text/event-stream")) {
            free(prompt);
            return;
        }
        /* First chunk carries the role, as OpenAI does. */
        struct sb ev = {};
        sb_printf(&ev,
                  "data: "
                  "{\"id\":\"%s\",\"object\":\"chat.completion.chunk\",\"created\":%lld,\"model\":"
                  "\"%s\","
                  "\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"\"},"
                  "\"finish_reason\":null}]}\n\n",
                  id,
                  (long long) x.created,
                  sv->name);
        stream_write(c, ev.len, ev.p);
        sb_free(&ev);
    }

    struct gen_result res;
    char              err[256];
    int               st = generate(sv, c, &o, prompt, emit_completion, &x, &res, err);
    free(prompt);
    if (st != 0) {
        if (stream)
            stream_end(c);
        else
            respond_error(c, st, err);
        sb_free(&text);
        return;
    }
    if (stream) {
        struct sb ev = {};
        sb_printf(
                &ev,
                "data: "
                "{\"id\":\"%s\",\"object\":\"chat.completion.chunk\",\"created\":%lld,\"model\":\"%"
                "s\","
                "\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"%s\"}],"
                "\"usage\":{\"prompt_tokens\":%d,\"completion_tokens\":%d,\"total_tokens\":%d}}\n\n"
                "data: [DONE]\n\n",
                id,
                (long long) x.created,
                sv->name,
                res.finish_reason,
                res.prompt_tokens,
                res.completion_tokens,
                res.prompt_tokens + res.completion_tokens);
        stream_write(c, ev.len, ev.p);
        stream_end(c);
        sb_free(&ev);
        return;
    }
    struct sb body = {};
    sb_printf(&body,
              "{\"id\":\"%s\",\"object\":\"chat.completion\",\"created\":%lld,\"model\":\"%s\","
              "\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":",
              id,
              (long long) x.created,
              sv->name);
    sb_json_str(&body, text.len, text.p ? text.p : "");
    sb_printf(&body,
              "},\"finish_reason\":\"%s\"}],\"usage\":{\"prompt_tokens\":%d,"
              "\"completion_tokens\":%d,\"total_tokens\":%d}}",
              res.finish_reason,
              res.prompt_tokens,
              res.completion_tokens,
              res.prompt_tokens + res.completion_tokens);
    respond_json(c, 200, body.p);
    sb_free(&body);
    sb_free(&text);
}

static void route_v1_models(struct server *sv, struct conn *c) {
    struct sb b = {};
    sb_printf(&b,
              "{\"object\":\"list\",\"data\":[{\"id\":\"%s\",\"object\":\"model\",\"created\":%lld,"
              "\"owned_by\":\"geist\"}]}",
              sv->name,
              (long long) sv->loaded_at);
    respond_json(c, 200, b.p);
    sb_free(&b);
}

static void route_v1_completions(struct server *sv, struct conn *c, struct req *r) {
    struct json j;
    if (r->body_len == 0 || json_parse(&j, r->body_len, r->body) < 0) {
        respond_error(c, 400, "body is not a JSON object");
        return;
    }
    if (!model_name_ok(sv, &j, 0)) {
        respond_error(c, 404, "model not found; GET /v1/models lists the one served");
        return;
    }
    char *prompt = json_strdup(&j, json_get(&j, 0, "prompt"));
    if (prompt == nullptr) {
        respond_error(c, 400, "prompt must be a string");
        return;
    }
    struct gen_opts o   = gen_opts_default();
    bool            ok  = gen_opts_from_json(&o, &j, 0);
    bool            bad = false;
    o.max_tokens        = (int) json_clamp(&j, json_get(&j, 0, "max_tokens"), 0, 0, CTX_CAP, &bad);
    if (!ok || bad) {
        free(prompt);
        respond_error(c, 400, "a numeric field has the wrong type");
        return;
    }
    bool stream = json_bool(&j, json_get(&j, 0, "stream"), false);

    char id[40];
    snprintf(id,
             sizeof id,
             "cmpl-%llx",
             (unsigned long long) time(nullptr) ^ (unsigned long long) clock());
    struct sb      text = {};
    struct sse_ctx x    = {.c       = c,
                           .model   = sv->name,
                           .id      = id,
                           .created = time(nullptr),
                           .text    = stream ? nullptr : &text};
    if (stream && !stream_begin(c, "text/event-stream")) {
        free(prompt);
        return;
    }

    struct gen_result res;
    char              err[256];
    int               st = generate(sv, c, &o, prompt, emit_completion, &x, &res, err);
    free(prompt);
    if (st != 0) {
        if (stream)
            stream_end(c); /* headers are out; the stream just ends */
        else
            respond_error(c, st, err);
        sb_free(&text);
        return;
    }
    if (stream) {
        struct sb ev = {};
        sb_printf(
                &ev,
                "data: "
                "{\"id\":\"%s\",\"object\":\"text_completion\",\"created\":%lld,\"model\":\"%s\","
                "\"choices\":[{\"index\":0,\"text\":\"\",\"finish_reason\":\"%s\"}],"
                "\"usage\":{\"prompt_tokens\":%d,\"completion_tokens\":%d,\"total_tokens\":%d}}\n\n"
                "data: [DONE]\n\n",
                id,
                (long long) x.created,
                sv->name,
                res.finish_reason,
                res.prompt_tokens,
                res.completion_tokens,
                res.prompt_tokens + res.completion_tokens);
        stream_write(c, ev.len, ev.p);
        stream_end(c);
        sb_free(&ev);
        return;
    }
    struct sb body = {};
    sb_printf(&body,
              "{\"id\":\"%s\",\"object\":\"text_completion\",\"created\":%lld,\"model\":\"%s\","
              "\"choices\":[{\"index\":0,\"text\":",
              id,
              (long long) x.created,
              sv->name);
    sb_json_str(&body, text.len, text.p ? text.p : "");
    sb_printf(&body,
              ",\"finish_reason\":\"%s\"}],\"usage\":{\"prompt_tokens\":%d,"
              "\"completion_tokens\":%d,\"total_tokens\":%d}}",
              res.finish_reason,
              res.prompt_tokens,
              res.completion_tokens,
              res.prompt_tokens + res.completion_tokens);
    respond_json(c, 200, body.p);
    sb_free(&body);
    sb_free(&text);
}

/* ====================================================================== */
/* Ollama API — /api/tags, /api/version, /api/show, /api/generate, /api/chat */
/* ====================================================================== */

/* Ollama's error shape is flat, unlike OpenAI's. */
static void respond_ollama_error(struct conn *c, int status, const char *msg) {
    char json[512];
    snprintf(json, sizeof json, "{\"error\":\"%s\"}", msg);
    respond_json(c, status, json);
}

/* RFC 3339 with nanoseconds in UTC, as Ollama prints timestamps. */
static void iso_time(time_t t, char out[static 40]) {
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, 40, "%Y-%m-%dT%H:%M:%S.000000000Z", &tm);
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec;
}

/* A stable 64-hex "digest" from name and size. Clients only display it;
 * hashing a multi-GB file at startup would buy nothing. */
static void fake_digest(const struct server *sv, char out[static 65]) {
    uint64_t h = 1469598103934665603ull;
    for (const char *p = sv->name; *p; p++)
        h = (h ^ (unsigned char) *p) * 1099511628211ull;
    h ^= (uint64_t) sv->file_size;
    for (int i = 0; i < 4; i++) {
        h = (h ^ (h >> 29)) * 0xbf58476d1ce4e5b9ull;
        snprintf(out + 16 * i, 17, "%016llx", (unsigned long long) h);
    }
}

static void sb_details(struct sb *b, const struct server *sv) {
    const char *arch = sv->meta.arch ? sv->meta.arch : geist_model_arch(sv->m);
    sb_printf(b,
              "{\"parent_model\":\"\",\"format\":\"gguf\",\"family\":\"%s\",\"families\":[\"%s\"],"
              "\"parameter_size\":\"%s\",\"quantization_level\":\"%s\"}",
              arch,
              arch,
              sv->meta.size_label ? sv->meta.size_label : "",
              gguf_file_type_name(sv->meta.file_type));
}

static void route_api_tags(struct server *sv, struct conn *c) {
    char      digest[65], mtime[40];
    struct sb b = {};
    fake_digest(sv, digest);
    iso_time(sv->file_mtime, mtime);
    sb_printf(&b,
              "{\"models\":[{\"name\":\"%s:latest\",\"model\":\"%s:latest\",\"modified_at\":\"%s\","
              "\"size\":%lld,\"digest\":\"%s\",\"details\":",
              sv->name,
              sv->name,
              mtime,
              (long long) sv->file_size,
              digest);
    sb_details(&b, sv);
    sb_puts(&b, "}]}");
    respond_json(c, 200, b.p);
    sb_free(&b);
}

static void route_api_ps(struct server *sv, struct conn *c) {
    char      digest[65], now[40];
    struct sb b = {};
    fake_digest(sv, digest);
    iso_time(time(nullptr) + 3600, now); /* "expires_at": never, in effect */
    sb_printf(&b,
              "{\"models\":[{\"name\":\"%s:latest\",\"model\":\"%s:latest\",\"size\":%lld,"
              "\"digest\":\"%s\","
              "\"details\":",
              sv->name,
              sv->name,
              (long long) sv->file_size,
              digest);
    sb_details(&b, sv);
    sb_printf(&b, ",\"expires_at\":\"%s\",\"size_vram\":0}]}", now);
    respond_json(c, 200, b.p);
    sb_free(&b);
}

static void route_api_show(struct server *sv, struct conn *c, struct req *r) {
    struct json j;
    if (r->body_len > 0 && json_parse(&j, r->body_len, r->body) >= 0 && !model_name_ok(sv, &j, 0) &&
        json_get(&j, 0, "name") < 0) {
        respond_ollama_error(c, 404, "model not found");
        return;
    }
    const char *arch = sv->meta.arch ? sv->meta.arch : geist_model_arch(sv->m);
    char        mtime[40];
    iso_time(sv->file_mtime, mtime);
    struct sb b = {};
    sb_puts(&b, "{\"license\":\"\",\"modelfile\":");
    struct sb mf = {};
    sb_printf(&mf, "# served by geist-serve\nFROM %s\n", sv->path);
    sb_json_str(&b, mf.len, mf.p);
    sb_free(&mf);
    sb_printf(&b, ",\"parameters\":\"num_ctx %d\",\"template\":", CTX_CAP);
    sb_json_str(&b, sv->meta.tpl ? strlen(sv->meta.tpl) : 0, sv->meta.tpl ? sv->meta.tpl : "");
    sb_puts(&b, ",\"details\":");
    sb_details(&b, sv);
    sb_printf(&b,
              ",\"model_info\":{\"general.architecture\":\"%s\",\"general.file_type\":%u,"
              "\"%s.context_length\":%u,\"geist.context_length\":%d},"
              "\"capabilities\":[\"completion\"],\"modified_at\":\"%s\"}",
              arch,
              sv->meta.file_type,
              arch,
              sv->meta.context_length,
              CTX_CAP,
              mtime);
    respond_json(c, 200, b.p);
    sb_free(&b);
}

/* Ollama's request shape: sampling under "options", num_predict for the
 * token budget, stream defaulting to true. */
static bool ollama_opts(const struct json *j, struct gen_opts *o, bool *stream) {
    int  opts = json_get(j, 0, "options");
    bool ok = true, bad = false;
    if (opts >= 0 && j->tok[opts].type == JSMN_OBJECT) {
        ok            = gen_opts_from_json(o, j, opts);
        o->max_tokens = (int) json_clamp(j, json_get(j, opts, "num_predict"), 0, -2, CTX_CAP, &bad);
        if (o->max_tokens < 0)
            o->max_tokens = 0; /* -1 infinite, -2 fill context: both = budget */
    } else if (opts >= 0) {
        bad = true;
    }
    *stream = json_bool(j, json_get(j, 0, "stream"), true);
    return ok && !bad;
}

/* The done:true object. `text` is the whole reply for a non-stream answer
 * and empty for the final streamed line. */
static void ollama_final(struct sb               *b,
                         const struct server     *sv,
                         bool                     chat,
                         size_t                   n,
                         const char               text[static n],
                         const struct gen_result *res,
                         const char              *done_reason,
                         uint64_t                 total_ns) {
    char now[40];
    iso_time(time(nullptr), now);
    sb_printf(b, "{\"model\":\"%s\",\"created_at\":\"%s\",", sv->name, now);
    if (chat) {
        sb_puts(b, "\"message\":{\"role\":\"assistant\",\"content\":");
        sb_json_str(b, n, text);
        sb_puts(b, "},");
    } else {
        sb_puts(b, "\"response\":");
        sb_json_str(b, n, text);
        sb_puts(b, ",");
    }
    sb_printf(b, "\"done\":true,\"done_reason\":\"%s\"", done_reason);
    if (res != nullptr)
        sb_printf(b,
                  ",\"total_duration\":%llu,\"load_duration\":0,\"prompt_eval_count\":%d,"
                  "\"prompt_eval_duration\":%llu,\"eval_count\":%d,\"eval_duration\":%llu",
                  (unsigned long long) total_ns,
                  res->prompt_tokens,
                  (unsigned long long) res->prefill_ns,
                  res->completion_tokens,
                  (unsigned long long) res->decode_ns);
    sb_puts(b, "}\n");
}

/* Shared tail of /api/generate and /api/chat once the prompt is rendered. */
static void ollama_run(struct server         *sv,
                       struct conn           *c,
                       bool                   chat,
                       bool                   stream,
                       const struct gen_opts *o,
                       const char            *prompt) {
    struct sb      text = {};
    struct sse_ctx x    = {.c     = c,
                           .model = sv->name,
                           .text  = stream ? nullptr : &text,
                           .kind  = chat ? EMIT_OLLAMA_CHAT : EMIT_OLLAMA_GEN};
    if (stream && !stream_begin(c, "application/x-ndjson"))
        return;
    uint64_t          t0 = now_ns();
    struct gen_result res;
    char              err[256];
    int               st = generate(sv, c, o, prompt, emit_completion, &x, &res, err);
    if (st != 0) {
        if (stream)
            stream_end(c);
        else
            respond_ollama_error(c, st, err);
        sb_free(&text);
        return;
    }
    struct sb b = {};
    ollama_final(
            &b, sv, chat, text.len, text.p ? text.p : "", &res, res.finish_reason, now_ns() - t0);
    if (stream) {
        stream_write(c, b.len, b.p);
        stream_end(c);
    } else {
        respond_json(c, 200, b.p);
    }
    sb_free(&b);
    sb_free(&text);
}

static void route_api_generate(struct server *sv, struct conn *c, struct req *r) {
    struct json j;
    if (r->body_len == 0 || json_parse(&j, r->body_len, r->body) < 0) {
        respond_ollama_error(c, 400, "body is not a JSON object");
        return;
    }
    if (!model_name_ok(sv, &j, 0)) {
        respond_ollama_error(c, 404, "model not found; GET /api/tags lists the one served");
        return;
    }
    struct gen_opts o = gen_opts_default();
    bool            stream;
    if (!ollama_opts(&j, &o, &stream)) {
        respond_ollama_error(c, 400, "options: a field has the wrong type");
        return;
    }
    char *prompt = json_strdup(&j, json_get(&j, 0, "prompt"));
    char *system = json_strdup(&j, json_get(&j, 0, "system"));
    if (prompt == nullptr || prompt[0] == '\0') {
        /* Ollama's "load the model" call: answer done at once. */
        struct sb b = {};
        ollama_final(&b, sv, false, 0, "", nullptr, "load", 0);
        respond_json(c, 200, b.p);
        sb_free(&b);
        free(prompt);
        free(system);
        return;
    }
    /* raw:true (or no known template) sends the prompt as-is; otherwise it
     * is one user turn rendered like /api/chat would. */
    char *rendered = nullptr;
    if (!json_bool(&j, json_get(&j, 0, "raw"), false) && sv->family != CHAT_UNKNOWN) {
        struct chat_msg msgs[2];
        size_t          n = 0;
        if (system && system[0])
            msgs[n++] = (struct chat_msg) {"system", system};
        msgs[n++] = (struct chat_msg) {"user", prompt};
        rendered  = chat_render(sv->family, n, msgs);
    }
    ollama_run(sv, c, false, stream, &o, rendered ? rendered : prompt);
    free(rendered);
    free(prompt);
    free(system);
}

static void route_api_chat(struct server *sv, struct conn *c, struct req *r) {
    struct json j;
    if (r->body_len == 0 || json_parse(&j, r->body_len, r->body) < 0) {
        respond_ollama_error(c, 400, "body is not a JSON object");
        return;
    }
    if (!model_name_ok(sv, &j, 0)) {
        respond_ollama_error(c, 404, "model not found; GET /api/tags lists the one served");
        return;
    }
    struct gen_opts o = gen_opts_default();
    bool            stream;
    if (!ollama_opts(&j, &o, &stream)) {
        respond_ollama_error(c, 400, "options: a field has the wrong type");
        return;
    }
    struct chat_msg msgs[MSG_CAP];
    size_t          n_msgs = 0;
    int             arr    = json_get(&j, 0, "messages");
    if (arr >= 0 && parse_messages(&j, arr, MSG_CAP, msgs, &n_msgs) != 0) {
        free_messages(n_msgs, msgs);
        respond_ollama_error(c, 400, "messages must be an array of {role, content}");
        return;
    }
    if (n_msgs == 0) { /* the "load" call */
        struct sb b = {};
        ollama_final(&b, sv, true, 0, "", nullptr, "load", 0);
        respond_json(c, 200, b.p);
        sb_free(&b);
        return;
    }
    if (sv->family == CHAT_UNKNOWN) {
        free_messages(n_msgs, msgs);
        respond_ollama_error(
                c, 501, "no chat template known for this model; use /api/generate with raw:true");
        return;
    }
    size_t reserve = o.max_tokens > 0 ? (size_t) o.max_tokens : 512;
    if (reserve > CTX_CAP / 2)
        reserve = CTX_CAP / 2;
    char *prompt =
            chat_render_fit(sv->family, n_msgs, msgs, CTX_CAP, reserve, count_tokens, sv, nullptr);
    free_messages(n_msgs, msgs);
    if (prompt == nullptr) {
        respond_ollama_error(c, 400, "the last message alone does not fit the context");
        return;
    }
    ollama_run(sv, c, true, stream, &o, prompt);
    free(prompt);
}

static void route_api(struct server *sv, struct conn *c, struct req *r) {
    const char *p    = r->path + 4; /* past "/api" */
    bool        post = strcmp(r->method, "POST") == 0;
    if (strcmp(p, "/tags") == 0)
        route_api_tags(sv, c);
    else if (strcmp(p, "/ps") == 0)
        route_api_ps(sv, c);
    else if (strcmp(p, "/version") == 0)
        respond_json(c, 200, "{\"version\":\"0.1.0\"}");
    else if (strcmp(p, "/show") == 0)
        route_api_show(sv, c, r);
    else if (strcmp(p, "/generate") == 0 && post)
        route_api_generate(sv, c, r);
    else if (strcmp(p, "/chat") == 0 && post)
        route_api_chat(sv, c, r);
    else if (strcmp(p, "/generate") == 0 || strcmp(p, "/chat") == 0)
        respond_ollama_error(c, 405, "POST only");
    else if (strcmp(p, "/pull") == 0 || strcmp(p, "/push") == 0 || strcmp(p, "/create") == 0 ||
             strcmp(p, "/copy") == 0 || strcmp(p, "/delete") == 0 || strncmp(p, "/blobs", 6) == 0)
        respond_ollama_error(
                c, 404, "no registry: geist-serve serves the one GGUF given on its command line");
    else if (strcmp(p, "/embed") == 0 || strcmp(p, "/embeddings") == 0)
        respond_ollama_error(c, 404, "embeddings are not served in v1");
    else
        respond_ollama_error(c, 404, "no such endpoint");
}

static void handle(struct server *sv, struct conn *c, struct req *r) {
    if (strcmp(r->method, "OPTIONS") == 0) {
        respond(c, 204, "text/plain", 0, "");
        return;
    }
    if (strcmp(r->path, "/health") == 0) {
        respond_json(c, 200, "{\"status\":\"ok\"}");
        return;
    }
    if (strcmp(r->path, "/") == 0) {
        respond(c, 200, "text/plain", 17, "Ollama is running"); /* HEAD: body suppressed */
        return;
    }
    if (strncmp(r->path, "/api/", 5) == 0) {
        route_api(sv, c, r);
        return;
    }
    if (strcmp(r->path, "/v1/models") == 0) {
        route_v1_models(sv, c);
        return;
    }
    if (strcmp(r->path, "/v1/chat/completions") == 0) {
        if (strcmp(r->method, "POST") != 0)
            respond_error(c, 405, "POST only");
        else
            route_v1_chat(sv, c, r);
        return;
    }
    if (strcmp(r->path, "/v1/completions") == 0) {
        if (strcmp(r->method, "POST") != 0)
            respond_error(c, 405, "POST only");
        else
            route_v1_completions(sv, c, r);
        return;
    }
    /* ponytail: the remaining /v1 (#5) and /api (#6) routes plug in here. */
    respond_error(c, 404, "no such endpoint");
}

/* One connection, one request; Connection: close after every response is
 * the v1 contract (keep-alive buys nothing on a serial server). */
static void serve_conn(struct server *sv, int in, int out) {
    struct conn c = {.in = in, .out = out, .stdio = in == STDIN_FILENO};
    struct req  r;
    int         st = read_request(&c, &r);
    if (st == -1)
        return;
    if (st != 0) {
        respond_error(&c, st, reason(st));
    } else {
        c.head = strcmp(r.method, "HEAD") == 0;
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
        /* Graceful close: unread request bytes (an oversize request we
         * answered early) would turn close() into a RST that eats our
         * response on the way to the client. Half-close, drain briefly. */
        shutdown(fd, SHUT_WR);
        tv = (struct timeval) {.tv_sec = 2};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        char sink[4096];
        for (int i = 0; i < 64 && read(fd, sink, sizeof sink) > 0; i++) {
        }
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
    int  lfd       = -1;
    bool inherited = false;
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
    /* Model name = GGUF basename without the extension, as /api/tags shows it. */
    char path_copy[1024];
    snprintf(path_copy, sizeof path_copy, "%s", a.model);
    snprintf(sv.name, sizeof sv.name, "%s", basename(path_copy));
    char *dot = strrchr(sv.name, '.');
    if (dot != nullptr && strcmp(dot, ".gguf") == 0)
        *dot = '\0';

    /* Stop tokens: EOS plus the end-of-turn markers of the families we
     * serve — some GGUFs set them as EOS, some do not (Gemma). */
    sv.eos = geist_model_eos_token(sv.m);
    for (const char **t =
                 (const char *[]) {
                         "<end_of_turn>", "<|im_end|>", "<|eot_id|>", "<|end_of_text|>", nullptr};
         *t != nullptr && sv.n_eot < 4;
         t++) {
        geist_token_t id = geist_model_token_by_text(sv.m, *t);
        if (id != GEIST_TOKEN_NONE)
            sv.eot[sv.n_eot++] = id;
    }
    /* Chat template family: fingerprint the GGUF's own template string
     * (SmolLM2 is arch "llama" but speaks ChatML), fall back to the arch. */
    gguf_read_meta(a.model, &sv.meta);
    sv.add_bos = sv.meta.add_bos;
    sv.path    = a.model;
    struct stat st;
    if (stat(a.model, &st) == 0) {
        sv.file_size  = st.st_size;
        sv.file_mtime = st.st_mtime;
    }
    sv.family = chat_family_from_template(sv.meta.tpl);
    if (sv.family == CHAT_UNKNOWN)
        sv.family = chat_family_from_arch(geist_model_arch(sv.m));
    fprintf(stderr,
            "geist-serve: loaded %s as \"%s\" (%s) on %s, chat template %s%s, %d stop tokens\n",
            a.model,
            sv.name,
            geist_model_arch(sv.m),
            geist_backend_name(sv.be),
            chat_family_name(sv.family),
            sv.meta.tpl ? "" : " (no template in GGUF)",
            1 + sv.n_eot);
    sv.loaded_at = time(nullptr);
    /* Tokenize-only session for counting: max_seq_len 16 keeps its KV tiny. */
    struct geist_session_opts tok_opts = {.max_seq_len = 16};
    if (geist_session_create(sv.m, sv.be, &tok_opts, &sv.tok) != GEIST_OK) {
        fprintf(stderr,
                "geist-serve: tokenizer session: %s\n",
                sv.tok ? geist_session_errmsg(sv.tok) : "failed");
        return 1;
    }

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
                inherited ? "inherited socket" : "own socket");
        accept_loop(&sv, lfd);
        close(lfd);
        fprintf(stderr, "geist-serve: stopped\n");
    }

    geist_session_destroy(sv.tok);
    gguf_meta_free(&sv.meta);
    geist_model_destroy(sv.m);
    geist_backend_destroy(sv.be);
    return 0;
}
