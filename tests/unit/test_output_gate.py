#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Production output serialization, admission and power API safety."""
from engine_harness import run_tests

TESTS = r'''
static void pass(void) { kp_rgb_output_handler(NULL); }
static void repaint(void) { zmk_rgb_matrix_flush(); pass(); }
static void black_due(void) {
 assert(kp_rgb_output_pending == KP_RGB_OUTPUT_BLACK && kp_rgb_output_retry_deadline_ms > 0);
 now=kp_rgb_output_retry_deadline_ms; pass();
}
static void *output_thread(void *unused) { (void)unused;pass();return NULL; }
static void *setter_thread(void *unused) {
 (void)unused;atomic_set(&setter_entered,1);
 assert(zmk_rgb_matrix_set_inhibited(true)==0);atomic_set(&setter_done,1);return NULL;
}
static void *on_thread(void *unused) {
 (void)unused;atomic_set(&setter_entered,1);
 assert(zmk_rgb_matrix_on()==0);atomic_set(&setter_done,1);return NULL;
}
static void blocked_render(void) {
 assert(held);
 host_render_hook=NULL;
 atomic_set(&transfer_entered,1);
 while(!atomic_get(&transfer_release)) sched_yield();
}
int main(void) {
 pthread_mutexattr_t attributes;
 assert(pthread_mutexattr_init(&attributes)==0);
 assert(pthread_mutexattr_settype(&attributes,PTHREAD_MUTEX_RECURSIVE)==0);
 assert(pthread_mutex_init(&lock,&attributes)==0);
 assert(pthread_mutexattr_destroy(&attributes)==0);
 assert(!zmk_rgb_matrix_is_inhibited());
 isr=true;assert(zmk_rgb_matrix_set_inhibited(true)==-EWOULDBLOCK);isr=false;
 ctx.state.user_on=true;ctx.state.active_fx=&fx;ctx.tuning.idle_brightness=23;
 assert(zmk_rgb_matrix_on()==0 && !atomic_get(&kp_rgb_output_allowed));
 host_try_lock_error=-EBUSY;
 assert(zmk_rgb_matrix_off()==-EBUSY && ctx.state.user_on);
 assert(zmk_rgb_matrix_toggle()==-EBUSY && ctx.state.user_on);
 host_try_lock_error=0;assert(zmk_rgb_matrix_off()==0 && !ctx.state.user_on);
 host_try_lock_error=-EBUSY;
 assert(zmk_rgb_matrix_on()==-EBUSY && !ctx.state.user_on);
 host_try_lock_error=0;assert(zmk_rgb_matrix_on()==0 && ctx.state.user_on);
 struct kp_rgb_key_event ev={.position=0,.pressed=true};
 assert(!kp_rgb_pending_push(&ev) && kp_rgb_next_frame_ms==INT64_MAX);
 repaint();assert(!kp_rgb_output_ready && !renders && !writes);
 if(KP_LED_COUNT) {
  host_strip_ready=false;assert(kp_rgb_matrix_init()==-ENODEV);
  repaint();assert(!kp_rgb_output_ready && !renders && !writes);host_strip_ready=true;
 }
 assert(zmk_rgb_matrix_set_inhibited(true)==0);
 assert(!kp_rgb_output_ready && writes==0);ctx.dev=&dummy;
 failures=KP_LED_COUNT?1:0;assert(kp_rgb_matrix_init()==0);
 assert(kp_rgb_output_ready && ctx.state.user_on && ctx.tuning.idle_brightness==23);
 assert(ctx.state.active_fx==&fx);
 bool state=false;assert(zmk_rgb_matrix_get_state(&state)==0 && state);
 assert(!kp_rgb_pending_push(&ev) && kp_rgb_next_frame_ms==INT64_MAX);
 if(KP_LED_COUNT) {
  assert(kp_rgb_output_pending == KP_RGB_OUTPUT_BLACK && kp_rgb_output_retry_deadline_ms==100);
  pass();assert(writes==1);failures=7;
  int delays[]={200,400,800,1000,1000,1000,1000};
  for(int i=0;i<7;i++) {
   black_due();assert(kp_rgb_output_retry_deadline_ms==(int64_t)now+delays[i] && colored==0);
  }
  black_due();assert(kp_rgb_output_pending == KP_RGB_OUTPUT_NONE);
 }
 now+=10000;repaint();assert(!renders && !feedback && !colored);
 struct binding binding={.param1=RGB_TOG_CMD};
 assert(convert_toggle(&ctx,&binding)==0 && binding.param1==RGB_OFF_CMD);
 assert(zmk_rgb_matrix_off()==0);binding.param1=RGB_TOG_CMD;
 assert(convert_toggle(&ctx,&binding)==0 && binding.param1==RGB_ON_CMD);
 assert(zmk_rgb_matrix_on()==0 && ctx.state.user_on);
 assert(zmk_rgb_matrix_is_inhibited() && !colored);
 assert(zmk_rgb_matrix_set_inhibited(false)==0);pass();
 if(KP_LED_COUNT) assert(renders==1 && rendered_elapsed==0);
 assert(kp_rgb_pending_push(&ev)==(KP_LED_COUNT>0));
 assert(zmk_rgb_matrix_set_inhibited(true)==0);
 assert(zmk_rgb_matrix_set_inhibited(false)==0);pass();assert(feedback==0);
 assert(kp_rgb_pending_push(&ev)==(KP_LED_COUNT>0));pass();assert(feedback==(KP_LED_COUNT>0));
 int before=writes;pass();assert(writes==before);
 assert(zmk_rgb_matrix_off()==0);
 if(KP_LED_COUNT) {
  failures=1;pass();assert(kp_rgb_output_pending == KP_RGB_OUTPUT_BLACK);
  black_due();assert(kp_rgb_output_pending == KP_RGB_OUTPUT_NONE);
 }
 assert(zmk_rgb_matrix_on()==0);pass();
 // A stale blackout wake follows current permission, never the old OFF state.
 assert(zmk_rgb_matrix_off()==0);assert(zmk_rgb_matrix_on()==0);pass();
 if(KP_LED_COUNT) assert(host_transfers[host_transfer_count-1].pixels[0].r==17);
 before=writes;pass();assert(writes==before);
 host_activity(false);assert(!kp_rgb_logical_on_locked() && ctx.state.user_on);
 host_activity(true);assert(kp_rgb_logical_on_locked());
 if(KP_LED_COUNT) {
  pthread_t output,setter;atomic_set(&block_transfer,1);
  pthread_create(&output,NULL,output_thread,NULL);
  while(!atomic_get(&transfer_entered)) sched_yield();
  pthread_create(&setter,NULL,setter_thread,NULL);
  while(!atomic_get(&setter_entered)) sched_yield();
  assert(!atomic_get(&setter_done));atomic_set(&transfer_release,1);
  pthread_join(output,NULL);pthread_join(setter,NULL);atomic_set(&block_transfer,0);
  before=colored;pass();assert(colored==before);pass();assert(colored==before);
 }
 if(KP_LED_COUNT) {
  // ON waits for black transfer completion but retains its failure deadline.
  pthread_t output,on;zmk_rgb_matrix_set_inhibited(false);zmk_rgb_matrix_off();
  atomic_set(&transfer_entered,0);atomic_set(&transfer_release,0);
  atomic_set(&setter_entered,0);atomic_set(&setter_done,0);
  atomic_set(&block_transfer,1);failures=1;
  pthread_create(&output,NULL,output_thread,NULL);
  while(!atomic_get(&transfer_entered)) sched_yield();
  pthread_create(&on,NULL,on_thread,NULL);
  while(!atomic_get(&setter_entered)) sched_yield();
  assert(!atomic_get(&setter_done));atomic_set(&transfer_release,1);
  pthread_join(output,NULL);pthread_join(on,NULL);atomic_set(&block_transfer,0);
  assert(kp_rgb_output_pending == KP_RGB_OUTPUT_NONE);before=writes;pass();
  assert(writes==before && kp_rgb_output_pending == KP_RGB_OUTPUT_SCENE);
  now=kp_rgb_output_retry_deadline_ms;pass();
  assert(host_transfers[host_transfer_count-1].pixels[0].r==17);
 }
 if(KP_LED_COUNT) {
  assert(zmk_rgb_matrix_set_inhibited(false)==0);
  assert(zmk_rgb_matrix_on()==0);
  atomic_set(&transfer_entered,0);atomic_set(&transfer_release,0);
  atomic_set(&setter_entered,0);atomic_set(&setter_done,0);
  host_render_hook=blocked_render;
  zmk_rgb_matrix_flush();
  pthread_t output,setter;
  pthread_create(&output,NULL,output_thread,NULL);
  while(!atomic_get(&transfer_entered)) sched_yield();
  pthread_create(&setter,NULL,setter_thread,NULL);
  while(!atomic_get(&setter_entered)) sched_yield();
  assert(!atomic_get(&setter_done));
  before=colored;atomic_set(&transfer_release,1);
  pthread_join(output,NULL);pthread_join(setter,NULL);
  assert(colored==before+1 && atomic_get(&setter_done));
  before=colored;repaint();repaint();assert(colored==before);
  assert(!runtime.active && !kp_rgb_pending_push(&ev));
 }
 assert(zmk_rgb_matrix_set_inhibited(true)==0);inject_pending_press=true;
 assert(zmk_rgb_matrix_set_inhibited(false)==0);
 if(KP_LED_COUNT) assert(!inject_pending_press);
 inject_pending_press=false;
 struct kp_rgb_key_event received;
 assert(kp_rgb_pending_pop(&received)==pending_press_accepted);
 assert(kp_rgb_pending_push(&ev)==(KP_LED_COUNT>0));
 assert(kp_rgb_pending_pop(&received)==(KP_LED_COUNT>0));
 inject_pending_press=KP_LED_COUNT>0;
 assert(zmk_rgb_matrix_set_inhibited(true)==0);
 assert(pending_press_accepted==(KP_LED_COUNT>0));assert(!kp_rgb_pending_pop(&received));
 assert(zmk_rgb_matrix_set_inhibited(false)==0);inject_after_unlock=KP_LED_COUNT>0;
 assert(zmk_rgb_matrix_set_inhibited(true)==0);
 assert(!post_release_accepted && !kp_rgb_pending_pop(&received));
 inject_after_unlock=KP_LED_COUNT>0;assert(zmk_rgb_matrix_set_inhibited(false)==0);
 assert(post_release_accepted==(KP_LED_COUNT>0));
 assert(kp_rgb_pending_pop(&received)==post_release_accepted);
 zmk_rgb_matrix_set_inhibited(false);zmk_rgb_matrix_off();
 kp_rgb_output_pending=KP_RGB_OUTPUT_NONE;failures=KP_LED_COUNT?1:0;
 assert(kp_rgb_matrix_init()==0);
 if(KP_LED_COUNT) { assert(kp_rgb_output_pending == KP_RGB_OUTPUT_BLACK);black_due();assert(kp_rgb_output_pending == KP_RGB_OUTPUT_NONE); }
 assert(!polls && !refreshes && !dispatches);
 assert(zmk_rgb_matrix_get_state(NULL)==-EINVAL);assert(zmk_rgb_matrix_off()==0);
 assert(zmk_rgb_matrix_toggle()==0 && ctx.state.user_on);
 assert(zmk_rgb_matrix_toggle()==0 && !ctx.state.user_on);
 puts("production-extracted output gate tests passed");
}
'''

if __name__ == '__main__':
    run_tests(TESTS)
