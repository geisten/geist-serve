/* main.c — argv, paths, signals, worker pool, accept loop. */
#include "app.h"

volatile sig_atomic_t interrupted;
atomic_bool closing, cancelled, compare_cancelled, request_cancelled, load_cancelled;
struct app_state app = {.mutex = PTHREAD_MUTEX_INITIALIZER, .drained = PTHREAD_COND_INITIALIZER};

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
    if (!path_join(app.paths.server, executable, "geistd"))
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
            char *out = strcmp(argv[i], "--home") == 0 ? app.paths.home : app.paths.server;
            if (strlen(argv[i + 1]) >= APP_PATH_CAP)
                return 2;
            strcpy(out, argv[++i]);
        } else {
            usage();
            return 2;
        }
    }
    /* Only the default folder is resolved, and migrated (#92): never as a side
     * effect of a run with an explicit --home. */
    if (!app.paths.home[0] && !app_home(app.paths.home)) {
        usage();
        return 2;
    }
    umask(077);
    if (!mkdirs(app.paths.home) || !path_join(app.paths.models, app.paths.home, "models") || !mkdirs(app.paths.models)) {
        perror("geist-app: data folder");
        return 1;
    }
    if (check) {
        struct app_hardware h;
        if (!app_hardware_read(&h, app.paths.models))
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
    if (!path_join(lockpath, app.paths.home, "app.lock"))
        return 1;
    int lock = open(lockpath, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "geisten is already running for this data folder.\n");
        return 1;
    }
    if (access(app.paths.server, X_OK) != 0) {
        fprintf(stderr, "Missing executable geistd: %s\n", app.paths.server);
        close(lock);
        return 1;
    }
    if (!app_key(app.paths.home, app.token)) {
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
    if (!app_connection_write(app.paths.home, app.port, app.token)) {
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
            app.paths.home,
            app.port);
    probe_backends();
    probe_engine();
    struct app_hardware measurement_hardware = {0};
    if (app_hardware_read(&measurement_hardware, app.paths.models)) {
        int n = snprintf(app.prefs.measurement_identity,
                         sizeof app.prefs.measurement_identity,
                         "v1 %s\n%s|%s|%s|%llu|%u\n",
                         APP_VERSION,
                         measurement_hardware.name,
                         measurement_hardware.arch,
                         measurement_hardware.os,
                         (unsigned long long) measurement_hardware.ram,
                         measurement_hardware.logical_cpus);
        if (n < 0 || (size_t) n >= sizeof app.prefs.measurement_identity)
            app.prefs.measurement_identity[0] = 0;
    }
    char     engine_hash[65]   = "unknown";
    bool     engine_identified = app_engine_sha256(app.paths.server, engine_hash);
    unsigned profile_threads   = measurement_hardware.cores;
    unsigned profile_limit     = measurement_hardware.device == APP_PI5 ? 4 : 2;
    if (profile_threads > profile_limit)
        profile_threads = profile_limit;
    snprintf(app.prefs.profile_series,
             sizeof app.prefs.profile_series,
             "v1;engine=%s;host=%s|%s|%s|%llu|%u;ctx=4096;sessions=1;threads=%u;wait=passive;kv="
             "engine-default;offload=engine-default",
             engine_identified ? engine_hash : "unknown",
             measurement_hardware.name,
             measurement_hardware.arch,
             measurement_hardware.os,
             (unsigned long long) measurement_hardware.ram,
             measurement_hardware.logical_cpus,
             profile_threads);
    perf_init(app.paths.home);
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
    (void) read_preference("answer-language", app.prefs.answer_language, sizeof app.prefs.answer_language);
    if (strcmp(app.prefs.answer_language, "en") && strcmp(app.prefs.answer_language, "de"))
        app.prefs.answer_language[0] = 0;
    restore_preview_preferences();
    migrate_measurements();
    restore_measurements();
    (void) read_preference("selected", app.prefs.selected, sizeof app.prefs.selected);
    if (model) {
        pthread_mutex_lock(&app.mutex);
        (void) start_child(model, "custom");
        pthread_mutex_unlock(&app.mutex);
    } else {
        const struct app_model *m = app_model_find(app.prefs.selected);
        char                    path[APP_PATH_CAP];
        /* A missing/cancelled download remains a resumable choice. Never start
         * verification of a missing file on reopen, or download without action. */
        if (m && path_join(path, app.paths.models, m->file) && regular_size(path) == m->bytes) {
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
    if (app.compare.joinable)
        pthread_join(app.compare.thread, nullptr);
    if (app.job.joinable)
        pthread_join(app.job.thread, nullptr);
    pthread_mutex_lock(&app.mutex);
    stop_child();
    pthread_mutex_unlock(&app.mutex);
    perf_close();
    app_connection_remove(app.paths.home);
    curl_global_cleanup();
    close(lock);
    return 0;
}
