#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Characterize the current engine under virtual time, not the proposed scheduler."""
from engine_harness import run_tests

CASES = (
    "periodic", "inhibited", "off", "coalesce", "starved", "reentrant",
    "black-retry", "feedback", "color-failure", "rollover", "render-inhibit",
    "scheduler-seams",
)

TESTS = r'''
static void fixture(bool on, bool inhibit) {
    pthread_mutexattr_t attr;
    assert(pthread_mutexattr_init(&attr) == 0);
    assert(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) == 0);
    assert(pthread_mutex_init(&lock, &attr) == 0);
    assert(pthread_mutexattr_destroy(&attr) == 0);
    kp_tick_work.handler = kp_rgb_matrix_tick;
    kp_off_work.handler = kp_rgb_matrix_off_handler;
    kp_pending_work.handler = kp_rgb_matrix_pending_handler;
    kp_tick_timer.handler = kp_rgb_matrix_tick_handler;
    ctx.dev = &dummy;
    ctx.state.on = ctx.state.user_on = on;
    ctx.state.active_fx = &fx;
    ctx.zone_valid = ctx.all_leds = true;
    assert(zmk_rgb_matrix_set_inhibited(inhibit) == 0);
    assert(kp_rgb_matrix_init() == 0);
    bool effective;
    assert(zmk_rgb_matrix_get_state(&dummy, &effective) == 0 && effective == on);
    struct binding binding = {.param1 = RGB_TOG_CMD};
    assert(convert_toggle(&ctx, &binding) == 0);
    assert(binding.param1 == (on ? RGB_OFF_CMD : RGB_ON_CMD));
}

static void assert_pixels(size_t index, uint8_t red) {
    assert(index < host_transfer_count);
    for (size_t i = 0; i < KP_LED_COUNT; i++) {
        const struct led_rgb *p = &host_transfers[index].pixels[i];
        assert(p->r == red && p->g == 0 && p->b == 0);
    }
}

static void flush_from_render(void) {
    host_render_hook = NULL;
    zmk_rgb_matrix_flush();
}

static void probe(struct k_work *work) {
    if (work->runs == 1) {
        assert(k_work_submit_to_queue(NULL, work) == 2);
        assert(k_work_submit_to_queue(NULL, work) == 0);
    }
}

int main(int argc, char **argv) {
    assert(argc == 2);
    const char *name = argv[1];
    if (!strcmp(name, "periodic")) {
        fixture(true, false);
        host_run_until(160);
        assert(host_timer_fires == 11 && kp_tick_work.runs == 11);
        assert(polls == 0 && refreshes == 0 && dispatches == 0);
        assert(renders == (KP_LED_COUNT ? 11 : 0));
        assert(writes == (KP_LED_COUNT ? 12 : 0));
        for (size_t i = 0; i < host_transfer_count; i++) {
            assert_pixels(i, i ? 17 : 0);
            assert(host_transfers[i].at_ms == (i ? (i - 1) * 16 : 0));
            assert(host_transfers[i].result == 0);
        }
    } else if (!strcmp(name, "inhibited")) {
        fixture(true, true);
        host_run_until(160);
        assert(host_timer_fires == 11 && polls == 0 && refreshes == 0 && dispatches == 0);
        assert(renders == 0 && writes == (KP_LED_COUNT ? 1 : 0));
        assert(!colored);
        assert(zmk_rgb_matrix_set_inhibited(false) == 0);
        host_run_ready();
        assert(renders == (KP_LED_COUNT ? 1 : 0));
        if (KP_LED_COUNT) assert(rendered_elapsed == 0);
    } else if (!strcmp(name, "off")) {
        fixture(true, false);
        host_run_ready();
        assert(zmk_rgb_matrix_off(&dummy) == 0);
        host_run_ready();
        int before = polls;
        host_run_until(160);
        assert(!kp_tick_timer.active && polls == before);
        assert(writes == (KP_LED_COUNT ? 3 : 0));
        if (KP_LED_COUNT) assert_pixels(host_transfer_count - 1, 0);
        assert(zmk_rgb_matrix_on(&dummy) == 0);
        host_run_ready();
        idle = true;
        kp_rgb_permission_handler(NULL);
        host_run_ready();
        assert(!ctx.state.on && ctx.state.user_on && !kp_tick_timer.active);
        idle = false;
        kp_rgb_permission_handler(NULL);
        host_run_ready();
        assert(ctx.state.on && kp_tick_timer.active);
    } else if (!strcmp(name, "coalesce")) {
        fixture(true, false);
        host_run_ready();
        for (int i = 0; i < 100; i++) zmk_rgb_matrix_flush();
        assert(kp_tick_work.runs == 1 && tick_submits == 101);
        host_run_ready();
        assert(kp_tick_work.runs == 2);
        host_run_until(8);
        zmk_rgb_matrix_flush();
        host_run_ready();
        if (KP_LED_COUNT) assert(rendered_elapsed == 8);
        assert(kp_tick_timer.due == 16);
        host_run_until(16);
        if (KP_LED_COUNT) assert(rendered_elapsed == 8);
        assert(kp_tick_work.runs == 4);
    } else if (!strcmp(name, "starved")) {
        fixture(true, false);
        host_run_ready();
        host_elapse_to(160);
        assert(host_timer_fires == 11 && tick_submits == 11);
        assert(kp_tick_work.runs == 1);
        host_run_ready();
        assert(kp_tick_work.runs == 2);
        if (KP_LED_COUNT) assert(rendered_elapsed == 160);
        host_run_until(176);
        if (KP_LED_COUNT) assert(rendered_elapsed == 16);
    } else if (!strcmp(name, "reentrant")) {
        fixture(true, false);
        host_render_hook = flush_from_render;
        host_run_ready();
        assert(kp_tick_work.runs == (KP_LED_COUNT ? 2 : 1));
        if (KP_LED_COUNT) assert(rendered_elapsed == 0);
    } else if (!strcmp(name, "black-retry")) {
        failures = KP_LED_COUNT ? 5 : 0;
        fixture(false, false);
        const uint64_t due[] = {100, 300, 700, 1500, 2500};
        for (size_t i = 0; i < sizeof(due)/sizeof(due[0]); i++) {
            host_run_until(due[i] - 1);
            assert(writes == (KP_LED_COUNT ? (int)i + 1 : 0));
            if (KP_LED_COUNT) assert(kp_off_work.deadline == due[i]);
            host_run_until(due[i]);
            assert(writes == (KP_LED_COUNT ? (int)i + 2 : 0));
        }
        assert(!kp_rgb_black_pending && !host_next_work() && !host_next_scheduled());
        assert(!polls && !renders && !colored);
        for (size_t i = 0; i < host_transfer_count; i++) {
            assert_pixels(i, 0);
            assert(host_transfers[i].at_ms == (i ? due[i - 1] : 0));
            assert(host_transfers[i].result == (i == 5 ? 0 : -EIO));
        }
    } else if (!strcmp(name, "feedback")) {
        fixture(true, false);
        host_run_ready();
        int rendered_before = renders;
        struct zmk_position_state_changed ev = {.position = 0, .state = true};
        for (int i = 0; i < KP_RGB_EVENT_QUEUE_LEN - 1; i++) {
            assert(kp_rgb_pending_push(&ev));
            k_work_submit_to_queue(NULL, &kp_pending_work);
        }
        assert(!kp_rgb_pending_push(&ev) && kp_rgb_pending_dropped == 1);
        host_lock_failures = 1;
        host_run_ready();
        assert(kp_pending_work.runs == 2 && feedback == KP_RGB_EVENT_QUEUE_LEN - 1);
        assert(!kp_rgb_pending_dropped && renders == rendered_before);
        assert(!kp_tick_work.queued);
        assert(kp_rgb_pending_push(&ev));
        k_work_submit_to_queue(NULL, &kp_pending_work);
        assert(zmk_rgb_matrix_set_inhibited(true) == 0);
        host_run_ready();
        assert(feedback == KP_RGB_EVENT_QUEUE_LEN - 1);
    } else if (!strcmp(name, "color-failure")) {
        fixture(true, false);
        host_run_ready();
        failures = KP_LED_COUNT ? 1 : 0;
        host_allow_color_failure = true;
        k_timer_stop(&kp_tick_timer);
        zmk_rgb_matrix_flush();
        host_run_ready();
        assert(!kp_rgb_black_pending && !host_next_work() && !host_next_scheduled());
        if (KP_LED_COUNT) {
            assert(host_transfers[host_transfer_count - 1].result == -EIO);
            assert_pixels(host_transfer_count - 1, 17);
        }
        int before = writes;
        host_run_until(1000);
        assert(writes == before);
    } else if (!strcmp(name, "rollover")) {
        now = UINT32_MAX - 7ULL;
        fixture(true, false);
        host_run_ready();
        host_run_until(now + 16);
        if (KP_LED_COUNT) assert(rendered_elapsed == 16);
        assert(kp_tick_work.runs == 2);
    } else if (!strcmp(name, "render-inhibit")) {
        fixture(true, false);
        inhibit_from_render = true;
        host_run_ready();
        assert(colored == 0);
        if (KP_LED_COUNT) {
            assert(zmk_rgb_matrix_is_inhibited() && writes == 2);
            assert_pixels(1, 0);
        }
    } else if (!strcmp(name, "scheduler-seams")) {
        fixture(false, false);
        host_probe_work.handler = probe;
        assert(k_work_reschedule_for_queue(NULL, &host_probe_work, 50) == 1);
        assert(k_work_reschedule_for_queue(NULL, &host_probe_work, 20) == 1);
        host_run_until(19);
        assert(!host_probe_work.runs);
        host_run_until(20);
        assert(host_probe_work.runs == 2 && !host_next_work());
        assert(k_work_submit_to_queue(NULL, &host_probe_work) == 1);
        uint64_t order = host_probe_work.order;
        assert(k_work_reschedule_for_queue(NULL, &host_probe_work, 0) == 0);
        assert(host_probe_work.order == order);
        assert(k_work_reschedule_for_queue(NULL, &host_probe_work, 50) == 1);
        assert(host_probe_work.queued && host_probe_work.scheduled);
        assert(host_probe_work.order == order && host_probe_work.due == 20);
        host_run_ready();
        assert(host_probe_work.runs == 3 && host_probe_work.scheduled);
        host_run_until(69);
        assert(host_probe_work.runs == 3);
        host_run_until(70);
        assert(host_probe_work.runs == 4);
        assert(k_work_reschedule_for_queue(NULL, &host_probe_work, 50) == 1);
        assert(k_work_reschedule_for_queue(NULL, &host_probe_work, 0) == 1);
        assert(!host_probe_work.scheduled);
        host_run_ready();
        assert(host_probe_work.runs == 5);
        assert(k_work_reschedule_for_queue(NULL, &host_probe_work, 20) == 1);
        host_elapse_to(200);
        assert(host_probe_work.queued && !host_probe_work.scheduled);
        assert(host_probe_work.due == 90 && host_probe_work.runs == 5);
        assert(k_work_reschedule_for_queue(NULL, &host_probe_work, 50) == 1);
        host_run_ready();
        assert(host_probe_work.runs == 6);
        host_run_until(250);
        assert(host_probe_work.runs == 7 && !host_next_scheduled());
        host_mutate_output = false;
        assert(zmk_rgb_matrix_on(&dummy) == 0);
        host_run_ready();
        if (KP_LED_COUNT) assert(pixels[0].r == 17);
        host_lock_failures = 1;
        zmk_rgb_matrix_flush();
        host_run_ready();
        assert(kp_tick_work.runs == (KP_LED_COUNT ? 3 : 2));
    } else {
        assert(!"unknown case");
    }
    assert(!polls && !refreshes && !dispatches);
    printf("%s leds=%d time_ms=%llu timer_fires=%u tick_runs=%u polls=%d "
           "renders=%d transfers=%zu: passed\n", name, KP_LED_COUNT,
           (unsigned long long)now, host_timer_fires, kp_tick_work.runs,
           polls, renders, host_transfer_count);
    assert(pthread_mutex_destroy(&lock) == 0);
}
'''

if __name__ == "__main__":
    run_tests(TESTS, scheduler=True, cases=CASES)
