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
#include <zmk/rgb_selector.h>
#include <zmk/events/position_state_changed.h>
#include "../../../../src/rgb_matrix_internal.h"
#include "../../../../src/rgb_persist_internal.h"
#include "../../../../src/rgb_selector_internal.h"

#define DEV(label) DEVICE_DT_GET(DT_NODELABEL(label))
#define KP_TEST_SELECTOR_CAPACITY 16
struct source {
    atomic_t value, calls, pause;
    bool short_circuit;
    bool first_value;
};
struct source_config {
    const struct device *selector, *effect;
    const struct kp_rgb_condition_registration *registration;
};
K_SEM_DEFINE(sample_entered, 0, 1);
K_SEM_DEFINE(sample_release, 0, 1);
static atomic_t irq_calls;

static void interrupt(const void *arg) {
    ARG_UNUSED(arg);
    KP_CHECK(k_is_in_isr());
    atomic_inc(&irq_calls);
    kp_rgb_condition_invalidate();
}
static bool sample(const struct device *dev) {
    struct source *s = dev->data;
    const struct source_config *cfg = dev->config;
    KP_CHECK(!k_is_in_isr());
    KP_CHECK(k_current_get() == k_work_queue_thread_get(kp_rgb_work_q()));
    atomic_inc(&s->calls);
    bool value = atomic_get(&s->value);
    if (cfg->selector)
        value = zmk_rgb_selector_get_selected(cfg->selector) == cfg->effect;
    if (atomic_get(&s->calls) == 1) {
        s->first_value = value;
        if (kp_test_case("startup-unrestored-settings") || kp_test_case("startup-settings-staged")) {
            KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_c));
            KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_b));
        }
    }
    if (atomic_cas(&s->pause, 1, 0)) {
        k_sem_give(&sample_entered);
        KP_CHECK(k_sem_take(&sample_release, K_SECONDS(2)) == 0);
    }
    const struct kp_rgb_condition_registration *registration = cfg->registration;
    for (size_t i = 0; registration && i < registration->dep_count; i++) {
        if (s->short_circuit && !value) break;
        value &= kp_rgb_condition_value(registration->deps[i]->dev);
    }
    return value;
}
#define DEFINE(inst) \
    COND_CODE_1(DT_INST_PROP(inst, unregistered), (), ( \
        KP_RGB_CONDITION_REGISTRATION_DEFINE(DT_DRV_INST(inst)); \
    )) \
    static struct source source_##inst; \
    static const struct source_config config_##inst = { \
        .registration = COND_CODE_1(DT_INST_PROP(inst, unregistered), \
            (NULL), (&KP_RGB_CONDITION_REGISTRATION(DT_DRV_INST(inst)))), \
        .selector = COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, selected_selector), \
            (DEVICE_DT_GET(DT_INST_PHANDLE(inst, selected_selector))), (NULL)), \
        .effect = COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, selected_effect), \
            (DEVICE_DT_GET(DT_INST_PHANDLE(inst, selected_effect))), (NULL)), \
    }; \
    static const struct kp_rgb_condition_api api_##inst = { \
        .scope = DT_INST_PROP(inst, invalid_scope) ? 99 : \
            (DT_INST_PROP(inst, central_only) ? KP_RGB_CONDITION_CENTRAL_ONLY : KP_RGB_CONDITION_ANY_SIDE), \
        .sample = sample, \
    }; \
    DEVICE_DT_INST_DEFINE(inst, NULL, NULL, &source_##inst, &config_##inst, \
        POST_KERNEL, KP_RGB_DEVICE_INIT_PRIORITY, &api_##inst);
DT_INST_FOREACH_STATUS_OKAY(DEFINE)

static struct source *src(const struct device *dev) { return dev->data; }
static unsigned calls(const struct device *dev) { return atomic_get(&src(dev)->calls); }
static void set(const struct device *dev, bool value) {
    atomic_set(&src(dev)->value, value);
    kp_rgb_condition_invalidate();
}
static bool gate(const struct device *dev) {
    kp_rgb_matrix_lock();
    bool value = kp_rgb_overlay_gate(dev);
    kp_rgb_matrix_unlock();
    return value;
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
    kp_rgb_condition_invalidate();
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

#if IS_ENABLED(CONFIG_SETTINGS)
static int store_snapshot(const struct device *target_selector,
                          const struct device *target_effect) {
    uint16_t selections[KP_TEST_SELECTOR_CAPACITY];
    size_t count = kp_rgb_selector_registry_count();
    KP_CHECK(count <= ARRAY_SIZE(selections));
    kp_rgb_matrix_lock();
    for (size_t i = 0; i < count; i++) {
        struct kp_rgb_selector *selector = kp_rgb_selector_registry_at(i);
        selections[i] = selector->effect_index;
        if (selector->owner == target_selector) {
            size_t slot;
            for (slot = 0; slot < selector->effect_count; slot++)
                if (selector->members[slot].effect == target_effect) break;
            KP_CHECK(slot < selector->effect_count && slot <= UINT16_MAX);
            selections[i] = (uint16_t)slot;
        }
    }
    struct kp_rgb_persist_state state = {
        .user_on = kp_rgb_global.user_on,
        .brightness = kp_rgb_global.brightness,
        .selection_count = (uint16_t)count,
    };
    kp_rgb_matrix_unlock();

    uint8_t bytes[KP_RGB_PERSIST_SIZE(KP_TEST_SELECTOR_CAPACITY)];
    size_t len = KP_RGB_PERSIST_SIZE(count);
    KP_CHECK(kp_rgb_persist_encode(&state, selections, bytes, len) == 0);
    return settings_runtime_set(KP_RGB_PERSIST_KEY, bytes, len);
}
#endif
struct action_record { uint32_t code, parameter; int64_t timestamp; };
static struct action_record actions[128];
static unsigned action_count, release_count;
static int action_error;
static bool finite_cascade, multi_cascade, capture_mutation, selector_mutation;
static unsigned mutation_budget, interleavings, sentinel_actions;
static unsigned sentinel_root_calls, sentinel_left_calls, sentinel_right_calls;
static unsigned cascade_actions;
K_SEM_DEFINE(cascade_done, 0, 1);
static void sentinel_handler(struct k_work *work) {
    ARG_UNUSED(work);
    KP_CHECK(k_current_get() == k_work_queue_thread_get(kp_rgb_work_q()));
    KP_CHECK(action_count == sentinel_actions);
    KP_CHECK(calls(DEV(cc_root)) == sentinel_root_calls);
    KP_CHECK(calls(DEV(cc_left)) == sentinel_left_calls);
    KP_CHECK(calls(DEV(cc_right)) == sentinel_right_calls);
    if (capture_mutation) {
        const unsigned expected[] = {10, 20, 25, 40};
        KP_CHECK(action_count == ARRAY_SIZE(expected));
        for (size_t n = 0; n < ARRAY_SIZE(expected); n++)
            KP_CHECK(actions[n].code == expected[n]);
    }
    interleavings++;
}
K_WORK_DEFINE(sentinel_work, sentinel_handler);
static void queue_sentinel(unsigned remaining_actions) {
    sentinel_actions = action_count + remaining_actions;
    sentinel_root_calls = calls(DEV(cc_root));
    sentinel_left_calls = calls(DEV(cc_left));
    sentinel_right_calls = calls(DEV(cc_right));
    KP_CHECK(k_work_submit_to_queue(kp_rgb_work_q(), &sentinel_work) > 0);
}
static void capture_setup_handler(struct k_work *work) {
    ARG_UNUSED(work);
    set(DEV(cc_root), true); set(DEV(cc_left), true); set(DEV(cc_right), true);
}
K_WORK_DEFINE(capture_setup_work, capture_setup_handler);
static int pressed(struct zmk_behavior_binding *binding, struct zmk_behavior_binding_event event) {
    KP_CHECK(k_current_get() == k_work_queue_thread_get(kp_rgb_work_q()));
    KP_CHECK(binding->param2 == 77);
    KP_CHECK(event.timestamp <= k_uptime_get());
    KP_CHECK(action_count < ARRAY_SIZE(actions));
    actions[action_count++] = (struct action_record){binding->param1, binding->param2, event.timestamp};
    if (capture_mutation && binding->param1 == 10) {
        queue_sentinel(3);
        set(DEV(cc_left), false); set(DEV(cc_right), false);
    }
    if (capture_mutation && (binding->param1 == 10 || binding->param1 == 20 ||
                             binding->param1 == 25 || binding->param1 == 40)) {
        KP_CHECK(kp_rgb_condition_value(DEV(cc_left)));
        KP_CHECK(kp_rgb_condition_value(DEV(cc_right)));
        KP_CHECK(calls(DEV(cc_left)) == sentinel_left_calls);
        KP_CHECK(calls(DEV(cc_right)) == sentinel_right_calls);
    }
    if (selector_mutation && (binding->param1 == 10 || binding->param1 == 11))
        KP_CHECK(zmk_rgb_selector_select(DEV(selector_a),
            binding->param1 == 10 ? DEV(probe_c) : DEV(probe_a)) == 0);
    bool root_edge = binding->param1 == 10 || binding->param1 == 11;
    bool right_edge = binding->param1 == 25 || binding->param1 == 26;
    if (finite_cascade && mutation_budget && (root_edge || (multi_cascade && right_edge))) {
        queue_sentinel(root_edge ? 1 : 0);
        mutation_budget--;
        const struct device *target = root_edge && multi_cascade ? DEV(cc_right) : DEV(cc_root);
        set(target, !atomic_get(&src(target)->value));
    }
    if (finite_cascade && action_count == cascade_actions) k_sem_give(&cascade_done);
    if (capture_mutation && binding->param1 == 26) k_sem_give(&cascade_done);
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
    kp_rgb_condition_invalidate();
    KP_CHECK(k_sem_take(&sample_entered, K_SECONDS(1)) == 0);
}
static void resume(void) { k_sem_give(&sample_release); drain(); }

static void graph_prepare(void) {
    const struct device *root = DEV(cc_root), *left = DEV(cc_left), *right = DEV(cc_right), *leaf = DEV(cc_leaf);
    if (kp_test_case("short-circuit")) src(root)->short_circuit = true;
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
    bool good = kp_test_case("dag") || kp_test_case("chain") || kp_test_case("combinators") || kp_test_case("short-circuit") || (kp_test_case("scope") && !peripheral());
    KP_CHECK(kp_rgb_condition_valid(root) == good);
    if (!good) {
        KP_CHECK(!gate(DEV(ordinary)));
        if (kp_test_case("invalid-scope") || kp_test_case("scope"))
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
        unsigned right_before = calls(right), root_before = calls(root);
        unsigned leaf_before = calls(leaf), left_before = calls(left);
        kp_rgb_condition_invalidate();
        drain();
        KP_CHECK(calls(right) == right_before + 1 && calls(root) == root_before + 1);
        KP_CHECK(calls(leaf) == leaf_before + 1 && calls(left) == left_before + 1);
    } else {
        KP_CHECK(gate(DEV(ordinary)));
        set(leaf, false); drain();
        KP_CHECK(calls(root) == 2 && calls(left) == 2 && calls(right) == 2 && calls(leaf) == 2);
        KP_CHECK(!gate(DEV(ordinary)));
        kp_rgb_condition_invalidate(); drain();
        KP_CHECK(calls(root) == 3 && calls(left) == 3 && calls(right) == 3 && calls(leaf) == 3);
        KP_CHECK(!gate(DEV(ordinary)));
    }
}

static void timing_prepare(void) {
    const struct device *root = DEV(cc_root);
    if (kp_test_case("external-timer")) {
        atomic_set(&src(root)->value, true);
        atomic_set(&src(DEV(cc_leaf))->value, false);
    }
    if (kp_test_case("early-refresh")) kp_rgb_condition_invalidate();
    kp_rgb_condition_invalidate();
    KP_CHECK(calls(root) == 0);
}
static void timing_cases(void) {
    const struct device *root = DEV(cc_root);
    wait_ready();
    KP_CHECK(calls(root) == 1);
    if (kp_test_case("same-value")) {
        kp_rgb_condition_invalidate(); drain();
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
        pause(root);
        atomic_set(&src(root)->value, true); irq_offload(interrupt, NULL);
        KP_CHECK(irq_calls == 1);
        resume(); KP_CHECK(calls(root) >= 3 && gate(DEV(ordinary)));
    } else if (kp_test_case("refresh-interrupted")) {
        pause(root); irq_offload(interrupt, NULL);
        kp_rgb_condition_invalidate(); resume();
        KP_CHECK(calls(root) >= 3 && kp_rgb_condition_valid(root));
    } else if (kp_test_case("refresh")) {
        unsigned before = calls(root);
        kp_rgb_condition_invalidate(); drain();
        KP_CHECK(calls(root) == before + 1 && calls(DEV(cc_unused)) == 0);
    } else {
        drain();
        KP_CHECK(calls(root) == 1);
        pause(root);
        for (unsigned n = 0; n < 100; n++) kp_rgb_condition_invalidate();
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
        KP_CHECK(action_count == 4 && actions[0].code == 10 && actions[1].code == 20 &&
                 actions[2].code == 30 && actions[3].code == 40);
        for (unsigned n = 0; n < action_count; n++)
            KP_CHECK(actions[n].code != 25 && actions[n].code != 26);
        kp_rgb_condition_invalidate();
        kp_rgb_condition_invalidate(); drain();
        KP_CHECK(action_count == 4);
        set(DEV(cc_root), false); set(DEV(cc_left), false); set(DEV(cc_leaf), false); drain();
        KP_CHECK(action_count == 8 && actions[4].code == 11 && actions[5].code == 21 &&
                 actions[6].code == 41 && actions[7].code == 50);
        set(DEV(cc_left), true); drain(); KP_CHECK(action_count == 9 && actions[8].code == 20);
    } else if (kp_test_case("trigger-capture")) {
        action_count = 0; capture_mutation = true;
        KP_CHECK(k_work_submit_to_queue(kp_rgb_work_q(), &capture_setup_work) > 0);
        KP_CHECK(k_sem_take(&cascade_done, K_SECONDS(2)) == 0);
        drain();
        KP_CHECK(action_count == 6);
        const unsigned expected[] = {10,20,25,40,21,26};
        for (size_t n = 0; n < ARRAY_SIZE(expected); n++) KP_CHECK(actions[n].code == expected[n]);
        KP_CHECK(actions[0].timestamp == actions[3].timestamp);
        KP_CHECK(interleavings == 1);
    } else if (kp_test_case("trigger-errors")) {
        action_count = 0; action_error = -EIO; set(DEV(cc_root), true); drain();
        KP_CHECK(action_count == 3 && actions[0].code == 10 && actions[1].code == 60 && actions[2].code == 40);
        drain(); KP_CHECK(action_count == 3);
        action_error = 1; set(DEV(cc_root), false); drain();
        KP_CHECK(action_count == 6 && actions[3].code == 11 && actions[4].code == 61 && actions[5].code == 41);
    } else {
        KP_CHECK(kp_test_case("finite-self-cascade") || kp_test_case("finite-multi-cascade"));
        action_count = 0; finite_cascade = true;
        multi_cascade = kp_test_case("finite-multi-cascade");
        mutation_budget = 12;
        action_error = multi_cascade ? 0 : -EIO;
        cascade_actions = multi_cascade ? 20 : 26;
        unsigned root_before = calls(DEV(cc_root)), right_before = calls(DEV(cc_right));
        unsigned left_before = calls(DEV(cc_left));
        set(DEV(cc_root), true);
        KP_CHECK(k_sem_take(&cascade_done, K_SECONDS(2)) == 0);
        drain();
        KP_CHECK(action_count == cascade_actions && mutation_budget == 0 && interleavings == 12);
        if (multi_cascade) {
            const unsigned expected[] = {10,40,25,11,41,26,10,40,25,11,41,26,10,40,25,11,41,26,10,40};
            KP_CHECK(action_count == ARRAY_SIZE(expected));
            for (size_t n = 0; n < ARRAY_SIZE(expected); n++) KP_CHECK(actions[n].code == expected[n]);
        } else {
            for (unsigned n = 0; n < 13; n++) {
                KP_CHECK(actions[2 * n].code == (n % 2 ? 11 : 10));
                KP_CHECK(actions[2 * n + 1].code == (n % 2 ? 41 : 40));
            }
        }
        KP_CHECK(calls(DEV(cc_root)) == root_before + 13);
        KP_CHECK(calls(DEV(cc_right)) == right_before + 13);
        KP_CHECK(calls(DEV(cc_left)) == left_before + 13);
        KP_CHECK(kp_rgb_condition_valid(DEV(cc_root)) && kp_rgb_condition_valid(DEV(cc_right)));
        finite_cascade = false;
        unsigned before = action_count;
        k_msleep(100); drain(); KP_CHECK(action_count == before);
        set(DEV(cc_left), true); drain();
        set(DEV(cc_left), false); drain();
        KP_CHECK(action_count == before + 2 && actions[before].code == 20 && actions[before + 1].code == 21);
        action_error = 1;
        set(DEV(cc_root), false); drain();
        set(DEV(cc_root), true); drain();
        KP_CHECK(action_count == before + 6 && actions[before + 2].code == 11 &&
                 actions[before + 3].code == 41 && actions[before + 4].code == 10 &&
                 actions[before + 5].code == 40);
        KP_CHECK(interleavings == 12);
    }
    KP_CHECK(release_count == 0);
}

static void selection_cases(void) {
    wait_ready(); expect_selection(true);
    KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
    unsigned event_samples = calls(DEV(cc_event));
    if (kp_test_case("no-poll")) {
        select_a(false); expect_selection(false);
        unsigned settled = calls(DEV(cc_event));
        KP_CHECK(settled == event_samples + 1);
        k_msleep(100); drain();
        KP_CHECK(calls(DEV(cc_event)) == settled);
        return;
    }
    if (kp_test_case("cross-selector")) {
        unsigned samples_b = calls(DEV(cc_event_b)), root_samples = calls(DEV(cc_root));
        select_a(false); expect_selection(false);
        KP_CHECK(calls(DEV(cc_event)) == event_samples + 1);
        KP_CHECK(calls(DEV(cc_event_b)) == samples_b + 1);
        KP_CHECK(calls(DEV(cc_root)) == root_samples + 1);
        KP_CHECK(gate(DEV(cc_event_b_gate)));
        KP_CHECK(zmk_rgb_selector_select(DEV(selector_b), DEV(probe_d)) == 0);
        drain(); expect_selection(false);
        KP_CHECK(calls(DEV(cc_event)) == event_samples + 2);
        KP_CHECK(calls(DEV(cc_event_b)) == samples_b + 2);
        KP_CHECK(calls(DEV(cc_root)) == root_samples + 2);
        KP_CHECK(!gate(DEV(cc_event_b_gate))); return;
    }
    if (kp_test_case("selection-burst")) {
        unsigned samples_b = calls(DEV(cc_event_b)), root_samples = calls(DEV(cc_root));
        pause(DEV(cc_root));
        select_a(false); select_a(true); select_a(false);
        KP_CHECK(zmk_rgb_selector_select(DEV(selector_b), DEV(probe_d)) == 0);
        resume(); expect_selection(false);
        KP_CHECK(calls(DEV(cc_root)) == root_samples + 2);
        KP_CHECK(calls(DEV(cc_event)) == event_samples + 2);
        KP_CHECK(calls(DEV(cc_event_b)) == samples_b + 2);
        KP_CHECK(!gate(DEV(cc_event_b_gate))); return;
    }
    if (kp_test_case("trigger-mutation")) {
        unsigned root_samples = calls(DEV(cc_root));
        action_count = 0; selector_mutation = true;
        set(DEV(cc_root), true); expect_selection(peripheral());
        set(DEV(cc_root), false); expect_selection(true);
        unsigned passes = peripheral() ? 2 : 4;
        KP_CHECK(calls(DEV(cc_root)) == root_samples + passes);
        KP_CHECK(calls(DEV(cc_event)) == event_samples + passes);
        if (peripheral()) KP_CHECK(action_count == 0);
        else {
            const unsigned expected[] = {10, 40, 11, 41};
            KP_CHECK(action_count == ARRAY_SIZE(expected));
            for (size_t n = 0; n < ARRAY_SIZE(expected); n++)
                KP_CHECK(actions[n].code == expected[n]);
        }
        return;
    }
    if (kp_test_case("external-interruption") || kp_test_case("external-unused-interruption")) {
        pause(DEV(cc_event)); select_a(false);
        if (kp_test_case("external-unused-interruption"))
            atomic_set(&src(DEV(cc_unused))->value, true);
        irq_offload(interrupt, NULL); resume(); expect_selection(false);
        KP_CHECK(calls(DEV(cc_unused)) == 0); return;
    }
    if (!kp_test_case("initial-shared")) {
        select_a(false); expect_selection(false);
        KP_CHECK(calls(DEV(cc_event)) > event_samples);
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
    KP_CHECK(!kp_rgb_overlay_set_word(KP_RGB_OVERLAY_WORDS, UINT16_MAX));
    KP_CHECK(kp_rgb_overlay_get_word(KP_RGB_OVERLAY_WORDS) == 0);
    set(DEV(cc_root), true); drain();
    KP_CHECK(zmk_rgb_matrix_off() == 0); drain(); KP_CHECK(gate(DEV(ordinary)));
    zmk_rgb_matrix_set_inhibited(true); drain(); KP_CHECK(gate(DEV(ordinary)));
    set(DEV(cc_root), false); drain(); KP_CHECK(!gate(DEV(ordinary)));
    set(DEV(cc_root), true); drain(); KP_CHECK(gate(DEV(ordinary)));
}

static int auto_prepare(void) {
    const char *graphs[] = {"dag", "chain", "combinators", "invalid-scope",
        "scope", "short-circuit"};
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
        kp_rgb_condition_invalidate();
    }
#if DT_NODE_EXISTS(DT_NODELABEL(cc_noreg))
    atomic_set(&src(DEV(cc_noreg))->value, true);
#endif
#if IS_ENABLED(CONFIG_SETTINGS)
    if (kp_test_case("startup-positional") || kp_test_case("startup-settings-staged")) {
        const struct device *effect = kp_test_case("startup-positional") ?
            DEV(probe_a) : DEV(probe_c);
        KP_CHECK(store_snapshot(DEV(selector_a), effect) == 0);
    }
#endif
    KP_CHECK(calls(DEV(cc_root)) == 0);
    KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == NULL);
    KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == NULL);
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
    KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == NULL);
    KP_CHECK(calls(DEV(cc_event)) == 0 && calls(DEV(cc_event_b)) == 0);
    atomic_set(&src(DEV(cc_root))->value, true);
    provider_commits++;
    if (kp_test_case("startup-settings-staged")) return 0;
    KP_CHECK(store_snapshot(DEV(selector_a), DEV(probe_c)) == 0);
    return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE_WITH_CPRIO(cc_settings_source, "cc_settings_source",
    NULL, NULL, settings_provider_commit, NULL, INT_MAX - 1);
#endif

static void special_cases(void) {
    if (kp_test_case("startup-positional")) {
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
        KP_CHECK(calls(DEV(cc_event_b)) == 1);
        KP_CHECK(src(DEV(cc_root))->first_value && !src(DEV(cc_event))->first_value &&
                 src(DEV(cc_event_b))->first_value);
        KP_CHECK(gate(DEV(ordinary)));
        KP_CHECK(action_count == (peripheral() ? 0 : 3));
        if (!peripheral())
            KP_CHECK(actions[0].code == 10 && actions[1].code == 30 && actions[2].code == 40);
        KP_CHECK(settings_commit() == 0);
        KP_CHECK(settings_commit() == 0);
        drain();
        KP_CHECK(calls(DEV(cc_root)) == 1 && calls(DEV(cc_event)) == 1);
        KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_c));
        KP_CHECK(calls(DEV(cc_event_b)) == 1 && provider_commits == 1);
        KP_CHECK(action_count == (peripheral() ? 0 : 3));
        KP_CHECK(settings_commit_subtree(KP_RGB_PERSIST_SUBTREE) == 0);
        KP_CHECK(settings_commit_subtree(KP_RGB_PERSIST_SUBTREE) == 0);
        drain();
        KP_CHECK(calls(DEV(cc_root)) == 1 && calls(DEV(cc_event)) == 1 &&
                 calls(DEV(cc_event_b)) == 1);
        KP_CHECK(store_snapshot(DEV(selector_a), DEV(probe_a)) == 0);
        KP_CHECK(settings_commit_subtree(KP_RGB_PERSIST_SUBTREE) == 0);
        expect_selection(true);
        KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
        KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_b)) == DEV(probe_b));
        KP_CHECK(calls(DEV(cc_root)) == 2 && calls(DEV(cc_event)) == 2 &&
                 calls(DEV(cc_event_b)) == 2);
        KP_CHECK(settings_commit_subtree(KP_RGB_PERSIST_SUBTREE) == 0);
        drain();
        KP_CHECK(calls(DEV(cc_root)) == 2 && calls(DEV(cc_event)) == 2 &&
                 calls(DEV(cc_event_b)) == 2);
        KP_CHECK(provider_commits == 1 && action_count == (peripheral() ? 0 : 3));
        KP_CHECK(release_count == 0);
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
        unsigned samples = calls(DEV(cc_event));
        unsigned root_samples = calls(DEV(cc_root));
        select_a(false); drain();
        KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_c));
        unsigned passes = calls(DEV(cc_root)) - root_samples;
        KP_CHECK(passes >= 1 && calls(DEV(cc_event)) == samples + passes);
    } else if (kp_test_case("trigger-unregistered-root")) {
#if DT_NODE_EXISTS(DT_NODELABEL(cc_noreg))
        KP_CHECK(device_is_ready(DEV(cc_noreg)));
        wait_ready();
        KP_CHECK(calls(DEV(cc_noreg)) == 0 && !kp_rgb_condition_valid(DEV(cc_noreg)));
        if (!peripheral()) KP_CHECK(action_count == 1 && actions[0].code == 30);
        unsigned before = action_count;
        atomic_set(&src(DEV(cc_noreg))->value, false);
        kp_rgb_condition_invalidate(); drain();
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

#if DT_NODE_EXISTS(DT_NODELABEL(cc_observer))
static atomic_t observer_paints, observer_events;
static unsigned observer_epoch;
static void observer_fresh(void) {
    if (!observer_epoch) return;
    KP_CHECK(calls(DEV(cc_root)) == observer_epoch);
    KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_c));
}
static bool observer_render(const struct device *dev, const struct kp_rgb_frame *frame) {
    ARG_UNUSED(dev);
    observer_fresh();
    atomic_inc(&observer_paints);
    for (size_t i = 0; i < frame->target_count; i++)
        frame->pixels[frame->targets[i]] = (struct led_rgb){.r = 1};
    return false;
}
static bool observer_event(const struct device *dev, const struct kp_rgb_key_event *event) {
    ARG_UNUSED(dev);
    observer_fresh();
    KP_CHECK(event->pressed && event->timestamp_ms == 123);
    atomic_inc(&observer_events);
    return true;
}
static const struct kp_rgb_effect_callbacks observer_callbacks = {
    .render = observer_render, .on_event = observer_event,
};
#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT keypaw_native_engine_overlay
#define OBSERVER(inst) \
    KP_RGB_OVERLAY_TARGET_ARRAYS(inst, observer_##inst); \
    static const struct { struct kp_rgb_overlay_common_config common; } observer_##inst##_cfg = { \
        .common = KP_RGB_OVERLAY_COMMON(DT_DRV_INST(inst), &observer_callbacks, true), \
    }; \
    static struct { struct kp_rgb_overlay_common_data common; } observer_##inst##_data = { \
        KP_RGB_OVERLAY_COMMON_DATA(inst, observer_##inst), \
    }; \
    KP_RGB_OVERLAY_DEFINE(inst, observer_##inst);
DT_INST_FOREACH_STATUS_OKAY(OBSERVER)
#undef DT_DRV_COMPAT
#endif
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

static void stale_source_refresh(bool source_only, bool newer_resume) {
    bool reload = kp_test_case("runtime-reload-freshness");
    wait_ready();
    KP_CHECK(atomic_get(&strip_colors) > 0);
    if (!source_only) {
        KP_CHECK(zmk_rgb_matrix_set_inhibited(true) == 0); drain();
        KP_CHECK(atomic_get(&strip_blacks) > 0 && atomic_get(&strip_pixel_one) == 0);
    }
    unsigned before = atomic_get(&strip_colors);
    unsigned samples = calls(DEV(cc_root));
    if (source_only) atomic_set(&src(DEV(cc_root))->value, true);
    atomic_set(&src(DEV(cc_root))->pause, 1);
    if (source_only) kp_rgb_condition_invalidate();
    else KP_CHECK(zmk_rgb_matrix_set_inhibited(false) == 0);
    KP_CHECK(k_sem_take(&sample_entered, K_SECONDS(1)) == 0);
    KP_CHECK(atomic_get(&strip_colors) == before);
#if DT_NODE_EXISTS(DT_NODELABEL(cc_observer)) && IS_ENABLED(CONFIG_SETTINGS)
    unsigned paints = atomic_get(&observer_paints);
    unsigned blacks = atomic_get(&strip_blacks);
    if (reload) {
        KP_CHECK(paints > 0 && atomic_get(&observer_events) == 0);
        observer_epoch = samples + 2;
    }
#endif
    atomic_set(&src(DEV(cc_root))->value, !source_only);
    if (reload) {
#if DT_NODE_EXISTS(DT_NODELABEL(cc_observer)) && IS_ENABLED(CONFIG_SETTINGS)
        KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
        KP_CHECK(store_snapshot(DEV(selector_a), DEV(probe_c)) == 0);
        KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_a));
        KP_CHECK(settings_commit_subtree(KP_RGB_PERSIST_SUBTREE) == 0);
        KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_c));
        raise_zmk_position_state_changed((struct zmk_position_state_changed){
            .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL, .position = 0,
            .state = true, .timestamp = 123,
        });
#else
        KP_CHECK(false);
#endif
    } else irq_offload(interrupt, NULL);
    if (newer_resume) {
        select_a(false);
        KP_CHECK(zmk_rgb_matrix_set_inhibited(true) == 0);
        KP_CHECK(zmk_rgb_matrix_set_inhibited(false) == 0);
    }
    atomic_set(&src(DEV(cc_root))->pause, 1);
    k_sem_give(&sample_release);
    KP_CHECK(k_sem_take(&sample_entered, K_SECONDS(1)) == 0);
    KP_CHECK(calls(DEV(cc_root)) == samples + 2);
    KP_CHECK(atomic_get(&strip_colors) == before);
#if DT_NODE_EXISTS(DT_NODELABEL(cc_observer)) && IS_ENABLED(CONFIG_SETTINGS)
    if (reload) {
        KP_CHECK(atomic_get(&observer_paints) == paints && atomic_get(&observer_events) == 0);
        KP_CHECK(atomic_get(&strip_blacks) == blacks);
        KP_CHECK(atomic_get(&strip_pixel_one) == 0);
    }
#endif
    if (!source_only) KP_CHECK(!gate(DEV(ordinary)));
    if (newer_resume) {
        select_a(true); select_a(false);
        KP_CHECK(zmk_rgb_matrix_set_inhibited(true) == 0);
        KP_CHECK(zmk_rgb_matrix_set_inhibited(false) == 0);
        atomic_set(&src(DEV(cc_root))->pause, 1);
        k_sem_give(&sample_release);
        KP_CHECK(k_sem_take(&sample_entered, K_SECONDS(1)) == 0);
        KP_CHECK(calls(DEV(cc_root)) == samples + 3);
        KP_CHECK(atomic_get(&strip_colors) == before);
    }
    k_sem_give(&sample_release);
    int64_t until = k_uptime_get() + 1000;
    while (atomic_get(&strip_colors) == before && k_uptime_get() < until) k_msleep(1);
    drain();
    KP_CHECK(atomic_get(&strip_colors) == before + 1);
    KP_CHECK(calls(DEV(cc_root)) == samples + (newer_resume ? 3 : 2));
    KP_CHECK(gate(DEV(ordinary)) == !source_only);
    /* The central-gated underlay has no received gate word on this peripheral. */
    if (source_only) KP_CHECK(gate(DEV(selector_a)) == !peripheral());
    uint32_t pixel = source_only ? (peripheral() ? 0 : 0x00ff00) : 0xffffff;
    KP_CHECK(atomic_get(&strip_pixel_one) == pixel);
    if (newer_resume || reload) expect_selection(false);
#if DT_NODE_EXISTS(DT_NODELABEL(cc_observer)) && IS_ENABLED(CONFIG_SETTINGS)
    if (reload) {
        KP_CHECK(atomic_get(&observer_paints) == paints + 1 && atomic_get(&observer_events) == 1);
        KP_CHECK(atomic_get(&strip_blacks) == blacks);
        KP_CHECK(zmk_rgb_selector_get_selected(DEV(selector_a)) == DEV(probe_c));
    }
#endif
    KP_CHECK(irq_calls == (reload ? 0 : 1));
}
#endif

void kp_test_conditions_run(void) {
    if (kp_test_case("stale-source-refresh") || kp_test_case("pending-source") ||
        kp_test_case("resume-selection") || kp_test_case("runtime-reload-freshness")) {
#if DT_HAS_COMPAT_STATUS_OKAY(test_rgb_condition_contract_strip)
        stale_source_refresh(kp_test_case("pending-source"), kp_test_case("resume-selection"));
#else
        KP_CHECK(false);
#endif
    } else if (kp_test_case("startup-positional") ||
        kp_test_case("startup-auto") || kp_test_case("startup-unrestored-settings") ||
        kp_test_case("startup-settings-staged") ||
        kp_test_case("overlay-zero") || kp_test_case("trigger-no-table") ||
        kp_test_case("trigger-actions") || kp_test_case("trigger-unregistered-root")) special_cases();
    else if (kp_test_case("dag") || kp_test_case("chain") || kp_test_case("combinators") ||
        kp_test_case("invalid-scope") || kp_test_case("scope") ||
        kp_test_case("short-circuit")) graph_cases();
    else if (kp_test_case("startup") || kp_test_case("same-value") || kp_test_case("isr") ||
        kp_test_case("external-timer") || kp_test_case("refresh") || kp_test_case("refresh-interrupted") ||
        kp_test_case("early-refresh")) timing_cases();
    else if (kp_test_case("trigger-startup") || kp_test_case("trigger-capture") ||
        kp_test_case("trigger-errors") ||
        kp_test_case("finite-self-cascade") || kp_test_case("finite-multi-cascade")) trigger_cases();
    else if (kp_test_case("overlay-gate") || kp_test_case("split-visibility")) overlay_cases();
    else if (kp_test_case("unused-scope")) {
        wait_ready(); KP_CHECK(calls(DEV(cc_unused)) == 0);
        KP_CHECK(calls(DEV(cc_central)) == (peripheral() ? 0 : 1));
        KP_CHECK(kp_rgb_condition_valid(DEV(cc_root)));
    } else if (kp_test_case("initial-shared") || kp_test_case("no-poll") ||
        kp_test_case("cross-selector") || kp_test_case("third-party") ||
        kp_test_case("selection-burst") || kp_test_case("trigger-mutation") ||
        kp_test_case("external-interruption") || kp_test_case("external-unused-interruption")) selection_cases();
    else KP_CHECK(false);
}
