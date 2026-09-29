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
