/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */
static bool ordering_case(void) { return !strncmp(split_case, "ordering-", 9); }

struct ordering_call {
    struct k_sem start, accepted, resume, done;
    struct zmk_behavior_binding binding;
    k_tid_t tid;
    bool pressed;
    int result;
    unsigned destinations;
};
static struct ordering_call ordering_calls[2];
static k_tid_t ordering_cursor_thread;
static bool ordering_enqueue_error;
static uint32_t ordering_remote[2];
static uint32_t ordering_inactive_color[2], ordering_inactive_period[2];
struct ordering_packet { uint32_t command, value; uint8_t source; };
static struct ordering_packet ordering_fifo[64];
static unsigned ordering_fifo_count;

static struct kp_rgb_replication_status ordering_status(void) {
    kp_rgb_matrix_lock();
    struct kp_rgb_replication_status value = kp_rgb_replication_status_locked();
    kp_rgb_matrix_unlock();
    return value;
}
static bool ordering_complete(uint64_t generation) {
    kp_rgb_matrix_lock();
    bool result = kp_rgb_replication_complete_locked(generation);
    kp_rgb_matrix_unlock();
    return result;
}
static void ordering_wait(struct k_sem *sem) {
    CHECK(k_sem_take(sem, K_SECONDS(2)) == 0);
}

static int ordering_send(uint8_t source, struct zmk_split_transport_central_command cmd) {
    struct ordering_call *call = NULL;
    for (unsigned i = 0; i < ARRAY_SIZE(ordering_calls); i++)
        if (ordering_calls[i].tid == k_current_get()) call = &ordering_calls[i];
    /* Old repair lanes see no sources; unsolicited coordinator traffic is not the cursor. */
    if (!call && k_current_get() != ordering_cursor_thread) return -ENOTCONN;
    uint32_t wire = cmd.data.invoke_behavior.param1;
    uint32_t command = kp_rgb_invocation_command(wire);
    CHECK(!strcmp(cmd.data.invoke_behavior.behavior_dev, DEV(selector_a)->name));
    if (call) {
        CHECK(wire & KP_RGB_INVOCATION_MARKER);
        CHECK(cmd.data.invoke_behavior.state == call->pressed);
        call->destinations |= BIT(source);
    } else {
        CHECK(!(wire & KP_RGB_INVOCATION_MARKER));
    }
    k_mutex_lock(&records_lock, K_FOREVER);
    CHECK(ordering_fifo_count < ARRAY_SIZE(ordering_fifo));
    ordering_fifo[ordering_fifo_count++] = (struct ordering_packet){
        .command = wire, .value = cmd.data.invoke_behavior.param2, .source = source};
    if (!(call && ordering_enqueue_error) && cmd.data.invoke_behavior.state &&
        command == KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, A_SLOT(probe_a)))
        ordering_remote[source] = cmd.data.invoke_behavior.param2;
    if (!(call && ordering_enqueue_error) && cmd.data.invoke_behavior.state) {
        if (command == KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, A_SLOT(probe_c)))
            ordering_inactive_color[source] = cmd.data.invoke_behavior.param2;
        if (command == KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD, A_SLOT(probe_c)))
            ordering_inactive_period[source] = cmd.data.invoke_behavior.param2;
    }
    k_mutex_unlock(&records_lock);
    /* Pause after both destination enqueues, before pinned ZMK's local callback. */
    if (call && source == 1) {
        k_sem_give(&query_request);
        ordering_wait(&query_done);
        k_sem_give(&call->accepted);
        ordering_wait(&call->resume);
    }
    return call && ordering_enqueue_error ? -EIO : 0;
}

static void ordering_invoker(void *arg, void *b, void *c) {
    ARG_UNUSED(b); ARG_UNUSED(c);
    struct ordering_call *call = arg;
    ordering_wait(&call->start);
    call->tid = k_current_get();
    call->result = zmk_behavior_invoke_binding(&call->binding,
        (struct zmk_behavior_binding_event){0}, call->pressed);
    k_sem_give(&call->done);
}
K_THREAD_STACK_ARRAY_DEFINE(ordering_stacks, 2, 2048);
static struct k_thread ordering_threads[2];

static void ordering_start(unsigned index, uint32_t color, bool pressed) {
    struct ordering_call *call = &ordering_calls[index];
    k_sem_init(&call->start, 0, 1); k_sem_init(&call->accepted, 0, 1);
    k_sem_init(&call->resume, 0, 1); k_sem_init(&call->done, 0, 1);
    call->binding = (struct zmk_behavior_binding){.behavior_dev = DEV(selector_a)->name,
        .param1 = KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, A_SLOT(probe_a)),
        .param2 = color};
    call->pressed = pressed;
    k_thread_create(&ordering_threads[index], ordering_stacks[index],
        K_THREAD_STACK_SIZEOF(ordering_stacks[index]), ordering_invoker, call, NULL, NULL,
        7, 0, K_NO_WAIT);
    k_sem_give(&call->start);
    ordering_wait(&call->accepted);
    CHECK(call->destinations == (BIT(0) | BIT(1)));
}
static void ordering_finish(unsigned index) {
    k_sem_give(&ordering_calls[index].resume);
    ordering_wait(&ordering_calls[index].done);
    CHECK(ordering_calls[index].result == 0);
}
static struct kp_rgb_selector_snapshot ordering_snapshot(void) {
    struct kp_rgb_selector_snapshot snapshot;
    kp_rgb_matrix_lock();
    CHECK(kp_rgb_selector_snapshot_locked(DEV(selector_a), &snapshot) == 0);
    kp_rgb_matrix_unlock();
    return snapshot;
}
static uint32_t ordering_color(struct kp_rgb_selector_snapshot snapshot) {
    return RGB_COLOR_HSB_VAL(snapshot.color.h, snapshot.color.s, snapshot.color.b);
}
static void ordering_replay(struct kp_rgb_selector_snapshot snapshot) {
    for (uint8_t source = 0; source < 2; source++)
        for (uint8_t step = 0; step < 3; step++)
            CHECK(kp_rgb_split_send_selector(source, &snapshot, step) == 0);
}

static struct kp_rgb_selector_snapshot ordering_inactive_snapshot(void) {
    size_t cursor = 0;
    struct kp_rgb_selector_snapshot snapshot;
    kp_rgb_matrix_lock();
    int next;
    while ((next = kp_rgb_selector_next_snapshot_locked(DEV(selector_a), &cursor, &snapshot)) > 0)
        if (snapshot.slot == A_SLOT(probe_c)) break;
    CHECK(next > 0);
    kp_rgb_matrix_unlock();
    return snapshot;
}
static void ordering_fifo_case(void) {
    uint32_t live = RGB_COLOR_HSB_VAL(211, 73, 91);
    bool release = !strcmp(split_case, "ordering-release");
    bool noop = !strcmp(split_case, "ordering-noop");
    bool reset = !strcmp(split_case, "ordering-reset");
    ordering_enqueue_error = !strcmp(split_case, "ordering-enqueue-error");
    if (reset) {
        CHECK(zmk_rgb_matrix_set_color(DEV(probe_c),
            &(struct kp_rgb_hsb){.h = 17, .s = 23, .b = 41}) == 0);
        CHECK(zmk_rgb_matrix_set_period(DEV(probe_c), 2345) == 0);
    }
    struct kp_rgb_selector_snapshot old = ordering_snapshot();
    struct kp_rgb_selector_snapshot old_inactive = ordering_inactive_snapshot();
    uint64_t generation = ordering_status().generation;
    if (noop) live = ordering_color(old);
    ordering_start(0, live, !release);
    struct kp_rgb_replication_status paused = ordering_status();
    CHECK(paused.outstanding == 1 && paused.generation == generation);
    if (!strcmp(split_case, "ordering-during")) generation = paused.generation;
    CHECK(ordering_color(ordering_snapshot()) == ordering_color(old));
    if (!release && !ordering_enqueue_error)
        CHECK(ordering_remote[0] == live && ordering_remote[1] == live);
    ordering_replay(old);
    CHECK(ordering_fifo_count == 8);
    CHECK(ordering_fifo[0].command & KP_RGB_INVOCATION_MARKER);
    CHECK(ordering_fifo[1].command & KP_RGB_INVOCATION_MARKER);
    CHECK(!(ordering_fifo[2].command & KP_RGB_INVOCATION_MARKER));
    CHECK(ordering_fifo[2].value == ordering_color(old));
    CHECK(ordering_remote[0] == ordering_color(old) && ordering_remote[1] == ordering_color(old));
    CHECK(!ordering_complete(generation));
    CHECK(!ordering_complete(paused.generation));
    if (reset) {
        global_apply(RGB_RESET_CMD, 0);
        CHECK(kp_rgb_effect_data(DEV(probe_c))->color.h == 240);
        CHECK(kp_rgb_effect_period(DEV(probe_c)) != 2345);
        CHECK(ordering_status().generation > paused.generation);
        CHECK(ordering_status().outstanding == 1);
        ordering_replay(old_inactive);
        CHECK(ordering_inactive_color[0] == ordering_color(old_inactive));
        CHECK(ordering_inactive_period[1] == old_inactive.period_ms);
        CHECK(!ordering_complete(paused.generation));
    }
    ordering_finish(0);
    struct kp_rgb_replication_status finished = ordering_status();
    CHECK(finished.outstanding == 0 && finished.generation > paused.generation);
    if (noop) CHECK(finished.generation == paused.generation + 1);
    CHECK(!ordering_complete(generation) && !ordering_complete(paused.generation));
    struct kp_rgb_selector_snapshot fresh = ordering_snapshot();
    CHECK(ordering_color(fresh) == ((release || noop) ? ordering_color(old) : live));
    if (reset) {
        struct kp_rgb_selector_snapshot inactive = ordering_inactive_snapshot();
        ordering_replay(inactive);
        for (unsigned source = 0; source < 2; source++) {
            CHECK(ordering_inactive_color[source] == ordering_color(inactive));
            CHECK(ordering_inactive_period[source] == inactive.period_ms);
        }
    }
    ordering_replay(fresh);
    CHECK(ordering_remote[0] == ordering_color(fresh) && ordering_remote[1] == ordering_color(fresh));
    CHECK(ordering_complete(finished.generation));
}

static void ordering_overlap(void) {
    uint64_t before = ordering_status().generation;
    ordering_start(0, RGB_COLOR_HSB_VAL(111, 70, 90), true);
    ordering_start(1, RGB_COLOR_HSB_VAL(222, 80, 90), true);
    struct kp_rgb_replication_status both = ordering_status();
    CHECK(both.outstanding == 2 && both.generation == before);
    CHECK(!ordering_complete(both.generation));
    unsigned first = !strcmp(split_case, "ordering-overlap-reverse") ? 1 : 0;
    ordering_finish(first);
    struct kp_rgb_replication_status one = ordering_status();
    CHECK(one.outstanding == 1 && one.generation > both.generation);
    CHECK(!ordering_complete(one.generation));
    ordering_replay(ordering_snapshot());
    CHECK(!ordering_complete(one.generation));
    ordering_finish(1 - first);
    struct kp_rgb_replication_status done = ordering_status();
    CHECK(done.outstanding == 0 && done.generation > one.generation);
    CHECK(!ordering_complete(both.generation) && !ordering_complete(one.generation));
    struct kp_rgb_selector_snapshot fresh = ordering_snapshot();
    ordering_replay(fresh);
    CHECK(ordering_remote[0] == ordering_color(fresh) && ordering_remote[1] == ordering_color(fresh));
    CHECK(ordering_complete(done.generation));
}

static void ordering_defaults(void) {
    kp_rgb_matrix_lock();
    struct kp_rgb_selector *selector = NULL;
    for (size_t i = 0; i < kp_rgb_selector_registry_count(); i++) {
        struct kp_rgb_selector *candidate = kp_rgb_selector_registry_at(i);
        if (candidate->owner == DEV(selector_a)) selector = candidate;
    }
    CHECK(selector != NULL);
    CHECK(kp_rgb_selector_selected(selector) != DEV(probe_c));
    kp_rgb_selector_apply_defaults_locked(selector);
    struct kp_rgb_effect_common_data *data = kp_rgb_effect_data(DEV(probe_c));
    const struct kp_rgb_effect_common_data *defaults = &kp_rgb_effect_cfg(DEV(probe_c))->defaults;
    data->color = defaults->color;
    data->color.h = (defaults->color.h + 1) % 360;
    data->duration_ms = defaults->duration_ms == UINT16_MAX ?
        defaults->duration_ms - 1 : defaults->duration_ms + 1;
    struct kp_rgb_replication_status changed = kp_rgb_replication_status_locked();
    kp_rgb_selector_apply_defaults_locked(selector);
    struct kp_rgb_replication_status restored = kp_rgb_replication_status_locked();
    CHECK(restored.outstanding == changed.outstanding);
    CHECK(restored.generation == changed.generation + 1);
    CHECK(data->color.h == kp_rgb_effect_cfg(DEV(probe_c))->defaults.color.h);
    CHECK(data->color.s == kp_rgb_effect_cfg(DEV(probe_c))->defaults.color.s);
    CHECK(data->color.b == kp_rgb_effect_cfg(DEV(probe_c))->defaults.color.b);
    CHECK(data->duration_ms == kp_rgb_effect_cfg(DEV(probe_c))->defaults.duration_ms);
    kp_rgb_selector_apply_defaults_locked(selector);
    CHECK(kp_rgb_replication_status_locked().generation == restored.generation);
    CHECK(kp_rgb_replication_status_locked().outstanding == restored.outstanding);
    kp_rgb_matrix_unlock();
}

static int ordering_callback(struct zmk_behavior_binding *binding, bool pressed) {
    return pressed ? behavior_keymap_binding_pressed(binding, (struct zmk_behavior_binding_event){0}) :
                     behavior_keymap_binding_released(binding, (struct zmk_behavior_binding_event){0});
}
static int ordering_irq_result;
static void ordering_irq(const void *arg) {
    CHECK(k_is_in_isr());
    ordering_irq_result = ordering_callback((struct zmk_behavior_binding *)arg, true);
}
static void ordering_cleanup(void) {
    for (unsigned global = 0; global < 2; global++) {
        const struct device *endpoint = global ? DEV(rgb_matrix) : DEV(selector_a);
        for (unsigned mode = 0; mode < 4; mode++) {
            struct zmk_behavior_binding binding = {.behavior_dev = endpoint->name,
                .param1 = global ? RGB_BRT_SET_CMD : RGB_EFS_CMD, .param2 = 0};
            struct kp_rgb_replication_status before = ordering_status();
            CHECK(behavior_keymap_binding_convert_central_state_dependent_params(
                &binding, (struct zmk_behavior_binding_event){0}) == 0);
            CHECK(binding.param1 & KP_RGB_INVOCATION_MARKER);
            CHECK(ordering_status().outstanding == before.outstanding + 1);
            CHECK(ordering_status().generation == before.generation);
            struct zmk_behavior_binding direct = {.behavior_dev = endpoint->name,
                .param1 = global ? RGB_BRT_SET_CMD : RGB_EFS_CMD, .param2 = 0};
            CHECK(ordering_callback(&direct, true) == 0);
            CHECK(ordering_callback(&direct, false) == 0);
            CHECK(ordering_status().outstanding == before.outstanding + 1);
            struct kp_rgb_selector_snapshot snapshot = ordering_snapshot();
            kp_rgb_selector_snapshot_binding(&snapshot, 0, &direct);
            CHECK(!(direct.param1 & KP_RGB_INVOCATION_MARKER));
            CHECK(ordering_callback(&direct, true) == 0);
            CHECK(ordering_status().outstanding == before.outstanding + 1);
            uint64_t pending = ordering_status().generation;
            CHECK(!ordering_complete(pending));
            CHECK(ordering_status().outstanding == before.outstanding + 1);
            if (mode == 2) binding.param2 = UINT32_MAX;
            if (mode == 3) {
                irq_offload(ordering_irq, &binding);
                CHECK(ordering_irq_result == -EWOULDBLOCK);
            } else {
                int result = ordering_callback(&binding, mode != 1);
                CHECK(mode == 2 ? result < 0 : result == 0);
            }
            CHECK(!(binding.param1 & KP_RGB_INVOCATION_MARKER));
            CHECK(ordering_status().outstanding == before.outstanding);
            CHECK(ordering_status().generation > pending);
        }
        struct zmk_behavior_binding invalid = {.behavior_dev = endpoint->name,
            .param1 = 0x10a, .param2 = 0};
        struct kp_rgb_replication_status before = ordering_status();
        CHECK(behavior_keymap_binding_convert_central_state_dependent_params(
            &invalid, (struct zmk_behavior_binding_event){0}) < 0);
        CHECK(!(invalid.param1 & KP_RGB_INVOCATION_MARKER));
        CHECK(ordering_status().outstanding == before.outstanding);
        CHECK(ordering_status().generation == before.generation);
        const uint32_t reserved[] = {UINT32_MAX, RGB_RESET_CMD | KP_RGB_INVOCATION_MARKER};
        for (size_t i = 0; i < ARRAY_SIZE(reserved); i++) {
            invalid.param1 = reserved[i];
            CHECK(behavior_keymap_binding_convert_central_state_dependent_params(
                &invalid, (struct zmk_behavior_binding_event){0}) == -ENOTSUP);
            CHECK(invalid.param1 == reserved[i]);
            CHECK(ordering_status().outstanding == before.outstanding);
            CHECK(ordering_status().generation == before.generation);
        }
    }
    struct kp_rgb_selector_snapshot high = {.control = DEV(selector_a), .slot = UINT16_MAX,
        .color = {.h = 1, .s = 2, .b = 3}, .period_ms = 1234};
    struct zmk_behavior_binding encoded;
    kp_rgb_selector_snapshot_binding(&high, 0, &encoded);
    uint32_t wire = encoded.param1 | KP_RGB_INVOCATION_MARKER;
    CHECK((kp_rgb_invocation_command(wire) >> 16) == UINT16_MAX);
    CHECK(kp_rgb_invocation_command(wire) == encoded.param1);
    struct kp_rgb_replication_status before = ordering_status();
    encoded = (struct zmk_behavior_binding){.behavior_dev = DEV(selector_a)->name,
        .param1 = RGB_HUI_CMD};
    CHECK(kp_rgb_selector_binding_convert(DEV(selector_a), &encoded) == 0);
    CHECK(!(encoded.param1 & KP_RGB_INVOCATION_MARKER));
    CHECK(ordering_status().generation == before.generation);
    CHECK(ordering_status().outstanding == 0);
    encoded = (struct zmk_behavior_binding){.behavior_dev = DEV(rgb_matrix)->name,
        .param1 = RGB_CLOCK_CMD, .param2 = 123456};
    CHECK(behavior_keymap_binding_convert_central_state_dependent_params(
        &encoded, (struct zmk_behavior_binding_event){0}) == 0);
    CHECK(!(encoded.param1 & KP_RGB_INVOCATION_MARKER));
    CHECK(ordering_status().generation == before.generation);
    CHECK(ordering_callback(&encoded, true) == 0);
    CHECK(ordering_status().generation == before.generation);
    CHECK(ordering_status().outstanding == 0);
}

#define ORDERING_PUBLICATION(operation, changed) do { \
    kp_rgb_matrix_lock(); \
    struct kp_rgb_replication_status prior = kp_rgb_replication_status_locked(); \
    operation; \
    struct kp_rgb_replication_status after = kp_rgb_replication_status_locked(); \
    CHECK(after.outstanding == prior.outstanding); \
    CHECK((changed) ? after.generation > prior.generation : after.generation == prior.generation); \
    kp_rgb_matrix_unlock(); \
} while (0)

static void ordering_publications(void) {
    ORDERING_PUBLICATION(CHECK(zmk_rgb_matrix_on() == 0), true);
    ORDERING_PUBLICATION(CHECK(zmk_rgb_matrix_on() == 0), false);
    ORDERING_PUBLICATION(CHECK(zmk_rgb_matrix_off() == 0), true);
    ORDERING_PUBLICATION(global_apply(RGB_BRT_SET_CMD, 37), true);
    ORDERING_PUBLICATION(global_apply(RGB_BRT_SET_CMD, 37), false);
    const struct device *members[] = {DEV(probe_a), DEV(probe_c)};
    for (size_t i = 0; i < ARRAY_SIZE(members); i++) {
        struct kp_rgb_hsb color = {.h = 19, .s = 83, .b = 67};
        ORDERING_PUBLICATION(CHECK(zmk_rgb_matrix_set_color(members[i], &color) == 0), true);
        ORDERING_PUBLICATION(CHECK(zmk_rgb_matrix_set_color(members[i], &color) == 0), false);
        ORDERING_PUBLICATION(CHECK(zmk_rgb_matrix_set_period(members[i], 2345) == 0), true);
        ORDERING_PUBLICATION(CHECK(zmk_rgb_matrix_set_period(members[i], 2345) == 0), false);
    }
    ORDERING_PUBLICATION(CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(probe_c)) == 0), true);
    ORDERING_PUBLICATION(CHECK(zmk_rgb_selector_select(DEV(selector_a), DEV(probe_c)) == 0), false);
    ORDERING_PUBLICATION(global_apply(RGB_RESET_CMD, 0), true);
    CHECK(kp_rgb_effect_data(DEV(probe_c))->color.h == 240);
    ORDERING_PUBLICATION(global_apply(RGB_CLOCK_CMD, 123456), false);
    ORDERING_PUBLICATION(CHECK(zmk_rgb_matrix_set_inhibited(false) == 0), false);
    struct kp_rgb_hsb private_color = {.h = 89, .s = 50, .b = 60};
    ORDERING_PUBLICATION(CHECK(zmk_rgb_matrix_set_color(DEV(private_white), &private_color) == 0), false);
    ORDERING_PUBLICATION(CHECK(zmk_rgb_matrix_set_period(DEV(private_white), 2345) == 0), false);
    kp_rgb_matrix_lock();
    uint16_t word = kp_rgb_overlay_get_word(0);
    ORDERING_PUBLICATION(CHECK(kp_rgb_overlay_set_word(0, word ^ 1)), true);
    ORDERING_PUBLICATION(CHECK(!kp_rgb_overlay_set_word(0, word ^ 1)), false);
    ORDERING_PUBLICATION(CHECK(kp_rgb_overlay_set_word(0, word)), true);
    kp_rgb_matrix_unlock();
}
#undef ORDERING_PUBLICATION

static K_SEM_DEFINE(ordering_barrier_done, 0, 1);
static void ordering_barrier_work(struct k_work *work) {
    ARG_UNUSED(work);
    k_sem_give(&ordering_barrier_done);
}
static K_WORK_DEFINE(ordering_barrier_item, ordering_barrier_work);
static void ordering_barrier(void) {
    CHECK(k_work_submit_to_queue(kp_rgb_work_q(), &ordering_barrier_item) >= 0);
    ordering_wait(&ordering_barrier_done);
}

#if DT_NODE_EXISTS(DT_NODELABEL(ordering_local))
static void ordering_wait_gate(const struct device *overlay, bool expected) {
    for (unsigned tries = 0; tries < 100; tries++) {
        ordering_barrier();
        kp_rgb_matrix_lock();
        const struct kp_rgb_overlay_common_data *data = overlay->data;
        bool settled = data->gate == expected &&
                       kp_rgb_replication_status_locked().outstanding == 0;
        kp_rgb_matrix_unlock();
        if (settled) return;
    }
    CHECK(false);
}

static void ordering_gates(void) {
    CHECK(zmk_rgb_selector_get_selected(DEV(preview_selector)) == DEV(fx_solid));
    ordering_wait_gate(DEV(selector_a), true);
    ordering_wait_gate(DEV(ordinary), false);
    for (unsigned pass = 0; pass < 2; pass++) {
        bool enabled = pass == 0;
        kp_rgb_matrix_lock();
        const struct kp_rgb_overlay_common_data *local = DEV(ordinary)->data;
        CHECK(local->local);
        uint16_t word = kp_rgb_overlay_get_word(0);
        uint64_t before = kp_rgb_replication_status_locked().generation;
        CHECK(zmk_rgb_selector_select(DEV(selector_b), enabled ? DEV(probe_d) : DEV(probe_b)) == 0);
        struct kp_rgb_replication_status selected = kp_rgb_replication_status_locked();
        CHECK(selected.generation > before && selected.outstanding == 0);
        kp_rgb_matrix_unlock();
        ordering_wait_gate(DEV(ordinary), enabled);
        kp_rgb_matrix_lock();
        CHECK(kp_rgb_overlay_get_word(0) == word);
        CHECK(kp_rgb_replication_status_locked().generation == selected.generation);
        kp_rgb_matrix_unlock();
    }
    for (unsigned pass = 0; pass < 2; pass++) {
        bool enabled = pass != 0;
        kp_rgb_matrix_lock();
        uint16_t word = kp_rgb_overlay_get_word(0);
        CHECK(zmk_rgb_selector_select(DEV(preview_selector), enabled ? DEV(fx_solid) : DEV(fx_breathe)) == 0);
        struct kp_rgb_replication_status selected = kp_rgb_replication_status_locked();
        kp_rgb_matrix_unlock();
        ordering_wait_gate(DEV(selector_a), enabled);
        kp_rgb_matrix_lock();
        CHECK(kp_rgb_overlay_get_word(0) != word);
        CHECK(kp_rgb_replication_status_locked().generation > selected.generation);
        CHECK(kp_rgb_replication_status_locked().outstanding == 0);
        kp_rgb_matrix_unlock();
    }
    ordering_barrier();
    struct kp_rgb_replication_status settled = ordering_status();
    ordering_barrier();
    CHECK(ordering_status().generation == settled.generation);
    CHECK(ordering_status().outstanding == 0 && ordering_fifo_count == 0);
}
#endif

#if IS_ENABLED(CONFIG_SETTINGS_RUNTIME)
#include <zephyr/settings/settings.h>
#include "rgb_persist_internal.h"

static void ordering_settings(void) {
    const struct device *effects[] = {DEV(probe_a), DEV(probe_c)};
    struct kp_rgb_hsb color = {.h = 217, .s = 43, .b = 61};
    for (size_t i = 0; i < ARRAY_SIZE(effects); i++) {
        CHECK(zmk_rgb_matrix_set_color(effects[i], &color) == 0);
        CHECK(zmk_rgb_matrix_set_period(effects[i], 2345) == 0);
    }
    ordering_barrier();
    kp_rgb_matrix_lock();
    size_t count = kp_rgb_selector_registry_count();
    CHECK(count <= KP_RGB_OVERLAY_COUNT && count <= UINT16_MAX);
    uint16_t slots[MAX(1, KP_RGB_OVERLAY_COUNT)];
    size_t target = SIZE_MAX;
    for (size_t i = 0; i < count; i++) {
        const struct kp_rgb_selector *selector = kp_rgb_selector_registry_at(i);
        slots[i] = selector->effect_index;
        if (selector->owner == DEV(selector_a)) target = i;
    }
    CHECK(target != SIZE_MAX);
    struct kp_rgb_persist_state state = {.user_on = kp_rgb_global.user_on,
        .brightness = kp_rgb_global.brightness, .selection_count = count};
    uint8_t bytes[KP_RGB_PERSIST_SIZE(MAX(1, KP_RGB_OVERLAY_COUNT))];
    size_t len = KP_RGB_PERSIST_SIZE(count);
    for (unsigned pass = 0; pass < 4; pass++) {
        if (pass == 1) state.user_on = !state.user_on;
        if (pass == 2) state.brightness = state.brightness == 37 ? 38 : 37;
        if (pass == 3) slots[target] = A_SLOT(probe_c);
        CHECK(kp_rgb_persist_encode(&state, slots, bytes, len) == 0);
        struct kp_rgb_replication_status before = kp_rgb_replication_status_locked();
        CHECK(before.outstanding == 0);
        bool previous_on = kp_rgb_global.user_on;
        uint8_t previous_brightness = kp_rgb_global.brightness;
        size_t previous_selection = kp_rgb_selector_registry_at(target)->effect_index;
        CHECK(settings_runtime_set(KP_RGB_PERSIST_KEY, bytes, len) == 0);
        CHECK(kp_rgb_replication_status_locked().generation == before.generation);
        CHECK(kp_rgb_replication_status_locked().outstanding == 0);
        CHECK(kp_rgb_global.user_on == previous_on);
        CHECK(kp_rgb_global.brightness == previous_brightness);
        CHECK(kp_rgb_selector_registry_at(target)->effect_index == previous_selection);
        /* runtime_commit invokes the registered handler's real h_commit without storage. */
        CHECK(settings_runtime_commit(KP_RGB_PERSIST_SUBTREE) == 0);
        struct kp_rgb_replication_status committed = kp_rgb_replication_status_locked();
        CHECK(pass ? committed.generation > before.generation :
                     committed.generation == before.generation);
        CHECK(committed.outstanding == 0);
        CHECK(kp_rgb_global.user_on == state.user_on && kp_rgb_global.brightness == state.brightness);
        for (size_t i = 0; i < count; i++)
            CHECK(kp_rgb_selector_registry_at(i)->effect_index == slots[i]);
        for (size_t i = 0; i < ARRAY_SIZE(effects); i++) {
            const struct kp_rgb_hsb *actual = &kp_rgb_effect_data(effects[i])->color;
            CHECK(actual->h == color.h && actual->s == color.s && actual->b == color.b);
            CHECK(kp_rgb_effect_period(effects[i]) == 2345);
        }
        CHECK(settings_runtime_commit(KP_RGB_PERSIST_SUBTREE) == 0);
        CHECK(kp_rgb_replication_status_locked().generation == committed.generation);
        CHECK(settings_runtime_set(KP_RGB_PERSIST_KEY, bytes, len) == 0);
        CHECK(settings_runtime_commit(KP_RGB_PERSIST_SUBTREE) == 0);
        CHECK(kp_rgb_replication_status_locked().generation == committed.generation);
        CHECK(kp_rgb_replication_status_locked().outstanding == 0);
    }
    kp_rgb_matrix_unlock();
    ordering_barrier();
    CHECK(ordering_status().outstanding == 0);
    CHECK(ordering_fifo_count == 0);
}
#endif
static void ordering_run(void) {
    ordering_cursor_thread = k_current_get();
    CHECK(ordering_status().outstanding == 0);
    if (!strcmp(split_case, "ordering-publications")) ordering_publications();
    else if (!strcmp(split_case, "ordering-defaults")) ordering_defaults();
#if DT_NODE_EXISTS(DT_NODELABEL(ordering_local))
    else if (!strcmp(split_case, "ordering-gates")) ordering_gates();
#endif
#if IS_ENABLED(CONFIG_SETTINGS_RUNTIME)
    else if (!strcmp(split_case, "ordering-settings")) ordering_settings();
#endif
    else if (!strcmp(split_case, "ordering-cleanup")) ordering_cleanup();
    else if (!strncmp(split_case, "ordering-overlap-", 17)) ordering_overlap();
    else ordering_fifo_case();
    CHECK(ordering_status().outstanding == 0);
    printk("PASS split %s\n", split_case);
    exit(0);
}
