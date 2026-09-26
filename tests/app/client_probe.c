#define GEISTD_CLIENT_IMPLEMENTATION
#include "../../clients/geistd_client.h"
static bool emit(void *ctx, const char *s) { (void)ctx; fputs(s, stdout); return true; }
static bool cancel(void *ctx) { (void)ctx; return true; }
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    struct geistd *g = geistd_connect_unix(argv[1], nullptr);
    if (!g) return 2;
    geistd_limits(g, 150, strcmp(argv[2], "cancel") == 0 ? cancel : nullptr, nullptr);
    int rc;
    if (strcmp(argv[2], "generate") == 0) {
        char reason[16]; struct geistd_generation s;
        rc = geistd_generate_ex(g, "0123456789abcdef", 10, emit, nullptr, reason, &s);
        if (!rc) printf("\n%zu %.0f %s", s.tokens, s.duration_ns, reason);
    } else { char info[100]; rc = geistd_info(g, sizeof info, info); if (!rc) puts(info); }
    geistd_close(g);
    return rc ? 1 : 0;
}
