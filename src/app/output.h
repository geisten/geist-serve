/* Request-local protocol separation, after UTF-8 decoding. No hidden text is retained. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
struct app_output {
    enum { OUTPUT_PREFIX, OUTPUT_REASONING, OUTPUT_ANSWER, OUTPUT_DISCARD, OUTPUT_FINISHED } state;
    char     prefix[40];
    size_t   used, closing, opening;
    unsigned depth;
    bool     enabled, reasoning, visible;
};
void app_output_init(struct app_output *out, const char *format);
bool app_output_feed(struct app_output *out,
                     const char        *text,
                     bool (*emit)(void *, const char *),
                     void *context);
/* Incomplete protocol prefixes and reasoning are discarded at EOF, never flushed. */
void app_output_finish(struct app_output *out);
