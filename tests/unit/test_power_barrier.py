#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Async condition-refresh barriers around permission resumes."""
from engine_harness import run_tests
from test_engine_scheduler import TESTS

TESTS = TESTS.split("int main(int argc, char **argv)", 1)[0] + r'''
int main(int argc, char **argv) {
    assert(argc == 2);
    const char *name=argv[1];
    host_manual_refresh=true;
    if(!KP_LED_COUNT) { fixture(true,false); assert(!kp_rgb_refresh_pending); assert(pthread_mutex_destroy(&lock)==0); return 0; }
    if(!strcmp(name,"refresh-barrier")) {
        fixture(true,false);
        isr=true;
        bool state=false,user_state=false;
        assert(zmk_rgb_matrix_on()==-EWOULDBLOCK);
        assert(zmk_rgb_matrix_off()==-EWOULDBLOCK);
        assert(zmk_rgb_matrix_toggle()==-EWOULDBLOCK);
        assert(zmk_rgb_matrix_get_state(&state)==-EWOULDBLOCK);
        assert(zmk_rgb_matrix_get_user_state(&user_state)==-EWOULDBLOCK);
        isr=false;
        assert(kp_rgb_global.user_on);
        uint64_t initial=kp_rgb_refresh_token;
        assert(kp_rgb_refresh_pending && initial != 0);
        host_run_ready();
        assert(!renders && !colored);
        struct kp_rgb_key_event ev={.position=0,.pressed=true};
        assert(kp_rgb_pending_push(&ev));
        kp_rgb_request_output_pass(false);
        host_run_ready();
        assert(!feedback && !renders && kp_rgb_pending_available());
        kp_rgb_conditions_refreshed(initial);
        host_run_ready();
        assert(!kp_rgb_refresh_pending && !kp_rgb_pending_available());
        assert(feedback==(KP_LED_COUNT>0));
        assert(renders==(KP_LED_COUNT?1:0));
        assert(colored==(KP_LED_COUNT?1:0));
    } else if(!strcmp(name,"stale-refresh")) {
        fixture(true,false);
        uint64_t initial=kp_rgb_refresh_token;
        kp_rgb_conditions_refreshed(initial);
        host_run_ready();
        assert(!kp_rgb_refresh_pending);
        assert(zmk_rgb_matrix_set_inhibited(true)==0);
        assert(zmk_rgb_matrix_set_inhibited(false)==0);
        uint64_t first_resume=kp_rgb_refresh_token;
        assert(kp_rgb_refresh_pending && first_resume>initial);
        assert(zmk_rgb_matrix_set_inhibited(true)==0);
        assert(zmk_rgb_matrix_set_inhibited(false)==0);
        uint64_t latest_resume=kp_rgb_refresh_token;
        assert(kp_rgb_refresh_pending && latest_resume>first_resume);
        host_run_ready();
        int before=renders;
        kp_rgb_conditions_refreshed(first_resume);
        host_run_ready();
        assert(kp_rgb_refresh_pending && renders==before);
        kp_rgb_conditions_refreshed(latest_resume);
        host_run_ready();
        assert(!kp_rgb_refresh_pending);
        assert(renders==before+(KP_LED_COUNT?1:0));
        if(KP_LED_COUNT) assert(rendered_elapsed==0);
    } else if(!strcmp(name,"selection-holds")) {
        host_allow_color_failure=true; failures=1;
        fixture(true,false);
        uint64_t initial=kp_rgb_refresh_token;
        kp_rgb_conditions_refreshed(initial);
        host_run_ready();
        assert(renders==1 && kp_rgb_output_pending==KP_RGB_OUTPUT_SCENE);
        assert(kp_rgb_output_retry_deadline_ms==100 && kp_rgb_output_retry_ms==200);
        int before=writes;
        kp_rgb_matrix_lock();
        kp_rgb_control_refresh_hold_locked();
        kp_rgb_control_refresh_hold_locked();
        kp_rgb_control_refresh_locked();
        uint64_t latest=kp_rgb_refresh_token;
        zmk_rgb_matrix_flush();
        kp_rgb_matrix_unlock();
        assert(host_refresh_token==initial);
        struct kp_rgb_key_event ev={.position=0,.pressed=true,.timestamp_ms=12};
        assert(kp_rgb_pending_push(&ev));
        kp_rgb_conditions_refreshed(initial);
        kp_rgb_conditions_refreshed(latest);
        host_run_until(150);
        assert(kp_rgb_refresh_pending && kp_rgb_refresh_holds==2);
        assert(!feedback && renders==1 && writes==before && kp_rgb_pending_available());
        assert(kp_rgb_output_retry_deadline_ms==100 && kp_rgb_output_retry_ms==200);
        kp_rgb_matrix_lock();
        kp_rgb_control_refresh_release_locked();
        assert(host_refresh_token==initial);
        kp_rgb_control_refresh_release_locked();
        kp_rgb_matrix_unlock();
        assert(host_refresh_token==latest && !kp_rgb_refresh_holds);
        kp_rgb_conditions_refreshed(initial);
        host_run_ready(); assert(kp_rgb_refresh_pending && writes==before);
        host_render_red=42;
        kp_rgb_conditions_refreshed(latest);
        host_run_ready();
        assert(!kp_rgb_refresh_pending && feedback==1 && renders==2 && writes==before+1);
        assert(rendered_elapsed==150 && !kp_rgb_pending_available());
        assert_pixels(host_transfer_count-1,42);
        assert(kp_rgb_output_pending==KP_RGB_OUTPUT_NONE && kp_rgb_output_retry_ms==100);
    } else if(!strcmp(name,"resume-held")) {
        fixture(true,false);
        uint64_t initial=kp_rgb_refresh_token;
        kp_rgb_conditions_refreshed(initial); host_run_ready();
        kp_rgb_matrix_lock(); kp_rgb_control_refresh_hold_locked(); kp_rgb_matrix_unlock();
        uint64_t selection=kp_rgb_refresh_token;
        assert(zmk_rgb_matrix_set_inhibited(true)==0);
        assert(zmk_rgb_matrix_set_inhibited(false)==0);
        uint64_t resume=kp_rgb_refresh_token;
        assert(resume>selection && host_refresh_token==initial);
        kp_rgb_conditions_refreshed(selection);
        kp_rgb_conditions_refreshed(resume);
        host_run_ready(); assert(kp_rgb_refresh_pending && renders==1);
        kp_rgb_matrix_lock(); kp_rgb_control_refresh_release_locked(); kp_rgb_matrix_unlock();
        assert(host_refresh_token==resume);
        kp_rgb_conditions_refreshed(selection);
        host_run_ready(); assert(kp_rgb_refresh_pending && renders==1);
        kp_rgb_conditions_refreshed(resume);
        host_run_ready(); assert(!kp_rgb_refresh_pending && renders==2 && rendered_elapsed==0);
    } else if(!strcmp(name,"resume-participation")) {
        static const struct kp_rgb_effect_config second_config = {.owner = &controller_dev, .slot = 1};
        const struct device second = {.api = &api, .config = &second_config};
        struct host_overlay second_data={.common={.index=1},.child=&second};
        const struct device second_overlay={.api=&host_child_api,.data=&second_data};
        host_overlays[1]=&second_overlay;host_overlay_count=2;
        fixture(true,false);
        kp_rgb_conditions_refreshed(kp_rgb_refresh_token);
        host_run_ready();
        assert(renders == 1 && kp_rgb_overlay_scenes[0].state.active);
        assert(zmk_rgb_matrix_off() == 0);
        host_run_ready();
        assert(host_bottom_data.child == &fx && !kp_rgb_overlay_scenes[0].state.active);
        kp_rgb_matrix_lock();
        host_bottom_data.gate=false;second_data.gate=true;
        zmk_rgb_matrix_flush();
        kp_rgb_matrix_unlock();
        host_run_ready();
        assert(!kp_rgb_global.user_on && renders == 1);
        assert(!kp_rgb_overlay_scenes[1].state.active);
        assert(zmk_rgb_matrix_on() == 0);
        uint64_t resume = kp_rgb_refresh_token;
        assert(kp_rgb_refresh_pending);
        struct kp_rgb_key_event ev = {.position=0,.pressed=true,.timestamp_ms=123};
        assert(kp_rgb_pending_push(&ev));
        kp_rgb_request_output_pass(false);
        int before = writes;
        host_run_ready();
        assert(renders == 1 && writes == before && !feedback);
        assert(kp_rgb_pending_available() && !kp_rgb_overlay_scenes[1].state.active);
        assert(kp_rgb_overlay_scenes[1].reset_pending);
        assert(second_data.gate && second_data.child == &second);
        kp_rgb_conditions_refreshed(resume);
        host_run_ready();
        assert(!kp_rgb_refresh_pending && !kp_rgb_pending_available());
        assert(feedback == 1 && renders == 2 && rendered_elapsed == 0);
        assert(!kp_rgb_overlay_scenes[0].state.active && kp_rgb_overlay_scenes[1].state.active);
    } else { assert(!"unknown case"); }
    assert(!polls && !refreshes && !dispatches);
    assert(pthread_mutex_destroy(&lock)==0);
    return 0;
}
'''

if __name__ == '__main__':
    run_tests(TESTS, scheduler=True,
              cases=("refresh-barrier", "stale-refresh", "resume-participation", "selection-holds", "resume-held"),
              allow_unused=True)
