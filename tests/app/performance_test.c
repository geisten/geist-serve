/* The writer can be slow/full without blocking request accounting. */
#include "../../src/app/performance.h"
#include "../../src/json.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
int main(int argc, char **argv) {
    assert(argc == 2);
    perf_init(argv[1]);
    struct perf_record r = {0};
    for (unsigned input = 0; input < 3; input++)
        for (unsigned output = 0; output < 4; output++) {
            r.input  = (unsigned[]) {512, 513, 2049}[input];
            r.output = (unsigned[]) {31, 32, 128, 512}[output];
            assert(perf_group(&r) == input + 3 * output);
        }
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (unsigned i = 0; i < 200; i++) {
        r = (struct perf_record) {
                .input = 100, .output = 64, .generation_ns = 1e9, .total_ns = 2e9, .top_p = 1};
        perf_begin(&r);
        r.first_ns = 1e8;
        strcpy(r.artifact, "test-artifact");
        strcpy(r.series, "test-series");
        strcpy(r.backend, "cpu_neon");
        strcpy(r.source, "app");
        strcpy(r.outcome, "completed");
        strcpy(r.finish, "stop");
        perf_submit(&r);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double            seconds = end.tv_sec - start.tv_sec + (end.tv_nsec - start.tv_nsec) / 1e9;
    char              text[65536];
    struct app_buffer b = {.data = text, .cap = sizeof text};
    perf_view(&b, "test-artifact", "test-series", "cpu_neon", "");
    struct json *j = calloc(1, sizeof *j);
    assert(j && !b.failed && json_parse(j, b.len, text) >= 0);
    assert(json_num(j, json_get(j, 0, "retained"), 0) == 200);
    assert(json_num(j, json_get(j, 0, "dropped"), 0) > 0);
    assert(json_num(j, json_get(j, json_get(j, 0, "cpu"), "count"), 0) == 30);
    /* This is a generous regression ceiling, not a negligible-overhead claim.
     * 65 synchronous 20-ms writes would take over 1.3 s. */
    assert(seconds < 1.0);
    printf("200 enqueues: %.6f s; bounded queue overflow visible\n", seconds);
    free(j);
    perf_close();
    return 0;
}
