#pragma once
#include "core.h"
#include "engine.h"
#include <stdint.h>

/* Caller owns synchronization. Bounded, numeric snapshots; no request text. */
#define ACTIVITY_PHASES 16
enum activity_stage {
    ACT_NONE,
    ACT_RECEIPT,
    ACT_HASH,
    ACT_DOWNLOAD,
    ACT_STOPPING,
    ACT_STARTING,
    ACT_LOADING,
    ACT_BACKEND,
    ACT_MODEL,
    ACT_METADATA,
    ACT_WARMUP,
    ACT_READY,
    ACT_CONNECT,
    ACT_OPEN,
    ACT_TOKENIZE,
    ACT_PREFILL,
    ACT_GENERATE,
    ACT_PREPARING,
    ACT_ANSWER
};
struct activity_phase {
    enum activity_stage stage;
    double              start, duration;
};
struct activity {
    uint64_t              id, generation, sequence, progress;
    double                started, event_at, ended;
    enum activity_stage   stage;
    char                  outcome[16], backend[24], model[65];
    int                   error;
    unsigned              phases;
    struct activity_phase phase[ACTIVITY_PHASES];
    struct app_engine     engine;
};
const char         *activity_name(enum activity_stage stage);
enum activity_stage activity_request_stage(const char *stage);
void                activity_begin(struct activity    *a,
                                   uint64_t            id,
                                   uint64_t            generation,
                                   enum activity_stage stage,
                                   double              now);
bool                activity_step(struct activity    *a,
                                  uint64_t            id,
                                  uint64_t            generation,
                                  enum activity_stage stage,
                                  double              now);
void                activity_progress(struct activity *a, uint64_t progress, double now);
bool                activity_end(struct activity *a, const char *outcome, int error, double now);
void activity_json(struct app_buffer *b, const struct activity *a, double now, bool alive);
