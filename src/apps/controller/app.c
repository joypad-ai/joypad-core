// app.c - Universal Controller App
// Pad buttons → USB HID Gamepad output
//
// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 Robert Dale Smith

#include "app.h"
#include "profiles.h"
#include "core/router/router.h"
#include "core/input_interface.h"
#include "core/output_interface.h"
#include "core/services/button/button.h"
#include "core/services/leds/leds.h"
#include "core/services/leds/neopixel/ws2812.h"
#include "core/services/speaker/speaker.h"
#include "core/services/display/display.h"
#include "core/services/codes/codes.h"
#include "core/services/profiles/profile.h"
#include "native/device/uart/uart_device.h"
#include "native/host/uart/uart_host.h"
#include "pad/pad_input.h"
#include "pad/pad_config_flash.h"
#include "usb/usbd/usbd.h"
#include "core/buttons.h"
#include "tusb.h"
#include "pico/stdlib.h"
#include <stdio.h>

// Active pad config (points to flash or compile-time default)
static const pad_device_config_t* pad_config = NULL;

// UART linking state
static bool uart_link_enabled = false;

// I2C peer state
#ifdef I2C_PEER_ENABLED
#include "i2c_peer/i2c_peer.h"
static bool i2c_peer_enabled = false;
static i2c_peer_status_t peer_status = {0};
static bool peer_status_valid = false;
#endif

// Track last displayed mode to avoid unnecessary redraws
static usb_output_mode_t last_displayed_mode = 0xFF;
static uint8_t last_rumble = 0;
static uint32_t last_buttons = 0;  // Track button state for edge detection

// Button name lookup table (matches JP_BUTTON_* bit positions)
typedef struct {
    uint32_t mask;
    const char* name;
} button_name_t;

// Arrow characters for display (1=up, 2=down, 3=left, 4=right)
#define ARROW_UP    "\x01"
#define ARROW_DOWN  "\x02"
#define ARROW_LEFT  "\x03"
#define ARROW_RIGHT "\x04"

static const button_name_t button_names[] = {
    { JP_BUTTON_DU, ARROW_UP },
    { JP_BUTTON_DR, ARROW_RIGHT },
    { JP_BUTTON_DD, ARROW_DOWN },
    { JP_BUTTON_DL, ARROW_LEFT },
    { JP_BUTTON_B1, "B1" },
    { JP_BUTTON_B2, "B2" },
    { JP_BUTTON_B3, "B3" },
    { JP_BUTTON_B4, "B4" },
    { JP_BUTTON_L1, "L1" },
    { JP_BUTTON_R1, "R1" },
    { JP_BUTTON_L2, "L2" },
    { JP_BUTTON_R2, "R2" },
    { JP_BUTTON_S1, "S1" },
    { JP_BUTTON_S2, "S2" },
    { JP_BUTTON_L3, "L3" },
    { JP_BUTTON_R3, "R3" },
    { JP_BUTTON_A1, "A1" },
    { JP_BUTTON_A2, "A2" },
    { 0, NULL }
};

// ============================================================================
// CONTROLLER TYPE CONFIGURATION
// ============================================================================

#if defined(CONTROLLER_TYPE_FISHERPRICE_V1)
    #include "pad/configs/fisherprice.h"
    #define PAD_CONFIG pad_config_fisherprice_v1
    #define CONTROLLER_NAME "Fisher Price V1"
#elif defined(CONTROLLER_TYPE_FISHERPRICE_V2)
    #include "pad/configs/fisherprice.h"
    #define PAD_CONFIG pad_config_fisherprice_v2
    #define CONTROLLER_NAME "Fisher Price V2"
#elif defined(CONTROLLER_TYPE_ALPAKKA)
    #include "pad/configs/alpakka.h"
    #define PAD_CONFIG pad_config_alpakka
    #define CONTROLLER_NAME "Alpakka"
#elif defined(CONTROLLER_TYPE_MACROPAD)
    #include "pad/configs/macropad.h"
    #define PAD_CONFIG pad_config_macropad
    #define CONTROLLER_NAME "MacroPad"
#else
    #error "No controller type defined! Define one of: CONTROLLER_TYPE_FISHERPRICE_V1, CONTROLLER_TYPE_FISHERPRICE_V2, CONTROLLER_TYPE_ALPAKKA, etc."
#endif

// ============================================================================
// BUTTON EVENT HANDLER
// ============================================================================

static void on_button_event(button_event_t event)
{
    switch (event) {
        case BUTTON_EVENT_CLICK:
            printf("[app:controller] Button click - current mode: %s\n",
                   usbd_get_mode_name(usbd_get_mode()));
            break;

        case BUTTON_EVENT_DOUBLE_CLICK: {
            // Cycle to next mode (usbd_set_mode flushes debug + saves to flash)
            usb_output_mode_t next = usbd_get_next_mode();
            printf("[app:controller] Double-click - switching USB mode → %s\n",
                   usbd_get_mode_name(next));
            usbd_set_mode(next);
            break;
        }

        case BUTTON_EVENT_TRIPLE_CLICK:
            printf("[app:controller] Button triple-click - resetting to default HID mode\n");
            usbd_reset_to_hid();
            break;

        default:
            break;
    }
}

// ============================================================================
// KONAMI CODE CALLBACK
// ============================================================================

static void on_code_detected(const char* code_name)
{
    printf("[app:controller] Code detected: %s\n", code_name);

    // Visual feedback - use profile indicator (flashes LEDs)
    neopixel_indicate_profile(3);  // Flash 4 times

    // Audio feedback - play victory melody
    if (speaker_is_initialized()) {
        speaker_tone(880, 200);    // A5
        sleep_ms(100);
        speaker_tone(1047, 200);   // C6
        sleep_ms(100);
        speaker_tone(1319, 255);   // E6
        sleep_ms(200);
        speaker_stop();
    }

    // Display feedback - show on marquee
    if (display_is_initialized()) {
        display_marquee_add("KONAMI!");
    }
}

// ============================================================================
// UART LINK TAP (sends local inputs to linked controller)
// ============================================================================

// Router tap callback - sends local inputs to linked controller via UART
// Filters out UART-received inputs (dev_addr >= 0xD0) to prevent loops
static void uart_link_tap(output_target_t output, uint8_t player_index,
                          const input_event_t* event)
{
    (void)output;

    if (!uart_link_enabled) return;

    // Don't resend inputs that came from UART (dev_addr 0xD0+ range)
    // This prevents infinite loops between linked controllers
    if (event->dev_addr >= 0xD0) return;

    // Send local input to linked controller
    uart_device_queue_input(event, player_index);
}

// ============================================================================
// APP INPUT INTERFACES
// ============================================================================

static const InputInterface* input_interfaces[] = {
    &pad_input_interface,
};

const InputInterface** app_get_input_interfaces(uint8_t* count)
{
    *count = sizeof(input_interfaces) / sizeof(input_interfaces[0]);
    return input_interfaces;
}

// ============================================================================
// APP OUTPUT INTERFACES
// ============================================================================

static const OutputInterface* output_interfaces[] = {
    &usbd_output_interface,
};

const OutputInterface** app_get_output_interfaces(uint8_t* count)
{
    *count = sizeof(output_interfaces) / sizeof(output_interfaces[0]);
    return output_interfaces;
}

// ============================================================================
// APP INITIALIZATION
// ============================================================================

void app_init(void)
{
    printf("[app:controller] Initializing %s Controller v%s\n", CONTROLLER_NAME, JOYPAD_VERSION);

    // Initialize button service
    button_init();
    button_set_callback(on_button_event);

    // Initialize codes service (Konami code detection)
    codes_set_callback(on_code_detected);

    // Load pad config: try flash first, fall back to compile-time default
    pad_config_flash_init();
    pad_config = pad_config_load_runtime();
    if (!pad_config) {
        pad_config = &PAD_CONFIG;
        printf("[app:controller] Using compile-time default config\n");
    } else {
        printf("[app:controller] Using flash config: %s\n", pad_config->name);
    }

    // Register pad device configuration BEFORE interface init
    int dev_idx = pad_input_add_device(pad_config);

    if (dev_idx < 0) {
        printf("[app:controller] ERROR: Failed to register pad device!\n");
        return;
    }

    printf("[app:controller] Pad config: %s\n", pad_config->name);

    // Set custom LED colors from pad config if defined
    if (pad_config->led_count > 0) {
        neopixel_set_custom_colors(pad_config->led_colors, pad_config->led_count);
        if (neopixel_has_custom_colors()) {
            printf("[app:controller] Using custom LED colors (%d LEDs)\n", pad_config->led_count);
        }
    }

    // Initialize speaker for haptic/rumble feedback if configured
    if (pad_config->speaker_pin != PAD_PIN_DISABLED) {
        speaker_init(pad_config->speaker_pin, pad_config->speaker_enable_pin);
        printf("[app:controller] Speaker initialized for rumble feedback\n");
    }

    // Initialize display if configured
    if (pad_config->display_spi >= 0) {
        display_config_t disp_cfg = {
            .spi_inst = pad_config->display_spi,
            .pin_sck = pad_config->display_sck,
            .pin_mosi = pad_config->display_mosi,
            .pin_cs = pad_config->display_cs,
            .pin_dc = pad_config->display_dc,
            .pin_rst = pad_config->display_rst,
        };
        display_init(&disp_cfg);
        // Async: display_update() marks dirty and returns; app_task() pushes
        // one page per iteration so no single iteration blocks on the transfer.
        display_set_async(true);
        printf("[app:controller] Display initialized\n");
    }

    // Initialize QWIIC link: I2C peer mode or UART link
#ifdef I2C_PEER_ENABLED
    if (pad_config->qwiic_i2c_inst >= 0 &&
        pad_config->qwiic_tx != PAD_PIN_DISABLED && pad_config->qwiic_rx != PAD_PIN_DISABLED) {
        // I2C peer slave mode — serve local inputs to master over I2C
        i2c_peer_config_t peer_cfg = {
            .i2c_inst = (uint8_t)pad_config->qwiic_i2c_inst,
            .sda_pin = (uint8_t)pad_config->qwiic_tx,
            .scl_pin = (uint8_t)pad_config->qwiic_rx,
            .addr = I2C_PEER_DEFAULT_ADDR,
            .skip_i2c_init = false,
        };
        i2c_peer_slave_set_name(CONTROLLER_NAME);
        i2c_peer_slave_init(&peer_cfg);
        i2c_peer_enabled = true;
    } else
#endif
    if (pad_config->qwiic_tx != PAD_PIN_DISABLED && pad_config->qwiic_rx != PAD_PIN_DISABLED) {
        // UART link mode — bidirectional UART between two controllers
        uart_host_init_pins(pad_config->qwiic_tx, pad_config->qwiic_rx, UART_PROTOCOL_BAUD_DEFAULT);
        uart_host_set_mode(UART_HOST_MODE_NORMAL);

        uart_device_init_pins(pad_config->qwiic_tx, pad_config->qwiic_rx, UART_PROTOCOL_BAUD_DEFAULT);
        uart_device_set_mode(UART_DEVICE_MODE_ON_CHANGE);

        uart_link_enabled = true;
        printf("[app:controller] UART link enabled on QWIIC (TX=%d, RX=%d)\n",
               pad_config->qwiic_tx, pad_config->qwiic_rx);
    }

    // Configure router for Pad → USB Device
    router_config_t router_cfg = {
        .mode = ROUTING_MODE_SIMPLE,
        .merge_mode = MERGE_PRIORITY,
        .max_players_per_output = {
            [OUTPUT_TARGET_USB_DEVICE] = 1,
        },
        .merge_all_inputs = false,
        .transform_flags = 0,
        .mouse_drain_rate = 0,
    };
    router_init(&router_cfg);

    // Initialize profile system with button combos (S1+S2=A1)
    static const profile_config_t profile_cfg = {
        .output_profiles = { NULL },
        .shared_profiles = &controller_profile_set,
    };
    profile_init(&profile_cfg);

    // Add route: Pad → USB Device
    router_add_route(INPUT_SOURCE_GPIO, OUTPUT_TARGET_USB_DEVICE, 0);

    // UART link forwarding is handled in app_task() via router_get_output()
    // (not via tap, since usbd already owns the USB_DEVICE tap)

    printf("[app:controller] Initialization complete\n");
    printf("[app:controller]   Routing: Pad → USB Device (HID Gamepad)\n");
#ifdef I2C_PEER_ENABLED
    if (i2c_peer_enabled) {
        printf("[app:controller]   I2C Peer: Slave mode (connect via STEMMA QT)\n");
    } else
#endif
    if (uart_link_enabled) {
        printf("[app:controller]   UART Link: Enabled (connect via QWIIC to merge inputs)\n");
    }
    printf("[app:controller]   Double-click encoder button to switch USB mode\n");
}

// ============================================================================
// APP TASK
// ============================================================================

// Update display with current mode and status
static void update_display(uint8_t rumble, uint32_t buttons)
{
    if (!display_is_initialized()) return;

    usb_output_mode_t mode = usbd_get_mode();
    bool needs_update = false;

#ifdef I2C_PEER_ENABLED
    // When peer status is available, show master's mode instead of local
    static usb_output_mode_t last_peer_mode = 0xFF;
    static bool last_peer_connected = false;
    if (peer_status_valid) {
        usb_output_mode_t peer_mode = (usb_output_mode_t)peer_status.usb_mode;
        bool peer_connected = (peer_status.flags & I2C_PEER_FLAG_CONNECTED) != 0;

        if (peer_mode != last_peer_mode || peer_connected != last_peer_connected) {
            last_peer_mode = peer_mode;
            last_peer_connected = peer_connected;
            last_displayed_mode = 0xFF;  // Force redraw
        }
    }
#endif

    // Check if mode changed
    if (mode != last_displayed_mode) {
        last_displayed_mode = mode;
        needs_update = true;

        display_clear();

        // Draw mode name in large text at top
        const char* mode_name = usbd_get_mode_name(mode);
#ifdef I2C_PEER_ENABLED
        if (peer_status_valid && peer_status.usb_mode < USB_OUTPUT_MODE_COUNT) {
            mode_name = usbd_get_mode_name((usb_output_mode_t)peer_status.usb_mode);
        }
#endif
        display_text_large(4, 4, mode_name);

        // Draw separator line
        display_hline(0, 24, 128);

#ifdef I2C_PEER_ENABLED
        // Show connected device name from master
        if (peer_status_valid && (peer_status.flags & I2C_PEER_FLAG_CONNECTED) &&
            (peer_status.flags & I2C_PEER_FLAG_NAME_VALID)) {
            display_text(4, 28, peer_status.name);
        } else if (peer_status_valid) {
            // No controller connected on master — leave blank
        } else {
#endif
        // Draw labels
        display_text(4, 28, "Rumble:");
#ifdef I2C_PEER_ENABLED
        }
#endif
    }

    // Update rumble bar if changed significantly
    if (needs_update || (rumble / 8) != (last_rumble / 8)) {
        last_rumble = rumble;

        // Clear rumble bar area and redraw
        display_fill_rect(4, 38, 120, 10, false);
        display_progress_bar(4, 38, 120, 10, (rumble * 100) / 255);

        needs_update = true;
    }

    // Detect newly pressed buttons
    // Buttons use active-high in router (1 = pressed, 0 = released)
    // Detect rising edge: was not pressed (0), now pressed (1)
    uint32_t newly_pressed = ~last_buttons & buttons;
    last_buttons = buttons;

    // Add newly pressed buttons to marquee
    bool button_added = false;
    for (int i = 0; button_names[i].name != NULL; i++) {
        if (newly_pressed & button_names[i].mask) {
            display_marquee_add(button_names[i].name);
            button_added = true;
        }
    }

    // Update marquee animation (handles scrolling and fade timeout)
    bool marquee_changed = display_marquee_tick();

    // Render marquee if anything changed
    if (button_added || marquee_changed) {
        display_marquee_render(54);  // Render at bottom of display
        needs_update = true;
    }

    if (needs_update) {
        display_update();
    }
}

void app_task(void)
{
    // Process button input for mode switching
    button_task();

    // Push at most one display page per iteration (no-op when nothing is
    // pending), so the OLED transfer never blocks the input path.
    display_flush_step();

    // Update LED colors when USB output mode changes (skip when peer controls LEDs)
#ifdef I2C_PEER_ENABLED
    if (!peer_status_valid)
#endif
    {
        static usb_output_mode_t last_led_mode = USB_OUTPUT_MODE_COUNT;
        usb_output_mode_t mode = usbd_get_mode();
        if (mode != last_led_mode) {
            last_led_mode = mode;
            uint8_t r, g, b;
            usbd_get_mode_color(mode, &r, &g, &b);
            if (pad_config->led_count > 1) {
                // Multi-LED: set all LEDs to mode color, enable pulse mask
                uint8_t colors[16][3];
                for (int i = 0; i < pad_config->led_count && i < 16; i++) {
                    colors[i][0] = r;
                    colors[i][1] = g;
                    colors[i][2] = b;
                }
                neopixel_set_custom_colors(colors, pad_config->led_count);
                neopixel_set_pulse_mask(pad_config->led_pulse_mask);
            } else if (!neopixel_has_custom_colors()) {
                // Single LED: use override color
                leds_set_color(r, g, b);
            }
        }
    }

    // Get current button state from router output (read ONCE, share with all consumers)
    // Persist across frames since router_get_output returns NULL when no new data
    static uint32_t buttons = 0;
    static uint32_t prev_buttons = 0;
    const input_event_t* event = router_get_output(OUTPUT_TARGET_USB_DEVICE, 0);
    if (event) {
        buttons = event->buttons;

        // Forward to linked controller via UART (if enabled)
        if (uart_link_enabled && event->dev_addr < 0xD0) {
            uart_device_queue_input(event, 0);
        }
    }
    if (buttons != prev_buttons) {
        printf("[app:controller] buttons=0x%08lx\n", (unsigned long)buttons);
        prev_buttons = buttons;
    }

    // Process codes detection (Konami code, etc.)
    codes_process_raw(buttons);

    // I2C peer: serve local inputs to master + consume master status
#ifdef I2C_PEER_ENABLED
    if (i2c_peer_enabled) {
        // Feed pad events to I2C peer slave (bypasses router tap
        // which requires player assignment — pad always has data to serve)
        const input_event_t* pad_ev = pad_input_get_event(0);
        if (pad_ev) {
            i2c_peer_slave_tap(OUTPUT_TARGET_USB_DEVICE, 0, pad_ev);
        }

        // Consume device status from master
        i2c_peer_status_t new_status;
        if (i2c_peer_slave_get_status(&new_status)) {
            peer_status = new_status;
            peer_status_valid = true;

            // Update LEDs with master's mode color
            if (pad_config->led_count > 1) {
                uint8_t colors[16][3];
                for (int i = 0; i < pad_config->led_count && i < 16; i++) {
                    colors[i][0] = new_status.mode_color[0];
                    colors[i][1] = new_status.mode_color[1];
                    colors[i][2] = new_status.mode_color[2];
                }
                neopixel_set_custom_colors(colors, pad_config->led_count);
                neopixel_set_pulse_mask(pad_config->led_pulse_mask);
            } else {
                leds_set_color(new_status.mode_color[0],
                               new_status.mode_color[1],
                               new_status.mode_color[2]);
            }

            // Mirror rumble via speaker
            if (speaker_is_initialized()) {
                uint8_t r = new_status.rumble_left > new_status.rumble_right
                          ? new_status.rumble_left : new_status.rumble_right;
                speaker_set_rumble(r);
            }
        }
    }
#endif

    // Process UART link communication (if enabled)
    if (uart_link_enabled) {
        uart_host_task();   // Receive inputs from linked controller
        uart_device_task(); // Send inputs to linked controller
    }

    // Get rumble value (skip when peer controls speaker)
    uint8_t rumble = 0;
#ifdef I2C_PEER_ENABLED
    if (peer_status_valid) {
        rumble = peer_status.rumble_left > peer_status.rumble_right
               ? peer_status.rumble_left : peer_status.rumble_right;
    } else
#endif
    {
        if (usbd_output_interface.get_rumble) {
            rumble = usbd_output_interface.get_rumble();
        }
        // Handle rumble feedback via speaker (if initialized)
        if (speaker_is_initialized()) {
            speaker_set_rumble(rumble);
        }
    }

    // Update LED press mask from button state
    if (pad_config->led_button_map[0]) {
        uint16_t press_mask = 0;
        for (int i = 0; i < pad_config->led_count && i < 16; i++) {
            if (pad_config->led_button_map[i] && (buttons & pad_config->led_button_map[i])) {
                press_mask |= (1 << i);
            }
        }
        neopixel_set_press_mask(press_mask);
    }

    // Update display
    update_display(rumble, buttons);
}
