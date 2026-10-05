#include "core.h"
#include <math.h>
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

const struct app_model *app_model_find(const char *id) {
    if (id)
        for (size_t i = 0; i < app_model_count; ++i)
            if (strcmp(id, app_models[i].id) == 0)
                return &app_models[i];
    return nullptr;
}

geistr_device app_device(const struct app_hardware *h, unsigned gpu) {
    geistr_device d = {.size            = sizeof d,
                       .kind            = h->device == APP_APPLE_SILICON ? GEISTR_DEVICE_APPLE_SILICON
                                          : h->device == APP_PI5         ? GEISTR_DEVICE_PI5
                                                                         : GEISTR_DEVICE_OTHER,
                       .ram             = h->ram,
                       .available       = h->available,
                       .disk            = h->disk,
                       .cores           = h->cores,
                       .logical_cpus    = h->logical_cpus,
                       .gpu             = gpu,
                       .supported       = h->supported,
                       .available_known = h->available_known,
                       .disk_known      = h->disk_known};
    snprintf(d.name, sizeof d.name, "%s", h->name);
    snprintf(d.arch, sizeof d.arch, "%s", h->arch);
    snprintf(d.os, sizeof d.os, "%s", h->os);
    return d;
}

geistr_catalog_entry app_entry(const struct app_model *m) {
    return (geistr_catalog_entry) {.id = m->id, .name = m->name, .file = m->file, .url = m->url,
                                   .sha256 = m->sha256, .group_id = m->group_id, .group_name = m->group_name,
                                   .quantization = m->quantization, .reasoning_format = m->reasoning_format,
                                   .unsupported_format = m->unsupported_format, .quality = m->quality,
                                   .reference = m->reference, .bytes = m->bytes, .working_mib = m->working_mib,
                                   .recommended_ram_gib = m->recommended_ram_gib, .backends = m->backends,
                                   .quality_passed = m->quality_passed, .quality_total = m->quality_total};
}

/* The rules are geist-runtime's (geistr_assess); the app words its reasons. */
const char *app_resource_text(const char *code, const struct app_hardware *h, const struct app_model *m) {
    static const char *const texts[][2] = {
            {"unsupported_format", "This model requires PQ2_0 and Hadamard support, unavailable in the bundled engine."},
            {"platform", "This CPU instruction set or platform is not supported by the bundled engine."},
            {"disk", "Not enough disk space for the download plus 256 MiB reserve."},
            {"ram", "RAM is smaller than the model file, before context and OS memory."},
            {"ram_recommended", "Below the RAM recommendation; swapping or allocation failures are possible."},
            {"available_ram", "Available RAM is tight now. Close other apps before loading this model."},
            {"not_measured", "No known resource restriction. Speed has not been measured on this device."}};
    if (!strcmp(code, "fits"))
        return h->device == APP_PI5 && !strcmp(m->id, "bitnet-2b")
                       ? "Fits Pi 5 memory. A reference Pi 5 measured about 18 tokens/s."
                       : "Fits this Mac's memory. Speed not measured yet.";
    for (size_t i = 0; i < sizeof texts / sizeof *texts; i++)
        if (!strcmp(code, texts[i][0]))
            return texts[i][1];
    return code;
}

struct app_assessment
app_assess(const struct app_hardware *h, const struct app_model *m, bool installed) {
    struct app_assessment a = {APP_RECOMMENDED, "", "Unknown on this device; measure after download."};
    if (h->device == APP_PI5 && strcmp(m->id, "bitnet-2b") == 0)
        a.performance = "Pi 5 reference: 17.8 tokens/s; your speed may differ.";
    else if (h->device == APP_APPLE_SILICON)
        a.performance = "Apple Silicon profile; run a local test for actual speed.";
    geistr_device        d = app_device(h, 0);
    geistr_catalog_entry e = app_entry(m);
    const char          *code;
    geistr_resource      r = geistr_assess(&e, &d, installed, &code);
    a.fit    = r == GEISTR_RESOURCE_FITS ? APP_RECOMMENDED : r == GEISTR_RESOURCE_LIMITED ? APP_CONDITIONAL : APP_UNAVAILABLE;
    a.reason = app_resource_text(code, h, m);
    return a;
}

bool app_rate_below_target(double rate) {
    return isfinite(rate) && rate > 0 && rate < APP_INTERACTIVE_TPS;
}

double app_device_rate(double cpu_rate, bool gpu_available, double gpu_rate) {
    bool cpu_known = isfinite(cpu_rate) && cpu_rate > 0;
    bool gpu_known = isfinite(gpu_rate) && gpu_rate > 0;
    if (!gpu_available)
        return cpu_known ? cpu_rate : 0;
    /* One adequate processor disproves a general speed limit. A slow verdict
     * requires observations for both supported processors. */
    if (cpu_known && cpu_rate >= APP_INTERACTIVE_TPS)
        return gpu_known && gpu_rate > cpu_rate ? gpu_rate : cpu_rate;
    if (gpu_known && gpu_rate >= APP_INTERACTIVE_TPS)
        return gpu_rate;
    return cpu_known && gpu_known ? (cpu_rate > gpu_rate ? cpu_rate : gpu_rate) : 0;
}

struct app_assessment app_assess_device(const struct app_hardware *h,
                                       const struct app_model *m, bool installed,
                                       double cpu_rate, bool gpu_available, double gpu_rate) {
    struct app_assessment a = app_assess(h, m, installed);
    if (a.fit == APP_RECOMMENDED &&
        app_rate_below_target(app_device_rate(cpu_rate, gpu_available, gpu_rate))) {
        a.fit = APP_CONDITIONAL;
        a.reason = "Last replies were below 8 tokens/s on every available processor. Slower tasks remain possible.";
    }
    /* Never say "not measured" when this model has a measured speed here (#84
     * regression): the reason is shown next to the model in the list. */
    bool measured = (isfinite(cpu_rate) && cpu_rate > 0) || (isfinite(gpu_rate) && gpu_rate > 0);
    if (measured && !strcmp(a.reason, "Fits this Mac's memory. Speed not measured yet."))
        a.reason = "Fits this Mac's memory. Speed measured on this Mac.";
    else if (measured && !strcmp(a.reason, "No known resource restriction. Speed has not been measured on this device."))
        a.reason = "No known resource restriction. Speed measured on this device.";
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

static const char *setup_limit(const struct app_hardware  *h,
                               const struct app_model     *m,
                               const struct app_inventory *inventory,
                               bool                        running) {
    if (m->unsupported_format || !m->backends)
        return "This model requires PQ2_0 and Hadamard support, unavailable in the bundled engine.";
    if (!h->supported)
        return "This CPU instruction set or platform is not supported by the bundled engine.";
    if (!h->ram || !h->available_known || !h->disk_known)
        return "Cannot check available memory or disk space. Retry the platform check.";
    if (h->ram < (uint64_t) m->recommended_ram_gib * APP_GIB * 95 / 100)
        return "Not enough total RAM for this model's planning budget.";
    if (!running && h->available < (uint64_t) m->working_mib * 1048576)
        return "Available RAM is tight now. Close other apps before loading this model.";
    uint64_t remaining = m->bytes - (inventory->partial <= m->bytes ? inventory->partial : 0);
    if (!inventory->installed &&
        (h->disk < remaining || h->disk - remaining < 256 * UINT64_C(1048576)))
        return "Not enough disk space for the download plus 256 MiB reserve.";
    return nullptr;
}

struct app_recommendation
app_recommend(const struct app_hardware *h,
              const struct app_inventory inventory[static APP_MODEL_COUNT],
              const char                *selected,
              const char                *running) {
    /* Compatibility-tested preview defaults, not task-quality winners. Generic
     * Linux uses the small model exercised by both native installer CI jobs. */
    const struct app_model *preferred = app_model_find(h->device == APP_APPLE_SILICON ? "gemma4-e2b"
                                                       : h->device == APP_PI5 ? "bitnet-2b"
                                                                              : "smollm2-360m");
    const struct app_model *small     = &app_models[0];
    for (size_t i = 1; i < app_model_count; ++i)
        if (app_models[i].working_mib < small->working_mib)
            small = &app_models[i];
    if (!preferred)
        preferred = small;
    const struct app_model *saved = app_model_find(selected);
    if (saved) {
        const char *limit = setup_limit(
                h, saved, &inventory[saved - app_models], running && !strcmp(running, saved->id));
        return (struct app_recommendation) {
                saved,
                preferred,
                "saved",
                limit ? limit : "Your model choice is kept. Select another model below.",
                !limit};
    }
    const struct app_model *choices[] = {
            preferred, app_model_find("smollm2-360m") ? app_model_find("smollm2-360m") : small};
    const char *reason = nullptr;
    for (unsigned i = 0; i < 2; ++i) {
        const struct app_model *m     = choices[i];
        const char             *limit = setup_limit(h, m, &inventory[m - app_models], false);
        if (!limit && ((m != choices[1] && h->cores < 4) ||
                       app_rate_below_target(inventory[m - app_models].tps)))
            limit = "Local performance is below the interactive setup target.";
        if (!i)
            reason = limit;
        if (!limit)
            return (struct app_recommendation) {
                    m,
                    preferred,
                    i ? "fallback" : "default",
                    i ? "A smaller model fits the available resources better."
                      : "Platform default. Memory is estimated; answer quality is still "
                        "unverified.",
                    true};
    }
    return (struct app_recommendation) {nullptr, preferred, "blocked", reason, false};
}
