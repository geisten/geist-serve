/* template.h — chat messages → prompt string, per model family.
 *
 * The family is fingerprinted from the GGUF's tokenizer.chat_template
 * (markers, not Jinja), with general.architecture as the fallback. Each
 * family has one hand-written renderer; a template we do not recognise
 * is CHAT_UNKNOWN and the chat routes answer 501. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

enum chat_family {
    CHAT_UNKNOWN = 0,
    CHAT_GEMMA3, /* <start_of_turn>user … <end_of_turn> */
    CHAT_GEMMA4, /* <|turn>user … <turn|> */
    CHAT_CHATML, /* <|im_start|>user … <|im_end|>  (Qwen, SmolLM2) */
    CHAT_LLAMA3, /* <|start_header_id|>user<|end_header_id|> … <|eot_id|> */
    CHAT_BITNET, /* Human: … \n\nBITNETAssistant: … <|eot_id|> */
};

struct chat_msg {
    const char *role;    /* "system" | "user" | "assistant" (others → user) */
    const char *content; /* UTF-8, NUL-terminated */
};

enum chat_family chat_family_from_template(const char *tpl); /* nullptr ok */
enum chat_family chat_family_from_arch(const char *arch);
const char      *chat_family_name(enum chat_family f);

/* Render msgs[0..n) with the generation prompt appended (n may be 0, hence
 * no [static n]). malloc'd string,
 * nullptr for CHAT_UNKNOWN or out of memory. A leading system message is
 * rendered per family; further system messages are treated as user text. */
char *chat_render(enum chat_family f, size_t n, const struct chat_msg msgs[]);

/* Render, dropping the oldest non-system turns until
 * count(prompt) + reserve <= budget. The system message (index 0) always
 * survives. Returns nullptr when even the last message does not fit;
 * *out_tokens receives the count of the returned prompt. */
typedef size_t (*chat_count_fn)(void *ctx, const char *text);
char *chat_render_fit(enum chat_family      f,
                      size_t                n,
                      const struct chat_msg msgs[],
                      size_t                budget,
                      size_t                reserve,
                      chat_count_fn         count,
                      void                 *ctx,
                      size_t               *out_tokens);

/* GGUF header scan for the two tokenizer keys the server needs. Reads only
 * the key-value section; never touches tensor data. *tpl is malloc'd or
 * nullptr; *add_bos defaults to true when the key is absent. Returns false
 * when the file is not a GGUF. */
bool gguf_read_chat_meta(const char *path, char **tpl, bool *add_bos);
