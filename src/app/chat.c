/* chat.c — the UI's generate proxy and the /v1 completions proxy to geistd. */
#include "app.h"

struct proxy {
    int                   fd;
    bool                  started, disconnected, expired, preparing;
    double                start, first, first_answer, heartbeat;
    struct app_utf8       utf8;
    struct app_output     output;
    struct app_run_stats *stats;
    bool (*send)(void *, const char *);
    bool (*keepalive)(void *);
    void    *target;
    char     phase[24];
    uint64_t operation, generation, pieces;
};
static bool proxy_cancel(void *opaque) {
    struct proxy *p = opaque;
    char          one;
    ssize_t       n = recv(p->fd, &one, 1, MSG_PEEK | MSG_DONTWAIT);
    p->disconnected =
            n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
    p->expired = monotonic_ms() - p->start > 3600000;
    if (atomic_load(&closing) || atomic_load(&request_cancelled) || p->expired || p->disconnected)
        return true;
    if (p->stats && p->stats->stage && strcmp(p->phase, p->stats->stage)) {
        snprintf(p->phase, sizeof p->phase, "%s", p->stats->stage);
        pthread_mutex_lock(&app.mutex);
        snprintf(app.activity.request_phase, sizeof app.activity.request_phase, "%s", p->phase);
        (void) activity_step(&app.activity.request,
                             p->operation,
                             p->generation,
                             activity_request_stage(p->phase),
                             monotonic_ms());
        pthread_mutex_unlock(&app.mutex);
    }
    if (p->keepalive && monotonic_ms() - (p->heartbeat ? p->heartbeat : p->start) >= 10000) {
        p->heartbeat = monotonic_ms();
        if (!p->keepalive(p->target)) {
            p->disconnected = true;
            return true;
        }
    }
    return false;
}
static bool proxy_send(void *opaque, const char *text) {
    struct proxy *p = opaque;
    if (text[strspn(text, " \t\r\n")] && !p->first_answer)
        p->first_answer = monotonic_ms() - p->start;
    if (!p->started) {
        const char *head = "HTTP/1.1 200 OK\r\nContent-Type: application/x-ndjson\r\n"
                           "Cache-Control: no-store\r\nConnection: close\r\n"
                           "X-Content-Type-Options: nosniff\r\n\r\n";
        if (!send_bytes(p->fd, head, strlen(head)))
            return false;
        p->started = true;
    }
    char              data[16384];
    struct app_buffer b = {.data = data, .cap = sizeof data};
    app_put(&b, "{\"response\":");
    app_quote(&b, text);
    app_put(&b, ",\"done\":false}\n");
    return !b.failed && send_bytes(p->fd, data, b.len);
}
static bool proxy_keepalive(void *opaque) {
    struct proxy *p = opaque;
    if (!p->started && !proxy_send(p, ""))
        return false;
    const char *event = p->output.state == OUTPUT_REASONING && !p->first_answer
                                ? "{\"phase\":\"preparing\"}\n"
                                : "{\"heartbeat\":true}\n";
    return send_bytes(p->fd, event, strlen(event));
}
static bool proxy_decode(struct proxy *p, const char *piece) {
    if (proxy_cancel(p))
        return false;
    char decoded[8192];
    if (!app_utf8_feed(&p->utf8, piece, decoded, sizeof decoded))
        return false;
    if (*decoded && !p->first)
        p->first = monotonic_ms() - p->start;
    bool ok = app_output_feed(&p->output, decoded, p->send, p->target);
    if (*decoded) {
        pthread_mutex_lock(&app.mutex);
        if (app.activity.request.id == p->operation &&
            app.activity.request.generation == p->generation) {
            activity_progress(&app.activity.request, ++p->pieces, monotonic_ms());
            if (p->output.visible)
                activity_change(&app.activity.request, ACT_ANSWER);
            else if (p->output.state == OUTPUT_REASONING)
                activity_change(&app.activity.request, ACT_PREPARING);
        }
        pthread_mutex_unlock(&app.mutex);
    }
    if (ok && p->output.reasoning && !p->preparing && p->keepalive) {
        ok           = p->keepalive(p->target);
        p->preparing = true;
    }
    return ok;
}
static bool proxy_emit(void *opaque, const char *piece) {
    return proxy_decode(opaque, piece);
}
static void proxy_init(struct proxy *p) {
    p->operation                  = app.activity.request.id;
    p->generation                 = app.activity.request.generation;
    const struct app_model *model = app_model_find(app.child.active_id);
    app_output_init(&p->output, model ? model->reasoning_format : nullptr);
    app.error.message[0] = app.error.stage[0] = app.error.model[0] = app.error.backend[0] = 0;
    app.error.code                                                                     = 0;
    strcpy(app.activity.request_phase, "connect");
}
static void
proxy_finish(struct proxy *p, struct app_run_stats *stats, int *rc, char error[static 256]) {
    stats->reasoning       = p->output.reasoning;
    stats->no_answer       = !p->output.visible;
    stats->first_answer_ns = p->output.visible && p->first_answer > 0 ? p->first_answer * 1e6 : -1;
    app_output_finish(&p->output);
    if (p->expired) {
        *rc = 504;
        snprintf(error,
                 256,
                 "The one-hour request limit was reached. Shorten the conversation or select GPU.");
    }
    pthread_mutex_lock(&app.mutex);
    if (*rc && *rc != 499 && !p->disconnected) {
        snprintf(app.error.message, sizeof app.error.message, "%s", error);
        snprintf(app.error.stage,
                 sizeof app.error.stage,
                 "%s",
                 stats->stage ? stats->stage : "output");
        snprintf(app.error.model, sizeof app.error.model, "%s", app.child.active_id);
        snprintf(app.error.backend, sizeof app.error.backend, "%s", app.backend.active);
        app.error.code = *rc;
    }
    pthread_mutex_unlock(&app.mutex);
}

void generate(int fd, struct request *r, struct app_arena *arena) {
    struct json *json = app_alloc(arena, 1, sizeof *json, _Alignof(struct json));
    if (!json) {
        error_response(fd, 503, "Request memory budget exhausted.");
        return;
    }
    if (json_parse(json, strlen(r->body), r->body) < 0 ||
        !json_is_str(json, json_get(json, 0, "prompt"))) {
        error_response(fd, 400, "A text prompt is required.");
        return;
    }
    char                  *task_id = json_strdup(json, json_get(json, 0, "task"));
    char                  *version = json_strdup(json, json_get(json, 0, "task_version"));
    const struct app_task *task    = app_task_find(task_id ? task_id : "freeform");
    bool                   valid =
            task && !task->url[0] && (!task_id || (version && !strcmp(version, task->version)));
    free(task_id);
    free(version);
    if (!valid) {
        error_response(fd, 400, "Choose an available task and its current version.");
        return;
    }
    char *prompt = json_strdup(json, json_get(json, 0, "prompt"));
    if (!prompt) {
        error_response(fd, 503, "Cannot allocate prompt.");
        return;
    }
    size_t length = strlen(prompt);
    if (!length || length > task->input_limit) {
        free(prompt);
        error_response(fd, 400, "The input is empty or exceeds this task's byte limit.");
        return;
    }
    int         language_token     = json_get(json, 0, "language");
    char       *requested_language = json_strdup(json, language_token);
    const char *language = requested_language && !strcmp(requested_language, "de") ? "de" : "en";
    bool        valid_language =
            language_token < 0 || (requested_language && (!strcmp(requested_language, "de") ||
                                                          !strcmp(requested_language, "en")));
    free(requested_language);
    if (!valid_language) {
        free(prompt);
        error_response(fd, 400, "Choose English or German for this task.");
        return;
    }
    /* Session chat is explicit: task/benchmark requests keep their pinned
     * instructions and limits. Reuse the bounded client-message parser. */
    struct app_chat chat;
    bool            conversation = json_get(json, 0, "messages") >= 0;
    if (conversation) {
        const char *why;
        int         code = app_chat_parse(arena, r->body, &chat, &why);
        if (!code && (strcmp(task->id, "freeform") ||
                      json_bool(json, json_get(json, 0, "benchmark"), false) ||
                      chat.count >= APP_CHAT_MESSAGES || !(chat.count % 2))) {
            code = 400;
            why  = "Use alternating user and assistant messages ending with the current input.";
        }
        for (size_t i = 0; !code && i < chat.count; ++i) {
            if (strcmp(chat.messages[i].role, i % 2 ? "assistant" : "user") ||
                !chat.messages[i].content[0]) {
                code = 400;
                why  = "Use alternating nonempty user and assistant messages.";
            }
        }
        if (!code && strcmp(chat.messages[chat.count - 1].content, prompt)) {
            code = 400;
            why  = "The final message must match the current input.";
        }
        if (code) {
            free(prompt);
            error_response(fd, code, why);
            return;
        }
        if (json_get(json, 0, "max_tokens") < 0 && json_get(json, 0, "max_completion_tokens") < 0)
            chat.max_tokens =
                    0; /* App conversation uses all remaining context in one generation. */
        memmove(chat.messages + 1, chat.messages, chat.count * sizeof *chat.messages);
        chat.messages[0] = (struct chat_msg) {
                .role    = "system",
                .content = !strcmp(language, "de") ? "Answer in German." : "Answer in English."};
        chat.count++;
    }
    size_t composed_cap = length + strlen(task->instruction) + 128;
    char  *composed     = app_alloc(arena, composed_cap, 1, 1);
    if (!composed) {
        free(prompt);
        error_response(fd, 503, "Request memory budget exhausted.");
        return;
    }
    snprintf(composed,
             composed_cap,
             "%s\n%s\n\nInput:\n%s",
             !strcmp(language, "de") ? "Answer in German." : "Answer in English.",
             task->instruction,
             prompt);
    pthread_mutex_lock(&app.mutex);
    if (atomic_load(&closing) || !app.child.ready || app.compare.running || app.child.generating ||
        (app.job.running && app.job.activate)) {
        pthread_mutex_unlock(&app.mutex);
        free(prompt);
        error_response(fd, 409, "Wait until the model is ready and idle.");
        return;
    }
    if (conversation && strcmp(chat.model, app.child.active_id)) {
        pthread_mutex_unlock(&app.mutex);
        free(prompt);
        error_response(fd, 409, "The loaded model changed. Check the model and send again.");
        return;
    }
    struct app_hardware hardware;
    bool                hardware_known = app_hardware_read(&hardware, app.paths.models);
    enum app_quality    quality = app_task_quality(app_model_find(app.child.active_id),
                                                   task,
                                                   language,
                                                   hardware_known ? hardware.device : APP_UNKNOWN);
    /* Single-task quality evidence does not certify multi-turn conversation. */
    if ((conversation || strcmp(app.backend.active, app.backend.cpu) || quality != APP_QUALITY_PASSED) &&
        !json_bool(json, json_get(json, 0, "experimental"), false)) {
        pthread_mutex_unlock(&app.mutex);
        free(prompt);
        error_response(
                fd,
                409,
                "This task/model/language is experimental. Enable experimental use explicitly.");
        return;
    }
    bool benchmark   = json_bool(json, json_get(json, 0, "benchmark"), false);
    int  model_index = -1;
    for (size_t i = 0; i < app_model_count; ++i)
        if (strcmp(app.child.active_id, app_models[i].id) == 0)
            model_index = i;
    app.child.generating = true;
    struct perf_record observation;
    observation_begin(&observation,
                      "app",
                      conversation ? chat.max_tokens
                      : benchmark  ? 64
                                   : task->output_limit,
                      conversation ? chat.temperature : .2f,
                      conversation ? chat.top_p : 1);
    struct app_run_stats stats = {0};
    struct proxy         proxy = {.fd        = fd,
                                  .start     = monotonic_ms(),
                                  .stats     = &stats,
                                  .send      = proxy_send,
                                  .keepalive = proxy_keepalive};
    proxy.target               = &proxy;
    proxy_init(&proxy);
    pthread_mutex_unlock(&app.mutex);
    char error[256];
    int  rc = conversation ? app_daemon_chat(app.child.socket_path,
                                             chat.count,
                                             chat.messages,
                                             chat.max_tokens,
                                             chat.temperature,
                                             chat.top_p,
                                             proxy_emit,
                                             proxy_cancel,
                                             &proxy,
                                             &stats,
                                             error)
                           : app_daemon_run(app.child.socket_path,
                                            composed,
                                            benchmark ? 64 : task->output_limit,
                                            proxy_emit,
                                            proxy_cancel,
                                            &proxy,
                                            &stats,
                                            error);
    free(prompt);
    if (proxy.utf8.used || proxy.utf8.failed) {
        rc = 502;
        snprintf(error, sizeof error, "The model stream ended with invalid text encoding.");
    }
    proxy_finish(&proxy, &stats, &rc, error);
    if (rc == 0 && !proxy.started && !proxy_send(&proxy, ""))
        rc = 502;
    if (!proxy.started)
        error_response(fd, rc ? rc : 502, error);
    else if (rc) {
        char              data[1024];
        struct app_buffer b = {.data = data, .cap = sizeof data};
        app_put(&b, "{\"error\":");
        app_quote(&b, error);
        app_put(&b, "}\n");
        if (!b.failed)
            (void) send_bytes(fd, data, b.len);
    } else {
        char final[512];
        int  n = snprintf(final,
                          sizeof final,
                          "{\"done\":true,\"eval_count\":%zu,\"eval_duration\":%.0f,"
                          "\"total_duration\":%.0f,\"prompt_eval_count\":%zu,\"reused\":%zu,"
                          "\"limited\":%s,\"reasoning\":%s,\"no_answer\":%s,\"max_tokens\":%u,"
                          "\"first_model_text_ns\":%.0f,\"first_answer_ns\":%.0f}\n",
                          stats.tokens,
                          stats.generation_ns,
                          stats.total_ns,
                          stats.prompt_tokens,
                          stats.reused,
                          stats.limited ? "true" : "false",
                          stats.reasoning ? "true" : "false",
                          stats.no_answer ? "true" : "false",
                          stats.max_tokens,
                          proxy.first > 0 ? proxy.first * 1e6 : -1,
                          stats.first_answer_ns);
        bool delivered = send_bytes(fd, final, (size_t) n);
        if (!delivered)
            rc = 498;
        if (delivered && model_index >= 0) {
            pthread_mutex_lock(&app.mutex);
            if (!stats.no_answer && stats.tokens >= 16 && stats.generation_ns > 1e6) {
                app.prefs.measurements[model_index].tps    = stats.tokens / (stats.generation_ns / 1e9);
                app.prefs.measurements[model_index].tokens = (unsigned) stats.tokens;
            }

            pthread_mutex_unlock(&app.mutex);
        }
    }
    observation_end(&observation,
                    proxy.disconnected ? 498 : rc,
                    proxy.first,
                    monotonic_ms() - proxy.start,
                    &stats);
}

/* External clients and the browser share the same owned daemon and busy flag. */
struct completion_proxy {
    struct proxy      transport;
    bool              stream;
    char              model[160], id[64];
    struct app_buffer text;
};

void api_error(int fd, int code, const char *message) {
    char              body[1024];
    struct app_buffer b = {.data = body, .cap = sizeof body};
    app_put(&b, "{\"error\":{\"message\":");
    app_quote(&b, message);
    app_printf(&b, ",\"type\":\"invalid_request_error\",\"code\":%d}}", code);
    response(fd, code, "application/json", body, b.len);
}

static void completion_prefix(struct app_buffer *b, const struct completion_proxy *p, bool chunk) {
    app_put(b, "{\"id\":");
    app_quote(b, p->id);
    app_printf(b,
               ",\"object\":\"chat.completion%s\",\"created\":%lld,\"model\":",
               chunk ? ".chunk" : "",
               (long long) time(nullptr));
    app_quote(b, p->model);
}

static bool completion_send(void *opaque, const char *decoded) {
    struct completion_proxy *p = opaque;
    if (decoded[strspn(decoded, " \t\r\n")] && !p->transport.first_answer)
        p->transport.first_answer = monotonic_ms() - p->transport.start;
    if (!p->stream) {
        app_put(&p->text, decoded);
        return !p->text.failed;
    }
    if (!p->transport.started) {
        const char *header =
                "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: "
                "no-store\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\n\r\n";
        if (!send_bytes(p->transport.fd, header, strlen(header)))
            return false;
        p->transport.started = true;
    }
    char              data[16384];
    struct app_buffer b = {.data = data, .cap = sizeof data};
    app_put(&b, "data: ");
    completion_prefix(&b, p, true);
    app_put(&b, ",\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":");
    app_quote(&b, decoded);
    app_put(&b, "},\"finish_reason\":null}]}\n\n");
    return !b.failed && send_bytes(p->transport.fd, data, b.len);
}

static bool completion_keepalive(void *opaque) {
    struct completion_proxy *p = opaque;
    if (!p->stream)
        return true;
    if (!p->transport.started && !completion_send(p, ""))
        return false;
    return send_bytes(p->transport.fd, ": preparing\n\n", 13);
}
static bool completion_emit(void *opaque, const char *piece) {
    struct completion_proxy *p = opaque;
    return proxy_decode(&p->transport, piece);
}

void completions(int fd, const struct request *r, struct app_arena *arena) {
    struct app_chat chat;
    const char     *why;
    int             code = app_chat_parse(arena, r->body, &chat, &why);
    if (code) {
        api_error(fd, code, why);
        return;
    }
    char *text = app_alloc(arena, 32768, 1, 1), *body = app_alloc(arena, 65536, 1, 1);
    if (!text || !body) {
        api_error(fd, 503, "Request memory budget exhausted.");
        return;
    }
    struct completion_proxy p = {.transport = {.fd = fd, .start = monotonic_ms()},
                                 .stream    = chat.stream,
                                 .text      = {.data = text, .cap = 32768}};
    static atomic_ulong     sequence;
    snprintf(p.id,
             sizeof p.id,
             "chatcmpl-geist-%ld-%lu",
             (long) getpid(),
             atomic_fetch_add(&sequence, 1));
    pthread_mutex_lock(&app.mutex);
    if (atomic_load(&closing) || !app.child.ready || app.compare.running ||
        (app.job.running && app.job.activate)) {
        pthread_mutex_unlock(&app.mutex);
        api_error(fd, 503, "Select and load a model in Geist first.");
        return;
    }
    if (strcmp(chat.model, app.child.active_id)) {
        pthread_mutex_unlock(&app.mutex);
        api_error(fd,
                  404,
                  "Requested model is not loaded. Refresh /v1/models after changing models.");
        return;
    }
    if (app.child.generating) {
        pthread_mutex_unlock(&app.mutex);
        api_error(fd, 429, "The shared model is busy. Retry after the current request completes.");
        return;
    }
    snprintf(p.model, sizeof p.model, "%s", app.child.active_id);
    app.child.generating = true;
    struct perf_record observation;
    observation_begin(&observation, "api", chat.max_tokens, chat.temperature, chat.top_p);
    struct app_run_stats stats = {0};
    p.transport.stats          = &stats;
    p.transport.send           = completion_send;
    p.transport.keepalive      = completion_keepalive;
    p.transport.target         = &p;
    proxy_init(&p.transport);
    pthread_mutex_unlock(&app.mutex);
    char error[256];
    int  rc = app_daemon_chat(app.child.socket_path,
                              chat.count,
                              chat.messages,
                              chat.max_tokens,
                              chat.temperature,
                              chat.top_p,
                              completion_emit,
                              proxy_cancel,
                              &p,
                              &stats,
                              error);
    /* completion_proxy begins with proxy, so the cancellation callback borrows it. */
    if (p.transport.utf8.used || p.transport.utf8.failed || p.text.failed) {
        rc = 502;
        snprintf(error, sizeof error, "The model produced invalid or oversized text.");
    }
    if (!rc && !p.transport.output.visible) {
        rc = 422;
        snprintf(error,
                 sizeof error,
                 "The model produced no answer. Try again or allow more output tokens.");
    }
    proxy_finish(&p.transport, &stats, &rc, error);
    if (!rc && chat.stream && !p.transport.started && !completion_send(&p, ""))
        rc = 502;
    struct app_buffer b = {.data = body, .cap = 65536};
    /* A clean, connected completion leaves the daemon idle: release the shared
     * model before the final bytes, so a client that sends its next request as
     * soon as it reads this answer is admitted instead of getting 429 (#68).
     * Failures keep the old order; observation_end may reap the child first.
     * Cost: a failed final send is recorded as completed, not disconnected. */
    bool observed = !rc && !p.transport.disconnected;
    if (observed)
        observation_end(&observation, 0, p.transport.first, monotonic_ms() - p.transport.start, &stats);
    if (rc) {
        if (!p.transport.started)
            api_error(fd, rc, error);
        else {
            app_put(&b, "data: {\"error\":{\"message\":");
            app_quote(&b, error);
            app_put(&b, "}}\n\n");
            (void) send_bytes(fd, body, b.len);
        }
    } else {
        if (chat.stream)
            app_put(&b, "data: ");
        completion_prefix(&b, &p, chat.stream);
        app_put(&b, ",\"choices\":[{\"index\":0,");
        if (chat.stream)
            app_put(&b, "\"delta\":{},");
        else {
            app_put(&b, "\"message\":{\"role\":\"assistant\",\"content\":");
            app_quote(&b, text);
            app_put(&b, "},");
        }
        app_put(&b, "\"finish_reason\":");
        app_quote(&b, stats.limited ? "length" : "stop");
        app_put(&b, "}]");
        if (!chat.stream)
            app_printf(&b,
                       ",\"usage\":{\"prompt_tokens\":%zu,\"completion_tokens\":%zu,\"total_"
                       "tokens\":%zu}",
                       stats.prompt_tokens,
                       stats.tokens,
                       stats.prompt_tokens + stats.tokens);
        app_put(&b, "}");
        if (chat.stream) {
            app_put(&b, "\n\n");
            if (chat.include_usage) {
                app_put(&b, "data: ");
                completion_prefix(&b, &p, true);
                app_printf(&b,
                           ",\"choices\":[],\"usage\":{\"prompt_tokens\":%zu,\"completion_tokens\":"
                           "%zu,\"total_tokens\":%zu}}\n\n",
                           stats.prompt_tokens,
                           stats.tokens,
                           stats.prompt_tokens + stats.tokens);
            }
            app_put(&b, "data: [DONE]\n\n");
            if (b.failed)
                rc = 502;
            else if (!send_bytes(fd, body, b.len))
                rc = 498;
        } else if (b.failed) {
            rc = 502;
            api_error(fd, 502, "Completion exceeds response capacity.");
        } else if (!response(fd, 200, "application/json", body, b.len))
            rc = 498;
    }
    if (!observed)
        observation_end(&observation,
                        p.transport.disconnected ? 498 : rc,
                        p.transport.first,
                        monotonic_ms() - p.transport.start,
                        &stats);
}
