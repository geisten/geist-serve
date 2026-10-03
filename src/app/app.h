/* app.h — geist-app's shared state and internal API. geist-app: bounded C23 supervisor and same-origin UI. The model is owned
 * by a separate geistd process; no private engine headers are used.
 * Each HTTP worker owns a 256 KiB arena, released on every exit path.
 * One joinable model job and at most eight HTTP workers may exist. */
#pragma once

#include "core.h"
#include "daemon.h"
#include "output.h"
#include "tasks.h"
#include "compat.h"
#include "connection.h"
#include <sys/un.h>
#include "../json.h"
#include <arpa/inet.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdckdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif


#include "version.h"
#include "resources.h"
#include "activity.h"
#include "../lifecycle.h"
#include "performance.h"


extern char **environ;

#define WORKER_BYTES (256u * 1024u)
#define REQUEST_CAP 32768u
#define HEADER_CAP 8192u
#define WORKER_CAP 8
#define REQUEST_TIMEOUT_MS 5000
extern volatile sig_atomic_t interrupted;
extern atomic_bool closing, cancelled, compare_cancelled, request_cancelled, load_cancelled;
struct processor_measurement {
    double rate, first, total, tokens, rss, recorded;
};
/* geistd, the one child process: its lifecycle and the model it serves. */
struct app_child {
    pid_t                     pid, previous_pid;
    double                    probe_ms, reaped_ms, spawned_ms, loading_started, loaded_ms;
    char                      runtime_dir[64], socket_path[100];
    struct lifecycle_shared  *lifecycle;
    struct lifecycle_snapshot lifecycle_snapshot;
    unsigned                  lifecycle_phase, runtime_requests, runtime_threads;
    struct app_engine         engine;
    bool                      ready, generating;
    char                      active[160], active_id[64], chosen[APP_PATH_CAP];
    uint64_t                  operation_id;
};

/* Which processor runs the model: the user's mode and what was probed. */
struct app_backend {
    char mode[8], active[24], cpu[24], gpu[24], notice[256];
    bool gpu_available, verified, save;
};

/* The one model job: download, verify, activate. */
struct app_job {
    pthread_t               thread;
    const struct app_model *model;
    bool                    running, joinable, download, activate;
    uint64_t                received, verified_bytes;
    char                    phase[32];
    bool                    receipt_checked, receipt_hit;
};

/* The CPU/GPU comparison run. */
struct app_compare {
    pthread_t thread;
    bool      running, joinable;
    char      phase[24], result[24], user_mode[8];
    unsigned  step;
};

/* Memory and resource sampling around a model operation. */
struct app_observe {
    struct perf_record        *record;
    struct app_resource_window window;
    double                     sample_ms, cpu_sum, process_ms;
    unsigned                   cpu_samples;
    struct app_process_sample  process;
    uint64_t                   generation;
};

/* What the UI shows as progress, and the last failure. */
struct app_activity {
    struct activity load, request, download;
    char            request_phase[24];
};
struct app_error {
    char message[256], stage[24], model[64], backend[24];
    int  code;
};

/* Persisted choices and per-model measurements. */
struct app_prefs {
    char selected[64], answer_language[3], profile_series[768], measurement_identity[512];
    bool preview_accepted[APP_MODEL_COUNT];
    struct processor_measurement history[APP_MODEL_COUNT][2];
    struct processor_measurement speed[APP_MODEL_COUNT][2]; /* history, or a newer speed measurement */
    struct app_limits            limits; /* #103: verdict thresholds for this computer */
    char                         intent[16]; /* "chat" (all tasks) or one APP_QUALITY_TASKS entry */
};

/* All mutable app state, guarded by app.mutex. The sub-structs group fields
 * by the file that owns them; they share the one lock on purpose (no lock
 * ordering to get wrong). */
struct app_state {
    pthread_mutex_t mutex;
    pthread_cond_t  drained;
    struct {
        char home[APP_PATH_CAP], server[APP_PATH_CAP], models[APP_PATH_CAP];
    } paths;
    char                token[65], instance[80], message[512];
    unsigned            port, workers;
    uint64_t            generation;
    bool                stopping;
    struct app_child    child;
    struct app_backend  backend;
    struct app_job      job;
    struct app_compare  compare;
    struct app_observe  observe;
    struct app_activity activity;
    struct app_error    error;
    struct app_prefs    prefs;
};
extern struct app_state app;

struct request {
    char  method[8], path[256], host[128], origin[160], auth[128];
    char *body;
};

static inline bool path_join(char out[static APP_PATH_CAP], const char *base, const char *name) {
    int n = snprintf(out, APP_PATH_CAP, "%s/%s", base, name);
    return n > 0 && n < APP_PATH_CAP;
}

static inline bool mkdirs(const char *path) {
    char copy[APP_PATH_CAP];
    if (strlen(path) >= sizeof copy)
        return false;
    strcpy(copy, path);
    for (char *p = copy + 1;; ++p) {
        if (*p != '/' && *p != 0)
            continue;
        char end = *p;
        *p       = 0;
        if (mkdir(copy, 0700) != 0 && errno != EEXIST)
            return false;
        *p = end;
        if (!end)
            break;
    }
    return true;
}

static inline double monotonic_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static inline uint64_t regular_size(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 ? (uint64_t) st.st_size
                                                                          : 0;
}

/* request.c — the app's own HTTP: bounded request read with a deadline, responses, token auth, the loopback listener */
bool send_bytes(int fd, const void *data, size_t n);
bool response(int fd, int status, const char *type, const void *body, size_t n);
void error_response(int fd, int code, const char *message);
int read_request(int fd, struct app_arena *arena, struct request *r);
bool authorized(const struct request *r);
int listener(unsigned *port);

/* child.c — geistd supervision: backend probe, spawn, poll, stop, lifecycle sampling */
void probe_backends(void);
void probe_engine(void);
bool gpu_supported(const struct app_model *model);
bool recommend_gpu(const struct app_model *model);
void activity_change(struct activity *a, enum activity_stage stage);
void begin_activity(struct activity *a, enum activity_stage stage, uint64_t generation, const char *model);
void poll_child(void);
void stop_child(void);
bool start_child_mode(const char *path, const char *id, const char *mode);
bool start_child(const char *path, const char *id);

/* prefs.c — preferences on disk, per-model measurements and their pre-0.5 migration, the selected model */
bool read_preference(const char *name, char *out, size_t cap);
/* #103: "<fast_s> <usable_s> <reliable> <intent>", e.g. "10 30 0.9 chat"; false if out of range. */
bool verdict_settings_parse(const char *text, struct app_limits *limits, char intent[static 16]);
bool save_preference(const char *name, const char *value);
void restore_measurements(void);
void migrate_measurements(void);
bool save_selection(const char *id);
void restore_preview_preferences(void);

/* observe.c — memory and resource observation around a model operation */
void memory_generation(char *out, size_t capacity);
void memory_observe(struct app_memory_record *record);
void *monitor_main(void *unused);
void observation_begin(struct perf_record *r, const char *source, unsigned max, float temperature, float top_p);
void observation_end(struct perf_record *r, int rc, double first, double total, const struct app_run_stats *stats);

/* jobs.c — the one model job: download (curl), verify, activate */
void model_inventory(struct app_inventory items[static APP_MODEL_COUNT]);
bool model_stamp(const char *path, const struct app_model *m, char out[static 512]);
bool begin_job(const struct app_model *m, bool download, bool activate);

/* status.c — status JSON, execution-mode switching, catalog import */
void execution_response(int fd, const char *text);
void import_catalog(int fd, const char *text);
void status_response(int fd, struct app_arena *arena);

/* chat.c — the UI's generate proxy and the /v1 completions proxy to geistd */
void generate(int fd, struct request *r, struct app_arena *arena);
void api_error(int fd, int code, const char *message);
void completions(int fd, const struct request *r, struct app_arena *arena);

/* compare.c — the CPU/GPU comparison run */
void comparison_start(int fd, const char *body, struct app_arena *arena);

/* routes.c — request dispatch, connection info and the embedded web assets */
void connections(int fd, bool models);
void handle(int fd, struct app_arena *arena);

