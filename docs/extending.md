# Extending the RGB matrix

This guide is for authors of third-party modules that wish to add RGB matrix
effects, overlays, or conditions. The public headers to be used are
[`<zmk/rgb_matrix.h>`](../include/zmk/rgb_matrix.h), and
[`<zmk/rgb_matrix_math.h>`](../include/zmk/rgb_matrix_math.h) for math helpers.

Some example modules are available:

| Extension | Example |
|---|---|
| Effect | [`modules/zmk-rgb-effect-example@keypaw48-zmk`](https://github.com/gudzpoz/keypaw48-zmk/tree/main/modules/zmk-rgb-effect-example) (comet sweep, own binding + renderer) |
| Condition | [`modules/zmk-behavior-dynamic-macro@keypaw48-zmk`](https://github.com/gudzpoz/keypaw48-zmk/tree/main/modules/zmk-behavior-dynamic-macro/src/condition_dynamic_macro.c) (`dm_rec`, recorder-lit overlay) |

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
> header and changes without notice. Use the public headers listed above.
>
> **Public API contexts**: Many APIs should not be called within ISR contexts. A
> rule of thumb is that, unless it's documented like (`zmk_rgb_matrix_flush()`,
> `kp_rgb_condition_invalidate()`), do not call any API function under ISR
> context.

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
`keypaw,rgb-overlay` node or any other overlays that allow the user to customize
their rendering.

### 1. Binding

Include the shared fragment and add your own properties:

```yaml
# dts/bindings/keypaw,rgb-matrix-mything.yaml
compatible: "keypaw,rgb-matrix-mything"
include: "keypaw,rgb-matrix-effect-common.yaml"

properties:
  spread:
    type: int
    required: false
    default: 40
```

`keypaw,rgb-matrix-effect-common.yaml` provides `color` and `duration`.

Since each effect implementation is a Zephyr device driver, we expect you to
structure your code like this, following Zephyr driver conventions:

```c
#define DT_DRV_COMPAT keypaw_rgb_matrix_mything

// Your #include clauses here...
#include <zephyr/device.h>
#include <zmk/rgb_matrix.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

// Your implementation code here...

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
```

> Each effect is, in fact, a Zephyr device. And these structs are the metadata
> we will be storing into the device's metadata. Read [Zephyr's Device Driver
> Model](https://docs.zephyrproject.org/latest/kernel/drivers/index.html#driver-data-structures)
> for more information. (For example, you should at least know that:
>
> 1. Each device driver has their own config struct and data struct,
>
> 2. and config structs should store immutable configs, while data structs are
>    mutable at runtime.)

#### Devicetree string enum helper

A string property with an `enum`, as the rainbow effect's `basis` uses, is
awkward to check in C. A few helpers turn it into a real C enum:

```yaml
properties:
  basis:
    type: string
    required: false
    default: "x"
    enum: ["x", "y", "radial", "pinwheel", "spiral", "chevron", "flag"]
```

```c
// Define the enum: the allowed values must still be listed in the YAML.
DEFINE_DT_ENUM(kp_rainbow_basis_t, basis, x, y, radial, pinwheel, spiral, chevron, flag);
// The first argument is the exact typedef name; no suffix is added.

// Convert a devicetree value in a config initializer:
.basis = CONV_DT_ENUM(inst, basis),

// Compare against a named constant in render code:
if (cfg->basis == DT_ENUM_CONST(basis, spiral)) { ... }
```

### 2. Config and data structs

We store the metadata required by the engine (and, potentially, your effect) in
the config and data struct slots provided by Zephyr. Both structs must embed the
shared structs **as their first member**, **name exactly as "common"**:

```c
// src/mything.c
struct kp_eff_mything_config {
  /* must be first, named exactly as "common" */
  struct kp_rgb_effect_common_config common;
  /* your own extra config */
  uint16_t spread;
};
struct kp_eff_mything_data {
  /* must be first, named exactly as "common" */
  struct kp_rgb_effect_common_data common;
  /* your own state */
  uint16_t radius;
};
```

> `struct kp_rgb_effect_common_data` contains the common properties define in
> `keypaw,rgb-matrix-effect-common.yaml`, like the accent color and animation
> duration. You should be using these in your implementation if you want user
> bindings to work correctly for your effect.

To implement effects and overlays, you also need to provide some callback
functions through a `kp_rgb_effect_callbacks` table:

```c
struct kp_rgb_effect_callbacks {
  bool (*render)(const struct device *dev, const struct kp_rgb_frame *frame);
  bool (*on_event)(const struct device *dev, const struct kp_rgb_key_event *event);
  void (*set_active)(const struct device *dev, bool active, int64_t now_ms);
  void (*reset)(const struct device *dev, int64_t now_ms);
};
```

Among those, `render` is required and all others are optional. The callbacks
table is directly stored in the Zephyr device API field (`dev->api`).

### 3. Rendering with `render`

`render(dev, frame)` is called by the engine to paint the effect while it is
active. All the information that a typical effect should need (LED positions
`coords`, timing, etc.), as well as the output RGB array (`pixels`), are all in
the `const struct kp_rgb_frame *` pointer.

> There are a few time-related fields in `field`: `local_ms`, `clock_ms` and
> `elapsed_ms`. `local_ms` is a monotonically increasing local uptime. But if
> you want your effect to synchronize between split halves, you should make sure
> that a periodic effect takes its phase from `frame->clock_ms`, which is
> synchronized across the split halves:
>
> ```c
> uint32_t phase = f->clock_ms % kp_rgb_effect_period(dev);
> ```
>
> Note that `clock_ms` wraps after about 49.7 days, where a one-frame glitch
> might happen.
>
> In some rare cases, `elapsed_ms` can be zero, so be mindful of zero divisions.

However, there are an extra level of indirection here to help with overlay
partial rendering: `frame->targets` (and `frame->target_count`) stores the
indices of the LEDs you should write to. Here is a demonstration:

```c
static bool kp_eff_mything_render(const struct device *dev,
                                  const struct kp_rgb_frame *f) {
  const struct kp_eff_mything_config *cfg = dev->config;
  struct kp_eff_mything_data *data = dev->data;
  uint32_t period = kp_rgb_effect_period(dev);
  uint32_t phase = f->clock_ms % period;

  for (size_t target = 0; target < f->target_count; target++) {
    size_t i = f->targets[target];
    struct kp_rgb_hsb hsb = data->common.color;
    /* ... derive hsb from f->coords[i] and phase ... */
    f->pixels[i] = kp_rgb_hsb_to_rgb(hsb);
  }

  return true;
}
```

See the comments for `struct kp_rgb_frame` in
[`<zmk/rgb_matrix.h>`](../include/zmk/rgb_matrix.h) for details.

Helpers that do some bookkeeping for you:

- `kp_rgb_effect_period(dev)`: the effect's animation period, never 0.
- `kp_rgb_effect_data(dev)` / `kp_rgb_effect_cfg(dev)`: typed access to the
  common part.
- `kp_rgb_rgb_scale()` / `kp_rgb_rgb_mix()`: brightness and compositing.
- `KP_RGB_HSB_FROM_HEX(hex)`: compile-time preset colour from a devicetree
  constant, and `kp_hex_to_rgb(hex)` for an RGB literal.
- `<zmk/rgb_matrix_math.h>`: `kp_rgb_sin8` (eased oscillation),
  `kp_rgb_atan2_8`, `kp_rgb_isqrt`, `kp_rgb_arm_phase`.

You must return `true` if your effect is animating, where the engine will try
its best to call your `render` constantly to keep the animation going. Returning
`false`, however, means the frame you rendered is temporarily static (when the
engine can save some CPU power by calling `render` less often), and you take the
responsibility to notify the engine when re-rendering is need, through
`zmk_rgb_matrix_flush()` or the return value of the `on_event` callback below.

Every painted frame renders all active, unoccluded contributors, including
settled effects when another participant animates. There is no prefix cache.
Input requests repaint only when an active participant's `on_event` returns true;
a settled effect must request repaint when its output changes.

> Here's three examples of typical usages of the different return values:
>
> 1. The `keypaw,rgb-matrix-solid` effect: fully static. Its `render` returns
>    `false`, but it still paints whenever another participant or an explicit
>    request causes composition.
>
> 2. The `keypaw,rgb-matrix-breathe` effect: always animating. Its LEDs are
>    always changing in brightness, so its `render` always return `true` so that
>    the engine will re-render it every frame.
>
> 3. The `keypaw,rgb-matrix-reactive` effect: animating pressed keys. It's
>    static if there's no keypress, but it will light up and then gradually dim
>    the LEDs when a keypress lands. Therefore, its `render` returns `false`
>    when there are no recent keypresses, and `true` if there is at least one
>    animating keypress.

### 4. Optional key event feedback through `on_event`

Supply an `on_event` callback to receive key events (keydown & keyup). The event
payload contains key index and a timestamp. Use the `kp_rgb_led_for_position`
public accessor to map the key index to its LED:

```c
static bool kp_eff_mything_event(const struct device *dev,
                                 const struct kp_rgb_key_event *ev) {
  if (!ev->pressed) {
    return false;
  }
  size_t led = kp_rgb_led_for_position(ev->position);
  if (led == SIZE_MAX) {
    return false;
  }
  /* Store feedback using ev->timestamp_ms. */
  return true;
}
```

Return `true` to request repainting after changing visual or future-animation
state; return `false` for ignored input. There's no event ordering and delivery
guaranteed though: you might receive older keypresses first, and you might even
miss some (if your chip is under some super heavy load).

### 5. Lifecycle callbacks: `set_active` and `reset`

The engine calls `set_active(true)` before the effect becomes visible, and,
after that, `set_active(false)` whenever the effect becomes completely invisible
(either the user switches to another effect, or a fully opaque overlay is now
overlaid upon the effect). `reset` can be called pretty much any time (on an
explicit RGB reset, or when a half resumes from a pause).

Effect/overlay callbacks run under the matrix mutex on the RGB worker. Please
try not to control the engine from those callbacks, or else the engine might
misbehave.

### 6. Instantiate

It's quite similar to how you would instantiate a ZMK/Zephyr driver. First you
need a struct storing your callbacks:

```c
static const struct kp_rgb_effect_callbacks kp_eff_mything_callbacks = {
  .render = kp_eff_mything_render,
  .on_event = kp_eff_mything_event,
};
```

Then you write a macro initializing config structs and data structs for an
instance of your effect. Please use the helper macros
`KP_RGB_EFFECT_COMMON_CONFIG` and `KP_RGB_EFFECT_COMMON_DATA` to initialize the
common data the engine requires:

```c
#define KP_MYTHING_DEFAULTS(inst) KP_RGB_EFFECT_DEFAULTS(inst, 0, 0)
#define KP_EFF_MYTHING_DEFINE(inst)                                            \
  static const struct kp_eff_mything_config kp_eff_mything_##inst##_cfg = {    \
      KP_RGB_EFFECT_COMMON_CONFIG(inst, KP_MYTHING_DEFAULTS(inst)),            \
      .spread = DT_PROP_OR(DT_DRV_INST(inst), spread, 40),                     \
  };                                                                           \
  static struct kp_eff_mything_data kp_eff_mything_##inst##_data = {           \
      KP_RGB_EFFECT_COMMON_DATA(KP_MYTHING_DEFAULTS(inst))                     \
  };                                                                           \
  KP_RGB_EFFECT_DEFINE(DT_DRV_INST(inst), &kp_eff_mything_callbacks,           \
                       kp_eff_mything_##inst)
```

Then, similar to Zephyr device driver, you run your macro on every effect
instance with `DT_INST_FOREACH_STATUS_OKAY`:

```c
DT_INST_FOREACH_STATUS_OKAY(KP_EFF_MYTHING_DEFINE)
```

`KP_RGB_EFFECT_DEFAULTS` is a small helper macro to define `default_color_` and
`default_duration_` arguments for the effect. Those arguments apply only when
the corresponding property is absent, and are restored upon selector/global
reset.

## Configuring an overlay

Overlays are rendering layers and the main devicetree API to customize the RGB
matrix. Typically, base effects that cover the whole keyboard are
`controllable`, `all-leds` unconditional "selector" overlays, while layer
indicators, CapsLock indicators, etc. are achieved through single-LED overlays.

An overlay composites at its declaration position under `keypaw,rgb-matrix`:
later enabled children paint on top. The following extends the README's
matrix:

```dts
/ {
    rgb_conditions: rgb_conditions {
        compatible = "keypaw,rgb-conditions";
        cond_rec: cond_rec {
            compatible = "keypaw,rgb-condition-mything";
        };
    };
};

&kprgb {
    rec: rec {
        compatible = "keypaw,rgb-overlay";
        condition = <&cond_rec>;
        keys = <11 23 35>;
        opacity = <100>;
        fx_rec {
            compatible = "keypaw,rgb-matrix-solid";
            color = <0xFF0000>;
        };
    };
};
```

`keypaw,rgb-overlay-common.yaml` provides `keys`, `leds`, `all-leds`, `opacity`,
and `local`; the generic overlay binding adds `condition`. `all-leds;` targets
every LED on the half; and `opacity` controls how much the overlay blends into
the colors below it. Targets resolve per half, so a `keys` list works unchanged
on both halves.

> **More on the built-in `keypaw,rgb-overlay`**: A `keypaw,rgb-overlay` without
> `controllable` is an single-effect overlay, and must have exactly one enabled
> private effect child. With `controllable`, then it is an effect-selector
> overlay with multiple effects as children, which the user can control, select
> or switch to. The overlay itself can be used as a behavior to control the
> selector (like `&zone`).

## Writing a condition

A condition is a reusable sampled predicate consumed by overlays and triggers.
Similar to effects, it is also a Zephyr device.

### 1. Binding

```yaml
compatible: "keypaw,rgb-condition-mything"
include: keypaw,rgb-condition-common.yaml
properties:
  threshold:
    type: int
    default: 20
```

### 2. Predicate

```c
#define DT_DRV_COMPAT keypaw_rgb_condition_mything

#include <zephyr/device.h>
#include <zephyr/sys/util.h>
#include <zmk/rgb_matrix.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static bool kp_cond_mything_sample(const struct device *dev) {
  return /* read synchronized current source state */;
}

/* The condition API */
static const struct kp_rgb_condition_api api = {
  .sample = kp_cond_mything_sample,
};

#define KP_COND_MYTHING_DEFINE(inst) \
  KP_RGB_CONDITION_DEFINE(inst, &api, NULL, NULL)

DT_INST_FOREACH_STATUS_OKAY(KP_COND_MYTHING_DEFINE)

#endif
```

Use the `KP_RGB_CONDITION_DEFINE(inst, api, config, data)` helper to initantiate
your conditions. Required provider state must be ready before automatic control
startup.

The whole condition pipeline is reactive: there is no timed polling, and the
condition implementation must call `kp_rgb_condition_invalidate()` when its
source changes. For example, a CapsLock provider can request a pass when the
host indicator event arrives:

```c
static int listener(const zmk_event_t *eh) {
  ARG_UNUSED(eh);
  kp_rgb_condition_invalidate();
  return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(kp_cond_caps_lock, listener);
ZMK_SUBSCRIPTION(kp_cond_caps_lock, zmk_hid_indicators_changed);
```

### Condition dependencies

A condition may depend on other conditions, like `keypaw,rgb-condition-and`,
with the dependencies listed in the node's `conditions` phandles property:

```dts
combined: combined {
    compatible = "keypaw,rgb-condition-and";
    conditions = <&layer_active &caps_active>;
};
```

### Source scope, split authority and startup

`KP_RGB_CONDITION_ANY_SIDE` is the default scope. Declare
`KP_RGB_CONDITION_CENTRAL_ONLY` when source state is available only on the
central (or standalone device), as with keymap layers or the active host BLE
profile. Keep the device linkable on both halves and compile out unavailable
source reads/listeners on peripherals. Do not substitute a false value as
permission for a local consumer: the engine rejects a local peripheral tree
containing a central-only dependency, transitively.

Scope restricts source evaluation; an overlay's `local` flag chooses gate authority:

- Default overlays use central cached gates and push visibility bits. Peripherals
  consume the bits without sampling that overlay's tree.
- `local;` overlays evaluate a tree independently on each half. Both halves must
  have valid sources. Renderers already run locally, so a battery gauge's local
  charge does not itself require local gate evaluation.

Provider sources must be ready before initial sampling and startup actions.

### Repainting on an event

`zmk_rgb_matrix_flush()` requests a scene pass that reconciles activity, then
repaints when output is eligible. A suppressed pass does not start periodic
rendering. It is any-context, non-blocking and coalescing, and does not reset
the animation clock or grant output permission. Use it for changed pixels, not
condition-source notification: it does not sample conditions or triggers. A
source event listener must publish its state and invalidate the corresponding
condition as above.

## Writing a trigger

A trigger maps a condition to a list of one-shot behavior commands. Triggers are
the children of a `keypaw,rgb-trigger-table` node. Each one fires independently:
its `on-enter` commands run on the condition's rising edge (false -> true) and
its optional `on-exit` on the falling edge (true -> false). **Declaration order
is the firing order** — it matters only when several edges land on the same
evaluation.

```dts
/ {
    rgb_conditions: rgb_conditions {
        compatible = "keypaw,rgb-conditions";
        cond_layer1: cond_layer1 {
            compatible = "keypaw,rgb-condition-layer";
            layer = <1>;
        };
    };

    rgb_triggers: rgb_triggers {
        compatible = "keypaw,rgb-trigger-table";
    };
};

&rgb_triggers {
    /* Entering layer 1 selects effect 1; leaving it restores effect 0. */
    trig_fn: trig_fn {
        condition = <&cond_layer1>;
        on-enter = <&zone_a RGB_EFS_CMD 1>;
        on-exit = <&zone_a RGB_EFS_CMD 0>;
    };
};
```

A rule with no `condition` is unconditional and fires `on-enter` once after
startup restoration. Previous condition values start false: initial true runs
`on-enter`, initial false is silent. At least one action list is required.

Semantics worth knowing:

- The evaluator runs on the split **central only** (conditions read keymap
  state), but the emitted commands reach both halves because `&zone_a` is
  `BEHAVIOR_LOCALITY_GLOBAL`.
- Condition sampling and trigger evaluation are independent of rendering and
  continue while RGB is OFF, inhibited, or has no local LEDs. Conditions are
  sampled initially and on source/dependency invalidation or engine refresh;
  there is no timed polling. Render ticks do not poll conditions. Trigger actions
  run asynchronously as part of control processing.
- Rules are **independent and edge-triggered**: one never shadows another, so a
  manual `RGB_EFF`/`RGB_EFS` survives between conditions instead of being
  re-fired on the intervening ticks. `on-exit` is how you undo a change when the
  condition clears; a falling edge with no `on-exit` does nothing.
- A relative command (`RGB_HUI`, `RGB_BRI`, ...) re-fires on every rising edge of
  its condition, so a condition that flickers will act on each toggle.
- All rules use the same cached condition values for a control pass. Actions run
  in declaration order without resampling between rules. Changes caused by an
  action take effect in a subsequent queued pass; earlier actions are not rolled
  back if another source change interrupts the pass. Intermediate source changes
  may coalesce before sampling.
- Rules must not form feedback loops, and condition samples must be side-effect-free
  and must not synchronously invalidate conditions. There is no runtime feedback
  detector, pass limit or quarantine. Invalid rules can keep work, radio traffic
  or settings writes running and prevent painting indefinitely. DTS dependency-cycle
  checks validate the condition graph, not action-driven feedback.
- Each action receives one pressed invocation, never a matching release.
  `on-exit` invokes its own actions; it does not release `on-enter` actions.
  You must choose behaviors that complete without a release. Third-party one-shot
  behaviors are allowed; ordinary `&kp`, momentary layers and hold-tap are unsuitable.

## Writing an overlayed filter

In the previous sections, we said:

> Under the hood, an overlay is a compositor...

Well, that was only partly true: you can use our built-in `keypaw,rgb-overlay`,
which is merely a compositor, but you can also code your own overlay for other
fancy effects.

An overlay runs at its fixed position in the scene, before the engine emits
final output to the LED strip. It can transform the output of earlier overlays.

Here is an untested example of writing a custom overlay to filter on the output:
`render()` receives a const frame whose target pixels contain earlier overlays'
output (black if none have painted). Transform only those target pixels.

> For an actual example, see [our battery overlay
> implementation](../src/overlays/battery.c);

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

static bool kp_ovl_myfilter_render(const struct device *dev,
                                   const struct kp_rgb_frame *frame) {
  const struct kp_ovl_myfilter_config *cfg = dev->config;
  for (size_t target = 0; target < frame->target_count; target++) {
    size_t i = frame->targets[target];
    struct led_rgb filtered = kp_rgb_rgb_scale(frame->pixels[i], 50);
    frame->pixels[i] = kp_rgb_rgb_mix(frame->pixels[i], filtered,
                                     cfg->common.opacity);
  }
  return false;
}

static const struct kp_rgb_effect_callbacks kp_ovl_myfilter_callbacks = {
  .render = kp_ovl_myfilter_render,
};

#define KP_OVL_MYFILTER_DEFINE(inst)                                           \
  KP_RGB_OVERLAY_TARGET_ARRAYS(inst, kp_ovl_myfilter_##inst);                  \
  static const struct kp_ovl_myfilter_config kp_ovl_myfilter_##inst##_cfg = {  \
      .common = KP_RGB_OVERLAY_COMMON(DT_DRV_INST(inst),                       \
                                     &kp_ovl_myfilter_callbacks, false),       \
  };                                                                           \
  static struct kp_ovl_myfilter_data kp_ovl_myfilter_##inst##_data = {         \
      KP_RGB_OVERLAY_COMMON_DATA(inst, kp_ovl_myfilter_##inst),                \
  };                                                                           \
  KP_RGB_OVERLAY_DEFINE(inst, kp_ovl_myfilter_##inst)

DT_INST_FOREACH_STATUS_OKAY(KP_OVL_MYFILTER_DEFINE)

#endif
```

`KP_RGB_OVERLAY_TARGET_ARRAYS(inst, cfg)` must precede the config and data
objects. It declares const `uint32_t` raw key and LED arrays plus a mutable
buffer for resolved index storage, populated by one internal registry pass
immediately after layout geometry at `POST_KERNEL OBJECTS`, before overlay
device initialization.

A filter is gated, ordered, and split-pushed exactly like a compositor overlay,
so it goes directly under the same `keypaw,rgb-matrix` node, and its declaration
order there is its paint order. Read
[`keypaw,rgb-overlay-common.yaml`](../dts/bindings/keypaw,rgb-overlay-common.yaml)
for more.

> You may or may not want to embed private effects in your custom overlay. But
> if you do, you are then responsible for passing the lifecycle events down to
> your private effect children. Read [the built-in
> overlay](../src/overlays/overlay.c) for an example and the used helper
> functions.

## Declaration order

Controller-selectable effects and registered overlays are identified by their
child index, and on a split build that index travels the link. **Both halves
must build the same set of registry effects and overlays, in the same order.**
The same applies across a flash: an effect's state is addressed by its slot, so
reordering `&kprgb`'s children while `CONFIG_SETTINGS=y` re-points persisted
state. Also, overlay declaration order is the paint order.

Settings address selectors and selected members by enabled DTS declaration
position, not name. Reordering members can redirect saved selections; overlay order
is not persisted.

## External output inhibition and activity

> This might only be useful if you're hoping to implement a battery-saving mode
> for your keyboard.

A single external policy owner can call `zmk_rgb_matrix_set_inhibited(bool)`
from `<zmk/rgb_matrix.h>` to gate local output permission. The gate defaults to
`false`, is runtime-only and does not change logical ON/user intent, saved
settings, or normal split commands.

Activity state is handled separately from external inhibition. `idle-brightness`
defaults to 0 and is constrained to 0..100. ACTIVE output uses the
`max-brightness` cap; IDLE uses `min(idle-brightness, max-brightness)` and then
master-brightness scaling. At IDLE brightness 0, output is logically OFF while
retained user intent remains set. A nonzero idle brightness dims without resetting
effects or restarting the shared clock. `zmk_rgb_matrix_get_state()` reports
retained intent after activity suppression, independent of inhibition and output
scale; `zmk_rgb_matrix_get_user_state()` reports retained intent directly.

SLEEP is always logically OFF. Each newly observed SLEEP transition causes one
synchronous black-strip call before the activity listener returns if the matrix
is initialized and has LEDs, even when already off or inhibited. A failed call
is logged without automatic retry and can leave the strip lit indefinitely.
A later output-blocking transition or newly observed SLEEP transition can attempt
black again. There is no guarantee of darkness after failure or of driver-call
duration; validate actual electrical behavior on hardware.

## Split checklist

Before shipping a third-party extension, confirm:

- [ ] The module uses public RGB headers like `<zmk/rgb_matrix.h>` and
      `<zmk/rgb_matrix_math.h>`, never `src/rgb_matrix_internal.h`.
- [ ] An effect's or overlay kind's config and data structs embed the common
      struct as the first member (a condition needs no such struct).
- [ ] RGB kinds use the provided helpers (`KP_RGB_EFFECT_DEFINE` /
      `KP_RGB_CONDITION_DEFINE` / `KP_RGB_OVERLAY_DEFINE`).
  - [ ] If you really need to manually declare your Zephyr devices, use
        `POST_KERNEL`, `KP_RGB_DEVICE_INIT_PRIORITY`.
- [ ] Any central-only state read is gated with `IS_ENABLED(...)` and returns a
      safe default on the peripheral, without gating the device out.
- [ ] Custom overlays should declare `replaces_target=false` if their image
      depends on incoming pixels, or, otherwise, `replaces_target=true`.

And if you are also drafting devicetree definitions:

- [ ] A `GLOBAL` behavior's effective device name is 8 characters or fewer. This
      includes the matrix: use `kprgb: kprgb` with `#binding-cells = <2>`.
- [ ] Exactly one enabled matrix owns geometry/global defaults and direct overlay
      children; there is no separate global behavior/container or compatibility bridge.
- [ ] Overlay/effect members and their ordering are identical on both halves;
      both firmware sides are rebuilt whenever addressing shifts.
