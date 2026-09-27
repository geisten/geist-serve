#include "resources.h"
#include <errno.h>
#include <math.h>
#include <stdckdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#include <mach/mach_time.h>
#include <sys/resource.h>
#endif

bool app_process_parse_linux(const char                *stat,
                             uint64_t                   hz,
                             uint64_t                   page,
                             struct app_process_sample *sample) {
    const char *p = strrchr(stat, ')');
    if (!p || p[1] != ' ' || !hz || !page)
        return false;
    p += 2;
    uint64_t user = 0, system = 0, start = 0, pages = 0;
    for (unsigned field = 3; field <= 24; ++field) {
        while (*p == ' ')
            ++p;
        const char *end = p;
        while (*end && *end != ' ' && *end != '\n')
            ++end;
        if (p == end)
            return false;
        if (field == 14 || field == 15 || field == 22 || field == 24) {
            if (*p < '0' || *p > '9')
                return false;
            errno = 0;
            char              *parsed;
            unsigned long long n = strtoull(p, &parsed, 10);
            if (errno || parsed != end)
                return false;
            if (field == 14)
                user = n;
            if (field == 15)
                system = n;
            if (field == 22)
                start = n;
            if (field == 24)
                pages = n;
        }
        p = end;
    }
    uint64_t ticks, seconds, remainder, rss;
    if (ckd_add(&ticks, user, system) || ckd_mul(&rss, pages, page) ||
        ckd_mul(&seconds, ticks / hz, UINT64_C(1000000000)) ||
        ckd_mul(&remainder, ticks % hz, UINT64_C(1000000000)) ||
        ckd_add(&sample->cpu_ns, seconds, remainder / hz))
        return false;
    sample->identity = start;
    sample->rss      = rss;
    return true;
}

bool app_process_read(pid_t pid, struct app_process_sample *sample) {
    *sample = (struct app_process_sample) {};
    if (pid <= 0)
        return false;
#ifdef __APPLE__
    struct rusage_info_v2     info = {};
    mach_timebase_info_data_t timebase;
    uint64_t                  ticks, whole, remainder;
    // libproc exposes Mach absolute time, not nanoseconds on Apple Silicon.
    if (proc_pid_rusage(pid, RUSAGE_INFO_V2, (rusage_info_t *) &info) != 0 ||
        info.ri_proc_exit_abstime || mach_timebase_info(&timebase) != KERN_SUCCESS ||
        !timebase.denom || !timebase.numer ||
        ckd_add(&ticks, info.ri_user_time, info.ri_system_time) ||
        ckd_mul(&whole, ticks / timebase.denom, (uint64_t) timebase.numer) ||
        ckd_mul(&remainder, ticks % timebase.denom, (uint64_t) timebase.numer) ||
        ckd_add(&sample->cpu_ns, whole, remainder / timebase.denom))
        return false;
    sample->identity = info.ri_proc_start_abstime;
    sample->rss      = info.ri_resident_size;
#else
    char path[64], buffer[4096];
    snprintf(path, sizeof path, "/proc/%ld/stat", (long) pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    size_t n     = fread(buffer, 1, sizeof buffer - 1, f);
    bool   valid = !ferror(f) && feof(f);
    fclose(f);
    buffer[n] = 0;
    long hz = sysconf(_SC_CLK_TCK), page = sysconf(_SC_PAGESIZE);
    if (!valid || hz <= 0 || page <= 0 ||
        !app_process_parse_linux(buffer, (uint64_t) hz, (uint64_t) page, sample))
        return false;
#endif
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return false;
    sample->monotonic_ms = (double) now.tv_sec * 1000.0 + (double) now.tv_nsec / 1e6;
    sample->pid          = pid;
    return true;
}

void app_resource_update(struct app_resource_window      *window,
                         const struct app_process_sample *sample,
                         unsigned                         logical_cpus) {
    if (!sample || !logical_cpus) {
        *window = (struct app_resource_window) {};
        return;
    }
    double elapsed = sample->monotonic_ms - window->previous.monotonic_ms;
    if (sample->pid != window->previous.pid || sample->identity != window->previous.identity ||
        sample->cpu_ns < window->previous.cpu_ns || elapsed < 0 || elapsed > 10000) {
        *window = (struct app_resource_window) {.previous = *sample};
        return;
    }
    /* Concurrent/rapid readers share a useful window instead of amplifying jitter. */
    if (elapsed < 500)
        return;
    double percent      = (double) (sample->cpu_ns - window->previous.cpu_ns) / 1e6 / elapsed /
                          logical_cpus * 100.0;
    window->cpu_known   = isfinite(percent);
    window->cpu_percent = percent > 100 ? 100 : percent;
    window->interval_ms = elapsed;
    window->previous    = *sample;
}
