/* model.c — see model.h. */
#include "model.h"

#include <libgen.h>
#include <stdio.h>
#include <string.h>

void model_describe(struct model *mo, const char *path) {
    mo->path = path;
    /* Model name = GGUF basename without the extension, as /api/tags shows it. */
    char path_copy[1024];
    snprintf(path_copy, sizeof path_copy, "%s", path);
    snprintf(mo->name, sizeof mo->name, "%s", basename(path_copy));
    char *dot = strrchr(mo->name, '.');
    if (dot != nullptr && strcmp(dot, ".gguf") == 0)
        *dot = '\0';

    gguf_read_meta(path, &mo->meta);

    /* Stop tokens: EOS plus the end-of-turn markers of the families we
     * serve — some GGUFs set them as EOS, some do not (Gemma). */
    mo->eos = geist_model_eos_token(mo->m);
    for (const char **t = (const char *[]) {"<end_of_turn>",
                                            "<turn|>",
                                            "<|im_end|>",
                                            "<|eot_id|>",
                                            "<|end_of_text|>",
                                            nullptr};
         *t != nullptr && mo->n_eot < 6;
         t++) {
        geist_token_t id = geist_model_token_by_text(mo->m, *t);
        if (id != GEIST_TOKEN_NONE)
            mo->eot[mo->n_eot++] = id;
    }
}

void model_close(struct model *mo) {
    gguf_meta_free(&mo->meta);
    geist_model_destroy(mo->m);
    geist_backend_destroy(mo->be);
}

bool model_is_stop(const struct model *mo, geist_token_t t) {
    if (t == mo->eos)
        return true;
    for (int k = 0; k < mo->n_eot; k++)
        if (t == mo->eot[k])
            return true;
    return false;
}
