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
 * Layout: http.c speaks HTTP, generate.c talks to libgeist, openai.c and
 * ollama.c are the two API dialects; this file is argv, routing and the
 * accept loop.
 *
 * Requests are served serially. ponytail: serial by design — a second
 * concurrent inference is a thermal problem on a Pi 5, not a throughput
 * gain; add a worker only when a client measurably blocks on it. */
#include "net.h"
#include "serve.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
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
/* Routing and the accept loop                                             */
/* ====================================================================== */

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
    if (strncmp(r->path, "/api/", 5) == 0)
        ollama_route(sv, c, r);
    else
        openai_route(sv, c, r); /* /v1/..., and the 404 for everything else */
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
        respond_error(&c, st, http_reason(st));
    } else {
        c.head = strcmp(r.method, "HEAD") == 0;
        handle(sv, &c, &r);
    }
    free(r.body);
}

static void accept_loop(struct server *sv, int lfd) {
    for (;;) {
        int fd = net_accept(lfd);
        if (fd < 0) {
            if (!net_stop)
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

int main(int argc, char **argv) {
    /* An idle server must not spin: the engine asks OpenMP for active waiting
     * unless told otherwise, which kept every core busy between requests.
     * Same policy as the app gives geistd (child.c); OMP_WAIT_POLICY wins. */
    setenv("OMP_WAIT_POLICY", "passive", 0);
    struct args a;
    if (!parse_args(argc, argv, &a))
        return usage(argv[0]);

    /* Decide the transport before the multi-second model load, so a bad
     * --host or a busy port fails in milliseconds. */
    int  lfd       = -1;
    bool inherited = false;
    if (!a.stdio) {
        lfd       = net_inherited_listener();
        inherited = lfd >= 0;
        if (lfd < 0)
            lfd = net_bind_tcp(a.host, a.port);
        if (lfd < 0)
            return 1;
    }

    struct server sv = {};
    if (geist_backend_create("auto", nullptr, nullptr, &sv.mo.be) != GEIST_OK) {
        fprintf(stderr,
                "backend: %s\n",
                sv.mo.be ? geist_backend_errmsg(sv.mo.be) : "create failed");
        return 1;
    }
    if (geist_model_load(a.model, sv.mo.be, &sv.mo.m) != GEIST_OK) {
        fprintf(stderr, "model: %s\n", sv.mo.m ? geist_model_errmsg(sv.mo.m) : "load failed");
        geist_backend_destroy(sv.mo.be);
        return 1;
    }
    model_describe(&sv.mo, a.model);
    struct stat st;
    if (stat(a.model, &st) == 0) {
        sv.file_size  = st.st_size;
        sv.file_mtime = st.st_mtime;
    }
    fprintf(stderr,
            "geist-serve: loaded %s as \"%s\" (%s) on %s, chat template %s%s, %d stop tokens\n",
            a.model,
            sv.mo.name,
            geist_model_arch(sv.mo.m),
            geist_backend_name(sv.mo.be),
            chat_family_name(sv.mo.family),
            sv.mo.meta.tpl ? "" : " (no template in GGUF)",
            1 + sv.mo.n_eot);
    sv.loaded_at = time(nullptr);
    /* Tokenize-only session for counting: max_seq_len 16 keeps its KV tiny. */
    struct geist_session_opts tok_opts = {.max_seq_len = 16};
    if (geist_session_create(sv.mo.m, sv.mo.be, &tok_opts, &sv.tok) != GEIST_OK) {
        fprintf(stderr,
                "geist-serve: tokenizer session: %s\n",
                sv.tok ? geist_session_errmsg(sv.tok) : "failed");
        return 1;
    }

    net_install_signals();

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
    model_close(&sv.mo);
    return 0;
}
