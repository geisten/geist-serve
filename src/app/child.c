/* child.c — geistd supervision: backend probe, spawn, poll, stop, lifecycle sampling. */
#include "app.h"

/* Run the packaged daemon with one informational flag and capture its stdout.
 * Bounded to ~5 s and 1 KiB; true only for a clean exit 0. */
static bool server_output(const char *flag, char output[static 1024]) {
    int pipefd[2];
    output[0] = 0;
    if (pipe(pipefd))
        return false;
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions)) {
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }
    int rc = posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    if (!rc)
        rc = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    char *args[] = {app.paths.server, (char *) flag, nullptr};
    pid_t child  = 0;
    if (!rc)
        rc = posix_spawn(&child, app.paths.server, &actions, nullptr, args, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]);
    size_t used   = 0;
    int    status = 0;
    bool   ended  = false;
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    for (unsigned attempt = 0; !rc && attempt < 100; ++attempt) {
        ssize_t n = read(pipefd[0], output + used, 1023 - used);
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
    ssize_t n = read(pipefd[0], output + used, 1023 - used);
    if (n > 0)
        used += (size_t) n;
    output[used] = 0;
    close(pipefd[0]);
    return !rc && ended && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* Probe the packaged engine once. Old CPU-only daemons remain usable. */
void probe_backends(void) {
#if defined(__aarch64__) || defined(__arm64__)
    strcpy(app.backend.cpu, "cpu_neon");
#elif defined(__x86_64__)
    strcpy(app.backend.cpu, "cpu_x86");
#else
    strcpy(app.backend.cpu, "cpu_scalar");
#endif
    char         output[1024];
    struct json *j = calloc(1, sizeof *j);
    if (server_output("--backends", output) && j && json_parse(j, strlen(output), output) >= 0) {
        int   gpu  = json_get(j, 0, "gpu");
        char *name = json_strdup(j, json_get(j, gpu, "name"));
        if (name && (!strcmp(name, "metal") || !strcmp(name, "vulkan"))) {
            snprintf(app.backend.gpu, sizeof app.backend.gpu, "%s", name);
            app.backend.gpu_available = json_bool(j, json_get(j, gpu, "available"), false);
        }
        free(name);
    }
    free(j);
}

static struct app_engine packaged_engine; /* from --build-info; empty for legacy daemons */

/* The packaged engine's provenance before any model loads (#55); the info
 * handshake replaces it with the running daemon's own report. A daemon
 * without --build-info (legacy) leaves it unreported. */
void probe_engine(void) {
    char              output[1024];
    struct app_engine identity;
    struct json      *j = calloc(1, sizeof *j);
    if (server_output("--build-info", output) && j && json_parse(j, strlen(output), output) >= 0 &&
        app_engine_parse(&identity, j, 0) && identity.version[0]) {
        snprintf(identity.payload_sha256, sizeof identity.payload_sha256, "%s", app.child.engine.payload_sha256);
        app.child.engine = packaged_engine = identity;
    }
    free(j);
}

bool gpu_supported(const struct app_model *model) {
    unsigned bit = !strcmp(app.backend.gpu, "metal") ? 2 : 4;
    return app.backend.gpu_available && model && (model->backends & bit);
}
bool recommend_gpu(const struct app_model *model) {
    /* A hardware default, not a claim of measured performance. */
    return gpu_supported(model) && model->bytes >= APP_GIB;
}

/* All activity mutations use app.mutex. A service heartbeat is never progress. */
void activity_change(struct activity *a, enum activity_stage stage) {
    (void) activity_step(a, a->id, a->generation, stage, monotonic_ms());
}
void begin_activity(struct activity    *a,
                           enum activity_stage stage,
                           uint64_t            generation,
                           const char         *model) {
    activity_begin(a, ++app.child.operation_id, generation, stage, monotonic_ms());
    snprintf(a->model, sizeof a->model, "%s", model ? model : "custom");
    snprintf(a->backend, sizeof a->backend, "%s", app.backend.active);
    a->engine = app.child.engine;
}

static void runtime_paths_close(void) {
    lifecycle_close(&app.child.lifecycle);
    if (app.child.socket_path[0])
        unlink(app.child.socket_path);
    if (app.child.runtime_dir[0])
        rmdir(app.child.runtime_dir);
    app.child.socket_path[0] = app.child.runtime_dir[0] = 0;
}

static bool archive_load_failure(void) {
    char source[APP_PATH_CAP], archive[APP_PATH_CAP];
    if (!path_join(source, app.paths.home, "server.log") ||
        !path_join(archive, app.paths.home, "load-failure-XXXXXX"))
        return false;
    int fd = mkstemp(archive);
    if (fd < 0)
        return false;
    close(fd);
    if (!rename(source, archive))
        return true;
    unlink(archive);
    return false;
}

/* Called with app.mutex held. The child is ours: never attach to or stop
 * Ollama or another user's process. Reap before inspecting health. */
static void lifecycle_sample(void) {
    struct lifecycle_snapshot sample;
    if (!lifecycle_read(app.child.lifecycle, app.generation, &sample) ||
        sample.process != (uint64_t) app.child.pid)
        return;
    app.child.lifecycle_snapshot                    = sample;
    static const enum activity_stage stages[] = {
            ACT_NONE, ACT_BACKEND, ACT_MODEL, ACT_METADATA, ACT_WARMUP, ACT_READY};
    for (unsigned i = app.child.lifecycle_phase + 1; i < LC_READY; i++) {
        if (!sample.phase_ns[i])
            break;
        double when = (double) sample.phase_ns[i] / 1e6;
        if (when < app.activity.load.event_at)
            when = app.activity.load.event_at;
        (void) activity_step(
                &app.activity.load, app.activity.load.id, app.generation, stages[i], when);
        app.child.lifecycle_phase = i;
    }
}

void poll_child(void) {
    if (!app.child.pid || app.stopping)
        return;
    if (atomic_load(&load_cancelled) && !app.child.ready) {
        activity_change(&app.activity.load, ACT_STOPPING);
        stop_child();
        (void) archive_load_failure();
        (void) activity_end(&app.activity.load, "cancelled", 499, monotonic_ms());
        return;
    }
    lifecycle_sample();
    int   status;
    pid_t result    = waitpid(app.child.pid, &status, WNOHANG);
    bool  timed_out = !app.child.ready && monotonic_ms() - app.child.loading_started > 120000;
    if (result == app.child.pid || (result < 0 && errno == ECHILD) || timed_out) {
        if (timed_out)
            stop_child();
        else {
            app.child.previous_pid = app.child.pid;
            app.child.reaped_ms    = monotonic_ms();
            runtime_paths_close();
        }
        app.child.pid = 0;
        app.child.ready = false;
        /* A replacement's verification may already have failed or be active.
         * Reaping the previous generation must not overwrite that newer
         * outcome, restart the old GPU choice or erase its diagnostic. */
        if (app.activity.load.generation > app.generation)
            return;
        (void) activity_end(&app.activity.load, "failed", timed_out ? 504 : 502, monotonic_ms());
        if (strcmp(app.backend.active, app.backend.cpu)) {
            char path[APP_PATH_CAP], id[64], log[APP_PATH_CAP], archive[APP_PATH_CAP];
            snprintf(path, sizeof path, "%s", app.child.chosen);
            snprintf(id, sizeof id, "%s", app.child.active_id);
            bool preserved = false;
            if (path_join(log, app.paths.home, "server.log") &&
                path_join(archive, app.paths.home, "gpu-failure-XXXXXX")) {
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
            snprintf(app.backend.notice,
                     sizeof app.backend.notice,
                     "GPU stopped or failed to load. Restored CPU; diagnostics are kept in the app "
                     "data folder.");
            if (start_child_mode(path, id, "cpu"))
                return;
        }
        bool archived = !app.child.generating && archive_load_failure();
        snprintf(app.message,
                 sizeof app.message,
                 archived ? "The model process stopped. Its diagnostics were preserved. Retry the "
                            "model."
                          : "The model process stopped. See server.log in the app data folder.");
        return;
    }
    if (app.child.ready)
        return;
    if (monotonic_ms() - app.child.probe_ms < 250)
        return;
    app.child.probe_ms = monotonic_ms();
    char              reported[24];
    struct app_engine identity;
    if (app_daemon_identity(app.child.socket_path, reported, &identity)) {
        /* The engine can complete several phases during the bounded info
         * handshake. Capture them before publishing readiness. */
        lifecycle_sample();
        snprintf(identity.payload_sha256,
                 sizeof identity.payload_sha256,
                 "%s",
                 app.child.engine.payload_sha256);
        app.child.engine           = identity;
        app.backend.verified = !strcmp(reported, app.backend.active);
        if (reported[0] && !app.backend.verified) {
            stop_child();
            (void) activity_end(&app.activity.load, "failed", 502, monotonic_ms());
            snprintf(app.message,
                     sizeof app.message,
                     "The engine reported a different processor. Reload the model.");
            return;
        }
        app.child.loaded_ms            = monotonic_ms() - app.child.loading_started;
        app.child.ready                = true;
        app.activity.load.engine = app.child.engine;
        activity_change(&app.activity.load, ACT_READY);
        (void) activity_end(&app.activity.load, "completed", 0, monotonic_ms());
        app.message[0]                = 0;
        const struct app_model *model = app_model_find(app.child.active_id);
        if (app.backend.save && model) {
            char key[80];
            snprintf(key, sizeof key, "backend-%s", model->sha256);
            if (!save_preference(key, app.backend.mode))
                snprintf(app.backend.notice,
                         sizeof app.backend.notice,
                         "The model is running, but its execution preference could not be saved.");
        }
        app.backend.save = false;
    }
}

void stop_child(void) {
    /* Readiness is revoked before releasing the mutex. Other workers can report
     * cached status, but cannot replace, reuse or reap this owned process. */
    app.child.ready    = false;
    app.stopping = true;
    if (!app.child.pid)
        goto cleanup;
    app.child.previous_pid = app.child.pid;
    kill(app.child.pid, SIGTERM);
    for (unsigned i = 0; i < 20; ++i) {
        int   status;
        pid_t result = waitpid(app.child.pid, &status, WNOHANG);
        if (result == app.child.pid || (result < 0 && errno == ECHILD)) {
            app.child.pid     = 0;
            app.child.reaped_ms = monotonic_ms();
            break;
        }
        struct timespec pause = {.tv_nsec = 25000000};
        pthread_mutex_unlock(&app.mutex);
        nanosleep(&pause, nullptr);
        pthread_mutex_lock(&app.mutex);
    }
    if (app.child.pid) {
        kill(app.child.pid, SIGKILL);
        while (waitpid(app.child.pid, nullptr, 0) < 0 && errno == EINTR) {
        }
    }
    app.child.pid     = 0;
    app.child.reaped_ms = monotonic_ms();
cleanup:
    runtime_paths_close();
    app.child.lifecycle_snapshot = (struct lifecycle_snapshot) {0};
    app.child.lifecycle_phase    = 0;
    app.stopping           = false;
    app.child.ready              = false;
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
    if (!app.activity.load.id || app.activity.load.outcome[0])
        begin_activity(&app.activity.load, ACT_STARTING, app.generation + 1, id);
    snprintf(app.activity.load.backend,
             sizeof app.activity.load.backend,
             "%s",
             gpu ? app.backend.gpu : app.backend.cpu);
    atomic_store(&load_cancelled, false);
    if (app.child.pid)
        activity_change(&app.activity.load, ACT_STOPPING);
    stop_child();
    activity_change(&app.activity.load, ACT_STARTING);
    app.child.engine = packaged_engine; /* until this daemon's handshake reports its own */
    (void) app_engine_sha256(app.paths.server, app.child.engine.payload_sha256);
    struct app_hardware hardware;
    bool                known = app_hardware_read(&hardware, app.paths.home);
    if (!known || !hardware.supported) {
        snprintf(app.message,
                 sizeof app.message,
                 "This CPU/platform does not support the bundled inference engine.");
        return false;
    }
    strcpy(app.child.runtime_dir, "/tmp/geist-app-XXXXXX");
    if (!mkdtemp(app.child.runtime_dir)) {
        app.child.runtime_dir[0] = 0;
        return false;
    }
    snprintf(app.child.socket_path, sizeof app.child.socket_path, "%s/inference.sock", app.child.runtime_dir);
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", app.child.socket_path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) != 0 ||
        bind(fd, (struct sockaddr *) &sa, sizeof sa) != 0 || chmod(app.child.socket_path, 0600) != 0 ||
        listen(fd, 8) != 0) {
        if (fd >= 0)
            close(fd);
        stop_child();
        return false;
    }
    char logpath[APP_PATH_CAP];
    if (!path_join(logpath, app.paths.home, "server.log")) {
        close(fd);
        return false;
    }
    int log = open(logpath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (log < 0) {
        close(fd);
        return false;
    }
    char *args[] = {
            app.paths.server, (char *) path, "--socket", app.child.socket_path, "--sessions", "1", nullptr};
    size_t count = 0;
    while (environ[count])
        ++count;
    char **env = calloc(count + 6, sizeof *env);
    if (!env) {
        close(fd);
        close(log);
        return false;
    }
    int lifecycle_fd = lifecycle_create(app.child.runtime_dir, app.generation + 1, &app.child.lifecycle);
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
    app.child.runtime_threads  = cores;
    app.child.runtime_requests = 0;
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
             gpu ? app.backend.gpu : app.backend.cpu);
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
            app.child.spawned_ms = monotonic_ms();
            rc             = posix_spawn(&app.child.pid, app.paths.server, &actions, nullptr, args, env);
        }
        posix_spawn_file_actions_destroy(&actions);
    }
    free(env);
    close(lifecycle_fd);
    close(fd);
    close(log);
    if (rc) {
        app.child.pid = 0;
        snprintf(app.message, sizeof app.message, "Cannot start geistd: %s", strerror(rc));
        return false;
    }
    snprintf(app.child.chosen, sizeof app.child.chosen, "%s", path);
    snprintf(app.child.active_id, sizeof app.child.active_id, "%s", id ? id : "custom");
    snprintf(app.backend.mode, sizeof app.backend.mode, "%s", mode);
    snprintf(app.backend.active, sizeof app.backend.active, "%s", gpu ? app.backend.gpu : app.backend.cpu);
    ++app.generation;
    app.activity.load.generation = app.generation;
    app.activity.load.engine     = app.child.engine;
    activity_change(&app.activity.load, ACT_LOADING);
    app.child.loading_started  = monotonic_ms();
    app.backend.verified = false;
    app.backend.save   = true;
    const char *base     = strrchr(path, '/');
    base                 = base ? base + 1 : path;
    snprintf(app.child.active, sizeof app.child.active, "%s", base);
    char *ext = strstr(app.child.active, ".gguf");
    if (ext)
        *ext = 0;
    snprintf(app.message, sizeof app.message, "Loading the model into memory…");
    return true;
}

bool start_child_mode(const char *path, const char *id, const char *mode) {
    bool ok = start_child_mode_impl(path, id, mode);
    if (!ok) {
        stop_child();
        (void) archive_load_failure();
        (void) activity_end(&app.activity.load, "failed", 502, monotonic_ms());
    }
    return ok;
}

bool start_child(const char *path, const char *id) {
    char                    mode[8] = "auto", key[80];
    const struct app_model *model   = app_model_find(id);
    if (model) {
        snprintf(key, sizeof key, "backend-%s", model->sha256);
        if (!read_preference(key, mode, sizeof mode) ||
            (strcmp(mode, "cpu") && strcmp(mode, "gpu")) ||
            (!strcmp(mode, "gpu") && !gpu_supported(model)))
            strcpy(mode, "auto");
    }
    app.backend.notice[0] = 0;
    return start_child_mode(path, id, mode);
}
