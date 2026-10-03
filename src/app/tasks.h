#pragma once
#include "core.h"
struct app_task {
    const char *id, *version, *title, *instruction, *url;
    unsigned    input_limit, output_limit;
};
const struct app_task *app_task_find(const char *id);
const char            *app_tasks_json(void);
/* Hardware feasibility is necessary but never sufficient evidence of quality. */
enum app_quality { APP_QUALITY_UNVERIFIED, APP_QUALITY_PASSED, APP_QUALITY_FAILED };
struct app_quality_record {
    const char *sha256, *task, *version, *language, *device;
    enum app_quality quality;
};
enum app_quality app_task_quality(const struct app_model *model, const struct app_task *task,
                                 const char *language, enum app_device device);
enum app_fit app_task_fit(enum app_fit resource, enum app_quality quality);
/* #103: one verdict per model for "good enough and fast enough on this computer". */
enum app_verdict { APP_VERDICT_GOOD, APP_VERDICT_USABLE, APP_VERDICT_NOT_RECOMMENDED, APP_VERDICT_UNKNOWN };
struct app_limits {
    double fast_s, usable_s; /* seconds per typical answer */
    double reliable;         /* reference test pass rate, 0..1 */
};
#define APP_LIMITS_DEFAULT ((struct app_limits) {.fast_s = 10, .usable_s = 30, .reliable = .9})
#define APP_TYPICAL_ANSWER_TOKENS 200 /* about 150 words */
struct app_judgement {
    enum app_verdict verdict;
    const char      *reason; /* stable code, see docs/UX-MODEL-MANAGER.md */
};
/* Seconds for a typical answer from a measured rate and first-token time; < 0 if unmeasured. */
double app_answer_seconds(double rate, double first);
/* seconds < 0: not measured; total 0: no reference test. */
struct app_judgement app_judge(enum app_fit resource, double seconds, unsigned passed, unsigned total,
                               struct app_limits limits);
const char *app_verdict_name(enum app_verdict verdict);
