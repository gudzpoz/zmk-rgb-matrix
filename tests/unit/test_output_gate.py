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
 assert(zmk_rgb_matrix_on(&dummy)==0);atomic_set(&setter_done,1);return NULL;
}
int main(void) {
 pthread_mutexattr_t attributes;
 assert(pthread_mutexattr_init(&attributes)==0);
 assert(pthread_mutexattr_settype(&attributes,PTHREAD_MUTEX_RECURSIVE)==0);
 assert(pthread_mutex_init(&lock,&attributes)==0);
 assert(pthread_mutexattr_destroy(&attributes)==0);
 assert(!zmk_rgb_matrix_is_inhibited());
 isr=true;assert(zmk_rgb_matrix_set_inhibited(true)==-EWOULDBLOCK);isr=false;
 ctx.state.on=true;ctx.state.user_on=true;ctx.state.active_fx=&fx;
 ctx.zone_valid=ctx.all_leds=true;ctx.tuning.sentinel=123;
 assert(zmk_rgb_matrix_set_inhibited(true)==0);
 assert(!kp_rgb_output_ready && writes==0);
 ctx.dev=&dummy;
 failures=KP_LED_COUNT?1:0;
 assert(kp_rgb_matrix_init()==0);
 assert(ctx.state.on && ctx.state.user_on && ctx.tuning.sentinel==123);
 assert(ctx.state.active_fx==&fx);
 bool state=false;assert(zmk_rgb_matrix_get_state(&dummy,&state)==0 && state);
 struct zmk_position_state_changed ev={.position=0,.state=true};
 assert(!kp_rgb_pending_push(&ev));
 assert(timer_period==CONFIG_KEYPAW_RGB_MATRIX_TICK_MS);
 int submitted=tick_submits;
 kp_rgb_matrix_tick_handler(&kp_tick_timer);
 assert(tick_submits==submitted+1);
 now=10000;kp_rgb_matrix_tick(NULL);
 assert(polls==1 && refreshes==1 && dispatches==1 && renders==0 && feedback==0 && colored==0);
 for(int i=0;i<3;i++) { now+=CONFIG_KEYPAW_RGB_MATRIX_TICK_MS;kp_rgb_matrix_tick(NULL); }
 assert(polls==4 && refreshes==4 && dispatches==4 && renders==0 && feedback==0);
 if(KP_LED_COUNT) {
  assert(kp_rgb_black_pending && retry_delay==100);failures=7;
  int delays[]={200,400,800,1000,1000,1000,1000};
  for(int i=0;i<7;i++) { kp_rgb_matrix_off_handler(NULL);assert(retry_delay==delays[i]);assert(colored==0); }
  kp_rgb_matrix_off_handler(NULL);assert(!kp_rgb_black_pending);
 }
 struct binding binding={.param1=RGB_TOG_CMD};
 assert(convert_toggle(&ctx,&binding)==0 && binding.param1==RGB_OFF_CMD);
 assert(zmk_rgb_matrix_off(&dummy)==0);
 binding.param1=RGB_TOG_CMD;
 assert(convert_toggle(&ctx,&binding)==0 && binding.param1==RGB_ON_CMD);
 assert(zmk_rgb_matrix_on(&dummy)==0 && ctx.state.on);
 assert(zmk_rgb_matrix_is_inhibited() && !colored);
 assert(zmk_rgb_matrix_set_inhibited(false)==0);
 now+=16;kp_rgb_matrix_tick(NULL);
 if(KP_LED_COUNT) assert(renders==1 && rendered_elapsed==16);
 assert(kp_rgb_pending_push(&ev));
 assert(zmk_rgb_matrix_set_inhibited(true)==0);
 assert(zmk_rgb_matrix_set_inhibited(false)==0);
 kp_rgb_matrix_pending_handler(NULL);assert(feedback==0);
 assert(kp_rgb_pending_push(&ev));kp_rgb_matrix_pending_handler(NULL);assert(feedback==1);
 int before=writes;kp_rgb_matrix_off_handler(NULL);assert(writes==before);
 // Normal OFF failure must retry even after the driver's buffer mutation.
 assert(zmk_rgb_matrix_off(&dummy)==0);
 if(KP_LED_COUNT) {
  failures=1;kp_rgb_matrix_off_handler(NULL);assert(kp_rgb_black_pending);
  kp_rgb_matrix_off_handler(NULL);assert(!kp_rgb_black_pending);
 }
 assert(zmk_rgb_matrix_on(&dummy)==0);
 // A queued normal OFF clear cannot overwrite a newer ON.
 assert(zmk_rgb_matrix_off(&dummy)==0);
 assert(zmk_rgb_matrix_on(&dummy)==0);
 before=writes;kp_rgb_matrix_off_handler(NULL);assert(writes==before);
 // Local idle policy remains independent of the inhibition gate.
 idle=true;kp_rgb_permission_handler(NULL);assert(!ctx.state.on && ctx.state.user_on);
 idle=false;kp_rgb_permission_handler(NULL);assert(ctx.state.on);
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
  zmk_rgb_matrix_set_inhibited(false);zmk_rgb_matrix_off(&dummy);
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
  assert(zmk_rgb_matrix_on(&dummy)==0);
  before=colored;inhibit_from_render=true;
  kp_rgb_matrix_tick(NULL);
  assert(zmk_rgb_matrix_is_inhibited() && colored==before);
 }
 assert(zmk_rgb_matrix_set_inhibited(true)==0);
 inject_pending_press=true;
 assert(zmk_rgb_matrix_set_inhibited(false)==0 && !inject_pending_press);
 struct zmk_position_state_changed received;
 assert(kp_rgb_pending_pop(&received)==pending_press_accepted);
 assert(kp_rgb_pending_push(&ev) && kp_rgb_pending_pop(&received));
 // Initial black failure with an already OFF controller is not lost.
 zmk_rgb_matrix_set_inhibited(false);
 zmk_rgb_matrix_off(&dummy);
 kp_rgb_black_pending=false;failures=KP_LED_COUNT?1:0;
 assert(kp_rgb_matrix_init()==0);
 if(KP_LED_COUNT) {
  assert(kp_rgb_black_pending);kp_rgb_matrix_off_handler(NULL);assert(!kp_rgb_black_pending);
 }
 puts("production-extracted output gate tests passed");
}
'''

if __name__ == "__main__":
    run_tests(TESTS)
