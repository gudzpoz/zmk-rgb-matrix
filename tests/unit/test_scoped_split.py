#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Exercise scoped selector split snapshots and qualified member updates."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from test_singleton import ROOT, function, structure

PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define LOG_WRN(...) ((void)0)
#define KP_RGB_HUE_MAX 360
#define KP_RGB_SAT_MAX 100
#define KP_RGB_BRT_MAX 100
#define CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS 100
#define CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS 10000
#define RGB_COLOR_HSB_VAL(h,s,b) (((uint32_t)(h)<<16)|((uint32_t)(s)<<8)|(b))
#define RGB_EFS_CMD 13u
#define RGB_RESET_CMD 0x102u
struct device { const char *name; const void *config; void *data; bool ready; };
struct kp_rgb_hsb { uint16_t h; uint8_t s,b; };
struct kp_rgb_effect_common_config { const struct device *owner; uint16_t slot; };
struct kp_rgb_effect_common_data { struct kp_rgb_hsb color; uint32_t duration_ms; };
struct zmk_behavior_binding { const char *behavior_dev; uint32_t param1,param2; };
struct zmk_behavior_binding_event { uint8_t layer,position; int64_t timestamp; uint8_t source; };
static int zmk_split_central_invoke_behavior(uint8_t,struct zmk_behavior_binding *,struct zmk_behavior_binding_event,bool);
static int64_t k_uptime_get(void);
struct kp_rgb_selector_snapshot;
int kp_rgb_split_send_selector(uint8_t,const struct kp_rgb_selector_snapshot *,uint8_t);
static unsigned held,locks,unlocks,flushes,saves,io_calls;
static bool isr;
static bool k_is_in_isr(void) { return isr; }
static bool device_is_ready(const struct device *d) { return d && d->ready; }
static void kp_rgb_matrix_lock(void) { assert(!held && !isr); held=1; locks++; }
static void kp_rgb_matrix_unlock(void) { assert(held); held=0; unlocks++; }
static void zmk_rgb_matrix_flush(void) { assert(held); flushes++; }
static int kp_rgb_save_selector_state(const struct device *d,bool reset) {
 assert(!held && d); (void)reset; saves++; return 0;
}
static struct kp_rgb_effect_common_data *kp_rgb_effect_data(const struct device *d) {
 assert(held && d); return d->data;
}
static const struct kp_rgb_effect_common_config *kp_rgb_effect_cfg(const struct device *d) {
 assert(held && d); return d->config;
}
static struct kp_rgb_selector *kp_rgb_selector_overlay_state_locked(const struct device *);
static const struct device *kp_rgb_selector_control(const struct device *);
static const struct device *kp_rgb_selector_at(const struct kp_rgb_selector *,size_t);
static int kp_rgb_selector_member_slot(const struct kp_rgb_selector *,const struct device *);
static int kp_rgb_set_color_locked(const struct device *,const struct kp_rgb_hsb *);
static int kp_rgb_set_period_locked(const struct device *,uint32_t);
'''

TESTS = r'''
struct selector_data { struct kp_rgb_selector selector; bool ready; };
static struct device owners[2], controls[2], effects[5];
static struct selector_data states[2];
static const struct device *members_a[]={&effects[0],NULL,&effects[1]};
static const struct device *members_b[]={&effects[2],&effects[3]};
static struct kp_rgb_effect_common_config configs[5];
static struct kp_rgb_effect_common_data values[5];
static struct kp_rgb_selector *kp_rgb_selector_overlay_state_locked(const struct device *d) {
 assert(held);
 for (size_t i=0;i<2;i++) if (d==&owners[i] && states[i].ready) return &states[i].selector;
 return NULL;
}
static const struct device *kp_rgb_selector_control(const struct device *d) {
 for(size_t i=0;i<2;i++) if(d==&owners[i]) return &controls[i];
 return NULL;
}
static int kp_rgb_set_color_locked(const struct device *fx,const struct kp_rgb_hsb *c) {
 assert(held && device_is_ready(fx));
 struct kp_rgb_effect_common_data *d=kp_rgb_effect_data(fx);
 if (memcmp(&d->color,c,sizeof(*c))) { d->color=*c; flushes++; }
 return 0;
}
static int kp_rgb_set_period_locked(const struct device *fx,uint32_t p) {
 assert(held && device_is_ready(fx));
 if(p<100 || p>10000) return -EINVAL;
 struct kp_rgb_effect_common_data *d=kp_rgb_effect_data(fx);
 if(d->duration_ms!=p) { d->duration_ms=p; flushes++; }
 return 0;
}
static int kp_rgb_selector_dispatch_locked(struct kp_rgb_selector *s,uint32_t c,uint32_t a) {
 return kp_rgb_selector_member_apply_locked(s,c,a);
}
static int kp_rgb_selector_overlay_reset_locked(const struct device *d) { (void)d; return -ENOTSUP; }
static struct zmk_behavior_binding sent_binding;
static struct zmk_behavior_binding_event sent_event;
static int send_result;
static int64_t send_now;
static int64_t k_uptime_get(void) { return send_now; }
static int zmk_split_central_invoke_behavior(uint8_t source,struct zmk_behavior_binding *binding,
                                             struct zmk_behavior_binding_event event,bool pressed) {
 assert(!held && pressed); (void)source; sent_binding=*binding; sent_event=event; return send_result;
}
'''


def main():
    selector_h = (ROOT / "src/rgb_selector_internal.h").read_text()
    sync_h = (ROOT / "src/rgb_selector_sync_internal.h").read_text()
    sync_c = (ROOT / "src/rgb_selector_sync.c").read_text()
    split_c = (ROOT / "src/rgb_split_sync.c").read_text()
    selector_c = (ROOT / "src/rgb_selector.c").read_text()
    behavior = (ROOT / "src/behaviors/selector.c").read_text()
    sync_api = sync_h[sync_h.index("#define RGB_SELECTOR_COLOR_CMD"):sync_h.index("/* Caller holds")]
    wrapper = function(behavior, "kp_rgb_selector_binding_apply")
    code = PRELUDE + structure(selector_h, "kp_rgb_selector") + "\n" + sync_api
    # The common config/data layouts above are the narrow host definitions.
    code += "\n".join(function(selector_c, n) for n in ("kp_rgb_selector_at", "kp_rgb_selector_member_slot"))
    code += "\n".join(function(sync_c, n) for n in ("kp_rgb_selector_snapshot_locked", "kp_rgb_selector_snapshot_binding", "kp_rgb_selector_member_apply_locked"))
    code += TESTS
    code += "\n".join(function(split_c, n) for n in ("kp_rgb_sync_send_binding", "kp_rgb_split_send_selector"))
    code += wrapper
    code += r'''
static void setup(void) {
 memset(states,0,sizeof(states)); memset(values,0,sizeof(values));
 memset(&held,0,sizeof(held)); locks=unlocks=flushes=saves=io_calls=0; isr=false;
 static const char *names[]={"a0","a2","b0","b1","unused"};
 for(size_t i=0;i<2;i++) {
  owners[i]=(struct device){.name=i?"owner-b":"owner-a",.ready=true};
  controls[i]=(struct device){.name=i?"control-b":"control-a",.ready=true};
 }
 for(size_t i=0;i<5;i++) {
  const struct device *owner=i<2?&owners[0]:i<4?&owners[1]:NULL;
  uint16_t slot=i==1?2:i==3?1:0;
  configs[i]=(struct kp_rgb_effect_common_config){owner,slot};
  values[i]=(struct kp_rgb_effect_common_data){{(uint16_t)(100+i),50,100},(uint32_t)(500+i)};
  effects[i]=(struct device){names[i],&configs[i],&values[i],true};
 }
 states[0].selector=(struct kp_rgb_selector){.owner=&owners[0],.effects=members_a,.effect_count=3,.effect_index=0,.active_fx=&effects[0]};
 states[1].selector=(struct kp_rgb_selector){.owner=&owners[1],.effects=members_b,.effect_count=2,.effect_index=0,.active_fx=&effects[2]};
 states[0].ready=states[1].ready=true;
}
static struct kp_rgb_selector_snapshot snapshot(unsigned i) {
 struct kp_rgb_selector_snapshot s;
 kp_rgb_matrix_lock(); assert(kp_rgb_selector_snapshot_locked(&owners[i],&s)==0); kp_rgb_matrix_unlock();
 return s;
}
static void test_snapshot_binding_and_frozen_data(void) {
 setup();
 struct kp_rgb_selector_snapshot a=snapshot(0),b=snapshot(1);
 assert(a.control==&controls[0] && a.slot==0 && a.color.h==100 && a.period_ms==500);
 assert(b.control==&controls[1] && b.slot==0 && b.color.h==102 && b.period_ms==502);
 struct zmk_behavior_binding bind;
 assert(kp_rgb_selector_snapshot_binding(&a,0,&bind)==0 && !strcmp(bind.behavior_dev,"control-a"));
 assert(bind.param1==KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD,0) && bind.param2==RGB_COLOR_HSB_VAL(100,50,100));
 assert(kp_rgb_selector_snapshot_binding(&a,1,&bind)==0 && bind.param1==KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD,0) && bind.param2==500);
 assert(kp_rgb_selector_snapshot_binding(&a,2,&bind)==0 && bind.param1==RGB_EFS_CMD && bind.param2==0);
 assert(kp_rgb_selector_snapshot_binding(&a,3,&bind)==-EINVAL);
 /* Switch active member and alter live values after capture: the old snapshot stays fixed. */
 kp_rgb_matrix_lock(); states[0].selector.active_fx=&effects[1]; states[0].selector.effect_index=2;
 values[0].color=(struct kp_rgb_hsb){250,10,20}; values[0].duration_ms=9000; kp_rgb_matrix_unlock();
 assert(kp_rgb_selector_snapshot_binding(&a,0,&bind)==0 && bind.param2==RGB_COLOR_HSB_VAL(100,50,100));
 assert(kp_rgb_selector_snapshot_binding(&a,1,&bind)==0 && bind.param2==500);
 assert(kp_rgb_selector_snapshot_binding(&b,0,&bind)==0 && bind.param2==RGB_COLOR_HSB_VAL(102,50,100));
 assert(locks==unlocks && !held && io_calls==0);
}
static void test_sender_transport_contract(void) {
 setup(); struct kp_rgb_selector_snapshot saved=snapshot(0), before=saved;
 send_now=765; send_result=-EIO;
 assert(kp_rgb_split_send_selector(1,&saved,0)==-EIO);
 assert(!held && sent_binding.behavior_dev==controls[0].name);
 assert(sent_binding.param1==KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD,0));
 assert(sent_binding.param2==RGB_COLOR_HSB_VAL(100,50,100));
 assert(sent_event.layer==0 && sent_event.position==0 && sent_event.timestamp==765 && sent_event.source==1);
 assert(!memcmp(&saved,&before,sizeof(saved)));
 send_result=0;
 assert(kp_rgb_split_send_selector(0,&saved,1)==0);
 assert(sent_binding.behavior_dev==controls[0].name);
 assert(sent_binding.param1==KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD,0));
 assert(sent_binding.param2==500 && sent_event.source==0 && !held);
}
static void test_addressed_apply_and_validation(void) {
 setup(); struct kp_rgb_selector *s=&states[0].selector;
 struct kp_rgb_hsb before=values[0].color;
 uint32_t color=RGB_COLOR_HSB_VAL(220,70,100);
 kp_rgb_matrix_lock(); assert(kp_rgb_selector_member_apply_locked(s,KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD,2),color)==0); kp_rgb_matrix_unlock();
 assert(values[1].color.h==220 && values[0].color.h==before.h && s->active_fx==&effects[0]);
 kp_rgb_matrix_lock(); assert(kp_rgb_selector_member_apply_locked(s,KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD,2),1200)==0); kp_rgb_matrix_unlock();
 assert(values[1].duration_ms==1200 && values[0].duration_ms==500 && s->effect_index==0);
 assert(kp_rgb_selector_binding_apply(&owners[0],KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD,2),1300)==0);
 assert(values[1].duration_ms==1300 && saves==1 && !held);
 unsigned saved=saves;
 kp_rgb_matrix_lock(); int ret=kp_rgb_selector_member_apply_locked(s,KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD,2),1300); kp_rgb_matrix_unlock();
 assert(ret==0 && saves==saved);
 const uint32_t badcmd[]={KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD,UINT16_MAX),KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD,UINT16_MAX),0x10a};
 for(size_t i=0;i<sizeof(badcmd)/sizeof(badcmd[0]);i++) { kp_rgb_matrix_lock(); ret=kp_rgb_selector_member_apply_locked(s,badcmd[i],100); kp_rgb_matrix_unlock(); assert(ret<0); }
 const uint32_t badcolor[]={RGB_COLOR_HSB_VAL(361,1,100),RGB_COLOR_HSB_VAL(1,101,100),RGB_COLOR_HSB_VAL(1,1,101),UINT32_MAX};
 for(size_t i=0;i<sizeof(badcolor)/sizeof(badcolor[0]);i++) { kp_rgb_matrix_lock(); ret=kp_rgb_selector_member_apply_locked(s,KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD,0),badcolor[i]); kp_rgb_matrix_unlock(); assert(ret==-EINVAL); }
 const uint32_t badperiod[]={0,99,10001,UINT32_MAX};
 for(size_t i=0;i<sizeof(badperiod)/sizeof(badperiod[0]);i++) { kp_rgb_matrix_lock(); ret=kp_rgb_selector_member_apply_locked(s,KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD,0),badperiod[i]); kp_rgb_matrix_unlock(); assert(ret==-EINVAL); }
 kp_rgb_matrix_lock(); ret=kp_rgb_selector_member_apply_locked(s,KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD,1),color); kp_rgb_matrix_unlock(); assert(ret==-ENOENT);
 effects[1].ready=false; kp_rgb_matrix_lock(); ret=kp_rgb_selector_member_apply_locked(s,KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD,2),color); kp_rgb_matrix_unlock(); assert(ret==-ENODEV);
 states[0].ready=false; kp_rgb_matrix_lock(); ret=kp_rgb_selector_snapshot_locked(&owners[0],&(struct kp_rgb_selector_snapshot){0}); kp_rgb_matrix_unlock(); assert(ret==-ENODEV);
 controls[0].ready=false; kp_rgb_matrix_lock(); ret=kp_rgb_selector_snapshot_locked(&owners[0],&(struct kp_rgb_selector_snapshot){0}); kp_rgb_matrix_unlock(); assert(ret==-ENODEV);
 assert(io_calls==0 && !held && locks==unlocks);
}
int main(void) { test_snapshot_binding_and_frozen_data(); test_addressed_apply_and_validation(); test_sender_transport_contract(); puts("scoped split snapshots, member commands and transport passed"); }
'''
    with tempfile.TemporaryDirectory() as d:
        source=Path(d)/"scoped_split.c"; binary=Path(d)/"scoped_split"; source.write_text(code)
        subprocess.run(shlex.split(os.environ.get("CC","cc"))+["-std=gnu11","-Wall","-Wextra","-Werror","-Wno-unused-function",str(source),"-o",str(binary)],check=True)
        subprocess.run([str(binary)],check=True,timeout=10)

if __name__ == "__main__": main()
