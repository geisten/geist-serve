#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "../template.h"
struct app_run_stats {
    bool   limited;
    size_t tokens, prompt_tokens, reused;
    double generation_ns, total_ns;
};
bool app_daemon_ready(const char *path);
int  app_daemon_chat(const char           *path,
                     size_t                count,
                     const struct chat_msg messages[],
                     unsigned              max,
                     float                 temperature,
                     float                 top_p,
                     bool (*emit)(void *, const char *),
                     bool (*cancel)(void *),
                     void                 *ctx,
                     struct app_run_stats *stats,
                     char                  error[static 256]);
/* Borrowed callbacks, one client/session owned by each invocation. */
int app_daemon_run(const char *path,
                   const char *prompt,
                   unsigned    max,
                   bool (*emit)(void *, const char *),
                   bool (*cancel)(void *),
                   void                 *ctx,
                   struct app_run_stats *stats,
                   char                  error[static 256]);
