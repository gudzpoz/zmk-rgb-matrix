# Extending the RGB matrix

This guide is for authors of third-party modules that wish to add RGB matrix
effects, overlays, or conditions. Everything here uses only the public API in
[`<zmk/rgb_matrix.h>`](../include/zmk/rgb_matrix.h).

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
`keypaw,behavior-rgb-matrix` node, making it selectable, cyclable,
split-addressed, and optionally persisted. They can also be embedded as children
of overlays (e.g., a `keypaw,rgb-overlay` node), when it will become "private",
never user-selectable or persisted.

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
and `no-overlays`. The overlay-list properties apply only to selectable effects.

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
  uint32_t phase_ms;
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

Among those, `render` is required and all others are optional.

### 3. Rendering with `render`

`render(dev, frame)` is called by the engine to paint the effect while it is
active. All the information that a typical effect should need (LED positions
`coords`, timing `elapsed_ms`, etc.), as well as the output RGB array
(`pixels`), are all in the `const struct kp_rgb_frame *` pointer.

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

  for (size_t target = 0; target < f->target_count; target++) {
    size_t i = f->targets[target];
    struct kp_rgb_hsb hsb = data->common.color;
    /* ... derive hsb from f->coords[i] and data->phase_ms ... */
    f->pixels[i] = kp_rgb_hsb_to_rgb(hsb);
  }

  data->phase_ms = ((uint64_t)data->phase_ms + f->elapsed_ms) % period;
  return true;
}
```

See the comments for `struct kp_rgb_frame` in
[`<zmk/rgb_matrix.h>`](../include/zmk/rgb_matrix.h) for details.

Helpers that do some bookkeeping for you:

- `kp_rgb_effect_period(dev)`: the effect's animation period, never 0.
- `kp_rgb_effect_data(dev)` / `kp_rgb_effect_cfg(dev)`: typed access to the
  common part.
- `kp_rgb_hsb_scale()` / `kp_rgb_rgb_scale()` / `kp_rgb_rgb_mix()`: brightness
  and compositing.
- `KP_RGB_HSB_FROM_HEX(hex)`: compile-time preset colour from a devicetree
  constant, and `kp_hex_to_rgb(hex)` for an RGB literal.
- `<zmk/rgb_matrix_math.h>`: `kp_rgb_sin8` (eased oscillation),
  `kp_rgb_atan2_8`, `kp_rgb_isqrt`, `kp_rgb_arm_phase`.

> Note that there's a global brightness enforced by the engine, which means you
> probably should not explicitly apply board-wide brightness adjustments in your
> effect implementation.

You must return `true` if your effect is animating, where the engine will try
its best to call your `render` constantly to keep the animation going. Returning
`false`, however, means the frame you rendered is temporarily static (when the
engine can save some CPU power by calling `render` less often), and you take the
responsibility to notify the engine when re-rendering is need, through
`kp_rgb_effect_invalidate(dev)` or the return value of the `on_event` callback
below.

While an animation is running, the engine caches the composed result of the
settled layers at the bottom of the scene and re-renders only the layers above
the first animating one. A settled participant can therefore be skipped on later
animation frames; it is still rendered on every requested composition (a flush,
input, or scene change), and it must invalidate whenever its output changes.

> Here's three examples of typical usages of the different return values:
>
> 1. The `keypaw,rgb-matrix-solid` effect: fully static. Its `render` always
>    returns `false`, and the engine will only update the LEDs on boot and when
>    waking up.
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

The engine calls `set_active(true)` before input delivery or then the effect
becomes visible, and, after that, `set_active(false)` whenever the effect
becomes completely invisible (either the user switches to another effect, or a
fully opaque overlay is now overlaid upon the effect). `reset` can be called
pretty much any time (maybe during synchronization or upon user requests), but a
call is guaranteed before first `set_active(true)` call.

We don't enforce what you do in these callbacks, but here are some
recommendations if you want your animations smoother:

- For `reset`: reset your animtation phase or state, so that after two halves
  (in a split keyboard) call `reset` at the same time, the animation will be in
  sync.
- For `set_active(false)`: clear keypress feedbacks, unless you want your user
  to see their keypresses from an hour ago still linger when they switch back
  from another effect.
- Also, please try to keep the callbacks "pure": do not modify the RGB engine
  state by, for example, switching to another effect when `set_active(false)`.

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
#define KP_EFF_MYTHING_DEFINE(inst)                                            \
  static const struct kp_eff_mything_config kp_eff_mything_##inst##_cfg = {    \
      KP_RGB_EFFECT_COMMON_CONFIG(inst),                                       \
      .spread = DT_PROP_OR(DT_DRV_INST(inst), spread, 40),                     \
  };                                                                           \
  static struct kp_eff_mything_data kp_eff_mything_##inst##_data = {           \
      KP_RGB_EFFECT_COMMON_DATA(inst, 0, 0)                                    \
  };                                                                           \
  KP_RGB_EFFECT_DEFINE(DT_DRV_INST(inst), &kp_eff_mything_callbacks,           \
                       kp_eff_mything_##inst)
```

Then, similar to Zephyr device driver, you run your macro on every effect
instance with `DT_INST_FOREACH_STATUS_OKAY`:

```c
DT_INST_FOREACH_STATUS_OKAY(KP_EFF_MYTHING_DEFINE)
```

## Configuring an overlay

Overlays are typically used as indicators: layer indicators, CapsLock
indicators... Under the hood, an overlay is a compositor: while its condition is
active it renders one effect and blends the result over the LEDs it targets. It
is not a behavior and cannot be keymap-bound; it is declared as a child of a
`keypaw,rgb-overlays` container, and its paint order is its declaration order
there (a later child composites on top).

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
        opacity = <100>;
        fx_rec {
            compatible = "keypaw,rgb-matrix-solid";
            #binding-cells = <0>;
            color = <0xFF0000>;
        };
    };
};
```

`keypaw,rgb-overlay-common.yaml` provides `keys`, `leds`, `all-leds`, `opacity`,
and `local`; the generic overlay binding adds `condition`. `all-leds;` targets
every LED on the half; and `opacity` controls how much the overlay blends into
the colors below it.

A generic overlay must have exactly one enabled private effect child. The
private effect has independent state, is not selectable or persisted, and does
not follow adjustments to a selectable effect's color. Its node must still
declare `#binding-cells = <0>` to satisfy the effect binding.

Targets resolve per half, so a `keys` list works unchanged on both halves.
Every registered overlay is used by default; a selectable effect may provide
`overlays` in an explicit order or `no-overlays`.

## Writing a condition

A condition is a reusable sampled predicate consumed by overlays and triggers.
Similar to effects, it is also a Zephyr device.

### 1. Binding

```yaml
compatible: "keypaw,rgb-condition-mything"
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

static bool kp_cond_mything_sample(const struct device *dev, int64_t now_ms,
                                   int64_t *next_wakeup_ms) {
  /* You may use `next_wakeup_ms` to schedule a condition update: */
  *next_wakeup_ms = now_ms + 5000;
  /* Or, if you can detect conditon changes through events, we recommend: */
  ARG_UNUSED(next_wakeup_ms); // and then use events to notify the engine

  return /* read synchronized source state */;
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

The RGB engine, and conditions, are reactive, which means a condition must
either (1) explicitly as the engine to poll it (through `next_wakeup_ms`, see
above), or (2) notify the engine whenever the condition changes with
`kp_rgb_condition_invalidate`. Here is an example for a CapsLock condition
notification:

```c
#define INVALIDATE(inst)                                                       \
  kp_rgb_condition_invalidate(DEVICE_DT_GET(DT_DRV_INST(inst)));
static int listener(const zmk_event_t *eh) {
  const struct zmk_hid_indicators_changed *ev =
      as_zmk_hid_indicators_changed(eh);
  if (ev) {
    DT_INST_FOREACH_STATUS_OKAY(INVALIDATE)
  }
  return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(kp_cond_caps_lock, listener);
ZMK_SUBSCRIPTION(kp_cond_caps_lock, zmk_hid_indicators_changed);
```

After changing source state, call `kp_rgb_condition_invalidate(dev)`. It is
any-context, non-blocking, and coalescing; synchronize source data separately.
There is no polling fallback or guarantee that every intermediate transition
will be sampled. Sampling runs on the control worker and must not execute
behaviors, write LEDs, or recursively sample another provider.

### Source scope, split authority and startup

`KP_RGB_CONDITION_ANY_SIDE` is the default scope. Declare
`KP_RGB_CONDITION_CENTRAL_ONLY` when source state is available only on the central
(or standalone device), as with keymap layers. Keep the device linkable on both
halves and compile out unavailable source reads/listeners on peripherals. Do not
substitute a false value as permission for a local consumer: the engine rejects a
local peripheral tree containing a central-only dependency, transitively.

Scope restricts source evaluation; an overlay's `local` flag chooses gate authority:

- Default overlays use central cached gates and push visibility bits. Peripherals
  consume the bits without sampling that overlay's tree.
- `local;` overlays evaluate a tree independently on each half. Both halves must
  have valid sources. Renderers already run locally, so a battery gauge's local
  charge does not itself require local gate evaluation.

All configured local and central roots remain live during logical OFF, idle
suppression, inhibition and zero local LEDs. Stage 1 does not prune consumer demand.

Provider sources must be ready before initial sampling and startup actions.
`CONFIG_KEYPAW_RGB_CONTROL_AUTO_START=y` starts via the settings commit hook at
`INT_MAX` after full key loading/lower-priority commits, or APPLICATION 99 with
settings disabled. It does not guarantee readiness after equal-priority commits,
later asynchronous initialization or subtree loads. Such integrations must disable
auto-start and call the idempotent, thread-only `zmk_rgb_matrix_start()` after
defaults, full settings restoration and provider readiness. See
[startup readiness](development.md#startup-readiness) for the init-priority
constraint and full contract; no ZMK patch/private symbols are needed.

### Repainting on an event

`zmk_rgb_matrix_flush()` requests a scene pass that reconciles activity, then
repaints when output is eligible. A suppressed pass does not start periodic
rendering. It is any-context, non-blocking and coalescing, and does not reset
the animation clock or grant output permission. Use it for changed pixels, not
condition-source notification: it does not sample conditions or triggers. A
source event listener must publish its state and invalidate the corresponding
condition as above.

## Writing a trigger

A trigger maps a condition to a list of `&kprgb` commands. Triggers are the
children of a `keypaw,rgb-trigger-table` node. Each one fires independently: its
`on-enter` commands run on the condition's rising edge (false -> true) and its
optional `on-exit` on the falling edge (true -> false). **Declaration order is
the firing order** — it matters only when several edges land on the same
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
        on-enter = <&kprgb RGB_EFS_CMD 1>;
        on-exit = <&kprgb RGB_EFS_CMD 0>;
    };
};
```

A rule with no `condition` is unconditional. With the default `startup =
"enter-if-active"`, it fires once after startup readiness. `startup =
"baseline"` records initial state without firing. At least one action list is
required; legacy trigger `bindings` is rejected.

Semantics worth knowing:

- The evaluator runs on the split **central only** (conditions read keymap
  state), but the emitted commands reach both halves because `&kprgb` is
  `BEHAVIOR_LOCALITY_GLOBAL`.
- The independent control worker samples dirty/due conditions and dispatches
  edges even while RGB is OFF, inhibited, or has no local LEDs. Providers must
  invalidate their conditions or supply a deadline; render ticks do not poll them.
- Rules are **independent and edge-triggered**: one never shadows another, so a
  manual `RGB_EFF`/`RGB_EFS` survives between conditions instead of being
  re-fired on the intervening ticks. `on-exit` is how you undo a change when the
  condition clears; a falling edge with no `on-exit` does nothing.
- A relative command (`RGB_HUI`, `RGB_BRI`, ...) re-fires on every rising edge of
  its condition, so a condition that flickers will act on each toggle.
- With `startup = "enter-if-active"`, initial true runs `on-enter`; initial false
  runs no action. Intermediate source changes may coalesce before sampling.
- Each action receives one pressed invocation, never a matching release.
  `on-exit` invokes its own actions; it does not release `on-enter` actions.
  You must choose behaviors that complete without a release. Third-party one-shot
  behaviors are allowed; ordinary `&kp`, momentary layers and hold-tap are unsuitable.

> There is deliberately no `triggers = <...>;` phandle list on the table or on
> `&kprgb`: a parent pointing at its own child is a devicetree cycle, and the
> failure mode is obscure. See
> [development.md](development.md#why-there-is-no-triggers-list) for the full
> story.

## Writing an overlayed filter

In the previous sections, we said:

> Under the hood, an overlay is a compositor...

Well, that was only partly true: you can use our built-in `keypaw,rgb-overlay`,
which is merely a compositor, but you can also code your own overlay for other
fancy effects.

Actually, under the hood, an overlay is no more than a processor that is run
after effect rendering, and before the engine emits the final output to the LED
strip, and it can do anything to transform the output.

Here is an untested example of writing a custom overlay to filter on the output:
`render()` receives a const frame whose target pixels contain the effect and
earlier overlays' output. Transform only those target pixels.

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
      .common =                                                                \
          KP_RGB_OVERLAY_COMMON(DT_DRV_INST(inst), kp_ovl_myfilter_##inst),    \
  };                                                                           \
  static struct kp_ovl_myfilter_data kp_ovl_myfilter_##inst##_data;            \
  KP_RGB_OVERLAY_DEFINE(inst, &kp_ovl_myfilter_callbacks, false,               \
                        kp_ovl_myfilter_##inst)

DT_INST_FOREACH_STATUS_OKAY(KP_OVL_MYFILTER_DEFINE)

#endif
```

A filter is gated, ordered, and split-pushed exactly like a compositor overlay,
so it goes in the same `keypaw,rgb-overlays` container, and its declaration
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
state.

Overlay declaration order is the default paint order. A selectable effect can
supply `overlays = <&a &b>;` to select and order its overlays, or `no-overlays`
to disable them. A list must not contain duplicates. Several base-effect lists
may name the same overlay because those bases are mutually exclusive. Each
generic overlay still owns its own private child, never a selectable effect.

Unlike effect slots, overlay order is not persisted, so reordering overlays is
safe across a flash as long as both halves use the same devicetree.

## External output inhibition

> This might only be useful if you're hoping to implement a battery-saving mode
> for your keyboard.

A single external policy aggregator can call
`zmk_rgb_matrix_set_inhibited(bool)` from `<zmk/rgb_matrix.h>` to gate the
local strip. The gate defaults to `false`, is runtime-only, and may be set from
thread context before matrix initialization (for example, in `POST_KERNEL`).
It does not change logical ON/user intent, saved settings, or normal split
commands; each split half needs its own local gate owner.

The setter returns `0` when accepted, or `-EWOULDBLOCK` from ISR context. A
completed inhibit call prevents further colored submissions, but does not
promise that the hardware is already black.

While inhibited, effects, overlays, and key feedback do not render or advance;
queued key feedback is discarded. Condition evaluation, triggers, and overlay
synchronization continue independently on invalidations/deadlines even while
logically OFF, including on a stripless central.

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
- [ ] Custom overlays should declare `replaces_target=false` if their image
      depends on incoming pixels, or, otherwise, `replaces_target=true`.
- [ ] A `GLOBAL` behavior's node name is 8 characters or fewer (the split link
      serialises it into a 9-byte field).

And if you are also drafting devicetree definitions:

- [ ] A custom overlay kind's node is a child of `keypaw,rgb-overlays`.
- [ ] Controller-effect slots and overlay ordinals are identical on both halves.
