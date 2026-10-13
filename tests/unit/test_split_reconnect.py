#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Production global reconnect and canonical selector snapshot transport."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
from test_singleton import ROOT, function
from test_singleton_split import fixture

TESTS = r'''
int main(void) {
 kp_rgb_global.brightness=37; kp_rgb_global.user_on=false;
 available_count=0; kp_rgb_sync_refresh(); assert(!sends);
 now_ms=1000; available_count=2; kp_rgb_sync_refresh();
 assert(kp_rgb_sync_sources[0].ready_at==1100 && kp_rgb_sync_sources[1].ready_at==1100);
 struct kp_rgb_selector_snapshot snapshot={.control=&owners[0],.slot=1,.color={222,77,100},.period_ms=3456};
 struct kp_rgb_selector_snapshot original=snapshot;
 fail_source[1]=true; send_cost=7;
 assert(kp_rgb_split_send_selector(1,&snapshot,0)==-EIO);
 assert(bindings[0].behavior_dev==owners[0].name && bindings[0].param1==KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_COLOR_CMD,1));
 assert(bindings[0].param2==RGB_COLOR_HSB_VAL(222,77,100) && destinations[0]==1 && now_ms==1007);
 assert(!memcmp(&snapshot,&original,sizeof(snapshot)) && !held);
 fail_source[1]=false;
 assert(kp_rgb_split_send_selector(0,&snapshot,1)==0);
 assert(bindings[1].param1==KP_RGB_SELECTOR_MEMBER_COMMAND(RGB_SELECTOR_PERIOD_CMD,1) && bindings[1].param2==3456);
 assert(kp_rgb_split_send_selector(0,&snapshot,2)==0);
 assert(bindings[2].param1==RGB_EFS_CMD && bindings[2].param2==1);
 unsigned before=sends;
 assert(kp_rgb_split_send_selector(0,&snapshot,3)==-EINVAL && sends==before);
 send_cost=0; kp_rgb_global.brightness=37; kp_rgb_global.user_on=false;
 available_count=1; ids[0]=0;
 now_ms=1100; kp_rgb_sync_step_handler(NULL);
 assert(sends==before+1 && bindings[before].param1==RGB_BRT_SET_CMD && bindings[before].param2==37);
 kp_rgb_global.brightness=99; kp_rgb_global.user_on=true;
 now_ms+=50; kp_rgb_sync_step_handler(NULL);
 assert(bindings[sends-1].param1==RGB_OFF_CMD);
 now_ms+=50; kp_rgb_sync_step_handler(NULL);
 assert(bindings[sends-1].param1==RGB_OVL_STATE_CMD && bindings[sends-1].param2==RGB_OVL_STATE_VAL(0,0x1234));
 now_ms+=50; kp_rgb_sync_step_handler(NULL);
 assert(bindings[sends-1].param2==RGB_OVL_STATE_VAL(1,0xabcd));
 now_ms+=50; kp_rgb_sync_step_handler(NULL);
 assert(!kp_rgb_sync_sources[0].pending && !kp_rgb_sync_sources[1].pending);
 available_count=1; ids[0]=1; kp_rgb_sync_refresh(); assert(!kp_rgb_sync_sources[0].seen);
 ids[0]=0; kp_rgb_sync_refresh(); assert(kp_rgb_sync_sources[0].pending);
 now_ms+=100; kp_rgb_sync_step_handler(NULL);
 assert(bindings[sends-1].param2==99);
 selected_transport=&transports[1]; kp_rgb_sync_refresh();
 assert(kp_rgb_sync_sources[0].ready_at==now_ms+100 && !kp_rgb_sync_sources[0].global_captured);
 before=sends; endpoint.ready=false; now_ms+=100; kp_rgb_sync_step_handler(NULL); assert(sends==before);
 endpoint.ready=true; fail_source[0]=true; send_cost=11;
 for(unsigned n=0;n<8;n++) {
   now_ms=kp_rgb_sync_sources[0].ready_at;
   kp_rgb_sync_step_handler(NULL);
 }
 assert(kp_rgb_sync_sources[0].ready_at-now_ms==1000);
 before=sends; kp_rgb_sync_clock_handler(NULL);
 assert(sends==before+1 && bindings[before].param1==RGB_CLOCK_CMD);
 assert(!held); puts("global reconnect, offline settling, canonical selector transport and capped retries passed");
 return 0;
}
'''


def main():
    code = fixture(selectors=False)
    source = (ROOT / 'src/rgb_selector_sync.c').read_text()
    header = (ROOT / 'src/rgb_selector_sync_internal.h').read_text()
    code = code.replace(function(code, 'kp_rgb_selector_snapshot_binding'),
                        function(source, 'kp_rgb_selector_snapshot_binding'))
    definitions = header[header.index('#define RGB_SELECTOR_COLOR_CMD'):header.index('struct kp_rgb_selector_snapshot')]
    code = '#define RGB_EFS_CMD 12\n#define RGB_COLOR_HSB_VAL(h,s,b) (((uint32_t)(h)<<16)|((s)<<8)|(b))\n' + definitions + code
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / 'reconnect.c'
        path.write_text(code + TESTS)
        binary = Path(tmp) / 'reconnect'
        subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
            '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
            str(path), '-o', str(binary)], check=True, timeout=20)
        subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    main()
