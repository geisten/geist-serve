#include "tasks.h"
#include <string.h>
#include "../../build/app_tasks.h"
const struct app_task *app_task_find(const char *id) {
    if (!id)
        return nullptr;
    for (size_t i = 0; i < sizeof task_registry / sizeof *task_registry; i++)
        if (!strcmp(id, task_registry[i].id))
            return &task_registry[i];
    return nullptr;
}
const char *app_tasks_json(void) {
    return task_catalog;
}
enum app_quality app_task_quality(const struct app_model *model, const struct app_task *task,
                                 const char *language, enum app_device device) {
    if (!model || !task || !language)
        return APP_QUALITY_UNVERIFIED;
    const char *name = device == APP_APPLE_SILICON ? "apple-silicon" : device == APP_PI5 ? "pi5" : "unknown";
    for (const struct app_quality_record *r = quality_registry; r->sha256; r++)
        if (!strcmp(r->sha256, model->sha256) && !strcmp(r->task, task->id) &&
            !strcmp(r->version, task->version) && !strcmp(r->language, language) &&
            !strcmp(r->device, name))
            return r->quality;
    return APP_QUALITY_UNVERIFIED;
}
enum app_fit app_task_fit(enum app_fit resource, enum app_quality quality) {
    if (resource == APP_UNAVAILABLE)
        return APP_UNAVAILABLE;
    return resource == APP_RECOMMENDED && quality == APP_QUALITY_PASSED ? APP_RECOMMENDED
                                                                        : APP_CONDITIONAL;
}
static double median(double *v, size_t n) {
    for (size_t i = 1; i < n; i++) /* insertion sort: n is at most the catalog size */
        for (size_t k = i; k && v[k - 1] > v[k]; k--) {
            double t = v[k];
            v[k] = v[k - 1], v[k - 1] = t;
        }
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}
/* ponytail: one bandwidth figure per processor; native ternary kernels (I2_S,
 * PQ2_0) decode faster per byte, so their estimates are conservative. */
double app_estimate_seconds(uint64_t bytes, const double *rate, const double *model_bytes, const double *first, size_t n) {
    double throughput[APP_MODEL_COUNT], firsts[APP_MODEL_COUNT];
    size_t k = 0;
    for (size_t i = 0; i < n && k < APP_MODEL_COUNT; i++)
        if (rate[i] > 0 && model_bytes[i] > 0)
            throughput[k] = rate[i] * model_bytes[i], firsts[k++] = first[i] > 0 ? first[i] : 0;
    if (!k || !bytes)
        return -1;
    return app_answer_seconds(median(throughput, k) / (double) bytes, median(firsts, k));
}
double app_answer_seconds(double rate, double first) {
    return rate > 0 ? (first > 0 ? first : 0) + APP_TYPICAL_ANSWER_TOKENS / rate : -1;
}
/* Known problems first, so a missing figure never hides a measured one. Memory
 * that does not fit, a pass rate below the limit or answers slower than usable
 * rule a model out; then missing data; then limits that leave it usable. */
struct app_judgement app_judge(enum app_fit resource, double seconds, bool estimated, unsigned passed, unsigned total,
                               struct app_limits limits) {
    bool known_quality = total > 0, known_speed = seconds >= 0;
    if (resource == APP_UNAVAILABLE)
        return (struct app_judgement) {APP_VERDICT_NOT_RECOMMENDED, "unavailable"};
    if (known_quality && passed < limits.reliable * total)
        return (struct app_judgement) {APP_VERDICT_NOT_RECOMMENDED, "unreliable"};
    /* #133: an estimate alone never rules a model out; it says "probably". */
    if (known_speed && seconds > limits.usable_s)
        return estimated ? (struct app_judgement) {APP_VERDICT_UNKNOWN, "probably_too_slow"}
                         : (struct app_judgement) {APP_VERDICT_NOT_RECOMMENDED, "too_slow"};
    if (!known_quality)
        return (struct app_judgement) {APP_VERDICT_UNKNOWN, "quality_unknown"};
    if (!known_speed)
        return (struct app_judgement) {APP_VERDICT_UNKNOWN, "speed_unknown"};
    if (seconds > limits.fast_s)
        return (struct app_judgement) {APP_VERDICT_USABLE, "slow"};
    if (resource == APP_CONDITIONAL)
        return (struct app_judgement) {APP_VERDICT_USABLE, "tight_memory"};
    return (struct app_judgement) {APP_VERDICT_GOOD, "good"};
}
const char *app_verdict_name(enum app_verdict verdict) {
    static const char *const names[] = {"good", "usable", "not_recommended", "unknown"};
    return names[verdict];
}
static int verdict_rank(enum app_verdict v) {
    return v == APP_VERDICT_GOOD ? 0 : v == APP_VERDICT_USABLE ? 1 : v == APP_VERDICT_UNKNOWN ? 2 : 3;
}
bool app_candidate_better(struct app_candidate a, struct app_candidate b) {
    if (verdict_rank(a.verdict) != verdict_rank(b.verdict))
        return verdict_rank(a.verdict) < verdict_rank(b.verdict);
    if ((a.rate >= 0) != (b.rate >= 0))
        return a.rate >= 0;
    if (a.installed != b.installed)
        return a.installed;
    /* One or two answers more in 160 is noise next to a much faster answer. */
    if (a.rate - b.rate > APP_RATE_TIE || b.rate - a.rate > APP_RATE_TIE)
        return a.rate > b.rate;
    if ((a.seconds >= 0) != (b.seconds >= 0))
        return a.seconds >= 0;
    return a.seconds < b.seconds;
}
