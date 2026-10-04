#include "daemon.h"
#include "../json.h"
#include "../template.h"
#define GEISTD_CLIENT_IMPLEMENTATION
#include "../../clients/geistd_client.h"

bool app_daemon_ready(const char *path) {
    char backend[24];
    return app_daemon_ready_backend(path, backend);
}

bool app_daemon_ready_backend(const char *path, char backend[static 24]) {
    struct app_engine ignored;
    return app_daemon_identity(path, backend, &ignored);
}

bool app_daemon_identity(const char *path, char backend[static 24], struct app_engine *engine) {
    *engine          = (struct app_engine) {0};
    backend[0]       = 0;
    struct geistd *g = geistd_connect_unix(path, nullptr);
    if (!g)
        return false;
    geistd_limits(g, 150, nullptr, nullptr);
    char         info[2048];
    bool         ok = geistd_info(g, sizeof info, info) == 0;
    struct json *j  = calloc(1, sizeof *j);
    if (ok && j && json_parse(j, strlen(info), info) >= 0) {
        char *name = json_strdup(j, json_get(j, 0, "backend"));
        if (name)
            snprintf(backend, 24, "%s", name);
        free(name);
        ok = app_engine_parse(engine, j, json_get(j, 0, "engine"));
    } else
        ok = false;
    free(j);
    geistd_close(g);
    return ok;
}

int app_daemon_chat(const char           *path,
                    size_t                count,
                    const struct chat_msg messages[],
                    unsigned              max,
                    float                 temperature,
                    float                 top_p,
                    bool (*emit)(void *, const char *),
                    bool (*cancel)(void *),
                    void                 *ctx,
                    struct app_run_stats *stats,
                    char                  error[static 256]) {
    int            rc       = 502;
    struct geistd *g        = geistd_connect_unix(path, nullptr);
    struct json   *j        = calloc(1, sizeof *j);
    char          *rendered = nullptr, *family = nullptr;
    char           session[17] = "", info[2048], reason[16] = "";
    int32_t        ids[4096];
    size_t         n = 0, used = 0;
    *stats = (struct app_run_stats) {.prefill_ns = -1, .first_answer_ns = -1, .stage = "connect"};
    double start = gd_now();
    snprintf(error, 256, "Could not contact the model service. Reload the model.");
    if (!g || !j)
        goto cleanup;
    geistd_limits(g, 120000, cancel, ctx);
    errno = 0;
    if (geistd_info(g, sizeof info, info) != 0 || json_parse(j, strlen(info), info) < 0)
        goto cleanup;
    family             = json_strdup(j, json_get(j, 0, "template"));
    enum chat_family f = CHAT_UNKNOWN;
    for (enum chat_family k = CHAT_GEMMA3; k <= CHAT_BITNET; k++)
        if (family && strcmp(family, chat_family_name(k)) == 0)
            f = k;
    if (f == CHAT_UNKNOWN) {
        rc = 400;
        snprintf(error, 256, "This model's chat format is not supported.");
        goto cleanup;
    }
    size_t  capacity = (size_t) json_num(j, json_get(j, 0, "ctx"), 0);
    bool    add_bos  = json_bool(j, json_get(j, 0, "add_bos"), false);
    int32_t bos      = (int32_t) json_num(j, json_get(j, 0, "bos"), -1);
    if (!capacity || capacity > 4096 || max > 4095)
        goto cleanup;
    stats->stage = "open";
    errno        = 0;
    rendered     = chat_render(f, count, messages);
    if (!rendered || geistd_open(g, temperature, top_p, 0, 0, session) != 0)
        goto cleanup;
    stats->stage = "tokenize";
    errno        = 0;
    if (geistd_tokenize(g, rendered, 4095, ids + 1, &n) != 0) {
        if (strstr(geistd_error(g), "too many tokens") ||
            !strcmp(geistd_error(g), "tokenize: text too long for the context")) {
            rc = 400;
            snprintf(error,
                     256,
                     "The prompt does not fit this model's context. Try a shorter text.");
        }
        goto cleanup;
    }
    if (add_bos && bos >= 0 && (!n || ids[1] != bos)) {
        ids[0] = bos;
        n++;
    } else
        memmove(ids, ids + 1, n * sizeof *ids);
    if (!n || n >= capacity - 1) {
        rc = 400;
        snprintf(error, 256, "The prompt does not fit this model's context. Try a shorter text.");
        goto cleanup;
    }
    /* Zero requests the remaining context; explicit editor limits remain upper bounds. */
    size_t remaining = capacity - n - 1;
    if (!max || max > remaining)
        max = (unsigned) remaining;
    stats->max_tokens    = max;
    stats->prompt_tokens = n;
    stats->stage         = "prefill";
    unsigned prefill_ms  = 600000;
#ifdef APP_TESTING
    /* Time-scaled fault fixture; release binaries have no environment override. */
    const char *test_ms = getenv("GEIST_TEST_PREFILL_MS");
    if (test_ms && !strcmp(test_ms, "100"))
        prefill_ms = 100;
#endif
    geistd_limits(g, prefill_ms, cancel, ctx);
    errno                = 0;
    double prefill_start = gd_now();
    if (geistd_prefill(g, session, n, ids, &used, &stats->reused) != 0)
        goto cleanup;
    stats->prefill_ns    = (gd_now() - prefill_start) * 1e6;
    stats->prompt_tokens = n;
    stats->stage         = "generate";
    geistd_limits(g, 120000, cancel, ctx);
    geistd_stream_idle(g, true);
    errno = 0;
    struct geistd_generation generation;
    if (geistd_generate_ex(g, session, max, emit, ctx, reason, &generation) != 0)
        goto cleanup;
    if (!strcmp(reason, "client")) {
        rc = 499;
        goto cleanup;
    }
    if (strcmp(reason, "stop") && strcmp(reason, "max") && strcmp(reason, "context"))
        goto cleanup;
    stats->limited       = strcmp(reason, "stop") != 0;
    stats->tokens        = generation.tokens;
    stats->generation_ns = generation.duration_ns;
    stats->total_ns      = (gd_now() - start) * 1e6;
    rc                   = 0;
cleanup:
    stats->total_ns = (gd_now() - start) * 1e6;
    if (rc == 502) {
        bool prefill  = !strcmp(stats->stage, "prefill");
        bool generate = !strcmp(stats->stage, "generate");
        if (errno == ECANCELED)
            rc = 499;
        else if (errno == ETIMEDOUT) {
            rc = 504;
            snprintf(error,
                     256,
                     "%s",
                     prefill    ? "Input processing timed out after 10 minutes. Shorten the "
                                  "conversation or select GPU."
                     : generate ? "The model stopped responding while generating. Try again or "
                                  "select GPU."
                                : "Could not contact the model service. Reload the model.");
        } else
            snprintf(error,
                     256,
                     "%s",
                     prefill ? "Model execution failed while processing the input. Try a shorter "
                               "conversation or select GPU."
                     : generate ? "Model execution failed while generating. Reload the model or "
                                  "select another processor."
                                : "Could not contact the model service. Reload the model.");
    }
    if (rc == 499)
        snprintf(error, 256, "Generation cancelled.");
    if (g && session[0]) {
        /* Cleanup must be bounded even after a vanished browser / daemon. */
        geistd_limits(g, 150, nullptr, nullptr);
        (void) geistd_close_session(g, session);
    }
    free(rendered);
    free(family);
    free(j);
    geistd_close(g);
    return rc;
}

int app_daemon_run(const char *path,
                   const char *prompt,
                   unsigned    max,
                   bool (*emit)(void *, const char *),
                   bool (*cancel)(void *),
                   void                 *ctx,
                   struct app_run_stats *stats,
                   char                  error[static 256]) {
    const struct chat_msg message = {.role = "user", .content = prompt};
    return app_daemon_chat(path, 1, &message, max, .2f, 1, emit, cancel, ctx, stats, error);
}
