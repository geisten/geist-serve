/* routes.c — request dispatch, connection info and the embedded web assets. */
#include "app.h"

#if defined(__has_embed) && !defined(APP_NO_EMBED)
static const unsigned char page[] = {
#embed "../../web/index.html"
};
static const unsigned char style[] = {
#embed "../../web/app.css"
};
static const unsigned char translations[] = {
#embed "../../web/i18n.js"
};
static const unsigned char script[] = {
#embed "../../web/app.js"
};
static const unsigned char marked_js[] = {
#embed "../../web/vendor/marked.umd.js"
};
static const unsigned char katex_js[] = {
#embed "../../web/vendor/katex.min.js"
};
static const unsigned char markdown_js[] = {
#embed "../../web/markdown.js"
};
#else
/* GCC 14 supports the C23 language used here but not #embed yet. */
#include "../../build/app_assets.h"
#endif

void connections(int fd, bool models) {
    char              body[4096];
    struct app_buffer b = {.data = body, .cap = sizeof body};
    pthread_mutex_lock(&app.mutex);
    if (models) {
        app_put(&b, "{\"object\":\"list\",\"data\":[");
        if (app.child.ready) {
            app_put(&b, "{\"id\":");
            app_quote(&b, app.child.active_id);
            app_put(&b,
                    ",\"object\":\"model\",\"created\":0,\"owned_by\":\"local\",\"context_window\":"
                    "4096,\"capabilities\":{\"chat\":true,\"tools\":false,\"vision\":false}}");
        }
        app_put(&b, "]}");
    } else {
        app_printf(&b, "{\"base_url\":\"http://127.0.0.1:%u/v1\",\"api_key\":", app.port);
        app_quote(&b, app.token);
        app_put(&b, ",\"model\":");
        app_quote(&b, app.child.pid > 0 ? app.child.active_id : "");
        app_printf(&b,
                   ",\"ready\":%s,\"daemon_pid\":%ld,\"context_tokens\":4096,\"max_output_tokens\":"
                   "4095,\"chat\":true,\"tools\":false,\"quality\":\"unverified\"}",
                   app.child.ready ? "true" : "false",
                   (long) app.child.pid);
    }
    pthread_mutex_unlock(&app.mutex);
    if (b.failed)
        api_error(fd, 503, "Connection description exceeds capacity.");
    else
        response(fd, 200, "application/json", body, b.len);
}

void handle(int fd, struct app_arena *arena) {
    struct request r      = {};
    int            status = read_request(fd, arena, &r);
    if (status) {
        error_response(fd, status, "Invalid, oversized or non-local request.");
        return;
    }
    if (strcmp(r.method, "GET") == 0) {
        if (strcmp(r.path, "/") == 0) {
            response(fd, 200, "text/html; charset=utf-8", page, sizeof page);
            return;
        }
        if (strcmp(r.path, "/app.css") == 0) {
            response(fd, 200, "text/css; charset=utf-8", style, sizeof style);
            return;
        }
        if (strcmp(r.path, "/i18n.js") == 0) {
            response(fd, 200, "text/javascript; charset=utf-8", translations, sizeof translations);
            return;
        }
        if (strcmp(r.path, "/marked.js") == 0) {
            response(fd, 200, "text/javascript; charset=utf-8", marked_js, sizeof marked_js);
            return;
        }
        if (strcmp(r.path, "/markdown.js") == 0) {
            response(fd, 200, "text/javascript; charset=utf-8", markdown_js, sizeof markdown_js);
            return;
        }
        if (strcmp(r.path, "/katex.js") == 0) {
            response(fd, 200, "text/javascript; charset=utf-8", katex_js, sizeof katex_js);
            return;
        }
        if (strcmp(r.path, "/app.js") == 0) {
            response(fd, 200, "text/javascript; charset=utf-8", script, sizeof script);
            return;
        }
        if (strcmp(r.path, "/health") == 0) {
            response(fd, 200, "application/json", "{\"app\":\"geist\"}", 15);
            return;
        }
    }
    if (!authorized(&r)) {
        if (!strncmp(r.path, "/v1/", 4)) {
            api_error(fd, 401, "A valid local geisten API key is required.");
            return;
        }
        error_response(fd, 403, "Open the private app link supplied by the launcher.");
        return;
    }
    if (!strcmp(r.method, "GET") && !strcmp(r.path, "/v1/models")) {
        connections(fd, true);
        return;
    }
    if (!strcmp(r.method, "GET") && !strcmp(r.path, "/app/connections")) {
        connections(fd, false);
        return;
    }
    if (!strcmp(r.method, "POST") && !strcmp(r.path, "/v1/chat/completions")) {
        completions(fd, &r, arena);
        return;
    }
    if (!strncmp(r.path, "/v1/", 4)) {
        api_error(fd, 404, "Unsupported endpoint. Use /v1/models or /v1/chat/completions.");
        return;
    }
    if (strcmp(r.method, "GET") == 0 && strcmp(r.path, "/app/status") == 0) {
        status_response(fd, arena);
        return;
    }
    if (!strcmp(r.path, "/app/performance/compare") && !strcmp(r.method, "POST")) {
        comparison_start(fd, r.body, arena);
        return;
    }
    if (!strcmp(r.path, "/app/performance/cancel") && !strcmp(r.method, "POST")) {
        atomic_store(&compare_cancelled, true);
        response(fd, 200, "application/json", "{}", 2);
        return;
    }
    if (!strcmp(r.path, "/app/performance/export") && !strcmp(r.method, "GET")) {
        size_t length = 0;
        char  *data   = perf_export(&length);
        if (data) {
            response(fd, 200, "application/x-ndjson", data, length);
            free(data);
        } else
            error_response(fd, 503, "History export exceeds the memory budget.");
        return;
    }
    if ((!strcmp(r.path, "/app/performance/settings") ||
         !strcmp(r.path, "/app/performance/clear")) &&
        !strcmp(r.method, "POST")) {
        struct json *j = app_alloc(arena, 1, sizeof *j, _Alignof(struct json));
        if (!j || json_parse(j, strlen(r.body), r.body) < 0) {
            error_response(fd, 400, "Invalid history settings.");
            return;
        }
        bool ok = false;
        if (!strcmp(r.path, "/app/performance/clear")) {
            if (!json_bool(j, json_get(j, 0, "confirm"), false)) {
                error_response(fd, 400, "Confirm deleting local measurement history.");
                return;
            }
            ok = perf_clear();
            pthread_mutex_lock(&app.mutex);
            restore_measurements();
            pthread_mutex_unlock(&app.mutex);
        } else {
            double days    = json_num(j, json_get(j, 0, "days"), 0);
            int    enabled = json_get(j, 0, "enabled");
            bool   boolean = enabled >= 0 && j->tok[enabled].type == JSMN_PRIMITIVE &&
                             ((j->tok[enabled].end - j->tok[enabled].start == 4 &&
                               !memcmp(r.body + j->tok[enabled].start, "true", 4)) ||
                              (j->tok[enabled].end - j->tok[enabled].start == 5 &&
                               !memcmp(r.body + j->tok[enabled].start, "false", 5)));
            if (!boolean || (days != 30 && days != 90 && days != 365)) {
                error_response(fd, 400, "Choose 30, 90 or 365 days.");
                return;
            }
            ok = perf_settings(json_bool(j, enabled, false), (unsigned) days);
        }
        if (ok)
            response(fd, 200, "application/json", "{}", 2);
        else
            error_response(fd, 503, "History could not be saved.");
        return;
    }
    if (!strcmp(r.path, "/app/catalog")) {
        if (!strcmp(r.method, "GET")) {
            pthread_mutex_lock(&app.mutex);
            response(fd, 200, "application/json", app_catalog_json, strlen(app_catalog_json));
            pthread_mutex_unlock(&app.mutex);
        } else if (!strcmp(r.method, "POST"))
            import_catalog(fd, r.body);
        else
            error_response(fd, 405, "Use GET or POST.");
        return;
    }
    if (!strcmp(r.path, "/app/execution")) {
        if (strcmp(r.method, "POST"))
            error_response(fd, 405, "Use POST.");
        else
            execution_response(fd, r.body);
        return;
    }
    if (!strcmp(r.path, "/app/tasks") && !strcmp(r.method, "GET")) {
        const char *catalog = app_tasks_json();
        response(fd, 200, "application/json", catalog, strlen(catalog));
        return;
    }
    if (strcmp(r.method, "POST") != 0) {
        error_response(fd, 405, "POST required.");
        return;
    }
    if (strcmp(r.path, "/app/generate") == 0) {
        generate(fd, &r, arena);
        return;
    }
    if (!strcmp(r.path, "/app/activity/cancel")) {
        struct json *j          = app_alloc(arena, 1, sizeof *j, _Alignof(struct json));
        bool         parsed     = j && json_parse(j, strlen(r.body), r.body) >= 0;
        double       id         = parsed ? json_num(j, json_get(j, 0, "id"), 0) : 0;
        double       generation = parsed ? json_num(j, json_get(j, 0, "generation"), -1) : -1;
        char        *instance   = parsed ? json_strdup(j, json_get(j, 0, "instance")) : nullptr;
        pthread_mutex_lock(&app.mutex);
        struct activity *a = app.child.generating ? &app.activity.request : &app.activity.load;
        bool valid = instance && !strcmp(instance, app.instance) &&
                     generation == (double) a->generation && id > 0 && id == (double) a->id &&
                     !a->outcome[0] &&
                     (app.child.generating || app.job.running || (app.child.pid && !app.child.ready));
        if (valid) {
            activity_change(a, ACT_STOPPING);
            if (app.child.generating) {
                atomic_store(&request_cancelled, true);
                if (app.compare.running)
                    atomic_store(&compare_cancelled, true);
            } else {
                atomic_store(&load_cancelled, true);
                if (app.job.activate)
                    atomic_store(&cancelled, true);
            }
        }
        pthread_mutex_unlock(&app.mutex);
        free(instance);
        if (valid)
            response(fd, 202, "application/json", "{}", 2);
        else
            error_response(fd, 409, "This operation is no longer active.");
        return;
    }
    if (strcmp(r.path, "/app/cancel") == 0) {
        atomic_store(&cancelled, true);
        response(fd, 200, "application/json", "{}", 2);
        return;
    }
    if (strcmp(r.path, "/app/quit-if-idle") == 0) {
        pthread_mutex_lock(&app.mutex);
        bool busy = app.compare.running || app.child.generating || app.job.running || (app.child.pid && !app.child.ready);
        if (!busy)
            atomic_store(&closing, true);
        pthread_mutex_unlock(&app.mutex);
        if (busy)
            error_response(fd, 409, "Finish the current task before updating geisten.");
        else
            response(fd, 202, "application/json", "{}", 2);
        return;
    }
    if (strcmp(r.path, "/app/quit") == 0) {
        atomic_store(&closing, true);
        response(fd, 202, "application/json", "{}", 2);
        return;
    }
    if (strcmp(r.path, "/app/stop") == 0) {
        pthread_mutex_lock(&app.mutex);
        if (app.stopping || app.compare.running || app.child.generating || app.job.running) {
            pthread_mutex_unlock(&app.mutex);
            error_response(fd, 409, "Stop the current task first.");
            return;
        }
        stop_child();
        app.child.active[0]    = 0;
        app.child.active_id[0] = 0;
        app.message[0] = app.backend.notice[0] = 0; /* nothing they describe remains (#82) */
        pthread_mutex_unlock(&app.mutex);
        response(fd, 200, "application/json", "{}", 2);
        return;
    }
    bool setup       = strcmp(r.path, "/app/setup") == 0;
    bool preview     = strcmp(r.path, "/app/preview") == 0;
    bool preferences = strcmp(r.path, "/app/preferences") == 0;
    bool download    = strcmp(r.path, "/app/download") == 0;
    bool remove      = strcmp(r.path, "/app/remove") == 0;
    if (!setup && !preview && !preferences && !download && !remove &&
        strcmp(r.path, "/app/select") != 0) {
        error_response(fd, 404, "Unknown action.");
        return;
    }
    struct json *json = app_alloc(arena, 1, sizeof *json, _Alignof(struct json));
    if (!json) {
        error_response(fd, 503, "Request memory budget exhausted.");
        return;
    }
    if (json_parse(json, strlen(r.body), r.body) < 0) {
        error_response(fd, 400, "Invalid JSON.");
        return;
    }
    if (preferences) {
        char *language = json_strdup(json, json_get(json, 0, "language"));
        bool  valid    = language && (!strcmp(language, "de") || !strcmp(language, "en"));
        pthread_mutex_lock(&app.mutex);
        bool ok = valid && save_preference("answer-language", language);
        if (ok)
            snprintf(app.prefs.answer_language, sizeof app.prefs.answer_language, "%s", language);
        pthread_mutex_unlock(&app.mutex);
        free(language);
        if (!valid)
            error_response(fd, 400, "Choose English or German.");
        else if (!ok)
            error_response(fd, 500, "Cannot save language preference.");
        else
            response(fd, 200, "application/json", "{}", 2);
        return;
    }
    char *id = json_strdup(json, json_get(json, 0, "id"));
    pthread_mutex_lock(&app.mutex);
    const struct app_model *model = app_model_find(id);
    free(id);
    if (!model) {
        pthread_mutex_unlock(&app.mutex);
        error_response(fd, 400, "Choose a model from the catalog.");
        return;
    }
    /* Consent and another artifact's download do not mutate the resident
     * runtime. Selection/removal/setup retain their exclusive boundary. */
    bool background = download && app.child.pid > 0 && strcmp(model->id, app.child.active_id);
    if (atomic_load(&closing) || app.stopping || app.compare.running || (!preview && app.job.running) ||
        (app.child.generating && !preview && !background) ||
        (download && app.child.pid > 0 && !strcmp(model->id, app.child.active_id))) {
        pthread_mutex_unlock(&app.mutex);
        error_response(fd, 409, "Another task is active.");
        return;
    }
    if (preview) {
        bool consent = json_bool(json, json_get(json, 0, "experimental"), false);
        char key[80];
        snprintf(key, sizeof key, "preview-%s", model->sha256);
        bool ok = consent && save_preference(key, "v1");
        if (ok)
            app.prefs.preview_accepted[model - app_models] = true;
        pthread_mutex_unlock(&app.mutex);
        if (!consent)
            error_response(fd, 400, "Preview consent must be explicit.");
        else if (!ok)
            error_response(fd, 500, "Cannot save preview consent.");
        else
            response(fd, 200, "application/json", "{}", 2);
        return;
    }
    char path[APP_PATH_CAP], part[APP_PATH_CAP];
    bool valid     = path_join(path, app.paths.models, model->file);
    bool installed = valid && regular_size(path) == model->bytes;
    if (remove) {
        if (!valid) {
            pthread_mutex_unlock(&app.mutex);
            error_response(fd, 409, "Cannot remove this download safely.");
            return;
        }
        // Catalog filenames only; never follow a replaced directory or a symlink.
        int         directory = open(app.paths.models, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        char        partial_name[256];
        bool        safe    = directory >= 0 &&
                              snprintf(partial_name, sizeof partial_name, "%s.part", model->file) <
                                      (int) sizeof partial_name;
        const char *names[] = {model->file, partial_name};
        for (unsigned i = 0; safe && i < 2; ++i) {
            struct stat info;
            if (fstatat(directory, names[i], &info, AT_SYMLINK_NOFOLLOW) == 0)
                safe = S_ISREG(info.st_mode);
            else
                safe = errno == ENOENT;
        }
        /* Deletion is explicit. Validate paths before stopping the owned model,
         * and hold the same lock through stop/removal so no client can start a
         * new generation between those operations. Busy requests are rejected
         * above. The catalog entry itself is immutable and remains available. */
        if (safe && app.child.pid > 0 &&
            (!strcmp(app.child.active_id, model->id) || !strcmp(app.child.chosen, path)))
            stop_child();
        for (unsigned i = 0; safe && i < 2; ++i)
            if (unlinkat(directory, names[i], 0) != 0 && errno != ENOENT)
                safe = false;
        if (directory >= 0)
            close(directory);
        if (safe && !strcmp(app.prefs.selected, model->id))
            safe = save_selection("");
        if (safe) {
            if (!strcmp(app.child.active_id, model->id)) {
                app.child.active_id[0] = app.child.active[0] = 0;
                app.backend.notice[0] = 0;
            }
            app.message[0] = 0; /* e.g. "Partial downloads can be resumed" for this file (#82) */
        }
        pthread_mutex_unlock(&app.mutex);
        if (safe)
            response(fd, 200, "application/json", "{}", 2);
        else
            error_response(fd, 409, "Cannot remove this download safely.");
        return;
    }
    /* A live identical model needs no replacement process. Still validate the
     * trusted receipt stamp: replacing or modifying its file invalidates this
     * no-op just as it invalidates the normal cached loading path. */
    if (!download && installed && app.child.ready && !strcmp(app.child.active_id, model->id) &&
        !strcmp(app.child.chosen, path)) {
        char key[80], stamp[512], receipt[512];
        snprintf(key, sizeof key, "verified-%s", model->sha256);
        if (model_stamp(path, model, stamp) && read_preference(key, receipt, sizeof receipt) &&
            !strcmp(stamp, receipt)) {
            pthread_mutex_unlock(&app.mutex);
            response(fd, 200, "application/json", "{}", 2);
            return;
        }
    }
    struct app_hardware h;
    bool                known = app_hardware_read(&h, app.paths.models);
    if (valid && snprintf(part, sizeof part, "%s.part", path) < (int) sizeof part) {
        uint64_t partial = regular_size(part);
        if (partial <= model->bytes && UINT64_MAX - h.disk > partial)
            h.disk += partial;
    }
    if (setup) {
        struct app_inventory inventory[APP_MODEL_COUNT];
        model_inventory(inventory);
        /* Re-evaluate resources on the server. Never silently accept a different
         * model from the one whose download/preview the user just approved. */
        struct app_hardware       current;
        bool                      current_known = app_hardware_read(&current, app.paths.models);
        struct app_recommendation choice        = app_recommend(
                &current, inventory, app.prefs.selected, app.child.ready ? app.child.active_id : nullptr);
        if (!current_known || !choice.eligible || choice.model != model) {
            pthread_mutex_unlock(&app.mutex);
            error_response(
                    fd, 409, "The platform check changed. Review the setup suggestion and retry.");
            return;
        }
        download = !installed;
    }
    struct app_assessment assessment = app_assess(&h, model, installed && !download);
    if (!known || assessment.fit == APP_UNAVAILABLE || (!download && !installed)) {
        pthread_mutex_unlock(&app.mutex);
        error_response(fd,
                       409,
                       !known ? "Cannot read this computer's resources."
                              : (assessment.fit == APP_UNAVAILABLE ? assessment.reason
                                                                   : "Download this model first."));
        return;
    }
    bool activate = !download || app.child.pid <= 0;
    /* With a model running, the choice is saved only once the new one has
     * started (jobs.c): a failed or cancelled switch keeps the working model
     * for the next launch (#82). Without one, a cancelled download stays a
     * resumable choice. */
    bool ok = (!activate || app.child.pid > 0 || save_selection(model->id)) &&
              begin_job(model, download, activate);
    pthread_mutex_unlock(&app.mutex);
    if (ok)
        response(fd, 202, "application/json", "{}", 2);
    else
        error_response(fd, 503, "Cannot start model worker.");
}
