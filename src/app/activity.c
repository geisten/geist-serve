#include "activity.h"
#include <string.h>
#include <stdio.h>

static const char *const names[] = {"idle",
                                    "receipt",
                                    "hash",
                                    "download",
                                    "stopping",
                                    "starting",
                                    "loading",
                                    "ready",
                                    "connect",
                                    "open",
                                    "tokenize",
                                    "prefill",
                                    "generate",
                                    "preparing",
                                    "answer"};
const char              *activity_name(enum activity_stage stage) {
    return stage >= ACT_NONE && stage <= ACT_ANSWER ? names[stage] : "unknown";
}
enum activity_stage activity_request_stage(const char *stage) {
    if (stage)
        for (unsigned i = ACT_CONNECT; i <= ACT_GENERATE; ++i)
            if (!strcmp(names[i], stage))
                return (enum activity_stage) i;
    return ACT_NONE;
}
void activity_begin(struct activity    *a,
                    uint64_t            id,
                    uint64_t            generation,
                    enum activity_stage stage,
                    double              now) {
    *a          = (struct activity) {.id         = id,
                                     .generation = generation,
                                     .sequence   = 1,
                                     .started    = now,
                                     .event_at   = now,
                                     .stage      = stage,
                                     .phases     = 1};
    a->phase[0] = (struct activity_phase) {.stage = stage, .start = now};
}
bool activity_step(struct activity    *a,
                   uint64_t            id,
                   uint64_t            generation,
                   enum activity_stage stage,
                   double              now) {
    if (!a->id || a->id != id || a->generation != generation || a->outcome[0] ||
        stage <= ACT_NONE || stage > ACT_ANSWER || a->stage == stage || now < a->event_at)
        return false;
    /* A bounded trace must never silently overwrite earlier phase evidence. */
    if (a->phases >= ACTIVITY_PHASES)
        return false;
    a->phase[a->phases - 1].duration = now - a->phase[a->phases - 1].start;
    a->phase[a->phases++]            = (struct activity_phase) {.stage = stage, .start = now};
    a->stage                         = stage;
    a->event_at                      = now;
    ++a->sequence;
    return true;
}
void activity_progress(struct activity *a, uint64_t progress, double now) {
    if (!a->id || a->outcome[0] || progress <= a->progress || now < a->event_at)
        return;
    a->progress = progress;
    a->event_at = now;
    ++a->sequence;
}
bool activity_end(struct activity *a, const char *outcome, int error, double now) {
    if (!a->id || a->outcome[0] || now < a->event_at)
        return false;
    a->ended    = now;
    a->event_at = now;
    a->error    = error;
    ++a->sequence;
    a->phase[a->phases - 1].duration = now - a->phase[a->phases - 1].start;
    snprintf(a->outcome, sizeof a->outcome, "%s", outcome);
    return true;
}
void activity_json(struct app_buffer *b, const struct activity *a, double now, bool alive) {
    if (!a->id) {
        app_put(b, "null");
        return;
    }
    const double end = a->outcome[0] ? a->ended : now;
    app_printf(b,
               "{\"id\":%llu,\"generation\":%llu,\"sequence\":%llu,\"stage\":",
               (unsigned long long) a->id,
               (unsigned long long) a->generation,
               (unsigned long long) a->sequence);
    app_quote(b, activity_name(a->stage));
    app_printf(b,
               ",\"elapsed_ms\":%.3f,\"stage_elapsed_ms\":%.3f,\"event_age_ms\":%.3f,"
               "\"runtime_alive\":%s,\"progress_events\":%llu,\"error_code\":%d,\"outcome\":",
               end - a->started,
               end - a->phase[a->phases - 1].start,
               now - a->event_at,
               alive ? "true" : "false",
               (unsigned long long) a->progress,
               a->error);
    app_quote(b, a->outcome);
    app_put(b, ",\"backend\":");
    app_quote(b, a->backend);
    app_put(b, ",\"model\":");
    app_quote(b, a->model);
    app_put(b, ",\"engine\":");
    app_engine_json(b, &a->engine);
    app_put(b, ",\"phases\":[");
    for (unsigned i = 0; i < a->phases; ++i) {
        if (i)
            app_put(b, ",");
        app_put(b, "{\"stage\":");
        app_quote(b, activity_name(a->phase[i].stage));
        app_printf(b,
                   ",\"offset_ms\":%.3f,\"duration_ms\":%.3f}",
                   a->phase[i].start - a->started,
                   i + 1 == a->phases && !a->outcome[0] ? now - a->phase[i].start
                                                        : a->phase[i].duration);
    }
    app_put(b, "]}");
}
