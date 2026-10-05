#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "engine.h"
#include "compat.h"
struct app_run_stats {
    bool        limited, reasoning, no_answer;
    unsigned    max_tokens;
    const char *stage; /* bounded operation identity; never prompt/output text */
    double      first_answer_ns;
    size_t      tokens, prompt_tokens, reused;
    double      generation_ns, total_ns, prefill_ns;
};
bool app_daemon_identity(const char *path, char backend[static 24], struct app_engine *engine);
bool app_daemon_ready(const char *path);
bool app_daemon_ready_backend(const char *path, char backend[static 24]);
/* #148: one resident conversation in geistd; only what changed is sent.
 * part() gets complete UTF-8, thinking separated (reasoning: the catalog's
 * reasoning_format). */
int  app_daemon_chat(const char           *path,
                     size_t                count,
                     const struct chat_msg messages[],
                     unsigned              max,
                     float                 temperature,
                     float                 top_p,
                     const char           *reasoning,
                     bool (*part)(void *, bool thinking, const char *),
                     bool (*cancel)(void *),
                     void                 *ctx,
                     struct app_run_stats *stats,
                     char                  error[static 256]);
/* Borrowed callbacks, one client/session owned by each invocation. */
int app_daemon_run(const char *path,
                   const char *prompt,
                   unsigned    max,
                   const char *reasoning,
                   bool (*part)(void *, bool thinking, const char *),
                   bool (*cancel)(void *),
                   void                 *ctx,
                   struct app_run_stats *stats,
                   char                  error[static 256]);
