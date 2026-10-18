/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */
#include "native_contract.h"
#include <limits.h>
#include <sys/types.h>
#include <zephyr/irq_offload.h>
#include <zephyr/init.h>
#include <zmk/rgb_matrix.h>
#include "../../../../src/rgb_matrix_internal.h"

static int effects_prepare(void) {
    KP_CHECK(zmk_rgb_matrix_set_inhibited(true) == 0);
    return 0;
}
BUILD_ASSERT(CONFIG_APPLICATION_INIT_PRIORITY > 89);
SYS_INIT(effects_prepare, APPLICATION, 89);

#define CAP MAX(KP_LED_COUNT, 1)
#define DEV(label) DEVICE_DT_GET(DT_NODELABEL(label))

/* Script only the external entropy boundary; providers and color conversion are real. */
static const uint32_t entropy[] = {0, 1, 2, 3, 35, 99, 255, 30, 60, 360, 1007, UINT32_MAX};
static size_t entropy_calls;
void __wrap_z_impl_sys_rand_get(void *dst, size_t length) {
    uint8_t *out = dst;
    while (length) {
        uint32_t word = entropy[entropy_calls++ % ARRAY_SIZE(entropy)];
        size_t n = MIN(length, sizeof(word));
        memcpy(out, &word, n);
        out += n;
        length -= n;
    }
}
static uint8_t battery_soc;
uint8_t __wrap_zmk_battery_state_of_charge(void) { return battery_soc; }

struct effects_observation {
    struct kp_rgb_effect_common_data common;
    unsigned resets, activates, deactivates, events, renders;
    unsigned order, reset_order, active_order;
    int64_t reset_time, active_time;
    struct kp_rgb_frame frame;
    struct kp_rgb_key_event event;
    bool animate, event_result, check_black;
};
static bool effects_probe_render(const struct device *dev, const struct kp_rgb_frame *frame) {
    struct effects_observation *o = dev->data;
    o->renders++;
    o->frame = *frame;
    KP_CHECK(frame->scratch == NULL);
    for (size_t t = 0; t < frame->target_count; t++) {
        size_t i = frame->targets[t];
        if (o->check_black) KP_CHECK(frame->pixels[i].r == 0 && frame->pixels[i].g == 0 && frame->pixels[i].b == 0);
        frame->pixels[i] = (struct led_rgb){104, 73, 101};
    }
    return o->animate;
}
static void effects_probe_reset(const struct device *dev, int64_t now) {
    struct effects_observation *o = dev->data;
    o->resets++;
    o->reset_time = now;
    o->reset_order = ++o->order;
}
static void effects_probe_active(const struct device *dev, bool active, int64_t now) {
    struct effects_observation *o = dev->data;
    if (active) o->activates++; else o->deactivates++;
    o->active_time = now;
    o->active_order = ++o->order;
}
static bool effects_probe_event(const struct device *dev, const struct kp_rgb_key_event *event) {
    struct effects_observation *o = dev->data;
    o->events++;
    o->event = *event;
    return o->event_result;
}
static const struct kp_rgb_effect_callbacks effects_probe_callbacks = {
    .render = effects_probe_render, .reset = effects_probe_reset,
    .set_active = effects_probe_active, .on_event = effects_probe_event,
};
static const struct kp_rgb_effect_callbacks effects_optional_callbacks = {.render = effects_probe_render};
#define DT_DRV_COMPAT test_rgb_effects_probe
#define EFFECTS_PROBE_DEFINE(inst) \
    static const struct { struct kp_rgb_effect_common_config common; } effects_probe_##inst##_cfg = { \
        KP_RGB_EFFECT_COMMON_CONFIG(inst) }; \
    static struct effects_observation effects_probe_##inst##_data = { KP_RGB_EFFECT_COMMON_DATA(inst, 0xffffff, 0) }; \
    KP_RGB_EFFECT_DEFINE(DT_DRV_INST(inst), \
        COND_CODE_1(DT_PROP(DT_DRV_INST(inst), optional_callbacks), \
                    (&effects_optional_callbacks), (&effects_probe_callbacks)), effects_probe_##inst);
DT_INST_FOREACH_STATUS_OKAY(EFFECTS_PROBE_DEFINE)
#undef DT_DRV_COMPAT

#define DT_DRV_COMPAT test_rgb_effects_enum_first
DEFINE_DT_ENUM(effects_first_mode_t, mode, idle, pulse, sweep);
BUILD_ASSERT(test_rgb_effects_enum_first_mode_idle == 0);
BUILD_ASSERT(test_rgb_effects_enum_first_mode_pulse == 1);
BUILD_ASSERT(test_rgb_effects_enum_first_mode_sweep == 2);
BUILD_ASSERT(DT_ENUM_CONST(mode, pulse) == test_rgb_effects_enum_first_mode_pulse);
#define EFFECTS_ENUM_VALUE(inst) CONV_DT_ENUM(inst, mode),
static const effects_first_mode_t first_modes[] = { DT_INST_FOREACH_STATUS_OKAY(EFFECTS_ENUM_VALUE) };
#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT test_rgb_effects_enum_second
DEFINE_DT_ENUM(effects_second_mode, mode, sweep, idle, pulse);
BUILD_ASSERT(test_rgb_effects_enum_second_mode_sweep == 0);
BUILD_ASSERT(test_rgb_effects_enum_second_mode_idle == 1);
BUILD_ASSERT(test_rgb_effects_enum_second_mode_pulse == 2);
BUILD_ASSERT(DT_ENUM_CONST(mode, pulse) == test_rgb_effects_enum_second_mode_pulse);
BUILD_ASSERT(!__builtin_types_compatible_p(effects_first_mode_t, effects_second_mode));
static const effects_second_mode second_modes[] = { DT_INST_FOREACH_STATUS_OKAY(EFFECTS_ENUM_VALUE) };
#undef DT_DRV_COMPAT

static struct led_rgb pixels[CAP], workspace[CAP], reference[CAP];
static struct kp_rgb_coord coords[CAP];
static size_t all[CAP], reverse_targets[CAP];
static struct kp_rgb_frame frame;
static const struct kp_rgb_effect_callbacks *callbacks(const struct device *dev) {
    return dev->api;
}
static const struct kp_rgb_effect_callbacks *overlay_callbacks(const struct device *dev) {
    return ((const struct kp_rgb_overlay_api *)dev->api)->callbacks;
}
static void geometry(void) {
    uint16_t min_x = UINT16_MAX, max_x = 0, min_y = UINT16_MAX, max_y = 0;
    for (size_t i = 0; i < KP_LED_COUNT; i++) {
        const struct kp_rgb_coord *c = kp_rgb_led_coord(i);
        KP_CHECK(c != NULL);
        coords[i] = *c;
        all[i] = i;
        reverse_targets[i] = KP_LED_COUNT - i - 1;
        min_x = MIN(min_x, c->x); max_x = MAX(max_x, c->x);
        min_y = MIN(min_y, c->y); max_y = MAX(max_y, c->y);
    }
    frame = (struct kp_rgb_frame){ .count = KP_LED_COUNT, .coords = coords,
        .pixels = pixels, .targets = all, .target_count = KP_LED_COUNT,
        .board_length = KP_LED_COUNT ? MAX(MAX(max_x - min_x, max_y - min_y), 1) : 1,
        .board_height = KP_LED_COUNT ? MAX(max_y - min_y, 1) : 1 };
}
static void clear_targets(void) {
    memset(pixels, 0xa5, sizeof(pixels));
    for (size_t t = 0; t < frame.target_count; t++) pixels[frame.targets[t]] = (struct led_rgb){0};
}
static bool rgb_equal(struct led_rgb a, struct led_rgb b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}
static void sentinels(void) {
    for (size_t i = 0; i < CAP; i++) {
        bool selected = false;
        for (size_t t = 0; t < frame.target_count; t++) selected |= frame.targets[t] == i;
        if (!selected) KP_CHECK(rgb_equal(pixels[i], (struct led_rgb){0xa5, 0xa5, 0xa5}));
    }
}
static void reset_device(const struct device *dev, int64_t now) {
    const struct kp_rgb_effect_callbacks *cb = callbacks(dev);
    if (cb->reset) cb->reset(dev, now);
    if (cb->set_active) cb->set_active(dev, true, now);
}
static void common_equal(const struct device *dev, struct kp_rgb_effect_common_data saved) {
    struct kp_rgb_effect_common_data *current = kp_rgb_effect_data(dev);
    KP_CHECK(current->duration_ms == saved.duration_ms);
    KP_CHECK(current->color.h == saved.color.h && current->color.s == saved.color.s && current->color.b == saved.color.b);
}

enum effects_kind { SOLID, STATIC, BREATHE, SPECTRUM, RAINBOW, BAND, REACTIVE, RIPPLE, RAIN, STARLIGHT, HEATMAP, DIGITAL, EXAMPLE };
struct effects_device { const struct device *dev; enum effects_kind kind; uint32_t interval; unsigned mode; };
#define ENTRY(node, kind_, interval_, mode_) {DEVICE_DT_GET(node), kind_, interval_, mode_},
#define SOLID_ENTRY(node) ENTRY(node, SOLID, 0, 0)
#define STATIC_ENTRY(node) ENTRY(node, STATIC, 0, 0)
#define BREATHE_ENTRY(node) ENTRY(node, BREATHE, 0, 0)
#define SPECTRUM_ENTRY(node) ENTRY(node, SPECTRUM, 0, 0)
#define RAINBOW_ENTRY(node) ENTRY(node, RAINBOW, 0, 0)
#define BAND_ENTRY(node) ENTRY(node, BAND, 0, 0)
#define REACTIVE_ENTRY(node) ENTRY(node, REACTIVE, 0, 0)
#define RIPPLE_ENTRY(node) ENTRY(node, RIPPLE, 0, 0)
#define RAIN_ENTRY(node) ENTRY(node, RAIN, DT_PROP(node, step_interval_ms), DT_ENUM_IDX(node, mode))
#define STAR_ENTRY(node) ENTRY(node, STARLIGHT, DT_PROP(node, step_interval_ms), 0)
#define HEAT_ENTRY(node) ENTRY(node, HEATMAP, 0, 0)
#define DIGITAL_ENTRY(node) ENTRY(node, DIGITAL, 0, 0)
#define EXAMPLE_ENTRY(node) ENTRY(node, EXAMPLE, 0, 0)
static const struct effects_device devices[] = {
    DT_FOREACH_STATUS_OKAY(keypaw_rgb_matrix_solid, SOLID_ENTRY)
    DT_FOREACH_STATUS_OKAY(keypaw_rgb_matrix_static, STATIC_ENTRY)
    DT_FOREACH_STATUS_OKAY(keypaw_rgb_matrix_breathe, BREATHE_ENTRY)
    DT_FOREACH_STATUS_OKAY(keypaw_rgb_matrix_spectrum, SPECTRUM_ENTRY)
    DT_FOREACH_STATUS_OKAY(keypaw_rgb_matrix_rainbow, RAINBOW_ENTRY)
    DT_FOREACH_STATUS_OKAY(keypaw_rgb_matrix_band, BAND_ENTRY)
    DT_FOREACH_STATUS_OKAY(keypaw_rgb_matrix_reactive, REACTIVE_ENTRY)
    DT_FOREACH_STATUS_OKAY(keypaw_rgb_matrix_ripple, RIPPLE_ENTRY)
    DT_FOREACH_STATUS_OKAY(keypaw_rgb_matrix_rain, RAIN_ENTRY)
    DT_FOREACH_STATUS_OKAY(keypaw_rgb_matrix_starlight, STAR_ENTRY)
    DT_FOREACH_STATUS_OKAY(keypaw_rgb_matrix_heatmap, HEAT_ENTRY)
    DT_FOREACH_STATUS_OKAY(keypaw_rgb_matrix_digital_rain, DIGITAL_ENTRY)
    DT_FOREACH_STATUS_OKAY(keypaw_rgb_matrix_example, EXAMPLE_ENTRY)
};
static bool initial_demand(enum effects_kind kind) {
    if (kind == SOLID || kind == STATIC) return false;
    if (kind == REACTIVE || kind == HEATMAP || kind == RIPPLE) return KP_LED_COUNT > 0;
    return true;
}
static void inject(const struct device *dev, uint32_t position, int64_t now) {
    if (callbacks(dev)->on_event) {
        struct kp_rgb_key_event event = {.position = position, .pressed = true, .timestamp_ms = now};
        KP_CHECK(callbacks(dev)->on_event(dev, &event) == (KP_LED_COUNT > 0));
    }
}
static void variants(void) {
    size_t kinds[13] = {0};
    for (size_t n = 0; n < ARRAY_SIZE(devices); n++) {
        const struct device *dev = devices[n].dev;
        printk("VARIANT %s\n", dev->name);
        KP_CHECK(device_is_ready(dev));
        kinds[devices[n].kind]++;
        struct kp_rgb_effect_common_data saved = *kp_rgb_effect_data(dev);
        entropy_calls = 0;
        reset_device(dev, 100);
        common_equal(dev, saved);
        if (callbacks(dev)->on_event) {
            struct kp_rgb_key_event event = {.position = 0, .pressed = false, .timestamp_ms = 100};
            KP_CHECK(!callbacks(dev)->on_event(dev, &event));
            event.pressed = true; event.position = UINT32_MAX;
            KP_CHECK(!callbacks(dev)->on_event(dev, &event));
        }
        inject(dev, 0, 100);
        frame.local_ms = 100; frame.clock_ms = 123; frame.elapsed_ms = 0;
        clear_targets();
        bool demand = kp_rgb_effect_render(dev, &frame);
        KP_CHECK(demand == initial_demand(devices[n].kind));
        memcpy(reference, pixels, sizeof(pixels));
        size_t calls = entropy_calls;
        KP_CHECK(kp_rgb_effect_render(dev, &frame) == demand);
        KP_CHECK(memcmp(reference, pixels, sizeof(pixels)) == 0 && entropy_calls == calls);
        frame.local_ms = 132; frame.elapsed_ms = 32;
        kp_rgb_effect_render(dev, &frame);
        frame.elapsed_ms = 0;
        memcpy(reference, pixels, sizeof(pixels)); calls = entropy_calls;
        kp_rgb_effect_render(dev, &frame);
        KP_CHECK(memcmp(reference, pixels, sizeof(pixels)) == 0 && entropy_calls == calls);
        frame.local_ms = INT64_MAX; frame.elapsed_ms = UINT32_MAX;
        kp_rgb_effect_render(dev, &frame);
        frame.elapsed_ms = 0;
        memcpy(reference, pixels, sizeof(pixels)); calls = entropy_calls;
        kp_rgb_effect_render(dev, &frame);
        KP_CHECK(memcmp(reference, pixels, sizeof(pixels)) == 0 && entropy_calls == calls);
        if (callbacks(dev)->set_active) callbacks(dev)->set_active(dev, false, frame.local_ms);
        reset_device(dev, frame.local_ms);
        common_equal(dev, saved);
        sentinels();
    }
    KP_CHECK(kinds[RAINBOW] == 67 && kinds[STARLIGHT] == 28 && kinds[BAND] == 10);
    KP_CHECK(kinds[BREATHE] == 10 && kinds[RAIN] == 20 && kinds[SOLID] == 7);
    KP_CHECK(kinds[STATIC] == 1 && kinds[SPECTRUM] == 1 && kinds[REACTIVE] == 11);
    KP_CHECK(kinds[RIPPLE] == 1 && kinds[HEATMAP] == 3 && kinds[DIGITAL] == 2);
    KP_CHECK(kinds[EXAMPLE] == DT_NODE_EXISTS(DT_NODELABEL(effects_example)));
}
static uint8_t peak(struct led_rgb rgb) { return MAX(rgb.r, MAX(rgb.g, rgb.b)); }

/* HSV conversion floors V first, then floors V * channel_factor / 255.
 * At B50, floor(50*f/d) == floor(floor(100*f/d)/2), including generated H/S.
 * Recover the exact integer V and enumerate only channel factors compatible
 * with the full-brightness sample; RGB/2 is not a valid oracle here.
 */
static void half_channel(uint8_t full, uint8_t half, unsigned v, unsigned half_v) {
    bool matched = false;
    for (unsigned factor = 0; factor <= 255; factor++) {
        if (v * factor / 255 == full && half_v * factor / 255 == half) matched = true;
    }
    KP_CHECK(matched);
}
static void half_pixel(struct led_rgb full, struct led_rgb half) {
    unsigned b = 0;
    while (b < 100 && b * 255 / 100 < peak(full)) b++;
    unsigned v = b * 255 / 100, half_v = (b / 2) * 255 / 100;
    KP_CHECK(v == peak(full) && peak(half) == half_v);
    half_channel(full.r, half.r, v, half_v);
    half_channel(full.g, half.g, v, half_v);
    half_channel(full.b, half.b, v, half_v);
}
static struct led_rgb ripple_expected(const struct device *dev, size_t led) {
    struct kp_rgb_hsb base = kp_rgb_effect_data(dev)->color;
    struct kp_rgb_hsb bg = base;
    bg.b = base.b * DT_PROP(DT_NODELABEL(fx_ripple), background_brightness) / 100;
    struct led_rgb result = kp_rgb_hsb_to_rgb(bg);
    uint32_t period = kp_rgb_effect_period(dev), age = frame.local_ms;
    if (age >= period) return result;
    unsigned radius = (uint64_t)frame.board_length * age / period;
    if (!radius) return result;
    unsigned width = MAX(radius * 60 / 100, 1), window = MAX(period / 4, 1);
    unsigned fade = age + window < period ? 255 : 255 * (period - age) / window;
    int32_t dx = (int32_t)coords[led].x - coords[0].x;
    int32_t dy = (int32_t)coords[led].y - coords[0].y;
    unsigned distance = 0, square = dx * dx + dy * dy;
    while ((uint64_t)(distance + 1) * (distance + 1) <= square) distance++;
    unsigned delta = distance > radius ? distance - radius : radius - distance;
    if (delta > width) return result;
    unsigned amplitude = (255 - delta * 255 / width) * fade / 255;
    base.h = (base.h + radius * 360 / frame.board_length) % 360;
    base.b = base.b * amplitude / 255;
    struct led_rgb ring = kp_rgb_hsb_to_rgb(base);
    /* Two coincident triggers must add independently, then saturate. */
    result.r = MIN(255, result.r + 2 * ring.r);
    result.g = MIN(255, result.g + 2 * ring.g);
    result.b = MIN(255, result.b + 2 * ring.b);
    return result;
}
static void brightness(void) {
    enum { STEPS = 24 };
    static struct led_rgb full[STEPS][CAP];
    bool demands[STEPS];
    size_t calls[STEPS];
    const uint8_t levels[] = {100, 50, 0};
    for (size_t n = 0; n < ARRAY_SIZE(devices); n++) {
        const struct effects_device *entry = &devices[n];
        const struct device *dev = entry->dev;
        struct kp_rgb_effect_common_data original = *kp_rgb_effect_data(dev);
        bool lit = false;
        printk("BRIGHTNESS %s\n", dev->name);
        for (size_t level = 0; level < ARRAY_SIZE(levels); level++) {
            struct kp_rgb_hsb color = {.h = 23, .s = 73, .b = levels[level]};
            kp_rgb_matrix_unlock();
            KP_CHECK(zmk_rgb_matrix_set_color(dev, &color) == 0);
            kp_rgb_matrix_lock();
            struct kp_rgb_effect_common_data saved = *kp_rgb_effect_data(dev);
            KP_CHECK(saved.color.h == 23 && saved.color.s == 73 && saved.color.b == levels[level]);
            entropy_calls = 0; reset_device(dev, 0); common_equal(dev, saved);
            inject(dev, 0, 0);
            if (entry->kind == RIPPLE) inject(dev, 0, 0);
            uint32_t now = 0;
            for (unsigned step = 0; step < STEPS; step++) {
                uint32_t delta = !step ? 0 : step % 2 && entry->interval ? entry->interval : 173;
                now += delta;
                frame.local_ms = now; frame.clock_ms = now; frame.elapsed_ms = delta;
                frame.targets = step % 2 ? reverse_targets : all;
                frame.target_count = KP_LED_COUNT;
                clear_targets();
                bool demand = kp_rgb_effect_render(dev, &frame);
                if (!level) {
                    memcpy(full[step], pixels, sizeof(pixels));
                    demands[step] = demand; calls[step] = entropy_calls;
                } else KP_CHECK(demand == demands[step] && entropy_calls == calls[step]);
                for (size_t i = 0; i < KP_LED_COUNT; i++) {
                    if (entry->kind == STATIC) {
                        /* Static's per-LED DTS colors intentionally ignore common HSB, even B0. */
                        KP_CHECK(rgb_equal(pixels[i], full[step][i]));
                    } else if (entry->kind == RIPPLE) {
                        KP_CHECK(rgb_equal(pixels[i], ripple_expected(dev, i)));
                    } else if (levels[level] == 50) half_pixel(full[step][i], pixels[i]);
                    else if (!levels[level]) KP_CHECK(rgb_equal(pixels[i], (struct led_rgb){0}));
                    if (level == 1) lit |= peak(pixels[i]) > 0;
                }
                sentinels(); common_equal(dev, saved);
                memcpy(reference, pixels, sizeof(pixels));
                size_t sparse[] = {3, 1}, before = entropy_calls;
                frame.elapsed_ms = 0; frame.targets = sparse;
                frame.target_count = KP_LED_COUNT ? ARRAY_SIZE(sparse) : 0;
                clear_targets();
                KP_CHECK(kp_rgb_effect_render(dev, &frame) == demand && entropy_calls == before);
                for (size_t t = 0; t < frame.target_count; t++)
                    KP_CHECK(rgb_equal(pixels[sparse[t]], reference[sparse[t]]));
                sentinels();
                frame.target_count = 0; clear_targets();
                KP_CHECK(kp_rgb_effect_render(dev, &frame) == demand && entropy_calls == before);
                sentinels(); common_equal(dev, saved);
            }
            if (callbacks(dev)->set_active) callbacks(dev)->set_active(dev, false, now);
            reset_device(dev, now); common_equal(dev, saved);
        }
        if (KP_LED_COUNT) KP_CHECK(lit);
        kp_rgb_matrix_unlock();
        KP_CHECK(zmk_rgb_matrix_set_color(dev, &original.color) == 0);
        kp_rgb_matrix_lock(); common_equal(dev, original);
    }
}
static void targets(void) {
    size_t sparse[] = {3, 1};
    for (size_t n = 0; n < ARRAY_SIZE(devices); n++) {
        const struct device *dev = devices[n].dev;
        bool demand = false, accepted[12] = {false};
        size_t reference_calls = 0;
        for (unsigned selection = 0; selection < 4; selection++) {
            reset_device(dev, 0); entropy_calls = 0;
            frame.targets = selection == 0 ? all : selection == 1 ? reverse_targets : sparse;
            frame.target_count = selection < 2 ? KP_LED_COUNT : selection == 2 && KP_LED_COUNT ? 2 : 0;
            for (unsigned step = 0; step < 12; step++) {
                if (callbacks(dev)->on_event) {
                    struct kp_rgb_key_event event = {.position = KP_LED_COUNT ? step % KP_LED_COUNT : 0,
                        .pressed = true, .timestamp_ms = step * 32};
                    bool result = callbacks(dev)->on_event(dev, &event);
                    if (!selection) accepted[step] = result;
                    else KP_CHECK(result == accepted[step]);
                }
                frame.local_ms = step * 32; frame.clock_ms = step * 32; frame.elapsed_ms = step ? 32 : 0;
                clear_targets();
                bool again = kp_rgb_effect_render(dev, &frame);
                sentinels();
                if (!selection && step == 11) demand = again;
                if (selection && step == 11) KP_CHECK(again == demand);
            }
            if (!selection) { memcpy(reference, pixels, sizeof(pixels)); reference_calls = entropy_calls; }
            else {
                KP_CHECK(entropy_calls == reference_calls);
                for (size_t t = 0; t < frame.target_count; t++) KP_CHECK(rgb_equal(pixels[frame.targets[t]], reference[frame.targets[t]]));
                frame.targets = all; frame.target_count = KP_LED_COUNT; frame.elapsed_ms = 0;
                clear_targets(); KP_CHECK(kp_rgb_effect_render(dev, &frame) == demand);
                KP_CHECK(entropy_calls == reference_calls && memcmp(reference, pixels, sizeof(pixels)) == 0);
            }
        }
    }
}
static void phase(void) {
    for (size_t n = 0; n < ARRAY_SIZE(devices); n++) {
        enum effects_kind kind = devices[n].kind;
        if (kind != BAND && kind != BREATHE && kind != RAINBOW && kind != SPECTRUM && kind != EXAMPLE) continue;
        const struct device *dev = devices[n].dev;
        frame.clock_ms = 123; frame.elapsed_ms = 0;
        clear_targets(); KP_CHECK(kp_rgb_effect_render(dev, &frame));
        memcpy(reference, pixels, sizeof(pixels));
        frame.elapsed_ms = UINT32_MAX; frame.local_ms = INT64_MAX;
        KP_CHECK(kp_rgb_effect_render(dev, &frame));
        KP_CHECK(memcmp(reference, pixels, sizeof(pixels)) == 0);
        frame.elapsed_ms = 0; frame.clock_ms += kp_rgb_effect_period(dev) / 4;
        KP_CHECK(kp_rgb_effect_render(dev, &frame));
        if (KP_LED_COUNT) KP_CHECK(memcmp(reference, pixels, sizeof(pixels)) != 0);
    }
}
static void random_timing(void) {
    for (size_t n = 0; n < ARRAY_SIZE(devices); n++) {
        const struct effects_device *entry = &devices[n];
        if (entry->kind != RAIN && entry->kind != STARLIGHT && entry->kind != DIGITAL) continue;
        const struct device *dev = entry->dev;
        uint32_t total = entry->kind == DIGITAL ? 100 : entry->kind == RAIN && entry->mode == 2 ? 125 :
            entry->kind == RAIN && entry->mode == 4 ? 1007 : 8 * entry->interval + entry->interval / 2;
        reset_device(dev, 0); entropy_calls = 0;
        frame.elapsed_ms = 0; frame.local_ms = 0; clear_targets(); kp_rgb_effect_render(dev, &frame);
        frame.elapsed_ms = total; frame.local_ms = total; kp_rgb_effect_render(dev, &frame);
        memcpy(reference, pixels, sizeof(pixels)); size_t calls = entropy_calls;
        if (KP_LED_COUNT && !(entry->kind == RAIN && entry->mode == 4)) KP_CHECK(calls > 0);
        reset_device(dev, 0); entropy_calls = 0;
        frame.elapsed_ms = 0; frame.local_ms = 0; clear_targets(); kp_rgb_effect_render(dev, &frame);
        for (uint32_t t = 0; t < total;) {
            uint32_t delta = MIN(total - t, t % 7 + 1);
            t += delta; frame.elapsed_ms = delta; frame.local_ms = t;
            kp_rgb_effect_render(dev, &frame);
            frame.elapsed_ms = 0; kp_rgb_effect_render(dev, &frame);
        }
        KP_CHECK(entropy_calls == calls && memcmp(reference, pixels, sizeof(pixels)) == 0);
        size_t before = entropy_calls;
        frame.elapsed_ms = UINT32_MAX; frame.local_ms = INT64_MAX; kp_rgb_effect_render(dev, &frame);
        size_t cap = entry->kind == DIGITAL ? 8 * 24 * 2 : entry->kind == RAIN ? 32 : 8 * KP_LED_COUNT * 4;
        KP_CHECK(entropy_calls - before <= cap);
        memcpy(reference, pixels, sizeof(pixels)); calls = entropy_calls;
        frame.elapsed_ms = 0; kp_rgb_effect_render(dev, &frame);
        KP_CHECK(entropy_calls == calls && memcmp(reference, pixels, sizeof(pixels)) == 0);
        reset_device(dev, 0); entropy_calls = 0;
        frame.local_ms = 0; frame.elapsed_ms = 0; kp_rgb_effect_render(dev, &frame);
        size_t seeded_calls = entropy_calls;
        if (entry->kind == RAIN && entry->mode == 2) {
            frame.elapsed_ms = kp_rgb_effect_period(dev) / 8;
            frame.local_ms = frame.elapsed_ms; kp_rgb_effect_render(dev, &frame);
            KP_CHECK(entropy_calls - seeded_calls == (KP_LED_COUNT ? 16 : 0));
        } else if (entry->kind == STARLIGHT || (entry->kind == RAIN && entry->mode != 4)) {
            frame.elapsed_ms = entry->interval - 1; frame.local_ms = frame.elapsed_ms;
            kp_rgb_effect_render(dev, &frame); KP_CHECK(entropy_calls == seeded_calls);
            frame.elapsed_ms = 1; frame.local_ms++;
            kp_rgb_effect_render(dev, &frame);
            size_t trials = entropy_calls - seeded_calls;
            if (!KP_LED_COUNT) KP_CHECK(trials == 0);
            else if (entry->kind == STARLIGHT) KP_CHECK(trials >= KP_LED_COUNT && trials <= 4 * KP_LED_COUNT);
            else KP_CHECK(trials == (entry->mode == 1 ? 1 : entry->mode == 3 ? 4 : 3));
            if (entry->kind == RAIN && entry->interval == 65535) {
                frame.elapsed_ms = 1;
                for (unsigned i = 0; i < 1000; i++) { frame.local_ms++; kp_rgb_effect_render(dev, &frame); }
                for (size_t i = 0; i < KP_LED_COUNT; i++) KP_CHECK(rgb_equal(pixels[i], (struct led_rgb){0}));
            }
        }
    }
    const struct device *smooth = DEV(effects_star_65535_1);
    reset_device(smooth, 0); entropy_calls = 0;
    frame.local_ms = 0; frame.elapsed_ms = 0; clear_targets(); kp_rgb_effect_render(smooth, &frame);
    frame.local_ms = 65535; frame.elapsed_ms = 65535; kp_rgb_effect_render(smooth, &frame);
    if (KP_LED_COUNT) KP_CHECK(rgb_equal(pixels[0], (struct led_rgb){0}));
    frame.elapsed_ms = 1;
    for (unsigned i = 0; i < 160; i++) { frame.local_ms++; kp_rgb_effect_render(smooth, &frame); }
    if (KP_LED_COUNT) KP_CHECK(rgb_equal(pixels[0], (struct led_rgb){0, 38, 38}));
    frame.local_ms++; kp_rgb_effect_render(smooth, &frame);
    if (KP_LED_COUNT) KP_CHECK(rgb_equal(pixels[0], (struct led_rgb){0, 40, 40}));
}
static void feedback(void) {
    const struct device *feedback_devices[] = {DEV(effects_reactive_a), DEV(effects_heat_no_spread), DEV(fx_ripple)};
    for (size_t n = 0; n < ARRAY_SIZE(feedback_devices); n++) {
        const struct device *dev = feedback_devices[n];
        const struct kp_rgb_effect_callbacks *cb = callbacks(dev);
        struct kp_rgb_effect_common_data saved = *kp_rgb_effect_data(dev);
        int64_t epoch = (int64_t)UINT32_MAX + 10000;
        reset_device(dev, epoch);
        inject(dev, 0, epoch + 1000);
        frame.local_ms = epoch + 1000; frame.elapsed_ms = 0; clear_targets();
        KP_CHECK(kp_rgb_effect_render(dev, &frame) == (KP_LED_COUNT > 0));
        if (n == 1 && KP_LED_COUNT > 0) {
            KP_CHECK(!rgb_equal(pixels[0], (struct led_rgb){0}));
            for (size_t i = 1; i < KP_LED_COUNT; i++)
                KP_CHECK(rgb_equal(pixels[i], (struct led_rgb){0}));
        }
        frame.local_ms += 100000; frame.elapsed_ms = 100000;
        KP_CHECK(!kp_rgb_effect_render(dev, &frame));
        inject(dev, 0, frame.local_ms + 1);
        frame.local_ms++; frame.elapsed_ms = 0;
        KP_CHECK(kp_rgb_effect_render(dev, &frame) == (KP_LED_COUNT > 0));
        cb->set_active(dev, false, frame.local_ms);
        KP_CHECK(!kp_rgb_effect_render(dev, &frame));
        common_equal(dev, saved);
        if (!KP_LED_COUNT) continue;
        if (n < 2) {
            reset_device(dev, 0);
            struct kp_rgb_key_event event = {.position = 0, .pressed = true, .timestamp_ms = 0};
            for (unsigned i = 0; i < 16; i++) KP_CHECK(cb->on_event(dev, &event));
            KP_CHECK(!cb->on_event(dev, &event));
            cb->set_active(dev, false, 0); KP_CHECK(cb->on_event(dev, &event));
            cb->reset(dev, 0); frame.local_ms = 0; frame.elapsed_ms = 0;
            KP_CHECK(!kp_rgb_effect_render(dev, &frame));
            event.timestamp_ms = INT64_MIN; KP_CHECK(cb->on_event(dev, &event));
            frame.local_ms = 0; frame.elapsed_ms = UINT32_MAX;
            KP_CHECK(kp_rgb_effect_render(dev, &frame));
            struct led_rgb expected_fresh = n == 0 ? (struct led_rgb){0, 255, 255} :
                kp_rgb_hsb_to_rgb((struct kp_rgb_hsb){.h = 209, .s = 100, .b = 12});
            KP_CHECK(rgb_equal(pixels[0], expected_fresh));
            frame.local_ms = INT64_MAX; KP_CHECK(!kp_rgb_effect_render(dev, &frame));
            cb->reset(dev, 0); event.timestamp_ms = INT64_MAX;
            KP_CHECK(cb->on_event(dev, &event));
            event.position = 1; event.timestamp_ms = INT64_MIN; KP_CHECK(cb->on_event(dev, &event));
            frame.local_ms = epoch; frame.elapsed_ms = UINT32_MAX;
            KP_CHECK(kp_rgb_effect_render(dev, &frame));
            KP_CHECK(rgb_equal(pixels[0], expected_fresh) && rgb_equal(pixels[1], expected_fresh));
            cb->reset(dev, 0); inject(dev, 0, 100);
            frame.local_ms = 100; frame.elapsed_ms = 0; kp_rgb_effect_render(dev, &frame);
            struct led_rgb fresh = pixels[0];
            frame.local_ms += 1000000; kp_rgb_effect_render(dev, &frame);
            KP_CHECK(rgb_equal(fresh, pixels[0]));
            memcpy(reference, pixels, sizeof(pixels));
            frame.local_ms += 1000000; kp_rgb_effect_render(dev, &frame);
            KP_CHECK(memcmp(reference, pixels, sizeof(pixels)) == 0);
            frame.elapsed_ms = 1; frame.local_ms++; kp_rgb_effect_render(dev, &frame);
            frame.elapsed_ms = 0; frame.local_ms += 1000000;
            KP_CHECK(kp_rgb_effect_render(dev, &frame));
            KP_CHECK(rgb_equal(fresh, pixels[0]));
            cb->reset(dev, 0); inject(dev, 0, 100);
            frame.local_ms = 100; frame.elapsed_ms = 0; kp_rgb_effect_render(dev, &frame);
            for (unsigned i = 0; i < 25; i++) { frame.local_ms++; frame.elapsed_ms = 1; kp_rgb_effect_render(dev, &frame); }
            struct led_rgb cooled = pixels[0];
            struct led_rgb expected_cooled = n == 0 ? (struct led_rgb){0, 247, 247} :
                kp_rgb_hsb_to_rgb((struct kp_rgb_hsb){.h = 210, .s = 100, .b = 12});
            KP_CHECK(rgb_equal(cooled, expected_cooled));
            inject(dev, 1, 200);
            KP_CHECK(rgb_equal(cooled, pixels[0]));
            frame.local_ms = 200; frame.elapsed_ms = 75; kp_rgb_effect_render(dev, &frame);
            KP_CHECK(rgb_equal(pixels[1], fresh));
            struct led_rgb expected_older = n == 0 ? (struct led_rgb){0, 229, 229} :
                kp_rgb_hsb_to_rgb((struct kp_rgb_hsb){.h = 213, .s = 100, .b = 10});
            KP_CHECK(rgb_equal(pixels[0], expected_older));
            inject(dev, 1, frame.local_ms + 100000);
            frame.local_ms += 100000; frame.elapsed_ms = 100000;
            KP_CHECK(kp_rgb_effect_render(dev, &frame));
            KP_CHECK(rgb_equal(pixels[0], (struct led_rgb){0}) && rgb_equal(pixels[1], expected_fresh));
        } else {
            uint32_t period = kp_rgb_effect_period(dev);
            cb->reset(dev, 0); inject(dev, 0, 100);
            frame.local_ms = 100 + period - 1; frame.elapsed_ms = 0;
            KP_CHECK(kp_rgb_effect_render(dev, &frame));
            frame.local_ms++; KP_CHECK(!kp_rgb_effect_render(dev, &frame));
            inject(dev, 0, INT64_MIN); frame.local_ms = INT64_MAX; KP_CHECK(!kp_rgb_effect_render(dev, &frame));
            inject(dev, 0, INT64_MAX); frame.local_ms = 0; KP_CHECK(kp_rgb_effect_render(dev, &frame));
            cb->reset(dev, 0); KP_CHECK(!kp_rgb_effect_render(dev, &frame));
        }
    }
}
static void instances(void) {
    struct kp_rgb_effect_instance a = KP_RGB_EFFECT_INSTANCE_INIT(DEV(effects_probe_a));
    struct kp_rgb_effect_instance b = KP_RGB_EFFECT_INSTANCE_INIT(DEV(effects_probe_b));
    struct kp_rgb_effect_instance optional = KP_RGB_EFFECT_INSTANCE_INIT(DEV(effects_optional));
    struct effects_observation *oa = a.effect->data, *ob = b.effect->data;
    oa->animate = ob->animate = true; oa->event_result = true;
    struct kp_rgb_key_event event = {.position = 17, .pressed = true, .timestamp_ms = -123};
    kp_rgb_effect_instance_set_active(&a, false, 9);
    KP_CHECK(!a.state.initialized && !oa->resets && !oa->activates);
    KP_CHECK(!kp_rgb_effect_instance_on_event(&a, &event));
    clear_targets(); KP_CHECK(!kp_rgb_effect_instance_render(&a, &frame));
    KP_CHECK(!oa->renders);
    kp_rgb_effect_instance_set_active(&a, true, 10);
    KP_CHECK(a.state.active && a.state.initialized && !a.state.animating);
    KP_CHECK(oa->resets == 1 && oa->reset_time == 10 && oa->activates == 1 && oa->active_time == 10);
    KP_CHECK(oa->reset_order < oa->active_order);
    kp_rgb_effect_instance_set_active(&a, true, 11); KP_CHECK(oa->activates == 1 && oa->resets == 1);
    KP_CHECK(kp_rgb_effect_instance_on_event(&a, &event));
    KP_CHECK(oa->events == 1 && oa->event.timestamp_ms == -123);
    kp_rgb_effect_instance_set_active(&a, false, 12);
    KP_CHECK(!kp_rgb_effect_instance_on_event(&a, &event) && oa->events == 1);
    kp_rgb_effect_instance_set_active(&a, false, 13); KP_CHECK(oa->deactivates == 1);
    kp_rgb_effect_instance_set_active(&a, true, 20); KP_CHECK(oa->resets == 1);
    frame.local_ms = 100; frame.elapsed_ms = 77;
    KP_CHECK(kp_rgb_effect_instance_render(&a, &frame) && oa->frame.elapsed_ms == 0);
    frame.local_ms = 140; KP_CHECK(kp_rgb_effect_instance_render(&a, &frame) && oa->frame.elapsed_ms == 40);
    kp_rgb_effect_instance_reset(&a, 141);
    KP_CHECK(a.state.active && !a.state.animating && oa->reset_time == 141 && oa->resets == 2);
    frame.local_ms = 200; kp_rgb_effect_instance_render(&a, &frame); KP_CHECK(oa->frame.elapsed_ms == 0);
    oa->animate = false; frame.local_ms = 220; KP_CHECK(!kp_rgb_effect_instance_render(&a, &frame));
    oa->animate = true; frame.local_ms = 1000; kp_rgb_effect_instance_render(&a, &frame); KP_CHECK(oa->frame.elapsed_ms == 0);
    frame.local_ms = 990; kp_rgb_effect_instance_render(&a, &frame); KP_CHECK(oa->frame.elapsed_ms == 0);
    kp_rgb_effect_instance_reset(&a, INT64_MIN); frame.local_ms = INT64_MIN;
    kp_rgb_effect_instance_render(&a, &frame); frame.local_ms = INT64_MAX;
    kp_rgb_effect_instance_render(&a, &frame); KP_CHECK(oa->frame.elapsed_ms == UINT32_MAX);
    kp_rgb_effect_instance_set_active(&b, true, 0); frame.local_ms = 20;
    kp_rgb_effect_instance_render(&b, &frame); KP_CHECK(ob->frame.elapsed_ms == 0);
    frame.local_ms = 30; kp_rgb_effect_instance_render(&b, &frame); KP_CHECK(ob->frame.elapsed_ms == 10);
    kp_rgb_effect_instance_restart_clock(&a); kp_rgb_effect_instance_restart_clock(&b);
    frame.local_ms = 500; kp_rgb_effect_instance_render(&a, &frame); KP_CHECK(oa->frame.elapsed_ms == 0);
    /* b was skipped in the owner's zero-delta render. */
    frame.local_ms = 510; kp_rgb_effect_instance_render(&a, &frame); kp_rgb_effect_instance_render(&b, &frame);
    KP_CHECK(oa->frame.elapsed_ms == 10 && ob->frame.elapsed_ms == 0);
    KP_CHECK(!kp_rgb_effect_instance_on_event(&optional, &event));
    kp_rgb_effect_instance_set_active(&optional, true, 8);
    KP_CHECK(optional.state.active && optional.state.initialized);
    kp_rgb_effect_instance_render(&optional, &frame);
    kp_rgb_effect_instance_reset(&optional, 9); kp_rgb_effect_instance_set_active(&optional, false, 10);
    KP_CHECK(!optional.state.active && optional.state.initialized);
    /* Independent real feedback devices are not shared by the helpers. */
    a = (struct kp_rgb_effect_instance)KP_RGB_EFFECT_INSTANCE_INIT(DEV(effects_reactive_a));
    b = (struct kp_rgb_effect_instance)KP_RGB_EFFECT_INSTANCE_INIT(DEV(effects_reactive_b));
    kp_rgb_effect_instance_set_active(&a, true, 0); kp_rgb_effect_instance_set_active(&b, true, 0);
    event.position = 0; event.timestamp_ms = 100;
    KP_CHECK(kp_rgb_effect_instance_on_event(&a, &event) == (KP_LED_COUNT > 0));
    frame.local_ms = 100; clear_targets(); kp_rgb_effect_instance_render(&a, &frame);
    if (KP_LED_COUNT) KP_CHECK(rgb_equal(pixels[0], (struct led_rgb){0, 255, 255}));
    clear_targets(); KP_CHECK(!kp_rgb_effect_instance_render(&b, &frame));
    if (KP_LED_COUNT) KP_CHECK(rgb_equal(pixels[0], (struct led_rgb){0}));
}
static void dispatch(void) {
    const struct device *dev = DEV(effects_probe_a);
    struct effects_observation *o = dev->data;
    o->animate = true;
    frame.local_ms = -7; frame.clock_ms = UINT32_MAX; frame.elapsed_ms = 77;
    clear_targets(); KP_CHECK(kp_rgb_effect_render(dev, &frame));
    KP_CHECK(o->renders == 1 && !o->resets && !o->activates && !o->events);
    KP_CHECK(o->frame.local_ms == -7 && o->frame.clock_ms == UINT32_MAX && o->frame.elapsed_ms == 77);
    KP_CHECK(o->frame.count == KP_LED_COUNT && o->frame.coords == coords && o->frame.targets == all && o->frame.pixels == pixels);
    sentinels();
    struct kp_rgb_effect_instance instance = KP_RGB_EFFECT_INSTANCE_INIT(dev);
    KP_CHECK(!instance.state.active && !instance.state.initialized);
    KP_CHECK(kp_rgb_effect_render(dev, &frame));
    KP_CHECK(!instance.state.initialized && !instance.state.animating && o->frame.elapsed_ms == 77);
    KP_CHECK(!kp_rgb_effect_render(DEV(effects_optional), &frame));
}
static void generic_overlay(void) {
    const struct device *owner = DEV(effects_owner);
    const struct kp_rgb_effect_callbacks *cb = overlay_callbacks(owner);
    struct effects_observation *o = DEV(effects_owned)->data;
    size_t targets[] = {3, 1};
    o->check_black = true;
    frame.targets = targets; frame.target_count = KP_LED_COUNT ? 2 : 0;
    frame.elapsed_ms = 37; frame.local_ms = 123; frame.clock_ms = 456; frame.scratch = workspace;
    memset(pixels, 0xa5, sizeof(pixels)); memset(workspace, 0xa5, sizeof(workspace));
    for (size_t t = 0; t < frame.target_count; t++) pixels[targets[t]] = (struct led_rgb){17, 0, 0};
    KP_CHECK(!cb->render(owner, &frame));
    KP_CHECK(o->frame.count == KP_LED_COUNT && o->frame.coords == coords && o->frame.targets == targets);
    KP_CHECK(o->frame.target_count == frame.target_count && o->frame.pixels == workspace && !o->frame.scratch);
    KP_CHECK(o->frame.elapsed_ms == 37 && o->frame.local_ms == 123 && o->frame.clock_ms == 456);
    for (size_t i = 0; i < CAP; i++) {
        bool selected = KP_LED_COUNT && (i == 1 || i == 3);
        KP_CHECK(rgb_equal(pixels[i], selected ? (struct led_rgb){49, 27, 37} : (struct led_rgb){0xa5, 0xa5, 0xa5}));
        KP_CHECK(rgb_equal(workspace[i], selected ? (struct led_rgb){104, 73, 101} : (struct led_rgb){0xa5, 0xa5, 0xa5}));
    }
    cb->reset(owner, 11); cb->set_active(owner, true, 12);
    struct kp_rgb_key_event event = {.position = 1, .pressed = true, .timestamp_ms = -123};
    o->event_result = true; KP_CHECK(cb->on_event(owner, &event));
    KP_CHECK(o->resets == 1 && o->reset_time == 11 && o->activates == 1 && o->active_time == 12);
    KP_CHECK(o->event.timestamp_ms == -123 && o->event.position == 1);
    cb->set_active(owner, false, 13); KP_CHECK(o->deactivates == 1);
    o->animate = true; KP_CHECK(cb->render(owner, &frame));
    frame.target_count = 0; memset(pixels, 0xa5, sizeof(pixels)); memset(workspace, 0xa5, sizeof(workspace));
    KP_CHECK(cb->render(owner, &frame)); sentinels();
    for (size_t i = 0; i < CAP; i++) KP_CHECK(rgb_equal(workspace[i], (struct led_rgb){0xa5, 0xa5, 0xa5}));
    /* Terminal real child leaves unpainted target black, never stale workspace. */
    owner = DEV(effects_real_owner); cb = overlay_callbacks(owner);
    cb->reset(owner, 0); cb->set_active(owner, true, 0);
    frame.target_count = KP_LED_COUNT ? 2 : 0;
    memset(pixels, 0xa5, sizeof(pixels)); memset(workspace, 0xa5, sizeof(workspace));
    KP_CHECK(!cb->render(owner, &frame));
    for (size_t t = 0; t < frame.target_count; t++) KP_CHECK(rgb_equal(pixels[targets[t]], (struct led_rgb){0}));
    sentinels();
    KP_CHECK(cb->on_event(owner, &event) == (KP_LED_COUNT > 0));
    frame.local_ms = 0; frame.elapsed_ms = 0;
    KP_CHECK(cb->render(owner, &frame) == (KP_LED_COUNT > 0));
    if (KP_LED_COUNT) KP_CHECK(rgb_equal(pixels[1], (struct led_rgb){255, 255, 255}));
    cb->set_active(owner, false, 14); KP_CHECK(!cb->render(owner, &frame));
    owner = DEV(effects_optional_owner); cb = overlay_callbacks(owner);
    o = DEV(effects_optional_child)->data;
    cb->reset(owner, 21); cb->set_active(owner, true, 22);
    KP_CHECK(!cb->on_event(owner, &event));
    cb->set_active(owner, false, 23);
    KP_CHECK(!o->resets && !o->activates && !o->deactivates && !o->events);
    o->check_black = true;
    memset(workspace, 0xa5, sizeof(workspace));
    KP_CHECK(!cb->render(owner, &frame));
    KP_CHECK(o->renders == 1 && o->frame.scratch == NULL && o->frame.elapsed_ms == frame.elapsed_ms);
    for (size_t t = 0; t < frame.target_count; t++) KP_CHECK(rgb_equal(pixels[targets[t]], (struct led_rgb){104, 73, 101}));
}
static void flush_isr(const void *arg) {
    ARG_UNUSED(arg);
    zmk_rgb_matrix_flush();
}
static void private_parameters(void) {
    const struct device *dev = DEV(effects_fallback);
    KP_CHECK(device_is_ready(dev) && kp_rgb_effect_cfg(dev)->owner == NULL);
    KP_CHECK(kp_rgb_effect_period(dev) == CLAMP(1000, CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS, CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS));
    KP_CHECK(rgb_equal(kp_rgb_hsb_to_rgb(kp_rgb_effect_data(dev)->color), kp_rgb_hsb_to_rgb((struct kp_rgb_hsb)KP_RGB_HSB_FROM_HEX(0x07111f))));
    kp_rgb_matrix_unlock();
    struct kp_rgb_hsb color = {.h = 120, .s = 90, .b = 80};
    KP_CHECK(zmk_rgb_matrix_set_color(DEV(effects_reactive_a), &color) == 0);
    KP_CHECK(zmk_rgb_matrix_set_period(DEV(effects_reactive_a), 1000) == 0);
    irq_offload(flush_isr, NULL);
    kp_rgb_matrix_lock();
    dev = DEV(effects_reactive_a);
    struct kp_rgb_effect_common_data saved = *kp_rgb_effect_data(dev);
    KP_CHECK(saved.color.h == 120 && saved.color.s == 90 && saved.color.b == 80);
    reset_device(dev, 0); common_equal(dev, saved);
    callbacks(dev)->set_active(dev, false, 0); common_equal(dev, saved);
    KP_CHECK(kp_rgb_effect_cfg(DEV(effects_owned_reactive))->owner == NULL);
}
static void enums(void) {
    mode_t permissions = 0644; KP_CHECK(permissions == 0644);
    KP_CHECK(ARRAY_SIZE(first_modes) == 3 && ARRAY_SIZE(second_modes) == 3);
    /* Instance numbering is dependency ordered; resolve each instance's real DTS token. */
#define DT_DRV_COMPAT test_rgb_effects_enum_first
#define CHECK_FIRST(inst) KP_CHECK(first_modes[inst] == DT_ENUM_IDX(DT_DRV_INST(inst), mode));
    DT_INST_FOREACH_STATUS_OKAY(CHECK_FIRST)
#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT test_rgb_effects_enum_second
#define CHECK_SECOND(inst) KP_CHECK(second_modes[inst] == DT_ENUM_IDX(DT_DRV_INST(inst), mode));
    DT_INST_FOREACH_STATUS_OKAY(CHECK_SECOND)
#undef DT_DRV_COMPAT
    KP_CHECK(DT_ENUM_IDX(DT_NODELABEL(effects_enum_first_0), mode) == 1);
    KP_CHECK(DT_ENUM_IDX(DT_NODELABEL(effects_enum_second_0), mode) == 2);
}
static void battery(void) {
    const size_t targets[] = {2, 3, 0, 1};
    const struct device *devs[] = {DEV(effects_battery), DEV(effects_battery_reverse)};
    const struct led_rgb on = {201, 73, 101}, off = {7, 17, 31};
    frame.targets = targets; frame.scratch = workspace;
    for (size_t d = 0; d < ARRAY_SIZE(devs); d++) {
        const struct kp_rgb_effect_callbacks *cb = overlay_callbacks(devs[d]);
        battery_soc = 50;
        frame.target_count = KP_LED_COUNT ? 4 : 0;
        memset(pixels, 0xa5, sizeof(pixels)); KP_CHECK(!cb->render(devs[d], &frame));
        if (KP_LED_COUNT) for (size_t i = 0; i < 4; i++) KP_CHECK(rgb_equal(pixels[i], (d ? i >= 2 : i < 2) ? on : off));
        sentinels();
        frame.target_count = KP_LED_COUNT ? 2 : 0;
        memset(pixels, 0xa5, sizeof(pixels)); KP_CHECK(!cb->render(devs[d], &frame));
        if (KP_LED_COUNT) {
            KP_CHECK(rgb_equal(pixels[d ? 3 : 2], on)); KP_CHECK(rgb_equal(pixels[d ? 2 : 3], off));
        }
        KP_CHECK(targets[0] == 2 && targets[1] == 3); sentinels();
        frame.target_count = 0; memset(pixels, 0xa5, sizeof(pixels));
        KP_CHECK(!cb->render(devs[d], &frame)); sentinels();
        frame.target_count = KP_LED_COUNT ? 4 : 0; battery_soc = 255;
        memset(pixels, 0xa5, sizeof(pixels)); cb->render(devs[d], &frame);
        for (size_t t = 0; t < frame.target_count; t++) KP_CHECK(rgb_equal(pixels[targets[t]], on));
        sentinels();
    }
}
void kp_test_effects_run(void) {
    geometry();
    kp_rgb_matrix_lock();
    if (kp_test_case("effects-variants")) variants();
    else if (kp_test_case("effects-targets")) targets();
    else if (kp_test_case("effects-brightness")) brightness();
    else if (kp_test_case("effects-phase")) phase();
    else if (kp_test_case("effects-random")) random_timing();
    else if (kp_test_case("effects-feedback")) feedback();
    else if (kp_test_case("effects-instances")) instances();
    else if (kp_test_case("effects-dispatch")) dispatch();
    else if (kp_test_case("effects-overlay")) generic_overlay();
    else if (kp_test_case("effects-private")) private_parameters();
    else if (kp_test_case("effects-enums")) enums();
    else if (kp_test_case("effects-battery")) battery();
    else KP_CHECK(false);
    kp_rgb_matrix_unlock();
}
