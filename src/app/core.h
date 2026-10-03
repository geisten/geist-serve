/* Application policy, independent of the inference engine. All sizes are bytes. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define APP_MODEL_COUNT 32
#define APP_CATALOG_BYTES 24576
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
/* Token pieces may end inside UTF-8. Emit only complete validated code points. */
struct app_utf8 {
    unsigned char bytes[4];
    unsigned      used, need;
    bool          failed;
};
[[nodiscard]] bool app_utf8_feed(struct app_utf8 *state, const char *piece, char *out, size_t cap);

struct app_model {
    const char *id, *name, *file, *url, *sha256;
    uint64_t    bytes;
    unsigned    working_mib; /* conservative planning estimate at <=4096 context */
    unsigned    recommended_ram_gib;
    unsigned    backends; /* 1: CPU, 2: Metal, 4: Vulkan; device probe is still required */
    const char *unsupported_format; /* optional catalog format unavailable in this engine */
    const char *group_id, *group_name,
            *quantization;        /* display grouping, never artifact identity */
    const char *reasoning_format; /* optional validated output protocol, not model-name inference */
    const char *quality;          /* optional validated reference benchmark JSON (#102) */
    unsigned    quality_passed, quality_total; /* its sums over tasks and languages */
    unsigned    quality_task[4][2]; /* passed, total per APP_QUALITY_TASKS entry (#103 intent) */
};
extern struct app_model app_models[APP_MODEL_COUNT];
extern size_t           app_model_count;
extern unsigned         app_catalog_revision;
extern const char      *app_catalog_json;
extern const char      *app_quality_suite; /* current mini benchmark suite id */
/* The mini benchmark tasks an intent can name; "chat" means all of them. */
#define APP_QUALITY_TASKS {"classify", "extract", "format", "context"}
struct app_catalog;
struct app_catalog     *app_catalog_parse(const char *text, char error[static 256]);
const struct app_model *app_catalog_find(const struct app_catalog *catalog, const char *id);
const struct app_model *app_catalog_entry(const struct app_catalog *catalog, size_t index);
unsigned                app_catalog_version(const struct app_catalog *catalog);
void app_catalog_apply(struct app_catalog *catalog); /* caller holds catalog/app lock */
void app_catalog_discard(struct app_catalog *catalog);
const struct app_model *app_model_find(const char *id);

enum app_device { APP_UNKNOWN, APP_APPLE_SILICON, APP_PI5 };
struct app_hardware {
    char            name[160], arch[32], os[128];
    enum app_device device;
    uint64_t        ram, available, disk;
    unsigned        cores, logical_cpus;
    bool            supported, available_known, disk_known;
};
[[nodiscard]] bool app_hardware_read(struct app_hardware *h, const char *directory);

enum app_fit { APP_RECOMMENDED, APP_CONDITIONAL, APP_UNAVAILABLE };
#define APP_INTERACTIVE_TPS 8.0
/* Zero means not measured. A partial CPU/GPU comparison is not a device verdict. */
double app_device_rate(double cpu_rate, bool gpu_available, double gpu_rate);
bool   app_rate_below_target(double rate);
struct app_assessment {
    enum app_fit fit;
    const char  *reason;
    const char  *performance;
};
struct app_assessment
app_assess(const struct app_hardware *h, const struct app_model *m, bool installed);
struct app_assessment app_assess_device(const struct app_hardware *h,
                                        const struct app_model    *m,
                                        bool                       installed,
                                        double                     cpu_rate,
                                        bool                       gpu_available,
                                        double                     gpu_rate);
struct app_assessment app_assess_observed(const struct app_hardware *h,
                                          const struct app_model    *m,
                                          bool                       installed,
                                          double                     tokens_per_second);
[[nodiscard]] bool    app_engine_sha256(const char *path, char out[static 65]);
[[nodiscard]] bool    app_sha256(const char *path, char out[static 65]);
[[nodiscard]] bool
app_sha256_interruptible(const char *path, char out[static 65], bool (*cancel)(void));

/* Product policy belongs to the shared application, not the inference library.
 * Inventory is a snapshot; eligibility must be checked again before setup. */
struct app_inventory {
    bool     installed;
    uint64_t partial;
    double   tps;
};
struct app_recommendation {
    const struct app_model *model, *preferred;
    const char             *source, *reason;
    bool                    eligible;
};
struct app_recommendation
app_recommend(const struct app_hardware *h,
              const struct app_inventory inventory[static APP_MODEL_COUNT],
              const char                *selected,
              const char                *running);
