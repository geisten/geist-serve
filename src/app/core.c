#include "core.h"
#include <stdalign.h>
#include <stdarg.h>
#include <stdckdint.h>
#include <stdio.h>
#include <string.h>

void *app_alloc(struct app_arena *a, size_t count, size_t size, size_t alignment) {
    size_t n, start, end;
    if (alignment == 0 || (alignment & (alignment - 1)) != 0 || alignment > alignof(max_align_t) ||
        ckd_mul(&n, count, size) || ckd_add(&start, a->used, alignment - 1))
        return nullptr;
    start &= ~(alignment - 1);
    if (ckd_add(&end, start, n) || end > a->cap)
        return nullptr;
    void *p = a->base + start;
    a->used = end;
    memset(p, 0, n);
    return p;
}

void app_put(struct app_buffer *b, const char *s) {
    size_t n = strlen(s), end;
    if (b->failed || ckd_add(&end, b->len, n) || end >= b->cap) {
        b->failed = true;
        return;
    }
    memcpy(b->data + b->len, s, n + 1);
    b->len = end;
}

void app_printf(struct app_buffer *b, const char *format, ...) {
    if (b->failed || b->len >= b->cap) {
        b->failed = true;
        return;
    }
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(b->data + b->len, b->cap - b->len, format, ap);
    va_end(ap);
    if (n < 0 || (size_t) n >= b->cap - b->len)
        b->failed = true;
    else
        b->len += (size_t) n;
}

void app_quote(struct app_buffer *b, const char *s) {
    app_put(b, "\"");
    for (const unsigned char *p = (const unsigned char *) s; *p; ++p) {
        if (*p == '"' || *p == '\\')
            app_printf(b, "\\%c", *p);
        else if (*p < 32)
            app_printf(b, "\\u%04x", *p);
        else {
            char one[] = {(char) *p, 0};
            app_put(b, one);
        }
    }
    app_put(b, "\"");
}

bool app_utf8_feed(struct app_utf8 *s, const char *piece, char *out, size_t cap) {
    size_t written = 0;
    if (!cap || s->failed)
        return false;
    out[0] = 0;
    for (const unsigned char *p = (const unsigned char *) piece; *p; ++p) {
        unsigned char c = *p;
        if (!s->used) {
            s->need = c < 0x80 ? 1 : c >= 0xc2 && c <= 0xdf ? 2 :
                      c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
            if (!s->need)
                goto invalid;
        } else if (c < 0x80 || c > 0xbf ||
                   (s->used == 1 && ((s->bytes[0] == 0xe0 && c < 0xa0) ||
                                    (s->bytes[0] == 0xed && c > 0x9f) ||
                                    (s->bytes[0] == 0xf0 && c < 0x90) ||
                                    (s->bytes[0] == 0xf4 && c > 0x8f))))
            goto invalid;
        s->bytes[s->used++] = c;
        if (s->used == s->need) {
            if (cap - written <= s->used)
                goto invalid;
            memcpy(out + written, s->bytes, s->used);
            written += s->used;
            s->used = s->need = 0;
        }
    }
    out[written] = 0;
    return true;
invalid:
    s->failed = true;
    return false;
}

/* SHA pins and sizes match the existing Mac catalog. Planning memory is
 * deliberately separate from GGUF size and is not a measured RSS claim.
 * Keep this catalog in application code, never in geistlib. */
const struct app_model app_models[APP_MODEL_COUNT] = {
        {"bitnet-2b",
         "BitNet b1.58 2B",
         "bitnet-b1.58-2B-4T-i2_s.gguf",
         "https://huggingface.co/microsoft/bitnet-b1.58-2B-4T-gguf/resolve/main/"
         "ggml-model-i2_s.gguf",
         "4221b252fdd5fd25e15847adfeb5ee88886506ba50b8a34548374492884c2162",
         1187801280,
         2304,
         4},
        {"smollm2-360m",
         "SmolLM2 360M",
         "smollm2-360m-instruct-q8_0.gguf",
         "https://huggingface.co/HuggingFaceTB/SmolLM2-360M-Instruct-GGUF/resolve/main/"
         "smollm2-360m-instruct-q8_0.gguf",
         "48ab3034d0dd401fbc721eb1df3217902fee7dab9078992d66431f09b7750201",
         386404992,
         768,
         2},
        {"qwen3-0.6b",
         "Qwen3 0.6B",
         "Qwen3-0.6B-Q8_0.gguf",
         "https://huggingface.co/Qwen/Qwen3-0.6B-GGUF/resolve/main/Qwen3-0.6B-Q8_0.gguf",
         "9465e63a22add5354d9bb4b99e90117043c7124007664907259bd16d043bb031",
         639446688,
         1280,
         4},
        {"qwen35-0.8b",
         "Qwen3.5 0.8B",
         "Qwen3.5-0.8B-Q8_0.gguf",
         "https://huggingface.co/unsloth/Qwen3.5-0.8B-GGUF/resolve/main/Qwen3.5-0.8B-Q8_0.gguf",
         "0ad885ffd4bb022fc4f0d33a3308fa108ef8613159d3b3a67e23abca056b7a6c",
         811843840,
         2048,
         4},
        {"gemma4-e2b",
         "Gemma 4 E2B",
         "gemma-4-E2B-it-Q4_K_M.gguf",
         "https://huggingface.co/unsloth/gemma-4-E2B-it-GGUF/resolve/main/"
         "gemma-4-E2B-it-Q4_K_M.gguf",
         "740185b21d22ceb83a11c3aa62ad5842ef32c70f6096d756bbee85a1e4ec34b8",
         3106738272,
         5120,
         8},
        {"gemma4-e4b",
         "Gemma 4 E4B",
         "gemma-4-E4B-it-Q4_K_M.gguf",
         "https://huggingface.co/unsloth/gemma-4-E4B-it-GGUF/resolve/main/"
         "gemma-4-E4B-it-Q4_K_M.gguf",
         "85a896a047553e842f25297ee5b031d64ff30147d9c4af17b1e4b394cd1fab87",
         4977171584,
         8192,
         16},
};

const struct app_model *app_model_find(const char *id) {
    if (id)
        for (size_t i = 0; i < APP_MODEL_COUNT; ++i)
            if (strcmp(id, app_models[i].id) == 0)
                return &app_models[i];
    return nullptr;
}

struct app_assessment
app_assess(const struct app_hardware *h, const struct app_model *m, bool installed) {
    struct app_assessment a = {APP_CONDITIONAL,
                               "Performance on this device is not measured yet.",
                               "Unknown on this device; measure after download."};
    if (h->device == APP_PI5 && strcmp(m->id, "bitnet-2b") == 0)
        a.performance = "Pi 5 reference: 17.8 tokens/s; your speed may differ.";
    else if (h->device == APP_APPLE_SILICON)
        a.performance = "Apple Silicon profile; run a local test for actual speed.";
    if (!h->supported) {
        a.fit    = APP_UNAVAILABLE;
        a.reason = "This app build does not support this platform.";
    } else if (!installed && h->disk_known &&
               (h->disk < m->bytes || h->disk - m->bytes < 256 * UINT64_C(1048576))) {
        a.fit    = APP_UNAVAILABLE;
        a.reason = "Not enough disk space for the download plus 256 MiB reserve.";
    } else if (h->ram && h->ram < m->bytes) {
        a.fit    = APP_UNAVAILABLE;
        a.reason = "RAM is smaller than the model file, before context and OS memory.";
    } else if (h->ram < (uint64_t) m->recommended_ram_gib * APP_GIB * 95 / 100) {
        a.reason = "Below the RAM recommendation; swapping or allocation failures are possible.";
    } else if (h->available_known && h->available < (uint64_t) m->working_mib * 1048576) {
        a.reason = "Available RAM is tight now. Close other apps before loading this model.";
    } else if (h->device == APP_PI5 && strcmp(m->id, "bitnet-2b") == 0) {
        a.fit    = APP_RECOMMENDED;
        a.reason = "Fits the Pi 5 memory profile and has a published speed reference.";
    } else if (h->device == APP_APPLE_SILICON && h->cores >= 4) {
        a.fit    = APP_RECOMMENDED;
        a.reason = "Fits the Apple Silicon hardware and RAM profile; speed is an estimate.";
    }
    return a;
}

struct app_assessment app_assess_observed(const struct app_hardware *h,
                                          const struct app_model    *m,
                                          bool                       installed,
                                          double                     tokens_per_second) {
    struct app_assessment a = app_assess(h, m, installed);
    if (!(tokens_per_second > 0) || a.fit == APP_UNAVAILABLE)
        return a;
    a.performance =
            "Measured on this device in this app session; workload and temperature affect speed.";
    if (tokens_per_second < 8) {
        if (a.fit == APP_RECOMMENDED || strstr(a.reason, "not measured"))
            a.reason = "Measured below the app's interactive target of 8 tokens/s. Still usable "
                       "for patient tasks.";
        a.fit = APP_CONDITIONAL;
    } else if (h->ram >= (uint64_t) m->recommended_ram_gib * APP_GIB * 95 / 100 &&
               (!h->available_known || h->available >= (uint64_t) m->working_mib * 1048576)) {
        a.fit = APP_RECOMMENDED;
        a.reason =
                "Memory fits and measured speed meets the app's interactive target of 8 tokens/s.";
    }
    return a;
}
