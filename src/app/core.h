/* Application policy, independent of the inference engine. All sizes are bytes. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define APP_MODEL_COUNT 6
#define APP_GIB UINT64_C(1073741824)
#define APP_PATH_CAP 4096

struct app_arena {
    unsigned char *base; /* owned by the caller, never reallocated */
    size_t         used, cap;
};
[[nodiscard]] void *app_alloc(struct app_arena *a, size_t count, size_t size, size_t alignment);

struct app_buffer {
    char  *data; /* borrowed fixed storage */
    size_t len, cap;
    bool   failed; /* sticky: a failed response must never be sent as success */
};
void app_put(struct app_buffer *b, const char *s);
void app_printf(struct app_buffer *b, const char *format, ...);
void app_quote(struct app_buffer *b, const char *s);

struct app_model {
    const char *id, *name, *file, *url, *sha256;
    uint64_t    bytes;
    unsigned    working_mib; /* conservative planning estimate at <=4096 context */
    unsigned    recommended_ram_gib;
};
extern const struct app_model app_models[APP_MODEL_COUNT];
const struct app_model       *app_model_find(const char *id);

enum app_device { APP_UNKNOWN, APP_APPLE_SILICON, APP_PI5 };
struct app_hardware {
    char            name[160], arch[32];
    enum app_device device;
    uint64_t        ram, available, disk;
    unsigned        cores;
    bool            supported, available_known, disk_known;
};
[[nodiscard]] bool app_hardware_read(struct app_hardware *h, const char *directory);

enum app_fit { APP_RECOMMENDED, APP_CONDITIONAL, APP_UNAVAILABLE };
struct app_assessment {
    enum app_fit fit;
    const char  *reason;
    const char  *performance;
};
struct app_assessment
app_assess(const struct app_hardware *h, const struct app_model *m, bool installed);
struct app_assessment app_assess_observed(const struct app_hardware *h,
                                          const struct app_model    *m,
                                          bool                       installed,
                                          double                     tokens_per_second);
[[nodiscard]] bool    app_sha256(const char *path, char out[static 65]);
[[nodiscard]] bool
app_sha256_interruptible(const char *path, char out[static 65], bool (*cancel)(void));
