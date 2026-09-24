/* json.h — small growable string buffer with a JSON writer, and lookup
 * helpers over a jsmn-parsed request. Shared by geist-serve and geistd. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define JSMN_HEADER
#define JSMN_STRICT
#define JSMN_PARENT_LINKS
#include "jsmn.h"

struct sb {
    char  *p;
    size_t len, cap;
};

void sb_put(struct sb *b, size_t n, const char s[static n]);
void sb_puts(struct sb *b, const char *s);
void sb_printf(struct sb *b, const char *fmt, ...);
void sb_json_str(struct sb *b, size_t n, const char s[static n]);
void sb_free(struct sb *b);

#define JSON_TOK_CAP 4096

struct json {
    const char *src;
    jsmntok_t   tok[JSON_TOK_CAP];
    int         n;
};

int    json_parse(struct json *j, size_t n, const char src[static n]);
int    json_get(const struct json *j, int obj, const char *key);
bool   json_is_str(const struct json *j, int t);
char  *json_strdup(const struct json *j, int t);
double json_num(const struct json *j, int t, double dflt);
double json_clamp(const struct json *j, int t, double dflt, double lo, double hi, bool *bad);
bool   json_bool(const struct json *j, int t, bool dflt);
