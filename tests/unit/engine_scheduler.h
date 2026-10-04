/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef RGB_ENGINE_SCHEDULER_H
#define RGB_ENGINE_SCHEDULER_H

/* Single-worker virtual time. Callbacks consume no time unless a test advances it.
 * This models only the timer/work calls used by the extracted engine, not Zephyr's
 * priorities, tick rounding, cancellation races, or real ISR preemption.
 */
static struct k_work host_probe_work;
static struct k_work *const host_works[] = {
    &kp_tick_work, &kp_off_work, &host_probe_work,
};
static uint64_t host_order;
static unsigned host_timer_fires;

static int k_work_submit_to_queue(void *queue, struct k_work *work) {
    (void)queue;
    if (work == &kp_tick_work) {
        tick_submits++;
    }
    if (work->queued) {
        return 0;
    }
    assert(work->handler);
    work->queued = true;
    work->due = now;
    work->order = ++host_order;
    return work->running ? 2 : 1;
}

static int k_work_reschedule_for_queue(void *queue, struct k_work *work, int delay) {
    (void)queue;
    assert(delay >= 0 && work->handler);
    retry_delay = delay;
    work->scheduled = false;
    if (!delay) {
        return k_work_submit_to_queue(queue, work);
    }
    work->scheduled = true;
    work->deadline = now + (uint64_t)delay;
    work->schedule_order = ++host_order;
    return 1;
}

static void k_timer_stop(struct k_timer *timer) {
    timer->active = false;
    timer_period = 0;
}

static void k_timer_start(struct k_timer *timer, int delay, int period) {
    assert(delay >= 0 && period >= 0 && timer->handler);
    timer->active = true;
    timer->due = now + (uint64_t)delay;
    timer->period = period;
    timer_period = period;
}

static void host_fire_timer(void) {
    struct k_timer *timer = &kp_tick_timer;
    assert(timer->active && timer->due <= now);
    if (timer->period) {
        timer->due += timer->period;
    } else {
        timer->active = false;
    }
    bool was_isr = isr;
    isr = true;
    host_timer_fires++;
    timer->handler(timer);
    isr = was_isr;
}

static struct k_work *host_next_work(void) {
    struct k_work *next = NULL;
    for (size_t i = 0; i < sizeof(host_works) / sizeof(host_works[0]); i++) {
        struct k_work *work = host_works[i];
        if (work->queued && (!next || work->due < next->due ||
            (work->due == next->due && work->order < next->order))) {
            next = work;
        }
    }
    return next;
}

static struct k_work *host_next_scheduled(void) {
    struct k_work *next = NULL;
    for (size_t i = 0; i < sizeof(host_works) / sizeof(host_works[0]); i++) {
        struct k_work *work = host_works[i];
        if (work->scheduled && (!next || work->deadline < next->deadline ||
            (work->deadline == next->deadline && work->schedule_order < next->schedule_order))) {
            next = work;
        }
    }
    return next;
}

static uint64_t host_next_deadline(void) {
    struct k_work *work = host_next_scheduled();
    uint64_t next = work ? work->deadline : UINT64_MAX;
    if (kp_tick_timer.active && kp_tick_timer.due < next) {
        next = kp_tick_timer.due;
    }
    return next;
}

static void host_fire_due(void) {
    unsigned budget = 100000;
    while (host_next_deadline() <= now) {
        assert(budget-- > 0);
        struct k_work *work = host_next_scheduled();
        if (kp_tick_timer.active && kp_tick_timer.due <= now &&
            (!work || kp_tick_timer.due <= work->deadline)) {
            host_fire_timer();
        } else {
            assert(work);
            work->scheduled = false;
            bool was_isr = isr;
            isr = true;
            k_work_submit_to_queue(NULL, work);
            isr = was_isr;
        }
    }
}

/* Advance timer interrupts but hold the worker, simulating queue starvation. */
static void host_elapse_to(uint64_t target) {
    assert(target >= now);
    unsigned budget = 100000;
    while (host_next_deadline() <= target) {
        assert(budget-- > 0);
        uint64_t deadline = host_next_deadline();
        if (deadline > now) now = deadline;
        host_fire_due();
    }
    now = target;
}

/* A running work item may submit itself once for a later pass. */
static void host_run_ready(void) {
    unsigned budget = 10000;
    for (;;) {
        host_fire_due();
        struct k_work *work = host_next_work();
        if (!work || work->due > now) {
            return;
        }
        assert(budget-- > 0);
        work->queued = false;
        work->running = true;
        work->runs++;
        work->handler(work);
        work->running = false;
    }
}

/* Run timer and worker in timestamp order, including work due exactly at target. */
static void host_run_until(uint64_t target) {
    assert(target >= now);
    while (now < target) {
        host_run_ready();
        if (now >= target) break;
        uint64_t next = host_next_deadline();
        if (next > target) next = target;
        assert(next > now);
        now = next;
    }
    host_run_ready();
}

#endif
