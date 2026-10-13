#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Host-execute Stage 3 controls, not replicas of their conversion algorithms.

DT generation is replaced only by two known devices/associations; native tests
cover its assertions. Explicit/missing aliases and central/peripheral dirty hooks run. Set
SELECTOR_CONTROLS_SANITIZE=1 to additionally enable ASan/UBSan.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

from test_singleton import function, structure

ROOT = Path(__file__).resolve().parents[2]

PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define ARG_UNUSED(x) ((void)(x))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define CLAMP(v,l,h) ((v)<(l)?(l):((v)>(h)?(h):(v)))
#define IS_ENABLED(x) (x)
#define CONFIG_SETTINGS 0
#define CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS 100
#define CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS 10000
#define CONFIG_KEYPAW_RGB_MATRIX_DURATION_STEP 500
#define CONFIG_KEYPAW_RGB_MATRIX_HUE_STEP 37
#define CONFIG_KEYPAW_RGB_MATRIX_SAT_STEP 10
#define CONFIG_KEYPAW_RGB_MATRIX_BRT_STEP 10
#define KP_RGB_HUE_MAX 360
#define KP_RGB_SAT_MAX 100
#define KP_RGB_BRT_MAX 100
#define ZMK_BEHAVIOR_OPAQUE 0
struct device { const char *name; const void *config; void *data; const void *api; bool ready; };
struct zmk_behavior_binding { const char *behavior_dev; uint32_t param1, param2; };
struct zmk_behavior_binding_event { int unused; };
struct kp_rgb_hsb { uint16_t h; uint8_t s,b; };
struct kp_rgb_tuning { uint8_t max_brightness,idle_brightness; };
struct kp_rgb_overlay_common_config { uint8_t opacity; };
struct kp_rgb_overlay_common_data { int unused; };
struct kp_rgb_key_event;
struct kp_rgb_frame;
struct kp_rgb_effect_callbacks {
 bool (*render)(const struct device *,const struct kp_rgb_frame *);
 bool (*on_event)(const struct device *,const struct kp_rgb_key_event *);
 void (*reset)(const struct device *,int64_t);
 void (*set_active)(const struct device *,bool,int64_t);
};
struct kp_rgb_effect_api { const struct kp_rgb_effect_callbacks *callbacks; };
struct kp_rgb_overlay_api { const void *condition; const struct kp_rgb_effect_callbacks *callbacks; bool replaces_target; };
struct kp_rgb_callback_state { int64_t last_render_ms; bool initialized,active,animating; };
struct kp_rgb_effect_instance { const struct device *effect; struct kp_rgb_callback_state state; };
static unsigned held,locks,unlocks,flushes,saves,power_reconciles,runtime_resets;
static unsigned callbacks,trace[64],trace_len;
static bool isr;
static int64_t uptime;
static struct device owners[2],controls[2],effects[5],flat;
static const struct device *alias;
static bool k_is_in_isr(void) { return isr; }
static bool device_is_ready(const struct device *dev) { return dev && dev->ready; }
static void kp_rgb_matrix_lock(void) { assert(!isr); held++; locks++; }
static void kp_rgb_matrix_unlock(void) { assert(held); held--; unlocks++; }
static void zmk_rgb_matrix_flush(void) { assert(held); flushes++; }
static int kp_rgb_save_state(void) { assert(!held); saves++; return 0; }
static unsigned scoped_saves;
static int kp_rgb_save_selector_state(const struct device *owner, bool reset) {
    assert(!held && owner); (void)reset; scoped_saves++; return 0;
}
static void kp_rgb_reconcile_power_locked(void) { assert(held); power_reconciles++; }
static void kp_rgb_request_runtime_reset_locked(void) { assert(held); runtime_resets++; }
static int64_t k_uptime_get(void) { return uptime; }
static void kp_rgb_set_clock_offset(int32_t offset) { (void)offset; }
static void kp_rgb_overlay_set_word(uint16_t word,uint16_t bits) { (void)word;(void)bits; }
static int zmk_rgb_matrix_on(void) { return 0; }
static int zmk_rgb_matrix_off(void) { return 0; }
static int zmk_rgb_matrix_toggle(void) { return 0; }
static const struct device *zmk_behavior_get_binding(const char *name) {
 assert(name);
 for(size_t i=0;i<ARRAY_SIZE(effects);i++) if(!strcmp(name,effects[i].name)) return &effects[i];
 for(size_t i=0;i<2;i++) if(!strcmp(name,controls[i].name)) return &controls[i];
 if(!strcmp(name,flat.name)) return &flat;
 return NULL;
}
/* Only expand the generated two-device lists; production lookup/reset bodies remain intact. */
#define DT_FOREACH_STATUS_OKAY(compat,fn) fn(0) fn(1)
#define DT_INST_FOREACH_STATUS_OKAY(fn) fn(0) fn(1)
#define DT_DRV_INST(inst) (inst)
#define DT_HAS_COMPAT_STATUS_OKAY(compat) 1
#define LOG_ERR(...) ((void)0)
#define DT_PHANDLE(node,prop) (node)
#define DEVICE_DT_GET(node) (&owners[node])
#define DT_NODE_HAS_PROP(node,prop) HAS_ALIAS
#define UNPAREN(...) __VA_ARGS__
#if HAS_ALIAS
#define COND_CODE_1(flag,yes,no) alias
#else
#define COND_CODE_1(flag,yes,no) UNPAREN no
#endif
'''

FIXTURE = r'''
static struct kp_rgb_effect_common_data parameters[5];
static struct kp_rgb_effect_common_config configs[5];
static const struct device *members_a[]={&effects[0],NULL,&effects[1]};
static const struct device *members_b[]={&effects[2],&effects[3]};
static const struct kp_rgb_effect_defaults defaults_a[]={{{10,20,1},0},{{0},0},{{50,60,2},700}};
static const struct kp_rgb_effect_defaults defaults_b[]={{{80,90,3},800},{{100,10,4},900}};
static struct kp_rgb_effect_instance instances_a[3],instances_b[2];
static bool pending_a[3],pending_b[2];
static struct kp_selector_data data_a,data_b;
static const struct kp_selector_control_config control_configs[2]={{&owners[0]},{&owners[1]}};
static const struct kp_selector_control_entry kp_selector_controls[]={
 {&owners[0],&controls[0]},{&owners[1],&controls[1]},{NULL,NULL}
};
static const struct kp_rgb_effect_callbacks kp_selector_callbacks={0};
static const struct kp_rgb_overlay_api owner_api={.callbacks=&kp_selector_callbacks};
static void child_reset(const struct device *dev,int64_t now) {
 assert(!held); assert(now==uptime); callbacks++; trace[trace_len++]=(unsigned)(dev-effects)*10+1;
}
static void child_active(const struct device *dev,bool active,int64_t now) {
 assert(!held); assert(now==uptime); callbacks++; trace[trace_len++]=(unsigned)(dev-effects)*10+(active?2:3);
}
static const struct kp_rgb_effect_callbacks child_callbacks={.reset=child_reset,.set_active=child_active};
static const struct kp_rgb_effect_api child_api={.callbacks=&child_callbacks};
static struct kp_rgb_global kp_rgb_global;
static struct kp_rgb_effect_common_data *kp_rgb_effect_data(const struct device *dev) {
 assert(held); assert(dev && dev->data); return dev->data;
}
static const struct kp_rgb_effect_common_config *kp_rgb_effect_cfg(const struct device *dev) {
 assert(held); return dev->config;
}
static uint32_t kp_rgb_effect_period(const struct device *dev) { return kp_rgb_effect_data(dev)->duration_ms; }
int kp_rgb_selector_binding_apply(const struct device *,uint32_t,uint32_t);
int kp_rgb_selector_member_apply_locked(struct kp_rgb_selector *,uint32_t,uint32_t);
int kp_rgb_selector_binding_convert(const struct device *,struct zmk_behavior_binding *);
struct kp_rgb_selector *kp_rgb_selector_overlay_state_locked(const struct device *);
const struct device *kp_rgb_selector_control(const struct device *);
int kp_rgb_selector_dispatch_locked(struct kp_rgb_selector *,uint32_t,uint32_t);
void kp_rgb_reset_state(void);
static unsigned dirty[2];
#if CENTRAL_SYNC
static void kp_rgb_split_selector_dirty_locked(const struct device *owner) {
 assert(held); assert(owner==&owners[0] || owner==&owners[1]); dirty[owner==&owners[1]]++;
}
static void kp_rgb_split_selectors_dirty_locked(void) {
 assert(held); kp_rgb_split_selector_dirty_locked(&owners[0]); kp_rgb_split_selector_dirty_locked(&owners[1]);
}
#endif
'''

TESTS = r'''
static const struct zmk_behavior_binding_event event={0};
static void setup(void) {
 assert(!held && locks==unlocks);
 held=locks=unlocks=flushes=saves=power_reconciles=runtime_resets=callbacks=trace_len=0;
 isr=false; uptime=123;
 memset(parameters,0,sizeof(parameters)); memset(instances_a,0,sizeof(instances_a));
 memset(instances_b,0,sizeof(instances_b)); memset(pending_a,0,sizeof(pending_a)); memset(pending_b,0,sizeof(pending_b));
 members_a[0]=&effects[0]; members_a[1]=NULL; members_a[2]=&effects[1];
 members_b[0]=&effects[2]; members_b[1]=&effects[3];
 for(size_t i=0;i<2;i++) {
   owners[i]=(struct device){.name=i?"owner-b":"owner-a",.data=i?(void *)&data_b:(void *)&data_a,.api=&owner_api,.ready=true};
   controls[i]=(struct device){.name=i?"ctrl-b":"ctrl-a",.config=&control_configs[i],.ready=true};
 }
 const char *names[]={"a0","a2","b0","b1","private"};
 for(size_t i=0;i<5;i++) {
   configs[i]=(struct kp_rgb_effect_common_config){.owner=i<2?&owners[0]:i<4?&owners[1]:NULL,.slot=i==1?2:i==3?1:0};
   effects[i]=(struct device){.name=names[i],.config=&configs[i],.data=&parameters[i],.api=&child_api,.ready=true};
   parameters[i]=(struct kp_rgb_effect_common_data){.color={350,95,100},.duration_ms=1000};
 }
 instances_a[0].effect=&effects[0]; instances_a[2].effect=&effects[1];
 instances_b[0].effect=&effects[2]; instances_b[1].effect=&effects[3];
 data_a=(struct kp_selector_data){.selector={.owner=&owners[0],.effects=members_a,.effect_defaults=defaults_a,.effect_count=3,.active_fx=&effects[0],.initial_duration_ms=600,.initial_effect=0},.members=members_a,.instances=instances_a,.reset_pending=pending_a,.ready=true,.live_slot=SIZE_MAX};
 data_b=(struct kp_selector_data){.selector={.owner=&owners[1],.effects=members_b,.effect_defaults=defaults_b,.effect_count=2,.active_fx=&effects[2],.initial_duration_ms=650,.initial_effect=1},.members=members_b,.instances=instances_b,.reset_pending=pending_b,.ready=true,.live_slot=SIZE_MAX};
 flat=(struct device){.name="flat",.ready=true}; alias=&owners[1];
 kp_rgb_global=(struct kp_rgb_global){.brightness=17,.user_on=true,.initial_on=false,.initial_brightness=29,.tuning={80,20}};
}
static int apply(unsigned owner,uint32_t cmd,uint32_t arg) {
 struct zmk_behavior_binding binding={controls[owner].name,cmd,arg};
 return kp_selector_pressed(&binding,event);
}
static void roundtrip(unsigned owner,uint32_t cmd,uint32_t absolute_cmd,uint32_t value) {
 unsigned scoped_before=scoped_saves;
 struct zmk_behavior_binding binding={controls[owner].name,cmd,0};
 struct kp_rgb_effect_common_data before[5]; memcpy(before,parameters,sizeof(before));
 size_t index=data_a.selector.effect_index, index_b=data_b.selector.effect_index;
 unsigned f=flushes,c=callbacks,l=locks,s=saves;
 assert(kp_selector_convert(&binding,event)==0);
 assert(binding.behavior_dev==controls[owner].name && binding.param1==(absolute_cmd==RGB_COLOR_HSB_CMD?KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD,0):absolute_cmd==RGB_SPI_CMD?KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD,0):absolute_cmd) && binding.param2==value);
 assert(!memcmp(before,parameters,sizeof(before)) && data_a.selector.effect_index==index && data_b.selector.effect_index==index_b);
 assert(flushes==f && callbacks==c && saves==s && locks==l+1 && !held);
 /* Simulate diverged peripheral parameters; receive the converted binding without converting again. */
 struct kp_rgb_selector *state=owner?&data_b.selector:&data_a.selector;
 struct kp_rgb_effect_common_data *target=state->active_fx->data;
 target->color=(struct kp_rgb_hsb){111,1,100}; target->duration_ms=7777;
 l=locks; assert(kp_selector_pressed(&binding,event)==0 && locks==l+1 && !held);
 if(absolute_cmd==RGB_COLOR_HSB_CMD) {
   assert(target->color.h==(value>>16) && target->color.s==((value>>8)&255) && target->color.b==100);
 } else if(absolute_cmd==RGB_SPI_CMD) assert(target->duration_ms==value);
 else assert(state->effect_index==value && state->active_fx==state->effects[value]);
 assert(saves==s && scoped_saves==scoped_before+1 && callbacks==c && power_reconciles==0 && runtime_resets==0);
 for(size_t i=0;i<ARRAY_SIZE(parameters);i++) {
   if(&parameters[i]!=target) assert(!memcmp(&parameters[i],&before[i],sizeof(parameters[i])));
 }
 assert(kp_rgb_global.brightness==17 && kp_rgb_global.user_on);
 assert(kp_rgb_global.tuning.max_brightness==80 && kp_rgb_global.tuning.idle_brightness==20);
 /* No forwarding symbol is provided: accidental forwarding also fails linking. */
}
static void test_conversion_application(void) {
 setup(); roundtrip(0,RGB_HUI_CMD,RGB_COLOR_HSB_CMD,RGB_COLOR_HSB_VAL(27,95,100));
 setup(); roundtrip(1,RGB_HUD_CMD,RGB_COLOR_HSB_CMD,RGB_COLOR_HSB_VAL(313,95,100));
 setup(); roundtrip(0,RGB_SAI_CMD,RGB_COLOR_HSB_CMD,RGB_COLOR_HSB_VAL(350,100,100));
 setup(); roundtrip(1,RGB_SAD_CMD,RGB_COLOR_HSB_CMD,RGB_COLOR_HSB_VAL(350,85,100));
 setup(); parameters[0].color.s=2; roundtrip(0,RGB_SAD_CMD,RGB_COLOR_HSB_CMD,RGB_COLOR_HSB_VAL(350,0,100));
 setup(); parameters[0].duration_ms=9999; roundtrip(0,RGB_SPI_CMD,RGB_SPI_CMD,10000);
 setup(); parameters[2].duration_ms=101; roundtrip(1,RGB_SPD_CMD,RGB_SPI_CMD,100);
 setup(); roundtrip(0,RGB_EFF_CMD,RGB_EFS_CMD,2);
 setup(); roundtrip(0,RGB_EFR_CMD,RGB_EFS_CMD,2);
 setup(); roundtrip(1,RGB_EFF_CMD,RGB_EFS_CMD,1);
 assert(kp_rgb_global.brightness==17);
}
static void test_arguments(void) {
 setup(); unsigned f=flushes;
 assert(apply(0,RGB_EFS_CMD,0)==0 && flushes==f);
 assert(apply(0,RGB_EFS_CMD,1)==-ENOENT);
 const uint32_t bad_slots[]={3,UINT16_MAX,65536,UINT32_MAX};
 for(size_t i=0;i<ARRAY_SIZE(bad_slots);i++) assert(apply(0,RGB_EFS_CMD,bad_slots[i])==-EINVAL);
 const uint32_t noarg[]={RGB_HUI_CMD,RGB_HUD_CMD,RGB_SAI_CMD,RGB_SAD_CMD,RGB_EFF_CMD,RGB_EFR_CMD,RGB_SPD_CMD,RGB_RESET_CMD};
 for(size_t i=0;i<ARRAY_SIZE(noarg);i++) {
   assert(apply(0,noarg[i],1)==-EINVAL);
   assert(apply(0,noarg[i],UINT32_MAX)==-EINVAL);
 }
 const uint32_t bad_periods[]={99,10001,65536+100,0x80000000u,UINT32_MAX};
 for(size_t i=0;i<ARRAY_SIZE(bad_periods);i++) assert(apply(0,RGB_SPI_CMD,bad_periods[i])==-EINVAL);
 assert(apply(0,RGB_SPI_CMD,100)==0); f=flushes;
 assert(apply(0,RGB_SPI_CMD,100)==0 && flushes==f);
 assert(apply(0,RGB_SPI_CMD,10000)==0);
 assert(apply(0,RGB_SPI_CMD,0)==0 && parameters[0].duration_ms==10000);
 assert(apply(0,RGB_COLOR_HSB_CMD,RGB_COLOR_HSB_VAL(360,100,0))==0);
 assert(parameters[0].color.h==0 && parameters[0].color.s==100 && parameters[0].color.b==100);
 f=flushes; assert(apply(0,RGB_COLOR_HSB_CMD,RGB_COLOR_HSB_VAL(0,100,42))==0 && flushes==f);
 const uint32_t bad_colors[]={RGB_COLOR_HSB_VAL(361,0,0),RGB_COLOR_HSB_VAL(0,101,0),RGB_COLOR_HSB_VAL(0,0,101),UINT32_MAX,0x80000000u};
 for(size_t i=0;i<ARRAY_SIZE(bad_colors);i++) assert(apply(0,RGB_COLOR_HSB_CMD,bad_colors[i])==-EINVAL);
 assert(apply(0,RGB_BRI_CMD,0)==-ENOTSUP && apply(0,UINT32_MAX,0)==-ENOTSUP);
 assert(flushes==f && callbacks==0 && saves==0 && power_reconciles==0);
 effects[1].ready=false;
 assert(apply(0,RGB_EFS_CMD,2)==-ENODEV);
 effects[0].ready=false; assert(apply(0,RGB_HUI_CMD,0)==-ENODEV);
 effects[0].ready=true; data_a.selector.active_fx=NULL;
 assert(apply(0,RGB_HUI_CMD,0)==-ENODEV && apply(0,RGB_SPI_CMD,100)==-ENODEV);
 assert(apply(0,RGB_EFS_CMD,0)==0 && data_a.selector.active_fx==&effects[0]);
 data_a.ready=false; assert(apply(0,RGB_EFS_CMD,0)==-ENODEV);
 assert(kp_rgb_selector_binding_apply(NULL,RGB_EFS_CMD,0)==-ENODEV);
 struct kp_rgb_overlay_api wrong_api={0}; struct device wrong={.ready=true,.api=&wrong_api};
 assert(kp_rgb_selector_binding_apply(&wrong,RGB_EFS_CMD,0)==-ENODEV);
}
static void direct(const struct device *effect,int result,const char *control,uint32_t command,uint32_t slot) {
 struct zmk_behavior_binding binding={effect->name,UINT32_MAX,UINT32_MAX},before=binding;
 unsigned f=flushes,c=callbacks;
 assert(kp_rgb_effect_convert_central_state_dependent_params(&binding,event)==result);
 if(result==0) assert(binding.behavior_dev==control && binding.param1==command && binding.param2==slot);
 else assert(binding.behavior_dev==before.behavior_dev && binding.param1==before.param1 && binding.param2==before.param2);
 assert(flushes==f && callbacks==c && !held);
}
static void test_associations(void) {
 setup(); direct(&effects[1],0,controls[0].name,RGB_EFS_CMD,2);
 direct(&effects[3],0,controls[1].name,RGB_EFS_CMD,1);
 direct(&effects[4],-ENODEV,NULL,0,0); /* Private effect has no selector owner. */
 configs[4].owner=&owners[0]; configs[4].slot=0;
 direct(&effects[4],-ENODEV,NULL,0,0); /* Owner metadata is insufficient without table identity. */
 configs[0].owner=&owners[1]; direct(&effects[0],-ENODEV,NULL,0,0);
 configs[0].owner=&owners[0]; configs[0].slot=UINT16_MAX; direct(&effects[0],-ENODEV,NULL,0,0);
 configs[0].slot=0; controls[0].ready=false; direct(&effects[0],-ENODEV,NULL,0,0);
 controls[0].ready=true; owners[0].ready=false; direct(&effects[0],-ENODEV,NULL,0,0);
 owners[0].ready=true; data_a.ready=false; direct(&effects[0],-ENODEV,NULL,0,0);
 data_a.ready=true;
 struct zmk_behavior_binding rejected={controls[0].name,RGB_HUI_CMD,0};
 controls[0].ready=false;
 assert(kp_selector_convert(&rejected,event)==-ENODEV && rejected.behavior_dev==controls[0].name);
 controls[0].ready=true;
 data_a.selector.active_fx=NULL;
 assert(kp_selector_convert(&rejected,event)==-ENODEV);
 data_a.selector.active_fx=&effects[0];
 effects[0].ready=false; direct(&effects[0],-ENODEV,NULL,0,0);
 effects[0].ready=true;
 struct device foreign={.name="foreign",.ready=true}; configs[0].owner=&foreign;
 direct(&effects[0],-ENODEV,NULL,0,0);
 /* Ready private/foreign effects still accept parameter APIs independently of membership. */
 struct kp_rgb_hsb color={360,40,0};
 assert(zmk_rgb_matrix_set_color(&effects[4],&color)==0 && parameters[4].color.h==0);
 assert(zmk_rgb_matrix_set_period(&effects[0],100)==0);
 assert(zmk_rgb_matrix_set_color(NULL,&color)==-ENODEV && zmk_rgb_matrix_set_period(NULL,100)==-ENODEV);
 assert(zmk_rgb_matrix_set_color(&effects[4],NULL)==-EINVAL);
 effects[4].ready=false; assert(zmk_rgb_matrix_set_period(&effects[4],100)==-ENODEV);
 configs[0].owner=&flat; configs[0].slot=0;
 direct(&effects[0],-ENODEV,NULL,0,0);
 assert(data_b.selector.effect_index==0);
}
static void test_alias_brightness(void) {
 setup();
 unsigned scoped_before=scoped_saves;
#if HAS_ALIAS
 assert(zmk_rgb_matrix_select_effect(1)==0 && data_b.selector.effect_index==1 && data_a.selector.effect_index==0);
 assert(scoped_saves==scoped_before);
 assert(zmk_rgb_matrix_cycle_effect(1)==0 && data_b.selector.effect_index==0);
 assert(scoped_saves==scoped_before);
 struct zmk_behavior_binding binding={flat.name,RGB_HUI_CMD,0};
 assert(on_keymap_binding_convert_central_state_dependent_params(&binding,event)==0);
 assert(binding.behavior_dev==controls[1].name && binding.param1==KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD,0));
 assert(kp_selector_pressed(&binding,event)==0 && parameters[2].color.h==27 && parameters[0].color.h==350);
 binding=(struct zmk_behavior_binding){flat.name,RGB_EFS_CMD,1};
 assert(on_keymap_binding_pressed(&binding,event)==0 && data_b.selector.effect_index==1);
 scoped_before=scoped_saves;
 owners[1].ready=false; assert(zmk_rgb_matrix_select_effect(0)==-ENODEV);
 assert(zmk_rgb_matrix_cycle_effect(1)==-ENODEV); owners[1].ready=true;
#else
 assert(zmk_rgb_matrix_select_effect(0)==-ENODEV && zmk_rgb_matrix_cycle_effect(1)==-ENODEV);
 const uint32_t commands[]={RGB_HUI_CMD,RGB_HUD_CMD,RGB_SAI_CMD,RGB_SAD_CMD,
                           RGB_SPI_CMD,RGB_SPD_CMD,RGB_EFS_CMD,RGB_EFF_CMD,RGB_EFR_CMD,RGB_COLOR_HSB_CMD};
 for(size_t i=0;i<ARRAY_SIZE(commands);i++) {
   struct zmk_behavior_binding binding={flat.name,commands[i],0},before=binding;
   unsigned f=flushes,s=scoped_saves;
   assert(on_keymap_binding_convert_central_state_dependent_params(&binding,event)==-ENODEV);
   assert(binding.behavior_dev==before.behavior_dev && binding.param1==before.param1 && binding.param2==before.param2);
   assert(on_keymap_binding_pressed(&binding,event)==-ENODEV && flushes==f && scoped_saves==s);
 }
#endif
 assert(scoped_saves==scoped_before);
 struct kp_rgb_effect_common_data before[5]; memcpy(before,parameters,sizeof(before));
 struct zmk_behavior_binding brt={flat.name,RGB_BRI_CMD,0};
 unsigned f=flushes,s=saves;
 assert(on_keymap_binding_convert_central_state_dependent_params(&brt,event)==0);
 assert(brt.param1==RGB_BRT_SET_CMD && brt.param2==27 && kp_rgb_global.brightness==17 && flushes==f);
 kp_rgb_global.brightness=99; /* Diverged peripheral. */
 assert(on_keymap_binding_pressed(&brt,event)==0 && kp_rgb_global.brightness==27 && saves==s+1);
 f=flushes; assert(on_keymap_binding_pressed(&brt,event)==0 && flushes==f);
 brt.param2=UINT32_MAX; assert(on_keymap_binding_pressed(&brt,event)==-EINVAL);
 assert(kp_rgb_change_brt(INT8_MAX)==0 && kp_rgb_global.brightness==100);
 assert(kp_rgb_change_brt(INT8_MIN)==0 && kp_rgb_global.brightness==0);
 assert(!memcmp(before,parameters,sizeof(before)));
 assert(kp_rgb_global.tuning.max_brightness==80 && kp_rgb_global.tuning.idle_brightness==20);
}
static void test_reset(void) {
 setup(); kp_selector_set_active(&owners[0],true,uptime);
 assert(callbacks==2); trace_len=0;
 assert(apply(0,RGB_EFS_CMD,2)==0); kp_selector_reconcile(&owners[0],uptime);
 assert(trace_len==3 && trace[0]==3 && trace[1]==11 && trace[2]==12);
 struct kp_rgb_effect_common_data b0=parameters[2],b1=parameters[3],priv=parameters[4];
 unsigned c=callbacks,f=flushes;
 assert(apply(0,RGB_RESET_CMD,0)==0);
 assert(callbacks==c && flushes==f+1 && data_a.reset_requested && data_a.live_slot==2);
 assert(parameters[0].color.h==10 && parameters[0].duration_ms==600 && parameters[1].duration_ms==700);
 assert(data_a.selector.effect_index==0 && data_a.selector.active_fx==&effects[0]);
 assert(!memcmp(&b0,&parameters[2],sizeof(b0)) && !memcmp(&b1,&parameters[3],sizeof(b1)) && !memcmp(&priv,&parameters[4],sizeof(priv)));
 assert(data_b.selector.effect_index==0 && !data_b.reset_requested && saves==0);
 trace_len=0; kp_selector_reconcile(&owners[0],uptime);
 assert(trace_len==3 && trace[0]==13 && trace[1]==1 && trace[2]==2);
 assert(!data_a.reset_requested && !pending_a[0] && pending_a[1] && pending_a[2]);
 kp_selector_set_active(&owners[0],false,uptime); c=callbacks;
 assert(apply(0,RGB_RESET_CMD,0)==0 && callbacks==c);
 kp_selector_reconcile(&owners[0],uptime); assert(callbacks==c && pending_a[0]);
 kp_selector_set_active(&owners[0],true,uptime); assert(callbacks==c+2);
 c=callbacks; unsigned r=runtime_resets;
 struct zmk_behavior_binding global={flat.name,RGB_RESET_CMD,0};
 assert(on_keymap_binding_pressed(&global,event)==0);
 assert(callbacks==c && data_a.reset_requested && data_b.reset_requested && runtime_resets==r+1);
 assert(parameters[2].color.h==80 && parameters[3].duration_ms==900 && data_b.selector.effect_index==1);
 assert(kp_rgb_global.brightness==29 && !kp_rgb_global.user_on && saves==0 && !held);
 global.param2=UINT32_MAX; assert(on_keymap_binding_pressed(&global,event)==-EINVAL);
 assert(callbacks==c && runtime_resets==r+1);
}
static void test_reset_startup_edges(void) {
 setup();
 /* A disabled initial middle slot falls back to first ready, not next sibling. */
 data_a.selector.initial_effect=1;
 assert(apply(0,RGB_EFS_CMD,2)==0 && data_a.selector.effect_index==2);
 unsigned c=callbacks;
 assert(apply(0,RGB_RESET_CMD,0)==0);
 assert(data_a.selector.effect_index==0 && data_a.selector.active_fx==&effects[0] && callbacks==c);
 data_a.selector.initial_effect=UINT16_MAX;
 assert(apply(0,RGB_RESET_CMD,0)==0 && data_a.selector.effect_index==0);
 /* Global reset also restores providers before selector startup; no selection/callback publication. */
 setup(); data_a.ready=false; data_a.prepared=false; data_a.selector.initial_effect=1;
 data_b.ready=false; data_b.prepared=true; data_b.selector.active_fx=NULL;
 members_a[0]=members_a[2]=members_b[0]=members_b[1]=NULL;
 effects[1].ready=false;
 struct kp_rgb_effect_common_data absent=parameters[1],priv=parameters[4];
 assert(apply(0,RGB_RESET_CMD,0)==-ENODEV && flushes==0);
 kp_rgb_reset_state();
 assert(data_a.prepared && data_b.prepared && !data_a.ready && !data_b.ready);
 assert(data_a.selector.effect_index==1 && data_b.selector.effect_index==1);
 assert(data_a.selector.active_fx==NULL && data_b.selector.active_fx==NULL);
 assert(data_a.reset_requested && data_b.reset_requested && callbacks==0 && saves==0);
 assert(members_a[0]==&effects[0] && members_a[2]==NULL && members_b[0]==&effects[2]);
 assert(parameters[0].color.h==10 && parameters[0].duration_ms==600 && parameters[2].duration_ms==800);
 assert(!memcmp(&absent,&parameters[1],sizeof(absent)) && !memcmp(&priv,&parameters[4],sizeof(priv)));
 assert(kp_selector_start(&owners[0])==0 && data_a.selector.effect_index==0);
 assert(kp_selector_start(&owners[1])==0 && data_b.selector.effect_index==1);
 assert(callbacks==0 && data_a.reset_requested && data_b.reset_requested);
 /* Late readiness applies its defaults at start without disturbing restored ready siblings. */
 data_a.ready=false; effects[1].ready=true; parameters[0].duration_ms=4321;
 assert(kp_selector_start(&owners[0])==0 && parameters[0].duration_ms==4321 && parameters[1].duration_ms==700);
 kp_selector_set_active(&owners[0],true,uptime);
 assert(callbacks==2 && !data_a.reset_requested && pending_a[2]);
 setup(); owners[0].ready=false; data_a.ready=false; data_a.prepared=false;
 kp_rgb_reset_state(); assert(!data_a.prepared && !data_a.reset_requested && data_b.reset_requested);
 assert(callbacks==0 && !held);
}
static void test_isr(void) {
 setup(); isr=true; unsigned l=locks;
 struct zmk_behavior_binding binding={controls[0].name,RGB_HUI_CMD,0};
 struct kp_rgb_hsb color={0};
 assert(kp_selector_convert(&binding,event)==-EWOULDBLOCK);
 assert(kp_selector_pressed(&binding,event)==-EWOULDBLOCK);
 assert(kp_rgb_selector_binding_apply(NULL,RGB_RESET_CMD,0)==-EWOULDBLOCK);
 assert(kp_rgb_selector_binding_convert(NULL,&binding)==-EWOULDBLOCK);
 binding.behavior_dev=effects[0].name;
 assert(kp_rgb_effect_convert_central_state_dependent_params(&binding,event)==-EWOULDBLOCK);
 assert(on_keymap_binding_convert_central_state_dependent_params(&binding,event)==-EWOULDBLOCK);
 assert(on_keymap_binding_pressed(&binding,event)==-EWOULDBLOCK);
 assert(zmk_rgb_matrix_select_effect(0)==-EWOULDBLOCK && zmk_rgb_matrix_cycle_effect(1)==-EWOULDBLOCK);
 assert(zmk_rgb_matrix_set_color(&effects[0],&color)==-EWOULDBLOCK);
 assert(zmk_rgb_matrix_set_period(&effects[0],100)==-EWOULDBLOCK);
 assert(kp_rgb_change_brt(1)==-EWOULDBLOCK);
 assert(locks==l && flushes==0 && callbacks==0 && saves==0 && !held); isr=false;
}
static void test_dirty_contract(void) {
 setup(); memset(dirty,0,sizeof(dirty));
 kp_rgb_global.user_on=false;
 assert(!data_a.active && !data_b.active); /* Hidden owners still accept control updates. */
 assert(apply(0,RGB_EFS_CMD,2)==0);
 assert(apply(0,RGB_EFS_CMD,0)==0);
 assert(apply(0,RGB_EFS_CMD,2)==0);
 assert(dirty[0]==3*CENTRAL_SYNC && dirty[1]==0);
 uint32_t color=KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD,2);
 assert(apply(0,color,RGB_COLOR_HSB_VAL(123,45,100))==0);
 assert(apply(0,color,RGB_COLOR_HSB_VAL(123,45,100))==0);
 assert(dirty[0]==5*CENTRAL_SYNC && dirty[1]==0);
 assert(apply(0,RGB_EFS_CMD,1)<0);
 assert(apply(0,color,UINT32_MAX)==-EINVAL);
 assert(apply(0,RGB_RESET_CMD,1)==-EINVAL);
 assert(apply(0,0xffff,0)<0);
 data_a.ready=false; assert(apply(0,RGB_EFS_CMD,0)==-ENODEV); data_a.ready=true;
 isr=true; assert(apply(0,RGB_EFS_CMD,0)==-EWOULDBLOCK); isr=false;
 assert(dirty[0]==5*CENTRAL_SYNC && dirty[1]==0);
 assert(!kp_rgb_global.user_on && !data_a.active && !data_b.active && callbacks==0);
 struct zmk_behavior_binding binding={controls[0].name,RGB_EFS_CMD,0};
 assert(kp_selector_convert(&binding,event)==0);
 assert(zmk_rgb_selector_select(&owners[0],&effects[0])==0);
 assert(zmk_rgb_selector_reset(&owners[0])==0);
#if HAS_ALIAS
 assert(zmk_rgb_matrix_select_effect(0)==0);
 assert(zmk_rgb_matrix_cycle_effect(1)==0);
#else
 assert(zmk_rgb_matrix_select_effect(0)==-ENODEV);
 assert(zmk_rgb_matrix_cycle_effect(1)==-ENODEV);
#endif
 struct kp_rgb_hsb hsb={1,2,100};
 assert(zmk_rgb_matrix_set_color(&effects[0],&hsb)==0);
 assert(zmk_rgb_matrix_set_period(&effects[0],1000)==0);
 kp_rgb_reset_state();
 assert(dirty[0]==5*CENTRAL_SYNC && dirty[1]==0);
 struct zmk_behavior_binding reset={flat.name,RGB_RESET_CMD,0};
 assert(on_keymap_binding_pressed(&reset,event)==0);
 assert(dirty[0]==6*CENTRAL_SYNC && dirty[1]==CENTRAL_SYNC);
 reset.param2=1; assert(on_keymap_binding_pressed(&reset,event)==-EINVAL);
 assert(dirty[0]==6*CENTRAL_SYNC && dirty[1]==CENTRAL_SYNC);
}
int main(void) {
 test_dirty_contract(); test_conversion_application(); test_arguments(); test_associations();
 test_alias_brightness(); test_reset(); test_reset_startup_edges(); test_isr();
 assert(locks==unlocks && !held);
 printf("selector controls (alias=%d central_sync=%d): passed\n",HAS_ALIAS,CENTRAL_SYNC); return 0;
}
'''


def production_fixture():
    # Read at execution time: other suites and production may evolve independently.
    behavior = (ROOT / "src/behavior_rgb_matrix.c").read_text()
    selector = (ROOT / "src/rgb_selector.c").read_text()
    control = (ROOT / "src/behaviors/selector.c").read_text()
    overlay = (ROOT / "src/overlays/selector.c").read_text()
    settings = (ROOT / "src/rgb_settings.c").read_text()
    internal = (ROOT / "src/rgb_matrix_internal.h").read_text()
    selector_header = (ROOT / "src/rgb_selector_internal.h").read_text()
    sync_header = (ROOT / "src/rgb_selector_sync_internal.h").read_text()
    sync_source = (ROOT / "src/rgb_selector_sync.c").read_text()
    public = (ROOT / "include/zmk/rgb_matrix.h").read_text()
    instance = (ROOT / "src/rgb_instance.c").read_text()
    # Numeric commands/packing come from the real public and internal headers.
    defines = (ROOT / "include/dt-bindings/keypaw/rgb_matrix.h").read_text()
    defines = re.sub(r"^#(?:include|pragma).*$", "", defines, flags=re.M)
    commands = (ROOT.parents[1] / "zmk/app/include/dt-bindings/zmk/rgb.h").read_text()
    parameters = (ROOT / "src/rgb_parameters.c").read_text()
    endpoints = internal[internal.index("#define RGB_BRT_SET_CMD"):internal.index("int kp_rgb_save_state(void)")]
    parts = [PRELUDE, commands, defines, endpoints]
    parts += [structure(selector_header, n) for n in ("kp_rgb_effect_defaults", "kp_rgb_selector")]
    parts.append(sync_header[sync_header.index("#define RGB_SELECTOR_COLOR_CMD"):sync_header.index("struct kp_rgb_selector_snapshot")])
    parts.append("#if !CENTRAL_SYNC\n" + sync_header[sync_header.index("static inline void kp_rgb_split_selector_dirty_locked"):sync_header.rindex("#endif")] + "#endif\n")
    parts += [structure(public, n) for n in ("kp_rgb_effect_common_config", "kp_rgb_effect_common_data")]
    parts += [structure(internal, n) for n in ("kp_rgb_global",)]
    parts += [structure(overlay, "kp_selector_data"), structure(control, "kp_selector_control_config"), structure(control, "kp_selector_control_entry"), FIXTURE]
    # Notification delivery is exercised by test_selector_public_api.py.
    parts.append('static void kp_rgb_selector_publish_locked(struct kp_rgb_selector *s, const struct device *p) { (void)s; (void)p; }')
    parts += [function(instance, n) for n in ("kp_rgb_instance_callbacks", "kp_rgb_callbacks_reset", "kp_rgb_callbacks_set_active", "kp_rgb_effect_instance_set_active", "kp_rgb_effect_instance_reset", "kp_rgb_effect_instance_restart_clock")]
    parts += [function(selector, n) for n in ("kp_rgb_selector_count", "kp_rgb_selector_at", "kp_rgb_selector_calc_index", "kp_rgb_selector_resolve_locked", "kp_rgb_selector_apply_defaults_locked", "kp_rgb_selector_select_locked", "kp_rgb_selector_member_slot", "kp_rgb_selector_command", "kp_rgb_selector_convert_locked")]
    parts += [function(parameters, n) for n in ("kp_rgb_selector_alias", "kp_rgb_parameter_target_ready", "kp_rgb_set_color_locked", "kp_rgb_set_period_locked", "zmk_rgb_matrix_set_color", "zmk_rgb_matrix_set_period", "kp_rgb_apply_defaults", "zmk_rgb_matrix_select_effect", "zmk_rgb_matrix_cycle_effect")]
    parts += [function(behavior, n) for n in ("kp_rgb_calc_brt_locked", "kp_rgb_change_brt")]
    parts.append(function(sync_source, "kp_rgb_selector_member_apply_locked"))
    parts.append(function(selector, "kp_rgb_selector_dispatch_locked"))
    parts += [function(overlay, n) for n in ("kp_selector_reconcile", "kp_selector_set_active", "kp_rgb_selector_overlay_state_locked", "kp_rgb_selector_overlay_reset_locked", "kp_selector_refresh_members", "kp_selector_prepare_locked", "kp_rgb_selector_overlay_prepare_locked", "kp_selector_defaults_locked", "kp_selector_start")]
    parts.append(function(overlay, "kp_rgb_selector_overlay_select_locked"))
    selector_api = (ROOT / "src/rgb_selector_api.c").read_text()
    parts += [function(selector_api, n) for n in ("zmk_rgb_selector_select", "zmk_rgb_selector_reset")]
    # Execute the actual defaults iterator, with two host DT instances only.
    macro = re.search(r"^#define KP_SELECTOR_DEFAULTS\(inst\).*?\n\n", overlay, re.M | re.S)
    assert macro, "selector defaults iterator"
    parts += [macro.group(), function(overlay, "kp_rgb_selector_overlays_defaults_locked")]
    parts += [function(control, n) for n in ("kp_rgb_selector_control", "kp_rgb_selector_binding_convert", "kp_rgb_selector_binding_apply", "kp_control_selector", "kp_selector_convert", "kp_selector_pressed")]
    parts.append(function(settings, "kp_rgb_reset_state"))
    parts.append(function(parameters, "kp_rgb_effect_convert_central_state_dependent_params"))
    parts += [function(behavior, n) for n in ("on_keymap_binding_convert_central_state_dependent_params", "on_keymap_binding_pressed")]
    return "\n".join(parts) + TESTS


def main():
    code = production_fixture()
    flags = shlex.split(os.environ.get("CFLAGS", ""))
    if os.environ.get("SELECTOR_CONTROLS_SANITIZE") == "1":
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / "selector_controls.c"
        source.write_text(code)
        for alias, central in ((0, 0), (1, 0), (0, 1), (1, 1)):
            binary = Path(directory) / f"selector_controls_{alias}_{central}"
            subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
                "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                f"-DHAS_ALIAS={alias}", f"-DCENTRAL_SYNC={central}", *flags, str(source), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    main()
