/* jobs.c — the one model job: download (curl), verify, activate. */
#include "app.h"

void model_inventory(struct app_inventory items[static APP_MODEL_COUNT]) {
    for (size_t i = 0; i < app_model_count; ++i) {
        char path[APP_PATH_CAP], part[APP_PATH_CAP];
        bool valid = path_join(path, app.paths.models, app_models[i].file);
        items[i]   = (struct app_inventory) {.installed = valid &&
                                                          regular_size(path) == app_models[i].bytes,
                                             .tps = app_device_rate(app.prefs.history[i][0].rate,
                                                                    gpu_supported(&app_models[i]),
                                                                    app.prefs.history[i][1].rate)};
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
    app.job.received = s->bytes;
    activity_progress(app.job.activate ? &app.activity.load : &app.activity.download,
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
bool model_stamp(const char *path, const struct app_model *m, char out[static 512]) {
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
    const struct app_model *m                    = app.job.model;
    char                    target[APP_PATH_CAP] = "", part[APP_PATH_CAP], hash[65], why[512] = "";
    bool                    ok = path_join(target, app.paths.models, m->file);
    int                     n  = snprintf(part, sizeof part, "%s.part", target);
    ok                         = ok && n > 0 && (size_t) n < sizeof part;
    if (ok && app.job.download)
        ok = download_model(m, part, why, sizeof why);
    const char *verify = app.job.download ? part : target;
    char        key[80], before[512] = "", after[512] = "", receipt[512] = "";
    snprintf(key, sizeof key, "verified-%s", m->sha256);
    bool stamped = ok && model_stamp(verify, m, before);
    bool cached  = !app.job.download && stamped && read_preference(key, receipt, sizeof receipt) &&
                   !strcmp(before, receipt);
    pthread_mutex_lock(&app.mutex);
    if (app.job.activate) {
        app.job.receipt_checked = true;
        app.job.receipt_hit     = cached;
        app.job.verified_bytes  = 0;
    }
    pthread_mutex_unlock(&app.mutex);
    if (ok && !atomic_load(&cancelled) && !atomic_load(&closing)) {
        if (!cached) {
            pthread_mutex_lock(&app.mutex);
            strcpy(app.job.phase, "verifying");
            activity_change(app.job.activate ? &app.activity.load : &app.activity.download,
                            ACT_HASH);
            pthread_mutex_unlock(&app.mutex);
#ifdef APP_TESTING
            fprintf(stderr, "model verification: hashing %s\n", m->id);
#endif
        }
        bool size_ok = stamped;
        bool hash_ok = cached || (size_ok && app_sha256_interruptible(verify, hash, job_cancelled));
        pthread_mutex_lock(&app.mutex);
        if (app.job.activate && !cached && hash_ok)
            app.job.verified_bytes = m->bytes;
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
        if (ok && app.job.download) {
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
        if (!app.job.activate)
            snprintf(app.message, sizeof app.message, "Download complete.");
        else if (start_child(target, m->id))
            (void) save_selection(m->id);
    } else
        snprintf(app.message, sizeof app.message, "%s", *why ? why : "Cannot prepare the model.");
    struct activity *job_activity = app.job.activate ? &app.activity.load : &app.activity.download;
    if (atomic_load(&cancelled) || atomic_load(&closing))
        (void) activity_end(job_activity, "cancelled", 499, monotonic_ms());
    else if (!ok || (app.job.activate && !app.child.pid))
        (void) activity_end(job_activity, "failed", 502, monotonic_ms());
    else if (!app.job.activate)
        (void) activity_end(job_activity, "completed", 0, monotonic_ms());
    app.job.running = false;
    app.job.phase[0]    = 0;
    pthread_mutex_unlock(&app.mutex);
    return nullptr;
}

bool begin_job(const struct app_model *m, bool download, bool activate) {
    /* Startup also enters here from the saved selection, without an HTTP
     * assessment. Never hash/download/start a known unsupported format. */
    if (m->unsupported_format || !m->backends) {
        snprintf(app.message,
                 sizeof app.message,
                 "This model requires PQ2_0 and Hadamard support, unavailable in the bundled "
                 "engine.");
        return false;
    }
    if (app.job.joinable) {
        pthread_join(app.job.thread, nullptr);
        app.job.joinable = false;
    }
    if (activate) {
        app.job.receipt_checked = false;
        app.job.receipt_hit     = false;
        app.job.verified_bytes  = 0;
    }
    begin_activity(activate ? &app.activity.load : &app.activity.download,
                   download ? ACT_DOWNLOAD : ACT_RECEIPT,
                   activate ? app.generation + 1 : app.generation,
                   m->id);
    if (activate)
        atomic_store(&load_cancelled, false);
    app.job.model    = m;
    app.job.download = download;
    /* Captured under the mutex. A background download must never replace the
     * resident daemon, even if it becomes idle or exits before completion. */
    app.job.activate = activate;
    app.job.running  = true;
    if (download && app.observe.record)
        app.observe.record->contention = true;
    app.job.received   = 0;
    app.message[0] = 0;
    strcpy(app.job.phase, download ? "downloading" : "preparing");
    atomic_store(&cancelled, false);
    if (pthread_create(&app.job.thread, nullptr, model_job, nullptr) != 0) {
        app.job.running = false;
        app.job.phase[0]    = 0;
        (void) activity_end(activate ? &app.activity.load : &app.activity.download,
                            "failed",
                            503,
                            monotonic_ms());
        strcpy(app.message, "Cannot start model worker.");
        return false;
    }
    app.job.joinable = true;
    return true;
}
