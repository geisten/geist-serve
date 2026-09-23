/* test_template.c — model-free goldens for the chat renderers, the family
 * fingerprint, oldest-turn truncation, and the GGUF header scan on a
 * synthetic file. Framework-free: a failed check prints and exits 1. */
#include "../src/template.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;

static void eq(const char *name, const char *want, const char *got) {
    if (got != nullptr && want != nullptr && strcmp(want, got) == 0) {
        printf("ok   %s\n", name);
        return;
    }
    printf("FAIL %s\n  want: %s\n  got:  %s\n", name, want ? want : "(null)", got ? got : "(null)");
    fails++;
}

static void eq_int(const char *name, long want, long got) {
    if (want == got)
        printf("ok   %s\n", name);
    else
        printf("FAIL %s: want %ld got %ld\n", name, want, got), fails++;
}

static const struct chat_msg CONV[] = {
        {"system", "Be brief."},
        {"user", "Hi"},
        {"assistant", "Hello!"},
        {"user", "Capital of France?"},
};
#define N (sizeof CONV / sizeof CONV[0])

static size_t count_words(void *ctx, const char *s) {
    (void) ctx;
    size_t n = 0;
    for (const char *p = s; *p; p++)
        n += (*p == ' ' || *p == '\n');
    return n;
}

int main(void) {
    /* --- fingerprint ------------------------------------------------- */
    eq_int("fp gemma4", CHAT_GEMMA4, chat_family_from_template("{{ '<|turn>system\\n' }}"));
    eq_int("fp gemma3", CHAT_GEMMA3, chat_family_from_template("<start_of_turn>user"));
    eq_int("fp chatml", CHAT_CHATML, chat_family_from_template("{{'<|im_start|>' + role}}"));
    eq_int("fp llama3", CHAT_LLAMA3, chat_family_from_template("<|start_header_id|>"));
    eq_int("fp bitnet", CHAT_BITNET, chat_family_from_template("'\\n\\nBITNETAssistant: '"));
    eq_int("fp unknown", CHAT_UNKNOWN, chat_family_from_template("{% for m in messages %}"));
    eq_int("fp null", CHAT_UNKNOWN, chat_family_from_template(nullptr));
    eq_int("arch smollm is chatml by template, not arch",
           CHAT_LLAMA3,
           chat_family_from_arch("llama"));
    eq_int("arch qwen35", CHAT_CHATML, chat_family_from_arch("qwen35"));
    eq_int("arch bitnet", CHAT_BITNET, chat_family_from_arch("bitnet-b1.58"));

    /* --- renderers --------------------------------------------------- */
    char *p;
    p = chat_render(CHAT_CHATML, N, CONV);
    eq("chatml",
       "<|im_start|>system\nBe brief.<|im_end|>\n"
       "<|im_start|>user\nHi<|im_end|>\n"
       "<|im_start|>assistant\nHello!<|im_end|>\n"
       "<|im_start|>user\nCapital of France?<|im_end|>\n"
       "<|im_start|>assistant\n",
       p);
    free(p);

    p = chat_render(CHAT_GEMMA4, N, CONV);
    eq("gemma4",
       "<|turn>system\nBe brief.<turn|>\n"
       "<|turn>user\nHi<turn|>\n"
       "<|turn>model\nHello!<turn|>\n"
       "<|turn>user\nCapital of France?<turn|>\n"
       "<|turn>model\n",
       p);
    free(p);

    p = chat_render(CHAT_GEMMA3, N, CONV);
    eq("gemma3 folds system into first user turn",
       "<start_of_turn>user\nBe brief.\n\nHi<end_of_turn>\n"
       "<start_of_turn>model\nHello!<end_of_turn>\n"
       "<start_of_turn>user\nCapital of France?<end_of_turn>\n"
       "<start_of_turn>model\n",
       p);
    free(p);

    p = chat_render(CHAT_LLAMA3, N, CONV);
    eq("llama3",
       "<|start_header_id|>system<|end_header_id|>\n\nBe brief.<|eot_id|>"
       "<|start_header_id|>user<|end_header_id|>\n\nHi<|eot_id|>"
       "<|start_header_id|>assistant<|end_header_id|>\n\nHello!<|eot_id|>"
       "<|start_header_id|>user<|end_header_id|>\n\nCapital of France?<|eot_id|>"
       "<|start_header_id|>assistant<|end_header_id|>\n\n",
       p);
    free(p);

    p = chat_render(CHAT_BITNET, N, CONV);
    eq("bitnet",
       "Human: Be brief.\n\nHi\n\nBITNETAssistant: "
       "Hello!<|eot_id|>"
       "Human: Capital of France?\n\nBITNETAssistant: ",
       p);
    free(p);

    p = chat_render(CHAT_CHATML, 1, (struct chat_msg[]) {{"user", "Hi"}});
    eq("no system message", "<|im_start|>user\nHi<|im_end|>\n<|im_start|>assistant\n", p);
    free(p);

    p = chat_render(CHAT_CHATML, 2, (struct chat_msg[]) {{"user", "a"}, {"tool", "x"}});
    eq("unknown role renders as user",
       "<|im_start|>user\na<|im_end|>\n<|im_start|>user\nx<|im_end|>\n<|im_start|>assistant\n",
       p);
    free(p);

    p = chat_render(CHAT_UNKNOWN, N, CONV);
    eq_int("unknown family renders nothing", 0, (long) (p != nullptr));

    p = chat_render(CHAT_CHATML, 0, (struct chat_msg[]) {{"user", ""}});
    eq("zero messages = generation prompt only", "<|im_start|>assistant\n", p);
    free(p);

    /* --- truncation -------------------------------------------------- */
    size_t tok = 0;
    /* count_words counts spaces and newlines: the full chatml render is 12,
     * system + last user turn is 8. */
    p = chat_render_fit(CHAT_CHATML, N, CONV, 100, 10, count_words, nullptr, &tok);
    eq_int("fit: nothing dropped", 1, p != nullptr && strstr(p, "Hi<|im_end|>") != nullptr);
    free(p);
    /* Tight budget: the oldest user/assistant pair goes, system stays. */
    p = chat_render_fit(CHAT_CHATML, N, CONV, 10, 2, count_words, nullptr, &tok);
    eq_int("fit: reports the count", 8, (long) tok);
    eq("fit: drops oldest turns, keeps system",
       "<|im_start|>system\nBe brief.<|im_end|>\n"
       "<|im_start|>user\nCapital of France?<|im_end|>\n"
       "<|im_start|>assistant\n",
       p);
    free(p);
    p = chat_render_fit(CHAT_CHATML, N, CONV, 3, 1, count_words, nullptr, &tok);
    eq_int("fit: last message alone too big → nullptr", 0, (long) (p != nullptr));

    /* --- GGUF scan on a synthetic header ----------------------------- */
    {
        const char *path = "/tmp/geist-serve-test.gguf";
        FILE       *f    = fopen(path, "wb");
        uint32_t    v    = 3;
        uint64_t    nt = 0, nkv = 4, len;
        fwrite("GGUF", 1, 4, f);
        fwrite(&v, 4, 1, f);
        fwrite(&nt, 8, 1, f);
        fwrite(&nkv, 8, 1, f);
        /* general.architecture: string */
        const char *k1 = "general.architecture", *v1 = "llama";
        uint32_t    t;
        len = strlen(k1);
        fwrite(&len, 8, 1, f);
        fwrite(k1, 1, len, f);
        t = 8;
        fwrite(&t, 4, 1, f);
        len = strlen(v1);
        fwrite(&len, 8, 1, f);
        fwrite(v1, 1, len, f);
        /* tokenizer.ggml.tokens: array of 2 strings (must be skipped) */
        const char *k2 = "tokenizer.ggml.tokens";
        len            = strlen(k2);
        fwrite(&len, 8, 1, f);
        fwrite(k2, 1, len, f);
        t = 9;
        fwrite(&t, 4, 1, f);
        t = 8;
        fwrite(&t, 4, 1, f);
        uint64_t cnt = 2;
        fwrite(&cnt, 8, 1, f);
        len = 1;
        fwrite(&len, 8, 1, f);
        fwrite("a", 1, 1, f);
        len = 2;
        fwrite(&len, 8, 1, f);
        fwrite("bc", 1, 2, f);
        /* tokenizer.ggml.add_bos_token: bool false */
        const char *k3 = "tokenizer.ggml.add_bos_token";
        len            = strlen(k3);
        fwrite(&len, 8, 1, f);
        fwrite(k3, 1, len, f);
        t = 7;
        fwrite(&t, 4, 1, f);
        uint8_t b = 0;
        fwrite(&b, 1, 1, f);
        /* tokenizer.chat_template: string */
        const char *k4 = "tokenizer.chat_template", *v4 = "{{'<|im_start|>' + m.role}}";
        len = strlen(k4);
        fwrite(&len, 8, 1, f);
        fwrite(k4, 1, len, f);
        t = 8;
        fwrite(&t, 4, 1, f);
        len = strlen(v4);
        fwrite(&len, 8, 1, f);
        fwrite(v4, 1, len, f);
        fclose(f);

        struct gguf_meta g;
        eq_int("gguf scan ok", 1, gguf_read_meta(path, &g));
        eq("gguf template", v4, g.tpl);
        eq_int("gguf add_bos false", 0, g.add_bos);
        eq("gguf arch", "llama", g.arch);
        eq_int("gguf → chatml", CHAT_CHATML, chat_family_from_template(g.tpl));
        gguf_meta_free(&g);
        eq("ftype 15", "Q4_K_M", gguf_file_type_name(15));
        eq("ftype 37", "TQ2_0", gguf_file_type_name(37));
        eq("ftype 99", "unknown", gguf_file_type_name(99));

        /* Truncated file: must fail cleanly, not read garbage. */
        f = fopen(path, "wb");
        fwrite("GGUF", 1, 4, f);
        fwrite(&v, 4, 1, f);
        fclose(f);
        eq_int("gguf truncated → false", 0, gguf_read_meta(path, &g));
        eq_int("gguf truncated → no template", 0, (long) (g.tpl != nullptr));
        /* Not a GGUF at all. */
        f = fopen(path, "wb");
        fwrite("NOPE1234", 1, 8, f);
        fclose(f);
        eq_int("not gguf → false", 0, gguf_read_meta(path, &g));
        eq_int("missing file → false", 0, gguf_read_meta("/nonexistent.gguf", &g));
        remove(path);
    }

    printf(fails ? "test_template: %d FAILED\n" : "test_template: all passed\n", fails);
    return fails ? 1 : 0;
}
