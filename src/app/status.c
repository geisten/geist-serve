/* status.c — status JSON, execution-mode switching, catalog import. */
#include "app.h"

void execution_response(int fd, const char *text) {
    struct json *j    = calloc(1, sizeof *j);
    char        *mode = nullptr;
    if (j && json_parse(j, strlen(text), text) >= 0)
        mode = json_strdup(j, json_get(j, 0, "mode"));
    free(j);
    if (!mode || (strcmp(mode, "auto") && strcmp(mode, "cpu") && strcmp(mode, "gpu"))) {
        free(mode);
        error_response(fd, 400, "Choose Auto, CPU or GPU.");
        return;
    }
    pthread_mutex_lock(&app.mutex);
    const struct app_model *model = app_model_find(app.active_id);
    int                     code  = 202;
    const char             *error = nullptr;
    if (!app.ready || app.comparing || app.generating || app.job_running) {
        code  = 409;
        error = "Wait until the loaded model is idle before changing execution.";
    } else if (!strcmp(mode, "gpu") && !gpu_supported(model)) {
        code  = 409;
        error = "GPU is not supported by this model and packaged engine.";
    } else {
        bool        gpu = !strcmp(mode, "gpu") || (!strcmp(mode, "auto") && recommend_gpu(model));
        const char *backend = gpu ? app.gpu_backend : app.cpu_backend;
        if (!strcmp(backend, app.backend)) {
            char key[80];
            if (model)
                snprintf(key, sizeof key, "backend-%s", model->sha256);
            if (model && !save_preference(key, mode)) {
                code  = 500;
                error = "Cannot save execution preference.";
            } else {
                snprintf(app.execution_mode, sizeof app.execution_mode, "%s", mode);
                code = 200;
            }
        } else {
            char path[APP_PATH_CAP], id[64];
            snprintf(path, sizeof path, "%s", app.chosen);
            snprintf(id, sizeof id, "%s", app.active_id);
            app.execution_notice[0] = 0;
            if (model)
                memset(&app.measurements[model - app_models], 0, sizeof app.measurements[0]);
            if (!start_child_mode(path, id, mode)) {
                code  = 500;
                error = "Cannot change execution. Restoring CPU.";
                (void) start_child_mode(path, id, "cpu");
            }
        }
    }
    pthread_mutex_unlock(&app.mutex);
    free(mode);
    if (error)
        error_response(fd, code, error);
    else
        response(fd, code, "application/json", "{}", 2);
}
void import_catalog(int fd, const char *text) {
    char                why[256];
    struct app_catalog *candidate = app_catalog_parse(text, why);
    if (!candidate) {
        error_response(fd, 400, why);
        return;
    }
    pthread_mutex_lock(&app.mutex);
    int code = 200;
    if (app.comparing || app.job_running || app.generating || (app.child && !app.ready)) {
        code = 409;
        snprintf(why, sizeof why, "Finish the current operation before importing a catalog.");
    } else if (app_catalog_version(candidate) <= app_catalog_revision) {
        code = 409;
        snprintf(why, sizeof why, "Import a catalog with a newer revision.");
    } else {
        const struct app_model *old  = app_model_find(app.active_id);
        const struct app_model *next = app_catalog_find(candidate, app.active_id);
        if (app.ready && old &&
            (!next || strcmp(old->sha256, next->sha256) || strcmp(old->file, next->file) ||
             old->backends != next->backends || old->bytes != next->bytes)) {
            code = 409;
            snprintf(why, sizeof why, "The running model must stay unchanged in this catalog.");
        }
        for (size_t i = 0; code == 200 && i < app_model_count; ++i) {
            const struct app_model *previous = &app_models[i];
            /* A filename may not be reassigned to another hash while files exist. */
            char path[APP_PATH_CAP], part[APP_PATH_CAP];
            for (size_t k = 0; app_catalog_entry(candidate, k); ++k) {
                const struct app_model *replacement = app_catalog_entry(candidate, k);
                if (replacement && !strcmp(previous->file, replacement->file) &&
                    (strcmp(previous->sha256, replacement->sha256) ||
                     previous->bytes != replacement->bytes) &&
                    path_join(path, app.models, previous->file) &&
                    (regular_size(path) ||
                     (snprintf(part, sizeof part, "%s.part", path) < (int) sizeof part &&
                      regular_size(part)))) {
                    code = 409;
                    snprintf(why,
                             sizeof why,
                             "Use a new filename when replacing an existing model hash.");
                    break;
                }
            }
        }
    }
    if (code == 200 && !save_preference("catalog.json", text)) {
        code = 500;
        snprintf(why, sizeof why, "Cannot save the catalog. The previous catalog is kept.");
    }
    if (code == 200) {
        app_catalog_apply(candidate);
        candidate = nullptr;
        memset(app.measurements, 0, sizeof app.measurements);
        restore_preview_preferences();
        restore_measurements();
        if (!app_model_find(app.selected))
            app.selected[0] = 0;
    }
    pthread_mutex_unlock(&app.mutex);
    app_catalog_discard(candidate);
    if (code == 200)
        response(fd, 200, "application/json", "{}", 2);
    else
        error_response(fd, code, why);
}

static struct app_resource_window resource_window; /* protected by app.mutex */

void status_response(int fd, struct app_arena *arena) {
    char *body = app_alloc(arena, 65536, 1, 1);
    if (!body) {
        error_response(fd, 503, "Request memory budget exhausted.");
        return;
    }
    struct app_buffer   b = {.data = body, .cap = 65536};
    struct app_hardware h;
    bool                known = app_hardware_read(&h, app.models);
    pthread_mutex_lock(&app.mutex);
    struct app_inventory inventory[APP_MODEL_COUNT];
    model_inventory(inventory);
    struct app_recommendation recommendation =
            app_recommend(&h, inventory, app.selected, app.ready ? app.active_id : nullptr);
    app_put(&b, "{\"version\":");
    app_quote(&b, APP_VERSION);
    app_printf(&b, ",\"catalog_revision\":%u", app_catalog_revision);
    app_put(&b, ",\"recommendation\":{\"id\":");
    app_quote(&b, recommendation.model ? recommendation.model->id : "");
    app_put(&b, ",\"preferred_id\":");
    app_quote(&b, recommendation.preferred->id);
    app_put(&b, ",\"source\":");
    app_quote(&b, recommendation.source);
    app_put(&b, ",\"reason\":");
    app_quote(&b, recommendation.reason);
    app_printf(&b,
               ",\"eligible\":%s},\"answer_language\":",
               recommendation.eligible ? "true" : "false");
    app_quote(&b, app.answer_language);
    const struct app_model *execution_model = app_model_find(app.active_id);
    bool                    gpu             = gpu_supported(execution_model);
    app_put(&b, ",\"engine\":");
    app_engine_json(&b, &app.engine);
    app_put(&b, ",\"activity\":{\"instance\":");
    app_quote(&b, app.instance);
    app_put(&b, ",\"load\":");
    activity_json(&b,
                  &app.load_activity,
                  monotonic_ms(),
                  app.child && app.load_activity.generation == app.generation);
    app_put(&b, ",\"request\":");
    activity_json(&b,
                  &app.request_activity,
                  monotonic_ms(),
                  app.child && app.request_activity.generation == app.generation);
    app_put(&b, ",\"download\":");
    activity_json(&b, &app.download_activity, monotonic_ms(), false);
    app_printf(&b, "},\"process_generation\":%llu", (unsigned long long) app.generation);
    app_put(&b, ",\"lifecycle\":{");
    app_printf(&b,
               "\"generation\":%llu,\"pid\":%ld,\"previous_pid\":%ld,\"spawned_ms\":%.3f,\"reaped_"
               "ms\":%.3f,\"receipt\":",
               (unsigned long long) app.generation,
               (long) app.child,
               (long) app.previous_pid,
               app.spawned_ms,
               app.reaped_ms);
    app_quote(&b, app.receipt_checked ? (app.receipt_hit ? "hit" : "miss") : "not_checked");
    app_printf(&b,
               ",\"verified_bytes\":%llu,\"engine_phases\":[",
               (unsigned long long) app.verified_bytes);
    bool phase_comma = false;
    for (unsigned i = LC_BACKEND; i < LC_PHASES; i++) {
        uint64_t start = app.lifecycle_snapshot.phase_ns[i];
        if (!start)
            continue;
        if (phase_comma)
            app_put(&b, ",");
        phase_comma  = true;
        uint64_t end = i == LC_READY ? start : lifecycle_now_ns();
        for (unsigned j = i + 1; j < LC_PHASES; j++)
            if (app.lifecycle_snapshot.phase_ns[j]) {
                end = app.lifecycle_snapshot.phase_ns[j];
                break;
            }
        app_put(&b, "{\"stage\":");
        app_quote(&b, lifecycle_name(i));
        app_printf(&b,
                   ",\"started_ms\":%.3f,\"duration_ms\":%.3f}",
                   (double) start / 1e6,
                   (double) (end - start) / 1e6);
    }
    app_put(&b, "]}");
    app_put(&b, ",\"request_phase\":");
    app_quote(&b, app.generating ? app.request_phase : "idle");
    app_put(&b, ",\"last_error\":{\"message\":");
    app_quote(&b, app.last_error);
    app_put(&b, ",\"stage\":");
    app_quote(&b, app.error_stage);
    app_put(&b, ",\"model\":");
    app_quote(&b, app.error_model);
    app_put(&b, ",\"backend\":");
    app_quote(&b, app.error_backend);
    app_printf(&b, ",\"code\":%d}", app.error_code);
    app_put(&b, ",\"execution\":{\"mode\":");
    app_quote(&b, app.execution_mode[0] ? app.execution_mode : "auto");
    app_put(&b, ",\"active\":");
    app_quote(&b, app.ready ? (!strcmp(app.backend, app.cpu_backend) ? "cpu" : "gpu") : "");
    app_put(&b, ",\"backend\":");
    app_quote(&b, app.ready ? app.backend : "");
    app_put(&b, ",\"recommended\":");
    app_quote(&b, recommend_gpu(execution_model) ? "gpu" : "cpu");
    app_printf(&b,
               ",\"basis\":\"hardware\",\"verified\":%s,\"gpu_available\":%s,\"gpu_backend\":",
               app.ready && app.backend_verified ? "true" : "false",
               gpu ? "true" : "false");
    app_quote(&b, app.gpu_backend);
    app_put(&b, ",\"reason\":");
    app_quote(&b,
              !gpu ? "GPU is not supported by this model and packaged engine."
              : recommend_gpu(execution_model)
                      ? "GPU is suggested for this larger model. This is a hardware default, not a "
                        "measured speed comparison."
                      : "CPU is suggested for this small model. This is a hardware default, not a "
                        "measured speed comparison.");
    app_put(&b, ",\"notice\":");
    app_quote(&b, app.execution_notice);
    double execution_rate = 0;
    if (app.ready && app.backend_verified && execution_model) {
        unsigned slot  = !strcmp(app.backend, app.cpu_backend) ? 0 : 1;
        execution_rate = app.history[execution_model - app_models][slot].rate;
    }
    app_printf(&b,
               ",\"performance\":{\"target_tps\":%.1f,\"below_target\":%s,\"rate\":",
               APP_INTERACTIVE_TPS,
               app_rate_below_target(execution_rate) ? "true" : "false");
    if (isfinite(execution_rate) && execution_rate > 0)
        app_printf(&b, "%.6f", execution_rate);
    else
        app_put(&b, "null");
    app_put(&b, "}}");
    app_printf(&b,
               ",\"comparison\":{\"running\":%s,\"step\":%u,\"phase\":",
               app.comparing ? "true" : "false",
               app.compare_step);
    app_quote(&b, app.compare_phase);
    app_put(&b, ",\"result\":");
    app_quote(&b, app.compare_result);
    app_put(&b, "}");
    app_put(&b, ",\"performance_history\":[");
    bool comma = false;
    if (execution_model) {
        for (unsigned slot = 0; slot < 2; ++slot) {
            const struct processor_measurement *m =
                    &app.history[execution_model - app_models][slot];
            if (!m->recorded)
                continue;
            if (comma)
                app_put(&b, ",");
            comma = true;
            app_printf(&b, "{\"processor\":\"%s\",\"backend\":", slot ? "gpu" : "cpu");
            app_quote(&b, slot ? app.gpu_backend : app.cpu_backend);
            app_printf(&b,
                       ",\"rate\":%.6f,\"first\":%.6f,\"total\":%.6f,\"tokens\":%.0f,"
                       "\"rss_bytes\":%.0f,\"recorded_at\":%.0f}",
                       m->rate,
                       m->first,
                       m->total,
                       m->tokens,
                       m->rss,
                       m->recorded);
        }
    }
    app_put(&b, "]");
    app_put(&b, ",\"hardware\":{\"name\":");
    app_quote(&b, h.name);
    app_put(&b, ",\"arch\":");
    app_quote(&b, h.arch);
    app_put(&b, ",\"os\":");
    app_quote(&b, h.os);
    app_printf(&b, ",\"logical_cpus\":%u", h.logical_cpus);
    app_put(&b, ",\"device\":");
    app_quote(&b,
              h.device == APP_APPLE_SILICON ? "apple-silicon"
              : h.device == APP_PI5         ? "pi5"
                                            : "unknown");
    app_printf(&b,
               ",\"ram\":%llu,\"available\":%llu,\"disk\":%llu,\"cores\":%u,\"known\":%s,\"disk_"
               "known\":%s,\"available_known\":%s},",
               (unsigned long long) h.ram,
               (unsigned long long) h.available,
               (unsigned long long) h.disk,
               h.cores,
               known ? "true" : "false",
               h.disk_known ? "true" : "false",
               h.available_known ? "true" : "false");
    struct app_process_sample sample;
    bool                      sampled = app.child > 0 && app_process_read(app.child, &sample);
    app_resource_update(&resource_window, sampled ? &sample : nullptr, h.logical_cpus);
    app_put(&b, "\"resources\":{\"scope\":\"geistd\",\"rss_bytes\":");
    if (sampled)
        app_printf(&b, "%llu", (unsigned long long) sample.rss);
    else
        app_put(&b, "null");
    app_put(&b, ",\"cpu_percent\":");
    if (resource_window.cpu_known)
        app_printf(&b, "%.2f", resource_window.cpu_percent);
    else
        app_put(&b, "null");
    app_printf(&b, ",\"cpu_interval_ms\":%.0f},", resource_window.interval_ms);
    struct app_memory_record live_memory;
    app_memory_reset(&live_memory);
    if (app.child > 0 && !app.stopping)
        memory_generation(live_memory.generation, sizeof live_memory.generation);
    memory_observe(&live_memory);
    double live_rss        = app.child > 0 && app.memory_generation == app.generation &&
                                             app.memory_process.pid == app.child &&
                                             monotonic_ms() - app.memory_process_ms <= 6000
                                     ? (double) app.memory_process.rss
                                     : -1;
    live_memory.rss_age_ms = app.child > 0 && app.memory_generation == app.generation
                                     ? monotonic_ms() - app.memory_process_ms
                                     : -1;
    app_put(&b, "\"memory\":");
    app_memory_json(&b, &live_memory, live_rss, -1, live_rss >= 0 ? 1 : 0);
    app_put(&b, ",");
    app_put(&b, "\"runtime\":\"geistd\",\"active\":");
    app_quote(&b, app.active);
    app_put(&b, ",\"active_id\":");
    app_quote(&b, app.active_id);
    app_put(&b, ",\"message\":");
    app_quote(&b, app.message);
    app_put(&b, ",\"phase\":");
    app_quote(&b, app.phase);
    app_put(&b, ",\"job_model\":");
    app_quote(&b, app.job_running ? app.job_model->id : "");
    app_printf(&b,
               ",\"received\":%llu,\"ready\":%s,\"loading\":%s,\"busy\":%s,"
               "\"inference_busy\":%s,\"background_download\":%s,\"models\":[",
               (unsigned long long) app.received,
               app.ready ? "true" : "false",
               app.child && !app.ready ? "true" : "false",
               app.comparing || app.job_running || app.generating ? "true" : "false",
               app.comparing || app.generating || (app.job_running && app.job_activate) ||
                               (app.child && !app.ready)
                       ? "true"
                       : "false",
               app.job_running && !app.job_activate ? "true" : "false");
    for (size_t i = 0; i < app_model_count; ++i) {
        const struct app_model *m         = &app_models[i];
        bool                    installed = inventory[i].installed;
        uint64_t                partial   = inventory[i].partial;
        struct app_hardware     adjusted  = h;
        if (partial <= m->bytes && h.disk_known && UINT64_MAX - adjusted.disk > partial)
            adjusted.disk += partial;
        /* Neither process RSS nor Metal allocation guarantees reclaimable
         * capacity on shared memory. Assess against current availability. */
        struct app_assessment a = app_assess_device(&adjusted,
                                                    m,
                                                    installed,
                                                    app.history[i][0].rate,
                                                    gpu_supported(m),
                                                    app.history[i][1].rate);
        if (i)
            app_put(&b, ",");
        app_put(&b, "{\"id\":");
        app_quote(&b, m->id);
        app_put(&b, ",\"name\":");
        app_quote(&b, m->name);
        app_put(&b, ",\"group_id\":");
        app_quote(&b, m->group_id);
        app_put(&b, ",\"group_name\":");
        app_quote(&b, m->group_name);
        app_put(&b, ",\"quantization\":");
        app_quote(&b, m->quantization ? m->quantization : "");
        app_put(&b, ",\"sha256\":");
        app_quote(&b, m->sha256);
        app_printf(&b,
                   ",\"bytes\":%llu,\"ram_gib\":%u,\"working_mib\":%u,\"installed\":%s,\"partial\":"
                   "%llu,\"resource_fit\":%d,\"fit\":%d,\"quality\":\"unverified\",\"reason\":",
                   (unsigned long long) m->bytes,
                   m->recommended_ram_gib,
                   m->working_mib,
                   installed ? "true" : "false",
                   (unsigned long long) partial,
                   a.fit,
                   app_task_fit(a.fit, APP_QUALITY_UNVERIFIED));
        app_quote(&b, a.reason);
        /* Only modalities implemented by the bundled service are advertised. */
        app_put(&b,
                ",\"capabilities\":{\"chat\":true,\"vision\":false,"
                "\"speech_recognition\":false}");
        app_printf(&b, ",\"preview_accepted\":%s", app.preview_accepted[i] ? "true" : "false");
        app_put(&b, ",\"performance\":");
        app_quote(&b, a.performance);
        app_printf(&b,
                   ",\"measured_tps\":%.3f,\"measured_tokens\":%u}",
                   app.measurements[i].tps,
                   app.measurements[i].tokens);
    }
    char artifact[65] = "";
    if (execution_model)
        snprintf(artifact, sizeof artifact, "%s", execution_model->sha256);
    app_put(&b, "],\"performance_profile\":");
    pthread_mutex_unlock(&app.mutex);
    perf_view(&b, artifact, app.profile_series, app.cpu_backend, app.gpu_backend);
    app_put(&b, "}");
    if (b.failed)
        error_response(fd, 503, "Status exceeds the response memory budget.");
    else
        response(fd, 200, "application/json", body, b.len);
}
