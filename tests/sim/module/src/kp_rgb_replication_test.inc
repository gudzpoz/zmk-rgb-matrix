/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */
static bool ordering_case(void) { return !strncmp(split_case, "ordering-", 9); }
struct ordering_call {
    struct k_sem accepted, resume, done;
    struct zmk_behavior_binding binding;
    k_tid_t tid;
    bool pressed, enqueue_error;
    int result;
    unsigned destinations;
    uint32_t expected_color;
};
static struct ordering_call ordering_calls[2];
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
static void ordering_wait(struct k_sem *sem) { bounded_wait(sem); }
static int ordering_live_send(uint8_t source, struct zmk_split_transport_central_command cmd) {
    struct ordering_call *call = NULL;
    for (unsigned i = 0; i < ARRAY_SIZE(ordering_calls); i++)
        if (ordering_calls[i].tid == k_current_get()) call = &ordering_calls[i];
    if (!call) return -ENOMSG;
    CHECK(cmd.data.invoke_behavior.param1 & KP_RGB_INVOCATION_MARKER);
    CHECK(kp_rgb_invocation_command(cmd.data.invoke_behavior.param1) ==
        KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, A_SLOT(probe_a)));
    CHECK(cmd.data.invoke_behavior.param2 == call->expected_color);
    CHECK(cmd.data.invoke_behavior.state == call->pressed);
    CHECK(cmd.data.invoke_behavior.position == 0 && cmd.data.invoke_behavior.event_source == 0);
    CHECK(!strcmp(cmd.data.invoke_behavior.behavior_dev, DEV(selector_a)->name));
    call->destinations |= BIT(source);
    unsigned index = record_packet(source, cmd, !call->enqueue_error && call->pressed, true);
    if (source == 1) {
        k_sem_give(&query_request); ordering_wait(&query_done);
        k_sem_give(&call->accepted); ordering_wait(&call->resume);
    }
    k_mutex_lock(&records_lock, K_FOREVER);
    attempts[index].completion = k_uptime_get();
    k_mutex_unlock(&records_lock);
    return call->enqueue_error ? -EIO : 0;
}
static void ordering_invoker(void *arg, void *b, void *c) {
    ARG_UNUSED(b); ARG_UNUSED(c);
    struct ordering_call *call = arg;
    call->tid = k_current_get();
    call->result = zmk_behavior_invoke_binding(&call->binding,
        (struct zmk_behavior_binding_event){0}, call->pressed);
    call->tid = NULL;
    k_sem_give(&call->done);
}
K_THREAD_STACK_ARRAY_DEFINE(ordering_stacks, 2, 2048);
static struct k_thread ordering_threads[2];
static void ordering_start(unsigned index, uint32_t color, bool pressed) {
    struct ordering_call *call = &ordering_calls[index];
    k_sem_init(&call->accepted, 0, 1); k_sem_init(&call->resume, 0, 1);
    k_sem_init(&call->done, 0, 1);
    call->destinations = 0;
    call->binding = (struct zmk_behavior_binding){.behavior_dev = DEV(selector_a)->name,
        .param1 = KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, A_SLOT(probe_a)),
        .param2 = color};
    call->expected_color = color;
    if (!strcmp(split_case, "ordering-relative")) {
        call->binding.param1 = RGB_HUI_CMD;
        call->binding.param2 = 0;
    }
    call->pressed = pressed;
    k_thread_create(&ordering_threads[index], ordering_stacks[index],
        K_THREAD_STACK_SIZEOF(ordering_stacks[index]), ordering_invoker, call, NULL, NULL,
        7, 0, K_NO_WAIT);
    ordering_wait(&call->accepted);
    CHECK(call->destinations == (BIT(0) | BIT(1)));
}
static void ordering_finish(unsigned index) {
    k_sem_give(&ordering_calls[index].resume);
    ordering_wait(&ordering_calls[index].done);
    CHECK(ordering_calls[index].result == 0);
    CHECK(k_thread_join(&ordering_threads[index], K_SECONDS(1)) == 0);
}
static uint32_t ordering_color(void) {
    kp_rgb_matrix_lock();
    struct kp_rgb_hsb color = kp_rgb_effect_data(DEV(probe_a))->color;
    kp_rgb_matrix_unlock();
    return RGB_COLOR_HSB_VAL(color.h, color.s, color.b);
}
static void ordering_fifo_case(void) {
    uint32_t command = KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, A_SLOT(probe_a));
    uint32_t old = ordering_color(), live = RGB_COLOR_HSB_VAL(211, 73, 91);
    bool during = !strcmp(split_case, "ordering-during");
    bool release = !strcmp(split_case, "ordering-release");
    bool noop = !strcmp(split_case, "ordering-noop");
    bool idle = release || noop || !strcmp(split_case, "ordering-enqueue-error") ||
                !strcmp(split_case, "ordering-isolated") || !strcmp(split_case, "ordering-relative");
    ordering_calls[0].enqueue_error = !strcmp(split_case, "ordering-enqueue-error");
    if (noop) live = old;
    if (!strcmp(split_case, "ordering-relative")) {
        kp_rgb_matrix_lock();
        struct kp_rgb_hsb color = kp_rgb_effect_data(DEV(probe_a))->color;
        kp_rgb_matrix_unlock();
        live = RGB_COLOR_HSB_VAL((color.h + CONFIG_KEYPAW_RGB_MATRIX_HUE_STEP) % 360, color.s, color.b);
    }
    if (idle) { connect(3); settle(); quiet(); }
    else if (!during) {
        arm_worker(0, RGB_BRT_SET_CMD, 0); connect(3); ordering_wait(&worker_accepted);
    }
    struct kp_rgb_replication_status before = ordering_status();
    unsigned count = state_count();
    ordering_start(0, live, !release);
    CHECK(ordering_status().outstanding == 1);
    CHECK(ordering_status().generation == before.generation);
    CHECK(ordering_color() == old);
    CHECK(!ordering_complete(before.generation));
    if (!idle) {
        if (during) { arm_worker(0, command, 0); connect(3); }
        else { arm_worker(0, command, 0); k_sem_give(&worker_resume); }
        ordering_wait(&worker_accepted);
        CHECK(attempts[worker_gated_index].argument == old);
        CHECK(remote_value(0, DEV(selector_a), command) == old);
        bool saw_live = false;
        for (unsigned i = 0; i < worker_gated_index; i++)
            saw_live |= attempts[i].live && attempts[i].source == 0 && attempts[i].argument == live;
        CHECK(saw_live);
    }
    ordering_finish(0);
    struct kp_rgb_replication_status done = ordering_status();
    CHECK(done.outstanding == 0 && done.generation > before.generation);
    if (noop || release) CHECK(done.generation == before.generation + 1);
    CHECK(done.repair_epoch == before.repair_epoch);
    CHECK(!ordering_complete(before.generation));
    CHECK(ordering_color() == (release ? old : live));
    if (!idle) { k_sem_give(&worker_resume); settle(); check_remote(0); check_remote(1); check_order(); }
    else { quiet(); CHECK(state_count() == count); }
    quiet();
}
static void ordering_overlap(void) {
    bool active = strstr(split_case, "active") != NULL;
    unsigned first = strstr(split_case, "reverse") ? 1 : 0;
    if (active) { arm_worker(0, RGB_BRT_SET_CMD, 0); connect(3); ordering_wait(&worker_accepted); }
    else { connect(3); settle(); quiet(); }
    unsigned before_sweeps[2] = {sweeps(0), sweeps(1)};
    struct kp_rgb_replication_status before = ordering_status();
    ordering_start(0, RGB_COLOR_HSB_VAL(111, 70, 90), true);
    ordering_start(1, RGB_COLOR_HSB_VAL(222, 80, 90), true);
    CHECK(ordering_status().outstanding == 2);
    CHECK(ordering_status().generation == before.generation);
    CHECK(ordering_status().repair_epoch == before.repair_epoch);
    ordering_finish(first);
    CHECK(ordering_status().outstanding == 1);
    CHECK(ordering_status().repair_epoch == before.repair_epoch);
    CHECK(!ordering_complete(ordering_status().generation));
    ordering_finish(1 - first);
    CHECK(ordering_status().outstanding == 0);
    CHECK(ordering_status().repair_epoch == before.repair_epoch + 1);
    if (active) k_sem_give(&worker_resume);
    settle(); check_remote(0); check_remote(1); check_order(); quiet();
    for (unsigned s = 0; s < 2; s++)
        CHECK(sweeps(s) == before_sweeps[s] + 1);
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
            direct = (struct zmk_behavior_binding){.behavior_dev = DEV(selector_a)->name,
                .param1 = KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, A_SLOT(probe_a)),
                .param2 = ordering_color()};
            CHECK(!(direct.param1 & KP_RGB_INVOCATION_MARKER));
            CHECK(ordering_callback(&direct, true) == 0);
            CHECK(ordering_status().outstanding == before.outstanding + 1);
            uint64_t pending = ordering_status().generation;
            CHECK(!ordering_complete(pending));
            CHECK(ordering_status().outstanding == before.outstanding + 1);
            if (mode == 2) binding.param2 = UINT32_MAX;
            if (mode == 3) binding.param1 = KP_RGB_INVOCATION_MARKER | 0x10a;
            int result = ordering_callback(&binding, mode != 1);
            CHECK(mode >= 2 ? result < 0 : result == 0);
            CHECK(!(binding.param1 & KP_RGB_INVOCATION_MARKER));
            CHECK(ordering_status().outstanding == before.outstanding);
            CHECK(ordering_status().generation > pending);
            CHECK(ordering_status().repair_epoch == before.repair_epoch + (mode >= 2 ? 1 : 0));
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
    struct zmk_behavior_binding encoded = {.behavior_dev = DEV(selector_a)->name,
        .param1 = KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, UINT16_MAX),
        .param2 = RGB_COLOR_HSB_VAL(1, 2, 3)};
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
    CHECK(after.repair_epoch == prior.repair_epoch); \
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
        ORDERING_PUBLICATION(CHECK(kp_test_set_color(members[i], &color) == 0), true);
        ORDERING_PUBLICATION(CHECK(kp_test_set_color(members[i], &color) == 0), false);
        ORDERING_PUBLICATION(CHECK(kp_test_set_period(members[i], 2345) == 0), true);
        ORDERING_PUBLICATION(CHECK(kp_test_set_period(members[i], 2345) == 0), false);
    }
    ORDERING_PUBLICATION(CHECK(kp_test_select(DEV(selector_a), DEV(probe_c)) == 0), true);
    ORDERING_PUBLICATION(CHECK(kp_test_select(DEV(selector_a), DEV(probe_c)) == 0), false);
    ORDERING_PUBLICATION(global_apply(RGB_RESET_CMD, 0), true);
    CHECK(kp_rgb_effect_data(DEV(probe_c))->color.h == 240);
    CHECK(kp_rgb_effect_data(DEV(probe_a))->color.b == 50);
    CHECK(kp_rgb_effect_data(DEV(probe_c))->color.b == 25);
    CHECK(kp_rgb_effect_data(DEV(probe_d))->color.b == 0);
    ORDERING_PUBLICATION(global_apply(RGB_CLOCK_CMD, 123456), false);
    ORDERING_PUBLICATION(CHECK(zmk_rgb_matrix_set_inhibited(false) == 0), false);
    struct kp_rgb_hsb private_color = {.h = 89, .s = 50, .b = 60};
    ORDERING_PUBLICATION(CHECK(kp_test_set_color(DEV(private_white), &private_color) == 0), false);
    ORDERING_PUBLICATION(CHECK(kp_test_set_period(DEV(private_white), 2345) == 0), false);
    kp_rgb_matrix_lock();
    for (size_t index = 0; index < KP_RGB_OVERLAY_WORDS; index++) {
        uint32_t word = kp_rgb_overlay_get_word(index);
        ORDERING_PUBLICATION(CHECK(kp_rgb_overlay_set_word(index, word ^ 1)), true);
        ORDERING_PUBLICATION(CHECK(!kp_rgb_overlay_set_word(index, word ^ 1)), false);
        ORDERING_PUBLICATION(CHECK(kp_rgb_overlay_set_word(index, word)), true);
    }
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

static void ordering_exception(void) {
    struct kp_rgb_replication_status before = ordering_status();
    struct zmk_behavior_binding a = {.behavior_dev = DEV(selector_a)->name,
        .param1 = RGB_EFS_CMD, .param2 = A_SLOT(probe_a)};
    struct zmk_behavior_binding b = {.behavior_dev = DEV(rgb_matrix)->name,
        .param1 = RGB_BRT_SET_CMD, .param2 = 37};
    CHECK(behavior_keymap_binding_convert_central_state_dependent_params(&a,
        (struct zmk_behavior_binding_event){0}) == 0);
    CHECK(behavior_keymap_binding_convert_central_state_dependent_params(&b,
        (struct zmk_behavior_binding_event){0}) == 0);
    CHECK(ordering_status().outstanding == 2);
    a.param2 = UINT32_MAX; b.param2 = UINT32_MAX;
    CHECK(ordering_callback(&a, true) < 0);
    CHECK(ordering_status().outstanding == 1);
    CHECK(ordering_status().repair_epoch == before.repair_epoch);
    CHECK(ordering_callback(&b, true) < 0);
    CHECK(ordering_status().outstanding == 0);
    CHECK(ordering_status().repair_epoch == before.repair_epoch + 1);
    kp_rgb_invocation_finish(&a, -EINVAL);
    CHECK(ordering_status().outstanding == 0);
    CHECK(ordering_status().repair_epoch == before.repair_epoch + 1);
}
static void ordering_requests(void) {
    bool settling = strstr(split_case, "settling") != NULL;
    bool disconnected = strstr(split_case, "disconnect") != NULL;
    bool completion = strstr(split_case, "completion") != NULL;
    bool exit_case = strstr(split_case, "exit") != NULL;
    if (settling) {
        connect(3); ordering_exception();
    } else if (disconnected) {
        connect(3); settle(); check_order();
        arm_worker(0, RGB_BRT_SET_CMD, 0); ordering_exception();
        ordering_wait(&worker_accepted);
        connect(0); k_sem_give(&worker_resume); ordering_barrier();
        k_sleep(K_MSEC(100));
        order_floor = attempt_count;
        ordering_exception(); quiet(); connect(3);
    } else {
        connect(3); settle(); quiet();
        arm_worker(completion ? 1 : 0, completion ? RGB_EFS_CMD : RGB_BRT_SET_CMD,
                   completion ? kp_rgb_selector_registry_count() - 1 : 0);
        ordering_exception();
        ordering_wait(&worker_accepted);
        uint64_t epoch = ordering_status().repair_epoch;
        if (exit_case) { k_sem_give(&worker_resume); ordering_barrier(); }
        CHECK(kp_test_set_color(DEV(probe_c), &(struct kp_rgb_hsb){.h=97, .s=63, .b=37}) == 0);
        CHECK(kp_test_set_period(DEV(probe_c), 3456) == 0);
        ordering_exception();
        CHECK(ordering_status().repair_epoch == epoch + 1);
        /* A new successful reservation must survive completion of the older pass. */
        ordering_start(0, RGB_COLOR_HSB_VAL(79, 83, 51), true);
        CHECK(ordering_status().outstanding == 1);
        if (!exit_case) k_sem_give(&worker_resume);
        ordering_barrier();
        CHECK(ordering_status().outstanding == 1);
        ordering_finish(0);
    }
    settle(); check_remote(0); check_remote(1); check_order(); quiet();
}
static void ordering_silence(void) {
    connect(3); settle(); quiet();
    unsigned before = state_count();
    uint64_t epoch = ordering_status().repair_epoch;
    ordering_publications();
    for (unsigned global = 0; global < 2; global++) {
        struct zmk_behavior_binding binding = {.behavior_dev = global ? DEV(rgb_matrix)->name : DEV(selector_a)->name,
            .param1 = 0x10a};
        CHECK(behavior_keymap_binding_convert_central_state_dependent_params(&binding,
            (struct zmk_behavior_binding_event){0}) < 0);
        CHECK(ordering_callback(&binding, true) < 0);
        CHECK(ordering_status().outstanding == 0);
        binding.param1 = global ? RGB_BRT_SET_CMD : RGB_EFS_CMD;
        binding.param2 = UINT32_MAX;
        CHECK(ordering_callback(&binding, true) < 0);
    }
    CHECK(ordering_status().repair_epoch == epoch);
    quiet(); CHECK(state_count() == before);
}
static void ordering_failure(void) {
    connect(3); settle(); quiet();
    unsigned before[2] = {sweeps(0), sweeps(1)};
    uint64_t epoch = ordering_status().repair_epoch;
    bool global = !strcmp(split_case, "ordering-global-failure");
    struct zmk_behavior_binding binding = {.behavior_dev = global ? DEV(rgb_matrix)->name : DEV(selector_a)->name,
        .param1 = global ? RGB_BRT_SET_CMD : RGB_EFS_CMD,
        .param2 = global ? 37 : A_SLOT(probe_a)};
    CHECK(behavior_keymap_binding_convert_central_state_dependent_params(&binding,
        (struct zmk_behavior_binding_event){0}) == 0);
    binding.param2 = UINT32_MAX;
    CHECK(ordering_callback(&binding, true) == -EINVAL);
    CHECK(ordering_status().outstanding == 0 && ordering_status().repair_epoch == epoch + 1);
    settle(); check_remote(0); check_remote(1); check_order(); quiet();
    CHECK(sweeps(0) == before[0] + 1 && sweeps(1) == before[1] + 1);
}
static void ordering_parameters(unsigned base) {
    kp_rgb_matrix_lock();
    for (size_t i = 0; i < kp_rgb_selector_registry_count(); i++) {
        const struct kp_rgb_selector *selector = kp_rgb_selector_registry_at(i);
        for (size_t slot = 0; slot < selector->effect_count; slot++) {
            const struct device *effect = kp_rgb_selector_at(selector, slot);
            if (!effect) continue;
            struct kp_rgb_hsb color = {.h = (base + i * 31 + slot * 7) % 360,
                .s = 43, .b = effect == DEV(probe_c) && base == 217 ? 0 : 37};
            CHECK(kp_test_set_color(effect, &color) == 0);
            CHECK(kp_test_set_period(effect, 1100 + base * 10 + i * 100 + slot) == 0);
        }
    }
    kp_rgb_matrix_unlock();
}
static void ordering_sweep(void) {
    unsigned phase = split_case[strlen(split_case) - 1] - '0';
    const uint32_t commands[] = {RGB_BRT_SET_CMD, RGB_OFF_CMD, KP_RGB_OVERLAY_WORD_COMMAND(0),
        KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, A_SLOT(probe_a)),
        KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD, A_SLOT(probe_a)),
        KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, A_SLOT(probe_c)),
        KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD, A_SLOT(probe_c)), RGB_EFS_CMD,
#if DT_NODE_EXISTS(DT_NODELABEL(probe_e))
        KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD, A_SLOT(probe_e)),
        KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD, A_SLOT(probe_e)),
#endif
    };
    CHECK(phase < ARRAY_SIZE(commands));
    ordering_parameters(17);
    worker_gate_fail = strstr(split_case, "failed") != NULL;
    arm_worker(0, commands[phase], 0); connect(3); ordering_wait(&worker_accepted);
    uint64_t generation = ordering_status().generation;
    if (strstr(split_case, "reset")) global_apply(RGB_RESET_CMD, 0);
    else {
        ordering_parameters(217);
        CHECK(kp_test_select(DEV(selector_a), DEV(probe_c)) == 0);
        global_apply(RGB_BRT_SET_CMD, 37);
    }
    CHECK(ordering_status().generation > generation);
    k_sem_give(&worker_resume);
    settle(); check_remote(0); check_remote(1); check_order();
    const struct attempt *gated = &attempts[worker_gated_index];
    if (!gated->accepted) {
        const struct device *endpoint = device_get_binding(gated->endpoint);
        CHECK(endpoint != NULL);
        bool retried = false;
        for (unsigned i = worker_gated_index + 1; i < attempt_count; i++) {
            const struct attempt *retry = &attempts[i];
            if (retry->source || retry->live || retry->command == RGB_CLOCK_CMD) continue;
            uint32_t expected_command = phase == 1 ?
                (kp_rgb_global.user_on ? RGB_ON_CMD : RGB_OFF_CMD) : gated->command;
            CHECK(retry->command == expected_command);
            CHECK(!strcmp(retry->endpoint, gated->endpoint));
            CHECK(retry->time >= gated->completion + 1000);
            CHECK(retry->argument == (phase == 1 ? 0 : remote_value(0, endpoint, expected_command)));
            retried = true;
            break;
        }
        CHECK(retried);
    }
    quiet();
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
        uint32_t word = kp_rgb_overlay_get_word(0);
        uint64_t before = kp_rgb_replication_status_locked().generation;
        CHECK(kp_test_select(DEV(selector_b), enabled ? DEV(probe_d) : DEV(probe_b)) == 0);
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
        uint32_t word = kp_rgb_overlay_get_word(0);
        CHECK(kp_test_select(DEV(preview_selector), enabled ? DEV(fx_solid) : DEV(fx_breathe)) == 0);
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
    CHECK(ordering_status().outstanding == 0 && state_count() == 0);
}
static void ordering_gates_repair(void) {
    ordering_gates();
    connect(3); settle(); check_remote(0); check_remote(1); quiet();
    unsigned before[2] = {sweeps(0), sweeps(1)};
    uint64_t epoch = ordering_status().repair_epoch;
    CHECK(kp_test_select(DEV(preview_selector), DEV(fx_breathe)) == 0);
    ordering_wait_gate(DEV(selector_a), false);
    CHECK(ordering_status().repair_epoch == epoch);
    quiet();
    CHECK(sweeps(0) == before[0] && sweeps(1) == before[1]);
    ordering_exception();
    settle(); check_remote(0); check_remote(1); check_order(); quiet();
    CHECK(sweeps(0) == before[0] + 1 && sweeps(1) == before[1] + 1);
}
#endif

#if IS_ENABLED(CONFIG_SETTINGS_RUNTIME)
#include <zephyr/settings/settings.h>
#include "rgb_persist_internal.h"

static void ordering_settings(void) {
    bool active = !strcmp(split_case, "ordering-settings-active");
    if (active) { arm_worker(0, RGB_BRT_SET_CMD, 0); connect(3); ordering_wait(&worker_accepted); }
    else { connect(3); settle(); quiet(); }
    unsigned packets_before = state_count();
    const struct device *effects[] = {DEV(probe_a), DEV(probe_c)};
    struct kp_rgb_hsb color = {.h = 217, .s = 43, .b = 61};
    for (size_t i = 0; i < ARRAY_SIZE(effects); i++) {
        CHECK(kp_test_set_color(effects[i], &color) == 0);
        CHECK(kp_test_set_period(effects[i], 2345) == 0);
    }
    if (!active) ordering_barrier();
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
        CHECK(kp_rgb_replication_status_locked().repair_epoch == before.repair_epoch);
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
        CHECK(committed.repair_epoch == before.repair_epoch);
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
    if (active) {
        k_sem_give(&worker_resume); settle(); check_remote(0); check_remote(1); check_order();
    } else {
        ordering_barrier();
        quiet(); CHECK(state_count() == packets_before);
    }
    CHECK(ordering_status().outstanding == 0);
}
#endif
static void ordering_run(void) {
    CHECK(ordering_status().outstanding == 0);
    if (!strcmp(split_case, "ordering-defaults")) ordering_defaults();
#if DT_NODE_EXISTS(DT_NODELABEL(ordering_local))
    else if (!strcmp(split_case, "ordering-gates-repair")) ordering_gates_repair();
#endif
#if IS_ENABLED(CONFIG_SETTINGS_RUNTIME)
    else if (!strncmp(split_case, "ordering-settings", 17)) ordering_settings();
#endif
    else if (!strcmp(split_case, "ordering-cleanup")) ordering_cleanup();
    else if (!strncmp(split_case, "ordering-overlap-", 17)) ordering_overlap();
    else if (!strncmp(split_case, "ordering-request-", 17)) ordering_requests();
    else if (!strncmp(split_case, "ordering-sweep-", 15)) ordering_sweep();
    else if (!strcmp(split_case, "ordering-failure") || !strcmp(split_case, "ordering-global-failure")) ordering_failure();
    else if (!strcmp(split_case, "ordering-silence")) ordering_silence();
    else ordering_fifo_case();
    CHECK(ordering_status().outstanding == 0);
    printk("PASS split %s\n", split_case);
    exit(0);
}
