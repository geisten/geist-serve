/* Bounded, atomically replaceable application catalog. Never inference-engine policy. */
#include "core.h"
#include "geistr_catalog.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../../build/app_models.h"

struct app_catalog {
    struct app_model models[APP_MODEL_COUNT];
    size_t           count;
    unsigned         revision;
    geistr_catalog  *parsed; /* owns every string of models[] */
    char             source[APP_CATALOG_BYTES + 1];
};
static struct app_catalog *owned;

/* Validation is geist-runtime's (geistr_catalog_parse, the rules that were
 * here); the app adds its own limits and keeps its model table. */
struct app_catalog *app_catalog_parse(const char *text, char error[static 256]) {
    snprintf(error, 256, "Invalid model catalog. Check schema, entries and unique IDs/files.");
    if (!text || strlen(text) > APP_CATALOG_BYTES)
        return nullptr;
    struct app_catalog *c = calloc(1, sizeof *c);
    if (!c || geistr_catalog_parse(text, strlen(text), &c->parsed, nullptr, 0) != GEISTR_OK ||
        geistr_catalog_count(c->parsed) > APP_MODEL_COUNT) {
        app_catalog_discard(c);
        return nullptr;
    }
    static const char *const tasks[] = APP_QUALITY_TASKS;
    c->count    = geistr_catalog_count(c->parsed);
    c->revision = geistr_catalog_revision(c->parsed);
    for (size_t i = 0; i < c->count; ++i) {
        const geistr_catalog_entry *e = geistr_catalog_get(c->parsed, i);
        struct app_model           *m = &c->models[i];
        *m = (struct app_model) {.id = e->id, .name = e->name, .file = e->file, .url = e->url, .sha256 = e->sha256,
                                 .bytes = e->bytes, .working_mib = e->working_mib,
                                 .recommended_ram_gib = e->recommended_ram_gib, .backends = e->backends,
                                 .unsupported_format = e->unsupported_format, .group_id = e->group_id,
                                 .group_name = e->group_name, .quantization = e->quantization,
                                 .reasoning_format = e->reasoning_format, .quality = e->quality,
                                 .quality_passed = e->quality_passed, .quality_total = e->quality_total,
                                 .reference = e->reference};
        for (unsigned k = 0; k < 4; k++)
            geistr_catalog_quality(c->parsed, e, tasks[k], &m->quality_task[k][0], &m->quality_task[k][1]);
    }
    strcpy(c->source, text);
    error[0] = 0;
    return c;
}
const struct app_model *app_catalog_find(const struct app_catalog *c, const char *id) {
    for (size_t i = 0; i < c->count; ++i)
        if (!strcmp(id, c->models[i].id))
            return &c->models[i];
    return nullptr;
}
const struct app_model *app_catalog_entry(const struct app_catalog *c, size_t index) {
    return index < c->count ? &c->models[index] : nullptr;
}
unsigned app_catalog_version(const struct app_catalog *c) {
    return c->revision;
}
void app_catalog_apply(struct app_catalog *c) {
    memcpy(app_models, c->models, sizeof app_models);
    app_model_count      = c->count;
    app_catalog_revision = c->revision;
    app_catalog_json     = c->source;
    app_catalog_discard(owned);
    owned = c;
}
void app_catalog_discard(struct app_catalog *c) {
    if (c)
        geistr_catalog_free(c->parsed);
    free(c);
}
