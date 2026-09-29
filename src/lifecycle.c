#define _POSIX_C_SOURCE 200809L
#include "lifecycle.h"
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LC_MAGIC 0x474c4302u
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2, "Lifecycle requires lock-free 64-bit atomics");
struct lifecycle_shared {
    uint32_t      magic, size;
    atomic_ullong generation, sequence, process, started_ns, phase_ns[LC_PHASES];
    atomic_ullong memory_sequence, memory_time, memory_bytes, memory_status, memory_source,
            memory_unified;
};
uint64_t lifecycle_now_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now))
        return 0;
    return (uint64_t) now.tv_sec * 1000000000 + (uint64_t) now.tv_nsec;
}
const char *lifecycle_name(enum lifecycle_phase phase) {
    static const char *const names[] = {"idle", "backend", "model", "metadata", "warmup", "ready"};
    return phase >= LC_NONE && phase < LC_PHASES ? names[phase] : "unknown";
}
void lifecycle_close(struct lifecycle_shared **state) {
    if (*state) {
        munmap(*state, sizeof **state);
        *state = nullptr;
    }
}
int lifecycle_create(const char *directory, uint64_t generation, struct lifecycle_shared **out) {
    *out = nullptr;
    char path[4096];
    int  n = snprintf(path, sizeof path, "%s/lifecycle-XXXXXX", directory);
    if (n < 0 || (size_t) n >= sizeof path)
        return -1;
    int fd = mkstemp(path);
    if (fd < 0)
        return -1;
    if (unlink(path)) {
        close(fd);
        return -1;
    }
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) || ftruncate(fd, sizeof(struct lifecycle_shared))) {
        close(fd);
        return -1;
    }
    struct lifecycle_shared *state =
            mmap(nullptr, sizeof *state, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (state == MAP_FAILED) {
        close(fd);
        return -1;
    }
    state->magic = LC_MAGIC;
    state->size  = sizeof *state;
    atomic_init(&state->generation, generation);
    atomic_init(&state->sequence, 0);
    atomic_init(&state->process, 0);
    atomic_init(&state->started_ns, 0);
    for (unsigned i = 0; i < LC_PHASES; i++)
        atomic_init(&state->phase_ns[i], 0);
    atomic_init(&state->memory_sequence, 0);
    atomic_init(&state->memory_time, 0);
    atomic_init(&state->memory_bytes, 0);
    atomic_init(&state->memory_status, 0);
    atomic_init(&state->memory_source, 0);
    atomic_init(&state->memory_unified, 0);
    *out = state;
    return fd;
}
struct lifecycle_shared *lifecycle_inherit(void) {
    const char *value = getenv("GEIST_LIFECYCLE_FD");
    if (!value || strcmp(value, "4"))
        return nullptr;
    struct stat info;
    if (fstat(4, &info) || !S_ISREG(info.st_mode) || info.st_uid != getuid() ||
        info.st_nlink != 0 || (info.st_mode & 077) != 0 ||
        info.st_size != sizeof(struct lifecycle_shared))
        return nullptr;
    struct lifecycle_shared *state =
            mmap(nullptr, sizeof *state, PROT_READ | PROT_WRITE, MAP_SHARED, 4, 0);
    close(4);
    if (state == MAP_FAILED)
        return nullptr;
    if (state->magic != LC_MAGIC || state->size != sizeof *state) {
        lifecycle_close(&state);
        return nullptr;
    }
    atomic_store(&state->process, (uint64_t) getpid());
    atomic_store(&state->started_ns, lifecycle_now_ns());
    return state;
}
void lifecycle_phase(struct lifecycle_shared *state, enum lifecycle_phase phase) {
    if (!state || phase <= LC_NONE || phase >= LC_PHASES || atomic_load(&state->phase_ns[phase]))
        return;
    atomic_fetch_add(&state->sequence, 1);
    atomic_store(&state->phase_ns[phase], lifecycle_now_ns());
    atomic_fetch_add(&state->sequence, 1);
}
bool lifecycle_read(const struct lifecycle_shared *state,
                    uint64_t                       generation,
                    struct lifecycle_snapshot     *out) {
    *out = (struct lifecycle_snapshot) {0};
    if (!state)
        return false;
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        struct lifecycle_snapshot sample = {.sequence   = atomic_load(&state->sequence),
                                            .generation = atomic_load(&state->generation)};
        if (sample.generation != generation || (sample.sequence & 1))
            continue;
        sample.process    = atomic_load(&state->process);
        sample.started_ns = atomic_load(&state->started_ns);
        for (unsigned i = 0; i < LC_PHASES; i++)
            sample.phase_ns[i] = atomic_load(&state->phase_ns[i]);
        if (sample.sequence == atomic_load(&state->sequence) && sample.process &&
            sample.started_ns) {
            *out = sample;
            return true;
        }
    }
    return false;
}

void lifecycle_memory_write(struct lifecycle_shared *state, const struct lifecycle_memory *sample) {
    if (!state || !sample)
        return;
    atomic_fetch_add(&state->memory_sequence, 1);
    atomic_store(&state->memory_time, sample->sampled_ns);
    atomic_store(&state->memory_bytes, sample->allocated_bytes);
    atomic_store(&state->memory_status, sample->status);
    atomic_store(&state->memory_source, sample->source);
    atomic_store(&state->memory_unified, sample->unified);
    atomic_fetch_add(&state->memory_sequence, 1);
}
bool lifecycle_memory_read(const struct lifecycle_shared *state,
                           uint64_t                       generation,
                           struct lifecycle_memory       *out) {
    *out = (struct lifecycle_memory) {0};
    if (!state)
        return false;
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        struct lifecycle_memory s = {.generation = atomic_load(&state->generation),
                                     .process    = atomic_load(&state->process),
                                     .sequence   = atomic_load(&state->memory_sequence)};
        if (s.generation != generation || (s.sequence & 1) || !s.sequence || !s.process)
            continue;
        s.sampled_ns      = atomic_load(&state->memory_time);
        s.allocated_bytes = atomic_load(&state->memory_bytes);
        s.status          = (unsigned) atomic_load(&state->memory_status);
        s.source          = (unsigned) atomic_load(&state->memory_source);
        s.unified         = atomic_load(&state->memory_unified) != 0;
        if (s.sequence == atomic_load(&state->memory_sequence) && s.sampled_ns && s.status <= 3 &&
            s.source <= 1) {
            *out = s;
            return true;
        }
    }
    return false;
}
