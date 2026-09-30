/* openai.c — the OpenAI API: /v1/models, /v1/completions,
 * /v1/chat/completions, streamed as server-sent events. */
#include "serve.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct sse_ctx {
    struct conn *c;
    const char  *model;
    const char  *id;
    time_t       created;
    bool         chat;
    struct sb   *text; /* non-stream: collect; stream: nullptr */
};

static bool emit_sse(void *vctx, size_t n, const char text[static n]) {
    struct sse_ctx *x = vctx;
    if (x->text != nullptr) {
        sb_put(x->text, n, text);
        return true;
    }
    struct sb ev = {};
    sb_printf(&ev,
              "data: "
              "{\"id\":\"%s\",\"object\":\"%s\",\"created\":%lld,\"model\":\"%s\",\"choices\":["
              "{\"index\":0,",
              x->id,
              x->chat ? "chat.completion.chunk" : "text_completion",
              (long long) x->created,
              x->model);
    sb_puts(&ev, x->chat ? "\"delta\":{\"content\":" : "\"text\":");
    sb_json_str(&ev, n, text);
    sb_puts(&ev, x->chat ? "},\"finish_reason\":null}]}\n\n" : ",\"finish_reason\":null}]}\n\n");
    bool ok = stream_write(x->c, ev.len, ev.p);
    sb_free(&ev);
    return ok;
}

static void sb_usage(struct sb *b, const struct gen_result *res) {
    sb_printf(b,
              "\"usage\":{\"prompt_tokens\":%d,\"completion_tokens\":%d,\"total_tokens\":%d}}",
              res->prompt_tokens,
              res->completion_tokens,
              res->prompt_tokens + res->completion_tokens);
}

/* Shared tail of both completion routes once the prompt is ready. */
static void oai_run(struct server         *sv,
                    struct conn           *c,
                    bool                   chat,
                    bool                   stream,
                    const struct gen_opts *o,
                    const char            *prompt) {
    const char *object = chat ? "chat.completion" : "text_completion";
    char        id[40];
    snprintf(id,
             sizeof id,
             "%s-%llx",
             chat ? "chatcmpl" : "cmpl",
             (unsigned long long) time(nullptr) ^ (unsigned long long) clock());
    struct sb      text = {};
    struct sse_ctx x    = {.c       = c,
                           .model   = sv->mo.name,
                           .id      = id,
                           .created = time(nullptr),
                           .chat    = chat,
                           .text    = stream ? nullptr : &text};
    if (stream) {
        if (!stream_begin(c, "text/event-stream"))
            return;
        if (chat) { /* first chunk carries the role, as OpenAI does */
            struct sb ev = {};
            sb_printf(&ev,
                      "data: {\"id\":\"%s\",\"object\":\"chat.completion.chunk\",\"created\":%lld,"
                      "\"model\":\"%s\",\"choices\":[{\"index\":0,\"delta\":{\"role\":"
                      "\"assistant\",\"content\":\"\"},\"finish_reason\":null}]}\n\n",
                      id,
                      (long long) x.created,
                      sv->mo.name);
            stream_write(c, ev.len, ev.p);
            sb_free(&ev);
        }
    }

    struct gen_result res;
    char              err[256];
    int               st = generate(sv, c, o, prompt, emit_sse, &x, &res, err);
    if (st != 0) {
        if (stream)
            stream_end(c); /* headers are out; the stream just ends */
        else
            respond_error(c, st, err);
        sb_free(&text);
        return;
    }
    struct sb b = {};
    if (stream) {
        sb_printf(&b,
                  "data: {\"id\":\"%s\",\"object\":\"%s\",\"created\":%lld,\"model\":\"%s\","
                  "\"choices\":[{\"index\":0,%s\"finish_reason\":\"%s\"}],",
                  id,
                  chat ? "chat.completion.chunk" : "text_completion",
                  (long long) x.created,
                  sv->mo.name,
                  chat ? "\"delta\":{}," : "\"text\":\"\",",
                  res.finish_reason);
        sb_usage(&b, &res);
        sb_puts(&b, "\n\ndata: [DONE]\n\n");
        stream_write(c, b.len, b.p);
        stream_end(c);
    } else {
        sb_printf(&b,
                  "{\"id\":\"%s\",\"object\":\"%s\",\"created\":%lld,\"model\":\"%s\","
                  "\"choices\":[{\"index\":0,",
                  id,
                  object,
                  (long long) x.created,
                  sv->mo.name);
        sb_puts(&b, chat ? "\"message\":{\"role\":\"assistant\",\"content\":" : "\"text\":");
        sb_json_str(&b, text.len, text.p ? text.p : "");
        sb_printf(&b, "%s,\"finish_reason\":\"%s\"}],", chat ? "}" : "", res.finish_reason);
        sb_usage(&b, &res);
        respond_json(c, 200, b.p);
    }
    sb_free(&b);
    sb_free(&text);
}

static void route_chat(struct server *sv, struct conn *c, struct req *r) {
    struct json j;
    if (r->body_len == 0 || json_parse(&j, r->body_len, r->body) < 0) {
        respond_error(c, 400, "body is not a JSON object");
        return;
    }
    if (!model_name_ok(sv, &j, 0)) {
        respond_error(c, 404, "model not found; GET /v1/models lists the one served");
        return;
    }
    if (sv->mo.family == CHAT_UNKNOWN) {
        respond_error(c, 501, "no chat template known for this model; use /v1/completions");
        return;
    }
    struct chat_msg msgs[MSG_CAP];
    size_t          n_msgs = 0;
    if (parse_messages(&j, json_get(&j, 0, "messages"), MSG_CAP, msgs, &n_msgs) != 0 ||
        n_msgs == 0) {
        free_messages(n_msgs, msgs);
        respond_error(c, 400, "messages must be a non-empty array of {role, content}");
        return;
    }
    struct gen_opts o   = gen_opts_default();
    bool            ok  = gen_opts_from_json(&o, &j, 0);
    bool            bad = false;
    int             mt  = json_get(&j, 0, "max_completion_tokens");
    if (mt < 0)
        mt = json_get(&j, 0, "max_tokens");
    o.max_tokens = (int) json_clamp(&j, mt, 0, 0, CTX_CAP, &bad);
    bool stream  = json_bool(&j, json_get(&j, 0, "stream"), false);
    if (!ok || bad) {
        free_messages(n_msgs, msgs);
        respond_error(c, 400, "a numeric field has the wrong type");
        return;
    }
    char *prompt = chat_prompt(sv, n_msgs, msgs, o.max_tokens);
    free_messages(n_msgs, msgs);
    if (prompt == nullptr) {
        respond_error(c, 400, "the last message alone does not fit the context");
        return;
    }
    oai_run(sv, c, true, stream, &o, prompt);
    free(prompt);
}

static void route_completions(struct server *sv, struct conn *c, struct req *r) {
    struct json j;
    if (r->body_len == 0 || json_parse(&j, r->body_len, r->body) < 0) {
        respond_error(c, 400, "body is not a JSON object");
        return;
    }
    if (!model_name_ok(sv, &j, 0)) {
        respond_error(c, 404, "model not found; GET /v1/models lists the one served");
        return;
    }
    char *prompt = json_strdup(&j, json_get(&j, 0, "prompt"));
    if (prompt == nullptr) {
        respond_error(c, 400, "prompt must be a string");
        return;
    }
    struct gen_opts o   = gen_opts_default();
    bool            ok  = gen_opts_from_json(&o, &j, 0);
    bool            bad = false;
    o.max_tokens        = (int) json_clamp(&j, json_get(&j, 0, "max_tokens"), 0, 0, CTX_CAP, &bad);
    if (!ok || bad) {
        free(prompt);
        respond_error(c, 400, "a numeric field has the wrong type");
        return;
    }
    oai_run(sv, c, false, json_bool(&j, json_get(&j, 0, "stream"), false), &o, prompt);
    free(prompt);
}

static void route_models(struct server *sv, struct conn *c) {
    struct sb b = {};
    sb_printf(&b,
              "{\"object\":\"list\",\"data\":[{\"id\":\"%s\",\"object\":\"model\",\"created\":%lld,"
              "\"owned_by\":\"geist\"}]}",
              sv->mo.name,
              (long long) sv->loaded_at);
    respond_json(c, 200, b.p);
    sb_free(&b);
}

void openai_route(struct server *sv, struct conn *c, struct req *r) {
    bool post = strcmp(r->method, "POST") == 0;
    if (strcmp(r->path, "/v1/models") == 0)
        route_models(sv, c);
    else if (strcmp(r->path, "/v1/chat/completions") == 0)
        post ? route_chat(sv, c, r) : respond_error(c, 405, "POST only");
    else if (strcmp(r->path, "/v1/completions") == 0)
        post ? route_completions(sv, c, r) : respond_error(c, 405, "POST only");
    else
        respond_error(c, 404, "no such endpoint");
}
