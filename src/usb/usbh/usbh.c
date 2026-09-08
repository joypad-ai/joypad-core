// usbh.c - USB Host Layer
//
// Provides unified USB host handling across HID, X-input, and Bluetooth protocols.
// Device drivers read per-player feedback state from feedback_get_state().

#include "usbh.h"
#include "tusb.h"
#include "intel/intel_wireless_series.h"
#include "core/services/players/manager.h"
#include "core/services/codes/codes.h"
#include <stdio.h>

#if defined(CONFIG_MAX3421) && CFG_TUH_MAX3421
extern bool max3421_host_init(void);
extern void max3421_host_enable_int(void);
extern bool max3421_is_detected(void);
extern uint8_t max3421_get_revision(void);
#elif defined(CONFIG_USB) && CFG_TUH_RPI_PIO_USB
#include "pio_usb.h"
#include "hardware/gpio.h"
#endif

// HID protocol handlers
extern void hid_init(void);
extern void hid_task(void);

// X-input protocol handlers
extern void xinput_task(void);

// BTstack (Bluetooth) protocol handlers
#if CFG_TUH_BTD
#include "btd/hci_transport_h2_tinyusb.h"
#include "bt/transport/bt_transport.h"
extern const bt_transport_t bt_transport_usb;

// Runtime flag: only run BTstack loop when BT hardware is actually present.
// Avoids ~1-3ms overhead per main loop iteration when no dongle is connected.
static bool bt_hardware_present = false;

void usbh_set_bt_available(bool available)
{
    bt_hardware_present = available;
}
#endif

// PIO USB pin definitions (configurable per board).
//
// Source of truth is CMake compile_definitions, NOT the pico-sdk board
// header — those macros (e.g. ADAFRUIT_FEATHER_RP2040_USB_HOST) come from
// pico/stdlib.h which usbh.c doesn't include, so they aren't reliably
// visible here. PICO_DEFAULT_PIO_USB_DP_PIN / VBUSEN_PIN ARE on the
// compile command line per-target, so prefer those.
#if defined(CONFIG_PIO_USB_DP_PIN)
    #define PIO_USB_DP_PIN      CONFIG_PIO_USB_DP_PIN
#elif defined(PICO_DEFAULT_PIO_USB_DP_PIN)
    #define PIO_USB_DP_PIN      PICO_DEFAULT_PIO_USB_DP_PIN
#elif defined(ADAFRUIT_FEATHER_RP2040_USB_HOST)
    // Fallback if the board header is visible but no compile def was set
    #define PIO_USB_DP_PIN      16
#endif

#if defined(PICO_DEFAULT_PIO_USB_VBUSEN_PIN)
    #define PIO_USB_VBUS_PIN    PICO_DEFAULT_PIO_USB_VBUSEN_PIN
#elif defined(ADAFRUIT_FEATHER_RP2040_USB_HOST)
    #define PIO_USB_VBUS_PIN    18
#endif

#if defined(PICO_DEFAULT_PIO_USB_VBUSEN_STATE)
    #define PIO_USB_VBUS_ACTIVE PICO_DEFAULT_PIO_USB_VBUSEN_STATE
#else
    #define PIO_USB_VBUS_ACTIVE 1
#endif

// Runtime D+ pin override (set via pad config before usbh_init)
static int8_t pio_dp_pin_override = -1;

void usbh_set_pio_dp_pin(int8_t pin)
{
    pio_dp_pin_override = pin;
}

void usbh_init(void)
{
    printf("[usbh] Initializing USB host\n");

    hid_init();

#if defined(CONFIG_MAX3421) && CFG_TUH_MAX3421
    // MAX3421E SPI USB host on rhport 1
    if (max3421_host_init()) {
        tusb_rhport_init_t host_init = {
            .role = TUSB_ROLE_HOST,
            .speed = TUSB_SPEED_FULL
        };
        tusb_init(1, &host_init);
        // Enable INT pin interrupt AFTER tusb_init configures the chip,
        // otherwise a floating INT pin causes interrupt storm
        max3421_host_enable_int();
    } else {
        printf("[usbh] MAX3421E not detected, USB host disabled\n");
    }
#elif defined(PLATFORM_CH32)
    // CH32V307: native USBFS host on rhport 1 (USBHS device owns rhport 0).
    // The USBFS controller is wired as host by the BSP (family.c); the async
    // ch32 HCD (src/portable/wch/hcd_ch32_usbfs.c) drives it. Full speed only.
    {
        tusb_rhport_init_t host_init = {
            .role = TUSB_ROLE_HOST,
            .speed = TUSB_SPEED_FULL
        };
        tusb_init(BOARD_TUH_RHPORT, &host_init);
    }
#elif defined(CONFIG_USB) && CFG_TUH_RPI_PIO_USB
    // Dual USB mode: Host on rhport 1 (PIO USB for boards with separate host port)

#ifdef PIO_USB_VBUS_PIN
    // Enable VBUS power for USB-A port (required on Feather RP2040 USB Host
    // and any board with a VBUS load switch driven by an MCU GPIO).
    gpio_init(PIO_USB_VBUS_PIN);
    gpio_set_dir(PIO_USB_VBUS_PIN, GPIO_OUT);
    gpio_put(PIO_USB_VBUS_PIN, PIO_USB_VBUS_ACTIVE);
    printf("[usbh] Enabled VBUS on GPIO %d (drive=%d)\n",
           PIO_USB_VBUS_PIN, PIO_USB_VBUS_ACTIVE);
#endif

    // Configure PIO USB - use PIO0 when CYW43 is active (CYW43 SPI uses PIO1),
    // otherwise PIO1 (PIO0 is used by NeoPixel on non-CYW43 boards)
#ifdef BTSTACK_USE_CYW43
    #define PIO_USB_PIO_NUM 0
#else
    #define PIO_USB_PIO_NUM 1
#endif
    // Use high DMA channel to avoid conflict with CYW43 SPI driver,
    // which dynamically claims channels starting from 0
    #ifdef BTSTACK_USE_CYW43
    #define PIO_USB_DMA_CH 10
    #else
    #define PIO_USB_DMA_CH 0
    #endif
    pio_usb_configuration_t pio_cfg = {
        .pin_dp = PIO_USB_DP_PIN_DEFAULT,
        .pio_tx_num = PIO_USB_PIO_NUM,
        .sm_tx = 0,
        .tx_ch = PIO_USB_DMA_CH,
        .pio_rx_num = PIO_USB_PIO_NUM,
        .sm_rx = 1,
        .sm_eop = 2,
        .alarm_pool = NULL,
        .debug_pin_rx = -1,
        .debug_pin_eop = -1,
        .skip_alarm_pool = false,
        .pinout = PIO_USB_PINOUT_DPDM,
    };

#ifdef PIO_USB_DP_PIN
    pio_cfg.pin_dp = PIO_USB_DP_PIN;
#endif

    // Runtime override from pad config (web config GPIO settings)
    if (pio_dp_pin_override >= 0) {
        pio_cfg.pin_dp = pio_dp_pin_override;
    }

    printf("[usbh] PIO-USB D+ pin: GPIO %d\n", pio_cfg.pin_dp);

    // Configure TinyUSB PIO USB driver before initialization
    tuh_configure(1, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);

    tusb_rhport_init_t host_init = {
        .role = TUSB_ROLE_HOST,
        .speed = TUSB_SPEED_FULL  // PIO USB is Full Speed only
    };
    tusb_init(1, &host_init);
#elif defined(CONFIG_USB)
    // CONFIG_USB but no PIO USB - shouldn't happen but handle gracefully
    printf("[usbh] Warning: CONFIG_USB without PIO USB support\n");
#else
    // Single USB mode: Host on rhport 0 (native USB)
    {
        tusb_rhport_init_t host_init = {
            .role = TUSB_ROLE_HOST,
            .speed = TUSB_SPEED_FULL
        };
        tusb_init(0, &host_init);
    }
#endif

#if CFG_TUH_BTD
    // Initialize Bluetooth transport (for USB BT dongle support)
    bt_init(&bt_transport_usb);

    // Pico W has onboard BT - enable BTstack loop at startup
#if defined(CYW43_WL_GPIO_ON) || defined(PICO_CYW43_SUPPORTED)
    bt_hardware_present = true;
#endif
#endif

    printf("[usbh] Initialization complete\n");
}

void usbh_task(void)
{

    // TinyUSB host polling
    // On FreeRTOS, tuh_task() = tuh_task_ext(UINT32_MAX, false) which blocks forever.
#ifdef PLATFORM_ESP32
    tuh_task_ext(0, false);
#else
    tuh_task();
#endif

#if CFG_TUH_ENABLED
    intel_wireless_series_task();
#endif

#if CFG_TUH_XINPUT
    xinput_task();
#endif

#if CFG_TUH_HID
    hid_task();
#endif

#if CFG_TUH_BTD
    if (bt_hardware_present) {
        hci_transport_h2_tinyusb_process();
        bt_task();
    }
#endif
}

//--------------------------------------------------------------------+
// TinyUSB Callbacks
//--------------------------------------------------------------------+

static uint8_t usb_host_device_count = 0;

uint8_t usbh_get_device_count(void) { return usb_host_device_count; }

void tuh_mount_cb(uint8_t dev_addr)
{
    printf("A device with address %d is mounted\r\n", dev_addr);
    usb_host_device_count++;
}

void tuh_umount_cb(uint8_t dev_addr)
{
    printf("A device with address %d is unmounted\r\n", dev_addr);
    if (usb_host_device_count > 0) usb_host_device_count--;

#if CFG_TUH_ENABLED
    // TinyUSB invokes this before class close. Preserve player mappings
    // until the receiver has cleared every RF slot's routed output.
    intel_wireless_series_disconnect(dev_addr);
#endif
    remove_players_by_address(dev_addr, -1);

    // Reset test mode when device disconnects
    codes_reset_test_mode();
}

//--------------------------------------------------------------------+
// Input Interface
//--------------------------------------------------------------------+

static bool usbh_is_connected(void) { return usb_host_device_count > 0; }

const InputInterface usbh_input_interface = {
    .name = "USB Host",
    .source = INPUT_SOURCE_USB_HOST,
    .init = usbh_init,
    .task = usbh_task,
    .is_connected = usbh_is_connected,
    .get_device_count = usbh_get_device_count,
};
