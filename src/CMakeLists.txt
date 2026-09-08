
cmake_minimum_required(VERSION 3.12)

# Pull in SDK from local submodule (must be before project)
set(PICO_SDK_PATH ${CMAKE_CURRENT_LIST_DIR}/lib/pico-sdk)
include(${PICO_SDK_PATH}/pico_sdk_init.cmake)

project(joypad C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the SDK
pico_sdk_init()

set(FAMILY rp2040)

# Enable TinyUSB debug logging if JOYPAD_DEBUG is set (via .env or environment)
if(DEFINED ENV{JOYPAD_DEBUG})
    add_compile_definitions(CFG_TUSB_DEBUG=$ENV{JOYPAD_DEBUG})
endif()

# Experimental GBA-link USB bridge mode (forward raw joybus to a forked
# Dolphin via a USB vendor endpoint). OFF by default — see
# docs/GBA_LINK_CABLE.md for the status of this experiment. Builders who
# want to try it (with a matching Dolphin fork) can enable with:
#   cmake -DJOYPAD_ENABLE_GBA_LINK_BRIDGE=ON ..
# When ON, gc2usb / gc2eth targets pick up CONFIG_JOYBUS_BRIDGE, the
# vendor class gets added to the USB descriptor, and USB_OUTPUT_MODE_
# GBA_LINK (mode 14) becomes selectable in the CDC mode menu.
option(JOYPAD_ENABLE_GBA_LINK_BRIDGE
       "Build experimental GBA Link Cable USB bridge mode (see docs/GBA_LINK_CABLE.md)"
       OFF)
if(JOYPAD_ENABLE_GBA_LINK_BRIDGE)
    set(JOYPAD_GBA_LINK_BRIDGE_DEFS CONFIG_JOYBUS_BRIDGE=1)
    message(STATUS "joypad: GBA Link Cable USB bridge mode ENABLED (experimental)")
else()
    set(JOYPAD_GBA_LINK_BRIDGE_DEFS "")
endif()

# ============================================================================
# VERSION AND BUILD INFO
# ============================================================================

# Read version from VERSION file
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" JOYPAD_VERSION)
string(STRIP "${JOYPAD_VERSION}" JOYPAD_VERSION)

# Derive USB bcdDevice (0xMMmp) from JOYPAD_VERSION so the SInput descriptor — and
# thus the "dongle version" the MouthPad utility reads from bcdDevice — reflects
# the actual firmware version instead of a hardcoded default.
string(REPLACE "." ";" _JP_VER_PARTS "${JOYPAD_VERSION}")
list(LENGTH _JP_VER_PARTS _JP_VER_LEN)
if(_JP_VER_LEN GREATER_EQUAL 3)
    list(GET _JP_VER_PARTS 0 _JP_VER_MAJOR)
    list(GET _JP_VER_PARTS 1 _JP_VER_MINOR)
    list(GET _JP_VER_PARTS 2 _JP_VER_PATCH)
    math(EXPR JOYPAD_BCD_DEVICE "(${_JP_VER_MAJOR} << 8) | ((${_JP_VER_MINOR} & 15) << 4) | (${_JP_VER_PATCH} & 15)")
else()
    set(JOYPAD_BCD_DEVICE 256)  # 0x0100 fallback
endif()

# Get git commit hash
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT GIT_COMMIT)
    set(GIT_COMMIT "unknown")
endif()

# Get build timestamp
string(TIMESTAMP BUILD_TIME "%Y-%m-%d %H:%M:%S")

message(STATUS "Joypad Version: ${JOYPAD_VERSION}")
message(STATUS "Git Commit: ${GIT_COMMIT}")
message(STATUS "Build Time: ${BUILD_TIME}")

# External libraries
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/lib/tusb_xinput xinput_host)

# Pico-PIO-USB for dual USB (host on PIO, device on native)
set(PICO_PIO_USB_PATH ${CMAKE_CURRENT_LIST_DIR}/lib/Pico-PIO-USB)
if(EXISTS ${PICO_PIO_USB_PATH})
    add_library(tinyusb_pico_pio_usb INTERFACE)
    target_sources(tinyusb_pico_pio_usb INTERFACE
        ${PICO_PIO_USB_PATH}/src/pio_usb.c
        ${PICO_PIO_USB_PATH}/src/pio_usb_host.c
        ${PICO_PIO_USB_PATH}/src/pio_usb_device.c
        ${PICO_PIO_USB_PATH}/src/usb_crc.c
        ${CMAKE_CURRENT_LIST_DIR}/lib/tinyusb/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c
    )
    target_include_directories(tinyusb_pico_pio_usb INTERFACE ${PICO_PIO_USB_PATH}/src)
    target_link_libraries(tinyusb_pico_pio_usb INTERFACE hardware_pio hardware_dma pico_stdlib)
    target_compile_definitions(tinyusb_pico_pio_usb INTERFACE PIO_USB_USE_TINYUSB=1 PIO_USB_DP_PIN_DEFAULT=16)
    pico_generate_pio_header(tinyusb_pico_pio_usb ${PICO_PIO_USB_PATH}/src/usb_tx.pio)
    pico_generate_pio_header(tinyusb_pico_pio_usb ${PICO_PIO_USB_PATH}/src/usb_rx.pio)
endif()

# MAX3421E SPI USB Host (alternative to PIO USB)
add_library(tinyusb_max3421 INTERFACE)
target_sources(tinyusb_max3421 INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/lib/tinyusb/src/portable/analog/max3421/hcd_max3421.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/max3421_host.c
)
target_link_libraries(tinyusb_max3421 INTERFACE hardware_spi)
target_compile_definitions(tinyusb_max3421 INTERFACE CONFIG_MAX3421=1)

# ============================================================================
# SOURCE LISTS
# ============================================================================

# Core sources (no USB host - used by native-input apps)
set(CORE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/app_registry.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/router/router.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services/leds/leds.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services/leds/neopixel/ws2812.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services/leds/player_leds_gpio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services/storage/storage.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services/storage/flash.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services/button/button.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services/codes/codes.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services/keymap/keymap.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services/hotkeys/hotkeys.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services/players/manager.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services/players/feedback.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services/profiles/profile.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services/profiles/profile_indicator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services/profiles/runtime_profile.c
    ${CMAKE_CURRENT_SOURCE_DIR}/platform/rp2040/platform_rp2040.c
)

# USB Host sources (HID + X-input)
set(USB_HOST_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/intel/intel_wireless_series.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/usbh.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/hid.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/hid_utils.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/hid_registry.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/devices/generic/hid_keyboard.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/devices/generic/hid_mouse.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/devices/generic/hid_parser.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/devices/generic/hid_gamepad.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/devices/vendors/8bitdo/8bitdo_bta.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/devices/vendors/8bitdo/8bitdo_m30.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/devices/vendors/8bitdo/8bitdo_pce.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/devices/vendors/8bitdo/8bitdo_sn30.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/devices/vendors/elo/elo_vagabond.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/devices/vendors/nintendo/gamecube_adapter.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/devices/vendors/nintendo/switch_pro.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/devices/vendors/nintendo/switch2_pro.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/devices/vendors/sony/sony_ds3.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/devices/vendors/sony/sony_ds4.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/devices/vendors/sony/ds5_auth.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/devices/vendors/sony/p5general_host.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/devices/vendors/sony/sony_ds5.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/devices/vendors/sony/sony_psc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/devices/vendors/hori/hori_horipad.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/devices/vendors/hori/hori_pokken.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/devices/vendors/logitech/logitech_wingman.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/devices/vendors/sega/sega_astrocity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/devices/vendors/google/google_stadia.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/devices/vendors/raphnet/raphnet_pce.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/devices/vendors/microsoft/ms_sidewinder_dualstrike.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/devices/vendors/microsoft/ms_sidewinder_commander.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/devices/vendors/valve/steam_controller_2.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/devices/vendors/valve/steam_controller.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/devices/generic/sinput_host.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/xinput/xinput.c
)

# Bluetooth HID device drivers (shared between USB dongle and CYW43 transports)
set(BTHID_DEVICE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/bt/bthid/bthid.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bt/bthid/bthid_registry.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bt/bthid/devices/generic/bthid_gamepad.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bt/bthid/devices/generic/sinput_ble.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bt/bthid/devices/vendors/sony/ds3_bt.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bt/bthid/devices/vendors/sony/ds4_bt.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bt/bthid/devices/vendors/sony/ds5_bt.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bt/bthid/devices/vendors/nintendo/switch_pro_bt.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bt/bthid/devices/vendors/nintendo/switch2_ble.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bt/bthid/devices/vendors/nintendo/wii_u_pro_bt.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bt/bthid/devices/vendors/nintendo/wiimote_bt.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bt/bthid/devices/vendors/microsoft/xbox_bt.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bt/bthid/devices/vendors/microsoft/xbox_ble.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bt/bthid/devices/vendors/google/stadia_bt.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bt/bthid/devices/vendors/valve/steam_controller_2_ble.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bt/bthid/devices/vendors/valve/steam_controller_ble.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bt/bthid/devices/vendors/augmental/mouthpad_ble.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bt/bthid/devices/vendors/ipega/ipega_bt.c
    # MouthPad NUS<->CDC relay (ambient: self-attaches to CDC when linked + a
    # MouthPad is connected; inert otherwise). Companion to the mouthpad_ble HID
    # driver so every BLE app gains MouthPad support without a dedicated app.
    ${CMAKE_CURRENT_SOURCE_DIR}/bt/mouthpad/mp_relay.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bt/mouthpad/mp_bridge.c
)

# Bluetooth sources for USB dongle transport (requires BTstack)
set(BT_HOST_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/btd/hci_transport_h2_tinyusb.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bt/transport/bt_transport.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bt/transport/bt_transport_usb.c
    ${BTHID_DEVICE_SOURCES}
)

# BTstack sources
set(PICO_BTSTACK_PATH ${CMAKE_CURRENT_SOURCE_DIR}/lib/pico-sdk/lib/btstack)
set(BTSTACK_SOURCES
    # Core
    ${PICO_BTSTACK_PATH}/src/btstack_linked_list.c
    ${PICO_BTSTACK_PATH}/src/btstack_memory.c
    ${PICO_BTSTACK_PATH}/src/btstack_memory_pool.c
    ${PICO_BTSTACK_PATH}/src/btstack_run_loop.c
    ${PICO_BTSTACK_PATH}/src/btstack_run_loop_base.c
    ${PICO_BTSTACK_PATH}/src/btstack_util.c
    ${PICO_BTSTACK_PATH}/src/btstack_tlv.c
    ${PICO_BTSTACK_PATH}/src/btstack_tlv_none.c
    ${PICO_BTSTACK_PATH}/src/btstack_crypto.c
    ${PICO_BTSTACK_PATH}/src/hci.c
    ${PICO_BTSTACK_PATH}/src/hci_cmd.c
    ${PICO_BTSTACK_PATH}/src/hci_dump.c
    ${PICO_BTSTACK_PATH}/src/hci_event.c
    ${PICO_BTSTACK_PATH}/src/hci_event_builder.c
    ${PICO_BTSTACK_PATH}/src/l2cap.c
    ${PICO_BTSTACK_PATH}/src/l2cap_signaling.c
    ${PICO_BTSTACK_PATH}/src/ad_parser.c
    # BLE
    ${PICO_BTSTACK_PATH}/src/ble/sm.c
    ${PICO_BTSTACK_PATH}/src/ble/att_dispatch.c
    ${PICO_BTSTACK_PATH}/src/ble/att_db.c
    ${PICO_BTSTACK_PATH}/src/ble/att_db_util.c
    ${PICO_BTSTACK_PATH}/src/ble/att_server.c
    ${PICO_BTSTACK_PATH}/src/ble/gatt_client.c
    ${PICO_BTSTACK_PATH}/src/ble/gatt_service_client.c
    ${PICO_BTSTACK_PATH}/src/ble/le_device_db_memory.c
    ${PICO_BTSTACK_PATH}/src/ble/le_device_db_tlv.c
    # BLE GATT services
    ${PICO_BTSTACK_PATH}/src/ble/gatt-service/hids_client.c
    ${PICO_BTSTACK_PATH}/src/ble/gatt-service/device_information_service_client.c
    ${PICO_BTSTACK_PATH}/src/ble/gatt-service/battery_service_client.c
    # Classic (for HID Host)
    ${PICO_BTSTACK_PATH}/src/classic/sdp_client.c
    ${PICO_BTSTACK_PATH}/src/classic/sdp_server.c
    ${PICO_BTSTACK_PATH}/src/classic/sdp_util.c
    ${PICO_BTSTACK_PATH}/src/classic/device_id_server.c
    ${PICO_BTSTACK_PATH}/src/classic/hid_host.c
    ${PICO_BTSTACK_PATH}/src/classic/btstack_link_key_db_memory.c
    ${PICO_BTSTACK_PATH}/src/classic/btstack_link_key_db_tlv.c
    # 3rd party
    ${PICO_BTSTACK_PATH}/3rd-party/micro-ecc/uECC.c
    ${PICO_BTSTACK_PATH}/3rd-party/rijndael/rijndael.c
    # Platform
    ${PICO_BTSTACK_PATH}/platform/embedded/btstack_run_loop_embedded.c
    ${PICO_BTSTACK_PATH}/platform/embedded/hci_dump_embedded_stdout.c
    ${PICO_BTSTACK_PATH}/platform/embedded/btstack_tlv_flash_bank.c
    # Pico SDK flash bank HAL (for persistent link key storage)
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/pico-sdk/src/rp2_common/pico_btstack/btstack_flash_bank.c
)

# USB Device sources (for USB output apps)
set(USB_DEVICE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd/usbd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd/modes/hid_mode.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd/modes/sinput_mode.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd/modes/xinput_mode.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd/modes/switch_mode.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd/modes/ps3_mode.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd/modes/psclassic_mode.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd/modes/pcemini_mode.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd/modes/ps4_mode.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd/modes/dualsense_mode.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd/modes/p5general_mode.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd/modes/xid_mode.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd/modes/xbone_mode.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd/modes/xac_mode.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd/modes/kbmouse_mode.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd/modes/gc_adapter_mode.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd/modes/gba_link_mode.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd/cdc/cdc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd/cdc/cdc_protocol.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd/cdc/cdc_commands.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd/drivers/tud_xid.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd/drivers/tud_xinput.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd/drivers/tud_xbone.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd/drivers/xgip_protocol.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/xbone_auth/xbone_auth.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd/kbmouse/kbmouse.c
    # libxsm3 - Xbox Security Method 3 (LGPL-2.1-or-later)
    # https://github.com/InvoxiPlayGames/libxsm3
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/libxsm3/xsm3.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/libxsm3/usbdsec.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/libxsm3/excrypt_des.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/libxsm3/excrypt_parve.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/libxsm3/excrypt_sha.c
    # PS4 auth flash storage — always compiled so PS4AUTH CDC commands are available
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services/storage/ps4_auth_flash.c
    # PS4 event log — flash-persistent debug log for auth troubleshooting
)

# PS4 local auth sources (RSA signing via mbedTLS, optional)
# Requires pico-sdk submodules to be initialized: make init
# Enabled by default for USB device apps; disable with ENABLE_PS4_LOCAL_AUTH=OFF
option(ENABLE_PS4_LOCAL_AUTH "Enable PS4 local RSA authentication (requires pico_mbedtls)" ON)

set(PS4_LOCAL_AUTH_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd/modes/ps4_local_auth.c
)

# NES host sources
set(NES_HOST_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/nes/nes_host.c
)

# PCEngine host sources
set(PCE_HOST_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/pcengine/pce_host.c
)

# Jaguar host sources
set(JAGUAR_HOST_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/jaguar/jaguar_host.c
)

# SNES host sources
set(SNES_HOST_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/snes/snes_host.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/SNESpad/src/snespad.c
)

set(PSX_HOST_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/psx/psx_host.c
)

# N64 host sources
set(N64_HOST_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/n64/n64_host.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/src/joybus.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/src/N64Controller.c
)

# GC host sources (GameCube controller reading + GBA-as-controller bridge)
set(GC_HOST_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/gc/gc_host.c
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/gc/gba_multiboot.c
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/gc/gba_payload.c
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/gc/joybus_bridge.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/src/joybus.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/src/GamecubeController.c
)

# LodgeNet host sources
set(LODGENET_HOST_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/lodgenet/lodgenet_host.c
)

# Wii extension host sources (hardware I2C via platform HAL + protocol lib)
set(WII_HOST_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/wii_ext/wii_ext_host.c
    ${CMAKE_CURRENT_SOURCE_DIR}/platform/rp2040/platform_i2c_rp2040.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/wii_ext/wii_ext.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/wii_ext/ext_nunchuck.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/wii_ext/ext_classic.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/wii_ext/ext_guitar.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/wii_ext/ext_drums.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/wii_ext/ext_turntable.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/wii_ext/ext_taiko.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/wii_ext/ext_udraw.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/wii_ext/ext_motionplus.c
)

# Nuon host sources (polyface master)
# TODO: uncomment when nuon_host.c is implemented
# set(NUON_HOST_SOURCES
#     ${CMAKE_CURRENT_SOURCE_DIR}/native/host/nuon/nuon_host.c
# )

# I2C peer input sharing sources (STEMMA QT)
set(I2C_PEER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/i2c_peer/i2c_peer.c
)

# UART peer input sharing sources (inter-MCU link, dual-RP2040 boards)
set(UART_PEER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/uart_peer/uart_peer.c
)

# Controller app common sources
set(CONTROLLER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/pad/pad_input.c
    ${CMAKE_CURRENT_SOURCE_DIR}/pad/pad_config_flash.c
    ${CMAKE_CURRENT_SOURCE_DIR}/pad/pad_config_storage_rp2040.c
    ${CMAKE_CURRENT_SOURCE_DIR}/platform/rp2040/platform_gpio_rp2040.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services/speaker/speaker.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services/display/display.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services/display/display_spi.c
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/uart/uart_device.c
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/uart/uart_host.c
)

# Common sources (core + USB host)
# SD card sources — opt-in via ${SD_SOURCES} in target_sources + lib/fatfs in target_include_directories
set(SD_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/fatfs/ff.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/fatfs/ffsystem.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/fatfs/ffunicode.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services/sd/sd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services/sd/diskio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/platform/rp2040/platform_sd_rp2040.c
)

set(COMMON_SOURCES ${CORE_SOURCES} ${USB_HOST_SOURCES})

# Common libraries for USB host apps
set(COMMON_LIBRARIES pico_stdlib pico_multicore hardware_pio pico_rand tinyusb_host tinyusb_board xinput_host)

# BTstack compile definitions (shared by all USB host apps)
# Note: Most BTstack feature flags are in bt/btstack/btstack_config.h
# Only ENABLE_BTSTACK is needed here to enable CFG_TUH_BTD in tusb_config.h
set(BTSTACK_DEFINITIONS
    ENABLE_BTSTACK=1
    # joypad_add_btstack() always adds BT_HOST_SOURCES, which contains
    # BTHID_DEVICE_SOURCES (incl. bthid_registry.c), so every BTstack target on
    # this platform is a BTHID host. bt_transport.c gates its registry call on
    # this instead of on a weak symbol — see the comment there.
    CONFIG_BT_HOST=1
)

# BTstack include directories
set(BTSTACK_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/bt/btstack
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/btd
    ${PICO_BTSTACK_PATH}/src
    ${PICO_BTSTACK_PATH}/src/ble
    ${PICO_BTSTACK_PATH}/platform/embedded
    ${PICO_BTSTACK_PATH}/3rd-party/micro-ecc
    ${PICO_BTSTACK_PATH}/3rd-party/rijndael
    # Pico SDK BTstack flash bank HAL
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/pico-sdk/src/rp2_common/pico_btstack/include
)

# Additional BTstack sources (beyond BT_HOST_SOURCES and BTSTACK_SOURCES)
set(BTSTACK_EXTRA_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/bt/btstack/btstack_host.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bt/btstack/bt_device_db.c
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/btd/btstack_hal.c
)

# Controller libraries
set(CONTROLLER_LIBRARIES pico_stdlib pico_multicore hardware_pio hardware_adc hardware_i2c hardware_pwm hardware_spi pico_rand tinyusb_device tinyusb_board)

# ============================================================================
# HELPER FUNCTIONS
# ============================================================================

# Common setup for all targets
function(joypad_target_common TARGET)
    target_compile_options(${TARGET} PRIVATE -O3)
    pico_add_extra_outputs(${TARGET})
    pico_generate_pio_header(${TARGET} ${CMAKE_CURRENT_LIST_DIR}/core/services/leds/neopixel/ws2812.pio)
    target_include_directories(${TARGET} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/common ${CMAKE_CURRENT_SOURCE_DIR}/lib/libxsm3)
    # Add version and build info to all targets
    target_compile_definitions(${TARGET} PRIVATE
        JOYPAD_VERSION="${JOYPAD_VERSION}"
        GIT_COMMIT="${GIT_COMMIT}"
        BUILD_TIME="${BUILD_TIME}"
        SINPUT_BCD_DEVICE=${JOYPAD_BCD_DEVICE}
    )
    # Set BOARD_NAME only if not already defined by the target
    get_target_property(_defs ${TARGET} COMPILE_DEFINITIONS)
    set(_has_board_name FALSE)
    if(_defs)
        foreach(_def ${_defs})
            if(_def MATCHES "^BOARD_NAME=")
                set(_has_board_name TRUE)
            endif()
        endforeach()
    endif()
    if(NOT _has_board_name)
        target_compile_definitions(${TARGET} PRIVATE BOARD_NAME="${PICO_BOARD}")
    endif()
endfunction()

# Add PS4 local auth (mbedTLS RSA signing) to a target
# Only added to targets that include USB_DEVICE_SOURCES (USB output apps)
function(joypad_add_ps4_local_auth TARGET)
    if(NOT ENABLE_PS4_LOCAL_AUTH)
        return()
    endif()

    # pico_mbedtls is provided by pico-sdk (lib/mbedtls submodule).
    # It must be available: run 'make init' to initialize all submodules.
    if(NOT TARGET pico_mbedtls)
        message(WARNING
            "pico_mbedtls not found — PS4 local auth disabled for ${TARGET}. "
            "Run 'make init' to initialize pico-sdk submodules.")
        return()
    endif()

    target_sources(${TARGET} PRIVATE ${PS4_LOCAL_AUTH_SOURCES})
    target_compile_definitions(${TARGET} PRIVATE
        ENABLE_PS4_LOCAL_AUTH=1
        MBEDTLS_CONFIG_FILE="ps4_mbedtls_config.h"
    )
    # Include the modes directory so MBEDTLS_CONFIG_FILE is found
    target_include_directories(${TARGET} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd/modes
    )
    target_link_libraries(${TARGET} PRIVATE pico_mbedtls pico_rand)
    message(STATUS "${TARGET}: PS4 local auth enabled (pico_mbedtls)")
endfunction()

# Add BTstack support to a target
function(joypad_add_btstack TARGET)
    target_compile_definitions(${TARGET} PRIVATE ${BTSTACK_DEFINITIONS})
    target_sources(${TARGET} PRIVATE
        ${BT_HOST_SOURCES}
        ${BTSTACK_SOURCES}
        ${BTSTACK_EXTRA_SOURCES}
    )
    target_include_directories(${TARGET} PUBLIC ${BTSTACK_INCLUDES})
    # Link hardware_flash and pico_flash for persistent link key storage
    target_link_libraries(${TARGET} PRIVATE hardware_flash pico_flash)
endfunction()

# USB host console app (USB input -> console output)
function(joypad_console_app TARGET CONFIG_NAME APP_DIR DEVICE_DIR)
    add_executable(${TARGET})
    target_compile_definitions(${TARGET} PRIVATE CONFIG_${CONFIG_NAME}=1)
    target_sources(${TARGET} PUBLIC ${COMMON_SOURCES}
        ${CMAKE_CURRENT_SOURCE_DIR}/native/device/${DEVICE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/${APP_DIR}/app.c
    )
    target_include_directories(${TARGET} PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/${APP_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/native/device/${DEVICE_DIR}
    )
    joypad_target_common(${TARGET})
endfunction()

# ============================================================================
# BOARD CONFIGURATION
# ============================================================================

if (PICO_BOARD STREQUAL "adafruit_kb2040")
    set(BOARD_BUTTON_GPIO 11)
elseif (PICO_BOARD STREQUAL "adafruit_qtpy_rp2040")
    set(BOARD_BUTTON_GPIO 21)
elseif (PICO_BOARD STREQUAL "adafruit_macropad_rp2040")
    set(BOARD_BUTTON_GPIO 0)
elseif (PICO_BOARD STREQUAL "adafruit_feather_rp2040")
    set(BOARD_BUTTON_GPIO 7)
else()
    set(BOARD_BUTTON_GPIO 7)
endif()
message(STATUS "Board ${PICO_BOARD}: BUTTON_USER_GPIO=${BOARD_BUTTON_GPIO}")

# ============================================================================
# USB HOST CONSOLE APPS (USB input -> retro console output)
# ============================================================================

# --- PCEngine ---
add_executable(joypad_pce)
target_compile_definitions(joypad_pce PRIVATE CONFIG_PCE=1)
# usb2pce drives the PCE multitap (5 players) reached through cascaded hubs (the
# PCE Mini 5-port hub is 2 hub chips + an optional chained hub). The global
# defaults (hub depth 4, device cap 10) cover that, so no per-app override.
if (PICO_BOARD STREQUAL "pico")
    target_compile_definitions(joypad_pce PRIVATE RPI_PICO_BUILD=1)
endif()
target_sources(joypad_pce PUBLIC ${COMMON_SOURCES}
    ${USB_DEVICE_SOURCES}    # USB device / CDC for web-config mode (off-console)
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/pcengine/pcengine_device.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2pce/app.c
)
target_include_directories(joypad_pce PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2pce
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/pcengine
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd   # descriptors/*.h for CDC config-mode device
)
target_link_libraries(joypad_pce PRIVATE ${COMMON_LIBRARIES} tinyusb_device)
joypad_target_common(joypad_pce)
joypad_add_btstack(joypad_pce)
pico_generate_pio_header(joypad_pce ${CMAKE_CURRENT_LIST_DIR}/native/device/pcengine/plex.pio)
pico_generate_pio_header(joypad_pce ${CMAKE_CURRENT_LIST_DIR}/native/device/pcengine/clock.pio)
pico_generate_pio_header(joypad_pce ${CMAKE_CURRENT_LIST_DIR}/native/device/pcengine/select.pio)

# --- GameCube (KB2040) ---
# CDC config mode: when no GC 3.3V detected, boots as USB device with CDC for web config
add_executable(joypad_ngc)
target_compile_definitions(joypad_ngc PRIVATE CONFIG_NGC=1 CONFIG_USB_HOST=1)
target_sources(joypad_ngc PUBLIC ${COMMON_SOURCES}
    ${USB_DEVICE_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/gamecube/gamecube_device.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/src/joybus.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/src/GamecubeConsole.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2gc/app.c
)
target_include_directories(joypad_ngc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2gc
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/gamecube
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/include
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
)
target_link_libraries(joypad_ngc PRIVATE ${COMMON_LIBRARIES} hardware_flash tinyusb_device)
joypad_target_common(joypad_ngc)
joypad_add_btstack(joypad_ngc)
pico_generate_pio_header(joypad_ngc ${CMAKE_CURRENT_LIST_DIR}/lib/joybus-pio/src/joybus.pio)

# --- GameCube (RP2040-Zero) ---
# Note: Uses native USB host (not PIO USB) to avoid PIO conflict with joybus
# Override default I2C pins (6/7) to avoid conflict with GameCube pins (GC_DATA=7, GC_3V3=6)
# CDC config mode: when no GC 3.3V detected, boots as USB device with CDC for web config
add_executable(joypad_ngc_rp2040zero)
target_compile_definitions(joypad_ngc_rp2040zero PRIVATE
    CONFIG_NGC=1 CONFIG_USB_HOST=1
    WS2812_PIN=16
    USE_BOOTSEL_BUTTON=1
    PICO_DEFAULT_I2C_SDA_PIN=14
    PICO_DEFAULT_I2C_SCL_PIN=15
)
target_sources(joypad_ngc_rp2040zero PUBLIC ${COMMON_SOURCES}
    ${USB_DEVICE_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/gamecube/gamecube_device.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/src/joybus.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/src/GamecubeConsole.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2gc/app.c
)
target_include_directories(joypad_ngc_rp2040zero PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2gc
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/gamecube
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/include
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
)
target_link_libraries(joypad_ngc_rp2040zero PRIVATE ${COMMON_LIBRARIES} hardware_flash tinyusb_device)
joypad_target_common(joypad_ngc_rp2040zero)
joypad_add_btstack(joypad_ngc_rp2040zero)
pico_generate_pio_header(joypad_ngc_rp2040zero ${CMAKE_CURRENT_LIST_DIR}/lib/joybus-pio/src/joybus.pio)

# --- Nuon Serial Adapter ---
# Polyface TYPE=0x04 serial device for Nuon homebrew debug logging.
# No USB host — uses USB device CDC to bridge SDATA register to PC terminal.
add_executable(joypad_nuonserial)
target_compile_definitions(joypad_nuonserial PRIVATE
    CONFIG_NUONSERIAL=1
    CONFIG_NO_NEOPIXEL=1
    CONFIG_NO_FLASH_LOCKOUT=1
    DISABLE_USB_HOST=1
)
target_sources(joypad_nuonserial PUBLIC ${CORE_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/nuon/nuon_serial_device.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/nuonserial/app.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/nuonserial/usb_descriptors.c
)
target_include_directories(joypad_nuonserial PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/nuonserial
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/nuon
)
target_link_libraries(joypad_nuonserial PRIVATE
    pico_stdlib pico_multicore hardware_pio pico_rand pico_bit_ops
    tinyusb_device tinyusb_board pico_unique_id
)
joypad_target_common(joypad_nuonserial)
pico_generate_pio_header(joypad_nuonserial ${CMAKE_CURRENT_LIST_DIR}/native/device/nuon/polyface_read.pio)
pico_generate_pio_header(joypad_nuonserial ${CMAKE_CURRENT_LIST_DIR}/native/device/nuon/polyface_send.pio)

# --- Nuon ---
add_executable(joypad_nuon)
target_compile_definitions(joypad_nuon PRIVATE CONFIG_NUON=1 CONFIG_NO_NEOPIXEL=1)
target_sources(joypad_nuon PUBLIC ${COMMON_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/nuon/nuon_device.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2nuon/app.c
)
target_include_directories(joypad_nuon PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2nuon
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/nuon
)
target_link_libraries(joypad_nuon PRIVATE ${COMMON_LIBRARIES} pico_bit_ops)
joypad_target_common(joypad_nuon)
joypad_add_btstack(joypad_nuon)
pico_generate_pio_header(joypad_nuon ${CMAKE_CURRENT_LIST_DIR}/native/device/nuon/polyface_read.pio)
pico_generate_pio_header(joypad_nuon ${CMAKE_CURRENT_LIST_DIR}/native/device/nuon/polyface_send.pio)

# --- N64 ---
add_executable(joypad_n64)
target_compile_definitions(joypad_n64 PRIVATE CONFIG_N64=1)
target_sources(joypad_n64 PUBLIC ${COMMON_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/n64/n64_device.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/src/joybus.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/src/N64Console.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2n64/app.c
)
target_include_directories(joypad_n64 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2n64
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/n64
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/include
)
target_link_libraries(joypad_n64 PRIVATE ${COMMON_LIBRARIES} hardware_flash)
joypad_target_common(joypad_n64)
joypad_add_btstack(joypad_n64)
pico_generate_pio_header(joypad_n64 ${CMAKE_CURRENT_LIST_DIR}/lib/joybus-pio/src/joybus.pio)

# --- Loopy ---
add_executable(joypad_loopy)
target_compile_definitions(joypad_loopy PRIVATE CONFIG_LOOPY=1)
target_sources(joypad_loopy PUBLIC ${COMMON_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/loopy/loopy_device.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2loopy/app.c
)
target_include_directories(joypad_loopy PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2loopy
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/loopy
)
target_link_libraries(joypad_loopy PRIVATE ${COMMON_LIBRARIES} pico_bit_ops)
joypad_target_common(joypad_loopy)
joypad_add_btstack(joypad_loopy)
pico_generate_pio_header(joypad_loopy ${CMAKE_CURRENT_LIST_DIR}/native/device/loopy/loopy.pio)

# --- Dreamcast ---
add_executable(joypad_dc)
# WORKAROUND: KB2040 + Core-1 TX hits an enumeration-dropoff bug we haven't
# root-caused (works fine on RP2040 Zero where gill validated the PR).
# usb2dc ARCHITECTURE lives in apps/usb2dc/usb2dc_config.h and is force-included
# into every translation unit of these targets, so the shared Dreamcast device
# code sees it too (not just app.c). Board targets below carry ONLY GPIO /
# peripherals — never architecture flags — so they can't drift (a kb2040 drift
# off CONFIG_DC_CORE1_TX was the root cause of the Maple-drops-on-VMU-save bug).
set(USB2DC_FORCE_CONFIG "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2dc/usb2dc_config.h")
# kb2040: default Maple pins (GP2/GP3), no NeoPixel.
target_compile_definitions(joypad_dc PRIVATE CONFIG_NO_NEOPIXEL=1)
target_compile_options(joypad_dc PRIVATE ${USB2DC_FORCE_CONFIG})
target_sources(joypad_dc PUBLIC ${COMMON_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/dreamcast/dreamcast_device.c
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/dreamcast/maple_state_machine.c
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/dreamcast/vmu.c
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/dreamcast/vmu_sd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/dreamcast/vmu_storage.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2dc/app.c
)
target_include_directories(joypad_dc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2dc
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/dreamcast
)
target_link_libraries(joypad_dc PRIVATE ${COMMON_LIBRARIES} hardware_dma)
joypad_target_common(joypad_dc)
joypad_add_btstack(joypad_dc)
pico_generate_pio_header(joypad_dc ${CMAKE_CURRENT_LIST_DIR}/native/device/dreamcast/maple.pio)

# --- Dreamcast (RP2040-Zero) ---
# USB4Maple-compatible pinout: GPIO 14/15 for Maple Bus
add_executable(joypad_dc_rp2040zero)
target_compile_definitions(joypad_dc_rp2040zero PRIVATE
    # board GPIO / peripherals only — architecture is force-included below
    MAPLE_PIN1=14 MAPLE_PIN5=15   # USB4Maple-compatible Maple Bus pins
    WS2812_PIN=16                 # onboard NeoPixel (kb2040 has none)
    USE_BOOTSEL_BUTTON=1
    # SD card (SPI1: SCK=GP10 MOSI=GP11 MISO=GP12 CS=GP13) is ONLY used for VMU
    # persistence, which is disabled for release — leaving it off keeps both
    # RP2040 usb2dc builds identical except GPIO. Re-enable alongside CONFIG_VMU.
    # CONFIG_SD=1 SD_SPI_INST=1 SD_SCK_PIN=10 SD_MOSI_PIN=11 SD_MISO_PIN=12 SD_CS_PIN=13
)
target_compile_options(joypad_dc_rp2040zero PRIVATE ${USB2DC_FORCE_CONFIG})
target_sources(joypad_dc_rp2040zero PUBLIC ${COMMON_SOURCES} ${SD_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/dreamcast/dreamcast_device.c
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/dreamcast/maple_state_machine.c
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/dreamcast/vmu.c
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/dreamcast/vmu_sd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/dreamcast/vmu_storage.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2dc/app.c
)
target_include_directories(joypad_dc_rp2040zero PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2dc
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/dreamcast
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/fatfs
)
target_link_libraries(joypad_dc_rp2040zero PRIVATE ${COMMON_LIBRARIES} hardware_dma hardware_spi pico_multicore)
joypad_target_common(joypad_dc_rp2040zero)
joypad_add_btstack(joypad_dc_rp2040zero)
pico_generate_pio_header(joypad_dc_rp2040zero ${CMAKE_CURRENT_LIST_DIR}/native/device/dreamcast/maple.pio)

# --- NEOGEO ---
add_executable(joypad_neogeo)
target_compile_definitions(joypad_neogeo PRIVATE CONFIG_NEOGEO=1)
target_sources(joypad_neogeo PUBLIC ${COMMON_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/gpio/gpio_device.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2neogeo/app.c
)
target_include_directories(joypad_neogeo PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2neogeo
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/gpio
)
target_link_libraries(joypad_neogeo PRIVATE ${COMMON_LIBRARIES})
joypad_target_common(joypad_neogeo)
joypad_add_btstack(joypad_neogeo)

# Neo Geo — Pi Pico (WS2812 PIO on GP25 drives onboard LED as on/off indicator)
add_executable(joypad_neogeo_pico)
target_compile_definitions(joypad_neogeo_pico PRIVATE
    CONFIG_NEOGEO=1
    WS2812_PIN=25
    USE_BOOTSEL_BUTTON=1
    RPI_PICO_BUILD=1
)
target_sources(joypad_neogeo_pico PUBLIC ${COMMON_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/gpio/gpio_device.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2neogeo/app.c
)
target_include_directories(joypad_neogeo_pico PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2neogeo
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/neogeo
)
target_link_libraries(joypad_neogeo_pico PRIVATE ${COMMON_LIBRARIES})
joypad_target_common(joypad_neogeo_pico)
joypad_add_btstack(joypad_neogeo_pico)

# Neo Geo — RP2040-Zero (WS2812 on GP16, auto from board def)
add_executable(joypad_neogeo_rp2040zero)
target_compile_definitions(joypad_neogeo_rp2040zero PRIVATE
    CONFIG_NEOGEO=1
    PICO_RP2040_ZERO_BUILD=1
)
target_sources(joypad_neogeo_rp2040zero PUBLIC ${COMMON_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/gpio/gpio_device.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2neogeo/app.c
)
target_include_directories(joypad_neogeo_rp2040zero PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2neogeo
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/neogeo
)
target_link_libraries(joypad_neogeo_rp2040zero PRIVATE ${COMMON_LIBRARIES})
joypad_target_common(joypad_neogeo_rp2040zero)
joypad_add_btstack(joypad_neogeo_rp2040zero)

add_executable(joypad_neogeo_retrofrog)
target_compile_definitions(joypad_neogeo_retrofrog PRIVATE
    CONFIG_NEOGEO=1
    PICO_RP2040_ZERO_BUILD=1
)
target_sources(joypad_neogeo_retrofrog PUBLIC ${COMMON_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/gpio/gpio_device.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2neogeo/app.c
)
target_include_directories(joypad_neogeo_retrofrog PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2neogeo
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/neogeo
)
target_link_libraries(joypad_neogeo_retrofrog PRIVATE ${COMMON_LIBRARIES})
joypad_target_common(joypad_neogeo_retrofrog)
joypad_add_btstack(joypad_neogeo_retrofrog)

# --- N64 to Dreamcast ---
add_executable(joypad_n642dc)
# Core 0 TX mode required: joybus uses PIO1 SM3, conflicts with Core 1 TX
# Core 0: N64 joybus polling + maple TX, Core 1: Maple RX only
target_compile_definitions(joypad_n642dc PRIVATE CONFIG_N642DC=1 CONFIG_DC=1 CONFIG_NO_NEOPIXEL=1 N64_PIN_DATA=29)
target_sources(joypad_n642dc PUBLIC ${COMMON_SOURCES}
    ${N64_HOST_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/dreamcast/dreamcast_device.c
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/dreamcast/maple_state_machine.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/n642dc/app.c
)
target_include_directories(joypad_n642dc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/n642dc
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/n64
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/dreamcast
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/include
)
target_link_libraries(joypad_n642dc PRIVATE ${COMMON_LIBRARIES} hardware_dma)
joypad_target_common(joypad_n642dc)
# No BTstack for n642dc - uses native N64 input, not USB host
pico_generate_pio_header(joypad_n642dc ${CMAKE_CURRENT_LIST_DIR}/lib/joybus-pio/src/joybus.pio)
pico_generate_pio_header(joypad_n642dc ${CMAKE_CURRENT_LIST_DIR}/native/device/dreamcast/maple.pio)

# --- GameCube to Dreamcast (KB2040) ---
# Same PIO layout as n642dc — gc_host shares joybus.pio with the N64 driver,
# so it uses PIO1 SM3 at offset 10 and leaves PIO1 SM0-2 for maple_rx.
# Core-0 TX required for the same reason (joybus on PIO1 SM3).
add_executable(joypad_gc2dc)
target_compile_definitions(joypad_gc2dc PRIVATE CONFIG_GC2DC=1 CONFIG_DC=1 CONFIG_NO_NEOPIXEL=1 GC_PIN_DATA=29)
target_sources(joypad_gc2dc PUBLIC ${COMMON_SOURCES}
    ${GC_HOST_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/dreamcast/dreamcast_device.c
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/dreamcast/maple_state_machine.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/gc2dc/app.c
)
target_include_directories(joypad_gc2dc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/gc2dc
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/gc
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/dreamcast
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/include
)
target_link_libraries(joypad_gc2dc PRIVATE ${COMMON_LIBRARIES} hardware_dma)
joypad_target_common(joypad_gc2dc)
# No BTstack — native GC input, no USB host.
pico_generate_pio_header(joypad_gc2dc ${CMAKE_CURRENT_LIST_DIR}/lib/joybus-pio/src/joybus.pio)
pico_generate_pio_header(joypad_gc2dc ${CMAKE_CURRENT_LIST_DIR}/native/device/dreamcast/maple.pio)

# --- N64 to Dreamcast (Pico 2 W) ---
# Pinout: DC SDCKA=GPIO14, SDCKB=GPIO15 (physical pins 19/20)
#         N64 Data=GPIO18 (physical pin 24)
#         Power: DC 5V=VSYS(pin39), N64 3.3V=3V3OUT(pin36), GND=pins 23/38
add_executable(joypad_n642dc_pico2_w)
target_compile_definitions(joypad_n642dc_pico2_w PRIVATE
    CONFIG_N642DC=1
    CONFIG_DC=1
    CONFIG_NO_NEOPIXEL=1
    USE_BOOTSEL_BUTTON=1
    MAPLE_PIN1=14
    MAPLE_PIN5=15
    N64_PIN_DATA=18
)
target_sources(joypad_n642dc_pico2_w PUBLIC ${COMMON_SOURCES}
    ${N64_HOST_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/dreamcast/dreamcast_device.c
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/dreamcast/maple_state_machine.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/n642dc/app.c
)
target_include_directories(joypad_n642dc_pico2_w PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/n642dc
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/n64
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/dreamcast
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/include
)
target_link_libraries(joypad_n642dc_pico2_w PRIVATE ${COMMON_LIBRARIES} hardware_dma)
joypad_target_common(joypad_n642dc_pico2_w)
# No BTstack for n642dc - uses native N64 input, not USB host
pico_generate_pio_header(joypad_n642dc_pico2_w ${CMAKE_CURRENT_LIST_DIR}/lib/joybus-pio/src/joybus.pio)
pico_generate_pio_header(joypad_n642dc_pico2_w ${CMAKE_CURRENT_LIST_DIR}/native/device/dreamcast/maple.pio)

# --- N64 to Nuon ---
add_executable(joypad_n642nuon)
target_compile_definitions(joypad_n642nuon PRIVATE CONFIG_N642NUON=1 CONFIG_NUON=1 CONFIG_NO_NEOPIXEL=1 N64_PIN_DATA=29)
target_sources(joypad_n642nuon PUBLIC ${COMMON_SOURCES}
    ${N64_HOST_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/nuon/nuon_device.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/n642nuon/app.c
)
target_include_directories(joypad_n642nuon PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/n642nuon
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/n64
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/nuon
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/include
)
target_link_libraries(joypad_n642nuon PRIVATE ${COMMON_LIBRARIES} pico_bit_ops)
joypad_target_common(joypad_n642nuon)
# No BTstack - native N64 input only
pico_generate_pio_header(joypad_n642nuon ${CMAKE_CURRENT_LIST_DIR}/lib/joybus-pio/src/joybus.pio)
pico_generate_pio_header(joypad_n642nuon ${CMAKE_CURRENT_LIST_DIR}/native/device/nuon/polyface_read.pio)
pico_generate_pio_header(joypad_n642nuon ${CMAKE_CURRENT_LIST_DIR}/native/device/nuon/polyface_send.pio)

# --- N64 to Nuon (Aries64 board) ---
# Aries64: RP2040 board with N64 data on GPIO11, Nuon polyface on GPIO12/13.
# GPIO10 is an open-drain driver transistor on the N64 data line (unused by firmware).
add_executable(joypad_n642nuon_aries64)
target_compile_definitions(joypad_n642nuon_aries64 PRIVATE
    CONFIG_N642NUON=1
    CONFIG_NUON=1
    CONFIG_NO_NEOPIXEL=1
    N64_PIN_DATA=11
    DATAIO_PIN=12
    BOARD_LED_PIN=25
)
target_sources(joypad_n642nuon_aries64 PUBLIC ${COMMON_SOURCES}
    ${N64_HOST_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/nuon/nuon_device.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/n642nuon/app.c
)
target_include_directories(joypad_n642nuon_aries64 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/n642nuon
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/n64
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/nuon
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/include
)
target_link_libraries(joypad_n642nuon_aries64 PRIVATE ${COMMON_LIBRARIES} pico_bit_ops)
joypad_target_common(joypad_n642nuon_aries64)
pico_generate_pio_header(joypad_n642nuon_aries64 ${CMAKE_CURRENT_LIST_DIR}/lib/joybus-pio/src/joybus.pio)
pico_generate_pio_header(joypad_n642nuon_aries64 ${CMAKE_CURRENT_LIST_DIR}/native/device/nuon/polyface_read.pio)
pico_generate_pio_header(joypad_n642nuon_aries64 ${CMAKE_CURRENT_LIST_DIR}/native/device/nuon/polyface_send.pio)

# --- 3DO ---
add_executable(joypad_3do)
target_compile_definitions(joypad_3do PRIVATE CONFIG_3DO=1)
target_sources(joypad_3do PUBLIC ${COMMON_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/3do/3do_device.c
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/3do/3do_keyboard.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb23do/app.c
)
target_include_directories(joypad_3do PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb23do
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/3do
)
target_link_libraries(joypad_3do PRIVATE ${COMMON_LIBRARIES} hardware_dma)
joypad_target_common(joypad_3do)
joypad_add_btstack(joypad_3do)
pico_generate_pio_header(joypad_3do ${CMAKE_CURRENT_LIST_DIR}/native/device/3do/sampling.pio)
pico_generate_pio_header(joypad_3do ${CMAKE_CURRENT_LIST_DIR}/native/device/3do/output.pio)

# --- SNES to 3DO ---
add_executable(joypad_snes3do)
target_compile_definitions(joypad_snes3do PRIVATE CONFIG_SNES3DO=1)
target_sources(joypad_snes3do PUBLIC ${COMMON_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/3do/3do_device.c
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/3do/3do_keyboard.c
    ${SNES_HOST_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/snes23do/app.c
)
target_include_directories(joypad_snes3do PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/snes23do
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/3do
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/snes
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/SNESpad/src
)
target_link_libraries(joypad_snes3do PRIVATE ${COMMON_LIBRARIES} hardware_dma)
joypad_target_common(joypad_snes3do)
# No BTstack for snes3do - uses SNES input, not USB host
pico_generate_pio_header(joypad_snes3do ${CMAKE_CURRENT_LIST_DIR}/native/device/3do/sampling.pio)
pico_generate_pio_header(joypad_snes3do ${CMAKE_CURRENT_LIST_DIR}/native/device/3do/output.pio)

# --- UART Bridge ---
add_executable(joypad_uart)
target_compile_definitions(joypad_uart PRIVATE CONFIG_UART=1)
target_sources(joypad_uart PUBLIC ${COMMON_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/uart/uart_device.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2uart/app.c
)
target_include_directories(joypad_uart PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2uart
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/uart
)
target_link_libraries(joypad_uart PRIVATE ${COMMON_LIBRARIES} hardware_uart)
joypad_target_common(joypad_uart)
joypad_add_btstack(joypad_uart)

# --- usb2usb dual: HOST/PRODUCER side (B) ---
# Native USB host + hub reads controllers; uart_peer ships events over the
# inter-MCU UART link to the device side (A). No USB device output here.
add_executable(joypad_usb2usb_remapper_v7_b)
target_compile_definitions(joypad_usb2usb_remapper_v7_b PRIVATE
    CONFIG_USB_HOST=1
    UART_PEER_ENABLED=1
)
target_sources(joypad_usb2usb_remapper_v7_b PUBLIC ${COMMON_SOURCES}
    ${UART_PEER_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2usb_remapper_v7_b/app.c
)
target_include_directories(joypad_usb2usb_remapper_v7_b PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2usb_remapper_v7_b
    ${CMAKE_CURRENT_SOURCE_DIR}/uart_peer
)
target_link_libraries(joypad_usb2usb_remapper_v7_b PRIVATE ${COMMON_LIBRARIES} hardware_uart)
joypad_target_common(joypad_usb2usb_remapper_v7_b)

# --- usb2usb dual: DEVICE/CONSUMER side (A) ---
# USB device output only (no USB host on this MCU); input arrives over the
# inter-MCU UART link and is routed to the USB device.
add_executable(joypad_usb2usb_remapper_v7_a)
target_compile_definitions(joypad_usb2usb_remapper_v7_a PRIVATE
    CONFIG_USB=1
    DISABLE_USB_HOST=1
    UART_PEER_ENABLED=1
)
target_sources(joypad_usb2usb_remapper_v7_a PUBLIC ${CORE_SOURCES} ${USB_DEVICE_SOURCES}
    ${UART_PEER_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2usb_remapper_v7_a/app.c
)
target_include_directories(joypad_usb2usb_remapper_v7_a PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2usb_remapper_v7_a
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
    ${CMAKE_CURRENT_SOURCE_DIR}/uart_peer
)
target_link_libraries(joypad_usb2usb_remapper_v7_a PRIVATE
    pico_stdlib pico_multicore hardware_pio pico_rand hardware_uart
    tinyusb_device tinyusb_board)
joypad_target_common(joypad_usb2usb_remapper_v7_a)
pico_enable_stdio_uart(joypad_usb2usb_remapper_v7_a 1)

# --- usb2usb dual: SWD relay stage (flashes B from an embedded image) ---
# The board's single USB-C only reaches the device side (A); the host side (B)
# is flashed over SWD (SWCLK=GP2, SWDIO=GP3) by this RAM-resident stage. The
# combined UF2 (A flash image + this RAM stage) is one drag-drop that flashes
# both chips. See .dev/docs/USB2USB_HIDREMAPPER_V7_PLAN.md.

# Embed B's binary as a C array (joypad_usb2usb_remapper_v7_b builds first).
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/dual_b_binary.c ${CMAKE_CURRENT_BINARY_DIR}/dual_b_binary.h
    DEPENDS joypad_usb2usb_remapper_v7_b
    COMMAND python3 ${CMAKE_CURRENT_LIST_DIR}/../tools/bin2carray.py
            $<TARGET_FILE_DIR:joypad_usb2usb_remapper_v7_b>/joypad_usb2usb_remapper_v7_b.bin
            ${CMAKE_CURRENT_BINARY_DIR}/dual_b_binary.c
            ${CMAKE_CURRENT_BINARY_DIR}/dual_b_binary.h
            dual_b_binary
    VERBATIM
)

add_executable(joypad_flash_b_side
    ${CMAKE_CURRENT_SOURCE_DIR}/swd_flash/flash_b_side.c
    ${CMAKE_CURRENT_SOURCE_DIR}/swd_flash/swd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/swd_flash/flash.c
    ${CMAKE_CURRENT_SOURCE_DIR}/swd_flash/adi.c
    ${CMAKE_CURRENT_BINARY_DIR}/dual_b_binary.c
)
target_include_directories(joypad_flash_b_side PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/swd_flash
    ${CMAKE_CURRENT_BINARY_DIR}
)
# SWD pins to the host-side (B) RP2040. remapper_v7 board: SWCLK=GP28, SWDIO=GP27
# (upstream swd.h defaults are GP2/GP3, which are wrong for this board).
target_compile_definitions(joypad_flash_b_side PRIVATE
    PIN_SWDCLK=28 PIN_SWDIO=27)
target_link_libraries(joypad_flash_b_side PRIVATE
    pico_stdlib hardware_pio hardware_watchdog)
pico_generate_pio_header(joypad_flash_b_side ${CMAKE_CURRENT_SOURCE_DIR}/swd_flash/swd.pio)
pico_add_extra_outputs(joypad_flash_b_side)
pico_set_binary_type(joypad_flash_b_side no_flash)

# Combine A (flash) + flash_b_side (RAM) into one drag-drop UF2.
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/joypad_usb2usb_remapper_v7.uf2
    DEPENDS joypad_usb2usb_remapper_v7_a joypad_flash_b_side
    COMMAND python3 ${CMAKE_CURRENT_LIST_DIR}/../tools/combine_uf2.py
            $<TARGET_FILE_DIR:joypad_usb2usb_remapper_v7_a>/joypad_usb2usb_remapper_v7_a.uf2
            $<TARGET_FILE_DIR:joypad_flash_b_side>/joypad_flash_b_side.uf2
            ${CMAKE_CURRENT_BINARY_DIR}/joypad_usb2usb_remapper_v7.uf2
    VERBATIM
)
add_custom_target(joypad_usb2usb_remapper_v7
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/joypad_usb2usb_remapper_v7.uf2
)

# ============================================================================
# USB HOST TO USB DEVICE APPS (USB input -> USB output)
# ============================================================================
# USB Host Methods:
#   PIO-USB (tinyusb_pico_pio_usb) - Software USB host via PIO, uses 2 GPIOs
#   MAX3421E (tinyusb_max3421)     - SPI USB host IC, uses SPI bus + CS + INT pins
#   Native (tinyusb_host)          - RP2040's built-in USB, only when no device output needed

# --- USB to USB (Feather RP2040 #4884, PIO-USB) ---
add_executable(joypad_usb_feather_rp2040)
target_compile_definitions(joypad_usb_feather_rp2040 PRIVATE
    CONFIG_USB=1
    PICO_DEFAULT_PIO_USB_DP_PIN=16
    BOARD_TUH_RHPORT=1
    USBR_CDC_DEBUG=1
    PICO_DEFAULT_UART=0
    PICO_DEFAULT_UART_TX_PIN=0
    PICO_DEFAULT_UART_RX_PIN=1
)
target_sources(joypad_usb_feather_rp2040 PUBLIC
    ${COMMON_SOURCES}
    ${USB_DEVICE_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2usb/app.c
)
target_include_directories(joypad_usb_feather_rp2040 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2usb
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
)
target_link_libraries(joypad_usb_feather_rp2040 PRIVATE ${COMMON_LIBRARIES} tinyusb_device tinyusb_pico_pio_usb)
joypad_target_common(joypad_usb_feather_rp2040)
joypad_add_btstack(joypad_usb_feather_rp2040)
pico_enable_stdio_uart(joypad_usb_feather_rp2040 1)

# --- USB to USB (Feather RP2040 USB Host #5723, PIO-USB) ---
add_executable(joypad_usb_feather_rp2040_usb_host)
target_compile_definitions(joypad_usb_feather_rp2040_usb_host PRIVATE
    CONFIG_USB=1
    PICO_DEFAULT_PIO_USB_DP_PIN=16
    PICO_DEFAULT_PIO_USB_VBUSEN_PIN=18
    PICO_DEFAULT_PIO_USB_VBUSEN_STATE=1
    BOARD_TUH_RHPORT=1
    USBR_CDC_DEBUG=1
    PICO_DEFAULT_UART=0
    PICO_DEFAULT_UART_TX_PIN=0
    PICO_DEFAULT_UART_RX_PIN=1
    # MCP-driven synthetic input over the same UART that carries stdio logs
    # (UART0 on GPIO0/1, 115200 baud — TX/RX header pins, USB-UART bridge).
    CONFIG_UART_HOST=1
    UART_HOST_PERIPHERAL=uart0
    CONFIG_UART_HOST_TX_PIN=0
    CONFIG_UART_HOST_RX_PIN=1
    CONFIG_UART_HOST_BAUD=115200
    # OLED FeatherWing 128x64 (SH1107) over I2C on STEMMA QT.
    # No MAX3421E here, so all three buttons (A=D9/GPIO9, B=D6/GPIO8,
    # C=D5/GPIO7) are free.
    OLED_I2C_INST=1
    OLED_I2C_SDA_PIN=2
    OLED_I2C_SCL_PIN=3
    OLED_I2C_ADDR=0x3C
    OLED_BUTTON_A_PIN=9
    OLED_BUTTON_B_PIN=8
    OLED_BUTTON_C_PIN=7
    I2C_PEER_ENABLED=1
    UART_PEER_ENABLED=1
)
target_sources(joypad_usb_feather_rp2040_usb_host PUBLIC
    ${COMMON_SOURCES}
    ${USB_DEVICE_SOURCES}
    ${I2C_PEER_SOURCES}
    ${UART_PEER_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services/display/display.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services/display/display_i2c.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services/display/joy_anim.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services/display/eyes_anim.c
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/uart/uart_host.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2usb/app.c
)
target_include_directories(joypad_usb_feather_rp2040_usb_host PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2usb
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
)
target_link_libraries(joypad_usb_feather_rp2040_usb_host PRIVATE ${COMMON_LIBRARIES} tinyusb_device tinyusb_pico_pio_usb hardware_i2c pico_i2c_slave)
joypad_target_common(joypad_usb_feather_rp2040_usb_host)
joypad_add_btstack(joypad_usb_feather_rp2040_usb_host)
pico_enable_stdio_uart(joypad_usb_feather_rp2040_usb_host 1)

# --- USB to USB (Feather RP2040 #4884 + MAX3421E USB Host FeatherWing #5858) ---
# MAX3421E pin defines for TinyUSB family.c BSP + max3421_host.c init
# FeatherWing SPI on Feather header: SPI0 (SCK=18, MOSI=19, MISO=20), CS=10, INT=9
add_executable(joypad_usb_feather_rp2040_max3421)
target_compile_definitions(joypad_usb_feather_rp2040_max3421 PRIVATE
    CONFIG_USB=1
    BOARD_TUH_RHPORT=1
    USBR_CDC_DEBUG=1
    PICO_DEFAULT_UART=0
    PICO_DEFAULT_UART_TX_PIN=0
    PICO_DEFAULT_UART_RX_PIN=1
    MAX3421_SPI=spi0
    MAX3421_SCK_PIN=18
    MAX3421_MOSI_PIN=19
    MAX3421_MISO_PIN=20
    MAX3421_CS_PIN=10
    MAX3421_INTR_PIN=9
    USE_BOOTSEL_BUTTON=1
    # FeatherWing OLED 128x64 (SH1107) over I2C on STEMMA QT
    OLED_I2C_INST=1
    OLED_I2C_SDA_PIN=2
    OLED_I2C_SCL_PIN=3
    OLED_I2C_ADDR=0x3C
    OLED_BUTTON_B_PIN=6
    OLED_BUTTON_C_PIN=5
    I2C_PEER_ENABLED=1
)
target_sources(joypad_usb_feather_rp2040_max3421 PUBLIC
    ${COMMON_SOURCES}
    ${USB_DEVICE_SOURCES}
    ${I2C_PEER_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services/display/display.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services/display/display_i2c.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services/display/joy_anim.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services/display/eyes_anim.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2usb/app.c
)
target_include_directories(joypad_usb_feather_rp2040_max3421 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2usb
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
)
target_link_libraries(joypad_usb_feather_rp2040_max3421 PRIVATE ${COMMON_LIBRARIES} tinyusb_device tinyusb_max3421 hardware_i2c pico_i2c_slave)
joypad_target_common(joypad_usb_feather_rp2040_max3421)
joypad_add_btstack(joypad_usb_feather_rp2040_max3421)
pico_enable_stdio_uart(joypad_usb_feather_rp2040_max3421 1)

# --- USB to USB (Feather RP2040 USB Host #5723 + MAX3421E USB Host FeatherWing #5858) ---
# Uses MAX3421E FeatherWing instead of onboard PIO USB host.
# Board SPI1: SCK=14, MOSI=15, MISO=8. NeoPixel on GPIO 21 (power gate GPIO 20).
add_executable(joypad_usb_feather_rp2040_usb_host_max3421)
target_compile_definitions(joypad_usb_feather_rp2040_usb_host_max3421 PRIVATE
    CONFIG_USB=1
    BOARD_TUH_RHPORT=1
    USBR_CDC_DEBUG=1
    PICO_DEFAULT_UART=0
    PICO_DEFAULT_UART_TX_PIN=0
    PICO_DEFAULT_UART_RX_PIN=1
    MAX3421_SPI=spi1
    MAX3421_SCK_PIN=14
    MAX3421_MOSI_PIN=15
    MAX3421_MISO_PIN=8
    MAX3421_CS_PIN=10
    MAX3421_INTR_PIN=9
    # FeatherWing OLED 128x64 (SH1107) over I2C on STEMMA QT
    OLED_I2C_INST=1
    OLED_I2C_SDA_PIN=2
    OLED_I2C_SCL_PIN=3
    OLED_I2C_ADDR=0x3C
    OLED_BUTTON_B_PIN=6
    OLED_BUTTON_C_PIN=5
    I2C_PEER_ENABLED=1
    # MCP-driven synthetic input over the same UART that carries stdio logs
    # (UART0 on GPIO0/1, 115200 baud — KB2040 USB-UART bridge).
    CONFIG_UART_HOST=1
    UART_HOST_PERIPHERAL=uart0
    CONFIG_UART_HOST_TX_PIN=0
    CONFIG_UART_HOST_RX_PIN=1
    CONFIG_UART_HOST_BAUD=115200
)
target_sources(joypad_usb_feather_rp2040_usb_host_max3421 PUBLIC
    ${COMMON_SOURCES}
    ${USB_DEVICE_SOURCES}
    ${I2C_PEER_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services/display/display.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services/display/display_i2c.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services/display/joy_anim.c

    ${CMAKE_CURRENT_SOURCE_DIR}/core/services/display/eyes_anim.c
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/uart/uart_host.c

    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2usb/app.c
)
target_include_directories(joypad_usb_feather_rp2040_usb_host_max3421 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2usb
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
)
target_link_libraries(joypad_usb_feather_rp2040_usb_host_max3421 PRIVATE ${COMMON_LIBRARIES} tinyusb_device tinyusb_max3421 hardware_i2c pico_i2c_slave)
# Wrap buggy tuh_max3421_spi_xfer_api in TinyUSB BSP (hardcodes spi0 instead of MAX3421_SPI)
target_link_options(joypad_usb_feather_rp2040_usb_host_max3421 PRIVATE -Wl,--wrap=tuh_max3421_spi_xfer_api)
joypad_target_common(joypad_usb_feather_rp2040_usb_host_max3421)
joypad_add_btstack(joypad_usb_feather_rp2040_usb_host_max3421)
pico_enable_stdio_uart(joypad_usb_feather_rp2040_usb_host_max3421 1)

# --- USB to USB (Raspberry Pi Pico, PIO-USB) ---
# PIO USB host on GP16/GP17 (default), standard LED on GP25
add_executable(joypad_usb_pico)
target_compile_definitions(joypad_usb_pico PRIVATE
    CONFIG_USB=1
    PICO_DEFAULT_PIO_USB_DP_PIN=16
    BOARD_TUH_RHPORT=1
    USE_BOOTSEL_BUTTON=1
)
target_sources(joypad_usb_pico PUBLIC ${COMMON_SOURCES} ${USB_DEVICE_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2usb/app.c
)
target_include_directories(joypad_usb_pico PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2usb
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
)
target_link_libraries(joypad_usb_pico PRIVATE ${COMMON_LIBRARIES} tinyusb_device tinyusb_pico_pio_usb)
joypad_target_common(joypad_usb_pico)
joypad_add_btstack(joypad_usb_pico)

# --- USB to USB (OGX-Mini Pi Pico hardware, PIO-USB) ---
# Matches the OGX-Mini Pico pinout: PIO-USB host D+ on GP0 (D- = GP1), status
# LED on GP25. GP0/1 are UART0, so UART stdio is disabled here — leaving it on
# would init UART0 on GP0/1 and collide with the PIO-USB host port. Everything
# else is identical to usb2usb_pico (native USB = console-facing device port).
add_executable(joypad_usb_ogxm_pico)
target_compile_definitions(joypad_usb_ogxm_pico PRIVATE
    CONFIG_USB=1
    CONFIG_PIO_USB_DP_PIN=0
    PICO_DEFAULT_PIO_USB_DP_PIN=0
    BOARD_TUH_RHPORT=1
    USE_BOOTSEL_BUTTON=1
)
target_sources(joypad_usb_ogxm_pico PUBLIC ${COMMON_SOURCES} ${USB_DEVICE_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2usb/app.c
)
target_include_directories(joypad_usb_ogxm_pico PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2usb
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
)
target_link_libraries(joypad_usb_ogxm_pico PRIVATE ${COMMON_LIBRARIES} tinyusb_device tinyusb_pico_pio_usb)
joypad_target_common(joypad_usb_ogxm_pico)
joypad_add_btstack(joypad_usb_ogxm_pico)
pico_enable_stdio_uart(joypad_usb_ogxm_pico 0)
pico_enable_stdio_usb(joypad_usb_ogxm_pico 0)

# --- USB to USB (Raspberry Pi Pico W, PIO-USB) ---
# PIO USB host on GP16/GP17, no WS2812, LED behind CYW43
add_executable(joypad_usb_pico_w)
target_compile_definitions(joypad_usb_pico_w PRIVATE
    CONFIG_USB=1
    PICO_DEFAULT_PIO_USB_DP_PIN=16
    BOARD_TUH_RHPORT=1
    USE_BOOTSEL_BUTTON=1
)
target_sources(joypad_usb_pico_w PUBLIC ${COMMON_SOURCES} ${USB_DEVICE_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2usb/app.c
)
target_include_directories(joypad_usb_pico_w PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2usb
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
)
target_link_libraries(joypad_usb_pico_w PRIVATE ${COMMON_LIBRARIES} tinyusb_device tinyusb_pico_pio_usb)
joypad_target_common(joypad_usb_pico_w)
joypad_add_btstack(joypad_usb_pico_w)

# --- USB to USB (Raspberry Pi Pico 2 W, PIO-USB) ---
# PIO USB host on GP16/GP17, no WS2812, LED behind CYW43
add_executable(joypad_usb_pico2_w)
target_compile_definitions(joypad_usb_pico2_w PRIVATE
    CONFIG_USB=1
    PICO_DEFAULT_PIO_USB_DP_PIN=16
    BOARD_TUH_RHPORT=1
    USE_BOOTSEL_BUTTON=1
)
target_sources(joypad_usb_pico2_w PUBLIC ${COMMON_SOURCES} ${USB_DEVICE_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2usb/app.c
)
target_include_directories(joypad_usb_pico2_w PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2usb
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
)
target_link_libraries(joypad_usb_pico2_w PRIVATE ${COMMON_LIBRARIES} tinyusb_device tinyusb_pico_pio_usb)
joypad_target_common(joypad_usb_pico2_w)
joypad_add_btstack(joypad_usb_pico2_w)

# --- USB to USB (RP2040-Zero, PIO-USB) ---
add_executable(joypad_usb_rp2040zero)
target_compile_definitions(joypad_usb_rp2040zero PRIVATE
    CONFIG_USB=1
    CONFIG_PIO_USB_DP_PIN=10
    PICO_DEFAULT_PIO_USB_DP_PIN=10
    BOARD_TUH_RHPORT=1
    WS2812_PIN=16
    # Also drive a plain status LED on GP14 (OGX-Mini's LED_INDICATOR_PIN).
    # Independent pin from the WS2812, so one build lights whichever the board
    # physically has: stock Waveshare / OGX-Mini v1 -> NeoPixel on 16,
    # OGX-Mini green v2/v3 -> plain LED on 14. Driving an absent LED is a no-op.
    BOARD_LED_PIN=14
    USE_BOOTSEL_BUTTON=1
)
target_sources(joypad_usb_rp2040zero PUBLIC ${COMMON_SOURCES} ${USB_DEVICE_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2usb/app.c
)
target_include_directories(joypad_usb_rp2040zero PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2usb
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
)
target_link_libraries(joypad_usb_rp2040zero PRIVATE ${COMMON_LIBRARIES} tinyusb_device tinyusb_pico_pio_usb)
joypad_target_common(joypad_usb_rp2040zero)
joypad_add_btstack(joypad_usb_rp2040zero)

# --- USB to USB (Waveshare RP2350A, PIO-USB) ---
add_executable(joypad_usb_rp2350usba)
target_compile_definitions(joypad_usb_rp2350usba PRIVATE
    CONFIG_USB=1
    CONFIG_USB_HOST=1
    CONFIG_PIO_USB_DP_PIN=12
    PICO_DEFAULT_PIO_USB_DP_PIN=12
    BOARD_TUH_RHPORT=1
    WS2812_PIN=16
    USE_BOOTSEL_BUTTON=1
    APP_NAME="usb2usb"
)
target_sources(joypad_usb_rp2350usba PUBLIC ${COMMON_SOURCES} ${USB_DEVICE_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2usb/app.c
)
target_include_directories(joypad_usb_rp2350usba PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2usb
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
)
target_link_libraries(joypad_usb_rp2350usba PRIVATE ${COMMON_LIBRARIES} tinyusb_device tinyusb_pico_pio_usb)
joypad_target_common(joypad_usb_rp2350usba)
joypad_add_btstack(joypad_usb_rp2350usba)

# --- BT to USB (Pico W / Pico 2 W / Waveshare RP2350B-Plus-W built-in Bluetooth) ---
# Only build when targeting a board with a built-in CYW43 / RM2 radio
if (PICO_BOARD STREQUAL "pico_w" OR
    PICO_BOARD STREQUAL "pico2_w" OR
    PICO_BOARD STREQUAL "waveshare_rp2350b_plus_w")
    add_executable(joypad_bt2usb)
    target_compile_definitions(joypad_bt2usb PRIVATE
        CONFIG_BT2USB=1
        CONFIG_USB=1
        DISABLE_USB_HOST=1
        USBD_DEFAULT_MODE=USB_OUTPUT_MODE_SINPUT
        USE_BOOTSEL_BUTTON=1
        CYW43_LWIP=0
        # BTstack uses async_context, not embedded run loop
        BTSTACK_USE_CYW43=1
        ENABLE_BTSTACK=1
        # bt_transport.c gates bthid_registry_init() on this (see #188). Manual-BT
        # apps don't go through joypad_add_btstack(), so they must define it here or
        # the registry is never initialised and every BT controller falls to the
        # generic gamepad driver (--gc-sections discards the whole registry).
        CONFIG_BT_HOST=1
    )
    # DualSense voice/show modes (opt-in, all disabled by default).
    # CONFIG_DS5_DROP_SCREAM doubles as the base voice-engine flag (0x36 audio
    # streaming, hci_dump CID tap, buffers); modes layer on top of it:
    #   DS5_DROP_SCREAM=1  make bt2usb_pico_w   -> drop-scream + fireworks
    #   DS5_FISHER_PRICE=1 make bt2usb_pico_w   -> kids numbers/letters mode
    #   DS5_COMPANION=1    make bt2usb_pico_w   -> LLM voice companion bridge
    # No BTstack modifications required. Assets: tools/ds5-scream/encode.py.
    if (DEFINED ENV{DS5_DROP_SCREAM})
        message(STATUS "joypad_bt2usb: DS5 drop-scream + fireworks ENABLED")
        target_compile_definitions(joypad_bt2usb PRIVATE CONFIG_DS5_DROP_SCREAM=1)
    endif()
    if (DEFINED ENV{DS5_FISHER_PRICE})
        message(STATUS "joypad_bt2usb: DS5 Fisher-Price mode ENABLED")
        target_compile_definitions(joypad_bt2usb PRIVATE
            CONFIG_DS5_DROP_SCREAM=1
            CONFIG_DS5_FISHER_PRICE=1
        )
    endif()
    if (DEFINED ENV{DS5_ROBOT})
        message(STATUS "joypad_bt2usb: DS5 Robot mode ENABLED")
        target_compile_definitions(joypad_bt2usb PRIVATE
            CONFIG_DS5_DROP_SCREAM=1
            CONFIG_DS5_ROBOT=1
        )
    endif()
    if (DEFINED ENV{DS5_COMPANION})
        message(STATUS "joypad_bt2usb: DS5 AI companion mode ENABLED")
        target_compile_definitions(joypad_bt2usb PRIVATE
            CONFIG_DS5_DROP_SCREAM=1
            CONFIG_DS5_COMPANION=1
        )
    endif()
    target_sources(joypad_bt2usb PUBLIC
        ${CORE_SOURCES}
        ${USB_DEVICE_SOURCES}
        ${BTHID_DEVICE_SOURCES}
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/devices/generic/hid_parser.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/btstack/btstack_host.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/btstack/bt_device_db.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/transport/bt_transport.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/transport/bt_transport_cyw43.c
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/bt2usb/app.c
    )
    target_include_directories(joypad_bt2usb PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/bt2usb
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
        ${CMAKE_CURRENT_SOURCE_DIR}/bt
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/btstack
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/bthid
        ${BTSTACK_INCLUDES}
    )
    target_link_libraries(joypad_bt2usb PRIVATE
        pico_stdlib
        pico_multicore
        pico_rand
        pico_cyw43_arch_poll
        pico_btstack_ble
        pico_btstack_classic
        pico_btstack_cyw43
        tinyusb_device
        tinyusb_board
    )
    joypad_target_common(joypad_bt2usb)
    pico_enable_stdio_usb(joypad_bt2usb 0)
    pico_enable_stdio_uart(joypad_bt2usb 1)

    # --- MouthPad bridge (Pico W / Pico 2 W) : BLE in (HID + NUS) -> USB out ---
    add_executable(joypad_mouthpad)
    target_compile_definitions(joypad_mouthpad PRIVATE
        CONFIG_MOUTHPAD=1
        CONFIG_USB=1
        DISABLE_USB_HOST=1
        USE_BOOTSEL_BUTTON=1
        CYW43_LWIP=0
        BTSTACK_USE_CYW43=1
        ENABLE_BTSTACK=1
        CONFIG_BT_HOST=1  # register BT-HID drivers (see #188 regression)
    )
    target_sources(joypad_mouthpad PUBLIC
        ${CORE_SOURCES}
        ${USB_DEVICE_SOURCES}
        ${BTHID_DEVICE_SOURCES}
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/devices/generic/hid_parser.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/btstack/btstack_host.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/btstack/bt_device_db.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/transport/bt_transport.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/transport/bt_transport_cyw43.c
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/mouthpad/app.c
    )
    target_include_directories(joypad_mouthpad PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/mouthpad
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
        ${CMAKE_CURRENT_SOURCE_DIR}/bt
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/btstack
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/bthid
        ${BTSTACK_INCLUDES}
    )
    target_link_libraries(joypad_mouthpad PRIVATE
        pico_stdlib
        pico_multicore
        pico_rand
        pico_cyw43_arch_poll
        pico_btstack_ble
        pico_btstack_classic
        pico_btstack_cyw43
        tinyusb_device
        tinyusb_board
    )
    joypad_target_common(joypad_mouthpad)
    pico_enable_stdio_usb(joypad_mouthpad 0)
    pico_enable_stdio_uart(joypad_mouthpad 1)

    # --- BT to Nuon (Pico W / Pico 2 W built-in Bluetooth -> Nuon) ---
    add_executable(joypad_bt2nuon)
    target_compile_definitions(joypad_bt2nuon PRIVATE
        CONFIG_BT2NUON=1
        DISABLE_USB_HOST=1
        DISABLE_USB_DEVICE=1
        CONFIG_NO_NEOPIXEL=1
        CONFIG_NO_FLASH_LOCKOUT=1
        USE_BOOTSEL_BUTTON=1
        CYW43_LWIP=0
        BTSTACK_USE_CYW43=1
        ENABLE_BTSTACK=1
        SCAN_INTERVAL=0x0500
        SCAN_WINDOW=0x0030
        CONFIG_BT_HOST=1  # register BT-HID drivers (see #188 regression)
    )
    target_sources(joypad_bt2nuon PUBLIC
        ${CORE_SOURCES}
        ${USB_DEVICE_SOURCES}
        ${BTHID_DEVICE_SOURCES}
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/devices/generic/hid_parser.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/btstack/btstack_host.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/btstack/bt_device_db.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/transport/bt_transport.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/transport/bt_transport_cyw43.c
        ${CMAKE_CURRENT_SOURCE_DIR}/native/device/nuon/nuon_device.c
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/bt2nuon/app.c
    )
    target_include_directories(joypad_bt2nuon PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/bt2nuon
        ${CMAKE_CURRENT_SOURCE_DIR}/native/device/nuon
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
        ${CMAKE_CURRENT_SOURCE_DIR}/bt
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/btstack
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/bthid
        ${BTSTACK_INCLUDES}
    )
    # Link tinyusb for headers but disable USB device initialization.
    # USB device DMA/interrupts interfere with Core 1's polyface timing.
    target_link_libraries(joypad_bt2nuon PRIVATE
        pico_stdlib
        pico_multicore
        pico_rand
        pico_bit_ops
        hardware_pio
        hardware_flash
        pico_cyw43_arch_poll
        pico_btstack_ble
        pico_btstack_classic
        pico_btstack_cyw43
        tinyusb_device
        tinyusb_board
    )
    joypad_target_common(joypad_bt2nuon)
    pico_enable_stdio_usb(joypad_bt2nuon 0)
    pico_enable_stdio_uart(joypad_bt2nuon 1)
    pico_generate_pio_header(joypad_bt2nuon ${CMAKE_CURRENT_LIST_DIR}/native/device/nuon/polyface_read.pio)
    pico_generate_pio_header(joypad_bt2nuon ${CMAKE_CURRENT_LIST_DIR}/native/device/nuon/polyface_send.pio)

    # --- BT to Loopy (Pico W / Pico 2 W built-in Bluetooth -> Casio Loopy) ---
    add_executable(joypad_bt2loopy)
    target_compile_definitions(joypad_bt2loopy PRIVATE
        CONFIG_BT2LOOPY=1
        DISABLE_USB_HOST=1
        USE_BOOTSEL_BUTTON=1
        CYW43_LWIP=0
        BTSTACK_USE_CYW43=1
        ENABLE_BTSTACK=1
        CONFIG_BT_HOST=1  # register BT-HID drivers (see #188 regression)
    )
    target_sources(joypad_bt2loopy PUBLIC
        ${CORE_SOURCES}
        ${USB_DEVICE_SOURCES}
        ${BTHID_DEVICE_SOURCES}
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/devices/generic/hid_parser.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/btstack/btstack_host.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/btstack/bt_device_db.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/transport/bt_transport.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/transport/bt_transport_cyw43.c
        ${CMAKE_CURRENT_SOURCE_DIR}/native/device/loopy/loopy_device.c
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/bt2loopy/app.c
    )
    target_include_directories(joypad_bt2loopy PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/bt2loopy
        ${CMAKE_CURRENT_SOURCE_DIR}/native/device/loopy
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
        ${CMAKE_CURRENT_SOURCE_DIR}/bt
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/btstack
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/bthid
        ${BTSTACK_INCLUDES}
    )
    target_link_libraries(joypad_bt2loopy PRIVATE
        pico_stdlib
        pico_multicore
        pico_rand
        pico_bit_ops
        hardware_pio
        pico_cyw43_arch_poll
        pico_btstack_ble
        pico_btstack_classic
        pico_btstack_cyw43
        tinyusb_device
        tinyusb_board
    )
    joypad_target_common(joypad_bt2loopy)
    pico_enable_stdio_usb(joypad_bt2loopy 0)
    pico_enable_stdio_uart(joypad_bt2loopy 1)
    pico_generate_pio_header(joypad_bt2loopy ${CMAKE_CURRENT_LIST_DIR}/native/device/loopy/loopy.pio)

    # --- BT to N64 (Pico W / Pico 2 W built-in Bluetooth -> N64 Console) ---
    add_executable(joypad_bt2n64)
    target_compile_definitions(joypad_bt2n64 PRIVATE
        CONFIG_BT2N64=1
        DISABLE_USB_HOST=1
        CONFIG_NO_NEOPIXEL=1
        USE_BOOTSEL_BUTTON=1
        CYW43_LWIP=0
        BTSTACK_USE_CYW43=1
        ENABLE_BTSTACK=1
        N64_DATA_PIN=2
        CONFIG_BT_HOST=1  # register BT-HID drivers (see #188 regression)
    )
    target_sources(joypad_bt2n64 PUBLIC
        ${CORE_SOURCES}
        ${BTHID_DEVICE_SOURCES}
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/devices/generic/hid_parser.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/btstack/btstack_host.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/btstack/bt_device_db.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/transport/bt_transport.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/transport/bt_transport_cyw43.c
        ${CMAKE_CURRENT_SOURCE_DIR}/native/device/n64/n64_device.c
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/src/joybus.c
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/src/N64Console.c
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/bt2n64/app.c
    )
    target_include_directories(joypad_bt2n64 PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/bt2n64
        ${CMAKE_CURRENT_SOURCE_DIR}/native/device/n64
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/include
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
        ${CMAKE_CURRENT_SOURCE_DIR}/bt
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/btstack
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/bthid
        ${BTSTACK_INCLUDES}
    )
    target_link_libraries(joypad_bt2n64 PRIVATE
        pico_stdlib
        pico_multicore
        pico_rand
        hardware_pio
        hardware_flash
        pico_cyw43_arch_poll
        pico_btstack_ble
        pico_btstack_classic
        pico_btstack_cyw43
    )
    joypad_target_common(joypad_bt2n64)
    pico_enable_stdio_usb(joypad_bt2n64 1)
    pico_enable_stdio_uart(joypad_bt2n64 1)
    pico_generate_pio_header(joypad_bt2n64 ${CMAKE_CURRENT_LIST_DIR}/lib/joybus-pio/src/joybus.pio)

    # --- BT to Wii extension (Pico W built-in BT -> I2C slave at 0x52) ---
    # A real Wiimote plugs into the Pico's I2C bus and sees a Classic
    # Controller responding to its polls, driven by any BT controller.
    add_executable(joypad_bt2wiiext)
    target_compile_definitions(joypad_bt2wiiext PRIVATE
        CONFIG_BT2WIIEXT=1
        DISABLE_USB_HOST=1
        CONFIG_NO_NEOPIXEL=1
        CONFIG_PAD_INPUT=1
        SENSOR_PAD=1
        REQUIRE_BT_INPUT=1
        USE_BOOTSEL_BUTTON=1
        CYW43_LWIP=0
        BTSTACK_USE_CYW43=1
        ENABLE_BTSTACK=1
        CONFIG_BT_HOST=1  # register BT-HID drivers (see #188 regression)
    )
    target_sources(joypad_bt2wiiext PUBLIC
        ${CORE_SOURCES}
        ${USB_DEVICE_SOURCES}
        ${CONTROLLER_SOURCES}
        ${BTHID_DEVICE_SOURCES}
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/devices/generic/hid_parser.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/btstack/btstack_host.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/btstack/bt_device_db.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/transport/bt_transport.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/transport/bt_transport_cyw43.c
        ${CMAKE_CURRENT_SOURCE_DIR}/native/device/wii_ext/wii_ext_device.c
        ${CMAKE_CURRENT_SOURCE_DIR}/native/device/wii_ext/wii_ext_crypto.c
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/bt2wiiext/app.c
    )
    target_include_directories(joypad_bt2wiiext PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/bt2wiiext
        ${CMAKE_CURRENT_SOURCE_DIR}/native/device/wii_ext
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
        ${CMAKE_CURRENT_SOURCE_DIR}/pad
        ${CMAKE_CURRENT_SOURCE_DIR}/bt
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/btstack
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/bthid
        ${BTSTACK_INCLUDES}
    )
    target_link_libraries(joypad_bt2wiiext PRIVATE
        pico_stdlib
        pico_multicore
        pico_rand
        hardware_pio
        hardware_i2c
        hardware_adc
        hardware_pwm
        hardware_spi
        hardware_flash
        pico_i2c_slave
        pico_cyw43_arch_poll
        pico_btstack_ble
        pico_btstack_classic
        pico_btstack_cyw43
        tinyusb_device
        tinyusb_board
    )
    joypad_target_common(joypad_bt2wiiext)
    pico_enable_stdio_usb(joypad_bt2wiiext 0)
    pico_enable_stdio_uart(joypad_bt2wiiext 0)

    # --- BT to GameCube (Pico W / Pico 2 W built-in Bluetooth -> GameCube) ---
    add_executable(joypad_bt2gc)
    target_compile_definitions(joypad_bt2gc PRIVATE
        CONFIG_BT2GC=1
        DISABLE_USB_HOST=1
        CONFIG_NO_NEOPIXEL=1
        USE_BOOTSEL_BUTTON=1
        CYW43_LWIP=0
        BTSTACK_USE_CYW43=1
        ENABLE_BTSTACK=1
        CONFIG_BT_HOST=1  # register BT-HID drivers (see #188 regression)
    )
    target_sources(joypad_bt2gc PUBLIC
        ${CORE_SOURCES}
        ${USB_DEVICE_SOURCES}
        ${BTHID_DEVICE_SOURCES}
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/devices/generic/hid_parser.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/btstack/btstack_host.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/btstack/bt_device_db.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/transport/bt_transport.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/transport/bt_transport_cyw43.c
        ${CMAKE_CURRENT_SOURCE_DIR}/native/device/gamecube/gamecube_device.c
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/src/joybus.c
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/src/GamecubeConsole.c
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/bt2gc/app.c
    )
    target_include_directories(joypad_bt2gc PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/bt2gc
        ${CMAKE_CURRENT_SOURCE_DIR}/native/device/gamecube
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/include
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
        ${CMAKE_CURRENT_SOURCE_DIR}/bt
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/btstack
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/bthid
        ${BTSTACK_INCLUDES}
    )
    target_link_libraries(joypad_bt2gc PRIVATE
        pico_stdlib
        pico_multicore
        pico_rand
        hardware_pio
        hardware_flash
        pico_cyw43_arch_poll
        pico_btstack_ble
        pico_btstack_classic
        pico_btstack_cyw43
        tinyusb_device
        tinyusb_board
    )
    joypad_target_common(joypad_bt2gc)
    pico_enable_stdio_usb(joypad_bt2gc 0)
    pico_enable_stdio_uart(joypad_bt2gc 1)
    pico_generate_pio_header(joypad_bt2gc ${CMAKE_CURRENT_LIST_DIR}/lib/joybus-pio/src/joybus.pio)

    # --- BT+USB to USB (Pico W / Pico 2 W: PIO-USB host + CYW43 Bluetooth + USB device) ---
    # Combines USB host (PIO-USB on GP16/17) with built-in CYW43 Bluetooth.
    # No pin conflicts: PIO-USB uses GP16/17, CYW43 uses dedicated SPI pins (23/24/25/29).
    # Does NOT use joypad_add_btstack() — CYW43 uses pico_btstack_* libs instead of
    # compiled BTstack sources to avoid duplicate symbol conflicts.
    add_executable(joypad_btusb2usb)
    target_compile_definitions(joypad_btusb2usb PRIVATE
        CONFIG_BTUSB2USB=1
        CONFIG_USB=1
        CONFIG_NO_NEOPIXEL=1
        PICO_DEFAULT_PIO_USB_DP_PIN=16
        BOARD_TUH_RHPORT=1
        USE_BOOTSEL_BUTTON=1
        CYW43_LWIP=0
        BTSTACK_USE_CYW43=1
        ENABLE_BTSTACK=1
        CONFIG_BT_HOST=1  # register BT-HID drivers (see #188 regression)
    )
    target_sources(joypad_btusb2usb PUBLIC
        ${COMMON_SOURCES}
        ${USB_DEVICE_SOURCES}
        ${BTHID_DEVICE_SOURCES}
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/devices/generic/hid_parser.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/btstack/btstack_host.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/btstack/bt_device_db.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/transport/bt_transport.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/transport/bt_transport_cyw43.c
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2usb/app.c
    )
    target_include_directories(joypad_btusb2usb PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2usb
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
        ${CMAKE_CURRENT_SOURCE_DIR}/bt
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/btstack
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/bthid
        ${BTSTACK_INCLUDES}
    )
    target_link_libraries(joypad_btusb2usb PRIVATE
        ${COMMON_LIBRARIES}
        tinyusb_device
        tinyusb_pico_pio_usb
        pico_cyw43_arch_poll
        pico_btstack_ble
        pico_btstack_classic
        pico_btstack_cyw43
    )
    joypad_target_common(joypad_btusb2usb)
    pico_enable_stdio_usb(joypad_btusb2usb 0)
    pico_enable_stdio_uart(joypad_btusb2usb 1)

    # --- USB to WiFi Remote Play (Pico W / Pico 2 W: PIO-USB host + CYW43 WiFi station) ---
    # USB controllers via PIO-USB host → PS Remote Play over WiFi (station mode).
    # Native USB device = CDC for web-config provisioning. CYW43 in WiFi (lwip)
    # mode, NOT Bluetooth — so no BTstack. Session engine is stubbed (staged
    # chiaki port; needs a Pico 2 W for RAM). Same PIO0/CYW43-PIO1 coexistence as
    # btusb2usb. See .dev/docs/ps5-remoteplay-output.md.
    # On-device PS5 pairing needs chiaki's rpcrypt (AGPL, vendored under src/lib/
    # chiaki which is gitignored/local-only). When present, compile the real
    # registration + rpcrypt; otherwise fall back to the stub so CI/fresh checkouts
    # still build. See src/net/remoteplay/rp_regist.h.
    set(CHIAKI_RPCRYPT ${CMAKE_CURRENT_SOURCE_DIR}/lib/chiaki/src/rpcrypt.c)
    if(EXISTS ${CHIAKI_RPCRYPT})
        set(RP_REGIST_SOURCES
            ${CMAKE_CURRENT_SOURCE_DIR}/net/remoteplay/rp_regist.c
            ${CHIAKI_RPCRYPT}
            # session-engine crypto: reuse chiaki's gkcrypt.c byte-exact (init with
            # key_buf_chunks=0 → no worker thread). ecdh is reimplemented (chiaki's
            # mbedtls path uses an old ECDH struct API pico's mbedtls dropped).
            ${CMAKE_CURRENT_SOURCE_DIR}/lib/chiaki/src/gkcrypt.c
            # pure serialization reused byte-exact (controller state, launch spec, base64)
            ${CMAKE_CURRENT_SOURCE_DIR}/lib/chiaki/src/feedback.c
            ${CMAKE_CURRENT_SOURCE_DIR}/lib/chiaki/src/launchspec.c
            ${CMAKE_CURRENT_SOURCE_DIR}/lib/chiaki/src/base64.c
            ${CMAKE_CURRENT_SOURCE_DIR}/net/remoteplay/rp_chiaki_shim.c
            ${CMAKE_CURRENT_SOURCE_DIR}/net/remoteplay/rp_session_crypto.c
            # the poll-mode streaming session engine + protobuf codec
            ${CMAKE_CURRENT_SOURCE_DIR}/net/remoteplay/rp_proto.c
            ${CMAKE_CURRENT_SOURCE_DIR}/net/remoteplay/rp_session_stream.c)
        message(STATUS "usb2wifi: chiaki vendored — PS5 pairing + streaming engine enabled")
    else()
        set(RP_REGIST_SOURCES
            ${CMAKE_CURRENT_SOURCE_DIR}/net/remoteplay/rp_regist_stub.c
            ${CMAKE_CURRENT_SOURCE_DIR}/net/remoteplay/rp_session_stub.c)
        message(STATUS "usb2wifi: chiaki not vendored — PS5 pairing + streaming stubbed")
    endif()

    add_executable(joypad_usb2wifi)
    target_compile_definitions(joypad_usb2wifi PRIVATE
        CONFIG_USB2WIFI=1
        CONFIG_USB=1
        CONFIG_NO_NEOPIXEL=1
        PICO_DEFAULT_PIO_USB_DP_PIN=16
        BOARD_TUH_RHPORT=1
        USE_BOOTSEL_BUTTON=1
        CYW43_LWIP=1
        PICO_CYW43_ARCH_POLL=1
    )
    target_sources(joypad_usb2wifi PUBLIC
        ${COMMON_SOURCES}
        ${USB_DEVICE_SOURCES}
        ${CMAKE_CURRENT_SOURCE_DIR}/net/remoteplay/rp_config.c
        ${CMAKE_CURRENT_SOURCE_DIR}/net/remoteplay/wifi_station.c
        ${CMAKE_CURRENT_SOURCE_DIR}/net/remoteplay/rp_discovery.c
        ${CMAKE_CURRENT_SOURCE_DIR}/net/remoteplay/rp_oauth.c
        ${CMAKE_CURRENT_SOURCE_DIR}/net/remoteplay/remoteplay_output.c
        # rp_session engine (stream when chiaki vendored, else stub) is in RP_REGIST_SOURCES
        ${RP_REGIST_SOURCES}
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2wifi/app.c
    )
    if(EXISTS ${CHIAKI_RPCRYPT})
        target_include_directories(joypad_usb2wifi PUBLIC
            # compat shims FIRST: satisfy chiaki's <sys/socket.h>/<arpa/inet.h>/
            # <netdb.h> includes with minimal bare-metal stubs (types + byte order;
            # never linked — we reimplement the socket paths on raw lwip)
            ${CMAKE_CURRENT_SOURCE_DIR}/net/remoteplay/chiaki_compat
            ${CMAKE_CURRENT_SOURCE_DIR}/lib/chiaki/include)
        target_compile_definitions(joypad_usb2wifi PRIVATE
            CHIAKI_LIB_ENABLE_MBEDTLS=1 RP_HAVE_REGIST=1)
    endif()
    target_include_directories(joypad_usb2wifi PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2wifi
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
        ${CMAKE_CURRENT_SOURCE_DIR}
        # on the include path so -DMBEDTLS_CONFIG_FILE="rp_mbedtls_config.h" resolves
        ${CMAKE_CURRENT_SOURCE_DIR}/net/remoteplay
    )
    # TLS client for PSN OAuth (rp_oauth.c): altcp_tls + mbedTLS with a TLS-1.2
    # client config (distinct from the PS4 RSA-only ps4_mbedtls_config.h).
    target_compile_definitions(joypad_usb2wifi PRIVATE
        MBEDTLS_CONFIG_FILE="rp_mbedtls_config.h"
    )
    target_link_libraries(joypad_usb2wifi PRIVATE
        ${COMMON_LIBRARIES}
        tinyusb_device
        tinyusb_pico_pio_usb
        pico_cyw43_arch_lwip_poll
        pico_lwip_mbedtls
        pico_mbedtls
    )
    joypad_target_common(joypad_usb2wifi)
    pico_enable_stdio_usb(joypad_usb2wifi 0)
    pico_enable_stdio_uart(joypad_usb2wifi 1)

    # --- USB to BLE (Pico W / Pico 2 W: PIO-USB host + CYW43 BLE peripheral output) ---
    # USB controllers via PIO-USB host → BLE gamepad output via HOGP peripheral.
    # Uses CYW43 Bluetooth for BLE advertising + GATT server (hids_device).
    # Same PIO/DMA resource management as btusb2usb.
    add_executable(joypad_usb2ble)
    target_compile_definitions(joypad_usb2ble PRIVATE
        CONFIG_USB2BLE=1
        CONFIG_USB=1
        CONFIG_NO_NEOPIXEL=1
        PICO_DEFAULT_PIO_USB_DP_PIN=16
        BOARD_TUH_RHPORT=1
        USE_BOOTSEL_BUTTON=1
        CYW43_LWIP=0
        BTSTACK_USE_CYW43=1
        ENABLE_BTSTACK=1
        CONFIG_BT_HOST=1  # register BT-HID drivers (see #188 regression)
    )
    target_sources(joypad_usb2ble PUBLIC
        ${COMMON_SOURCES}
        ${USB_DEVICE_SOURCES}
        ${BTHID_DEVICE_SOURCES}
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/devices/generic/hid_parser.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/btstack/btstack_host.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/btstack/bt_device_db.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/transport/bt_transport.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/transport/bt_transport_cyw43.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/ble_output/ble_output.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/ble_output/ble_output_keyboard.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/ble_output/ble_output_mouse.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/ble_output/ble_output_xbox.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/ble_output/ble_nus.c
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2ble/app.c
    )
    target_include_directories(joypad_usb2ble PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2ble
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
        ${CMAKE_CURRENT_SOURCE_DIR}/bt
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/btstack
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/bthid
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/ble_output
        ${BTSTACK_INCLUDES}
    )
    # Xbox BLE GATT database — compiled as a separate library to avoid
    # profile_data symbol collision with the standard composite GATT.
    add_library(usb2ble_xbox_gatt OBJECT
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/ble_output/ble_xbox_gatt_db.c
    )
    target_include_directories(usb2ble_xbox_gatt PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/ble_output
        ${BTSTACK_INCLUDES}
    )
    pico_btstack_make_gatt_header(usb2ble_xbox_gatt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/bt/ble_output/ble_xbox.gatt)
    target_link_libraries(joypad_usb2ble PRIVATE
        ${COMMON_LIBRARIES}
        tinyusb_device
        tinyusb_pico_pio_usb
        pico_cyw43_arch_poll
        pico_btstack_ble
        pico_btstack_classic
        pico_btstack_cyw43
        usb2ble_xbox_gatt
    )
    pico_btstack_make_gatt_header(joypad_usb2ble PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/bt/ble_output/ble_gamepad.gatt)
    joypad_target_common(joypad_usb2ble)
    pico_enable_stdio_usb(joypad_usb2ble 0)
    pico_enable_stdio_uart(joypad_usb2ble 1)

    # --- ControllerBTUSB (Pico W / Pico 2 W: JoyWing sensor + BLE peripheral + USB) ---
    # Full-featured universal controller on Pico W:
    #   GPIO pad + JoyWing → BLE + USB device output
    #   BT Central (CYW43) for BT controller input
    #   PIO-USB host for USB controller input (D+ pin configurable via web config)
    add_executable(joypad_controller_btusb)
    target_compile_definitions(joypad_controller_btusb PRIVATE
        CONFIG_CONTROLLER_BTUSB=1
        CONFIG_USB=1
        CONFIG_PAD_INPUT=1
        USE_BOOTSEL_BUTTON=1
        CYW43_LWIP=0
        BTSTACK_USE_CYW43=1
        ENABLE_BTSTACK=1
        REQUIRE_BLE_OUTPUT=1
        REQUIRE_BT_INPUT=1
        # Bluetooth Classic output modes (e.g. Switch Pro Controller emulation).
        # Only the CYW43 radio supports BR/EDR — ESP32-S3 / nRF52840 builds are
        # BLE-only and must NOT define this, so those modes are hidden + rejected
        # by the mode-list/select plumbing (see ble_output_mode_available()).
        CONFIG_BT_CLASSIC_OUTPUT=1
        SENSOR_PAD=1
        SENSOR_JOYWING=1
        PICO_DEFAULT_PIO_USB_DP_PIN=16
        PIO_USB_DP_PIN_DEFAULT=16
        BOARD_TUH_RHPORT=1

        # Optional: 4-LED player indicator on raw GPIOs (PS3/Switch-style).
        # Define all four pins to enable. Active-high. LED1↔bit0, LED2↔bit1,
        # LED3↔bit2, LED4↔bit3 of the PLAYER_LEDS[] bitmap. Player 1 lights
        # LED1 alone, P5..P10 use the standard combo patterns. See
        # .dev/docs/player_leds_gpio.md for wiring/usage.
        # PLAYER_LED_PIN_1=10
        # PLAYER_LED_PIN_2=11
        # PLAYER_LED_PIN_3=12
        # PLAYER_LED_PIN_4=13
        CONFIG_BT_HOST=1  # register BT-HID drivers (see #188 regression)
    )
    target_sources(joypad_controller_btusb PUBLIC
        ${CORE_SOURCES}
        ${USB_HOST_SOURCES}
        ${USB_DEVICE_SOURCES}
        ${CONTROLLER_SOURCES}
        ${CMAKE_CURRENT_SOURCE_DIR}/stubs_peripheral.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/joywing/joywing_input.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/seesaw/seesaw.c
        ${CMAKE_CURRENT_SOURCE_DIR}/platform/rp2040/platform_i2c_rp2040.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/ble_output/ble_output.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/ble_output/ble_output_keyboard.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/ble_output/ble_output_mouse.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/ble_output/ble_output_xbox.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/ble_output/ble_nus.c
        # Switch Pro Controller over BT Classic (build-gated by CONFIG_BT_CLASSIC_OUTPUT).
        # Transport-agnostic protocol engine + emulated SPI flash; the BTstack Classic
        # HID-device role that drives them is wired separately.
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/switch_bt/switch_proto.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/switch_bt/switch_spi.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/switch_bt/switch_bt.c
        ${BTHID_DEVICE_SOURCES}
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbh/hid/devices/generic/hid_parser.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/btstack/btstack_host.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/btstack/bt_device_db.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/transport/bt_transport.c
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/transport/bt_transport_cyw43.c
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/controller_btusb/app.c
    )
    target_include_directories(joypad_controller_btusb PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/controller_btusb
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
        ${CMAKE_CURRENT_SOURCE_DIR}/pad
        ${CMAKE_CURRENT_SOURCE_DIR}/bt
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/btstack
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/bthid
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/ble_output
        ${BTSTACK_INCLUDES}
    )
    # Xbox BLE GATT database — separate library to avoid profile_data collision
    add_library(controller_btusb_xbox_gatt OBJECT
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/ble_output/ble_xbox_gatt_db.c
    )
    target_include_directories(controller_btusb_xbox_gatt PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/ble_output
        ${BTSTACK_INCLUDES}
    )
    pico_btstack_make_gatt_header(controller_btusb_xbox_gatt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/bt/ble_output/ble_xbox.gatt)
    target_link_libraries(joypad_controller_btusb PRIVATE
        pico_stdlib
        pico_multicore
        pico_rand
        hardware_pio
        hardware_i2c
        hardware_adc
        hardware_pwm
        hardware_spi
        pico_cyw43_arch_poll
        pico_btstack_ble
        pico_btstack_classic
        pico_btstack_cyw43
        tinyusb_device
        tinyusb_board
        tinyusb_host
        tinyusb_pico_pio_usb
        xinput_host
        controller_btusb_xbox_gatt
    )
    pico_btstack_make_gatt_header(joypad_controller_btusb PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/bt/ble_output/ble_gamepad.gatt)
    joypad_target_common(joypad_controller_btusb)
    pico_enable_stdio_usb(joypad_controller_btusb 0)
    pico_enable_stdio_uart(joypad_controller_btusb 1)

    # --- WiFi to USB (Pico W / Pico 2 W WiFi AP mode) ---
    add_executable(joypad_wifi2usb)
    target_compile_definitions(joypad_wifi2usb PRIVATE
        CONFIG_WIFI2USB=1
        CONFIG_USB=1
        DISABLE_USB_HOST=1
        USE_BOOTSEL_BUTTON=1
        CYW43_LWIP=1
        # WiFi AP mode + BLE beacon for iOS discovery
        PICO_CYW43_ARCH_POLL=1
        BTSTACK_USE_CYW43=1
    )
    target_sources(joypad_wifi2usb PUBLIC
        ${CORE_SOURCES}
        ${USB_DEVICE_SOURCES}
        ${CMAKE_CURRENT_SOURCE_DIR}/wifi/jocp/jocp_input.c
        ${CMAKE_CURRENT_SOURCE_DIR}/wifi/jocp/wifi_transport.c
        ${CMAKE_CURRENT_SOURCE_DIR}/wifi/jocp/dhcpserver.c
        ${CMAKE_CURRENT_SOURCE_DIR}/wifi/ble_beacon.c
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/wifi2usb/app.c
    )
    target_include_directories(joypad_wifi2usb PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/wifi2usb
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
        ${CMAKE_CURRENT_SOURCE_DIR}/wifi/jocp
        ${CMAKE_CURRENT_SOURCE_DIR}/wifi
        ${CMAKE_CURRENT_SOURCE_DIR}/bt/btstack
    )
    target_link_libraries(joypad_wifi2usb PRIVATE
        pico_stdlib
        pico_multicore
        pico_rand
        pico_cyw43_arch_lwip_poll
        pico_btstack_cyw43
        pico_btstack_ble
        pico_btstack_classic
        pico_btstack_flash_bank
        tinyusb_device
        tinyusb_board
    )
    joypad_target_common(joypad_wifi2usb)
    pico_enable_stdio_usb(joypad_wifi2usb 0)
    pico_enable_stdio_uart(joypad_wifi2usb 1)
endif()

# --- Controller for ABB (RP2040 Advanced Breakout Board - Passthrough) ---
# ABB is a plain RP2040 with no BT antenna — USB device output only, no BLE.
# USB host via PIO-USB on GPIO 23/24 (USB-A passthrough port, slider in USB position).
add_executable(joypad_controller_btusb_rp2040_abb)
target_compile_definitions(joypad_controller_btusb_rp2040_abb PRIVATE
    CONFIG_CONTROLLER_BTUSB=1
    CONFIG_USB=1
    CONFIG_PAD_INPUT=1
    USE_BOOTSEL_BUTTON=1
    REQUIRE_BLE_OUTPUT=0
    REQUIRE_BT_INPUT=1
    SENSOR_PAD=1
    PAD_CONFIG_ABB=1
    SENSOR_JOYWING=1
    BOARD_LED_PIN=25
    BOARD_NAME="rp2040_abb"
    PICO_DEFAULT_PIO_USB_DP_PIN=23
    PIO_USB_DP_PIN_DEFAULT=23
    BOARD_TUH_RHPORT=1
)
target_sources(joypad_controller_btusb_rp2040_abb PUBLIC
    ${CORE_SOURCES}
    ${USB_HOST_SOURCES}
    ${USB_DEVICE_SOURCES}
    ${CONTROLLER_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs_peripheral.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/joywing/joywing_input.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/seesaw/seesaw.c
    ${CMAKE_CURRENT_SOURCE_DIR}/platform/rp2040/platform_i2c_rp2040.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/controller_btusb/app.c
)
target_include_directories(joypad_controller_btusb_rp2040_abb PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/controller_btusb
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
    ${CMAKE_CURRENT_SOURCE_DIR}/pad
    ${CMAKE_CURRENT_SOURCE_DIR}/bt
    ${CMAKE_CURRENT_SOURCE_DIR}/bt/btstack
    ${CMAKE_CURRENT_SOURCE_DIR}/bt/bthid
)
target_link_libraries(joypad_controller_btusb_rp2040_abb PRIVATE
    pico_stdlib
    pico_multicore
    pico_rand
    hardware_pio
    hardware_i2c
    hardware_adc
    hardware_pwm
    hardware_spi
    tinyusb_device
    tinyusb_board
    tinyusb_host
    tinyusb_pico_pio_usb
    xinput_host
)
joypad_add_btstack(joypad_controller_btusb_rp2040_abb)
joypad_target_common(joypad_controller_btusb_rp2040_abb)
pico_enable_stdio_usb(joypad_controller_btusb_rp2040_abb 0)
pico_enable_stdio_uart(joypad_controller_btusb_rp2040_abb 1)

# --- Controller BTUSB: Fisher Price V1 (KB2040, button-only mod) ---
add_executable(joypad_controller_btusb_fisherprice_v1)
target_compile_definitions(joypad_controller_btusb_fisherprice_v1 PRIVATE
    CONFIG_CONTROLLER_BTUSB=1 CONFIG_USB=1 CONFIG_PAD_INPUT=1
    DISABLE_USB_HOST=1 REQUIRE_BLE_OUTPUT=0
    SENSOR_PAD=1 PAD_CONFIG_FISHERPRICE_V1=1
    BUTTON_USER_GPIO=18 BOARD_NAME="kb2040"
)
target_sources(joypad_controller_btusb_fisherprice_v1 PUBLIC
    ${CORE_SOURCES} ${USB_DEVICE_SOURCES} ${CONTROLLER_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs_peripheral.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/controller_btusb/app.c
)
target_include_directories(joypad_controller_btusb_fisherprice_v1 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/controller_btusb
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
    ${CMAKE_CURRENT_SOURCE_DIR}/pad
)
target_link_libraries(joypad_controller_btusb_fisherprice_v1 PRIVATE
    pico_stdlib pico_multicore pico_rand hardware_pio hardware_i2c hardware_adc
    hardware_pwm hardware_spi tinyusb_device tinyusb_board
)
joypad_target_common(joypad_controller_btusb_fisherprice_v1)

# --- Controller BTUSB: Fisher Price V2 (KB2040, analog stick + shoulders mod) ---
add_executable(joypad_controller_btusb_fisherprice_v2)
target_compile_definitions(joypad_controller_btusb_fisherprice_v2 PRIVATE
    CONFIG_CONTROLLER_BTUSB=1 CONFIG_USB=1 CONFIG_PAD_INPUT=1
    DISABLE_USB_HOST=1 REQUIRE_BLE_OUTPUT=0
    SENSOR_PAD=1 PAD_CONFIG_FISHERPRICE_V2=1
    BUTTON_USER_GPIO=18 BOARD_NAME="kb2040"
)
target_sources(joypad_controller_btusb_fisherprice_v2 PUBLIC
    ${CORE_SOURCES} ${USB_DEVICE_SOURCES} ${CONTROLLER_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs_peripheral.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/controller_btusb/app.c
)
target_include_directories(joypad_controller_btusb_fisherprice_v2 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/controller_btusb
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
    ${CMAKE_CURRENT_SOURCE_DIR}/pad
)
target_link_libraries(joypad_controller_btusb_fisherprice_v2 PRIVATE
    pico_stdlib pico_multicore pico_rand hardware_pio hardware_i2c hardware_adc
    hardware_pwm hardware_spi tinyusb_device tinyusb_board
)
joypad_target_common(joypad_controller_btusb_fisherprice_v2)

# --- Controller BTUSB: Alpakka (Pico) ---
add_executable(joypad_controller_btusb_alpakka)
target_compile_definitions(joypad_controller_btusb_alpakka PRIVATE
    CONFIG_CONTROLLER_BTUSB=1 CONFIG_USB=1 CONFIG_PAD_INPUT=1
    DISABLE_USB_HOST=1 REQUIRE_BLE_OUTPUT=0
    SENSOR_PAD=1 PAD_CONFIG_ALPAKKA=1
    USE_BOOTSEL_BUTTON=1 BOARD_NAME="pico" BOARD_LED_PIN=25
)
target_sources(joypad_controller_btusb_alpakka PUBLIC
    ${CORE_SOURCES} ${USB_DEVICE_SOURCES} ${CONTROLLER_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs_peripheral.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/controller_btusb/app.c
)
target_include_directories(joypad_controller_btusb_alpakka PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/controller_btusb
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
    ${CMAKE_CURRENT_SOURCE_DIR}/pad
)
target_link_libraries(joypad_controller_btusb_alpakka PRIVATE
    pico_stdlib pico_multicore pico_rand hardware_pio hardware_i2c hardware_adc
    hardware_pwm hardware_spi tinyusb_device tinyusb_board
)
joypad_target_common(joypad_controller_btusb_alpakka)

# --- SNES to USB ---
add_executable(joypad_snes2usb)
target_compile_definitions(joypad_snes2usb PRIVATE CONFIG_SNES2USB=1 CONFIG_USB=1 DISABLE_USB_HOST=1)
target_sources(joypad_snes2usb PUBLIC ${CORE_SOURCES} ${USB_DEVICE_SOURCES}
    ${SNES_HOST_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/snes2usb/app.c
)
target_include_directories(joypad_snes2usb PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/snes2usb
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/snes
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/SNESpad/src
)
target_link_libraries(joypad_snes2usb PRIVATE pico_stdlib pico_multicore hardware_pio pico_rand tinyusb_device tinyusb_board)
joypad_target_common(joypad_snes2usb)

# --- PSX2USB (PS1/PS2 controller -> USB HID) ---
add_executable(joypad_psx2usb)
target_compile_definitions(joypad_psx2usb PRIVATE CONFIG_PSX2USB=1 CONFIG_USB=1 DISABLE_USB_HOST=1
    USE_BOOTSEL_BUTTON=1)
# QT Py RP2040 wiring (Adafruit learn guide): A0/A1/A2/A3 map to GPIO29/28/27/26
# (the board numbers the analog pads in DESCENDING GPIO order). Wires:
# A0=DAT=GP29, A1=CMD=GP28, A2=ATT=GP27, A3=CLK=GP26.
if(PICO_BOARD STREQUAL "adafruit_qtpy_rp2040")
    target_compile_definitions(joypad_psx2usb PRIVATE
        PSX_PIN_DAT=29 PSX_PIN_CMD=28 PSX_PIN_ATT=27 PSX_PIN_CLK=26)
# KB2040 wiring on the A0-A3 pads: A0=CLK=GP26, A1=ATT=GP27, A2=CMD=GP28,
# A3=DAT=GP29. CLK/ATT (26/27) must stay consecutive for the PIO side-set.
elseif(PICO_BOARD STREQUAL "adafruit_kb2040")
    target_compile_definitions(joypad_psx2usb PRIVATE
        PSX_PIN_DAT=29 PSX_PIN_CMD=28 PSX_PIN_ATT=27 PSX_PIN_CLK=26)
# Raspberry Pi Pico wiring: DAT=GP22, CMD=GP19, ATT(LATCH)=GP21, CLK=GP20,
# VCC=3V3, GND, 10k pull-up on DAT.
elseif(PICO_BOARD STREQUAL "pico")
    target_compile_definitions(joypad_psx2usb PRIVATE
        PSX_PIN_DAT=22 PSX_PIN_CMD=19 PSX_PIN_ATT=21 PSX_PIN_CLK=20)
endif()
target_sources(joypad_psx2usb PUBLIC ${CORE_SOURCES} ${USB_DEVICE_SOURCES}
    ${PSX_HOST_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/psx2usb/app.c
)
target_include_directories(joypad_psx2usb PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/psx2usb
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/psx
)
target_link_libraries(joypad_psx2usb PRIVATE pico_stdlib pico_multicore hardware_pio hardware_dma pico_rand tinyusb_device tinyusb_board)
pico_generate_pio_header(joypad_psx2usb ${CMAKE_CURRENT_SOURCE_DIR}/native/host/psx/psx.pio)
joypad_target_common(joypad_psx2usb)

# --- WII2N64 (Wii extension -> N64) — Pi Pico target ---
# Pico has no NeoPixel or STEMMA QT. I2C0 on GP4/GP5, N64 data on GP2.
add_executable(joypad_wii2n64)
target_compile_definitions(joypad_wii2n64 PRIVATE
    CONFIG_WII2N64=1 CONFIG_USB=1 DISABLE_USB_HOST=1
    CONFIG_NO_NEOPIXEL=1
    N64_DATA_PIN=2
    N64_PIN_DATA=2
    PICO_DEFAULT_UART=0
    PICO_DEFAULT_UART_TX_PIN=0
    PICO_DEFAULT_UART_RX_PIN=1
)
target_sources(joypad_wii2n64 PUBLIC ${CORE_SOURCES} ${USB_DEVICE_SOURCES}
    ${WII_HOST_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/n64/n64_device.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/src/joybus.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/src/N64Console.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/wii2n64/app.c
)
target_include_directories(joypad_wii2n64 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/wii2n64
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/wii_ext
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/n64
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/wii_ext
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/include
)
target_link_libraries(joypad_wii2n64 PRIVATE pico_stdlib pico_multicore hardware_pio hardware_i2c hardware_flash pico_rand tinyusb_device tinyusb_board)
joypad_target_common(joypad_wii2n64)
pico_generate_pio_header(joypad_wii2n64 ${CMAKE_CURRENT_LIST_DIR}/lib/joybus-pio/src/joybus.pio)

# --- WII2GC (Wii extension -> GameCube) ---
add_executable(joypad_wii2gc)
target_compile_definitions(joypad_wii2gc PRIVATE CONFIG_WII2GC=1 CONFIG_USB=1 DISABLE_USB_HOST=1
    PICO_DEFAULT_UART=0
    PICO_DEFAULT_UART_TX_PIN=0
    PICO_DEFAULT_UART_RX_PIN=1
)
target_sources(joypad_wii2gc PUBLIC ${CORE_SOURCES} ${USB_DEVICE_SOURCES}
    ${WII_HOST_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/gamecube/gamecube_device.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/src/joybus.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/src/GamecubeConsole.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/wii2gc/app.c
)
target_include_directories(joypad_wii2gc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/wii2gc
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/wii_ext
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/gamecube
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/wii_ext
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/include
)
target_link_libraries(joypad_wii2gc PRIVATE pico_stdlib pico_multicore hardware_pio hardware_i2c hardware_flash pico_rand tinyusb_device tinyusb_board)
joypad_target_common(joypad_wii2gc)
pico_generate_pio_header(joypad_wii2gc ${CMAKE_CURRENT_LIST_DIR}/lib/joybus-pio/src/joybus.pio)

# --- WII2USB (Wii extension -> USB HID) ---
add_executable(joypad_wii2usb)
target_compile_definitions(joypad_wii2usb PRIVATE CONFIG_WII2USB=1 CONFIG_USB=1 DISABLE_USB_HOST=1
    PICO_DEFAULT_UART=0
    PICO_DEFAULT_UART_TX_PIN=0
    PICO_DEFAULT_UART_RX_PIN=1
    USE_BOOTSEL_BUTTON=1
)
target_sources(joypad_wii2usb PUBLIC ${CORE_SOURCES} ${USB_DEVICE_SOURCES}
    ${WII_HOST_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/wii2usb/app.c
)
target_include_directories(joypad_wii2usb PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/wii2usb
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/wii_ext
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/wii_ext
)
target_link_libraries(joypad_wii2usb PRIVATE pico_stdlib pico_multicore hardware_pio hardware_i2c pico_rand tinyusb_device tinyusb_board)
joypad_target_common(joypad_wii2usb)

# --- N642USB (N64 -> USB HID) ---
add_executable(joypad_n642usb)
target_compile_definitions(joypad_n642usb PRIVATE CONFIG_N642USB=1 CONFIG_USB=1 DISABLE_USB_HOST=1 N64_PIN_DATA=29)
target_sources(joypad_n642usb PUBLIC ${CORE_SOURCES} ${USB_DEVICE_SOURCES}
    ${N64_HOST_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/n642usb/app.c
)
target_include_directories(joypad_n642usb PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/n642usb
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/n64
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/include
)
target_link_libraries(joypad_n642usb PRIVATE pico_stdlib pico_multicore hardware_pio pico_rand tinyusb_device tinyusb_board)
joypad_target_common(joypad_n642usb)
pico_generate_pio_header(joypad_n642usb ${CMAKE_CURRENT_LIST_DIR}/lib/joybus-pio/src/joybus.pio)

# --- NUON2USB (Nuon -> USB HID) ---
# Uses same nuon_device.c and polyface PIOs as usb2nuon (joypad_nuon).
# Currently in development/monitor mode.
add_executable(joypad_nuon2usb)
target_compile_definitions(joypad_nuon2usb PRIVATE CONFIG_NUON2USB=1 CONFIG_USB=1 DISABLE_USB_HOST=1 CONFIG_NO_NEOPIXEL=1
    PICO_DEFAULT_UART=0
    PICO_DEFAULT_UART_TX_PIN=0
    PICO_DEFAULT_UART_RX_PIN=1
)
target_sources(joypad_nuon2usb PUBLIC ${CORE_SOURCES} ${USB_DEVICE_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/nuon/nuon_device.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/nuon2usb/app.c
)
target_include_directories(joypad_nuon2usb PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/nuon2usb
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/nuon
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
)
target_link_libraries(joypad_nuon2usb PRIVATE pico_stdlib pico_multicore hardware_pio pico_rand pico_bit_ops tinyusb_device tinyusb_board)
joypad_target_common(joypad_nuon2usb)
pico_generate_pio_header(joypad_nuon2usb ${CMAKE_CURRENT_LIST_DIR}/native/device/nuon/polyface_read.pio)
pico_generate_pio_header(joypad_nuon2usb ${CMAKE_CURRENT_LIST_DIR}/native/device/nuon/polyface_send.pio)
pico_generate_pio_header(joypad_nuon2usb ${CMAKE_CURRENT_LIST_DIR}/apps/nuon2usb/polyface_host_send.pio)
pico_generate_pio_header(joypad_nuon2usb ${CMAKE_CURRENT_LIST_DIR}/apps/nuon2usb/polyface_host_read.pio)
pico_generate_pio_header(joypad_nuon2usb ${CMAKE_CURRENT_LIST_DIR}/apps/nuon2usb/polyface_clock.pio)

# --- GC2USB (GameCube -> USB HID) ---
add_executable(joypad_gc2usb)
target_compile_definitions(joypad_gc2usb PRIVATE
    CONFIG_GC2USB=1 CONFIG_USB=1 DISABLE_USB_HOST=1 GC_PIN_DATA=29
    PICO_DEFAULT_UART=0
    PICO_DEFAULT_UART_TX_PIN=12
    PICO_DEFAULT_UART_RX_PIN=13
    USE_BOOTSEL_BUTTON=1   # KB2040 has no GP-wired user button; read BOOTSEL
    ${JOYPAD_GBA_LINK_BRIDGE_DEFS}
)
target_sources(joypad_gc2usb PUBLIC ${CORE_SOURCES} ${USB_DEVICE_SOURCES}
    ${GC_HOST_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/gc2usb/app.c
)
target_include_directories(joypad_gc2usb PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/gc2usb
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/gc
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/include
)
target_link_libraries(joypad_gc2usb PRIVATE pico_stdlib pico_multicore hardware_pio pico_rand tinyusb_device tinyusb_board)
joypad_target_common(joypad_gc2usb)
pico_enable_stdio_uart(joypad_gc2usb 1)
pico_generate_pio_header(joypad_gc2usb ${CMAKE_CURRENT_LIST_DIR}/lib/joybus-pio/src/joybus.pio)

# --- GC2USB (GameCube -> USB HID, RP2040-Zero) ---
# Note: this target is currently NOT what `make gc2usb_rp2040zero` actually
# builds — that uses joypad_gc2usb (GP29) with the rp2040zero board script.
# Keeping GC_PIN_DATA=29 here so that any future Makefile rewire (or anyone
# building this target directly) matches the wiring shipped to existing
# customers since v2.0.0.
add_executable(joypad_gc2usb_rp2040zero)
target_compile_definitions(joypad_gc2usb_rp2040zero PRIVATE
    CONFIG_GC2USB=1 CONFIG_USB=1 DISABLE_USB_HOST=1
    GC_PIN_DATA=29
    WS2812_PIN=16
    USE_BOOTSEL_BUTTON=1
    ${JOYPAD_GBA_LINK_BRIDGE_DEFS}
)
target_sources(joypad_gc2usb_rp2040zero PUBLIC ${CORE_SOURCES} ${USB_DEVICE_SOURCES}
    ${GC_HOST_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/gc2usb/app.c
)
target_include_directories(joypad_gc2usb_rp2040zero PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/gc2usb
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/gc
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/include
)
target_link_libraries(joypad_gc2usb_rp2040zero PRIVATE pico_stdlib pico_multicore hardware_pio pico_rand tinyusb_device tinyusb_board)
joypad_target_common(joypad_gc2usb_rp2040zero)
pico_generate_pio_header(joypad_gc2usb_rp2040zero ${CMAKE_CURRENT_LIST_DIR}/lib/joybus-pio/src/joybus.pio)

# --- GC2USB (GameCube -> USB HID, Pico) ---
# GP29 isn't broken out on the standard Pico, so use GP28 (last regular GPIO)
# for the joybus data line.
add_executable(joypad_gc2usb_pico)
target_compile_definitions(joypad_gc2usb_pico PRIVATE
    CONFIG_GC2USB=1 CONFIG_USB=1 DISABLE_USB_HOST=1
    GC_PIN_DATA=28
    BOARD_LED_PIN=25
    USE_BOOTSEL_BUTTON=1
    ${JOYPAD_GBA_LINK_BRIDGE_DEFS}
    BOARD_NAME="pico"
)
target_sources(joypad_gc2usb_pico PUBLIC ${CORE_SOURCES} ${USB_DEVICE_SOURCES}
    ${GC_HOST_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/gc2usb/app.c
)
target_include_directories(joypad_gc2usb_pico PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/gc2usb
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/gc
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/include
)
target_link_libraries(joypad_gc2usb_pico PRIVATE pico_stdlib pico_multicore hardware_pio pico_rand tinyusb_device tinyusb_board)
joypad_target_common(joypad_gc2usb_pico)
pico_generate_pio_header(joypad_gc2usb_pico ${CMAKE_CURRENT_LIST_DIR}/lib/joybus-pio/src/joybus.pio)

# --- GC2ETH (GameCube/GBA -> Dolphin TCP via CH9120 Ethernet) ---
# Waveshare RP2040-ETH (CH9120 UART-to-Ethernet variant). Joybus on
# GP2, CH9120 on GP17-21. M1 = skeleton/heartbeat only — see
# .dev/docs/gc2eth-ch9120-bridge.md for the full plan.
add_executable(joypad_gc2eth)
target_compile_definitions(joypad_gc2eth PRIVATE
    CONFIG_GC2ETH=1 CONFIG_USB=1 CONFIG_JOYBUS_BRIDGE=1 DISABLE_USB_HOST=1
    CONFIG_NO_NEOPIXEL=1   # board's WS2812 pin (GP25) may be unrouted on this variant
    USE_BOOTSEL_BUTTON=1
    GC_PIN_DATA=2
)
# Clone gc2usb_pico exactly: project USB stack + GC host + app. Stays
# in the default HID-gamepad+CDC branch of tusb_config. The HID
# gamepad is unused (we publish no router output to it) but the
# joypad-os CDC interface enumerates as /dev/cu.usbmodem* and
# carries debug logs via the binary protocol.
target_sources(joypad_gc2eth PUBLIC ${CORE_SOURCES} ${USB_DEVICE_SOURCES}
    ${GC_HOST_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/gc2eth/app.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/gc2eth/ch9120.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/gc2eth/wshare_ch9120.c
)
target_include_directories(joypad_gc2eth PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/gc2eth
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/gc
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/include
)
target_link_libraries(joypad_gc2eth PRIVATE
    pico_stdlib pico_multicore hardware_pio hardware_uart pico_rand
    tinyusb_device tinyusb_board
)
joypad_target_common(joypad_gc2eth)
pico_generate_pio_header(joypad_gc2eth ${CMAKE_CURRENT_LIST_DIR}/lib/joybus-pio/src/joybus.pio)
pico_enable_stdio_uart(joypad_gc2eth 1)   # USB-CDC breaks CH9120; debug via UART0 (GP0=TX, GP1=RX) @ 115200

# --- GC2ETH_FEATHER (GameCube/GBA -> Dolphin TCP via W5500) ---
# Adafruit Feather RP2040 USB Host + W5500 Ethernet FeatherWing.
# W5500 SPI tops out at ~50µs RTT (vs CH9120's 30ms), so Madden's
# multiboot timing budget actually fits. Same intercept-replay code as
# gc2eth — only the transport driver changes.
add_executable(joypad_gc2eth_feather)
target_compile_definitions(joypad_gc2eth_feather PRIVATE
    CONFIG_GC2ETH_FEATHER=1 CONFIG_USB=1 CONFIG_JOYBUS_BRIDGE=1 DISABLE_USB_HOST=1
    CONFIG_NO_NEOPIXEL=1
    USE_BOOTSEL_BUTTON=1
    GC_PIN_DATA=4
)
target_sources(joypad_gc2eth_feather PUBLIC ${CORE_SOURCES} ${USB_DEVICE_SOURCES}
    ${GC_HOST_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/gc2eth_feather/app.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/gc2eth_feather/w5500.c
)
target_include_directories(joypad_gc2eth_feather PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/gc2eth_feather
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/gc
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/include
)
target_link_libraries(joypad_gc2eth_feather PRIVATE
    pico_stdlib pico_multicore hardware_pio hardware_spi pico_rand
    tinyusb_device tinyusb_board
)
joypad_target_common(joypad_gc2eth_feather)
pico_generate_pio_header(joypad_gc2eth_feather ${CMAKE_CURRENT_LIST_DIR}/lib/joybus-pio/src/joybus.pio)
pico_enable_stdio_usb(joypad_gc2eth_feather 1)   # CDC over USB-C for debug + remote BOOTSEL

# --- GC2USB_FEATHER_USBHOST (joybus-PIO sanity test on the same hardware) ---
# Hardware-identical sibling of gc2eth_feather, minus the W5500. Same
# board, same GP4 wiring — but acts as a USB HID gamepad. Lets us
# isolate "joybus + GP4 wiring works" from "TCP path works." Plug a
# real GameCube controller into the cable's GC-end and you should see
# a HID device on the Mac.
add_executable(joypad_gc2usb_feather_usbhost)
target_compile_definitions(joypad_gc2usb_feather_usbhost PRIVATE
    CONFIG_GC2USB=1 CONFIG_USB=1 DISABLE_USB_HOST=1
    CONFIG_NO_NEOPIXEL=1
    USE_BOOTSEL_BUTTON=1
    GC_PIN_DATA=4
    ${JOYPAD_GBA_LINK_BRIDGE_DEFS}
)
target_sources(joypad_gc2usb_feather_usbhost PUBLIC ${CORE_SOURCES} ${USB_DEVICE_SOURCES}
    ${GC_HOST_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/gc2usb/app.c
)
target_include_directories(joypad_gc2usb_feather_usbhost PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/gc2usb
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/gc
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/include
)
target_link_libraries(joypad_gc2usb_feather_usbhost PRIVATE
    pico_stdlib pico_multicore hardware_pio pico_rand
    tinyusb_device tinyusb_board
)
joypad_target_common(joypad_gc2usb_feather_usbhost)
pico_generate_pio_header(joypad_gc2usb_feather_usbhost ${CMAKE_CURRENT_LIST_DIR}/lib/joybus-pio/src/joybus.pio)

# --- NEGEO2USB (NEOGEO -> USB HID) ---
add_executable(joypad_neogeo2usb)
target_compile_definitions(joypad_neogeo2usb PRIVATE CONFIG_NEOGEO2USB=1 CONFIG_USB=1 DISABLE_USB_HOST=1 BUTTON_USER_GPIO=${BOARD_BUTTON_GPIO})
target_sources(joypad_neogeo2usb PUBLIC ${CORE_SOURCES} ${USB_DEVICE_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/arcade/arcade_host.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/neogeo2usb/app.c
)
target_include_directories(joypad_neogeo2usb PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/neogeo2usb
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/arcade
)
target_link_libraries(joypad_neogeo2usb PRIVATE pico_stdlib pico_multicore hardware_pio pico_rand tinyusb_device tinyusb_board)
joypad_target_common(joypad_neogeo2usb)

# NEGEO2USB — RP2040-Zero (WS2812 on GP16, auto from board def)
add_executable(joypad_neogeo2usb_rp2040zero)
target_compile_definitions(joypad_neogeo2usb_rp2040zero PRIVATE
    CONFIG_NEOGEO2USB=1
    CONFIG_USB=1
    DISABLE_USB_HOST=1
    USE_BOOTSEL_BUTTON=1
    PICO_RP2040_ZERO_BUILD=1
)
target_sources(joypad_neogeo2usb_rp2040zero PUBLIC ${CORE_SOURCES} ${USB_DEVICE_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/arcade/arcade_host.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/neogeo2usb/app.c
)
target_include_directories(joypad_neogeo2usb_rp2040zero PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/neogeo2usb
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/arcade
)
target_link_libraries(joypad_neogeo2usb_rp2040zero PRIVATE pico_stdlib pico_multicore hardware_pio pico_rand tinyusb_device tinyusb_board)
joypad_target_common(joypad_neogeo2usb_rp2040zero)

# JVS2USB — RP2040-Zero (WS2812 on GP16, auto from board def)
add_executable(joypad_jvs2usb_rp2040zero)
target_compile_definitions(joypad_jvs2usb_rp2040zero PRIVATE
    CONFIG_JVS2USB=1
    CONFIG_USB=1
    DISABLE_USB_HOST=1
    USE_BOOTSEL_BUTTON=1
    PICO_RP2040_ZERO_BUILD=1
)
target_sources(joypad_jvs2usb_rp2040zero PUBLIC ${CORE_SOURCES} ${USB_DEVICE_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/jvs/jvs_host.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/jvs2usb/app.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/jvsio/jvsio_host.c
)
target_include_directories(joypad_jvs2usb_rp2040zero PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/jvs2usb
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/jvsio
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/jvs
)
target_link_libraries(joypad_jvs2usb_rp2040zero PRIVATE pico_stdlib pico_multicore hardware_pio hardware_uart pico_rand tinyusb_device tinyusb_board)
joypad_target_common(joypad_jvs2usb_rp2040zero)

# --- NES2USB (NES -> USB HID) ---
add_executable(joypad_nes2usb)
target_compile_definitions(joypad_nes2usb PRIVATE CONFIG_NES2USB=1 CONFIG_USB=1 DISABLE_USB_HOST=1)
target_sources(joypad_nes2usb PUBLIC ${CORE_SOURCES} ${USB_DEVICE_SOURCES}
    ${NES_HOST_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/nes2usb/app.c
)
target_include_directories(joypad_nes2usb PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/nes2usb
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/nes
)
target_link_libraries(joypad_nes2usb PRIVATE pico_stdlib pico_multicore hardware_pio pico_rand tinyusb_device tinyusb_board)
joypad_target_common(joypad_nes2usb)
pico_generate_pio_header(joypad_nes2usb ${CMAKE_CURRENT_LIST_DIR}/native/host/nes/nes_host.pio)


# --- PCE2USB (PCEngine -> USB HID) ---
add_executable(joypad_pce2usb)
target_compile_definitions(joypad_pce2usb PRIVATE CONFIG_PCE2USB=1 CONFIG_USB=1 DISABLE_USB_HOST=1)
target_sources(joypad_pce2usb PUBLIC ${CORE_SOURCES} ${USB_DEVICE_SOURCES}
    ${PCE_HOST_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/pce2usb/app.c
)
target_include_directories(joypad_pce2usb PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/pce2usb
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/pcengine
)
target_link_libraries(joypad_pce2usb PRIVATE pico_stdlib pico_multicore hardware_pio pico_rand tinyusb_device tinyusb_board)
# Status LED: Pico W / Pico 2 W's onboard LED is behind the CYW43 chip
# (WL_GPIO0), so bring up cyw43_arch (no wireless) just to drive it. A plain
# Pico's LED is a normal GPIO (GP25).
if (PICO_BOARD STREQUAL "pico_w" OR PICO_BOARD STREQUAL "pico2_w")
    target_compile_definitions(joypad_pce2usb PRIVATE BOARD_LED_CYW43=1)
    target_link_libraries(joypad_pce2usb PRIVATE pico_cyw43_arch_none)
elseif (PICO_BOARD STREQUAL "pico")
    target_compile_definitions(joypad_pce2usb PRIVATE BOARD_LED_PIN=25)
endif()
joypad_target_common(joypad_pce2usb)


# --- JAG2USB (Atari Jaguar -> USB HID) ---
add_executable(joypad_jag2usb)
target_compile_definitions(joypad_jag2usb PRIVATE CONFIG_JAG2USB=1 CONFIG_USB=1 DISABLE_USB_HOST=1)
target_sources(joypad_jag2usb PUBLIC ${CORE_SOURCES} ${USB_DEVICE_SOURCES}
    ${JAGUAR_HOST_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/jag2usb/app.c
)
target_include_directories(joypad_jag2usb PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/jag2usb
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/jaguar
)
target_link_libraries(joypad_jag2usb PRIVATE pico_stdlib pico_multicore hardware_pio pico_rand tinyusb_device tinyusb_board)
# Status LED: Pico W / Pico 2 W's onboard LED is behind the CYW43 chip
# (WL_GPIO0), so bring up cyw43_arch (no wireless) just to drive it. A plain
# Pico's LED is a normal GPIO (GP25).
if (PICO_BOARD STREQUAL "pico_w" OR PICO_BOARD STREQUAL "pico2_w")
    target_compile_definitions(joypad_jag2usb PRIVATE BOARD_LED_CYW43=1)
    target_link_libraries(joypad_jag2usb PRIVATE pico_cyw43_arch_none)
elseif (PICO_BOARD STREQUAL "pico")
    target_compile_definitions(joypad_jag2usb PRIVATE BOARD_LED_PIN=25)
endif()
joypad_target_common(joypad_jag2usb)


# --- LODGENET2USB (LodgeNet -> USB HID) ---
add_executable(joypad_lodgenet2usb)
target_compile_definitions(joypad_lodgenet2usb PRIVATE CONFIG_LODGENET2USB=1 CONFIG_USB=1 DISABLE_USB_HOST=1 BOARD_LED_PIN=25)
target_sources(joypad_lodgenet2usb PUBLIC ${CORE_SOURCES} ${USB_DEVICE_SOURCES}
    ${LODGENET_HOST_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/lodgenet2usb/app.c
)
target_include_directories(joypad_lodgenet2usb PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/lodgenet2usb
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/lodgenet
)
target_link_libraries(joypad_lodgenet2usb PRIVATE pico_stdlib pico_multicore hardware_pio pico_rand tinyusb_device tinyusb_board)
joypad_target_common(joypad_lodgenet2usb)
pico_generate_pio_header(joypad_lodgenet2usb ${CMAKE_CURRENT_LIST_DIR}/native/host/lodgenet/lodgenet.pio)


# --- LODGENET2N64 (LodgeNet -> N64 Console) ---
add_executable(joypad_lodgenet2n64)
target_compile_definitions(joypad_lodgenet2n64 PRIVATE
    CONFIG_LODGENET2N64=1
    DISABLE_USB_HOST=1
    CONFIG_NO_NEOPIXEL=1
    BOARD_LED_PIN=25
    N64_DATA_PIN=7
    NO_CUSTOM_UART_PINS=1
)
target_sources(joypad_lodgenet2n64 PUBLIC ${CORE_SOURCES}
    ${LODGENET_HOST_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/n64/n64_device.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/src/joybus.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/src/N64Console.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/lodgenet2n64/app.c
)
target_include_directories(joypad_lodgenet2n64 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/lodgenet2n64
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/lodgenet
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/n64
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/include
)
target_link_libraries(joypad_lodgenet2n64 PRIVATE pico_stdlib pico_multicore hardware_pio hardware_flash pico_rand)
joypad_target_common(joypad_lodgenet2n64)
pico_enable_stdio_usb(joypad_lodgenet2n64 1)
pico_enable_stdio_uart(joypad_lodgenet2n64 1)
pico_generate_pio_header(joypad_lodgenet2n64 ${CMAKE_CURRENT_LIST_DIR}/native/host/lodgenet/lodgenet.pio)
pico_generate_pio_header(joypad_lodgenet2n64 ${CMAKE_CURRENT_LIST_DIR}/lib/joybus-pio/src/joybus.pio)


# --- LODGENET2GC (LodgeNet -> GameCube Console) ---
add_executable(joypad_lodgenet2gc)
target_compile_definitions(joypad_lodgenet2gc PRIVATE
    CONFIG_LODGENET2GC=1
    CONFIG_USB=1
    DISABLE_USB_HOST=1
    CONFIG_NO_NEOPIXEL=1
    BOARD_LED_PIN=25
    GC_DATA_PIN=7
    NO_CUSTOM_UART_PINS=1
)
target_sources(joypad_lodgenet2gc PUBLIC ${CORE_SOURCES} ${USB_DEVICE_SOURCES}
    ${LODGENET_HOST_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/gamecube/gamecube_device.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/src/joybus.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/src/GamecubeConsole.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/lodgenet2gc/app.c
)
target_include_directories(joypad_lodgenet2gc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/lodgenet2gc
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/lodgenet
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/gamecube
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/joybus-pio/include
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
)
target_link_libraries(joypad_lodgenet2gc PRIVATE pico_stdlib pico_multicore hardware_pio hardware_flash pico_rand tinyusb_device tinyusb_board)
joypad_target_common(joypad_lodgenet2gc)
pico_enable_stdio_usb(joypad_lodgenet2gc 1)
pico_enable_stdio_uart(joypad_lodgenet2gc 1)
pico_generate_pio_header(joypad_lodgenet2gc ${CMAKE_CURRENT_LIST_DIR}/native/host/lodgenet/lodgenet.pio)
pico_generate_pio_header(joypad_lodgenet2gc ${CMAKE_CURRENT_LIST_DIR}/lib/joybus-pio/src/joybus.pio)


# ============================================================================
# CONTROLLER APPS (GPIO/pad input -> USB output)
# ============================================================================

set(CONTROLLER_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/controller
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
    ${CMAKE_CURRENT_SOURCE_DIR}/pad
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/uart
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host/uart
    ${CMAKE_CURRENT_SOURCE_DIR}/native/host
)

# --- Fisher Price V1 (button-only mod) ---
add_executable(joypad_controller_fisherprice_v1)
target_compile_definitions(joypad_controller_fisherprice_v1 PRIVATE
    CONFIG_CONTROLLER=1 CONFIG_USB=1 CONFIG_PAD_INPUT=1 CONTROLLER_TYPE_FISHERPRICE_V1=1 DISABLE_USB_HOST=1 BUTTON_USER_GPIO=${BOARD_BUTTON_GPIO}
)
target_sources(joypad_controller_fisherprice_v1 PUBLIC ${CORE_SOURCES} ${USB_DEVICE_SOURCES} ${CONTROLLER_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/controller/app.c
)
target_include_directories(joypad_controller_fisherprice_v1 PUBLIC ${CONTROLLER_INCLUDES})
target_link_libraries(joypad_controller_fisherprice_v1 PRIVATE ${CONTROLLER_LIBRARIES})
joypad_target_common(joypad_controller_fisherprice_v1)

# --- Fisher Price V2 (analog stick + shoulders mod) ---
add_executable(joypad_controller_fisherprice_v2)
target_compile_definitions(joypad_controller_fisherprice_v2 PRIVATE
    CONFIG_CONTROLLER=1 CONFIG_USB=1 CONFIG_PAD_INPUT=1 CONTROLLER_TYPE_FISHERPRICE_V2=1 DISABLE_USB_HOST=1 BUTTON_USER_GPIO=${BOARD_BUTTON_GPIO}
)
target_sources(joypad_controller_fisherprice_v2 PUBLIC ${CORE_SOURCES} ${USB_DEVICE_SOURCES} ${CONTROLLER_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/controller/app.c
)
target_include_directories(joypad_controller_fisherprice_v2 PUBLIC ${CONTROLLER_INCLUDES})
target_link_libraries(joypad_controller_fisherprice_v2 PRIVATE ${CONTROLLER_LIBRARIES})
joypad_target_common(joypad_controller_fisherprice_v2)

# --- Alpakka ---
add_executable(joypad_controller_alpakka)
target_compile_definitions(joypad_controller_alpakka PRIVATE
    CONFIG_CONTROLLER=1 CONFIG_USB=1 CONFIG_PAD_INPUT=1 CONTROLLER_TYPE_ALPAKKA=1 DISABLE_USB_HOST=1 BUTTON_USER_GPIO=${BOARD_BUTTON_GPIO}
)
target_sources(joypad_controller_alpakka PUBLIC ${CORE_SOURCES} ${USB_DEVICE_SOURCES} ${CONTROLLER_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/controller/app.c
)
target_include_directories(joypad_controller_alpakka PUBLIC ${CONTROLLER_INCLUDES})
target_link_libraries(joypad_controller_alpakka PRIVATE ${CONTROLLER_LIBRARIES})
joypad_target_common(joypad_controller_alpakka)

# --- MacroPad ---
add_executable(joypad_controller_macropad)
target_compile_definitions(joypad_controller_macropad PRIVATE
    CONFIG_CONTROLLER=1 CONFIG_USB=1 CONFIG_PAD_INPUT=1 CONTROLLER_TYPE_MACROPAD=1 DISABLE_USB_HOST=1 BUTTON_USER_GPIO=${BOARD_BUTTON_GPIO}
    ADAFRUIT_MACROPAD_RP2040=1 WS2812_NUM_PIXELS=12
    I2C_PEER_ENABLED=1
)
target_sources(joypad_controller_macropad PUBLIC ${CORE_SOURCES} ${USB_DEVICE_SOURCES} ${CONTROLLER_SOURCES}
    ${I2C_PEER_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/controller/app.c
)
target_include_directories(joypad_controller_macropad PUBLIC ${CONTROLLER_INCLUDES})
target_link_libraries(joypad_controller_macropad PRIVATE ${CONTROLLER_LIBRARIES} pico_i2c_slave)
joypad_target_common(joypad_controller_macropad)

# --- ControllerBTUSB (Feather RP2040, USB-only, JoyWing + OLED + Joy) ---
# No BLE on Feather RP2040 — USB device output only.
# JoyWing + OLED share I2C1 on STEMMA QT (SDA=2, SCL=3).
add_executable(joypad_controller_btusb_feather_rp2040)
target_compile_definitions(joypad_controller_btusb_feather_rp2040 PRIVATE
    DISABLE_USB_HOST=1
    BOARD_FEATHER_RP2040=1
    USE_BOOTSEL_BUTTON=1
    REQUIRE_BLE_OUTPUT=0
    SENSOR_JOYWING=1
    PICO_DEFAULT_UART=0
    PICO_DEFAULT_UART_TX_PIN=0
    PICO_DEFAULT_UART_RX_PIN=1
    # FeatherWing OLED 128x64 (SH1107) over I2C on STEMMA QT
    OLED_I2C_INST=1
    OLED_I2C_SDA_PIN=2
    OLED_I2C_SCL_PIN=3
    OLED_I2C_ADDR=0x3C
    OLED_BUTTON_A_PIN=9
    OLED_BUTTON_B_PIN=6
    OLED_BUTTON_C_PIN=5
)
target_sources(joypad_controller_btusb_feather_rp2040 PUBLIC
    ${CORE_SOURCES}
    ${USB_DEVICE_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/joywing/joywing_input.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/seesaw/seesaw.c
    ${CMAKE_CURRENT_SOURCE_DIR}/platform/rp2040/platform_i2c_rp2040.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services/display/display.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services/display/display_i2c.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services/display/joy_anim.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/controller_btusb/app.c
)
target_include_directories(joypad_controller_btusb_feather_rp2040 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/controller_btusb
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
)
target_link_libraries(joypad_controller_btusb_feather_rp2040 PRIVATE
    pico_stdlib pico_multicore hardware_pio hardware_i2c pico_rand tinyusb_device tinyusb_board
)
joypad_target_common(joypad_controller_btusb_feather_rp2040)
pico_enable_stdio_uart(joypad_controller_btusb_feather_rp2040 1)

# Feather RP2040 USB Host (#5723) variant of controller_btusb. Same JoyWing +
# OLED FeatherWing wiring on STEMMA QT (I2C1 = GP2/3). The onboard PIO USB
# host port (DP=GP16, DM=GP17, VBUSEN=GP18) accepts USB HID/X-input
# controllers as additional input — they merge with JoyWing/pad input into
# the same composite USB device output. CONFIG_UART_HOST lets joypad-mcp
# drive synthetic input over the TX/RX header.
add_executable(joypad_controller_btusb_feather_rp2040_usb_host)
target_compile_definitions(joypad_controller_btusb_feather_rp2040_usb_host PRIVATE
    CONFIG_CONTROLLER_BTUSB=1
    CONFIG_USB=1
    CONFIG_PAD_INPUT=1
    BOARD_FEATHER_RP2040=1
    USE_BOOTSEL_BUTTON=1
    REQUIRE_BLE_OUTPUT=0
    SENSOR_PAD=1
    SENSOR_JOYWING=1
    PAD_CONFIG_BOOT_WATCHDOG=1
    PICO_DEFAULT_UART=0
    PICO_DEFAULT_UART_TX_PIN=0
    PICO_DEFAULT_UART_RX_PIN=1
    # PIO USB host — onboard host port. D+ is GP16 (D- implicitly GP17,
    # PIO-USB always uses DP+1). VBUS load switch on GP18, active high.
    # Native USB peripheral is RHPort 0 (we use it as the device output);
    # PIO USB takes RHPort 1 for host.
    PICO_DEFAULT_PIO_USB_DP_PIN=16
    PICO_DEFAULT_PIO_USB_VBUSEN_PIN=18
    PICO_DEFAULT_PIO_USB_VBUSEN_STATE=1
    BOARD_TUH_RHPORT=1
    USBR_CDC_DEBUG=1
    # PIO assignment on this board (no CYW43): pico-pio-usb on PIO1, NeoPixel
    # on PIO0 (its default). Do NOT define CONFIG_NEOPIXEL_USE_PIO1 here — it
    # would put NeoPixel on the same PIO as PIO-USB and break enumeration
    # (PIO instruction memory + state machine collisions).
    # OLED FeatherWing 128x64 (SH1107) over I2C on STEMMA QT
    OLED_I2C_INST=1
    OLED_I2C_SDA_PIN=2
    OLED_I2C_SCL_PIN=3
    OLED_I2C_ADDR=0x3C
    OLED_BUTTON_A_PIN=9
    OLED_BUTTON_B_PIN=8
    OLED_BUTTON_C_PIN=7
    # MCP-driven synthetic input over the same UART that carries stdio logs
    # (UART0 on GPIO0/1, 115200 baud — TX/RX header pins, USB-UART bridge).
    CONFIG_UART_HOST=1
    UART_HOST_PERIPHERAL=uart0
    CONFIG_UART_HOST_TX_PIN=0
    CONFIG_UART_HOST_RX_PIN=1
    CONFIG_UART_HOST_BAUD=115200
)
target_sources(joypad_controller_btusb_feather_rp2040_usb_host PUBLIC
    ${CORE_SOURCES}
    ${USB_HOST_SOURCES}
    ${USB_DEVICE_SOURCES}
    ${CONTROLLER_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs_peripheral.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/joywing/joywing_input.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/seesaw/seesaw.c
    ${CMAKE_CURRENT_SOURCE_DIR}/platform/rp2040/platform_i2c_rp2040.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services/display/display_i2c.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services/display/joy_anim.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services/display/eyes_anim.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services/display/menu.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/controller_btusb/app.c
)
target_include_directories(joypad_controller_btusb_feather_rp2040_usb_host PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/controller_btusb
    ${CMAKE_CURRENT_SOURCE_DIR}/usb/usbd
)
target_link_libraries(joypad_controller_btusb_feather_rp2040_usb_host PRIVATE
    pico_stdlib pico_multicore pico_rand
    hardware_pio hardware_i2c hardware_adc hardware_pwm hardware_spi
    tinyusb_device tinyusb_board tinyusb_host tinyusb_pico_pio_usb xinput_host
)
joypad_target_common(joypad_controller_btusb_feather_rp2040_usb_host)
pico_enable_stdio_uart(joypad_controller_btusb_feather_rp2040_usb_host 1)

# ============================================================================
# USB2AMI (USB -> Amiga/Atari DE9, RP2040 Zero)
# DE9: UP=GP2 DOWN=GP3 LEFT=GP4 RIGHT=GP5 CLK=GP6 JOYMODE=GP7 DATA=GP8
# TXS0108E level shifting (3.3V <-> 5V)
# ============================================================================

add_executable(joypad_ami_rp2040zero)
target_compile_definitions(joypad_ami_rp2040zero PRIVATE
    CONFIG_AMIGA=1
    CONFIG_USB_HOST=1
    WS2812_PIN=16
    USE_BOOTSEL_BUTTON=1
    AMIGA_PIN_UP=2
    AMIGA_PIN_DOWN=3
    AMIGA_PIN_LEFT=4
    AMIGA_PIN_RIGHT=5
    AMIGA_PIN_CLK=6
    AMIGA_PIN_JOYMODE=7
    AMIGA_PIN_DATA=8
)
target_sources(joypad_ami_rp2040zero PUBLIC ${COMMON_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/amiga/amiga_device.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2ami/app.c
)
target_include_directories(joypad_ami_rp2040zero PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2ami
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/amiga
)
target_link_libraries(joypad_ami_rp2040zero PRIVATE ${COMMON_LIBRARIES} pico_multicore)
joypad_target_common(joypad_ami_rp2040zero)
joypad_add_btstack(joypad_ami_rp2040zero)

# ============================================================================
# USB2AMI (USB -> Amiga/Atari DE9, XIAO RP2040)
# DE9: UP=GP26 DOWN=GP27 LEFT=GP28 RIGHT=GP29 CLK=GP6 JOYMODE=GP7 DATA=GP0
# ============================================================================

add_executable(joypad_ami_xiao)
target_compile_definitions(joypad_ami_xiao PRIVATE
    CONFIG_AMIGA=1
    CONFIG_USB_HOST=1
    WS2812_PIN=12
    WS2812_POWER_PIN=11
    REQUIRE_FLASH_SETTINGS=1
    DISABLE_BUTTON_SERVICE=1
    AMIGA_PIN_UP=26
    AMIGA_PIN_DOWN=27
    AMIGA_PIN_LEFT=28
    AMIGA_PIN_RIGHT=29
    AMIGA_PIN_CLK=6
    AMIGA_PIN_JOYMODE=7
    AMIGA_PIN_DATA=0
)
target_sources(joypad_ami_xiao PUBLIC ${COMMON_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/amiga/amiga_device.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2ami/app.c
)
target_include_directories(joypad_ami_xiao PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/usb2ami
    ${CMAKE_CURRENT_SOURCE_DIR}/native/device/amiga
)
target_link_libraries(joypad_ami_xiao PRIVATE ${COMMON_LIBRARIES} pico_multicore)
joypad_target_common(joypad_ami_xiao)
joypad_add_btstack(joypad_ami_xiao)
pico_enable_stdio_uart(joypad_ami_xiao 0)
pico_enable_stdio_usb(joypad_ami_xiao 0)

# ============================================================================
# DEBUG BUILD CONFIGURATION
# ============================================================================

if(CMAKE_BUILD_TYPE MATCHES Debug)
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -Og")
else()
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3")
endif()

# ============================================================================
# PS4 LOCAL AUTH ATTACHMENT
# ============================================================================
# PS4 (DS4) local RSA authentication is a property of USB *output*, not of BT or
# any single app. Every target that presents a USB gamepad compiles the PS4
# output mode (ps4_mode.c, part of USB_DEVICE_SOURCES); console-output-only apps
# (usb2dc, usb2gc, usb2pce, …) do not, so they never link mbedTLS.
#
# Rather than wire each of the ~55 USB-output targets by hand (and risk new apps
# silently missing PS4 auth), attach the signing code + mbedTLS here, after all
# targets are defined, to any executable whose sources include ps4_mode.c. This
# is the single call site for joypad_add_ps4_local_auth() — the function is not
# idempotent (it appends sources), so it must be invoked exactly once per target.
#
# Activation stays runtime-gated: local auth only engages once key material is
# uploaded via PS4AUTH.SET (web config). No keys installed → passthrough / normal.
get_property(_joypad_all_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_t IN LISTS _joypad_all_targets)
    get_target_property(_t_type ${_t} TYPE)
    if(_t_type STREQUAL "EXECUTABLE")
        get_target_property(_t_srcs ${_t} SOURCES)
        if(_t_srcs MATCHES "ps4_mode\\.c")
            joypad_add_ps4_local_auth(${_t})
        endif()
    endif()
endforeach()

# ----------------------------------------------------------------------------
# USB-output fast clock (200 MHz) — GP2040-CE-style board clock policy.
#
# PS4 local-auth RSA signing must land inside the console's challenge window;
# at the stock 125 MHz a sign takes ~3.4 s, risking the timeout. GP2040-CE
# raises the whole board to the fastest build-supported clock (200 MHz), which
# brings signing to ~1.7 s. We do the same — but as a build-time / early-boot
# board policy (set once in usbd_init() before the USB stack starts, NOT a
# per-driver runtime hack), and only for the wired controller-emulation
# USB-output apps.
#
# Deliberately EXCLUDED (each keeps its own tuned clock):
#   - native-input apps (snes2usb/n642usb/gc2usb/lodgenet2usb/…): timing-
#     critical PIO input is tuned around their clock.
#   - CYW43 wireless variants (usb_pico_w/pico2_w/bt2usb): BT + SPI-PIO timing
#     needs separate validation before raising the clock.
#   - ESP32-S3 / nRF52840: different clock domain, handled by their own ports.
set(_joypad_usb_fast_clock_targets
    joypad_usb_feather_rp2040
    joypad_usb_feather_rp2040_max3421
    joypad_usb_feather_rp2040_usb_host
    joypad_usb_feather_rp2040_usb_host_max3421
    joypad_usb_ogxm_pico
    joypad_usb_pico
    joypad_usb_rp2040zero
    joypad_usb_rp2350usba
    joypad_usb2usb_remapper_v7_a
    joypad_usb2usb_remapper_v7_b
)
foreach(_t IN LISTS _joypad_usb_fast_clock_targets)
    if(TARGET ${_t})
        target_compile_definitions(${_t} PRIVATE JOYPAD_USB_FAST_CLOCK=1)
    endif()
endforeach()
