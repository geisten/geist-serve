#pragma once
#include "core.h"
#include "../lifecycle.h"
struct json;
/* Numeric scopes, never a sum of RSS and device allocation. -1 is unknown. */
struct app_memory_record {
    char     generation[112];
    double   sampled_at, gpu_age_ms, gpu_end, gpu_peak;
    unsigned gpu_samples, status, source, rss_source;
    bool     unified;
    uint64_t last_sequence; /* transient deduplication, never serialized */
};
const char *app_rss_source(void);
void        app_memory_reset(struct app_memory_record *m);
void        app_memory_observe(struct app_memory_record      *m,
                               const struct lifecycle_memory *s,
                               uint64_t                       now_ns,
                               double                         timestamp);
void        app_memory_json(struct app_buffer              *b,
                            const struct app_memory_record *m,
                            double                          rss,
                            double                          peak_rss,
                            unsigned                        rss_samples);
bool        app_memory_parse(struct app_memory_record *m, const struct json *j, int index);
bool        app_memory_valid(const struct app_memory_record *m);
