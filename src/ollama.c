/* ollama.c — the Ollama API: /api/tags, /api/ps, /api/version, /api/show,
 * /api/generate, /api/chat, streamed as NDJSON. The registry routes answer
 * 404: geist-serve serves the one GGUF on its command line. */
#include "serve.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Ollama's error shape is flat, unlike OpenAI's. */
static void respond_ollama_error(struct conn *c, int status, const char *msg) {
    char json[512];
    snprintf(json, sizeof json, "{\"error\":\"%s\"}", msg);
    respond_json(c, status, json);
}

/* RFC 3339 with nanoseconds in UTC, as Ollama prints timestamps. */
static void iso_time(time_t t, char out[static 40]) {
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, 40, "%Y-%m-%dT%H:%M:%S.000000000Z", &tm);
}

struct ndjson_ctx {
    struct conn *c;
    const char  *model;
    bool         chat;
    struct sb   *text; /* non-stream: collect; stream: nullptr */
};

static bool emit_ndjson(void *vctx, size_t n, const char text[static n]) {
    struct ndjson_ctx *x = vctx;
    if (x->text != nullptr) {
        sb_put(x->text, n, text);
        return true;
    }
    char now[40];
    iso_time(time(nullptr), now);
    struct sb ev = {};
    sb_printf(&ev, "{\"model\":\"%s\",\"created_at\":\"%s\",", x->model, now);
    sb_puts(&ev, x->chat ? "\"message\":{\"role\":\"assistant\",\"content\":" : "\"response\":");
    sb_json_str(&ev, n, text);
    sb_puts(&ev, x->chat ? "},\"done\":false}\n" : ",\"done\":false}\n");
    bool ok = stream_write(x->c, ev.len, ev.p);
    sb_free(&ev);
    return ok;
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec;
}

/* A stable 64-hex "digest" from name and size. Clients only display it;
 * hashing a multi-GB file at startup would buy nothing. */
static void fake_digest(const struct server *sv, char out[static 65]) {
    uint64_t h = 1469598103934665603ull;
    for (const char *p = sv->mo.name; *p; p++)
        h = (h ^ (unsigned char) *p) * 1099511628211ull;
    h ^= (uint64_t) sv->file_size;
    for (int i = 0; i < 4; i++) {
        h = (h ^ (h >> 29)) * 0xbf58476d1ce4e5b9ull;
        snprintf(out + 16 * i, 17, "%016llx", (unsigned long long) h);
    }
}

static void sb_details(struct sb *b, const struct server *sv) {
    const char *arch = sv->mo.meta.arch ? sv->mo.meta.arch : geist_model_arch(sv->mo.m);
    sb_printf(b,
              "{\"parent_model\":\"\",\"format\":\"gguf\",\"family\":\"%s\",\"families\":[\"%s\"],"
              "\"parameter_size\":\"%s\",\"quantization_level\":\"%s\"}",
              arch,
              arch,
              sv->mo.meta.size_label ? sv->mo.meta.size_label : "",
              gguf_file_type_name(sv->mo.meta.file_type));
}

static void route_tags(struct server *sv, struct conn *c) {
    char      digest[65], mtime[40];
    struct sb b = {};
    fake_digest(sv, digest);
    iso_time(sv->file_mtime, mtime);
    sb_printf(&b,
              "{\"models\":[{\"name\":\"%s:latest\",\"model\":\"%s:latest\",\"modified_at\":\"%s\","
              "\"size\":%lld,\"digest\":\"%s\",\"details\":",
              sv->mo.name,
              sv->mo.name,
              mtime,
              (long long) sv->file_size,
              digest);
    sb_details(&b, sv);
    sb_puts(&b, "}]}");
    respond_json(c, 200, b.p);
    sb_free(&b);
}

static void route_ps(struct server *sv, struct conn *c) {
    char      digest[65], now[40];
    struct sb b = {};
    fake_digest(sv, digest);
    iso_time(time(nullptr) + 3600, now); /* "expires_at": never, in effect */
    sb_printf(&b,
              "{\"models\":[{\"name\":\"%s:latest\",\"model\":\"%s:latest\",\"size\":%lld,"
              "\"digest\":\"%s\","
              "\"details\":",
              sv->mo.name,
              sv->mo.name,
              (long long) sv->file_size,
              digest);
    sb_details(&b, sv);
    sb_printf(&b, ",\"expires_at\":\"%s\",\"size_vram\":0}]}", now);
    respond_json(c, 200, b.p);
    sb_free(&b);
}

static void route_show(struct server *sv, struct conn *c, struct req *r) {
    struct json j;
    if (r->body_len > 0 && json_parse(&j, r->body_len, r->body) >= 0 && !model_name_ok(sv, &j, 0) &&
        json_get(&j, 0, "name") < 0) {
        respond_ollama_error(c, 404, "model not found");
        return;
    }
    const char *arch = sv->mo.meta.arch ? sv->mo.meta.arch : geist_model_arch(sv->mo.m);
    char        mtime[40];
    iso_time(sv->file_mtime, mtime);
    struct sb b = {};
    sb_puts(&b, "{\"license\":\"\",\"modelfile\":");
    struct sb mf = {};
    sb_printf(&mf, "# served by geist-serve\nFROM %s\n", sv->mo.path);
    sb_json_str(&b, mf.len, mf.p);
    sb_free(&mf);
    sb_printf(&b, ",\"parameters\":\"num_ctx %d\",\"template\":", CTX_CAP);
    sb_json_str(&b,
                sv->mo.meta.tpl ? strlen(sv->mo.meta.tpl) : 0,
                sv->mo.meta.tpl ? sv->mo.meta.tpl : "");
    sb_puts(&b, ",\"details\":");
    sb_details(&b, sv);
    sb_printf(&b,
              ",\"model_info\":{\"general.architecture\":\"%s\",\"general.file_type\":%u,"
              "\"%s.context_length\":%u,\"geist.context_length\":%d},"
              "\"capabilities\":[\"completion\"],\"modified_at\":\"%s\"}",
              arch,
              sv->mo.meta.file_type,
              arch,
              sv->mo.meta.context_length,
              CTX_CAP,
              mtime);
    respond_json(c, 200, b.p);
    sb_free(&b);
}

/* Ollama's request shape: sampling under "options", num_predict for the
 * token budget, stream defaulting to true. */
static bool ollama_opts(const struct json *j, struct gen_opts *o, bool *stream) {
    int  opts = json_get(j, 0, "options");
    bool ok = true, bad = false;
    if (opts >= 0 && j->tok[opts].type == JSMN_OBJECT) {
        ok            = gen_opts_from_json(o, j, opts);
        o->max_tokens = (int) json_clamp(j, json_get(j, opts, "num_predict"), 0, -2, CTX_CAP, &bad);
        if (o->max_tokens < 0)
            o->max_tokens = 0; /* -1 infinite, -2 fill context: both = budget */
    } else if (opts >= 0) {
        bad = true;
    }
    *stream = json_bool(j, json_get(j, 0, "stream"), true);
    return ok && !bad;
}

/* The done:true object. `text` is the whole reply for a non-stream answer
 * and empty for the final streamed line. */
static void ollama_final(struct sb               *b,
                         const struct server     *sv,
                         bool                     chat,
                         size_t                   n,
                         const char               text[static n],
                         const struct gen_result *res,
                         const char              *done_reason,
                         uint64_t                 total_ns) {
    char now[40];
    iso_time(time(nullptr), now);
    sb_printf(b, "{\"model\":\"%s\",\"created_at\":\"%s\",", sv->mo.name, now);
    if (chat) {
        sb_puts(b, "\"message\":{\"role\":\"assistant\",\"content\":");
        sb_json_str(b, n, text);
        sb_puts(b, "},");
    } else {
        sb_puts(b, "\"response\":");
        sb_json_str(b, n, text);
        sb_puts(b, ",");
    }
    sb_printf(b, "\"done\":true,\"done_reason\":\"%s\"", done_reason);
    if (res != nullptr)
        sb_printf(b,
                  ",\"total_duration\":%llu,\"load_duration\":0,\"prompt_eval_count\":%d,"
                  "\"prompt_eval_duration\":%llu,\"eval_count\":%d,\"eval_duration\":%llu",
                  (unsigned long long) total_ns,
                  res->prompt_tokens,
                  (unsigned long long) res->prefill_ns,
                  res->completion_tokens,
                  (unsigned long long) res->decode_ns);
    sb_puts(b, "}\n");
}

/* Shared tail of /api/generate and /api/chat once the prompt is rendered. */
static void ollama_run(struct server         *sv,
                       struct conn           *c,
                       bool                   chat,
                       bool                   stream,
                       const struct gen_opts *o,
                       const char            *prompt) {
    struct sb         text = {};
    struct ndjson_ctx x    = {
            .c = c, .model = sv->mo.name, .chat = chat, .text = stream ? nullptr : &text};
    if (stream && !stream_begin(c, "application/x-ndjson"))
        return;
    uint64_t          t0 = now_ns();
    struct gen_result res;
    char              err[256];
    int               st = generate(sv, c, o, prompt, emit_ndjson, &x, &res, err);
    if (st != 0) {
        if (stream)
            stream_end(c);
        else
            respond_ollama_error(c, st, err);
        sb_free(&text);
        return;
    }
    struct sb b = {};
    ollama_final(
            &b, sv, chat, text.len, text.p ? text.p : "", &res, res.finish_reason, now_ns() - t0);
    if (stream) {
        stream_write(c, b.len, b.p);
        stream_end(c);
    } else {
        respond_json(c, 200, b.p);
    }
    sb_free(&b);
    sb_free(&text);
}

static void route_generate(struct server *sv, struct conn *c, struct req *r) {
    struct json j;
    if (r->body_len == 0 || json_parse(&j, r->body_len, r->body) < 0) {
        respond_ollama_error(c, 400, "body is not a JSON object");
        return;
    }
    if (!model_name_ok(sv, &j, 0)) {
        respond_ollama_error(c, 404, "model not found; GET /api/tags lists the one served");
        return;
    }
    struct gen_opts o = gen_opts_default();
    bool            stream;
    if (!ollama_opts(&j, &o, &stream)) {
        respond_ollama_error(c, 400, "options: a field has the wrong type");
        return;
    }
    char *prompt = json_strdup(&j, json_get(&j, 0, "prompt"));
    char *system = json_strdup(&j, json_get(&j, 0, "system"));
    if (prompt == nullptr || prompt[0] == '\0') {
        /* Ollama's "load the model" call: answer done at once. */
        struct sb b = {};
        ollama_final(&b, sv, false, 0, "", nullptr, "load", 0);
        respond_json(c, 200, b.p);
        sb_free(&b);
        free(prompt);
        free(system);
        return;
    }
    /* raw:true (or no known template) sends the prompt as-is; otherwise it
     * is one user turn rendered like /api/chat would. */
    char *rendered = nullptr;
    if (!json_bool(&j, json_get(&j, 0, "raw"), false) && sv->mo.family != CHAT_UNKNOWN) {
        struct chat_msg msgs[2];
        size_t          n = 0;
        if (system && system[0])
            msgs[n++] = (struct chat_msg) {"system", system};
        msgs[n++] = (struct chat_msg) {"user", prompt};
        rendered  = chat_render(sv->mo.family, n, msgs);
    }
    ollama_run(sv, c, false, stream, &o, rendered ? rendered : prompt);
    free(rendered);
    free(prompt);
    free(system);
}

static void route_chat(struct server *sv, struct conn *c, struct req *r) {
    struct json j;
    if (r->body_len == 0 || json_parse(&j, r->body_len, r->body) < 0) {
        respond_ollama_error(c, 400, "body is not a JSON object");
        return;
    }
    if (!model_name_ok(sv, &j, 0)) {
        respond_ollama_error(c, 404, "model not found; GET /api/tags lists the one served");
        return;
    }
    struct gen_opts o = gen_opts_default();
    bool            stream;
    if (!ollama_opts(&j, &o, &stream)) {
        respond_ollama_error(c, 400, "options: a field has the wrong type");
        return;
    }
    struct chat_msg msgs[MSG_CAP];
    size_t          n_msgs = 0;
    int             arr    = json_get(&j, 0, "messages");
    if (arr >= 0 && parse_messages(&j, arr, MSG_CAP, msgs, &n_msgs) != 0) {
        free_messages(n_msgs, msgs);
        respond_ollama_error(c, 400, "messages must be an array of {role, content}");
        return;
    }
    if (n_msgs == 0) { /* the "load" call */
        struct sb b = {};
        ollama_final(&b, sv, true, 0, "", nullptr, "load", 0);
        respond_json(c, 200, b.p);
        sb_free(&b);
        return;
    }
    if (sv->mo.family == CHAT_UNKNOWN) {
        free_messages(n_msgs, msgs);
        respond_ollama_error(
                c, 501, "no chat template known for this model; use /api/generate with raw:true");
        return;
    }
    char *prompt = chat_prompt(sv, n_msgs, msgs, o.max_tokens);
    free_messages(n_msgs, msgs);
    if (prompt == nullptr) {
        respond_ollama_error(c, 400, "the last message alone does not fit the context");
        return;
    }
    ollama_run(sv, c, true, stream, &o, prompt);
    free(prompt);
}

void ollama_route(struct server *sv, struct conn *c, struct req *r) {
    const char *p    = r->path + 4; /* past "/api" */
    bool        post = strcmp(r->method, "POST") == 0;
    if (strcmp(p, "/tags") == 0)
        route_tags(sv, c);
    else if (strcmp(p, "/ps") == 0)
        route_ps(sv, c);
    else if (strcmp(p, "/version") == 0)
        respond_json(c, 200, "{\"version\":\"0.1.0\"}");
    else if (strcmp(p, "/show") == 0)
        route_show(sv, c, r);
    else if (strcmp(p, "/generate") == 0 && post)
        route_generate(sv, c, r);
    else if (strcmp(p, "/chat") == 0 && post)
        route_chat(sv, c, r);
    else if (strcmp(p, "/generate") == 0 || strcmp(p, "/chat") == 0)
        respond_ollama_error(c, 405, "POST only");
    else if (strcmp(p, "/pull") == 0 || strcmp(p, "/push") == 0 || strcmp(p, "/create") == 0 ||
             strcmp(p, "/copy") == 0 || strcmp(p, "/delete") == 0 || strncmp(p, "/blobs", 6) == 0)
        respond_ollama_error(
                c, 404, "no registry: geist-serve serves the one GGUF given on its command line");
    else if (strcmp(p, "/embed") == 0 || strcmp(p, "/embeddings") == 0)
        respond_ollama_error(c, 404, "embeddings are not served in v1");
    else
        respond_ollama_error(c, 404, "no such endpoint");
}
