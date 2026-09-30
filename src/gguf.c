/* gguf.c — see gguf.h. */
#include "gguf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* GGUF v2/v3: magic, u32 version, u64 n_tensors, u64 n_kv, then n_kv of
 * (string key, u32 type, value). Strings are u64 length + bytes; arrays
 * are u32 elem type + u64 count + values. Everything little-endian. */
enum {
    GG_U8,
    GG_I8,
    GG_U16,
    GG_I16,
    GG_U32,
    GG_I32,
    GG_F32,
    GG_BOOL,
    GG_STR,
    GG_ARR,
    GG_U64,
    GG_I64,
    GG_F64
};

static bool rd(FILE *f, size_t n, void *out) {
    return fread(out, 1, n, f) == n;
}

static bool rd_u32(FILE *f, uint32_t *v) {
    return rd(f, 4, v);
}

static bool rd_u64(FILE *f, uint64_t *v) {
    return rd(f, 8, v);
}

/* Skip a value of type t; returns false on read error. */
static bool skip_val(FILE *f, uint32_t t) {
    static const int fixed[] = {1, 1, 2, 2, 4, 4, 4, 1, -1, -1, 8, 8, 8};
    if (t < 13 && fixed[t] > 0)
        return fseek(f, fixed[t], SEEK_CUR) == 0;
    if (t == GG_STR) {
        uint64_t n;
        return rd_u64(f, &n) && n < (1u << 30) && fseek(f, (long) n, SEEK_CUR) == 0;
    }
    if (t == GG_ARR) {
        uint32_t et;
        uint64_t n;
        if (!rd_u32(f, &et) || !rd_u64(f, &n))
            return false;
        if (et < 13 && fixed[et] > 0)
            return fseek(f, (long) (n * (uint64_t) fixed[et]), SEEK_CUR) == 0;
        for (uint64_t i = 0; i < n; i++)
            if (!skip_val(f, et))
                return false;
        return true;
    }
    return false;
}

static char *rd_str(FILE *f, size_t cap) {
    uint64_t n;
    if (!rd_u64(f, &n) || n > cap)
        return nullptr;
    char *s = malloc((size_t) n + 1);
    if (s == nullptr || !rd(f, (size_t) n, s)) {
        free(s);
        return nullptr;
    }
    s[n] = '\0';
    return s;
}

static bool ends_with(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

bool gguf_read_meta(const char *path, struct gguf_meta *out) {
    *out    = (struct gguf_meta) {.add_bos = true};
    FILE *f = fopen(path, "rb");
    if (f == nullptr)
        return false;
    char     magic[4];
    uint32_t version;
    uint64_t n_tensors, n_kv;
    bool     ok = rd(f, 4, magic) && memcmp(magic, "GGUF", 4) == 0 && rd_u32(f, &version) &&
                  version >= 2 && rd_u64(f, &n_tensors) && rd_u64(f, &n_kv) && n_kv < 65536;
    for (uint64_t i = 0; ok && i < n_kv; i++) {
        char    *key = rd_str(f, 4096);
        uint32_t t;
        if (key == nullptr || !rd_u32(f, &t)) {
            free(key);
            ok = false;
            break;
        }
        char **str_dst = nullptr;
        if (t == GG_STR && strcmp(key, "tokenizer.chat_template") == 0)
            str_dst = &out->tpl;
        else if (t == GG_STR && strcmp(key, "general.architecture") == 0)
            str_dst = &out->arch;
        else if (t == GG_STR && strcmp(key, "general.size_label") == 0)
            str_dst = &out->size_label;

        if (str_dst != nullptr) {
            free(*str_dst);
            *str_dst = rd_str(f, 1u << 20);
            ok       = *str_dst != nullptr;
        } else if (t == GG_BOOL && strcmp(key, "tokenizer.ggml.add_bos_token") == 0) {
            uint8_t b;
            ok           = rd(f, 1, &b);
            out->add_bos = b != 0;
        } else if (t == GG_U32 && strcmp(key, "general.file_type") == 0) {
            ok = rd_u32(f, &out->file_type);
        } else if (t == GG_U32 && ends_with(key, ".context_length")) {
            ok = rd_u32(f, &out->context_length);
        } else {
            ok = skip_val(f, t);
        }
        free(key);
    }
    fclose(f);
    if (!ok)
        gguf_meta_free(out);
    return ok;
}

void gguf_meta_free(struct gguf_meta *m) {
    free(m->tpl);
    free(m->arch);
    free(m->size_label);
    *m = (struct gguf_meta) {.add_bos = true};
}

/* llama_ftype names as Ollama shows them in quantization_level. */
const char *gguf_file_type_name(uint32_t ft) {
    static const char *names[] = {
            [0] = "F32",      [1] = "F16",      [2] = "Q4_0",    [3] = "Q4_1",    [7] = "Q8_0",
            [8] = "Q5_0",     [9] = "Q5_1",     [10] = "Q2_K",   [11] = "Q3_K_S", [12] = "Q3_K_M",
            [13] = "Q3_K_L",  [14] = "Q4_K_S",  [15] = "Q4_K_M", [16] = "Q5_K_S", [17] = "Q5_K_M",
            [18] = "Q6_K",    [19] = "IQ2_XXS", [20] = "IQ2_XS", [21] = "Q2_K_S", [22] = "IQ3_XS",
            [23] = "IQ3_XXS", [24] = "IQ1_S",   [25] = "IQ4_NL", [26] = "IQ3_S",  [27] = "IQ3_M",
            [28] = "IQ2_S",   [29] = "IQ2_M",   [30] = "IQ4_XS", [31] = "IQ1_M",  [32] = "BF16",
            [36] = "TQ1_0",   [37] = "TQ2_0",
    };
    size_t n = sizeof names / sizeof names[0];
    return ft < n && names[ft] != nullptr ? names[ft] : "unknown";
}
