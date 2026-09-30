/* observe.c — memory and resource observation around a model operation. */
#include "app.h"

/* These functions capture/update the one active request under app.mutex. Disk
 * work is confined to performance.c, after this mutex is released. */
void memory_generation(char *out, size_t capacity) {
    snprintf(out, capacity, "%s:%llu", app.instance, (unsigned long long) app.generation);
}
void memory_observe(struct app_memory_record *record) {
#ifdef __APPLE__
    record->rss_source = 1;
#else
    record->rss_source = 2;
#endif
    struct lifecycle_memory sample;
    char                    generation[112];
    memory_generation(generation, sizeof generation);
    bool valid = app.child > 0 && !app.stopping && !strcmp(generation, record->generation) &&
                 lifecycle_memory_read(app.lifecycle, app.generation, &sample) &&
                 sample.process == (uint64_t) app.child;
    app_memory_observe(
            record, valid ? &sample : nullptr, lifecycle_now_ns(), (double) time(nullptr));
}
static void memory_process_sample(void) {
    double now = monotonic_ms();
    if (app.child <= 0 || app.stopping) {
        app.memory_process    = (struct app_process_sample) {0};
        app.memory_generation = 0;
        return;
    }
    if (app.memory_generation == app.generation && now - app.memory_process_ms < 2000)
        return;
    app.memory_generation = app.generation;
    app.memory_process_ms = now;
    if (!app_process_read(app.child, &app.memory_process))
        app.memory_process = (struct app_process_sample) {0};
}
static void observation_sample(bool force) {
    if (!app.observation || (!force && monotonic_ms() - app.sample_ms < 2000))
        return;
    app.sample_ms         = monotonic_ms();
    struct perf_record *r = app.observation;
    memory_observe(&r->memory);
    struct app_process_sample sample;
    r->rss               = -1;
    r->memory.rss_age_ms = app.child > 0 ? 0 : -1;
    if (app.child <= 0 || !app_process_read(app.child, &sample))
        return;
    r->rss               = (double) sample.rss;
    r->memory.rss_age_ms = 0;
    if (r->rss > r->peak_rss)
        r->peak_rss = r->rss;
    ++r->samples;
    struct app_hardware h;
    if (app_hardware_read(&h, app.models)) {
        app_resource_update(&app.observation_window, &sample, h.logical_cpus);
        if (app.observation_window.cpu_known) {
            app.cpu_sum += app.observation_window.cpu_percent;
            ++app.cpu_samples;
            r->cpu_percent = app.cpu_sum / app.cpu_samples;
        }
    }
}
/* Cached lifecycle status stays independent of HTTP admission and UI polling. */
void *monitor_main(void *unused) {
    (void) unused;
    while (!atomic_load(&closing)) {
        pthread_mutex_lock(&app.mutex);
        poll_child();
        memory_process_sample();
        observation_sample(false);
        pthread_mutex_unlock(&app.mutex);
        struct timespec pause = {.tv_nsec = 100000000};
        nanosleep(&pause, nullptr);
    }
    return nullptr;
}
void observation_begin(
        struct perf_record *r, const char *source, unsigned max, float temperature, float top_p) {
    memset(r, 0, sizeof *r);
    perf_begin(r);
    r->engine = app.engine;
    memory_generation(r->memory.generation, sizeof r->memory.generation);
    const struct app_model *model = app_model_find(app.active_id);
    snprintf(r->model, sizeof r->model, "%s", app.active_id);
    /* Uncatalogued files have no verified artifact identity and stay diagnostic. */
    snprintf(r->artifact,
             sizeof r->artifact,
             "%s",
             model ? model->sha256 : "unknown-custom-artifact");
    snprintf(r->quantization, sizeof r->quantization, "%s", model ? model->quantization : "");
    snprintf(r->backend, sizeof r->backend, "%s", app.backend);
    snprintf(r->series,
             sizeof r->series,
             "%s",
             model ? app.profile_series : "unknown-custom-series");
    snprintf(r->source, sizeof r->source, "%s", source);
    r->threads     = app.runtime_threads;
    r->max_tokens  = max;
    r->temperature = temperature;
    r->top_p       = top_p;
    r->cold        = app.runtime_requests++ == 0;
    r->load_ns     = r->cold ? app.loaded_ms * 1e6 : -1;
    r->contention  = app.job_running && !app.job_activate;
    begin_activity(&app.request_activity, ACT_CONNECT, app.generation, app.active_id);
    atomic_store(&request_cancelled, false);
    app.observation        = r;
    app.observation_window = (struct app_resource_window) {0};
    app.cpu_samples        = 0;
    app.cpu_sum            = 0;
    observation_sample(true);
}
void observation_end(struct perf_record         *r,
                            int                         rc,
                            double                      first,
                            double                      total,
                            const struct app_run_stats *stats) {
    pthread_mutex_lock(&app.mutex);
    observation_sample(true);
    if (rc == 498 && atomic_load(&request_cancelled))
        rc = 499;
    app.observation    = nullptr;
    r->generation_ns   = stats->generation_ns;
    r->first_ns        = first > 0 ? first * 1e6 : -1;
    r->first_answer_ns = stats->first_answer_ns;
    r->reasoning       = stats->reasoning;
    if (stats->max_tokens)
        r->max_tokens = stats->max_tokens;
    r->total_ns   = total * 1e6;
    r->prefill_ns = stats->prefill_ns;
    r->input      = stats->prompt_tokens;
    r->output     = stats->tokens;
    r->reused     = stats->reused;
    strcpy(r->outcome,
           rc == 499                       ? "cancelled"
           : rc == 498                     ? "disconnected"
           : rc == 422 && stats->no_answer ? "no_answer"
           : rc == 400                     ? "error"
           : rc                            ? "interrupted"
           : stats->no_answer              ? "no_answer"
                                           : "completed");
    strcpy(r->finish, rc ? "unknown" : stats->limited ? "length" : "stop");
    (void) activity_end(&app.request_activity,
                        rc == 499 ? "cancelled"
                        : rc      ? "failed"
                                  : "completed",
                        rc,
                        monotonic_ms());
    pthread_mutex_unlock(&app.mutex);
    perf_submit(r);
    pthread_mutex_lock(&app.mutex);
    restore_measurements();
    /* Prefill is a synchronous engine call. A closed client alone cannot interrupt it.
     * Reap only our owned child before admitting a new request; preserve its diagnostics. */
    if ((rc == 499 || rc == 498 || rc == 502 || rc == 504) && !atomic_load(&closing)) {
        char path[APP_PATH_CAP], id[64], mode[8], log[APP_PATH_CAP], archive[APP_PATH_CAP];
        snprintf(path, sizeof path, "%s", app.chosen);
        snprintf(id, sizeof id, "%s", app.active_id);
        snprintf(mode, sizeof mode, "%s", app.execution_mode);
        begin_activity(&app.load_activity, ACT_STOPPING, app.generation + 1, id);
        stop_child();
        bool saved = false;
        if (path_join(log, app.home, "server.log") &&
            path_join(archive, app.home, "request-failure-XXXXXX")) {
            int fd = mkstemp(archive);
            if (fd >= 0) {
                close(fd);
                saved = rename(log, archive) == 0;
                if (!saved)
                    unlink(archive);
            }
        }
        if (saved)
            (void) start_child_mode(path, id, mode);
        else {
            (void) activity_end(&app.load_activity, "failed", 502, monotonic_ms());
            snprintf(app.message,
                     sizeof app.message,
                     "The model is stopped. Its diagnostics could not be archived.");
        }
    }
    app.generating = false;
    pthread_mutex_unlock(&app.mutex);
}
