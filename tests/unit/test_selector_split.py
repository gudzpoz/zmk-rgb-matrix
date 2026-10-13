#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Production per-source/selector revisions, frozen progress, retries and fairness."""
from test_singleton_split import run

TESTS = r'''
int main(void) {
 selected_slots[0] = 3; selected_slots[1] = 8;
 kp_rgb_sync_refresh();
 kp_rgb_sync_selector_step_handler(NULL); assert(!sends);
 now_ms = 100; fail_source[0] = true; send_cost = 7;
 kp_rgb_sync_selector_step_handler(NULL);
 assert(sends == 1 && destinations[0] == 0 && bindings[0].param2 == 3);
 assert(kp_rgb_sync_sources[0].selectors[0].ready_at == 157);
 assert(kp_rgb_sync_sources[0].selectors[0].step == 0);
 kp_rgb_matrix_lock(); selected_slots[0] = 9;
 kp_rgb_split_selector_dirty_locked(&owners[0]); kp_rgb_matrix_unlock();
 assert(kp_rgb_sync_selectors[0].revision == 1 && kp_rgb_sync_selectors[1].revision == 0);
 kp_rgb_sync_selector_step_handler(NULL);
 assert(destinations[1] == 0 && !strcmp(bindings[1].behavior_dev,"selector-b"));
 kp_rgb_sync_selector_step_handler(NULL);
 assert(destinations[2] == 1 && !strcmp(bindings[2].behavior_dev,"selector-a"));
 kp_rgb_sync_selector_step_handler(NULL);
 assert(destinations[3] == 1 && !strcmp(bindings[3].behavior_dev,"selector-b"));
 assert(kp_rgb_sync_sources[1].selectors[0].step == 1);
 now_ms = 157; fail_source[0] = false; send_cost = 0;
 kp_rgb_sync_selector_step_handler(NULL);
 assert(destinations[4] == 0 && bindings[4].param1 == bindings[0].param1);
 assert(bindings[4].param2 == 3 && captures[0] == 2);
 assert(kp_rgb_sync_sources[0].selectors[0].retry_count == 0);
 unsigned guard = 0;
 while ((!kp_rgb_sync_sources[0].selectors[0].synced ||
         kp_rgb_sync_sources[0].selectors[0].revision != 1 ||
         kp_rgb_sync_sources[0].selectors[0].pending ||
         !kp_rgb_sync_sources[1].selectors[1].synced) && guard++ < 40) {
  now_ms += 50; kp_rgb_sync_selector_step_handler(NULL);
 }
 assert(guard < 40);
 assert(kp_rgb_sync_sources[0].selectors[0].snapshot.slot == 9);
 unsigned steps = 0;
 for (unsigned n=0;n<sends;n++) {
  if (destinations[n] == 0 && !strcmp(bindings[n].behavior_dev,"selector-a")) {
   if (steps == 0) { assert(bindings[n].param1 == 0x108 && bindings[n].param2 == 3); }
   else if (steps < 4) { assert(bindings[n].param1 == 0x107 + steps && bindings[n].param2 == 3); }
   else { assert(bindings[n].param1 == 0x108 + steps - 4 && bindings[n].param2 == 9); }
   steps++;
  }
 }
 assert(steps == 7);
 unsigned before = sends;
 selected_slots[1] = 10; kp_rgb_sync_selector_step_handler(NULL);
 assert(sends == before); /* Local changes alone do not request sync. */
 kp_rgb_matrix_lock(); kp_rgb_split_selectors_dirty_locked(); kp_rgb_matrix_unlock();
 assert(kp_rgb_sync_selectors[0].revision == 2 && kp_rgb_sync_selectors[1].revision == 1);
 fail_source[0] = true;
 for (unsigned n=0;n<40;n++) { now_ms += 1000; kp_rgb_sync_selector_step_handler(NULL); }
 assert(kp_rgb_sync_sources[0].selectors[0].pending);
 assert(kp_rgb_sync_sources[0].selectors[0].ready_at == now_ms + 1000 ||
        kp_rgb_sync_sources[0].selectors[1].ready_at == now_ms + 1000);
 assert(kp_rgb_sync_sources[1].selectors[0].synced && !kp_rgb_sync_sources[1].selectors[0].pending);
 assert(kp_rgb_sync_sources[1].selectors[1].snapshot.slot == 10);
 selected_transport = &transports[1]; kp_rgb_sync_refresh();
 assert(!kp_rgb_sync_sources[0].selectors[0].captured);
 assert(kp_rgb_sync_sources[0].selectors[0].step == 0);
 assert(kp_rgb_sync_sources[0].selectors[0].ready_at == now_ms + 100);
 available_count = 1; ids[0] = 1; kp_rgb_sync_refresh();
 assert(!kp_rgb_sync_sources[0].selectors[0].pending);
 assert(kp_rgb_sync_sources[1].selectors[0].pending);
 puts("selector split revisions, frozen progress, completion retries and fairness passed");
 return 0;
}
'''

if __name__ == "__main__":
    run(TESTS)
