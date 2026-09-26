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
enum app_fit app_task_fit(enum app_fit resource, enum app_quality quality) {
    if (resource == APP_UNAVAILABLE)
        return APP_UNAVAILABLE;
    return resource == APP_RECOMMENDED && quality == APP_QUALITY_PASSED ? APP_RECOMMENDED
                                                                        : APP_CONDITIONAL;
}
