// router.c
// Joypad Core Router Implementation
//
// Zero-latency event-driven routing system.
// Replaces console-specific post_input_event() with unified routing.

#include "router.h"
#include "core/buttons.h"
#include "core/services/storage/flash.h"
#include "core/services/profiles/profile.h"
#include "core/services/profiles/runtime_profile.h"
#include "platform/platform.h"
#include "core/services/players/manager.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// Pad input config (for GPIO device name lookup)
#ifdef CONFIG_PAD_INPUT
#include "pad/pad_input.h"
#include "pad/pad_config_flash.h"
#endif

// CDC input streaming (optional, for web config)
#ifdef CONFIG_USB
#include "usb/usbd/cdc/cdc_commands.h"
#endif

// Device name lookup for USB HID
#ifndef DISABLE_USB_HOST
#include "usb/usbh/hid/hid_registry.h"
#include "tusb.h"
extern int hid_get_ctrl_type(uint8_t dev_addr, uint8_t instance);
extern const char* hid_get_product_name(uint8_t dev_addr);
extern int switch_pro_get_grip_side(uint8_t dev_addr, uint8_t instance);  // 0=L, 1=R, -1=unknown
#endif

// Device name lookup for Bluetooth
#ifdef ENABLE_BTSTACK
#include "bt/bthid/bthid.h"
#endif

// Device name lookup for I2C peer
#ifdef I2C_PEER_ENABLED
#include "i2c_peer/i2c_peer.h"
#endif

// ============================================================================
// AUTO-ASSIGN CONFIGURATION
// ============================================================================

// Log tag for consistent logging
#define LOG_TAG "[ROUTER]"

// Threshold for analog stick movement to trigger player auto-assign
// Value is distance from center (128). Range 0-127.
// 50 means stick must move to < 78 or > 178 to trigger (about 40% deflection)
#define ANALOG_ASSIGN_THRESHOLD 50

// Map a submitted event to the instance value used for player slot lookup.
// Most devices return event->instance unchanged. Joy-Con Charging Grip
// (PID 0x200e) exposes both Joy-Cons as separate HID interfaces of the
// same USB device — both should map to a single shared player slot so
// they don't show up as Player 1 + Player 2.
static inline int8_t player_slot_instance(const input_event_t* event) {
#ifndef DISABLE_USB_HOST
    if (event->transport == INPUT_TRANSPORT_USB && event->instance > 0) {
        int ctrl_type = hid_get_ctrl_type(event->dev_addr, event->instance);
        if (ctrl_type == CONTROLLER_SWITCH) {
            uint16_t vid, pid;
            tuh_vid_pid_get(event->dev_addr, &vid, &pid);
            if (pid == 0x200e) return 0;  // Joy-Con Grip — share root slot
        }
    }
#endif
    return event->instance;
}

// Check if any analog stick is moved beyond threshold
// Returns true if left or right stick is deflected significantly
static inline bool analog_beyond_threshold(const input_event_t* event) {
    // Check left stick X/Y and right stick X/Y (first 4 analog axes)
    for (int i = 0; i < 4; i++) {
        int deflection = (int)event->analog[i] - 128;
        if (deflection < 0) deflection = -deflection;  // abs()
        if (deflection > ANALOG_ASSIGN_THRESHOLD) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// DEVICE NAME LOOKUP
// ============================================================================

// Get device name based on transport type and device address
// Returns pointer to static string or device name buffer
static const char* get_device_name(const input_event_t* event) {
    switch (event->transport) {
#ifndef DISABLE_USB_HOST
        case INPUT_TRANSPORT_USB: {
            uint16_t vid, pid;
            if (tuh_vid_pid_get(event->dev_addr, &vid, &pid) &&
                vid == 0x8086 && pid == 0xc013) {
                return "Intel Wireless Series Gamepad";
            }
            int ctrl_type = hid_get_ctrl_type(event->dev_addr, event->instance);
            if (ctrl_type >= 0 && ctrl_type < CONTROLLER_TYPE_COUNT &&
                device_interfaces[ctrl_type] && device_interfaces[ctrl_type]->name) {
                // Check for Switch 2 variants by PID
                if (ctrl_type == CONTROLLER_SWITCH2) {
                    uint16_t vid, pid;
                    tuh_vid_pid_get(event->dev_addr, &vid, &pid);
                    if (pid == 0x2073) {  // Switch 2 GameCube PID
                        return "Switch 2 GameCube";
                    }
                }
                // Joy-Con Charging Grip: each HID interface is one Joy-Con.
                // Driver detects L/R from raw report stick fields and
                // exposes via switch_pro_get_grip_side().
                if (ctrl_type == CONTROLLER_SWITCH) {
                    uint16_t vid, pid;
                    tuh_vid_pid_get(event->dev_addr, &vid, &pid);
                    if (pid == 0x200e) {
                        int side = switch_pro_get_grip_side(event->dev_addr, event->instance);
                        if (side == 0) return "Joy-Con (L)";
                        if (side == 1) return "Joy-Con (R)";
                        return "Joy-Con";  // Not yet detected
                    }
                }
                return device_interfaces[ctrl_type]->name;
            }
            // Fallback: USB product string (fetched at mount time)
            const char* product = hid_get_product_name(event->dev_addr);
            if (product && product[0]) {
                return product;
            }
            return "USB Device";
        }
#endif
#ifdef ENABLE_BTSTACK
        case INPUT_TRANSPORT_BT_CLASSIC:
        case INPUT_TRANSPORT_BT_BLE: {
            bthid_device_t* bt_dev = bthid_get_device(event->dev_addr);
            if (bt_dev) {
                // Check for Switch 2 variants by PID (Nintendo VID 0x057E)
                if (bt_dev->vendor_id == 0x057E) {
                    if (bt_dev->product_id == 0x2073) {
                        return "Switch 2 GameCube";
                    } else if (bt_dev->product_id == 0x2069) {
                        return "Switch 2 Pro";
                    } else if (bt_dev->product_id == 0x2066) {
                        return "Switch 2 Joy-Con L";
                    } else if (bt_dev->product_id == 0x2067) {
                        return "Switch 2 Joy-Con R";
                    }
                }
                // Prefer the device's actual BT name if available (more specific
                // than the driver name, especially for generic gamepad driver)
                if (bt_dev->name[0]) {
                    return bt_dev->name;
                }
                // Fallback to driver's friendly name
                if (bt_dev->driver) {
                    const bthid_driver_t* driver = (const bthid_driver_t*)bt_dev->driver;
                    if (driver->name) {
                        return driver->name;
                    }
                }
            }
            return "BT Device";
        }
#endif
        case INPUT_TRANSPORT_NATIVE:
            // Resolve a human-readable controller name from the layout hint
            // that the native host set when submitting the event.
            switch (event->layout) {
                case LAYOUT_NINTENDO_4FACE:   return "SNES";
                case LAYOUT_NINTENDO_N64:     return "N64";
                case LAYOUT_GAMECUBE:         return "GameCube";
                case LAYOUT_3DO_3BUTTON:      return "3DO";
                case LAYOUT_SEGA_6BUTTON:     return "Sega 6-Button";
                case LAYOUT_PCE_6BUTTON:      return "PCEngine 6-Button";
                case LAYOUT_ASTROCITY:        return "Astro City";
                case LAYOUT_WII_NUNCHUCK:     return "Wii Nunchuck";
                case LAYOUT_WII_CLASSIC:      return "Wii Classic";
                case LAYOUT_WII_CLASSIC_PRO:  return "Wii Classic Pro";
                case LAYOUT_WII_GUITAR:       return "Wii Guitar";
                case LAYOUT_WII_DRUMS:        return "Wii Drums";
                case LAYOUT_WII_TURNTABLE:    return "DJ Hero Turntable";
                case LAYOUT_WII_TAIKO:        return "Taiko Drum";
                case LAYOUT_WII_UDRAW:        return "uDraw Tablet";
                case LAYOUT_WII_MOTIONPLUS:   return "MotionPlus";
                case LAYOUT_WII_DUAL_NUNCHUCK: return "Dual Nunchuck";
                case LAYOUT_PSX_DIGITAL:      return "PS1 Controller";
                case LAYOUT_PSX_DUALSHOCK:    return "DualShock";
                case LAYOUT_PSX_DUALSHOCK2:   return "DualShock 2";
                case LAYOUT_PSX_NEGCON:       return "neGcon";
                case LAYOUT_PSX_FLIGHTSTICK:  return "Analog Joystick";
                case LAYOUT_PSX_GUNCON:       return "GunCon";
                case LAYOUT_PSX_JOGCON:       return "JogCon";
                case LAYOUT_PSX_MOUSE:        return "PlayStation Mouse";
                default:                      return "Native";
            }
#ifdef I2C_PEER_ENABLED
        case INPUT_TRANSPORT_I2C:
            return i2c_peer_get_device_name();
#endif
#ifdef CONFIG_PAD_INPUT
        case INPUT_TRANSPORT_GPIO: {
            // Return pad config name for custom controller devices
            uint8_t pad_idx = event->dev_addr - 0xF0;
            const pad_device_config_t* cfg = pad_input_get_config(pad_idx);
            if (cfg && cfg->name) return cfg->name;
            // Fallback: check flash config name
            const char* flash_name = pad_config_get_name();
            return flash_name ? flash_name : "Custom Pad";
        }
#endif
        default:
            return "Unknown";
    }
}

// ============================================================================
// OUTPUT STATE (replaces players[] array)
// ============================================================================

// Output state per output type (GameCube, PCEngine, 3DO, etc.)
// Each output has up to MAX_PLAYERS_PER_OUTPUT player slots
static output_state_t router_outputs[MAX_OUTPUTS][MAX_PLAYERS_PER_OUTPUT];

// ---- Cross-core seqlock handoff (see output_state_t doc in router.h) ----------
// Producer (Core 0) publishes a full event into a slot wait-free; consumer
// (Core 1) reads a torn-free snapshot via router_get_output(). Barriers use GCC
// __atomic builtins (no <stdatomic.h>, C++-safe) → DMB on ARM / MEMW on Xtensa.
// Bound the consumer's retries so a pathological collision run can't stall the
// timing-critical console core — it falls back to "no new data" (reuse last frame).
#define ROUTER_CONSUME_MAX_RETRY 4

static inline void router_publish(output_state_t* s, const input_event_t* ev) {
    uint32_t seq = s->seq;
    s->seq = seq + 1;                          // odd: write in progress
    __atomic_thread_fence(__ATOMIC_RELEASE);   // payload writes land after the odd bump
    s->current_state = *ev;
    __atomic_thread_fence(__ATOMIC_RELEASE);   // publish only after payload is committed
    s->seq = seq + 2;                          // even: new version visible
}

// Router configuration (set at init)
static router_config_t router_config;

// Global d-pad mode (0=dpad, 1=left stick, 2=right stick)
static uint8_t global_dpad_mode = 0;
static bool global_shoulder_swap = false;  // swap L1<->L2, R1<->R2

// True after router_init() installs the built-in SELECT+D-pad hotkeys. The first
// app router_set_combo() call clears them so the app's own combo table takes over
// (gc2usb, controller_btusb); apps that register nothing keep the defaults.
static bool router_default_combos_active = false;

// ---- Global on-the-fly runtime profile (SELECT-hold → autofire / live remap) ----
// Wired for every app in router_init(). Apps that call runtime_profile_init()
// themselves (e.g. usb2neogeo) override this because their init runs afterward.
static const uint32_t rt_output_buttons[] = {
    JP_BUTTON_B1, JP_BUTTON_B2, JP_BUTTON_B3, JP_BUTTON_B4,
    JP_BUTTON_L1, JP_BUTTON_R1, JP_BUTTON_L2, JP_BUTTON_R2,
};
static profile_t rt_scratch_profile = {
    .name = "runtime", .description = "Runtime profile",
    .button_map = NULL, .button_map_count = 0,
    .l2_behavior = TRIGGER_PASSTHROUGH, .r2_behavior = TRIGGER_PASSTHROUGH,
    .l2_threshold = 128, .r2_threshold = 128,
    .left_stick_sensitivity = 1.0f, .right_stick_sensitivity = 1.0f,
    .socd_mode = SOCD_UP_PRIORITY,
};
static const runtime_profile_output_config_t rt_output_config = {
    .output_buttons      = rt_output_buttons,
    .output_button_count = sizeof(rt_output_buttons) / sizeof(rt_output_buttons[0]),
    .input_mask          = (JP_BUTTON_B1 | JP_BUTTON_B2 | JP_BUTTON_B3 | JP_BUTTON_B4 |
                            JP_BUTTON_L1 | JP_BUTTON_R1 | JP_BUTTON_L2 | JP_BUTTON_R2 |
                            JP_BUTTON_L3 | JP_BUTTON_R3 | JP_BUTTON_A1 |
                            JP_BUTTON_L4 | JP_BUTTON_R4 | JP_BUTTON_L5 | JP_BUTTON_R5),
    .hold_ms             = 2000,
    .output_button_names = NULL,
    .profile             = &rt_scratch_profile,
};
static runtime_profile_config_t rt_config;  // output_configs[] filled in router_init()

// Global button combo hotkeys
static struct {
    uint32_t input_mask;
    uint32_t output_mask;
    uint8_t required_layout;   // controller_layout_t, 0 = any layout
    bool fired;
    uint32_t held_since;       // ms timestamp the combo became fully held (0 = not held)
    uint32_t held_dev;         // device that owns the in-progress hold (0 = none)
} router_combos[ROUTER_COMBO_MAX];

// Identity of the device an event came from, for combo hold ownership. Raw
// dev_addr/instance rather than the shared player slot: a Joy-Con Grip's two
// interfaces are one player but only one of them is pressing the combo, so the
// other half's traffic must not count as the holder. Never 0 — 0 means "none".
static inline uint32_t combo_dev_key(const input_event_t* event) {
    return 0x10000u | ((uint32_t)event->dev_addr << 8) | (uint8_t)event->instance;
}

// Built-in default combos (SELECT/START + D-pad) require a brief deliberate hold
// before they fire AND before they consume the buttons, so a quick in-game
// SELECT/START + D-pad passes through to the console instead of silently
// switching a profile on the 40+ apps that inherit the defaults. App-registered
// combo tables (router_default_combos_active == false) keep instant behavior.
#define ROUTER_DEFAULT_COMBO_HOLD_MS 700

// Active output count (for broadcast mode)
static output_target_t active_outputs[MAX_OUTPUTS];
static uint8_t active_output_count = 0;

// ============================================================================
// TRANSFORMATION STATE (Phase 5)
// ============================================================================

// Mouse-to-analog accumulators (per output, per player)
static mouse_accumulator_t mouse_accumulators[MAX_OUTPUTS][MAX_PLAYERS_PER_OUTPUT];

// Instance merging state (per output, per player)
static instance_merge_t instance_merges[MAX_OUTPUTS][MAX_PLAYERS_PER_OUTPUT];

// ============================================================================
// MERGE_BLEND STATE - Per-device input tracking for proper blending
// ============================================================================

#ifndef MAX_BLEND_DEVICES
#define MAX_BLEND_DEVICES 8  // Max devices to track for blending (overridable for low-RAM targets)
#endif

typedef struct {
    uint8_t dev_addr;
    int8_t instance;
    bool active;
    input_event_t state;
} blend_device_state_t;

// Per-output blend state (tracks each device's contribution)
static blend_device_state_t blend_devices[MAX_OUTPUTS][MAX_BLEND_DEVICES];

// ============================================================================
// ROUTING TABLE (Phase 6)
// ============================================================================

// Routing table for N:M input-to-output mapping
static route_entry_t routing_table[MAX_ROUTES];
static uint8_t route_count = 0;

// ============================================================================
// OUTPUT TAPS (Push-based notification)
// ============================================================================

static router_tap_callback_t output_taps[MAX_OUTPUTS] = {NULL};
static bool output_tap_exclusive[MAX_OUTPUTS] = {false};

// Onboard battery: this device's OWN battery (e.g. controller_btusb on a board
// with a LiPo), as opposed to a battery reported by a connected input
// controller. The app updates it from an ADC read; the router stamps it into
// output states that have no input-device battery, so the SInput report's
// charge_level/plug_status reflect the device itself. percent <0 = none.
static volatile int onboard_batt_pct = -1;
static volatile bool onboard_batt_charging = false;

void router_set_onboard_battery(int percent, bool charging) {
    onboard_batt_pct = percent;
    onboard_batt_charging = charging;
}
int router_onboard_battery_percent(void) { return onboard_batt_pct; }
bool router_onboard_battery_charging(void) { return onboard_batt_charging; }

// Look up a routed input device's last-reported battery by dev_addr. Returns
// true and fills level (0-100) / charging if any output slot has a battery
// reading for that device. level 0 = not reported.
bool router_get_device_battery(uint8_t dev_addr, uint8_t* level, bool* charging) {
    for (int output = 0; output < MAX_OUTPUTS; output++) {
        for (int i = 0; i < MAX_BLEND_DEVICES; i++) {
            if (blend_devices[output][i].active &&
                blend_devices[output][i].dev_addr == dev_addr &&
                blend_devices[output][i].state.battery_level > 0) {
                if (level) *level = blend_devices[output][i].state.battery_level;
                if (charging) *charging = blend_devices[output][i].state.battery_charging;
                return true;
            }
        }
    }
    return false;
}

// Onboard motion: this device's OWN IMU (e.g. controller_btusb on a XIAO Sense),
// as opposed to motion from a connected input controller (DS4/DS5). The app
// updates it from the IMU; the router stamps it into output states that have no
// input-device motion, so the SInput report carries accel/gyro. valid=false
// when there's no IMU.
static struct {
    int16_t accel[3];
    int16_t gyro[3];
    uint16_t accel_range;
    uint16_t gyro_range;
    volatile bool valid;
} onboard_motion = { .accel_range = 4000, .gyro_range = 2000 };

// Onboard IMU axis remap (mounting orientation). For each output axis i,
// out[i] = sign(motion_remap[i]) * in[ |motion_remap[i]|-1 ]. Default {1,2,3}
// is identity. Applied to accel and gyro together (same physical chip axes), so
// a rotated/flipped IMU mount is corrected once, at the source, for every
// consumer (SInput report, Steam/SDL, joypad-web). Set at runtime via the CDC
// "IMU.MAP" command while tuning; bake the final values in as the default.
static int8_t motion_remap[3] = { 1, 2, 3 };

void router_set_motion_remap(int x, int y, int z) {
    if (x >= -3 && x <= 3 && x != 0) motion_remap[0] = (int8_t)x;
    if (y >= -3 && y <= 3 && y != 0) motion_remap[1] = (int8_t)y;
    if (z >= -3 && z <= 3 && z != 0) motion_remap[2] = (int8_t)z;
}

void router_get_motion_remap(int out[3]) {
    for (int i = 0; i < 3; i++) out[i] = motion_remap[i];
}

static inline int16_t remap_one(const int16_t v[3], int8_t m) {
    int idx = (m < 0 ? -m : m) - 1;
    int16_t val = v[idx];
    if (val == INT16_MIN) val = INT16_MIN + 1;  // avoid negate overflow
    return m < 0 ? (int16_t)(-val) : val;
}

void router_set_onboard_motion(const int16_t accel[3], const int16_t gyro[3],
                               uint16_t accel_range, uint16_t gyro_range) {
    for (int i = 0; i < 3; i++) {
        onboard_motion.accel[i] = remap_one(accel, motion_remap[i]);
        onboard_motion.gyro[i]  = remap_one(gyro,  motion_remap[i]);
    }
    onboard_motion.accel_range = accel_range;
    onboard_motion.gyro_range = gyro_range;
    onboard_motion.valid = true;
}

bool router_onboard_motion_get(int16_t accel[3], int16_t gyro[3]) {
    if (!onboard_motion.valid) return false;
    for (int i = 0; i < 3; i++) {
        accel[i] = onboard_motion.accel[i];
        gyro[i] = onboard_motion.gyro[i];
    }
    return true;
}

// ============================================================================
// INITIALIZATION
// ============================================================================

// Built-in hotkeys so profile-switch / d-pad-mode / shoulder-swap work on every
// app out of the box (previously only gc2usb/controller_btusb registered combos).
// All require a ~0.7 s deliberate hold (ROUTER_DEFAULT_COMBO_HOLD_MS) before they fire and
// before they consume the buttons — see the note above the constant. App-registered tables are
// instant.
//   SELECT + Up/Down     → profile prev/next   (clamps at the ends, no wrap)
//   SELECT + Left/Right  → d-pad mode slider [left stick <- d-pad -> right stick]
//   START  + Up          → shoulder swap toggle (L1<->L2, R1<->R2)
// Choices persist via flash (profile index / dpad mode / shoulder swap) and are
// restored on boot. Any app that registers its own combos wipes these on its
// first router_set_combo() call (see router_set_combo).
static void router_install_default_combos(void) {
    router_set_combo(0, JP_BUTTON_S1 | JP_BUTTON_DU, (10u << 24)); // profile prev (clamp)
    router_set_combo(1, JP_BUTTON_S1 | JP_BUTTON_DD, (11u << 24)); // profile next (clamp)
    router_set_combo(2, JP_BUTTON_S1 | JP_BUTTON_DL, (8u  << 24)); // d-pad slider ← left stick
    router_set_combo(3, JP_BUTTON_S1 | JP_BUTTON_DR, (9u  << 24)); // d-pad slider → right stick
    router_set_combo(4, JP_BUTTON_S2 | JP_BUTTON_DU, (7u  << 24)); // shoulder swap toggle
    router_default_combos_active = true;
}

void router_init(const router_config_t* config) {
    if (!config) {
        printf(LOG_TAG "ERROR: NULL config\n");
        return;
    }

    // Copy configuration
    router_config = *config;

    // Restore persisted router settings (d-pad mode + shoulder swap) so a saved
    // selection survives a reboot on EVERY app, not just the few that restore
    // themselves. Gated on router_saved so untouched flash is left at defaults.
    flash_t flash_data;
    if (flash_load(&flash_data) && flash_data.router_saved) {
        if (flash_data.dpad_mode <= 3) router_set_dpad_mode(flash_data.dpad_mode);
        router_set_shoulder_swap(flash_data.shoulder_swap != 0);
    }

    // Install the built-in SELECT+D-pad hotkeys. Apps that register their own
    // combos (gc2usb, controller_btusb) clear these on first router_set_combo().
    router_install_default_combos();

    // Register the global on-the-fly runtime profile for every output target so
    // the SELECT-hold autofire/remap gesture works on every app. Apps that call
    // runtime_profile_init() themselves override this (their init runs later).
    for (int t = 0; t < MAX_OUTPUT_TARGETS; t++) {
        rt_config.output_configs[t] = &rt_output_config;
    }
    runtime_profile_init(&rt_config);

    printf(LOG_TAG "Initializing router\n");
    printf(LOG_TAG "  Mode: %s\n",
        config->mode == ROUTING_MODE_SIMPLE ? "SIMPLE" :
        config->mode == ROUTING_MODE_MERGE ? "MERGE" :
        config->mode == ROUTING_MODE_BROADCAST ? "BROADCAST" : "CONFIGURABLE");

    if (config->mode == ROUTING_MODE_MERGE) {
        printf(LOG_TAG "  Merge mode: %s\n",
            config->merge_mode == MERGE_PRIORITY ? "PRIORITY" :
            config->merge_mode == MERGE_BLEND ? "BLEND" : "ALL");
        printf(LOG_TAG "  Merge all inputs: %s\n", config->merge_all_inputs ? "YES" : "NO");
    }

    // Initialize output states
    for (uint8_t output = 0; output < MAX_OUTPUTS; output++) {
        for (uint8_t player = 0; player < MAX_PLAYERS_PER_OUTPUT; player++) {
            init_input_event(&router_outputs[output][player].current_state);
            router_outputs[output][player].seq = 0;   // even, "no data yet"
            router_outputs[output][player].player_id = player;
            router_outputs[output][player].source = INPUT_SOURCE_USB_HOST;  // Default

            // Initialize transformation state
            mouse_accumulators[output][player].accum_x = 0;
            mouse_accumulators[output][player].accum_y = 0;
            mouse_accumulators[output][player].drain_rate = config->mouse_drain_rate;
            mouse_accumulators[output][player].target_x = config->mouse_target_x;
            mouse_accumulators[output][player].target_y = config->mouse_target_y;

            instance_merges[output][player].active = false;
            instance_merges[output][player].instance_count = 0;
            instance_merges[output][player].root_instance = 0;
        }

        // Initialize blend device tracking
        for (uint8_t i = 0; i < MAX_BLEND_DEVICES; i++) {
            blend_devices[output][i].active = false;
            blend_devices[output][i].dev_addr = 0;
            blend_devices[output][i].instance = -1;
            init_input_event(&blend_devices[output][i].state);
        }
    }

    // Initialize routing table
    router_clear_routes();

    printf(LOG_TAG "Initialized successfully\n");
    if (config->transform_flags) {
        printf(LOG_TAG "  Transformations enabled: 0x%02x\n", config->transform_flags);
        if (config->transform_flags & TRANSFORM_MOUSE_TO_ANALOG) {
            printf(LOG_TAG "    - Mouse-to-analog (target_x=%d, target_y=%d, drain=%d)\n",
                   config->mouse_target_x, config->mouse_target_y, config->mouse_drain_rate);
        }
        if (config->transform_flags & TRANSFORM_MERGE_INSTANCES)
            printf(LOG_TAG "    - Instance merging\n");
        if (config->transform_flags & TRANSFORM_SPINNER)
            printf(LOG_TAG "    - Spinner accumulation\n");
    }
}

// ============================================================================
// INPUT TRANSFORMATIONS (Phase 5)
// ============================================================================

// Mouse-to-analog: Accumulate mouse deltas into analog stick positions
// Configurable target axes and drain behavior for different use cases:
// - Left stick (default): mouse controls movement
// - Right stick: mouse controls camera (e.g., mouthpad for accessibility)
// - drain_rate=0: hold position until input returns to center (no auto-drain)
static void transform_mouse_to_analog(input_event_t* event, output_target_t output, int player_index) {
    if (event->type != INPUT_TYPE_MOUSE) return;
    if (player_index < 0 || player_index >= MAX_PLAYERS_PER_OUTPUT) return;

    mouse_accumulator_t* accum = &mouse_accumulators[output][player_index];

    // Accumulate X-axis if enabled
    if (accum->target_x != MOUSE_AXIS_DISABLED) {
        // delta_x is already signed (int16); accumulate directly.
        accum->accum_x += event->delta_x;

        // Clamp to [-127, 127]
        if (accum->accum_x > 127) accum->accum_x = 127;
        if (accum->accum_x < -127) accum->accum_x = -127;

        // Convert to analog position (centered at 128)
        event->analog[accum->target_x] = 128 + accum->accum_x;

        // Drain toward center (only if drain_rate > 0)
        if (accum->drain_rate > 0) {
            if (accum->accum_x > 0) {
                accum->accum_x -= (accum->accum_x > accum->drain_rate) ? accum->drain_rate : accum->accum_x;
            } else if (accum->accum_x < 0) {
                accum->accum_x += (-accum->accum_x > accum->drain_rate) ? accum->drain_rate : -accum->accum_x;
            }
        }
    }

    // Accumulate Y-axis if enabled
    if (accum->target_y != MOUSE_AXIS_DISABLED) {
        // delta_y is already signed (int16); accumulate directly.
        accum->accum_y += event->delta_y;

        // Clamp to [-127, 127]
        if (accum->accum_y > 127) accum->accum_y = 127;
        if (accum->accum_y < -127) accum->accum_y = -127;

        // Convert to analog position (centered at 128)
        event->analog[accum->target_y] = 128 + accum->accum_y;

        // Drain toward center (only if drain_rate > 0)
        if (accum->drain_rate > 0) {
            if (accum->accum_y > 0) {
                accum->accum_y -= (accum->accum_y > accum->drain_rate) ? accum->drain_rate : accum->accum_y;
            } else if (accum->accum_y < 0) {
                accum->accum_y += (-accum->accum_y > accum->drain_rate) ? accum->drain_rate : -accum->accum_y;
            }
        }
    }

    // Clear delta fields (analog values now set)
    event->delta_x = 0;
    event->delta_y = 0;
}

// Instance merging: Merge multi-instance devices (Joy-Con Grip, etc.)
// TODO Phase 5: Implement Joy-Con Grip merging
static void transform_merge_instances(input_event_t* event, output_target_t output, int player_index) {
    if (player_index < 0 || player_index >= MAX_PLAYERS_PER_OUTPUT) return;

    // TODO: Detect multi-instance devices (instance == -1 flag from device driver)
    // TODO: Merge button states and analog inputs from both instances
    // TODO: Present as single unified controller

    (void)event;  // Suppress unused warning for now
    (void)output;
}

// Apply transformations to input event (modifies event in-place)
static void apply_transformations(input_event_t* event, output_target_t output, int player_index) {
    if (!router_config.transform_flags) return;  // No transformations enabled

    // Apply mouse-to-analog transformation
    if (router_config.transform_flags & TRANSFORM_MOUSE_TO_ANALOG) {
        transform_mouse_to_analog(event, output, player_index);
    }

    // Apply instance merging
    if (router_config.transform_flags & TRANSFORM_MERGE_INSTANCES) {
        transform_merge_instances(event, output, player_index);
    }

    // TODO: TRANSFORM_SPINNER (Nuon spinner accumulation)
}

// ============================================================================
// ROUTING TABLE MANAGEMENT (Phase 6)
// ============================================================================

// Add simple route (input → output)
bool router_add_route(input_source_t input, output_target_t output, uint8_t priority) {
    if (route_count >= MAX_ROUTES) {
        printf(LOG_TAG "ERROR: Routing table full (%d routes)\n", MAX_ROUTES);
        return false;
    }

    routing_table[route_count].input = input;
    routing_table[route_count].output = output;
    routing_table[route_count].priority = priority;
    routing_table[route_count].active = true;
    routing_table[route_count].input_dev_addr = 0;      // Wildcard
    routing_table[route_count].input_instance = -1;     // Wildcard
    routing_table[route_count].output_player_id = 0xFF; // Auto-assign

    route_count++;
    printf(LOG_TAG "Route added: %s → %s (priority=%d)\n",
        input == INPUT_SOURCE_USB_HOST ? "USB" : "?",
        output == OUTPUT_TARGET_GAMECUBE ? "GameCube" :
        output == OUTPUT_TARGET_PCENGINE ? "PCEngine" :
        output == OUTPUT_TARGET_NUON ? "Nuon" :
        output == OUTPUT_TARGET_XBOXONE ? "XboxOne" :
        output == OUTPUT_TARGET_GPIO ? "GPIO" :
        output == OUTPUT_TARGET_LOOPY ? "Loopy" : "?",
        priority);

    return true;
}

// Add route with filters (advanced)
bool router_add_route_filtered(const route_entry_t* route) {
    if (!route || route_count >= MAX_ROUTES) {
        printf(LOG_TAG "ERROR: Cannot add filtered route\n");
        return false;
    }

    routing_table[route_count] = *route;
    routing_table[route_count].active = true;
    route_count++;

    printf(LOG_TAG "Filtered route added (dev_addr=%d, instance=%d, player=%d)\n",
        route->input_dev_addr, route->input_instance, route->output_player_id);

    return true;
}

// Remove route by index
void router_remove_route(uint8_t route_index) {
    if (route_index >= MAX_ROUTES || !routing_table[route_index].active) return;

    routing_table[route_index].active = false;
    printf(LOG_TAG "Route %d removed\n", route_index);
}

// Clear all routes
void router_clear_routes(void) {
    for (uint8_t i = 0; i < MAX_ROUTES; i++) {
        routing_table[i].active = false;
    }
    route_count = 0;
    printf(LOG_TAG "All routes cleared\n");
}

// Get number of active routes
uint8_t router_get_route_count(void) {
    uint8_t count = 0;
    for (uint8_t i = 0; i < MAX_ROUTES; i++) {
        if (routing_table[i].active) count++;
    }
    return count;
}

// Get route by index
const route_entry_t* router_get_route(uint8_t route_index) {
    if (route_index >= MAX_ROUTES || !routing_table[route_index].active) {
        return NULL;
    }
    return &routing_table[route_index];
}

// Find matching routes for an input event
// Returns number of matches found (fills matches array)
static uint8_t router_find_routes(const input_event_t* event, route_entry_t* matches, uint8_t max_matches) {
    uint8_t match_count = 0;

    for (uint8_t i = 0; i < MAX_ROUTES && match_count < max_matches; i++) {
        if (!routing_table[i].active) continue;

        // Check if input source matches (wildcard = INPUT_SOURCE_USB_HOST)
        if (routing_table[i].input != INPUT_SOURCE_USB_HOST) {
            // TODO: Add other input sources (BLE, GPIO, etc.)
            continue;
        }

        // Check device address filter (0 = wildcard)
        if (routing_table[i].input_dev_addr != 0 &&
            routing_table[i].input_dev_addr != event->dev_addr) {
            continue;
        }

        // Check instance filter (-1 = wildcard)
        if (routing_table[i].input_instance != -1 &&
            routing_table[i].input_instance != event->instance) {
            continue;
        }

        // Match found!
        matches[match_count++] = routing_table[i];
    }

    return match_count;
}

// ============================================================================
// INPUT SUBMISSION (Core 0 - Event Driven)
// ============================================================================

// SIMPLE MODE: Direct 1:1 pass-through (zero overhead, can be inlined)
static inline void router_simple_mode(const input_event_t* event, output_target_t output) {
    // Find or add player (multi-instance devices like Joy-Con Grip share one slot)
    int8_t slot_inst = player_slot_instance(event);
    int player_index = find_player_index(event->dev_addr, slot_inst);

    if (player_index < 0) {
        // Check if any button pressed or analog stick moved beyond threshold.
        // Native and GPIO devices are physically attached — register as soon
        // as they submit any event, without waiting for input activity, so
        // the web-config player list reflects them immediately on connect.
        uint32_t buttons_pressed = event->buttons | event->keys;
        bool analog_active = analog_beyond_threshold(event);
        bool physically_attached = (event->transport == INPUT_TRANSPORT_NATIVE ||
                                    event->transport == INPUT_TRANSPORT_GPIO);
        if (buttons_pressed || analog_active || physically_attached) {
            const char* device_name = get_device_name(event);
            player_index = add_player(event->dev_addr, slot_inst, event->transport, device_name);
            if (player_index >= 0) {
                printf(LOG_TAG "Player %d assigned: %s (dev_addr=%d, instance=%d)\n",
                    player_index + 1, device_name, event->dev_addr, slot_inst);
            }
        }
    }

    if (player_index >= 0 && player_index < router_config.max_players_per_output[output]) {
        // Avoid struct copy when no transformations are active (common case)
        const input_event_t* final_event;
        input_event_t transformed;

        if (router_config.transform_flags) {
            transformed = *event;
            apply_transformations(&transformed, output, player_index);
            final_event = &transformed;
        } else {
            final_event = event;  // Zero-copy pass-through
        }

        // Store to output slot (skip when tap-exclusive — tap delivers directly)
        if (!output_tap_exclusive[output]) {
            router_outputs[output][player_index].source = INPUT_SOURCE_USB_HOST;
            router_publish(&router_outputs[output][player_index], final_event);
        }

        // Notify tap if registered (for push-based outputs like UART)
        if (output_taps[output]) {
            output_taps[output](output, player_index, final_event);
        }
    }
}


// MERGE MODE: Multiple inputs → single output
static inline void router_merge_mode(const input_event_t* event, output_target_t output) {
    // Register player if not already registered (for LED and rumble support).
    // Multi-instance devices (Joy-Con Grip) share one slot via player_slot_instance.
    int8_t slot_inst = player_slot_instance(event);
    int player_index = find_player_index(event->dev_addr, slot_inst);
    if (player_index < 0) {
        uint32_t buttons_pressed = event->buttons | event->keys;
        bool analog_active = analog_beyond_threshold(event);
        if (buttons_pressed || analog_active || event->type == INPUT_TYPE_MOUSE) {
            const char* device_name = get_device_name(event);
            player_index = add_player(event->dev_addr, slot_inst, event->transport, device_name);
            if (player_index >= 0) {
                printf(LOG_TAG "Player %d assigned in merge mode: %s (dev_addr=%d, instance=%d)\n",
                    player_index + 1, device_name, event->dev_addr, slot_inst);
            }
        }
    }

    // Only process if player is registered
    if (player_index < 0) return;

    // Avoid struct copy when no transformations are active
    const input_event_t* final_event;
    input_event_t transformed;

    if (router_config.transform_flags) {
        transformed = *event;
        apply_transformations(&transformed, output, 0);
        final_event = &transformed;
    } else {
        final_event = event;  // Zero-copy pass-through
    }

    // Build the merged result in a local, then publish once (seqlock) so Core 1
    // never sees a torn frame mid-blend. See output_state_t doc in router.h.
    input_event_t merged;

    switch (router_config.merge_mode) {
        case MERGE_ALL:
            // Latest active input wins (overwrites previous state)
            merged = *final_event;
            break;

        case MERGE_BLEND: {
            // Blend button states together from ALL active devices
            // 1. Update this device's state in blend_devices[]
            // 2. Re-blend all active devices into output

            // Find or create slot for this device
            int slot = -1;
            for (int i = 0; i < MAX_BLEND_DEVICES; i++) {
                if (blend_devices[output][i].active &&
                    blend_devices[output][i].dev_addr == final_event->dev_addr &&
                    blend_devices[output][i].instance == final_event->instance) {
                    slot = i;
                    break;
                }
            }
            if (slot < 0) {
                // Find empty slot
                for (int i = 0; i < MAX_BLEND_DEVICES; i++) {
                    if (!blend_devices[output][i].active) {
                        slot = i;
                        blend_devices[output][i].active = true;
                        blend_devices[output][i].dev_addr = final_event->dev_addr;
                        blend_devices[output][i].instance = final_event->instance;
                        break;
                    }
                }
            }

            if (slot >= 0) {
                // Update this device's state
                blend_devices[output][slot].state = *final_event;

                // Now re-blend ALL active devices into a local, published below

                // Start with neutral state (all buttons released)
                // Note: deltas are cleared here but accumulated fresh from blend devices
                input_event_t x_current_state;
                init_input_event(&x_current_state);

                // Blend all active devices
                bool first = true;
                for (int i = 0; i < MAX_BLEND_DEVICES; i++) {
                    if (!blend_devices[output][i].active) continue;

                    input_event_t* dev = &blend_devices[output][i].state;

                    // Buttons: OR together (active-high, 1 = pressed)
                    x_current_state.buttons |= dev->buttons;

                    // Keys: OR together (active-high)
                    x_current_state.keys |= dev->keys;

                    // Aux buttons (e.g. Jaguar keypad): OR together. The blend
                    // path builds a fresh neutral event, so this field must be
                    // merged explicitly like buttons/keys or it would be lost.
                    x_current_state.aux_buttons |= dev->aux_buttons;

                    // Present-as-gamepad flag (e.g. MouthPad): if ANY blended
                    // device wants gamepad output, the merged event does too.
                    // Without this, blend mode drops the flag and sinput never
                    // emits the gamepad report.
                    x_current_state.as_gamepad |= dev->as_gamepad;

                    // Raw HID keyboard state. The `keys` field above is the
                    // lossy gamepad-mapped encoding; output paths that need real
                    // keystrokes (sinput_mode's composite keyboard interface)
                    // read kb_modifier/kb_keys instead, and those were being
                    // dropped here. Modifier is a bitmask, so OR it like buttons;
                    // keycodes are a 6-slot rollover set, so append each distinct
                    // non-zero key until the slots are full.
                    x_current_state.kb_modifier |= dev->kb_modifier;
                    for (int j = 0; j < 6 && dev->kb_keys[j] != 0; j++) {
                        uint8_t k = dev->kb_keys[j];
                        bool dup = false;
                        int free_slot = -1;
                        for (int s = 0; s < 6; s++) {
                            if (x_current_state.kb_keys[s] == k) { dup = true; break; }
                            if (x_current_state.kb_keys[s] == 0) { free_slot = s; break; }
                        }
                        if (!dup && free_slot >= 0) {
                            x_current_state.kb_keys[free_slot] = k;
                        }
                    }

                    // Consumer control (media/volume) is a single usage selector,
                    // not a bitmask — it cannot be OR'd. Use the first device
                    // reporting one, mirroring the motion/pressure/touch rule.
                    if (dev->consumer_usage != 0 && x_current_state.consumer_usage == 0) {
                        x_current_state.consumer_usage = dev->consumer_usage;
                    }

                    // Chatpad (Xbox 360): first device that has one.
                    if (dev->has_chatpad && !x_current_state.has_chatpad) {
                        x_current_state.has_chatpad = true;
                        x_current_state.chatpad[0] = dev->chatpad[0];
                        x_current_state.chatpad[1] = dev->chatpad[1];
                        x_current_state.chatpad[2] = dev->chatpad[2];
                    }

                    // Analog: use furthest from center for sticks, max for triggers
                    // New format: [0]=LX, [1]=LY, [2]=RX, [3]=RY, [4]=L2, [5]=R2
                    for (int j = 0; j < ANALOG_COUNT; j++) {
                        if (j >= ANALOG_L2) {
                            // Triggers (L2, R2): use max value
                            if (dev->analog[j] > x_current_state.analog[j]) {
                                x_current_state.analog[j] = dev->analog[j];
                            }
                        } else {
                            // Sticks (LX, LY, RX, RY): use furthest from center
                            int8_t cur_delta = (int8_t)(x_current_state.analog[j] - 128);
                            int8_t dev_delta = (int8_t)(dev->analog[j] - 128);
                            if (abs(dev_delta) > abs(cur_delta)) {
                                x_current_state.analog[j] = dev->analog[j];
                            }
                        }
                    }

                    // Mouse deltas: accumulate from all, then clear device to prevent re-adding
                    x_current_state.delta_x += dev->delta_x;
                    x_current_state.delta_y += dev->delta_y;
                    dev->delta_x = 0;
                    dev->delta_y = 0;

                    // Scroll wheel is one-shot per event like delta_x/delta_y and
                    // was the one relative axis this loop did not merge, so every
                    // consumer (sinput mouse report, BLE mouse, Amiga wheel
                    // accumulator) saw a permanent zero in blend mode. Clamped
                    // because the field is int8_t, unlike the int16 deltas above.
                    {
                        int wheel = (int)x_current_state.delta_wheel + (int)dev->delta_wheel;
                        if (wheel >  127) wheel =  127;
                        if (wheel < -128) wheel = -128;
                        x_current_state.delta_wheel = (int8_t)wheel;
                    }
                    dev->delta_wheel = 0;

                    // Motion: use first device that has motion data
                    if (dev->has_motion && !x_current_state.has_motion) {
                        x_current_state.has_motion = true;
                        x_current_state.accel[0] = dev->accel[0];
                        x_current_state.accel[1] = dev->accel[1];
                        x_current_state.accel[2] = dev->accel[2];
                        x_current_state.gyro[0] = dev->gyro[0];
                        x_current_state.gyro[1] = dev->gyro[1];
                        x_current_state.gyro[2] = dev->gyro[2];
                    }

                    // Pressure: use first device that has pressure data
                    if (dev->has_pressure && !x_current_state.has_pressure) {
                        x_current_state.has_pressure = true;
                        for (int j = 0; j < 12; j++) {
                            x_current_state.pressure[j] = dev->pressure[j];
                        }
                    }

                    // Touch: use first device that has touch data
                    if (dev->has_touch && !x_current_state.has_touch) {
                        x_current_state.has_touch = true;
                        x_current_state.touch[0] = dev->touch[0];
                        x_current_state.touch[1] = dev->touch[1];
                    }

                    // Battery: use first device that reports battery
                    if (dev->battery_level > 0 && x_current_state.battery_level == 0) {
                        x_current_state.battery_level = dev->battery_level;
                        x_current_state.battery_charging = dev->battery_charging;
                    }

                    // Use metadata from first active device.
                    // transport/layout belong here too: sinput_mode.c reads both
                    // off the merged event (update_device_info + cached_layout)
                    // and INPUT_TRANSPORT_NONE / LAYOUT_UNKNOWN are both 0, so
                    // omitting them didn't leave the value stale — it reported a
                    // defined "unknown" for a device the router knew exactly.
                    if (first) {
                        x_current_state.dev_addr = dev->dev_addr;
                        x_current_state.instance = dev->instance;
                        x_current_state.type = dev->type;
                        x_current_state.transport = dev->transport;
                        x_current_state.layout = dev->layout;
                        x_current_state.button_count = dev->button_count;
                        x_current_state.has_rumble = dev->has_rumble;
                        x_current_state.has_force_feedback = dev->has_force_feedback;
                        first = false;
                    }
                }
                // Onboard battery fallback (see SIMPLE path) — no input device
                // reported a battery, so use this device's own.
                if (x_current_state.battery_level == 0 && onboard_batt_pct >= 0) {
                    x_current_state.battery_level = (uint8_t)onboard_batt_pct;
                    x_current_state.battery_charging = onboard_batt_charging;
                }
                if (!x_current_state.has_motion && onboard_motion.valid) {
                    for (int mi = 0; mi < 3; mi++) {
                        x_current_state.accel[mi] = onboard_motion.accel[mi];
                        x_current_state.gyro[mi] = onboard_motion.gyro[mi];
                    }
                    x_current_state.accel_range = onboard_motion.accel_range;
                    x_current_state.gyro_range = onboard_motion.gyro_range;
                    x_current_state.has_motion = true;
                }
                merged = x_current_state;
            }
            break;
        }

        case MERGE_PRIORITY:
            // High priority input wins, low priority fallback
            // Used by Super3D0USB (USB priority, SNES fallback)
            // Start from the current published state; USB (highest priority)
            // overwrites it. Reading the slot here is producer-core-safe.
            merged = router_outputs[output][0].current_state;
            if (router_outputs[output][0].source <= INPUT_SOURCE_USB_HOST) {
                // USB has highest priority (0), always wins
                merged = *final_event;
            }
            // Lower priority sources only update if no USB input active
            // TODO: Track activity timeout for priority fallback
            break;
    }

    router_outputs[output][0].source = INPUT_SOURCE_USB_HOST;
    router_publish(&router_outputs[output][0], &merged);

    // Notify tap if registered (for push-based outputs like UART)
    if (output_taps[output]) {
        output_taps[output](output, 0, &merged);
    }
}

// Main input submission function (called by input drivers)
// Host-side synthetic button overlay (INPUT.INJECT). OR'd into every real
// input event before profile/overlay processing. RAM only, never persisted.
static uint32_t s_inject_buttons = 0;
// Injected analog, and whether any is being injected at all. The flag is not
// redundant with an all-neutral array: neutral is a real instruction to hold the
// sticks centred, and must still override a real stick that is drifting.
static uint8_t s_inject_analog[ANALOG_COUNT];
static bool s_inject_analog_set = false;
// Last time a real controller submitted anything, so the heartbeat below only
// runs when there is nothing else carrying the injection.
static uint32_t s_last_real_input_ms = 0;
static uint32_t s_inject_last_beat_ms = 0;

// Resting position per axis. Sticks sit at centre and triggers at zero, so
// "further from rest" has to be measured from a different place for each — from
// centre, a released trigger looks like a large deflection and would beat a real
// pull every time.
static uint8_t inject_axis_rest(uint8_t index) {
    return (index == ANALOG_L2 || index == ANALOG_R2) ? 0 : 128;
}

void router_set_inject_buttons(uint32_t buttons) {
    s_inject_buttons = buttons;
}

uint32_t router_get_inject_buttons(void) {
    return s_inject_buttons;
}

void router_set_inject_analog(const uint8_t* analog) {
    if (!analog) {
        s_inject_analog_set = false;
        return;
    }
    memcpy(s_inject_analog, analog, ANALOG_COUNT);
    s_inject_analog_set = true;
}

bool router_get_inject_analog(uint8_t* out) {
    if (!s_inject_analog_set) return false;
    if (out) memcpy(out, s_inject_analog, ANALOG_COUNT);
    return true;
}

// Take whichever of the two sits further from the axis's resting position.
static void inject_merge_analog(input_event_t* event) {
    for (uint8_t i = 0; i < ANALOG_COUNT; i++) {
        int rest = inject_axis_rest(i);
        int real = (int)event->analog[i] - rest;
        int injected = (int)s_inject_analog[i] - rest;
        if (abs(injected) > abs(real)) event->analog[i] = s_inject_analog[i];
    }
}

void router_inject_task(void) {
    // needs_flush: we last published an injected frame that still needs one more
    // beat to clear once inject goes empty. Without this, releasing the last
    // injected button leaves it stuck in the output — nothing re-publishes the
    // cleared state when no real controller is actively feeding events.
    static bool needs_flush = false;
    bool active = s_inject_buttons || s_inject_analog_set;
    if (!active && !needs_flush) return;

    uint32_t now = platform_time_ms();
    // A real controller's events already carry the injection (and clear it on
    // release); a second source would press everything twice, so stand down —
    // the real events also flush any residual inject.
    if ((uint32_t)(now - s_last_real_input_ms) < 100) { needs_flush = false; return; }
    // 60Hz is what the outputs consume; faster only adds work.
    if ((uint32_t)(now - s_inject_last_beat_ms) < 16) return;
    s_inject_last_beat_ms = now;

    input_event_t event;
    memset(&event, 0, sizeof(event));
    event.dev_addr = ROUTER_INJECT_ADDR;
    event.instance = 0;
    event.type = INPUT_TYPE_GAMEPAD;
    event.transport = INPUT_TRANSPORT_NATIVE;
    for (uint8_t i = 0; i < ANALOG_COUNT; i++) event.analog[i] = inject_axis_rest(i);
    // router_submit_input OR-s in the current s_inject_buttons (0 while clearing).
    router_submit_input(&event);

    // While inject is active, keep the flush pending; once it's empty this beat
    // just published the neutral/cleared frame, so we're done.
    needs_flush = active;
}

// Timestamp of the last "active" input across all sources, for idle/sleep
// detection. Active = any button held or a stick pushed past a noise margin.
static volatile uint32_t s_last_activity_ms = 0;

uint32_t router_ms_since_activity(void) {
    return platform_time_ms() - s_last_activity_ms;
}

void router_submit_input(const input_event_t* event) {
    if (!event) return;
    if (route_count == 0) return;

    // Note real input so the injection heartbeat stands down while a controller
    // is attached — its events already carry the injected state.
    if (event->dev_addr != ROUTER_INJECT_ADDR) s_last_real_input_ms = platform_time_ms();

    // Global on-the-fly runtime profile gesture (SELECT-hold → autofire / live
    // remap). Detected on the raw input; the mapping it builds is applied by
    // profile_get_active() in the output drivers. Output is frozen while a
    // mapping is being entered so the buttons you press don't leak through.
    runtime_profile_check_combo(event->buttons,
                                event->analog[ANALOG_L2], event->analog[ANALOG_R2]);
    // Freeze output only while a live remap is being entered; rapid-fire set mode
    // stays live so you feel each rate as you cycle it.
    if (runtime_profile_is_mapping()) return;

    // Track activity for idle-sleep: any button, or a stick off-center.
    if (event->buttons != 0) {
        s_last_activity_ms = platform_time_ms();
    } else {
        for (int i = 0; i < 4; i++) {  // analog[0..3] = LX,LY,RX,RY
            int d = (int)event->analog[i] - 128;
            if (d > 24 || d < -24) { s_last_activity_ms = platform_time_ms(); break; }
        }
    }

    // Stream input to CDC for web config (only when a host is actively
    // consuming the stream). Without this gate the prep work below —
    // notably get_device_name(), which reaches into tuh_vid_pid_get() and
    // the HID registry — runs on every USB controller report (~1 kHz on a
    // native HID pad) even on output modes whose USB device is in HOST
    // mode (usb2gc, usb2dc, etc.) where CDC isn't enumerated at all and
    // the callee returns immediately anyway. Tight per-event loop matters
    // for high-precision input like Melee dash dancing.
#ifdef CONFIG_USB
    if (cdc_commands_is_input_streaming()) {
        static const char* transport_names[] = {
            [INPUT_TRANSPORT_NONE]       = "none",
            [INPUT_TRANSPORT_USB]        = "usb",
            [INPUT_TRANSPORT_BT_CLASSIC] = "bt classic",
            [INPUT_TRANSPORT_BT_BLE]     = "ble",
            [INPUT_TRANSPORT_NATIVE]     = "native",
            [INPUT_TRANSPORT_I2C]        = "i2c",
            [INPUT_TRANSPORT_GPIO]       = "gpio",
        };
        // Streaming: look up the shared player slot (Joy-Con Grip non-root
        // remaps to root). Note: stream_addr below still uses event->instance
        // so each Joy-Con appears as its own input source. Use the per-event
        // device name (not the player slot's cached name) so the two Joy-Con
        // sources display "Joy-Con (L)" and "Joy-Con (R)" individually.
        int pi = find_player_index(event->dev_addr, player_slot_instance(event));
        const char* name = get_device_name(event);
        (void)pi;  // pi reserved if needed for future per-player decisions
        const char* src = (event->transport < sizeof(transport_names)/sizeof(transport_names[0]))
                          ? transport_names[event->transport] : "?";
        // In merge mode, all inputs go to output player 0
        int stream_player = (router_config.mode == ROUTING_MODE_MERGE) ? 0
                          : (pi >= 0 ? pi : 0);
        // Encode instance into the high bit of the streaming addr so
        // multi-interface devices (e.g. Joy-Con Charging Grip — one HID
        // interface per Joy-Con) appear as distinct input sources in
        // the web config. Single-instance devices are unaffected
        // (instance 0 leaves the value unchanged).
        uint8_t stream_addr = (uint8_t)(event->dev_addr | (event->instance << 7));
        cdc_commands_send_player_input(stream_player, stream_addr,
                                       name, src,
                                       event->buttons, event->analog);
    }
#endif

    // Working copy used by every layer below (custom profile, overlay,
    // host-injected buttons, hotkey combos).
    static input_event_t remapped;
    bool did_remap = false;

    // Host-side synthetic input overlay (INPUT.INJECT) — merged into the real
    // event so host-driven presses join the streamer's controller regardless of
    // routing mode (works on SIMPLE, MERGE, BROADCAST). Buttons are OR'd;
    // analog takes whichever value is further from the axis's resting position.
    if (s_inject_buttons || s_inject_analog_set) {
        remapped = *event;
        remapped.buttons |= s_inject_buttons;
        if (s_inject_analog_set) inject_merge_analog(&remapped);
        did_remap = true;
        event = &remapped;
    }

    // Apply custom profile (button remap, stick sens, SOCD, flags, thresholds).
    // Done here in the router so it applies uniformly to ALL output interfaces.
    {
        const custom_profile_t* cp = flash_get_active_custom_profile();
        if (cp) {
            remapped = *event;

            // Turbo/auto-fire: gate held turbo-flagged PHYSICAL buttons with a 50%
            // duty cycle at the profile's shared rate, BEFORE the remap so the pulse
            // follows the button's assignment. Free-running phase (now % period) —
            // stateless, all turbo buttons share one phase (in-sync is desirable).
            uint32_t in = event->buttons;
            uint8_t rate_ms = profile_autofire_rate_ms(cp->autofire_rate);
            if (rate_ms) {
                bool duty_on = (platform_time_ms() % rate_ms) < (rate_ms / 2u);
                if (!duty_on) {
                    for (uint8_t i = 0; i < CUSTOM_PROFILE_BUTTON_COUNT; i++)
                        if (custom_profile_turbo_get(cp, i)) in &= ~(1u << i);
                }
            }

            // Button remap (so Fn key remaps are visible to hotkeys below)
            remapped.buttons = custom_profile_apply_buttons(cp, in);

            // Stick sensitivity
            if (cp->left_stick_sens != 100) {
                float sens = cp->left_stick_sens / 100.0f;
                int16_t rx = (int16_t)remapped.analog[ANALOG_LX] - 128;
                int16_t ry = (int16_t)remapped.analog[ANALOG_LY] - 128;
                remapped.analog[ANALOG_LX] = (uint8_t)(128 + (int16_t)(rx * sens));
                remapped.analog[ANALOG_LY] = (uint8_t)(128 + (int16_t)(ry * sens));
            }
            if (cp->right_stick_sens != 100) {
                float sens = cp->right_stick_sens / 100.0f;
                int16_t rx = (int16_t)remapped.analog[ANALOG_RX] - 128;
                int16_t ry = (int16_t)remapped.analog[ANALOG_RY] - 128;
                remapped.analog[ANALOG_RX] = (uint8_t)(128 + (int16_t)(rx * sens));
                remapped.analog[ANALOG_RY] = (uint8_t)(128 + (int16_t)(ry * sens));
            }

            // Swap sticks
            if (cp->flags & PROFILE_FLAG_SWAP_STICKS) {
                uint8_t tx = remapped.analog[ANALOG_LX], ty = remapped.analog[ANALOG_LY];
                remapped.analog[ANALOG_LX] = remapped.analog[ANALOG_RX];
                remapped.analog[ANALOG_LY] = remapped.analog[ANALOG_RY];
                remapped.analog[ANALOG_RX] = tx;
                remapped.analog[ANALOG_RY] = ty;
            }
            // Invert Y axes
            if (cp->flags & PROFILE_FLAG_INVERT_LY) {
                remapped.analog[ANALOG_LY] = 255 - remapped.analog[ANALOG_LY];
            }
            if (cp->flags & PROFILE_FLAG_INVERT_RY) {
                remapped.analog[ANALOG_RY] = 255 - remapped.analog[ANALOG_RY];
            }
            // Invert X axes
            if (cp->flags & PROFILE_FLAG_INVERT_LX) {
                remapped.analog[ANALOG_LX] = 255 - remapped.analog[ANALOG_LX];
            }
            if (cp->flags & PROFILE_FLAG_INVERT_RX) {
                remapped.analog[ANALOG_RX] = 255 - remapped.analog[ANALOG_RX];
            }

            // SOCD cleaning
            if (cp->socd_mode > 0 && cp->socd_mode <= 3) {
                remapped.buttons = apply_socd(remapped.buttons,
                    (socd_mode_t)cp->socd_mode, 0);
            }

            // Custom L2/R2 analog→digital thresholds
            if (cp->l2_threshold != 0) {
                remapped.buttons &= ~JP_BUTTON_L2;
                if (remapped.analog[ANALOG_L2] >= cp->l2_threshold) {
                    remapped.buttons |= JP_BUTTON_L2;
                }
            }
            if (cp->r2_threshold != 0) {
                remapped.buttons &= ~JP_BUTTON_R2;
                if (remapped.analog[ANALOG_R2] >= cp->r2_threshold) {
                    remapped.buttons |= JP_BUTTON_R2;
                }
            }

            did_remap = true;
            event = &remapped;
        }
    }

    // Apply runtime overlay (OVERLAY.SET) — composes ON TOP of whatever
    // profile is active, including the output device's built-in profile_t
    // (the overlay's stick/SOCD/threshold tweaks run before output dispatch).
    // Lets joypad-live add things like "invert LX" without disturbing the
    // active profile's button_map. Fields with value 0 are skipped, so the
    // overlay is strictly additive.
    {
        const runtime_overlay_t* ov = flash_get_overlay();
        if (ov) {
            if (!did_remap) {
                remapped = *event;
            }
            // Stick sensitivity (replace if non-zero)
            if (ov->left_stick_sens != 0 && ov->left_stick_sens != 100) {
                float sens = ov->left_stick_sens / 100.0f;
                int16_t rx = (int16_t)remapped.analog[ANALOG_LX] - 128;
                int16_t ry = (int16_t)remapped.analog[ANALOG_LY] - 128;
                remapped.analog[ANALOG_LX] = (uint8_t)(128 + (int16_t)(rx * sens));
                remapped.analog[ANALOG_LY] = (uint8_t)(128 + (int16_t)(ry * sens));
            }
            if (ov->right_stick_sens != 0 && ov->right_stick_sens != 100) {
                float sens = ov->right_stick_sens / 100.0f;
                int16_t rx = (int16_t)remapped.analog[ANALOG_RX] - 128;
                int16_t ry = (int16_t)remapped.analog[ANALOG_RY] - 128;
                remapped.analog[ANALOG_RX] = (uint8_t)(128 + (int16_t)(rx * sens));
                remapped.analog[ANALOG_RY] = (uint8_t)(128 + (int16_t)(ry * sens));
            }
            // Swap sticks
            if (ov->flags & PROFILE_FLAG_SWAP_STICKS) {
                uint8_t tx = remapped.analog[ANALOG_LX], ty = remapped.analog[ANALOG_LY];
                remapped.analog[ANALOG_LX] = remapped.analog[ANALOG_RX];
                remapped.analog[ANALOG_LY] = remapped.analog[ANALOG_RY];
                remapped.analog[ANALOG_RX] = tx;
                remapped.analog[ANALOG_RY] = ty;
            }
            // Invert axes
            if (ov->flags & PROFILE_FLAG_INVERT_LY) {
                remapped.analog[ANALOG_LY] = 255 - remapped.analog[ANALOG_LY];
            }
            if (ov->flags & PROFILE_FLAG_INVERT_RY) {
                remapped.analog[ANALOG_RY] = 255 - remapped.analog[ANALOG_RY];
            }
            if (ov->flags & PROFILE_FLAG_INVERT_LX) {
                remapped.analog[ANALOG_LX] = 255 - remapped.analog[ANALOG_LX];
            }
            if (ov->flags & PROFILE_FLAG_INVERT_RX) {
                remapped.analog[ANALOG_RX] = 255 - remapped.analog[ANALOG_RX];
            }
            // SOCD cleaning
            if (ov->socd_mode > 0 && ov->socd_mode <= 3) {
                remapped.buttons = apply_socd(remapped.buttons,
                    (socd_mode_t)ov->socd_mode, 0);
            }
            // L2/R2 thresholds
            if (ov->l2_threshold != 0) {
                remapped.buttons &= ~JP_BUTTON_L2;
                if (remapped.analog[ANALOG_L2] >= ov->l2_threshold) {
                    remapped.buttons |= JP_BUTTON_L2;
                }
            }
            if (ov->r2_threshold != 0) {
                remapped.buttons &= ~JP_BUTTON_R2;
                if (remapped.analog[ANALOG_R2] >= ov->r2_threshold) {
                    remapped.buttons |= JP_BUTTON_R2;
                }
            }
            did_remap = true;
            event = &remapped;
        }
    }

    // Apply button combo hotkeys
    for (int c = 0; c < ROUTER_COMBO_MAX; c++) {
        uint32_t in = router_combos[c].input_mask;
        if (!in) continue;

        // Layout filter: a combo can be restricted to one controller type
        // (e.g. GameCube S2+dpad vs GBA S1+dpad on the same gc2usb app).
        if (router_combos[c].required_layout &&
            event->layout != router_combos[c].required_layout) {
            router_combos[c].fired = false;
            router_combos[c].held_since = 0;   // both halves of the state, or a
            router_combos[c].held_dev = 0;     // stale timer outlives the filter
            continue;
        }

        bool held = (event->buttons & in) == in;

        // Decide whether to run the combo action this pass.
        //
        // Default combos require a deliberate ~0.7s hold, so a quick in-game
        // SELECT/START + D-pad passes straight through instead of switching a
        // profile. The catch: the USB HID input path is change-gated (a driver
        // only submits an event when the report *changes* — hid_gamepad.c), so a
        // static hold produces no events after the press and a timer can't
        // advance on the input path alone. So fire on WHICHEVER edge arrives:
        // while still held once the hold has elapsed (streaming controllers), or
        // on the release edge if it was held long enough (change-gated ones). A
        // quick tap (< hold) reaches neither and passes through. App-registered
        // combos stay instant.
        //
        // The hold is owned by ONE device. router_combos[] is global while the
        // router runs up to MAX_PLAYERS_PER_OUTPUT players, so without an owner
        // every event from a second controller arrives with held == false and
        // resets the timer — on a 2+ pad adapter player 1 could never accumulate
        // 0.7s and the default hotkeys would be unreachable.
        bool process;
        if (!router_default_combos_active) {
            process = held;
        } else {
            uint32_t key = combo_dev_key(event);
            if (router_combos[c].held_since != 0 && router_combos[c].held_dev != key) {
                continue;  // someone else's hold in progress — don't touch it
            }
            uint32_t now = platform_time_ms();
            if (held && router_combos[c].held_since == 0) {
                router_combos[c].held_since = now ? now : 1;  // 0 = not held
                router_combos[c].held_dev = key;
            }
            bool elapsed = router_combos[c].held_since != 0 &&
                           (now - router_combos[c].held_since) >= ROUTER_DEFAULT_COMBO_HOLD_MS;
            process = held ? elapsed                                // streaming: fire while held
                           : (elapsed && !router_combos[c].fired);  // release: fire on the up-edge
        }

        if (!process) {
            if (!held) {  // released (or quick tap): clear state, pass through
                router_combos[c].fired = false;
                router_combos[c].held_since = 0;
                router_combos[c].held_dev = 0;
            }
            continue;
        }

        if (!did_remap) {
            remapped = *event;
            did_remap = true;
        }

        uint32_t out = router_combos[c].output_mask;
        uint8_t action = (out >> 24) & 0xFF;
        switch (action) {
            case 0:  // Button remap
                remapped.buttons = (remapped.buttons & ~in) | (out & 0x003FFFFF);
                break;
            case 1:  // D-Pad → D-Pad
            case 2:  // D-Pad → Left Stick
            case 3:  // D-Pad → Right Stick
                if (!router_combos[c].fired) {
                    uint8_t new_mode = action - 1;
                    router_set_dpad_mode(new_mode);
                    flash_set_dpad_mode(new_mode);   // persist across reboot
                    router_combos[c].fired = true;
                }
                remapped.buttons &= ~in;
                break;
            case 4:  // Cycle D-Pad mode
                if (!router_combos[c].fired) {
                    uint8_t new_mode = (global_dpad_mode + 1) % 3;
                    router_set_dpad_mode(new_mode);
                    flash_set_dpad_mode(new_mode);   // persist across reboot
                    router_combos[c].fired = true;
                }
                remapped.buttons &= ~in;
                break;
            case 5:  // Next Profile (wrap)
                if (!router_combos[c].fired) {
                    profile_cycle_next(router_get_primary_output(), true);
                    router_combos[c].fired = true;
                }
                remapped.buttons &= ~in;
                break;
            case 6:  // Previous Profile (wrap)
                if (!router_combos[c].fired) {
                    profile_cycle_prev(router_get_primary_output(), true);
                    router_combos[c].fired = true;
                }
                remapped.buttons &= ~in;
                break;
            case 7:  // Toggle shoulder swap (L1<->L2, R1<->R2)
                if (!router_combos[c].fired) {
                    global_shoulder_swap = !global_shoulder_swap;
                    flash_set_shoulder_swap(global_shoulder_swap);  // persist
                    router_combos[c].fired = true;
                }
                remapped.buttons &= ~in;
                break;
            // D-pad/stick swap as a 4-position slider that clamps at the ends,
            // left -> right:
            //   [d-pad<->Lstick] [normal] [d-pad<->Rstick] [Lstick<->Rstick]
            //   modes:      1        0           2               3
            case 8:   // slider step LEFT
            case 9:   // slider step RIGHT
                if (!router_combos[c].fired) {
                    static const uint8_t pos_to_mode[4] = {1, 0, 2, 3};
                    int pos = (global_dpad_mode == 1) ? 0 :
                              (global_dpad_mode == 0) ? 1 :
                              (global_dpad_mode == 2) ? 2 : 3;
                    pos += (action == 9) ? 1 : -1;
                    if (pos < 0) pos = 0;
                    if (pos > 3) pos = 3;
                    uint8_t nm = pos_to_mode[pos];
                    router_set_dpad_mode(nm);
                    flash_set_dpad_mode(nm);   // persist across reboot
                    router_combos[c].fired = true;
                }
                remapped.buttons &= ~in;
                break;
            case 10:  // Previous Profile (clamp at first)
                if (!router_combos[c].fired) {
                    profile_cycle_prev(router_get_primary_output(), false);
                    router_combos[c].fired = true;
                }
                remapped.buttons &= ~in;
                break;
            case 11:  // Next Profile (clamp at last)
                if (!router_combos[c].fired) {
                    profile_cycle_next(router_get_primary_output(), false);
                    router_combos[c].fired = true;
                }
                remapped.buttons &= ~in;
                break;
        }

        // If this was the release-edge fire (change-gated controller went quiet
        // during the hold), the action just ran via the !fired latch above —
        // clear the hold so the next press starts fresh.
        if (!held) {
            router_combos[c].fired = false;
            router_combos[c].held_since = 0;
            router_combos[c].held_dev = 0;
        }
    }
    if (did_remap) event = &remapped;

    // Strip function keys from output (F1/F2 are internal-only for hotkey combos)
    if (did_remap) {
        remapped.buttons &= ~JP_BUTTON_FN_MASK;
    } else if (event->buttons & JP_BUTTON_FN_MASK) {
        remapped = *event;
        remapped.buttons &= ~JP_BUTTON_FN_MASK;
        did_remap = true;
        event = &remapped;
    }

    // Apply global d-pad/stick swap. Each mode trades both directions:
    //   1 = d-pad <-> left stick, 2 = d-pad <-> right stick, 3 = left <-> right stick.
    if (global_dpad_mode > 0) {
        if (!did_remap) { remapped = *event; did_remap = true; }
        if (global_dpad_mode == 3) {
            // Swap the two analog sticks (d-pad untouched).
            uint8_t tx = remapped.analog[0], ty = remapped.analog[1];
            remapped.analog[0] = remapped.analog[2];
            remapped.analog[1] = remapped.analog[3];
            remapped.analog[2] = tx;
            remapped.analog[3] = ty;
        } else {
            // D-pad <-> one stick, both directions. sx/sy = that stick's axes.
            const int sx = (global_dpad_mode == 1) ? 0 : 2;
            const int sy = sx + 1;
            const uint8_t TH = 60;  // stick->d-pad activation threshold (center 128)
            uint32_t dpad_bits = remapped.buttons &
                (JP_BUTTON_DU | JP_BUTTON_DD | JP_BUTTON_DL | JP_BUTTON_DR);
            uint8_t old_x = remapped.analog[sx], old_y = remapped.analog[sy];
            // stick -> d-pad
            remapped.buttons &= ~(JP_BUTTON_DU | JP_BUTTON_DD | JP_BUTTON_DL | JP_BUTTON_DR);
            if (old_x < 128 - TH)      remapped.buttons |= JP_BUTTON_DL;
            else if (old_x > 128 + TH) remapped.buttons |= JP_BUTTON_DR;
            if (old_y < 128 - TH)      remapped.buttons |= JP_BUTTON_DU;
            else if (old_y > 128 + TH) remapped.buttons |= JP_BUTTON_DD;
            // d-pad -> stick, with a circular gate: a diagonal lands on the unit
            // circle (~0.707 per axis, same magnitude as a cardinal) like a real
            // stick, instead of at the square's corner (255,0).
            bool x_dir = (dpad_bits & (JP_BUTTON_DL | JP_BUTTON_DR)) != 0;
            bool y_dir = (dpad_bits & (JP_BUTTON_DU | JP_BUTTON_DD)) != 0;
            bool diag  = x_dir && y_dir;
            uint8_t lo = diag ? 38  : 0;    // 128 - ~128*0.707
            uint8_t hi = diag ? 218 : 255;  // 128 + ~128*0.707
            uint8_t ax = 128, ay = 128;
            if (dpad_bits & JP_BUTTON_DL) ax = lo; else if (dpad_bits & JP_BUTTON_DR) ax = hi;
            if (dpad_bits & JP_BUTTON_DU) ay = lo; else if (dpad_bits & JP_BUTTON_DD) ay = hi;
            remapped.analog[sx] = ax;
            remapped.analog[sy] = ay;
        }
        event = &remapped;
    }

    // Apply global shoulder swap (L1<->L2, R1<->R2). Swaps the digital button
    // bits; the analog trigger values aren't touched (GBA shoulders are
    // digital-only, which is the use case this serves).
    if (global_shoulder_swap) {
        if (!did_remap) { remapped = *event; did_remap = true; }
        uint32_t b = remapped.buttons;
        uint32_t swapped = b & ~(JP_BUTTON_L1 | JP_BUTTON_R1 | JP_BUTTON_L2 | JP_BUTTON_R2);
        if (b & JP_BUTTON_L1) swapped |= JP_BUTTON_L2;
        if (b & JP_BUTTON_R1) swapped |= JP_BUTTON_R2;
        if (b & JP_BUTTON_L2) swapped |= JP_BUTTON_L1;
        if (b & JP_BUTTON_R2) swapped |= JP_BUTTON_R1;
        remapped.buttons = swapped;
        // The analog trigger axes must follow the swap: a trigger moved to L1/R1
        // becomes a digital bumper, so drive the L2/R2 analog axes from the
        // swapped digital state (0/255) instead of leaving the old trigger
        // travel firing on them.
        remapped.analog[ANALOG_L2] = (swapped & JP_BUTTON_L2) ? 255 : 0;
        remapped.analog[ANALOG_R2] = (swapped & JP_BUTTON_R2) ? 255 : 0;
        event = &remapped;
    }

    // Find first active route to determine output target
    output_target_t output = OUTPUT_TARGET_USB_DEVICE;
    for (uint8_t i = 0; i < MAX_ROUTES; i++) {
        if (routing_table[i].active) {
            output = routing_table[i].output;
            break;
        }
    }

    // Route based on mode
    switch (router_config.mode) {
        case ROUTING_MODE_SIMPLE:
            router_simple_mode(event, output);
            break;

        case ROUTING_MODE_MERGE:
            // Route to all unique output targets from active routes
            for (uint8_t i = 0; i < MAX_ROUTES; i++) {
                if (!routing_table[i].active) continue;
                // Deduplicate: skip if a previous route already targeted this output
                bool dup = false;
                for (uint8_t j = 0; j < i; j++) {
                    if (routing_table[j].active && routing_table[j].output == routing_table[i].output) {
                        dup = true;
                        break;
                    }
                }
                if (!dup) {
                    router_merge_mode(event, routing_table[i].output);
                }
            }
            break;

        case ROUTING_MODE_BROADCAST:
            if (active_output_count > 0) {
                for (uint8_t i = 0; i < active_output_count; i++) {
                    router_simple_mode(event, active_outputs[i]);
                }
            } else {
                router_simple_mode(event, output);
            }
            break;

        case ROUTING_MODE_CONFIGURABLE:
            {
                route_entry_t matches[MAX_ROUTES];
                uint8_t match_count = router_find_routes(event, matches, MAX_ROUTES);

                if (match_count == 0) {
                    router_simple_mode(event, output);
                } else {
                    for (uint8_t i = 0; i < match_count; i++) {
                        output_target_t target = matches[i].output;
                        uint8_t target_player = matches[i].output_player_id;

                        if (target_player != 0xFF && target_player < MAX_PLAYERS_PER_OUTPUT) {
                            const input_event_t* final_event;
                            input_event_t transformed;

                            if (router_config.transform_flags) {
                                transformed = *event;
                                apply_transformations(&transformed, target, target_player);
                                final_event = &transformed;
                            } else {
                                final_event = event;
                            }

                            if (!output_tap_exclusive[target]) {
                                router_outputs[target][target_player].source = INPUT_SOURCE_USB_HOST;
                                router_publish(&router_outputs[target][target_player], final_event);
                            }

                            if (output_taps[target]) {
                                output_taps[target](target, target_player, final_event);
                            }
                        } else {
                            router_simple_mode(event, target);
                        }
                    }
                }
            }
            break;
    }
}

// ============================================================================
// OUTPUT RETRIEVAL (Core 1 - Poll or Event Driven)
// ============================================================================

// Consumer-side (Core 1) private buffers: the returned snapshot, and the last
// seq we handed out per slot (edge detection — return each published event once,
// NULL otherwise, matching the old `updated`-flag semantics several consumers
// rely on). Written only by the consumer, never by the producer.
static input_event_t router_output_copy[MAX_OUTPUTS][MAX_PLAYERS_PER_OUTPUT];
static uint32_t router_output_last_seq[MAX_OUTPUTS][MAX_PLAYERS_PER_OUTPUT];

const input_event_t* __not_in_flash_func(router_get_output)(output_target_t output, uint8_t player_id) {
    if (output >= MAX_OUTPUTS || player_id >= MAX_PLAYERS_PER_OUTPUT) {
        return NULL;
    }

    output_state_t* s = &router_outputs[output][player_id];

    // Seqlock read: copy the payload between two matching even seq reads. If the
    // producer wrote concurrently (seq odd, or changed across the copy), retry —
    // bounded so the timing-critical console core can't stall (falls back to
    // "no new data"). Copy straight into the private return buffer; a torn
    // attempt is simply overwritten by the next try and never returned.
    uint32_t s1, s2;
    int tries = 0;
    do {
        s1 = s->seq;
        if (s1 & 1u) {                          // writer mid-update
            if (++tries > ROUTER_CONSUME_MAX_RETRY) return NULL;
            continue;
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        router_output_copy[output][player_id] = s->current_state;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        s2 = s->seq;
    } while (s1 != s2 && ++tries <= ROUTER_CONSUME_MAX_RETRY);

    if ((s1 & 1u) || s1 != s2) return NULL;     // gave up cleanly → reuse last frame

    // Edge detect: only hand out each published version once. seq==0 means "no
    // data yet". Deltas are one-shot for free (each version returned exactly
    // once), so no write-back into shared state is needed.
    if (s1 == 0 || s1 == router_output_last_seq[output][player_id]) {
        return NULL;
    }
    router_output_last_seq[output][player_id] = s1;
    return &router_output_copy[output][player_id];
}

bool router_has_updates(output_target_t output) {
    if (output >= MAX_OUTPUTS) return false;

    for (uint8_t player = 0; player < MAX_PLAYERS_PER_OUTPUT; player++) {
        uint32_t seq = router_outputs[output][player].seq;
        if ((seq & 1u) == 0 && seq != 0 && seq != router_output_last_seq[output][player]) {
            return true;
        }
    }
    return false;
}

uint8_t router_get_player_count(output_target_t output) {
    if (output >= MAX_OUTPUTS) return 0;

    // Return current playersCount (from player management system)
    extern int playersCount;
    return (uint8_t)playersCount;
}

uint8_t router_get_max_players(output_target_t output) {
    if (output < 0 || output >= MAX_OUTPUTS) return 0;
    return router_config.max_players_per_output[output];
}

// ============================================================================
// ROUTING CONFIGURATION
// ============================================================================

void router_set_merge_mode(output_target_t output, merge_mode_t mode) {
    router_config.merge_mode = mode;
    printf(LOG_TAG "Merge mode set: %s\n",
        mode == MERGE_PRIORITY ? "PRIORITY" :
        mode == MERGE_BLEND ? "BLEND" : "ALL");
}

void router_set_active_outputs(output_target_t* outputs, uint8_t count) {
    if (!outputs || count > MAX_OUTPUTS) return;

    active_output_count = count;
    for (uint8_t i = 0; i < count; i++) {
        active_outputs[i] = outputs[i];
    }

    printf(LOG_TAG "Active outputs set: count=%d\n", count);
}

output_target_t router_get_primary_output(void) {
    // First check active_outputs (used by BROADCAST mode)
    if (active_output_count > 0) {
        return active_outputs[0];
    }

    // Fall back to first active route's output (used by SIMPLE/MERGE modes)
    for (uint8_t i = 0; i < MAX_ROUTES; i++) {
        if (routing_table[i].active) {
            return routing_table[i].output;
        }
    }

    return OUTPUT_TARGET_NONE;
}

// ============================================================================
// OUTPUT TAPS
// ============================================================================

void router_set_tap(output_target_t output, router_tap_callback_t callback) {
    if (output >= 0 && output < MAX_OUTPUTS) {
        output_taps[output] = callback;
        output_tap_exclusive[output] = false;
        printf(LOG_TAG "Tap %s for output %d\n",
               callback ? "registered" : "unregistered", output);
    }
}

void router_set_tap_exclusive(output_target_t output, router_tap_callback_t callback) {
    if (output >= 0 && output < MAX_OUTPUTS) {
        output_taps[output] = callback;
        output_tap_exclusive[output] = (callback != NULL);
        printf(LOG_TAG "Exclusive tap %s for output %d\n",
               callback ? "registered" : "unregistered", output);
    }
}

// ============================================================================
// DEBUG/TESTING
// ============================================================================

output_state_t* router_get_state_ptr(output_target_t output) {
    if (output >= MAX_OUTPUTS) return NULL;
    return router_outputs[output];
}

// Reset all output states to neutral (call when all controllers disconnect)
void router_reset_outputs(void) {
    printf(LOG_TAG "Resetting all outputs to neutral\n");

    // Reset all output states — publish a neutral event so consumers see the
    // change (seqlock bump) and drive their outputs to neutral.
    input_event_t neutral;
    init_input_event(&neutral);
    for (uint8_t output = 0; output < MAX_OUTPUTS; output++) {
        for (uint8_t player = 0; player < MAX_PLAYERS_PER_OUTPUT; player++) {
            router_publish(&router_outputs[output][player], &neutral);
        }

        // Clear blend device tracking
        for (uint8_t i = 0; i < MAX_BLEND_DEVICES; i++) {
            blend_devices[output][i].active = false;
            blend_devices[output][i].dev_addr = 0;
            blend_devices[output][i].instance = -1;
            init_input_event(&blend_devices[output][i].state);
        }
    }
}

// Clean up router state when a device disconnects
void router_device_disconnected(uint8_t dev_addr, int8_t instance) {
    printf(LOG_TAG "Device disconnected: dev_addr=%d, instance=%d\n", dev_addr, instance);

    // Drop any combo hold this device owned. Unplugging mid-hold produces no
    // further events from it, so nothing else would ever clear the timer and
    // the next press would measure against a stale timestamp — firing on frame
    // one, which is exactly what the hold guard exists to prevent.
    uint32_t gone = 0x10000u | ((uint32_t)dev_addr << 8) | (uint8_t)instance;
    for (int c = 0; c < ROUTER_COMBO_MAX; c++) {
        if (router_combos[c].held_dev == gone) {
            router_combos[c].held_since = 0;
            router_combos[c].held_dev = 0;
            router_combos[c].fired = false;
        }
    }

    // Find the player index for this device
    int player_index = find_player_index(dev_addr, instance);

    // Find first active route to determine output target
    output_target_t output = OUTPUT_TARGET_USB_DEVICE;
    for (uint8_t i = 0; i < MAX_ROUTES; i++) {
        if (routing_table[i].active) {
            output = routing_table[i].output;
            break;
        }
    }

    // Clear blend device tracking for this device (MERGE_BLEND mode)
    for (uint8_t out = 0; out < MAX_OUTPUTS; out++) {
        for (uint8_t i = 0; i < MAX_BLEND_DEVICES; i++) {
            if (blend_devices[out][i].active &&
                blend_devices[out][i].dev_addr == dev_addr &&
                blend_devices[out][i].instance == instance) {
                blend_devices[out][i].active = false;
                blend_devices[out][i].dev_addr = 0;
                blend_devices[out][i].instance = -1;
                init_input_event(&blend_devices[out][i].state);
                printf(LOG_TAG "Cleared blend device slot %d for output %d\n", i, out);
            }
        }
    }

    // For MERGE mode, all inputs go to player 0 - re-blend remaining devices.
    // Build into a local and publish once (seqlock) so Core 1 never sees a torn
    // frame mid-reblend.
    if (router_config.mode == ROUTING_MODE_MERGE) {
        input_event_t rebuilt;
        init_input_event(&rebuilt);

        if (router_config.merge_mode == MERGE_BLEND) {
            // Re-blend all remaining active devices
            for (uint8_t i = 0; i < MAX_BLEND_DEVICES; i++) {
                if (!blend_devices[output][i].active) continue;

                input_event_t* dev = &blend_devices[output][i].state;

                // Buttons: OR together
                rebuilt.buttons |= dev->buttons;
                rebuilt.keys |= dev->keys;

                // Analog: use furthest from center for sticks, max for triggers
                // Format: [0]=LX, [1]=LY, [2]=RX, [3]=RY, [4]=L2, [5]=R2
                for (int j = 0; j < ANALOG_COUNT; j++) {
                    if (j >= ANALOG_L2) {
                        // Triggers: use max value
                        if (dev->analog[j] > rebuilt.analog[j]) {
                            rebuilt.analog[j] = dev->analog[j];
                        }
                    } else {
                        // Sticks: use furthest from center
                        int8_t cur_delta = (int8_t)(rebuilt.analog[j] - 128);
                        int8_t dev_delta = (int8_t)(dev->analog[j] - 128);
                        if (abs(dev_delta) > abs(cur_delta)) {
                            rebuilt.analog[j] = dev->analog[j];
                        }
                    }
                }

                // Battery: use first device that reports battery
                if (dev->battery_level > 0 && rebuilt.battery_level == 0) {
                    rebuilt.battery_level = dev->battery_level;
                    rebuilt.battery_charging = dev->battery_charging;
                }
            }
        }

        // No input controller reported a battery → fall back to this device's
        // own battery (e.g. controller_btusb on a LiPo) so the SInput report
        // carries real charge_level/plug_status.
        if (rebuilt.battery_level == 0 && onboard_batt_pct >= 0) {
            rebuilt.battery_level = (uint8_t)onboard_batt_pct;
            rebuilt.battery_charging = onboard_batt_charging;
        }
        // Onboard IMU motion when no input device supplied any.
        if (!rebuilt.has_motion && onboard_motion.valid) {
            for (int mi = 0; mi < 3; mi++) {
                rebuilt.accel[mi] = onboard_motion.accel[mi];
                rebuilt.gyro[mi] = onboard_motion.gyro[mi];
            }
            rebuilt.accel_range = onboard_motion.accel_range;
            rebuilt.gyro_range = onboard_motion.gyro_range;
            rebuilt.has_motion = true;
        }

        router_publish(&router_outputs[output][0], &rebuilt);

        // Always notify tap with current state (zeroed or re-blended)
        if (output_taps[output]) {
            output_taps[output](output, 0, &rebuilt);
        }

        printf(LOG_TAG "Updated merged output (player 0)\n");
    } else {
        // SIMPLE/BROADCAST mode: clear this player's specific output state
        if (player_index >= 0 && player_index < MAX_PLAYERS_PER_OUTPUT) {
            input_event_t cleared;
            init_input_event(&cleared);
            router_publish(&router_outputs[output][player_index], &cleared);

            // Notify tap if registered (sends zeroed state to USB/UART output)
            if (output_taps[output]) {
                output_taps[output](output, player_index, &cleared);
            }

            printf(LOG_TAG "Cleared output state for player %d\n", player_index);
        }
    }
}


void router_set_dpad_mode(uint8_t mode) {
    if (mode <= 3) {
        global_dpad_mode = mode;
        static const char* names[] = {
            "NORMAL", "D-PAD<->LSTICK", "D-PAD<->RSTICK", "LSTICK<->RSTICK"
        };
        printf(LOG_TAG "D-pad mode: %s\n", names[mode]);
    }
}

void router_set_combo(uint8_t index, uint32_t input_mask, uint32_t output_mask) {
    if (index >= ROUTER_COMBO_MAX) return;
    // The first app-registered combo takes ownership of the table: wipe the
    // built-in defaults installed by router_init() so they can't linger in slots
    // the app leaves unused.
    if (router_default_combos_active) {
        router_default_combos_active = false;
        for (int i = 0; i < ROUTER_COMBO_MAX; i++) {
            router_combos[i].input_mask = 0;
            router_combos[i].output_mask = 0;
            router_combos[i].required_layout = 0;
            router_combos[i].fired = false;
            router_combos[i].held_since = 0;
            router_combos[i].held_dev = 0;
        }
    }
    router_combos[index].input_mask = input_mask;
    router_combos[index].output_mask = output_mask;
    router_combos[index].required_layout = 0;  // any layout by default
    router_combos[index].fired = false;
    router_combos[index].held_since = 0;
    router_combos[index].held_dev = 0;
}

void router_set_combo_layout(uint8_t index, uint8_t required_layout) {
    if (index >= ROUTER_COMBO_MAX) return;
    router_combos[index].required_layout = required_layout;
}

void router_set_shoulder_swap(bool on) {
    global_shoulder_swap = on;
}

// Live getters — reflect the boot-restored value plus any hotkey/CDC change
// immediately (unlike the flash record, whose write is debounced ~5s).
uint8_t router_get_dpad_mode(void) { return global_dpad_mode; }
bool router_get_shoulder_swap(void) { return global_shoulder_swap; }
