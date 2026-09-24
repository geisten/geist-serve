/* net.c — see net.h. */
#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

volatile sig_atomic_t net_stop = 0;

static void on_stop(int sig) {
    (void) sig;
    net_stop = 1;
}

void net_install_signals(void) {
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa = {.sa_handler = on_stop};
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
}

/* SO_ACCEPTCONN is unreliable on macOS, so probe the way that works
 * everywhere: listen() on a listening socket succeeds (it only resets the
 * backlog); on a pipe, tty or connected socket it fails. */
bool net_is_listener(int fd) {
    struct sockaddr_storage ss;
    socklen_t               l = sizeof ss;
    return getsockname(fd, (struct sockaddr *) &ss, &l) == 0 && listen(fd, 16) == 0;
}

/* systemd passes listeners starting at fd 3; inetd wait-mode passes the
 * listener as fd 0. Both are "use as-is". */
int net_inherited_listener(void) {
    const char *n = getenv("LISTEN_FDS");
    if (n != nullptr && atoi(n) >= 1 && net_is_listener(3))
        return 3;
    if (net_is_listener(0))
        return 0;
    return -1;
}

int net_bind_tcp(const char *host, int port) {
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t) port)};
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        fprintf(stderr, "--host %s: not an IPv4 address\n", host);
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(fd, (struct sockaddr *) &sa, sizeof sa) != 0 || listen(fd, 16) != 0) {
        fprintf(stderr, "bind %s:%d: %s\n", host, port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* A stale socket file from a crashed run is removed first; the file mode
 * is the local access control. */
int net_bind_unix(const char *path) {
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    if (strlen(path) >= sizeof sa.sun_path) {
        fprintf(stderr, "socket path too long: %s\n", path);
        return -1;
    }
    strcpy(sa.sun_path, path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    unlink(path);
    mode_t old = umask(0177);
    int    rc  = bind(fd, (struct sockaddr *) &sa, sizeof sa);
    umask(old);
    if (rc != 0 || listen(fd, 16) != 0) {
        fprintf(stderr, "bind %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }
    chmod(path, 0600);
    return fd;
}
