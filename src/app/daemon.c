#include "daemon.h"
#include "../json.h"
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

/* #148: the conversation lives in geistd (geist-runtime). The app keeps one
 * resident chat and what it holds; a request is matched against it: rewind
 * to the common start, send only the rest. ponytail: one conversation (the
 * memory of one KV cache, as before); an alternating second client costs a
 * full prefill, like every request did before. */
static struct {
    char   id[17];
    char   path[256];
    float  temperature, top_p;
    char   reasoning[16];
    size_t n;
    char  *role[APP_CHAT_MESSAGES + 1], *content[APP_CHAT_MESSAGES + 1];
} held;

static void held_keep(size_t n) {
    for (size_t i = n; i < held.n; i++)
        free(held.role[i]), free(held.content[i]), held.role[i] = held.content[i] = nullptr;
    held.n = n < held.n ? n : held.n;
}
static bool held_push(const char *role, const char *content) {
    if (held.n > APP_CHAT_MESSAGES)
        return false;
    held.role[held.n]    = strdup(role);
    held.content[held.n] = strdup(content);
    if (!held.role[held.n] || !held.content[held.n]) {
        free(held.role[held.n]), free(held.content[held.n]);
        return false;
    }
    held.n++;
    return true;
}
static bool same_role(const char *a, const char *b) {
    bool sa = !strcmp(a, "system"), aa = !strcmp(a, "assistant"), sb = !strcmp(b, "system"), ab = !strcmp(b, "assistant");
    return sa == sb && aa == ab; /* anything else counts as user, as in the runtime */
}

struct chat_sink {
    struct geistd *g;
    bool (*part)(void *, bool, const char *);
    bool (*cancel)(void *);
    void                 *ctx;
    struct app_run_stats *stats;
    double                start;
    bool                  answering;
    char                 *answer; /* what the client sees: the answer text */
    size_t                len, cap;
};
static bool chat_part(void *opaque, bool thinking, const char *text) {
    struct chat_sink *k = opaque;
    if (!strcmp(k->stats->stage, "prefill")) {
        /* Input processed: generation has its own per-frame deadline. */
        k->stats->prefill_ns = (gd_now() - k->start) * 1e6;
        k->stats->stage      = "generate";
        geistd_limits(k->g, 120000, k->cancel, k->ctx);
    }
    if (!thinking) {
        size_t n = strlen(text);
        if (k->len + n + 1 > k->cap) {
            size_t cap = k->cap ? k->cap * 2 : 4096;
            while (cap < k->len + n + 1)
                cap *= 2;
            char *p = realloc(k->answer, cap);
            if (!p)
                return false;
            k->answer = p, k->cap = cap;
        }
        memcpy(k->answer + k->len, text, n + 1);
        k->len += n;
    }
    return k->part(k->ctx, thinking, text);
}

int app_daemon_chat(const char           *path,
                    size_t                count,
                    const struct chat_msg messages[],
                    unsigned              max,
                    float                 temperature,
                    float                 top_p,
                    const char           *reasoning,
                    bool (*part)(void *, bool thinking, const char *),
                    bool (*cancel)(void *),
                    void                 *ctx,
                    struct app_run_stats *stats,
                    char                  error[static 256]) {
    int            rc   = 502;
    struct geistd *g    = geistd_connect_unix(path, nullptr);
    struct json   *j    = calloc(1, sizeof *j);
    char           info[2048];
    struct chat_sink sink = {.g = g, .part = part, .cancel = cancel, .ctx = ctx, .stats = stats};
    struct geistd_chat_stats cs;
    *stats = (struct app_run_stats) {.prefill_ns = -1, .first_answer_ns = -1, .stage = "connect"};
    double start = gd_now();
    snprintf(error, 256, "Could not contact the model service. Reload the model.");
    if (!g || !j || !count || count > APP_CHAT_MESSAGES)
        goto cleanup;
    geistd_limits(g, 120000, cancel, ctx);
    errno = 0;
    if (geistd_info(g, sizeof info, info) != 0 || json_parse(j, strlen(info), info) < 0)
        goto cleanup;
    size_t capacity = (size_t) json_num(j, json_get(j, 0, "ctx"), 0);
    if (!json_bool(j, json_get(j, 0, "chat_api"), false)) {
        rc = 400;
        snprintf(error, 256, "This model's chat format is not supported.");
        goto cleanup;
    }
    if (!capacity || capacity > 4096 || max > 4095)
        goto cleanup;
    const char *format = reasoning && !strcmp(reasoning, "think_tags") ? "think_tags" : "none";
    stats->stage       = "open";
    for (int attempt = 0; attempt < 2; attempt++) {
        /* The held chat fits when the daemon and the sampling options are the same. */
        bool fits = held.id[0] && !strcmp(held.path, path) && held.temperature == temperature &&
                    held.top_p == top_p && !strcmp(held.reasoning, format);
        if (!fits) {
            if (held.id[0] && !strcmp(held.path, path))
                (void) geistd_chat_close(g, held.id);
            held_keep(0);
            held.id[0] = 0;
            errno      = 0;
            if (geistd_chat_open(g, temperature, top_p, format, true, held.id) != 0) {
                held.id[0] = 0;
                goto cleanup;
            }
            snprintf(held.path, sizeof held.path, "%s", path);
            snprintf(held.reasoning, sizeof held.reasoning, "%s", format);
            held.temperature = temperature, held.top_p = top_p;
        }
        /* Common start with what the chat holds; at least the last message is sent. */
        size_t keep = 0;
        while (keep < held.n && keep < count - 1 && same_role(held.role[keep], messages[keep].role) &&
               !strcmp(held.content[keep], messages[keep].content))
            keep++;
        size_t length = held.n;
        if (keep < held.n && geistd_chat_rewind(g, held.id, keep, &length) != 0) {
            if (strstr(geistd_error(g), "unknown chat")) { /* geistd restarted or evicted it */
                held.id[0] = 0;
                continue;
            }
            goto cleanup;
        }
        held_keep(keep);
        const char *roles[APP_CHAT_MESSAGES], *contents[APP_CHAT_MESSAGES];
        for (size_t i = keep; i < count; i++)
            roles[i - keep] = messages[i].role, contents[i - keep] = messages[i].content;
        /* What is read now: only the new messages. ponytail: estimated (about 4
         * bytes a token) for the "reading N tokens" hint; geistd counts exactly
         * inside the send and the stats replace it. Exact needs a runtime
         * progress callback. */
        size_t bytes = 0;
        for (size_t i = keep; i < count; i++)
            bytes += strlen(messages[i].content);
        stats->prompt_tokens = bytes / 4 + 1;
        stats->stage         = "prefill";
        unsigned prefill_ms  = 600000;
#ifdef APP_TESTING
        /* Time-scaled fault fixture; release binaries have no environment override. */
        const char *test_ms = getenv("GEIST_TEST_PREFILL_MS");
        if (test_ms && !strcmp(test_ms, "100"))
            prefill_ms = 100;
#endif
        geistd_limits(g, prefill_ms, cancel, ctx);
        geistd_stream_idle(g, true);
        sink.start = gd_now();
        errno      = 0;
        int sent   = geistd_chat_send(g, held.id, max, count - keep, roles, contents, chat_part, &sink, &cs);
        if (sent != 0 && strstr(geistd_error(g), "unknown chat")) {
            held.id[0] = 0;
            continue;
        }
        if (sent != 0) {
            if (!strcmp(cs.status, "context")) {
                rc = 400;
                snprintf(error, 256, "The prompt does not fit this model's context. Try a shorter text.");
            }
            goto cleanup;
        }
        /* The chat now holds the request and the answer as the client sees it. */
        for (size_t i = keep; i < count; i++)
            if (!held_push(messages[i].role, messages[i].content))
                held.id[0] = 0;
        if (!held_push("assistant", sink.answer ? sink.answer : ""))
            held.id[0] = 0;
        if (!strcmp(cs.finish, "client")) {
            rc = 499;
            goto cleanup;
        }
        if (strcmp(cs.finish, "stop") && strcmp(cs.finish, "length") && strcmp(cs.finish, "context"))
            goto cleanup;
        size_t prompt        = cs.context_tokens - cs.output_tokens;
        stats->prompt_tokens = prompt;
        stats->reused        = prompt > cs.input_tokens ? prompt - cs.input_tokens : 0;
        /* Zero requests the remaining context; explicit limits stay upper bounds. */
        unsigned room        = prompt + 1 < capacity ? (unsigned) (capacity - prompt - 1) : 0;
        stats->max_tokens    = max && max < room ? max : room;
        stats->limited       = strcmp(cs.finish, "stop") != 0;
        stats->tokens        = cs.output_tokens;
        stats->generation_ns = cs.generation_ms * 1e6;
        if (stats->prefill_ns < 0)
            stats->prefill_ns = cs.prefill_ms * 1e6;
        rc = 0;
        break;
    }
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
    free(sink.answer);
    free(j);
    geistd_close(g);
    return rc;
}

int app_daemon_run(const char *path,
                   const char *prompt,
                   unsigned    max,
                   const char *reasoning,
                   bool (*part)(void *, bool thinking, const char *),
                   bool (*cancel)(void *),
                   void                 *ctx,
                   struct app_run_stats *stats,
                   char                  error[static 256]) {
    const struct chat_msg message = {.role = "user", .content = prompt};
    return app_daemon_chat(path, 1, &message, max, .2f, 1, reasoning, part, cancel, ctx, stats, error);
}
