/* Small C23 client. Models and inference remain owned by the shared service. */
#include "connection.h"
#include "version.h"
#include "../json.h"
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
extern char   **environ;
static char     home[APP_PATH_CAP], directory[APP_PATH_CAP], token[65];
static unsigned port;
static char     reply[65536];
static int      start_error = 1;

static size_t receive(char *data, size_t size, size_t count, void *opaque) {
    struct app_buffer *b = opaque;
    if (size && count > SIZE_MAX / size)
        return 0;
    size_t n = size * count;
    if (n >= b->cap - b->len)
        return 0;
    memcpy(b->data + b->len, data, n);
    b->len += n;
    b->data[b->len] = 0;
    return n;
}
static long request(const char *path, const char *body, unsigned timeout) {
    if (!app_connection_read(home, &port, token))
        return 0;
    CURL *c = curl_easy_init();
    if (!c)
        return 0;
    char url[256], auth[96];
    snprintf(url, sizeof url, "http://127.0.0.1:%u%s", port, path);
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", token);
    struct curl_slist *headers = curl_slist_append(nullptr, auth);
    headers                    = curl_slist_append(headers, "Content-Type: application/json");
    struct app_buffer b        = {.data = reply, .cap = sizeof reply};
    reply[0]                   = 0;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_PROXY, "");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 2L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, (long) timeout);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    if (body)
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    long status = 0;
    if (curl_easy_perform(c) == CURLE_OK)
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);
    return status;
}
static void pause_short(void) {
    struct timespec t = {.tv_nsec = 100000000};
    nanosleep(&t, nullptr);
}
static int command(char *const args[]) {
    pid_t pid;
    int   status;
    if (posix_spawnp(&pid, args[0], nullptr, nullptr, args, environ) != 0)
        return -1;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
static bool systemd_home(void) {
#ifdef __APPLE__
    return false;
#else
    char standard[APP_PATH_CAP];
    int  n = snprintf(standard,
                      sizeof standard,
                      "%s/.local/share/geist",
                      getenv("HOME") ? getenv("HOME") : "");
    /* Only the packaged CLI owns the packaged unit: a rootless installation
     * (#46) must never route to a different, package-owned service. */
    return n > 0 && n < (int) sizeof standard && !getenv("GEIST_HOME") && !strcmp(home, standard) &&
           !strcmp(directory, "/usr/lib/geist") &&
           access("/usr/lib/systemd/user/geist.service", R_OK) == 0;
#endif
}

static bool owner_released(void) {
    char path[APP_PATH_CAP];
    if (snprintf(path, sizeof path, "%s/app.lock", home) >= (int) sizeof path)
        return false;
    int fd = open(path, O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return errno == ENOENT;
    struct stat info;
    bool released = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == getuid() &&
                    info.st_nlink == 1 && flock(fd, LOCK_EX | LOCK_NB) == 0;
    close(fd);
    return released;
}

static bool wait_stopped(void) {
    /* Discovery removal precedes process exit. Wait for the actual user
     * unit stop job: `start` on a still-active unit otherwise does nothing. */
    if (systemd_home()) {
        char *args[] = {"systemctl", "--user", "stop", "geist.service", nullptr};
        if (command(args) != 0) {
            fputs("Cannot confirm the user service stopped. Check its journal before "
                  "restarting.\n",
                  stderr);
            return false;
        }
    }
    bool stopped = false;
    for (unsigned i = 0; i < 100; i++) {
        char descriptor[APP_PATH_CAP];
        if (snprintf(descriptor, sizeof descriptor, "%s/connection.json", home) >=
            (int) sizeof descriptor)
            return false;
        if (access(descriptor, F_OK) < 0 && errno == ENOENT && owner_released()) {
            stopped = true;
            break;
        }
        pause_short();
    }
    if (!stopped) {
        fputs("Cannot confirm the service stopped. Check the service log before restarting.\n",
              stderr);
        return false;
    }
    return true;
}

/* Stable versions only. Unknown/pre-versioned services require one explicit
 * restart; a newer service must never be downgraded by an old app copy. */
static int version_order(const char *remote) {
    unsigned a[3], b[3];
    char     tail;
    if (!remote || sscanf(remote, "%u.%u.%u%c", &a[0], &a[1], &a[2], &tail) != 3 ||
        sscanf(APP_VERSION, "%u.%u.%u%c", &b[0], &b[1], &b[2], &tail) != 3)
        return 2;
    for (unsigned i = 0; i < 3; i++)
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    return 0;
}

static bool current_service(void) {
    struct json *j = calloc(1, sizeof *j);
    if (!j)
        return false;
    char *version = nullptr;
    if (json_parse(j, strlen(reply), reply) >= 0)
        version = json_strdup(j, json_get(j, 0, "version"));
    int order = version_order(version);
    free(version);
    free(j);
    if (order == 0)
        return true;
    if (order == 1) {
        start_error = 44;
        fputs("A newer Geisten service is running. Open the newest installed app.\n", stderr);
        return false;
    }
    if (order == 2) {
        start_error = 42;
        fputs("An older unversioned service is running. Finish your work, then run: geist "
              "restart\n",
              stderr);
        return false;
    }
    long code = request("/app/quit-if-idle", "{}", 5);
    if (code != 202) {
        start_error = code == 409 ? 43 : 42;
        fputs("Finish the current task before updating Geisten; then reopen the app.\n", stderr);
        return false;
    }
    return wait_stopped();
}

static bool start(void) {
    if (request("/app/status", nullptr, 2) == 200) {
        if (!current_service())
            return false;
        if (request("/app/status", nullptr, 2) == 200)
            return true;
    }
    char folder[APP_PATH_CAP];
    snprintf(folder, sizeof folder, "%s", home);
    for (char *p = folder + 1;; p++) {
        if (*p && *p != '/')
            continue;
        char end = *p;
        *p       = 0;
        if (mkdir(folder, 0700) != 0 && errno != EEXIST)
            return false;
        *p = end;
        if (!end)
            break;
    }
    if (systemd_home()) {
        char *args[] = {"systemctl", "--user", "start", "geist.service", nullptr};
        if (command(args) != 0) {
            fprintf(stderr,
                    "Cannot start the user service. In a headless session, run geist-app in the "
                    "foreground.\n");
            return false;
        }
    } else {
        char executable[APP_PATH_CAP];
        if (snprintf(executable, sizeof executable, "%s/geist-app", directory) >=
            (int) sizeof executable)
            return false;
        /* geist-app refuses to start without its geistd, but its stderr goes to
         * /dev/null below: say so here instead of a misleading port hint (#70). */
        char daemon[APP_PATH_CAP];
        if (snprintf(daemon, sizeof daemon, "%s/geistd", directory) >= (int) sizeof daemon)
            return false;
        if (access(daemon, X_OK) != 0) {
            fprintf(stderr,
                    "Geisten is incomplete: %s is missing. Reinstall Geisten, or in a source "
                    "checkout run 'make' first.\n",
                    daemon);
            return false;
        }
        const char *port_arg = getenv("GEIST_PORT");
        if (!port_arg)
            port_arg = "8766";
        char         *end;
        unsigned long requested = strtoul(port_arg, &end, 10);
        if (!*port_arg || *end || requested > 65535)
            return false;
        /* Parent exits only after the service has published authenticated discovery. */
        pid_t pid = fork();
        if (pid < 0)
            return false;
        if (pid == 0) {
            /* Detach from the launching terminal; geist-app owns this process group. */
            if (setsid() < 0)
                _exit(127);
            int null = open("/dev/null", O_RDWR);
            if (null >= 0) {
                dup2(null, 0);
                dup2(null, 1);
                dup2(null, 2);
                if (null > 2)
                    close(null);
            }
            const char *model = getenv("GEIST_MODEL");
            if (model && *model)
                execl(executable,
                      executable,
                      "--home",
                      home,
                      "--port",
                      port_arg,
                      "--model",
                      model,
                      (char *) nullptr);
            else
                execl(executable, executable, "--home", home, "--port", port_arg, (char *) nullptr);
            _exit(127);
        }
    }
    for (unsigned i = 0; i < 100; i++) {
        if (request("/app/status", nullptr, 2) == 200)
            return true;
        pause_short();
    }
    fprintf(stderr, "Geisten did not start. Check port 8766 and service permissions.\n");
    return false;
}
static bool executable_directory(void) {
#ifdef __APPLE__
    uint32_t n = sizeof directory;
    if (_NSGetExecutablePath(directory, &n) != 0)
        return false;
#else
    ssize_t n = readlink("/proc/self/exe", directory, sizeof directory - 1);
    if (n < 0)
        return false;
    directory[n] = 0;
#endif
    char *slash = strrchr(directory, '/');
    if (!slash)
        return false;
    *slash = 0;
    return true;
}
/* Status fields for setup; strings are malloc'd, NULL when absent. */
struct setup_state {
    char  *id, *reason, *active, *job, *phase, *message, *name;
    bool   eligible, ready, loading, installed;
    double received, bytes;
};
static void setup_state_free(struct setup_state *s) {
    free(s->id), free(s->reason), free(s->active), free(s->job), free(s->phase), free(s->message),
            free(s->name);
    *s = (struct setup_state) {0};
}
/* Reads /app/status. The model fields describe `want`, or the recommendation. */
static bool setup_state_read(struct setup_state *s, const char *want) {
    setup_state_free(s);
    struct json *j = calloc(1, sizeof *j);
    if (!j || request("/app/status", nullptr, 5) != 200 || json_parse(j, strlen(reply), reply) < 0) {
        free(j);
        return false;
    }
    int rec     = json_get(j, 0, "recommendation");
    s->id       = json_strdup(j, json_get(j, rec, "id"));
    s->reason   = json_strdup(j, json_get(j, rec, "reason"));
    s->eligible = json_bool(j, json_get(j, rec, "eligible"), false);
    s->active   = json_strdup(j, json_get(j, 0, "active_id"));
    s->job      = json_strdup(j, json_get(j, 0, "job_model"));
    s->phase    = json_strdup(j, json_get(j, 0, "phase"));
    s->message  = json_strdup(j, json_get(j, 0, "message"));
    s->ready    = json_bool(j, json_get(j, 0, "ready"), false);
    s->loading  = json_bool(j, json_get(j, 0, "loading"), false);
    s->received = json_num(j, json_get(j, 0, "received"), 0);
    const char *id     = want ? want : s->id;
    int         models = json_get(j, 0, "models");
    for (int i = models + 1; models >= 0 && id && i < j->n; i++) {
        if (j->tok[i].parent != models)
            continue;
        char *model = json_strdup(j, json_get(j, i, "id"));
        if (model && !strcmp(model, id)) {
            s->name      = json_strdup(j, json_get(j, i, "name"));
            s->bytes     = json_num(j, json_get(j, i, "bytes"), 0);
            s->installed = json_bool(j, json_get(j, i, "installed"), false);
        }
        free(model);
    }
    free(j);
    return true;
}
/* One confirmation on the controlling terminal: stdin may be the installer script. */
static bool confirm(const char *question) {
    int tty = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (tty < 0)
        return false;
    char answer[16] = "";
    dprintf(tty, "%s [y/N] ", question);
    ssize_t n = read(tty, answer, sizeof answer - 1);
    close(tty);
    answer[n > 0 ? n : 0] = 0;
    return answer[0] == 'y' || answer[0] == 'Y' || answer[0] == 'j' || answer[0] == 'J';
}
/* `geist setup [--yes]` (#46): the first model for a fresh installation.
 * The service chooses the model (/app/setup refuses a stale choice); this
 * only asks, follows progress and runs one short real generation. */
static int setup(bool yes) {
    if (!start())
        return start_error;
    struct setup_state s = {0};
    if (!setup_state_read(&s, nullptr)) {
        fputs("Geisten is not responding.\n", stderr);
        return 1;
    }
    char id[128] = "", name[128] = "";
    if (s.ready && s.active && *s.active) {
        snprintf(id, sizeof id, "%s", s.active);
        printf("A model is already loaded: %s\n", id);
    } else if (!s.eligible || !s.id || !*s.id || !s.name) {
        fprintf(stderr, "No model fits this computer: %s\n", s.reason ? s.reason : "unknown reason");
        setup_state_free(&s);
        return 4;
    } else {
        snprintf(id, sizeof id, "%s", s.id);
        snprintf(name, sizeof name, "%s", s.name);
        char question[512];
        if (s.installed)
            snprintf(question, sizeof question, "Load %s (already downloaded)?", name);
        else
            snprintf(question, sizeof question, "Download %.1f GB and load %s?", s.bytes / 1e9, name);
        printf("Recommended model: %s. %s\n", name, s.reason ? s.reason : "");
        if (!yes && !confirm(question)) {
            printf("No model set up. Later, run: geist setup\n");
            setup_state_free(&s);
            return 0;
        }
        char              body[256];
        struct app_buffer b = {.data = body, .cap = sizeof body};
        app_put(&b, "{\"id\":");
        app_quote(&b, id);
        app_put(&b, ",\"experimental\":true}");
        /* An answer typed at the terminal is the same deliberate choice as the
         * app's model button; --yes is not, so it leaves preview consent alone. */
        if (!yes && request("/app/preview", body, 5) != 200)
            fprintf(stderr, "Cannot record the model choice: %s\n", reply);
        long code = request("/app/setup", body, 10);
        if (code != 200 && code != 202) {
            fprintf(stderr, "Setup refused: %s\n", reply);
            setup_state_free(&s);
            return 1;
        }
        /* The service downloads, verifies and loads. Ctrl-C leaves that
         * running; a later `geist setup` resumes the download. */
        char   phase[128] = "";
        int    shown = -1, idle = 0;
        bool   started = false;
        for (;;) {
            if (!setup_state_read(&s, id)) {
                fputs("Lost the connection to Geisten during setup. Run geist setup again.\n", stderr);
                setup_state_free(&s);
                return 1;
            }
            if (s.ready && s.active && !strcmp(s.active, id))
                break;
            bool working = s.loading || (s.job && !strcmp(s.job, id));
            started |= working;
            if (!working && (started || ++idle > 150)) {
                fprintf(stderr, "Setup stopped: %s\n", s.message && *s.message ? s.message : "no details");
                setup_state_free(&s);
                return 1;
            }
            if (s.phase && *s.phase && strcmp(phase, s.phase)) {
                snprintf(phase, sizeof phase, "%s", s.phase);
                printf("%s\n", phase);
            }
            int percent = s.bytes > 0 ? (int) (100 * s.received / s.bytes) / 10 * 10 : -1;
            if (s.job && *s.job && percent > shown && percent < 100 && s.received > 0) {
                printf("  %d%%\n", percent);
                shown = percent;
            }
            fflush(stdout);
            for (int i = 0; i < 2; i++)
                pause_short();
        }
        printf("Model ready: %s\n", name);
    }
    setup_state_free(&s);
    char              body[512];
    struct app_buffer b = {.data = body, .cap = sizeof body};
    app_put(&b, "{\"model\":");
    app_quote(&b, id);
    app_put(&b, ",\"messages\":[{\"role\":\"user\",\"content\":\"Say hello in one sentence.\"}],"
                "\"max_tokens\":64}");
    if (request("/v1/chat/completions", body, 180) != 200) {
        fprintf(stderr, "Test failed: %s\nThe model stays loaded; retry with: geist test\n", reply);
        return 1;
    }
    puts("Test passed: the model answered through the local API.\n"
         "Next: geist chat \"Hello\" | geist config continue | geist config opencode");
    return 0;
}
static void usage(void) {
    puts("geist start | stop | restart | status | models | open | setup [--yes]\n"
         "geist download MODEL | use MODEL | chat TEXT | test | test-agent\n"
         "geist connection | config continue | config opencode\n"
         "Connection/config output contains your private local API key.\n"
         "Set GEIST_HOME to use an isolated service data folder.");
}
static int run(int argc, char **argv) {
    if (argc < 2 || !strcmp(argv[1], "--help")) {
        usage();
        return 0;
    }
    if (!app_home(home) || !executable_directory())
        return 1;
    const char *cmd = argv[1];
    if (!strcmp(cmd, "stop") || !strcmp(cmd, "restart")) {
        long code = request("/app/quit", "{}", 5);
        if (code && code != 202) {
            fputs(reply, stderr);
            return 1;
        }
        if (!wait_stopped())
            return 1;
        if (!strcmp(cmd, "stop")) {
            puts("Geisten stopped. Downloaded models are preserved.");
            return 0;
        }
        return start() ? 0 : start_error;
    }
    if (!strcmp(cmd, "setup")) {
        if (argc > 3 || (argc == 3 && strcmp(argv[2], "--yes"))) {
            usage();
            return 2;
        }
        return setup(argc == 3);
    }
    if (!strcmp(cmd, "start") || !strcmp(cmd, "open")) {
        if (!start())
            return start_error;
        if (!strcmp(cmd, "start")) {
            puts("Geisten is running.");
            return 0;
        }
        char url[256];
        snprintf(url, sizeof url, "http://127.0.0.1:%u/#%s", port, token);
#ifdef __APPLE__
        char *args[] = {"open", url, nullptr};
#else
        if (!getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY")) {
            printf("Open through your SSH tunnel: %s\n", url);
            return 0;
        }
        if (access("/usr/bin/geist-desktop", X_OK) == 0) {
            char *desktop[] = {"/usr/bin/geist-desktop", nullptr};
            return command(desktop) == 0 ? 0 : 1;
        }
        char *args[] = {"xdg-open", url, nullptr};
#endif
        return command(args) == 0 ? 0 : 1;
    }
    if (request("/app/status", nullptr, 5) != 200) {
        fputs("Geisten is not running. Start the app or run: geist start\n", stderr);
        return 1;
    }
    if (!strcmp(cmd, "status") || !strcmp(cmd, "models")) {
        puts(reply);
        return 0;
    }
    if (!strcmp(cmd, "download") || !strcmp(cmd, "use")) {
        if (argc != 3) {
            usage();
            return 2;
        }
        char              body[1024];
        struct app_buffer b = {.data = body, .cap = sizeof body};
        app_put(&b, "{\"id\":");
        app_quote(&b, argv[2]);
        app_put(&b, "}");
        if (b.failed)
            return 2;
        long code = request(!strcmp(cmd, "download") ? "/app/download" : "/app/select", body, 5);
        puts(reply);
        return code == 202 ? 0 : 1;
    }
    if (request("/app/connections", nullptr, 5) != 200)
        return 1;
    if (!strcmp(cmd, "connection")) {
        puts(reply);
        return 0;
    }
    struct json *j = calloc(1, sizeof *j);
    if (!j)
        return 1;
    if (json_parse(j, strlen(reply), reply) < 0) {
        free(j);
        return 1;
    }
    char *model = json_strdup(j, json_get(j, 0, "model"));
    free(j);
    if (!model || !*model) {
        free(model);
        fputs("Choose and load a model in Geisten first.\n", stderr);
        return 1;
    }
    char              body[32768];
    struct app_buffer b = {.data = body, .cap = sizeof body};
    if (!strcmp(cmd, "config") && argc == 3) {
        if (!strcmp(argv[2], "continue")) {
            app_put(&b,
                    "{\"name\":\"Geisten "
                    "Local\",\"version\":\"1.0.0\",\"schema\":\"v1\",\"models\":[{\"name\":"
                    "\"Geisten\",\"provider\":\"openai\",\"model\":");
            app_quote(&b, model);
            app_printf(&b, ",\"apiBase\":\"http://127.0.0.1:%u/v1\",\"apiKey\":", port);
            app_quote(&b, token);
            app_put(&b,
                    ",\"roles\":[\"chat\"],\"capabilities\":[],\"defaultCompletionOptions\":{"
                    "\"contextLength\":4096,\"maxTokens\":512}}]}");
        } else if (!strcmp(argv[2], "opencode")) {
            app_put(&b,
                    "{\"$schema\":\"https://opencode.ai/"
                    "config.json\",\"provider\":{\"geist\":{\"npm\":\"@ai-sdk/"
                    "openai-compatible\",\"name\":\"Geisten\",\"options\":{");
            app_printf(&b, "\"baseURL\":\"http://127.0.0.1:%u/v1\",\"apiKey\":", port);
            app_quote(&b, token);
            app_put(&b, "},\"models\":{");
            app_quote(&b, model);
            app_put(&b,
                    ":{\"name\":\"Geisten local "
                    "text\",\"tool_call\":false,\"limit\":{\"context\":4096,\"output\":512}}}}},"
                    "\"model\":");
            char id[256];
            snprintf(id, sizeof id, "geist/%s", model);
            app_quote(&b, id);
            app_put(&b,
                    ",\"default_agent\":\"geist-chat\",\"agent\":{\"geist-chat\":{\"mode\":"
                    "\"primary\",\"description\":\"Local text chat without "
                    "tools\",\"prompt\":\"Answer the user briefly. You cannot access files or "
                    "execute tools.\",\"permission\":{\"*\":\"deny\"}}}}");
        } else {
            free(model);
            usage();
            return 2;
        }
        free(model);
        if (b.failed)
            return 1;
        puts(body);
        return 0;
    }
    bool test = !strcmp(cmd, "test"), agent = !strcmp(cmd, "test-agent");
    if (strcmp(cmd, "chat") && !test && !agent) {
        free(model);
        usage();
        return 2;
    }
    if (!test && !agent && argc != 3) {
        free(model);
        usage();
        return 2;
    }
    app_put(&b, "{\"model\":");
    app_quote(&b, model);
    free(model);
    app_put(&b, ",\"messages\":[{\"role\":\"user\",\"content\":");
    app_quote(&b, test || agent ? "Say hello in one sentence." : argv[2]);
    app_printf(&b, "}],\"max_tokens\":%u", agent ? 32 : test ? 512 : 256);
    if (agent)
        app_put(&b,
                ",\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"connection_probe\","
                "\"parameters\":{\"type\":\"object\",\"properties\":{}}}}]");
    app_put(&b, "}");
    if (b.failed)
        return 2;
    long code = request("/v1/chat/completions", body, 180);
    if (agent && code == 422) {
        puts("Agent tools: unsupported. Explicit rejection verified; this is not agent "
             "acceptance.");
        return 3;
    }
    if (code != 200) {
        fprintf(stderr, "Request failed (%ld): %s\n", code, reply);
        return 1;
    }
    puts(reply);
    return 0;
}
int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        return 1;
    int code = run(argc, argv);
    curl_global_cleanup();
    return code;
}
