#pragma once
#include "core.h"
#include "../json.h"
/* Public daemon provenance. Fixed bounds, no filesystem paths or user content. */
struct app_engine {
    char version[64], revision[65], source_state[16], payload_sha256[65];
};
bool app_engine_parse(struct app_engine *out, const struct json *j, int object);
bool app_engine_valid(const struct app_engine *engine);
void app_engine_json(struct app_buffer *b, const struct app_engine *engine);
