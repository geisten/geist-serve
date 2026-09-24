/* geistd_client_test.c — the C client against a running daemon: open,
 * tokenize, prefill, peek two candidates, feed the winner, step, generate.
 *   cc -std=c23 -I clients -I src tests/geistd_client_test.c -o build/geistd_client_test && build/geistd_client_test <socket> */
#define GEISTD_CLIENT_IMPLEMENTATION
#include "geistd_client.h"

#include <stdio.h>
#include <string.h>

static bool emit(void *ctx, const char *piece) {
    (void) ctx;
    fputs(piece, stdout);
    return true;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    struct geistd *g = geistd_connect_unix(argv[1], nullptr);
    char info[2048];
    if (geistd_info(g, sizeof info, info) != 0) return fprintf(stderr, "info: %s\n", geistd_error(g)), 1;
    printf("info ok: %.60s...\n", info);
    char id[17];
    if (geistd_open(g, 0, 1, 0, 0, id) != 0) return fprintf(stderr, "open: %s\n", geistd_error(g)), 1;
    int32_t ids[512], cands[2];
    size_t  n, k;
    const char *prompt = "<|im_start|>user\nWhat is the capital of France? One word.<|im_end|>\n<|im_start|>assistant\n";
    if (geistd_tokenize(g, prompt, 512, ids, &n) != 0) return fprintf(stderr, "tokenize: %s\n", geistd_error(g)), 1;
    size_t pre, re;
    if (geistd_prefill(g, id, n, ids, &pre, &re) != 0) return fprintf(stderr, "prefill: %s\n", geistd_error(g)), 1;
    printf("prefill: %zu tokens, prefilled %zu reused %zu\n", n, pre, re);
    geistd_tokenize(g, "Paris", 1, &cands[0], &k);
    geistd_tokenize(g, "London", 1, &cands[1], &k);
    float lp[2];
    if (geistd_peek_logprobs(g, id, 2, cands, lp) != 0) return fprintf(stderr, "peek: %s\n", geistd_error(g)), 1;
    printf("logprobs: Paris %.3f London %.3f\n", (double) lp[0], (double) lp[1]);
    if (lp[0] <= lp[1]) return fprintf(stderr, "FAIL: London beat Paris\n"), 1;
    ids[n++] = cands[0];
    geistd_prefill(g, id, n, ids, &pre, &re);
    printf("fed winner: prefilled %zu reused %zu\n", pre, re);
    int32_t t;
    bool    stop;
    if (geistd_step(g, id, &t, &stop) != 0) return fprintf(stderr, "step: %s\n", geistd_error(g)), 1;
    printf("step: token %d stop %d\n", t, stop);
    char reason[16];
    printf("generate: ");
    if (geistd_generate(g, id, 8, emit, nullptr, reason) != 0) return fprintf(stderr, "generate: %s\n", geistd_error(g)), 1;
    printf("\n  reason %s\n", reason);
    geistd_close_session(g, id);
    geistd_close(g);
    printf("geistd_client_test: all passed\n");
    return 0;
}
