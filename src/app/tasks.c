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
double app_answer_seconds(double rate, double first) {
    return rate > 0 ? (first > 0 ? first : 0) + APP_TYPICAL_ANSWER_TOKENS / rate : -1;
}
/* Known problems first, so a missing figure never hides a measured one. Memory
 * that does not fit, a pass rate below the limit or answers slower than usable
 * rule a model out; then missing data; then limits that leave it usable. */
struct app_judgement app_judge(enum app_fit resource, double seconds, unsigned passed, unsigned total,
                               struct app_limits limits) {
    bool known_quality = total > 0, known_speed = seconds >= 0;
    if (resource == APP_UNAVAILABLE)
        return (struct app_judgement) {APP_VERDICT_NOT_RECOMMENDED, "unavailable"};
    if (known_quality && passed < limits.reliable * total)
        return (struct app_judgement) {APP_VERDICT_NOT_RECOMMENDED, "unreliable"};
    if (known_speed && seconds > limits.usable_s)
        return (struct app_judgement) {APP_VERDICT_NOT_RECOMMENDED, "too_slow"};
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
