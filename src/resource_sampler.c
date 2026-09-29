#define _POSIX_C_SOURCE 200809L
#include "resource_sampler.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Single non-waiting publisher for periodic and request-boundary callers.
 * The public backend getter does not acquire an inference mutex or sync a GPU. */
void resource_sampler_capture(struct resource_sampler *s) {
    if (!s->channel || atomic_flag_test_and_set(&s->writing))
        return;
    struct geist_backend_resources resources;
    enum geist_status              rc = geist_backend_resources_snapshot(s->backend, &resources);
    struct lifecycle_memory sample    = {.sampled_ns = lifecycle_now_ns(),
                                         .allocated_bytes =
                                                 rc == GEIST_OK ? resources.allocated_bytes : 0,
                                         .status  = rc == GEIST_OK              ? 1
                                                    : rc == GEIST_E_UNSUPPORTED ? 2
                                                                                : 3,
                                         .source  = rc == GEIST_OK ? (unsigned) resources.source : 0,
                                         .unified = rc == GEIST_OK && resources.unified_memory};
    lifecycle_memory_write(s->channel, &sample);
    atomic_flag_clear(&s->writing);
}
static void *run(void *argument) {
    struct resource_sampler *s    = argument;
    uint64_t                 next = 0;
    while (!atomic_load(&s->stopped)) {
        uint64_t now = lifecycle_now_ns();
        if (now >= next) {
            resource_sampler_capture(s);
            next = now + 2000000000ull;
        }
        struct timespec delay = {.tv_nsec = 100000000};
        nanosleep(&delay, nullptr);
    }
    return nullptr;
}
void resource_sampler_start(struct resource_sampler *s,
                            struct geist_backend    *be,
                            struct lifecycle_shared *channel) {
    *s = (struct resource_sampler) {.backend = be, .channel = channel, .writing = ATOMIC_FLAG_INIT};
    atomic_init(&s->stopped, false);
    const char *disabled = getenv("GEIST_RESOURCE_SAMPLING");
    if (!channel || (disabled && !strcmp(disabled, "0"))) {
        s->channel = nullptr;
        return;
    }
    resource_sampler_capture(s);
    s->started = pthread_create(&s->thread, nullptr, run, s) == 0;
}
void resource_sampler_stop(struct resource_sampler *s) {
    atomic_store(&s->stopped, true);
    if (s->started)
        pthread_join(s->thread, nullptr);
    s->started = false;
    s->channel = nullptr;
}
