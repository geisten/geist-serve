/* test_net_signal.c — SIGTERM must end the accept loop whichever thread the
 * kernel delivers it to (#69). geist-serve and geistd run OpenMP workers; a
 * process-directed SIGTERM that lands on one of them used to set net_stop
 * without interrupting accept() in the main thread, so the server hung until
 * the next connection. Here a helper thread plays the OpenMP worker and gets
 * the signal directly; then a signal arrives before the wait begins. No engine, no model. alarm() fails a hang in 5 s. */
#include "../src/net.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static pthread_t worker;

static void *idle(void *unused) {
    (void) unused;
    for (;;)
        pause();
    return nullptr;
}

static void *signal_worker(void *unused) {
    (void) unused;
    nanosleep(&(struct timespec) {.tv_nsec = 200000000}, nullptr); /* let main enter accept() */
    pthread_kill(worker, SIGTERM);
    return nullptr;
}

int main(void) {
    alarm(5); /* default SIGALRM action kills the process: a hang fails loudly */
    net_install_signals();
    int lfd = net_bind_tcp("127.0.0.1", 0);
    if (lfd < 0)
        return 2;
    pthread_t killer;
    pthread_create(&worker, nullptr, idle, nullptr);
    pthread_create(&killer, nullptr, signal_worker, nullptr);
    for (int fd; (fd = net_accept(lfd)) >= 0;)
        close(fd);
    if (!net_stop)
        return 3;
    /* A signal that lands before the wait begins must not be lost either. */
    net_stop = 0;
    raise(SIGTERM);
    if (net_accept(lfd) != -1 || !net_stop)
        return 4;
    printf("test_net_signal: SIGTERM ends the accept loop from any thread and before the wait\n");
    return 0;
}
