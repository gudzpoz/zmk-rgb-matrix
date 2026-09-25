# Developing zmk-rgb-matrix

Notes for people / agents working **on this module**: the engine, the split
push, the trigger table, latency, and the preview harness. It hopefully records
what the code does and why it is shaped that way, including the dead ends.

## Source layout

```
include/zmk/rgb_matrix.h          public API: effects, conditions, overlays, triggers
include/zmk/rgb_matrix_math.h     public math helpers (sin8, atan2_8, isqrt)
include/dt-bindings/keypaw/       RGB command constants for devicetree
src/rgb_matrix.c                  engine: tick, geometry, pixel output
src/rgb_utils.c                   colour math, overlay registry + state words
src/behavior_rgb_matrix.c         &kprgb command dispatch, effect registry
src/rgb_settings.c                persistence (CONFIG_SETTINGS)
src/rgb_split_sync.c              central-only connect-time state push
src/rgb_triggers.c                central-only trigger evaluator
src/overlays/overlay.c            the generic compositor (the only overlay kind)
src/conditions/, src/triggers/    built-in conditions and the trigger kind
src/effects/                      built-in effects
src/rgb_matrix_internal.h         private; never included by third-party code
dts/bindings/                     one binding per compatible
tests/sim/                        native_sim preview harness
```

## Engine architecture

### One engine, many behavior zones

There is a single physical matrix engine (`src/rgb_matrix.c`) shared by possibly
several `keypaw,behavior-rgb-matrix` nodes. Each behavior owns a **zone**: its
`leds` list, or — when omitted, for backward compatibility — the whole matrix.
The engine merges the zones per frame, so multiple behaviors can drive disjoint
LED ranges (a main matrix plus an accent strip).

The engine holds the per-behavior state in `struct kp_rgb_behavior_context`
(`src/rgb_matrix_internal.h`) and serialises all access with `kp_rgb_matrix_lock()`
— a global lock covering the physical output and every context. The one rule: do
**not** hold the lock across `settings_save_one()`, which can block on flash I/O.

### Geometry and the frame

`kp_resolve_layout()` walks the `mapping` property once, resolves each LED's
centre from `zmk,physical-layout` (or from the `KP_RGB_NO_KEY(x, y)` encoded
coordinates), and derives `board_length` / `board_height`. It also builds
`kp_key_to_led[]`, the table behind `kp_rgb_led_for_position()`.

Each tick the engine prepares a `struct kp_rgb_frame` (public, in
`include/zmk/rgb_matrix.h`) and hands it to the active effect's renderer, then to
each overlay in paint order, then dims by the tuning brightness and flushes the
strip. Effects render at full scale; dimming happens centrally so one global
brightness applies uniformly.

`board_length`/`board_height` are floored at 100 layout units (one key width) so
a single-LED or degenerate half never yields a zero divisor in an effect.

### Tick, flush, and the low-priority queue

A `k_work_delayable` on ZMK's low-priority work queue repaints every
`CONFIG_KEYPAW_RGB_MATRIX_TICK_MS` (default 32 ms; upstream underglow uses 50 ms,
32 ms is noticeably smoother for `reactive`/`ripple`). One update costs ~1.5 ms
of blocking SPI.

`zmk_rgb_matrix_flush()` schedules an immediate repaint by submitting the same
tick work. It is **submit-only, non-blocking, and does not reset `last_tick`**,
so the extra frame does not restart an animation. It is safe from any context and
public, so a third-party kind can call it from a listener (see
[extending.md](extending.md#repainting-on-an-event)). Concurrent calls coalesce
because a Zephyr work item can be queued only once, so a burst costs at most one
extra frame.

The tick paints locally *before* dispatching overlay state (see below), so a
blocking split send does not delay the central's own LEDs — that ordering is why
a layer change used to lag visibly while `RGB_EFF` felt instant.

### Effect registry

Effects are the children of a behavior; an effect's slot is its child index
(`DT_NODE_CHILD_IDX`), which the shared command handlers read from
`struct kp_rgb_effect_common_config.index`. With persistence compiled in the
registry is capped at `KP_RGB_PERSIST_MAX_EFFECTS` (16) because the persisted
blob holds one fixed entry per effect; otherwise the only cap is the byte the
index travels in (`UINT8_MAX`, asserted in `behavior_rgb_matrix.c`). Declaration
order is the on-wire identity — see
[extending.md](extending.md#declaration-order-is-identity).

## Conditions, overlays, triggers: the view/state split

Two questions look alike but are not, and the split between them shapes the whole
API:

- **View** — *what should be lit right now?* A pure function of the current
  conditions and the static devicetree: stateless, sampled every tick, reversible
  by construction. This is an **overlay**.
- **State** — *change the user's selection.* Edge-triggered, persisted,
  split-synced, and **not idempotent** (`RGB_TOG` twice is a no-op, `RGB_EFF`
  twice advances twice). This is a **trigger**.

Routing "layer 1 → effect 3" through the state mechanism is the obvious first
instinct, and an earlier design did exactly that. It then needs a winner table
*and* a global catch-all trigger to restore, because a state change has no
inverse. An overlay avoids that entirely: it never mutates the base state, so
removing it restores the view for free. Only the stateful path keeps a catch-all.

### The parent determines an effect's role

Registry identity was the reason effects had to be children of the behavior, and
it is the *only* reason. Every consumer of `index`, `effects[]`, `effect_count`
and `owner` is registry-scoped, so an effect outside the registry needs none of
it:

- child of `keypaw,behavior-rgb-matrix` → **registry effect**: selectable,
  cyclable, persisted, split-addressed, keymap-bindable, seeded with the
  behavior's boot defaults;
- child of `keypaw,rgb-overlay` → **private effect**: composited by that overlay
  only. No slot, no identity, no persistence, and it must not set `no-cycle`,
  `overlays` or `no-overlays`.

`KP_RGB_EFFECT_DEFINE` asserts the parent is one of those two and NULLs `owner`
and the binding-conversion hook for a private effect, so a stray keymap binding
fails loudly instead of retargeting at the overlay's name. `KP_RGB_EFFECT_INDEX`
is deliberately unchanged: the field is stored but never read for a private
effect.

### Conditions are the shared primitive

A condition is a stateless predicate (`kp_rgb_condition_api`) consumed by both an
overlay and a trigger, so a layer or CapsLock predicate is written once. The
compositor's `active()` delegates to its node's `condition` phandle and the
trigger's does the same; `NULL` means unconditionally active. Conditions carry no
ordinal and no split state of their own — the central-evaluated **overlay** pushes
a bit per overlay, not per condition. Note the split of authority: whether the
condition is evaluated once on the central or per half is the consuming overlay's
`local` flag to declare, because the push bit belongs to the overlay.

### The compositor

The one overlay kind is a compositor: while its condition holds it renders its
single effect into a layer buffer and blends it back onto the targeted LEDs:

```c
struct led_rgb *base = frame->pixels;
memset(layer, 0, sizeof(layer));
frame->pixels = layer;
fx->render(effect, frame);
frame->pixels = base;
kp_rgb_overlay_paint_pixels(frame, data->leds, data->led_count, layer, opacity);
```

Effects overwrite the whole frame, so the buffer swap is what confines the effect
to the overlay's targets. The frame's geometry, clock and tuning are untouched,
so the sub-effect renders the real board on the real clock. Depth is bounded at 1
by construction — the referenced node is an effect, never another compositor — so
there is no recursion. One shared buffer suffices because overlay renderers run
one at a time, serialised under `kp_rgb_matrix_lock()`.

`all-leds;` on the overlay skips the target list and blends the whole layer
buffer across every LED.

The compositor is one kind, not the definition of an overlay.
`kp_rgb_overlay_api` is deliberately general — `render(dev, frame)` receives the
mutable frame — so a third-party kind can transform the composited result in
place (a *filter*: invert, dim, tint) instead of compositing an effect. The one
contract it must honour is the cover skip below: never claim `all-leds` at
`opacity` 100 unless `render()` really replaces every pixel. See
[extending.md](extending.md#writing-a-filter).

### An opaque full-cover overlay skips what it hides

`opacity >= 100` makes `kp_rgb_rgb_mix()` return the overlay pixel untouched, so
an **active** overlay that is `all-leds` at full opacity replaces
every LED. Anything painted before it — the active effect and every overlay below
it in paint order — is therefore invisible, and the engine skips those renders
(`kp_last_covering_overlay()` in `src/rgb_matrix.c` picks the *last* such
overlay; painting starts there). Overlays after it still paint on top.

Two contracts make this sound, so a kind must honour both:

- `opacity >= 100` really means "replace every targeted pixel", and
- `all-leds` really means every LED on the half, not a subset.

The built-in compositor does. A kind that lies about either would let a stale
frame show through. This is a per-render skip only: the effect's `on_event` still
runs, and its animation clock does not advance while it is hidden, so it resumes
where it left off when the cover goes away.

An overlay composites exactly **one** effect, and how it is given decides its
lifetime:

- a nested child is a **private preset**: fixed DTS colour, no registry slot, not
  cyclable, and *not seeded* by `kp_rgb_apply_defaults` — it has no mutable state
  to seed;
- `effect = <&fx>;` names a **registry effect**: live and shared, so it follows
  the user's persisted hue/brightness/duration.

Not seeding a private effect has one consequence worth knowing: `duration` would
default to 0 and `kp_rgb_effect_period()` would return its old 1 ms floor. It now
falls back to `KP_RGB_EFFECT_PERIOD_FALLBACK_MS` (1000), so an animated private
effect runs at a sane rate without seeding. `duration = 0` therefore means
"inherit `initial-duration-ms`" for a registry effect and "use the fallback" for
a private one.

### Input events reach composited effects

The engine's `position_state_changed` listener delivers `on_event` to the active
effect *and* to the effect each active overlay composites (through the overlay
api's `event_target`), deduped by device pointer. Without it a composited
`reactive`/`ripple` would never see a keystroke, because `render` reads state its
`on_event` fills. Delivery is gated on the overlay's gate, matching the render
gate, so an inactive overlay does not accumulate stale state, and it happens
under the same lock as the active-effect delivery. Both presses and releases are
delivered, to every effect whose behavior owns the LED under the key, and the
payload is the typed `zmk_position_state_changed` — so `ev->state` is the only
discriminator a callback needs.

### Triggers stay edge-triggered

A trigger is a condition plus the `&kprgb` bindings to run when it **becomes**
the winner; the table's declaration order is its precedence. It has to be
edge-triggered: binding a level condition straight to `RGB_TOG` would toggle
every tick. There is deliberately no `on-exit` list — a state change has no
inverse anyway, so the `else` case stays an explicit unconditional trigger
declared last.

### The devicetree trick that makes the phandles safe

Both `condition` and `effect` resolve through
`COND_CODE_1(DT_NODE_HAS_PROP(...), (DT_PROP(...)), (...))`. Zephyr's
`COND_CODE_1` does **not** expand the branch it did not select (verified with
`gcc -E`), so `DT_PROP(node, effect)` is never evaluated on a node without that
property — which would be a hard dtc error. The same trick picks between `effect
= <&fx>;` and a nested child, and between a present and an absent `condition`.

## Overlay registry and the pushed state words

An overlay's ordinal is its position under the `keypaw,rgb-overlays` container.
Overlay state is one bit per ordinal, held as `ceil(count / 16)` `uint16_t`
words (`kp_overlay_state` in `src/rgb_utils.c`) — sized from
`DT_CHILD_NUM`, so there is **no fixed cap** (the old `KP_RGB_OVERLAY_MAX` is
gone).

### Predicate vs renderer

There is one overlay kind, a compositor. Its `active(dev)` delegates to the
node's `condition` phandle (`NULL` condition means always active), and its
`render(dev, frame)` renders the composited effect into a private layer buffer
and blends it onto the frame at `opacity` percent. The gate still lives in the
engine (`kp_rgb_overlay_gate()`), so `render` keeps its signature;
`active == NULL` means "render every tick and self-sample".

### Central by default, `local` for per-half

Where a condition is evaluated is the overlay's choice, and the default is the
common case:

- **Central-evaluated (default).** The central evaluates `active()` in the render
  tick (`kp_rgb_overlay_refresh()`) and pushes only the words whose bits changed,
  one 16-bit word per command, packed as `(word << 16) | bits`
  (`RGB_OVL_STATE_BITS`/`_WORD`). A peripheral gates its renderer on the pushed
  bit and never calls `active()`. This covers a central-only source (a layer or
  the dynamic-macro recorder) and a global host source (CapsLock) alike, so the
  DTS needs no flag for any of them.
- **Per-half (`local;`).** The overlay sets `local;` and each half calls
  `active()` for itself, so each half shows its own state (a battery bar,
  per-half activity). Such an overlay is never in the pushed words.

The default is deliberately the common case: getting it wrong the other way (a
central source left to the peripherals) fails silently, whereas a global source
under central evaluation is simply correct. `local;` is the opt-in that promises
the condition is truthful on the peripheral.

`RGB_OVL_STATE_CMD` lives beside the upstream include in
`include/dt-bindings/keypaw/rgb_matrix.h` (it claims slot 15, maintained there);
the payload encoders are C-only in `src/rgb_matrix_internal.h` and deliberately
carry no dtc-safe constraint.

Current order on the shield: `caps=0`, `dm_rec=1`, `layer_*=2..5` (one word, 6
bits used).

### Design points that were not obvious

- **Only changed words travel.** `dispatch()` keeps a mirror (`kp_overlay_sent`)
  of what it last broadcast and sends only the differing words. A mirror rather
  than a changed-word list is deliberate: if a tick cannot take the matrix lock
  and skips its dispatch, `state != sent` stays true and the next tick still has
  something to push. A changed-word list would have dropped that update
  permanently, since `refresh()` has already written the new state.
- **The command must not persist.** The `RGB_OVL_STATE_CMD` handler applies the
  word and returns *before* the `kp_rgb_save_state()` that closes the switch, so
  a layer toggle never schedules a flash write.
- **Level, not edge.** The central also re-pushes a full snapshot when a
  peripheral connects (`rgb_split_sync.c`, `CONFIG_KEYPAW_RGB_SPLIT_SYNC`), so a
  half that was off while a layer was held still shows it. Unlike an effect
  selection, overlay state is volatile and is not persisted.
- **Threads.** The word array is written on the split's system workqueue (the
  command handler) and read on the RGB low-priority workqueue (the render tick).
  Each aligned `uint16_t` access cannot tear, so the worst case is one stale
  ~32 ms frame.
- **Flush wiring.** The layer condition subscribes to `zmk_layer_state_changed`
  (central only), the `RGB_OVL_STATE_CMD` handler flushes when a pushed word
  actually changes (peripheral), and the caps condition flushes on its
  `zmk_hid_indicators_changed` listener. Note extra frames are not free for
  effects that advance state per render (`reactive`, `starlight`, `rain`): they
  advance one extra step per event, imperceptible at human event rates.

### Where conditions can run

The peripheral half builds neither `src/keymap.c` nor `src/hid_indicators.c`
(ZMK gates both in `zmk/app/CMakeLists.txt` on `(NOT CONFIG_ZMK_SPLIT) OR
CONFIG_ZMK_SPLIT_ROLE_CENTRAL`). Consequences:

- The caps condition is **event-driven on purpose**: it subscribes to
  `zmk_hid_indicators_changed` instead of reading
  `zmk_hid_indicators_get_current_profile()`, because the peripheral re-raises
  that event (`split/peripheral.c`, needs
  `CONFIG_ZMK_SPLIT_PERIPHERAL_HID_INDICATORS`) but has no getter to call. Both
  `CONFIG_ZMK_HID_INDICATORS` and its peripheral counterpart are set in
  `config/keypaw48.conf`; without them the condition is only truthful on the
  central.
- The layer condition can only be truthful on the central, so the overlay it
  drives uses the default (central evaluation). Key positions *do* resolve per
  half, so `layer_fn1`'s left-half keys already had LEDs on the peripheral; the
  compositor simply never ran because the condition had no source. The push makes
  those LEDs light.
- A condition whose source is central-only must still **compile and link on the
  peripheral**, because a shared DTSI expands it on both halves. Gate the *read*,
  not the device: the layer condition returns false there, and the caps condition
  reads the event the peripheral re-raises.

## Split state sync (`src/rgb_split_sync.c`)

A `&kprgb` command is `BEHAVIOR_LOCALITY_GLOBAL`, so every command typed on the
central reaches the peripheral and is persisted there. A peripheral that was
powered off while the settings changed never saw the command, keeps its stale
flash defaults, and reloads them on the next power-up. With
`CONFIG_KEYPAW_RGB_SPLIT_SYNC=y` (default) the central instead pushes the
selected effect, that effect's colour and period, and the user on/off intent to a
peripheral whenever it newly connects. Nothing is needed on the peripheral: its
command handler ends in `kp_rgb_save_state()`, so a push is applied *and*
persisted.

The push order is mandatory: `RGB_EFS_CMD(index)` → `RGB_COLOR_HSB_CMD(colour)` →
`RGB_SPI_CMD(period)` → `RGB_ON_CMD`/`RGB_OFF_CMD(user_on)`. The colour and
period commands act on the *receiver's* active effect
(`kp_rgb_set_hsb`/`kp_rgb_set_duration` go through `ctx->state.active_fx`), so
the effect must be selected first.

### Why the trigger is a poll

There is **no central-side "peripheral connected" event**:

- `zmk_split_peripheral_status_changed` is raised only on the peripheral.
- The central's only hook, `set_status_callback`, is a single pointer already
  owned by `central_init()`; taking it breaks transport selection.
- A peripheral-originated `BATTERY_EVENT` is an explicit no-op over BLE, and
  `zmk_peripheral_battery_state_changed` is compiled out here anyway because
  `CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING` is unset.

So the sync polls `get_available_source_ids()` (public API, reached through the
`zmk_split_transport_central` iterable section; only the `active_transport`
pointer is private). Upstream precedent is
`zmk_split_central_update_hid_indicator`. No edit to the `zmk/` submodule is
needed.

### Two BLE hazards the design works around

1. **GATT handles lag "connected".** `split_central_connected()` marks the slot
   connected before the asynchronous `bt_gatt_discover()`; `run_behavior_handle`
   stays 0 until discovery finishes, and the run worker drops each command with
   `LOG_ERR("Run behavior handle not found")` while `send_command()` still
   returns 0. Hence `CONFIG_KEYPAW_RGB_SPLIT_SYNC_SETTLE_MS` (default 1000) after
   a connection is first seen. Upstream's
   `update_peripheral_selected_layout()` works around the same window.
2. **The split run queue is 5 deep and discards the oldest entry on overflow.**
   Because the low-priority work queue also carries the 32 ms render tick, the
   sync emits **one command per work item** and reschedules, rather than bursting
   the whole state.

### Semantics and trade-offs

- Only the **active** effect's colour and period are sent (4 commands per
  context). Other effects keep whatever the peripheral already had, so selecting
  an effect edited while the peripheral was off shows that effect's own colour
  until it is edited again. A faithful version needs one select + colour + period
  per effect (~30 commands), which the 5-deep queue makes pacing-sensitive.
- `state.user_on`, not `state.on`: `state.on` is cleared by idle auto-off, so
  syncing it would persist `user_on=false` on the peripheral and destroy the
  user's intent. The trade-off is that a sync performed while the central is
  idle sends `RGB_ON` and can light an idle peripheral until its next activity
  transition clears it.
- Only **absolute** opcodes are sent. The peripheral never runs
  `binding_convert_central_state_dependent_params`, so `RGB_TOG`/`RGB_EFF`/
  `RGB_EFR` and the relative HUI/SAI/BRI commands would be applied against the
  peripheral's own state. `RGB_SPI_CMD` with `param2 == 0` means "increase", so
  the pushed period is `MAX(duration_ms, 1)`.
- By default, `CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE=60000` (see `app/Kconfig` in
  ZMK), so a peripheral power-cycled within 60 s of a sync may not have written
  the pushed state.
- No periodic re-assert, so a command lost to a queue overflow is only healed by
  the next reconnect.

## Trigger table internals

`src/rgb_triggers.c` owns the ordered table declared by a
`keypaw,rgb-trigger-table` node. It is modelled on
`zmk/app/src/conditional_layer.c`: a listener on `zmk_layer_state_changed`,
built only for the split central, enumerating children with
`DT_INST_FOREACH_CHILD`.

### Edge-triggered on winner change

A conditional layer can be activated *and deactivated*; there is no way to
"deactivate" an effect. So a trigger asserts on the transition into winning and
is otherwise left alone. That edge behaviour is what lets a manual effect
selection, or a relative command such as `RGB_HUI`, survive until the mapping
actually changes instead of being re-fired on every layer event. Consequence:
triggers do **not** continuously enforce the mapping.

Boot evaluates once at `APPLICATION` init, after the behavior's `POST_KERNEL`
init applied `initial-effect`, so a catch-all asserts immediately and can
override `initial-effect`.

### Why there is no `triggers` list

Declaration order is the precedence order, and the table holds **no phandle list
of its own children**. Two devicetree cycles, both found the hard way:

- On `&kprgb`: a trigger's `bindings` reference `&kprgb`, so a list there is
  `kprgb -> trigger -> kprgb`. edtlib rejects it outright.
- On the table itself: edtlib makes every node depend on its parent, so a parent
  listing its own child is also a 2-cycle. It is **not** reported as a cycle:
  `gen_defines` gives non-singleton SCCs `_ORD -1`, and the failure surfaces much
  later as
  `error: pasting "dts_ord_" and "-" does not give a valid preprocessing token`
  from `Z_MAYBE_DEVICE_DECLARE_INTERNAL` predeclaring devices in
  `zephyr/arch/arm/core/offsets/offsets.c`.

Rejected syntax: a flat mixed list such as
`triggers = <&lst &layer_one &target_effect>;`. One phandle array cannot carry a
condition and a target together: `phandle-array` would demand `#trigger-cells`
on every node, and cell-encoding the target would lose the relative-command and
colour payloads. The action property name is not free either:
`ZMK_KEYMAP_EXTRACT_BINDING` hardcodes `bindings`.

### `no-cycle`

`no-cycle;` on an effect excludes it from `RGB_EFF`/`RGB_EFR` only; explicit
`RGB_EFS`, a trigger and `initial-effect` can still select it. It is a parallel
`kp_effect_no_cycle[]` derived with `DT_FOREACH_CHILD` in
`behavior_rgb_matrix.c`, so no per-effect file changed. Note `DT_PROP()` (not
`DT_PROP_OR`) is the accessor for an absent boolean.

## Threading and latency

This is the module's most counter-intuitive area, and it was misdiagnosed twice
before instrumentation settled it.

**Symptom.** With the flush in place the central was immediate, but the
peripheral showed *random* latency.

**Two mechanisms:**

1. The peripheral must be *told* (one extra BLE hop), and ZMK's default split
   peripheral latency of 30 connection intervals lets it skip listening for up
   to ~30 events, so the push waits for the next listen whenever it misses the
   window the peripheral is awake after sending a key event. Landed vs missed is
   a race — hence "sometimes immediate, sometimes late".
2. The push is coupled to the render, running on the lowest-priority thread
   (`CONFIG_ZMK_LOW_PRIORITY_THREAD_PRIORITY=10`) below the display's dedicated
   thread (5) and split BLE (1).

`CONFIG_BT_PERIPHERAL_PREF_LATENCY=0` on the left half did **not** remove the
randomness, and neither did disabling the on-keypress animation widget.

**Root cause.** The display thread preempted the latency-critical work. ZMK's
default `CONFIG_ZMK_DISPLAY_DEDICATED_THREAD_PRIORITY` is 5 — the same priority
as both split transport queues and *above* the RGB render on the low-priority
queue (10). With `CONFIG_TIMESLICE_SIZE=0`, an equal-priority thread only yields
when it blocks, so an LVGL cycle could hold off the split send and preempt the
RGB frame outright.

A temporary `#define KP_RGB_DIAG_LATENCY` in `src/rgb_matrix.c` logged
`flush->paint` on both halves and separated them cleanly: the **central** took
**17 ms** from layer event to painted (and the push inherits all of it), while
the **peripheral** took only **2 ms** from word arrival to painted. The
peripheral's render was never the problem. The instrumentation has been removed
(verified absent from both images).

**Fix.** `CONFIG_ZMK_DISPLAY_DEDICATED_THREAD_PRIORITY=11` in the shared config,
which puts the display below both split queues and below the RGB render. Costs a
little refresh smoothness; the display yields to each ~2-3 ms frame (every 32
ms, ~8% of idle time). Confirmed on hardware: the jitter is gone.

If a similar latency question comes up, **measure before hypothesising** —
stamp `flush->paint` on both halves again. Guessing got it wrong twice.

## Testing and previews

`tests/sim/` builds the module for `native_sim//zmk_test_mock` and renders one
GIF per devicetree effect node.

```sh
tests/sim/run-preview.sh            # every effect
tests/sim/run-preview.sh rainbow    # slug substring match
tests/sim/run-preview.sh ripple 5   # slug or exact index
```

The harness:

- builds once for `native_sim`, then runs `zmk.exe --effect=<idx>
  --capture=<file>` per effect;
- takes the effect list from the firmware itself (`zmk.exe --list-effects`), so
  filenames cannot drift from `tests/sim/config/native_sim.overlay`;
- names each GIF after the node's `display-name`, slugs it, and refuses to run if
  two effects slugify to the same name;
- writes a `previews.tsv` manifest (`slug<TAB>display-name`) so consumers such
  as the gallery page do not re-derive the slug rule;
- warns (`MIN_DISTINCT`, default 3) when a preview never changes — a static
  capture is nearly always a bug, though `solid` and `static` legitimately warn.

GIFs land in `tests/sim/out/`, captures and logs in `tests/sim/out/.work/`. The
gallery page lives in `tests/sim/site/`; `make_index.py` rebuilds it. Useful
environment overrides: `DURATION`, `TILE`, `MIN_DISTINCT`, `OUT_DIR`, `ZMK_WS`,
`WEST`.

## Devicetree traps

Found while building this module; they generalise.

- `DT_NODE_HAS_PROP()` is **true for an absent boolean** (implicit default 0), so
  it is useless as a `COND_CODE_1` flag for one — use `DT_PROP()` there. Arrays
  without a `default:` are unaffected.
- `condition` and `effect` must be `type: phandle` (a bare phandle), not
  `phandle-array`, which would demand `#condition-cells` / `#effect-cells` on
  every target. The related `overlays = <&a &b>;` list is `type: phandles`.
- An effect binding includes `zero_param.yaml`, which marks `#binding-cells`
  required. A nested (private) effect is *not* a keymap binding, but it must
  still declare `#binding-cells = <0>` or dtc rejects the node.
- A boolean cannot carry `default:` in edtlib, so `initial-on` is documented as
  "enabled unless explicitly set to false" and read with `DT_PROP`.
- In Zephyr 4.1 / hwmv2, a `label` must be declared in the binding's
  `properties:` section (no longer implicitly allowed).
