/* generate.c — the one place geist-serve talks to libgeist, plus the
 * request parsing both API front ends share. See serve.h. */
#include "serve.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Bytes of prompt text worth tokenizing at all: 4096 tokens of any script
 * fit in 32 KiB, and tokenizer time on a 1 MiB "word" is a DoS, not a
 * request (measured: 32 KB single-word = 5 s just to be refused). */
#define PROMPT_BYTES_CAP (32u * 1024u)
/* Text pending emission: held back for the longest partial stop-string
 * match and for an incomplete trailing UTF-8 sequence. */
struct holdback {
    char   buf[1024];
    size_t len;
};

struct gen_opts gen_opts_default(void) {
    return (struct gen_opts) {.temperature = 0.7f, .top_p = 0.9f, .top_k = 40, .seed = 0};
}

/* Longest prefix of buf[0..n) that ends on a complete UTF-8 sequence. A
 * byte-fallback token is itself a fragment of one, so "back up to a lead
 * byte" is not enough: the lead byte's sequence must also be complete. */
static size_t utf8_complete(size_t n, const unsigned char buf[static n]) {
    size_t p = n;
    while (p > 0 && n - p < 4 && (buf[p - 1] & 0xC0) == 0x80)
        p--; /* skip continuations */
    if (p == 0)
        return 0;
    unsigned char lead = buf[p - 1];
    size_t        need = lead < 0x80             ? 1
                         : (lead & 0xE0) == 0xC0 ? 2
                         : (lead & 0xF0) == 0xE0 ? 3
                         : (lead & 0xF8) == 0xF0 ? 4
                                                 : 1;
    return (p - 1) + need <= n ? n : p - 1;
}

static bool hb_flush(struct holdback *h, size_t keep, emit_fn emit, void *ctx) {
    if (h->len <= keep)
        return true;
    size_t cut = utf8_complete(h->len - keep, (const unsigned char *) h->buf);
    if (cut == 0)
        return true;
    if (!emit(ctx, cut, h->buf))
        return false;
    memmove(h->buf, h->buf + cut, h->len - cut);
    h->len -= cut;
    return true;
}

int generate(struct server         *sv,
             struct conn           *c,
             const struct gen_opts *o,
             const char            *prompt,
             emit_fn                emit,
             void                  *ctx,
             struct gen_result     *res,
             char                   err[static 256]) {
    *res = (struct gen_result) {.finish_reason = "stop"};

    struct geist_session_opts so = {
            .max_seq_len = CTX_CAP,
            .temperature = o->temperature,
            .top_p       = o->top_p,
            .top_k       = o->top_k,
            .random_seed = o->seed ? o->seed : (uint64_t) time(nullptr) ^ (uint64_t) clock(),
    };
    struct geist_session *s = nullptr;
    if (geist_session_create(sv->mo.m, sv->mo.be, &so, &s) != GEIST_OK) {
        snprintf(err, 256, "session: %s", s ? geist_session_errmsg(s) : "create failed");
        if (s)
            geist_session_destroy(s);
        return 500;
    }

    if (prompt[0] == '\0') {
        snprintf(err, 256, "prompt is empty");
        geist_session_destroy(s);
        return 400;
    }
    if (strlen(prompt) > PROMPT_BYTES_CAP) {
        snprintf(err, 256, "prompt exceeds %u bytes", PROMPT_BYTES_CAP);
        geist_session_destroy(s);
        return 400;
    }
    /* Token budget: tokenize() reports content tokens; set_prompt adds BOS
     * when the tokenizer says so. */
    static geist_token_t ids[CTX_CAP];
    size_t               n_ids = 0;
    if (geist_session_tokenize(s, prompt, CTX_CAP, ids, &n_ids) != GEIST_OK ||
        n_ids + 1 >= CTX_CAP) {
        snprintf(err, 256, "prompt does not fit the %d-token context", CTX_CAP);
        geist_session_destroy(s);
        return 400;
    }
    res->prompt_tokens = (int) n_ids + (sv->mo.meta.add_bos ? 1 : 0);
    int budget         = CTX_CAP - res->prompt_tokens;
    int max_tokens     = (o->max_tokens > 0 && o->max_tokens < budget) ? o->max_tokens : budget;

    if (geist_session_set_prompt(s, prompt) != GEIST_OK) {
        snprintf(err, 256, "prefill: %s", geist_session_errmsg(s));
        geist_session_destroy(s);
        return 500;
    }

    size_t keep = 0;
    for (int i = 0; i < o->n_stop; i++) {
        size_t l = strlen(o->stop[i]);
        if (l > 0 && l - 1 > keep)
            keep = l - 1;
    }

    struct holdback hb   = {};
    bool            more = true;
    for (int n = 0; n < max_tokens && more; n++) {
        if ((n & 7) == 7 && client_gone(c))
            break;
        geist_token_t t;
        if (geist_session_decode_step(s, &t) != GEIST_OK) {
            snprintf(err, 256, "decode: %s", geist_session_errmsg(s));
            geist_session_destroy(s);
            return 500;
        }
        if (model_is_stop(&sv->mo, t))
            break;

        res->completion_tokens++;
        const char *piece = geist_session_token_to_str(s, t);
        if (piece == nullptr)
            continue; /* control token */
        size_t pl = strlen(piece);
        if (hb.len + pl >= sizeof hb.buf)
            more = hb_flush(&hb, 0, emit, ctx);
        if (pl >= sizeof hb.buf)
            continue; /* absurd token; drop it */
        memcpy(hb.buf + hb.len, piece, pl);
        hb.len += pl;
        hb.buf[hb.len] = '\0';

        for (int k = 0; k < o->n_stop; k++) {
            char *at = o->stop[k][0] ? strstr(hb.buf, o->stop[k]) : nullptr;
            if (at != nullptr) {
                hb.len = (size_t) (at - hb.buf);
                more   = false;
                keep   = 0;
                break;
            }
        }
        if (more)
            more = hb_flush(&hb, keep, emit, ctx);
        if (n + 1 == max_tokens)
            res->finish_reason = "length";
    }
    hb_flush(&hb, 0, emit, ctx);

    struct geist_session_stats st = {};
    geist_session_get_stats(s, &st);
    res->prefill_ns = st.total_prefill_ns;
    res->decode_ns  = st.total_decode_ns;
    geist_session_destroy(s);
    return 0;
}

bool gen_opts_from_json(struct gen_opts *o, const struct json *j, int obj) {
    bool bad = false;
    o->temperature =
            (float) json_clamp(j, json_get(j, obj, "temperature"), o->temperature, 0, 2, &bad);
    o->top_p = (float) json_clamp(j, json_get(j, obj, "top_p"), o->top_p, 0.01, 1, &bad);
    o->top_k = (int) json_clamp(j, json_get(j, obj, "top_k"), o->top_k, 0, 1000, &bad);
    o->seed  = (uint64_t) json_clamp(
            j, json_get(j, obj, "seed"), (double) o->seed, 0, 9007199254740992.0, &bad);
    int stop = json_get(j, obj, "stop");
    if (json_is_str(j, stop)) {
        char *s = json_strdup(j, stop);
        if (s)
            snprintf(o->stop[o->n_stop++], STOP_LEN, "%s", s);
        free(s);
    } else if (stop >= 0 && j->tok[stop].type == JSMN_ARRAY) {
        for (int i = stop + 1; i < j->n && o->n_stop < STOP_MAX; i++) {
            if (j->tok[i].parent != stop)
                continue;
            char *s = json_strdup(j, i);
            if (s && s[0])
                snprintf(o->stop[o->n_stop++], STOP_LEN, "%s", s);
            free(s);
        }
    } else if (stop >= 0 && j->tok[stop].type != JSMN_PRIMITIVE) {
        bad = true; /* null is fine, an object is not */
    }
    return !bad;
}

bool model_name_ok(const struct server *sv, const struct json *j, int obj) {
    int t = json_get(j, obj, "model");
    if (t < 0)
        return true;
    char *m = json_strdup(j, t);
    if (m == nullptr)
        return false;
    char *colon = strstr(m, ":latest");
    if (colon != nullptr && colon[7] == '\0')
        *colon = '\0';
    bool ok = strcmp(m, sv->mo.name) == 0;
    free(m);
    return ok;
}

/* Token count of a rendered prompt for chat_render_fit, via the session
 * kept for that purpose. Over the byte cap counts as "does not fit". */
static size_t count_tokens(void *vsv, const char *text) {
    struct server *sv = vsv;
    if (strlen(text) > PROMPT_BYTES_CAP)
        return CTX_CAP + 1;
    static geist_token_t ids[CTX_CAP];
    size_t               n = 0;
    if (geist_session_tokenize(sv->tok, text, CTX_CAP, ids, &n) != GEIST_OK)
        return CTX_CAP + 1;
    return n + (sv->mo.meta.add_bos ? 1 : 0);
}

int parse_messages(
        const struct json *j, int arr, size_t cap, struct chat_msg out[static cap], size_t *n_out) {
    *n_out = 0;
    if (arr < 0 || j->tok[arr].type != JSMN_ARRAY)
        return 400;
    for (int i = arr + 1; i < j->n; i++) {
        if (j->tok[i].parent != arr)
            continue;
        if (j->tok[i].type != JSMN_OBJECT || *n_out == cap)
            return 400;
        char *role    = json_strdup(j, json_get(j, i, "role"));
        int   ct      = json_get(j, i, "content");
        char *content = nullptr;
        if (json_is_str(j, ct)) {
            content = json_strdup(j, ct);
        } else if (ct >= 0 && j->tok[ct].type == JSMN_ARRAY) {
            struct sb b = {};
            for (int k = ct + 1; k < j->n; k++) {
                if (j->tok[k].parent != ct)
                    continue;
                char *txt = json_strdup(j, json_get(j, k, "text"));
                if (txt)
                    sb_puts(&b, txt);
                free(txt);
            }
            content = b.p ? b.p : strdup("");
        } else if (ct >= 0 && j->tok[ct].type == JSMN_PRIMITIVE &&
                   j->src[j->tok[ct].start] == 'n') {
            content = strdup(""); /* null content (tool-call turns) */
        }
        if (role == nullptr || content == nullptr) {
            free(role);
            free(content);
            return 400;
        }
        out[(*n_out)++] = (struct chat_msg) {.role = role, .content = content};
    }
    return 0;
}

void free_messages(size_t n, const struct chat_msg msgs[]) {
    for (size_t i = 0; i < n; i++) {
        free((char *) msgs[i].role);
        free((char *) msgs[i].content);
    }
}

char *chat_prompt(struct server *sv, size_t n, const struct chat_msg msgs[], int max_tokens) {
    size_t reserve = max_tokens > 0 ? (size_t) max_tokens : 512;
    if (reserve > CTX_CAP / 2)
        reserve = CTX_CAP / 2;
    return chat_render_fit(sv->mo.family, n, msgs, CTX_CAP, reserve, count_tokens, sv, nullptr);
}
