/* json.c — see json.h. The one translation unit that compiles jsmn. */
#define JSMN_STRICT
#define JSMN_PARENT_LINKS
#include "jsmn.h"
#include "json.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ====================================================================== */
/* Small string buffer + JSON writer                                       */
/* ====================================================================== */

void sb_put(struct sb *b, size_t n, const char s[static n]) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n + 1)
            cap *= 2;
        char *np = realloc(b->p, cap);
        if (np == nullptr)
            return; /* dropped write; response ends short */
        b->p   = np;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

void sb_puts(struct sb *b, const char *s) {
    sb_put(b, strlen(s), s);
}

void sb_printf(struct sb *b, const char *fmt, ...) {
    char    tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (k > 0)
        sb_put(b, (size_t) k < sizeof tmp ? (size_t) k : sizeof tmp - 1, tmp);
}

/* JSON string literal, quotes included. Bytes are emitted as-is except the
 * JSON escapes; the generation loop only hands us complete UTF-8. */
void sb_json_str(struct sb *b, size_t n, const char s[static n]) {
    sb_put(b, 1, "\"");
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char) s[i];
        switch (c) {
        case '"':
            sb_puts(b, "\\\"");
            break;
        case '\\':
            sb_puts(b, "\\\\");
            break;
        case '\n':
            sb_puts(b, "\\n");
            break;
        case '\r':
            sb_puts(b, "\\r");
            break;
        case '\t':
            sb_puts(b, "\\t");
            break;
        default:
            if (c < 0x20)
                sb_printf(b, "\\u%04x", c);
            else
                sb_put(b, 1, (const char *) &c);
        }
    }
    sb_put(b, 1, "\"");
}

void sb_free(struct sb *b) {
    free(b->p);
    *b = (struct sb) {};
}

/* ====================================================================== */
/* JSON reader (jsmn) — lookup helpers over a parsed request body           */
/* ====================================================================== */

/* Returns the token count, or -1 on malformed / too large. */
int json_parse(struct json *j, size_t n, const char src[static n]) {
    jsmn_parser p;
    jsmn_init(&p);
    j->src = src;
    j->n   = jsmn_parse(&p, src, n, j->tok, JSON_TOK_CAP);
    if (j->n < 1 || j->tok[0].type != JSMN_OBJECT)
        j->n = -1;
    /* Anything after the root object ("{...}xxx") is a second top-level
     * token with no parent: refuse it rather than run the request. */
    for (int i = 1; i < j->n; i++)
        if (j->tok[i].parent < 0)
            j->n = -1;
    return j->n;
}

/* Direct child `key` of object `obj`; -1 when absent. Only direct children
 * match (parent links), so "options.stop" needs two hops. */
int json_get(const struct json *j, int obj, const char *key) {
    if (obj < 0 || j->tok[obj].type != JSMN_OBJECT)
        return -1;
    size_t kl = strlen(key);
    for (int i = obj + 1; i < j->n; i++) {
        const jsmntok_t *t = &j->tok[i];
        if (t->parent != obj)
            continue;
        if (t->type == JSMN_STRING && (size_t) (t->end - t->start) == kl &&
            memcmp(j->src + t->start, key, kl) == 0 && i + 1 < j->n)
            return i + 1;
    }
    return -1;
}

bool json_is_str(const struct json *j, int t) {
    return t >= 0 && j->tok[t].type == JSMN_STRING;
}

/* Unescape a JSON string token into a fresh malloc'd buffer. \uXXXX is
 * decoded to UTF-8 (surrogate pairs included); unknown escapes are copied. */
char *json_strdup(const struct json *j, int t) {
    if (!json_is_str(j, t))
        return nullptr;
    const char *s   = j->src + j->tok[t].start;
    size_t      n   = (size_t) (j->tok[t].end - j->tok[t].start);
    char       *out = malloc(n + 1);
    if (out == nullptr)
        return nullptr;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '\\' || i + 1 >= n) {
            out[o++] = s[i];
            continue;
        }
        char e = s[++i];
        switch (e) {
        case 'n':
            out[o++] = '\n';
            break;
        case 't':
            out[o++] = '\t';
            break;
        case 'r':
            out[o++] = '\r';
            break;
        case 'b':
            out[o++] = '\b';
            break;
        case 'f':
            out[o++] = '\f';
            break;
        case 'u': {
            if (i + 4 >= n)
                break;
            unsigned cp = (unsigned) strtoul(
                    (char[]) {s[i + 1], s[i + 2], s[i + 3], s[i + 4], 0}, nullptr, 16);
            i += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < n && s[i + 1] == '\\' && s[i + 2] == 'u') {
                unsigned lo = (unsigned) strtoul(
                        (char[]) {s[i + 3], s[i + 4], s[i + 5], s[i + 6], 0}, nullptr, 16);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 6;
                }
            }
            if (cp < 0x80) {
                out[o++] = (char) cp;
            } else if (cp < 0x800) {
                out[o++] = (char) (0xC0 | (cp >> 6));
                out[o++] = (char) (0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                out[o++] = (char) (0xE0 | (cp >> 12));
                out[o++] = (char) (0x80 | ((cp >> 6) & 0x3F));
                out[o++] = (char) (0x80 | (cp & 0x3F));
            } else {
                out[o++] = (char) (0xF0 | (cp >> 18));
                out[o++] = (char) (0x80 | ((cp >> 12) & 0x3F));
                out[o++] = (char) (0x80 | ((cp >> 6) & 0x3F));
                out[o++] = (char) (0x80 | (cp & 0x3F));
            }
            break;
        }
        default:
            out[o++] = e; /* \" \\ \/ */
        }
    }
    out[o] = '\0';
    return out;
}

double json_num(const struct json *j, int t, double dflt) {
    if (t < 0 || j->tok[t].type != JSMN_PRIMITIVE)
        return dflt;
    char c = j->src[j->tok[t].start];
    if (c != '-' && (c < '0' || c > '9'))
        return dflt;
    return strtod(j->src + j->tok[t].start, nullptr);
}

/* Number clamped to [lo, hi]; absent or NaN → dflt. A present field that is
 * not a number sets *bad: ignoring a string max_tokens would silently turn
 * a client bug into a 4096-token run, so the request is refused instead. */
double json_clamp(const struct json *j, int t, double dflt, double lo, double hi, bool *bad) {
    if (t >= 0 && (j->tok[t].type != JSMN_PRIMITIVE || j->src[j->tok[t].start] == 't' ||
                   j->src[j->tok[t].start] == 'f'))
        *bad = true;
    double v = json_num(j, t, dflt);
    if (isnan(v))
        v = dflt;
    return v < lo ? lo : v > hi ? hi : v;
}

bool json_bool(const struct json *j, int t, bool dflt) {
    if (t < 0 || j->tok[t].type != JSMN_PRIMITIVE)
        return dflt;
    return j->src[j->tok[t].start] == 't';
}
