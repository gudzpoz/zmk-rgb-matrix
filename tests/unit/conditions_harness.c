/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 * Host seams only; the runner inserts production API/storage and coordinator.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#define CONFIG_ZMK_SPLIT 1
#define IS_ENABLED(x) (x)
#define ARG_UNUSED(x) (void)(x)
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define K_NO_WAIT 0
#define K_MSEC(x) (x)
#define __ASSERT(test, ...) assert(test)
#define LOG_MODULE_DECLARE(...)
static unsigned errors;
#define LOG_ERR(...) do { errors++; if (0) printf(__VA_ARGS__); } while (0)
typedef _Atomic int atomic_t;
#define atomic_get(p) atomic_load(p)
#define atomic_set(p,v) atomic_exchange(p,v)
struct device { const char *name; const void *api; void *data; bool ready; };
static bool device_is_ready(const struct device *d) { return d && d->ready; }
struct k_spinlock { int unused; };
typedef int k_spinlock_key_t;
static unsigned held;
static int k_spin_lock(struct k_spinlock *l) { (void)l; assert(!held++); return 0; }
static void k_spin_unlock(struct k_spinlock *l, int key) { (void)l; (void)key; assert(held-- == 1); }
struct k_work { void (*handler)(struct k_work *); };
struct k_work_delayable { struct k_work work; bool queued, scheduled; int64_t due; };
#define K_WORK_DELAYABLE_DEFINE(name, fn) struct k_work_delayable name = {.work = {fn}}
static int64_t clock_ms;
static bool worker, isr;
static unsigned work_runs, schedules;
static int queue;
static void *zmk_workqueue_lowprio_work_q(void) { return &queue; }
static void *k_work_queue_thread_get(void *q) { return q; }
static void *k_current_get(void) { return worker ? &queue : NULL; }
static bool k_is_in_isr(void) { return isr; }
static int64_t k_uptime_get(void) { return clock_ms; }
static int k_work_reschedule_for_queue(void *q, struct k_work_delayable *w, int64_t delay) {
    assert(q == &queue && delay >= 0);
    schedules++;
    if (!delay) { w->queued = true; w->scheduled = false; }
    else { w->scheduled = true; w->due = clock_ms + delay; }
    return 1;
}
#include <limits.h>
#define CONFIG_APPLICATION_INIT_PRIORITY 90
#define BUILD_ASSERT(test, ...) _Static_assert(test, "startup ordering")
#define SETTINGS_STATIC_HANDLER_DEFINE_WITH_CPRIO(id, name, get, set, commit, exp, prio) \
    static int (*startup_hook)(void) = commit; \
    _Static_assert((prio) == INT_MAX, "settings must precede control")
#define SYS_INIT(fn, level, priority) \
    static int (*startup_hook)(void) = fn; \
    _Static_assert((priority) == 99, "matrix must precede control")
/* PRODUCTION_API */
static void kp_rgb_overlay_conditions_init(void);
static void kp_rgb_triggers_init(void);
static void kp_rgb_triggers_begin(void);
static bool kp_rgb_overlay_refresh(void);
static void kp_rgb_overlay_dispatch(void);
static void zmk_rgb_matrix_flush(void);
static void kp_rgb_triggers_evaluate(int64_t now);
static void kp_rgb_triggers_quarantine(void);
/* PRODUCTION_COORDINATOR */

#define N 6
struct source {
    bool value, composite, short_circuit, undeclared, inject_isr, bad_metadata;
    int invalid_deadline;
    int64_t deadline, period, sampled_at;
    unsigned calls;
    const struct device *deps[N];
    size_t count;
};
static struct source sources[N];
static struct device devices[N];
static struct kp_rgb_condition_registration entries[N];
static struct kp_rgb_condition_api apis[N];
static bool roots[N], gate, quarantined;
static unsigned refreshes, dispatches, repaints, evaluations, begins, quarantines;
static unsigned feedback_mode, feedback_actions[N];
static const struct device *gate_root;

static const struct device *const *dependencies(const struct device *dev, size_t *count) {
    struct source *s = dev->data;
    *count = s->count;
    return s->bad_metadata ? NULL : s->deps;
}
static bool sample(const struct device *dev, int64_t now, int64_t *next_wakeup_ms) {
    assert(worker && !isr && !held && now == clock_ms);
    assert(next_wakeup_ms && *next_wakeup_ms == KP_RGB_CONDITION_NEVER);
    struct source *s = dev->data;
    s->calls++;
    s->sampled_at = now;
    bool active = s->value;
    if (s->period) *next_wakeup_ms = (now / s->period + 1) * s->period;
    else if (s->deadline != KP_RGB_CONDITION_NEVER) *next_wakeup_ms = s->deadline;
    if (s->invalid_deadline) *next_wakeup_ms = now - (s->invalid_deadline - 1);
    if (s->composite) {
        active = true;
        for (size_t i = 0; i < s->count; i++) {
            unsigned before = ((struct source *)s->deps[i]->data)->calls;
            bool value = kp_rgb_condition_value(s->deps[i]);
            assert(value == kp_rgb_condition_value(s->deps[i]));
            assert(before == ((struct source *)s->deps[i]->data)->calls);
            active &= value;
            if (s->short_circuit && !active) break;
        }
    }
    if (s->undeclared) (void)kp_rgb_condition_value(&devices[5]);
    if (s->inject_isr) {
        s->inject_isr = false;
        /* Interrupt after the old snapshot: not an illegal synchronous provider notification. */
        isr = true;
        s->value = !s->value;
        kp_rgb_condition_invalidate(dev);
        isr = false;
    }
    return active;
}
static void kp_rgb_overlay_conditions_init(void) {
    for (size_t i = 0; i < N; i++) if (roots[i]) (void)kp_rgb_condition_require(&devices[i]);
}
static void kp_rgb_triggers_init(void) {}
static void kp_rgb_triggers_begin(void) { assert(worker && !held); begins++; }
static bool kp_rgb_overlay_refresh(void) {
    assert(worker && !held);
    refreshes++;
    bool next = gate_root && kp_rgb_condition_valid(gate_root) && kp_rgb_condition_value(gate_root);
    bool changed = next != gate;
    gate = next;
    return changed;
}
static void kp_rgb_overlay_dispatch(void) { assert(worker && !held); dispatches++; }
static void zmk_rgb_matrix_flush(void) { assert(worker && !held); repaints++; }
static void kp_rgb_triggers_evaluate(int64_t now) {
    assert(worker && !held && now == clock_ms);
    evaluations++;
    for (size_t i = 0; i < N; i++) {
        if (roots[i] && !entries[i].fault) assert(kp_rgb_condition_valid(&devices[i]));
        if (roots[i] && kp_rgb_condition_valid(&devices[i])) {
            assert(sources[i].calls && sources[i].sampled_at <= now);
            assert(kp_rgb_condition_value(&devices[i]) == entries[i].value);
        }
    }
    /* Exercise coordinator feedback bounds; real trigger edge/action tests live separately. */
    if (feedback_mode && !quarantined) {
        size_t target = feedback_mode == 1 ? 0 : (evaluations - 1) % 2;
        feedback_actions[target]++;
        sources[target].value = !sources[target].value;
        kp_rgb_condition_invalidate(&devices[target]);
    }
}
static void kp_rgb_triggers_quarantine(void) {
    assert(evaluations == 8 && !held);
    quarantines++;
    quarantined = true;
}
static void fixture(void) {
    for (size_t i = 0; i < N; i++) {
        sources[i].deadline = KP_RGB_CONDITION_NEVER;
        apis[i] = (struct kp_rgb_condition_api){.sample = sample, .dependencies = dependencies};
        devices[i] = (struct device){.name = "fake", .api = &apis[i], .data = &sources[i], .ready = true};
        kp_rgb_condition_register(&devices[i], &entries[i]);
    }
    roots[0] = true;
    gate_root = &devices[0];
}
static void run_one(void) {
    assert(control_work.queued && !worker);
    control_work.queued = false;
    worker = true;
    work_runs++;
    control_work.work.handler(&control_work.work);
    worker = false;
}
static void ready(void) {
    unsigned budget = 32;
    while (control_work.queued) { assert(budget--); run_one(); }
}
static void advance(int64_t to) {
    assert(to >= clock_ms);
    clock_ms = to;
    if (control_work.scheduled && control_work.due <= clock_ms) {
        control_work.scheduled = false;
        control_work.queued = true;
    }
    ready();
}
static void start(void) {
#if CONFIG_KEYPAW_RGB_CONTROL_AUTO_START
    assert(startup_hook() == 0);
#else
    zmk_rgb_matrix_start();
#endif
    ready();
}
static void link_to(size_t parent, size_t child) {
    sources[parent].composite = true;
    sources[parent].deps[sources[parent].count++] = &devices[child];
}
int main(int argc, char **argv) {
    assert(argc == 2);
    const char *name = argv[1];
    fixture();
    if (!strcmp(name, "startup")) {
        sources[0].value = true;
        kp_rgb_condition_invalidate(&devices[0]);
        assert(!evaluations && !schedules);
        start();
        assert(sources[0].calls == 1 && evaluations == 1 && repaints == 1 && gate);
        zmk_rgb_matrix_start();
        start();
        advance(1000000);
        assert(work_runs == 1 && sources[0].calls == 1 && !control_work.scheduled);
        for (unsigned i = 0; i < 100; i++) kp_rgb_condition_invalidate(&devices[0]);
        ready();
        assert(sources[0].calls == 2 && evaluations == 2 && repaints == 1);
    } else if (!strcmp(name, "dag")) {
        link_to(0, 1); link_to(0, 2); link_to(1, 3); link_to(2, 3);
        sources[3].value = true;
        start();
        for (size_t i = 0; i < 4; i++) assert(sources[i].calls == 1 && entries[i].value);
        assert(!sources[4].calls && !sources[5].calls && gate);
        sources[3].value = false;
        kp_rgb_condition_invalidate(&devices[3]); ready();
        for (size_t i = 0; i < 4; i++) assert(sources[i].calls == 2 && !entries[i].value);
        assert(!gate && repaints == 2);
    } else if (!strcmp(name, "deadline")) {
        sources[0].period = 100;
        start();
        assert(control_work.scheduled && control_work.due == 100);
        advance(99); assert(sources[0].calls == 1);
        advance(100); assert(sources[0].calls == 2 && control_work.due == 200);
        advance(950); assert(sources[0].calls == 3 && control_work.due == 1000);
        assert(sources[0].sampled_at == 950 && !repaints);
    } else if (!strcmp(name, "deadline-reset")) {
        sources[0].deadline = 100;
        start(); advance(10);
        assert(!gate && control_work.due == 100);
        sources[0].deadline = KP_RGB_CONDITION_NEVER;
        sources[0].value = true;
        kp_rgb_condition_invalidate(&devices[0]); ready();
        assert(gate && !entries[0].fault && entries[0].deadline == KP_RGB_CONDITION_NEVER);
        assert(!control_work.scheduled && sources[0].calls == 2);
        advance(1000); assert(sources[0].calls == 2);
        sources[0].deadline = 1100;
        sources[0].value = false;
        kp_rgb_condition_invalidate(&devices[0]); ready();
        assert(!gate && control_work.scheduled && control_work.due == 1100);
        sources[0].deadline = KP_RGB_CONDITION_NEVER;
        advance(1100);
        assert(sources[0].calls == 4 && !entries[0].fault);
        assert(entries[0].deadline == KP_RGB_CONDITION_NEVER && !control_work.scheduled);
        advance(2000); assert(sources[0].calls == 4);
    } else if (!strcmp(name, "same-value")) {
        sources[0].deadline = 100;
        start(); advance(10);
        sources[0].deadline = 20;
        kp_rgb_condition_invalidate(&devices[0]); ready();
        assert(control_work.due == 20 && sources[0].calls == 2 && !repaints);
        sources[0].deadline = 200;
        advance(20);
        assert(sources[0].calls == 3 && control_work.due == 200 && !repaints);
        sources[0].value = true;
        kp_rgb_condition_invalidate(&devices[0]); ready();
        assert(repaints == 1 && gate);
    } else if (!strcmp(name, "isr")) {
        sources[0].inject_isr = true;
        kp_rgb_conditions_start(); run_one();
        assert(sources[0].calls == 1 && !entries[0].value && !entries[0].fault);
        assert(control_work.queued);
        ready();
        assert(sources[0].calls == 2 && entries[0].value && gate && repaints == 1);
        advance(10000); assert(work_runs == 2);
    } else if (!strcmp(name, "cycle")) {
        link_to(0, 1); link_to(1, 2); link_to(2, 1);
        start();
        assert(!kp_rgb_condition_valid(&devices[0]) && errors && !repaints);
        assert(!sources[0].calls && !sources[1].calls && !sources[2].calls);
    } else if (!strcmp(name, "missing")) {
        struct device missing = {.name = "missing", .ready = true};
        sources[0].deps[sources[0].count++] = &missing;
        start(); assert(entries[0].fault && !sources[0].calls && errors);
    } else if (!strcmp(name, "undeclared")) {
        sources[0].undeclared = true;
        start(); assert(entries[0].fault && errors && !gate && !repaints);
    } else if (!strcmp(name, "invalid-deadline")) {
        roots[1] = true;
        sources[0].invalid_deadline = 1;
        sources[1].invalid_deadline = 2;
        start(); assert(entries[0].fault && entries[1].fault && errors && !control_work.scheduled);
        advance(1000); assert(sources[0].calls == 1 && sources[1].calls == 1);
    } else if (!strcmp(name, "scope")) {
        link_to(0, 1); link_to(1, 2);
        apis[2].scope = KP_RGB_CONDITION_CENTRAL_ONLY;
        sources[2].value = true;
        start();
        if (CONFIG_ZMK_SPLIT_ROLE_CENTRAL) assert(gate && sources[2].calls == 1 && !errors);
        else assert(!gate && !sources[2].calls && entries[0].fault && errors);
    } else if (!strcmp(name, "unused-scope")) {
        apis[5].scope = KP_RGB_CONDITION_CENTRAL_ONLY;
        start(); assert(!errors && !sources[5].calls && !entries[5].fault && entries[0].sampled);
    } else if (!strcmp(name, "unready")) {
        link_to(0, 1); devices[1].ready = false;
        start(); assert(entries[0].fault && entries[1].fault && !sources[1].calls && errors);
    } else if (!strcmp(name, "metadata")) {
        sources[0].count = 1; sources[0].bad_metadata = true;
        start(); assert(entries[0].fault && errors && !sources[0].calls);
    } else if (!strcmp(name, "short-circuit")) {
        link_to(0, 1); link_to(0, 2); sources[0].short_circuit = true;
        sources[2].period = 100;
        start(); assert(!gate && sources[2].calls == 1 && control_work.due == 100);
        advance(100); assert(sources[0].calls == 2 && sources[1].calls == 1 && sources[2].calls == 2);
    } else if (!strcmp(name, "self-feedback") || !strcmp(name, "multi-feedback")) {
        feedback_mode = !strcmp(name, "self-feedback") ? 1 : 2;
        roots[1] = feedback_mode == 2;
        kp_rgb_conditions_start(); run_one();
        assert(evaluations == 8 && quarantines == 1 && quarantined && begins == 1);
        assert(feedback_actions[0] == (feedback_mode == 1 ? 8 : 4));
        if (feedback_mode == 2) assert(feedback_actions[1] == 4);
        ready();
        assert(evaluations <= 9 && quarantines == 1);
        unsigned before = evaluations;
        advance(10000); assert(evaluations == before);
        kp_rgb_condition_invalidate(&devices[0]); ready();
        assert(quarantines == 1 && feedback_actions[0] == (feedback_mode == 1 ? 8 : 4));
    } else assert(!"unknown case");
    assert(refreshes == dispatches && refreshes == evaluations);
    assert(!held);
    printf("conditions %s central=%d: passed\n", name, CONFIG_ZMK_SPLIT_ROLE_CENTRAL);
}
