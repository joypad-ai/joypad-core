/* 
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
 extern "C" {
#endif

//--------------------------------------------------------------------
// COMMON CONFIGURATION
//--------------------------------------------------------------------

// defined by compiler flags for flexibility
#ifndef CFG_TUSB_MCU
  #error CFG_TUSB_MCU must be defined
#endif

// USB role configuration
#if defined(DISABLE_USB_HOST)
  // Device-only mode (e.g., snes2usb - no USB host needed)
  #define CFG_TUSB_RHPORT0_MODE       OPT_MODE_DEVICE
#elif defined(CONFIG_USB)
  // Dual-role USB configuration (host + device)
  // Device mode on RHPORT0 (native USB), Host mode on RHPORT1
  #define CFG_TUSB_RHPORT0_MODE       OPT_MODE_DEVICE
  #define CFG_TUSB_RHPORT1_MODE       OPT_MODE_HOST
  #ifdef CONFIG_MAX3421
    #define CFG_TUH_MAX3421           1  // Enable MAX3421E SPI USB host driver
  #else
    #define CFG_TUH_RPI_PIO_USB       1  // Enable PIO USB host driver
  #endif
#elif defined(CONFIG_NGC) || defined(CONFIG_PCE)
  // GameCube / PCEngine: runtime host OR device on RHPORT0 (native USB)
  // Play mode (console detected): USB host for controllers
  // Config mode (no console): USB device with CDC for web configuration
  #define CFG_TUSB_RHPORT0_MODE       (OPT_MODE_HOST | OPT_MODE_DEVICE)
#else
  // Host-only mode for existing console implementations
  #ifdef CONFIG_MAX3421
    #define CFG_TUSB_RHPORT1_MODE     OPT_MODE_HOST
    #define CFG_TUH_MAX3421           1  // Enable MAX3421E SPI USB host driver
  #elif CFG_TUSB_MCU == OPT_MCU_LPC43XX || CFG_TUSB_MCU == OPT_MCU_LPC18XX || CFG_TUSB_MCU == OPT_MCU_MIMXRT10XX
    #define CFG_TUSB_RHPORT0_MODE     (OPT_MODE_HOST | OPT_MODE_HIGH_SPEED)
  #else
    #define CFG_TUSB_RHPORT0_MODE     OPT_MODE_HOST
  #endif
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS                 OPT_OS_NONE
#endif

// CFG_TUSB_DEBUG: defaults to 0, set JOYPAD_DEBUG=1 in .env to enable locally
#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG           0
#endif

/* USB DMA on some MCUs can only access a specific SRAM region with restriction on alignment.
 * Tinyusb use follows macros to declare transferring memory so that they can be put
 * into those specific section.
 * e.g
 * - CFG_TUD_MEM_SECTION : __attribute__ (( section(".usb_ram") ))
 * - CFG_TUD_MEM_ALIGN   : __attribute__ ((aligned(4)))
 */
#ifndef CFG_TUD_MEM_SECTION
#define CFG_TUD_MEM_SECTION
#endif

#ifndef CFG_TUD_MEM_ALIGN
#define CFG_TUD_MEM_ALIGN          __attribute__ ((aligned(4)))
#endif

//--------------------------------------------------------------------
// CONFIGURATION
//--------------------------------------------------------------------

// Size of buffer to hold descriptors and other data used for enumeration
#define CFG_TUH_ENUMERATION_BUFSIZE 1280

// PIO-USB (RP2040 software USB host) enumeration hardening. These override
// upstream TinyUSB defaults from the app side — the library keeps stock
// behavior for anyone who leaves them undefined. Needed for slow / marginal
// full-speed devices such as the Intel Wireless Series 8086:C013 receiver:
//  - Longer SET_ADDRESS recovery so the device answers the following
//    device-descriptor read (upstream default is the 2 ms spec minimum).
//  - Skip string-descriptor fetching during enumeration: strings are purely
//    informational, but a failed string control-IN aborts the whole enum
//    before any interface driver binds. Apps fetch strings lazily on demand
//    (see tuh_descriptor_get_product_string_sync in hid.c).
#define CFG_TUH_ENUM_SET_ADDRESS_RECOVERY_MS 100
#define CFG_TUH_ENUM_SKIP_STRINGS            1

#ifndef CFG_TUH_MEM_SECTION
#define CFG_TUH_MEM_SECTION
#endif

#ifndef CFG_TUH_MEM_ALIGN
#define CFG_TUH_MEM_ALIGN        __attribute__ ((aligned(4)))
#endif

// Number of hub devices the host tracks = how deep/wide a hub tree we allow.
// Many multi-port hubs (e.g. the PC Engine Mini 5-port hub) are two cascaded
// hub chips internally, so one such hub already counts as 2; 4 covers that plus
// a chained hub and a few honest tiers (USB tops out at 5). This is hub COUNT,
// independent of CFG_TUH_DEVICE_MAX below (device count). Hardware-proven nested
// case is 2 (PCE Mini); 4 is headroom. Overridable per-app (usb2dc lowers it).
#ifndef CFG_TUH_HUB
#define CFG_TUH_HUB                 4
#endif
#define CFG_TUH_CDC                 0
#define CFG_TUH_HID                 8   // Max 8 HID interfaces total (2 per device typical)
// Mass storage host: opt-in per target via CONFIG_USB_MSC. Default-off so
// targets that don't link msc_host.c don't drag in the TinyUSB MSC class.
#ifdef CONFIG_USB_MSC
#define CFG_TUH_MSC                 1
#else
#define CFG_TUH_MSC                 0
#endif
#define CFG_TUH_VENDOR              0
#define CFG_TUH_XINPUT              8   // Max 8 XInput interfaces (5 pads through a hub + Xbox wireless adapter ports)

// Bluetooth dongle support - only enabled when ENABLE_BTSTACK is defined by CMake
// CYW43 targets use built-in BT via pico_btstack, not USB dongle class
#ifdef ENABLE_BTSTACK
  #ifdef BTSTACK_USE_CYW43
    #define CFG_TUH_BTD             0
  #else
    #define CFG_TUH_BTD             1
  #endif
#else
#define CFG_TUH_BTD                 0
#endif

// Max NON-hub devices (controllers) the host tracks. This is the expensive
// knob — every per-device driver array is sized by it — so it's a fixed sane
// cap, decoupled from hub count (TinyUSB's default 4*CFG_TUH_HUB+1 wrongly
// inflates it with hub depth). 10 covers the most any console needs (3DO = 8
// players) plus margin for merge-mode accessibility setups; the 11th pad simply
// won't enumerate. Overridable per-app (usb2dc lowers it).
#ifndef CFG_TUH_DEVICE_MAX
#define CFG_TUH_DEVICE_MAX          10
#endif

// Enable endpoint transfer API with callback support (needed for Switch 2 bulk transfers)
#define CFG_TUH_API_EDPT_XFER       1

//------------- HID -------------//
#define CFG_TUH_HID_EPIN_BUFSIZE    64
#define CFG_TUH_HID_EPOUT_BUFSIZE   64

//--------------------------------------------------------------------
// USB DEVICE CONFIGURATION (CONFIG_USB or DISABLE_USB_HOST builds)
//--------------------------------------------------------------------

#if defined(CONFIG_USB) || defined(DISABLE_USB_HOST) || defined(CONFIG_NGC) || defined(CONFIG_BT2WIIEXT) || defined(CONFIG_PCE)
  // Device configuration
  #define CFG_TUD_ENDPOINT0_SIZE    64

#if defined(CONFIG_BT2N64) || defined(CONFIG_LODGENET2N64) || defined(CONFIG_NUONSERIAL)
  // CDC-only mode (no HID, no gamepad output)
  #define CFG_TUD_HID               0
  #define CFG_TUD_CDC               1
  #define CFG_TUD_MSC               0
  #define CFG_TUD_MIDI              0
  #define CFG_TUD_VENDOR            0
  #define CFG_TUD_CDC_RX_BUFSIZE    4096  // large: audio-command streaming (VOICE.SPEAK) must survive slow main-loop passes during BT audio
  #define CFG_TUD_CDC_TX_BUFSIZE    2048
  #define CFG_TUD_CDC_EP_BUFSIZE    64
#else
  // Standard HID gamepad mode (default)
  #define CFG_TUD_HID               4   // Up to 4 HID gamepads

  // Xbox Original (XID) mode support
  #define CFG_TUD_XID               1   // Enable XID class driver
  #define CFG_TUD_XID_EP_BUFSIZE    32  // XID endpoint buffer size

  // Xbox 360 (XInput) mode support
  #define CFG_TUD_XINPUT            1   // Enable XInput class driver
  #define CFG_TUD_XINPUT_EP_BUFSIZE 32  // XInput endpoint buffer size

  // GameCube Adapter mode support
  #define CFG_TUD_GC_ADAPTER        1   // Enable GC adapter class driver
  #define CFG_TUD_GC_ADAPTER_EP_BUFSIZE 37  // GC adapter endpoint buffer size (37 bytes)

  // CDC configuration: single data port (debug logs streamed as protocol events)
  #define CFG_TUD_CDC               1

  #define CFG_TUD_MSC               0   // No mass storage
  #define CFG_TUD_MIDI              0   // No MIDI
  // Vendor class is opt-in: only built into the device descriptor when
  // CONFIG_JOYBUS_BRIDGE is defined (the experimental USB-vendor
  // GBA-link transport to a forked Dolphin — see docs/GBA_LINK_CABLE.md
  // for status). Default builds don't pay the descriptor + endpoint
  // overhead.
  #ifdef CONFIG_JOYBUS_BRIDGE
    #define CFG_TUD_VENDOR            1
    // Larger FIFOs so we don't stall on Dolphin's multiboot bursts
    // (~13K WRITEs streamed at near-real-time pace).
    #define CFG_TUD_VENDOR_RX_BUFSIZE 1024
    #define CFG_TUD_VENDOR_TX_BUFSIZE 1024
    #define CFG_TUD_VENDOR_EPSIZE     64
  #else
    #define CFG_TUD_VENDOR            0
  #endif

  // HID buffer sizes
  #define CFG_TUD_HID_EP_BUFSIZE    64

  // CDC buffer sizes (TX enlarged for high-rate MouthPad NUS->CDC telemetry relay)
  #define CFG_TUD_CDC_RX_BUFSIZE    4096  // large: audio-command streaming (VOICE.SPEAK) must survive slow main-loop passes during BT audio
  #define CFG_TUD_CDC_TX_BUFSIZE    4096
  #define CFG_TUD_CDC_EP_BUFSIZE    64
#endif
#endif

#ifdef __cplusplus
 }
#endif

#endif /* _TUSB_CONFIG_H_ */
