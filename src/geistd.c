/* geistd — libgeist over a socket, for agents that run as many short calls.
 *
 * The session and its KV cache live here and outlive the connection: an
 * agent process opens a session, prefills, steps, peeks at logits, and
 * exits; the next process resumes the same id and pays only for the tokens
 * it adds. Nothing is interpreted: no chat template, no sampling policy
 * beyond what the engine's session options carry. See docs/GEISTD.md.
 *
 * Wire: frame = u32 header_len, u32 body_len (little-endian), JSON header,
 * raw body. Token ids are int32 arrays, logits float32 arrays.
 *
 * Serial, one process, like geist-serve. Unix socket by default, TCP with
 * --host/--port (a shared token is required off loopback), --stdio and
 * LISTEN_FDS as in geist-serve. */
#include <geist.h>
#include <geist_util.h>

#include "json.h"
#include "net.h"
#include "template.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define CTX_CAP 4096 /* engine cap today (geistlib#428) */
#define HDR_CAP (64u * 1024u)
#define BODY_CAP (16u * 1024u * 1024u)
#define TOPK_MAX 256
#define SESS_MAX 16
#define TOKEN_MIN 32

/* ====================================================================== */
/* Daemon state                                                            */
/* ====================================================================== */

struct sess {
    bool                  live;
    char                  id[17];
    struct geist_session *s;
    geist_token_t        *hist; /* what the KV cache holds, in order */
    size_t                n_hist;
    size_t                pinned; /* geist_session_pin_prefix: reset keeps this many */
    time_t                used;
};

struct daemon {
    struct geist_backend *be;
    struct geist_model   *m;
    char                  name[128];
    struct gguf_meta      meta;
    enum chat_family      family;
    bool                  add_bos;
    geist_token_t         eos;
    geist_token_t         eot[6];
    int                   n_eot;
    size_t                vocab;
    struct sess           sess[SESS_MAX];
    int                   n_max;
    int                   idle_s;
    const char           *token; /* required off-loopback; nullptr = none */
    bool                  need_hello;
};

/* ====================================================================== */
/* Framing                                                                 */
/* ====================================================================== */

struct conn {
    int  fd;  /* read side */
    int  out; /* write side: same socket, or stdout under --stdio */
    bool broken;
    bool authed;
};

static bool read_all(struct conn *c, size_t n, unsigned char buf[static n]) {
    while (n > 0) {
        ssize_t r = read(c->fd, buf, n);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return false;
        buf += r;
        n -= (size_t) r;
    }
    return true;
}

static bool write_all(struct conn *c, size_t n, const unsigned char buf[static n]) {
    while (n > 0 && !c->broken) {
        ssize_t w = write(c->out, buf, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0) {
            c->broken = true;
            return false;
        }
        buf += w;
        n -= (size_t) w;
    }
    return !c->broken;
}

static uint32_t rd_u32(const unsigned char b[static 4]) {
    return (uint32_t) b[0] | (uint32_t) b[1] << 8 | (uint32_t) b[2] << 16 | (uint32_t) b[3] << 24;
}

static void wr_u32(uint32_t v, unsigned char b[static 4]) {
    b[0] = (unsigned char) v, b[1] = (unsigned char) (v >> 8), b[2] = (unsigned char) (v >> 16),
    b[3] = (unsigned char) (v >> 24);
}

/* Returns 1 on a frame, 0 on clean EOF, -1 on a malformed/oversize frame
 * (the connection is dropped: a client that cannot frame cannot be
 * resynchronised). *hdr is NUL-terminated, malloc'd; *body malloc'd or
 * nullptr when empty. */
static int read_frame(struct conn *c, char **hdr, unsigned char **body, size_t *body_len) {
    unsigned char pre[8];
    if (!read_all(c, 8, pre))
        return 0;
    size_t hl = rd_u32(pre), bl = rd_u32(pre + 4);
    if (hl == 0 || hl > HDR_CAP || bl > BODY_CAP)
        return -1;
    *hdr  = malloc(hl + 1);
    *body = bl ? malloc(bl) : nullptr;
    if (*hdr == nullptr || (bl && *body == nullptr))
        return -1;
    if (!read_all(c, hl, (unsigned char *) *hdr) || (bl && !read_all(c, bl, *body))) {
        free(*hdr);
        free(*body);
        return -1;
    }
    (*hdr)[hl] = '\0';
    *body_len  = bl;
    return 1;
}

static bool
write_frame(struct conn *c, size_t hl, const char hdr[static hl], size_t bl, const void *body) {
    unsigned char pre[8];
    wr_u32((uint32_t) hl, pre);
    wr_u32((uint32_t) bl, pre + 4);
    return write_all(c, 8, pre) && write_all(c, hl, (const unsigned char *) hdr) &&
           (bl == 0 || write_all(c, bl, body));
}

static bool reply(struct conn *c, struct sb *hdr, size_t bl, const void *body) {
    bool ok = write_frame(c, hdr->len, hdr->p, bl, body);
    sb_free(hdr);
    return ok;
}

static bool reply_error(struct conn *c, const char *msg) {
    struct sb h = {};
    sb_puts(&h, "{\"ok\":false,\"error\":");
    sb_json_str(&h, strlen(msg), msg);
    sb_puts(&h, "}");
    return reply(c, &h, 0, nullptr);
}

/* ====================================================================== */
/* Sessions                                                                */
/* ====================================================================== */

static void sess_free(struct sess *x) {
    if (x->s)
        geist_session_destroy(x->s);
    free(x->hist);
    *x = (struct sess) {};
}

static void sess_evict_idle(struct daemon *d) {
    time_t now = time(nullptr);
    for (int i = 0; i < d->n_max; i++)
        if (d->sess[i].live && now - d->sess[i].used > d->idle_s) {
            fprintf(stderr, "geistd: session %s idle, evicted\n", d->sess[i].id);
            sess_free(&d->sess[i]);
        }
}

static struct sess *sess_find(struct daemon *d, const char *id) {
    if (id == nullptr)
        return nullptr;
    for (int i = 0; i < d->n_max; i++)
        if (d->sess[i].live && strcmp(d->sess[i].id, id) == 0) {
            d->sess[i].used = time(nullptr);
            return &d->sess[i];
        }
    return nullptr;
}

static void random_id(char out[static 17]) {
    unsigned char r[8] = {};
    int           fd   = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, r, sizeof r);
        (void) n;
        close(fd);
    }
    for (int i = 0; i < 8; i++)
        snprintf(out + 2 * i, 3, "%02x", r[i]);
}

/* A free slot, else the least recently used live one (evicted). */
static struct sess *sess_slot(struct daemon *d) {
    for (int i = 0; i < d->n_max; i++)
        if (!d->sess[i].live)
            return &d->sess[i];
    struct sess *lru = &d->sess[0];
    for (int i = 1; i < d->n_max; i++)
        if (d->sess[i].used < lru->used)
            lru = &d->sess[i];
    fprintf(stderr, "geistd: session %s evicted (table full)\n", lru->id);
    sess_free(lru);
    return lru;
}

/* ====================================================================== */
/* Logits helpers                                                          */
/* ====================================================================== */

/* log-softmax normaliser over the pending logits. */
static double logz(size_t n, const float x[static n]) {
    float mx = x[0];
    for (size_t i = 1; i < n; i++)
        mx = x[i] > mx ? x[i] : mx;
    double s = 0;
    for (size_t i = 0; i < n; i++)
        s += exp((double) x[i] - mx);
    return mx + log(s);
}

/* Top-k by logit, k <= TOPK_MAX, O(n k) insertion — k is small. */
static size_t
topk(size_t n, const float x[static n], size_t k, int32_t ids[static k], float vals[static k]) {
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        if (m == k && x[i] <= vals[m - 1])
            continue;
        size_t j = m < k ? m : k - 1;
        while (j > 0 && vals[j - 1] < x[i]) {
            vals[j] = vals[j - 1];
            ids[j]  = ids[j - 1];
            j--;
        }
        vals[j] = x[i];
        ids[j]  = (int32_t) i;
        if (m < k)
            m++;
    }
    return m;
}

static void sb_topk(struct sb *h, size_t n, const float x[static n], size_t k) {
    static int32_t ids[TOPK_MAX];
    static float   vals[TOPK_MAX];
    if (k > TOPK_MAX)
        k = TOPK_MAX;
    size_t m = topk(n, x, k, ids, vals);
    double z = logz(n, x);
    sb_puts(h, ",\"top\":[");
    for (size_t i = 0; i < m; i++)
        sb_printf(h, "%s[%d,%.4f]", i ? "," : "", ids[i], (double) vals[i] - z);
    sb_puts(h, "]");
}

/* ====================================================================== */
/* Ops                                                                     */
/* ====================================================================== */

static bool is_stop_token(const struct daemon *d, geist_token_t t) {
    if (t == d->eos)
        return true;
    for (int k = 0; k < d->n_eot; k++)
        if (t == d->eot[k])
            return true;
    return false;
}

static bool hist_push(struct sess *x, size_t n, const geist_token_t ids[static n]) {
    if (x->n_hist + n > CTX_CAP)
        return false;
    memcpy(x->hist + x->n_hist, ids, n * sizeof *ids);
    x->n_hist += n;
    return true;
}

static bool op_info(struct daemon *d, struct conn *c) {
    struct sb h = {};
    sb_printf(&h,
              "{\"ok\":true,\"model\":\"%s\",\"arch\":\"%s\",\"eos\":%d,\"eot\":[",
              d->name,
              geist_model_arch(d->m),
              d->eos);
    for (int k = 0; k < d->n_eot; k++)
        sb_printf(&h, "%s%d", k ? "," : "", d->eot[k]);
    int live = 0;
    for (int i = 0; i < d->n_max; i++)
        live += d->sess[i].live;
    sb_printf(&h,
              "],\"ctx\":%d,\"vocab\":%zu,\"add_bos\":%s,\"bos\":%d,\"template\":\"%s\","
              "\"sessions\":%d,\"max_sessions\":%d}",
              CTX_CAP,
              d->vocab,
              d->add_bos ? "true" : "false",
              geist_model_bos_token(d->m),
              chat_family_name(d->family),
              live,
              d->n_max);
    return reply(c, &h, 0, nullptr);
}

static bool op_open(struct daemon *d, struct conn *c, const struct json *j) {
    bool                      bad = false;
    struct geist_session_opts o   = {
            .max_seq_len = CTX_CAP,
            .temperature = (float) json_clamp(j, json_get(j, 0, "temperature"), 0, 0, 2, &bad),
            .top_p       = (float) json_clamp(j, json_get(j, 0, "top_p"), 1, 0.01, 1, &bad),
            .top_k       = (int) json_clamp(j, json_get(j, 0, "top_k"), 0, 0, 1000, &bad),
            .random_seed = (uint64_t) json_clamp(
                    j, json_get(j, 0, "seed"), 0, 0, 9007199254740992.0, &bad),
    };
    if (bad)
        return reply_error(c, "open: a sampler field has the wrong type");
    sess_evict_idle(d);
    struct sess *x = sess_slot(d);
    if (geist_session_create(d->m, d->be, &o, &x->s) != GEIST_OK) {
        const char *why = x->s ? geist_session_errmsg(x->s) : "session create failed";
        char        msg[300];
        snprintf(msg, sizeof msg, "open: %s", why);
        sess_free(x);
        return reply_error(c, msg);
    }
    x->hist = malloc(CTX_CAP * sizeof *x->hist);
    if (x->hist == nullptr) {
        sess_free(x);
        return reply_error(c, "open: out of memory");
    }
    x->live = true;
    x->used = time(nullptr);
    random_id(x->id);
    struct sb h = {};
    sb_printf(&h, "{\"ok\":true,\"session\":\"%s\"}", x->id);
    return reply(c, &h, 0, nullptr);
}

static bool op_tokenize(struct daemon *d, struct conn *c, const struct json *j) {
    char *text = json_strdup(j, json_get(j, 0, "text"));
    if (text == nullptr)
        return reply_error(c, "tokenize: text must be a string");
    /* Any live session tokenizes; open a throwaway one if none exists. */
    struct sess *x = nullptr;
    for (int i = 0; i < d->n_max && x == nullptr; i++)
        if (d->sess[i].live)
            x = &d->sess[i];
    struct geist_session *tmp = nullptr;
    struct geist_session *s   = x ? x->s : nullptr;
    if (s == nullptr) {
        struct geist_session_opts o = {.max_seq_len = 16};
        if (geist_session_create(d->m, d->be, &o, &tmp) != GEIST_OK) {
            free(text);
            return reply_error(c, "tokenize: no session");
        }
        s = tmp;
    }
    static geist_token_t ids[CTX_CAP];
    size_t               n  = 0;
    bool                 ok = strlen(text) <= 32u * 1024u &&
                              geist_session_tokenize(s, text, CTX_CAP, ids, &n) == GEIST_OK;
    free(text);
    if (tmp)
        geist_session_destroy(tmp);
    if (!ok)
        return reply_error(c, "tokenize: text too long for the context");
    struct sb h = {};
    sb_printf(&h, "{\"ok\":true,\"n\":%zu}", n);
    return reply(c, &h, n * sizeof(int32_t), ids);
}

static bool
op_str(struct daemon *d, struct conn *c, struct sess *x, size_t bl, const unsigned char *body) {
    if (x == nullptr)
        return reply_error(c, "str: unknown session");
    size_t               n   = bl / sizeof(int32_t);
    const geist_token_t *ids = (const geist_token_t *) body;
    struct sb            h   = {};
    sb_puts(&h, "{\"ok\":true,\"pieces\":[");
    for (size_t i = 0; i < n; i++) {
        const char *p = geist_session_token_to_str(x->s, ids[i]);
        sb_puts(&h, i ? "," : "");
        if (p)
            sb_json_str(&h, strlen(p), p);
        else
            sb_puts(&h, "null");
    }
    sb_puts(&h, "]}");
    (void) d;
    return reply(c, &h, 0, nullptr);
}

/* Diff prefill: the request is the whole intended context. If the session
 * history is a prefix of it, only the tail is prefilled; otherwise the
 * session is reset and everything is (the engine has no rollback). */
static bool
op_prefill(struct daemon *d, struct conn *c, struct sess *x, size_t bl, const unsigned char *body) {
    if (x == nullptr)
        return reply_error(c, "prefill: unknown session");
    if (bl % sizeof(int32_t))
        return reply_error(c, "prefill: body is not an int32 array");
    size_t               n   = bl / sizeof(int32_t);
    const geist_token_t *ids = (const geist_token_t *) body;
    if (n == 0)
        return reply_error(c, "prefill: no tokens");
    if (n > CTX_CAP - 1)
        return reply_error(c, "prefill: context overflow (4096)");
    for (size_t i = 0; i < n; i++)
        if (ids[i] < 0 || (size_t) ids[i] >= d->vocab)
            return reply_error(c, "prefill: token id out of range");

    size_t common = 0;
    while (common < x->n_hist && common < n && x->hist[common] == ids[common])
        common++;
    size_t reused = 0;
    if (common == x->n_hist && x->n_hist > 0 && n > x->n_hist) {
        reused = x->n_hist; /* history is a proper prefix: append the tail */
    } else if (common == n && x->n_hist == n) {
        struct sb h = {};
        sb_printf(&h, "{\"ok\":true,\"prefilled\":0,\"reused\":%zu,\"n\":%zu}", n, x->n_hist);
        return reply(c, &h, 0, nullptr); /* identical: nothing to do */
    } else {
        /* Divergence. A pinned prefix cannot be undone: refuse when the
         * request differs inside it, otherwise reset back to the pin. */
        if (x->pinned > 0 && common < x->pinned)
            return reply_error(c, "prefill: request differs inside the pinned prefix");
        if (geist_session_reset(x->s) != GEIST_OK)
            return reply_error(c, "prefill: reset failed");
        x->n_hist = x->pinned;
        reused    = x->pinned;
    }
    size_t tail = n - reused;
    if (tail > 0 && geist_session_prefill_tokens(x->s, tail, ids + reused) != GEIST_OK) {
        char msg[300];
        snprintf(msg, sizeof msg, "prefill: %s", geist_session_errmsg(x->s));
        geist_session_reset(x->s);
        x->n_hist = x->pinned;
        return reply_error(c, msg);
    }
    hist_push(x, tail, ids + reused);
    struct sb h = {};
    sb_printf(&h,
              "{\"ok\":true,\"prefilled\":%zu,\"reused\":%zu,\"n\":%zu}",
              tail,
              reused,
              x->n_hist);
    return reply(c, &h, 0, nullptr);
}

static bool op_peek(struct daemon       *d,
                    struct conn         *c,
                    struct sess         *x,
                    const struct json   *j,
                    size_t               bl,
                    const unsigned char *body) {
    if (x == nullptr)
        return reply_error(c, "peek: unknown session");
    size_t       n  = 0;
    const float *lg = geist_session_peek_logits(&n, x->s);
    if (lg == nullptr || n == 0)
        return reply_error(c, "peek: no pending logits (prefill first)");
    d->vocab    = n;
    struct sb h = {};
    sb_printf(&h, "{\"ok\":true,\"n\":%zu", n);
    bool   full = json_bool(j, json_get(j, 0, "full"), false);
    double k    = json_num(j, json_get(j, 0, "topk"), 0);
    if (k > 0)
        sb_topk(&h, n, lg, (size_t) k);
    if (bl >= sizeof(int32_t)) { /* logits and logprobs for the given ids */
        const int32_t *ids = (const int32_t *) body;
        size_t         m   = bl / sizeof(int32_t);
        double         z   = logz(n, lg);
        sb_puts(&h, ",\"logits\":[");
        for (size_t i = 0; i < m; i++) {
            bool ok = ids[i] >= 0 && (size_t) ids[i] < n;
            sb_printf(&h, "%s%s", i ? "," : "", ok ? "" : "null");
            if (ok)
                sb_printf(&h, "%.4f", (double) lg[ids[i]]);
        }
        sb_puts(&h, "],\"logprobs\":[");
        for (size_t i = 0; i < m; i++) {
            bool ok = ids[i] >= 0 && (size_t) ids[i] < n;
            sb_printf(&h, "%s%s", i ? "," : "", ok ? "" : "null");
            if (ok)
                sb_printf(&h, "%.4f", (double) lg[ids[i]] - z);
        }
        sb_puts(&h, "]");
    }
    sb_puts(&h, "}");
    return reply(c, &h, full ? n * sizeof(float) : 0, full ? lg : nullptr);
}

static bool op_step(struct daemon *d, struct conn *c, struct sess *x, const struct json *j) {
    if (x == nullptr)
        return reply_error(c, "step: unknown session");
    if (x->n_hist == 0)
        return reply_error(c, "step: prefill first");
    if (x->n_hist >= CTX_CAP - 1)
        return reply_error(c, "step: context full");
    struct sb h = {};
    sb_puts(&h, "{\"ok\":true");
    double k = json_num(j, json_get(j, 0, "topk"), 0);
    if (k > 0) { /* the distribution this step samples from */
        size_t       n  = 0;
        const float *lg = geist_session_peek_logits(&n, x->s);
        if (lg && n)
            sb_topk(&h, n, lg, (size_t) k);
    }
    geist_token_t t;
    if (geist_session_decode_step(x->s, &t) != GEIST_OK) {
        sb_free(&h);
        return reply_error(c, "step: decode failed");
    }
    hist_push(x, 1, &t);
    const char *p = geist_session_token_to_str(x->s, t);
    sb_printf(&h,
              ",\"token\":%d,\"stop\":%s,\"n\":%zu,\"piece\":",
              t,
              is_stop_token(d, t) ? "true" : "false",
              x->n_hist);
    if (p)
        sb_json_str(&h, strlen(p), p);
    else
        sb_puts(&h, "null");
    sb_puts(&h, "}");
    return reply(c, &h, 0, nullptr);
}

/* Streams one frame per token, then a final frame. Stops on EOS/end-of-
 * turn, a stop id, a stop string in the decoded tail, max, or the context. */
static bool op_generate(struct daemon *d, struct conn *c, struct sess *x, const struct json *j) {
    if (x == nullptr)
        return reply_error(c, "generate: unknown session");
    if (x->n_hist == 0)
        return reply_error(c, "generate: prefill first");
    size_t  max = (size_t) json_clamp(j, json_get(j, 0, "max"), 256, 1, CTX_CAP, &(bool) {false});
    int32_t stop_ids[16];
    size_t  n_stop = 0;
    int     arr    = json_get(j, 0, "stop_ids");
    if (arr >= 0 && j->tok[arr].type == JSMN_ARRAY)
        for (int i = arr + 1; i < j->n && n_stop < 16; i++)
            if (j->tok[i].parent == arr)
                stop_ids[n_stop++] = (int32_t) json_num(j, i, -1);
    char  *stops[8];
    size_t n_str = 0;
    int    sarr  = json_get(j, 0, "stop_strings");
    if (sarr >= 0 && j->tok[sarr].type == JSMN_ARRAY)
        for (int i = sarr + 1; i < j->n && n_str < 8; i++)
            if (j->tok[i].parent == sarr && json_is_str(j, i))
                stops[n_str++] = json_strdup(j, i);

    char        tail[512] = "";
    const char *reason    = "max";
    size_t      k         = 0;
    for (; k < max; k++) {
        if (x->n_hist >= CTX_CAP - 1) {
            reason = "context";
            break;
        }
        geist_token_t t;
        if (geist_session_decode_step(x->s, &t) != GEIST_OK) {
            reason = "error";
            break;
        }
        hist_push(x, 1, &t);
        const char *p    = geist_session_token_to_str(x->s, t);
        bool        stop = is_stop_token(d, t);
        for (size_t i = 0; i < n_stop; i++)
            stop |= (t == stop_ids[i]);
        if (p) {
            size_t pl = strlen(p), tl = strlen(tail);
            if (tl + pl >= sizeof tail) {
                memmove(tail,
                        tail + (tl + pl - sizeof tail + 1),
                        sizeof tail - (tl + pl - sizeof tail + 1));
                tl = strlen(tail);
            }
            memcpy(tail + tl, p, pl + 1);
            for (size_t i = 0; i < n_str; i++)
                stop |= strstr(tail, stops[i]) != nullptr;
        }
        struct sb h = {};
        sb_printf(&h, "{\"ok\":true,\"token\":%d,\"done\":false,\"piece\":", t);
        if (p)
            sb_json_str(&h, strlen(p), p);
        else
            sb_puts(&h, "null");
        sb_puts(&h, "}");
        if (!reply(c, &h, 0, nullptr)) {
            reason = "client";
            break;
        }
        if (stop) {
            reason = "stop";
            k++;
            break;
        }
    }
    for (size_t i = 0; i < n_str; i++)
        free(stops[i]);
    if (c->broken)
        return false;
    struct sb h = {};
    sb_printf(&h,
              "{\"ok\":true,\"done\":true,\"reason\":\"%s\",\"generated\":%zu,\"n\":%zu}",
              reason,
              k,
              x->n_hist);
    return reply(c, &h, 0, nullptr);
}

/* ====================================================================== */
/* Dispatch                                                                */
/* ====================================================================== */

static bool handle(struct daemon       *d,
                   struct conn         *c,
                   const char          *hdr,
                   size_t               hl,
                   size_t               bl,
                   const unsigned char *body) {
    struct json j;
    if (json_parse(&j, hl, hdr) < 0)
        return reply_error(c, "header is not a JSON object");
    char *op = json_strdup(&j, json_get(&j, 0, "op"));
    if (op == nullptr)
        return reply_error(c, "missing op");
    bool ok;
    if (strcmp(op, "hello") == 0) {
        char *tok  = json_strdup(&j, json_get(&j, 0, "token"));
        bool  good = d->token == nullptr;
        if (!good && tok != nullptr && strlen(tok) == strlen(d->token)) {
            unsigned diff = 0;
            for (size_t i = 0; d->token[i]; i++)
                diff |= (unsigned) (tok[i] ^ d->token[i]);
            good = diff == 0;
        }
        free(tok);
        c->authed = good;
        ok        = good ? reply(c,
                                 &(struct sb) {.p = strdup("{\"ok\":true}"), .len = 11, .cap = 12},
                                 0,
                                 nullptr)
                         : (reply_error(c, "hello: bad token"), false);
        free(op);
        return ok;
    }
    if (d->need_hello && !c->authed) {
        free(op);
        reply_error(c, "hello first");
        return false;
    }
    char        *sid = json_strdup(&j, json_get(&j, 0, "session"));
    struct sess *x   = sess_find(d, sid);
    free(sid);
    if (strcmp(op, "info") == 0)
        ok = op_info(d, c);
    else if (strcmp(op, "open") == 0)
        ok = op_open(d, c, &j);
    else if (strcmp(op, "tokenize") == 0)
        ok = op_tokenize(d, c, &j);
    else if (strcmp(op, "str") == 0)
        ok = op_str(d, c, x, bl, body);
    else if (strcmp(op, "prefill") == 0)
        ok = op_prefill(d, c, x, bl, body);
    else if (strcmp(op, "peek") == 0)
        ok = op_peek(d, c, x, &j, bl, body);
    else if (strcmp(op, "step") == 0)
        ok = op_step(d, c, x, &j);
    else if (strcmp(op, "generate") == 0)
        ok = op_generate(d, c, x, &j);
    else if (strcmp(op, "reset") == 0) {
        if (x == nullptr)
            ok = reply_error(c, "reset: unknown session");
        else {
            geist_session_reset(x->s); /* the engine keeps a pinned prefix */
            x->n_hist   = x->pinned;
            struct sb h = {};
            sb_printf(&h, "{\"ok\":true,\"n\":%zu}", x->n_hist);
            ok = reply(c, &h, 0, nullptr);
        }
    } else if (strcmp(op, "pin") == 0) {
        /* Pin the first n history tokens: reset then truncates to them
         * instead of to zero (geist_session_pin_prefix). Once pinned, a
         * prefill that differs inside the prefix is refused. */
        size_t n = (size_t) json_num(&j, json_get(&j, 0, "n"), 0);
        if (x == nullptr)
            ok = reply_error(c, "pin: unknown session");
        else if (x->pinned > 0)
            ok = reply_error(c, "pin: already pinned");
        else if (n == 0 || n > x->n_hist)
            ok = reply_error(c, "pin: n must be 1..history length");
        else if (geist_session_pin_prefix(x->s, n, x->hist) != GEIST_OK)
            ok = reply_error(c, "pin: unsupported by this architecture");
        else {
            x->pinned   = n;
            struct sb h = {};
            sb_printf(&h, "{\"ok\":true,\"pinned\":%zu}", n);
            ok = reply(c, &h, 0, nullptr);
        }
    } else if (strcmp(op, "close") == 0) {
        if (x == nullptr)
            ok = reply_error(c, "close: unknown session");
        else {
            sess_free(x);
            ok = reply(c,
                       &(struct sb) {.p = strdup("{\"ok\":true}"), .len = 11, .cap = 12},
                       0,
                       nullptr);
        }
    } else {
        ok = reply_error(c, "unknown op");
    }
    free(op);
    return ok;
}

static void serve_conn(struct daemon *d, int fd, int out) {
    struct conn c = {.fd = fd, .out = out};
    for (;;) {
        char          *hdr  = nullptr;
        unsigned char *body = nullptr;
        size_t         bl   = 0;
        int            r    = read_frame(&c, &hdr, &body, &bl);
        if (r <= 0) {
            if (r < 0)
                reply_error(&c, "malformed frame; closing");
            return;
        }
        bool ok = handle(d, &c, hdr, strlen(hdr), bl, body);
        free(hdr);
        free(body);
        if (!ok || c.broken)
            return;
    }
}

/* ====================================================================== */
/* main                                                                    */
/* ====================================================================== */

static int usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s <model.gguf> [--socket PATH] [--host ADDR --port N] [--sessions N] [--idle "
            "SECONDS] [--stdio]\n"
            "  default: Unix socket $XDG_RUNTIME_DIR/geistd.sock (else /tmp/geistd-<uid>.sock)\n"
            "  --host/--port  TCP instead; off loopback GEISTD_TOKEN must be set and clients send "
            "hello first\n"
            "  --sessions N   resident sessions (default 4, max %d); --idle S  evict after S s "
            "idle (default 1800)\n"
            "  --stdio        one connection on stdin/stdout\n",
            argv0,
            SESS_MAX);
    return 2;
}

int main(int argc, char **argv) {
    const char   *model = nullptr, *sock = nullptr, *host = nullptr;
    int           port  = 0;
    bool          stdio = false;
    struct daemon d     = {.n_max = 4, .idle_s = 1800};
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--stdio") == 0)
            stdio = true;
        else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc)
            sock = argv[++i];
        else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc)
            host = argv[++i];
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--sessions") == 0 && i + 1 < argc)
            d.n_max = atoi(argv[++i]);
        else if (strcmp(argv[i], "--idle") == 0 && i + 1 < argc)
            d.idle_s = atoi(argv[++i]);
        else if (argv[i][0] == '-' || model != nullptr)
            return usage(argv[0]);
        else
            model = argv[i];
    }
    if (model == nullptr || d.n_max < 1 || d.n_max > SESS_MAX || d.idle_s < 1)
        return usage(argv[0]);
    if ((host != nullptr) != (port > 0))
        return usage(argv[0]);

    d.token = getenv("GEISTD_TOKEN");
    if (d.token != nullptr && strlen(d.token) < TOKEN_MIN) {
        fprintf(stderr, "geistd: GEISTD_TOKEN must be at least %d characters\n", TOKEN_MIN);
        return 1;
    }
    bool off_loopback =
            host != nullptr && strcmp(host, "127.0.0.1") != 0 && strcmp(host, "localhost") != 0;
    if (off_loopback && d.token == nullptr) {
        fprintf(stderr,
                "geistd: --host %s is not loopback; set GEISTD_TOKEN (>= %d chars) or use an SSH "
                "tunnel\n",
                host,
                TOKEN_MIN);
        return 1;
    }
    d.need_hello = off_loopback;

    char sock_buf[256];
    int  lfd       = -1;
    bool inherited = false;
    if (!stdio) {
        lfd       = net_inherited_listener();
        inherited = lfd >= 0;
        if (lfd < 0 && host != nullptr)
            lfd = net_bind_tcp(host, port);
        if (lfd < 0 && host == nullptr) {
            if (sock == nullptr) {
                const char *rt = getenv("XDG_RUNTIME_DIR");
                if (rt)
                    snprintf(sock_buf, sizeof sock_buf, "%s/geistd.sock", rt);
                else
                    snprintf(sock_buf, sizeof sock_buf, "/tmp/geistd-%d.sock", (int) getuid());
                sock = sock_buf;
            }
            lfd = net_bind_unix(sock);
        }
        if (lfd < 0)
            return 1;
    }

    if (geist_backend_create("auto", nullptr, nullptr, &d.be) != GEIST_OK) {
        fprintf(stderr, "backend: %s\n", d.be ? geist_backend_errmsg(d.be) : "create failed");
        return 1;
    }
    if (geist_model_load(model, d.be, &d.m) != GEIST_OK) {
        fprintf(stderr, "model: %s\n", d.m ? geist_model_errmsg(d.m) : "load failed");
        return 1;
    }
    char path_copy[1024];
    snprintf(path_copy, sizeof path_copy, "%s", model);
    snprintf(d.name, sizeof d.name, "%s", basename(path_copy));
    char *dot = strrchr(d.name, '.');
    if (dot && strcmp(dot, ".gguf") == 0)
        *dot = '\0';
    gguf_read_meta(model, &d.meta);
    d.add_bos = d.meta.add_bos;
    d.family  = chat_family_from_template(d.meta.tpl);
    if (d.family == CHAT_UNKNOWN)
        d.family = chat_family_from_arch(geist_model_arch(d.m));
    d.eos = geist_model_eos_token(d.m);
    for (const char **t = (const char *[]) {"<end_of_turn>",
                                            "<turn|>",
                                            "<|im_end|>",
                                            "<|eot_id|>",
                                            "<|end_of_text|>",
                                            nullptr};
         *t && d.n_eot < 6;
         t++) {
        geist_token_t id = geist_model_token_by_text(d.m, *t);
        if (id != GEIST_TOKEN_NONE)
            d.eot[d.n_eot++] = id;
    }
    /* Vocabulary size: one forward pass over BOS in a throwaway session. */
    {
        struct geist_session_opts o   = {.max_seq_len = 16};
        struct geist_session     *s   = nullptr;
        geist_token_t             bos = geist_model_bos_token(d.m);
        if (geist_session_create(d.m, d.be, &o, &s) == GEIST_OK) {
            geist_token_t one = bos != GEIST_TOKEN_NONE ? bos : d.eos;
            if (geist_session_prefill_tokens(s, 1, &one) == GEIST_OK) {
                size_t n = 0;
                if (geist_session_peek_logits(&n, s))
                    d.vocab = n;
            }
            geist_session_destroy(s);
        }
    }
    fprintf(stderr,
            "geistd: loaded %s as \"%s\" (%s), vocab %zu, %d sessions, idle %ds%s\n",
            model,
            d.name,
            geist_model_arch(d.m),
            d.vocab,
            d.n_max,
            d.idle_s,
            d.need_hello ? ", token required" : "");

    net_install_signals();
    if (stdio) {
        serve_conn(&d, STDIN_FILENO, STDOUT_FILENO);
    } else {
        fprintf(stderr,
                "geistd: listening on %s\n",
                inherited ? "inherited socket"
                : host    ? "tcp"
                          : sock);
        while (!net_stop) {
            int fd = accept(lfd, nullptr, nullptr);
            if (fd < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            struct timeval tv = {.tv_sec = 30};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            serve_conn(&d, fd, fd);
            shutdown(fd, SHUT_WR);
            close(fd);
        }
        close(lfd);
        if (host == nullptr && !inherited)
            unlink(sock);
        fprintf(stderr, "geistd: stopped\n");
    }
    for (int i = 0; i < SESS_MAX; i++)
        if (d.sess[i].live)
            sess_free(&d.sess[i]);
    gguf_meta_free(&d.meta);
    geist_model_destroy(d.m);
    geist_backend_destroy(d.be);
    return 0;
}
