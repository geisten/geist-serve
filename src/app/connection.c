#include "connection.h"
#include "../json.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

bool app_home(char out[static APP_PATH_CAP]) {
    const char *home = getenv("GEIST_HOME"), *user = getenv("HOME");
    int         n;
    if (home)
        n = snprintf(out, APP_PATH_CAP, "%s", home);
    else if (!user)
        return false;
#ifdef __APPLE__
    else
        n = snprintf(out, APP_PATH_CAP, "%s/Library/Application Support/Geist", user);
#else
    else if (getenv("XDG_DATA_HOME"))
        n = snprintf(out, APP_PATH_CAP, "%s/geist", getenv("XDG_DATA_HOME"));
    else
        n = snprintf(out, APP_PATH_CAP, "%s/.local/share/geist", user);
#endif
    return n > 0 && n < APP_PATH_CAP;
}

static bool path(char out[static APP_PATH_CAP], const char *home, const char *name) {
    int n = snprintf(out, APP_PATH_CAP, "%s/%s", home, name);
    return n > 0 && n < APP_PATH_CAP;
}
static bool private_file(int fd) {
    struct stat s;
    return fstat(fd, &s) == 0 && S_ISREG(s.st_mode) && s.st_uid == getuid() && !(s.st_mode & 077);
}
static bool key_valid(const char *key) {
    if (strlen(key) != 64)
        return false;
    for (size_t i = 0; i < 64; i++)
        if (!strchr("0123456789abcdef", key[i]))
            return false;
    return true;
}
bool app_key(const char *home, char token[static 65]) {
    char name[APP_PATH_CAP];
    if (!path(name, home, "api-key"))
        return false;
    int fd = open(name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0) {
        char    bytes[66] = {0};
        ssize_t n         = private_file(fd) ? read(fd, bytes, 65) : -1;
        close(fd);
        if (n != 64 || !key_valid(bytes))
            return false;
        memcpy(token, bytes, 65);
        return true;
    }
    if (errno != ENOENT)
        return false;
    fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    unsigned char bytes[32];
    bool          ok = fd >= 0 && read(fd, bytes, sizeof bytes) == sizeof bytes;
    if (fd >= 0)
        close(fd);
    if (!ok)
        return false;
    for (size_t i = 0; i < sizeof bytes; i++)
        snprintf(token + i * 2, 3, "%02x", bytes[i]);
    fd = open(name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    ok = write(fd, token, 64) == 64 && fsync(fd) == 0;
    if (close(fd) != 0)
        ok = false;
    if (!ok)
        unlink(name);
    return ok;
}
bool app_connection_write(const char *home, unsigned port, const char *token) {
    char dest[APP_PATH_CAP], tmp[APP_PATH_CAP], data[512];
    if (!path(dest, home, "connection.json") || !path(tmp, home, ".connection.XXXXXX"))
        return false;
    int fd = mkstemp(tmp);
    if (fd < 0)
        return false;
    int  n  = snprintf(data,
                       sizeof data,
                       "{\"version\":1,\"port\":%u,\"api_key\":\"%s\",\"pid\":%ld}\n",
                       port,
                       token,
                       (long) getpid());
    bool ok = fchmod(fd, 0600) == 0 && n > 0 && (size_t) n < sizeof data &&
              write(fd, data, (size_t) n) == n && fsync(fd) == 0;
    if (close(fd) != 0)
        ok = false;
    if (ok)
        ok = rename(tmp, dest) == 0;
    if (!ok)
        unlink(tmp);
    return ok;
}
bool app_connection_read(const char *home, unsigned *port, char token[static 65]) {
    char name[APP_PATH_CAP], data[512];
    if (!path(name, home, "connection.json"))
        return false;
    int fd = open(name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return false;
    ssize_t n = private_file(fd) ? read(fd, data, sizeof data - 1) : -1;
    close(fd);
    if (n <= 0 || n == (ssize_t) sizeof data - 1)
        return false;
    data[n]        = 0;
    struct json *j = calloc(1, sizeof *j);
    if (!j)
        return false;
    bool ok = false;
    if (json_parse(j, (size_t) n, data) >= 0) {
        double p   = json_num(j, json_get(j, 0, "port"), 0);
        char  *key = json_strdup(j, json_get(j, 0, "api_key"));
        if (p >= 1 && p <= 65535 && p == (unsigned) p && key && key_valid(key)) {
            *port = (unsigned) p;
            memcpy(token, key, 65);
            ok = true;
        }
        free(key);
    }
    free(j);
    return ok;
}
void app_connection_remove(const char *home) {
    char name[APP_PATH_CAP];
    if (path(name, home, "connection.json"))
        unlink(name);
}
