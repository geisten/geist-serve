/* child.c — geistd supervision: backend probe, spawn, poll, stop, lifecycle sampling. */
#include "app.h"

/* Probe the packaged engine once. Old CPU-only daemons remain usable. */
void probe_backends(void) {
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

bool gpu_supported(const struct app_model *model) {
    unsigned bit = !strcmp(app.gpu_backend, "metal") ? 2 : 4;
    return app.gpu_available && model && (model->backends & bit);
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
    activity_begin(a, ++app.operation_id, generation, stage, monotonic_ms());
    snprintf(a->model, sizeof a->model, "%s", model ? model : "custom");
    snprintf(a->backend, sizeof a->backend, "%s", app.backend);
    a->engine = app.engine;
}

static void runtime_paths_close(void) {
    lifecycle_close(&app.lifecycle);
    if (app.socket_path[0])
        unlink(app.socket_path);
    if (app.runtime_dir[0])
        rmdir(app.runtime_dir);
    app.socket_path[0] = app.runtime_dir[0] = 0;
}

static bool archive_load_failure(void) {
    char source[APP_PATH_CAP], archive[APP_PATH_CAP];
    if (!path_join(source, app.home, "server.log") ||
        !path_join(archive, app.home, "load-failure-XXXXXX"))
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

void poll_child(void) {
    if (!app.child || app.stopping)
        return;
    if (atomic_load(&load_cancelled) && !app.ready) {
        activity_change(&app.load_activity, ACT_STOPPING);
        stop_child();
        (void) archive_load_failure();
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
        else {
            app.previous_pid = app.child;
            app.reaped_ms    = monotonic_ms();
            runtime_paths_close();
        }
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
        bool archived = !app.generating && archive_load_failure();
        snprintf(app.message,
                 sizeof app.message,
                 archived ? "The model process stopped. Its diagnostics were preserved. Retry the "
                            "model."
                          : "The model process stopped. See server.log in the app data folder.");
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
        /* The engine can complete several phases during the bounded info
         * handshake. Capture them before publishing readiness. */
        lifecycle_sample();
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

void stop_child(void) {
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
    runtime_paths_close();
    app.lifecycle_snapshot = (struct lifecycle_snapshot) {0};
    app.lifecycle_phase    = 0;
    app.stopping           = false;
    app.ready              = false;
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

bool start_child_mode(const char *path, const char *id, const char *mode) {
    bool ok = start_child_mode_impl(path, id, mode);
    if (!ok) {
        stop_child();
        (void) archive_load_failure();
        (void) activity_end(&app.load_activity, "failed", 502, monotonic_ms());
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
    app.execution_notice[0] = 0;
    return start_child_mode(path, id, mode);
}
