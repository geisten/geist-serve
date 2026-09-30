/* request.c — the app's own HTTP: bounded request read with a deadline, responses, token auth, the loopback listener. */
#include "app.h"

bool send_bytes(int fd, const void *data, size_t n) {
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

bool response(int fd, int status, const char *type, const void *body, size_t n) {
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

void error_response(int fd, int code, const char *message) {
    char              body[1024];
    struct app_buffer b = {.data = body, .cap = sizeof body};
    app_put(&b, "{\"error\":");
    app_quote(&b, message);
    app_put(&b, "}");
    response(fd, code, "application/json", body, b.len);
}


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

int read_request(int fd, struct app_arena *arena, struct request *r) {
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

bool authorized(const struct request *r) {
    char expected[80];
    snprintf(expected, sizeof expected, "Bearer %s", app.token);
    if (strlen(r->auth) != strlen(expected))
        return false;
    unsigned mismatch = 0;
    for (size_t i = 0; expected[i]; ++i)
        mismatch |= (unsigned char) r->auth[i] ^ (unsigned char) expected[i];
    return mismatch == 0;
}

int listener(unsigned *port) {
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
