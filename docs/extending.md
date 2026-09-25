# Extending the RGB matrix

This guide is for authors of third-party modules that wish to add RGB matrix
effects, overlays, or triggers. Everything here uses only the public API in
[`<zmk/rgb_matrix.h>`](../include/zmk/rgb_matrix.h).

Some example modules are available:

| Extension | Example |
|---|---|
| Effect | [`modules/zmk-rgb-effect-example@keypaw48-zmk`](https://github.com/gudzpoz/keypaw48-zmk/tree/main/modules/zmk-rgb-effect-example) (comet sweep, own binding + renderer) |
| Condition | [`modules/zmk-behavior-dynamic-macro@keypaw48-zmk`](https://github.com/gudzpoz/keypaw48-zmk/tree/main/modules/zmk-behavior-dynamic-macro/src/condition_dynamic_macro.c) (`dm_rec`, recorder-lit overlay) |
| Trigger | [`src/triggers/trigger.c`](../src/triggers/trigger.c) in this module |

## The extension points

| You want to... | Write a... | Public macro |
|---|---|---|
| Animate pixels from a position-aware frame | **effect** | `KP_RGB_EFFECT_DEFINE` |
| A reusable predicate ("CapsLock on", "layer 2 active") | **condition** | `KP_RGB_CONDITION_DEFINE` |
| Transform the already-composited frame (a filter) | **overlay kind** | `KP_RGB_OVERLAY_DEFINE` |

Effect, condition, and overlay kinds are ordinary Zephyr devices instantiated
from a devicetree compatible, and a module only needs its binding YAML, its
source, and a node placed by the user.

> **Never include `src/rgb_matrix_internal.h`.** It is the engine's private
> header and changes without notice. A third-party module is confined to
> `<zmk/rgb_matrix.h>` (and, for math helpers, `<zmk/rgb_matrix_math.h>`).

## Module skeleton

Minimal layout for an extension module (mirrors `zmk-rgb-effect-example`):

```
modules/zmk-rgb-mything/
├── dts/bindings/keypaw,rgb-matrix-mything.yaml
├── include/                     # only if you ship dt-bindings constants
├── src/mything.c
├── src/CMakeLists.txt
├── CMakeLists.txt
├── Kconfig
└── zephyr/module.yml            # settings: dts_root: .
```

Two build details:

- `zephyr/module.yml` must set `dts_root: .` so your binding directory is found.
- `src/CMakeLists.txt` needs the application's include directory visible, since
  `<zmk/rgb_matrix.h>` lives in the core module:

  ```cmake
  zephyr_library()
  zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)
  zephyr_library_sources_ifdef(CONFIG_ZMK_RGB_MYTHING mything.c)
  ```

## Writing an effect

An effect renders a frame of pixels. Most often, it is placed under a
`keypaw,behavior-rgb-matrix` node, making it selectable, cyclable, persisted and
split-addressed. It can also be embedded as a child of a `keypaw,rgb-overlay`
node, when it will be come "private", composited by that overlay only and never
user-selectable or persisted.

The same code and binding serve both roles; only the node's position differs.

### 1. Binding

Include the shared fragment and add your own properties:

```yaml
# dts/bindings/keypaw,rgb-matrix-mything.yaml
compatible: "keypaw,rgb-matrix-mything"
include: [zero_param.yaml, "keypaw,rgb-matrix-effect-common.yaml"]

properties:
  spread:
    type: int
    required: false
    default: 40
```

`keypaw,rgb-matrix-effect-common.yaml` provides `color`, `duration`, `overlays`,
`no-overlays`, and `no-cycle`.

#### Devicetree string enum helper

The built-in rainbow effect, for example, allows the user to choose from an list
of supported styles:

```yaml
properties:
  basis:
    type: string
    required: false
    default: "x"
    enum: ["x", "y", "radial", "pinwheel", "spiral", "chevron", "flag"]
```

And properties like this, at least for me, are a pain to check against in C
code. If you agree on that, we have a few tiny helpers for it:

```c
// Define the enum: you still need to supply the allowed values in the YAML
DEFINE_DT_ENUM(basis, x, y, radial, pinwheel, spiral, chevron, flag);
// The type name is `<enum_name>_t`, so `basis` -> `basis_t`.

// You may use this to convert DTS values to our enum value with CONV_DT_ENUM,
// which is often used in your config initializer:
  .mode = CONV_DT_ENUM(inst, mode),

// Then, in your code, you may check the enum value against DT_ENUM_CONST:
if (cfg->mode == DT_ENUM_CONST(mode, disc)) { ... }
```

### 2. Config and data structs

Both must embed the shared structs **as their first member**, as is required by
the engine.

```c
// src/mything.c

#define DT_DRV_COMPAT keypaw_rgb_matrix_mything

#include <zephyr/device.h>
#include <zmk/rgb_matrix.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct kp_eff_mything_config {
  struct kp_rgb_effect_common_config common; /* must be first */
  uint16_t spread;
};
struct kp_eff_mything_data {
  struct kp_rgb_effect_common_data common; /* must be first */
  uint32_t phase_ms;                       /* your own state */
};
```

### 3. Renderer

`render(dev, frame)` is called once per animation tick while the effect is
active. Fill `frame->pixels[0 .. frame->count-1]`.

```c
static void kp_eff_mything_render(const struct device *dev,
                                  struct kp_rgb_frame *f) {
  const struct kp_eff_mything_config *cfg = dev->config;
  struct kp_eff_mything_data *data = dev->data;
  uint32_t period = kp_rgb_effect_period(dev);
  uint8_t pct = kp_rgb_brightness_pct(f);

  for (size_t i = 0; i < f->count; i++) {
    struct kp_rgb_hsb hsb = data->common.color;
    /* ... derive hsb from f->coords[i] and data->phase_ms ... */
    f->pixels[i] = kp_rgb_hsb_to_rgb(kp_rgb_hsb_scale(hsb, pct));
  }

  data->phase_ms = (data->phase_ms + f->elapsed) % period;
}
```

The `struct kp_rgb_frame` contains `pixels` where you should be writing to,
along with some other information like `elapsed` and `coords` that might help
you render a continuous animation. See the comments in
[`<zmk/rgb_matrix.h>`](../include/zmk/rgb_matrix.h) for details.

Helpers that do the usual bookkeeping for you:

- `kp_rgb_effect_period(dev)`: the effect's animation period, never 0.
- `kp_rgb_brightness_pct(frame)`: the brightness to apply this frame
  (`idle_brightness` while idle, else `max_brightness`).
- `kp_rgb_effect_data(dev)` / `kp_rgb_effect_cfg(dev)`: typed access to the
  common part.
- `kp_rgb_hsb_scale()` / `kp_rgb_rgb_scale()` / `kp_rgb_rgb_mix()`: brightness
  and compositing.
- `KP_RGB_HSB_FROM_HEX(hex)`: compile-time preset colour from a devicetree
  constant, and `kp_hex_to_rgb(hex)` for an RGB literal.
- `<zmk/rgb_matrix_math.h>`: `kp_rgb_sin8` (eased oscillation),
  `kp_rgb_atan2_8`, `kp_rgb_isqrt`, `kp_rgb_arm_phase`.

Effects always render at full scale and then dim the result; that is what makes
a single global brightness apply uniformly.

### 4. Optional event callback

Pass an `on_event` handler when the effect needs to react to key events (a
keystroke for a reactive effect, or a release for a held-key glow). The payload
is typed: only `zmk_position_state_changed` is ever delivered, and `ev->state`
separates a press from a release. There is no frame, so use the public accessors
to map the key position to its LED:

```c
static void kp_eff_mything_event(const struct device *dev,
                                 const struct zmk_position_state_changed *ev) {
  if (!ev->state) {
    // Both presses and releases are delivered.
    // Check ev->state and ignore the release if you want to.
    return;
  }
  size_t led = kp_rgb_led_for_position(ev->position); /* SIZE_MAX if none */
  /* ... */
}
```

`kp_rgb_led_coord(led)` returns that LED's geometry when no frame is available.
At most one LED resolves per key position (the last `mapping` entry naming that
key wins).

An event is delivered only when a running behavior owns the LED under the key,
so an effect never sees a key outside its zone. The callback runs under the
matrix lock: keep it short and never block.

> Normally, the event doesn't change frame render scheduling, so there might be
> a very slight delay between the key press and the next frame. If you want
> immediate animation change, call `zmk_rgb_matrix_flush()`. See
> [Repaining](#repainting-on-an-event) below.

### 5. Instantiate

```c
#define KP_EFF_MYTHING_DEFINE(inst)                                            \
  static const struct kp_eff_mything_config kp_eff_mything_##inst##_cfg = {    \
      .common = {.index = KP_RGB_EFFECT_INDEX(inst)},                          \
      .spread = DT_PROP_OR(DT_DRV_INST(inst), spread, 40),                     \
  };                                                                           \
  static struct kp_eff_mything_data kp_eff_mything_##inst##_data = {           \
      .common = {.color = KP_RGB_HSB_FROM_HEX(                                 \
                     DT_PROP_OR(DT_DRV_INST(inst), color, 0xFFFFFF)),          \
                 .duration_ms = DT_PROP_OR(DT_DRV_INST(inst), duration, 0)},   \
  };                                                                           \
  KP_RGB_EFFECT_DEFINE(DT_DRV_INST(inst), kp_eff_mything_render,               \
                       kp_eff_mything_event, kp_eff_mything_##inst)

DT_INST_FOREACH_STATUS_OKAY(KP_EFF_MYTHING_DEFINE)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
```

`KP_RGB_EFFECT_INDEX(inst)` bakes in the child index. Pass `NULL` for the event
callback if you do not need one. The effect is then usable as an actual
animation, or from a keymap as `&fx_mything`, once the user declares the node
under `&kprgb`.

## Configuring an overlay

Overlays are typically used as indicators: layer indicators, CapsLock
indicators... Under the hood, an overlay is a **compositor**: while its
condition is active it renders one effect and blends the result over the LEDs it
targets. It is not a behavior and cannot be keymap-bound; it is declared as a
child of a `keypaw,rgb-overlays` container and listed on the behavior (or on an
individual effect, which overrides the list) in paint order.

```dts
rgb_conditions: rgb_conditions {
    compatible = "keypaw,rgb-conditions";
    cond_rec: cond_rec {
        compatible = "keypaw,rgb-condition-mything";
    };
};

rgb_overlays: rgb_overlays {
    compatible = "keypaw,rgb-overlays";
    rec: rec {
        compatible = "keypaw,rgb-overlay";
        condition = <&cond_rec>;
        keys = <11 23 35>;
        opacity = <100>;            /* 100 replaces, lower blends */
        fx_rec {                    /* nested: a private effect */
            compatible = "keypaw,rgb-matrix-solid";
            #binding-cells = <0>;
            color = <0xFF0000>;
        };
    };
};
```

`keypaw,rgb-overlay-common.yaml` provides `keys`, `leds`, `all-leds`, `opacity`,
and `local`; `keypaw,rgb-overlay.yaml` adds `condition` and `effect`.
`all-leds;` targets every LED on the half instead of a list, and combined with
`opacity` 100 it fully replaces the frame, so the engine skips the active effect
and any overlay below this one. An overlay composites exactly one effect, given
either way:

- a **nested child** (as above) is a private effect: no registry slot, not
  cyclable, and does not follow the user's brightness/hue changes. The effect
  binding marks `#binding-cells` required, so the node must still declare
  `#binding-cells = <0>` even though it is a no-op here;
- `effect = <&fx>;` names a **shared registry effect** (a child of `&kprgb`),
  which is live and adjustable.

Use one or the other, never both. Targets are resolved per half, so a `keys`
list works unchanged on both halves for split keyboards.

## Writing a condition

A condition is a reusable predicate consumed by overlays (and, later, triggers).
It carries no registry identity and no state of its own, so it passes no data
pointer and needs no init function.

### 1. Binding

```yaml
compatible: "keypaw,rgb-condition-mything"
properties:
  threshold:
    type: int
    required: false
    default: 20
```

### 2. Predicate

```c
#define DT_DRV_COMPAT keypaw_rgb_condition_mything

#include <zephyr/device.h>
#include <zephyr/sys/util.h>
#include <zmk/rgb_matrix.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static bool kp_cond_mything_active(const struct device *dev) {
  ARG_UNUSED(dev);
  return /* read whatever this condition watches */;
}

#define KP_COND_MYTHING_DEFINE(inst)                                          \
  KP_RGB_CONDITION_DEFINE(inst, kp_cond_mything_active, NULL)

DT_INST_FOREACH_STATUS_OKAY(KP_COND_MYTHING_DEFINE)

#endif
```

`KP_RGB_CONDITION_DEFINE(inst, active_fn, cfg_expr)` takes the config as a full
expression: pass `&my_cfg` when the predicate needs devicetree parameters (see
[`layer.c`](../src/conditions/layer.c)) or `NULL` when it does not (see
[`always.c`](../src/conditions/always.c)).

### Where does the state live?

This is important for split keyboards, as ZMK offers a different set of events
for the central half and peripherals.

- **Central-evaluated (default).** The central evaluates the condition each
  tick, and pushes only the bits whose value changed; a peripheral gates its
  renderer on that pushed state without calling the condition. This covers both
  a source that is inherently central-only (keymap layer states) and a global
  host source (CapsLock).

- **Per-half (`local;`).** When the source genuinely differs per half — a
  battery bar, per-half activity — add `local;` to the overlay devicetree
  definition. Each half then evaluates the condition for itself, so each half
  shows its own state. The condition must be truthful on a peripheral, which is
  what the `local` flag promises.

A condition that reads a central-only symbol must still **compile and link on
the peripheral**, even when only default (central-evaluated) overlays use it,
because a shared DTSI expands it on both halves — and a `local` overlay would
call it there. Do not gate the device out; gate the *read*, like this:

```c
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#define KP_COND_LAYER_HAS_KEYMAP 1
#else
#define KP_COND_LAYER_HAS_KEYMAP 0
#endif

static bool kp_cond_layer_active(const struct device *dev) {
#if KP_COND_LAYER_HAS_KEYMAP
  // ...
  return zmk_keymap_layer_active((zmk_keymap_layer_id_t)cfg->layer);
#else
  ARG_UNUSED(dev);
  return false;
#endif
}
```

### Repainting on an event

The engine repaints every tick (`CONFIG_KEYPAW_RGB_MATRIX_TICK_MS`, 32 ms by
default), and on the central-only path a peripheral adds a second tick of
latency. If the condition's source has an event, subscribe to it and call
`zmk_rgb_matrix_flush()` when the value changes:

```c
static int my_listener(const zmk_event_t *eh) {
  if (as_zmk_some_event(eh) != NULL) {
    zmk_rgb_matrix_flush();
  }
  return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(my_cond, my_listener);
ZMK_SUBSCRIPTION(my_cond, zmk_some_event);
```

`zmk_rgb_matrix_flush()` is public, submit-only, non-blocking, and does not
reset the animation clock, so the extra frame does not restart an effect.
Concurrent calls coalesce.

A listener is never needed for correctness as the engine repaints every tick.
But still, it buys you a bit of **latency**, and where it helps depends on how
the condition is evaluated:

- **Central-evaluated (default).** Subscribe on the central: the flush submits
  the same tick work that evaluates the conditions and dispatches the push, so
  the change is immediate *and* the push starts at once instead of waiting for
  the next timer tick. The peripheral automatically flushes when a pushed word
  arrives changed.
- **`local`.** Each half evaluates for itself and nothing is pushed, so each
  half needs its own listener to avoid waiting a tick.

See [`layer.c`](../src/conditions/layer.c) for an example.

## Writing a trigger

A trigger maps a condition to a list of `&kprgb` commands. Triggers are the
children of a `keypaw,rgb-trigger-table` node, and **their declaration order is
their precedence**: the first trigger whose condition is active wins, and its
`bindings` run only when the winner changes.

```dts
/ {
    rgb_conditions: rgb_conditions {
        compatible = "keypaw,rgb-conditions";
        cond_layer1: cond_layer1 {
            compatible = "keypaw,rgb-condition-layer";
            layer = <1>;
        };
        cond_always: cond_always {
            compatible = "keypaw,rgb-condition-always";
        };
    };

    rgb_triggers: rgb_triggers {
        compatible = "keypaw,rgb-trigger-table";
    };
};

&rgb_triggers {
    trig_fn: trig_fn {
        compatible = "keypaw,rgb-trigger";
        condition = <&cond_layer1>;
        bindings = <&kprgb RGB_EFS_CMD 1>;
    };
    fallback: fallback {                       /* declare the catch-all LAST */
        compatible = "keypaw,rgb-trigger";
        condition = <&cond_always>;            /* or omit `condition` entirely */
        bindings = <&kprgb RGB_EFS_CMD 0>;
    };
};
```

Semantics worth knowing:

- The evaluator runs on the split **central only** (conditions read keymap
  state), but the emitted commands reach both halves because `&kprgb` is
  `BEHAVIOR_LOCALITY_GLOBAL`.
- It is **edge-triggered on winner change**, not level-triggered. A manual
  `RGB_EFF`/`RGB_EFS` therefore survives until the mapping actually changes, and
  relative commands (`RGB_HUI`, `RGB_BRI`, ...) do not re-fire on unrelated
  layer events.
- "No match" emits nothing; an unconditional trigger (no `condition`, or
  `keypaw,rgb-condition-always`) declared last is the `else`.
- A trigger asserts on the transition and does not continuously enforce the
  mapping.
- Boot evaluates once, after `initial-effect`, so a catch-all can override it.

> There is deliberately no `triggers = <...>;` phandle list on the table or on
> `&kprgb`: a parent pointing at its own child is a devicetree cycle, and the
> failure mode is obscure. See
> [development.md](development.md#why-there-is-no-triggers-list) for the full
> story. Ordering footguns (an early broad trigger shadows every later one; the
> catch-all must be last) are not enforced by anything.

## Writing an overlayed filter

In the previous sections, we said:

> Under the hood, an overlay is a **compositor**...

Well, that was only partly true: you can use our built-in `keypaw,rgb-overlay`,
which is merely a compositor, but you can also code your own overlay for other
fancy effects.

Actually, under the hood, an overlay is no more than a processor that is run
after effect rendering, and before the engine emits the final output to the LED
strip, and it can do anything to transform the output.

Here is an **untested** example of writing a custom overlay to filter on the
output: `render()` receives the mutable frame, so we just read what the effect
and the overlays below it produced, and then transform it.

We write it as any overlay kind: a binding including
`keypaw,rgb-overlay-common.yaml`, and `KP_RGB_OVERLAY_DEFINE`.

```c
#define DT_DRV_COMPAT keypaw_rgb_overlay_myfilter

#include <zephyr/device.h>
#include <zephyr/sys/util.h>
#include <zmk/rgb_matrix.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct kp_ovl_myfilter_config {
  struct kp_rgb_overlay_common_config common; /* must be first */
};
struct kp_ovl_myfilter_data {
  struct kp_rgb_overlay_common_data common; /* must be first */
};

static void kp_ovl_myfilter_render(const struct device *dev,
                                   struct kp_rgb_frame *frame) {
  ARG_UNUSED(dev);
  for (size_t i = 0; i < frame->count; i++) {
    frame->pixels[i] = /* transform frame->pixels[i] */;
  }
}

#define KP_OVL_MYFILTER_DEFINE(inst)                                           \
  KP_RGB_OVERLAY_TARGET_ARRAYS(inst, kp_ovl_myfilter_##inst);                  \
  static const struct kp_ovl_myfilter_config kp_ovl_myfilter_##inst##_cfg = {  \
      .common =                                                                \
          KP_RGB_OVERLAY_COMMON(DT_DRV_INST(inst), kp_ovl_myfilter_##inst),    \
  };                                                                           \
  static struct kp_ovl_myfilter_data kp_ovl_myfilter_##inst##_data;            \
  KP_RGB_OVERLAY_DEFINE(inst, NULL, kp_ovl_myfilter_render, NULL,              \
                        kp_ovl_myfilter_##inst)

DT_INST_FOREACH_STATUS_OKAY(KP_OVL_MYFILTER_DEFINE)

#endif
```

`NULL` for `active_fn` means "always on" (the engine sets the state bit every
tick, and the peripheral reads that bit). To gate the filter, follow what the
built-in `keypaw,rgb-overlay` does and add `condition: {type: phandle}` to your
binding, carry it per instance, and delegate:

```c
struct kp_ovl_myfilter_config {
  struct kp_rgb_overlay_common_config common; /* must be first */
  const struct device *condition;             /* NULL = always active */
};

static bool kp_ovl_myfilter_active(const struct device *dev) {
  const struct kp_ovl_myfilter_config *cfg = dev->config;
  if (cfg->condition == NULL) {
    return true;
  }
  const struct kp_rgb_condition_api *cond =
      (const struct kp_rgb_condition_api *)cfg->condition->api;
  return cond->active(cfg->condition);
}
```

...with `.condition = KP_RGB_CONDITION_PTR(DT_DRV_INST(inst))` in the config
initializer and `kp_ovl_myfilter_active` (not `NULL`) passed to
`KP_RGB_OVERLAY_DEFINE`. Keeping it in the config rather than reaching for
`DT_DRV_INST(0)` is what makes each instance's condition independent.

Two contracts it must respect:

- **Never claim `all-leds` at `opacity` 100.** That combination suggests to the
  engine that the overlay covers and are independent of every rendered pixel,
  when the engine will skip everything below it. If the overlay needs the
  incoming `frame->pixels` data, keep the opacity non-`100`.
- **Be cheap and touch no state.** `render()` runs every tick under the matrix
  lock on the low-priority workqueue, and its output must be a pure function of
  the frame. Changing RGB state (the active effect, power, colour) is a
  trigger's job, not a filter's.

A filter is gated, ordered, and split-pushed exactly like a compositor overlay,
so it goes in the same `keypaw,rgb-overlays` container and the same paint-order
list.

## Declaration order

Effects and overlays are identified by their child index, and on a split build
that index travels the link. **Both halves must build the same set of effects
and overlays, in the same order.** The same applies across a flash: an effect's
state is addressed by its slot, so reordering `&kprgb`'s children while
`CONFIG_SETTINGS=y` re-points persisted state.

## Split checklist

Before shipping a third-party extension, confirm:

- [ ] The module includes only `<zmk/rgb_matrix.h>` / `<zmk/rgb_matrix_math.h>`,
      never `src/rgb_matrix_internal.h`.
- [ ] An effect's or overlay kind's config and data structs embed the common
      struct as the first member (a condition needs no such struct).
- [ ] The device is declared with the provided macro (`KP_RGB_EFFECT_DEFINE` /
      `KP_RGB_CONDITION_DEFINE` / `KP_RGB_OVERLAY_DEFINE`), not
      `DEVICE_DT_DEFINE` directly.
- [ ] Any central-only state read is gated with `IS_ENABLED(...)` and returns a
      safe default on the peripheral, without gating the device out.
- [ ] Custom overlay kinds should assert about its `opacity` being non-`100`
      unless its `render()` truly replaces every pixel, independent of previous
      values.

And if you are also drafting devicetree definitions:

- [ ] A custom overlay kind's node is a child of `keypaw,rgb-overlays`.
- [ ] Effect and overlay declaration order is identical on both halves.
