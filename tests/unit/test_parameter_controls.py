#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Ready-effect validation independent of selection, with production controls."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
from test_selector_controls import production_fixture, TESTS as CONTROL_TESTS
from test_singleton import function

TESTS = r'''
int main(void) {
 setup();
 const struct device *private_fx=&effects[4];
 struct kp_rgb_hsb c={360,100,100};
 struct kp_rgb_global global=kp_rgb_global;
 size_t a=data_a.selector.effect_index,b=data_b.selector.effect_index;
 assert(zmk_rgb_matrix_set_color(NULL,&c)==-ENODEV);
 assert(zmk_rgb_matrix_set_period(NULL,100)==-ENODEV);
 assert(zmk_rgb_matrix_set_color(private_fx,NULL)==-EINVAL);
 unsigned f=flushes;
 assert(zmk_rgb_matrix_set_color(private_fx,&c)==0 && parameters[4].color.h==0 && flushes==f+1);
 assert(zmk_rgb_matrix_set_color(private_fx,&c)==0 && flushes==f+1);
 c.h=361; assert(zmk_rgb_matrix_set_color(private_fx,&c)==-EINVAL);
 c.h=0; c.s=101; assert(zmk_rgb_matrix_set_color(private_fx,&c)==-EINVAL);
 c.s=0; c.b=101; assert(zmk_rgb_matrix_set_color(private_fx,&c)==-EINVAL);
 c.b=0; assert(zmk_rgb_matrix_set_color(private_fx,&c)==0 && parameters[4].color.b==100);
 uint32_t bad[]={0,99,10001,65536+100,0x80000000u,UINT32_MAX};
 for(size_t i=0;i<ARRAY_SIZE(bad);i++) assert(zmk_rgb_matrix_set_period(private_fx,bad[i])==-EINVAL);
 assert(zmk_rgb_matrix_set_period(private_fx,100)==0);
 f=flushes; assert(zmk_rgb_matrix_set_period(private_fx,100)==0 && flushes==f);
 assert(zmk_rgb_matrix_set_period(private_fx,10000)==0);
 effects[4].ready=false; f=flushes;
 assert(zmk_rgb_matrix_set_color(private_fx,&c)==-ENODEV);
 assert(zmk_rgb_matrix_set_period(private_fx,100)==-ENODEV && flushes==f);
 effects[4].ready=true;
 struct device foreign={.ready=true}; configs[4].owner=&foreign;
 c=(struct kp_rgb_hsb){47,66,88};
 assert(zmk_rgb_matrix_set_color(private_fx,&c)==0 && parameters[4].color.h==47 && parameters[4].color.s==66 && parameters[4].color.b==100);
 assert(zmk_rgb_matrix_set_period(private_fx,2345)==0);
 configs[4].owner=NULL;
 data_a.selector.active_fx=NULL; data_b.selector.active_fx=NULL;
 assert(zmk_rgb_matrix_set_period(private_fx,3456)==0);
 assert(data_a.selector.effect_index==a && data_b.selector.effect_index==b);
 assert(!memcmp(&global,&kp_rgb_global,sizeof(global)) && saves==0 && scoped_saves==0 && power_reconciles==0 && callbacks==0);
 isr=true; unsigned l=locks;
 assert(zmk_rgb_matrix_set_color(private_fx,&c)==-EWOULDBLOCK);
 assert(zmk_rgb_matrix_set_period(private_fx,100)==-EWOULDBLOCK);
 assert(locks==l && !held); isr=false;
 setup(); parameters[0].duration_ms=9999;
 assert(apply(0,RGB_SPI_CMD,0)==0 && parameters[0].duration_ms==10000);
 assert(apply(0,RGB_SPD_CMD,0)==0 && parameters[0].duration_ms==100);
 test_arguments(); test_alias_brightness(); test_isr();
 puts("ready-effect parameters, selector-local bounds, alias and brightness controls passed");
 return 0;
}
'''


def main():
    code = production_fixture().removesuffix(CONTROL_TESTS)
    code += CONTROL_TESTS[:CONTROL_TESTS.index('int main(void)')]
    code = code.replace('#define CONFIG_KEYPAW_RGB_MATRIX_DURATION_STEP 500',
                        '#define CONFIG_KEYPAW_RGB_MATRIX_DURATION_STEP 100000')
    with tempfile.TemporaryDirectory() as tmp:
        source = Path(tmp) / 'parameters.c'
        source.write_text(code + TESTS)
        for alias in (0, 1):
            binary = Path(tmp) / f'parameters-{alias}'
            subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
                '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                f'-DHAS_ALIAS={alias}', '-DCENTRAL_SYNC=0', str(source), '-o', str(binary)],
                check=True, timeout=20)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    main()
