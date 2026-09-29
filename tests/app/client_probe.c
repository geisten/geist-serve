#define GEISTD_CLIENT_IMPLEMENTATION
#include "../../clients/geistd_client.h"
static bool emit(void *ctx, const char *s) { (void)ctx; fputs(s, stdout); return true; }
static bool cancel(void *ctx) { (void)ctx; return true; }
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    struct geistd *g = geistd_connect_unix(argv[1], nullptr);
    if (!g) return 2;
    geistd_limits(g, strncmp(argv[2], "generate", 8) == 0 ? 600 : 150, strcmp(argv[2], "cancel") == 0 ? cancel : nullptr, nullptr);
    struct timespec started, finished;
    clock_gettime(CLOCK_MONOTONIC, &started);
    int rc;
    if (strncmp(argv[2], "generate", 8) == 0) {
        geistd_stream_idle(g, !strcmp(argv[2], "generate"));
        char reason[16]; struct geistd_generation s;
        rc = geistd_generate_ex(g, "0123456789abcdef", 10, emit, nullptr, reason, &s);
        if (!rc) printf("\n%zu %.0f %s", s.tokens, s.duration_ns, reason);
    } else { char info[100]; rc = geistd_info(g, sizeof info, info); if (!rc) puts(info); }
    clock_gettime(CLOCK_MONOTONIC, &finished);
    double operation_ms = (finished.tv_sec - started.tv_sec) * 1000.0 +
                          (finished.tv_nsec - started.tv_nsec) / 1e6;
    fprintf(stderr, "operation_ms=%.3f\n", operation_ms);
    if (rc) fprintf(stderr, "%s\n", geistd_error(g));
    geistd_close(g);
    return rc ? 1 : 0;
}
