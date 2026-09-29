/* geist-app: bounded C23 supervisor and same-origin UI. The model is owned
 * by a separate geistd process; no private engine headers are used.
 * Each HTTP worker owns a 256 KiB arena, released on every exit path.
 * One joinable model job and at most eight HTTP workers may exist. */
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
extern char **environ;

#if defined(__has_embed) && !defined(APP_NO_EMBED)
static const unsigned char page[] = {
#embed "../../web/index.html"
};
static const unsigned char style[] = {
#embed "../../web/app.css"
};
static const unsigned char translations[] = {
#embed "../../web/i18n.js"
};
static const unsigned char script[] = {
#embed "../../web/app.js"
};
static const unsigned char marked_js[] = {
#embed "../../web/vendor/marked.umd.js"
};
static const unsigned char katex_js[] = {
#embed "../../web/vendor/katex.min.js"
};
static const unsigned char markdown_js[] = {
#embed "../../web/markdown.js"
};
#else
/* GCC 14 supports the C23 language used here but not #embed yet. */
#include "../../build/app_assets.h"
#endif

#include "version.h"
#include "resources.h"
#include "activity.h"
#include "../lifecycle.h"
#include "performance.h"

#define WORKER_BYTES (256u * 1024u)
#define REQUEST_CAP 32768u
#define HEADER_CAP 8192u
#define WORKER_CAP 8
#define REQUEST_TIMEOUT_MS 5000
static volatile sig_atomic_t interrupted;
static atomic_bool closing, cancelled, compare_cancelled, request_cancelled, load_cancelled;
struct processor_measurement {
    double rate, first, total, tokens, rss, recorded;
};
static struct {
    pthread_mutex_t            mutex;
    pthread_cond_t             drained;
    char                       home[APP_PATH_CAP], server[APP_PATH_CAP], models[APP_PATH_CAP];
    char                       token[65], message[512], active[160], active_id[64];
    char                       chosen[APP_PATH_CAP], selected[64], answer_language[3];
    char                       execution_mode[8], backend[24], cpu_backend[24], gpu_backend[24];
    char                       execution_notice[256];
    char                       request_phase[24], last_error[256], error_stage[24];
    char                       error_model[64], error_backend[24];
    int                        error_code;
    bool                       gpu_available, save_execution, backend_verified;
    double                     loading_started, loaded_ms;
    unsigned                   runtime_requests, runtime_threads;
    char                       profile_series[768];
    struct app_engine          engine;
    struct activity            load_activity, request_activity, download_activity;
    uint64_t                   operation_id, generation;
    char                       instance[80];
    bool                       stopping;
    double                     child_probe_ms;
    struct lifecycle_shared   *lifecycle;
    struct lifecycle_snapshot  lifecycle_snapshot;
    unsigned                   lifecycle_phase;
    pid_t                      previous_pid;
    double                     reaped_ms, spawned_ms;
    bool                       receipt_checked, receipt_hit;
    uint64_t                   verified_bytes;
    struct perf_record        *observation;
    struct app_resource_window observation_window;
    double                     sample_ms, cpu_sum;
    unsigned                   cpu_samples;
    bool                       preview_accepted[APP_MODEL_COUNT];
    unsigned                   port, workers;
    char                       runtime_dir[64], socket_path[100];
    pid_t                      child;
    bool                       ready, generating, job_running, job_joinable;
    bool                       comparing, compare_joinable;
    pthread_t                  comparison;
    char                       compare_phase[24], compare_result[24];
    unsigned                   compare_step;
    pthread_t                  job;
    const struct app_model    *job_model;
    bool                       job_download, job_activate;
    uint64_t                   received;
    char                       phase[32];
    struct {
        double   tps;
        unsigned tokens;
    } measurements[APP_MODEL_COUNT];
    struct processor_measurement history[APP_MODEL_COUNT][2];
    char                         measurement_identity[512];
} app = {.mutex = PTHREAD_MUTEX_INITIALIZER, .drained = PTHREAD_COND_INITIALIZER};

static bool read_preference(const char *, char *, size_t);
static bool save_preference(const char *, const char *);
static bool start_child_mode(const char *, const char *, const char *);
static void stop_child(void);

/* Probe the packaged engine once. Old CPU-only daemons remain usable. */
static void probe_backends(void) {
#if defined(__aarch64__) || defined(__arm64__)
    strcpy(app.cpu_backend, "cpu_neon");
#elif defined(__x86_64__)
    strcpy(app.cpu_backend, "cpu_x86");
#else
    strcpy(app.cpu_backend, "cpu_scalar");
#endif
    int pipefd[2];
    if (pipe(pipefd))
        return;
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions)) {
        close(pipefd[0]);
        close(pipefd[1]);
        return;
    }
    int rc = posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    if (!rc)
        rc = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    char *args[] = {app.server, "--backends", nullptr};
    pid_t child  = 0;
    if (!rc)
        rc = posix_spawn(&child, app.server, &actions, nullptr, args, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]);
    char   output[1024] = {0};
    size_t used         = 0;
    int    status       = 0;
    bool   ended        = false;
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    for (unsigned attempt = 0; !rc && attempt < 100; ++attempt) {
        ssize_t n = read(pipefd[0], output + used, sizeof output - 1 - used);
        if (n > 0)
            used += (size_t) n;
        if (waitpid(child, &status, WNOHANG) == child) {
            ended = true;
            break;
        }
        struct timespec pause = {.tv_nsec = 50000000};
        nanosleep(&pause, nullptr);
    }
    if (!rc && !ended) {
        kill(child, SIGKILL);
        while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
        }
    }
    ssize_t n = read(pipefd[0], output + used, sizeof output - 1 - used);
    if (n > 0)
        used += (size_t) n;
    output[used] = 0;
    close(pipefd[0]);
    struct json *j = calloc(1, sizeof *j);
    if (!rc && ended && WIFEXITED(status) && WEXITSTATUS(status) == 0 && j &&
        json_parse(j, used, output) >= 0) {
        int   gpu  = json_get(j, 0, "gpu");
        char *name = json_strdup(j, json_get(j, gpu, "name"));
        if (name && (!strcmp(name, "metal") || !strcmp(name, "vulkan"))) {
            snprintf(app.gpu_backend, sizeof app.gpu_backend, "%s", name);
            app.gpu_available = json_bool(j, json_get(j, gpu, "available"), false);
        }
        free(name);
    }
    free(j);
}

static bool gpu_supported(const struct app_model *model) {
    unsigned bit = !strcmp(app.gpu_backend, "metal") ? 2 : 4;
    return app.gpu_available && model && (model->backends & bit);
}
static bool recommend_gpu(const struct app_model *model) {
    /* A hardware default, not a claim of measured performance. */
    return gpu_supported(model) && model->bytes >= APP_GIB;
}

static bool path_join(char out[static APP_PATH_CAP], const char *base, const char *name) {
    int n = snprintf(out, APP_PATH_CAP, "%s/%s", base, name);
    return n > 0 && n < APP_PATH_CAP;
}

static bool mkdirs(const char *path) {
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

static bool send_bytes(int fd, const void *data, size_t n) {
    const char *p = data;
    while (n) {
        ssize_t k = send(fd, p, n, 0);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            return false;
        p += k;
        n -= (size_t) k;
    }
    return true;
}

static double monotonic_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

/* One deadline covers headers and body. Per-recv timeouts alone let a
 * trickling client occupy a bounded worker indefinitely. */
static ssize_t request_recv(int fd, void *buffer, size_t size, double deadline) {
    while (!atomic_load(&closing)) {
        double left = deadline - monotonic_ms();
        if (left <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        struct pollfd wait  = {.fd = fd, .events = POLLIN};
        int           ms    = left > 50 ? 50 : (int) left + 1;
        int           ready = poll(&wait, 1, ms);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0)
            return -1;
        if (!ready)
            continue;
        ssize_t n = recv(fd, buffer, size, MSG_DONTWAIT);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        return n;
    }
    errno = ECANCELED;
    return -1;
}

static bool response(int fd, int status, const char *type, const void *body, size_t n) {
    char header[1024];
    int  k = snprintf(
            header,
            sizeof header,
            "HTTP/1.1 %d Response\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
            "Connection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
            "Referrer-Policy: no-referrer\r\n"
            "Content-Security-Policy: default-src 'none'; script-src 'self'; style-src 'self'; "
            "connect-src 'self'; img-src 'self' data:; base-uri 'none'; frame-ancestors 'none'; "
            "form-action 'self'\r\n\r\n",
            status,
            type,
            n);
    return k > 0 && (size_t) k < sizeof header && send_bytes(fd, header, (size_t) k) &&
           send_bytes(fd, body, n);
}

static void error_response(int fd, int code, const char *message) {
    char              body[1024];
    struct app_buffer b = {.data = body, .cap = sizeof body};
    app_put(&b, "{\"error\":");
    app_quote(&b, message);
    app_put(&b, "}");
    response(fd, code, "application/json", body, b.len);
}

struct request {
    char  method[8], path[256], host[128], origin[160], auth[128];
    char *body;
};

static bool header_value(char *out, size_t cap, const char *s) {
    while (*s == ' ' || *s == '\t')
        ++s;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t'))
        --n;
    if (n >= cap || *out)
        return false;
    memcpy(out, s, n);
    out[n] = 0;
    return true;
}

static bool loopback_host(const char *host) {
    const char *port = nullptr;
    if (strncmp(host, "127.0.0.1:", 10) == 0)
        port = host + 10;
    if (strncmp(host, "localhost:", 10) == 0)
        port = host + 10;
    if (!port || !*port)
        return false;
    unsigned value = 0;
    for (; *port; ++port) {
        if (*port < '0' || *port > '9' || value > 6553)
            return false;
        value = value * 10 + (unsigned) (*port - '0');
    }
    return value > 0 && value <= 65535;
}

/* All activity mutations use app.mutex. A service heartbeat is never progress. */
static void activity_change(struct activity *a, enum activity_stage stage) {
    (void) activity_step(a, a->id, a->generation, stage, monotonic_ms());
}
static void begin_activity(struct activity    *a,
                           enum activity_stage stage,
                           uint64_t            generation,
                           const char         *model) {
    activity_begin(a, ++app.operation_id, generation, stage, monotonic_ms());
    snprintf(a->model, sizeof a->model, "%s", model ? model : "custom");
    snprintf(a->backend, sizeof a->backend, "%s", app.backend);
    a->engine = app.engine;
}

static int read_request(int fd, struct app_arena *arena, struct request *r) {
    double deadline = monotonic_ms() + REQUEST_TIMEOUT_MS;
    char  *head     = app_alloc(arena, HEADER_CAP + 1, 1, 1);
    r->body         = app_alloc(arena, REQUEST_CAP + 1, 1, 1);
    if (!head || !r->body)
        return 503;
    size_t got = 0;
    char  *end = nullptr;
    while (!end) {
        if (atomic_load(&closing))
            return 400;
        if (got == HEADER_CAP)
            return 431;
        ssize_t n = request_recv(fd, head + got, HEADER_CAP - got, deadline);
        if (n <= 0)
            return n < 0 && errno == ETIMEDOUT ? 408 : 400;
        got += (size_t) n;
        head[got] = 0;
        end       = strstr(head, "\r\n\r\n");
    }
    char *line = strstr(head, "\r\n");
    char  version[16], extra;
    if (!line)
        return 400;
    *line = 0;
    if (sscanf(head, "%7s %255s %15s %c", r->method, r->path, version, &extra) != 3 ||
        (strcmp(version, "HTTP/1.1") && strcmp(version, "HTTP/1.0")))
        return 400;
    bool   have_length = false;
    size_t length      = 0;
    for (line += 2; line < end;) {
        char *next = strstr(line, "\r\n");
        if (!next)
            return 400;
        *next = 0;
        if (strncasecmp(line, "Host:", 5) == 0) {
            if (!header_value(r->host, sizeof r->host, line + 5))
                return 400;
        } else if (strncasecmp(line, "Origin:", 7) == 0) {
            if (!header_value(r->origin, sizeof r->origin, line + 7))
                return 400;
        } else if (strncasecmp(line, "Authorization:", 14) == 0) {
            if (!header_value(r->auth, sizeof r->auth, line + 14))
                return 400;
        } else if (strncasecmp(line, "Transfer-Encoding:", 18) == 0)
            return 400;
        else if (strncasecmp(line, "Content-Length:", 15) == 0) {
            if (have_length)
                return 400;
            have_length   = true;
            const char *p = line + 15;
            while (*p == ' ')
                ++p;
            if (!*p)
                return 400;
            for (; *p; ++p) {
                if (*p < '0' || *p > '9')
                    return 400;
                length = length * 10 + (unsigned) (*p - '0');
                if (length > REQUEST_CAP)
                    return 413;
            }
        }
        line = next + 2;
    }
    if (!loopback_host(r->host))
        return 403;
    if (*r->origin) {
        char expected[160];
        snprintf(expected, sizeof expected, "http://%s", r->host);
        if (strcmp(r->origin, expected))
            return 403;
    }
    size_t offset = (size_t) (end + 4 - head), have = got - offset;
    if (have > length)
        have = length;
    memcpy(r->body, head + offset, have);
    while (have < length) {
        if (atomic_load(&closing))
            return 400;
        ssize_t n = request_recv(fd, r->body + have, length - have, deadline);
        if (n <= 0)
            return n < 0 && errno == ETIMEDOUT ? 408 : 400;
        have += (size_t) n;
    }
    if (memchr(r->body, 0, length))
        return 400;
    r->body[length] = 0;
    return 0;
}

static bool authorized(const struct request *r) {
    char expected[80];
    snprintf(expected, sizeof expected, "Bearer %s", app.token);
    if (strlen(r->auth) != strlen(expected))
        return false;
    unsigned mismatch = 0;
    for (size_t i = 0; expected[i]; ++i)
        mismatch |= (unsigned char) r->auth[i] ^ (unsigned char) expected[i];
    return mismatch == 0;
}

static int listener(unsigned *port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int reuse = 1;
    (void) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
    (void) fcntl(fd, F_SETFD, FD_CLOEXEC);
    struct sockaddr_in sa = {.sin_family      = AF_INET,
                             .sin_port        = htons((uint16_t) *port),
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (bind(fd, (struct sockaddr *) &sa, sizeof sa) != 0 || listen(fd, 16) != 0) {
        close(fd);
        return -1;
    }
    socklen_t n = sizeof sa;
    if (getsockname(fd, (struct sockaddr *) &sa, &n) != 0) {
        close(fd);
        return -1;
    }
    *port = ntohs(sa.sin_port);
    return fd;
}

/* Called with app.mutex held. The child is ours: never attach to or stop
 * Ollama or another user's process. Reap before inspecting health. */
static void lifecycle_sample(void) {
    struct lifecycle_snapshot sample;
    if (!lifecycle_read(app.lifecycle, app.generation, &sample) ||
        sample.process != (uint64_t) app.child)
        return;
    app.lifecycle_snapshot                    = sample;
    static const enum activity_stage stages[] = {
            ACT_NONE, ACT_BACKEND, ACT_MODEL, ACT_METADATA, ACT_WARMUP, ACT_READY};
    for (unsigned i = app.lifecycle_phase + 1; i < LC_READY; i++) {
        if (!sample.phase_ns[i])
            break;
        double when = (double) sample.phase_ns[i] / 1e6;
        if (when < app.load_activity.event_at)
            when = app.load_activity.event_at;
        (void) activity_step(
                &app.load_activity, app.load_activity.id, app.generation, stages[i], when);
        app.lifecycle_phase = i;
    }
}

static void poll_child(void) {
    if (!app.child || app.stopping)
        return;
    if (atomic_load(&load_cancelled) && !app.ready) {
        activity_change(&app.load_activity, ACT_STOPPING);
        stop_child();
        (void) activity_end(&app.load_activity, "cancelled", 499, monotonic_ms());
        return;
    }
    lifecycle_sample();
    int   status;
    pid_t result    = waitpid(app.child, &status, WNOHANG);
    bool  timed_out = !app.ready && monotonic_ms() - app.loading_started > 120000;
    if (result == app.child || (result < 0 && errno == ECHILD) || timed_out) {
        if (timed_out)
            stop_child();
        app.child = 0;
        app.ready = false;
        /* A replacement's verification may already have failed or be active.
         * Reaping the previous generation must not overwrite that newer
         * outcome, restart the old GPU choice or erase its diagnostic. */
        if (app.load_activity.generation > app.generation)
            return;
        (void) activity_end(&app.load_activity, "failed", timed_out ? 504 : 502, monotonic_ms());
        if (strcmp(app.backend, app.cpu_backend)) {
            char path[APP_PATH_CAP], id[64], log[APP_PATH_CAP], archive[APP_PATH_CAP];
            snprintf(path, sizeof path, "%s", app.chosen);
            snprintf(id, sizeof id, "%s", app.active_id);
            bool preserved = false;
            if (path_join(log, app.home, "server.log") &&
                path_join(archive, app.home, "gpu-failure-XXXXXX")) {
                int saved = mkstemp(archive);
                if (saved >= 0) {
                    close(saved);
                    preserved = rename(log, archive) == 0;
                    if (!preserved)
                        unlink(archive);
                }
            }
            if (!preserved) {
                snprintf(app.message,
                         sizeof app.message,
                         "GPU failed. Diagnostics could not be archived; the model remains "
                         "stopped.");
                return;
            }
            snprintf(app.execution_notice,
                     sizeof app.execution_notice,
                     "GPU stopped or failed to load. Restored CPU; diagnostics are kept in the app "
                     "data folder.");
            if (start_child_mode(path, id, "cpu"))
                return;
        }
        snprintf(app.message,
                 sizeof app.message,
                 "The model process stopped. See server.log in the app data folder.");
        return;
    }
    if (app.ready)
        return;
    if (monotonic_ms() - app.child_probe_ms < 250)
        return;
    app.child_probe_ms = monotonic_ms();
    char              reported[24];
    struct app_engine identity;
    if (app_daemon_identity(app.socket_path, reported, &identity)) {
        snprintf(identity.payload_sha256,
                 sizeof identity.payload_sha256,
                 "%s",
                 app.engine.payload_sha256);
        app.engine           = identity;
        app.backend_verified = !strcmp(reported, app.backend);
        if (reported[0] && !app.backend_verified) {
            stop_child();
            (void) activity_end(&app.load_activity, "failed", 502, monotonic_ms());
            snprintf(app.message,
                     sizeof app.message,
                     "The engine reported a different processor. Reload the model.");
            return;
        }
        app.loaded_ms            = monotonic_ms() - app.loading_started;
        app.ready                = true;
        app.load_activity.engine = app.engine;
        activity_change(&app.load_activity, ACT_READY);
        (void) activity_end(&app.load_activity, "completed", 0, monotonic_ms());
        app.message[0]                = 0;
        const struct app_model *model = app_model_find(app.active_id);
        if (app.save_execution && model) {
            char key[80];
            snprintf(key, sizeof key, "backend-%s", model->sha256);
            if (!save_preference(key, app.execution_mode))
                snprintf(app.execution_notice,
                         sizeof app.execution_notice,
                         "The model is running, but its execution preference could not be saved.");
        }
        app.save_execution = false;
    }
}

static void stop_child(void) {
    /* Readiness is revoked before releasing the mutex. Other workers can report
     * cached status, but cannot replace, reuse or reap this owned process. */
    app.ready    = false;
    app.stopping = true;
    if (!app.child)
        goto cleanup;
    app.previous_pid = app.child;
    kill(app.child, SIGTERM);
    for (unsigned i = 0; i < 20; ++i) {
        int   status;
        pid_t result = waitpid(app.child, &status, WNOHANG);
        if (result == app.child || (result < 0 && errno == ECHILD)) {
            app.child     = 0;
            app.reaped_ms = monotonic_ms();
            break;
        }
        struct timespec pause = {.tv_nsec = 25000000};
        pthread_mutex_unlock(&app.mutex);
        nanosleep(&pause, nullptr);
        pthread_mutex_lock(&app.mutex);
    }
    if (app.child) {
        kill(app.child, SIGKILL);
        while (waitpid(app.child, nullptr, 0) < 0 && errno == EINTR) {
        }
    }
    app.child     = 0;
    app.reaped_ms = monotonic_ms();
cleanup:
    lifecycle_close(&app.lifecycle);
    app.lifecycle_snapshot = (struct lifecycle_snapshot) {0};
    app.lifecycle_phase    = 0;
    app.stopping           = false;
    app.ready              = false;
    if (app.socket_path[0])
        unlink(app.socket_path);
    if (app.runtime_dir[0])
        rmdir(app.runtime_dir);
    app.socket_path[0] = app.runtime_dir[0] = 0;
}

static bool start_child_mode_impl(const char *path, const char *id, const char *mode) {
    const struct app_model *model = app_model_find(id);
    bool gpu = !strcmp(mode, "gpu") || (!strcmp(mode, "auto") && recommend_gpu(model));
    if (gpu && !gpu_supported(model)) {
        snprintf(app.message,
                 sizeof app.message,
                 "GPU is not supported by this model and packaged engine.");
        return false;
    }
    if (!app.load_activity.id || app.load_activity.outcome[0])
        begin_activity(&app.load_activity, ACT_STARTING, app.generation + 1, id);
    snprintf(app.load_activity.backend,
             sizeof app.load_activity.backend,
             "%s",
             gpu ? app.gpu_backend : app.cpu_backend);
    atomic_store(&load_cancelled, false);
    if (app.child)
        activity_change(&app.load_activity, ACT_STOPPING);
    stop_child();
    activity_change(&app.load_activity, ACT_STARTING);
    app.engine = (struct app_engine) {0};
    (void) app_engine_sha256(app.server, app.engine.payload_sha256);
    struct app_hardware hardware;
    bool                known = app_hardware_read(&hardware, app.home);
    if (!known || !hardware.supported) {
        snprintf(app.message,
                 sizeof app.message,
                 "This CPU/platform does not support the bundled inference engine.");
        return false;
    }
    strcpy(app.runtime_dir, "/tmp/geist-app-XXXXXX");
    if (!mkdtemp(app.runtime_dir)) {
        app.runtime_dir[0] = 0;
        return false;
    }
    snprintf(app.socket_path, sizeof app.socket_path, "%s/inference.sock", app.runtime_dir);
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", app.socket_path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) != 0 ||
        bind(fd, (struct sockaddr *) &sa, sizeof sa) != 0 || chmod(app.socket_path, 0600) != 0 ||
        listen(fd, 8) != 0) {
        if (fd >= 0)
            close(fd);
        stop_child();
        return false;
    }
    char logpath[APP_PATH_CAP];
    if (!path_join(logpath, app.home, "server.log")) {
        close(fd);
        return false;
    }
    int log = open(logpath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (log < 0) {
        close(fd);
        return false;
    }
    char *args[] = {
            app.server, (char *) path, "--socket", app.socket_path, "--sessions", "1", nullptr};
    size_t count = 0;
    while (environ[count])
        ++count;
    char **env = calloc(count + 6, sizeof *env);
    if (!env) {
        close(fd);
        close(log);
        return false;
    }
    int lifecycle_fd = lifecycle_create(app.runtime_dir, app.generation + 1, &app.lifecycle);
    if (lifecycle_fd < 0) {
        free(env);
        close(fd);
        close(log);
        return false;
    }
    size_t used = 0;
    for (size_t i = 0; i < count; ++i)
        if (strncmp(environ[i], "LISTEN_FDS=", 11) && strncmp(environ[i], "LISTEN_PID=", 11) &&
            strncmp(environ[i], "OMP_NUM_THREADS=", 16) &&
            strncmp(environ[i], "OMP_WAIT_POLICY=", 16) &&
            strncmp(environ[i], "GEIST_LIFECYCLE_FD=", 19) &&
            strncmp(environ[i], "GEIST_BACKEND=", 14))
            env[used++] = environ[i];
    unsigned cores = known ? hardware.cores : 1;
    unsigned limit = known && hardware.device == APP_PI5 ? 4 : 2;
    if (cores > limit)
        cores = limit;
    app.runtime_threads  = cores;
    app.runtime_requests = 0;
    char threads[40];
    snprintf(threads, sizeof threads, "OMP_NUM_THREADS=%u", cores);
    env[used++] = threads;
    env[used++] = "OMP_WAIT_POLICY=passive";
    env[used++] = "LISTEN_FDS=1";
    env[used++] = "GEIST_LIFECYCLE_FD=4";
    char backend_env[48];
    snprintf(backend_env,
             sizeof backend_env,
             "GEIST_BACKEND=%s",
             gpu ? app.gpu_backend : app.cpu_backend);
    env[used++] = backend_env;
    posix_spawn_file_actions_t actions;
    int                        rc = posix_spawn_file_actions_init(&actions);
    if (!rc) {
        rc = posix_spawn_file_actions_adddup2(&actions, log, STDOUT_FILENO);
        if (!rc)
            rc = posix_spawn_file_actions_adddup2(&actions, log, STDERR_FILENO);
        if (!rc)
            rc = posix_spawn_file_actions_adddup2(&actions, fd, 3);
        if (!rc)
            rc = posix_spawn_file_actions_adddup2(&actions, lifecycle_fd, 4);
        if (!rc)
            rc = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        if (!rc) {
            app.spawned_ms = monotonic_ms();
            rc             = posix_spawn(&app.child, app.server, &actions, nullptr, args, env);
        }
        posix_spawn_file_actions_destroy(&actions);
    }
    free(env);
    close(lifecycle_fd);
    close(fd);
    close(log);
    if (rc) {
        app.child = 0;
        snprintf(app.message, sizeof app.message, "Cannot start geistd: %s", strerror(rc));
        return false;
    }
    snprintf(app.chosen, sizeof app.chosen, "%s", path);
    snprintf(app.active_id, sizeof app.active_id, "%s", id ? id : "custom");
    snprintf(app.execution_mode, sizeof app.execution_mode, "%s", mode);
    snprintf(app.backend, sizeof app.backend, "%s", gpu ? app.gpu_backend : app.cpu_backend);
    ++app.generation;
    app.load_activity.generation = app.generation;
    app.load_activity.engine     = app.engine;
    activity_change(&app.load_activity, ACT_LOADING);
    app.loading_started  = monotonic_ms();
    app.backend_verified = false;
    app.save_execution   = true;
    const char *base     = strrchr(path, '/');
    base                 = base ? base + 1 : path;
    snprintf(app.active, sizeof app.active, "%s", base);
    char *ext = strstr(app.active, ".gguf");
    if (ext)
        *ext = 0;
    snprintf(app.message, sizeof app.message, "Loading the model into memory…");
    return true;
}

static bool start_child_mode(const char *path, const char *id, const char *mode) {
    bool ok = start_child_mode_impl(path, id, mode);
    if (!ok) {
        stop_child();
        (void) activity_end(&app.load_activity, "failed", 502, monotonic_ms());
    }
    return ok;
}

static bool start_child(const char *path, const char *id) {
    char                    mode[8] = "auto", key[80];
    const struct app_model *model   = app_model_find(id);
    if (model) {
        snprintf(key, sizeof key, "backend-%s", model->sha256);
        if (!read_preference(key, mode, sizeof mode) ||
            (strcmp(mode, "cpu") && strcmp(mode, "gpu")) ||
            (!strcmp(mode, "gpu") && !gpu_supported(model)))
            strcpy(mode, "auto");
    }
    app.execution_notice[0] = 0;
    return start_child_mode(path, id, mode);
}

static uint64_t regular_size(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 ? (uint64_t) st.st_size
                                                                          : 0;
}

/* Only fixed application keys are passed here. Atomic private files never
 * contain prompts, output or capabilities. Callers hold app.mutex after startup. */
static bool read_preference(const char *name, char *out, size_t cap) {
    char path[APP_PATH_CAP];
    if (!path_join(path, app.home, name))
        return false;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return false;
    struct stat st;
    bool        ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == getuid() &&
                     st.st_nlink == 1 && st.st_size > 0 && (uint64_t) st.st_size < cap;
    ssize_t     n  = ok ? read(fd, out, cap - 1) : -1;
    close(fd);
    if (n <= 0 || n != st.st_size || memchr(out, 0, (size_t) n)) {
        out[0] = 0;
        return false;
    }
    out[n] = 0;
    return true;
}

static bool save_preference(const char *name, const char *value) {
    char target[APP_PATH_CAP], temporary[APP_PATH_CAP];
    if (!path_join(target, app.home, name) || !path_join(temporary, app.home, ".preference-XXXXXX"))
        return false;
    int fd = mkstemp(temporary);
    if (fd < 0)
        return false;
    size_t n  = strlen(value);
    bool   ok = write(fd, value, n) == (ssize_t) n && fsync(fd) == 0;
    close(fd);
    if (ok)
        ok = rename(temporary, target) == 0;
    if (!ok)
        unlink(temporary);
    return ok;
}

/* Legacy last-reply files remain readable for one-time archival migration. */
static void measurement_key(char key[128], const struct app_model *model, unsigned slot) {
    snprintf(
            key, 128, "performance-%s-%s", model->sha256, slot ? app.gpu_backend : app.cpu_backend);
}
static bool valid_measurement(const struct processor_measurement *m) {
    return isfinite(m->rate) && m->rate > 0 && m->rate <= 1e9 && isfinite(m->first) &&
           m->first >= 0 && m->first <= 3600 && isfinite(m->total) && m->total >= m->first &&
           m->total <= 3600 && isfinite(m->tokens) && m->tokens >= 1 && m->tokens <= 1000000 &&
           m->tokens == (double) (uint64_t) m->tokens && isfinite(m->rss) && m->rss >= 0 &&
           m->rss <= 1e15 && isfinite(m->recorded) && m->recorded > 0 &&
           m->recorded <= (double) time(nullptr) + 300;
}
static void restore_measurements(void) {
    memset(app.history, 0, sizeof app.history);
    for (size_t i = 0; i < app_model_count; i++)
        for (unsigned slot = 0; slot < 2; slot++) {
            const char        *backend = slot ? app.gpu_backend : app.cpu_backend;
            struct perf_record r;
            perf_last(app_models[i].sha256, app.profile_series, backend, &r);
            if (r.id[0])
                app.history[i][slot] =
                        (struct processor_measurement) {.rate  = r.output / (r.generation_ns / 1e9),
                                                        .first = r.first_ns / 1e9,
                                                        .total = r.total_ns / 1e9,
                                                        .tokens   = r.output,
                                                        .rss      = r.rss > 0 ? r.rss : 0,
                                                        .recorded = r.timestamp};
        }
}
static void migrate_measurements(void) {
    for (size_t i = 0; i < app_model_count; i++)
        for (unsigned slot = 0; slot < 2; slot++) {
            if (slot && !app.gpu_backend[0])
                continue;
            char key[128], text[1024];
            measurement_key(key, &app_models[i], slot);
            if (!read_preference(key, text, sizeof text) || strncmp(text, "v1 ", 3))
                continue;
            char *identity = strchr(text, '\n'),
                 *numbers  = identity ? strchr(identity + 1, '\n') : nullptr;
            if (!identity || !numbers)
                continue;
            struct processor_measurement m    = {0};
            int                          used = 0;
            if (sscanf(numbers + 1,
                       "%lf %lf %lf %lf %lf %lf%n",
                       &m.rate,
                       &m.first,
                       &m.total,
                       &m.tokens,
                       &m.rss,
                       &m.recorded,
                       &used) != 6 ||
                numbers[1 + used] || !valid_measurement(&m))
                continue;
            *numbers             = 0;
            struct perf_record r = {.timestamp     = m.recorded,
                                    .output        = m.tokens,
                                    .generation_ns = m.tokens / m.rate * 1e9,
                                    .first_ns      = m.first * 1e9,
                                    .total_ns      = m.total * 1e9,
                                    .rss           = m.rss,
                                    .peak_rss      = -1,
                                    .cpu_percent   = -1,
                                    .load_ns       = -1,
                                    .prefill_ns    = -1,
                                    .top_p         = 1};
            snprintf(r.model, sizeof r.model, "%s", app_models[i].id);
            snprintf(r.artifact, sizeof r.artifact, "%s", app_models[i].sha256);
            snprintf(r.backend, sizeof r.backend, "%s", slot ? app.gpu_backend : app.cpu_backend);
            snprintf(r.quantization, sizeof r.quantization, "%s", app_models[i].quantization);
            snprintf(r.id, sizeof r.id, "legacy-%s-%s-%.0f", r.artifact, r.backend, m.recorded);
            snprintf(r.series, sizeof r.series, "legacy;unknown-engine-config;%s", identity + 1);
            strcpy(r.source, "legacy_last_reply");
            strcpy(r.outcome, "completed");
            strcpy(r.finish, "unknown");
            perf_import(&r);
        }
}
/* These functions capture/update the one active request under app.mutex. Disk
 * work is confined to performance.c, after this mutex is released. */
static void observation_sample(bool force) {
    if (!app.observation || (!force && monotonic_ms() - app.sample_ms < 2000))
        return;
    app.sample_ms = monotonic_ms();
    struct app_process_sample sample;
    if (app.child <= 0 || !app_process_read(app.child, &sample))
        return;
    struct perf_record *r = app.observation;
    r->rss                = (double) sample.rss;
    if (r->rss > r->peak_rss)
        r->peak_rss = r->rss;
    ++r->samples;
    struct app_hardware h;
    if (app_hardware_read(&h, app.models)) {
        app_resource_update(&app.observation_window, &sample, h.logical_cpus);
        if (app.observation_window.cpu_known) {
            app.cpu_sum += app.observation_window.cpu_percent;
            ++app.cpu_samples;
            r->cpu_percent = app.cpu_sum / app.cpu_samples;
        }
    }
}
/* Cached lifecycle status stays independent of HTTP admission and UI polling. */
static void *monitor_main(void *unused) {
    (void) unused;
    while (!atomic_load(&closing)) {
        pthread_mutex_lock(&app.mutex);
        poll_child();
        observation_sample(false);
        pthread_mutex_unlock(&app.mutex);
        struct timespec pause = {.tv_nsec = 100000000};
        nanosleep(&pause, nullptr);
    }
    return nullptr;
}
static void observation_begin(
        struct perf_record *r, const char *source, unsigned max, float temperature, float top_p) {
    memset(r, 0, sizeof *r);
    perf_begin(r);
    r->engine                     = app.engine;
    const struct app_model *model = app_model_find(app.active_id);
    snprintf(r->model, sizeof r->model, "%s", app.active_id);
    /* Uncatalogued files have no verified artifact identity and stay diagnostic. */
    snprintf(r->artifact,
             sizeof r->artifact,
             "%s",
             model ? model->sha256 : "unknown-custom-artifact");
    snprintf(r->quantization, sizeof r->quantization, "%s", model ? model->quantization : "");
    snprintf(r->backend, sizeof r->backend, "%s", app.backend);
    snprintf(r->series,
             sizeof r->series,
             "%s",
             model ? app.profile_series : "unknown-custom-series");
    snprintf(r->source, sizeof r->source, "%s", source);
    r->threads     = app.runtime_threads;
    r->max_tokens  = max;
    r->temperature = temperature;
    r->top_p       = top_p;
    r->cold        = app.runtime_requests++ == 0;
    r->load_ns     = r->cold ? app.loaded_ms * 1e6 : -1;
    r->contention  = app.job_running && !app.job_activate;
    begin_activity(&app.request_activity, ACT_CONNECT, app.generation, app.active_id);
    atomic_store(&request_cancelled, false);
    app.observation        = r;
    app.observation_window = (struct app_resource_window) {0};
    app.cpu_samples        = 0;
    app.cpu_sum            = 0;
    observation_sample(true);
}
static void observation_end(struct perf_record         *r,
                            int                         rc,
                            double                      first,
                            double                      total,
                            const struct app_run_stats *stats) {
    pthread_mutex_lock(&app.mutex);
    observation_sample(true);
    if (rc == 498 && atomic_load(&request_cancelled))
        rc = 499;
    app.observation    = nullptr;
    r->generation_ns   = stats->generation_ns;
    r->first_ns        = first > 0 ? first * 1e6 : -1;
    r->first_answer_ns = stats->first_answer_ns;
    r->reasoning       = stats->reasoning;
    if (stats->max_tokens)
        r->max_tokens = stats->max_tokens;
    r->total_ns   = total * 1e6;
    r->prefill_ns = stats->prefill_ns;
    r->input      = stats->prompt_tokens;
    r->output     = stats->tokens;
    r->reused     = stats->reused;
    strcpy(r->outcome,
           rc == 499                       ? "cancelled"
           : rc == 498                     ? "disconnected"
           : rc == 422 && stats->no_answer ? "no_answer"
           : rc == 400                     ? "error"
           : rc                            ? "interrupted"
           : stats->no_answer              ? "no_answer"
                                           : "completed");
    strcpy(r->finish, rc ? "unknown" : stats->limited ? "length" : "stop");
    (void) activity_end(&app.request_activity,
                        rc == 499 ? "cancelled"
                        : rc      ? "failed"
                                  : "completed",
                        rc,
                        monotonic_ms());
    pthread_mutex_unlock(&app.mutex);
    perf_submit(r);
    pthread_mutex_lock(&app.mutex);
    restore_measurements();
    /* Prefill is a synchronous engine call. A closed client alone cannot interrupt it.
     * Reap only our owned child before admitting a new request; preserve its diagnostics. */
    if ((rc == 499 || rc == 498 || rc == 502 || rc == 504) && !atomic_load(&closing)) {
        char path[APP_PATH_CAP], id[64], mode[8], log[APP_PATH_CAP], archive[APP_PATH_CAP];
        snprintf(path, sizeof path, "%s", app.chosen);
        snprintf(id, sizeof id, "%s", app.active_id);
        snprintf(mode, sizeof mode, "%s", app.execution_mode);
        begin_activity(&app.load_activity, ACT_STOPPING, app.generation + 1, id);
        stop_child();
        bool saved = false;
        if (path_join(log, app.home, "server.log") &&
            path_join(archive, app.home, "request-failure-XXXXXX")) {
            int fd = mkstemp(archive);
            if (fd >= 0) {
                close(fd);
                saved = rename(log, archive) == 0;
                if (!saved)
                    unlink(archive);
            }
        }
        if (saved)
            (void) start_child_mode(path, id, mode);
        else {
            (void) activity_end(&app.load_activity, "failed", 502, monotonic_ms());
            snprintf(app.message,
                     sizeof app.message,
                     "The model is stopped. Its diagnostics could not be archived.");
        }
    }
    app.generating = false;
    pthread_mutex_unlock(&app.mutex);
}

static bool save_selection(const char *id) {
    if (!save_preference("selected", id))
        return false;
    snprintf(app.selected, sizeof app.selected, "%s", id);
    return true;
}

static void model_inventory(struct app_inventory items[static APP_MODEL_COUNT]) {
    for (size_t i = 0; i < app_model_count; ++i) {
        char path[APP_PATH_CAP], part[APP_PATH_CAP];
        bool valid = path_join(path, app.models, app_models[i].file);
        items[i]   = (struct app_inventory) {.installed = valid &&
                                                          regular_size(path) == app_models[i].bytes,
                                             .tps = app_device_rate(app.history[i][0].rate,
                                                                    gpu_supported(&app_models[i]),
                                                                    app.history[i][1].rate)};
        if (valid && snprintf(part, sizeof part, "%s.part", path) < (int) sizeof part)
            items[i].partial = regular_size(part);
    }
}

struct download_sink {
    FILE    *file;
    uint64_t offset, bytes, limit;
};
static size_t download_write(char *p, size_t size, size_t n, void *opaque) {
    struct download_sink *s = opaque;
    size_t                total;
    uint64_t              end;
    if (atomic_load(&cancelled) || atomic_load(&closing) || ckd_mul(&total, size, n) ||
        ckd_add(&end, s->bytes, total) || end > s->limit)
        return 0;
    size_t written = fwrite(p, 1, total, s->file);
    s->bytes += written;
    pthread_mutex_lock(&app.mutex);
    app.received = s->bytes;
    activity_progress(app.job_activate ? &app.load_activity : &app.download_activity,
                      s->bytes,
                      monotonic_ms());
    pthread_mutex_unlock(&app.mutex);
    return written;
}
static int
download_progress(void *p, curl_off_t total, curl_off_t now, curl_off_t up, curl_off_t sent) {
    (void) p;
    (void) total;
    (void) now;
    (void) up;
    (void) sent;
    return atomic_load(&cancelled) || atomic_load(&closing);
}

static bool download_model(const struct app_model *m, const char *part, char *why, size_t cap) {
    int fd = open(part, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        snprintf(why, cap, "Cannot create download file: %s", strerror(errno));
        return false;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uint64_t) st.st_size > m->bytes) {
        close(fd);
        snprintf(why, cap, "Invalid partial download. Remove the .part file and retry.");
        return false;
    }
    FILE *file = fdopen(fd, "r+b");
    if (!file) {
        close(fd);
        snprintf(why, cap, "Cannot open download stream.");
        return false;
    }
    struct download_sink sink = {.file   = file,
                                 .offset = (uint64_t) st.st_size,
                                 .bytes  = (uint64_t) st.st_size,
                                 .limit  = m->bytes};
    bool                 ok   = sink.bytes == m->bytes;
    CURL                *curl = nullptr;
    if (!ok && fseeko(file, 0, SEEK_END) == 0 && (curl = curl_easy_init())) {
        const char *url = m->url;
#ifdef APP_TESTING
        const char *fixture = getenv("GEIST_TEST_MODEL_URL");
        if (fixture)
            url = fixture;
#endif
        curl_easy_setopt(curl, CURLOPT_URL, url);
#ifndef __APPLE__
        /* Static Alpine builds also run on Debian/Raspberry Pi OS. Use
         * the host's maintained trust store, not Alpine's compiled-in path. */
        if (access("/etc/ssl/certs/ca-certificates.crt", R_OK) == 0)
            curl_easy_setopt(curl, CURLOPT_CAINFO, "/etc/ssl/certs/ca-certificates.crt");
        else if (access("/etc/ssl/cert.pem", R_OK) == 0)
            curl_easy_setopt(curl, CURLOPT_CAINFO, "/etc/ssl/cert.pem");
#endif
#ifdef APP_TESTING
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#endif
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 256L * 1024L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 128L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, (curl_off_t) sink.offset);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, download_write);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, download_progress);
        CURLcode rc     = curl_easy_perform(curl);
        long     status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        ok = rc == CURLE_OK && (status == 200 || status == 206) && sink.bytes == m->bytes;
        if (!ok)
            snprintf(why,
                     cap,
                     "Download paused: %s. Retry resumes the partial file.",
                     curl_easy_strerror(rc));
        curl_easy_cleanup(curl);
    } else if (!ok)
        snprintf(why, cap, "Cannot allocate the download connection.");
    if (fflush(file) != 0 || fsync(fd) != 0) {
        ok = false;
        snprintf(why, cap, "Cannot save download: disk full or I/O error.");
    }
    if (fclose(file) != 0)
        ok = false;
    return ok;
}

static bool job_cancelled(void) {
    return atomic_load(&cancelled) || atomic_load(&closing);
}

/* A private receipt avoids rereading unchanged multi-GB files on each selection.
 * Bind it to the expected digest AND the file identity, including nanosecond
 * ctime: restoring mtime after an edit must not preserve the receipt. This is
 * a local cache, not an attestation against code running as the same user. */
static bool model_stamp(const char *path, const struct app_model *m, char out[static 512]) {
    struct stat s;
    if (lstat(path, &s) != 0 || !S_ISREG(s.st_mode) || s.st_size < 0 ||
        (uint64_t) s.st_size != m->bytes)
        return false;
#ifdef __APPLE__
    struct timespec modified = s.st_mtimespec, changed = s.st_ctimespec;
#else
    struct timespec modified = s.st_mtim, changed = s.st_ctim;
#endif
    int n = snprintf(out,
                     512,
                     "v1 %s %ju %ju %ju %ju %ju %ju %ju %jd %ld %jd %ld",
                     m->sha256,
                     (uintmax_t) s.st_dev,
                     (uintmax_t) s.st_ino,
                     (uintmax_t) s.st_size,
                     (uintmax_t) s.st_mode,
                     (uintmax_t) s.st_uid,
                     (uintmax_t) s.st_gid,
                     (uintmax_t) s.st_nlink,
                     (intmax_t) modified.tv_sec,
                     modified.tv_nsec,
                     (intmax_t) changed.tv_sec,
                     changed.tv_nsec);
    return n > 0 && n < 512;
}

static void *model_job(void *unused) {
    (void) unused;
    const struct app_model *m                    = app.job_model;
    char                    target[APP_PATH_CAP] = "", part[APP_PATH_CAP], hash[65], why[512] = "";
    bool                    ok = path_join(target, app.models, m->file);
    int                     n  = snprintf(part, sizeof part, "%s.part", target);
    ok                         = ok && n > 0 && (size_t) n < sizeof part;
    if (ok && app.job_download)
        ok = download_model(m, part, why, sizeof why);
    const char *verify = app.job_download ? part : target;
    char        key[80], before[512] = "", after[512] = "", receipt[512] = "";
    snprintf(key, sizeof key, "verified-%s", m->sha256);
    bool stamped = ok && model_stamp(verify, m, before);
    bool cached  = !app.job_download && stamped && read_preference(key, receipt, sizeof receipt) &&
                   !strcmp(before, receipt);
    pthread_mutex_lock(&app.mutex);
    if (app.job_activate) {
        app.receipt_checked = true;
        app.receipt_hit     = cached;
        app.verified_bytes  = 0;
    }
    pthread_mutex_unlock(&app.mutex);
    if (ok && !atomic_load(&cancelled) && !atomic_load(&closing)) {
        if (!cached) {
            pthread_mutex_lock(&app.mutex);
            strcpy(app.phase, "verifying");
            activity_change(app.job_activate ? &app.load_activity : &app.download_activity,
                            ACT_HASH);
            pthread_mutex_unlock(&app.mutex);
#ifdef APP_TESTING
            fprintf(stderr, "model verification: hashing %s\n", m->id);
#endif
        }
        bool size_ok = stamped;
        bool hash_ok = cached || (size_ok && app_sha256_interruptible(verify, hash, job_cancelled));
        pthread_mutex_lock(&app.mutex);
        if (app.job_activate && !cached && hash_ok)
            app.verified_bytes = m->bytes;
        pthread_mutex_unlock(&app.mutex);
        bool stable = model_stamp(verify, m, after) && !strcmp(before, after);
        ok          = hash_ok && stable && (cached || strcmp(hash, m->sha256) == 0);
        if (!ok) {
            snprintf(why,
                     sizeof why,
                     "Checksum or size mismatch. The model was not started; download it again.");
            if (!job_cancelled() && (!size_ok || (hash_ok && stable)))
                unlink(verify);
            else if (!job_cancelled())
                snprintf(
                        why,
                        sizeof why,
                        "Cannot read the model for verification. Check disk and file permissions.");
        }
        if (ok && app.job_download) {
            /* Keep the verified inode open across rename. Capture its new ctime
             * only if the destination still describes that exact file. */
            int         verified_fd = open(part, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
            struct stat held, placed;
            ok = verified_fd >= 0 && model_stamp(part, m, after) && !strcmp(before, after);
            if (ok && rename(part, target) != 0) {
                ok = false;
                snprintf(why, sizeof why, "Cannot finish download: %s", strerror(errno));
            }
            ok = ok && fstat(verified_fd, &held) == 0 && lstat(target, &placed) == 0 &&
                 held.st_dev == placed.st_dev && held.st_ino == placed.st_ino &&
                 model_stamp(target, m, after);
            if (verified_fd >= 0)
                close(verified_fd);
            if (!ok && !*why)
                snprintf(why, sizeof why, "Cannot finish verification. The model file changed.");
        }
        if (ok && !cached && !job_cancelled())
            (void) save_preference(key, after);
    }
    pthread_mutex_lock(&app.mutex);
    if (atomic_load(&cancelled) || atomic_load(&closing)) {
        snprintf(app.message,
                 sizeof app.message,
                 "Download or verification cancelled. Partial downloads can be resumed.");
    } else if (ok) {
        if (!app.job_activate)
            snprintf(app.message, sizeof app.message, "Download complete.");
        else if (start_child(target, m->id))
            (void) save_selection(m->id);
    } else
        snprintf(app.message, sizeof app.message, "%s", *why ? why : "Cannot prepare the model.");
    struct activity *job_activity = app.job_activate ? &app.load_activity : &app.download_activity;
    if (atomic_load(&cancelled) || atomic_load(&closing))
        (void) activity_end(job_activity, "cancelled", 499, monotonic_ms());
    else if (!ok || (app.job_activate && !app.child))
        (void) activity_end(job_activity, "failed", 502, monotonic_ms());
    else if (!app.job_activate)
        (void) activity_end(job_activity, "completed", 0, monotonic_ms());
    app.job_running = false;
    app.phase[0]    = 0;
    pthread_mutex_unlock(&app.mutex);
    return nullptr;
}

static bool begin_job(const struct app_model *m, bool download, bool activate) {
    /* Startup also enters here from the saved selection, without an HTTP
     * assessment. Never hash/download/start a known unsupported format. */
    if (m->unsupported_format || !m->backends) {
        snprintf(app.message,
                 sizeof app.message,
                 "This model requires PQ2_0 and Hadamard support, unavailable in the bundled "
                 "engine.");
        return false;
    }
    if (app.job_joinable) {
        pthread_join(app.job, nullptr);
        app.job_joinable = false;
    }
    if (activate) {
        app.receipt_checked = false;
        app.receipt_hit     = false;
        app.verified_bytes  = 0;
    }
    begin_activity(activate ? &app.load_activity : &app.download_activity,
                   download ? ACT_DOWNLOAD : ACT_RECEIPT,
                   activate ? app.generation + 1 : app.generation,
                   m->id);
    if (activate)
        atomic_store(&load_cancelled, false);
    app.job_model    = m;
    app.job_download = download;
    /* Captured under the mutex. A background download must never replace the
     * resident daemon, even if it becomes idle or exits before completion. */
    app.job_activate = activate;
    app.job_running  = true;
    if (download && app.observation)
        app.observation->contention = true;
    app.received   = 0;
    app.message[0] = 0;
    strcpy(app.phase, download ? "downloading" : "preparing");
    atomic_store(&cancelled, false);
    if (pthread_create(&app.job, nullptr, model_job, nullptr) != 0) {
        app.job_running = false;
        app.phase[0]    = 0;
        (void) activity_end(activate ? &app.load_activity : &app.download_activity,
                            "failed",
                            503,
                            monotonic_ms());
        strcpy(app.message, "Cannot start model worker.");
        return false;
    }
    app.job_joinable = true;
    return true;
}

static void restore_preview_preferences(void) {
    memset(app.preview_accepted, 0, sizeof app.preview_accepted);
    for (size_t i = 0; i < app_model_count; ++i) {
        char key[80], value[8] = "";
        snprintf(key, sizeof key, "preview-%s", app_models[i].sha256);
        app.preview_accepted[i] = read_preference(key, value, sizeof value) && !strcmp(value, "v1");
    }
}
static void execution_response(int fd, const char *text) {
    struct json *j    = calloc(1, sizeof *j);
    char        *mode = nullptr;
    if (j && json_parse(j, strlen(text), text) >= 0)
        mode = json_strdup(j, json_get(j, 0, "mode"));
    free(j);
    if (!mode || (strcmp(mode, "auto") && strcmp(mode, "cpu") && strcmp(mode, "gpu"))) {
        free(mode);
        error_response(fd, 400, "Choose Auto, CPU or GPU.");
        return;
    }
    pthread_mutex_lock(&app.mutex);
    const struct app_model *model = app_model_find(app.active_id);
    int                     code  = 202;
    const char             *error = nullptr;
    if (!app.ready || app.comparing || app.generating || app.job_running) {
        code  = 409;
        error = "Wait until the loaded model is idle before changing execution.";
    } else if (!strcmp(mode, "gpu") && !gpu_supported(model)) {
        code  = 409;
        error = "GPU is not supported by this model and packaged engine.";
    } else {
        bool        gpu = !strcmp(mode, "gpu") || (!strcmp(mode, "auto") && recommend_gpu(model));
        const char *backend = gpu ? app.gpu_backend : app.cpu_backend;
        if (!strcmp(backend, app.backend)) {
            char key[80];
            if (model)
                snprintf(key, sizeof key, "backend-%s", model->sha256);
            if (model && !save_preference(key, mode)) {
                code  = 500;
                error = "Cannot save execution preference.";
            } else {
                snprintf(app.execution_mode, sizeof app.execution_mode, "%s", mode);
                code = 200;
            }
        } else {
            char path[APP_PATH_CAP], id[64];
            snprintf(path, sizeof path, "%s", app.chosen);
            snprintf(id, sizeof id, "%s", app.active_id);
            app.execution_notice[0] = 0;
            if (model)
                memset(&app.measurements[model - app_models], 0, sizeof app.measurements[0]);
            if (!start_child_mode(path, id, mode)) {
                code  = 500;
                error = "Cannot change execution. Restoring CPU.";
                (void) start_child_mode(path, id, "cpu");
            }
        }
    }
    pthread_mutex_unlock(&app.mutex);
    free(mode);
    if (error)
        error_response(fd, code, error);
    else
        response(fd, code, "application/json", "{}", 2);
}
static void import_catalog(int fd, const char *text) {
    char                why[256];
    struct app_catalog *candidate = app_catalog_parse(text, why);
    if (!candidate) {
        error_response(fd, 400, why);
        return;
    }
    pthread_mutex_lock(&app.mutex);
    int code = 200;
    if (app.comparing || app.job_running || app.generating || (app.child && !app.ready)) {
        code = 409;
        snprintf(why, sizeof why, "Finish the current operation before importing a catalog.");
    } else if (app_catalog_version(candidate) <= app_catalog_revision) {
        code = 409;
        snprintf(why, sizeof why, "Import a catalog with a newer revision.");
    } else {
        const struct app_model *old  = app_model_find(app.active_id);
        const struct app_model *next = app_catalog_find(candidate, app.active_id);
        if (app.ready && old &&
            (!next || strcmp(old->sha256, next->sha256) || strcmp(old->file, next->file) ||
             old->backends != next->backends || old->bytes != next->bytes)) {
            code = 409;
            snprintf(why, sizeof why, "The running model must stay unchanged in this catalog.");
        }
        for (size_t i = 0; code == 200 && i < app_model_count; ++i) {
            const struct app_model *previous = &app_models[i];
            /* A filename may not be reassigned to another hash while files exist. */
            char path[APP_PATH_CAP], part[APP_PATH_CAP];
            for (size_t k = 0; app_catalog_entry(candidate, k); ++k) {
                const struct app_model *replacement = app_catalog_entry(candidate, k);
                if (replacement && !strcmp(previous->file, replacement->file) &&
                    (strcmp(previous->sha256, replacement->sha256) ||
                     previous->bytes != replacement->bytes) &&
                    path_join(path, app.models, previous->file) &&
                    (regular_size(path) ||
                     (snprintf(part, sizeof part, "%s.part", path) < (int) sizeof part &&
                      regular_size(part)))) {
                    code = 409;
                    snprintf(why,
                             sizeof why,
                             "Use a new filename when replacing an existing model hash.");
                    break;
                }
            }
        }
    }
    if (code == 200 && !save_preference("catalog.json", text)) {
        code = 500;
        snprintf(why, sizeof why, "Cannot save the catalog. The previous catalog is kept.");
    }
    if (code == 200) {
        app_catalog_apply(candidate);
        candidate = nullptr;
        memset(app.measurements, 0, sizeof app.measurements);
        restore_preview_preferences();
        restore_measurements();
        if (!app_model_find(app.selected))
            app.selected[0] = 0;
    }
    pthread_mutex_unlock(&app.mutex);
    app_catalog_discard(candidate);
    if (code == 200)
        response(fd, 200, "application/json", "{}", 2);
    else
        error_response(fd, code, why);
}

static struct app_resource_window resource_window; /* protected by app.mutex */

static void status_response(int fd, struct app_arena *arena) {
    char *body = app_alloc(arena, 65536, 1, 1);
    if (!body) {
        error_response(fd, 503, "Request memory budget exhausted.");
        return;
    }
    struct app_buffer   b = {.data = body, .cap = 65536};
    struct app_hardware h;
    bool                known = app_hardware_read(&h, app.models);
    pthread_mutex_lock(&app.mutex);
    struct app_inventory inventory[APP_MODEL_COUNT];
    model_inventory(inventory);
    struct app_recommendation recommendation =
            app_recommend(&h, inventory, app.selected, app.ready ? app.active_id : nullptr);
    app_put(&b, "{\"version\":");
    app_quote(&b, APP_VERSION);
    app_printf(&b, ",\"catalog_revision\":%u", app_catalog_revision);
    app_put(&b, ",\"recommendation\":{\"id\":");
    app_quote(&b, recommendation.model ? recommendation.model->id : "");
    app_put(&b, ",\"preferred_id\":");
    app_quote(&b, recommendation.preferred->id);
    app_put(&b, ",\"source\":");
    app_quote(&b, recommendation.source);
    app_put(&b, ",\"reason\":");
    app_quote(&b, recommendation.reason);
    app_printf(&b,
               ",\"eligible\":%s},\"answer_language\":",
               recommendation.eligible ? "true" : "false");
    app_quote(&b, app.answer_language);
    const struct app_model *execution_model = app_model_find(app.active_id);
    bool                    gpu             = gpu_supported(execution_model);
    app_put(&b, ",\"engine\":");
    app_engine_json(&b, &app.engine);
    app_put(&b, ",\"activity\":{\"instance\":");
    app_quote(&b, app.instance);
    app_put(&b, ",\"load\":");
    activity_json(&b,
                  &app.load_activity,
                  monotonic_ms(),
                  app.child && app.load_activity.generation == app.generation);
    app_put(&b, ",\"request\":");
    activity_json(&b,
                  &app.request_activity,
                  monotonic_ms(),
                  app.child && app.request_activity.generation == app.generation);
    app_put(&b, ",\"download\":");
    activity_json(&b, &app.download_activity, monotonic_ms(), false);
    app_printf(&b, "},\"process_generation\":%llu", (unsigned long long) app.generation);
    app_put(&b, ",\"lifecycle\":{");
    app_printf(&b,
               "\"generation\":%llu,\"pid\":%ld,\"previous_pid\":%ld,\"spawned_ms\":%.3f,\"reaped_"
               "ms\":%.3f,\"receipt\":",
               (unsigned long long) app.generation,
               (long) app.child,
               (long) app.previous_pid,
               app.spawned_ms,
               app.reaped_ms);
    app_quote(&b, app.receipt_checked ? (app.receipt_hit ? "hit" : "miss") : "not_checked");
    app_printf(&b,
               ",\"verified_bytes\":%llu,\"engine_phases\":[",
               (unsigned long long) app.verified_bytes);
    bool phase_comma = false;
    for (unsigned i = LC_BACKEND; i < LC_PHASES; i++) {
        uint64_t start = app.lifecycle_snapshot.phase_ns[i];
        if (!start)
            continue;
        if (phase_comma)
            app_put(&b, ",");
        phase_comma  = true;
        uint64_t end = i == LC_READY ? start : lifecycle_now_ns();
        for (unsigned j = i + 1; j < LC_PHASES; j++)
            if (app.lifecycle_snapshot.phase_ns[j]) {
                end = app.lifecycle_snapshot.phase_ns[j];
                break;
            }
        app_put(&b, "{\"stage\":");
        app_quote(&b, lifecycle_name(i));
        app_printf(&b,
                   ",\"started_ms\":%.3f,\"duration_ms\":%.3f}",
                   (double) start / 1e6,
                   (double) (end - start) / 1e6);
    }
    app_put(&b, "]}");
    app_put(&b, ",\"request_phase\":");
    app_quote(&b, app.generating ? app.request_phase : "idle");
    app_put(&b, ",\"last_error\":{\"message\":");
    app_quote(&b, app.last_error);
    app_put(&b, ",\"stage\":");
    app_quote(&b, app.error_stage);
    app_put(&b, ",\"model\":");
    app_quote(&b, app.error_model);
    app_put(&b, ",\"backend\":");
    app_quote(&b, app.error_backend);
    app_printf(&b, ",\"code\":%d}", app.error_code);
    app_put(&b, ",\"execution\":{\"mode\":");
    app_quote(&b, app.execution_mode[0] ? app.execution_mode : "auto");
    app_put(&b, ",\"active\":");
    app_quote(&b, app.ready ? (!strcmp(app.backend, app.cpu_backend) ? "cpu" : "gpu") : "");
    app_put(&b, ",\"backend\":");
    app_quote(&b, app.ready ? app.backend : "");
    app_put(&b, ",\"recommended\":");
    app_quote(&b, recommend_gpu(execution_model) ? "gpu" : "cpu");
    app_printf(&b,
               ",\"basis\":\"hardware\",\"verified\":%s,\"gpu_available\":%s,\"gpu_backend\":",
               app.ready && app.backend_verified ? "true" : "false",
               gpu ? "true" : "false");
    app_quote(&b, app.gpu_backend);
    app_put(&b, ",\"reason\":");
    app_quote(&b,
              !gpu ? "GPU is not supported by this model and packaged engine."
              : recommend_gpu(execution_model)
                      ? "GPU is suggested for this larger model. This is a hardware default, not a "
                        "measured speed comparison."
                      : "CPU is suggested for this small model. This is a hardware default, not a "
                        "measured speed comparison.");
    app_put(&b, ",\"notice\":");
    app_quote(&b, app.execution_notice);
    double execution_rate = 0;
    if (app.ready && app.backend_verified && execution_model) {
        unsigned slot  = !strcmp(app.backend, app.cpu_backend) ? 0 : 1;
        execution_rate = app.history[execution_model - app_models][slot].rate;
    }
    app_printf(&b,
               ",\"performance\":{\"target_tps\":%.1f,\"below_target\":%s,\"rate\":",
               APP_INTERACTIVE_TPS,
               app_rate_below_target(execution_rate) ? "true" : "false");
    if (isfinite(execution_rate) && execution_rate > 0)
        app_printf(&b, "%.6f", execution_rate);
    else
        app_put(&b, "null");
    app_put(&b, "}}");
    app_printf(&b,
               ",\"comparison\":{\"running\":%s,\"step\":%u,\"phase\":",
               app.comparing ? "true" : "false",
               app.compare_step);
    app_quote(&b, app.compare_phase);
    app_put(&b, ",\"result\":");
    app_quote(&b, app.compare_result);
    app_put(&b, "}");
    app_put(&b, ",\"performance_history\":[");
    bool comma = false;
    if (execution_model) {
        for (unsigned slot = 0; slot < 2; ++slot) {
            const struct processor_measurement *m =
                    &app.history[execution_model - app_models][slot];
            if (!m->recorded)
                continue;
            if (comma)
                app_put(&b, ",");
            comma = true;
            app_printf(&b, "{\"processor\":\"%s\",\"backend\":", slot ? "gpu" : "cpu");
            app_quote(&b, slot ? app.gpu_backend : app.cpu_backend);
            app_printf(&b,
                       ",\"rate\":%.6f,\"first\":%.6f,\"total\":%.6f,\"tokens\":%.0f,"
                       "\"rss_bytes\":%.0f,\"recorded_at\":%.0f}",
                       m->rate,
                       m->first,
                       m->total,
                       m->tokens,
                       m->rss,
                       m->recorded);
        }
    }
    app_put(&b, "]");
    app_put(&b, ",\"hardware\":{\"name\":");
    app_quote(&b, h.name);
    app_put(&b, ",\"arch\":");
    app_quote(&b, h.arch);
    app_put(&b, ",\"os\":");
    app_quote(&b, h.os);
    app_printf(&b, ",\"logical_cpus\":%u", h.logical_cpus);
    app_put(&b, ",\"device\":");
    app_quote(&b,
              h.device == APP_APPLE_SILICON ? "apple-silicon"
              : h.device == APP_PI5         ? "pi5"
                                            : "unknown");
    app_printf(&b,
               ",\"ram\":%llu,\"available\":%llu,\"disk\":%llu,\"cores\":%u,\"known\":%s,\"disk_"
               "known\":%s,\"available_known\":%s},",
               (unsigned long long) h.ram,
               (unsigned long long) h.available,
               (unsigned long long) h.disk,
               h.cores,
               known ? "true" : "false",
               h.disk_known ? "true" : "false",
               h.available_known ? "true" : "false");
    struct app_process_sample sample;
    bool                      sampled = app.child > 0 && app_process_read(app.child, &sample);
    app_resource_update(&resource_window, sampled ? &sample : nullptr, h.logical_cpus);
    app_put(&b, "\"resources\":{\"scope\":\"geistd\",\"rss_bytes\":");
    if (sampled)
        app_printf(&b, "%llu", (unsigned long long) sample.rss);
    else
        app_put(&b, "null");
    app_put(&b, ",\"cpu_percent\":");
    if (resource_window.cpu_known)
        app_printf(&b, "%.2f", resource_window.cpu_percent);
    else
        app_put(&b, "null");
    app_printf(&b, ",\"cpu_interval_ms\":%.0f},", resource_window.interval_ms);
    app_put(&b, "\"runtime\":\"geistd\",\"active\":");
    app_quote(&b, app.active);
    app_put(&b, ",\"active_id\":");
    app_quote(&b, app.active_id);
    app_put(&b, ",\"message\":");
    app_quote(&b, app.message);
    app_put(&b, ",\"phase\":");
    app_quote(&b, app.phase);
    app_put(&b, ",\"job_model\":");
    app_quote(&b, app.job_running ? app.job_model->id : "");
    app_printf(&b,
               ",\"received\":%llu,\"ready\":%s,\"loading\":%s,\"busy\":%s,"
               "\"inference_busy\":%s,\"background_download\":%s,\"models\":[",
               (unsigned long long) app.received,
               app.ready ? "true" : "false",
               app.child && !app.ready ? "true" : "false",
               app.comparing || app.job_running || app.generating ? "true" : "false",
               app.comparing || app.generating || (app.job_running && app.job_activate) ||
                               (app.child && !app.ready)
                       ? "true"
                       : "false",
               app.job_running && !app.job_activate ? "true" : "false");
    for (size_t i = 0; i < app_model_count; ++i) {
        const struct app_model *m         = &app_models[i];
        bool                    installed = inventory[i].installed;
        uint64_t                partial   = inventory[i].partial;
        struct app_hardware     adjusted  = h;
        if (partial <= m->bytes && h.disk_known && UINT64_MAX - adjusted.disk > partial)
            adjusted.disk += partial;
        /* The running model already owns its resident memory. Do not charge it
         * twice when displaying its device suitability. Other apps still count. */
        if (sampled && app.ready && execution_model == m && adjusted.available_known &&
            adjusted.available < adjusted.ram)
            adjusted.available += sample.rss < adjusted.ram - adjusted.available
                                          ? sample.rss
                                          : adjusted.ram - adjusted.available;
        struct app_assessment a = app_assess_device(&adjusted,
                                                    m,
                                                    installed,
                                                    app.history[i][0].rate,
                                                    gpu_supported(m),
                                                    app.history[i][1].rate);
        if (i)
            app_put(&b, ",");
        app_put(&b, "{\"id\":");
        app_quote(&b, m->id);
        app_put(&b, ",\"name\":");
        app_quote(&b, m->name);
        app_put(&b, ",\"group_id\":");
        app_quote(&b, m->group_id);
        app_put(&b, ",\"group_name\":");
        app_quote(&b, m->group_name);
        app_put(&b, ",\"quantization\":");
        app_quote(&b, m->quantization ? m->quantization : "");
        app_put(&b, ",\"sha256\":");
        app_quote(&b, m->sha256);
        app_printf(&b,
                   ",\"bytes\":%llu,\"ram_gib\":%u,\"working_mib\":%u,\"installed\":%s,\"partial\":"
                   "%llu,\"resource_fit\":%d,\"fit\":%d,\"quality\":\"unverified\",\"reason\":",
                   (unsigned long long) m->bytes,
                   m->recommended_ram_gib,
                   m->working_mib,
                   installed ? "true" : "false",
                   (unsigned long long) partial,
                   a.fit,
                   app_task_fit(a.fit, APP_QUALITY_UNVERIFIED));
        app_quote(&b, a.reason);
        /* Only modalities implemented by the bundled service are advertised. */
        app_put(&b,
                ",\"capabilities\":{\"chat\":true,\"vision\":false,"
                "\"speech_recognition\":false}");
        app_printf(&b, ",\"preview_accepted\":%s", app.preview_accepted[i] ? "true" : "false");
        app_put(&b, ",\"performance\":");
        app_quote(&b, a.performance);
        app_printf(&b,
                   ",\"measured_tps\":%.3f,\"measured_tokens\":%u}",
                   app.measurements[i].tps,
                   app.measurements[i].tokens);
    }
    char artifact[65] = "";
    if (execution_model)
        snprintf(artifact, sizeof artifact, "%s", execution_model->sha256);
    app_put(&b, "],\"performance_profile\":");
    pthread_mutex_unlock(&app.mutex);
    perf_view(&b, artifact, app.profile_series, app.cpu_backend, app.gpu_backend);
    app_put(&b, "}");
    if (b.failed)
        error_response(fd, 503, "Status exceeds the response memory budget.");
    else
        response(fd, 200, "application/json", body, b.len);
}

struct proxy {
    int                   fd;
    bool                  started, disconnected, expired, preparing;
    double                start, first, first_answer, heartbeat;
    struct app_utf8       utf8;
    struct app_output     output;
    struct app_run_stats *stats;
    bool (*send)(void *, const char *);
    bool (*keepalive)(void *);
    void    *target;
    char     phase[24];
    uint64_t operation, generation, pieces;
};
static bool proxy_cancel(void *opaque) {
    struct proxy *p = opaque;
    char          one;
    ssize_t       n = recv(p->fd, &one, 1, MSG_PEEK | MSG_DONTWAIT);
    p->disconnected =
            n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
    p->expired = monotonic_ms() - p->start > 3600000;
    if (atomic_load(&closing) || atomic_load(&request_cancelled) || p->expired || p->disconnected)
        return true;
    if (p->stats && p->stats->stage && strcmp(p->phase, p->stats->stage)) {
        snprintf(p->phase, sizeof p->phase, "%s", p->stats->stage);
        pthread_mutex_lock(&app.mutex);
        snprintf(app.request_phase, sizeof app.request_phase, "%s", p->phase);
        (void) activity_step(&app.request_activity,
                             p->operation,
                             p->generation,
                             activity_request_stage(p->phase),
                             monotonic_ms());
        pthread_mutex_unlock(&app.mutex);
    }
    if (p->keepalive && monotonic_ms() - (p->heartbeat ? p->heartbeat : p->start) >= 10000) {
        p->heartbeat = monotonic_ms();
        if (!p->keepalive(p->target)) {
            p->disconnected = true;
            return true;
        }
    }
    return false;
}
static bool proxy_send(void *opaque, const char *text) {
    struct proxy *p = opaque;
    if (text[strspn(text, " \t\r\n")] && !p->first_answer)
        p->first_answer = monotonic_ms() - p->start;
    if (!p->started) {
        const char *head = "HTTP/1.1 200 OK\r\nContent-Type: application/x-ndjson\r\n"
                           "Cache-Control: no-store\r\nConnection: close\r\n"
                           "X-Content-Type-Options: nosniff\r\n\r\n";
        if (!send_bytes(p->fd, head, strlen(head)))
            return false;
        p->started = true;
    }
    char              data[16384];
    struct app_buffer b = {.data = data, .cap = sizeof data};
    app_put(&b, "{\"response\":");
    app_quote(&b, text);
    app_put(&b, ",\"done\":false}\n");
    return !b.failed && send_bytes(p->fd, data, b.len);
}
static bool proxy_keepalive(void *opaque) {
    struct proxy *p = opaque;
    if (!p->started && !proxy_send(p, ""))
        return false;
    const char *event = p->output.state == OUTPUT_REASONING && !p->first_answer
                                ? "{\"phase\":\"preparing\"}\n"
                                : "{\"heartbeat\":true}\n";
    return send_bytes(p->fd, event, strlen(event));
}
static bool proxy_decode(struct proxy *p, const char *piece) {
    if (proxy_cancel(p))
        return false;
    char decoded[8192];
    if (!app_utf8_feed(&p->utf8, piece, decoded, sizeof decoded))
        return false;
    if (*decoded && !p->first)
        p->first = monotonic_ms() - p->start;
    bool ok = app_output_feed(&p->output, decoded, p->send, p->target);
    if (*decoded) {
        pthread_mutex_lock(&app.mutex);
        if (app.request_activity.id == p->operation &&
            app.request_activity.generation == p->generation) {
            activity_progress(&app.request_activity, ++p->pieces, monotonic_ms());
            if (p->output.visible)
                activity_change(&app.request_activity, ACT_ANSWER);
            else if (p->output.state == OUTPUT_REASONING)
                activity_change(&app.request_activity, ACT_PREPARING);
        }
        pthread_mutex_unlock(&app.mutex);
    }
    if (ok && p->output.reasoning && !p->preparing && p->keepalive) {
        ok           = p->keepalive(p->target);
        p->preparing = true;
    }
    return ok;
}
static bool proxy_emit(void *opaque, const char *piece) {
    return proxy_decode(opaque, piece);
}
static void proxy_init(struct proxy *p) {
    p->operation                  = app.request_activity.id;
    p->generation                 = app.request_activity.generation;
    const struct app_model *model = app_model_find(app.active_id);
    app_output_init(&p->output, model ? model->reasoning_format : nullptr);
    app.last_error[0] = app.error_stage[0] = app.error_model[0] = app.error_backend[0] = 0;
    app.error_code                                                                     = 0;
    strcpy(app.request_phase, "connect");
}
static void
proxy_finish(struct proxy *p, struct app_run_stats *stats, int *rc, char error[static 256]) {
    stats->reasoning       = p->output.reasoning;
    stats->no_answer       = !p->output.visible;
    stats->first_answer_ns = p->output.visible && p->first_answer > 0 ? p->first_answer * 1e6 : -1;
    app_output_finish(&p->output);
    if (p->expired) {
        *rc = 504;
        snprintf(error,
                 256,
                 "The one-hour request limit was reached. Shorten the conversation or select GPU.");
    }
    pthread_mutex_lock(&app.mutex);
    if (*rc && *rc != 499 && !p->disconnected) {
        snprintf(app.last_error, sizeof app.last_error, "%s", error);
        snprintf(app.error_stage,
                 sizeof app.error_stage,
                 "%s",
                 stats->stage ? stats->stage : "output");
        snprintf(app.error_model, sizeof app.error_model, "%s", app.active_id);
        snprintf(app.error_backend, sizeof app.error_backend, "%s", app.backend);
        app.error_code = *rc;
    }
    pthread_mutex_unlock(&app.mutex);
}

static void generate(int fd, struct request *r, struct app_arena *arena) {
    struct json *json = app_alloc(arena, 1, sizeof *json, _Alignof(struct json));
    if (!json) {
        error_response(fd, 503, "Request memory budget exhausted.");
        return;
    }
    if (json_parse(json, strlen(r->body), r->body) < 0 ||
        !json_is_str(json, json_get(json, 0, "prompt"))) {
        error_response(fd, 400, "A text prompt is required.");
        return;
    }
    char                  *task_id = json_strdup(json, json_get(json, 0, "task"));
    char                  *version = json_strdup(json, json_get(json, 0, "task_version"));
    const struct app_task *task    = app_task_find(task_id ? task_id : "freeform");
    bool                   valid =
            task && !task->url[0] && (!task_id || (version && !strcmp(version, task->version)));
    free(task_id);
    free(version);
    if (!valid) {
        error_response(fd, 400, "Choose an available task and its current version.");
        return;
    }
    char *prompt = json_strdup(json, json_get(json, 0, "prompt"));
    if (!prompt) {
        error_response(fd, 503, "Cannot allocate prompt.");
        return;
    }
    size_t length = strlen(prompt);
    if (!length || length > task->input_limit) {
        free(prompt);
        error_response(fd, 400, "The input is empty or exceeds this task's byte limit.");
        return;
    }
    int         language_token     = json_get(json, 0, "language");
    char       *requested_language = json_strdup(json, language_token);
    const char *language = requested_language && !strcmp(requested_language, "de") ? "de" : "en";
    bool        valid_language =
            language_token < 0 || (requested_language && (!strcmp(requested_language, "de") ||
                                                          !strcmp(requested_language, "en")));
    free(requested_language);
    if (!valid_language) {
        free(prompt);
        error_response(fd, 400, "Choose English or German for this task.");
        return;
    }
    /* Session chat is explicit: task/benchmark requests keep their pinned
     * instructions and limits. Reuse the bounded client-message parser. */
    struct app_chat chat;
    bool            conversation = json_get(json, 0, "messages") >= 0;
    if (conversation) {
        const char *why;
        int         code = app_chat_parse(arena, r->body, &chat, &why);
        if (!code && (strcmp(task->id, "freeform") ||
                      json_bool(json, json_get(json, 0, "benchmark"), false) ||
                      chat.count >= APP_CHAT_MESSAGES || !(chat.count % 2))) {
            code = 400;
            why  = "Use alternating user and assistant messages ending with the current input.";
        }
        for (size_t i = 0; !code && i < chat.count; ++i) {
            if (strcmp(chat.messages[i].role, i % 2 ? "assistant" : "user") ||
                !chat.messages[i].content[0]) {
                code = 400;
                why  = "Use alternating nonempty user and assistant messages.";
            }
        }
        if (!code && strcmp(chat.messages[chat.count - 1].content, prompt)) {
            code = 400;
            why  = "The final message must match the current input.";
        }
        if (code) {
            free(prompt);
            error_response(fd, code, why);
            return;
        }
        if (json_get(json, 0, "max_tokens") < 0 && json_get(json, 0, "max_completion_tokens") < 0)
            chat.max_tokens =
                    0; /* App conversation uses all remaining context in one generation. */
        memmove(chat.messages + 1, chat.messages, chat.count * sizeof *chat.messages);
        chat.messages[0] = (struct chat_msg) {
                .role    = "system",
                .content = !strcmp(language, "de") ? "Answer in German." : "Answer in English."};
        chat.count++;
    }
    size_t composed_cap = length + strlen(task->instruction) + 128;
    char  *composed     = app_alloc(arena, composed_cap, 1, 1);
    if (!composed) {
        free(prompt);
        error_response(fd, 503, "Request memory budget exhausted.");
        return;
    }
    snprintf(composed,
             composed_cap,
             "%s\n%s\n\nInput:\n%s",
             !strcmp(language, "de") ? "Answer in German." : "Answer in English.",
             task->instruction,
             prompt);
    pthread_mutex_lock(&app.mutex);
    if (atomic_load(&closing) || !app.ready || app.comparing || app.generating ||
        (app.job_running && app.job_activate)) {
        pthread_mutex_unlock(&app.mutex);
        free(prompt);
        error_response(fd, 409, "Wait until the model is ready and idle.");
        return;
    }
    if (conversation && strcmp(chat.model, app.active_id)) {
        pthread_mutex_unlock(&app.mutex);
        free(prompt);
        error_response(fd, 409, "The loaded model changed. Check the model and send again.");
        return;
    }
    struct app_hardware hardware;
    bool                hardware_known = app_hardware_read(&hardware, app.models);
    enum app_quality    quality = app_task_quality(app_model_find(app.active_id),
                                                   task,
                                                   language,
                                                   hardware_known ? hardware.device : APP_UNKNOWN);
    /* Single-task quality evidence does not certify multi-turn conversation. */
    if ((conversation || strcmp(app.backend, app.cpu_backend) || quality != APP_QUALITY_PASSED) &&
        !json_bool(json, json_get(json, 0, "experimental"), false)) {
        pthread_mutex_unlock(&app.mutex);
        free(prompt);
        error_response(
                fd,
                409,
                "This task/model/language is experimental. Enable experimental use explicitly.");
        return;
    }
    bool benchmark   = json_bool(json, json_get(json, 0, "benchmark"), false);
    int  model_index = -1;
    for (size_t i = 0; i < app_model_count; ++i)
        if (strcmp(app.active_id, app_models[i].id) == 0)
            model_index = i;
    app.generating = true;
    struct perf_record observation;
    observation_begin(&observation,
                      "app",
                      conversation ? chat.max_tokens
                      : benchmark  ? 64
                                   : task->output_limit,
                      conversation ? chat.temperature : .2f,
                      conversation ? chat.top_p : 1);
    struct app_run_stats stats = {0};
    struct proxy         proxy = {.fd        = fd,
                                  .start     = monotonic_ms(),
                                  .stats     = &stats,
                                  .send      = proxy_send,
                                  .keepalive = proxy_keepalive};
    proxy.target               = &proxy;
    proxy_init(&proxy);
    pthread_mutex_unlock(&app.mutex);
    char error[256];
    int  rc = conversation ? app_daemon_chat(app.socket_path,
                                             chat.count,
                                             chat.messages,
                                             chat.max_tokens,
                                             chat.temperature,
                                             chat.top_p,
                                             proxy_emit,
                                             proxy_cancel,
                                             &proxy,
                                             &stats,
                                             error)
                           : app_daemon_run(app.socket_path,
                                            composed,
                                            benchmark ? 64 : task->output_limit,
                                            proxy_emit,
                                            proxy_cancel,
                                            &proxy,
                                            &stats,
                                            error);
    free(prompt);
    if (proxy.utf8.used || proxy.utf8.failed) {
        rc = 502;
        snprintf(error, sizeof error, "The model stream ended with invalid text encoding.");
    }
    proxy_finish(&proxy, &stats, &rc, error);
    if (rc == 0 && !proxy.started && !proxy_send(&proxy, ""))
        rc = 502;
    if (!proxy.started)
        error_response(fd, rc ? rc : 502, error);
    else if (rc) {
        char              data[1024];
        struct app_buffer b = {.data = data, .cap = sizeof data};
        app_put(&b, "{\"error\":");
        app_quote(&b, error);
        app_put(&b, "}\n");
        if (!b.failed)
            (void) send_bytes(fd, data, b.len);
    } else {
        char final[512];
        int  n = snprintf(final,
                          sizeof final,
                          "{\"done\":true,\"eval_count\":%zu,\"eval_duration\":%.0f,"
                          "\"total_duration\":%.0f,\"prompt_eval_count\":%zu,\"reused\":%zu,"
                          "\"limited\":%s,\"reasoning\":%s,\"no_answer\":%s,\"max_tokens\":%u,"
                          "\"first_model_text_ns\":%.0f,\"first_answer_ns\":%.0f}\n",
                          stats.tokens,
                          stats.generation_ns,
                          stats.total_ns,
                          stats.prompt_tokens,
                          stats.reused,
                          stats.limited ? "true" : "false",
                          stats.reasoning ? "true" : "false",
                          stats.no_answer ? "true" : "false",
                          stats.max_tokens,
                          proxy.first > 0 ? proxy.first * 1e6 : -1,
                          stats.first_answer_ns);
        bool delivered = send_bytes(fd, final, (size_t) n);
        if (!delivered)
            rc = 498;
        if (delivered && model_index >= 0) {
            pthread_mutex_lock(&app.mutex);
            if (!stats.no_answer && stats.tokens >= 16 && stats.generation_ns > 1e6) {
                app.measurements[model_index].tps    = stats.tokens / (stats.generation_ns / 1e9);
                app.measurements[model_index].tokens = (unsigned) stats.tokens;
            }

            pthread_mutex_unlock(&app.mutex);
        }
    }
    observation_end(&observation,
                    proxy.disconnected ? 498 : rc,
                    proxy.first,
                    monotonic_ms() - proxy.start,
                    &stats);
}

/* External clients and the browser share the same owned daemon and busy flag. */
struct completion_proxy {
    struct proxy      transport;
    bool              stream;
    char              model[160], id[64];
    struct app_buffer text;
};

static void api_error(int fd, int code, const char *message) {
    char              body[1024];
    struct app_buffer b = {.data = body, .cap = sizeof body};
    app_put(&b, "{\"error\":{\"message\":");
    app_quote(&b, message);
    app_printf(&b, ",\"type\":\"invalid_request_error\",\"code\":%d}}", code);
    response(fd, code, "application/json", body, b.len);
}

static void completion_prefix(struct app_buffer *b, const struct completion_proxy *p, bool chunk) {
    app_put(b, "{\"id\":");
    app_quote(b, p->id);
    app_printf(b,
               ",\"object\":\"chat.completion%s\",\"created\":%lld,\"model\":",
               chunk ? ".chunk" : "",
               (long long) time(nullptr));
    app_quote(b, p->model);
}

static bool completion_send(void *opaque, const char *decoded) {
    struct completion_proxy *p = opaque;
    if (decoded[strspn(decoded, " \t\r\n")] && !p->transport.first_answer)
        p->transport.first_answer = monotonic_ms() - p->transport.start;
    if (!p->stream) {
        app_put(&p->text, decoded);
        return !p->text.failed;
    }
    if (!p->transport.started) {
        const char *header =
                "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: "
                "no-store\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\n\r\n";
        if (!send_bytes(p->transport.fd, header, strlen(header)))
            return false;
        p->transport.started = true;
    }
    char              data[16384];
    struct app_buffer b = {.data = data, .cap = sizeof data};
    app_put(&b, "data: ");
    completion_prefix(&b, p, true);
    app_put(&b, ",\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":");
    app_quote(&b, decoded);
    app_put(&b, "},\"finish_reason\":null}]}\n\n");
    return !b.failed && send_bytes(p->transport.fd, data, b.len);
}

static bool completion_keepalive(void *opaque) {
    struct completion_proxy *p = opaque;
    if (!p->stream)
        return true;
    if (!p->transport.started && !completion_send(p, ""))
        return false;
    return send_bytes(p->transport.fd, ": preparing\n\n", 13);
}
static bool completion_emit(void *opaque, const char *piece) {
    struct completion_proxy *p = opaque;
    return proxy_decode(&p->transport, piece);
}

static void completions(int fd, const struct request *r, struct app_arena *arena) {
    struct app_chat chat;
    const char     *why;
    int             code = app_chat_parse(arena, r->body, &chat, &why);
    if (code) {
        api_error(fd, code, why);
        return;
    }
    char *text = app_alloc(arena, 32768, 1, 1), *body = app_alloc(arena, 65536, 1, 1);
    if (!text || !body) {
        api_error(fd, 503, "Request memory budget exhausted.");
        return;
    }
    struct completion_proxy p = {.transport = {.fd = fd, .start = monotonic_ms()},
                                 .stream    = chat.stream,
                                 .text      = {.data = text, .cap = 32768}};
    static atomic_ulong     sequence;
    snprintf(p.id,
             sizeof p.id,
             "chatcmpl-geist-%ld-%lu",
             (long) getpid(),
             atomic_fetch_add(&sequence, 1));
    pthread_mutex_lock(&app.mutex);
    if (atomic_load(&closing) || !app.ready || app.comparing ||
        (app.job_running && app.job_activate)) {
        pthread_mutex_unlock(&app.mutex);
        api_error(fd, 503, "Select and load a model in Geist first.");
        return;
    }
    if (strcmp(chat.model, app.active_id)) {
        pthread_mutex_unlock(&app.mutex);
        api_error(fd,
                  404,
                  "Requested model is not loaded. Refresh /v1/models after changing models.");
        return;
    }
    if (app.generating) {
        pthread_mutex_unlock(&app.mutex);
        api_error(fd, 429, "The shared model is busy. Retry after the current request completes.");
        return;
    }
    snprintf(p.model, sizeof p.model, "%s", app.active_id);
    app.generating = true;
    struct perf_record observation;
    observation_begin(&observation, "api", chat.max_tokens, chat.temperature, chat.top_p);
    struct app_run_stats stats = {0};
    p.transport.stats          = &stats;
    p.transport.send           = completion_send;
    p.transport.keepalive      = completion_keepalive;
    p.transport.target         = &p;
    proxy_init(&p.transport);
    pthread_mutex_unlock(&app.mutex);
    char error[256];
    int  rc = app_daemon_chat(app.socket_path,
                              chat.count,
                              chat.messages,
                              chat.max_tokens,
                              chat.temperature,
                              chat.top_p,
                              completion_emit,
                              proxy_cancel,
                              &p,
                              &stats,
                              error);
    /* completion_proxy begins with proxy, so the cancellation callback borrows it. */
    if (p.transport.utf8.used || p.transport.utf8.failed || p.text.failed) {
        rc = 502;
        snprintf(error, sizeof error, "The model produced invalid or oversized text.");
    }
    if (!rc && !p.transport.output.visible) {
        rc = 422;
        snprintf(error,
                 sizeof error,
                 "The model produced no answer. Try again or allow more output tokens.");
    }
    proxy_finish(&p.transport, &stats, &rc, error);
    if (!rc && chat.stream && !p.transport.started && !completion_send(&p, ""))
        rc = 502;
    struct app_buffer b = {.data = body, .cap = 65536};
    if (rc) {
        if (!p.transport.started)
            api_error(fd, rc, error);
        else {
            app_put(&b, "data: {\"error\":{\"message\":");
            app_quote(&b, error);
            app_put(&b, "}}\n\n");
            (void) send_bytes(fd, body, b.len);
        }
    } else {
        if (chat.stream)
            app_put(&b, "data: ");
        completion_prefix(&b, &p, chat.stream);
        app_put(&b, ",\"choices\":[{\"index\":0,");
        if (chat.stream)
            app_put(&b, "\"delta\":{},");
        else {
            app_put(&b, "\"message\":{\"role\":\"assistant\",\"content\":");
            app_quote(&b, text);
            app_put(&b, "},");
        }
        app_put(&b, "\"finish_reason\":");
        app_quote(&b, stats.limited ? "length" : "stop");
        app_put(&b, "}]");
        if (!chat.stream)
            app_printf(&b,
                       ",\"usage\":{\"prompt_tokens\":%zu,\"completion_tokens\":%zu,\"total_"
                       "tokens\":%zu}",
                       stats.prompt_tokens,
                       stats.tokens,
                       stats.prompt_tokens + stats.tokens);
        app_put(&b, "}");
        if (chat.stream) {
            app_put(&b, "\n\n");
            if (chat.include_usage) {
                app_put(&b, "data: ");
                completion_prefix(&b, &p, true);
                app_printf(&b,
                           ",\"choices\":[],\"usage\":{\"prompt_tokens\":%zu,\"completion_tokens\":"
                           "%zu,\"total_tokens\":%zu}}\n\n",
                           stats.prompt_tokens,
                           stats.tokens,
                           stats.prompt_tokens + stats.tokens);
            }
            app_put(&b, "data: [DONE]\n\n");
            if (b.failed)
                rc = 502;
            else if (!send_bytes(fd, body, b.len))
                rc = 498;
        } else if (b.failed) {
            rc = 502;
            api_error(fd, 502, "Completion exceeds response capacity.");
        } else if (!response(fd, 200, "application/json", body, b.len))
            rc = 498;
    }
    observation_end(&observation,
                    p.transport.disconnected ? 498 : rc,
                    p.transport.first,
                    monotonic_ms() - p.transport.start,
                    &stats);
}

/* An explicit, idle-only local comparison. No HTTP worker waits for inference.
 * A fresh daemon per processor fixes cache warmup policy; measured repetitions
 * use fresh sessions on that daemon. Actual prefix reuse is recorded. */
struct compare_output {
    double                start, first;
    struct app_utf8       utf8;
    struct app_run_stats *stats;
    uint64_t              operation, generation;
};
static bool comparison_cancel(void *context) {
    struct compare_output *o = context;
    if (o && o->stats && o->stats->stage) {
        pthread_mutex_lock(&app.mutex);
        (void) activity_step(&app.request_activity,
                             o->operation,
                             o->generation,
                             activity_request_stage(o->stats->stage),
                             monotonic_ms());
        pthread_mutex_unlock(&app.mutex);
    }
    return atomic_load(&compare_cancelled) || atomic_load(&closing);
}
static bool comparison_emit(void *context, const char *piece) {
    struct compare_output *o = context;
    char                   decoded[8192];
    if (!app_utf8_feed(&o->utf8, piece, decoded, sizeof decoded))
        return false;
    if (decoded[0] && !o->first)
        o->first = monotonic_ms() - o->start;
    return !comparison_cancel(nullptr);
}
static bool comparison_ready(bool restoring) {
    for (unsigned i = 0; i < 600; i++) {
        if (atomic_load(&closing) || (!restoring && comparison_cancel(nullptr)))
            return false;
        pthread_mutex_lock(&app.mutex);
        bool ready = app.ready, exists = app.child > 0;
        pthread_mutex_unlock(&app.mutex);
        if (ready)
            return true;
        if (!exists)
            return false;
        struct timespec pause = {.tv_nsec = 100000000};
        nanosleep(&pause, nullptr);
    }
    return false;
}
static void *comparison_main(void *unused) {
    (void) unused;
    char path[APP_PATH_CAP], id[64], previous[8], run[80];
    bool gpu;
    pthread_mutex_lock(&app.mutex);
    snprintf(path, sizeof path, "%s", app.chosen);
    snprintf(id, sizeof id, "%s", app.active_id);
    snprintf(previous, sizeof previous, "%s", app.execution_mode);
    gpu = gpu_supported(app_model_find(id));
    snprintf(run, sizeof run, "compare-v1-%ld-%.0f", (long) getpid(), monotonic_ms());
    pthread_mutex_unlock(&app.mutex);
    bool ok = true;
    for (unsigned slot = 0; ok && slot < (gpu ? 2u : 1u); slot++) {
        pthread_mutex_lock(&app.mutex);
        strcpy(app.compare_phase, "loading");
        ok = !comparison_cancel(nullptr) && start_child_mode(path, id, slot ? "gpu" : "cpu");
        /* Comparison must not rewrite the user's saved processor preference. */
        app.save_execution = false;
        pthread_mutex_unlock(&app.mutex);
        ok = ok && comparison_ready(false);
        pthread_mutex_lock(&app.mutex);
        ok = ok && !strcmp(app.backend, slot ? app.gpu_backend : app.cpu_backend);
        pthread_mutex_unlock(&app.mutex);
        for (unsigned repeat = 0; ok && repeat < 4; repeat++) {
            if (comparison_cancel(nullptr)) {
                ok = false;
                break;
            }
            struct perf_record r;
            pthread_mutex_lock(&app.mutex);
            app.generating   = true;
            app.compare_step = slot * 4 + repeat + 1;
            strcpy(app.compare_phase, repeat ? "measuring" : "warmup");
            observation_begin(&r, "controlled_test", 128, 0, 1);
            r.warmup = !repeat;
            snprintf(r.run, sizeof r.run, "%s", run);
            pthread_mutex_unlock(&app.mutex);
            const struct chat_msg prompt = {
                    .role    = "user",
                    .content = "Explain how a seed grows into a plant. Describe the stages in six "
                               "numbered sentences."};
            struct app_run_stats  stats;
            char                  error[256];
            struct compare_output output = {.start      = monotonic_ms(),
                                            .stats      = &stats,
                                            .operation  = app.request_activity.id,
                                            .generation = app.generation};
            int                   rc     = app_daemon_chat(app.socket_path,
                                                           1,
                                                           &prompt,
                                                           128,
                                                           0,
                                                           1,
                                                           comparison_emit,
                                                           comparison_cancel,
                                                           &output,
                                                           &stats,
                                                           error);
            if (output.utf8.failed || output.utf8.used)
                rc = 502;
            if (comparison_cancel(nullptr))
                rc = 499;
            observation_end(&r, rc, output.first, monotonic_ms() - output.start, &stats);
            ok = rc == 0 && stats.tokens > 0;
        }
    }
    pthread_mutex_lock(&app.mutex);
    strcpy(app.compare_phase, "restoring");
    bool restore       = !atomic_load(&closing) && start_child_mode(path, id, previous);
    app.save_execution = false;
    pthread_mutex_unlock(&app.mutex);
    restore = restore && comparison_ready(true);
    pthread_mutex_lock(&app.mutex);
    strcpy(app.compare_result,
           !restore                     ? "restore_failed"
           : comparison_cancel(nullptr) ? "cancelled"
           : ok                         ? "completed"
                                        : "failed");
    app.compare_phase[0] = 0;
    app.comparing        = false;
    pthread_mutex_unlock(&app.mutex);
    return nullptr;
}
static void comparison_start(int fd, const char *body, struct app_arena *arena) {
    struct json *j = app_alloc(arena, 1, sizeof *j, _Alignof(struct json));
    if (!j || json_parse(j, strlen(body), body) < 0 ||
        !json_bool(j, json_get(j, 0, "confirm"), false)) {
        error_response(fd, 400, "Confirm the local processor comparison.");
        return;
    }
    pthread_mutex_lock(&app.mutex);
    if (!app.ready || app.generating || app.job_running || app.comparing ||
        !app_model_find(app.active_id)) {
        pthread_mutex_unlock(&app.mutex);
        error_response(fd, 409, "Load a model and wait for other work to finish.");
        return;
    }
    if (app.compare_joinable) {
        pthread_join(app.comparison, nullptr);
        app.compare_joinable = false;
    }
    app.comparing         = true;
    app.compare_step      = 0;
    app.compare_result[0] = 0;
    strcpy(app.compare_phase, "loading");
    atomic_store(&compare_cancelled, false);
    bool ok              = pthread_create(&app.comparison, nullptr, comparison_main, nullptr) == 0;
    app.compare_joinable = ok;
    if (!ok)
        app.comparing = false;
    pthread_mutex_unlock(&app.mutex);
    if (ok)
        response(fd, 202, "application/json", "{}", 2);
    else
        error_response(fd, 503, "Cannot start comparison.");
}

static void connections(int fd, bool models) {
    char              body[4096];
    struct app_buffer b = {.data = body, .cap = sizeof body};
    pthread_mutex_lock(&app.mutex);
    if (models) {
        app_put(&b, "{\"object\":\"list\",\"data\":[");
        if (app.ready) {
            app_put(&b, "{\"id\":");
            app_quote(&b, app.active_id);
            app_put(&b,
                    ",\"object\":\"model\",\"created\":0,\"owned_by\":\"local\",\"context_window\":"
                    "4096,\"capabilities\":{\"chat\":true,\"tools\":false,\"vision\":false}}");
        }
        app_put(&b, "]}");
    } else {
        app_printf(&b, "{\"base_url\":\"http://127.0.0.1:%u/v1\",\"api_key\":", app.port);
        app_quote(&b, app.token);
        app_put(&b, ",\"model\":");
        app_quote(&b, app.active_id);
        app_printf(&b,
                   ",\"ready\":%s,\"daemon_pid\":%ld,\"context_tokens\":4096,\"max_output_tokens\":"
                   "4095,\"chat\":true,\"tools\":false,\"quality\":\"unverified\"}",
                   app.ready ? "true" : "false",
                   (long) app.child);
    }
    pthread_mutex_unlock(&app.mutex);
    if (b.failed)
        api_error(fd, 503, "Connection description exceeds capacity.");
    else
        response(fd, 200, "application/json", body, b.len);
}

static void handle(int fd, struct app_arena *arena) {
    struct request r      = {};
    int            status = read_request(fd, arena, &r);
    if (status) {
        error_response(fd, status, "Invalid, oversized or non-local request.");
        return;
    }
    if (strcmp(r.method, "GET") == 0) {
        if (strcmp(r.path, "/") == 0) {
            response(fd, 200, "text/html; charset=utf-8", page, sizeof page);
            return;
        }
        if (strcmp(r.path, "/app.css") == 0) {
            response(fd, 200, "text/css; charset=utf-8", style, sizeof style);
            return;
        }
        if (strcmp(r.path, "/i18n.js") == 0) {
            response(fd, 200, "text/javascript; charset=utf-8", translations, sizeof translations);
            return;
        }
        if (strcmp(r.path, "/marked.js") == 0) {
            response(fd, 200, "text/javascript; charset=utf-8", marked_js, sizeof marked_js);
            return;
        }
        if (strcmp(r.path, "/markdown.js") == 0) {
            response(fd, 200, "text/javascript; charset=utf-8", markdown_js, sizeof markdown_js);
            return;
        }
        if (strcmp(r.path, "/katex.js") == 0) {
            response(fd, 200, "text/javascript; charset=utf-8", katex_js, sizeof katex_js);
            return;
        }
        if (strcmp(r.path, "/app.js") == 0) {
            response(fd, 200, "text/javascript; charset=utf-8", script, sizeof script);
            return;
        }
        if (strcmp(r.path, "/health") == 0) {
            response(fd, 200, "application/json", "{\"app\":\"geist\"}", 15);
            return;
        }
    }
    if (!authorized(&r)) {
        if (!strncmp(r.path, "/v1/", 4)) {
            api_error(fd, 401, "A valid local Geist API key is required.");
            return;
        }
        error_response(fd, 403, "Open the private app link supplied by the launcher.");
        return;
    }
    if (!strcmp(r.method, "GET") && !strcmp(r.path, "/v1/models")) {
        connections(fd, true);
        return;
    }
    if (!strcmp(r.method, "GET") && !strcmp(r.path, "/app/connections")) {
        connections(fd, false);
        return;
    }
    if (!strcmp(r.method, "POST") && !strcmp(r.path, "/v1/chat/completions")) {
        completions(fd, &r, arena);
        return;
    }
    if (!strncmp(r.path, "/v1/", 4)) {
        api_error(fd, 404, "Unsupported endpoint. Use /v1/models or /v1/chat/completions.");
        return;
    }
    if (strcmp(r.method, "GET") == 0 && strcmp(r.path, "/app/status") == 0) {
        status_response(fd, arena);
        return;
    }
    if (!strcmp(r.path, "/app/performance/compare") && !strcmp(r.method, "POST")) {
        comparison_start(fd, r.body, arena);
        return;
    }
    if (!strcmp(r.path, "/app/performance/cancel") && !strcmp(r.method, "POST")) {
        atomic_store(&compare_cancelled, true);
        response(fd, 200, "application/json", "{}", 2);
        return;
    }
    if (!strcmp(r.path, "/app/performance/export") && !strcmp(r.method, "POST")) {
        if (perf_save_export()) {
            char              data[APP_PATH_CAP * 2], path[APP_PATH_CAP];
            struct app_buffer b = {.data = data, .cap = sizeof data};
            snprintf(path, sizeof path, "%s/performance/export.jsonl", app.home);
            app_put(&b, "{\"path\":");
            app_quote(&b, path);
            app_put(&b, "}");
            response(fd, 200, "application/json", data, b.len);
        } else
            error_response(fd, 503, "History could not be saved.");
        return;
    }
    if (!strcmp(r.path, "/app/performance/export") && !strcmp(r.method, "GET")) {
        size_t length = 0;
        char  *data   = perf_export(&length);
        if (data) {
            response(fd, 200, "application/x-ndjson", data, length);
            free(data);
        } else
            error_response(fd, 503, "History export exceeds the memory budget.");
        return;
    }
    if ((!strcmp(r.path, "/app/performance/settings") ||
         !strcmp(r.path, "/app/performance/clear")) &&
        !strcmp(r.method, "POST")) {
        struct json *j = app_alloc(arena, 1, sizeof *j, _Alignof(struct json));
        if (!j || json_parse(j, strlen(r.body), r.body) < 0) {
            error_response(fd, 400, "Invalid history settings.");
            return;
        }
        bool ok = false;
        if (!strcmp(r.path, "/app/performance/clear")) {
            if (!json_bool(j, json_get(j, 0, "confirm"), false)) {
                error_response(fd, 400, "Confirm deleting local measurement history.");
                return;
            }
            ok = perf_clear();
            pthread_mutex_lock(&app.mutex);
            restore_measurements();
            pthread_mutex_unlock(&app.mutex);
        } else {
            double days    = json_num(j, json_get(j, 0, "days"), 0);
            int    enabled = json_get(j, 0, "enabled");
            bool   boolean = enabled >= 0 && j->tok[enabled].type == JSMN_PRIMITIVE &&
                             ((j->tok[enabled].end - j->tok[enabled].start == 4 &&
                               !memcmp(r.body + j->tok[enabled].start, "true", 4)) ||
                              (j->tok[enabled].end - j->tok[enabled].start == 5 &&
                               !memcmp(r.body + j->tok[enabled].start, "false", 5)));
            if (!boolean || (days != 30 && days != 90 && days != 365)) {
                error_response(fd, 400, "Choose 30, 90 or 365 days.");
                return;
            }
            ok = perf_settings(json_bool(j, enabled, false), (unsigned) days);
        }
        if (ok)
            response(fd, 200, "application/json", "{}", 2);
        else
            error_response(fd, 503, "History could not be saved.");
        return;
    }
    if (!strcmp(r.path, "/app/catalog")) {
        if (!strcmp(r.method, "GET")) {
            pthread_mutex_lock(&app.mutex);
            response(fd, 200, "application/json", app_catalog_json, strlen(app_catalog_json));
            pthread_mutex_unlock(&app.mutex);
        } else if (!strcmp(r.method, "POST"))
            import_catalog(fd, r.body);
        else
            error_response(fd, 405, "Use GET or POST.");
        return;
    }
    if (!strcmp(r.path, "/app/execution")) {
        if (strcmp(r.method, "POST"))
            error_response(fd, 405, "Use POST.");
        else
            execution_response(fd, r.body);
        return;
    }
    if (!strcmp(r.path, "/app/tasks") && !strcmp(r.method, "GET")) {
        const char *catalog = app_tasks_json();
        response(fd, 200, "application/json", catalog, strlen(catalog));
        return;
    }
    if (strcmp(r.method, "POST") != 0) {
        error_response(fd, 405, "POST required.");
        return;
    }
    if (strcmp(r.path, "/app/generate") == 0) {
        generate(fd, &r, arena);
        return;
    }
    if (!strcmp(r.path, "/app/activity/cancel")) {
        struct json *j          = app_alloc(arena, 1, sizeof *j, _Alignof(struct json));
        bool         parsed     = j && json_parse(j, strlen(r.body), r.body) >= 0;
        double       id         = parsed ? json_num(j, json_get(j, 0, "id"), 0) : 0;
        double       generation = parsed ? json_num(j, json_get(j, 0, "generation"), -1) : -1;
        char        *instance   = parsed ? json_strdup(j, json_get(j, 0, "instance")) : nullptr;
        pthread_mutex_lock(&app.mutex);
        struct activity *a = app.generating ? &app.request_activity : &app.load_activity;
        bool valid = instance && !strcmp(instance, app.instance) &&
                     generation == (double) a->generation && id > 0 && id == (double) a->id &&
                     !a->outcome[0] &&
                     (app.generating || app.job_running || (app.child && !app.ready));
        if (valid) {
            activity_change(a, ACT_STOPPING);
            if (app.generating) {
                atomic_store(&request_cancelled, true);
                if (app.comparing)
                    atomic_store(&compare_cancelled, true);
            } else {
                atomic_store(&load_cancelled, true);
                if (app.job_activate)
                    atomic_store(&cancelled, true);
            }
        }
        pthread_mutex_unlock(&app.mutex);
        free(instance);
        if (valid)
            response(fd, 202, "application/json", "{}", 2);
        else
            error_response(fd, 409, "This operation is no longer active.");
        return;
    }
    if (strcmp(r.path, "/app/cancel") == 0) {
        atomic_store(&cancelled, true);
        response(fd, 200, "application/json", "{}", 2);
        return;
    }
    if (strcmp(r.path, "/app/quit-if-idle") == 0) {
        pthread_mutex_lock(&app.mutex);
        bool busy = app.comparing || app.generating || app.job_running || (app.child && !app.ready);
        if (!busy)
            atomic_store(&closing, true);
        pthread_mutex_unlock(&app.mutex);
        if (busy)
            error_response(fd, 409, "Finish the current task before updating Geist.");
        else
            response(fd, 202, "application/json", "{}", 2);
        return;
    }
    if (strcmp(r.path, "/app/quit") == 0) {
        atomic_store(&closing, true);
        response(fd, 202, "application/json", "{}", 2);
        return;
    }
    if (strcmp(r.path, "/app/stop") == 0) {
        pthread_mutex_lock(&app.mutex);
        if (app.stopping || app.comparing || app.generating || app.job_running) {
            pthread_mutex_unlock(&app.mutex);
            error_response(fd, 409, "Stop the current task first.");
            return;
        }
        stop_child();
        app.active[0]    = 0;
        app.active_id[0] = 0;
        pthread_mutex_unlock(&app.mutex);
        response(fd, 200, "application/json", "{}", 2);
        return;
    }
    bool setup       = strcmp(r.path, "/app/setup") == 0;
    bool preview     = strcmp(r.path, "/app/preview") == 0;
    bool preferences = strcmp(r.path, "/app/preferences") == 0;
    bool download    = strcmp(r.path, "/app/download") == 0;
    bool remove      = strcmp(r.path, "/app/remove") == 0;
    if (!setup && !preview && !preferences && !download && !remove &&
        strcmp(r.path, "/app/select") != 0) {
        error_response(fd, 404, "Unknown action.");
        return;
    }
    struct json *json = app_alloc(arena, 1, sizeof *json, _Alignof(struct json));
    if (!json) {
        error_response(fd, 503, "Request memory budget exhausted.");
        return;
    }
    if (json_parse(json, strlen(r.body), r.body) < 0) {
        error_response(fd, 400, "Invalid JSON.");
        return;
    }
    if (preferences) {
        char *language = json_strdup(json, json_get(json, 0, "language"));
        bool  valid    = language && (!strcmp(language, "de") || !strcmp(language, "en"));
        pthread_mutex_lock(&app.mutex);
        bool ok = valid && save_preference("answer-language", language);
        if (ok)
            snprintf(app.answer_language, sizeof app.answer_language, "%s", language);
        pthread_mutex_unlock(&app.mutex);
        free(language);
        if (!valid)
            error_response(fd, 400, "Choose English or German.");
        else if (!ok)
            error_response(fd, 500, "Cannot save language preference.");
        else
            response(fd, 200, "application/json", "{}", 2);
        return;
    }
    char *id = json_strdup(json, json_get(json, 0, "id"));
    pthread_mutex_lock(&app.mutex);
    const struct app_model *model = app_model_find(id);
    free(id);
    if (!model) {
        pthread_mutex_unlock(&app.mutex);
        error_response(fd, 400, "Choose a model from the catalog.");
        return;
    }
    /* Consent and another artifact's download do not mutate the resident
     * runtime. Selection/removal/setup retain their exclusive boundary. */
    bool background = download && app.child > 0 && strcmp(model->id, app.active_id);
    if (atomic_load(&closing) || app.stopping || app.comparing || (!preview && app.job_running) ||
        (app.generating && !preview && !background) ||
        (download && app.child > 0 && !strcmp(model->id, app.active_id))) {
        pthread_mutex_unlock(&app.mutex);
        error_response(fd, 409, "Another task is active.");
        return;
    }
    if (preview) {
        bool consent = json_bool(json, json_get(json, 0, "experimental"), false);
        char key[80];
        snprintf(key, sizeof key, "preview-%s", model->sha256);
        bool ok = consent && save_preference(key, "v1");
        if (ok)
            app.preview_accepted[model - app_models] = true;
        pthread_mutex_unlock(&app.mutex);
        if (!consent)
            error_response(fd, 400, "Preview consent must be explicit.");
        else if (!ok)
            error_response(fd, 500, "Cannot save preview consent.");
        else
            response(fd, 200, "application/json", "{}", 2);
        return;
    }
    char path[APP_PATH_CAP], part[APP_PATH_CAP];
    bool valid     = path_join(path, app.models, model->file);
    bool installed = valid && regular_size(path) == model->bytes;
    if (remove) {
        if (!valid) {
            pthread_mutex_unlock(&app.mutex);
            error_response(fd, 409, "Cannot remove this download safely.");
            return;
        }
        // Catalog filenames only; never follow a replaced directory or a symlink.
        int         directory = open(app.models, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        char        partial_name[256];
        bool        safe    = directory >= 0 &&
                              snprintf(partial_name, sizeof partial_name, "%s.part", model->file) <
                                      (int) sizeof partial_name;
        const char *names[] = {model->file, partial_name};
        for (unsigned i = 0; safe && i < 2; ++i) {
            struct stat info;
            if (fstatat(directory, names[i], &info, AT_SYMLINK_NOFOLLOW) == 0)
                safe = S_ISREG(info.st_mode);
            else
                safe = errno == ENOENT;
        }
        /* Deletion is explicit. Validate paths before stopping the owned model,
         * and hold the same lock through stop/removal so no client can start a
         * new generation between those operations. Busy requests are rejected
         * above. The catalog entry itself is immutable and remains available. */
        if (safe && app.child > 0 &&
            (!strcmp(app.active_id, model->id) || !strcmp(app.chosen, path)))
            stop_child();
        for (unsigned i = 0; safe && i < 2; ++i)
            if (unlinkat(directory, names[i], 0) != 0 && errno != ENOENT)
                safe = false;
        if (directory >= 0)
            close(directory);
        if (safe && !strcmp(app.selected, model->id))
            safe = save_selection("");
        if (safe) {
            if (!strcmp(app.active_id, model->id)) {
                app.active_id[0] = app.active[0] = 0;
            }
            app.measurements[model - app_models].tps    = 0;
            app.measurements[model - app_models].tokens = 0;
        }
        pthread_mutex_unlock(&app.mutex);
        if (safe)
            response(fd, 200, "application/json", "{}", 2);
        else
            error_response(fd, 409, "Cannot remove this download safely.");
        return;
    }
    struct app_hardware h;
    bool                known = app_hardware_read(&h, app.models);
    if (valid && snprintf(part, sizeof part, "%s.part", path) < (int) sizeof part) {
        uint64_t partial = regular_size(part);
        if (partial <= model->bytes && UINT64_MAX - h.disk > partial)
            h.disk += partial;
    }
    if (setup) {
        struct app_inventory inventory[APP_MODEL_COUNT];
        model_inventory(inventory);
        /* Re-evaluate resources on the server. Never silently accept a different
         * model from the one whose download/preview the user just approved. */
        struct app_hardware       current;
        bool                      current_known = app_hardware_read(&current, app.models);
        struct app_recommendation choice        = app_recommend(
                &current, inventory, app.selected, app.ready ? app.active_id : nullptr);
        if (!current_known || !choice.eligible || choice.model != model) {
            pthread_mutex_unlock(&app.mutex);
            error_response(
                    fd, 409, "The platform check changed. Review the setup suggestion and retry.");
            return;
        }
        download = !installed;
    }
    struct app_assessment assessment = app_assess(&h, model, installed && !download);
    if (!known || assessment.fit == APP_UNAVAILABLE || (!download && !installed)) {
        pthread_mutex_unlock(&app.mutex);
        error_response(fd,
                       409,
                       !known ? "Cannot read this computer's resources."
                              : (assessment.fit == APP_UNAVAILABLE ? assessment.reason
                                                                   : "Download this model first."));
        return;
    }
    bool activate = !download || app.child <= 0;
    bool ok = (!activate || save_selection(model->id)) && begin_job(model, download, activate);
    pthread_mutex_unlock(&app.mutex);
    if (ok)
        response(fd, 202, "application/json", "{}", 2);
    else
        error_response(fd, 503, "Cannot start model worker.");
}

struct worker {
    int fd;
};
static void *worker_main(void *opaque) {
    struct worker *w  = opaque;
    int            fd = w->fd;
    free(w);
    unsigned char *storage = malloc(WORKER_BYTES);
    if (storage) {
        struct app_arena arena = {.base = storage, .cap = WORKER_BYTES};
        handle(fd, &arena);
        free(storage);
    } else
        error_response(fd, 503, "Cannot allocate request memory.");
    shutdown(fd, SHUT_RDWR);
    close(fd);
    pthread_mutex_lock(&app.mutex);
    --app.workers;
    pthread_cond_broadcast(&app.drained);
    pthread_mutex_unlock(&app.mutex);
    return nullptr;
}

static void on_signal(int sig) {
    (void) sig;
    interrupted = 1;
}
static void usage(void) {
    fprintf(stderr,
            "geist-app [--port N] [--home PATH] [--daemon PATH] [--model FILE] [--check]\n"
            "Local model setup and demo. The private URL is printed at startup.\n"
            "--model explicitly loads your own local GGUF without catalog verification.\n");
}

int main(int argc, char **argv) {
    struct timespec instance_time;
    clock_gettime(CLOCK_REALTIME, &instance_time);
    snprintf(app.instance,
             sizeof app.instance,
             "%ld-%lld-%ld",
             (long) getpid(),
             (long long) instance_time.tv_sec,
             instance_time.tv_nsec);
    /* The native shell can terminate this private group if graceful shutdown fails. */
    if (setpgid(0, 0) != 0 && getpgrp() != getpid()) {
        perror("geist-app: process group");
        return 1;
    }
    bool        check = false;
    const char *model = nullptr;
    app.port          = 8766;
    const char *home  = getenv("GEIST_HOME");
    if (home)
        snprintf(app.home, sizeof app.home, "%s", home);
    else {
        const char *user = getenv("HOME");
        if (!user) {
            usage();
            return 2;
        }
#ifdef __APPLE__
        snprintf(app.home, sizeof app.home, "%s/Library/Application Support/Geist", user);
#else
        const char *data = getenv("XDG_DATA_HOME");
        if (data)
            snprintf(app.home, sizeof app.home, "%s/geist", data);
        else
            snprintf(app.home, sizeof app.home, "%s/.local/share/geist", user);
#endif
    }
    char executable[APP_PATH_CAP];
#ifdef __APPLE__
    uint32_t size = sizeof executable;
    if (_NSGetExecutablePath(executable, &size) != 0)
        return 2;
#else
    ssize_t got = readlink("/proc/self/exe", executable, sizeof executable - 1);
    if (got < 0)
        return 2;
    executable[got] = 0;
#endif
    char *slash = strrchr(executable, '/');
    if (!slash)
        return 2;
    *slash = 0;
    if (!path_join(app.server, executable, "geistd"))
        return 2;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--check") == 0)
            check = true;
        else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc)
            model = argv[++i];
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            char         *end;
            unsigned long value = strtoul(argv[++i], &end, 10);
            if (*end || value > 65535) {
                usage();
                return 2;
            }
            app.port = (unsigned) value;
        } else if ((strcmp(argv[i], "--home") == 0 || strcmp(argv[i], "--daemon") == 0) &&
                   i + 1 < argc) {
            char *out = strcmp(argv[i], "--home") == 0 ? app.home : app.server;
            if (strlen(argv[i + 1]) >= APP_PATH_CAP)
                return 2;
            strcpy(out, argv[++i]);
        } else {
            usage();
            return 2;
        }
    }
    umask(077);
    if (!mkdirs(app.home) || !path_join(app.models, app.home, "models") || !mkdirs(app.models)) {
        perror("geist-app: data folder");
        return 1;
    }
    if (check) {
        struct app_hardware h;
        if (!app_hardware_read(&h, app.models))
            return 1;
        printf("%s | %s | %llu MiB RAM | %u cores\n",
               h.name,
               h.arch,
               (unsigned long long) (h.ram / 1048576),
               h.cores);
        for (size_t i = 0; i < app_model_count; ++i) {
            struct app_assessment a = app_assess(&h, &app_models[i], false);
            printf("%s: %s — %s\n",
                   app_models[i].name,
                   a.fit == APP_RECOMMENDED   ? "resource fit"
                   : a.fit == APP_CONDITIONAL ? "conditional"
                                              : "unavailable",
                   a.reason);
        }
        return 0;
    }
    char lockpath[APP_PATH_CAP];
    if (!path_join(lockpath, app.home, "app.lock"))
        return 1;
    int lock = open(lockpath, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "Geist is already running for this data folder.\n");
        return 1;
    }
    if (access(app.server, X_OK) != 0) {
        fprintf(stderr, "Missing executable geistd: %s\n", app.server);
        close(lock);
        return 1;
    }
    if (!app_key(app.home, app.token)) {
        fprintf(stderr,
                "Cannot create or read the private API key. Check data-folder ownership and "
                "permissions.\n");
        close(lock);
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    struct sigaction action = {.sa_handler = on_signal};
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGHUP, &action, nullptr);
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        close(lock);
        return 1;
    }
    int fd = listener(&app.port);
    if (fd < 0) {
        fprintf(stderr, "Port is unavailable. Use --port 0 to choose a free port.\n");
        curl_global_cleanup();
        close(lock);
        return 1;
    }
    if (!app_connection_write(app.home, app.port, app.token)) {
        fprintf(stderr, "Cannot save private connection details.\n");
        close(fd);
        close(lock);
        curl_global_cleanup();
        return 1;
    }
    printf("GEIST_APP_URL=http://127.0.0.1:%u/#%s\n", app.port, app.token);
    fflush(stdout);
    fprintf(stderr,
            "Runs here. Stays here.\nData: %s\nFor a headless Pi, forward port %u over SSH and "
            "open the private link on your computer.\n",
            app.home,
            app.port);
    probe_backends();
    struct app_hardware measurement_hardware = {0};
    if (app_hardware_read(&measurement_hardware, app.models)) {
        int n = snprintf(app.measurement_identity,
                         sizeof app.measurement_identity,
                         "v1 %s\n%s|%s|%s|%llu|%u\n",
                         APP_VERSION,
                         measurement_hardware.name,
                         measurement_hardware.arch,
                         measurement_hardware.os,
                         (unsigned long long) measurement_hardware.ram,
                         measurement_hardware.logical_cpus);
        if (n < 0 || (size_t) n >= sizeof app.measurement_identity)
            app.measurement_identity[0] = 0;
    }
    char     engine_hash[65]   = "unknown";
    bool     engine_identified = app_engine_sha256(app.server, engine_hash);
    unsigned profile_threads   = measurement_hardware.cores;
    unsigned profile_limit     = measurement_hardware.device == APP_PI5 ? 4 : 2;
    if (profile_threads > profile_limit)
        profile_threads = profile_limit;
    snprintf(app.profile_series,
             sizeof app.profile_series,
             "v1;engine=%s;host=%s|%s|%s|%llu|%u;ctx=4096;sessions=1;threads=%u;wait=passive;kv="
             "engine-default;offload=engine-default",
             engine_identified ? engine_hash : "unknown",
             measurement_hardware.name,
             measurement_hardware.arch,
             measurement_hardware.os,
             (unsigned long long) measurement_hardware.ram,
             measurement_hardware.logical_cpus,
             profile_threads);
    perf_init(app.home);
    char *catalog_text = malloc(APP_CATALOG_BYTES + 1);
    if (catalog_text && read_preference("catalog.json", catalog_text, APP_CATALOG_BYTES + 1)) {
        char                why[256];
        struct app_catalog *saved = app_catalog_parse(catalog_text, why);
        if (saved && app_catalog_version(saved) >= app_catalog_revision)
            app_catalog_apply(saved);
        else {
            app_catalog_discard(saved);
            snprintf(app.message,
                     sizeof app.message,
                     "Saved catalog is invalid or older; using the bundled catalog.");
        }
    }
    free(catalog_text);
    (void) read_preference("answer-language", app.answer_language, sizeof app.answer_language);
    if (strcmp(app.answer_language, "en") && strcmp(app.answer_language, "de"))
        app.answer_language[0] = 0;
    restore_preview_preferences();
    migrate_measurements();
    restore_measurements();
    (void) read_preference("selected", app.selected, sizeof app.selected);
    if (model) {
        pthread_mutex_lock(&app.mutex);
        (void) start_child(model, "custom");
        pthread_mutex_unlock(&app.mutex);
    } else {
        const struct app_model *m = app_model_find(app.selected);
        char                    path[APP_PATH_CAP];
        /* A missing/cancelled download remains a resumable choice. Never start
         * verification of a missing file on reopen, or download without action. */
        if (m && path_join(path, app.models, m->file) && regular_size(path) == m->bytes) {
            pthread_mutex_lock(&app.mutex);
            (void) begin_job(m, false, true);
            pthread_mutex_unlock(&app.mutex);
        }
    }
    pthread_t monitor;
    bool      monitoring = pthread_create(&monitor, nullptr, monitor_main, nullptr) == 0;
    if (!monitoring) {
        fprintf(stderr, "Cannot start process monitor.\n");
        atomic_store(&closing, true);
    }
    while (!interrupted && !atomic_load(&closing)) {
        struct timeval timeout = {.tv_usec = 100000};
        fd_set         set;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        int ready = select(fd + 1, &set, nullptr, nullptr, &timeout);
        if (ready <= 0)
            continue;
        int client = accept(fd, nullptr, nullptr);
        if (client < 0)
            continue;
        (void) fcntl(client, F_SETFD, FD_CLOEXEC);
        struct timeval io_timeout = {.tv_sec = 5};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &io_timeout, sizeof io_timeout);
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &io_timeout, sizeof io_timeout);
        pthread_mutex_lock(&app.mutex);
        bool room = app.workers < WORKER_CAP;
        if (room)
            ++app.workers;
        pthread_mutex_unlock(&app.mutex);
        if (!room) {
            error_response(client, 503, "Too many connections. Try again shortly.");
            close(client);
            continue;
        }
        struct worker *w = malloc(sizeof *w);
        pthread_t      thread;
        if (w)
            w->fd = client;
        if (!w || pthread_create(&thread, nullptr, worker_main, w) != 0) {
            free(w);
            close(client);
            pthread_mutex_lock(&app.mutex);
            --app.workers;
            pthread_mutex_unlock(&app.mutex);
        } else
            pthread_detach(thread);
    }
    atomic_store(&closing, true);
    atomic_store(&cancelled, true);
    close(fd);
    if (monitoring)
        pthread_join(monitor, nullptr);
    pthread_mutex_lock(&app.mutex);
    while (app.workers)
        pthread_cond_wait(&app.drained, &app.mutex);
    pthread_mutex_unlock(&app.mutex);
    if (app.compare_joinable)
        pthread_join(app.comparison, nullptr);
    if (app.job_joinable)
        pthread_join(app.job, nullptr);
    pthread_mutex_lock(&app.mutex);
    stop_child();
    pthread_mutex_unlock(&app.mutex);
    perf_close();
    app_connection_remove(app.home);
    curl_global_cleanup();
    close(lock);
    return 0;
}
