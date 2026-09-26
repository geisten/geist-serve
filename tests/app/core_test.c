#include "../../src/app/core.h"
#include <assert.h>
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool cancel_hash(void) {
    return true;
}

int main(void) {
    struct app_utf8 utf8 = {};
    char decoded[64];
    assert(app_utf8_feed(&utf8, "Gr\xc3", decoded, sizeof decoded) && !strcmp(decoded, "Gr"));
    assert(app_utf8_feed(&utf8, "\xbc\xc3", decoded, sizeof decoded) && !strcmp(decoded, "\xc3\xbc"));
    assert(app_utf8_feed(&utf8, "\x9f" "e \xf0\x9f", decoded, sizeof decoded) && !strcmp(decoded, "\xc3\x9f" "e "));
    assert(app_utf8_feed(&utf8, "\x8c\xb1", decoded, sizeof decoded) && !strcmp(decoded, "\xf0\x9f\x8c\xb1"));
    assert(!utf8.used);
    const char *invalid[] = {"\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\x80"};
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        struct app_utf8 bad = {};
        assert(!app_utf8_feed(&bad, invalid[i], decoded, sizeof decoded) && bad.failed);
        assert(!app_utf8_feed(&bad, "ok", decoded, sizeof decoded));
    }
    alignas(max_align_t) unsigned char storage[64];
    struct app_arena                   a = {.base = storage, .cap = sizeof storage};
    assert(app_alloc(&a, 1, 3, 1));
    void *p = app_alloc(&a, 2, sizeof(uint64_t), alignof(uint64_t));
    assert(p && (uintptr_t) p % alignof(uint64_t) == 0);
    size_t used = a.used;
    assert(!app_alloc(&a, SIZE_MAX, 2, 1) && a.used == used);
    assert(!app_alloc(&a, 1, 64, 1) && a.used == used);
    assert(!app_alloc(&a, 1, 1, 3));
    char              output[16] = "";
    struct app_buffer b          = {.data = output, .cap = sizeof output};
    app_quote(&b, "a\n\"b");
    assert(!b.failed && strcmp(output, "\"a\\u000a\\\"b\"") == 0);
    app_put(&b, "too much data");
    assert(b.failed);
    app_put(&b, "x");
    assert(b.failed); /* failure cannot silently recover */

    struct app_hardware     h      = {.supported       = true,
                                      .ram             = 4 * APP_GIB,
                                      .available       = 3 * APP_GIB,
                                      .available_known = true,
                                      .disk_known      = true,
                                      .disk            = 20 * APP_GIB,
                                      .cores           = 4,
                                      .device          = APP_PI5};
    const struct app_model *bitnet = app_model_find("bitnet-2b");
    const struct app_model *gemma  = app_model_find("gemma4-e2b");
    assert(bitnet && gemma && !app_model_find("../../bad"));
    assert(app_assess(&h, bitnet, false).fit == APP_RECOMMENDED);
    assert(app_assess(&h, gemma, false).fit == APP_CONDITIONAL);
    h.available = APP_GIB;
    assert(app_assess(&h, bitnet, false).fit == APP_CONDITIONAL);
    h.disk = 10;
    assert(app_assess(&h, bitnet, false).fit == APP_UNAVAILABLE);
    assert(app_assess(&h, bitnet, true).fit == APP_CONDITIONAL);
    h.disk_known      = false;
    h.available_known = false;
    h.device          = APP_UNKNOWN;
    assert(app_assess(&h, bitnet, false).fit == APP_CONDITIONAL);
    h.ram = APP_GIB;
    assert(app_assess(&h, bitnet, false).fit == APP_UNAVAILABLE);
    h.ram    = 16 * APP_GIB;
    h.device = APP_APPLE_SILICON;
    assert(app_assess(&h, gemma, false).fit == APP_RECOMMENDED);
    assert(app_assess_observed(&h, gemma, false, 3).fit == APP_CONDITIONAL);
    h.device = APP_UNKNOWN;
    assert(app_assess_observed(&h, gemma, false, 15).fit == APP_RECOMMENDED);
    h.supported = false;
    assert(app_assess(&h, gemma, true).fit == APP_UNAVAILABLE);

    char path[] = "/tmp/geist-sha-XXXXXX";
    int  fd     = mkstemp(path);
    assert(fd >= 0 && write(fd, "abc", 3) == 3);
    close(fd);
    char hash[65];
    assert(!app_sha256_interruptible(path, hash, cancel_hash));
    assert(access(path, F_OK) == 0);
    assert(app_sha256(path, hash));
    assert(strcmp(hash, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
    unlink(path);
    puts("app core: arena limits, sticky errors, model decisions and SHA-256 passed");
}
