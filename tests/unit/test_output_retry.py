#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Production output debt, shared retry history and immutable scene tests."""
from engine_harness import BEHAVIOR_SOURCE, run_tests
from test_engine_scheduler import TESTS as SCHEDULER_TESTS
from test_singleton import function

CASES = (
    "color-backoff", "latest-scene", "urgent-black", "coalesced-gates",
    "slow-failure", "boot-off", "boot-on", "boot-inhibited", "unready",
    "retry-lock", "requests", "healthy-mutation", "mutation-before-lock",
    "urgent-black-success", "selection-backoff",
)

TESTS = (function(BEHAVIOR_SOURCE, "kp_rgb_select_effect")
         + SCHEDULER_TESTS.split("static void flush_from_render(", 1)[0]) + r'''
static struct kp_rgb_effect_runtime selected_runtime;
static unsigned selected_paints;
static bool selected_render(const struct device *dev, const struct kp_rgb_frame *frame) {
    (void)dev;
    assert(held && frame->targets && !frame->scratch);
    if (!selected_paints) assert(frame->elapsed_ms==0);
    selected_paints++;renders++;
    for(size_t n=0;n<frame->target_count;n++) frame->pixels[frame->targets[n]].r=127;
    return true;
}
static const struct kp_rgb_effect_callbacks selected_callbacks={.render=selected_render};
static const struct kp_rgb_effect_api selected_api={.callbacks=&selected_callbacks,.runtime=&selected_runtime};
static const struct device selected_fx={.api=&selected_api};
static void assert_debt(enum kp_rgb_output_kind kind, int64_t deadline, uint32_t delay) {
    assert(kp_rgb_output_pending == kind);
    assert(kp_rgb_output_retry_deadline_ms == deadline);
    assert(kp_rgb_output_retry_ms == delay);
    assert(kp_rgb_output_deadline_locked() ==
           (kind == KP_RGB_OUTPUT_NONE ? INT64_MAX : deadline));
}
static void mutate_before_lock(void) {
    if (!kp_rgb_pass_active) return;
    host_schedule_unlock_hook=NULL;
    assert(!held && !kp_rgb_scene_requested);
    kp_rgb_matrix_lock();
    host_render_red=99;
    zmk_rgb_matrix_flush();
    kp_rgb_matrix_unlock();
}
static void assert_scene(uint8_t red) {
    for(size_t i=0;i<KP_LED_COUNT;i++) {
        assert(scene[i].r==red && !scene[i].g && !scene[i].b);
    }
}
int main(int argc, char **argv) {
    assert(argc==2);
    const char *name=argv[1];
    if (!strcmp(name,"unready")) host_strip_ready=false;
    if (!strncmp(name,"boot-",5)) failures=KP_LED_COUNT?1:0;
    if (!strcmp(name,"unready") && KP_LED_COUNT > 0) {
        pthread_mutex_init(&lock,NULL);
        kp_output_work.work.handler=kp_rgb_output_handler;
        ctx.dev=&dummy;ctx.state.user_on=true;ctx.state.active_fx=&fx;
        assert(kp_rgb_matrix_init()==-ENODEV);
        zmk_rgb_matrix_flush();host_run_until(1000);
        assert(!writes && !renders && !kp_rgb_output_ready);
        assert_debt(KP_RGB_OUTPUT_NONE,0,100);
        return 0;
    }
    fixture(strcmp(name,"boot-off")!=0, !strcmp(name,"boot-inhibited"));
    host_elapse_to(now);host_run_ready();
    if (!KP_LED_COUNT) {
        zmk_rgb_matrix_off();host_run_ready();zmk_rgb_matrix_on();host_run_ready();
        zmk_rgb_matrix_set_inhibited(true);host_run_until(10000);
        assert(!writes && !renders && !kp_rgb_output_urgent_black);
        assert_debt(KP_RGB_OUTPUT_NONE,0,100);
        return 0;
    }
    if (!strncmp(name,"boot-",5)) {
        bool on=!strcmp(name,"boot-on");
        assert(writes==1 && !colored);
        assert_debt(on?KP_RGB_OUTPUT_SCENE:KP_RGB_OUTPUT_BLACK,100,200);
        host_run_until(99);assert(writes==1);
        int painted=renders;host_run_until(100);
        assert(writes==2 && renders==painted);
        assert_pixels(0,0);assert_pixels(1,on?17:0);
        assert_debt(KP_RGB_OUTPUT_NONE,0,100);
        if (!on) { host_run_until(1000);assert(writes==2 && !renders); }
    } else if (!strcmp(name,"color-backoff")) {
        host_render_animating=false;host_allow_color_failure=true;failures=7;
        host_run_until(7);zmk_rgb_matrix_flush();host_run_ready();
        int terminal_paints=renders;
        const uint64_t due[]={107,307,707,1507,2507,3507,4507};
        const uint32_t delays[]={200,400,800,1000,1000,1000,1000};
        for(size_t i=0;i<7;i++) {
            assert_debt(KP_RGB_OUTPUT_SCENE,due[i],delays[i]);
            host_run_until(due[i]-1);assert(writes==(int)i+3);
            int painted=renders;
            host_run_until(due[i]);assert(writes==(int)i+4 && renders==painted);
            assert_pixels(host_transfer_count-1,17);assert_scene(17);
        }
        assert(renders==terminal_paints && !runtime.animating);
        assert_debt(KP_RGB_OUTPUT_NONE,0,100);
    } else if (!strcmp(name,"latest-scene")) {
        host_allow_color_failure=true;failures=1;ctx.tuning.max_brightness=50;
        host_render_red=80;host_run_until(7);zmk_rgb_matrix_flush();host_run_ready();
        assert_pixels(2,40);assert_scene(40);
        host_render_red=120;host_run_until(96);
        assert(writes==3);assert_scene(60);
        int painted=renders;ctx.tuning.max_brightness=25;
        host_run_until(107);
        assert(renders==painted && writes==4);assert_pixels(3,60);assert_scene(60);
        host_run_until(112);assert_pixels(4,30);assert_scene(30);
    } else if (!strcmp(name,"urgent-black")) {
        host_allow_color_failure=true;failures=4;
        host_run_until(7);zmk_rgb_matrix_flush();host_run_ready();
        assert_debt(KP_RGB_OUTPUT_SCENE,107,200);
        host_run_until(8);assert(zmk_rgb_matrix_set_inhibited(true)==0);
        assert(kp_rgb_output_urgent_black);host_run_ready();
        assert_pixels(3,0);assert(writes==4 && !kp_rgb_output_urgent_black);
        assert_debt(KP_RGB_OUTPUT_BLACK,208,400);
        for(int i=0;i<20;i++) {
            zmk_rgb_matrix_set_inhibited(true);zmk_rgb_matrix_off();
            host_activity(false);zmk_rgb_matrix_flush();host_run_ready();
        }
        assert(writes==4);assert_debt(KP_RGB_OUTPUT_BLACK,208,400);
        host_run_until(9);host_activity(true);zmk_rgb_matrix_on();
        zmk_rgb_matrix_set_inhibited(false);
        assert_debt(KP_RGB_OUTPUT_NONE,208,400);
        host_render_red=91;host_run_ready();
        assert(rendered_elapsed==0 && writes==4 && !kp_rgb_output_urgent_black);
        host_run_until(208);assert_pixels(4,91);
        assert_debt(KP_RGB_OUTPUT_SCENE,608,800);
        host_run_until(209);zmk_rgb_matrix_off();host_run_ready();
        assert_pixels(5,0);assert_debt(KP_RGB_OUTPUT_BLACK,1009,1000);
        host_run_until(210);zmk_rgb_matrix_on();host_run_ready();
        assert(writes==6 && rendered_elapsed==0);
        host_run_until(1008);assert(writes==6);
        int painted=renders;host_run_until(1009);assert(renders==painted);
        assert_pixels(6,91);assert_debt(KP_RGB_OUTPUT_NONE,0,100);
    } else if (!strcmp(name,"urgent-black-success")) {
        host_allow_color_failure=true;failures=1;
        host_run_until(7);zmk_rgb_matrix_flush();host_run_ready();
        assert_debt(KP_RGB_OUTPUT_SCENE,107,200);
        host_run_until(8);zmk_rgb_matrix_off();
        assert(kp_rgb_output_urgent_black && kp_rgb_output_deadline_locked()==0);
        host_run_ready();assert(writes==4 && !kp_rgb_output_urgent_black);
        assert_pixels(3,0);assert(host_transfers[3].result==0);
        assert_debt(KP_RGB_OUTPUT_NONE,0,100);
        host_run_until(9);host_render_red=93;zmk_rgb_matrix_on();host_run_ready();
        assert(writes==5 && rendered_elapsed==0);
        assert(host_transfers[4].at_ms==9);assert_pixels(4,93);assert_scene(93);
        assert_debt(KP_RGB_OUTPUT_NONE,0,100);
    } else if (!strcmp(name,"selection-backoff")) {
        host_effects[1]=&selected_fx;host_effect_count=2;
        host_allow_color_failure=true;failures=1;
        host_run_until(7);zmk_rgb_matrix_flush();host_run_ready();
        assert_pixels(2,17);assert_debt(KP_RGB_OUTPUT_SCENE,107,200);
        host_run_until(8);assert(kp_rgb_select_effect(1)==0);
        assert(ctx.state.active_fx==&selected_fx && ctx.effect_index==1);
        assert_debt(KP_RGB_OUTPUT_SCENE,107,200);
        host_run_ready();assert(writes==3 && selected_paints==1);
        assert(!runtime.active && selected_runtime.active);assert_scene(127);
        assert_debt(KP_RGB_OUTPUT_SCENE,107,200);
        host_run_until(106);assert(writes==3);
        unsigned selected_before=selected_paints;int painted=renders;
        host_run_until(107);assert(writes==4 && renders==painted);
        assert(selected_paints==selected_before && !runtime.active && selected_runtime.active);
        assert_pixels(3,127);assert_scene(127);
        assert_debt(KP_RGB_OUTPUT_NONE,0,100);
    } else if (!strcmp(name,"coalesced-gates")) {
        host_allow_color_failure=true;failures=1;
        host_run_until(7);zmk_rgb_matrix_flush();host_run_ready();
        host_run_until(8);zmk_rgb_matrix_off();zmk_rgb_matrix_on();
        zmk_rgb_matrix_set_inhibited(true);zmk_rgb_matrix_set_inhibited(false);
        host_activity(false);host_activity(true);
        assert(!kp_rgb_output_urgent_black);host_run_ready();
        assert(writes==3 && rendered_elapsed==0);
        assert_debt(KP_RGB_OUTPUT_SCENE,107,200);
        host_run_until(107);assert_pixels(3,17);
        host_run_until(108);zmk_rgb_matrix_off();zmk_rgb_matrix_on();zmk_rgb_matrix_off();
        host_run_ready();assert_pixels(4,0);assert(!kp_rgb_output_urgent_black);
    } else if (!strcmp(name,"slow-failure")) {
        host_allow_color_failure=true;failures=2;host_transfer_ms=37;
        host_run_until(7);zmk_rgb_matrix_flush();host_run_ready();
        assert(now==44 && kp_rgb_next_frame_ms==48);
        assert_debt(KP_RGB_OUTPUT_SCENE,144,200);
        host_run_until(143);assert(writes==3);
        host_run_until(144);assert(now==181 && writes==4);
        assert_debt(KP_RGB_OUTPUT_SCENE,381,400);
        host_transfer_ms=0;host_run_until(380);int painted=renders;
        host_run_until(381);assert(renders==painted && writes==5);
        assert_debt(KP_RGB_OUTPUT_NONE,0,100);
    } else if (!strcmp(name,"retry-lock")) {
        host_allow_color_failure=true;failures=1;
        host_run_until(7);zmk_rgb_matrix_flush();host_run_ready();
        host_run_until(106);int painted=renders;
        host_lock_failures=2;host_run_until(107);assert(writes==3);
        assert(kp_output_work.work.deadline==108);
        host_run_until(108);assert(writes==3 && kp_output_work.work.deadline==109);
        host_run_until(109);assert(writes==4 && renders==painted);
        assert(!kp_rgb_scene_requested && kp_rgb_next_frame_ms==112);
        assert_debt(KP_RGB_OUTPUT_NONE,0,100);
    } else if (!strcmp(name,"mutation-before-lock")) {
        host_allow_color_failure=true;failures=1;
        host_run_until(7);zmk_rgb_matrix_flush();host_run_ready();
        host_run_until(106);assert_scene(17);
        assert(!kp_rgb_scene_requested && kp_rgb_next_frame_ms==112);
        int painted=renders;
        unsigned passes=kp_output_work.work.runs;
        host_schedule_unlock_hook=mutate_before_lock;
        host_run_until(107);
        assert(!host_schedule_unlock_hook && !kp_rgb_scene_requested);
        assert_pixels(3,99);assert_scene(99);
        assert(renders==painted+1 && kp_output_work.work.runs==passes+1);
        assert(writes==4);
        assert_debt(KP_RGB_OUTPUT_NONE,0,100);
    } else if (!strcmp(name,"requests")) {
        host_allow_color_failure=true;failures=1;
        host_run_until(7);zmk_rgb_matrix_flush();host_run_ready();
        for(uint64_t t=8;t<107;t++) {
            host_run_until(t);
            const struct zmk_position_state_changed position={.position=0,.state=true,.timestamp=now};
            const zmk_event_t event={.position=&position};
            isr=true;kp_rgb_matrix_event_listener(&event);zmk_rgb_matrix_flush();isr=false;
            host_run_ready();assert(writes==3);
            assert_debt(KP_RGB_OUTPUT_SCENE,107,200);
        }
        assert(feedback==99);host_run_until(107);assert(writes==4);
    } else if (!strcmp(name,"healthy-mutation")) {
        assert(host_mutate_output);assert_scene(17);
        host_run_until(32);assert_scene(17);
        for(size_t i=1;i<host_transfer_count;i++) assert_pixels(i,17);
        zmk_rgb_matrix_off();host_run_ready();assert_pixels(host_transfer_count-1,0);
        assert_debt(KP_RGB_OUTPUT_NONE,0,100);
    } else { assert(!strcmp(name,"unready")); }
    assert(!kp_rgb_output_urgent_black && !polls && !refreshes && !dispatches);
    printf("output-retry %s leds=%d time_ms=%llu passes=%u renders=%d transfers=%zu: passed\n",
           name,KP_LED_COUNT,(unsigned long long)now,kp_output_work.work.runs,renders,host_transfer_count);
    assert(pthread_mutex_destroy(&lock)==0);
}
'''

if __name__ == '__main__':
    run_tests(TESTS, scheduler=True, cases=CASES)
