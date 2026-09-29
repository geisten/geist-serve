#include "../../src/app/memory.h"
#include "../../src/json.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    struct app_memory_record m;
    app_memory_reset(&m);
    assert(m.gpu_end == -1 && m.gpu_peak == -1 && app_memory_valid(&m));
    struct lifecycle_memory s = {.sequence        = 2,
                                 .sampled_ns      = 1000000000,
                                 .allocated_bytes = 10737418240ull,
                                 .status          = 1,
                                 .source          = 1,
                                 .unified         = true};
    app_memory_observe(&m, &s, 1000000000, 1790000000);
    assert(m.gpu_end == 10737418240. && m.gpu_samples == 1);
    app_memory_observe(&m, &s, 2000000000, 1790000001);
    assert(m.gpu_samples == 1 && m.gpu_age_ms == 1000);
    s.sequence        = 4;
    s.allocated_bytes = 0;
    app_memory_observe(&m, &s, 2000000000, 1790000001);
    assert(m.gpu_end == 0 && m.gpu_peak == 10737418240. && m.gpu_samples == 2);
    app_memory_observe(&m, &s, 7000000001, 1790000007);
    assert(m.status == 4 && m.gpu_end == -1 && m.gpu_samples == 2);
    s.sequence = 2;
    app_memory_observe(&m, &s, 2000000000, 1790000001);
    assert(m.status == 0 && m.gpu_end == -1); /* delayed sample cannot become current */
    s.sequence = 6;
    s.status   = 3;
    s.source   = 0;
    app_memory_observe(&m, &s, 2000000000, 1790000001);
    assert(m.status == 3 && m.gpu_end == -1 && m.gpu_peak == 10737418240.);
    assert(app_memory_valid(&m)); /* failed query retains a scoped historical peak */
    s.status = 1;
    s.source = 0;
    app_memory_observe(&m, &s, 2000000000, 1790000001);
    assert(m.status == 3 && m.gpu_end == -1); /* old source cannot legitimize a bad new sample */
    app_memory_reset(&m);
    s.status = 2;
    app_memory_observe(&m, &s, 2000000000, 1790000001);
    assert(m.status == 2 && m.gpu_end == -1 && !m.gpu_samples);
    app_memory_observe(&m, &s, 1, 1790000001);
    assert(m.status == 0); /* future timestamp */
    app_memory_observe(&m, nullptr, 1, 1790000001);
    assert(m.gpu_end == -1);
    app_memory_reset(&m);
    s.sequence        = 2;
    s.status          = 1;
    s.source          = 1;
    s.allocated_bytes = 10737418240ull;
    app_memory_observe(&m, &s, 2000000000, 1790000001);
    strcpy(m.generation, "fixture:42");
    m.rss_source = 1;
    char              data[8192];
    struct app_buffer b = {.data = data, .cap = sizeof data};
    app_memory_json(&b, &m, 322122547., 322122547., 1);
    struct json *j = calloc(1, sizeof *j);
    assert(j && !b.failed && json_parse(j, b.len, data) >= 0);
    assert(json_num(j, json_get(j, 0, "process_rss_bytes"), -1) == 322122547.);
    assert(json_num(j, json_get(j, 0, "gpu_allocated_bytes"), -1) == 10737418240.);
    assert(json_num(j, json_get(j, 0, "total_unique_physical_bytes"), -1) == -1);
    struct app_memory_record parsed;
    assert(app_memory_parse(&parsed, j, 0));
    assert(parsed.gpu_end == m.gpu_end && parsed.gpu_peak == m.gpu_peak && parsed.rss_source == 1);
    assert(!strcmp(parsed.generation, m.generation));
    for (unsigned i = 0; i < 3; i++) {
        m.rss_age_ms = (double[]) {-1, 0, 6001}[i];
        b            = (struct app_buffer) {.data = data, .cap = sizeof data};
        app_memory_json(&b, &m, -1, 400000000., 2);
        assert(!b.failed && json_parse(j, b.len, data) >= 0);
        char *reason = json_strdup(j, json_get(j, 0, "rss_unavailable_reason"));
        assert(reason &&
               !strcmp(reason, (const char *[]) {"not_collected", "query_failed", "stale"}[i]));
        free(reason);
    }
    assert(app_memory_parse(&parsed, j, -1) && parsed.gpu_end == -1 && !parsed.source);
    m.gpu_end = INFINITY;
    assert(!app_memory_valid(&m));
    m.gpu_end = 1e16;
    assert(!app_memory_valid(&m));
    const char *invalid = "{\"status\":1,\"source\":0,\"gpu_allocated_bytes\":0}";
    assert(json_parse(j, strlen(invalid), invalid) >= 0 && !app_memory_parse(&parsed, j, 0));
    invalid = "{\"gpu_samples\":4294967296}";
    assert(json_parse(j, strlen(invalid), invalid) >= 0 && !app_memory_parse(&parsed, j, 0));
    free(j);
    puts("memory: scoped 64-bit values, known zero, unavailable/stale, out-of-order, source "
         "validation and roundtrip passed");
}
