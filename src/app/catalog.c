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
static bool hex(const char *s, size_t n) {
    if (!s || strlen(s) != n)
        return false;
    for (; *s; ++s)
        if (!(*s >= '0' && *s <= '9') && !(*s >= 'a' && *s <= 'f'))
            return false;
    return true;
}
/* [passed, total], 0 <= passed <= total. */
static bool counts(const struct json *j, int object, const char *key, struct app_model *m) {
    int      t = json_get(j, object, key);
    uint64_t v[2];
    if (t < 0 || j->tok[t].type != JSMN_ARRAY || j->tok[t].size != 2)
        return false;
    for (int k = 0; k < 2; ++k) {
        const jsmntok_t *e = &j->tok[t + 1 + k];
        if (e->type != JSMN_PRIMITIVE || e->parent != t)
            return false;
        const char *s   = j->src + e->start;
        int         len = e->end - e->start;
        if (len < 1 || len > 6 || (len > 1 && s[0] == '0'))
            return false;
        v[k] = 0;
        for (int i = 0; i < len; ++i) {
            if (s[i] < '0' || s[i] > '9')
                return false;
            v[k] = v[k] * 10 + (unsigned) (s[i] - '0');
        }
    }
    m->quality_passed += (unsigned) v[0];
    m->quality_total += (unsigned) v[1];
    return v[1] && v[0] <= v[1];
}
static bool iso_date(const char *date) {
    if (!date || strlen(date) != 10)
        return false;
    for (int i = 0; i < 10; ++i)
        if (i == 4 || i == 7 ? date[i] != '-' : !isdigit((unsigned char) date[i]))
            return false;
    return true;
}
/* #102: reference benchmark evidence. Validated, then kept as its raw JSON text
 * (plain strings and digits only), so status can pass it through unchanged. */
static const char *quality(struct app_catalog *c, const struct json *j, int object, struct app_model *m) {
    static const char *const quality_keys[] = {"suite", "date", "engine", "evidence", "tasks", nullptr};
    static const char *const language_keys[] = {"de", "en", nullptr};
    if (!keys(j, object, quality_keys) || !hex(string(c, j, object, "suite", 12), 12) ||
        !hex(string(c, j, object, "evidence", 64), 64) || !component(string(c, j, object, "engine", 48), false))
        return nullptr;
    if (!iso_date(string(c, j, object, "date", 10)))
        return nullptr;
    int tasks = json_get(j, object, "tasks");
    if (tasks < 0 || j->tok[tasks].type != JSMN_OBJECT || j->tok[tasks].size < 1 || j->tok[tasks].size > 8)
        return nullptr;
    for (int i = tasks + 1; i < j->n && j->tok[i].start < j->tok[tasks].end; ++i) {
        if (j->tok[i].parent != tasks)
            continue;
        char   name[33];
        size_t n = (size_t) (j->tok[i].end - j->tok[i].start);
        if (j->tok[i].type != JSMN_STRING || !n || n >= sizeof name)
            return nullptr;
        memcpy(name, j->src + j->tok[i].start, n);
        name[n] = 0;
        unsigned before[2] = {m->quality_passed, m->quality_total};
        if (!component(name, false) || !keys(j, i + 1, language_keys) || !counts(j, i + 1, "de", m) ||
            !counts(j, i + 1, "en", m))
            return nullptr;
        static const char *const tasks[] = APP_QUALITY_TASKS;
        for (unsigned k = 0; k < 4; k++)
            if (!strcmp(name, tasks[k])) {
                m->quality_task[k][0] = m->quality_passed - before[0];
                m->quality_task[k][1] = m->quality_total - before[1];
            }
    }
    size_t n = (size_t) (j->tok[object].end - j->tok[object].start);
    if (n + 1 > sizeof c->strings - c->used)
        return nullptr;
    char *out = c->strings + c->used;
    memcpy(out, j->src + j->tok[object].start, n);
    out[n] = 0;
    c->used += n + 1;
    return out;
}
/* #104: speed on reference platforms, 1..4 entries of integers and plain
 * strings; kept as validated raw JSON for status, like quality. */
static const char *reference(struct app_catalog *c, const struct json *j, int list) {
    static const char *const entry_keys[] = {"platform", "backend", "answer_ms", "tokens_per_s",
                                             "memory_mib", "date", "engine", nullptr};
    if (list < 0 || j->tok[list].type != JSMN_ARRAY || j->tok[list].size < 1 || j->tok[list].size > 4)
        return nullptr;
    for (int i = list + 1; i < j->n && j->tok[i].start < j->tok[list].end; ++i) {
        if (j->tok[i].parent != list)
            continue;
        uint64_t    v;
        const char *backend = keys(j, i, entry_keys) ? string(c, j, i, "backend", 3) : nullptr;
        if (!backend || (strcmp(backend, "cpu") && strcmp(backend, "gpu")) || !string(c, j, i, "platform", 64) ||
            !number(j, i, "answer_ms", 3600000, &v) || !number(j, i, "tokens_per_s", 100000, &v) ||
            !number(j, i, "memory_mib", 1048576, &v) || !iso_date(string(c, j, i, "date", 10)) ||
            !component(string(c, j, i, "engine", 48), false))
            return nullptr;
    }
    size_t n = (size_t) (j->tok[list].end - j->tok[list].start);
    if (n + 1 > sizeof c->strings - c->used)
        return nullptr;
    char *out = c->strings + c->used;
    memcpy(out, j->src + j->tok[list].start, n);
    out[n] = 0;
    c->used += n + 1;
    return out;
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
                                             "unsupported_format",
                                             "group_id",
                                             "group_name",
                                             "quantization",
                                             "reasoning_format",
                                             "quality",
                                             "reference",
                                             nullptr};
    uint64_t                 v, schema;
    if (json_parse(j, strlen(text), text) < 0 || !keys(j, 0, root_keys) ||
        !number(j, 0, "schema", 2, &schema) || !number(j, 0, "revision", 1000000000, &v))
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
        if (json_get(j, i, "reasoning_format") >= 0) {
            m->reasoning_format = string(c, j, i, "reasoning_format", 24);
            if (!m->reasoning_format ||
                (strcmp(m->reasoning_format, "none") && strcmp(m->reasoning_format, "think_tags")))
                goto bad;
        }
        if (json_get(j, i, "quality") >= 0 && !(m->quality = quality(c, j, json_get(j, i, "quality"), m)))
            goto bad;
        if (json_get(j, i, "reference") >= 0 && !(m->reference = reference(c, j, json_get(j, i, "reference"))))
            goto bad;
        if (schema == 2) {
            m->group_id     = string(c, j, i, "group_id", 63);
            m->group_name   = string(c, j, i, "group_name", 100);
            m->quantization = string(c, j, i, "quantization", 32);
            if (!component(m->group_id, false) || !strcmp(m->group_id, "custom") ||
                !m->group_name || !component(m->quantization, false))
                goto bad;
        } else {
            /* Legacy catalogs have no trusted grouping metadata. Never guess from names. */
            if (json_get(j, i, "group_id") >= 0 || json_get(j, i, "group_name") >= 0 ||
                json_get(j, i, "quantization") >= 0)
                goto bad;
            m->group_id   = m->id;
            m->group_name = m->name;
        }
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
        if (json_get(j, i, "unsupported_format") >= 0) {
            m->unsupported_format = string(c, j, i, "unsupported_format", 32);
            if (!m->unsupported_format || strcmp(m->unsupported_format, "pq2_0") || m->backends)
                goto bad;
        }
        if (!(m->backends & 1) && !m->unsupported_format)
            goto bad;
        for (size_t k = 0; k < c->count; ++k) {
            const struct app_model *previous = &c->models[k];
            if (!strcmp(m->id, previous->id) || !strcmp(m->file, previous->file))
                goto bad;
            if (!strcmp(m->group_id, previous->group_id) &&
                (strcmp(m->group_name, previous->group_name) ||
                 (m->quantization && !strcmp(m->quantization, previous->quantization))))
                goto bad;
        }
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
