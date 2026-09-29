#include "memory.h"
#include "../json.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

const char *app_rss_source(void) {
#ifdef __APPLE__
    return "macos.proc_pid_rusage.ri_resident_size";
#else
    return "linux.proc_pid_stat.rss";
#endif
}
void app_memory_reset(struct app_memory_record *m) {
    *m = (struct app_memory_record) {.gpu_end = -1, .gpu_peak = -1, .gpu_age_ms = -1};
}
void app_memory_observe(struct app_memory_record      *m,
                        const struct lifecycle_memory *s,
                        uint64_t                       now_ns,
                        double                         timestamp) {
    m->gpu_end    = -1;
    m->status     = 0;
    m->gpu_age_ms = -1;
    m->sampled_at = timestamp;
    if (!s || !s->sampled_ns || s->sampled_ns > now_ns)
        return;
    m->gpu_age_ms = (double) (now_ns - s->sampled_ns) / 1e6;
    m->status     = m->gpu_age_ms > 6000 ? 4 : s->status;
    m->source     = s->source;
    m->unified    = s->unified;
    if (m->status != 1 || m->source != 1)
        return;
    m->gpu_end = (double) s->allocated_bytes;
    if (m->last_sequence != s->sequence) {
        m->last_sequence = s->sequence;
        ++m->gpu_samples;
        if (m->gpu_end > m->gpu_peak)
            m->gpu_peak = m->gpu_end;
    }
}
static void numeric(struct app_buffer *b, double n) {
    if (n < 0 || !isfinite(n))
        app_put(b, "null");
    else
        app_printf(b, "%.17g", n);
}
bool app_memory_valid(const struct app_memory_record *m) {
    return m->status <= 4 && m->source <= 1 && m->rss_source <= 2 && m->gpu_samples <= 1000000 &&
           isfinite(m->gpu_end) && m->gpu_end >= -1 && m->gpu_end <= 1e15 &&
           isfinite(m->gpu_peak) && m->gpu_peak >= -1 && m->gpu_peak <= 1e15 &&
           isfinite(m->gpu_age_ms) && m->gpu_age_ms >= -1 && m->gpu_age_ms <= 1e12 &&
           isfinite(m->sampled_at) && m->sampled_at >= 0 && m->sampled_at <= 1e12 &&
           (m->gpu_samples == 0 || (m->source == 1 && m->gpu_peak >= 0)) &&
           (m->status != 1 ||
            (m->source == 1 && m->gpu_end >= 0 && m->gpu_age_ms >= 0 && m->gpu_age_ms <= 6000));
}
void app_memory_json(struct app_buffer              *b,
                     const struct app_memory_record *m,
                     double                          rss,
                     double                          peak_rss,
                     unsigned                        rss_samples) {
    const char *reasons[] = {"not_collected", "", "unsupported", "query_failed", "stale"};
    app_put(b, "{\"process_generation\":");
    if (m->generation[0])
        app_quote(b, m->generation);
    else
        app_put(b, "null");
    app_put(b, ",\"sampled_at\":");
    numeric(b, m->sampled_at > 0 ? m->sampled_at : -1);
    app_put(b, ",\"process_rss_bytes\":");
    numeric(b, rss);
    app_put(b, ",\"process_rss_sampled_peak_bytes\":");
    numeric(b, peak_rss);
    app_put(b, ",\"rss_source\":");
    app_quote(b,
              m->rss_source == 1   ? "macos.proc_pid_rusage.ri_resident_size"
              : m->rss_source == 2 ? "linux.proc_pid_stat.rss"
                                   : "legacy_process_rss");
    app_printf(b,
               ",\"rss_samples\":%u,\"sample_interval_ms\":2000,\"status\":%u,\"source\":%u,\"gpu_"
               "samples\":%u,",
               rss_samples,
               m->status,
               m->source,
               m->gpu_samples);
    app_put(b, "\"gpu_allocated_bytes\":");
    numeric(b, m->status == 1 ? m->gpu_end : -1);
    app_put(b, ",\"gpu_allocated_sampled_peak_bytes\":");
    numeric(b, m->gpu_samples ? m->gpu_peak : -1);
    app_put(b, ",\"gpu_sample_age_ms\":");
    numeric(b, m->gpu_age_ms);
    app_put(b, ",\"gpu_source\":");
    if (m->source == 1)
        app_quote(b, "metal.MTLDevice.currentAllocatedSize");
    else
        app_put(b, "null");
    app_put(b, ",\"gpu_unavailable_reason\":");
    if (m->status == 1)
        app_put(b, "null");
    else
        app_quote(b, reasons[m->status <= 4 ? m->status : 0]);
    app_put(b, ",\"unified_memory\":");
    app_put(b, m->source == 1 ? (m->unified ? "true" : "false") : "null");
    app_put(b, ",\"process_physical_footprint_bytes\":null,\"total_unique_physical_bytes\":null}");
}
bool app_memory_parse(struct app_memory_record *m, const struct json *j, int index) {
    app_memory_reset(m);
    if (index < 0)
        return true; /* Preserve legacy RSS without inventing telemetry. */
    if (j->tok[index].type != JSMN_OBJECT)
        return false;
    char *generation = json_strdup(j, json_get(j, index, "process_generation"));
    if (generation) {
        if (strlen(generation) >= sizeof m->generation) {
            free(generation);
            return false;
        }
        strcpy(m->generation, generation);
        free(generation);
    }
    m->sampled_at    = json_num(j, json_get(j, index, "sampled_at"), 0);
    m->gpu_age_ms    = json_num(j, json_get(j, index, "gpu_sample_age_ms"), -1);
    m->gpu_end       = json_num(j, json_get(j, index, "gpu_allocated_bytes"), -1);
    m->gpu_peak      = json_num(j, json_get(j, index, "gpu_allocated_sampled_peak_bytes"), -1);
    char *rss_source = json_strdup(j, json_get(j, index, "rss_source"));
    if (rss_source) {
        m->rss_source = !strcmp(rss_source, "macos.proc_pid_rusage.ri_resident_size") ? 1
                        : !strcmp(rss_source, "linux.proc_pid_stat.rss")              ? 2
                                                                                      : 0;
        free(rss_source);
    }
    const char *keys[]   = {"status", "source", "gpu_samples"};
    unsigned   *values[] = {&m->status, &m->source, &m->gpu_samples};
    for (unsigned i = 0; i < 3; i++) {
        double n = json_num(j, json_get(j, index, keys[i]), 0);
        if (!isfinite(n) || n < 0 || n > 1000000 || floor(n) != n)
            return false;
        *values[i] = (unsigned) n;
    }
    m->unified = json_bool(j, json_get(j, index, "unified_memory"), false);
    return app_memory_valid(m);
}
