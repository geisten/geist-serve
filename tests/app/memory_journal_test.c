/* Synthetic scope fixtures only; never installed as product measurements. */
#include "../../src/app/performance.h"
#include "../../src/json.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    assert(argc == 3);
    perf_init(argv[1]);
    if (!strcmp(argv[2], "write")) {
        for (unsigned i = 0; i < 5; i++) {
            struct perf_record r = {
                    .input = 20, .output = 32, .generation_ns = 1e9, .total_ns = 2e9, .top_p = 1};
            perf_begin(&r);
            r.first_ns = 1e8;
            r.rss      = 322122547.;
            r.peak_rss = 400000000.;
            r.samples  = 2;
            strcpy(r.artifact, "synthetic-artifact");
            strcpy(r.series, "synthetic-series");
            strcpy(r.backend, "metal");
            strcpy(r.source, i == 2 ? "controlled_test" : "app");
            strcpy(r.outcome, i == 3 ? "cancelled" : i == 4 ? "interrupted" : "completed");
            strcpy(r.finish, "stop");
            r.memory = (struct app_memory_record) {.gpu_end     = i ? 10737418240. : 0,
                                                   .gpu_peak    = 10737418240.,
                                                   .gpu_samples = 2,
                                                   .gpu_age_ms  = 1,
                                                   .rss_age_ms  = 0,
                                                   .sampled_at  = r.timestamp,
                                                   .status      = 1,
                                                   .source      = 1,
                                                   .rss_source  = 1,
                                                   .unified     = true};
            strcpy(r.memory.generation, "fixture:42");
            if (i == 3) {
                r.memory.status  = 3;
                r.memory.gpu_end = -1;
            }
            if (i == 4) {
                app_memory_reset(&r.memory);
                r.schema = 1;
            }
            perf_submit(&r);
        }
    }
    size_t length;
    char  *data = perf_export(&length);
    assert(data && length);
    struct json *j = calloc(1, sizeof *j);
    assert(j);
    unsigned count = 0;
    char    *line  = data;
    while (line && *line) {
        char *end = strchr(line, '\n');
        assert(end);
        *end = 0;
        assert(json_parse(j, strlen(line), line) >= 0);
        int m = json_get(j, 0, "memory");
        assert(m >= 0);
        assert(json_num(j, json_get(j, m, "process_rss_bytes"), -1) == 322122547.);
        assert(json_num(j, json_get(j, m, "total_unique_physical_bytes"), -1) == -1);
        double gpu = json_num(j, json_get(j, m, "gpu_allocated_bytes"), -1);
        assert(gpu == (count == 0 ? 0 : count < 3 ? 10737418240. : -1));
        assert(json_num(j, json_get(j, m, "gpu_samples"), -1) == (count == 4 ? 0 : 2));
        if (count == 4)
            assert(json_num(j, json_get(j, 0, "schema"), 0) == 1);
        ++count;
        line = end + 1;
    }
    assert(count == 5);
    free(j);
    free(data);
    perf_close();
    puts("memory journal: null/zero, large allocation, end/peak/source, cancel/failure, legacy and "
         "restart/export passed");
}
