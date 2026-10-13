#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Compile production settings/codecs with host metadata and journaled byte storage."""
import os
import re
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def without_includes(path):
    return re.sub(r"^#(?:include|pragma).*\n", "", path.read_text(), flags=re.M)


def function(source, name):
    match = re.search(r"^(?:static )?(?:inline )?[\w\s*]+\b" + name + r"\([^;]*?\) \{", source, re.M)
    assert match, name
    start = match.start()
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


PRELUDE = r'''
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define CONFIG_SETTINGS 1
#define CONFIG_ZMK_LOG_LEVEL 0
#define CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE 100
#define CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS 100
#define CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS 10000
#define CONFIG_KEYPAW_RGB_MATRIX_HUE_STEP 1
#define CONFIG_KEYPAW_RGB_MATRIX_SAT_STEP 1
#define CONFIG_KEYPAW_RGB_MATRIX_DURATION_STEP 1
#define SETTINGS_MAX_NAME_LEN 64
#define SETTINGS_MAX_VAL_LEN 256
#define KP_RGB_BRT_MAX 100
#define KP_RGB_HUE_MAX 360
#define KP_RGB_SAT_MAX 100
#define IS_ENABLED(x) (x)
#define DT_DRV_INST(i) (i)
#define DT_INST(i,c) (i)
#define DT_CHILD_NUM_STATUS_OKAY(n) 3
#define DT_NUM_INST_STATUS_OKAY(c) 2
#define DT_INST_FOREACH_STATUS_OKAY(fn) fn(0) fn(1)
#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define CLAMP(v,l,h) ((v)<(l)?(l):((v)>(h)?(h):(v)))
#define ARG_UNUSED(x) ((void)(x))
#define BUILD_ASSERT(x,...) _Static_assert(x,#x)
#define snprintk snprintf
#define LOG_MODULE_DECLARE(...)
#define LOG_WRN(...) ((void)0)
#define K_MSEC(x) (x)
#define K_NO_WAIT 0

struct device { void *data; const char *name; const void *config; };
struct kp_rgb_hsb { uint16_t h; uint8_t s,b; };
struct kp_rgb_effect_common_data { struct kp_rgb_hsb color; uint32_t duration_ms; };
struct kp_rgb_effect_common_config { const struct device *owner; uint16_t slot; const char *persist_id; bool persist_parameters; };
struct kp_rgb_effect_defaults { struct kp_rgb_hsb color; uint32_t duration_ms; };
struct kp_rgb_selector {
    const struct device *owner;
    const struct device *const *effects;
    const struct kp_rgb_effect_defaults *effect_defaults;
    size_t effect_count, effect_index;
    const struct device *active_fx;
    int64_t initial_duration_ms;
    uint16_t initial_effect;
    bool publish_changes, notification_pending;
    struct kp_rgb_selector *notification_next;
};
struct kp_rgb_global { bool user_on; uint8_t brightness; bool initial_on; int32_t initial_brightness; };
struct kp_rgb_global kp_rgb_global;
static bool held, host_device_ready=true;
static const struct device *unready_member;
static unsigned lock_entries, flushes, reconciles;
static bool reconciled_user_on;
static void kp_rgb_matrix_lock(void) { assert(!held); held=true; lock_entries++; }
static void kp_rgb_matrix_unlock(void) { assert(held); held=false; }
static void zmk_rgb_matrix_flush(void) { flushes++; }
static void kp_rgb_reconcile_power_locked(void) { assert(held); reconciles++; reconciled_user_on=kp_rgb_global.user_on; }
static int kp_rgb_apply_defaults(void) {
    bool acquired=!held;
    if(acquired) kp_rgb_matrix_lock();
    kp_rgb_global.user_on=kp_rgb_global.initial_on;
    kp_rgb_global.brightness=(uint8_t)CLAMP(kp_rgb_global.initial_brightness,0,KP_RGB_BRT_MAX);
    kp_rgb_reconcile_power_locked(); zmk_rgb_matrix_flush();
    if(acquired) kp_rgb_matrix_unlock();
    return 0;
}
static bool device_is_ready(const struct device *dev) { return host_device_ready && dev!=NULL && dev!=unready_member; }
static bool k_is_in_isr(void) { return false; }
static struct kp_rgb_effect_common_data *kp_rgb_effect_data(const struct device *dev) { return dev->data; }
static const struct kp_rgb_effect_common_config *kp_rgb_effect_cfg(const struct device *dev) { return dev->config; }
static void kp_rgb_selector_publish_locked(struct kp_rgb_selector *selector,const struct device *previous) { (void)selector;(void)previous;assert(held); }
static size_t kp_rgb_selector_count(const struct kp_rgb_selector *selector);
static const struct device *kp_rgb_selector_at(const struct kp_rgb_selector *selector,size_t slot);
static uint16_t kp_rgb_selector_calc_index(const struct kp_rgb_selector *selector,uint16_t current,int16_t delta);
static int kp_rgb_selector_resolve_locked(struct kp_rgb_selector *selector);
static int kp_rgb_selector_apply_defaults_locked(struct kp_rgb_selector *selector);
static int kp_rgb_selector_select_locked(struct kp_rgb_selector *selector,uint16_t slot);
struct k_work { void (*handler)(struct k_work *); };
struct k_work_delayable { struct k_work work; };
static struct k_work_delayable *pending;
static int scheduled_delay;
static int k_work_reschedule(struct k_work_delayable *work,int delay) { pending=work;scheduled_delay=delay;return 1; }
#define K_WORK_DELAYABLE_DEFINE(name,fn) static struct k_work_delayable name={{fn}}
typedef int (*settings_read_cb)(void *,void *,size_t);
#define SETTINGS_STATIC_HANDLER_DEFINE_WITH_CPRIO(name,path,get,set,commit,export,prio) static const char *registered_subtree=path; static int (*registered_set)(const char *,size_t,settings_read_cb,void *)=set; static int (*registered_commit)(void)=commit; _Static_assert(prio==INT_MAX-1,"settings commit priority")
struct entry { char path[65]; unsigned char bytes[256]; size_t len; };
static struct entry store[32];
struct operation { char path[65]; bool deletion; int result; };
static struct operation journal[256];
static unsigned journal_count,reads;
static const char *fail_path,*hook_path;
static bool fail_delete,immutable_snapshot;
static unsigned fail_count;
static void (*save_hook)(void);
static struct entry direct_snapshot[32];
static struct entry *lookup(const char *path) { for(size_t i=0;i<32;i++) if(store[i].len && !strcmp(store[i].path,path)) return &store[i]; return NULL; }
static void put(const char *path,const void *bytes,size_t len) { struct entry *e=lookup(path);if(!e)for(size_t i=0;i<32;i++)if(!store[i].len){e=&store[i];break;}assert(e&&len<=sizeof(e->bytes));strcpy(e->path,path);memcpy(e->bytes,bytes,len);e->len=len; }
static int io(const char *path,bool deletion) {
    assert(!held&&journal_count<256);int rc=0;
    if(fail_count&&deletion==fail_delete&&fail_path&&!strcmp(path,fail_path)){fail_count--;rc=-EIO;}
    struct operation *op=&journal[journal_count++];strcpy(op->path,path);op->deletion=deletion;op->result=rc;
    if(save_hook&&!deletion&&hook_path&&!strcmp(path,hook_path)){void(*hook)(void)=save_hook;save_hook=NULL;hook();assert(!held);}
    return rc;
}
static int settings_save_one(const char *path,const void *bytes,size_t len) { int rc=io(path,false);if(!rc)put(path,bytes,len);return rc; }
static int settings_delete(const char *path) { int rc=io(path,true);if(rc)return rc;struct entry *e=lookup(path);if(!e)return -ENOENT;e->len=0;return 0; }
static int read_blob(void *arg,void *out,size_t len);
static int settings_load_subtree_direct(const char *prefix,int(*cb)(const char*,size_t,settings_read_cb,void*,void*),void *arg) {
    assert(!held);size_t n=strlen(prefix);struct entry *source=immutable_snapshot?direct_snapshot:store;
    for(size_t i=0;i<32;i++)if(source[i].len&&!strncmp(source[i].path,prefix,n)&&source[i].path[n]=='/'){int rc=cb(source[i].path+n+1,source[i].len,read_blob,source[i].bytes,arg);if(rc)return rc;}
    return 0;
}
static int read_blob(void *arg,void *out,size_t len) { assert(!held);reads++;memcpy(out,arg,len);return (int)len; }
static int short_read(void *arg,void *out,size_t len) { assert(!held);memcpy(out,arg,len-1);return (int)len-1; }
static int failed_read(void *arg,void *out,size_t len) { (void)arg;(void)out;(void)len;assert(!held);return -EIO; }
static void run_work(void) { assert(pending&&!held);struct k_work_delayable*w=pending;pending=NULL;w->work.handler(&w->work); }
'''
METADATA = r'''
static const struct device owners[2]={{.name="one"},{.name="two"}};
static struct kp_rgb_effect_common_data data[2][3];
static struct kp_rgb_effect_common_config configs[2][3]={
    {{.owner=&owners[0],.slot=0,.persist_id="shared",.persist_parameters=true},
     {.owner=&owners[0],.slot=2,.persist_id="other",.persist_parameters=true},
     {.owner=&owners[0],.slot=3,.persist_id="optout",.persist_parameters=false}},
    {{.owner=&owners[1],.slot=0,.persist_id="shared",.persist_parameters=true},
     {.owner=&owners[1],.slot=2,.persist_id="other",.persist_parameters=true},
     {.owner=&owners[1],.slot=3,.persist_id="optout",.persist_parameters=false}}};
static const struct device members[2][3]={
    {{&data[0][0],"a",&configs[0][0]},{&data[0][1],"b",&configs[0][1]},{&data[0][2],"c",&configs[0][2]}},
    {{&data[1][0],"d",&configs[1][0]},{&data[1][1],"e",&configs[1][1]},{&data[1][2],"f",&configs[1][2]}}};
static const struct device *slots[2][4];
static const struct kp_rgb_effect_defaults defaults[4]={{{10,20,100},500},{{0},0},{{40,50,100},600},{{70,80,100},700}};
static struct kp_rgb_selector selectors[2];
static const struct kp_rgb_persist_effect_id ids_members[3]={{"shared",true},{"other",true},{"optout",false}};
static struct kp_rgb_persist_selector_ids ids[2]={{"one",ids_members,3},{"two",ids_members,3}};
static unsigned owner_order[2]={0,1}, member_order[3]={0,1,2};
static bool has_alias=true, prepared=true;
static size_t kp_rgb_persist_selector_count(void) {return 2;}
static const struct kp_rgb_persist_selector_ids *kp_rgb_persist_selector_at(size_t i,const struct device **owner) {
    assert(i<2);if(owner)*owner=&owners[owner_order[i]];return &ids[i];
}
static const struct device *kp_rgb_persist_member_at(size_t i,size_t j) {assert(i<2 && j<3);return &members[owner_order[i]][member_order[j]];}
static int kp_rgb_persist_ids_validate(void) {return kp_rgb_persist_validate_ids(ids,2);}
static const struct device *kp_rgb_selector_alias(void) {return has_alias?&owners[0]:NULL;}
static struct kp_rgb_selector *kp_rgb_selector_overlay_prepare_locked(const struct device *owner) {
    assert(held); if(!prepared)return NULL; for(size_t i=0;i<2;i++)if(owner==&owners[i])return &selectors[i];return NULL;
}
static unsigned resets;
static void kp_rgb_request_runtime_reset_locked(void) {assert(held);resets++;}
static void kp_rgb_selector_overlays_defaults_locked(void) {
    assert(held);for(size_t i=0;i<2;i++)assert(kp_rgb_selector_apply_defaults_locked(&selectors[i])==0);
}
'''

TESTS = r'''
#define GLOBAL "keypaw/rgb_matrix/state"
#define SEL1 "keypaw/rgb_matrix/selectors/one/state"
#define PAR1 "keypaw/rgb_matrix/selectors/one/effects/shared"
#define SEL2 "keypaw/rgb_matrix/selectors/two/state"
#define PAR2 "keypaw/rgb_matrix/selectors/two/effects/shared"
#define LEGPAR "keypaw/rgb_matrix/state/effects/shared"
#define OTHER1 "keypaw/rgb_matrix/selectors/one/effects/other"
#define LEGOTHER "keypaw/rgb_matrix/state/effects/other"
static void boot(void) {
    owner_order[0]=0;owner_order[1]=1;
    for(size_t j=0;j<3;j++) member_order[j]=j;
    ids[0]=(struct kp_rgb_persist_selector_ids){"one",ids_members,3};
    ids[1]=(struct kp_rgb_persist_selector_ids){"two",ids_members,3};
    for(size_t i=0;i<2;i++) {configs[i][0].slot=0;configs[i][1].slot=2;configs[i][2].slot=3;}
    assert(!held);pending=NULL;host_device_ready=true;prepared=true;unready_member=NULL;
    memset(kp_selectors,0,sizeof(kp_selectors));
    kp_selectors[0].parameters=kp_parameters_0; kp_selectors[1].parameters=kp_parameters_1;
    memset(kp_parameters_0,0,sizeof(kp_parameters_0));memset(kp_parameters_1,0,sizeof(kp_parameters_1));
    kp_legacy_valid=kp_legacy_pending=kp_global_pending=kp_global_seen=false;
    kp_global_dirty=kp_global_reset=false;kp_global_revision=0;
    for(size_t i=0;i<2;i++) {
        slots[i][0]=&members[i][0];slots[i][1]=NULL;slots[i][2]=&members[i][1];slots[i][3]=&members[i][2];
        selectors[i]=(struct kp_rgb_selector){.owner=&owners[i],.effects=slots[i],.effect_count=4,
            .effect_defaults=defaults,.initial_effect=0,.initial_duration_ms=1000};
    }
    kp_rgb_global.initial_on=true;kp_rgb_global.initial_brightness=70;
    assert(kp_rgb_apply_defaults()==0);
    kp_rgb_matrix_lock();kp_rgb_selector_overlays_defaults_locked();kp_rgb_matrix_unlock();
}
static void clean_boot(void) {memset(store,0,sizeof(store));journal_count=0;fail_count=0;save_hook=NULL;immutable_snapshot=false;has_alias=true;boot();}
static int load(const char *key,const void *bytes,size_t len) {return registered_set(key,len,read_blob,(void *)bytes);}
static void load_store(bool reverse) {
    for(size_t n=0;n<32;n++) {size_t i=reverse?31-n:n;if(store[i].len)
        assert(load(store[i].path+sizeof(KP_RGB_PERSIST_SUBTREE),store[i].bytes,store[i].len)==0);}
}
static void stage_legacy(void) {
    struct kp_rgb_persist_blob old;kp_rgb_persist_pack(false,33,"other",&old);
    struct kp_rgb_persist_effect p={.h=123,.s=44,.duration_ms=888};
    put(GLOBAL,&old,sizeof(old));put(LEGPAR,&p,sizeof(p));load_store(false);
}
static void assert_default(void) {assert(selectors[0].active_fx==&members[0][0] && selectors[1].active_fx==&members[1][0]);
    assert(kp_rgb_global.user_on && kp_rgb_global.brightness==70 && data[0][0].color.h==10);}
static void precedence(bool reverse) {
    clean_boot();
    struct kp_rgb_persist_blob old;kp_rgb_persist_pack(false,33,"other",&old);
    struct kp_rgb_persist_global_state g={true,91};struct kp_rgb_persist_global_record gr;
    struct kp_rgb_persist_selector_record sr;struct kp_rgb_persist_parameters_record pr;
    struct kp_rgb_persist_effect p={.h=222,.s=66,.duration_ms=999},lp={.h=111,.s=22,.duration_ms=777};
    assert(kp_rgb_persist_encode_global(&g,&gr)==0);assert(kp_rgb_persist_encode_selector("shared",&sr)==0);
    assert(kp_rgb_persist_encode_parameters(&p,&pr)==0);
    if(reverse) {assert(load("selectors/one/effects/shared",&pr,sizeof(pr))==0);assert(load("selectors/one/state",&sr,sizeof(sr))==0);}
    if(reverse) assert(load("state",&gr,sizeof(gr))==0);
    assert(load("state",&old,sizeof(old))==0);assert(load("state/effects/shared",&lp,sizeof(lp))==0);
    if(!reverse) assert(load("state",&gr,sizeof(gr))==0);
    if(!reverse) {assert(load("selectors/one/state",&sr,sizeof(sr))==0);assert(load("selectors/one/effects/shared",&pr,sizeof(pr))==0);}
    assert_default();assert(!pending && journal_count==0);
    assert(registered_commit()==0);assert(kp_rgb_global.brightness==91 && kp_rgb_global.user_on);
    assert(selectors[0].active_fx==&members[0][0] && data[0][0].color.h==222 && data[1][0].color.h==10);
    assert(!pending);assert(registered_commit()==0 && data[0][0].color.h==222);
}
static void validation(void) {
    clean_boot();struct kp_rgb_persist_selector_record sr;assert(kp_rgb_persist_encode_selector("removed",&sr)==0);
    kp_rgb_matrix_lock();assert(kp_rgb_selector_select_locked(&selectors[0],2)==1);kp_rgb_matrix_unlock();
    assert(load("selectors/one/state",&sr,sizeof(sr))==0);
    assert(selectors[0].active_fx==&members[0][1]);assert(registered_commit()==0);
    assert(selectors[0].active_fx==&members[0][0]);
    unsigned before=reads;
    assert(load("selectors/missing/state",&sr,sizeof(sr))==0 && reads==before);
    assert(load("selectors/one/state",&sr,sizeof(sr)-1)==-EINVAL);
    assert(registered_set("selectors/one/state",sizeof(sr),short_read,&sr)==-EINVAL);
    assert(registered_set("selectors/one/state",sizeof(sr),failed_read,&sr)==-EIO);
    sr.version=99;assert(load("selectors/one/state",&sr,sizeof(sr))==-EINVAL);
    struct kp_rgb_persist_parameters_record p={.version=99};
    assert(load("selectors/one/effects/shared",&p,sizeof(p))==-EINVAL);
    assert(load("selectors/one/effects/shared",&p,sizeof(p)-1)==-EINVAL);
    assert(registered_set("selectors/one/effects/shared",sizeof(p),short_read,&p)==-EINVAL);
    struct kp_rgb_persist_global_record g={.version=KP_RGB_PERSIST_GLOBAL_VERSION,.user_on=2,.brightness=50};
    assert(load("state",&g,sizeof(g))==-EINVAL);
    g.user_on=1;g.reserved=1;assert(load("state",&g,sizeof(g))==-EINVAL);
    assert(load("state",&g,sizeof(g)-1)==-EINVAL);
    assert(registered_set("state",sizeof(g),short_read,&g)==-EINVAL);
    before=reads;assert(load("selectors/one/effects/optout",&p,sizeof(p))==0 && reads==before);
    assert(load("selectors/one/effects/disabled",&p,sizeof(p))==0 && reads==before);
    assert_default();
}
static void readiness(void) {
    clean_boot();stage_legacy();assert_default();unready_member=&members[1][2];
    assert(registered_commit()==-ENODEV);assert_default();assert(!pending && !journal_count);
    unready_member=NULL;host_device_ready=false;
    assert(registered_commit()==-ENODEV);assert_default();assert(!pending && !journal_count);
    host_device_ready=true;prepared=false;assert(registered_commit()==-ENODEV);assert_default();
    prepared=true;assert(registered_commit()==0);
    assert(!kp_rgb_global.user_on && selectors[0].active_fx==&members[0][1] && data[0][0].color.h==123);
    run_work();assert(!pending);
}
static void migration(const char *failure) {
    clean_boot();stage_legacy();assert(registered_commit()==0 && pending);
    fail_path=failure;fail_delete=false;fail_count=1;run_work();
    assert(pending && scheduled_delay==1000 && lookup(GLOBAL)->len==sizeof(struct kp_rgb_persist_blob));
    bool failed=false;
    for(unsigned i=0;i<journal_count;i++) if(journal[i].result==-EIO) {
        assert(!strcmp(journal[i].path,failure));failed=true;
    }
    assert(failed);
    boot();load_store(true);assert(registered_commit()==0);
    assert(selectors[0].active_fx==&members[0][1] && data[0][0].color.h==123);
    assert(pending);run_work();assert(!pending && lookup(GLOBAL)->len==sizeof(struct kp_rgb_persist_global_record));
    assert(!lookup(LEGPAR) && lookup(PAR1) && lookup(SEL1));
    boot();load_store(false);assert(registered_commit()==0 && !pending);
    assert(data[0][0].color.h==123 && selectors[0].active_fx==&members[0][1]);
}
static void isolation(void) {
    clean_boot();
    kp_rgb_matrix_lock();assert(kp_rgb_selector_select_locked(&selectors[0],2)==1);
    kp_rgb_matrix_unlock();
    assert(zmk_rgb_matrix_set_color(&members[0][2],&(struct kp_rgb_hsb){234,55,100})==0);
    assert(zmk_rgb_matrix_set_period(&members[0][2],1234)==0 && data[0][2].duration_ms==1234);
    assert(!pending && !journal_count);
    assert(data[0][2].color.h==234);assert(kp_rgb_save_selector_state(&owners[0],false)==0 && pending);
    run_work();assert(!pending && journal_count==5); /* three writes, two legacy deletions */
    assert(lookup(SEL1) && lookup(PAR1) && !lookup(SEL2) && !lookup(GLOBAL));
    assert(!lookup("keypaw/rgb_matrix/selectors/one/effects/optout"));
    kp_rgb_matrix_lock();assert(kp_rgb_selector_select_locked(&selectors[1],2)==1);data[1][0].color.h=321;kp_rgb_matrix_unlock();
    assert(kp_rgb_save_selector_state(&owners[1],false)==0);run_work();
    struct entry saved_two=*lookup(PAR2),saved_sel=*lookup(SEL2);
    kp_rgb_matrix_lock();assert(kp_rgb_selector_apply_defaults_locked(&selectors[0])==0);kp_rgb_matrix_unlock();
    assert(kp_rgb_save_selector_state(&owners[0],true)==0);run_work();
    assert(!memcmp(&saved_two,lookup(PAR2),sizeof(saved_two)) && !memcmp(&saved_sel,lookup(SEL2),sizeof(saved_sel)));
    assert(data[1][0].color.h==321 && selectors[1].active_fx==&members[1][1]);
    assert(kp_rgb_save_selector_state(&members[0][0],false)==-ENODEV);
    struct entry saved_one=*lookup(PAR1);
    saved_two=*lookup(PAR2);
    kp_rgb_global.brightness=42;kp_rgb_global.user_on=false;
    assert(kp_rgb_save_state()==0);run_work();
    assert(!memcmp(&saved_one,lookup(PAR1),sizeof(saved_one)) && !memcmp(&saved_two,lookup(PAR2),sizeof(saved_two)));
    struct kp_rgb_persist_global_state global;
    assert(kp_rgb_persist_decode_global(lookup(GLOBAL)->bytes,lookup(GLOBAL)->len,&global)==0);
    assert(!global.user_on && global.brightness==42);
    kp_rgb_reset_state();assert(pending && scheduled_delay==0 && resets);
    assert_default();assert(data[1][0].color.h==10);run_work();assert(!pending);
    boot();load_store(true);assert(registered_commit()==0);assert_default();
}
static void cleanup(void) {
    clean_boot();stage_legacy();assert(registered_commit()==0);
    fail_path=LEGPAR;fail_delete=true;fail_count=1;run_work();
    assert(pending && lookup(LEGPAR) && lookup(PAR1));
    assert(lookup(GLOBAL)->len==sizeof(struct kp_rgb_persist_blob));
    run_work();assert(!pending && !lookup(LEGPAR));
    assert(lookup(GLOBAL)->len==sizeof(struct kp_rgb_persist_global_record));
    stage_legacy();kp_rgb_matrix_lock();assert(kp_rgb_selector_apply_defaults_locked(&selectors[0])==0);kp_rgb_matrix_unlock();
    fail_count=1;assert(kp_rgb_save_selector_state(&owners[0],true)==0);run_work();assert(pending);
    run_work();assert(!pending && !lookup(LEGPAR));
}
static void reset_legacy_scope(void) {
    clean_boot();has_alias=false;stage_legacy();assert(registered_commit()==0);run_work();
    has_alias=true; /* Select the alias after retaining the legacy snapshot without one. */
    struct kp_rgb_persist_effect stale={.h=99,.s=11,.duration_ms=444};
    put("keypaw/rgb_matrix/state/effects/removed",&stale,sizeof(stale));
    kp_rgb_global.user_on=true;kp_rgb_global.brightness=99;
    kp_rgb_matrix_lock();assert(kp_rgb_selector_apply_defaults_locked(&selectors[0])==0);kp_rgb_matrix_unlock();
    unsigned start=journal_count;
    assert(kp_rgb_save_selector_state(&owners[0],true)==0);run_work();assert(!pending);
    struct kp_rgb_persist_blob old;
    assert(kp_rgb_persist_decode_legacy(lookup(GLOBAL)->bytes,lookup(GLOBAL)->len,&old)==0);
    assert(!old.has_selected && !old.user_on && old.brightness==33);
    assert(kp_rgb_global.user_on && kp_rgb_global.brightness==99);
    assert(!lookup(LEGPAR) && !lookup("keypaw/rgb_matrix/state/effects/removed"));
    for(unsigned i=start;i<journal_count;i++) assert(strncmp(journal[i].path,
        "keypaw/rgb_matrix/selectors/two/",32)!=0);
    put("keypaw/rgb_matrix/state/effects/removed",&stale,sizeof(stale));
    fail_path="keypaw/rgb_matrix/state/effects/removed";fail_delete=true;fail_count=1;
    kp_rgb_reset_state();assert_default();run_work();assert(pending && scheduled_delay==1000);
    run_work();assert(!pending && !lookup(fail_path));
    assert(lookup(GLOBAL)->len==sizeof(struct kp_rgb_persist_global_record));
}
static void no_alias(void) {
    clean_boot();has_alias=false;stage_legacy();assert(registered_commit()==0);
    assert(selectors[0].active_fx==&members[0][0] && data[0][0].color.h==10);
    run_work();struct kp_rgb_persist_blob b;assert(lookup(GLOBAL)->len==sizeof(b));
    assert(kp_rgb_persist_decode_legacy(lookup(GLOBAL)->bytes,sizeof(b),&b)==0);
    assert(b.has_selected && !strcmp(b.selected_id,"other") && lookup(LEGPAR));
    boot();load_store(true);assert(registered_commit()==0);run_work();
    assert(selectors[0].active_fx==&members[0][0]);
}
static void assert_persisted_selection(const char *path,const char *id) {
    struct kp_rgb_persist_selector_record value;
    assert(lookup(path) && kp_rgb_persist_decode_selector(lookup(path)->bytes,lookup(path)->len,&value)==0);
    assert(!strcmp(value.selected_id,id));
}
static void assert_persisted_parameters(const char *path,uint16_t hue,uint16_t duration) {
    struct kp_rgb_persist_effect value;
    assert(lookup(path) && kp_rgb_persist_decode_parameters(lookup(path)->bytes,lookup(path)->len,&value)==0);
    assert(value.h==hue && value.duration_ms==duration);
}
static void reordering(void) {
    clean_boot();
    kp_rgb_matrix_lock();
    assert(kp_rgb_selector_select_locked(&selectors[0],2)==1);
    for(size_t i=0;i<2;i++) for(size_t j=0;j<3;j++) {
        data[i][j].color.h=100+100*i+10*j;data[i][j].duration_ms=800+100*i+10*j;
    }
    kp_rgb_matrix_unlock();
    assert(kp_rgb_save_selector_state(&owners[0],false)==0);
    assert(kp_rgb_save_selector_state(&owners[1],false)==0);run_work();assert(!pending);
    boot();
    static const struct kp_rgb_persist_effect_id reordered_ids[3]={{"other",true},{"optout",false},{"shared",true}};
    static const struct kp_rgb_effect_defaults reordered_defaults[4]={{{40,50,100},600},{{0},0},{{70,80,100},700},{{10,20,100},500}};
    owner_order[0]=1;owner_order[1]=0;
    member_order[0]=1;member_order[1]=2;member_order[2]=0;
    ids[0]=(struct kp_rgb_persist_selector_ids){"two",reordered_ids,3};
    ids[1]=(struct kp_rgb_persist_selector_ids){"one",reordered_ids,3};
    kp_rgb_matrix_lock();
    for(size_t i=0;i<2;i++) {
        configs[i][0].slot=3;configs[i][1].slot=0;configs[i][2].slot=2;
        slots[i][0]=&members[i][1];slots[i][1]=NULL;slots[i][2]=&members[i][2];slots[i][3]=&members[i][0];
        selectors[i].effect_defaults=reordered_defaults;selectors[i].initial_effect=3;
        assert(kp_rgb_selector_apply_defaults_locked(&selectors[i])==0);
    }
    kp_rgb_matrix_unlock();
    load_store(true);assert(selectors[0].active_fx==&members[0][0] && data[0][0].color.h==10);
    assert(registered_commit()==0 && !pending);
    assert(selectors[0].active_fx==&members[0][1] && selectors[0].effect_index==0);
    assert(selectors[1].active_fx==&members[1][0] && selectors[1].effect_index==3);
    for(size_t i=0;i<2;i++) for(size_t j=0;j<2;j++) {
        assert(data[i][j].color.h==100+100*i+10*j && data[i][j].duration_ms==800+100*i+10*j);
    }
    assert(data[0][2].color.h==70 && data[1][2].color.h==70);
    assert(kp_rgb_save_selector_state(&owners[0],false)==0);
    assert(kp_rgb_save_selector_state(&owners[1],false)==0);run_work();assert(!pending);
    assert_persisted_selection(SEL1,"other");assert_persisted_selection(SEL2,"shared");
    boot();load_store(false);assert(registered_commit()==0 && !pending);
    assert(selectors[0].effect_index==2 && selectors[0].active_fx==&members[0][1]);
    assert(selectors[1].effect_index==0 && selectors[1].active_fx==&members[1][0]);
    assert(data[0][1].color.h==110 && data[1][0].color.h==200);
    assert(configs[0][0].slot==0 && configs[1][1].slot==2 && owner_order[0]==0);
}
static void import_other(void) {
    clean_boot();stage_legacy();
    struct kp_rgb_persist_effect other={.h=267,.s=61,.duration_ms=2345};
    put(LEGOTHER,&other,sizeof(other));assert(load("state/effects/other",&other,sizeof(other))==0);
    assert(data[0][1].color.h==40 && data[1][1].color.h==40);
    assert(registered_commit()==0 && data[0][1].color.h==267 && data[1][1].color.h==40);
    fail_path=OTHER1;fail_delete=false;fail_count=1;run_work();
    assert(pending && lookup(LEGOTHER) && lookup(GLOBAL)->len==sizeof(struct kp_rgb_persist_blob));
    run_work();assert(!pending && !lookup(LEGOTHER));assert_persisted_parameters(OTHER1,267,2345);
    boot();load_store(true);assert(registered_commit()==0 && !pending);
    assert(data[0][1].color.h==267 && data[0][1].duration_ms==2345 && data[1][1].color.h==40);
}
static void change_during_save(void) {
    assert(!held);
    kp_rgb_matrix_lock();
    data[0][0].color.h=288;data[0][0].duration_ms=2888;
    assert(kp_rgb_selector_select_locked(&selectors[0],2)==1);
    kp_rgb_matrix_unlock();assert(kp_rgb_save_selector_state(&owners[0],false)==0);
}
static void reset_during_save(void) {
    assert(!held);kp_rgb_matrix_lock();
    assert(kp_rgb_selector_apply_defaults_locked(&selectors[0])==0);
    kp_rgb_matrix_unlock();assert(kp_rgb_save_selector_state(&owners[0],true)==0);
}
static void global_reset_during_save(void) {assert(!held);kp_rgb_reset_state();}
static void interleaving(void) {
    clean_boot();
    assert(kp_rgb_save_selector_state(&owners[0],false)==0);
    hook_path=PAR1;save_hook=change_during_save;run_work();
    assert(!save_hook && pending && scheduled_delay==1000 && kp_selectors[0].dirty);
    assert_persisted_selection(SEL1,"shared");assert_persisted_parameters(PAR1,10,500);
    run_work();assert(!pending && !kp_selectors[0].dirty);
    assert_persisted_selection(SEL1,"other");assert_persisted_parameters(PAR1,288,2888);
    boot();load_store(false);assert(registered_commit()==0 && selectors[0].active_fx==&members[0][1]);
    assert(data[0][0].color.h==288);
    struct kp_rgb_persist_effect stale={.h=99,.s=11,.duration_ms=444};
    put("keypaw/rgb_matrix/state/effects/removed",&stale,sizeof(stale));
    hook_path=PAR1;save_hook=reset_during_save;
    fail_path=OTHER1;fail_delete=false;fail_count=1;
    assert(kp_rgb_save_selector_state(&owners[0],false)==0);run_work();
    assert(!save_hook && pending && kp_selectors[0].dirty && kp_selectors[0].reset);
    assert(selectors[0].active_fx==&members[0][0] && data[0][0].color.h==10);
    run_work();assert(!pending && !kp_selectors[0].dirty && !kp_selectors[0].reset);
    assert_persisted_selection(SEL1,"shared");assert_persisted_parameters(PAR1,10,500);
    assert(!lookup("keypaw/rgb_matrix/state/effects/removed"));
    boot();load_store(true);assert(registered_commit()==0);assert_default();
    kp_rgb_global.user_on=false;kp_rgb_global.brightness=12;
    hook_path=GLOBAL;save_hook=global_reset_during_save;fail_path=GLOBAL;fail_count=1;
    assert(kp_rgb_save_state()==0);run_work();
    assert(!save_hook && pending && kp_global_dirty && kp_global_reset && kp_selectors[0].dirty && kp_selectors[1].dirty);
    assert_default();run_work();assert(!pending && !kp_global_dirty);
    boot();load_store(false);assert(registered_commit()==0);assert_default();
}
static void change_global_during_save(void) {
    assert(!held);kp_rgb_matrix_lock();kp_rgb_global.brightness=45;kp_rgb_matrix_unlock();
    assert(kp_rgb_save_state()==0);
}
static void reset_revision_interleaving(void) {
    clean_boot();
    kp_rgb_matrix_lock();assert(kp_rgb_selector_select_locked(&selectors[0],2)==1);
    data[0][0].color.h=299;kp_rgb_matrix_unlock();
    struct kp_rgb_persist_effect stale={.h=99,.s=11,.duration_ms=444};
    const char *removed="keypaw/rgb_matrix/state/effects/removed";
    put(removed,&stale,sizeof(stale));
    hook_path=PAR1;save_hook=reset_during_save;
    assert(kp_rgb_save_selector_state(&owners[0],false)==0);run_work();
    assert(pending && scheduled_delay==1000 && kp_selectors[0].dirty && kp_selectors[0].reset);
    assert_persisted_selection(SEL1,"other");assert_persisted_parameters(PAR1,299,500);
    assert(lookup(removed));
    fail_path=removed;fail_delete=true;fail_count=1;run_work();
    assert(pending && kp_selectors[0].dirty && kp_selectors[0].reset && lookup(removed));
    assert_persisted_selection(SEL1,"shared");assert_persisted_parameters(PAR1,10,500);
    run_work();assert(!pending && !lookup(removed) && !kp_selectors[0].reset);
    kp_rgb_global.brightness=12;hook_path=GLOBAL;save_hook=change_global_during_save;
    assert(kp_rgb_save_state()==0);run_work();
    assert(pending && kp_global_dirty && kp_rgb_global.brightness==45);
    struct kp_rgb_persist_global_state global;
    assert(kp_rgb_persist_decode_global(lookup(GLOBAL)->bytes,lookup(GLOBAL)->len,&global)==0 && global.brightness==12);
    run_work();assert(!pending && !kp_global_dirty);
    assert(kp_rgb_persist_decode_global(lookup(GLOBAL)->bytes,lookup(GLOBAL)->len,&global)==0 && global.brightness==45);
    hook_path=GLOBAL;save_hook=global_reset_during_save;
    assert(kp_rgb_save_state()==0);run_work();
    assert(pending && kp_global_dirty && kp_global_reset && kp_selectors[0].dirty && kp_selectors[1].dirty);
    assert_default();run_work();assert(!pending && !kp_global_reset);
    boot();load_store(true);assert(registered_commit()==0);assert_default();
}
static void reset_staged_legacy(void) {
    clean_boot();stage_legacy();assert(registered_commit()==0);run_work();
    kp_rgb_matrix_lock();assert(kp_rgb_selector_apply_defaults_locked(&selectors[0])==0);kp_rgb_matrix_unlock();
    assert(kp_rgb_save_selector_state(&owners[0],true)==0);
    struct kp_rgb_persist_effect stale={.h=299,.s=11,.duration_ms=444};
    assert(load("state/effects/shared",&stale,sizeof(stale))==0);
    assert(registered_commit()==0 && data[0][0].color.h==10 && data[0][0].duration_ms==500);
    run_work();assert(!pending);
    kp_rgb_reset_state();
    assert(load("state/effects/shared",&stale,sizeof(stale))==0);
    assert(registered_commit()==0);assert_default();run_work();assert(!pending);
}
static void immutable_purge(void) {
    clean_boot();struct kp_rgb_persist_effect stale={.h=99,.s=11,.duration_ms=444};
    put("keypaw/rgb_matrix/state/effects/removed",&stale,sizeof(stale));
    memcpy(direct_snapshot,store,sizeof(store));immutable_snapshot=true;
    kp_rgb_reset_state();run_work();
    assert(pending && scheduled_delay==1000 && kp_selectors[0].reset);
    assert(!lookup("keypaw/rgb_matrix/state/effects/removed"));
    unsigned before=journal_count;immutable_snapshot=false;run_work();
    assert(!pending && journal_count>before && !kp_selectors[0].reset);
    boot();load_store(true);assert(registered_commit()==0);assert_default();
}
int main(void) {
    assert(!strcmp(registered_subtree,KP_RGB_PERSIST_SUBTREE));
    precedence(false);precedence(true);validation();readiness();migration(PAR1);migration(SEL1);migration(GLOBAL);isolation();cleanup();no_alias();reset_legacy_scope();
    reordering();import_other();interleaving();reset_revision_interleaving();immutable_purge();reset_staged_legacy();
    puts("scoped settings staging/precedence/migration/isolation/reset/retries passed");return 0;
}
'''


def fixture():
    selector_source = (ROOT / "src/rgb_selector.c").read_text()
    parameters_source = (ROOT / "src/rgb_parameters.c").read_text()
    selector_functions = "\n".join(function(selector_source, name) for name in (
        "kp_rgb_selector_count", "kp_rgb_selector_at", "kp_rgb_selector_calc_index",
        "kp_rgb_selector_resolve_locked", "kp_rgb_selector_apply_defaults_locked",
        "kp_rgb_selector_select_locked"))
    parameter_functions = "\n".join(function(parameters_source, name) for name in (
        "kp_rgb_parameter_target_ready", "kp_rgb_set_color_locked",
        "kp_rgb_set_period_locked", "zmk_rgb_matrix_set_color",
        "zmk_rgb_matrix_set_period"))
    return (PRELUDE + without_includes(ROOT / "include/zmk/rgb_persist.h") +
            without_includes(ROOT / "src/rgb_persist.c") + selector_functions +
            parameter_functions + METADATA +
            without_includes(ROOT / "src/rgb_settings.c") + TESTS)


def main():
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "scoped_settings.c"
        path.write_text(fixture())
        binary = Path(directory) / "scoped_settings"
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(path), "-o", str(binary)
        ], check=True, timeout=30)
        subprocess.run([str(binary)], check=True, timeout=15)


if __name__ == "__main__":
    main()
