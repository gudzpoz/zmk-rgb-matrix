#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Compile actual engine functions with host queue/strip mocks, with and without LEDs."""
from engine_harness import run_tests

TESTS = r'''
static void *tick_thread(void *unused) { (void)unused;kp_rgb_matrix_tick(NULL);return NULL; }
static void *setter_thread(void *unused) {
 (void)unused;atomic_set(&setter_entered,1);
 assert(zmk_rgb_matrix_set_inhibited(true)==0);atomic_set(&setter_done,1);return NULL;
}
static void *off_thread(void *unused) { (void)unused;kp_rgb_matrix_off_handler(NULL);return NULL; }
static void *on_thread(void *unused) {
 (void)unused;atomic_set(&setter_entered,1);
 assert(zmk_rgb_matrix_on()==0);atomic_set(&setter_done,1);return NULL;
}
int main(void) {
 pthread_mutexattr_t attributes;
 assert(pthread_mutexattr_init(&attributes)==0);
 assert(pthread_mutexattr_settype(&attributes,PTHREAD_MUTEX_RECURSIVE)==0);
 assert(pthread_mutex_init(&lock,&attributes)==0);
 assert(pthread_mutexattr_destroy(&attributes)==0);
 assert(!zmk_rgb_matrix_is_inhibited());
 isr=true;assert(zmk_rgb_matrix_set_inhibited(true)==-EWOULDBLOCK);isr=false;
 ctx.state.user_on=true;ctx.state.active_fx=&fx;
 ctx.tuning.sentinel=123;
 assert(zmk_rgb_matrix_on()==0 && !atomic_get(&kp_rgb_output_allowed));
 host_try_lock_error=-EBUSY;
 assert(zmk_rgb_matrix_off()==-EBUSY && ctx.state.user_on);
 assert(zmk_rgb_matrix_toggle()==-EBUSY && ctx.state.user_on);
 host_try_lock_error=0;
 assert(zmk_rgb_matrix_off()==0 && !ctx.state.user_on);
 host_try_lock_error=-EBUSY;
 assert(zmk_rgb_matrix_on()==-EBUSY && !ctx.state.user_on);
 host_try_lock_error=0;
 assert(zmk_rgb_matrix_on()==0 && ctx.state.user_on);
 struct kp_rgb_key_event ev={.position=0,.pressed=true};
 assert(!kp_rgb_pending_push(&ev) && !timer_period);
 zmk_rgb_matrix_flush();kp_rgb_matrix_tick_handler(&kp_tick_timer);kp_rgb_matrix_tick(NULL);
 assert(!kp_rgb_output_ready && tick_submits==1 && !renders && !writes);
 if(KP_LED_COUNT) {
  host_strip_ready=false;
  assert(kp_rgb_matrix_init()==-ENODEV);
  zmk_rgb_matrix_flush();kp_rgb_matrix_tick(NULL);
  assert(!kp_rgb_output_ready && tick_submits==2 && !renders && !writes);
  host_strip_ready=true;
 }
 assert(zmk_rgb_matrix_set_inhibited(true)==0);
 assert(!kp_rgb_output_ready && writes==0);
 ctx.dev=&dummy;
 failures=KP_LED_COUNT?1:0;
 assert(kp_rgb_matrix_init()==0);
 assert(kp_rgb_output_ready);
 assert(ctx.state.user_on && ctx.tuning.sentinel==123);
 assert(ctx.state.active_fx==&fx);
 bool state=false;assert(zmk_rgb_matrix_get_state(&state)==0 && state);
 assert(!kp_rgb_pending_push(&ev));
 assert(timer_period==0);
 int submitted=tick_submits;
 kp_rgb_matrix_tick_handler(&kp_tick_timer);
 assert(tick_submits==submitted);
 now=10000;kp_rgb_matrix_tick(NULL);
 assert(polls==0 && refreshes==0 && dispatches==0 && renders==0 && feedback==0 && colored==0);
 for(int i=0;i<3;i++) { now+=CONFIG_KEYPAW_RGB_MATRIX_TICK_MS;kp_rgb_matrix_tick(NULL); }
 assert(polls==0 && refreshes==0 && dispatches==0 && renders==0 && feedback==0);
 if(KP_LED_COUNT) {
  assert(kp_rgb_black_pending && retry_delay==100);failures=7;
  int delays[]={200,400,800,1000,1000,1000,1000};
  for(int i=0;i<7;i++) { kp_rgb_matrix_off_handler(NULL);assert(retry_delay==delays[i]);assert(colored==0); }
  kp_rgb_matrix_off_handler(NULL);assert(!kp_rgb_black_pending);
 }
 struct binding binding={.param1=RGB_TOG_CMD};
 assert(convert_toggle(&ctx,&binding)==0 && binding.param1==RGB_OFF_CMD);
 assert(zmk_rgb_matrix_off()==0);
 binding.param1=RGB_TOG_CMD;
 assert(convert_toggle(&ctx,&binding)==0 && binding.param1==RGB_ON_CMD);
 assert(zmk_rgb_matrix_on()==0 && ctx.state.user_on);
 assert(zmk_rgb_matrix_is_inhibited() && !colored);
 assert(zmk_rgb_matrix_set_inhibited(false)==0);
 now+=16;kp_rgb_matrix_tick(NULL);
 if(KP_LED_COUNT) assert(renders==1 && rendered_elapsed==0);
 assert(kp_rgb_pending_push(&ev)==(KP_LED_COUNT>0));
 assert(zmk_rgb_matrix_set_inhibited(true)==0);
 assert(zmk_rgb_matrix_set_inhibited(false)==0);
 kp_rgb_matrix_tick(NULL);assert(feedback==0);
 assert(kp_rgb_pending_push(&ev)==(KP_LED_COUNT>0));kp_rgb_matrix_tick(NULL);assert(feedback==(KP_LED_COUNT>0));
 int before=writes;kp_rgb_matrix_off_handler(NULL);assert(writes==before);
 // Normal OFF failure must retry even after the driver's buffer mutation.
 assert(zmk_rgb_matrix_off()==0);
 if(KP_LED_COUNT) {
  failures=1;kp_rgb_matrix_off_handler(NULL);assert(kp_rgb_black_pending);
  kp_rgb_matrix_off_handler(NULL);assert(!kp_rgb_black_pending);
 }
 assert(zmk_rgb_matrix_on()==0);
 // A queued normal OFF clear cannot overwrite a newer ON.
 assert(zmk_rgb_matrix_off()==0);
 assert(zmk_rgb_matrix_on()==0);
 before=writes;kp_rgb_matrix_off_handler(NULL);assert(writes==before);
 // Local idle policy remains independent of the inhibition gate.
 host_activity(false);assert(!kp_rgb_logical_on_locked() && ctx.state.user_on);
 host_activity(true);assert(kp_rgb_logical_on_locked());
 if(KP_LED_COUNT) {
  pthread_t tick,setter;
  atomic_set(&block_transfer,1);
  pthread_create(&tick,NULL,tick_thread,NULL);
  while(!atomic_get(&transfer_entered)) sched_yield();
  pthread_create(&setter,NULL,setter_thread,NULL);
  while(!atomic_get(&setter_entered)) sched_yield();
  assert(!atomic_get(&setter_done));
  atomic_set(&transfer_release,1);
  pthread_join(tick,NULL);pthread_join(setter,NULL);
  atomic_set(&block_transfer,0);
  before=colored;kp_rgb_matrix_tick(NULL);assert(colored==before);
  kp_rgb_matrix_off_handler(NULL);assert(colored==before);
 }
 if(KP_LED_COUNT) {
  // ON waits for an in-flight black transfer and suppresses its failed retry.
  pthread_t off,on;
  zmk_rgb_matrix_set_inhibited(false);zmk_rgb_matrix_off();
  atomic_set(&transfer_entered,0);atomic_set(&transfer_release,0);
  atomic_set(&setter_entered,0);atomic_set(&setter_done,0);
  atomic_set(&block_transfer,1);failures=1;
  pthread_create(&off,NULL,off_thread,NULL);
  while(!atomic_get(&transfer_entered)) sched_yield();
  pthread_create(&on,NULL,on_thread,NULL);
  while(!atomic_get(&setter_entered)) sched_yield();
  assert(!atomic_get(&setter_done));atomic_set(&transfer_release,1);
  pthread_join(off,NULL);pthread_join(on,NULL);atomic_set(&block_transfer,0);
  before=writes;kp_rgb_matrix_off_handler(NULL);assert(writes==before);
 }
 if(KP_LED_COUNT) {
  zmk_rgb_matrix_set_inhibited(false);
  assert(zmk_rgb_matrix_on()==0);
  before=colored;inhibit_from_render=true;
  kp_rgb_matrix_tick(NULL);
  assert(zmk_rgb_matrix_is_inhibited() && colored==before);
 }
 assert(zmk_rgb_matrix_set_inhibited(true)==0);
 inject_pending_press=true;
 assert(zmk_rgb_matrix_set_inhibited(false)==0);
 if(KP_LED_COUNT) assert(!inject_pending_press);
 inject_pending_press=false;
 struct kp_rgb_key_event received;
 assert(kp_rgb_pending_pop(&received)==pending_press_accepted);
 assert(kp_rgb_pending_push(&ev)==(KP_LED_COUNT>0));
 assert(kp_rgb_pending_pop(&received)==(KP_LED_COUNT>0));
 // Input racing the close is purged; input after closing is rejected.
 inject_pending_press=KP_LED_COUNT>0;
 assert(zmk_rgb_matrix_set_inhibited(true)==0);
 assert(pending_press_accepted==(KP_LED_COUNT>0));
 assert(!kp_rgb_pending_pop(&received));
 assert(zmk_rgb_matrix_set_inhibited(false)==0);
 inject_after_unlock=KP_LED_COUNT>0;
 assert(zmk_rgb_matrix_set_inhibited(true)==0);
 assert(!post_release_accepted && !kp_rgb_pending_pop(&received));
 // Inject immediately after publication releases the pending spinlock.
 inject_after_unlock=KP_LED_COUNT>0;
 assert(zmk_rgb_matrix_set_inhibited(false)==0);
 assert(post_release_accepted==(KP_LED_COUNT>0));
 assert(kp_rgb_pending_pop(&received)==post_release_accepted);
 // Initial black failure with an already OFF controller is not lost.
 zmk_rgb_matrix_set_inhibited(false);
 zmk_rgb_matrix_off();
 kp_rgb_black_pending=false;failures=KP_LED_COUNT?1:0;
 assert(kp_rgb_matrix_init()==0);
 if(KP_LED_COUNT) {
  assert(kp_rgb_black_pending);kp_rgb_matrix_off_handler(NULL);assert(!kp_rgb_black_pending);
 }
 assert(!polls && !refreshes && !dispatches);
 assert(zmk_rgb_matrix_get_state(NULL)==-EINVAL);
 assert(zmk_rgb_matrix_off()==0);
 assert(zmk_rgb_matrix_toggle()==0 && ctx.state.user_on);
 assert(zmk_rgb_matrix_toggle()==0 && !ctx.state.user_on);
 puts("production-extracted output gate tests passed");
}
'''

if __name__ == "__main__":
    run_tests(TESTS)
