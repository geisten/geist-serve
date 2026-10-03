/* Request-local protocol separation, after UTF-8 decoding. Reasoning text is
 * discarded, or passed to an optional thinking callback (#93); it is never kept. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
struct app_output {
    enum { OUTPUT_PREFIX, OUTPUT_REASONING, OUTPUT_ANSWER, OUTPUT_DISCARD, OUTPUT_FINISHED } state;
    char     prefix[40];
    char     held[8]; /* a possible partial closing marker, until it resolves */
    size_t   used, closing, opening, held_len;
    unsigned depth;
    bool     enabled, reasoning, visible;
    bool (*think)(void *, const char *);
};
void app_output_init(struct app_output *out, const char *format);
/* Receive reasoning text (without markers) through think, with feed's context. */
void app_output_thinking(struct app_output *out, bool (*think)(void *, const char *));
bool app_output_feed(struct app_output *out,
                     const char        *text,
                     bool (*emit)(void *, const char *),
                     void *context);
/* Incomplete protocol prefixes and reasoning are discarded at EOF, never flushed. */
void app_output_finish(struct app_output *out);
