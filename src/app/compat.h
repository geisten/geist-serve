/* Bounded OpenAI chat request parsing; no inference-engine dependency. */
#pragma once
#include "core.h"
#include "../template.h"
#define APP_CHAT_MESSAGES 64
struct app_chat {
    struct chat_msg messages[APP_CHAT_MESSAGES];
    size_t          count;
    char           *model;
    unsigned        max_tokens;
    float           temperature, top_p;
    bool            stream, include_usage;
};
/* All decoded strings belong to the request arena. */
int app_chat_parse(struct app_arena *arena,
                   const char       *body,
                   struct app_chat  *chat,
                   const char      **error);
