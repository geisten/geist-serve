#pragma once
#include "lifecycle.h"
#include <geist_util.h>
#include <pthread.h>
#include <stdatomic.h>
struct resource_sampler {
    struct geist_backend    *backend;
    struct lifecycle_shared *channel;
    pthread_t                thread;
    atomic_bool              stopped;
    atomic_flag              writing;
    bool                     started;
};
void resource_sampler_start(struct resource_sampler *s,
                            struct geist_backend    *be,
                            struct lifecycle_shared *channel);
void resource_sampler_capture(struct resource_sampler *s);
void resource_sampler_stop(struct resource_sampler *s);
