#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Frozen global/reset repairs and completion deadlines using production workers."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
from test_singleton import ROOT, function
from test_singleton_split import fixture

MODEL = r'''
static uint16_t colors[2][3], remote_colors[2][3];
static uint32_t periods[2][3], remote_periods[2][3];
static unsigned remote_selection[2];
static bool remote_on;
static unsigned remote_brightness;
static int inject_global;
void kp_rgb_split_global_dirty_locked(void);
static int snapshot_member(const struct device *owner, unsigned slot,
                           struct kp_rgb_selector_snapshot *s) {
 assert(held); unsigned i=owner-owners; captures[i]++;
 *s=(struct kp_rgb_selector_snapshot){.control=owner,.slot=slot,
  .color={colors[i][slot],100,100},.period_ms=periods[i][slot]};
 return 0;
}
'''
SNAPSHOT = r'''
static int kp_rgb_selector_snapshot_locked(const struct device *owner,
 struct kp_rgb_selector_snapshot *s) {
 return snapshot_member(owner,selected_slots[owner-owners],s);
}
static int kp_rgb_selector_next_snapshot_locked(const struct device *owner,
 size_t *cursor, struct kp_rgb_selector_snapshot *s) {
 if (*cursor==3) return 0;
 snapshot_member(owner,(*cursor)++,s); return 1;
}
'''
SEND = r'''
static int zmk_split_central_invoke_behavior(uint8_t source, struct zmk_behavior_binding *b,
 struct zmk_behavior_binding_event event, bool pressed) {
 assert(!held && pressed && event.timestamp==now_ms);
 assert(sends<ARRAY_SIZE(bindings));
 bindings[sends]=*b; destinations[sends++]=source; now_ms+=send_cost;
 if (inject_global) {
  inject_global=0;
  kp_rgb_matrix_lock(); kp_rgb_global.user_on=false; kp_rgb_global.brightness=19;
  remote_on=false; remote_brightness=19;
  kp_rgb_split_global_dirty_locked(); kp_rgb_matrix_unlock();
 }
 if(fail_source[source]) return -EIO;
 if(!strcmp(b->behavior_dev,"global")) {
  if(b->param1==RGB_ON_CMD) remote_on=true;
  if(b->param1==RGB_OFF_CMD) remote_on=false;
  if(b->param1==RGB_BRT_SET_CMD) remote_brightness=b->param2;
 } else {
  unsigned i=!strcmp(b->behavior_dev,owners[0].name)?0:1;
  unsigned slot=b->param1>>16;
  switch(b->param1&0xffff) {
  case RGB_SELECTOR_COLOR_CMD: assert(slot<3); remote_colors[i][slot]=b->param2>>16; break;
  case RGB_SELECTOR_PERIOD_CMD: assert(slot<3); remote_periods[i][slot]=b->param2; break;
  case RGB_EFS_CMD: remote_selection[i]=b->param2; break;
  default: assert(false);
  }
 }
 return 0;
}
'''
TESTS = r'''
static void setup(void) {
 memset(kp_rgb_sync_sources,0,sizeof(kp_rgb_sync_sources));
 memset(remote_colors,0,sizeof(remote_colors)); memset(remote_periods,0,sizeof(remote_periods));
 for(unsigned i=0;i<2;i++) {
  kp_rgb_sync_selectors[i].revision=kp_rgb_sync_selectors[i].reset_generation=0;
  selected_slots[i]=2;
  for(unsigned j=0;j<3;j++) { colors[i][j]=100+j; periods[i][j]=1000+j; }
 }
 fail_source[0]=fail_source[1]=false; inject_global=0;
 kp_rgb_global_revision=0; kp_rgb_global.user_on=true; kp_rgb_global.brightness=87;
 remote_on=true; remote_brightness=87;
 kp_rgb_sync_transport=NULL; selected_transport=&transports[0];
 available_count=1; ids[0]=0; now_ms=0; send_cost=0; sends=0;
 kp_rgb_sync_selector_cursor=0; kp_rgb_sync_refresh(); now_ms=100;
}
static void global_change(bool reset) {
 kp_rgb_matrix_lock(); kp_rgb_global.user_on=false; kp_rgb_global.brightness=19;
 remote_on=false; remote_brightness=19; /* ordinary command accepted before stale repair */
 kp_rgb_split_global_dirty_locked();
 if(reset) kp_rgb_split_selectors_dirty_locked();
 kp_rgb_matrix_unlock();
}
static void drain_global(void) {
 for(unsigned i=0;i<30;i++) { now_ms+=50; kp_rgb_sync_step_handler(NULL); }
 assert(!kp_rgb_sync_sources[0].pending);
 assert(!remote_on && remote_brightness==19);
}
static void reset_selector(bool all) {
 kp_rgb_matrix_lock();
 for(unsigned i=0;i<(all?2u:1u);i++) {
  for(unsigned j=0;j<3;j++) {
   colors[i][j]=20+j; periods[i][j]=200+j;
   remote_colors[i][j]=20+j; remote_periods[i][j]=200+j;
  }
  selected_slots[i]=0; remote_selection[i]=0;
 }
 if(all) kp_rgb_split_selectors_dirty_locked();
 else kp_rgb_split_selector_reset_locked(&owners[0]);
 kp_rgb_matrix_unlock();
}
static void drain_selectors(void) {
 for(unsigned n=0;n<120;n++) { now_ms+=50; kp_rgb_sync_selector_step_handler(NULL); }
 for(unsigned i=0;i<2;i++) assert(!kp_rgb_sync_sources[0].selectors[i].pending);
}
static void check_members(unsigned i) {
 for(unsigned j=0;j<3;j++) {
  assert(remote_colors[i][j]==colors[i][j]);
  assert(remote_periods[i][j]==periods[i][j]);
 }
 assert(remote_selection[i]==selected_slots[i]);
 assert(kp_rgb_sync_sources[0].selectors[i].completed_reset_generation==
        kp_rgb_sync_selectors[i].reset_generation);
}
int main(void) {
 /* Mutations at each global phase, both before and after failed transport. */
 for(unsigned phase=0;phase<5;phase++) for(unsigned fail=0;fail<2;fail++) {
  setup();
  for(unsigned n=0;n<phase;n++) { kp_rgb_sync_step_handler(NULL); now_ms+=50; }
  fail_source[0]=fail; kp_rgb_sync_step_handler(NULL);
  global_change(phase%2); fail_source[0]=false; drain_global();
 }
 setup(); inject_global=1; kp_rgb_sync_step_handler(NULL); drain_global();
 /* Poll wakes cannot bypass completion-based success/backoff deadlines. */
 for(unsigned fail=0;fail<2;fail++) {
  setup(); send_cost=100; fail_source[0]=fail; kp_rgb_sync_step_handler(NULL);
  assert(now_ms==200 && kp_rgb_sync_sources[0].ready_at==250);
  unsigned before=sends;
  now_ms=249; kp_rgb_sync_poll_handler(NULL); kp_rgb_sync_step_handler(NULL); assert(sends==before);
  assert(scheduled_work==&kp_rgb_sync_step && scheduled_delay==1);
  now_ms=250; kp_rgb_sync_step_handler(NULL); assert(sends==before+1);
  assert(kp_rgb_sync_sources[0].ready_at==350+(fail?100:50));
 }
 /* Reset while old color, period or selection is frozen/retrying. */
 for(unsigned phase=0;phase<3;phase++) for(unsigned fail=0;fail<2;fail++) {
  setup();
  struct kp_rgb_sync_selector_progress *st=&kp_rgb_sync_sources[0].selectors[0];
  for(unsigned n=0;n<30 && st->step<phase;n++) { kp_rgb_sync_selector_step_handler(NULL); now_ms+=50; }
  kp_rgb_sync_selector_cursor=0; fail_source[0]=fail;
  kp_rgb_sync_selector_step_handler(NULL);
  reset_selector(true); fail_source[0]=false; drain_selectors();
  check_members(0); check_members(1);
 }
 /* New reset and normal revisions at every sweep send: inactive members matter. */
 for(unsigned phase=0;phase<7;phase++) for(unsigned again=0;again<2;again++) {
  setup(); drain_selectors(); reset_selector(false);
  struct kp_rgb_sync_selector_progress *st=&kp_rgb_sync_sources[0].selectors[0];
  for(unsigned n=0;n<phase;n++) { now_ms+=50; kp_rgb_sync_selector_step_handler(NULL); }
  fail_source[0]=true; now_ms+=50; kp_rgb_sync_selector_step_handler(NULL);
  if(again) reset_selector(false);
  kp_rgb_matrix_lock();
  colors[0][0]=311; periods[0][0]=7311;
  remote_colors[0][0]=311; remote_periods[0][0]=7311;
  selected_slots[0]=2; remote_selection[0]=2;
  kp_rgb_split_selector_dirty_locked(&owners[0]); kp_rgb_matrix_unlock();
  assert(st->completed_reset_generation!=kp_rgb_sync_selectors[0].reset_generation);
  fail_source[0]=false; drain_selectors(); check_members(0);
 }
 setup(); drain_selectors(); unsigned before=sends;
 colors[0][0]=333; kp_rgb_global.brightness=7;
 for(unsigned n=0;n<10;n++) { now_ms+=50; kp_rgb_sync_selector_step_handler(NULL); }
 assert(sends==before); /* local-only state does not dirty selectors */
 puts("global revision interleavings, reset generations/all members and completion deadlines passed");
}
'''

def main():
    code = fixture()
    header = (ROOT / 'src/rgb_selector_sync_internal.h').read_text()
    definitions = header[header.index('#define RGB_SELECTOR_COLOR_CMD'):header.index('struct kp_rgb_selector_snapshot')]
    code = '#define RGB_EFS_CMD 12\n#define RGB_COLOR_HSB_VAL(h,s,b) (((uint32_t)(h)<<16)|((s)<<8)|(b))\n' + definitions + code
    old = function(code, 'zmk_split_central_invoke_behavior')
    code = code.replace(old, '')
    at = code.index('static int kp_rgb_selector_snapshot_locked')
    code = code[:at] + MODEL + SEND + code[at:]
    code = code.replace(function(code, 'kp_rgb_selector_snapshot_locked'), '')
    code = code.replace(function(code, 'kp_rgb_selector_next_snapshot_locked'), SNAPSHOT)
    source = (ROOT / 'src/rgb_selector_sync.c').read_text()
    code = code.replace(function(code, 'kp_rgb_selector_snapshot_binding'), function(source, 'kp_rgb_selector_snapshot_binding'))
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / 'repair.c'; path.write_text(code + TESTS)
        binary = Path(tmp) / 'repair'
        subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', str(path), '-o', str(binary)], check=True, timeout=20)
        subprocess.run([str(binary)], check=True, timeout=10)

if __name__ == '__main__':
    main()
