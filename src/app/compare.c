/* compare.c — the CPU/GPU comparison run. */
#include "app.h"

/* An explicit, idle-only local comparison. No HTTP worker waits for inference.
 * A fresh daemon per processor fixes cache warmup policy; measured repetitions
 * use fresh sessions on that daemon. Actual prefix reuse is recorded. */
struct compare_output {
    double                start, first;
    struct app_run_stats *stats;
    uint64_t              operation, generation;
};
static bool comparison_cancel(void *context) {
    struct compare_output *o = context;
    if (o && o->stats && o->stats->stage) {
        pthread_mutex_lock(&app.mutex);
        (void) activity_step(&app.activity.request,
                             o->operation,
                             o->generation,
                             activity_request_stage(o->stats->stage),
                             monotonic_ms());
        pthread_mutex_unlock(&app.mutex);
    }
    return atomic_load(&compare_cancelled) || atomic_load(&closing);
}
static bool comparison_part(void *context, bool thinking, const char *text) {
    struct compare_output *o = context;
    (void) thinking;
    if (text[0] && !o->first)
        o->first = monotonic_ms() - o->start;
    return !comparison_cancel(nullptr);
}
static bool comparison_ready(bool restoring) {
    for (unsigned i = 0; i < 600; i++) {
        if (atomic_load(&closing) || (!restoring && comparison_cancel(nullptr)))
            return false;
        pthread_mutex_lock(&app.mutex);
        bool ready = app.child.ready, exists = app.child.pid > 0;
        pthread_mutex_unlock(&app.mutex);
        if (ready)
            return true;
        if (!exists)
            return false;
        struct timespec pause = {.tv_nsec = 100000000};
        nanosleep(&pause, nullptr);
    }
    return false;
}
static void *comparison_main(void *unused) {
    (void) unused;
    char path[APP_PATH_CAP], id[64], previous[8], run[80];
    bool gpu;
    pthread_mutex_lock(&app.mutex);
    snprintf(path, sizeof path, "%s", app.child.chosen);
    snprintf(id, sizeof id, "%s", app.child.active_id);
    snprintf(previous, sizeof previous, "%s", app.backend.mode);
    /* Status keeps showing the user's choice while the run switches CPU/GPU (#83). */
    snprintf(app.compare.user_mode, sizeof app.compare.user_mode, "%s", previous);
    gpu = gpu_supported(app_model_find(id));
    snprintf(run, sizeof run, "compare-v1-%ld-%.0f", (long) getpid(), monotonic_ms());
    pthread_mutex_unlock(&app.mutex);
    bool ok = true;
    for (unsigned slot = 0; ok && slot < (gpu ? 2u : 1u); slot++) {
        pthread_mutex_lock(&app.mutex);
        strcpy(app.compare.phase, "loading");
        ok = !comparison_cancel(nullptr) && start_child_mode(path, id, slot ? "gpu" : "cpu");
        /* Comparison must not rewrite the user's saved processor preference. */
        app.backend.save = false;
        pthread_mutex_unlock(&app.mutex);
        ok = ok && comparison_ready(false);
        pthread_mutex_lock(&app.mutex);
        ok = ok && !strcmp(app.backend.active, slot ? app.backend.gpu : app.backend.cpu);
        pthread_mutex_unlock(&app.mutex);
        for (unsigned repeat = 0; ok && repeat < 4; repeat++) {
            if (comparison_cancel(nullptr)) {
                ok = false;
                break;
            }
            struct perf_record r;
            pthread_mutex_lock(&app.mutex);
            app.child.generating   = true;
            app.compare.step = slot * 4 + repeat + 1;
            strcpy(app.compare.phase, repeat ? "measuring" : "warmup");
            observation_begin(&r, "controlled_test", 128, 0, 1);
            r.warmup = !repeat;
            snprintf(r.run, sizeof r.run, "%s", run);
            pthread_mutex_unlock(&app.mutex);
            const struct chat_msg prompt = {
                    .role    = "user",
                    .content = "Explain how a seed grows into a plant. Describe the stages in six "
                               "numbered sentences."};
            struct app_run_stats  stats;
            char                  error[256];
            struct compare_output output = {.start      = monotonic_ms(),
                                            .stats      = &stats,
                                            .operation  = app.activity.request.id,
                                            .generation = app.generation};
            int                   rc     = app_daemon_chat(app.child.socket_path,
                                                           1,
                                                           &prompt,
                                                           128,
                                                           0,
                                                           1,
                                                           nullptr,
                                                           comparison_part,
                                                           comparison_cancel,
                                                           &output,
                                                           &stats,
                                                           error);
            if (comparison_cancel(nullptr))
                rc = 499;
            observation_end(&r, rc, output.first, monotonic_ms() - output.start, &stats);
            ok = rc == 0 && stats.tokens > 0;
        }
    }
    pthread_mutex_lock(&app.mutex);
    strcpy(app.compare.phase, "restoring");
    bool restore       = !atomic_load(&closing) && start_child_mode(path, id, previous);
    app.backend.save = false;
    pthread_mutex_unlock(&app.mutex);
    restore = restore && comparison_ready(true);
    pthread_mutex_lock(&app.mutex);
    strcpy(app.compare.result,
           !restore                     ? "restore_failed"
           : comparison_cancel(nullptr) ? "cancelled"
           : ok                         ? "completed"
                                        : "failed");
    app.compare.phase[0] = 0;
    app.compare.running        = false;
    pthread_mutex_unlock(&app.mutex);
    return nullptr;
}
void comparison_start(int fd, const char *body, struct app_arena *arena) {
    struct json *j = app_alloc(arena, 1, sizeof *j, _Alignof(struct json));
    if (!j || json_parse(j, strlen(body), body) < 0 ||
        !json_bool(j, json_get(j, 0, "confirm"), false)) {
        error_response(fd, 400, "Confirm the local processor comparison.");
        return;
    }
    pthread_mutex_lock(&app.mutex);
    if (!app.child.ready || app.child.generating || app.job.running || app.compare.running ||
        !app_model_find(app.child.active_id)) {
        pthread_mutex_unlock(&app.mutex);
        error_response(fd, 409, "Load a model and wait for other work to finish.");
        return;
    }
    if (app.compare.joinable) {
        pthread_join(app.compare.thread, nullptr);
        app.compare.joinable = false;
    }
    app.compare.running         = true;
    app.compare.step      = 0;
    app.compare.result[0] = 0;
    strcpy(app.compare.phase, "loading");
    atomic_store(&compare_cancelled, false);
    bool ok              = pthread_create(&app.compare.thread, nullptr, comparison_main, nullptr) == 0;
    app.compare.joinable = ok;
    if (!ok)
        app.compare.running = false;
    pthread_mutex_unlock(&app.mutex);
    if (ok)
        response(fd, 202, "application/json", "{}", 2);
    else
        error_response(fd, 503, "Cannot start comparison.");
}
