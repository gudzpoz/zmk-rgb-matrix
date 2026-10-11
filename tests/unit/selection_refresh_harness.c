/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 * Platform seams and scene/trigger consumers; marked blocks are production code.
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
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
#define LOG_ERR(...) do { if (0) printf(__VA_ARGS__); assert(!"condition fault"); } while (0)
typedef _Atomic int atomic_t;
#define atomic_get(p) atomic_load(p)
#define atomic_set(p,v) atomic_exchange(p,v)
struct device { const char *name; const void *api, *config; void *data; };
static bool device_is_ready(const struct device *d) { return d != NULL; }
struct k_spinlock { int unused; };
typedef int k_spinlock_key_t;
static unsigned spin_held, mutex_held;
static int k_spin_lock(struct k_spinlock *l) {
    ARG_UNUSED(l); assert(!spin_held++); return 0;
}
static void k_spin_unlock(struct k_spinlock *l, int key) {
    ARG_UNUSED(l); ARG_UNUSED(key); assert(spin_held-- == 1);
}
static bool worker, isr;
static void kp_rgb_matrix_lock(void) { assert(!isr && !mutex_held++); }
static void kp_rgb_matrix_unlock(void) { assert(mutex_held-- == 1); }
struct k_work { void (*handler)(struct k_work *); bool queued; };
struct k_work_delayable { struct k_work work; };
#define K_WORK_DEFINE(name, fn) struct k_work name = {.handler = fn}
#define K_WORK_DELAYABLE_DEFINE(name, fn) struct k_work_delayable name = {.work = {.handler = fn}}
static int queue;
static void *kp_rgb_work_q(void) { return &queue; }
static void *k_work_queue_thread_get(void *q) { return q; }
static void *k_current_get(void) { return worker ? &queue : NULL; }
static bool k_is_in_isr(void) { return isr; }
static int64_t k_uptime_get(void) { return 100; }
static int k_work_submit_to_queue(void *q, struct k_work *w) {
    assert(q == &queue); w->queued = true; return 1;
}
static int k_work_reschedule_for_queue(void *q, struct k_work_delayable *w, int64_t delay) {
    assert(q == &queue && delay == 0); w->work.queued = true; return 1;
}
/* API */
static uint64_t kp_rgb_refresh_token;
static bool kp_rgb_refresh_pending;
static size_t kp_rgb_refresh_holds;
static unsigned flushes, completions;
static uint64_t last_completion;
static void zmk_rgb_matrix_flush(void) { flushes++; }
void kp_rgb_conditions_request_refresh(uint64_t token);
/* REFRESH */
static void kp_rgb_conditions_refreshed(uint64_t token) {
    assert(worker && !mutex_held && !spin_held);
    completions++; last_completion = token; apply_completion(token);
}
static void kp_rgb_overlay_conditions_init(void);
static void kp_rgb_triggers_init(void);
static void kp_rgb_triggers_begin(void);
static bool kp_rgb_overlay_refresh(void);
static void kp_rgb_overlay_dispatch(void);
static void kp_rgb_triggers_evaluate(int64_t now);
static void kp_rgb_triggers_quarantine(void);
/* COORDINATOR */

static struct device owners[2], effects[4];
static struct kp_rgb_selector selectors[2];
static struct kp_rgb_selector *kp_rgb_selector_overlay_state_locked(const struct device *d) {
    assert(mutex_held);
    for (unsigned i = 0; i < 2; i++) if (d == &owners[i]) return &selectors[i];
    return NULL;
}
struct zmk_rgb_selector_selection_changed { const struct device *selector; };
typedef struct zmk_rgb_selector_selection_changed zmk_event_t;
#define ZMK_EV_EVENT_BUBBLE 0
static const struct zmk_rgb_selector_selection_changed *as_zmk_rgb_selector_selection_changed(
    const zmk_event_t *event) { return event; }
static void raise_zmk_rgb_selector_selection_changed(struct zmk_rgb_selector_selection_changed event);
/* PUBLICATION */

static struct device conditions[3];
/* Only the provider's generated DT binding tables are replaced. */
#define DT_INST_FOREACH_STATUS_OKAY(fn) fn(0) fn(1) fn(2)
#define DT_DRV_INST(inst) inst
#define DEVICE_DT_GET(inst) (&conditions[inst])
#define cfg_0 selected_configs[0]
#define cfg_1 selected_configs[1]
#define cfg_2 selected_configs[2]
static const struct selected_cfg selected_configs[3];
/* PROVIDER */
static const struct selected_cfg selected_configs[3] = {
    {&owners[0], &effects[0]}, {&owners[0], &effects[1]}, {&owners[1], &effects[2]}
};
static unsigned samples[3];
static bool counted_sample(const struct device *d, int64_t now, int64_t *deadline) {
    assert(worker && !mutex_held && !spin_held && *deadline == KP_RGB_CONDITION_NEVER);
    samples[d - conditions]++;
    return sample(d, now, deadline);
}
static struct kp_rgb_condition_registration entries[3];
static const struct kp_rgb_condition_api selected_api = {.sample = counted_sample};
static struct device external = {.name = "third-party"};
static struct kp_rgb_condition_registration external_entry;
static struct device unused_external = {.name = "unused-third-party"};
static struct kp_rgb_condition_registration unused_entry;
static bool interrupt_trigger;
static bool external_value, interrupt_sample;
static unsigned external_samples;
static bool external_sample(const struct device *d, int64_t now, int64_t *deadline) {
    ARG_UNUSED(now); ARG_UNUSED(deadline);
    assert(d == &external && worker && !mutex_held);
    external_samples++;
    bool value = external_value;
    if (interrupt_sample) {
        interrupt_sample = false;
        isr = true;
        external_value = !external_value;
        kp_rgb_condition_invalidate(d);
        isr = false;
    }
    return value;
}
static const struct kp_rgb_condition_api external_api = {.sample = external_sample};
static bool gates[4], third_party, reenter, resume_in_event, trigger_mutation;
static unsigned events, dispatches;
static void mutate(unsigned i, const struct device *effect) {
    kp_rgb_matrix_lock();
    const struct device *previous = selectors[i].active_fx;
    selectors[i].active_fx = effect;
    kp_rgb_selector_publish_locked(&selectors[i], previous);
    kp_rgb_matrix_unlock();
}
static void resume(void) {
    /* Focused seam for the allowed transition's production refresh call. */
    kp_rgb_matrix_lock();
    kp_rgb_control_refresh_locked();
    kp_rgb_matrix_unlock();
}
static void raise_zmk_rgb_selector_selection_changed(struct zmk_rgb_selector_selection_changed event) {
    assert(worker && !mutex_held && !spin_held && kp_rgb_refresh_holds);
    unsigned i = event.selector == &owners[0] ? 0 : 1;
    assert(!selectors[i].notification_pending);
    events++;
    /* ZMK delivery is synchronous; exercise a third-party listener before the built-in one. */
    if (third_party) {
        external_value = true;
        kp_rgb_condition_invalidate(&external);
    }
    if (resume_in_event) {
        resume_in_event = false;
        uint64_t requested = refresh_requested;
        resume();
        assert(refresh_requested == requested && kp_rgb_refresh_pending);
    }
    if (reenter) {
        reenter = false;
        uint64_t token = kp_rgb_refresh_token;
        mutate(i, &effects[0]);
        assert(kp_rgb_refresh_holds == 2 && kp_rgb_refresh_token == token + 1);
        assert(selectors[i].notification_pending);
    }
    assert(listener(&event) == ZMK_EV_EVENT_BUBBLE);
}
static void kp_rgb_overlay_conditions_init(void) {
    for (unsigned i = 0; i < 3; i++) assert(kp_rgb_condition_require(&conditions[i]));
    assert(kp_rgb_condition_require(&external));
}
static void kp_rgb_triggers_init(void) {}
static void kp_rgb_triggers_begin(void) {}
static bool kp_rgb_overlay_refresh(void) {
    bool changed = false;
    for (unsigned i = 0; i < 4; i++) {
        const struct device *d = i < 3 ? &conditions[i] : &external;
        bool next = kp_rgb_condition_value(d);
        changed |= gates[i] != next;
        gates[i] = next;
    }
    return changed;
}
static void kp_rgb_overlay_dispatch(void) { dispatches++; }
static void kp_rgb_triggers_evaluate(int64_t now) {
    assert(now == 100 && worker && !mutex_held);
    /* Two scene/trigger consumers share one sampled selected-effect root. */
    bool first = kp_rgb_condition_value(&conditions[0]);
    bool second = kp_rgb_condition_value(&conditions[0]);
    assert(first == second && first == gates[0]);
    if (interrupt_trigger) {
        interrupt_trigger = false;
        isr = true;
        kp_rgb_condition_invalidate(&unused_external);
        isr = false;
    }
    if (trigger_mutation) {
        trigger_mutation = false;
        mutate(0, &effects[1]);
    }
}
static void kp_rgb_triggers_quarantine(void) { assert(!"unexpected feedback loop"); }
static void run(struct k_work *w) {
    assert(w->queued && !worker && !mutex_held && !spin_held);
    w->queued = false;
    worker = true; w->handler(w); worker = false;
    assert(!mutex_held && !spin_held);
}
static void control(void) { run(&control_work.work); }
static void notify(void) { run(&notification_work); }
static void drain(void) {
    unsigned budget = 32;
    while (control_work.work.queued || notification_work.queued) {
        assert(budget--);
        if (notification_work.queued) notify();
        if (control_work.work.queued) control();
    }
}
static void fixture(void) {
    for (unsigned i = 0; i < 2; i++) {
        selectors[i] = (struct kp_rgb_selector){.owner = &owners[i],
            .active_fx = &effects[i * 2], .publish_changes = true};
    }
    for (unsigned i = 0; i < 3; i++) {
        conditions[i] = (struct device){.name = "selected", .api = &selected_api,
            .config = &selected_configs[i]};
        kp_rgb_condition_register(&conditions[i], &entries[i]);
    }
    external.api = &external_api;
    kp_rgb_condition_register(&external, &external_entry);
    unused_external.api = &external_api;
    kp_rgb_condition_register(&unused_external, &unused_entry);
    kp_rgb_conditions_start();
    control();
    assert(!events && !completions && gates[0] && !gates[1] && gates[2]);
    for (unsigned i = 0; i < 3; i++) assert(samples[i] == 1);
    assert(external_samples == 1);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    fixture();
    const char *name = argv[1];
    if (!strcmp(name, "initial-shared")) {
        assert(dispatches == 1 && !control_work.work.queued);
        assert(!notification_work.queued && !kp_rgb_refresh_pending);
    } else if (!strcmp(name, "scoped-listener")) {
        /* No global refresh: isolate the production listener's per-selector routing. */
        zmk_event_t event = {.selector = &owners[0]};
        assert(listener(&event) == ZMK_EV_EVENT_BUBBLE);
        assert(atomic_get(&entries[0].pending) && atomic_get(&entries[1].pending));
        assert(!atomic_get(&entries[2].pending) && !atomic_get(&external_entry.pending));
        control();
        assert(samples[0] == 2 && samples[1] == 2 && samples[2] == 1);
        event.selector = &owners[1]; listener(&event); control();
        assert(samples[0] == 2 && samples[1] == 2 && samples[2] == 2);
        assert(external_samples == 1);
    } else if (!strcmp(name, "third-party")) {
        third_party = true;
        mutate(0, &effects[1]);
        assert(kp_rgb_refresh_holds == 1 && !refresh_requested);
        notify();
        assert(events == 1 && !kp_rgb_refresh_holds && kp_rgb_refresh_pending);
        assert(atomic_get(&external_entry.pending));
        assert(refresh_requested == kp_rgb_refresh_token);
        control();
        assert(!kp_rgb_refresh_pending && gates[3] && !gates[0] && gates[1]);
        assert(completions == 1 && last_completion == kp_rgb_refresh_token);
    } else if (!strcmp(name, "coalesced")) {
        mutate(0, &effects[0]);
        assert(!kp_rgb_refresh_token && !notification_work.queued);
        mutate(0, &effects[1]); mutate(0, &effects[0]);
        assert(kp_rgb_refresh_token == 2 && kp_rgb_refresh_holds == 1);
        mutate(1, &effects[3]);
        assert(kp_rgb_refresh_token == 3 && kp_rgb_refresh_holds == 2);
        assert(!refresh_requested);
        drain();
        assert(events == 2 && completions == 1 && last_completion == 3);
        assert(!kp_rgb_refresh_pending && gates[0] && !gates[1] && !gates[2]);
    } else if (!strcmp(name, "reentrant")) {
        reenter = true;
        mutate(0, &effects[1]);
        notify();
        assert(events == 2 && kp_rgb_refresh_token == 2 && !kp_rgb_refresh_holds);
        assert(refresh_requested == 2 && kp_rgb_refresh_pending);
        drain();
        assert(completions == 1 && !kp_rgb_refresh_pending && gates[0] && !gates[1]);
    } else if (!strcmp(name, "trigger-mutation")) {
        resume();
        uint64_t old = kp_rgb_refresh_token;
        trigger_mutation = true;
        control();
        /* The coordinator finishes the old pass, but the real matrix barrier rejects it. */
        assert(completions == 1 && last_completion == old);
        assert(kp_rgb_refresh_pending && kp_rgb_refresh_token == old + 1);
        assert(kp_rgb_refresh_holds == 1 && events == 0);
        unsigned before = flushes;
        apply_completion(kp_rgb_refresh_token);
        assert(kp_rgb_refresh_pending && flushes == before); /* matching token, still held */
        notify();
        apply_completion(old);
        assert(kp_rgb_refresh_pending); /* no holds, stale token */
        control();
        assert(completions == 2 && last_completion == old + 1 && !kp_rgb_refresh_pending);
        assert(!gates[0] && gates[1] && gates[2]);
    } else if (!strcmp(name, "resume-held")) {
        mutate(0, &effects[1]);
        resume();
        assert(kp_rgb_refresh_token == 2 && !refresh_requested);
        assert(kp_rgb_refresh_holds == 1 && !control_work.work.queued);
        resume_in_event = true;
        notify();
        assert(events == 1 && kp_rgb_refresh_token == 3 && refresh_requested == 3);
        assert(kp_rgb_refresh_pending && !completions);
        control();
        assert(!kp_rgb_refresh_pending && last_completion == 3 && gates[1]);
    } else if (!strcmp(name, "external-unused-interruption")) {
        resume(); interrupt_trigger = true;
        uint64_t token = kp_rgb_refresh_token;
        control();
        /* No live-root pending bit: external_pending alone must prevent completion. */
        assert(!pending() && atomic_get(&external_pending));
        assert(!completions && kp_rgb_refresh_pending && refresh_requested == token);
        assert(control_work.work.queued && !unused_entry.sampled);
        control();
        assert(completions == 1 && last_completion == token && !kp_rgb_refresh_pending);
        assert(!unused_entry.sampled && !control_work.work.queued);
    } else if (!strcmp(name, "external-interruption")) {
        resume(); interrupt_sample = true;
        uint64_t token = kp_rgb_refresh_token;
        control();
        assert(!completions && kp_rgb_refresh_pending && control_work.work.queued);
        assert(refresh_requested == token && !gates[3]);
        assert(external_samples == 2 && atomic_get(&external_entry.pending));
        control();
        assert(completions == 1 && last_completion == token && !kp_rgb_refresh_pending);
        assert(gates[3] && external_samples == 3 && !control_work.work.queued);
    } else assert(!"unknown case");
    drain();
    assert(!kp_rgb_refresh_holds && !mutex_held && !spin_held);
    printf("selection refresh %s central=%d: passed\n", name, CONFIG_ZMK_SPLIT_ROLE_CENTRAL);
    return 0;
}
