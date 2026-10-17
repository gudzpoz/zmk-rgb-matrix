/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT test_rgb_condition_contract
#include "native_contract.h"
#include <zephyr/irq_offload.h>
#include <zephyr/init.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>
#include <limits.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/rgb_persist.h>
#include <zmk/rgb_selector.h>
#include <zmk/events/rgb_selector_selection_changed.h>
#include "../../../../src/rgb_matrix_internal.h"

#define DEV(label) DEVICE_DT_GET(DT_NODELABEL(label))
#define KP_TEST_NODE_TOKEN(node) STRINGIFY(DT_NODE_FULL_NAME_TOKEN(node))
#define KP_TEST_SELECTOR_A_STATE \
    "keypaw/rgb_matrix/selectors/" KP_TEST_NODE_TOKEN(DT_NODELABEL(selector_a)) "/state"
struct source {
    atomic_t value, calls, undeclared, notify, pause;
    const struct device *deps[3];
    size_t dep_count;
    bool bad_metadata, short_circuit;
};
struct source_config { const struct device *selector, *effect; };
K_SEM_DEFINE(sample_entered, 0, 1);
K_SEM_DEFINE(sample_release, 0, 1);
static atomic_t notifications, notifications_b, reenter, irq_calls;
static const struct device *irq_device;

static void interrupt(const void *arg) {
    ARG_UNUSED(arg);
    KP_CHECK(k_is_in_isr());
    atomic_inc(&irq_calls);
    kp_rgb_condition_invalidate(irq_device);
}
static bool sample(const struct device *dev) {
    struct source *s = dev->data;
    const struct source_config *cfg = dev->config;
    KP_CHECK(!k_is_in_isr());
    KP_CHECK(k_current_get() == k_work_queue_thread_get(kp_rgb_work_q()));
    atomic_inc(&s->calls);
    bool value = atomic_get(&s->value);
    if (atomic_cas(&s->pause, 1, 0)) {
        k_sem_give(&sample_entered);
        KP_CHECK(k_sem_take(&sample_release, K_SECONDS(2)) == 0);
    }
    if (cfg->selector)
        value = zmk_rgb_selector_get_selected(cfg->selector) == cfg->effect;
    for (size_t i = 0; i < s->dep_count; i++) {
        if (s->short_circuit && !value) break;
        value &= kp_rgb_condition_value(s->deps[i]);
    }
    if (atomic_get(&s->undeclared))
        (void)kp_rgb_condition_value(DEV(cc_unused));
    if (atomic_cas(&s->notify, 1, 0))
        kp_rgb_condition_invalidate(dev);
    return value;
}
static const struct device *const *dependencies(const struct device *dev, size_t *count) {
    struct source *s = dev->data;
    *count = s->dep_count;
    return s->bad_metadata ? NULL : s->deps;
}
#define DEFINE(inst) \
    static struct source source_##inst; \
    static struct kp_rgb_condition_registration registration_##inst; \
    static const struct source_config config_##inst = { \
        .selector = COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, selected_selector), \
            (DEVICE_DT_GET(DT_INST_PHANDLE(inst, selected_selector))), (NULL)), \
        .effect = COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, selected_effect), \
            (DEVICE_DT_GET(DT_INST_PHANDLE(inst, selected_effect))), (NULL)), \
    }; \
    static const struct kp_rgb_condition_api api_##inst = { \
        .scope = DT_INST_PROP(inst, invalid_scope) ? 99 : \
            (DT_INST_PROP(inst, central_only) ? KP_RGB_CONDITION_CENTRAL_ONLY : KP_RGB_CONDITION_ANY_SIDE), \
        .sample = sample, .dependencies = dependencies, \
    }; \
    static int init_##inst(const struct device *dev) { \
        if (!DT_INST_PROP(inst, unregistered)) \
            kp_rgb_condition_register(dev, &registration_##inst); \
        return DT_INST_PROP(inst, unready) ? -ENODEV : 0; \
    } \
    DEVICE_DT_INST_DEFINE(inst, init_##inst, NULL, &source_##inst, &config_##inst, \
        POST_KERNEL, KP_RGB_DEVICE_INIT_PRIORITY, &api_##inst);
DT_INST_FOREACH_STATUS_OKAY(DEFINE)

/* Deliberately a real device without condition registration. */
static const struct kp_rgb_condition_api missing_api = {
    .scope = KP_RGB_CONDITION_ANY_SIDE, .sample = sample,
};
DEVICE_DEFINE(cc_missing, "cc_missing", NULL, NULL, NULL, NULL,
              POST_KERNEL, KP_RGB_DEVICE_INIT_PRIORITY, &missing_api);

static struct source *src(const struct device *dev) { return dev->data; }
static unsigned calls(const struct device *dev) { return atomic_get(&src(dev)->calls); }
static void set(const struct device *dev, bool value) {
    atomic_set(&src(dev)->value, value);
    kp_rgb_condition_invalidate(dev);
}
static bool gate(const struct device *dev) {
    kp_rgb_matrix_lock();
    bool value = kp_rgb_overlay_gate(dev);
    kp_rgb_matrix_unlock();
    return value;
}
static void link(const struct device *parent, const struct device *child) {
    struct source *s = src(parent);
    KP_CHECK(s->dep_count < ARRAY_SIZE(s->deps));
    s->deps[s->dep_count++] = child;
}
K_SEM_DEFINE(drained, 0, 1);
static void drain_handler(struct k_work *work) { ARG_UNUSED(work); k_sem_give(&drained); }
K_WORK_DEFINE(drain_work, drain_handler);
static void drain(void) {
    for (unsigned n = 0; n < 3; n++) {
        KP_CHECK(k_work_submit_to_queue(kp_rgb_work_q(), &drain_work) >= 0);
        KP_CHECK(k_sem_take(&drained, K_SECONDS(2)) == 0);
        k_msleep(1);
    }
}
static void external_invalidation_handler(struct k_work *work) {
    ARG_UNUSED(work);
    KP_CHECK(k_current_get() != k_work_queue_thread_get(kp_rgb_work_q()));
    atomic_set(&src(DEV(cc_leaf))->value, true);
    kp_rgb_condition_invalidate(DEV(cc_leaf));
}
K_WORK_DELAYABLE_DEFINE(external_invalidation_work, external_invalidation_handler);
static void wait_calls(const struct device *dev, unsigned count) {
    int64_t until = k_uptime_get() + 1000;
    while (calls(dev) < count && k_uptime_get() < until) k_msleep(1);
    KP_CHECK(calls(dev) >= count);
    drain();
}
static void wait_ready(void) {
    int64_t until = k_uptime_get() + 1000;
    while (!zmk_rgb_selector_get_selected(DEV(selector_b)) && k_uptime_get() < until) k_msleep(1);
    KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) != NULL);
    drain();
}
static bool peripheral(void) {
    return IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL);
}

struct action_record { uint32_t code, parameter; int64_t timestamp; };
static struct action_record actions[128];
static unsigned action_count, release_count;
static int action_error;
static bool feedback, multi_feedback, capture_mutation, selector_mutation;
static int pressed(struct zmk_behavior_binding *binding, struct zmk_behavior_binding_event event) {
    KP_CHECK(k_current_get() == k_work_queue_thread_get(kp_rgb_work_q()));
    KP_CHECK(binding->param2 == 77);
    KP_CHECK(event.timestamp <= k_uptime_get());
    KP_CHECK(action_count < ARRAY_SIZE(actions));
    actions[action_count++] = (struct action_record){binding->param1, binding->param2, event.timestamp};
    if (capture_mutation && binding->param1 == 10) {
        set(DEV(cc_left), false); set(DEV(cc_right), false);
    }
    if (selector_mutation && binding->param1 == 10)
        KP_CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(probe_c)) == 0);
    if (feedback && (binding->param1 == 10 || binding->param1 == 11)) {
        const struct device *target = multi_feedback ? DEV(cc_right) : DEV(cc_root);
        set(target, !atomic_get(&src(target)->value));
    }
    if (multi_feedback && (binding->param1 == 25 || binding->param1 == 26))
        set(DEV(cc_root), !atomic_get(&src(DEV(cc_root))->value));
    return action_error;
}
static int released(struct zmk_behavior_binding *binding, struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding); ARG_UNUSED(event); release_count++; return 0;
}
static const struct behavior_parameter_metadata action_metadata = {.sets_len = 0};
static const struct behavior_driver_api action_api = {
    .locality = BEHAVIOR_LOCALITY_CENTRAL, .binding_pressed = pressed,
    .binding_released = released, .parameter_metadata = &action_metadata,
};
BEHAVIOR_DT_DEFINE(DT_NODELABEL(ccact), NULL, NULL, NULL, NULL, POST_KERNEL,
                   KP_RGB_DEVICE_INIT_PRIORITY, &action_api);

static int listener(const zmk_event_t *eh) {
    const struct zmk_rgb_selector_selection_changed *ev = as_zmk_rgb_selector_selection_changed(eh);
    if (ev->selector == DEV(selector_a)) {
        atomic_inc(&notifications);
        kp_rgb_condition_invalidate(DEV(cc_event));
        if (atomic_cas(&reenter, 1, 0))
            KP_CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(probe_a)) == 0);
    } else if (ev->selector == DEV(selector_b)) {
        atomic_inc(&notifications_b);
        kp_rgb_condition_invalidate(DEV(cc_event_b));
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(cc_selection_listener, listener);
ZMK_SUBSCRIPTION(cc_selection_listener, zmk_rgb_selector_selection_changed);

static void expect_selection(bool a) {
    drain();
    KP_CHECK(gate(DEV(cc_shared)) == a);
    KP_CHECK(gate(DEV(cc_shared2)) == a);
    KP_CHECK(gate(DEV(cc_opposite)) == !a);
    KP_CHECK(gate(DEV(cc_event_gate)) == a);
}
static void select_a(bool a) {
    KP_CHECK(zmk_rgb_selector_select(DEV(selector_a), a ? DEV(probe_a) : DEV(probe_c)) == 0);
}
static void pause(const struct device *dev) {
    atomic_set(&src(dev)->pause, 1);
    kp_rgb_condition_invalidate(dev);
    KP_CHECK(k_sem_take(&sample_entered, K_SECONDS(1)) == 0);
}
static void resume(void) { k_sem_give(&sample_release); drain(); }

static void graph_prepare(void) {
    const struct device *root = DEV(cc_root), *left = DEV(cc_left), *right = DEV(cc_right), *leaf = DEV(cc_leaf);
    if (kp_test_case("cycle")) { link(root, left); link(left, right); link(right, left); }
    else if (kp_test_case("missing")) link(root, DEVICE_GET(cc_missing));
    else if (kp_test_case("metadata")) { src(root)->dep_count = 1; src(root)->bad_metadata = true; }
    else if (kp_test_case("unready")) link(root, DEV(cc_unready));
    else if (kp_test_case("invalid-scope")) link(root, DEV(cc_bad));
    else if (kp_test_case("scope")) { link(root, left); link(left, DEV(cc_central)); }
    else if (kp_test_case("undeclared")) atomic_set(&src(root)->undeclared, 1);
    else if (kp_test_case("sample-notification")) atomic_set(&src(root)->notify, 1);
    else {
        link(root, left); link(root, right); link(left, leaf); link(right, leaf);
        if (kp_test_case("short-circuit")) {
            src(root)->short_circuit = true;
        }
    }
    for (size_t i = 0; i < 4; i++) {
        const struct device *devs[] = {root, left, right, leaf};
        atomic_set(&src(devs[i])->value, true);
    }
    atomic_set(&src(DEV(cc_central))->value, true);
    if (kp_test_case("short-circuit")) atomic_set(&src(root)->value, false);
}
static void graph_cases(void) {
    const struct device *root = DEV(cc_root), *left = DEV(cc_left), *right = DEV(cc_right), *leaf = DEV(cc_leaf);
    wait_ready();
    bool good = kp_test_case("dag") || kp_test_case("short-circuit") || (kp_test_case("scope") && !peripheral());
    KP_CHECK(kp_rgb_condition_valid(root) == good);
    if (!good) {
        KP_CHECK(!gate(DEV(ordinary)));
        if (kp_test_case("cycle") || kp_test_case("missing") || kp_test_case("metadata") ||
            kp_test_case("unready") || kp_test_case("invalid-scope") || kp_test_case("scope"))
            KP_CHECK(calls(root) == 0);
        if (kp_test_case("scope")) KP_CHECK(calls(DEV(cc_central)) == 0);
        for (unsigned n = 0; n < action_count; n++)
            KP_CHECK(actions[n].code != 10 && actions[n].code != 11 && actions[n].code != 40 && actions[n].code != 41);
        unsigned before = calls(root);
        set(root, false); drain();
        KP_CHECK(calls(root) == before);
        return;
    }
    if (kp_test_case("scope")) { KP_CHECK(gate(DEV(ordinary))); return; }
    KP_CHECK(calls(root) == 1 && calls(left) == 1 && calls(right) == 1 && calls(leaf) == 1);
    KP_CHECK(calls(DEV(cc_unused)) == 0);
    if (kp_test_case("short-circuit")) {
        KP_CHECK(!gate(DEV(ordinary)));
        unsigned right_before = calls(right), root_before = calls(root), leaf_before = calls(leaf);
        kp_rgb_condition_invalidate(right);
        drain();
        KP_CHECK(calls(right) == right_before + 1 && calls(root) == root_before + 1);
        KP_CHECK(calls(leaf) == leaf_before);
    } else {
        KP_CHECK(gate(DEV(ordinary)));
        set(leaf, false); drain();
        KP_CHECK(calls(root) == 2 && calls(left) == 2 && calls(right) == 2 && calls(leaf) == 2);
        KP_CHECK(!gate(DEV(ordinary)));
    }
}

static void timing_prepare(void) {
    const struct device *root = DEV(cc_root);
    if (kp_test_case("external-timer")) {
        link(root, DEV(cc_leaf));
        atomic_set(&src(root)->value, true);
        atomic_set(&src(DEV(cc_leaf))->value, false);
    }
    if (kp_test_case("early-refresh")) kp_rgb_conditions_request_refresh(17);
    kp_rgb_condition_invalidate(root);
    KP_CHECK(calls(root) == 0);
}
static void timing_cases(void) {
    const struct device *root = DEV(cc_root);
    wait_ready();
    KP_CHECK(calls(root) == 1);
    if (kp_test_case("same-value")) {
        kp_rgb_condition_invalidate(root); drain();
        KP_CHECK(!gate(DEV(ordinary)) && calls(root) == 2);
        k_msleep(100); KP_CHECK(calls(root) == 2);
        set(root, true); drain();
        KP_CHECK(gate(DEV(ordinary)) && calls(root) == 3);
    } else if (kp_test_case("external-timer")) {
        unsigned root_before = calls(root), leaf_before = calls(DEV(cc_leaf));
        KP_CHECK(!gate(DEV(ordinary)) && leaf_before == 1);
        KP_CHECK(zmk_rgb_matrix_set_inhibited(true) == 0); drain();
        KP_CHECK(k_work_schedule(&external_invalidation_work, K_MSEC(50)) >= 0);
        wait_calls(DEV(cc_leaf), leaf_before + 1);
        KP_CHECK(calls(root) == root_before + 1 && calls(DEV(cc_leaf)) == leaf_before + 1);
        KP_CHECK(gate(DEV(ordinary)));
    } else if (kp_test_case("isr")) {
        pause(root); irq_device = root;
        atomic_set(&src(root)->value, true); irq_offload(interrupt, NULL);
        KP_CHECK(irq_calls == 1);
        resume(); KP_CHECK(calls(root) >= 3 && gate(DEV(ordinary)));
    } else if (kp_test_case("refresh-interrupted")) {
        pause(root); irq_device = root; irq_offload(interrupt, NULL);
        kp_rgb_conditions_request_refresh(22); resume();
        KP_CHECK(calls(root) >= 3 && kp_rgb_condition_valid(root));
    } else if (kp_test_case("refresh")) {
        unsigned before = calls(root);
        kp_rgb_conditions_request_refresh(21); drain();
        KP_CHECK(calls(root) == before + 1 && calls(DEV(cc_unused)) == 0);
    } else {
        drain();
        KP_CHECK(calls(root) == 1);
        pause(root);
        for (unsigned n = 0; n < 100; n++) kp_rgb_condition_invalidate(root);
        resume(); KP_CHECK(calls(root) == 3);
    }
}

static void trigger_prepare(void) {
    if (kp_test_case("trigger-startup")) {
        atomic_set(&src(DEV(cc_root))->value, true);
        atomic_set(&src(DEV(cc_left))->value, true);
        atomic_set(&src(DEV(cc_leaf))->value, true);
    }
}
static void trigger_cases(void) {
    wait_ready();
    if (peripheral()) {
        KP_CHECK(action_count == 0);
        set(DEV(cc_root), true); set(DEV(cc_right), true); drain();
        KP_CHECK(action_count == 0 && release_count == 0); return;
    }
    if (kp_test_case("trigger-startup")) {
        KP_CHECK(action_count == 3 && actions[0].code == 10 && actions[1].code == 30 && actions[2].code == 40);
        set(DEV(cc_root), false); set(DEV(cc_left), false); set(DEV(cc_leaf), false); drain();
        KP_CHECK(action_count == 7 && actions[3].code == 11 && actions[4].code == 21 && actions[5].code == 41 && actions[6].code == 50);
        set(DEV(cc_left), true); drain(); KP_CHECK(actions[7].code == 20);
    } else if (kp_test_case("trigger-capture")) {
        action_count = 0; capture_mutation = true;
        pause(DEV(cc_root));
        atomic_set(&src(DEV(cc_root))->value, true);
        set(DEV(cc_left), true); set(DEV(cc_right), true);
        kp_rgb_condition_invalidate(DEV(cc_root)); resume();
        KP_CHECK(action_count == 6);
        const unsigned expected[] = {10,20,25,40,21,26};
        for (size_t n = 0; n < ARRAY_SIZE(expected); n++) KP_CHECK(actions[n].code == expected[n]);
        KP_CHECK(actions[0].timestamp == actions[3].timestamp);
    } else if (kp_test_case("trigger-fault")) {
        set(DEV(cc_root), true); drain(); unsigned before = action_count;
        atomic_set(&src(DEV(cc_root))->undeclared, 1);
        kp_rgb_condition_invalidate(DEV(cc_root)); drain();
        KP_CHECK(!kp_rgb_condition_valid(DEV(cc_root)) && action_count == before);
        set(DEV(cc_root), false); drain(); KP_CHECK(action_count == before);
        set(DEV(cc_right), true); drain(); KP_CHECK(actions[action_count - 1].code == 25);
    } else if (kp_test_case("trigger-errors")) {
        action_count = 0; action_error = -EIO; set(DEV(cc_root), true); drain();
        KP_CHECK(action_count == 3 && actions[0].code == 10 && actions[1].code == 60 && actions[2].code == 40);
        drain(); KP_CHECK(action_count == 3);
        action_error = 1; set(DEV(cc_root), false); drain();
        KP_CHECK(action_count == 6 && actions[3].code == 11 && actions[4].code == 61 && actions[5].code == 41);
    } else {
        action_count = 0; feedback = true; multi_feedback = kp_test_case("multi-feedback");
        action_error = multi_feedback ? 0 : -EIO;
        set(DEV(cc_root), true); drain();
        KP_CHECK(action_count == (multi_feedback ? 12 : 16));
        unsigned before = action_count;
        set(DEV(cc_root), false); set(DEV(cc_right), false); drain();
        KP_CHECK(action_count == before);
        set(DEV(cc_left), true); drain(); KP_CHECK(actions[action_count - 1].code == 20);
    }
    KP_CHECK(release_count == 0);
}

static void selection_cases(void) {
    wait_ready(); expect_selection(true);
    KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
    unsigned event_samples = calls(DEV(cc_event));
    if (kp_test_case("event-only")) {
        k_msleep(100);
        KP_CHECK(calls(DEV(cc_event)) == event_samples);
        return;
    }
    if (kp_test_case("scoped-listener")) {
        unsigned samples_b = calls(DEV(cc_event_b)), root_samples = calls(DEV(cc_root));
        KP_CHECK(raise_zmk_rgb_selector_selection_changed((struct zmk_rgb_selector_selection_changed){.selector = DEV(selector_a)}) >= 0);
        drain();
        KP_CHECK(calls(DEV(cc_event)) == event_samples + 1);
        KP_CHECK(calls(DEV(cc_event_b)) == samples_b && calls(DEV(cc_root)) == root_samples);
        KP_CHECK(raise_zmk_rgb_selector_selection_changed((struct zmk_rgb_selector_selection_changed){.selector = DEV(selector_b)}) >= 0);
        drain();
        KP_CHECK(calls(DEV(cc_event_b)) == samples_b + 1);
        KP_CHECK(calls(DEV(cc_event)) == event_samples + 1 && calls(DEV(cc_root)) == root_samples);
        event_samples = calls(DEV(cc_event));
        unsigned before = atomic_get(&notifications);
        KP_CHECK(zmk_rgb_selector_select(DEV(selector_b), DEV(probe_d)) == 0);
        drain(); KP_CHECK(atomic_get(&notifications) == before && calls(DEV(cc_event)) == event_samples + 1);
        expect_selection(true); return;
    }
    if (kp_test_case("coalesced")) {
        pause(DEV(cc_root)); unsigned before = atomic_get(&notifications);
        unsigned before_b = atomic_get(&notifications_b);
        select_a(false); select_a(true); select_a(false);
        KP_CHECK(zmk_rgb_selector_select(DEV(selector_b), DEV(probe_d)) == 0);
        KP_CHECK(atomic_get(&notifications) == before);
        resume(); expect_selection(false);
        KP_CHECK(atomic_get(&notifications) == before + 1);
        KP_CHECK(atomic_get(&notifications_b) == before_b + 1);
        KP_CHECK(!gate(DEV(cc_event_b_gate))); return;
    }
    if (kp_test_case("reentrant")) {
        atomic_set(&reenter, 1); select_a(false); drain();
        expect_selection(true); KP_CHECK(atomic_get(&notifications) >= 2); return;
    }
    if (kp_test_case("trigger-mutation")) {
        selector_mutation = true; set(DEV(cc_root), true); drain();
        expect_selection(peripheral()); return;
    }
    if (kp_test_case("external-interruption") || kp_test_case("external-unused-interruption")) {
        pause(DEV(cc_event)); select_a(false);
        irq_device = kp_test_case("external-unused-interruption") ? DEV(cc_unused) : DEV(cc_root);
        irq_offload(interrupt, NULL); resume(); expect_selection(false);
        KP_CHECK(calls(DEV(cc_unused)) == 0); return;
    }
    if (kp_test_case("resume-held")) {
        kp_rgb_matrix_lock();
        kp_rgb_control_refresh_hold_locked();
        kp_rgb_control_refresh_hold_locked();
        kp_rgb_matrix_unlock();
        zmk_rgb_matrix_set_inhibited(true);
        select_a(false);
        zmk_rgb_matrix_set_inhibited(false);
        drain();
        kp_rgb_conditions_refreshed(UINT64_MAX);
        kp_rgb_matrix_lock(); kp_rgb_control_refresh_release_locked(); kp_rgb_matrix_unlock();
        select_a(true); select_a(false); drain();
        kp_rgb_conditions_refreshed(1);
        kp_rgb_matrix_lock(); kp_rgb_control_refresh_release_locked(); kp_rgb_matrix_unlock();
        expect_selection(false); return;
    }
    if (!kp_test_case("initial-shared")) {
        select_a(false); expect_selection(false);
        KP_CHECK(calls(DEV(cc_event)) > event_samples && atomic_get(&notifications) > 0);
        select_a(true); expect_selection(true);
    }
}

static void overlay_cases(void) {
    wait_ready();
    KP_CHECK(gate(DEV(ordinary)));
    KP_CHECK(!gate(DEV(cc_transparent)) && !gate(DEV(cc_empty)));
    KP_CHECK(kp_rgb_overlay_target_count(DEV(cc_transparent)) == KP_LED_COUNT);
    KP_CHECK(kp_rgb_overlay_targets(DEV(cc_transparent)) != NULL);
    KP_CHECK(!kp_rgb_overlay_covers_all(DEV(cc_transparent)));
    KP_CHECK(gate(DEV(cc_half)) && !kp_rgb_overlay_covers_all(DEV(cc_half)));
    KP_CHECK(kp_rgb_overlay_covers_all(DEV(preview_selector)));
    const uint32_t keys[] = {3, 3, 99, 1}, leds[] = {1, 2, 0, 98};
    size_t resolved[5] = {99, 99, 99, 99, 99};
    const size_t expected[] = {3, 1, 2, 0};
    KP_CHECK(kp_rgb_resolve_targets(keys, 4, leds, 4, resolved, 4) == 4);
    KP_CHECK(memcmp(resolved, expected, sizeof(expected)) == 0 && resolved[4] == 99);
    KP_CHECK(kp_rgb_resolve_targets(keys, 4, leds, 4, resolved, 1) == 1 && resolved[0] == 3);
    KP_CHECK(kp_rgb_resolve_targets(keys, 4, leds, 4, resolved, 0) == 0);
    const struct kp_rgb_overlay_common_data *data = DEV(cc_remote)->data;
    unsigned word = data->index / 16;
    uint16_t bit = BIT(data->index % 16);
    if (peripheral()) {
        KP_CHECK(calls(DEV(cc_central)) == 0 && !gate(DEV(cc_remote)));
        KP_CHECK(kp_rgb_overlay_set_word(word, bit));
        KP_CHECK(gate(DEV(cc_remote)) && kp_rgb_overlay_get_word(word) == bit);
        KP_CHECK(!kp_rgb_overlay_set_word(word, bit));
        set(DEV(cc_root), false); drain();
        KP_CHECK(!gate(DEV(ordinary)) && gate(DEV(cc_remote)));
    } else {
        KP_CHECK(calls(DEV(cc_central)) == 1 && gate(DEV(cc_remote)));
        set(DEV(cc_central), false); drain(); KP_CHECK(!gate(DEV(cc_remote)));
    }
    KP_CHECK(!kp_rgb_overlay_set_word(kp_rgb_overlay_word_count(), UINT16_MAX));
    KP_CHECK(kp_rgb_overlay_get_word(kp_rgb_overlay_word_count()) == 0);
    set(DEV(cc_root), true); drain();
    KP_CHECK(zmk_rgb_matrix_off() == 0); drain(); KP_CHECK(gate(DEV(ordinary)));
    zmk_rgb_matrix_set_inhibited(true); drain(); KP_CHECK(gate(DEV(ordinary)));
    set(DEV(cc_root), false); drain(); KP_CHECK(!gate(DEV(ordinary)));
    set(DEV(cc_root), true); drain(); KP_CHECK(gate(DEV(ordinary)));
}

static int auto_prepare(void) {
    const char *graphs[] = {"dag", "cycle", "missing", "metadata", "unready", "invalid-scope",
        "scope", "undeclared", "sample-notification", "short-circuit"};
    const char *timings[] = {"startup", "same-value", "isr", "refresh", "refresh-interrupted",
        "early-refresh", "external-timer"};
    for (size_t i = 0; i < ARRAY_SIZE(graphs); i++)
        if (kp_test_case(graphs[i])) graph_prepare();
    for (size_t i = 0; i < ARRAY_SIZE(timings); i++)
        if (kp_test_case(timings[i])) timing_prepare();
    trigger_prepare();
    if (kp_test_case("startup-auto") || kp_test_case("overlay-gate") ||
        kp_test_case("split-visibility") || kp_test_case("overlay-zero")) {
        atomic_set(&src(DEV(cc_root))->value, true);
        atomic_set(&src(DEV(cc_central))->value, true);
        kp_rgb_condition_invalidate(DEV(cc_root));
    }
#if DT_NODE_EXISTS(DT_NODELABEL(cc_noreg))
    atomic_set(&src(DEV(cc_noreg))->value, true);
#endif
#if IS_ENABLED(CONFIG_SETTINGS)
    if (kp_test_case("startup-normalized-names") || kp_test_case("startup-settings-staged")) {
        struct kp_rgb_persist_selector_record record;
        const char *member = kp_test_case("startup-normalized-names") ?
            KP_TEST_NODE_TOKEN(DT_NODELABEL(probe_a)) : KP_TEST_NODE_TOKEN(DT_NODELABEL(probe_c));
        KP_CHECK(kp_rgb_persist_encode_selector(member, &record) == 0);
        KP_CHECK(settings_runtime_set(KP_TEST_SELECTOR_A_STATE, &record, sizeof(record)) == 0);
    }
#endif
    KP_CHECK(calls(DEV(cc_root)) == 0);
    KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == NULL);
    return 0;
}
SYS_INIT(auto_prepare, APPLICATION, 98);

#if IS_ENABLED(CONFIG_SETTINGS)
static unsigned provider_commits;
static int settings_provider_commit(void) {
    if (!kp_test_case("startup-unrestored-settings") && !kp_test_case("startup-settings-staged")) return 0;
    if (provider_commits) return 0;
    KP_CHECK(calls(DEV(cc_root)) == 0);
    KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == NULL);
    provider_commits++;
    if (kp_test_case("startup-settings-staged")) return 0;
    struct kp_rgb_persist_selector_record record;
    KP_CHECK(kp_rgb_persist_encode_selector(KP_TEST_NODE_TOKEN(DT_NODELABEL(probe_c)), &record) == 0);
    KP_CHECK(settings_runtime_set(KP_TEST_SELECTOR_A_STATE, &record, sizeof(record)) == 0);
    return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE_WITH_CPRIO(cc_settings_source, "cc_settings_source",
    NULL, NULL, settings_provider_commit, NULL, INT_MAX - 2);
#endif

static void special_cases(void) {
    if (kp_test_case("startup-unready-selector")) {
        KP_CHECK(!device_is_ready(DEV(rgbs_a)));
        drain(); KP_CHECK(calls(DEV(cc_root)) == 0);
        KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == NULL);
        KP_CHECK(device_init(DEV(rgbs_a)) == 0);
        k_msleep(100); drain();
        KP_CHECK(calls(DEV(cc_root)) == 0);
        KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == NULL);
    } else if (kp_test_case("startup-normalized-names")) {
        wait_ready();
        KP_CHECK(device_is_ready(DEV(selector_a)) && device_is_ready(DEV(probe_a)));
        KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
        KP_CHECK(kp_rgb_condition_valid(DEV(cc_selected_a)));
        expect_selection(true);
    } else if (kp_test_case("startup-auto")) {
        wait_calls(DEV(cc_root), 1);
        KP_CHECK(gate(DEV(ordinary)) && calls(DEV(cc_root)) == 1);
    } else if (kp_test_case("startup-unrestored-settings") || kp_test_case("startup-settings-staged")) {
#if IS_ENABLED(CONFIG_SETTINGS)
        wait_ready();
        KP_CHECK(provider_commits == 1);
        KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_c));
        expect_selection(false);
        KP_CHECK(calls(DEV(cc_root)) == 1 && calls(DEV(cc_event)) == 1);
        KP_CHECK(settings_commit() == 0);
        KP_CHECK(settings_commit() == 0);
        drain();
        KP_CHECK(calls(DEV(cc_root)) == 1 && calls(DEV(cc_event)) == 1);
        KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_c));
#else
        KP_CHECK(false);
#endif
    } else if (kp_test_case("overlay-zero")) {
        wait_ready();
        KP_CHECK(KP_LED_COUNT == 0);
        KP_CHECK(kp_rgb_condition_valid(DEV(cc_root)));
        KP_CHECK(kp_rgb_overlay_target_count(DEV(cc_transparent)) == 0);
        KP_CHECK(kp_rgb_overlay_targets(DEV(cc_transparent)) != NULL);
        KP_CHECK(!gate(DEV(ordinary)) && !gate(DEV(cc_shared)));
        KP_CHECK(!kp_rgb_overlay_covers_all(DEV(cc_transparent)));
        unsigned before = atomic_get(&notifications);
        unsigned samples = calls(DEV(cc_event));
        select_a(false); drain();
        KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_c));
        KP_CHECK(atomic_get(&notifications) == before + 1);
        KP_CHECK(calls(DEV(cc_event)) == samples + 1);
    } else if (kp_test_case("trigger-unregistered-root")) {
#if DT_NODE_EXISTS(DT_NODELABEL(cc_noreg))
        KP_CHECK(device_is_ready(DEV(cc_noreg)));
        wait_ready();
        KP_CHECK(calls(DEV(cc_noreg)) == 0 && !kp_rgb_condition_valid(DEV(cc_noreg)));
        if (!peripheral()) KP_CHECK(action_count == 1 && actions[0].code == 30);
        unsigned before = action_count;
        atomic_set(&src(DEV(cc_noreg))->value, false);
        kp_rgb_conditions_request_refresh(41); drain();
        KP_CHECK(action_count == before && calls(DEV(cc_noreg)) == 0);
        set(DEV(cc_root), true); drain();
        if (!peripheral()) {
            KP_CHECK(action_count == before + 2);
            KP_CHECK(actions[before].code == 10 && actions[before + 1].code == 40);
        } else KP_CHECK(action_count == 0);
#else
        KP_CHECK(false);
#endif
    } else if (kp_test_case("trigger-actions")) {
#if DT_NODE_EXISTS(DT_NODELABEL(cc_latchgate))
        wait_ready();
        KP_CHECK(!gate(DEV(cc_latchgate)));
        set(DEV(cc_root), true); drain();
        bool on;
        KP_CHECK(zmk_rgb_matrix_get_user_state(&on) == 0);
        KP_CHECK(on == peripheral());
        expect_selection(peripheral());
        KP_CHECK(gate(DEV(cc_latchgate)) == !peripheral());
        set(DEV(cc_root), false); drain();
        KP_CHECK(zmk_rgb_matrix_get_user_state(&on) == 0 && on);
        expect_selection(true);
        KP_CHECK(!gate(DEV(cc_latchgate)));
#else
        KP_CHECK(false);
#endif
    } else {
        KP_CHECK(kp_test_case("trigger-no-table"));
        wait_ready(); set(DEV(cc_root), true); drain();
        KP_CHECK(action_count == 0 && release_count == 0 && gate(DEV(ordinary)));
        KP_CHECK(calls(DEV(cc_left)) == 0 && calls(DEV(cc_right)) == 0);
    }
}

#if DT_HAS_COMPAT_STATUS_OKAY(test_rgb_condition_contract_strip)
static atomic_t strip_colors, strip_blacks, strip_pixel_one;
static int strip_update(const struct device *dev, struct led_rgb *pixels, size_t count) {
    ARG_UNUSED(dev);
    KP_CHECK(count == 48);
    bool black = true;
    for (size_t n = 0; n < count; n++) black &= !(pixels[n].r || pixels[n].g || pixels[n].b);
    atomic_set(&strip_pixel_one, ((uint32_t)pixels[1].r << 16) | ((uint32_t)pixels[1].g << 8) | pixels[1].b);
    atomic_inc(black ? &strip_blacks : &strip_colors);
    return 0;
}
static size_t strip_length(const struct device *dev) { ARG_UNUSED(dev); return 48; }
static const struct led_strip_driver_api strip_api = {.update_rgb = strip_update, .length = strip_length};
DEVICE_DT_DEFINE(DT_NODELABEL(cc_strip), NULL, NULL, NULL, NULL, POST_KERNEL,
                 CONFIG_LED_STRIP_INIT_PRIORITY, &strip_api);

static void stale_source_refresh(bool held) {
    wait_ready();
    KP_CHECK(atomic_get(&strip_colors) > 0);
    zmk_rgb_matrix_set_inhibited(true); drain();
    KP_CHECK(atomic_get(&strip_blacks) > 0 && atomic_get(&strip_pixel_one) == 0);
    unsigned before = atomic_get(&strip_colors);
    if (held) {
        kp_rgb_matrix_lock(); kp_rgb_control_refresh_hold_locked(); kp_rgb_matrix_unlock();
    }
    atomic_set(&src(DEV(cc_root))->pause, 1);
    zmk_rgb_matrix_set_inhibited(false);
    if (held) kp_rgb_condition_invalidate(DEV(cc_root));
    KP_CHECK(k_sem_take(&sample_entered, K_SECONDS(1)) == 0);
    KP_CHECK(atomic_get(&strip_colors) == before);
    atomic_set(&src(DEV(cc_root))->value, true);
    irq_device = DEV(cc_root); irq_offload(interrupt, NULL);
    atomic_set(&src(DEV(cc_root))->pause, 1);
    /* Boot requested token 1; resume advanced it. Without held=true, these
     * completions must be rejected by token alone, not by outstanding holds. */
    kp_rgb_conditions_refreshed(1);
    kp_rgb_conditions_refreshed(UINT64_MAX);
    kp_rgb_conditions_refreshed(0);
    k_sem_give(&sample_release);
    KP_CHECK(k_sem_take(&sample_entered, K_SECONDS(1)) == 0);
    KP_CHECK(atomic_get(&strip_colors) == before);
    KP_CHECK(!gate(DEV(ordinary)));
    if (held) {
        atomic_set(&src(DEV(cc_root))->pause, 1);
        kp_rgb_matrix_lock(); kp_rgb_control_refresh_release_locked(); kp_rgb_matrix_unlock();
        k_sem_give(&sample_release);
        KP_CHECK(k_sem_take(&sample_entered, K_SECONDS(1)) == 0);
        KP_CHECK(atomic_get(&strip_colors) == before);
    }
    k_sem_give(&sample_release);
    int64_t until = k_uptime_get() + 1000;
    while (atomic_get(&strip_colors) == before && k_uptime_get() < until) k_msleep(1);
    drain();
    KP_CHECK(atomic_get(&strip_colors) == before + 1);
    KP_CHECK(gate(DEV(ordinary)) && atomic_get(&strip_pixel_one) == 0xffffff);
    KP_CHECK(irq_calls == 1);
}
#endif

void kp_test_conditions_run(void) {
    if (kp_test_case("stale-source-refresh") || kp_test_case("held-source-refresh")) {
#if DT_HAS_COMPAT_STATUS_OKAY(test_rgb_condition_contract_strip)
        stale_source_refresh(kp_test_case("held-source-refresh"));
#else
        KP_CHECK(false);
#endif
    } else if (kp_test_case("startup-unready-selector") || kp_test_case("startup-normalized-names") ||
        kp_test_case("startup-auto") || kp_test_case("startup-unrestored-settings") ||
        kp_test_case("startup-settings-staged") ||
        kp_test_case("overlay-zero") || kp_test_case("trigger-no-table") ||
        kp_test_case("trigger-actions") || kp_test_case("trigger-unregistered-root")) special_cases();
    else if (kp_test_case("dag") || kp_test_case("cycle") || kp_test_case("missing") ||
        kp_test_case("metadata") || kp_test_case("unready") || kp_test_case("invalid-scope") || kp_test_case("scope") ||
        kp_test_case("undeclared") || kp_test_case("sample-notification") ||
        kp_test_case("short-circuit")) graph_cases();
    else if (kp_test_case("startup") || kp_test_case("same-value") || kp_test_case("isr") ||
        kp_test_case("external-timer") || kp_test_case("refresh") || kp_test_case("refresh-interrupted") ||
        kp_test_case("early-refresh")) timing_cases();
    else if (kp_test_case("trigger-startup") || kp_test_case("trigger-capture") ||
        kp_test_case("trigger-fault") || kp_test_case("trigger-errors") ||
        kp_test_case("self-feedback") || kp_test_case("multi-feedback")) trigger_cases();
    else if (kp_test_case("overlay-gate") || kp_test_case("split-visibility")) overlay_cases();
    else if (kp_test_case("unused-scope")) {
        wait_ready(); KP_CHECK(calls(DEV(cc_unused)) == 0);
        KP_CHECK(calls(DEV(cc_central)) == (peripheral() ? 0 : 1));
        KP_CHECK(kp_rgb_condition_valid(DEV(cc_root)));
    } else if (kp_test_case("initial-shared") || kp_test_case("event-only") ||
        kp_test_case("scoped-listener") || kp_test_case("third-party") ||
        kp_test_case("coalesced") || kp_test_case("reentrant") ||
        kp_test_case("trigger-mutation") || kp_test_case("resume-held") ||
        kp_test_case("external-interruption") || kp_test_case("external-unused-interruption")) selection_cases();
    else KP_CHECK(false);
}
