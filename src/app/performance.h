#pragma once
#include "core.h"
#include "engine.h"
#include "memory.h"
#include <stdint.h>
/* Typed numeric observations. No request/reply strings enter this interface. */
#define PERF_RECORDS 8192u
#define PERF_QUEUE 64u
struct perf_record {
    unsigned                 schema;
    struct app_memory_record memory;
    struct app_engine        engine;
    char                     id[160], model[65], artifact[65], quantization[32];
    char                     series[768], backend[24], source[24], outcome[24], finish[16];
    char                     run[80];
    double   timestamp, generation_ns, first_ns, first_answer_ns, total_ns, prefill_ns, load_ns;
    double   rss, peak_rss, cpu_percent;
    uint64_t input, output, reused, epoch;
    unsigned max_tokens, threads, samples;
    double   temperature, top_p;
    bool     cold, contention, warmup, enabled, reasoning;
};
void  perf_init(const char *home);
void  perf_close(void);
void  perf_begin(struct perf_record *r);
void  perf_submit(const struct perf_record *r);
void  perf_import(struct perf_record *r);
void  perf_view(struct app_buffer *b,
                const char        *artifact,
                const char        *series,
                const char        *cpu,
                const char        *gpu);
/* Newest completed reply per processor (0 CPU, 1 GPU) for this artifact from
 * an earlier engine build or Geisten version; version is "" when unknown. */
struct perf_earlier {
    double rate[2];
    char   version[64];
};
bool  perf_earlier(const char *artifact, const char *series, struct perf_earlier *out);
void  perf_last(const char         *artifact,
                const char         *series,
                const char         *backend,
                struct perf_record *out);
bool  perf_settings(bool enabled, unsigned days);
bool  perf_clear(void);
bool  perf_save_export(void);
char *perf_export(size_t *length); /* Owned bounded JSONL; caller frees. */
/* Shared definition, independently tested with golden observations. */
unsigned perf_group(const struct perf_record *r);
