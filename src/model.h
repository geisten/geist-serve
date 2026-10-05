/* model.h — one loaded GGUF as geistd holds it: the
 * engine handles, the name clients see, the header metadata and the stop
 * tokens. The chat format is geist-runtime's (#148). */
#pragma once

#include <geist.h>
#include <geist_util.h>

#include "gguf.h"

/* The engine caps every session at 4096 tokens today (geistlib#428). */
#define CTX_CAP 4096

struct model {
    struct geist_backend *be;
    struct geist_model   *m;
    const char           *path;      /* the GGUF as given on the command line */
    char                  name[128]; /* GGUF basename without .gguf */
    struct gguf_meta      meta;      /* template, add_bos, arch, size label, file type */
    geist_token_t         eos;
    geist_token_t         eot[6]; /* end-of-turn tokens by family */
    int                   n_eot;
};

/* After geist_model_load into mo->m: name, GGUF metadata and stop tokens. The caller creates be and m, since geistd picks a backend
 * and records lifecycle phases around both. */
void model_describe(struct model *mo, const char *path);
void model_close(struct model *mo);                          /* metadata, model, backend */
bool model_is_stop(const struct model *mo, geist_token_t t); /* EOS or end of turn */
