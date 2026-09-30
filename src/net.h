/* net.h — listener acquisition shared by geist-serve and geistd: an
 * inherited socket (systemd LISTEN_FDS, inetd wait-mode), a TCP bind, or
 * a Unix socket; plus the stop flag SIGTERM/SIGINT set. */
#pragma once

#include <signal.h>
#include <stdbool.h>

extern volatile sig_atomic_t net_stop; /* set by SIGTERM / SIGINT */

void net_install_signals(void); /* SIGPIPE ignored; SIGTERM/SIGINT set net_stop */
int  net_accept(int lfd);        /* next client fd, or -1 once net_stop is set (seen within 250 ms) or on error */
bool net_is_listener(int fd);
int  net_inherited_listener(void);             /* fd or -1 */
int  net_bind_tcp(const char *host, int port); /* fd or -1 (message on stderr) */
int  net_bind_unix(const char *path);          /* fd or -1; mode 0600 */
