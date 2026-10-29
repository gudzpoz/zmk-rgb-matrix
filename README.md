# ZMK RGB Matrix

[![GitHub Actions Workflow
Status](https://img.shields.io/github/actions/workflow/status/gudzpoz/zmk-rgb-matrix/previews.yml?label=previews)](https://gudzpoz.github.io/zmk-rgb-matrix/)
[![GitHub Actions Workflow
Status](https://img.shields.io/github/actions/workflow/status/gudzpoz/zmk-rgb-matrix/tests.yml?label=tests)](https://github.com/gudzpoz/zmk-rgb-matrix/actions/workflows/tests.yml)

<table><tbody>
<tr>
<th><center>reactive-gradient</center></th>
<th><center>rainbow-pinwheel</center></th>
<th><center>starlight-smooth</center></th></tr>
<tr>
<td><img alt="reactive-gradient" src="https://gudzpoz.github.io/zmk-rgb-matrix/reactive-gradient.gif"></td>
<td><img alt="rainbow-pinwheel" src="https://gudzpoz.github.io/zmk-rgb-matrix/rainbow-pinwheel.gif"></td>
<td><img alt="starlight-smooth" src="https://gudzpoz.github.io/zmk-rgb-matrix/starlight-smooth.gif"></td>
</tr>
</tbody></table>

An extensible, position-aware / per-key LED RGB matrix framework for ZMK, with
split support and indicator overlays. This module is developed against ZMK
`main` and Zephyr 4.1. See [Built-in effects](#built-in-effects) for more
previews of everything it ships with.

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

## Usage

For the RGB matrix to work, you will need to provide (1) the positions of the
LEDs and (2) configuratinos for the actual lighting effects. Specifying LED
positions can be particularly tricky, but nevertheless, necessary, so bear with
me as we walk through it.

### LED position definitions

First, please follow the official ZMK guide to define for your keyboard:

- [a `zmk,physical-layout`, describing key positions with its `keys`
  property](https://zmk.dev/docs/hardware-integration/physical-layouts#optional-keys-property)

  The referenced `zmk,physical-layout` must provide `keys`, which supplies the
  required RGB geometry. Currently, we don't support runtime layout changes.

- and [your LED strip
  hardware](https://zmk.dev/docs/hardware-integration/lighting/underglow), but
  make sure to leave out the `zmk,underglow = &led_strip;` step: we'll be using
  our position-aware `keypaw,rgb-matrix` instead of the built-in
  `zmk,underglow`.

Now, as you probably know, your addressable LEDs are chained together, and the
order that they're chained are not always the same as that of your physical
keys. And that is why, in addition to specifying your physical layout and LED
strip, you now need to define that exact order for the RGB matrix with the
`mapping` attribute:

```dts
#include <dt-bindings/keypaw/rgb_matrix.h>

/ {
    rgb_matrix: rgb {
        compatible = "keypaw,rgb-matrix";
        #binding-cells = <2>;
        strip = <&led_strip>;
        physical-layout = <&default_layout>;
        mapping = <0 1 2 KP_RGB_NO_KEY(350, 50)>;
    };
};
```

> The `&led_strip` and `&default_layout` in the config should match what you've
> previously defined. For example, if you are following the official guide like this:
>
> ```dts
> / {
>     physical_layout0: physical_layout_0 {
>         compatible = "zmk,physical-layout";
>         display-name = "Default Layout";
>         keys = <...>;
>     };
> };
> ```
>
> Then you should be using `&physical_layout0` instead of `&default_layout`.
>
> In `rgb_matrix: rgb { ... };`, `rgb_matrix` is a [node
> label](https://docs.zephyrproject.org/latest/build/dts/intro-syntax-structure.html#dt-node-labels)
> used by references such as `&rgb_matrix`, while `rgb` is the node name. ZMK
> uses node names to sync behaviors between split halves, so keep the node name
> within eight characters if you rename `rgb`. This limit does not apply to the
> node label.

Each entry in this `mapping = <0 1 2 KP_RGB_NO_KEY(350, 50)>;` thing specifies
the position of an LED: an integer `N` means a per-key LED under the `N`-th key
(`0` means that LED is under the key represented by the first
`&key_physical_attrs` entry in your physical layout, while `5` means the sixth
entry). The mapping length must match the strip's `chain-length`.

> Use `KP_RGB_NO_KEY(x, y)` for an LED that is not directly underneath a key;
> coordinates use the layout's units (`100` is one key width, similar to
> `&key_physical_attrs`).

> A split central with no local LEDs (a USB dongle) should omit `strip` and then
> explicitly set `mapping = <>;`. It renders nothing, but still drives the
> peripherals' effects, overlays and split sync.

Confused? Let's break it down. First, I recommend using [ZMK physical layout
converter](https://zmk-physical-layout-converter.streamlit.app/) to visualize
your actual key ordering: open the page, paste in your ZMK layout in the "ZMK
DTS" panel and click "Update JSON using this", after which you should see a
visualization with your keys numbered:

![Example of a key order visualization](./docs/key-order.svg)

Now, bring out your PCB design and trace through your LED chain. Then (maybe
mentally) overlay the chain on your keyboard layout, and that is what you need
for your RGB matrix `mapping`. For example, the following PCB and keyboard
layout give <code>mapping = &lt;6 7 8 9 10 11 &nbsp;&nbsp; 23 22 21 20 19 18
&nbsp;&nbsp; 30 31 32 33 34 35 ...&gt;;</code>.

<table><tbody>
<tr><td><img alt="The order of the LED chain on a PCB" src="./docs/led-order.svg"></td>
<td>→</td>
<td><img alt="The order of the LED chain overlaid on the key layout" src="./docs/key-in-led-order.svg"></td></tr>
</tbody></table>

> The example above also has a few LEDs with no corresponding keys, for which you will need to calculate the coordinates yourself.
> Here is the full mapping for the example:
> ```
> / {
>     rgb_matrix: rgb {
>         compatible = "keypaw,rgb-matrix";
>         #binding-cells = <2>;
>         strip = <&led_strip>;
>         physical-layout = <&default_layout>;
>
>         mapping = <
>         6 7 8 9 10 11
>         23 22 21 20 19 18
>         30 31 32 33 34 35
>
>         KP_RGB_NO_KEY(1450, 387) // between 35 & 47
>         KP_RGB_NO_KEY(1350, 387) // between 34 & 46
>         KP_RGB_NO_KEY(1250, 400) // ~to the left of the previous LED
>         KP_RGB_NO_KEY(1180, 350) // ~to the top left of the previous LED
>         47 46
>         KP_RGB_NO_KEY(1000, 500) // ~to the right of 42
>         42
>         KP_RGB_NO_KEY(850, 475)  // ~to the left of 42
>         43 44 45
>         >;
>     };
> };
> ```
>
> A split keyboard normally declares one matrix node and one local mapping on
> each half. See my ZMK config for a rather complex example: [`rgb_matrix` for
> the left
> half](https://github.com/gudzpoz/keypaw48-zmk/blob/main/boards/shields/keypaw48/keypaw48_left.overlay),
> [`rgb_matrix` for the right
> half](https://github.com/gudzpoz/keypaw48-zmk/blob/main/boards/shields/keypaw48/keypaw48_right.overlay)
> and [the `physical_layout` they map
> to](https://github.com/gudzpoz/keypaw48-zmk/blob/main/boards/shields/keypaw48/keypaw48-layouts.dtsi).

> If your board contains multiple LED strips, you can use the
> `zmk,led-strip-composite` driver implemented by this module to concatenate
> them into one single strip, which can then be used in the RGB matrix
> definition.

### Effect configurations

With the LED geometry specified, you can now configure your RGB effects. All
effects and overlays should go under the same `keypaw,rgb-matrix` devicetree
node, with the effects you want as its children:

<!-- documented-setup-scene -->
```dts
&rgb_matrix {
    initial-on;
    initial-brightness = <30>;
    zone: zone {
        compatible = "keypaw,rgb-overlay";
        controllable;
        #binding-cells = <2>;
        all-leds;
        initial-effect = <0>;
        fx_breathe: fx_breathe {
            compatible = "keypaw,rgb-matrix-breathe";
            color = <0x0000FF>;
            duration = <2400>;
        };
        fx_rainbow: fx_rainbow {
            compatible = "keypaw,rgb-matrix-rainbow";
            duration = <4000>;
            basis = "x";
            direction = "out";
            palette = "rainbow";
        };
        // More effects go here...
    };
};
```

> For split builds, you need to make sure that boards get the same
> effect/overlay definitions in the same order: an effect is identified by its
> position in the registry, so a different set or order desyncs the halves. The
> same applies to overlays.

### Optional overlay configurations

Optionally, you may also specify some LED overlays to indicate CapsLock or the
current active layer, for example. The built-in overlay composites: it renders
one effect while a condition is active, and blends the result over the LEDs it
targets.

```dts
/ {
    rgb_conditions: rgb_conditions {
        compatible = "keypaw,rgb-conditions";
        // Host CapsLock state
        cond_caps: cond_caps {
            compatible = "keypaw,rgb-condition-caps-lock";
        };
        // Layer 1 active state
        cond_layer1: cond_layer1 {
            compatible = "keypaw,rgb-condition-layer";
            layer = <1>;
        };
        // When the user toggles &tg_batt with key bindings
        cond_batt: cond_batt {
            compatible = "keypaw,rgb-condition-latch";
            tg_batt: tg_batt {
                compatible = "keypaw,behavior-rgb-overlay-toggle";
                #binding-cells = <0>;
            };
        };
    };
};

&rgb_matrix {
    // Light up Key No. 41 when CapsLock is active
    caps: caps {
        compatible = "keypaw,rgb-overlay";
        condition = <&cond_caps>;
        keys = <41>;
        fx_caps {
            compatible = "keypaw,rgb-matrix-solid";
            color = <0x00FFFF>;
        };
    };
    // Light up keys when Layer 1 is active
    layer_fn1: layer_fn1 {
        compatible = "keypaw,rgb-overlay";
        condition = <&cond_layer1>;
        keys = <13 14 15 16 17 18 19 20 21 22>;
        fx_fn1 {
            compatible = "keypaw,rgb-matrix-solid";
            color = <0xFF8000>;
        };
    };
    // A built-in battery indicator overlay
    batt: batt {
        compatible = "keypaw,rgb-overlay-battery";
        condition = <&cond_batt>;
        all-leds;
        color = <0x00FF00>;
    };
};
```

These `keypaw,rgb-overlay` overlays should contain exactly one enabled effect
child (as above), whose contents will be rendered onto the selected LEDs.
`all-leds;` targets every LED; `keys` or `leds` select a subset. For subsets,
resolved targets must be unique: do not repeat keys/raw LEDs or address the same
LED through both lists. There is no automatic deduplication or uniqueness check.
`opacity = <...>;` adjusts how much output is mixed into earlier layers.

Overlays compose on top of each other in declaration order.

> We've also used `keypaw,rgb-overlay` in our `zone` definition above. The
> difference is that the `zone` node has a `controllable` property, making it an
> effect selector. And you may use `&zone` as a behavior to control this
> zone/selector. See the "Controlling the RGB matrix" section below.

Conditions control when an overlay is displayed. They should be declared under a
`keypaw,rgb-conditions` container node.

> The `cond_batt` latch condition is a bit different from other conditions
> above: it allows the user to toggle the condition via key bindings. Just bind
> `&tg_batt` anywhere in the keymap.

### Controlling the RGB matrix

Currently, we provide two kinds of behaviors to control the RGB matrix:e

- `&rgb_matrix`: the `keypaw,rgb-matrix` node, controlling power, global brightness
  and reset.
- `&zone`: where `zone` is a `keypaw,rgb-overlay` with an explicit
  `controllable` property, controlling its children's selection, colors and
  duration.

See [RGB Underglow behavior](https://zmk.dev/docs/keymaps/behaviors/underglow)
for a list of usable constants.

<!-- documented-setup-bindings -->
```dts
bindings = <
    // Toggles the RGB matrix on and off
    &rgb_matrix RGB_TOG
    // Increase the brightness
    &rgb_matrix RGB_BRI
    // Decrease the brightness
    &rgb_matrix RGB_BRD

    // Cycles to the next effect in this zone
    &zone RGB_EFF
    // Cycles to the previous effect in this zone
    &zone RGB_EFR
    // Switches to the N-th effect (0 means the first, and 1 the second, etc.)
    &zone RGB_EFS_CMD 0
    &zone RGB_EFS_CMD 1
    // Increases the hue of the current effect
    &zone RGB_HUI
    // Increases the speed of the current effect
    &zone RGB_SPI

    // Resets settings
    &rgb_matrix RGB_RESET
    &zone RGB_RESET
>;
```

Power/brightness and selection are persisted if `CONFIG_SETTINGS` is set.
Saves are debounced, similar to ZMK underglow.

## Built-in components

This ZMK module is designed to be extensible: you can implement effects and
conditions in your own custom module without having to modify code in this
module. Overlays and triggers are configured entirely in devicetree from those.
We also aim to provide a good out-of-the-box experience with a number of
built-in effects, overlays and conditions, which you can just use without having
to touch any C code.

### Built-in effects

These effects are grouped by rendering/state family: static fields (`solid`,
`static`), global oscillators (`breathe`, `spectrum`), moving spatial fields
(`rainbow`, `band`), reactive effects (`reactive`, `ripple`), and stochastic
effects (`rain`, `starlight`, `heatmap`, `digital_rain`).

Every animation below is rendered from ZMK's `native_sim//zmk_test_mock`
"board". The links in the table point to the [gallery
site](https://gudzpoz.github.io/zmk-rgb-matrix/), where each variant sits next
to a copy-and-pastable devicetree snippet that produced it.

<!-- No GIFs inside links: GitHub hides them for people with Accessibility.Motion = Disabled -->

<table>
<thead><tr><th>Effect</th><th>Description</th><th>Attributes</th></tr></thead>
<tbody>
<tr><td><code><a href="https://gudzpoz.github.io/zmk-rgb-matrix/#solid">solid</a></code></td>
    <td>One flat colour across every LED, or a static hue gradient swept across the board.</td>
    <td><code>axis</code> = <code>none</code> (flat colour) |
        <code>vertical</code> (hue sweep across y) |
        <code>horizontal</code> (hue sweep across x)</td></tr>
<tr><td colspan="3">
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#solid">default</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/solid.gif" width="200" alt="Solid" title="Solid"><br>
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#solid-vertical">vertical</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/solid-vertical.gif" width="200" alt="Solid (vertical)" title="Solid (vertical)"><br>
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#solid-horizontal">horizontal</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/solid-horizontal.gif" width="200" alt="Solid (horizontal)" title="Solid (horizontal)">
</td></tr>
<tr><td><code><a href="https://gudzpoz.github.io/zmk-rgb-matrix/#breathe">breathe</a></code></td>
    <td>Board-wide fade or a position-aware oscillator over one period.</td>
    <td><code>mode</code> = <code>brightness</code> (BREATHING) |
        <code>river</code> (RIVERFLOW) | <code>hue</code> (HUE_BREATHING) |
        <code>pendulum</code> (HUE_PENDULUM) | <code>wave</code> (HUE_WAVE)<br>
        <code>hue-amplitude</code> (hue swing in degrees for the hue modes, default 45)</td></tr>
<tr><td colspan="3">
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#breathe-brightness">brightness</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/breathe-brightness.gif" width="200" alt="Breathe (brightness)" title="Breathe (brightness)"><br>
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#breathe-river">river</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/breathe-river.gif" width="200" alt="Breathe (river)" title="Breathe (river)"><br>
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#breathe-hue">hue</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/breathe-hue.gif" width="200" alt="Breathe (hue)" title="Breathe (hue)"><br>
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#breathe-pendulum">pendulum</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/breathe-pendulum.gif" width="200" alt="Breathe (pendulum)" title="Breathe (pendulum)"><br>
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#breathe-wave">wave</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/breathe-wave.gif" width="200" alt="Breathe (wave)" title="Breathe (wave)">
</td></tr>
<tr><td><code><a href="https://gudzpoz.github.io/zmk-rgb-matrix/#spectrum">spectrum</a></code></td>
    <td>Whole board cycles the hue wheel globally (QMK <code>CYCLE_ALL</code>).</td>
    <td>&mdash;</td></tr>
<tr><td colspan="3">
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#spectrum">default</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/spectrum.gif" width="200" alt="Spectrum" title="Spectrum">
</td></tr>
<tr><td><code><a href="https://gudzpoz.github.io/zmk-rgb-matrix/#reactive">reactive</a></code></td>
    <td>Lights a pressed key and fades it back; the lit shape is configurable.</td>
    <td><code>spread</code> = <code>point</code> (key only) |
        <code>disc</code> (filled circle) | <code>cross</code> (row + column) |
        <code>nexus</code> (all but the cross, radial falloff)<br>
        <code>spread-radius</code> (disc/nexus radius, in layout units; for nexus it is the band width measured outward from the excluded cross)<br>
        <code>background-brightness</code> (unlit-LED brightness, percent)<br>
        <code>multi</code> (accumulate several presses instead of the latest)<br>
        <code>palette</code> = <code>solid</code> (user hue) |
        <code>gradient</code> (position-based hue) |
        <code>complement</code> (unlit floor keeps the hue, a pressed key flashes the opposite hue)</td></tr>
<tr><td colspan="3">
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#reactive-point">point</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/reactive-point.gif" width="200" alt="Reactive (point)" title="Reactive (point)"><br>
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#reactive-disc">disc</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/reactive-disc.gif" width="200" alt="Reactive (disc)" title="Reactive (disc)"><br>
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#reactive-cross">cross</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/reactive-cross.gif" width="200" alt="Reactive (cross)" title="Reactive (cross)"><br>
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#reactive-nexus">nexus</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/reactive-nexus.gif" width="200" alt="Reactive (nexus)" title="Reactive (nexus)"><br>
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#reactive-gradient">gradient</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/reactive-gradient.gif" width="200" alt="Reactive (gradient)" title="Reactive (gradient)"><br>
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#reactive-complement">complement</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/reactive-complement.gif" width="200" alt="Reactive (complement)" title="Reactive (complement)"><br>
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#reactive-multi">multi</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/reactive-multi.gif" width="200" alt="Reactive (multi)" title="Reactive (multi)">
</td></tr>
<tr><td><code><a href="https://gudzpoz.github.io/zmk-rgb-matrix/#ripple">ripple</a></code></td>
    <td>Radial ripples expand from each pressed key over an unlit background.</td>
    <td><code>background-brightness</code> (unlit background brightness, percent)</td></tr>
<tr><td colspan="3">
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#ripple">default</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/ripple.gif" width="200" alt="Ripple" title="Ripple">
</td></tr>
<tr><td><code><a href="https://gudzpoz.github.io/zmk-rgb-matrix/#rainbow">rainbow</a></code></td>
    <td>Position-aware rainbow gradient with selectable spatial basis and direction.</td>
    <td><code>basis</code> = <code>x</code> | <code>y</code> | <code>radial</code> |
        <code>pinwheel</code> | <code>spiral</code> | <code>chevron</code> |
        <code>flag</code>
        (spatial coordinate the colour maps onto; <code>flag</code> is a
        left-right sweep skewed per row so it undulates)<br>
        <code>direction</code> = <code>out</code> | <code>in</code> (reverse) |
        <code>dual</code> (two centres) | <code>bloom</code> (mirror each half, with basis x)<br>
        <code>palette</code> = <code>rainbow</code> (full hue wheel) |
        <code>solid</code> (single hue, travelling brightness band)</td></tr>
<tr><td colspan="3">
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#rainbow-x">x</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/rainbow-x.gif" width="200" alt="Rainbow (x)" title="Rainbow (x)"><br>
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#rainbow-y">y</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/rainbow-y.gif" width="200" alt="Rainbow (y)" title="Rainbow (y)"><br>
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#rainbow-radial">radial</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/rainbow-radial.gif" width="200" alt="Rainbow (radial)" title="Rainbow (radial)"><br>
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#rainbow-pinwheel">pinwheel</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/rainbow-pinwheel.gif" width="200" alt="Rainbow (pinwheel)" title="Rainbow (pinwheel)"><br>
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#rainbow-spiral">spiral</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/rainbow-spiral.gif" width="200" alt="Rainbow (spiral)" title="Rainbow (spiral)"><br>
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#rainbow-chevron">chevron</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/rainbow-chevron.gif" width="200" alt="Rainbow (chevron)" title="Rainbow (chevron)"><br>
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#rainbow-flag">flag</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/rainbow-flag.gif" width="200" alt="Rainbow (flag)" title="Rainbow (flag)"><br>
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#rainbow-in">in</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/rainbow-in.gif" width="200" alt="Rainbow (in)" title="Rainbow (in)"><br>
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#rainbow-dual">dual</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/rainbow-dual.gif" width="200" alt="Rainbow (dual)" title="Rainbow (dual)"><br>
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#rainbow-bloom">bloom</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/rainbow-bloom.gif" width="200" alt="Rainbow (bloom)" title="Rainbow (bloom)"><br>
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#rainbow-solid">solid</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/rainbow-solid.gif" width="200" alt="Rainbow (solid)" title="Rainbow (solid)">
</td></tr>
<tr><td><code><a href="https://gudzpoz.github.io/zmk-rgb-matrix/#band">band</a></code></td>
    <td>A single-hue board with a moving saturation or brightness band.</td>
    <td><code>channel</code> = <code>sat</code> (fades saturation) |
        <code>val</code> (fades brightness)<br>
        <code>shape</code> = <code>linear</code> (scroll) |
        <code>pinwheel</code> (rotate) | <code>spiral</code> (wind outward)</td></tr>
<tr><td colspan="3">
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#band-saturation-linear">saturation, linear</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/band-saturation-linear.gif" width="200" alt="Band (saturation, linear)" title="Band (saturation, linear)"><br>
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#band-saturation-pinwheel">saturation, pinwheel</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/band-saturation-pinwheel.gif" width="200" alt="Band (saturation, pinwheel)" title="Band (saturation, pinwheel)"><br>
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#band-saturation-spiral">saturation, spiral</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/band-saturation-spiral.gif" width="200" alt="Band (saturation, spiral)" title="Band (saturation, spiral)"><br>
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#band-value-linear">value, linear</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/band-value-linear.gif" width="200" alt="Band (value, linear)" title="Band (value, linear)">
</td></tr>
<tr><td><code><a href="https://gudzpoz.github.io/zmk-rgb-matrix/#static">static</a></code></td>
    <td>Fixed per-LED colour map (QMK <code>ALPHAS_MODS</code> and similar art); each LED
        takes a colour from the <code>led-colors</code> array.</td>
    <td><code>led-colors</code> (RGB values, one per LED by chain index; LEDs past the end stay off)</td></tr>
<tr><td colspan="3">
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#static">default</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/static.gif" width="200" alt="Static" title="Static">
</td></tr>
<tr><td><code><a href="https://gudzpoz.github.io/zmk-rgb-matrix/#rain">rain</a></code></td>
    <td>Randomly lit keys with random colours (QMK <code>PIXEL_RAIN</code>, <code>PIXEL_FLOW</code>,
        <code>RAINDROPS</code>, <code>JELLYBEAN_RAINDROPS</code>, <code>PIXEL_FRACTAL</code>).</td>
    <td><code>mode</code> = <code>pixel</code> (random keys, random hues) |
        <code>drops</code> (nudges one random key's hue each step; the board stays lit, so it needs a saturated colour) |
        <code>jellybean</code> (also randomises saturation) |
        <code>flow</code> (cursor along chain) |
        <code>fractal</code> (single hue pulses from centre)</td></tr>
<tr><td colspan="3">
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#rain-pixel">pixel</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/rain-pixel.gif" width="200" alt="Rain (pixel)" title="Rain (pixel)"><br>
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#rain-flow">flow</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/rain-flow.gif" width="200" alt="Rain (flow)" title="Rain (flow)"><br>
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#rain-drops">drops</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/rain-drops.gif" width="200" alt="Rain (drops)" title="Rain (drops)"><br>
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#rain-jellybean">jellybean</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/rain-jellybean.gif" width="200" alt="Rain (jellybean)" title="Rain (jellybean)"><br>
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#rain-fractal">fractal</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/rain-fractal.gif" width="200" alt="Rain (fractal)" title="Rain (fractal)">
</td></tr>
<tr><td><code><a href="https://gudzpoz.github.io/zmk-rgb-matrix/#starlight">starlight</a></code></td>
    <td>LEDs turn on and off at random at varying brightness, keeping the user colour.</td>
    <td><code>smooth</code> (ramp brightness instead of switching) |
        <code>dual-hue</code> (jitter hue &plusmn;30) |
        <code>dual-sat</code> (jitter saturation &plusmn;30)</td></tr>
<tr><td colspan="3">
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#starlight">default</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/starlight.gif" width="200" alt="Starlight" title="Starlight"><br>
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#starlight-smooth">smooth</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/starlight-smooth.gif" width="200" alt="Starlight (smooth)" title="Starlight (smooth)"><br>
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#starlight-dual-hue">dual hue</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/starlight-dual-hue.gif" width="200" alt="Starlight (dual hue)" title="Starlight (dual hue)"><br>
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#starlight-dual-sat">dual sat</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/starlight-dual-sat.gif" width="200" alt="Starlight (dual sat)" title="Starlight (dual sat)">
</td></tr>
<tr><td><code><a href="https://gudzpoz.github.io/zmk-rgb-matrix/#heatmap">heatmap</a></code></td>
    <td>Per-key heat map of recent keypresses, decaying over time (QMK <code>TYPING_HEATMAP</code>).</td>
    <td><code>decrease-delay-ms</code> (ms idle before one shade of heat is lost)<br>
        <code>spread</code> (radius heating neighbours, layout units; 0 heats only the pressed key)<br>
        <code>area-limit</code> (per-press cap on heat a neighbour receives)<br>
        <code>increase-step</code> (shades added per keypress)</td></tr>
<tr><td colspan="3">
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#heatmap">default</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/heatmap.gif" width="200" alt="Heatmap" title="Heatmap"><br>
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#heatmap-slim"><code>spread = &lt;0&gt;</code></a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/heatmap-slim.gif" width="200" alt="Heatmap (no spread)" title="Heatmap (no spread)">
</td></tr>
<tr><td><code><a href="https://gudzpoz.github.io/zmk-rgb-matrix/#digital-rain">digital_rain</a></code></td>
    <td>Matrix-style falling columns: a head falls per column leaving a fading green tail.</td>
    <td><code>color</code> (tail hue; green default) |
        <code>duration</code> (fall speed; shorter is faster)</td></tr>
<tr><td colspan="3">
<a href="https://gudzpoz.github.io/zmk-rgb-matrix/#digital-rain">default</a>: <img src="https://gudzpoz.github.io/zmk-rgb-matrix/digital-rain.gif" width="200" alt="Digital Rain" title="Digital Rain">
</td></tr>
</tbody>
</table>

Every effect accepts `color` and `duration` properties.

### Built-in conditions and overlays

| Component | Compatible | Notes |
|---|---|---|
| Caps Lock condition | `keypaw,rgb-condition-caps-lock` | Active while the host reports Caps Lock |
| Layer condition | `keypaw,rgb-condition-layer` | Active while a keymap layer is active |
| Selected-effect condition | `keypaw,rgb-condition-effect-selected` | Matches retained selection; `selector` and enabled child `effect` phandles; local source on either half |
| BLE condition | `keypaw,rgb-condition-ble` | Central-only; matches the active BLE profile index and connection state |
| Latch condition | `keypaw,rgb-condition-latch` | Stateful; toggled by the behavior nested under it |
| And condition | `keypaw,rgb-condition-and` | All listed conditions active |
| Or condition | `keypaw,rgb-condition-or` | Any listed condition active |
| Overlay toggle | `keypaw,behavior-rgb-overlay-toggle` | Child of `keypaw,rgb-condition-latch`. Node name ≤ 8 chars |
| Battery gauge | `keypaw,rgb-overlay-battery` | Level bar from this half's charge |

The generic `keypaw,rgb-overlay` compositor is the other overlay kind: it
renders one effect while its condition holds. Omit `condition` on an overlay or
trigger to make it unconditional.

## Custom effects and conditions

Third-party modules can add effects, conditions and overlays without editing
this module; overlays and triggers are then configured in devicetree from them.
The full authoring guide is [`docs/extending.md`](./docs/extending.md).

As a quick taste, a third-party effect module may:
- include the public API as `<zmk/rgb_matrix.h>`,
- provide a new effect by:
  1. adding a binding that includes `keypaw,rgb-matrix-effect-common.yaml`,
  2. embedding `struct kp_rgb_effect_common_config` and `struct
     kp_rgb_effect_common_data` as the first member of the effect's config and
     data structures,
  3. defining `const struct kp_rgb_effect_callbacks *`,
  4. and instantiating each enabled devicetree node with
     `KP_RGB_EFFECT_DEFINE()`.

`modules/zmk-rgb-effect-example` in the parent
[`keypaw48-zmk`](https://github.com/gudzpoz/keypaw48-zmk/tree/main/modules/zmk-rgb-effect-example)
repository is a complete separate-module example. It demonstrates the required
binding, data layout and renderer.

## Testing

Run `bash tests/unit_run.sh` for SDK-free helper checks and `bash
tests/sim/run-native.sh` for the complete native contract and configuration
matrix. Set `ZMK_WS` to an existing ZMK west workspace when the module is
checked out separately. Stateful tests compile real production sources, headers
and devicetree rather than extracting source fragments.

See [native test usage](tests/sim/README.md) and the [fixture coverage
ledger](tests/fixture-coverage.md) for focused commands, artifact handling and
case mappings. [Architecture decisions](docs/architecture-decisions.md) record
the retained design. (Please note that these docs are not actively
human-maintained.)

## License

MIT. See [LICENSE](./LICENSE).
