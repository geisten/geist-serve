#include "../../src/app/engine.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    struct json      *j = calloc(1, sizeof *j);
    struct app_engine e = {0};
    assert(j && app_engine_valid(&e));
    const char *cases[] = {
            "{}",
            "{\"geistlib\":{\"version\":null,\"revision\":null}}",
            "{\"geistlib\":{\"version\":\"0.11.0-dev\",\"source_state\":\"modified\"}}",
            "{\"geistlib\":{\"version\":123}}",
            "{\"geistlib\":[]}",
            "{\"geistlib\":{\"version\":\"<script>\"}}",
            "{\"geistlib\":{\"source_state\":\"clean\"}}",
            "{\"geistlib\":{\"revision\":\"deadbeef\"}}",
            "{\"payload_sha256\":true}"};
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
        assert(json_parse(j, strlen(cases[i]), cases[i]) >= 0);
        assert(app_engine_parse(&e, j, 0) == (i < 3));
    }
    memset(e.version, 'a', sizeof e.version);
    assert(!app_engine_valid(&e));
    e = (struct app_engine) {0};
    strcpy(e.version, "0.11.0");
    memset(e.revision, 'a', 40);
    strcpy(e.source_state, "clean");
    struct app_engine admitted = e;
    memset(e.revision, 'b', 40);
    assert(strcmp(admitted.revision, e.revision));
    char              text[1024];
    struct app_buffer b = {.data = text, .cap = sizeof text};
    app_engine_json(&b, &admitted);
    assert(!b.failed && json_parse(j, b.len, text) >= 0);
    assert(app_engine_parse(&e, j, 0) && !memcmp(&e, &admitted, sizeof e));
    free(j);
    return 0;
}
