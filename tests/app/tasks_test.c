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
    /* #103: verdicts. Known problems beat missing data; missing data is never "good". */
    struct app_limits l = APP_LIMITS_DEFAULT;
    struct {
        enum app_fit     fit;
        double           seconds;
        unsigned         passed, total;
        enum app_verdict verdict;
        const char      *reason;
    } cases[] = {
            {APP_RECOMMENDED, 3, 146, 160, APP_VERDICT_GOOD, "good"},
            {APP_RECOMMENDED, 10, 144, 160, APP_VERDICT_GOOD, "good"},           /* both limits inclusive */
            {APP_RECOMMENDED, 3, 143, 160, APP_VERDICT_NOT_RECOMMENDED, "unreliable"},
            {APP_RECOMMENDED, 31, 160, 160, APP_VERDICT_NOT_RECOMMENDED, "too_slow"},
            {APP_RECOMMENDED, 30, 160, 160, APP_VERDICT_USABLE, "slow"},
            {APP_CONDITIONAL, 3, 160, 160, APP_VERDICT_USABLE, "tight_memory"},
            {APP_UNAVAILABLE, 3, 160, 160, APP_VERDICT_NOT_RECOMMENDED, "unavailable"},
            {APP_RECOMMENDED, -1, 160, 160, APP_VERDICT_UNKNOWN, "speed_unknown"},
            {APP_RECOMMENDED, 3, 0, 0, APP_VERDICT_UNKNOWN, "quality_unknown"},
            {APP_RECOMMENDED, -1, 31, 160, APP_VERDICT_NOT_RECOMMENDED, "unreliable"}, /* unmeasured, still known bad */
            {APP_RECOMMENDED, 99, 0, 0, APP_VERDICT_NOT_RECOMMENDED, "too_slow"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        struct app_judgement j = app_judge(cases[i].fit, cases[i].seconds, cases[i].passed, cases[i].total, l);
        assert(j.verdict == cases[i].verdict && !strcmp(j.reason, cases[i].reason));
    }
    assert(app_answer_seconds(0, 1) < 0 && app_answer_seconds(20, 0.5) == 10.5 && app_answer_seconds(40, -1) == 5);
    assert(!strcmp(app_verdict_name(APP_VERDICT_NOT_RECOMMENDED), "not_recommended"));
    /* Estimates scale the measured throughput by file size; no measurement, no estimate. */
    double rate[] = {20, 40, 0}, bytes[] = {4e9, 1e9, 2e9}, first[] = {.5, .3, 9};
    assert(app_estimate_seconds(2000000000, rate, bytes, first, 3) > 0);
    /* median throughput (80e9 + 40e9) / 2 = 60e9 B/s → 30 tokens/s for 2 GB; first (.3 + .5) / 2 */
    double e = app_estimate_seconds(2000000000, rate, bytes, first, 3);
    assert(e > .4 + 200 / 30. - 1e-9 && e < .4 + 200 / 30. + 1e-9);
    assert(app_estimate_seconds(2000000000, rate, bytes, first, 0) < 0 && app_estimate_seconds(0, rate, bytes, first, 3) < 0);
    /* #122: one recommendation. A good model to download beats a usable installed
     * one; known quality beats unknown; then installed, pass rate, speed. */
    struct app_candidate good_new = {APP_VERDICT_GOOD, .91, 5, false}, usable_installed = {APP_VERDICT_USABLE, .95, 12, true},
                         unknown_known = {APP_VERDICT_UNKNOWN, .91, -1, false}, unknown_unknown = {APP_VERDICT_UNKNOWN, -1, 3, true},
                         good_installed = {APP_VERDICT_GOOD, .90, 6, true};
    assert(app_candidate_better(good_new, usable_installed) && !app_candidate_better(usable_installed, good_new));
    assert(app_candidate_better(good_installed, good_new));       /* same verdict: installed first */
    assert(app_candidate_better(unknown_known, unknown_unknown)); /* quality known first */
    assert(app_candidate_better(usable_installed, unknown_known));
    struct app_candidate fast = {APP_VERDICT_GOOD, .9, 3, true}, slow = {APP_VERDICT_GOOD, .9, 8, true}, unmeasured = {APP_VERDICT_GOOD, .9, -1, true};
    assert(app_candidate_better(fast, slow) && app_candidate_better(slow, unmeasured) && !app_candidate_better(fast, fast));
    /* Gemma 4 E4B 147/160 at 29 s vs E2B 146/160 at 18 s: one answer is noise, speed decides. */
    struct app_candidate e4b = {APP_VERDICT_USABLE, 147. / 160, 29, false}, e2b = {APP_VERDICT_USABLE, 146. / 160, 18, false};
    assert(app_candidate_better(e2b, e4b) && !app_candidate_better(e4b, e2b));
    struct app_candidate clear = {APP_VERDICT_USABLE, .95, 29, false}, fast_weaker = {APP_VERDICT_USABLE, .90, 18, false};
    assert(app_candidate_better(clear, fast_weaker)); /* five points are not noise */
}
