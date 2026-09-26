#include "daemon.h"
#include "../json.h"
#include "../template.h"
#define GEISTD_CLIENT_IMPLEMENTATION
#include "../../clients/geistd_client.h"

bool app_daemon_ready(const char *path) {
    struct geistd *g = geistd_connect_unix(path, nullptr);
    if (!g)
        return false;
    geistd_limits(g, 150, nullptr, nullptr);
    char info[2048];
    bool ok = geistd_info(g, sizeof info, info) == 0;
    geistd_close(g);
    return ok;
}

int app_daemon_run(const char *path,
                   const char *prompt,
                   unsigned    max,
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
    *stats = (struct app_run_stats) {};
    snprintf(error, 256, "The model could not answer. Check its status and try again.");
    if (!g || !j)
        goto cleanup;
    geistd_limits(g, 120000, cancel, ctx);
    double start = gd_now();
    if (geistd_info(g, sizeof info, info) != 0 || json_parse(j, strlen(info), info) < 0)
        goto cleanup;
    family             = json_strdup(j, json_get(j, 0, "template"));
    enum chat_family f = CHAT_UNKNOWN;
    for (enum chat_family k = CHAT_GEMMA3; k <= CHAT_BITNET; k++)
        if (family && strcmp(family, chat_family_name(k)) == 0)
            f = k;
    /* Match geistagent's existing BitNet policy: the shipped 2B-4T model
     * follows Llama-3 turn markers; the GGUF Human:/BITNETAssistant template
     * produced repeating continuations in the app smoke test. */
    if (f == CHAT_BITNET)
        f = CHAT_LLAMA3;
    if (f == CHAT_UNKNOWN) {
        rc = 400;
        snprintf(error, 256, "This model's chat format is not supported.");
        goto cleanup;
    }
    size_t  capacity = (size_t) json_num(j, json_get(j, 0, "ctx"), 0);
    bool    add_bos  = json_bool(j, json_get(j, 0, "add_bos"), false);
    int32_t bos      = (int32_t) json_num(j, json_get(j, 0, "bos"), -1);
    if (!capacity || capacity > 4096 || !max || max >= capacity)
        goto cleanup;
    rendered = chat_render(f, 1, &(struct chat_msg) {.role = "user", .content = prompt});
    if (!rendered || geistd_open(g, .2f, 1, 0, 0, session) != 0)
        goto cleanup;
    if (geistd_tokenize(g, rendered, 4095, ids + 1, &n) != 0) {
        rc = 400;
        snprintf(error, 256, "The prompt does not fit this model's context. Try a shorter text.");
        goto cleanup;
    }
    if (add_bos && bos >= 0 && (!n || ids[1] != bos)) {
        ids[0] = bos;
        n++;
    } else
        memmove(ids, ids + 1, n * sizeof *ids);
    if (!n || n + max >= capacity) {
        rc = 400;
        snprintf(error, 256, "The prompt does not fit this model's context. Try a shorter text.");
        goto cleanup;
    }
    if (geistd_prefill(g, session, n, ids, &used, &stats->reused) != 0)
        goto cleanup;
    stats->prompt_tokens = n;
    struct geistd_generation generation;
    if (geistd_generate_ex(g, session, max, emit, ctx, reason, &generation) != 0)
        goto cleanup;
    if (strcmp(reason, "stop") && strcmp(reason, "max"))
        goto cleanup;
    stats->limited       = !strcmp(reason, "max");
    stats->tokens        = generation.tokens;
    stats->generation_ns = generation.duration_ns;
    stats->total_ns      = (gd_now() - start) * 1e6;
    rc                   = 0;
cleanup:
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
