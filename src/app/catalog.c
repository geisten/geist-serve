/* Bounded, atomically replaceable application catalog. Never inference-engine policy. */
#include "core.h"
#include "../json.h"
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../../build/app_models.h"

struct app_catalog {
    struct app_model models[APP_MODEL_COUNT];
    size_t           count, used;
    unsigned         revision;
    char             strings[APP_CATALOG_BYTES];
    char             source[APP_CATALOG_BYTES + 1];
};
static struct app_catalog *owned;

/* No unknown or duplicate keys, and no escaped keys masquerading as known names. */
static bool keys(const struct json *j, int object, const char *const *allowed) {
    if (object < 0 || j->tok[object].type != JSMN_OBJECT)
        return false;
    unsigned seen = 0;
    for (int i = object + 1; i < j->n && j->tok[i].start < j->tok[object].end; ++i) {
        if (j->tok[i].parent != object)
            continue;
        if (j->tok[i].type != JSMN_STRING)
            return false;
        unsigned k = 0;
        for (; allowed[k]; ++k)
            if ((size_t) (j->tok[i].end - j->tok[i].start) == strlen(allowed[k]) &&
                !memcmp(j->src + j->tok[i].start, allowed[k], strlen(allowed[k])))
                break;
        if (!allowed[k] || (seen & (1u << k)))
            return false;
        seen |= 1u << k;
    }
    return true;
}
static bool number(const struct json *j, int object, const char *key, uint64_t max, uint64_t *out) {
    int t = json_get(j, object, key);
    if (t < 0 || j->tok[t].type != JSMN_PRIMITIVE)
        return false;
    size_t      n = (size_t) (j->tok[t].end - j->tok[t].start);
    const char *s = j->src + j->tok[t].start;
    if (!n || n > 20 || (n > 1 && s[0] == '0'))
        return false;
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] < '0' || s[i] > '9' || v > (max - (unsigned) (s[i] - '0')) / 10)
            return false;
        v = v * 10 + (unsigned) (s[i] - '0');
    }
    if (!v || v > max)
        return false;
    *out = v;
    return true;
}
static const char *
string(struct app_catalog *c, const struct json *j, int object, const char *key, size_t max) {
    int t = json_get(j, object, key);
    if (!json_is_str(j, t))
        return nullptr;
    /* Import deliberately accepts plain UTF-8 strings, not escaped control/path bytes. */
    size_t n = (size_t) (j->tok[t].end - j->tok[t].start);
    if (!n || n > max || n + 1 > sizeof c->strings - c->used)
        return nullptr;
    const char *s = j->src + j->tok[t].start;
    for (size_t i = 0; i < n; ++i)
        if ((unsigned char) s[i] < 32 || s[i] == '\\' || s[i] == '"' || (unsigned char) s[i] == 127)
            return nullptr;
    char *out = c->strings + c->used;
    memcpy(out, s, n);
    out[n]              = 0;
    struct app_utf8 utf = {0};
    char            validated[1025];
    if (!app_utf8_feed(&utf, out, validated, sizeof validated) || utf.used)
        return nullptr;
    c->used += n + 1;
    return out;
}
static bool component(const char *s, bool file) {
    if (!s || !isalnum((unsigned char) *s) || strstr(s, ".."))
        return false;
    for (const char *p = s; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
              *p == '-' || *p == '_' || *p == '.'))
            return false;
    size_t n = strlen(s);
    return !file || (n > 5 && !strcmp(s + n - 5, ".gguf"));
}
struct app_catalog *app_catalog_parse(const char *text, char error[static 256]) {
    snprintf(error, 256, "Invalid model catalog. Check schema, entries and unique IDs/files.");
    if (!text || strlen(text) > APP_CATALOG_BYTES)
        return nullptr;
    struct json        *j = calloc(1, sizeof *j);
    struct app_catalog *c = calloc(1, sizeof *c);
    if (!j || !c)
        goto bad;
    static const char *const root_keys[]  = {"schema", "revision", "models", nullptr};
    static const char *const model_keys[] = {"id",
                                             "name",
                                             "file",
                                             "url",
                                             "sha256",
                                             "bytes",
                                             "working_mib",
                                             "recommended_ram_gib",
                                             "backends",
                                             nullptr};
    uint64_t                 v;
    if (json_parse(j, strlen(text), text) < 0 || !keys(j, 0, root_keys) ||
        !number(j, 0, "schema", 1, &v) || !number(j, 0, "revision", 1000000000, &v))
        goto bad;
    c->revision = (unsigned) v;
    int list    = json_get(j, 0, "models");
    if (list < 0 || j->tok[list].type != JSMN_ARRAY)
        goto bad;
    for (int i = list + 1; i < j->n && j->tok[i].start < j->tok[list].end; ++i) {
        if (j->tok[i].parent != list)
            continue;
        if (c->count == APP_MODEL_COUNT || !keys(j, i, model_keys))
            goto bad;
        struct app_model *m = &c->models[c->count];
        m->id               = string(c, j, i, "id", 63);
        m->name             = string(c, j, i, "name", 100);
        m->file             = string(c, j, i, "file", 180);
        m->url              = string(c, j, i, "url", 1024);
        m->sha256           = string(c, j, i, "sha256", 64);
        if (!component(m->id, false) || !strcmp(m->id, "custom") || !m->name ||
            !component(m->file, true) || !m->url || !m->sha256 || strlen(m->sha256) != 64)
            goto bad;
        for (const char *p = m->sha256; *p; ++p)
            if (!(*p >= '0' && *p <= '9') && !(*p >= 'a' && *p <= 'f'))
                goto bad;
        if (strncmp(m->url, "https://huggingface.co/", 23) || !strstr(m->url, "/resolve/") ||
            strpbrk(m->url, "@?# ") || strstr(m->url, "..") || strchr(m->url, '%'))
            goto bad;
        if (!number(j, i, "bytes", 256 * APP_GIB, &m->bytes) ||
            !number(j, i, "working_mib", 1048576, &v))
            goto bad;
        m->working_mib = (unsigned) v;
        if (!number(j, i, "recommended_ram_gib", 4096, &v))
            goto bad;
        m->recommended_ram_gib = (unsigned) v;
        int backends           = json_get(j, i, "backends");
        if (backends < 0 || j->tok[backends].type != JSMN_ARRAY)
            goto bad;
        for (int k = backends + 1; k < j->n && j->tok[k].start < j->tok[backends].end; ++k) {
            if (j->tok[k].parent != backends)
                continue;
            /* Compare complete raw enum tokens; decoded NUL must not truncate
             * an unrecognized name into a supported backend. */
            if (!json_is_str(j, k))
                goto bad;
            const char *name   = j->src + j->tok[k].start;
            size_t      length = (size_t) (j->tok[k].end - j->tok[k].start);
            unsigned    bit    = length == 3 && !memcmp(name, "cpu", 3)      ? 1
                                 : length == 5 && !memcmp(name, "metal", 5)  ? 2
                                 : length == 6 && !memcmp(name, "vulkan", 6) ? 4
                                                                             : 0;
            if (!bit || (m->backends & bit))
                goto bad;
            m->backends |= bit;
        }
        if (!(m->backends & 1))
            goto bad;
        for (size_t k = 0; k < c->count; ++k)
            if (!strcmp(m->id, c->models[k].id) || !strcmp(m->file, c->models[k].file))
                goto bad;
        ++c->count;
    }
    if (!c->count)
        goto bad;
    strcpy(c->source, text);
    free(j);
    error[0] = 0;
    return c;
bad:
    free(j);
    free(c);
    return nullptr;
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
    free(owned);
    owned = c;
}
void app_catalog_discard(struct app_catalog *c) {
    free(c);
}
