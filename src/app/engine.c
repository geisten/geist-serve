#include "engine.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static bool hex(const char *s, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return false;
    return true;
}
bool app_engine_valid(const struct app_engine *e) {
    size_t version  = strnlen(e->version, sizeof e->version);
    size_t revision = strnlen(e->revision, sizeof e->revision);
    size_t payload  = strnlen(e->payload_sha256, sizeof e->payload_sha256);
    if (strnlen(e->source_state, sizeof e->source_state) == sizeof e->source_state ||
        (!strcmp(e->source_state, "clean") && !revision) || version == sizeof e->version ||
        (revision && ((revision != 40 && revision != 64) || !hex(e->revision, revision))) ||
        (payload && (payload != 64 || !hex(e->payload_sha256, payload))))
        return false;
    for (size_t i = 0; i < version; ++i)
        if (!(isalnum((unsigned char) e->version[i]) || strchr(".-+_", e->version[i])))
            return false;
    return !e->source_state[0] || !strcmp(e->source_state, "clean") ||
           !strcmp(e->source_state, "modified") || !strcmp(e->source_state, "unknown");
}
static bool field(char *out, size_t cap, const struct json *j, int obj, const char *key) {
    int t = obj < 0 ? -1 : json_get(j, obj, key);
    if (t < 0 || (j->tok[t].type == JSMN_PRIMITIVE && j->tok[t].end - j->tok[t].start == 4 &&
                  !memcmp(j->src + j->tok[t].start, "null", 4)))
        return true;
    if (j->tok[t].type != JSMN_STRING)
        return false;
    /* Provenance uses plain ASCII tokens; reject escapes/NUL before decoding. */
    for (int i = j->tok[t].start; i < j->tok[t].end; ++i)
        if ((unsigned char) j->src[i] < 32 || (unsigned char) j->src[i] > 126 || j->src[i] == '\\')
            return false;
    char *s = json_strdup(j, t);
    if (!s || strlen(s) >= cap) {
        free(s);
        return false;
    }
    strcpy(out, s);
    free(s);
    return true;
}
bool app_engine_parse(struct app_engine *out, const struct json *j, int obj) {
    *out = (struct app_engine) {0};
    if (obj < 0)
        return true; /* A legacy daemon has no provenance. */
    if (j->tok[obj].type != JSMN_OBJECT)
        return false;
    int lib = json_get(j, obj, "geistlib");
    if (lib >= 0 && j->tok[lib].type != JSMN_OBJECT)
        return false;
    bool ok = field(out->version, sizeof out->version, j, lib, "version") &&
              field(out->revision, sizeof out->revision, j, lib, "revision") &&
              field(out->source_state, sizeof out->source_state, j, lib, "source_state") &&
              field(out->payload_sha256, sizeof out->payload_sha256, j, obj, "payload_sha256") &&
              app_engine_valid(out);
    if (!ok)
        *out = (struct app_engine) {0};
    return ok;
}
static void value(struct app_buffer *b, const char *s) {
    if (*s)
        app_quote(b, s);
    else
        app_put(b, "null");
}
void app_engine_json(struct app_buffer *b, const struct app_engine *e) {
    app_put(b, "{\"geistlib\":{\"version\":");
    value(b, e->version);
    app_put(b, ",\"revision\":");
    value(b, e->revision);
    app_put(b, ",\"source_state\":");
    app_quote(b, e->source_state[0] ? e->source_state : "unknown");
    app_put(b, "},\"payload_sha256\":");
    value(b, e->payload_sha256);
    app_put(b, ",\"unavailable_reason\":");
    if (!e->version[0] || !e->revision[0])
        app_quote(b, "not_reported");
    else
        app_put(b, "null");
    app_put(b, "}");
}
