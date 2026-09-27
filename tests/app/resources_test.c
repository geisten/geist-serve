#include "../../src/app/resources.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static bool parse(const char                *user,
                  const char                *system,
                  const char                *start,
                  const char                *rss,
                  struct app_process_sample *sample) {
    char line[1024];
    /* A process name can contain spaces and closing parentheses. */
    snprintf(line,
             sizeof line,
             "42 (model ) worker) S 1 2 3 4 5 6 7 8 9 10 %s %s -1 -1 20 0 4 0 %s 100000 %s 0\n",
             user,
             system,
             start,
             rss);
    return app_process_parse_linux(line, 100, 4096, sample);
}
int main(void) {
    struct app_process_sample sample = {};
    assert(parse("125", "25", "4567", "32", &sample));
    assert(sample.cpu_ns == 1500000000 && sample.identity == 4567 && sample.rss == 131072);
    assert(!parse("-1", "0", "1", "32", &sample));
    assert(!parse("1x", "0", "1", "32", &sample));
    assert(!parse("18446744073709551615", "1", "1", "32", &sample));
    assert(!parse("1", "0", "1", "18446744073709551615", &sample));
    assert(!parse("18446744073709551615", "0", "1", "32", &sample));
    assert(!app_process_parse_linux("42 (partial) S 1", 100, 4096, &sample));
    assert(!app_process_parse_linux("invalid", 100, 4096, &sample));

    struct app_resource_window window = {};
    sample                            = (struct app_process_sample) {
            .pid = 42, .identity = 100, .cpu_ns = 1000000000, .monotonic_ms = 1000};
    app_resource_update(&window, &sample, 8);
    assert(!window.cpu_known);
    sample.cpu_ns += 2000000000;
    sample.monotonic_ms += 1000;
    app_resource_update(&window, &sample, 8);
    assert(window.cpu_known && window.cpu_percent == 25 && window.interval_ms == 1000);
    sample.monotonic_ms += 1;
    app_resource_update(&window, &sample, 8);
    assert(window.cpu_percent == 25 && window.interval_ms == 1000);
    sample.monotonic_ms += 999;
    app_resource_update(&window, &sample, 8);
    assert(window.cpu_known && window.cpu_percent == 0);
    sample.identity++; // PID reuse must not carry CPU usage across processes.
    app_resource_update(&window, &sample, 8);
    assert(!window.cpu_known);
    sample.monotonic_ms += 11000;
    app_resource_update(&window, &sample, 8);
    assert(!window.cpu_known);
    sample.monotonic_ms += 1000;
    sample.cpu_ns += 8000000000;
    app_resource_update(&window, &sample, 8);
    assert(window.cpu_known && window.cpu_percent == 100);
    sample.cpu_ns = 0;
    app_resource_update(&window, &sample, 8);
    assert(!window.cpu_known);
    app_resource_update(&window, nullptr, 8);
    assert(!window.cpu_known && window.previous.pid == 0);
    app_resource_update(&window, &sample, 0);
    assert(!window.cpu_known);

    assert(!app_process_read(0, &sample));
    assert(app_process_read(getpid(), &sample));
    assert(sample.pid == getpid() && sample.rss > 0 && sample.monotonic_ms > 0);
    // Check OS counter units against the independent process CPU clock.
    // This catches Mach ticks being mistaken for nanoseconds on Apple Silicon.
    struct timespec before, after;
    assert(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &before) == 0);
    struct app_process_sample cpu_before, cpu_after;
    assert(app_process_read(getpid(), &cpu_before));
    double elapsed;
    do {
        assert(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &after) == 0);
        elapsed = (double) (after.tv_sec - before.tv_sec) * 1e9 + after.tv_nsec - before.tv_nsec;
    } while (elapsed < 300000000);
    assert(app_process_read(getpid(), &cpu_after));
    double ratio = (double) (cpu_after.cpu_ns - cpu_before.cpu_ns) / elapsed;
    printf("OS CPU counter / process clock ratio: %.3f\n", ratio);
    assert(ratio > 0.7 && ratio < 1.3);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0)
        _exit(0);
    assert(waitpid(child, nullptr, 0) == child);
    assert(!app_process_read(child, &sample));
    puts("Resource counters: parser, overflow, sampling, identity, idle and process exit passed");
}
