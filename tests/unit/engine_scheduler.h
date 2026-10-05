/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef RGB_ENGINE_SCHEDULER_H
#define RGB_ENGINE_SCHEDULER_H

/* Callbacks consume no virtual time unless a test advances it. This models
 * delayable work, not Zephyr priorities, tick rounding or real ISR preemption.
 */
static struct k_work_delayable host_probe_work;
static struct k_work_delayable host_refresh_work;
static uint64_t host_refresh_token;
static bool host_manual_refresh,host_refresh_queued;
static struct k_work *const host_works[] = {
    &kp_output_work.work, &host_probe_work.work, &host_refresh_work.work,
};
static uint64_t host_order;
static unsigned host_reschedule_failures;
static void kp_rgb_conditions_refreshed(uint64_t token);
static int k_work_submit_to_queue(void *queue, struct k_work *work);
static void host_refresh_handler(struct k_work *work) {
    (void)work;
    host_refresh_queued=false;
    kp_rgb_conditions_refreshed(host_refresh_token);
}
static void kp_rgb_conditions_request_refresh(uint64_t token) {
    host_refresh_token=token;
    if(host_manual_refresh) return;
    host_refresh_work.work.handler=host_refresh_handler;
    host_refresh_queued=true;
    k_work_submit_to_queue(NULL,&host_refresh_work.work);
}

static int k_work_submit_to_queue(void *queue, struct k_work *work) {
    (void)queue;
    if (work == &kp_output_work.work) {
        output_submits++;
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

static int k_work_reschedule_for_queue(void *queue, struct k_work_delayable *dwork, int64_t delay) {
    struct k_work *work = &dwork->work;
    (void)queue;
    if (host_reschedule_failures) { host_reschedule_failures--; return -EBUSY; }
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
    return next;
}

static void host_fire_due(void) {
    unsigned budget = 100000;
    while (host_next_deadline() <= now) {
        assert(budget-- > 0);
        struct k_work *work = host_next_scheduled();
        assert(work);
        work->scheduled = false;
        bool was_isr = isr;
        isr = true;
        k_work_submit_to_queue(NULL, work);
        isr = was_isr;
    }
}

/* Expire delayed work without running the queue. */
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

/* Include work due exactly at target. */
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
