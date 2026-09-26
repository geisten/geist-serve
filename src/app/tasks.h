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
enum app_fit app_task_fit(enum app_fit resource, enum app_quality quality);
