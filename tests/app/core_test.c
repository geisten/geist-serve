#include "../../src/app/core.h"
#include <assert.h>
#include <math.h>
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

    // Whole-device speed must not inherit the most recent (possibly slow) CPU reply.
    h.supported = true;
    h.device = APP_APPLE_SILICON;
    assert(app_assess_device(&h, gemma, true, 3, true, 45).fit == APP_RECOMMENDED);
    assert(app_assess_device(&h, gemma, true, 3, true, 0).fit == APP_RECOMMENDED);
    assert(app_assess_device(&h, gemma, true, 0, true, 3).fit == APP_RECOMMENDED);
    assert(app_assess_device(&h, gemma, true, 3, true, 7.99).fit == APP_CONDITIONAL);
    assert(app_assess_device(&h, gemma, true, 3, true, 8).fit == APP_RECOMMENDED);
    assert(app_assess_device(&h, gemma, true, 8, true, 0).fit == APP_RECOMMENDED);
    assert(app_assess_device(&h, gemma, true, 3, false, 45).fit == APP_CONDITIONAL);
    assert(app_assess_device(&h, gemma, true, 0, false, 0).fit == APP_RECOMMENDED);
    assert(!app_rate_below_target(NAN) && !app_rate_below_target(INFINITY));
    assert(!app_rate_below_target(-1) && !app_rate_below_target(0) && !app_rate_below_target(8));
    assert(app_device_rate(3, true, NAN) == 0 && app_device_rate(INFINITY, true, 3) == 0);
    h.device = APP_UNKNOWN;
    assert(app_assess_device(&h, gemma, true, 0, false, 0).fit == APP_RECOMMENDED);
    h.available_known = true;
    h.available = 1;
    assert(app_assess_device(&h, gemma, true, 30, true, 45).fit == APP_CONDITIONAL);
    h.available_known = false;
    h.disk_known = true;
    h.disk = 1;
    assert(app_assess_device(&h, gemma, false, 30, true, 45).fit == APP_UNAVAILABLE);
    const struct app_model *bonsai = app_model_find("bonsai2-27b-pq2");
    assert(bonsai && !bonsai->unsupported_format && bonsai->backends == 3);
    h.device = APP_APPLE_SILICON;
    h.ram = 32 * APP_GIB;
    h.available_known = true;
    h.available = 20 * APP_GIB;
    assert(app_assess_device(&h, bonsai, true, 3, true, 20).fit == APP_RECOMMENDED);
    h.available--;
    assert(app_assess_device(&h, bonsai, true, 3, true, 20).fit == APP_CONDITIONAL);
    h.available = 20 * APP_GIB;
    h.ram = 16 * APP_GIB;
    h.available = 14 * APP_GIB;
    assert(app_assess_device(&h, bonsai, true, 3, true, 20).fit == APP_CONDITIONAL);
    struct app_model unsupported = *bonsai;
    unsupported.backends = 0;
    unsupported.unsupported_format = "pq2_0";
    assert(app_assess_device(&h, &unsupported, true, 30, true, 45).fit == APP_UNAVAILABLE);

    struct app_inventory inventory[APP_MODEL_COUNT] = {};
    h                             = (struct app_hardware) {.supported       = true,
                                                           .ram             = 16 * APP_GIB,
                                                           .available       = 8 * APP_GIB,
                                                           .available_known = true,
                                                           .disk_known      = true,
                                                           .disk            = 20 * APP_GIB,
                                                           .cores           = 8,
                                                           .device          = APP_APPLE_SILICON};
    struct app_recommendation rec = app_recommend(&h, inventory, "", "");
    assert(rec.eligible && rec.model == gemma && !strcmp(rec.source, "default"));
    h.available = APP_GIB;
    rec         = app_recommend(&h, inventory, "", "");
    assert(rec.eligible && !strcmp(rec.model->id, "smollm2-360m") &&
           !strcmp(rec.source, "fallback"));
    h.available = 767 * UINT64_C(1048576);
    assert(!app_recommend(&h, inventory, "", "").eligible);
    h.available = 8 * APP_GIB;
    h.cores     = 2;
    assert(!strcmp(app_recommend(&h, inventory, "", "").source, "fallback"));
    h.cores                           = 4;
    inventory[gemma - app_models].tps = 5;
    assert(!strcmp(app_recommend(&h, inventory, "", "").source, "fallback"));
    inventory[gemma - app_models].tps = 0;
    inventory[gemma - app_models].tps = app_device_rate(3, true, 0);
    assert(app_recommend(&h, inventory, "", "").model == gemma);
    inventory[gemma - app_models].tps = app_device_rate(3, true, 45);
    assert(app_recommend(&h, inventory, "", "").model == gemma);
    assert(!app_recommend(&h, inventory, bonsai->id, "").eligible);
    inventory[gemma - app_models].tps = 0;
    h.device                          = APP_PI5;
    h.ram                             = 4 * APP_GIB;
    h.available                       = 3 * APP_GIB;
    assert(app_recommend(&h, inventory, "", "").model == bitnet);
    h.device                      = APP_UNKNOWN;
    const struct app_model *small = app_model_find("smollm2-360m");
    assert(app_recommend(&h, inventory, "", "").model == small);
    h.disk = small->bytes + 256 * UINT64_C(1048576) - 1;
    assert(!app_recommend(&h, inventory, "", "").eligible);
    inventory[small - app_models].partial = 100;
    assert(app_recommend(&h, inventory, "", "").eligible);
    inventory[small - app_models].partial = UINT64_MAX;
    assert(!app_recommend(&h, inventory, "", "").eligible);
    inventory[small - app_models].installed = true;
    assert(app_recommend(&h, inventory, "", "").eligible);
    h.disk_known = false;
    assert(!app_recommend(&h, inventory, "", "").eligible);
    h.disk_known      = true;
    h.available_known = false;
    assert(!app_recommend(&h, inventory, "", "").eligible);
    h.available_known = true;
    h.supported       = false;
    assert(!app_recommend(&h, inventory, "", "").eligible);
    h.supported = true;
    h.disk      = 20 * APP_GIB;
    // Saved choices never silently switch, even when the current budget fails.
    rec = app_recommend(&h, inventory, gemma->id, "");
    assert(!rec.eligible && rec.model == gemma && !strcmp(rec.source, "saved"));
    h.ram       = 16 * APP_GIB;
    h.available = 100;
    rec         = app_recommend(&h, inventory, small->id, small->id);
    assert(rec.eligible && rec.model == small); // already loaded memory is not charged twice

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
