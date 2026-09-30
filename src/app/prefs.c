/* prefs.c — preferences on disk, per-model measurements and their pre-0.5 migration, the selected model. */
#include "app.h"

/* Only fixed application keys are passed here. Atomic private files never
 * contain prompts, output or capabilities. Callers hold app.mutex after startup. */
bool read_preference(const char *name, char *out, size_t cap) {
    char path[APP_PATH_CAP];
    if (!path_join(path, app.paths.home, name))
        return false;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return false;
    struct stat st;
    bool        ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == getuid() &&
                     st.st_nlink == 1 && st.st_size > 0 && (uint64_t) st.st_size < cap;
    ssize_t     n  = ok ? read(fd, out, cap - 1) : -1;
    close(fd);
    if (n <= 0 || n != st.st_size || memchr(out, 0, (size_t) n)) {
        out[0] = 0;
        return false;
    }
    out[n] = 0;
    return true;
}

bool save_preference(const char *name, const char *value) {
    char target[APP_PATH_CAP], temporary[APP_PATH_CAP];
    if (!path_join(target, app.paths.home, name) || !path_join(temporary, app.paths.home, ".preference-XXXXXX"))
        return false;
    int fd = mkstemp(temporary);
    if (fd < 0)
        return false;
    size_t n  = strlen(value);
    bool   ok = write(fd, value, n) == (ssize_t) n && fsync(fd) == 0;
    close(fd);
    if (ok)
        ok = rename(temporary, target) == 0;
    if (!ok)
        unlink(temporary);
    return ok;
}

/* Legacy last-reply files remain readable for one-time archival migration. */
static void measurement_key(char key[128], const struct app_model *model, unsigned slot) {
    snprintf(
            key, 128, "performance-%s-%s", model->sha256, slot ? app.backend.gpu : app.backend.cpu);
}
static bool valid_measurement(const struct processor_measurement *m) {
    return isfinite(m->rate) && m->rate > 0 && m->rate <= 1e9 && isfinite(m->first) &&
           m->first >= 0 && m->first <= 3600 && isfinite(m->total) && m->total >= m->first &&
           m->total <= 3600 && isfinite(m->tokens) && m->tokens >= 1 && m->tokens <= 1000000 &&
           m->tokens == (double) (uint64_t) m->tokens && isfinite(m->rss) && m->rss >= 0 &&
           m->rss <= 1e15 && isfinite(m->recorded) && m->recorded > 0 &&
           m->recorded <= (double) time(nullptr) + 300;
}
void restore_measurements(void) {
    memset(app.prefs.history, 0, sizeof app.prefs.history);
    for (size_t i = 0; i < app_model_count; i++)
        for (unsigned slot = 0; slot < 2; slot++) {
            const char        *backend = slot ? app.backend.gpu : app.backend.cpu;
            struct perf_record r;
            perf_last(app_models[i].sha256, app.prefs.profile_series, backend, &r);
            if (r.id[0])
                app.prefs.history[i][slot] =
                        (struct processor_measurement) {.rate  = r.output / (r.generation_ns / 1e9),
                                                        .first = r.first_ns / 1e9,
                                                        .total = r.total_ns / 1e9,
                                                        .tokens   = r.output,
                                                        .rss      = r.rss > 0 ? r.rss : 0,
                                                        .recorded = r.timestamp};
        }
}
void migrate_measurements(void) {
    for (size_t i = 0; i < app_model_count; i++)
        for (unsigned slot = 0; slot < 2; slot++) {
            if (slot && !app.backend.gpu[0])
                continue;
            char key[128], text[1024];
            measurement_key(key, &app_models[i], slot);
            if (!read_preference(key, text, sizeof text) || strncmp(text, "v1 ", 3))
                continue;
            char *identity = strchr(text, '\n'),
                 *numbers  = identity ? strchr(identity + 1, '\n') : nullptr;
            if (!identity || !numbers)
                continue;
            struct processor_measurement m    = {0};
            int                          used = 0;
            if (sscanf(numbers + 1,
                       "%lf %lf %lf %lf %lf %lf%n",
                       &m.rate,
                       &m.first,
                       &m.total,
                       &m.tokens,
                       &m.rss,
                       &m.recorded,
                       &used) != 6 ||
                numbers[1 + used] || !valid_measurement(&m))
                continue;
            *numbers             = 0;
            struct perf_record r = {.timestamp     = m.recorded,
                                    .output        = m.tokens,
                                    .generation_ns = m.tokens / m.rate * 1e9,
                                    .first_ns      = m.first * 1e9,
                                    .total_ns      = m.total * 1e9,
                                    .rss           = m.rss,
                                    .peak_rss      = -1,
                                    .cpu_percent   = -1,
                                    .load_ns       = -1,
                                    .prefill_ns    = -1,
                                    .top_p         = 1};
            snprintf(r.model, sizeof r.model, "%s", app_models[i].id);
            snprintf(r.artifact, sizeof r.artifact, "%s", app_models[i].sha256);
            snprintf(r.backend, sizeof r.backend, "%s", slot ? app.backend.gpu : app.backend.cpu);
            snprintf(r.quantization, sizeof r.quantization, "%s", app_models[i].quantization);
            snprintf(r.id, sizeof r.id, "legacy-%s-%s-%.0f", r.artifact, r.backend, m.recorded);
            snprintf(r.series, sizeof r.series, "legacy;unknown-engine-config;%s", identity + 1);
            strcpy(r.source, "legacy_last_reply");
            strcpy(r.outcome, "completed");
            strcpy(r.finish, "unknown");
            perf_import(&r);
        }
}
bool save_selection(const char *id) {
    if (!save_preference("selected", id))
        return false;
    snprintf(app.prefs.selected, sizeof app.prefs.selected, "%s", id);
    return true;
}

void restore_preview_preferences(void) {
    memset(app.prefs.preview_accepted, 0, sizeof app.prefs.preview_accepted);
    for (size_t i = 0; i < app_model_count; ++i) {
        char key[80], value[8] = "";
        snprintf(key, sizeof key, "preview-%s", app_models[i].sha256);
        app.prefs.preview_accepted[i] = read_preference(key, value, sizeof value) && !strcmp(value, "v1");
    }
}
