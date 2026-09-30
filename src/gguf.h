/* gguf.h — GGUF header scan for what the servers report and render with.
 * Reads only the key-value section; never touches tensor data. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Strings are malloc'd or nullptr; add_bos defaults to true; numbers to 0. Returns
 * false when the file is not a GGUF (out is still zeroed/valid). */
struct gguf_meta {
    char    *tpl;            /* tokenizer.chat_template */
    bool     add_bos;        /* tokenizer.ggml.add_bos_token */
    char    *arch;           /* general.architecture */
    char    *size_label;     /* general.size_label, e.g. "360M" */
    uint32_t file_type;      /* general.file_type (llama_ftype enum) */
    uint32_t context_length; /* <arch>.context_length */
};
bool        gguf_read_meta(const char *path, struct gguf_meta *out);
void        gguf_meta_free(struct gguf_meta *m);
const char *gguf_file_type_name(uint32_t file_type); /* "Q4_K_M", "unknown" */
