/* Local numeric history. One writer owns disk I/O; enqueue never touches disk.
 * Memory is bounded independently of journal size. No engine or UI dependency. */
#include "performance.h"
#include "../json.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define LINE_CAP 8192u
#define SEGMENT_BYTES (10u * 1024u * 1024u)
static struct {
    pthread_mutex_t    mutex, disk;
    pthread_cond_t     wake;
    pthread_t          writer;
    bool               started, closing, enabled, error, legacy_deleted, expired;
    double             compacted;
    unsigned           days;
    uint64_t           epoch, sequence, dropped, persisted, invalid, boot;
    int                dir;
    struct perf_record records[PERF_RECORDS], queue[PERF_QUEUE];
    size_t             count, head, queued;
} p = {.mutex   = PTHREAD_MUTEX_INITIALIZER,
       .disk    = PTHREAD_MUTEX_INITIALIZER,
       .wake    = PTHREAD_COND_INITIALIZER,
       .dir     = -1,
       .enabled = true,
       .days    = 90,
       .epoch   = 1};
static bool token(const char *s, size_t cap) {
    size_t n = strnlen(s, cap);
    return n > 0 && n < cap;
}
static bool valid(const struct perf_record *r) {
    return app_engine_valid(&r->engine) && token(r->id, sizeof r->id) &&
           token(r->artifact, sizeof r->artifact) && token(r->series, sizeof r->series) &&
           token(r->backend, sizeof r->backend) &&
           (!strcmp(r->source, "app") || !strcmp(r->source, "api") ||
            !strcmp(r->source, "controlled_test") || !strcmp(r->source, "legacy_last_reply")) &&
           (!strcmp(r->outcome, "no_answer") || !strcmp(r->outcome, "completed") ||
            !strcmp(r->outcome, "error") || !strcmp(r->outcome, "interrupted") ||
            !strcmp(r->outcome, "disconnected") || !strcmp(r->outcome, "cancelled")) &&
           isfinite(r->timestamp) && r->timestamp > 0 && r->timestamp <= time(nullptr) + 300 &&
           isfinite(r->generation_ns) && r->generation_ns >= 0 && r->generation_ns <= 3.6e12 &&
           isfinite(r->total_ns) && r->total_ns >= 0 && r->total_ns <= 3.6e12 &&
           isfinite(r->first_ns) && r->first_ns >= -1 && r->first_ns <= r->total_ns &&
           isfinite(r->first_answer_ns) && r->first_answer_ns >= -1 &&
           r->first_answer_ns <= r->total_ns && isfinite(r->prefill_ns) && r->prefill_ns >= -1 &&
           r->prefill_ns <= 3.6e12 && isfinite(r->load_ns) && r->load_ns >= -1 &&
           r->load_ns <= 3.6e12 && isfinite(r->rss) && r->rss >= -1 && r->rss <= 1e15 &&
           isfinite(r->peak_rss) && r->peak_rss >= -1 && r->peak_rss <= 1e15 &&
           isfinite(r->cpu_percent) && r->cpu_percent >= -1 && r->cpu_percent <= 100.01 &&
           app_memory_valid(&r->memory) && r->input <= 1000000 && r->output <= 1000000 &&
           r->reused <= r->input && isfinite(r->temperature) && r->temperature >= 0 &&
           r->temperature <= 10 && isfinite(r->top_p) && r->top_p >= 0 && r->top_p <= 1 &&
           r->samples <= 3601;
}
static bool eligible(const struct perf_record *r) {
    return !strcmp(r->outcome, "completed") && strcmp(r->source, "legacy_last_reply") &&
           !r->warmup && r->output > 0 && r->generation_ns > 0 && r->first_ns >= 0;
}
unsigned perf_group(const struct perf_record *r) {
    unsigned in  = r->input <= 512 ? 0 : r->input <= 2048 ? 1 : 2;
    unsigned out = r->output < 32 ? 0 : r->output < 128 ? 1 : r->output < 512 ? 2 : 3;
    return in + 3 * out + 12 * (r->reused > 0) + 24 * r->cold + 48 * r->contention +
           96 * (!strcmp(r->source, "controlled_test"));
}
static void number(struct app_buffer *b, double n) {
    if (n < 0 || !isfinite(n))
        app_put(b, "null");
    else
        app_printf(b, "%.17g", n);
}
static void record_json(struct app_buffer *b, const struct perf_record *r) {
    app_printf(b,
               "{\"schema\":%u,\"reasoning\":%s",
               r->schema == 1 ? 1 : 2,
               r->reasoning ? "true" : "false");
    if (r->schema != 1) {
        app_put(b, ",\"engine\":");
        app_engine_json(b, &r->engine);
    }
#define S(k)                    \
    app_put(b, ",\"" #k "\":"); \
    app_quote(b, r->k)
    S(id);
    S(model);
    S(artifact);
    S(quantization);
    S(series);
    S(backend);
    S(source);
    S(outcome);
    S(finish);
    S(run);
#undef S
#define N(k)                    \
    app_put(b, ",\"" #k "\":"); \
    number(b, r->k)
    N(timestamp);
    N(generation_ns);
    N(first_ns);
    N(first_answer_ns);
    N(total_ns);
    N(prefill_ns);
    N(load_ns);
    N(rss);
    N(peak_rss);
    N(cpu_percent);
    N(temperature);
    N(top_p);
#undef N
    app_printf(b,
               ",\"input\":%llu,\"output\":%llu,\"reused\":%llu,\"max_tokens\":%u,"
               "\"threads\":%u,\"samples\":%u,\"sample_interval_ms\":2000,"
               "\"cold\":%s,\"contention\":%s,\"warmup\":%s,\"gpu_memory\":null,"
               "\"unavailable_reason\":\"legacy_field_use_memory_scopes\"",
               (unsigned long long) r->input,
               (unsigned long long) r->output,
               (unsigned long long) r->reused,
               r->max_tokens,
               r->threads,
               r->samples,
               r->cold ? "true" : "false",
               r->contention ? "true" : "false",
               r->warmup ? "true" : "false");
    app_put(b, ",\"memory\":");
    app_memory_json(b, &r->memory, r->rss, r->peak_rss, r->samples);
    app_put(b, "}\n");
}
static bool parse(const char *line, struct perf_record *r) {
    struct json *j  = calloc(1, sizeof *j);
    bool         ok = false;
    if (!j || json_parse(j, strlen(line), line) < 0 ||
        (json_num(j, json_get(j, 0, "schema"), 0) != 1 &&
         json_num(j, json_get(j, 0, "schema"), 0) != 2))
        goto end;
    memset(r, 0, sizeof *r);
    r->schema = (unsigned) json_num(j, json_get(j, 0, "schema"), 0);
    if (r->schema == 2 && (json_get(j, 0, "engine") < 0 ||
                           !app_engine_parse(&r->engine, j, json_get(j, 0, "engine"))))
        goto end;
    r->reasoning = json_bool(j, json_get(j, 0, "reasoning"), false);
#define S(k)                                          \
    do {                                              \
        char *s = json_strdup(j, json_get(j, 0, #k)); \
        if (!s || strlen(s) >= sizeof r->k) {         \
            free(s);                                  \
            goto end;                                 \
        }                                             \
        strcpy(r->k, s);                              \
        free(s);                                      \
    } while (0)
    S(id);
    S(model);
    S(artifact);
    S(quantization);
    S(series);
    S(backend);
    S(source);
    S(outcome);
    S(finish);
    S(run);
#undef S
#define N(k) r->k = json_num(j, json_get(j, 0, #k), -1)
    N(timestamp);
    N(generation_ns);
    N(first_ns);
    N(first_answer_ns);
    N(total_ns);
    N(prefill_ns);
    N(load_ns);
    N(rss);
    N(peak_rss);
    N(cpu_percent);
    N(temperature);
    N(top_p);
#undef N
#define U(k)                                                       \
    do {                                                           \
        double n = json_num(j, json_get(j, 0, #k), -1);            \
        if (!isfinite(n) || n < 0 || n > 1000000 || floor(n) != n) \
            goto end;                                              \
        r->k = (unsigned) n;                                       \
    } while (0)
    U(input);
    U(output);
    U(reused);
    U(max_tokens);
    U(threads);
    U(samples);
#undef U
    r->cold       = json_bool(j, json_get(j, 0, "cold"), false);
    r->contention = json_bool(j, json_get(j, 0, "contention"), false);
    r->warmup     = json_bool(j, json_get(j, 0, "warmup"), false);
    ok            = app_memory_parse(&r->memory, j, json_get(j, 0, "memory")) && valid(r);
end:
    free(j);
    return ok;
}
static bool known(const char *id) {
    for (size_t i = 0; i < p.count; i++)
        if (!strcmp(p.records[i].id, id))
            return true;
    return false;
}
static void retain(const struct perf_record *r) {
    double cutoff = (double) time(nullptr) - p.days * 86400.;
    size_t kept   = 0;
    for (size_t i = 0; i < p.count; i++)
        if (p.records[i].timestamp >= cutoff)
            p.records[kept++] = p.records[i];
    if (kept < p.count)
        p.expired = true;
    p.count = kept;
    if (r->timestamp < cutoff) {
        p.expired = true;
        return;
    }
    if (known(r->id))
        return;
    if (p.count == PERF_RECORDS) {
        memmove(p.records, p.records + 1, (PERF_RECORDS - 1) * sizeof *r);
        --p.count;
    }
    p.records[p.count++] = *r;
}
static int private_file(const char *name, int flags) {
    if (p.dir < 0)
        return -1;
    int         fd = openat(p.dir, name, flags | O_NOFOLLOW | O_CLOEXEC, 0600);
    struct stat st;
    if (fd >= 0 && (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != getuid() ||
                    st.st_nlink != 1 || (st.st_mode & 077))) {
        close(fd);
        errno = EPERM;
        return -1;
    }
    return fd;
}
static bool write_all(int fd, const char *s, size_t n) {
    while (n) {
        ssize_t w = write(fd, s, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return false;
        s += w;
        n -= (size_t) w;
    }
    return true;
}
static bool atomic_file(const char *name, const char *data, size_t n) {
    char temp[80];
    snprintf(temp, sizeof temp, ".%s.tmp", name);
    /* Never follow or truncate an existing unknown temporary file. */
    int fd = private_file(temp, O_WRONLY | O_CREAT | O_EXCL);
    if (fd < 0)
        return false;
    bool ok = write_all(fd, data, n) && fsync(fd) == 0;
    close(fd);
    if (ok)
        ok = renameat(p.dir, temp, p.dir, name) == 0;
    if (!ok)
        unlinkat(p.dir, temp, 0);
    if (ok)
        ok = fsync(p.dir) == 0;
    return ok;
}
static bool append(const struct perf_record *r) {
#ifdef APP_TESTING
    if (getenv("GEIST_TEST_PERFORMANCE_FULL")) {
        errno = ENOSPC;
        return false;
    }
    if (getenv("GEIST_TEST_PERFORMANCE_SLOW")) {
        struct timespec pause = {.tv_nsec = 20000000};
        nanosleep(&pause, nullptr);
    }
#endif
    char              line[LINE_CAP];
    struct app_buffer b = {.data = line, .cap = sizeof line};
    record_json(&b, r);
    if (b.failed)
        return false;
    int fd = private_file("observations.jsonl", O_WRONLY | O_APPEND | O_CREAT);
    if (fd < 0)
        return false;
    struct stat st;
    bool        ok = fstat(fd, &st) == 0;
    if (ok && (uint64_t) st.st_size + b.len > SEGMENT_BYTES) {
        close(fd);
        /* rename replaces the directory entry, never a symlink's target. */
        if (renameat(p.dir, "observations.jsonl", p.dir, "observations.1.jsonl"))
            return false;
        fd = private_file("observations.jsonl", O_WRONLY | O_APPEND | O_CREAT | O_EXCL);
        if (fd < 0)
            return false;
        ok = fsync(p.dir) == 0;
    }
    if (ok)
        ok = write_all(fd, line, b.len) && fsync(fd) == 0;
    close(fd);
    return ok;
}
/* One writer. Disk lock also serializes explicit settings/delete/export. The
 * queue lock is released before every filesystem call. */
static bool  compact_history(void);
static void *writer(void *unused) {
    (void) unused;
    for (;;) {
        pthread_mutex_lock(&p.mutex);
        while (!p.queued && !p.closing) {
            struct timespec deadline;
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_sec += 60;
            if (pthread_cond_timedwait(&p.wake, &p.mutex, &deadline) == ETIMEDOUT)
                break;
        }
        bool done = p.closing && !p.queued;
        pthread_mutex_unlock(&p.mutex);
        if (done)
            break;
        pthread_mutex_lock(&p.disk);
        pthread_mutex_lock(&p.mutex);
        bool               available = p.queued > 0;
        struct perf_record r         = {0};
        if (available) {
            r      = p.queue[p.head];
            p.head = (p.head + 1) % PERF_QUEUE;
            --p.queued;
        }
        pthread_mutex_unlock(&p.mutex);
        if (available) {
            bool stored = append(&r);
            bool ok     = stored;
            if (ok && ((double) time(nullptr) - p.compacted >= 86400))
                ok = compact_history();
            if (ok) {
                char              cache[65536];
                struct app_buffer b   = {.data = cache, .cap = sizeof cache};
                bool              cpu = !strncmp(r.backend, "cpu", 3);
                perf_view(&b, r.artifact, r.series, cpu ? r.backend : "", cpu ? "" : r.backend);
                if (b.failed || !atomic_file("profiles.json", cache, b.len))
                    ok = false;
            }
            pthread_mutex_lock(&p.mutex);
            if (stored)
                ++p.persisted;
            else
                ++p.dropped;
            if (!ok)
                p.error = true;
            pthread_mutex_unlock(&p.mutex);
        }
        if (!available && (double) time(nullptr) - p.compacted >= 86400) {
            bool ok = compact_history();
            if (!ok) {
                pthread_mutex_lock(&p.mutex);
                p.error = true;
                pthread_mutex_unlock(&p.mutex);
            }
        }
        pthread_mutex_unlock(&p.disk);
    }
    return nullptr;
}
static void load(const char *name) {
    int fd = private_file(name, O_RDONLY);
    if (fd < 0) {
        if (errno != ENOENT)
            p.error = true;
        return;
    }
    struct stat st;
    if (fstat(fd, &st) || st.st_size > SEGMENT_BYTES) {
        p.error = true;
        close(fd);
        return;
    }
    FILE *f = fdopen(fd, "r");
    if (!f) {
        close(fd);
        return;
    }
    char line[LINE_CAP];
    bool dropping = false;
    while (fgets(line, sizeof line, f)) {
        bool newline = strchr(line, '\n') != nullptr;
        if (dropping) {
            if (newline)
                dropping = false;
            continue;
        }
        struct perf_record r;
        if (!newline || !parse(line, &r)) {
            ++p.invalid;
            if (!newline)
                dropping = true;
            continue;
        }
        if (!known(r.id)) {
            retain(&r);
            ++p.persisted;
        }
    }
    fclose(f);
}
void perf_init(const char *home) {
    int root = open(home, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root >= 0) {
        if (mkdirat(root, "performance", 0700) == 0 || errno == EEXIST)
            p.dir = openat(root, "performance", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(root);
    }
    struct stat st;
    if (p.dir >= 0 && (fstat(p.dir, &st) || st.st_uid != getuid() || (st.st_mode & 077))) {
        close(p.dir);
        p.dir = -1;
    }
    if (p.dir < 0)
        p.error = true;
    const char *staging[] = {".settings.json.tmp",
                             ".profiles.json.tmp",
                             ".observations.jsonl.tmp",
                             ".observations.1.jsonl.tmp",
                             ".export.jsonl.tmp"};
    for (size_t i = 0; i < sizeof staging / sizeof *staging; i++) {
        int pending = private_file(staging[i], O_RDONLY);
        if (pending >= 0) {
            close(pending);
            if (unlinkat(p.dir, staging[i], 0))
                p.error = true;
        } else if (errno != ENOENT)
            p.error = true;
    }
    int settings = private_file("settings.json", O_RDONLY);
    if (settings >= 0) {
        char    text[256] = {0};
        ssize_t n         = read(settings, text, sizeof text - 1);
        close(settings);
        struct json *j = calloc(1, sizeof *j);
        if (n > 0 && j && json_parse(j, (size_t) n, text) >= 0) {
            p.legacy_deleted = json_bool(j, json_get(j, 0, "legacy_deleted"), false);
            p.enabled        = json_bool(j, json_get(j, 0, "enabled"), false);
            double days      = json_num(j, json_get(j, 0, "days"), 90);
            if (days == 30 || days == 90 || days == 365)
                p.days = (unsigned) days;
            else
                p.error = true;
        } else {
            p.enabled = false;
            p.error   = true;
        }
        free(j);
    } else if (errno != ENOENT) {
        p.enabled = false;
        p.error   = true;
    }
    load("observations.1.jsonl");
    load("observations.jsonl");
    if (p.expired && !compact_history())
        p.error = true;
    p.compacted = (double) time(nullptr);
    /* A torn tail stays untouched; seal it with a newline before a new append.
     * It remains invalid evidence, rather than corrupting the next valid record. */
    int tail = private_file("observations.jsonl", O_RDWR | O_APPEND);
    if (tail >= 0) {
        char  last;
        off_t size = lseek(tail, 0, SEEK_END);
        if (size > 0 && pread(tail, &last, 1, size - 1) == 1 && last != '\n')
            if (!write_all(tail, "\n", 1))
                p.error = true;
        close(tail);
    }
    int random = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (random >= 0) {
        if (read(random, &p.boot, sizeof p.boot) != sizeof p.boot)
            p.boot = 0;
        close(random);
    }
    if (!p.boot)
        p.boot = (uint64_t) time(nullptr) * 1000000 + (unsigned) getpid();
    p.started = pthread_create(&p.writer, nullptr, writer, nullptr) == 0;
    if (!p.started)
        p.error = true;
}
void perf_begin(struct perf_record *r) {
    r->schema = 2;
    pthread_mutex_lock(&p.mutex);
    r->epoch   = p.epoch;
    r->enabled = p.enabled;
    snprintf(r->id,
             sizeof r->id,
             "%016llx-%llu",
             (unsigned long long) p.boot,
             (unsigned long long) ++p.sequence);
    pthread_mutex_unlock(&p.mutex);
    r->timestamp = (double) time(nullptr);
    r->rss = r->peak_rss = r->cpu_percent = -1;
    app_memory_reset(&r->memory);
    r->first_answer_ns = r->first_ns = r->prefill_ns = r->load_ns = -1;
}
void perf_submit(const struct perf_record *r) {
    if (!valid(r))
        return;
    pthread_mutex_lock(&p.mutex);
    if (r->enabled && p.enabled && r->epoch == p.epoch &&
        r->timestamp >= (double) time(nullptr) - p.days * 86400. && !known(r->id)) {
        retain(r);
        if (p.started && p.queued < PERF_QUEUE) {
            p.queue[(p.head + p.queued) % PERF_QUEUE] = *r;
            ++p.queued;
            pthread_cond_signal(&p.wake);
        } else {
            p.error = true;
            ++p.dropped;
        }
    }
    pthread_mutex_unlock(&p.mutex);
}
void perf_import(struct perf_record *r) {
    pthread_mutex_lock(&p.mutex);
    r->enabled = p.enabled && !p.legacy_deleted;
    r->epoch   = p.epoch;
    pthread_mutex_unlock(&p.mutex);
    perf_submit(r);
}
bool perf_earlier(const char *artifact, const char *series, struct perf_earlier *out) {
    memset(out, 0, sizeof *out);
    bool found = false;
    pthread_mutex_lock(&p.mutex);
    for (size_t i = p.count; i > 0; i--) {
        const struct perf_record *r = &p.records[i - 1];
        if (strcmp(r->artifact, artifact) || !strcmp(r->series, series) || strcmp(r->outcome, "completed") ||
            r->output <= 0 || r->generation_ns <= 0 || r->warmup)
            continue;
        unsigned slot = strncmp(r->backend, "cpu", 3) ? 1 : 0;
        if (!out->rate[slot])
            out->rate[slot] = r->output / (r->generation_ns / 1e9);
        if (!found)
            snprintf(out->version, sizeof out->version, "%s", r->engine.version);
        found = true;
    }
    pthread_mutex_unlock(&p.mutex);
    return found;
}
void perf_last(const char         *artifact,
               const char         *series,
               const char         *backend,
               struct perf_record *out) {
    memset(out, 0, sizeof *out);
    pthread_mutex_lock(&p.mutex);
    for (size_t i = 0; i < p.count; i++) {
        const struct perf_record *r = &p.records[i];
        /* "Last reply" feeds slow/below-target warnings and setup: ordinary
         * use only, never a reply that overlapped a download or a comparison (#81). */
        if (eligible(r) && !r->contention && strcmp(r->source, "controlled_test") &&
            !strcmp(r->artifact, artifact) && !strcmp(r->series, series) && !strcmp(r->backend, backend))
            *out = *r;
    }
    pthread_mutex_unlock(&p.mutex);
}
static int compare(const void *a, const void *b) {
    double x = *(const double *) a, y = *(const double *) b;
    return (x > y) - (x < y);
}
static double percentile(double *values, size_t n, double q) {
    if (!n)
        return -1;
    qsort(values, n, sizeof *values, compare);
    double pos = (n - 1) * q;
    size_t i   = (size_t) pos;
    return values[i] + (values[i + (i + 1 < n)] - values[i]) * (pos - i);
}
static void group_json(struct app_buffer *b, const struct perf_record *g) {
    if (!g->id[0]) {
        app_put(b, "null");
        return;
    }
    app_printf(b,
               "{\"input\":%u,\"output\":%u,\"cached\":%s,\"cold\":%s,\"contention\":%s,"
               "\"controlled\":%s}",
               g->input <= 512 ? 0 : g->input <= 2048 ? 1 : 2,
               g->output < 32 ? 0 : g->output < 128 ? 1 : g->output < 512 ? 2 : 3,
               g->reused ? "true" : "false",
               g->cold ? "true" : "false",
               g->contention ? "true" : "false",
               !strcmp(g->source, "controlled_test") ? "true" : "false");
}
static void aggregate(struct app_buffer *b, const struct perf_record *rs, size_t n) {
    if (!n) {
        app_put(b, "null");
        return;
    }
    double values[30];
    app_printf(b, "{\"count\":%zu,\"backend\":", n);
    app_quote(b, rs[0].backend);
    app_put(b, ",\"engine\":");
    app_engine_json(b, &rs[0].engine);
#define AGG(name, expr)                          \
    do {                                         \
        size_t used = 0;                         \
        for (size_t i = 0; i < n; i++) {         \
            double v = (expr);                   \
            if (v >= 0)                          \
                values[used++] = v;              \
        }                                        \
        app_put(b, ",\"" name "\":");            \
        number(b, percentile(values, used, .5)); \
    } while (0)
    AGG("rate", rs[i].output / (rs[i].generation_ns / 1e9));
    for (size_t i = 0; i < n; i++)
        values[i] = rs[i].output / (rs[i].generation_ns / 1e9);
    app_put(b, ",\"q25\":");
    number(b, percentile(values, n, .25));
    app_put(b, ",\"q75\":");
    number(b, percentile(values, n, .75));
    AGG("first", rs[i].first_ns / 1e9);
    AGG("first_answer", rs[i].first_answer_ns / 1e9);
    size_t answer_count = 0;
    for (size_t i = 0; i < n; i++)
        if (rs[i].first_answer_ns >= 0)
            ++answer_count;
    app_printf(b, ",\"first_answer_count\":%zu", answer_count);
    AGG("total", rs[i].total_ns / 1e9);
    AGG("tokens", (double) rs[i].output);
    AGG("rss_bytes", rs[i].rss);
    AGG("sampled_peak_rss", rs[i].peak_rss);
    AGG("gpu_allocated_bytes", rs[i].memory.status == 1 ? rs[i].memory.gpu_end : -1);
    AGG("gpu_sampled_peak", rs[i].memory.gpu_samples ? rs[i].memory.gpu_peak : -1);
    unsigned gpu_count = 0;
    for (size_t i = 0; i < n; i++)
        if (rs[i].memory.status == 1)
            ++gpu_count;
    app_printf(b, ",\"gpu_known_count\":%u", gpu_count);
#undef AGG
    app_printf(b, ",\"recorded_at\":%.0f}", rs[0].timestamp);
}
void perf_view(struct app_buffer *b,
               const char        *artifact,
               const char        *series,
               const char        *cpu,
               const char        *gpu) {
    struct perf_record *samples = calloc(60, sizeof *samples), recent[12], group = {0};
    size_t              n[2] = {0}, nr = 0;
    if (!samples) {
        app_put(b, "null");
        return;
    }
    pthread_mutex_lock(&p.mutex);
    app_printf(b,
               "{\"enabled\":%s,\"days\":%u,\"error\":%s,\"dropped\":%llu,\"invalid\":%llu,"
               "\"persisted\":%llu,\"retained\":%zu,\"pending\":%zu,\"limit\":%u,\"artifact\":",
               p.enabled ? "true" : "false",
               p.days,
               p.error ? "true" : "false",
               (unsigned long long) p.dropped,
               (unsigned long long) p.invalid,
               (unsigned long long) p.persisted,
               p.count,
               p.queued,
               PERF_RECORDS);
    app_quote(b, artifact);
    /* Each processor's median comes from its own newest ordinary workload (#81):
     * a reply on one processor must never hide the other's samples. Replies that
     * overlapped a download are left out. A finished comparison is the one view
     * where both processors share a run and a workload, so it is shown as such
     * until the next ordinary reply. */
    struct perf_record ref[2] = {0};
    bool               decided = false, comparison = false;
    char               run[sizeof group.run] = "";
    for (size_t i = p.count; i > 0; i--) {
        const struct perf_record *r = &p.records[i - 1];
        if (strcmp(r->artifact, artifact))
            continue;
        if (nr < 12)
            recent[nr++] = *r;
        if (strcmp(r->series, series) || !eligible(r) || r->contention)
            continue;
        bool controlled = !strcmp(r->source, "controlled_test");
        if (!decided) {
            decided    = true;
            comparison = controlled;
            snprintf(run, sizeof run, "%s", r->run);
            group = *r; /* the engine in use now: older builds are historical */
        }
        if (comparison ? !controlled || strcmp(r->run, run) : controlled)
            continue;
        if (memcmp(&r->engine, &group.engine, sizeof r->engine))
            continue;
        unsigned slot = !strcmp(r->backend, cpu) ? 0 : !strcmp(r->backend, gpu) ? 1 : 2;
        if (slot == 2)
            continue;
        if (!ref[slot].id[0])
            ref[slot] = *r;
        if (perf_group(r) != perf_group(&ref[slot]))
            continue;
        if (n[slot] < 30)
            samples[slot * 30 + n[slot]++] = *r;
    }
    pthread_mutex_unlock(&p.mutex);
    app_put(b, ",\"group\":");
    group_json(b, &group);
    app_put(b, ",\"cpu_group\":");
    group_json(b, &ref[0]);
    app_put(b, ",\"gpu_group\":");
    group_json(b, &ref[1]);
    app_put(b, ",\"cpu\":");
    aggregate(b, samples, n[0]);
    app_put(b, ",\"gpu\":");
    aggregate(b, samples + 30, n[1]);
    app_put(b, ",\"recent\":[");
    for (size_t i = 0; i < nr; i++) {
        if (i)
            app_put(b, ",");
        const struct perf_record *r = &recent[i];
        app_put(b, "{\"id\":");
        app_quote(b, r->id);
        app_put(b, ",\"backend\":");
        app_quote(b, r->backend);
        app_put(b, ",\"source\":");
        app_quote(b, r->source);
        app_put(b, ",\"outcome\":");
        app_quote(b, r->outcome);
        app_put(b, ",\"engine\":");
        app_engine_json(b, &r->engine);
        app_printf(b,
                   ",\"timestamp\":%.0f,\"generation_ns\":%.0f,\"input\":%llu,\"output\":%llu,"
                   "\"warmup\":%s,\"contention\":%s,\"historical\":%s}",
                   r->timestamp,
                   r->generation_ns,
                   (unsigned long long) r->input,
                   (unsigned long long) r->output,
                   r->warmup ? "true" : "false",
                   r->contention ? "true" : "false",
                   (strcmp(r->series, series) ||
                    (group.id[0] && memcmp(&r->engine, &group.engine, sizeof r->engine)))
                           ? "true"
                           : "false");
    }
    app_put(b, "]}");
    free(samples);
}
static bool store_settings(bool enabled, unsigned days, bool deleted) {
    char text[128];
    int  n = snprintf(text,
                      sizeof text,
                      "{\"schema\":1,\"enabled\":%s,\"days\":%u,\"legacy_deleted\":%s}\n",
                      enabled ? "true" : "false",
                      days,
                      deleted ? "true" : "false");
    return atomic_file("settings.json", text, (size_t) n);
}
bool perf_settings(bool enabled, unsigned days) {
    if (days != 30 && days != 90 && days != 365)
        return false;
    pthread_mutex_lock(&p.disk);
    bool ok = store_settings(enabled, days, p.legacy_deleted);
    pthread_mutex_lock(&p.mutex);
    /* Opt-out takes effect immediately, even if the settings disk write fails. */
    if (ok || !enabled) {
        p.enabled = enabled;
        p.days    = days;
        ++p.epoch;
    }
    if (!ok)
        p.error = true;
    pthread_mutex_unlock(&p.mutex);
    if (ok)
        ok = compact_history();
    if (!ok) {
        pthread_mutex_lock(&p.mutex);
        p.error = true;
        pthread_mutex_unlock(&p.mutex);
    }
    pthread_mutex_unlock(&p.disk);
    return ok;
}
bool perf_clear(void) {
    pthread_mutex_lock(&p.disk);
    pthread_mutex_lock(&p.mutex);
    bool     enabled = p.enabled;
    unsigned days    = p.days;
    pthread_mutex_unlock(&p.mutex);
    bool        ok      = store_settings(enabled, days, true);
    const char *names[] = {
            "observations.jsonl", "observations.1.jsonl", "profiles.json", "export.jsonl"};
    for (size_t i = 0; i < 4; i++)
        if (unlinkat(p.dir, names[i], 0) && errno != ENOENT)
            ok = false;
    pthread_mutex_lock(&p.mutex);
    p.legacy_deleted = true;
    p.count = p.queued = p.head = 0;
    p.persisted = p.invalid = p.dropped = 0;
    ++p.epoch;
    p.error = !ok;
    pthread_mutex_unlock(&p.mutex);
    pthread_mutex_unlock(&p.disk);
    return ok;
}
char *perf_export(size_t *length) {
    struct perf_record *copy = malloc(sizeof p.records);
    if (!copy)
        return nullptr;
    pthread_mutex_lock(&p.mutex);
    size_t kept   = 0;
    double cutoff = (double) time(nullptr) - p.days * 86400.;
    for (size_t i = 0; i < p.count; i++)
        if (p.records[i].timestamp >= cutoff)
            p.records[kept++] = p.records[i];
    p.count      = kept;
    size_t count = p.count;
    memcpy(copy, p.records, count * sizeof *copy);
    pthread_mutex_unlock(&p.mutex);
    /* Every serialized record is capped; export has an independent 20 MiB cap. */
    size_t cap  = 2 * SEGMENT_BYTES;
    char  *data = malloc(cap);
    if (!data) {
        free(copy);
        return nullptr;
    }
    struct app_buffer b = {.data = data, .cap = cap};
    for (size_t i = 0; i < count; i++)
        record_json(&b, &copy[i]);
    free(copy);
    if (b.failed) {
        free(data);
        return nullptr;
    }
    *length = b.len;
    return data;
}
bool perf_save_export(void) {
    size_t length;
    char  *data = perf_export(&length);
    if (!data)
        return false;
    pthread_mutex_lock(&p.disk);
    bool ok = atomic_file("export.jsonl", data, length);
    pthread_mutex_unlock(&p.disk);
    free(data);
    return ok;
}
static bool compact_history(void) {
    size_t length;
    char  *data = perf_export(&length);
    if (!data)
        return false;
    size_t split = 0;
    if (length > SEGMENT_BYTES) {
        split = length - SEGMENT_BYTES;
        while (split < length && data[split - 1] != '\n')
            ++split;
        if (split > SEGMENT_BYTES) {
            free(data);
            return false;
        }
    }
    /* Previous first, current second: interrupted replacement leaves either the
     * old current or the new current recoverable. IDs deduplicate their overlap. */
    bool ok = atomic_file("observations.1.jsonl", data, split) &&
              atomic_file("observations.jsonl", data + split, length - split);
    free(data);
    if (ok)
        p.compacted = (double) time(nullptr);
    return ok;
}
void perf_close(void) {
    pthread_mutex_lock(&p.mutex);
    p.closing = true;
    pthread_cond_signal(&p.wake);
    pthread_mutex_unlock(&p.mutex);
    if (p.started)
        pthread_join(p.writer, nullptr);
    if (p.dir >= 0)
        close(p.dir);
}
