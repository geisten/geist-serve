/* Small C23 client. Models and inference remain owned by the shared service. */
#include "connection.h"
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
static bool start(void) {
    if (request("/app/status", nullptr, 2) == 200)
        return true;
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
#ifndef __APPLE__
    char standard[APP_PATH_CAP];
    snprintf(standard,
             sizeof standard,
             "%s/.local/share/geist",
             getenv("HOME") ? getenv("HOME") : "");
    if (!getenv("GEIST_HOME") && !strcmp(home, standard) &&
        access("/usr/lib/systemd/user/geist.service", R_OK) == 0) {
        char *args[] = {"systemctl", "--user", "start", "geist.service", nullptr};
        if (command(args) != 0) {
            fprintf(stderr,
                    "Cannot start the user service. In a headless session, run geist-app in the "
                    "foreground.\n");
            return false;
        }
    } else
#endif
    {
        char executable[APP_PATH_CAP];
        if (snprintf(executable, sizeof executable, "%s/geist-app", directory) >=
            (int) sizeof executable)
            return false;
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
    fprintf(stderr, "Geist did not start. Check port 8766 and service permissions.\n");
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
static void usage(void) {
    puts("geist start | stop | restart | status | models | open\n"
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
        bool stopped = false;
        for (unsigned i = 0; i < 100; i++) {
            char descriptor[APP_PATH_CAP];
            if (snprintf(descriptor, sizeof descriptor, "%s/connection.json", home) >=
                (int) sizeof descriptor)
                return 1;
            if (access(descriptor, F_OK) < 0 && errno == ENOENT) {
                stopped = true;
                break;
            }
            pause_short();
        }
        if (!stopped) {
            fputs("Cannot confirm the service stopped. Check the service log before restarting.\n",
                  stderr);
            return 1;
        }
        if (!strcmp(cmd, "stop")) {
            puts("Geist stopped. Downloaded models are preserved.");
            return 0;
        }
        /* Allow the old owner to release its process lock after unlinking discovery. */
        pause_short();
        return start() ? 0 : 1;
    }
    if (!strcmp(cmd, "start") || !strcmp(cmd, "open")) {
        if (!start())
            return 1;
        if (!strcmp(cmd, "start")) {
            puts("Geist is running.");
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
        char *args[] = {"xdg-open", url, nullptr};
#endif
        return command(args) == 0 ? 0 : 1;
    }
    if (request("/app/status", nullptr, 5) != 200) {
        fputs("Geist is not running. Start the app or run: geist start\n", stderr);
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
        fputs("Choose and load a model in Geist first.\n", stderr);
        return 1;
    }
    char              body[32768];
    struct app_buffer b = {.data = body, .cap = sizeof body};
    if (!strcmp(cmd, "config") && argc == 3) {
        if (!strcmp(argv[2], "continue")) {
            app_put(&b,
                    "{\"name\":\"Geist "
                    "Local\",\"version\":\"1.0.0\",\"schema\":\"v1\",\"models\":[{\"name\":"
                    "\"Geist\",\"provider\":\"openai\",\"model\":");
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
                    "openai-compatible\",\"name\":\"Geist\",\"options\":{");
            app_printf(&b, "\"baseURL\":\"http://127.0.0.1:%u/v1\",\"apiKey\":", port);
            app_quote(&b, token);
            app_put(&b, "},\"models\":{");
            app_quote(&b, model);
            app_put(&b,
                    ":{\"name\":\"Geist local "
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
    app_printf(&b, "}],\"max_tokens\":%u", test || agent ? 32 : 256);
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
