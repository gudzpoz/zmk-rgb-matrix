# ZMK RGB Matrix

A position-aware RGB matrix module for ZMK. It renders customizable effects and
optional overlays on a Zephyr `led_strip` device, using the positions from a
`zmk,physical-layout`. Multiple matrix behavior nodes can independently own
disjoint LED zones while sharing one physical output engine. The module also
supports keymap behaviors, layer/layout triggers, and a public API for separately
packaged effects.

This module is developed against ZMK `main` and Zephyr 4.1.

## Installation

Add the repository to the `config/west.yml` in a ZMK config repository:

```yaml
manifest:
  remotes:
    - name: gudzpoz
      url-base: https://github.com/gudzpoz
  projects:
    - name: zmk-rgb-matrix
      remote: gudzpoz
      revision: main
```

## Required hardware description

Your shield must already define:

- an enabled WS2812-compatible `led_strip` device with `chain-length`;
- a `zmk,physical-layout` node describing the keyboard keys (with its `keys`
  attribute defining the position of keys); and
- a `keypaw,rgb-matrix` node that connects both.

To render animations, this module requires position information of the LEDs,
which are supplied through the `mapping` attribute in `rgb_matrix`:

```dts
#include <dt-bindings/keypaw/rgb_matrix.h>

/ {
    rgb_matrix: rgb-matrix {
        compatible = "keypaw,rgb-matrix";
        strip = <&led_strip>;
        physical-layout = <&default_layout>;
        mapping = <0 1 2 KP_RGB_NO_KEY(350, 50)>;
    };
};
```

Provide exactly one `mapping` entry per LED in strip order. An integer mapping
entry is a global key position in the physical layout: `0` means that LED is
under the key represented by the first `&key_physical_attrs` entry in your
physical layout, while `5` means the sixth entry. Use `KP_RGB_NO_KEY(x, y)` for
an LED that is not underneath a key; coordinates use the layout's units (`100`
is one key width, similar to `&key_physical_attrs`).

The mapping length must match the strip's `chain-length`. A split keyboard
normally declares one matrix node and one local mapping on each half. See my ZMK
config for a rather complex example: [`rgb_matrix` for the left
half](https://github.com/gudzpoz/keypaw48-zmk/blob/main/boards/shields/keypaw48/keypaw48_left.overlay),
[`rgb_matrix` for the right
half](https://github.com/gudzpoz/keypaw48-zmk/blob/main/boards/shields/keypaw48/keypaw48_right.overlay)
and [the `physical_layout` they map
to](https://github.com/gudzpoz/keypaw48-zmk/blob/main/boards/shields/keypaw48/keypaw48-layouts.dtsi).
## Behavior, effects, and zones

Define one or more two-parameter behaviors and add effect nodes as their
children. Each behavior controls only its own `leds` zone. Effect indices are
scoped to the parent behavior, transmitted over the split link, and must equal
the child declaration position; keep them stable across firmware revisions.
When `leds` is omitted, the behavior owns all LEDs for backward compatibility.
Multiple behaviors must therefore provide unique, disjoint local chain indices.

```dts
/ {
    behaviors {
        rgb_main: rgb_main {
            compatible = "keypaw,behavior-rgb-matrix";
            #binding-cells = <2>;
            display-name = "Main RGB";
            leds = <0 1 2 3 4 5 6 7>;
            overlays = <&caps>;
            initial-effect = <0>;

            fx_main: fx_main {
                compatible = "keypaw,rgb-matrix-solid";
                #binding-cells = <0>;
                index = <0>;
                color = <0xFF0000>;
            };
        };

        rgb_numpad: rgb_numpad {
            compatible = "keypaw,behavior-rgb-matrix";
            #binding-cells = <2>;
            display-name = "Numpad RGB";
            leds = <8 9 10 11 12 13 14 15>;

            fx_numpad: fx_numpad {
                compatible = "keypaw,rgb-matrix-solid";
                #binding-cells = <0>;
                index = <0>;
                color = <0x0000FF>;
            };
        };
    };
};
```

Commands target the behavior they are bound to, so `&rgb_main RGB_TOG` does
not change `rgb_numpad`. The same rule applies to effect selection, colour,
duration, persistence, overlays, and split forwarding. Zone overlap is an
initialization error; the engine leaves the strip disabled rather than applying
implicit blending or precedence.

The existing one-behavior form remains valid:

```dts
behaviors {
    kprgb: kprgb {
        compatible = "keypaw,behavior-rgb-matrix";
        #binding-cells = <2>;
        display-name = "RGB Matrix";
        initial-effect = <0>;
        initial-on;

        fx_solid: fx_solid {
            compatible = "keypaw,rgb-matrix-solid";
            #binding-cells = <0>;
            index = <0>;
            color = <0xFF0000>;
        };
    };
};
```
***
```

### Built-in effects

There are a number of built-in effects:

These effects are grouped by rendering/state family: static fields (`solid`,
`static`), global oscillators (`breathe`, `spectrum`), moving spatial fields
(`rainbow`, `band`), reactive effects (`reactive`, `ripple`), and stochastic
effects (`rain`, `starlight`, `heatmap`, `digital_rain`). The attributes below
select behavior within a compatible; they do not imply unimplemented QMK modes.

<table>
<thead><tr><th>Effect</th><th>Description</th><th>Attributes</th></tr></thead>
<tbody>
<tr><td><code>solid</code></td>
    <td>One flat colour across every LED, or a static hue gradient swept across the board.</td>
    <td><code>axis</code> = <code>none</code> (flat colour) |
        <code>vertical</code> (hue sweep across y) |
        <code>horizontal</code> (hue sweep across x)</td></tr>
<tr><td><code>breathe</code></td>
    <td>Board-wide fade or a position-aware oscillator over one period.</td>
    <td><code>mode</code> = <code>brightness</code> (BREATHING) |
        <code>river</code> (RIVERFLOW) | <code>hue</code> (HUE_BREATHING) |
        <code>pendulum</code> (HUE_PENDULUM) | <code>wave</code> (HUE_WAVE)</td></tr>
<tr><td><code>spectrum</code></td>
    <td>Whole board cycles the hue wheel globally (QMK <code>CYCLE_ALL</code>).</td>
    <td>&mdash;</td></tr>
<tr><td><code>reactive</code></td>
    <td>Lights a pressed key and fades it back; the lit shape is configurable.</td>
    <td><code>spread</code> = <code>point</code> (key only) |
        <code>disc</code> (filled circle) | <code>cross</code> (row + column) |
        <code>nexus</code> (all but the cross, radial falloff)<br>
        <code>spread-radius</code> (disc/nexus radius, in layout units)<br>
        <code>background-brightness</code> (unlit-LED brightness, percent)<br>
        <code>multi</code> (accumulate several presses instead of the latest)<br>
        <code>palette</code> = <code>solid</code> (user hue) |
        <code>gradient</code> (position-based hue)</td></tr>
<tr><td><code>ripple</code></td>
    <td>Radial ripples expand from each pressed key over an unlit background.</td>
    <td><code>background-brightness</code> (unlit background brightness, percent)</td></tr>
<tr><td><code>rainbow</code></td>
    <td>Position-aware rainbow gradient with selectable spatial basis and direction.</td>
    <td><code>basis</code> = <code>x</code> | <code>y</code> | <code>radial</code> |
        <code>pinwheel</code> | <code>spiral</code> | <code>chevron</code>
        (spatial coordinate the colour maps onto)<br>
        <code>direction</code> = <code>out</code> | <code>in</code> (reverse) |
        <code>dual</code> (two centres) | <code>bloom</code> (mirror each half, with basis x)<br>
        <code>palette</code> = <code>rainbow</code> (full hue wheel) |
        <code>solid</code> (single hue, travelling brightness band)</td></tr>
<tr><td><code>band</code></td>
    <td>A single-hue board with a moving saturation or brightness band.</td>
    <td><code>channel</code> = <code>sat</code> (fades saturation) |
        <code>val</code> (fades brightness)<br>
        <code>shape</code> = <code>linear</code> (scroll) |
        <code>pinwheel</code> (rotate) | <code>spiral</code> (wind outward)</td></tr>
<tr><td><code>static</code></td>
    <td>Fixed per-LED colour map (QMK <code>ALPHAS_MODS</code> and similar art); each LED
        takes a colour from the <code>led-colors</code> array.</td>
    <td><code>led-colors</code> (RGB values, one per LED by chain index; LEDs past the end stay off)</td></tr>
<tr><td><code>rain</code></td>
    <td>Randomly lit keys with random colours (QMK <code>PIXEL_RAIN</code>, <code>PIXEL_FLOW</code>,
        <code>RAINDROPS</code>, <code>JELLYBEAN_RAINDROPS</code>, <code>PIXEL_FRACTAL</code>).</td>
    <td><code>mode</code> = <code>pixel</code> / <code>drops</code> (random keys, random hues) |
        <code>jellybean</code> (also randomises saturation) |
        <code>flow</code> (cursor along chain) |
        <code>fractal</code> (single hue pulses from centre)</td></tr>
<tr><td><code>starlight</code></td>
    <td>LEDs turn on and off at random at varying brightness, keeping the user colour.</td>
    <td><code>smooth</code> (ramp brightness instead of switching) |
        <code>dual-hue</code> (jitter hue &plusmn;30) |
        <code>dual-sat</code> (jitter saturation &plusmn;30)</td></tr>
<tr><td><code>heatmap</code></td>
    <td>Per-key heat map of recent keypresses, decaying over time (QMK <code>TYPING_HEATMAP</code>).</td>
    <td><code>decrease-delay-ms</code> (ms idle before one shade of heat is lost)<br>
        <code>spread</code> (radius heating neighbours, layout units)<br>
        <code>area-limit</code> (per-press cap on heat a neighbour receives)<br>
        <code>increase-step</code> (shades added per keypress)<br>
        <code>slim</code> (heat only the pressed key, not neighbours)</td></tr>
<tr><td><code>digital_rain</code></td>
    <td>Matrix-style falling columns: a head falls per column leaving a fading green tail.</td>
    <td><code>color</code> (tail hue; green default) |
        <code>duration</code> (fall speed; shorter is faster)</td></tr>
</tbody>
</table>

Every effect accepts `color`, `duration`, `overlays`, `no-overlays`, and
`no-cycle` attributes (though some attributes might be meaningless to some
effects).

### Built-in commands and behaviors

Use the standard ZMK RGB command definitions from
`<dt-bindings/keypaw/rgb_matrix.h>` in a keymap. See [RGB Underglow
Behavior](https://zmk.dev/docs/keymaps/behaviors/underglow) in ZMK documentation
for details. However, you need to use the `behavior-rgb-matrix` you defined
above (like `&kprgb`) instead of ZMK's `&rgb_ug`:

```dts
bindings = <
    &kprgb RGB_TOG &fx_solid &fx_reactive
    &kprgb RGB_BRI &kprgb RGB_BRD
>;
```

Effect state is persisted when `CONFIG_SETTINGS=y`; otherwise the configured
initial state is used at boot. The matrix turns off on idle by default;
`CONFIG_KEYPAW_RGB_MATRIX_AUTO_OFF_IDLE=n` keeps it lit and applies
`idle-brightness` while idle.

## Overlays and triggers

Overlays are regular devices that paint over the active effect. Add their
nodes as children of a `keypaw,rgb-overlays` container outside `&kprgb`, then
list them in paint order on the owning behavior node. Effect-level `overlays`
and `no-overlays` still override this list.

The container is the engine's overlay registry: its children are enumerated in
declaration order, and that index is the overlay's on/off bit. The state is a
`uint16_t[]` sized from the child count, so there is no cap on how many
overlays a board may declare; one word (16 overlays) travels per split
command. Both halves build the same devicetree, so the indices agree.

```dts
/ {
    rgb_overlays {
        compatible = "keypaw,rgb-overlays";

        caps: caps {
            compatible = "keypaw,rgb-overlay-caps-lock";
            keys = <0>;
            color = <0x00FF00>;
        };
    };
};

&kprgb {
    overlays = <&caps>;
};
```

### Where an overlay is evaluated

A kind may supply an `active` predicate. When it does, the renderer runs only
while the predicate is true; when it does not, the renderer runs every tick and
samples whatever it needs itself.

The predicate's source may not exist on both halves, so a kind declares which it
is with the `central-authoritative` property:

- **Locally determined** (the default; Caps Lock, and a future battery
  overlay). Every half evaluates the predicate for itself, so each half shows
  its own state. Caps Lock needs `CONFIG_ZMK_HID_INDICATORS=y`; a split
  peripheral also needs `CONFIG_ZMK_SPLIT_PERIPHERAL_HID_INDICATORS=y`, because
  the host's HID indicator state is what ZMK forwards.
- **Central-authoritative** (`central-authoritative;` on the node; the layer and
  dynamic-macro kinds). The source exists only on the split central, so the
  central evaluates the predicate and pushes only the words whose bits changed,
  one 16-bit word per command (word index in the high half of the command's
  parameter, the bits in the low half). A peripheral gates its renderers on that
  pushed state and never calls the predicate, which is why a layer overlay
  lights the peripheral's LEDs for the keys it lists that live on that half.

The pushed state is a level, not an edge: the central also re-pushes a full
snapshot when a peripheral connects (`CONFIG_KEYPAW_RGB_SPLIT_SYNC`), so a half
that was off while a layer was held still shows it. The snapshot uses the same
`&kprgb` command path as everything else but is deliberately not persisted --
unlike an effect selection, overlay state is volatile and must not schedule a
flash write on every layer change.

A predicate is otherwise sampled once per engine tick, so a change would wait up
to `CONFIG_KEYPAW_RGB_MATRIX_TICK_MS`, and on a peripheral a second tick for the
push to arrive. `zmk_rgb_matrix_flush()` removes that wait: it asks the engine to
repaint now by submitting its tick work to the low-priority queue, without
blocking and without resetting the animation clock, so the extra frame does not
restart an effect. Any kind may call it from a listener -- including one for a
custom event declared in a third-party module. The engine already calls it for
the central-authoritative path itself: a layer-state event on the central, and a
changed word arriving over the split link. Concurrent calls coalesce, because a
work item can be queued only once, so a burst costs at most one extra frame.

### Writing a kind

A kind provides a renderer, and optionally a predicate, through
`KP_RGB_OVERLAY_DEFINE(inst, active_fn, render_fn, cfg_inst)` (pass `NULL` for
`active_fn`). Its config and data structs embed
`struct kp_rgb_overlay_common_config` / `..._data` as the first member, as
before. Targeting (`keys` / `leds`) is resolved per half, so an overlay only
lights LEDs physically present on the half that renders it.

State that has an event source should be repainted from that event rather than
left to the tick: subscribe in the kind (as the caps-lock kind does for
`zmk_hid_indicators_changed`) and call `zmk_rgb_matrix_flush()` when the value
changes. A predicate-driven kind does not need to -- the engine already flushes
it on the events that drive it.

A trigger table selects commands when its first matching child changes. It runs
on the split central; behavior locality sends the selected state to peripherals.
Declare specific triggers before the unconditional fallback. A layer trigger is
the right tool for changing an effect while an Fn layer is active; a physically
separate Fn/accent LED region is instead represented by another behavior with
its own `leds` list.


```dts
/ {
    rgb_triggers {
        compatible = "keypaw,rgb-trigger-table";

        fn: fn {
            compatible = "keypaw,rgb-trigger-layer";
            layer = <1>;
            bindings = <&kprgb RGB_EFS_CMD 1>;
        };

        fallback: fallback {
            compatible = "keypaw,rgb-trigger-always";
            bindings = <&kprgb RGB_EFS_CMD 0>;
        };
    };
};
```

### Split state sync

A `&kprgb` command is `BEHAVIOR_LOCALITY_GLOBAL`, so ZMK forwards it to every
peripheral, which applies and persists it. A peripheral that was powered off while
the user changed settings therefore never saw the command, keeps its stale flash
defaults, and reloads them on the next power-up. With
`CONFIG_KEYPAW_RGB_SPLIT_SYNC=y` (the default) the central instead pushes the
selected effect, that effect's colour and period, and the user on/off intent to a
peripheral whenever it newly connects. Nothing is needed on the peripheral: its
command handler ends in `kp_rgb_save_state()`, so a push is applied and persisted
there. The option is built for the split central only.

The trigger is a poll of the transport's in-RAM connected-source list
(`CONFIG_KEYPAW_RGB_SPLIT_SYNC_POLL_MS`), because ZMK has no central-side
"peripheral connected" event and the transport's single status callback already
belongs to `central_init()`. The first command of a sync waits
`CONFIG_KEYPAW_RGB_SPLIT_SYNC_SETTLE_MS` after the connection is seen: a peripheral
is reported connected before its GATT characteristics have been discovered, and a
command sent in that window is dropped without any error reaching the sender.
Commands are emitted one per work item, since the split run queue is shallow and
overflows by discarding its oldest entry.

Only the *active* effect's colour and period are sent. Other effects keep whatever
the peripheral already had, so selecting an effect that was edited while the
peripheral was off still shows that effect's own colour until it is edited again.
Syncing `user_on` also necessarily sets the peripheral's live on/off state, since no
command changes one without the other — a sync performed while the central is idle
sends `RGB_ON` anyway, so an idle peripheral can light up until its next activity
transition clears it.

## Custom effects

Third-party effect modules include the public API as
`<zmk/rgb_matrix.h>`. Provide a binding that includes
`keypaw,rgb-matrix-effect-common.yaml`, embed
`struct kp_rgb_effect_common_config` and `struct kp_rgb_effect_common_data`
as the first member of the effect's config and data structures, and instantiate
each enabled devicetree node with `KP_RGB_EFFECT_DEFINE()`.

`modules/zmk-rgb-effect-example` in the parent
[`keypaw48-zmk`](https://github.com/gudzpoz/keypaw48-zmk/tree/main/modules/zmk-rgb-effect-example)
repository is a complete separate-module example. It demonstrates the required
binding, data layout, renderer, and `&kprgb` child node without using internal
matrix headers.

## License

MIT. See [LICENSE](./LICENSE).
