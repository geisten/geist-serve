#include "../../src/app/output.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct sink {
    char   text[2048];
    size_t n;
};
static bool emit(void *ctx, const char *s) {
    struct sink *o = ctx;
    size_t       n = strlen(s);
    assert(o->n + n < sizeof o->text);
    memcpy(o->text + o->n, s, n + 1);
    o->n += n;
    assert(!strstr(o->text, "SECRET"));
    return true;
}
static void check(const char *input, const char *expected, bool reasoning, const char *format) {
    for (size_t split = 0; split <= strlen(input); ++split) {
        char left[1024];
        memcpy(left, input, split);
        left[split]         = 0;
        struct sink       s = {0};
        struct app_output p;
        app_output_init(&p, format);
        assert(app_output_feed(&p, left, emit, &s));
        assert(app_output_feed(&p, input + split, emit, &s));
        app_output_finish(&p);
        assert(!strcmp(s.text, expected));
        assert(p.reasoning == reasoning);
        assert(!app_output_feed(&p, "late", emit, &s));
    }
    struct sink       s = {0};
    struct app_output p;
    app_output_init(&p, format);
    for (const char *c = input; *c; ++c) {
        char chunk[2] = {*c, 0};
        assert(app_output_feed(&p, chunk, emit, &s));
    }
    app_output_finish(&p);
    assert(!strcmp(s.text, expected));
}
struct pair {
    struct sink answer;
    char        thought[1 << 21];
    size_t      thought_n, chunks;
    bool        boundaries;
};
static bool emit_answer(void *ctx, const char *s) {
    return emit(&((struct pair *) ctx)->answer, s);
}
/* Thinking chunks: plain text without markers, each a whole run of UTF-8 characters. */
static bool emit_thought(void *ctx, const char *s) {
    struct pair *o = ctx;
    size_t       n = strlen(s);
    assert(n > 0 && n <= 512 && o->thought_n + n < sizeof o->thought);
    if (((unsigned char) s[0] & 0xC0) == 0x80)
        o->boundaries = false;
    size_t tail = n, need = 0;
    while (tail && ((unsigned char) s[tail - 1] & 0xC0) == 0x80)
        --tail;
    if (tail) {
        unsigned char lead = (unsigned char) s[tail - 1];
        need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        if (n - (tail - 1) != need)
            o->boundaries = false;
    }
    memcpy(o->thought + o->thought_n, s, n + 1);
    o->thought_n += n;
    ++o->chunks;
    return true;
}
/* #93: with a thinking callback, the reasoning text arrives without markers, for every split. */
static void check_thinking(const char *input, const char *answer, const char *thought) {
    for (size_t split = 0; split <= strlen(input); ++split) {
        char left[1024];
        memcpy(left, input, split);
        left[split]               = 0;
        static struct pair  pair;
        pair                      = (struct pair) {.boundaries = true};
        struct app_output   p;
        app_output_init(&p, "think_tags");
        app_output_thinking(&p, emit_thought);
        assert(app_output_feed(&p, left, emit_answer, &pair));
        assert(app_output_feed(&p, input + split, emit_answer, &pair));
        app_output_finish(&p);
        assert(!strcmp(pair.answer.text, answer));
        assert(!strcmp(pair.thought, thought));
        /* Production feeds whole characters (app_utf8_feed); only such splits must keep boundaries. */
        if (((unsigned char) input[split] & 0xC0) != 0x80)
            assert(pair.boundaries);
    }
}
int main(void) {
    check("<think>SECRET Grüß 🌿</think>Answer", "Answer", true, "think_tags");
    check(" \n<think></think>\n<think>SECRET</think> Grüß 🌿", " Grüß 🌿", true, "think_tags");
    check("<think>SECRET </thin", "", true, "think_tags");
    check("<think><think>SECRET</think>SECRET</think>Answer", "Answer", true, "think_tags");
    check("<thi", "", false, "think_tags");
    check("Normal <think> literal", "Normal <think> literal", false, "think_tags");
    check("```html\n<think>literal</think>\n```",
          "```html\n<think>literal</think>\n```",
          false,
          "think_tags");
    check("`<think>` &lt;think&gt; \\<think>",
          "`<think>` &lt;think&gt; \\<think>",
          false,
          "think_tags");
    check("  α + β", "  α + β", false, "think_tags");
    check("<think>literal</think>", "<think>literal</think>", false, "none");
    check("</think>literal", "</think>literal", false, "think_tags");
    struct sink       s = {0};
    struct app_output p;
    app_output_init(&p, "think_tags");
    assert(app_output_feed(&p, "<think>", emit, &s));
    for (unsigned i = 0; i < 1000000; ++i)
        assert(app_output_feed(&p, "SECRET", emit, &s));
    assert(p.used < sizeof p.prefix && p.closing < 8 && s.n == 0);
    assert(app_output_feed(&p, "</think>done", emit, &s));
    assert(!strcmp(s.text, "done"));
    app_output_finish(&p);
    assert(p.prefix[0] == 0);
    app_output_init(&p, "think_tags");
    s = (struct sink) {0};
    for (unsigned i = 0; i < 32; ++i)
        assert(app_output_feed(&p, "<think>", emit, &s));
    for (unsigned i = 0; i < 32; ++i)
        assert(app_output_feed(&p, "</think>", emit, &s));
    assert(app_output_feed(&p, "SECRET", emit, &s));
    assert(!s.n);
    check_thinking("<think>Plan: grüß 🌿 </thinker> ok</think>Answer", "Answer", "Plan: grüß 🌿 </thinker> ok");
    check_thinking("<think>a<think>b</think>c</think>Done", "Done", "a<think>bc");
    check_thinking("<think>no close yet </th", "", "no close yet ");
    { /* 1 MB of mixed-width text: bounded chunks on character boundaries, nothing lost. */
        static struct pair big;
        big = (struct pair) {.boundaries = true};
        struct app_output p;
        app_output_init(&p, "think_tags");
        app_output_thinking(&p, emit_thought);
        assert(app_output_feed(&p, "<think>", emit_answer, &big));
        size_t sent = 0;
        for (unsigned i = 0; i < 60000; ++i) {
            assert(app_output_feed(&p, "Grüß 🌿 x", emit_answer, &big));
            sent += strlen("Grüß 🌿 x");
        }
        assert(app_output_feed(&p, "</think>fin", emit_answer, &big));
        assert(big.thought_n == sent && big.boundaries && big.chunks > 1000 && !strcmp(big.answer.text, "fin"));
    }
    puts("output protocol: all splits, literal Markdown, Unicode, EOF, reset and 6 MB bounded "
         "discard, thinking text (#93) passed");
}
