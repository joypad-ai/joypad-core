// dualsense_mode.c - PlayStation 5 DualSense output mode (auth passthrough)
// SPDX-License-Identifier: Apache-2.0
//
// Presents as a DualSense (054c:0ce6) and relays the PS5's controller-auth
// challenge to a REAL DualSense on the USB host port via ds5_auth. Input/output
// reports are built by overlaying the SAME structs our DualSense USB *input*
// driver uses (sony_ds5.h) — so the byte layout is exact by construction (input
// read-offset == output write-offset == real DualSense). Modeled on ps4_mode.c.
//
// Complete except: no USB audio (headset), no adaptive-trigger effects.
// Auth handshake remains sniff-gated (see .dev/docs/ds5-auth-passthrough-plan.md).

#include "usbd_mode.h"
#include "descriptors/dualsense_descriptors.h"
#include "usb/usbh/hid/devices/vendors/sony/sony_ds5.h"  // sony_ds5_report_t + ds5_feedback_t
#ifndef DISABLE_USB_HOST
#include "usb/usbh/hid/devices/vendors/sony/ds5_auth.h"   // host-side relay (USB host only)
#endif
#include "core/buttons.h"
#include "platform/platform.h"
#include "app_config.h"   // LED_Px_PATTERN — reverse-map host player-LED patterns
#include "tusb.h"
#include <stdio.h>
#include <string.h>

static uint8_t ds5_report_buffer[64];
static uint8_t ds5_report_counter = 0;
// Per-finger touch id state (id bumps on each new touch-down so hosts track it).
static uint8_t ds5_tp_id[2] = {0, 0};
static bool ds5_tp_prev[2] = {false, false};

static void ds5_mode_init(void)
{
    memset(ds5_report_buffer, 0, sizeof(ds5_report_buffer));
    ds5_report_buffer[0] = 0x01;
    sony_ds5_report_t* r = (sony_ds5_report_t*)&ds5_report_buffer[1];
    r->x1 = r->y1 = r->x2 = r->y2 = DS5_JOYSTICK_MID;
    r->dpad = DS5_HAT_NOTHING;
    r->tpad_f1_down = 1;  // released
    r->tpad_f2_down = 1;
    ds5_report_counter = 0;
}

static bool ds5_mode_is_ready(void) { return tud_hid_ready(); }

// Pack a 12-bit X / 12-bit Y touch point into the DualSense 3-byte format.
static void ds5_pack_touch(int8_t pos[3], uint16_t x, uint16_t y)
{
    pos[0] = (int8_t)(x & 0xFF);
    pos[1] = (int8_t)(((x >> 8) & 0x0F) | ((y & 0x0F) << 4));
    pos[2] = (int8_t)((y >> 4) & 0xFF);
}

// Build the DualSense input report 0x01 by populating sony_ds5_report_t in place.
static bool ds5_mode_send_report(uint8_t player_index,
                                 const input_event_t* event,
                                 const profile_output_t* profile_out,
                                 uint32_t buttons)
{
    (void)player_index;

    memset(ds5_report_buffer, 0, sizeof(ds5_report_buffer));
    ds5_report_buffer[0] = 0x01;  // report id (TinyUSB prepends its own copy)
    sony_ds5_report_t* r = (sony_ds5_report_t*)&ds5_report_buffer[1];

    // Sticks + analog triggers (x1/y1/x2/y2 = LX/LY/RX/RY, rx/ry = L2/R2, rz = seq)
    r->x1 = profile_out->left_x;
    r->y1 = profile_out->left_y;
    r->x2 = profile_out->right_x;
    r->y2 = profile_out->right_y;
    r->rx = profile_out->l2_analog;
    r->ry = profile_out->r2_analog;
    r->rz = ds5_report_counter;

    // D-pad hat
    uint8_t up    = (buttons & JP_BUTTON_DU) ? 1 : 0;
    uint8_t down  = (buttons & JP_BUTTON_DD) ? 1 : 0;
    uint8_t left  = (buttons & JP_BUTTON_DL) ? 1 : 0;
    uint8_t right = (buttons & JP_BUTTON_DR) ? 1 : 0;
    if      (up && right)   r->dpad = 1;
    else if (down && right) r->dpad = 3;
    else if (down && left)  r->dpad = 5;
    else if (up && left)    r->dpad = 7;
    else if (up)            r->dpad = 0;
    else if (right)         r->dpad = 2;
    else if (down)          r->dpad = 4;
    else if (left)          r->dpad = 6;
    else                    r->dpad = DS5_HAT_NOTHING;

    // Face buttons
    r->square   = (buttons & JP_BUTTON_B3) ? 1 : 0;
    r->cross    = (buttons & JP_BUTTON_B1) ? 1 : 0;
    r->circle   = (buttons & JP_BUTTON_B2) ? 1 : 0;
    r->triangle = (buttons & JP_BUTTON_B4) ? 1 : 0;

    // Shoulders / triggers-digital / create / options / stick clicks
    r->l1     = (buttons & JP_BUTTON_L1) ? 1 : 0;
    r->r1     = (buttons & JP_BUTTON_R1) ? 1 : 0;
    r->l2     = usbd_l2_digital(profile_out, buttons) ? 1 : 0;
    r->r2     = usbd_r2_digital(profile_out, buttons) ? 1 : 0;
    r->share  = (buttons & JP_BUTTON_S1) ? 1 : 0;
    r->option = (buttons & JP_BUTTON_S2) ? 1 : 0;
    r->l3     = (buttons & JP_BUTTON_L3) ? 1 : 0;
    r->r3     = (buttons & JP_BUTTON_R3) ? 1 : 0;

    // PS / touchpad-click / mute + counter
    r->ps      = (buttons & JP_BUTTON_A1) ? 1 : 0;
    r->tpad    = (buttons & JP_BUTTON_A2) ? 1 : 0;
    r->mute    = 0;
    r->counter = ds5_report_counter & 0x1F;
    ds5_report_counter++;

    // Motion — pass the input's gyro/accel through, scaled by its declared range
    // into the DualSense's ±2000 dps / ±4 g full-scale.
    if (event->has_motion) {
        int32_t gr = event->gyro_range  ? event->gyro_range  : 2000;
        int32_t ar = event->accel_range ? event->accel_range : 4000;
        for (int i = 0; i < 3; i++) {
            int32_t g = (int32_t)event->gyro[i]  * gr / 2000;
            int32_t a = (int32_t)event->accel[i] * ar / 4000;
            if (g > 32767) g = 32767; else if (g < -32768) g = -32768;
            if (a > 32767) a = 32767; else if (a < -32768) a = -32768;
            r->gyro[i]  = (int16_t)g;
            r->accel[i] = (int16_t)a;
        }
    } else {
        r->accel[2] = 8192;  // +1 g on Z: level & still
    }
    r->sensor_timestamp = (uint32_t)(platform_time_ms() * 3000u);  // ~0.33us units

    // Touchpad — both fingers, native 1920x1080, packed 12-bit X/Y.
    for (int f = 0; f < 2; f++) {
        bool active = event->has_touch && event->touch[f].active;
        if (active && !ds5_tp_prev[f]) ds5_tp_id[f] = (uint8_t)((ds5_tp_id[f] + 1) & 0x7F);
        ds5_tp_prev[f] = active;
        uint8_t count = ds5_tp_id[f] & 0x7F;
        int8_t* pos = (f == 0) ? r->tpad_f1_pos : r->tpad_f2_pos;
        if (f == 0) { r->tpad_f1_count = count; r->tpad_f1_down = active ? 0 : 1; }
        else        { r->tpad_f2_count = count; r->tpad_f2_down = active ? 0 : 1; }
        if (active) {
            uint16_t x = (uint16_t)((uint32_t)event->touch[f].x * 1919 / 65535);
            uint16_t y = (uint16_t)((uint32_t)event->touch[f].y * 1079 / 65535);
            ds5_pack_touch(pos, x, y);
        }
    }

    // Battery/status at data offset 53 (past sony_ds5_report_t). Low nibble =
    // level/10 (0-10), bit 4 = charging.
    if (event->battery_level > 0) {
        uint8_t lvl = event->battery_level / 10; if (lvl > 10) lvl = 10;
        ds5_report_buffer[54] = lvl | (event->battery_charging ? 0x10 : 0x00);
    } else {
        ds5_report_buffer[54] = 0x0A | 0x10;  // full + charging fallback
    }

    return tud_hid_report(0x01, &ds5_report_buffer[1], 63);
}

// GET_REPORT feature dispatch — firmware info + auth via ds5_auth.
static uint16_t ds5_mode_get_report(uint8_t report_id, hid_report_type_t report_type,
                                    uint8_t* buffer, uint16_t reqlen)
{
    if (report_type != HID_REPORT_TYPE_FEATURE) return 0;

    if (report_id == DS5_REPORT_ID_FIRMWARE) {  // 0x20 — host identifies the DualSense
        uint16_t n = reqlen < sizeof(ds5_feature_20) ? reqlen : sizeof(ds5_feature_20);
        memcpy(buffer, ds5_feature_20, n);
        return n;
    }

    switch (report_id) {
        case DS5_REPORT_ID_AUTH_RESPONSE:  // 0xF1
#ifndef DISABLE_USB_HOST
            if (ds5_auth_is_available()) return ds5_auth_get_next_signature(buffer, reqlen);
#endif
            memset(buffer, 0, reqlen); return reqlen;

        case DS5_REPORT_ID_AUTH_STATUS:    // 0xF2
#ifndef DISABLE_USB_HOST
            if (ds5_auth_is_available()) return ds5_auth_get_status(buffer, reqlen);
#endif
            memset(buffer, 0, reqlen); buffer[1] = 0x10; return reqlen;  // "signing"

        case DS5_REPORT_ID_AUTH_RESET:     // 0xF3
#ifndef DISABLE_USB_HOST
            ds5_auth_reset();
#endif
            memset(buffer, 0, reqlen);
            memcpy(buffer, ds5_feature_f3,
                   reqlen < sizeof(ds5_feature_f3) ? reqlen : sizeof(ds5_feature_f3));
            return reqlen;

        default:
            return 0;
    }
}

// SET_REPORT feature dispatch — called from usbd.c tud_hid_set_report_cb.
void ds5_mode_set_feature_report(uint8_t report_id, const uint8_t* buffer, uint16_t bufsize)
{
    switch (report_id) {
        case DS5_REPORT_ID_AUTH_NONCE:  // 0xF0
#ifndef DISABLE_USB_HOST
            if (ds5_auth_is_available()) ds5_auth_send_nonce(buffer, bufsize);
#else
            (void)buffer; (void)bufsize;
#endif
            break;
        case DS5_REPORT_ID_AUTH_RESET:  // 0xF3
#ifndef DISABLE_USB_HOST
            ds5_auth_reset();
#endif
            break;
        default:
            break;
    }
}

// --- Output (host → controller): rumble / lightbar / player LEDs -------------
static struct {
    uint8_t motor_left, motor_right;
    uint8_t led_r, led_g, led_b;
    uint8_t player_leds;   // DualSense 5-bit bitmask
    bool available;
} ds5_out;

// The host's DualSense output report 0x02 body IS a ds5_feedback_t (sony_ds5.h):
// rumble_r@2, rumble_l@3, player_led@43, lightbar RGB@44-46. On the OUT endpoint
// TinyUSB delivers report_id=0 with the real id in data[0].
static void ds5_mode_handle_output(uint8_t report_id, const uint8_t* data, uint16_t len)
{
    if (!data || len == 0) return;
    if (report_id == 0 && data[0] == DS5_REPORT_ID_OUTPUT) { report_id = data[0]; data++; len--; }
    if (report_id != DS5_REPORT_ID_OUTPUT || len < 4) return;  // need at least the motors

    // Parse whatever fields the host sent (rumble @2/3, player_led @43, RGB @44-46).
    const ds5_feedback_t* fb = (const ds5_feedback_t*)data;
    uint8_t new_ml = fb->rumble_l;
    uint8_t new_mr = fb->rumble_r;
    uint8_t new_pl = ds5_out.player_leds;
    uint8_t new_r  = ds5_out.led_r, new_g = ds5_out.led_g, new_b = ds5_out.led_b;
    // Player LED only when the host asserts the player_led flag (bit 12); a
    // report without it must not clear the pattern we last latched. NOTE: read
    // the flag byte-wise — flags is a uint16_t at data[0..1] and `data` is often
    // odd-aligned, so `fb->flags` (an unpacked 16-bit load) HardFaults the
    // Cortex-M0+ inside tud_task. Bit 12 lives in the high byte (bit 4 = 0x10).
    if ((data[1] & 0x10) && len > offsetof(ds5_feedback_t, player_led))
        new_pl = fb->player_led & 0x1F;
    // Lightbar RGB (@44-46) only when the host asserts the lightbar flag (bit 10
    // = high byte bit 2 = 0x04), byte-wise for the same odd-alignment reason.
    // Gate on the field offset, NOT sizeof(ds5_feedback_t): the struct is
    // unpacked with a uint16_t so it pads to 48, and a 47-byte body would drop
    // the RGB entirely.
    if ((data[1] & 0x04) && len >= offsetof(ds5_feedback_t, lightbar_r) + 3) {
        new_r = fb->lightbar_r;
        new_g = fb->lightbar_g;
        new_b = fb->lightbar_b;
    }

    // Change-gate like sinput_mode_handle_output / ps4_mode: only mark feedback
    // available when a value actually changes. The host's DualSense driver
    // streams identical output reports continuously; without this gate
    // get_feedback would fire the whole cascade every loop forever.
    if (new_ml != ds5_out.motor_left || new_mr != ds5_out.motor_right ||
        new_pl != ds5_out.player_leds ||
        new_r != ds5_out.led_r || new_g != ds5_out.led_g || new_b != ds5_out.led_b) {
        ds5_out.motor_left  = new_ml;
        ds5_out.motor_right = new_mr;
        ds5_out.player_leds = new_pl;
        ds5_out.led_r = new_r;
        ds5_out.led_g = new_g;
        ds5_out.led_b = new_b;
        ds5_out.available = true;
    }
}

static uint8_t ds5_mode_get_rumble(void)
{
    return (ds5_out.motor_left > ds5_out.motor_right) ? ds5_out.motor_left : ds5_out.motor_right;
}

// Reverse of output_sony_ds5()'s player-number → DualSense LED pattern mapping
// (LED_Px_PATTERN in app_config.h). The host's output report carries the raw
// 5-bit pattern; translate it back to a player number for the feedback system,
// so the pattern round-trips identically to how we drive a connected pad.
static uint8_t ds5_playerled_pattern_to_number(uint8_t pattern)
{
    // if/else (not switch) so this compiles for every app: LED_Px_PATTERN values
    // are per-app and may collide (which a switch rejects as duplicate cases), and
    // the higher slots are optional. First match wins; unknown -> 0.
    uint8_t p = pattern & 0x1F;
    if (p == LED_P1_PATTERN) return 1;
    if (p == LED_P2_PATTERN) return 2;
    if (p == LED_P3_PATTERN) return 3;
    if (p == LED_P4_PATTERN) return 4;
#ifdef LED_P5_PATTERN
    if (p == LED_P5_PATTERN) return 5;
#endif
#ifdef LED_P6_PATTERN
    if (p == LED_P6_PATTERN) return 6;
#endif
#ifdef LED_P7_PATTERN
    if (p == LED_P7_PATTERN) return 7;
#endif
    return 0;  // unrecognized / off
}

static bool ds5_mode_get_feedback(output_feedback_t* fb)
{
    if (!ds5_out.available) return false;
    // Mirror ps4_mode_get_feedback: two 8-bit motors + RGB lightbar. The host's
    // DualSense driver refreshes these continuously; the connected pad's
    // output_sony_ds5() dirty-check throttles what actually goes out.
    fb->rumble_left  = ds5_out.motor_left;
    fb->rumble_right = ds5_out.motor_right;
    fb->led_r = ds5_out.led_r;
    fb->led_g = ds5_out.led_g;
    fb->led_b = ds5_out.led_b;
    // Translate the host's 5-bit player-LED pattern back to a player number
    // (reverse of the host-side player-number → pattern mapping). Emit it ONLY
    // when it actually changes: feedback_set_led_player() writes the LED RGB,
    // which the cascade's feedback_set_led_rgb() then overwrites, so calling it
    // every pass forces led_dirty on and adds churn at the host's output rate.
    // The host streams identical reports continuously, so gate to real changes.
    static uint8_t last_player = 0;
    uint8_t player = ds5_playerled_pattern_to_number(ds5_out.player_leds);
    fb->led_player = (player != last_player) ? player : 0;
    last_player = player;
    fb->dirty = true;
    ds5_out.available = false;
    return true;
}

static const uint8_t* ds5_mode_get_device_descriptor(void) { return (const uint8_t*)&ds5_device_descriptor; }
static const uint8_t* ds5_mode_get_config_descriptor(void) { return ds5_config_descriptor; }
static const uint8_t* ds5_mode_get_report_descriptor(void) { return ds5_report_descriptor; }

const usbd_mode_t dualsense_mode = {
    .name = "PS5 (DualSense passthrough)",
    .mode = USB_OUTPUT_MODE_DUALSENSE,
    .get_device_descriptor = ds5_mode_get_device_descriptor,
    .get_config_descriptor = ds5_mode_get_config_descriptor,
    .get_report_descriptor = ds5_mode_get_report_descriptor,
    .init = ds5_mode_init,
    .send_report = ds5_mode_send_report,
    .is_ready = ds5_mode_is_ready,
    // Feedback to the connected pad: host DualSense driver → handle_output →
    // get_feedback → output_sony_ds5() (report 0x02). Modeled on ps4_mode.
    .handle_output = ds5_mode_handle_output,
    .get_rumble = ds5_mode_get_rumble,
    .get_feedback = ds5_mode_get_feedback,
    .get_report = ds5_mode_get_report,
    .get_class_driver = NULL,
    .task = NULL,
};
