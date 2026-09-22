/* geist-serve — an Ollama- and OpenAI-compatible HTTP front for the geist
 * engine. One model, one process, one request at a time.
 *
 * Process model (issue #2): a new-style foreground daemon. No fork, no pid
 * file; logs to stderr, exits on SIGTERM. The listening socket comes from
 * systemd (LISTEN_FDS) or inetd wait-mode when one is handed in, otherwise
 * we bind --host:--port ourselves. --stdio serves exactly one connection on
 * fd 0/1, which is inetd accept-mode and the test harness in one flag.
 *
 * This file is the skeleton: argument parsing and model load. The transport,
 * templates and endpoints land per issue; see the v0.1 milestone. */
#include <geist.h>
#include <geist_util.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
            "A listening socket passed via LISTEN_FDS (systemd) is used as-is.\n",
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
            if (a->port <= 0 || a->port > 65535) return false;
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

int main(int argc, char **argv) {
    struct args a;
    if (!parse_args(argc, argv, &a)) return usage(argv[0]);

    struct geist_backend *be = nullptr;
    if (geist_backend_create("auto", nullptr, nullptr, &be) != GEIST_OK) {
        fprintf(stderr, "backend: %s\n", be ? geist_backend_errmsg(be) : "create failed");
        return 1;
    }
    struct geist_model *m = nullptr;
    if (geist_model_load(a.model, be, &m) != GEIST_OK) {
        fprintf(stderr, "model: %s\n", m ? geist_model_errmsg(m) : "load failed");
        geist_backend_destroy(be);
        return 1;
    }
    fprintf(stderr, "geist-serve: loaded %s (%s) on %s\n", a.model, geist_model_arch(m),
            geist_backend_name(be));
    /* ponytail: transport lands with issue #2; until then this proves the
     * build links against the pinned engine and loads a model. */
    fprintf(stderr, "geist-serve: HTTP transport not implemented yet (issue #2)\n");

    geist_model_destroy(m);
    geist_backend_destroy(be);
    return 1;
}
