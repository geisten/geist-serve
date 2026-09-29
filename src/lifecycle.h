#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Private inherited channel. Same architecture/ABI in parent and child;
 * no pointers, strings, paths or model input. A bounded snapshot never waits. */
enum lifecycle_phase { LC_NONE, LC_BACKEND, LC_MODEL, LC_METADATA, LC_WARMUP, LC_READY, LC_PHASES };
struct lifecycle_shared;
struct lifecycle_snapshot {
    uint64_t generation, sequence, process, started_ns;
    uint64_t phase_ns[LC_PHASES];
};
int lifecycle_create(const char *directory, uint64_t generation, struct lifecycle_shared **out);
struct lifecycle_shared *lifecycle_inherit(void);
void                     lifecycle_close(struct lifecycle_shared **state);
void        lifecycle_phase(struct lifecycle_shared *state, enum lifecycle_phase phase);
bool        lifecycle_read(const struct lifecycle_shared *state,
                           uint64_t                       generation,
                           struct lifecycle_snapshot     *out);
uint64_t    lifecycle_now_ns(void);
const char *lifecycle_name(enum lifecycle_phase phase);
