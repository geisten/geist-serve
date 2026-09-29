#include "../../src/app/activity.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(void) {
    struct activity a;
    activity_begin(&a, 1, 8, ACT_CONNECT, 100);
    assert(activity_step(&a, 1, 8, ACT_OPEN, 120));
    assert(!activity_step(&a, 1, 7, ACT_ANSWER, 125)); /* Reused PID / older generation. */
    assert(!activity_step(&a, 0, 8, ACT_ANSWER, 125));
    assert(!activity_step(&a, 1, 8, ACT_OPEN, 130));
    assert(!activity_step(&a, 1, 8, ACT_PREFILL, 110));
    assert(a.sequence == 2 && a.event_at == 120);
    assert(activity_step(&a, 1, 8, ACT_PREFILL, 140));
    char              out[8192];
    struct app_buffer b = {.data = out, .cap = sizeof out};
    activity_json(&b, &a, 1000, true);
    assert(strstr(out, "\"event_age_ms\":860.000"));
    assert(a.event_at == 140); /* Heartbeat/snapshot is not engine progress. */
    assert(strstr(out, "\"duration_ms\":20.000"));
    activity_progress(&a, 1, 1100);
    activity_progress(&a, 1, 1200);
    assert(a.event_at == 1100);
    assert(activity_end(&a, "cancelled", 499, 1300));
    assert(!activity_end(&a, "completed", 0, 1400));
    assert(!activity_step(&a, 1, 8, ACT_ANSWER, 1400));
    activity_progress(&a, 2, 1500);
    assert(a.progress == 1);
    assert(a.phase[2].duration == 1160);
    activity_begin(&a, 2, 9, ACT_LOADING, 2000);
    for (unsigned i = 0; i < 1000; i++)
        (void) activity_step(&a, 2, 9, i % 2 ? ACT_LOADING : ACT_STARTING, 2001 + i);
    assert(a.phases == ACTIVITY_PHASES);
    assert(activity_end(&a, "failed", 502, 4000));
    assert(activity_request_stage("fake") == ACT_NONE);
    puts("activity: identity, ordering, bounded phases, heartbeat separation and exactly-once "
         "terminal passed");
}
