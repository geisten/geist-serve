/* geist-app: bounded C23 supervisor and same-origin UI. The model is owned
 * by a separate geistd process; no private engine headers are used.
 * Each HTTP worker owns a 256 KiB arena, released on every exit path.
 * One joinable model job and at most eight HTTP workers may exist. */
#include "core.h"
#include "daemon.h"
#include "tasks.h"
#include "compat.h"
#include "connection.h"
#include <sys/un.h>
#include "../json.h"
#include <arpa/inet.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
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
static const unsigned char script[] = {
#embed "../../web/app.js"
};
#else
/* GCC 14 supports the C23 language used here but not #embed yet. */
#include "../../build/app_assets.h"
#endif

#define WORKER_BYTES (256u * 1024u)
#define REQUEST_CAP 32768u
#define HEADER_CAP 8192u
#define WORKER_CAP 8
#define REQUEST_TIMEOUT_MS 5000
static volatile sig_atomic_t interrupted;
static atomic_bool           closing, cancelled;
static struct {
    pthread_mutex_t         mutex;
    pthread_cond_t          drained;
    char                    home[APP_PATH_CAP], server[APP_PATH_CAP], models[APP_PATH_CAP];
    char                    token[65], message[512], active[160], active_id[64];
    char                    chosen[APP_PATH_CAP];
    unsigned                port, workers;
    char                    runtime_dir[64], socket_path[100];
    pid_t                   child;
    bool                    ready, generating, job_running, job_joinable;
    pthread_t               job;
    const struct app_model *job_model;
    bool                    job_download;
    uint64_t                received;
    char                    phase[32];
    struct {
        double   tps;
        unsigned tokens;
    } measurements[APP_MODEL_COUNT];
} app = {.mutex = PTHREAD_MUTEX_INITIALIZER, .drained = PTHREAD_COND_INITIALIZER};

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

static void response(int fd, int status, const char *type, const void *body, size_t n) {
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
    if (k > 0 && (size_t) k < sizeof header && send_bytes(fd, header, (size_t) k))
        (void) send_bytes(fd, body, n);
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
static void poll_child(void) {
    if (!app.child)
        return;
    int   status;
    pid_t result = waitpid(app.child, &status, WNOHANG);
    if (result == app.child || (result < 0 && errno == ECHILD)) {
        app.child = 0;
        app.ready = false;
        snprintf(app.message,
                 sizeof app.message,
                 "The model process stopped. See server.log in the app data folder.");
        return;
    }
    if (app.ready)
        return;
    if (app_daemon_ready(app.socket_path)) {
        app.ready      = true;
        app.message[0] = 0;
    }
}

static void stop_child(void) {
    if (!app.child)
        goto cleanup;
    kill(app.child, SIGTERM);
    for (unsigned i = 0; i < 20; ++i) {
        int   status;
        pid_t result = waitpid(app.child, &status, WNOHANG);
        if (result == app.child || (result < 0 && errno == ECHILD)) {
            app.child = 0;
            break;
        }
        struct timespec pause = {.tv_nsec = 25000000};
        nanosleep(&pause, nullptr);
    }
    if (app.child) {
        kill(app.child, SIGKILL);
        while (waitpid(app.child, nullptr, 0) < 0 && errno == EINTR) {
        }
    }
    app.child = 0;
cleanup:
    app.ready = false;
    if (app.socket_path[0])
        unlink(app.socket_path);
    if (app.runtime_dir[0])
        rmdir(app.runtime_dir);
    app.socket_path[0] = app.runtime_dir[0] = 0;
}

static bool start_child(const char *path, const char *id) {
    stop_child();
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
    char **env = calloc(count + 4, sizeof *env);
    if (!env) {
        close(fd);
        close(log);
        return false;
    }
    size_t used = 0;
    for (size_t i = 0; i < count; ++i)
        if (strncmp(environ[i], "LISTEN_FDS=", 11) && strncmp(environ[i], "LISTEN_PID=", 11) &&
            strncmp(environ[i], "OMP_NUM_THREADS=", 16) &&
            strncmp(environ[i], "OMP_WAIT_POLICY=", 16))
            env[used++] = environ[i];
    unsigned cores = known ? hardware.cores : 1;
    unsigned limit = known && hardware.device == APP_PI5 ? 4 : 2;
    if (cores > limit)
        cores = limit;
    char threads[40];
    snprintf(threads, sizeof threads, "OMP_NUM_THREADS=%u", cores);
    env[used++] = threads;
    env[used++] = "OMP_WAIT_POLICY=passive";
    env[used++] = "LISTEN_FDS=1";
    posix_spawn_file_actions_t actions;
    int                        rc = posix_spawn_file_actions_init(&actions);
    if (!rc) {
        rc = posix_spawn_file_actions_adddup2(&actions, log, STDOUT_FILENO);
        if (!rc)
            rc = posix_spawn_file_actions_adddup2(&actions, log, STDERR_FILENO);
        if (!rc)
            rc = posix_spawn_file_actions_adddup2(&actions, fd, 3);
        if (!rc)
            rc = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        if (!rc)
            rc = posix_spawn(&app.child, app.server, &actions, nullptr, args, env);
        posix_spawn_file_actions_destroy(&actions);
    }
    free(env);
    close(fd);
    close(log);
    if (rc) {
        app.child = 0;
        snprintf(app.message, sizeof app.message, "Cannot start geistd: %s", strerror(rc));
        return false;
    }
    snprintf(app.chosen, sizeof app.chosen, "%s", path);
    snprintf(app.active_id, sizeof app.active_id, "%s", id ? id : "custom");
    const char *base = strrchr(path, '/');
    base             = base ? base + 1 : path;
    snprintf(app.active, sizeof app.active, "%s", base);
    char *ext = strstr(app.active, ".gguf");
    if (ext)
        *ext = 0;
    snprintf(app.message, sizeof app.message, "Loading the model into memory…");
    return true;
}

static uint64_t regular_size(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 ? (uint64_t) st.st_size
                                                                          : 0;
}

static void save_selection(const char *id) {
    char target[APP_PATH_CAP], temporary[APP_PATH_CAP];
    if (!path_join(target, app.home, "selected") || !path_join(temporary, app.home, "selected.tmp"))
        return;
    int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return;
    size_t n  = strlen(id);
    bool   ok = write(fd, id, n) == (ssize_t) n && fsync(fd) == 0;
    close(fd);
    if (ok)
        (void) rename(temporary, target);
    else
        unlink(temporary);
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
    if (ok && !atomic_load(&cancelled) && !atomic_load(&closing)) {
        pthread_mutex_lock(&app.mutex);
        strcpy(app.phase, "verifying");
        pthread_mutex_unlock(&app.mutex);
        bool size_ok = regular_size(verify) == m->bytes;
        bool hash_ok = size_ok && app_sha256_interruptible(verify, hash, job_cancelled);
        ok           = hash_ok && strcmp(hash, m->sha256) == 0;
        if (!ok) {
            snprintf(why,
                     sizeof why,
                     "Checksum or size mismatch. The model was not started; download it again.");
            if (!job_cancelled() && (!size_ok || hash_ok))
                unlink(verify);
            else if (!job_cancelled())
                snprintf(
                        why,
                        sizeof why,
                        "Cannot read the model for verification. Check disk and file permissions.");
        }
        if (ok && app.job_download && rename(part, target) != 0) {
            ok = false;
            snprintf(why, sizeof why, "Cannot finish download: %s", strerror(errno));
        }
    }
    pthread_mutex_lock(&app.mutex);
    if (atomic_load(&cancelled) || atomic_load(&closing)) {
        snprintf(app.message,
                 sizeof app.message,
                 "Download or verification cancelled. Partial downloads can be resumed.");
    } else if (ok) {
        if (start_child(target, m->id))
            save_selection(m->id);
    } else
        snprintf(app.message, sizeof app.message, "%s", *why ? why : "Cannot prepare the model.");
    app.job_running = false;
    app.phase[0]    = 0;
    pthread_mutex_unlock(&app.mutex);
    return nullptr;
}

static bool begin_job(const struct app_model *m, bool download) {
    if (app.job_joinable) {
        pthread_join(app.job, nullptr);
        app.job_joinable = false;
    }
    app.job_model    = m;
    app.job_download = download;
    app.job_running  = true;
    app.received     = 0;
    app.message[0]   = 0;
    strcpy(app.phase, download ? "downloading" : "verifying");
    atomic_store(&cancelled, false);
    if (pthread_create(&app.job, nullptr, model_job, nullptr) != 0) {
        app.job_running = false;
        strcpy(app.message, "Cannot start model worker.");
        return false;
    }
    app.job_joinable = true;
    return true;
}

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
    poll_child();
    app_put(&b, "{\"hardware\":{\"name\":");
    app_quote(&b, h.name);
    app_put(&b, ",\"arch\":");
    app_quote(&b, h.arch);
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
               ",\"received\":%llu,\"ready\":%s,\"loading\":%s,\"busy\":%s,\"models\":[",
               (unsigned long long) app.received,
               app.ready ? "true" : "false",
               app.child && !app.ready ? "true" : "false",
               app.job_running || app.generating ? "true" : "false");
    for (size_t i = 0; i < APP_MODEL_COUNT; ++i) {
        const struct app_model *m = &app_models[i];
        char                    path[APP_PATH_CAP], part[APP_PATH_CAP];
        bool                    valid_path = path_join(path, app.models, m->file);
        bool                    installed  = valid_path && regular_size(path) == m->bytes;
        uint64_t                partial    = 0;
        if (valid_path && snprintf(part, sizeof part, "%s.part", path) < (int) sizeof part)
            partial = regular_size(part);
        struct app_hardware adjusted = h;
        if (partial <= m->bytes && h.disk_known && UINT64_MAX - adjusted.disk > partial)
            adjusted.disk += partial;
        struct app_assessment a =
                app_assess_observed(&adjusted, m, installed, app.measurements[i].tps);
        if (i)
            app_put(&b, ",");
        app_put(&b, "{\"id\":");
        app_quote(&b, m->id);
        app_put(&b, ",\"name\":");
        app_quote(&b, m->name);
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
        app_put(&b, ",\"performance\":");
        app_quote(&b, a.performance);
        app_printf(&b,
                   ",\"measured_tps\":%.3f,\"measured_tokens\":%u}",
                   app.measurements[i].tps,
                   app.measurements[i].tokens);
    }
    app_put(&b, "]}");
    pthread_mutex_unlock(&app.mutex);
    if (b.failed)
        error_response(fd, 503, "Status exceeds the response memory budget.");
    else
        response(fd, 200, "application/json", body, b.len);
}

struct proxy {
    int             fd;
    bool            started;
    double          start;
    struct app_utf8 utf8;
};
static bool proxy_cancel(void *opaque) {
    struct proxy *p = opaque;
    char          one;
    ssize_t       n = recv(p->fd, &one, 1, MSG_PEEK | MSG_DONTWAIT);
    return atomic_load(&closing) || monotonic_ms() - p->start > 180000 || n == 0 ||
           (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
}
static bool proxy_emit(void *opaque, const char *piece) {
    struct proxy *p = opaque;
    if (proxy_cancel(p))
        return false;
    char decoded[8192];
    if (!app_utf8_feed(&p->utf8, piece, decoded, sizeof decoded))
        return false;
    if (!decoded[0] && piece[0])
        return true;
    if (!p->started) {
        const char *head = "HTTP/1.1 200 OK\r\nContent-Type: application/x-ndjson\r\n"
                           "Cache-Control: no-store\r\nConnection: "
                           "close\r\nX-Content-Type-Options: nosniff\r\n\r\n";
        if (!send_bytes(p->fd, head, strlen(head)))
            return false;
        p->started = true;
    }
    char              data[16384];
    struct app_buffer b = {.data = data, .cap = sizeof data};
    app_put(&b, "{\"response\":");
    app_quote(&b, decoded);
    app_put(&b, ",\"done\":false}\n");
    return !b.failed && send_bytes(p->fd, data, b.len);
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
    poll_child();
    if (!app.ready || app.generating || app.job_running) {
        pthread_mutex_unlock(&app.mutex);
        free(prompt);
        error_response(fd, 409, "Wait until the model is ready and idle.");
        return;
    }
    struct app_hardware hardware;
    bool                hardware_known = app_hardware_read(&hardware, app.models);
    enum app_quality    quality = app_task_quality(app_model_find(app.active_id),
                                                   task,
                                                   language,
                                                   hardware_known ? hardware.device : APP_UNKNOWN);
    if (quality != APP_QUALITY_PASSED &&
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
    for (int i = 0; i < APP_MODEL_COUNT; ++i)
        if (strcmp(app.active_id, app_models[i].id) == 0)
            model_index = i;
    app.generating = true;
    pthread_mutex_unlock(&app.mutex);
    struct proxy         proxy = {.fd = fd, .start = monotonic_ms()};
    struct app_run_stats stats;
    char                 error[256];
    int                  rc = app_daemon_run(app.socket_path,
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
    if (rc == 0 && !proxy.started && !proxy_emit(&proxy, ""))
        rc = 502;
    if (!proxy.started)
        error_response(fd, rc ? rc : 502, error);
    else if (rc) {
        const char *err = "{\"error\":\"Generation interrupted before completion.\"}\n";
        (void) send_bytes(fd, err, strlen(err));
    } else {
        char final[512];
        int  n = snprintf(final,
                          sizeof final,
                          "{\"done\":true,\"eval_count\":%zu,\"eval_duration\":%.0f,"
                          "\"total_duration\":%.0f,\"prompt_eval_count\":%zu,\"reused\":%zu,"
                          "\"limited\":%s}\n",
                          stats.tokens,
                          stats.generation_ns,
                          stats.total_ns,
                          stats.prompt_tokens,
                          stats.reused,
                          stats.limited ? "true" : "false");
        (void) send_bytes(fd, final, (size_t) n);
        if (model_index >= 0 && stats.tokens >= 16 && stats.generation_ns > 1e6) {
            pthread_mutex_lock(&app.mutex);
            app.measurements[model_index].tps    = stats.tokens / (stats.generation_ns / 1e9);
            app.measurements[model_index].tokens = (unsigned) stats.tokens;
            pthread_mutex_unlock(&app.mutex);
        }
    }
    pthread_mutex_lock(&app.mutex);
    app.generating = false;
    pthread_mutex_unlock(&app.mutex);
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

static bool completion_emit(void *opaque, const char *piece) {
    struct completion_proxy *p = opaque;
    if (proxy_cancel(&p->transport))
        return false;
    char decoded[8192];
    if (!app_utf8_feed(&p->transport.utf8, piece, decoded, sizeof decoded))
        return false;
    if (!decoded[0] && piece[0])
        return true;
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
    poll_child();
    if (!app.ready || app.job_running) {
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
    pthread_mutex_unlock(&app.mutex);
    struct app_run_stats stats;
    char                 error[256];
    int                  rc = app_daemon_chat(app.socket_path,
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
    if (!rc && chat.stream && !p.transport.started && !completion_emit(&p, ""))
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
            if (!b.failed)
                (void) send_bytes(fd, body, b.len);
        } else if (b.failed)
            api_error(fd, 502, "Completion exceeds response capacity.");
        else
            response(fd, 200, "application/json", body, b.len);
    }
    pthread_mutex_lock(&app.mutex);
    app.generating = false;
    pthread_mutex_unlock(&app.mutex);
}

static void connections(int fd, bool models) {
    char              body[4096];
    struct app_buffer b = {.data = body, .cap = sizeof body};
    pthread_mutex_lock(&app.mutex);
    poll_child();
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
                   "1024,\"chat\":true,\"tools\":false,\"quality\":\"unverified\"}",
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
    if (strcmp(r.path, "/app/cancel") == 0) {
        atomic_store(&cancelled, true);
        response(fd, 200, "application/json", "{}", 2);
        return;
    }
    if (strcmp(r.path, "/app/quit") == 0) {
        atomic_store(&closing, true);
        response(fd, 202, "application/json", "{}", 2);
        return;
    }
    if (strcmp(r.path, "/app/stop") == 0) {
        pthread_mutex_lock(&app.mutex);
        if (app.generating || app.job_running) {
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
    bool download = strcmp(r.path, "/app/download") == 0;
    if (!download && strcmp(r.path, "/app/select") != 0) {
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
    char                   *id    = json_strdup(json, json_get(json, 0, "id"));
    const struct app_model *model = app_model_find(id);
    free(id);
    if (!model) {
        error_response(fd, 400, "Choose a model from the catalog.");
        return;
    }
    pthread_mutex_lock(&app.mutex);
    if (app.job_running || app.generating) {
        pthread_mutex_unlock(&app.mutex);
        error_response(fd, 409, "Another task is active.");
        return;
    }
    char                path[APP_PATH_CAP], part[APP_PATH_CAP];
    bool                valid     = path_join(path, app.models, model->file);
    bool                installed = valid && regular_size(path) == model->bytes;
    struct app_hardware h;
    bool                known = app_hardware_read(&h, app.models);
    if (valid && snprintf(part, sizeof part, "%s.part", path) < (int) sizeof part) {
        uint64_t partial = regular_size(part);
        if (partial <= model->bytes && UINT64_MAX - h.disk > partial)
            h.disk += partial;
    }
    struct app_assessment assessment = app_assess(&h, model, installed && !download);
    if (!known || assessment.fit == APP_UNAVAILABLE || (!download && !installed)) {
        pthread_mutex_unlock(&app.mutex);
        error_response(fd,
                       409,
                       !known ? "Cannot read this computer's resources."
                              : (!download && !installed ? "Download this model first."
                                                         : assessment.reason));
        return;
    }
    bool ok = begin_job(model, download);
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
        for (size_t i = 0; i < APP_MODEL_COUNT; ++i) {
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
    if (model) {
        pthread_mutex_lock(&app.mutex);
        (void) start_child(model, "custom");
        pthread_mutex_unlock(&app.mutex);
    } else {
        char selected[APP_PATH_CAP], id[64] = "";
        if (path_join(selected, app.home, "selected")) {
            FILE *f = fopen(selected, "r");
            if (f) {
                (void) fgets(id, sizeof id, f);
                fclose(f);
            }
        }
        const struct app_model *m = app_model_find(id);
        if (m) {
            pthread_mutex_lock(&app.mutex);
            (void) begin_job(m, false);
            pthread_mutex_unlock(&app.mutex);
        }
    }
    while (!interrupted && !atomic_load(&closing)) {
        struct timeval timeout = {.tv_sec = 1};
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
    pthread_mutex_lock(&app.mutex);
    while (app.workers)
        pthread_cond_wait(&app.drained, &app.mutex);
    pthread_mutex_unlock(&app.mutex);
    if (app.job_joinable)
        pthread_join(app.job, nullptr);
    stop_child();
    app_connection_remove(app.home);
    curl_global_cleanup();
    close(lock);
    return 0;
}
