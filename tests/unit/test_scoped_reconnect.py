#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Production selector reconnect worker: independent, revisioned frozen lanes.

Mocks model transport acceptance, not remote acknowledgments or renderer output.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

from test_singleton import ROOT, function, structure
from test_scoped_split import PRELUDE, TESTS as SCOPED_MOCKS

MOCKS = r'''
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define ARG_UNUSED(x) ((void)(x))
#define LOG_INF(...) ((void)0)
#define KP_RGB_SYNC_SOURCES 2
#define KP_RGB_SYNC_SELECTOR_CAPACITY 2
#define KP_RGB_SYNC_NO_EFFECT UINT16_MAX
#define CONFIG_KEYPAW_RGB_SPLIT_SYNC_SETTLE_MS 100
#define CONFIG_KEYPAW_RGB_SPLIT_SYNC_POLL_MS 250
#define CONFIG_KEYPAW_RGB_SPLIT_SYNC_CLOCK_MS 500
#define K_MSEC(x) (x)
#define K_NO_WAIT 0
struct k_work { int unused; };
struct k_work_delayable { void (*handler)(struct k_work *); int64_t due; unsigned schedules; };
static int64_t now_ms;
static int64_t k_uptime_get(void) { return now_ms; }
static void *kp_rgb_work_q(void) { return NULL; }
static int k_work_reschedule_for_queue(void *q,struct k_work_delayable *w,int64_t delay) {
 assert(!q && delay>=0); w->due=now_ms+delay; w->schedules++; return 0;
}
static int k_work_schedule_for_queue(void *q,struct k_work_delayable *w,int64_t delay) {
 return k_work_reschedule_for_queue(q,w,delay);
}
static void k_work_init_delayable(struct k_work_delayable *w,void (*f)(struct k_work *)) {
 *w=(struct k_work_delayable){.handler=f,.due=-1};
}
static void kp_rgb_sync_step_handler(struct k_work *w) { (void)w; }
static void kp_rgb_sync_clock_handler(struct k_work *w) { (void)w; }
struct zmk_split_transport_central_api { int (*get_available_source_ids)(uint8_t *); };
struct zmk_split_transport_central { const struct zmk_split_transport_central_api *api; };
static bool present[2];
static int query_error;
static int get_ids(uint8_t *ids) {
 assert(!held); if(query_error) return query_error;
 unsigned count=0; for(unsigned s=0;s<2;s++) if(present[s]) ids[count++]=s; return count;
}
static const struct zmk_split_transport_central_api transport_api={get_ids};
static const struct zmk_split_transport_central transport_a={&transport_api},transport_b={&transport_api};
static const struct zmk_split_transport_central *transport;
static const struct zmk_split_transport_central *kp_rgb_sync_pick_transport(void) { assert(!held); return transport; }
static void kp_rgb_split_selector_dirty_locked(const struct device *);
'''

TRANSPORT = r'''
struct attempt { unsigned source,selector,step; struct zmk_behavior_binding binding; int result; int64_t start,end; };
static struct attempt attempts[2048];
static unsigned attempt_count,counts[2][2];
static bool failing[2][2];
static int64_t send_duration;
static void (*during_send)(void);
static int zmk_split_central_invoke_behavior(uint8_t source,struct zmk_behavior_binding *binding,
                                             struct zmk_behavior_binding_event event,bool pressed) {
 assert(!held && pressed && event.source==source && event.timestamp==now_ms);
 assert(attempt_count<ARRAY_SIZE(attempts));
 unsigned i=binding->behavior_dev==controls[1].name;
 assert(binding->behavior_dev==controls[i].name);
 unsigned cmd=binding->param1 & KP_RGB_SELECTOR_COMMAND_MASK;
 unsigned step=cmd==RGB_SELECTOR_COLOR_CMD?0:cmd==RGB_SELECTOR_PERIOD_CMD?1:2;
 assert(cmd==RGB_SELECTOR_COLOR_CMD || cmd==RGB_SELECTOR_PERIOD_CMD || cmd==RGB_EFS_CMD);
 struct attempt *a=&attempts[attempt_count++];
 *a=(struct attempt){source,i,step,*binding,failing[source][i]?-EIO:0,now_ms,now_ms+send_duration};
 counts[source][i]++; now_ms+=send_duration;
 if(during_send) { void (*hook)(void)=during_send; during_send=NULL; hook(); }
 return a->result;
}
'''

TESTS = r'''
static void setup(void) {
 assert(!held && locks==unlocks && io_calls==0);
 memset(states,0,sizeof(states)); memset(values,0,sizeof(values));
 memset(kp_rgb_sync_sources,0,sizeof(kp_rgb_sync_sources));
 memset(attempts,0,sizeof(attempts)); memset(counts,0,sizeof(counts)); memset(failing,0,sizeof(failing));
 attempt_count=0; send_duration=0; during_send=NULL; now_ms=0; query_error=0; isr=false;
 transport=&transport_a; kp_rgb_sync_transport=NULL;
 kp_rgb_sync_selector_cursor=0; present[0]=present[1]=true;
 for(unsigned i=0;i<2;i++) {
  owners[i]=(struct device){.name=i?"owner-b":"owner-a",.ready=true};
  controls[i]=(struct device){.name=i?"ctrl-b":"ctrl-a",.ready=true};
  kp_rgb_sync_selectors[i]=(struct kp_rgb_sync_selector){.owner=&owners[i]};
 }
 static const char *names[]={"a0","a2","b0","b1","private"};
 for(unsigned i=0;i<5;i++) {
  configs[i]=(struct kp_rgb_effect_common_config){i<2?&owners[0]:i<4?&owners[1]:NULL,i==1?2:i==3?1:0};
  values[i]=(struct kp_rgb_effect_common_data){{(uint16_t)(100+i),50,100},500+i};
  effects[i]=(struct device){names[i],&configs[i],&values[i],true};
 }
 states[0].selector=(struct kp_rgb_selector){.owner=&owners[0],.effects=members_a,.effect_count=3,.active_fx=&effects[0]};
 states[1].selector=(struct kp_rgb_selector){.owner=&owners[1],.effects=members_b,.effect_count=2,.active_fx=&effects[2]};
 states[0].ready=states[1].ready=true;
 assert(kp_rgb_split_sync_init()==0);
}
static void dirty(unsigned i) { kp_rgb_matrix_lock(); kp_rgb_split_selector_dirty_locked(&owners[i]); kp_rgb_matrix_unlock(); }
static void select_member(unsigned i,unsigned slot) {
 kp_rgb_matrix_lock(); states[i].selector.effect_index=slot; states[i].selector.active_fx=states[i].selector.effects[slot];
 kp_rgb_split_selector_dirty_locked(&owners[i]); kp_rgb_matrix_unlock();
}
static void tick(void) {
 kp_rgb_sync_selector_step_handler(NULL);
 assert(!held && locks==unlocks);
}
static void advance_tick(void) {
 assert(kp_rgb_sync_selector_step.due>=now_ms);
 now_ms=kp_rgb_sync_selector_step.due; tick();
}
static void drain(unsigned limit) {
 while(limit--) {
  bool pending=false;
  for(unsigned s=0;s<2;s++) for(unsigned i=0;i<2;i++) pending|=kp_rgb_sync_sources[s].selectors[i].pending;
  if(!pending) return;
  advance_tick();
 }
 assert(!"selector worker did not settle");
}
static void connected(void) { tick(); assert(attempt_count==0); now_ms=100; tick(); }
static void test_init_poll_settle(void) {
 setup();
 assert(kp_rgb_sync_poll.handler==kp_rgb_sync_poll_handler && kp_rgb_sync_poll.due==250);
 assert(kp_rgb_sync_clock.handler==kp_rgb_sync_clock_handler && kp_rgb_sync_clock.due==500);
 assert(kp_rgb_sync_selector_step.handler==kp_rgb_sync_selector_step_handler && kp_rgb_sync_selector_step.schedules==0);
 kp_rgb_sync_poll_handler(NULL);
 assert(kp_rgb_sync_step.due==0 && kp_rgb_sync_selector_step.due==0 && kp_rgb_sync_poll.due==250);
 assert(kp_rgb_sync_clock.due==100 && attempt_count==0);
 for(unsigned s=0;s<2;s++) for(unsigned i=0;i<2;i++) assert(kp_rgb_sync_sources[s].selectors[i].ready_at==100);
 tick(); assert(attempt_count==0 && kp_rgb_sync_selector_step.due==100);
 now_ms=99; tick(); assert(attempt_count==0);
 now_ms=100; tick(); drain(20);
 assert(attempt_count==12);
 unsigned schedules=kp_rgb_sync_selector_step.schedules;
 kp_rgb_matrix_lock(); kp_rgb_split_selector_dirty_locked(&effects[4]); kp_rgb_matrix_unlock();
 assert(kp_rgb_sync_selector_step.schedules==schedules && kp_rgb_sync_selectors[0].revision==0);
 dirty(0); assert(kp_rgb_sync_selector_step.due==now_ms);
 tick(); assert(attempt_count==13); /* discovery settling is not repeated for dirty state */
 drain(20);
 unsigned scheduled=kp_rgb_sync_selector_step.schedules;
 kp_rgb_sync_poll_handler(NULL); assert(kp_rgb_sync_selector_step.schedules==scheduled);
}
static void test_fairness_continual_dirty(void) {
 setup(); connected();
 for(unsigned n=0;n<24;n++) { now_ms+=50; dirty(0); tick(); }
 for(unsigned s=0;s<2;s++) {
  assert(counts[s][1]==3 && kp_rgb_sync_sources[s].selectors[1].synced);
  assert(counts[s][0]>=9);
 }
 /* The first three rounds are exactly round-robin despite repeated low-index dirties. */
 for(unsigned n=0;n<12;n++) { assert(attempts[n].source==n%4/2 && attempts[n].selector==n%2 && attempts[n].step==n/4); }
 drain(40);
 for(unsigned s=0;s<2;s++) assert(kp_rgb_sync_sources[s].selectors[0].revision==kp_rgb_sync_selectors[0].revision);
}
static void test_failure_isolation_and_completion_backoff(void) {
 setup(); failing[0][0]=true; send_duration=37; connected();
 struct kp_rgb_sync_selector_progress *st=&kp_rgb_sync_sources[0].selectors[0];
 assert(st->ready_at==187 && st->step==0 && st->captured && st->retry_count==1);
 int64_t delays[]={50,100,200,400,800,1000,1000,1000};
 unsigned failures=1;
 for(unsigned n=0;n<100 && failures<ARRAY_SIZE(delays);n++) {
  unsigned before=attempt_count; advance_tick();
  assert(attempt_count==before+1);
  struct attempt *a=&attempts[before];
  if(a->source==0 && a->selector==0) {
   assert(a->step==0 && a->result==-EIO);
   assert(st->ready_at==a->end+delays[failures]); failures++;
  }
 }
 assert(failures==ARRAY_SIZE(delays) && st->retry_count==5);
 assert(counts[0][1]==3 && counts[1][0]==3 && counts[1][1]==3);
 assert(st->pending && st->step==0);
 failing[0][0]=false; advance_tick();
 assert(st->step==1 && st->retry_count==0 && st->ready_at==now_ms+50);
 drain(20); assert(st->synced && !st->pending);
}
static void test_frozen_retry_and_latest_revision(void) {
 setup(); failing[0][0]=true; connected();
 struct zmk_behavior_binding frozen=attempts[0].binding;
 uint64_t revision=kp_rgb_sync_sources[0].selectors[0].revision;
 select_member(0,2);
 kp_rgb_matrix_lock(); values[0].color=(struct kp_rgb_hsb){222,10,100}; values[0].duration_ms=9000;
 kp_rgb_split_selector_dirty_locked(&owners[0]); kp_rgb_matrix_unlock();
 unsigned retries=0;
 for(unsigned n=0;n<12 && !retries;n++) {
  unsigned before=attempt_count; advance_tick(); struct attempt *a=&attempts[before];
  if(a->source==0 && a->selector==0) {
   assert(a->binding.behavior_dev==frozen.behavior_dev && a->binding.param1==frozen.param1 && a->binding.param2==frozen.param2); retries++;
  }
 }
 assert(retries==1 && kp_rgb_sync_sources[0].selectors[0].revision==revision);
 failing[0][0]=false; drain(50);
 unsigned old_step=0,new_step=0;
 for(unsigned n=0;n<attempt_count;n++) {
  struct attempt *a=&attempts[n]; if(a->source || a->selector || a->result<0) continue;
  if(old_step<3) {
   assert(a->step==old_step++);
   if(a->step==0) assert(a->binding.param2==RGB_COLOR_HSB_VAL(100,50,100));
   if(a->step==1) assert(a->binding.param2==500);
   if(a->step==2) assert(a->binding.param2==0);
  } else {
   assert(a->step==new_step++);
   if(a->step==0) assert(a->binding.param1==KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD,2) && a->binding.param2==RGB_COLOR_HSB_VAL(101,50,100));
   if(a->step==1) assert(a->binding.param2==501);
   if(a->step==2) assert(a->binding.param2==2);
  }
 }
 assert(old_step==3 && new_step==3);
 assert(kp_rgb_sync_sources[0].selectors[0].revision==kp_rgb_sync_selectors[0].revision);
}
static void test_each_step_retries_frozen_binding(void) {
 for(unsigned step=0;step<3;step++) {
  setup(); tick();
  struct kp_rgb_sync_selector_progress *st=&kp_rgb_sync_sources[0].selectors[0];
  while(st->step<step) advance_tick();
  failing[0][0]=true;
  unsigned failed=0;
  for(unsigned n=0;n<12;n++) {
   unsigned before=attempt_count; advance_tick();
   if(attempts[before].source==0 && attempts[before].selector==0) { failed=before; break; }
  }
  assert(attempts[failed].result==-EIO && attempts[failed].step==step);
  struct zmk_behavior_binding binding=attempts[failed].binding;
  select_member(0,2);
  kp_rgb_matrix_lock(); values[0].color.h=299; values[0].duration_ms=9999;
  kp_rgb_split_selector_dirty_locked(&owners[0]); kp_rgb_matrix_unlock();
  bool retried=false;
  for(unsigned n=0;n<12 && !retried;n++) {
   unsigned before=attempt_count; advance_tick();
   struct attempt *a=&attempts[before];
   if(a->source==0 && a->selector==0) {
    assert(a->result==-EIO && a->step==step && a->binding.behavior_dev==binding.behavior_dev);
    assert(a->binding.param1==binding.param1 && a->binding.param2==binding.param2); retried=true;
   }
  }
  assert(retried && st->step==step && st->retry_count==2);
  failing[0][0]=false; drain(40);
  assert(st->revision==kp_rgb_sync_selectors[0].revision && st->snapshot.slot==2);
 }
}
static void test_rapid_switch_and_same_member_parameter(void) {
 setup(); connected(); drain(20); unsigned before=attempt_count;
 select_member(0,2); select_member(0,0);
 tick(); drain(20);
 assert(attempt_count==before+6 && kp_rgb_sync_selectors[0].revision==2);
 for(unsigned s=0;s<2;s++) assert(kp_rgb_sync_sources[s].selectors[0].revision==2);
 before=attempt_count;
 assert(kp_rgb_selector_binding_apply(&owners[0],KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD,0),1234)==0);
 tick(); drain(20); assert(attempt_count==before+6 && kp_rgb_sync_selectors[0].revision==3);
 unsigned period_sends=0;
 for(unsigned n=before;n<attempt_count;n++) if(attempts[n].step==1) { assert(attempts[n].binding.param2==1234); period_sends++; }
 assert(period_sends==2);
}
static void mutate_during_send(void) {
 assert(!held);
 assert(kp_rgb_selector_binding_apply(&owners[0],KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD,0),4321)==0);
 select_member(0,2);
}
static void test_change_during_unlocked_send(void) {
 setup(); during_send=mutate_during_send; connected();
 assert(kp_rgb_sync_sources[0].selectors[0].revision==0 && kp_rgb_sync_selectors[0].revision==2);
 drain(40);
 unsigned sequence=0;
 for(unsigned n=0;n<attempt_count;n++) if(!attempts[n].source && !attempts[n].selector) {
  struct attempt *a=&attempts[n]; assert(a->step==sequence%3);
  if(sequence==1) assert(a->binding.param2==500);
  if(sequence==2) assert(a->binding.param2==0);
  if(sequence==4) assert(a->binding.param2==501);
  if(sequence==5) assert(a->binding.param2==2);
  sequence++;
 }
 assert(sequence==6 && kp_rgb_sync_sources[0].selectors[0].revision==2);
}
static void test_disconnect_replacement(void) {
 setup(); failing[0][0]=true; connected();
 present[0]=false; tick();
 for(unsigned i=0;i<2;i++) {
  struct kp_rgb_sync_selector_progress *st=&kp_rgb_sync_sources[0].selectors[i];
  assert(!st->pending && !st->captured && !st->synced && !st->step && !st->retry_count);
 }
 select_member(0,2); failing[0][0]=false; present[0]=true;
 int64_t reconnect=now_ms; tick();
 assert(kp_rgb_sync_sources[0].selectors_ready_at==reconnect+100);
 unsigned before=counts[0][0];
 now_ms=reconnect+99; tick(); assert(counts[0][0]==before);
 now_ms=reconnect+100; tick(); drain(30);
 assert(kp_rgb_sync_sources[0].selectors[0].snapshot.slot==2 && counts[0][0]==before+3);
 failing[0][0]=true; dirty(0); kp_rgb_sync_selector_cursor=0; tick();
 assert(kp_rgb_sync_sources[0].selectors[0].captured && kp_rgb_sync_sources[0].selectors[0].retry_count==1);
 before=attempt_count; transport=&transport_b; tick();
 assert(attempt_count==before);
 for(unsigned s=0;s<2;s++) for(unsigned i=0;i<2;i++) {
  struct kp_rgb_sync_selector_progress *st=&kp_rgb_sync_sources[s].selectors[i];
  assert(st->pending && !st->captured && !st->synced && !st->step && !st->retry_count);
 }
 failing[0][0]=false; now_ms+=100; tick(); drain(30); assert(attempt_count==before+12);
 transport=NULL; tick();
 for(unsigned s=0;s<2;s++) assert(!kp_rgb_sync_sources[s].seen && !kp_rgb_sync_sources[s].selectors[0].pending);
 transport=&transport_a; query_error=-EIO; tick(); assert(!kp_rgb_sync_sources[0].seen);
 query_error=0; tick(); assert(kp_rgb_sync_sources[0].seen);
}
static void test_unready_and_control_independence(void) {
 setup(); states[0].ready=false; connected();
 assert(attempt_count==0 && !kp_rgb_sync_sources[0].selectors[0].captured);
 assert(kp_rgb_sync_sources[0].selectors[0].ready_at==150);
 for(unsigned n=0;n<18;n++) advance_tick();
 assert(counts[0][0]==0 && counts[1][0]==0 && counts[0][1]==3 && counts[1][1]==3);
 states[0].ready=true; drain(30);
 assert(counts[0][0]==3 && counts[1][0]==3);
 /* No renderer, power, inhibition, visibility or LED-count dependency is provided
  * to the extracted worker. A control change must still reach every source. */
 unsigned before=attempt_count;
 dirty(0); dirty(1); tick(); drain(30); assert(attempt_count==before+12);
 kp_rgb_matrix_lock(); uint64_t a=kp_rgb_sync_selectors[0].revision,b=kp_rgb_sync_selectors[1].revision;
 kp_rgb_split_selectors_dirty_locked(); kp_rgb_matrix_unlock();
 assert(kp_rgb_sync_selectors[0].revision==a+1 && kp_rgb_sync_selectors[1].revision==b+1);
 before=attempt_count; tick(); drain(30); assert(attempt_count==before+12);
}
int main(void) {
 test_init_poll_settle(); test_fairness_continual_dirty(); test_failure_isolation_and_completion_backoff();
 test_frozen_retry_and_latest_revision(); test_each_step_retries_frozen_binding(); test_rapid_switch_and_same_member_parameter();
 test_change_during_unlocked_send(); test_disconnect_replacement(); test_unready_and_control_independence();
 puts("scoped reconnect: fairness, frozen revisions, completion backoff, readiness and transport lifecycle passed");
}
'''


def fixture():
    split = (ROOT / "src/rgb_split_sync.c").read_text()
    sync_h = (ROOT / "src/rgb_selector_sync_internal.h").read_text()
    sync = (ROOT / "src/rgb_selector_sync.c").read_text()
    selector_h = (ROOT / "src/rgb_selector_internal.h").read_text()
    selector = (ROOT / "src/rgb_selector.c").read_text()
    control = (ROOT / "src/behaviors/selector.c").read_text()
    # The existing snapshot fixture supplies locked provider and parameter mocks.
    prelude = PRELUDE[:PRELUDE.index("static unsigned dirty_calls;")]
    code = prelude + MOCKS + structure(selector_h, "kp_rgb_selector") + "\n"
    code += sync_h[sync_h.index("#define RGB_SELECTOR_COLOR_CMD"):sync_h.index("/* Caller holds")]
    code += "\n".join(function(selector, n) for n in ("kp_rgb_selector_at", "kp_rgb_selector_member_slot"))
    code += "\n".join(function(sync, n) for n in ("kp_rgb_selector_snapshot_locked", "kp_rgb_selector_snapshot_binding", "kp_rgb_selector_member_apply_locked"))
    code += SCOPED_MOCKS[:SCOPED_MOCKS.index("static struct zmk_behavior_binding sent_binding;")]
    code += TRANSPORT
    code += "\n".join(re.search(r"enum " + n + r" \{.*?\};", split, re.S).group() for n in ("kp_rgb_sync_phase", "kp_rgb_sync_emit"))
    code += "\n".join(structure(split, n) for n in ("kp_rgb_global_snapshot", "kp_rgb_sync_selector", "kp_rgb_sync_selector_progress", "kp_rgb_sync_source"))
    code += r'''
static struct kp_rgb_sync_selector kp_rgb_sync_selectors[3];
static struct kp_rgb_sync_source kp_rgb_sync_sources[2];
static const struct zmk_split_transport_central *kp_rgb_sync_transport;
static struct k_work_delayable kp_rgb_sync_poll,kp_rgb_sync_step,kp_rgb_sync_clock,kp_rgb_sync_selector_step;
static size_t kp_rgb_sync_selector_cursor;
'''
    # Constants are extracted rather than copied into the scheduler fixture.
    code += "\n".join(re.findall(r"^#define KP_RGB_SYNC_(?:STEP_DELAY_MS|MAX_BACKOFF_MS|BACKOFF_SHIFT_MAX) .*$", split, re.M)) + "\n"
    code += "\n".join(function(split, n) for n in (
        "kp_rgb_sync_selector_count", "kp_rgb_split_selector_dirty_locked", "kp_rgb_split_selectors_dirty_locked",
        "kp_rgb_sync_selectors_reset", "kp_rgb_sync_selectors_refresh", "kp_rgb_sync_refresh",
        "kp_rgb_sync_send_binding", "kp_rgb_split_send_selector", "kp_rgb_sync_selector_step_handler",
        "kp_rgb_sync_poll_handler", "kp_rgb_split_sync_init"))
    code += function(control, "kp_rgb_selector_binding_apply") + TESTS
    return code


def main():
    with tempfile.TemporaryDirectory() as d:
        source = Path(d) / "scoped_reconnect.c"
        binary = Path(d) / "scoped_reconnect"
        source.write_text(fixture())
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
            str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    main()
