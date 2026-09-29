#include "output.h"
#include <string.h>

void app_output_init(struct app_output *o, const char *format) {
    *o = (struct app_output) {.enabled = format && !strcmp(format, "think_tags")};
    if (!o->enabled)
        o->state = OUTPUT_ANSWER;
}
static bool whitespace(char c) {
    return c == ' ' || c == '\n' || c == '\r' || c == '\t';
}
static bool
answer(struct app_output *o, const char *s, bool (*emit)(void *, const char *), void *ctx) {
    for (const char *p = s; *p; ++p)
        if (!whitespace(*p))
            o->visible = true;
    return !*s || emit(ctx, s);
}
bool app_output_feed(struct app_output *o,
                     const char        *s,
                     bool (*emit)(void *, const char *),
                     void *ctx) {
    static const char open[] = "<think>", close[] = "</think>";
    if (o->state == OUTPUT_FINISHED)
        return false;
    while (*s) {
        if (o->state == OUTPUT_DISCARD)
            return true;
        if (o->state == OUTPUT_ANSWER)
            return answer(o, s, emit, ctx);
        if (o->state == OUTPUT_REASONING) {
            char c = *s++;
            if (c == open[o->opening])
                ++o->opening;
            else
                o->opening = c == open[0] ? 1 : 0;
            if (o->opening == sizeof open - 1) {
                o->opening = 0;
                if (++o->depth > 16) {
                    o->state = OUTPUT_DISCARD;
                    continue;
                }
            }
            if (c == close[o->closing])
                ++o->closing;
            else
                o->closing = c == close[0] ? 1 : 0;
            if (o->closing == sizeof close - 1) {
                o->closing = 0;
                if (--o->depth)
                    continue;
                o->opening   = 0;
                o->state     = OUTPUT_PREFIX;
                o->closing   = 0;
                o->used      = 0;
                o->prefix[0] = 0;
            }
            continue;
        }
        size_t leading = 0;
        while (leading < o->used && whitespace(o->prefix[leading]))
            ++leading;
        /* Retain at most 32 leading whitespace bytes, regardless of generation length. */
        if (leading == o->used && whitespace(*s) && leading >= 32) {
            ++s;
            continue;
        }
        size_t matched = o->used - leading;
        if ((matched == 0 && whitespace(*s)) || *s == open[matched]) {
            o->prefix[o->used++] = *s++;
            o->prefix[o->used]   = 0;
            if (!whitespace(o->prefix[o->used - 1]) && o->used - leading == sizeof open - 1) {
                o->state     = OUTPUT_REASONING;
                o->reasoning = true;
                o->depth     = 1;
                o->used      = 0;
                o->prefix[0] = 0;
            }
        } else {
            o->state = OUTPUT_ANSWER;
            if (!answer(o, o->prefix, emit, ctx))
                return false;
            o->used = 0;
            return answer(o, s, emit, ctx);
        }
    }
    return true;
}
void app_output_finish(struct app_output *o) {
    o->used = o->closing = o->opening = o->depth = 0;
    memset(o->prefix, 0, sizeof o->prefix);
    o->state = OUTPUT_FINISHED;
}
