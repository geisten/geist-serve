#include "compat.h"
#include "../json.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool literal(const struct json *j, int t, const char *value) {
    return t >= 0 && j->tok[t].type == JSMN_PRIMITIVE &&
           (size_t) (j->tok[t].end - j->tok[t].start) == strlen(value) &&
           !memcmp(j->src + j->tok[t].start, value, strlen(value));
}
static bool valid_primitive(const struct json *j, int t) {
    if (literal(j, t, "true") || literal(j, t, "false") || literal(j, t, "null"))
        return true;
    const char *p = j->src + j->tok[t].start, *end = j->src + j->tok[t].end;
    if (p < end && *p == '-')
        p++;
    if (p == end)
        return false;
    if (*p == '0')
        p++;
    else {
        if (*p < '1' || *p > '9')
            return false;
        while (p < end && *p >= '0' && *p <= '9')
            p++;
    }
    if (p < end && *p == '.') {
        p++;
        if (p == end || *p < '0' || *p > '9')
            return false;
        while (p < end && *p >= '0' && *p <= '9')
            p++;
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
        p++;
        if (p < end && (*p == '+' || *p == '-'))
            p++;
        if (p == end || *p < '0' || *p > '9')
            return false;
        while (p < end && *p >= '0' && *p <= '9')
            p++;
    }
    return p == end;
}
static char *string(struct app_arena *arena, const struct json *j, int token) {
    if (!json_is_str(j, token))
        return nullptr;
    /* The downstream text protocol uses C strings; reject embedded NUL. */
    for (int i = j->tok[token].start; i < j->tok[token].end; i++) {
        if (j->src[i] != '\\')
            continue;
        i++;
        if (i + 4 < j->tok[token].end && j->src[i] == 'u' && !memcmp(j->src + i + 1, "0000", 4))
            return nullptr;
    }
    char *decoded = json_strdup(j, token);
    if (!decoded)
        return nullptr;
    size_t size = strlen(decoded) + 1;
    char  *copy = app_alloc(arena, size, 1, 1);
    if (copy)
        memcpy(copy, decoded, size);
    free(decoded);
    return copy;
}

int app_chat_parse(struct app_arena *arena,
                   const char       *body,
                   struct app_chat  *chat,
                   const char      **error) {
    *chat          = (struct app_chat) {.max_tokens = 256, .temperature = .2f, .top_p = 1};
    *error         = "Invalid chat request. Expected a model and text messages.";
    struct json *j = app_alloc(arena, 1, sizeof *j, _Alignof(struct json));
    if (!j)
        return 503;
    if (json_parse(j, strlen(body), body) < 0 || j->tok[0].type != JSMN_OBJECT)
        return 400;
    for (int i = 1; i < j->n; i++) {
        if (j->tok[i].type == JSMN_PRIMITIVE && !valid_primitive(j, i))
            return 400;
        if (j->tok[i].parent != 0)
            continue;
        for (int k = 1; k < i; k++) {
            int len = j->tok[i].end - j->tok[i].start;
            if (j->tok[k].parent == 0 && j->tok[k].end - j->tok[k].start == len &&
                !memcmp(body + j->tok[i].start, body + j->tok[k].start, (size_t) len))
                return 400;
        }
    }
    const char *unsupported[] = {"tools",
                                 "functions",
                                 "tool_choice",
                                 "function_call",
                                 "response_format",
                                 "audio",
                                 "modalities",
                                 "stop",
                                 "logprobs",
                                 "logit_bias"};
    for (size_t i = 0; i < sizeof unsupported / sizeof *unsupported; i++) {
        int t = json_get(j, 0, unsupported[i]);
        if (t < 0 || (i < 2 && j->tok[t].type == JSMN_ARRAY && !j->tok[t].size))
            continue;
        /* Explicitly disabled tools are allowed; never silently discard a tool request. */
        if (!strcmp(unsupported[i], "tool_choice")) {
            char *choice = string(arena, j, t);
            if (choice && !strcmp(choice, "none"))
                continue;
        }
        *error = "This endpoint supports text chat only. Tools, structured output and multimodal "
                 "requests are not supported.";
        return 422;
    }
    chat->model  = string(arena, j, json_get(j, 0, "model"));
    int messages = json_get(j, 0, "messages");
    if (!chat->model || !chat->model[0] || messages < 0 || j->tok[messages].type != JSMN_ARRAY ||
        !j->tok[messages].size)
        return 400;
    for (int i = messages + 1; i < j->n && j->tok[i].start < j->tok[messages].end; i++) {
        if (j->tok[i].parent != messages)
            continue;
        if (j->tok[i].type != JSMN_OBJECT || chat->count == APP_CHAT_MESSAGES)
            return 400;
        char *role = string(arena, j, json_get(j, i, "role"));
        if (!role || (strcmp(role, "user") && strcmp(role, "assistant") && strcmp(role, "system")))
            return 400;
        if (json_get(j, i, "tool_calls") >= 0 || json_get(j, i, "function_call") >= 0) {
            *error = "Tool-call history is not supported by this text-chat endpoint.";
            return 422;
        }
        int   t       = json_get(j, i, "content");
        char *content = nullptr;
        if (json_is_str(j, t))
            content = string(arena, j, t);
        else if (t >= 0 && j->tok[t].type == JSMN_ARRAY) {
            size_t cap = (size_t) (j->tok[t].end - j->tok[t].start) + 1;
            content    = app_alloc(arena, cap, 1, 1);
            if (!content)
                return 503;
            struct app_buffer b = {.data = content, .cap = cap};
            for (int k = t + 1; k < j->n && j->tok[k].start < j->tok[t].end; k++) {
                if (j->tok[k].parent != t)
                    continue;
                char *type = string(arena, j, json_get(j, k, "type"));
                char *text = string(arena, j, json_get(j, k, "text"));
                if (!type || strcmp(type, "text") || !text) {
                    *error = "Only text content parts are supported.";
                    return 422;
                }
                app_put(&b, text);
            }
            if (b.failed)
                return 503;
        }
        if (!content)
            return 400;
        chat->messages[chat->count++] = (struct chat_msg) {.role = role, .content = content};
    }
    if (!chat->count)
        return 400;
    const char *numbers[] = {"max_tokens", "max_completion_tokens", "temperature", "top_p", "n"};
    for (size_t i = 0; i < sizeof numbers / sizeof *numbers; i++) {
        int t = json_get(j, 0, numbers[i]);
        if (t < 0)
            continue;
        double v = json_num(j, t, NAN);
        if (!isfinite(v))
            return 400;
        if (i < 2) {
            if (v < 1 || v > 1024 || v != (unsigned) v)
                return 400;
            chat->max_tokens = (unsigned) v;
        }
        if (i == 2) {
            if (v < 0 || v > 2)
                return 400;
            chat->temperature = (float) v;
        }
        if (i == 3) {
            if (v <= 0 || v > 1)
                return 400;
            chat->top_p = (float) v;
        }
        if (i == 4 && v != 1) {
            *error = "Only one completion per request is supported.";
            return 422;
        }
    }
    int stream = json_get(j, 0, "stream");
    if (stream >= 0 && !literal(j, stream, "true") && !literal(j, stream, "false"))
        return 400;
    chat->stream = json_bool(j, stream, false);
    int options  = json_get(j, 0, "stream_options");
    if (options >= 0 && j->tok[options].type != JSMN_OBJECT)
        return 400;
    int usage = json_get(j, options, "include_usage");
    if (usage >= 0 && !literal(j, usage, "true") && !literal(j, usage, "false"))
        return 400;
    chat->include_usage = json_bool(j, json_get(j, options, "include_usage"), false);
    return 0;
}
