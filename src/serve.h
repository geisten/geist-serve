/* serve.h — geist-serve's insides, shared by its API front ends: the
 * loaded model, the generation core that talks to libgeist, and the request
 * parsing the OpenAI (/v1) and Ollama (/api) routes have in common. */
#pragma once

#include "http.h"
#include "json.h"
#include "model.h"

#include <stdint.h>
#include <sys/types.h>
#include <time.h>

struct server {
    struct model          mo;
    off_t                 file_size;
    time_t                file_mtime;
    time_t                loaded_at;
    struct geist_session *tok; /* tiny session kept for tokenize-only counting */
};

/* ---- API front ends ---------------------------------------------------- */

void openai_route(struct server *sv, struct conn *c, struct req *r); /* /v1/... */
void ollama_route(struct server *sv, struct conn *c, struct req *r); /* /api/... */

/* ---- Generation -------------------------------------------------------- */

#define STOP_MAX 8
#define STOP_LEN 64

struct gen_opts {
    float    temperature, top_p;
    int      top_k;
    uint64_t seed;
    int      max_tokens; /* <= 0: fill the context */
    int      n_stop;
    char     stop[STOP_MAX][STOP_LEN];
};

struct gen_result {
    int         prompt_tokens, completion_tokens;
    const char *finish_reason; /* "stop" | "length" */
    uint64_t    prefill_ns, decode_ns;
};

/* Called with complete UTF-8 text as it is decoded; return false to stop
 * (the client hung up). */
typedef bool (*emit_fn)(void *ctx, size_t n, const char text[static n]);

/* Ollama's defaults; every editor client that sends nothing gets a
 * non-greedy chat. Engine note: with both top_k > 1 and top_p < 1 set,
 * the engine applies top_k and ignores top_p. */
struct gen_opts gen_opts_default(void);

/* Sampling fields shared by the OpenAI and Ollama request shapes: `obj` is
 * the object that carries them (the request itself, or Ollama's options). */
bool gen_opts_from_json(struct gen_opts *o, const struct json *j, int obj);

/* Returns 0, or an HTTP status: 400 prompt does not fit, 500 engine error. */
int generate(struct server         *sv,
             struct conn           *c,
             const struct gen_opts *o,
             const char            *prompt,
             emit_fn                emit,
             void                  *ctx,
             struct gen_result     *res,
             char                   err[static 256]);

/* ---- Request helpers --------------------------------------------------- */

#define MSG_CAP 256

/* The served model's name, with or without Ollama's ":latest"; absent is
 * fine (Cursor sends whatever was typed into its model box). */
bool model_name_ok(const struct server *sv, const struct json *j, int obj);

/* messages[] → chat_msg[]. content is a string, or an array of parts of
 * which the {"type":"text"} ones are concatenated (image parts ignored:
 * no vision in v1). All strings are malloc'd; free with free_messages. */
int parse_messages(
        const struct json *j, int arr, size_t cap, struct chat_msg out[static cap], size_t *n_out);
void free_messages(size_t n, const struct chat_msg msgs[]);

/* Render msgs into a prompt that fits the context, dropping the oldest
 * turns, with room kept for max_tokens (or a 512-token reserve). nullptr
 * when even the last message does not fit. */
char *chat_prompt(struct server *sv, size_t n, const struct chat_msg msgs[], int max_tokens);
