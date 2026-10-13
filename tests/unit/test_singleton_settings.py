#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Exercise selector-free global settings with the production persistence code."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

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
#define SETTINGS_MAX_NAME_LEN 64
#define SETTINGS_MAX_VAL_LEN 256
#define KP_RGB_BRT_MAX 100
#define KP_RGB_HUE_MAX 360
#define KP_RGB_SAT_MAX 100
#define IS_ENABLED(x) (x)
#define DT_DRV_INST(i) (i)
#define DT_INST(i, c) (i)
#define DT_CHILD_NUM_STATUS_OKAY(n) 0
#define DT_NUM_INST_STATUS_OKAY(c) 0
#define DT_INST_FOREACH_STATUS_OKAY(fn)
#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define CLAMP(v,l,h) ((v)<(l)?(l):((v)>(h)?(h):(v)))
#define ARG_UNUSED(x) ((void)(x))
#define BUILD_ASSERT(x,...) _Static_assert(x, #x)
#define snprintk snprintf
#define LOG_MODULE_DECLARE(...)
#define LOG_WRN(...) ((void)0)
#define K_MSEC(x) (x)
#define K_NO_WAIT 0

struct device { void *data; const char *name; const void *config; };
struct kp_rgb_hsb { uint16_t h; uint8_t s, b; };
struct kp_rgb_effect_common_data { struct kp_rgb_hsb color; uint32_t duration_ms; };
struct kp_rgb_effect_common_config { uint16_t slot; const char *persist_id; bool persist_parameters; };
struct kp_rgb_selector { const struct device *active_fx; size_t initial_effect; };
struct kp_rgb_global {
    bool user_on;
    uint8_t brightness;
    bool initial_on;
    int32_t initial_brightness;
};
struct kp_rgb_global kp_rgb_global;
extern struct kp_rgb_global kp_rgb_global;
static bool held;
static unsigned lock_entries, flushes, reconciles, resets;
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
static void kp_rgb_request_runtime_reset_locked(void) { assert(held); resets++; zmk_rgb_matrix_flush(); }
static void kp_rgb_selector_overlays_defaults_locked(void) { assert(held); }
static bool device_is_ready(const struct device *dev) { return dev != NULL; }
static struct kp_rgb_effect_common_data *kp_rgb_effect_data(const struct device *dev) { return dev->data; }
static const struct kp_rgb_effect_common_config *kp_rgb_effect_cfg(const struct device *dev) { return dev->config; }
static size_t kp_rgb_persist_selector_count(void) { return 0; }
static int kp_rgb_persist_ids_validate(void) { return 0; }
static const struct kp_rgb_persist_selector_ids *kp_rgb_persist_selector_at(size_t i,const struct device **owner) { (void)i; (void)owner; return NULL; }
static const struct device *kp_rgb_persist_member_at(size_t i,size_t j) { (void)i; (void)j; return NULL; }
static struct kp_rgb_selector *kp_rgb_selector_overlay_prepare_locked(const struct device *owner) { (void)owner; return NULL; }
static const struct device *kp_rgb_selector_at(const struct kp_rgb_selector *s,size_t i) { (void)s; (void)i; return NULL; }
static size_t kp_rgb_selector_calc_index(const struct kp_rgb_selector *s,size_t i,int d) { (void)s;(void)i;(void)d;return 0; }
static int kp_rgb_selector_select_locked(struct kp_rgb_selector *s,size_t i) { (void)s;(void)i;return 0; }

#define CONFIG_SETTINGS_SAVE_DEBOUNCE 100
struct k_work { void (*handler)(struct k_work *); };
struct k_work_delayable { struct k_work work; };
static struct k_work_delayable *pending;
static int scheduled_delay;
static int k_work_reschedule(struct k_work_delayable *work,int delay) { pending=work; scheduled_delay=delay; return 1; }
#define K_WORK_DELAYABLE_DEFINE(name,fn) static struct k_work_delayable name={{fn}}

typedef int (*settings_read_cb)(void *,void *,size_t);
#define SETTINGS_STATIC_HANDLER_DEFINE_WITH_CPRIO(name,path,get,set,commit,export,prio) \
    static const char *registered_subtree=path; \
    static int (*registered_set)(const char *,size_t,settings_read_cb,void *)=set; \
    static int (*registered_commit)(void)=commit; \
    _Static_assert(prio==INT_MAX-1,"settings commit priority")

struct stored { char path[65]; unsigned char bytes[256]; size_t len; };
struct op { char path[65]; bool deletion; int result; };
static struct stored values[32];
static struct op journal[256];
static unsigned journal_len, reads, saves, deletes, fail_count;
static void (*save_hook)(void);
static const char *fail_path;
static struct stored *lookup(const char *path) {
    for(size_t i=0;i<32;i++) if(values[i].len && !strcmp(values[i].path,path)) return &values[i];
    return NULL;
}
static void put(const char *path,const void *bytes,size_t len) {
    struct stored *v=lookup(path);
    if(!v) for(size_t i=0;i<32;i++) if(!values[i].len) {v=&values[i];break;}
    assert(v && len<=sizeof(v->bytes)); strcpy(v->path,path); memcpy(v->bytes,bytes,len); v->len=len;
}
static int record_io(const char *path,bool deletion) {
    assert(!held && journal_len<256); int rc=0;
    if(fail_count && !strcmp(path,fail_path)) {fail_count--;rc=-EIO;}
    struct op *op=&journal[journal_len++];strcpy(op->path,path);op->deletion=deletion;op->result=rc;
    if(save_hook) {void (*hook)(void)=save_hook;save_hook=NULL;hook();assert(!held);}
    return rc;
}
static int settings_save_one(const char *path,const void *data,size_t len) {
    saves++; int rc=record_io(path,false); if(!rc) put(path,data,len); return rc;
}
static int settings_read_cb_impl(void *arg,void *out,size_t len) { assert(!held);reads++;memcpy(out,arg,len);return (int)len; }
static int short_read(void *arg,void *out,size_t len) { assert(!held);memcpy(out,arg,len-1);return (int)len-1; }
static int failed_read(void *arg,void *out,size_t len) { (void)arg;(void)out;(void)len;assert(!held);return -EIO; }
static void run_work(void) { assert(pending && !held); struct k_work_delayable *w=pending;pending=NULL;w->work.handler(&w->work); }
'''

ENABLED_TESTS = r'''
#define GLOBAL "keypaw/rgb_matrix/state"
static const unsigned char legacy[36]={2,0,33,1,'r','e','t','a','i','n','e','d'};
static const unsigned char legacy_parameters[8]={100,0,90,0,32,3,0,0};
static void boot(void) {
    assert(!held);pending=NULL;kp_global_pending=kp_global_dirty=false;kp_global_revision=0;
    memset(&kp_global_load,0,sizeof(kp_global_load));
    kp_rgb_global.initial_on=true;kp_rgb_global.initial_brightness=70;
    assert(kp_rgb_apply_defaults()==0);
}
static void clean_boot(void) {memset(values,0,sizeof(values));journal_len=reads=saves=deletes=fail_count=0;save_hook=NULL;boot();}
static void assert_saved(bool on,unsigned brightness) {
    struct stored *v=lookup(GLOBAL);struct kp_rgb_persist_global_state g;
    assert(v && v->len==4 && kp_rgb_persist_decode_global(v->bytes,v->len,&g)==0);
    assert(g.user_on==on && g.brightness==brightness);
}
static void change_during_save(void) {
    assert(!held);kp_rgb_matrix_lock();kp_rgb_global.brightness=45;kp_rgb_matrix_unlock();
    assert(kp_rgb_save_state()==0);
}
static void reset_during_save(void) {assert(!held);kp_rgb_reset_state();}
static void rejection(bool current_first) {
    clean_boot();struct kp_rgb_persist_global_state g={false,88};struct kp_rgb_persist_global_record gr;
    assert(kp_rgb_persist_encode_global(&g,&gr)==0);
    if(current_first) assert(load("state",&gr,sizeof(gr))==0);
    unsigned before=reads,locks=lock_entries;
    assert(load("state",legacy,sizeof(legacy))==-EINVAL);
    assert(registered_set("state",sizeof(legacy),failed_read,(void *)legacy)==-EINVAL);
    assert(load("state/effects/any-effect",legacy_parameters,sizeof(legacy_parameters))==0);
    assert(registered_set("state/effects/any-effect",SIZE_MAX,failed_read,NULL)==0);
    assert(registered_set("unknown",SIZE_MAX,failed_read,NULL)==0);
    assert(reads==before && lock_entries==locks && !pending && !journal_len);
    assert(kp_rgb_global.user_on && kp_rgb_global.brightness==70);
    if(!current_first) assert(load("state",&gr,sizeof(gr))==0);
    assert(registered_commit()==0 && !kp_rgb_global.user_on && kp_rgb_global.brightness==88 && !reconciled_user_on);
    assert(registered_commit()==0 && !pending && !kp_global_dirty && !journal_len);
}
static void validation(void) {
    clean_boot();struct kp_rgb_persist_global_state g={false,66};struct kp_rgb_persist_global_record gr;
    assert(kp_rgb_persist_encode_global(&g,&gr)==0);
    assert(load("state",&gr,sizeof(gr))==0);
    struct kp_rgb_persist_global_record bad=gr;bad.version=2;
    assert(load("state",&bad,sizeof(bad))==-EINVAL);
    bad=gr;bad.user_on=2;assert(load("state",&bad,sizeof(bad))==-EINVAL);
    bad=gr;bad.brightness=101;assert(load("state",&bad,sizeof(bad))==-EINVAL);
    bad=gr;bad.reserved=1;assert(load("state",&bad,sizeof(bad))==-EINVAL);
    assert(load("state",&gr,3)==-EINVAL);
    assert(registered_set("state",4,short_read,&gr)==-EINVAL);
    assert(registered_set("state",4,failed_read,&gr)==-EIO);
    assert(registered_commit()==0 && !kp_rgb_global.user_on && kp_rgb_global.brightness==66 && !pending);
}
static void saves_and_reset(void) {
    clean_boot();put(GLOBAL,legacy,sizeof(legacy));
    put("keypaw/rgb_matrix/state/effects/a",legacy_parameters,sizeof(legacy_parameters));
    assert(load("state",legacy,sizeof(legacy))==-EINVAL);
    assert(registered_commit()==0 && !pending && !journal_len);
    kp_rgb_global.user_on=false;kp_rgb_global.brightness=21;
    assert(kp_rgb_save_state()==0 && pending && scheduled_delay==100);
    kp_rgb_global.brightness=22;assert(kp_rgb_save_state()==0 && scheduled_delay==100);
    fail_path=GLOBAL;fail_count=1;run_work();
    assert(pending && scheduled_delay==1000 && kp_global_dirty && lookup(GLOBAL)->len==36);
    run_work();assert(!pending && !kp_global_dirty);assert_saved(false,22);
    struct kp_rgb_persist_global_state staged={false,39};struct kp_rgb_persist_global_record gr;
    assert(kp_rgb_persist_encode_global(&staged,&gr)==0 && load("state",&gr,sizeof(gr))==0);
    unsigned before=resets;kp_rgb_reset_state();
    assert(resets==before+1 && pending && scheduled_delay==0 && !kp_global_pending);
    assert(registered_commit()==0 && kp_rgb_global.user_on && kp_rgb_global.brightness==70);
    fail_count=1;run_work();assert(pending && scheduled_delay==1000 && kp_global_dirty);
    run_work();assert(!pending && !kp_global_dirty);assert_saved(true,70);
    assert(lookup("keypaw/rgb_matrix/state/effects/a")->len==8 && !deletes);
    for(unsigned i=0;i<journal_len;i++) assert(!journal[i].deletion && !strcmp(journal[i].path,GLOBAL));
    boot();struct stored *v=lookup(GLOBAL);assert(load("state",v->bytes,v->len)==0);
    assert(registered_commit()==0 && kp_rgb_global.user_on && kp_rgb_global.brightness==70 && !pending);
}
static void interleavings(void) {
    clean_boot();kp_rgb_global.brightness=12;save_hook=change_during_save;
    assert(kp_rgb_save_state()==0);run_work();assert_saved(true,12);
    assert(pending && scheduled_delay==1000 && kp_global_dirty && kp_rgb_global.brightness==45);
    run_work();assert(!pending && !kp_global_dirty);assert_saved(true,45);
    kp_rgb_global.user_on=false;kp_rgb_global.brightness=18;save_hook=reset_during_save;
    assert(kp_rgb_save_state()==0);run_work();assert_saved(false,18);
    assert(pending && kp_global_dirty && kp_rgb_global.user_on && kp_rgb_global.brightness==70);
    run_work();assert(!pending && !kp_global_dirty);assert_saved(true,70);
    kp_rgb_global.brightness=11;save_hook=change_during_save;fail_path=GLOBAL;fail_count=1;
    assert(kp_rgb_save_state()==0);run_work();assert_saved(true,70);
    assert(pending && kp_global_dirty);run_work();assert_saved(true,45);assert(!pending && !kp_global_dirty);
}
int main(void) {
    assert(!strcmp(registered_subtree,"keypaw/rgb_matrix"));
    assert(sizeof(struct kp_rgb_persist_global_record)==4);
    rejection(false);rejection(true);validation();saves_and_reset();interleavings();
    puts("selector-free current global settings rejection/reset/revision retries passed");return 0;
}
'''

DISABLED_TESTS = r'''
int main(void) {
    kp_rgb_global.initial_on=true; kp_rgb_global.initial_brightness=70;
    assert(kp_rgb_settings_commit()==0);
    assert(kp_rgb_save_state()==0);
    assert(kp_rgb_save_selector_state(NULL,true)==0);
    kp_rgb_reset_state();
    assert(kp_rgb_global.user_on && kp_rgb_global.brightness==70 && !held);
    puts("settings-disabled global APIs passed");
    return 0;
}
'''


def without_includes(path):
    return re.sub(r"^#(?:include|pragma).*\n", "", path.read_text(), flags=re.M)


def compile_run(code, directory, name, settings_disabled=False):
    source = Path(directory) / f"{name}.c"
    binary = Path(directory) / name
    source.write_text(code)
    flags = ["-std=gnu11", "-Wall", "-Wextra", "-Werror"]
    if settings_disabled:
        flags += ["-Wno-unused-function", "-Wno-unused-variable"]
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + flags + [
        str(source), "-o", str(binary)
    ], check=True, timeout=20)
    subprocess.run([str(binary)], check=True, timeout=10)


def main():
    persist_h = without_includes(ROOT / "include/zmk/rgb_persist.h")
    persist_c = without_includes(ROOT / "src/rgb_persist.c")
    settings_c = without_includes(ROOT / "src/rgb_settings.c")
    with tempfile.TemporaryDirectory() as directory:
        enabled = (PRELUDE + "\n" + persist_h + "\n" + persist_c + "\n" + settings_c +
                   "\nstatic int load(const char *key,const void *bytes,size_t len) { return registered_set(key,len,settings_read_cb_impl,(void *)bytes); }\n" + ENABLED_TESTS)
        compile_run(enabled, directory, "global_settings")
        disabled_prelude = PRELUDE.replace("#define CONFIG_SETTINGS 1", "#define CONFIG_SETTINGS 0")
        disabled = disabled_prelude + "\n" + persist_h + "\n" + persist_c + "\n" + settings_c + "\n" + DISABLED_TESTS
        compile_run(disabled, directory, "global_settings_disabled", settings_disabled=True)


if __name__ == "__main__":
    main()
