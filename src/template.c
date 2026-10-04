/* template.c — see template.h. */
#include "template.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ====================================================================== */
/* Family detection                                                        */
/* ====================================================================== */

enum chat_family chat_family_from_template(const char *tpl) {
    if (tpl == nullptr)
        return CHAT_UNKNOWN;
    if (strstr(tpl, "<|turn>"))
        return CHAT_GEMMA4;
    if (strstr(tpl, "<start_of_turn>"))
        return CHAT_GEMMA3;
    if (strstr(tpl, "<|im_start|>"))
        return CHAT_CHATML;
    if (strstr(tpl, "<|start_header_id|>"))
        return CHAT_LLAMA3;
    if (strstr(tpl, "BITNETAssistant"))
        return CHAT_BITNET;
    return CHAT_UNKNOWN;
}

enum chat_family chat_family_from_arch(const char *arch) {
    if (arch == nullptr)
        return CHAT_UNKNOWN;
    if (strcmp(arch, "gemma4") == 0)
        return CHAT_GEMMA4;
    if (strncmp(arch, "gemma", 5) == 0)
        return CHAT_GEMMA3;
    if (strncmp(arch, "qwen", 4) == 0)
        return CHAT_CHATML;
    if (strcmp(arch, "llama") == 0)
        return CHAT_LLAMA3;
    if (strncmp(arch, "bitnet", 6) == 0)
        return CHAT_BITNET;
    return CHAT_UNKNOWN;
}

const char *chat_family_name(enum chat_family f) {
    switch (f) {
    case CHAT_GEMMA3:
        return "gemma3";
    case CHAT_GEMMA4:
        return "gemma4";
    case CHAT_CHATML:
        return "chatml";
    case CHAT_LLAMA3:
        return "llama3";
    case CHAT_BITNET:
        return "bitnet";
    default:
        return "unknown";
    }
}

/* ====================================================================== */
/* Rendering                                                               */
/* ====================================================================== */

struct out {
    char  *p;
    size_t len, cap;
    bool   oom;
};

static void put(struct out *o, const char *s) {
    size_t n = strlen(s);
    if (o->len + n + 1 > o->cap) {
        size_t cap = o->cap ? o->cap : 1024;
        while (cap < o->len + n + 1)
            cap *= 2;
        char *np = realloc(o->p, cap);
        if (np == nullptr) {
            o->oom = true;
            return;
        }
        o->p   = np;
        o->cap = cap;
    }
    memcpy(o->p + o->len, s, n + 1);
    o->len += n;
}

static bool is_system(const struct chat_msg *m) {
    return m->role != nullptr && strcmp(m->role, "system") == 0;
}

static bool is_assistant(const struct chat_msg *m) {
    return m->role != nullptr && strcmp(m->role, "assistant") == 0;
}

/* Turn-marker families share one shape: open(role) content close. The
 * system message is a turn of its own where the model has a system role,
 * and is folded into the first user turn (Gemma 3) where it has none. */
struct turn_fmt {
    const char *open, *role_end, *close, *sys_role, *user_role, *asst_role;
};

static void
render_turns(struct out *o, const struct turn_fmt *f, size_t n, const struct chat_msg msgs[]) {
    size_t      i      = 0;
    const char *folded = nullptr; /* system text awaiting the first user turn */
    if (n > 0 && is_system(&msgs[0])) {
        if (f->sys_role != nullptr) {
            put(o, f->open);
            put(o, f->sys_role);
            put(o, f->role_end);
            put(o, msgs[0].content);
            put(o, f->close);
        } else {
            folded = msgs[0].content;
        }
        i = 1;
    }
    for (; i < n; i++) {
        bool asst = is_assistant(&msgs[i]);
        put(o, f->open);
        put(o, asst ? f->asst_role : f->user_role);
        put(o, f->role_end);
        if (folded != nullptr && !asst) {
            put(o, folded);
            put(o, "\n\n");
            folded = nullptr;
        }
        put(o, msgs[i].content);
        put(o, f->close);
    }
    put(o, f->open);
    put(o, f->asst_role);
    put(o, f->role_end);
}

static const struct turn_fmt FMT_GEMMA3 = {
        "<start_of_turn>", "\n", "<end_of_turn>\n", nullptr, "user", "model"};
static const struct turn_fmt FMT_GEMMA4 = {"<|turn>", "\n", "<turn|>\n", "system", "user", "model"};
static const struct turn_fmt FMT_CHATML = {
        "<|im_start|>", "\n", "<|im_end|>\n", "system", "user", "assistant"};
static const struct turn_fmt FMT_LLAMA3 = {"<|start_header_id|>",
                                           "<|end_header_id|>\n\n",
                                           "<|eot_id|>",
                                           "system",
                                           "user",
                                           "assistant"};

/* BitNet b1.58 2B-4T (Llama-3 vocab): the turn format from the model card,
 * "System: …<|eot_id|>User: …<|eot_id|>Assistant: ". The GGUF's own
 * "Human: … BITNETAssistant: " template never ends the turn, and Llama-3
 * headers make the model end it at the first line break (#106). */
static void render_bitnet(struct out *o, size_t n, const struct chat_msg msgs[]) {
    for (size_t i = 0; i < n; i++) {
        put(o, is_system(&msgs[i]) ? "System: " : is_assistant(&msgs[i]) ? "Assistant: " : "User: ");
        put(o, msgs[i].content);
        put(o, "<|eot_id|>");
    }
    put(o, "Assistant: ");
}

char *chat_render(enum chat_family f, size_t n, const struct chat_msg msgs[]) {
    struct out o = {};
    switch (f) {
    case CHAT_GEMMA3:
        render_turns(&o, &FMT_GEMMA3, n, msgs);
        break;
    case CHAT_GEMMA4:
        render_turns(&o, &FMT_GEMMA4, n, msgs);
        break;
    case CHAT_CHATML:
        render_turns(&o, &FMT_CHATML, n, msgs);
        break;
    case CHAT_LLAMA3:
        render_turns(&o, &FMT_LLAMA3, n, msgs);
        break;
    case CHAT_BITNET:
        render_bitnet(&o, n, msgs);
        break;
    default:
        return nullptr;
    }
    if (o.oom) {
        free(o.p);
        return nullptr;
    }
    return o.p;
}

char *chat_render_fit(enum chat_family      f,
                      size_t                n,
                      const struct chat_msg msgs[],
                      size_t                budget,
                      size_t                reserve,
                      chat_count_fn         count,
                      void                 *ctx,
                      size_t               *out_tokens) {
    size_t sys = (n > 0 && is_system(&msgs[0])) ? 1 : 0;
    /* ponytail: linear scan from the oldest turn; conversations are short
     * and count() is a tokenizer call, not a forward pass. */
    for (size_t drop = 0; sys + drop < n; drop++) {
        /* Rebuild [system?] + msgs[sys+drop..n) contiguously. */
        size_t           m   = n - drop;
        struct chat_msg *tmp = malloc(m * sizeof *tmp);
        if (tmp == nullptr)
            return nullptr;
        size_t k = 0;
        if (sys)
            tmp[k++] = msgs[0];
        for (size_t i = sys + drop; i < n; i++)
            tmp[k++] = msgs[i];
        char *p = chat_render(f, k, tmp);
        free(tmp);
        if (p == nullptr)
            return nullptr;
        size_t t = count(ctx, p);
        if (t + reserve <= budget) {
            if (out_tokens)
                *out_tokens = t;
            return p;
        }
        free(p);
    }
    return nullptr;
}
