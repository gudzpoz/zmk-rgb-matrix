#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Compile production providers with host geometry, color, RNG and DT stubs.

Provider bodies and math helpers are unchanged. Device declarations and enum
plumbing are excluded; firmware builds cover those. UBSan checks timing arithmetic.
"""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
EXAMPLE = ROOT.parent / "zmk-rgb-effect-example/src/effect.c"

MOCKS = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#define ARG_UNUSED(x) (void)(x)
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define CLAMP(a,b,c) MIN(MAX(a,b),c)
#define KP_RGB_HUE_MAX 360
#define KP_RGB_SAT_MAX 100
#define KP_RGB_SCALE(a,b) ((a)*(b)/100)
#define DT_HAS_COMPAT_STATUS_OKAY(x) 1
#define DT_INST_FOREACH_STATUS_OKAY(x)
#define DT_ENUM_CONST(p,v) p##_##v
struct kp_rgb_hsb {uint16_t h; uint8_t s,b;};
struct kp_rgb_effect_common_config {int index;};
struct kp_rgb_effect_common_data {struct kp_rgb_hsb color; uint32_t duration_ms;};
struct device {void *data; const void *config;};
struct led_rgb {uint8_t r,g,b;};
struct kp_rgb_coord {uint16_t x,y;};
struct kp_rgb_frame {
    int64_t now_ms;
    uint32_t elapsed_ms;
    size_t count;
    const size_t *targets;
    size_t target_count;
    struct led_rgb *scratch;
    const struct kp_rgb_coord *coords;
    struct led_rgb *pixels;
    uint16_t board_length,board_height;
};
struct kp_rgb_key_event {uint32_t position; bool pressed; int64_t timestamp_ms;};
struct kp_rgb_effect_callbacks {
    bool (*render)(const struct device*,const struct kp_rgb_frame*);
    bool (*on_event)(const struct device*,const struct kp_rgb_key_event*);
    void (*set_active)(const struct device*,bool,int64_t);
    void (*reset)(const struct device*,int64_t);
};
static const size_t kp_rgb_all_targets[4]={0,1,2,3};
static struct kp_rgb_coord coords[4]={{0,0},{100,100},{200,200},{300,300}};
static uint32_t rng=1, calls;
static uint32_t sys_rand32_get(void) {
    calls++;
    return rng=rng*1664525u+1013904223u;
}
static uint32_t kp_rgb_effect_period(const struct device *d) {
    return ((struct kp_rgb_effect_common_data*)d->data)->duration_ms;
}
static struct kp_rgb_hsb kp_rgb_hsb_scale(struct kp_rgb_hsb c,uint8_t p) {
    c.b=c.b*p/100;
    return c;
}
static struct led_rgb kp_rgb_hsb_to_rgb(struct kp_rgb_hsb c) {
    return (struct led_rgb){c.h%256,c.s,c.b};
}
static struct led_rgb kp_rgb_rgb_scale(struct led_rgb c,uint8_t p) {
    c.r=c.r*p/100; c.g=c.g*p/100; c.b=c.b*p/100;
    return c;
}
static struct led_rgb kp_hex_to_rgb(uint32_t c) {
    return (struct led_rgb){c>>16,c>>8,c};
}
static size_t kp_rgb_led_for_position(uint32_t p) {return p;}
static const struct kp_rgb_coord *kp_rgb_led_coord(size_t p) {
    return p<KP_LED_COUNT?&coords[p]:NULL;
}
'''

VARIANTS = {"rain": 5, "breathe": 5, "rainbow": 56, "band": 6,
            "solid": 3, "starlight": 8}


def configuration(name, variant):
    if name == "rain":
        return f"cfg.mode={variant}; cfg.step_interval_ms=32;"
    if name == "breathe":
        return f"cfg.mode={variant};"
    if name == "rainbow":
        return (f"cfg.basis={variant % 7}; cfg.direction={(variant // 7) % 4};"
                f"cfg.palette={variant // 28};")
    if name == "band":
        return f"cfg.channel={variant % 2}; cfg.shape={variant // 2};"
    if name == "solid":
        return f"cfg.axis={variant};"
    if name == "starlight":
        return (f"cfg.step_interval_ms=32; cfg.smooth={variant & 1}; cfg.dual_hue={(variant >> 1) & 1};"
                f"cfg.dual_sat={(variant >> 2) & 1};")
    if name == "heatmap":
        return "cfg.decrease_delay_ms=25; cfg.increase_step=32; cfg.slim=true;"
    if name == "reactive":
        return "cfg.multi=true;"
    return ""


def provider_source(path):
    text = re.sub(r"^#include .*\n", "", path.read_text(), flags=re.M)
    text = re.sub(r"^#define KP_EFF_\w+_DEFINE\(inst\).*?(?=\n\n)",
                  "", text, flags=re.M | re.S)

    def enum(match):
        typ, prop, *values = (part.strip() for part in match[1].split(","))
        return "typedef enum {" + ", ".join(prop + "_" + v for v in values) + "} " + typ + ";"

    return re.sub(r"DEFINE_DT_ENUM\((.*?)\);", enum, text)


def target_tests(name):
    return f'''
    const size_t selections[4][4]={{{{0,1,2,3}},{{3,1,2,0}},{{3,1,0,0}},{{0,0,0,0}}}};
    const size_t lengths[4]={{KP_LED_COUNT,KP_LED_COUNT,KP_LED_COUNT ? 2 : 0,0}};
    struct kp_eff_{name}_data reference={{0}};
    struct led_rgb expected_pixels[4];
    uint32_t expected_rng=0, expected_calls=0;
    bool expected_running=false;
    for (size_t selection=0;selection<4;selection++) {{
        memset(&data,0,sizeof(data)); data.common=common; rng=1; calls=0;
        if(cb->reset) cb->reset(&dev,0);
        if(cb->set_active) cb->set_active(&dev,true,0);
        f.targets=selections[selection]; f.target_count=lengths[selection];
        for(size_t step=0;step<12;step++) {{
            if(cb->on_event && KP_LED_COUNT) {{
                struct kp_rgb_key_event ev={{step%(KP_LED_COUNT ? KP_LED_COUNT : 1),true,step*32}};
                cb->on_event(&dev,&ev);
            }}
            memset(pixels,0xa5,sizeof(pixels));
            for(size_t j=0;j<f.target_count;j++) pixels[f.targets[j]]=(struct led_rgb){{0}};
            f.now_ms=step*32; f.elapsed_ms=step ? 32 : 0;
            running=cb->render(&dev,&f);
            for(size_t led=0;led<4;led++) {{
                bool selected=false;
                for(size_t j=0;j<f.target_count;j++) selected |= f.targets[j]==led;
                if(!selected) assert(pixels[led].r==0xa5 && pixels[led].g==0xa5 && pixels[led].b==0xa5);
            }}
        }}
        if(!selection) {{
            reference=data; expected_rng=rng; expected_calls=calls; expected_running=running;
            memcpy(expected_pixels,pixels,sizeof(pixels));
        }} else {{
            assert(memcmp(&reference,&data,sizeof(data))==0);
            assert(rng==expected_rng && calls==expected_calls && running==expected_running);
            for(size_t j=0;j<f.target_count;j++) {{
                size_t led=f.targets[j];
                assert(memcmp(&pixels[led],&expected_pixels[led],sizeof(pixels[led]))==0);
            }}
        }}
    }}
    f.targets=kp_rgb_all_targets; f.target_count=KP_LED_COUNT;
    memset(&data,0,sizeof(data)); data.common=common;
'''


def test_body(name, count, variant):
    expected = ("false" if name in ("solid", "static") else
                "(KP_LED_COUNT>0)" if name in ("heatmap", "reactive", "ripple") else "true")
    body = f'''
int main(void) {{
    struct kp_eff_{name}_config cfg={{0}};
    {configuration(name, variant)}
    struct kp_eff_{name}_data data={{.common={{.color={{120,90,80}},.duration_ms=1000}}}};
    struct device dev={{&data,&cfg}};
    const struct kp_rgb_effect_callbacks *cb=&kp_eff_{name}_callbacks;
    struct kp_rgb_effect_common_data common=data.common;
    struct led_rgb pixels[4]={{0}}, saved[4];
    struct kp_rgb_frame f={{.now_ms=100,.count=KP_LED_COUNT,.coords=coords,
        .pixels=pixels,.targets=kp_rgb_all_targets,.target_count=KP_LED_COUNT,
        .scratch=NULL,.board_length=300,.board_height=300}};
    if (cb->reset) cb->reset(&dev,100);
    assert(memcmp(&common,&data.common,sizeof(common))==0);
    if (cb->set_active) cb->set_active(&dev,true,100);
    if (cb->on_event) {{
        struct kp_rgb_key_event ev={{0,true,100}};
        assert(cb->on_event(&dev,&ev)==(KP_LED_COUNT>0));
        ev.pressed=false; assert(!cb->on_event(&dev,&ev));
        ev.pressed=true; ev.position=UINT32_MAX; assert(!cb->on_event(&dev,&ev));
    }}
    bool running=cb->render(&dev,&f);
    assert(running=={expected});
    memcpy(saved,pixels,sizeof(saved));
    struct kp_eff_{name}_data snapshot=data;
    uint32_t oldcalls=calls;
    assert(cb->render(&dev,&f)==running);
    assert(memcmp(saved,pixels,sizeof(saved))==0);
    assert(memcmp(&snapshot,&data,sizeof(data))==0);
    assert(calls==oldcalls);
    f.elapsed_ms=32; f.now_ms=132; cb->render(&dev,&f);
    f.elapsed_ms=0; snapshot=data; memcpy(saved,pixels,sizeof(saved)); oldcalls=calls;
    cb->render(&dev,&f);
    assert(memcmp(&snapshot,&data,sizeof(data))==0);
    assert(memcmp(saved,pixels,sizeof(saved))==0);
    assert(calls==oldcalls);
'''
    body += target_tests(name)
    if count and name in ("heatmap", "reactive"):
        array = "temp" if name == "heatmap" else "levels"
        fresh, cooled, older = (32, 31, 28) if name == "heatmap" else (255, 249, 230)
        body += f'''
    cb->reset(&dev,100); cb->set_active(&dev,true,100);
    struct kp_rgb_key_event ev={{0,true,100}}; cb->on_event(&dev,&ev);
    for (int i=1;i<=25;i++) {{f.now_ms=100+i; f.elapsed_ms=1; cb->render(&dev,&f);}}
    assert(data.{array}[0]=={cooled});
    ev.position=1; ev.timestamp_ms=200; cb->on_event(&dev,&ev);
    assert(data.{array}[0]=={cooled}); assert(data.{array}[1]==0);
    f.now_ms=200; f.elapsed_ms=75; cb->render(&dev,&f);
    assert(data.{array}[0]=={older});
    assert(data.{array}[1]=={fresh});
    f.now_ms=100000; f.elapsed_ms=99800; assert(!cb->render(&dev,&f));
    ev.timestamp_ms=100001; cb->on_event(&dev,&ev);
    f.now_ms=100001; f.elapsed_ms=0; assert(cb->render(&dev,&f));
    assert(data.{array}[1]=={fresh});
    cb->set_active(&dev,false,100001); assert(data.{array}[1]==0);
    assert(memcmp(&common,&data.common,sizeof(common))==0);
    int64_t epoch=(int64_t)UINT32_MAX+10000;
    cb->reset(&dev,epoch); cb->set_active(&dev,true,epoch);
    ev.timestamp_ms=epoch+1000; cb->on_event(&dev,&ev);
    f.now_ms=epoch+1000; f.elapsed_ms=0; assert(cb->render(&dev,&f));
    assert(data.{array}[1]=={fresh});
    f.now_ms=epoch+100000; f.elapsed_ms=99000; assert(!cb->render(&dev,&f));
    cb->reset(&dev,f.now_ms); assert(!cb->render(&dev,&f));

    ev.position=0; ev.timestamp_ms=epoch; assert(cb->on_event(&dev,&ev));
    assert(data.{array}[0]==0);
    f.now_ms=epoch; f.elapsed_ms=0; assert(cb->render(&dev,&f));
    f.now_ms+=1000000; assert(cb->render(&dev,&f));
    assert(data.{array}[0]=={fresh});
    snapshot=data; memcpy(saved,pixels,sizeof(saved));
    f.now_ms+=1000000; assert(cb->render(&dev,&f));
    assert(memcmp(&snapshot,&data,sizeof(data))==0);
    assert(memcmp(saved,pixels,sizeof(saved))==0);
    f.elapsed_ms=1; f.now_ms++; cb->render(&dev,&f);
    uint32_t fraction=data.remainder[0]; assert(fraction!=0);
    f.elapsed_ms=0; f.now_ms+=1000000; cb->render(&dev,&f);
    assert(data.remainder[0]==fraction); assert(data.{array}[0]=={fresh});

    ev.position=1; ev.timestamp_ms=f.now_ms+100000;
    assert(cb->on_event(&dev,&ev));
    ev.position=UINT32_MAX; ev.timestamp_ms=INT64_MIN;
    f.now_ms+=100000; f.elapsed_ms=100000; assert(cb->render(&dev,&f));
    assert(data.{array}[0]==0); assert(data.{array}[1]=={fresh});
    assert(data.pending_count==0);

    cb->reset(&dev,0); ev.position=0; ev.timestamp_ms=INT64_MIN;
    assert(cb->on_event(&dev,&ev));
    f.now_ms=epoch; f.elapsed_ms=25; cb->render(&dev,&f);
    assert(data.{array}[0]=={cooled});
    cb->reset(&dev,0); ev.timestamp_ms=INT64_MAX;
    assert(cb->on_event(&dev,&ev));
    ev.position=1; ev.timestamp_ms=INT64_MIN; assert(cb->on_event(&dev,&ev));
    f.now_ms=epoch; f.elapsed_ms=100000; cb->render(&dev,&f);
    assert(data.{array}[0]=={fresh}); assert(data.{array}[1]=={fresh});
    cb->reset(&dev,0); ev.position=0; ev.timestamp_ms=INT64_MIN;
    assert(cb->on_event(&dev,&ev));
    f.now_ms=0; f.elapsed_ms=UINT32_MAX; cb->render(&dev,&f);
    assert(data.{array}[0]=={fresh});
    f.now_ms=INT64_MAX; f.elapsed_ms=UINT32_MAX; assert(!cb->render(&dev,&f));

    cb->reset(&dev,0); ev.position=0; ev.timestamp_ms=0;
    for (int i=0;i<16;i++) assert(cb->on_event(&dev,&ev));
    assert(!cb->on_event(&dev,&ev)); assert(data.{array}[0]==0);
    assert(data.pending_count==16);
    cb->set_active(&dev,false,0); assert(data.pending_count==0);
    f.now_ms=0; f.elapsed_ms=0; assert(!cb->render(&dev,&f));
    assert(cb->on_event(&dev,&ev)); cb->reset(&dev,0);
    assert(data.pending_count==0); assert(!cb->render(&dev,&f));
'''
    if count and name == "ripple":
        body += r'''
    cb->reset(&dev,0);
    struct kp_rgb_key_event ev={0,true,100}; cb->on_event(&dev,&ev);
    f.now_ms=100; f.elapsed_ms=0; f.board_length=0; assert(cb->render(&dev,&f));
    f.now_ms=1099; assert(cb->render(&dev,&f));
    f.now_ms=1100; assert(!cb->render(&dev,&f));
    cb->reset(&dev,0); ev.timestamp_ms=100; cb->on_event(&dev,&ev);
    f.now_ms=600; f.board_length=300; assert(cb->render(&dev,&f));
    assert(data.triggers[0].start_ms==100);
    f.now_ms=1100; assert(!cb->render(&dev,&f));
    ev.timestamp_ms=INT64_MIN; cb->on_event(&dev,&ev);
    f.now_ms=INT64_MAX; assert(!cb->render(&dev,&f));
    ev.timestamp_ms=INT64_MAX; cb->on_event(&dev,&ev);
    f.now_ms=0; assert(cb->render(&dev,&f));
    cb->set_active(&dev,false,0); assert(!cb->render(&dev,&f));
    ev.timestamp_ms=(int64_t)UINT32_MAX+10000; cb->on_event(&dev,&ev);
    f.now_ms=ev.timestamp_ms+999; assert(cb->render(&dev,&f));
    f.now_ms++; assert(!cb->render(&dev,&f));
    ev.timestamp_ms=f.now_ms; cb->on_event(&dev,&ev);
    cb->reset(&dev,f.now_ms); assert(!cb->render(&dev,&f));
'''
    if count and name in ("rain", "starlight"):
        total = ("(cfg.mode==mode_drops ? 125u : cfg.mode==mode_fractal ? 1007u : 8u*interval+interval/2)"
                 if name == "rain" else "8u*interval+interval/2")
        body += f'''
    for (uint32_t interval=1; interval<=65535; interval=(interval==1 ? 32 : 65535)) {{
        cfg.step_interval_ms=interval;
        uint32_t total={total};
        cb->reset(&dev,0); rng=1; f.now_ms=0; f.elapsed_ms=0; cb->render(&dev,&f);
        f.elapsed_ms=total; f.now_ms=total; cb->render(&dev,&f);
        struct kp_eff_{name}_data bulk=data; uint32_t bulk_rng=rng;
        memcpy(saved,pixels,sizeof(saved));
        cb->reset(&dev,0); rng=1; f.now_ms=0; f.elapsed_ms=0; cb->render(&dev,&f);
        for (uint32_t t=0; t<total;) {{
            uint32_t delta=MIN(total-t, (t%7)+1);
            t+=delta; f.elapsed_ms=delta; f.now_ms=t; cb->render(&dev,&f);
            f.elapsed_ms=0; cb->render(&dev,&f);
        }}
        assert(memcmp(&bulk,&data,sizeof(data))==0); assert(rng==bulk_rng);
        assert(memcmp(saved,pixels,sizeof(saved))==0);
        if (interval==65535) break;
    }}
    cfg.step_interval_ms=32;
    cb->reset(&dev,0); rng=1; calls=0; f.now_ms=0; f.elapsed_ms=0; cb->render(&dev,&f);
    uint32_t seed_calls=calls;
    f.elapsed_ms=1000*32+7; f.now_ms=f.elapsed_ms; cb->render(&dev,&f);
    assert(calls-seed_calls<=8u*KP_LED_COUNT*4u);
'''
        if name == "rain":
            body += r'''
    if (cfg.mode==mode_fractal) assert(data.phase_ms==7);
    else if (cfg.mode==mode_drops) assert(data.drop_remainder==(32007u*64u)%1000u);
    else {
        assert(data.step_remainder_ms==7);
        for (size_t i=0;i<KP_LED_COUNT;i++) assert(data.val[i]==0);
    }
    cb->reset(&dev,0); cfg.mode=mode_flow; cfg.step_interval_ms=65535;
    data.val[0]=255; f.elapsed_ms=1;
    for (int i=0;i<1000;i++) cb->render(&dev,&f);
    assert(data.val[0]==0);
    cb->reset(&dev,0); cfg.mode=mode_drops; f.elapsed_ms=0; cb->render(&dev,&f);
    calls=0; f.elapsed_ms=125; cb->render(&dev,&f);
    assert(calls==16); assert(data.drop_remainder==0);
    cb->reset(&dev,0); cfg.mode=mode_flow; cfg.step_interval_ms=17; calls=0;
    f.elapsed_ms=16; cb->render(&dev,&f); assert(calls==0); assert(data.flow_idx==0);
    f.elapsed_ms=1; cb->render(&dev,&f); assert(calls==1);
    assert(data.flow_idx==1 && data.val[1]==255);
    cfg.mode=mode_fractal; f.elapsed_ms=UINT32_MAX; cb->render(&dev,&f);
    assert(data.phase_ms==UINT32_MAX%1000u);
'''
        else:
            body += r'''
    assert(data.step_remainder_ms==7);
    for (size_t i=0;i<KP_LED_COUNT;i++) assert(data.cur[i]==data.target[i]);
    cb->reset(&dev,0); cfg.smooth=true; cfg.step_interval_ms=65535;
    data.target[0]=99; f.elapsed_ms=1;
    for (int i=0;i<388;i++) cb->render(&dev,&f);
    assert(data.cur[0]==98);
    cb->render(&dev,&f); assert(data.cur[0]==99);
    cb->reset(&dev,0); cfg.step_interval_ms=17; calls=0;
    f.elapsed_ms=16; cb->render(&dev,&f); assert(calls==0);
    f.elapsed_ms=1; cb->render(&dev,&f);
    assert(calls>=KP_LED_COUNT && calls<=4u*KP_LED_COUNT);
    assert(data.step_remainder_ms==0);
'''
        body += r'''
    f.elapsed_ms=UINT32_MAX; f.now_ms+=UINT32_MAX; cb->render(&dev,&f);
    snapshot=data; oldcalls=calls; f.elapsed_ms=0; cb->render(&dev,&f);
    assert(memcmp(&snapshot,&data,sizeof(data))==0); assert(calls==oldcalls);
'''
    if count and name == "digital_rain":
        body += r'''
    cb->reset(&dev,0); f.elapsed_ms=0; cb->render(&dev,&f);
    for (size_t c=0;c<KP_DIGITAL_COLS;c++) {data.head_q8[c]=0; data.speed_q8[c]=c+1;}
    snapshot=data; oldcalls=calls;
    f.elapsed_ms=100; cb->render(&dev,&f);
    struct kp_eff_digital_rain_data bulk=data;
    for (size_t c=0;c<KP_DIGITAL_COLS;c++) assert(data.head_q8[c]==100*(int32_t)(c+1));
    data=snapshot; f.elapsed_ms=1;
    for (int i=0;i<100;i++) cb->render(&dev,&f);
    assert(memcmp(&bulk,&data,sizeof(data))==0); assert(calls==oldcalls);
    cb->reset(&dev,0);
    assert(!data.seeded);
    coords[3]=(struct kp_rgb_coord){UINT16_MAX,UINT16_MAX};
    data.common.duration_ms=1;
    f.elapsed_ms=0; cb->render(&dev,&f);
    oldcalls=calls; f.elapsed_ms=UINT32_MAX; f.now_ms+=UINT32_MAX; cb->render(&dev,&f);
    assert(calls-oldcalls<=8u*KP_DIGITAL_COLS*2u);
    for (size_t c=0;c<KP_DIGITAL_COLS;c++) {
        assert(data.head_q8[c]<=KP_RAIN_TO_Q8((int32_t)data.max_y+300));
        assert(data.head_q8[c]>=KP_RAIN_TO_Q8((int32_t)data.min_y-(data.max_y-data.min_y)));
    }
    snapshot=data; oldcalls=calls; f.elapsed_ms=0; cb->render(&dev,&f);
    assert(memcmp(&snapshot,&data,sizeof(data))==0); assert(calls==oldcalls);
    data.common=common;
'''
    if name in ("band", "breathe", "rainbow", "spectrum", "example"):
        body += r'''
    cb->reset(&dev,0); assert(data.phase_ms==0);
    f.elapsed_ms=UINT32_MAX; f.now_ms=(int64_t)UINT32_MAX+10000;
    assert(cb->render(&dev,&f)); assert(data.phase_ms==UINT32_MAX%1000u);
    uint32_t phase=data.phase_ms;
    f.elapsed_ms=0; f.now_ms+=100000;
    if (cb->set_active) {cb->set_active(&dev,false,f.now_ms); cb->set_active(&dev,true,f.now_ms);}
    cb->render(&dev,&f); assert(data.phase_ms==phase);
    cb->reset(&dev,f.now_ms); assert(data.phase_ms==0);
'''
    body += '''
    assert(memcmp(&common,&data.common,sizeof(common))==0);
    return 0;
}
'''
    return body


def main():
    paths = sorted((ROOT / "src/effects").glob("*.c"))
    if EXAMPLE.exists():
        paths.append(EXAMPLE)
    else:
        print(f"SKIP external example: {EXAMPLE} is not available")
    math = (ROOT / "include/zmk/rgb_matrix_math.h").read_text().replace("#pragma once", "")
    compiler = shlex.split(os.environ.get("CC", "cc"))
    cases = 0
    with tempfile.TemporaryDirectory(prefix="rgb-effect-timing-") as tmp:
        for path in paths:
            name = path.stem if path != EXAMPLE else "example"
            provider = provider_source(path)
            variants = VARIANTS.get(name, 1)
            for count in (0, 4):
                for variant in range(variants):
                    source = Path(tmp) / f"{name}_{count}_{variant}.c"
                    binary = source.with_suffix("")
                    source.write_text(f"#define KP_LED_COUNT {count}\n" + MOCKS + math + provider
                                      + test_body(name, count, variant))
                    subprocess.run(compiler + ["-std=c11", "-Wall", "-Wextra", "-Werror",
                                   "-Wno-unused-function", "-Wno-type-limits", "-Wno-unused-variable",
                                   "-Wno-missing-field-initializers", "-fsanitize=undefined",
                                   "-fno-sanitize-recover=all", str(source), "-o", str(binary)], check=True)
                    subprocess.run([str(binary)], check=True)
                    cases += 1
            print(f"{name}: {variants} variants, zero/nonzero LED builds passed")
    print(f"effect timing: {cases} host cases passed (UBSan)")


if __name__ == "__main__":
    main()
