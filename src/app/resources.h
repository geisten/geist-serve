#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

struct app_process_sample {
    pid_t    pid;
    uint64_t identity, cpu_ns, rss;
    double   monotonic_ms;
};
struct app_resource_window {
    struct app_process_sample previous;
    double                    cpu_percent, interval_ms;
    bool                      cpu_known;
};
/* Read only the child owned by the caller; no enumeration of other processes. */
[[nodiscard]] bool app_process_read(pid_t pid, struct app_process_sample *sample);
void               app_resource_update(struct app_resource_window      *window,
                                       const struct app_process_sample *sample,
                                       unsigned                         logical_cpus);
/* Shared parser is testable on either OS, including process names with ')'. */
[[nodiscard]] bool app_process_parse_linux(const char                *stat,
                                           uint64_t                   ticks_per_second,
                                           uint64_t                   page_size,
                                           struct app_process_sample *sample);
