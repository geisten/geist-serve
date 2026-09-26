#include "../../src/app/tasks.h"
#include <assert.h>
#include <string.h>
int main(void) {
    assert(!app_task_find("missing") && !app_task_find(nullptr));
    assert(app_task_find("summary")->output_limit == 96);
    assert(app_task_find("home-assistant")->url[0] &&
           !app_task_find("home-assistant")->output_limit);
    for (enum app_fit f = APP_RECOMMENDED; f <= APP_UNAVAILABLE; f++) {
        assert(app_task_fit(f, APP_QUALITY_UNVERIFIED) != APP_RECOMMENDED);
        assert(app_task_fit(f, APP_QUALITY_FAILED) != APP_RECOMMENDED);
    }
    assert(app_task_fit(APP_RECOMMENDED, APP_QUALITY_PASSED) == APP_RECOMMENDED);
    assert(app_task_fit(APP_CONDITIONAL, APP_QUALITY_PASSED) == APP_CONDITIONAL);
    assert(app_task_fit(APP_UNAVAILABLE, APP_QUALITY_PASSED) == APP_UNAVAILABLE);
}
