#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Production single output worker under deterministic virtual time."""
from engine_harness import run_tests

CASES = (
    "periodic", "inhibited", "off", "coalesce", "starved", "reentrant",
    "black-retry", "feedback", "color-failure", "rollover", "input-during-render",
    "scheduler-seams", "idle-policy", "idempotent", "intent", "purge", "stale", "resume",
    "slow-render", "slow-driver", "black-slow", "boundary", "lock-retry",
    "boot-on-failure", "black-resume", "stale-early", "submit-failure", "finish-failure",
    "settled", "slow-early", "fairness", "ignored-release", "source-invalidate", "callback-invalidate",
)

TESTS = r'''
static void fixture(bool on, bool inhibit) {
    pthread_mutexattr_t attr;
    assert(pthread_mutexattr_init(&attr) == 0);
    assert(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) == 0);
    assert(pthread_mutex_init(&lock, &attr) == 0);
    assert(pthread_mutexattr_destroy(&attr) == 0);
    kp_output_work.work.handler = kp_rgb_output_handler;
    ctx.dev = &dummy;
    ctx.state.user_on = on;
    ctx.state.active_fx = &fx;
    assert(zmk_rgb_matrix_set_inhibited(inhibit) == 0);
    assert(kp_rgb_matrix_init() == 0);
    bool effective;
    assert(zmk_rgb_matrix_get_state(&effective) == 0 && effective == on);
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
static void input_during_render(void) {
    host_render_hook = NULL;
    const struct zmk_position_state_changed position = {
        .position = 0, .state = true, .timestamp = now,
    };
    const zmk_event_t event = {.position = &position};
    isr = true;
    assert(kp_rgb_matrix_event_listener(&event) == ZMK_EV_EVENT_BUBBLE);
    isr = false;
    assert(feedback == 0);
}
static void slow_render(void) { host_render_hook = NULL; now += 37; }
static void invalidate_during_render(void) { host_render_hook=NULL; kp_rgb_effect_invalidate(&fx); }
static void fail_finish(void) { host_render_hook = NULL; host_reschedule_failures = 1; }
static unsigned flush_budget = 4;
static int probe_renders;
static void request_again(void) {
    if (--flush_budget) zmk_rgb_matrix_flush();
    else host_render_hook = NULL;
}
static void capture_probe(struct k_work *work) {
    (void)work;
    probe_renders = renders;
}
static void flush_after_finish(void) {
    if (kp_rgb_pass_active) return;
    host_schedule_unlock_hook = NULL;
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
        assert(renders == 0 && writes == (KP_LED_COUNT ? 1 : 0));
        assert(kp_rgb_next_frame_ms == INT64_MAX && !colored);
        assert(zmk_rgb_matrix_set_inhibited(false) == 0);
        host_run_ready();
        assert(renders == (KP_LED_COUNT ? 1 : 0));
        if (KP_LED_COUNT) assert(rendered_elapsed == 0);
    } else if (!strcmp(name, "off")) {
        fixture(true, false); host_run_ready();
        assert(zmk_rgb_matrix_off() == 0); host_run_ready();
        unsigned runs = kp_output_work.work.runs;
        host_run_until(160);
        assert(kp_rgb_next_frame_ms == INT64_MAX && kp_output_work.work.runs == runs);
        assert(writes == (KP_LED_COUNT ? 3 : 0));
        if (KP_LED_COUNT) assert_pixels(host_transfer_count - 1, 0);
        assert(zmk_rgb_matrix_on() == 0); host_run_ready();
        host_activity(false); host_run_ready();
        assert(!kp_rgb_logical_on_locked() && ctx.state.user_on && kp_rgb_next_frame_ms == INT64_MAX);
        host_activity(true); host_run_ready();
        assert(kp_rgb_logical_on_locked());
        assert((kp_rgb_next_frame_ms != INT64_MAX) == (KP_LED_COUNT > 0));
    } else if (!strcmp(name, "coalesce")) {
        fixture(true, false); host_run_ready();
        unsigned runs = kp_output_work.work.runs;
        isr = true;
        for (int i = 0; i < 100; i++) zmk_rgb_matrix_flush();
        isr = false;
        host_run_ready();
        assert(kp_output_work.work.runs == runs + 1);
        host_run_until(8); zmk_rgb_matrix_flush(); host_run_ready();
        if (KP_LED_COUNT) assert(rendered_elapsed == 8 && kp_rgb_next_frame_ms == 16);
        host_run_until(16);
        if (KP_LED_COUNT) assert(rendered_elapsed == 8 && renders == 4);
    } else if (!strcmp(name, "starved")) {
        fixture(true, false); host_run_ready();
        unsigned runs = kp_output_work.work.runs;
        host_elapse_to(160);
        assert(kp_output_work.work.runs == runs);
        host_run_ready();
        assert(kp_output_work.work.runs == runs + (KP_LED_COUNT ? 1 : 0));
        if (KP_LED_COUNT) assert(rendered_elapsed == 160 && kp_rgb_next_frame_ms == 176);
        host_run_until(176);
        if (KP_LED_COUNT) assert(rendered_elapsed == 16);
    } else if (!strcmp(name, "reentrant")) {
        fixture(true, false); host_render_hook = flush_from_render; host_run_ready();
        assert(renders == (KP_LED_COUNT ? 2 : 0));
        if (KP_LED_COUNT) assert(rendered_elapsed == 0 && kp_rgb_next_frame_ms == 16);
    } else if (!strcmp(name, "black-retry")) {
        failures = KP_LED_COUNT ? 7 : 0;
        fixture(false, false);
        const uint64_t due[] = {100, 300, 700, 1500, 2500, 3500, 4500};
        for (size_t i = 0; i < sizeof(due)/sizeof(due[0]); i++) {
            host_run_until(due[i] - 1);
            assert(zmk_rgb_matrix_off() == 0);
            assert(zmk_rgb_matrix_set_inhibited(true) == 0);
            for (int n=0; n<20; n++) zmk_rgb_matrix_flush();
            host_run_ready();
            assert(writes == (KP_LED_COUNT ? (int)i + 1 : 0));
            if (KP_LED_COUNT) assert(kp_rgb_output_retry_deadline_ms == (int64_t)due[i]);
            host_run_until(due[i]);
            assert(writes == (KP_LED_COUNT ? (int)i + 2 : 0));
        }
        assert(kp_rgb_output_pending == KP_RGB_OUTPUT_NONE && !host_next_work() && !host_next_scheduled());
        assert(!renders && !colored);
        for (size_t i = 0; i < host_transfer_count; i++) {
            assert_pixels(i, 0);
            assert(host_transfers[i].at_ms == (i ? due[i-1] : 0));
            assert(host_transfers[i].result == (i == 7 ? 0 : -EIO));
        }
    } else if (!strcmp(name, "feedback")) {
        fixture(true, false); host_run_ready();
        struct kp_rgb_key_event ev = {.position=0,.pressed=true};
        if (!KP_LED_COUNT) { assert(!kp_rgb_pending_push(&ev)); return 0; }
        for (int i=0; i<KP_RGB_EVENT_QUEUE_LEN-1; i++) {
            assert(kp_rgb_pending_push(&ev)); zmk_rgb_matrix_flush();
        }
        assert(!kp_rgb_pending_push(&ev) && kp_rgb_pending_dropped == 1);
        host_lock_failures = 1; host_run_ready();
        assert(!feedback && kp_output_work.work.deadline == now+1);
        host_run_until(now+1);
        assert(feedback == KP_RGB_EVENT_QUEUE_LEN-1 && !kp_rgb_pending_dropped);
        assert(kp_rgb_pending_push(&ev)); zmk_rgb_matrix_flush();
        assert(zmk_rgb_matrix_set_inhibited(true) == 0); host_run_ready();
        assert(feedback == KP_RGB_EVENT_QUEUE_LEN-1);
    } else if (!strcmp(name, "color-failure")) {
        fixture(true, false); host_run_ready();
        failures = KP_LED_COUNT ? 1 : 0; host_allow_color_failure = true;
        host_run_until(7); zmk_rgb_matrix_flush(); host_run_ready();
        assert(kp_rgb_output_pending == (KP_LED_COUNT ? KP_RGB_OUTPUT_SCENE : KP_RGB_OUTPUT_NONE));
        if (KP_LED_COUNT) {
            assert(host_transfers[host_transfer_count-1].result == -EIO);
            assert_pixels(host_transfer_count-1,17);
            assert(kp_rgb_next_frame_ms == 16);
        }
        int before=writes; host_run_until(15); assert(writes==before);
        host_run_until(106); assert(writes==before);
        int painted=renders;
        host_run_until(107); assert(writes==before+(KP_LED_COUNT?1:0) && renders==painted);
        if (KP_LED_COUNT) assert(host_transfers[host_transfer_count-1].result == 0);
    } else if (!strcmp(name, "rollover")) {
        now = UINT32_MAX-7ULL; fixture(true,false); host_run_ready();
        host_run_until(now+16);
        if (KP_LED_COUNT) assert(rendered_elapsed==16 && renders==2);
    } else if (!strcmp(name, "input-during-render")) {
        fixture(true, false);
        host_render_hook = input_during_render;
        host_run_ready();
        assert(feedback == (KP_LED_COUNT ? 1 : 0));
        assert(renders == (KP_LED_COUNT ? 2 : 0));
        assert(!kp_rgb_pending_available() && !kp_rgb_scene_requested);
        if (KP_LED_COUNT) assert(kp_rgb_next_frame_ms == 16);
    } else if (!strcmp(name, "scheduler-seams")) {
        fixture(false,false); host_probe_work.work.handler=probe;
        assert(k_work_reschedule_for_queue(NULL,&host_probe_work,50)==1);
        assert(k_work_reschedule_for_queue(NULL,&host_probe_work,20)==1);
        host_run_until(19); assert(!host_probe_work.work.runs);
        host_run_until(20); assert(host_probe_work.work.runs==2);
        assert(k_work_submit_to_queue(NULL,&host_probe_work.work)==1);
        uint64_t order=host_probe_work.work.order;
        assert(k_work_reschedule_for_queue(NULL,&host_probe_work,0)==0);
        assert(host_probe_work.work.order==order);
        assert(k_work_reschedule_for_queue(NULL,&host_probe_work,50)==1);
        assert(host_probe_work.work.queued && host_probe_work.work.scheduled);
        host_run_ready(); assert(host_probe_work.work.runs==3);
        host_run_until(70); assert(host_probe_work.work.runs==4);
        assert(k_work_reschedule_for_queue(NULL,&host_probe_work,50)==1);
        assert(k_work_reschedule_for_queue(NULL,&host_probe_work,0)==1);
        assert(!host_probe_work.work.scheduled); host_run_ready();
        assert(host_probe_work.work.runs==5);
    } else if (!strcmp(name, "idle-policy")) {
        fixture(true,false); host_run_until(7); host_activity(false); host_run_ready();
        bool logical; assert(zmk_rgb_matrix_get_state(&logical)==0);
        assert(logical==!CONFIG_KEYPAW_RGB_MATRIX_AUTO_OFF_IDLE && ctx.state.user_on);
        assert((kp_rgb_next_frame_ms!=INT64_MAX)==(logical && KP_LED_COUNT>0));
        int after_change=renders;
        host_activity(false); host_run_ready(); assert(renders==after_change);
        host_run_until(16); assert(renders==(KP_LED_COUNT?(logical?3:after_change):0));
        host_activity(true); host_run_ready(); assert(renders==(KP_LED_COUNT?(logical?4:2):0));
        if (KP_LED_COUNT) assert(rendered_elapsed==0);
    } else if (!strcmp(name, "idempotent")) {
        fixture(true,false); host_run_until(7);
        int64_t due=kp_rgb_next_frame_ms,tick=runtime.last_render_ms;
        assert(zmk_rgb_matrix_on()==0); assert(zmk_rgb_matrix_set_inhibited(false)==0);
        host_activity(true); assert(kp_rgb_next_frame_ms==due && runtime.last_render_ms==tick);
        host_run_until(16); if(KP_LED_COUNT) assert(rendered_elapsed==16 && renders==2);
    } else if (!strcmp(name, "intent")) {
        fixture(true,false); host_run_ready(); host_activity(false);
        bool logical=true; assert(zmk_rgb_matrix_get_state(&logical)==0 && !logical);
        struct binding binding={.param1=RGB_TOG_CMD};
        assert(convert_toggle(&ctx,&binding)==0 && binding.param1==RGB_OFF_CMD);
        assert(zmk_rgb_matrix_toggle()==0 && !ctx.state.user_on); host_activity(true);
        assert(!atomic_get(&kp_rgb_output_allowed) && kp_rgb_next_frame_ms==INT64_MAX);
        host_activity(false); assert(zmk_rgb_matrix_toggle()==0 && ctx.state.user_on);
        assert(!atomic_get(&kp_rgb_output_allowed)); host_activity(true);
        assert(atomic_get(&kp_rgb_output_allowed)==(KP_LED_COUNT>0));
    } else if (!strcmp(name, "purge")) {
        fixture(true,false); host_run_ready();
        const struct kp_rgb_key_event ev={.position=0,.pressed=true};
        for(int idle_close=0;idle_close<2;idle_close++) {
            assert(kp_rgb_pending_push(&ev)==(KP_LED_COUNT>0));
            if(idle_close) host_activity(false); else assert(zmk_rgb_matrix_off()==0);
            assert(!kp_rgb_pending_push(&ev));
            if(idle_close) host_activity(true); else assert(zmk_rgb_matrix_on()==0);
            host_run_ready(); assert(!feedback);
        }
    } else if (!strcmp(name, "stale")) {
        fixture(true,false); host_run_ready(); zmk_rgb_matrix_flush();
        int before=colored; assert(zmk_rgb_matrix_set_inhibited(true)==0); host_run_ready();
        assert(colored==before && kp_rgb_next_frame_ms==INT64_MAX);
    } else if (!strcmp(name, "resume")) {
        fixture(true,false); host_run_until(32);
        for(int reason=0;reason<3;reason++) {
            if(reason==0) assert(zmk_rgb_matrix_set_inhibited(true)==0);
            else if(reason==1) assert(zmk_rgb_matrix_off()==0); else host_activity(false);
            uint64_t resumed_at=now+10000; host_run_until(resumed_at);
            if(reason==0) assert(zmk_rgb_matrix_set_inhibited(false)==0);
            else if(reason==1) assert(zmk_rgb_matrix_on()==0); else host_activity(true);
            host_run_ready(); if(KP_LED_COUNT) assert(rendered_elapsed==0 && runtime.last_render_ms==(int64_t)resumed_at);
            host_run_until(resumed_at+16); if(KP_LED_COUNT) assert(rendered_elapsed==16);
        }
    } else if (!strcmp(name,"settled")) {
        host_render_animating = false;
        fixture(true,false);
        host_run_ready();
        int settled_renders=renders,settled_writes=writes;
        host_run_until(160);
        assert(renders == settled_renders);
        assert(writes == settled_writes);
        assert(!runtime.animating && rendered_elapsed == 0);
    } else if (!strcmp(name,"slow-early")) {
        fixture(true,false);
        host_run_until(7);
        host_render_hook = slow_render;
        zmk_rgb_matrix_flush();
        host_run_ready();
        if (KP_LED_COUNT) assert(now == 44 && renders == 2 && kp_rgb_next_frame_ms == 48);
        host_run_until(47);
        assert(renders == (KP_LED_COUNT ? 2 : 0));
        host_run_until(48);
        if (KP_LED_COUNT) assert(renders == 3 && rendered_elapsed == 41);
    } else if (!strcmp(name,"ignored-release")) {
        if(!KP_LED_COUNT) return 0;
        host_render_animating=false;
        fixture(true,false); host_run_ready();
        int before_renders=renders,before_writes=writes;
        struct kp_rgb_key_event ev={.position=0,.pressed=false};
        assert(kp_rgb_pending_push(&ev)); kp_rgb_request_output_pass(false); host_run_ready();
        assert(renders==before_renders && writes==before_writes && feedback==1);
    } else if (!strcmp(name,"source-invalidate")) {
        host_render_animating=false;
        fixture(true,false); host_run_ready();
        int before_renders=renders;
        host_render_red=91; kp_rgb_effect_invalidate(&fx); host_run_ready();
        assert(renders==before_renders+(KP_LED_COUNT?1:0));
        if(KP_LED_COUNT) assert(scene[0].r==91);
        static const struct kp_rgb_effect_api embedded_api={.callbacks=&callbacks};
        static const struct device embedded_fx={.api=&embedded_api};
        before_renders=renders;
        host_render_red=117; kp_rgb_effect_invalidate(&embedded_fx); host_run_ready();
        assert(renders==before_renders+(KP_LED_COUNT?1:0));
        if(KP_LED_COUNT) assert(scene[0].r==117);
    } else if (!strcmp(name,"callback-invalidate")) {
        host_render_animating=false; fixture(true,false);
        host_render_hook=invalidate_during_render; host_run_ready();
        assert(renders==(KP_LED_COUNT?2:0));
    } else if (!strcmp(name,"fairness")) {
        fixture(true,false);
        host_render_hook = request_again;
        host_probe_work.work.handler = capture_probe;
        k_work_submit_to_queue(NULL,&host_probe_work.work);
        host_run_ready();
        assert(probe_renders == (KP_LED_COUNT ? 1 : 0));
        assert(renders == (KP_LED_COUNT ? 4 : 0));
        if (KP_LED_COUNT) assert(kp_rgb_next_frame_ms == 16);
    } else if (!strcmp(name,"slow-render") || !strcmp(name,"slow-driver")) {
        fixture(true,false);
        if(!strcmp(name,"slow-render")) host_render_hook=slow_render; else host_transfer_ms=37;
        host_run_ready(); host_transfer_ms=0;
        if(KP_LED_COUNT) { assert(now==37 && renders==1 && kp_rgb_next_frame_ms==48); }
        host_run_until(47); assert(renders==(KP_LED_COUNT?1:0));
        host_run_until(48); if(KP_LED_COUNT) assert(renders==2 && rendered_elapsed==48);
    } else if (!strcmp(name,"black-slow")) {
        failures=KP_LED_COUNT?2:0; host_transfer_ms=37; fixture(false,false);
        if(KP_LED_COUNT) assert(now==37 && kp_rgb_output_retry_deadline_ms==137);
        host_run_until(137);
        if(KP_LED_COUNT) assert(now==174 && writes==2 && kp_rgb_output_retry_deadline_ms==374);
        host_run_until(374); assert(kp_rgb_output_pending == KP_RGB_OUTPUT_NONE);
    } else if (!strcmp(name,"boundary")) {
        fixture(true,false); host_run_ready();
        zmk_rgb_matrix_flush();
        host_schedule_unlock_hook=flush_after_finish;
        host_run_ready();
        assert(!host_schedule_unlock_hook);
        if(KP_LED_COUNT) assert(renders==3 && kp_rgb_next_frame_ms==16);
        assert(!kp_rgb_scene_requested && !kp_rgb_pass_active);
    } else if (!strcmp(name,"lock-retry")) {
        fixture(true,false); host_lock_failures=2; host_run_ready();
        if(KP_LED_COUNT) {
            assert(!renders && kp_output_work.work.deadline==1);
            host_run_until(1); assert(!renders && kp_output_work.work.deadline==2);
            host_run_until(2); assert(renders==1 && rendered_elapsed==0 && kp_rgb_next_frame_ms==(int64_t)now+16);
        }
    } else if (!strcmp(name,"boot-on-failure")) {
        failures=KP_LED_COUNT?1:0; fixture(true,false); host_run_ready();
        assert(kp_rgb_output_pending == (KP_LED_COUNT ? KP_RGB_OUTPUT_SCENE : KP_RGB_OUTPUT_NONE));
        assert(!colored); host_run_until(99); assert(!colored);
        int painted=renders; host_run_until(100); assert(renders==painted);
        if(KP_LED_COUNT) assert(colored==1);
        for(size_t i=1;i<host_transfer_count;i++) assert_pixels(i,17);
    } else if (!strcmp(name,"black-resume")) {
        failures=KP_LED_COUNT?1:0; fixture(false,false); host_run_until(50);
        assert(zmk_rgb_matrix_on()==0); host_run_ready();
        assert(!colored); host_run_until(99); assert(!colored);
        host_run_until(100); if(KP_LED_COUNT) assert(colored==1);
        host_run_until(150); for(size_t i=1;i<host_transfer_count;i++) assert_pixels(i,17);
    } else if (!strcmp(name,"stale-early")) {
        fixture(true,false); host_run_ready(); host_run_until(7);
        int before=renders;
        k_work_submit_to_queue(NULL,&kp_output_work.work); host_run_ready();
        assert(renders==before); if(KP_LED_COUNT) assert(kp_rgb_next_frame_ms==16);
        host_run_until(16); assert(renders==before+(KP_LED_COUNT?1:0));
    } else if (!strcmp(name,"submit-failure")) {
        fixture(true,false); host_run_ready();
        int before=renders; host_reschedule_failures=1;
        zmk_rgb_matrix_flush(); host_run_ready();
        assert(renders==before && kp_rgb_scene_requested && kp_output_work.work.deadline==1);
        host_run_until(1); assert(!kp_rgb_scene_requested);
        assert(renders==before+(KP_LED_COUNT?1:0));
        if(KP_LED_COUNT) assert(kp_rgb_next_frame_ms==16);
    } else if (!strcmp(name,"finish-failure")) {
        fixture(true,false); host_render_hook=fail_finish; host_run_ready();
        if(KP_LED_COUNT) {
            assert(renders==1 && kp_rgb_pass_requested && kp_output_work.work.deadline==1);
            host_run_until(1); assert(renders==1 && !kp_rgb_pass_requested);
            assert(kp_rgb_next_frame_ms==16);
        }
    } else { assert(!"unknown case"); }
    assert(!polls && !refreshes && !dispatches);
    if(!KP_LED_COUNT) assert(kp_rgb_next_frame_ms==INT64_MAX && !feedback);
    printf("%s leds=%d time_ms=%llu passes=%u renders=%d transfers=%zu: passed\n",
           name,KP_LED_COUNT,(unsigned long long)now,kp_output_work.work.runs,renders,host_transfer_count);
    assert(pthread_mutex_destroy(&lock)==0);
}
'''

if __name__ == '__main__':
    run_tests(TESTS, scheduler=True, cases=CASES)
    run_tests(TESTS, scheduler=True, cases=("idle-policy",), auto_off_idle=False)
