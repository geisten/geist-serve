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
    puts("output protocol: all splits, literal Markdown, Unicode, EOF, reset and 6 MB bounded "
         "discard passed");
}
