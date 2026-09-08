# ESP-IDF component CMakeLists for Joypad apps on ESP32-S3
#
# Compiles shared sources from ../../src/ alongside ESP32-specific code.
# BTstack from pico-sdk submodule, TinyUSB device-only (our own copy, not esp_tinyusb).
#
# App selection: pass -DCONFIG_APP=controller_btusb (default: bt2usb)

set(SHARED_SRC "${CMAKE_CURRENT_LIST_DIR}/../../src")
set(BTSTACK_PATH "${SHARED_SRC}/lib/pico-sdk/lib/btstack")
set(TINYUSB_PATH "${SHARED_SRC}/lib/tinyusb")
set(BTSTACK_ESP32_PORT "${BTSTACK_PATH}/port/esp32/components/btstack")

# Default app selection
if(NOT DEFINED CONFIG_APP)
    set(CONFIG_APP "bt2usb")
endif()


# ============================================================================
# Source files
# ============================================================================

# Common ESP32-specific sources
set(ESP32_COMMON_SRCS
    "main.c"
    "flash_esp32.c"
    "ps4_auth_stubs.c"
    "ws2812_esp32.c"
    "button_esp32.c"
    "rm67162_amoled.c"
    "${SHARED_SRC}/platform/esp32/platform_esp32.c"
)

# App-specific sources and config
if(CONFIG_APP STREQUAL "controller_btusb")
    set(APP_SRCS
        "btstack_hal_esp32.c"
        "stubs_peripheral.c"
        "pad_config_storage_esp32.c"
        "platform_gpio_esp32.c"
        "${SHARED_SRC}/apps/controller_btusb/app.c"
        # Pad input + config (runtime GPIO pin config via web config)
        "${SHARED_SRC}/pad/pad_input.c"
        "${SHARED_SRC}/pad/pad_config_flash.c"
        # Sensor drivers
        "${SHARED_SRC}/drivers/seesaw/seesaw.c"
        "${SHARED_SRC}/drivers/joywing/joywing_input.c"
        "${SHARED_SRC}/platform/esp32/platform_i2c_esp32.c"
        # BLE output (peripheral mode)
        "${SHARED_SRC}/bt/ble_output/ble_output.c"
        "${SHARED_SRC}/bt/ble_output/ble_output_keyboard.c"
        "${SHARED_SRC}/bt/ble_output/ble_output_mouse.c"
        "${SHARED_SRC}/bt/ble_output/ble_output_xbox.c"
        "${SHARED_SRC}/bt/ble_output/ble_nus.c"
        "${SHARED_SRC}/bt/ble_output/ble_xbox_gatt_db.c"
        # BT transport (peripheral mode — no btstack_host, no bthid)
        "${SHARED_SRC}/bt/transport/bt_transport.c"
        "${SHARED_SRC}/bt/transport/bt_transport_esp32.c"
        # Display (I2C, SH1107 FeatherWing OLED)
        "display_i2c_esp32.c"
        "${SHARED_SRC}/core/services/display/display.c"
        "${SHARED_SRC}/core/services/display/joy_anim.c"
        "${SHARED_SRC}/core/services/display/eyes_anim.c"
        "${SHARED_SRC}/core/services/display/menu.c"
        # Battery monitoring (I2C fuel gauge)
        "battery_esp32.c"
    )
    set(APP_INCLUDE_DIR "${SHARED_SRC}/apps/controller_btusb")
    set(APP_NEEDS_BTSTACK ON)
    set(APP_BTSTACK_MODE "peripheral")

elseif(CONFIG_APP STREQUAL "usb2usb")
    # usb2usb: USB Host (MAX3421E) → USB Device (ESP32 native OTG)
    # No Bluetooth — pure USB adapter with OLED display
    set(APP_SRCS
        "stubs_peripheral.c"
        "${SHARED_SRC}/apps/usb2usb/app.c"

        # --- MAX3421E host (ESP32-S3 port) ---
        "max3421_host_esp32.c"

        # --- Display (I2C, SH1107 FeatherWing) ---
        "display_i2c_esp32.c"
        "${SHARED_SRC}/core/services/display/display.c"
        "${SHARED_SRC}/core/services/display/joy_anim.c"
        "${SHARED_SRC}/core/services/display/eyes_anim.c"
        "${SHARED_SRC}/platform/esp32/platform_i2c_esp32.c"

        # --- USB Host layer ---
        "${SHARED_SRC}/usb/usbh/usbh.c"
        "${SHARED_SRC}/usb/usbh/intel/intel_wireless_series.c"
        "${SHARED_SRC}/usb/usbh/hid/hid.c"
        "${SHARED_SRC}/usb/usbh/hid/hid_registry.c"
        "${SHARED_SRC}/usb/usbh/hid/hid_utils.c"
        "${SHARED_SRC}/usb/usbh/xinput/xinput.c"
        "${SHARED_SRC}/usb/usbh/xbone_auth/xbone_auth.c"

        # --- USB HID device drivers ---
        "${SHARED_SRC}/usb/usbh/hid/devices/generic/hid_parser.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/generic/hid_gamepad.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/generic/hid_keyboard.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/generic/hid_mouse.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/generic/sinput_host.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/8bitdo/8bitdo_bta.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/8bitdo/8bitdo_m30.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/8bitdo/8bitdo_neo.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/8bitdo/8bitdo_pce.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/8bitdo/8bitdo_sn30.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/dragonrise/dragonrise.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/google/google_stadia.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/hori/hori_horipad.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/hori/hori_pokken.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/logitech/logitech_wingman.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/microsoft/ms_sidewinder_commander.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/microsoft/ms_sidewinder_dualstrike.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/misc/triple_adapter_v1.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/misc/triple_adapter_v2.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/nintendo/gamecube_adapter.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/nintendo/switch2_pro.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/nintendo/switch_pro.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/raphnet/raphnet_pce.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/sega/sega_astrocity.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/sony/sony_ds3.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/sony/sony_ds4.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/sony/ds5_auth.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/sony/p5general_host.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/sony/sony_ds5.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/sony/sony_psc.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/valve/steam_controller_2.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/valve/steam_controller.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/elo/elo_vagabond.c"

        # --- XInput host driver (tusb_xinput library) ---
        "${SHARED_SRC}/lib/tusb_xinput/xinput_host.c"

        # --- TinyUSB host stack ---
        "${TINYUSB_PATH}/src/host/usbh.c"
        "${TINYUSB_PATH}/src/host/hub.c"
        "${TINYUSB_PATH}/src/class/hid/hid_host.c"
        "${TINYUSB_PATH}/src/class/vendor/vendor_host.c"
        "${TINYUSB_PATH}/src/portable/analog/max3421/hcd_max3421.c"
    )
    set(APP_INCLUDE_DIR "${SHARED_SRC}/apps/usb2usb")
    set(APP_NEEDS_BTSTACK OFF)
    set(APP_NEEDS_USB_HOST ON)

elseif(CONFIG_APP STREQUAL "btusb2usb")
    # btusb2usb: USB Host (MAX3421E) + BLE → USB Device (ESP32 native OTG)
    # Combines usb2usb (USB host + display) with BTstack central mode (BLE input)
    set(APP_SRCS
        "btstack_hal_esp32.c"
        "stubs_peripheral.c"
        "${SHARED_SRC}/apps/usb2usb/app.c"

        # --- MAX3421E host (ESP32-S3 port) ---
        "max3421_host_esp32.c"

        # --- Display (I2C, SH1107 FeatherWing) ---
        "display_i2c_esp32.c"
        "${SHARED_SRC}/core/services/display/display.c"
        "${SHARED_SRC}/core/services/display/joy_anim.c"
        "${SHARED_SRC}/core/services/display/eyes_anim.c"
        "${SHARED_SRC}/platform/esp32/platform_i2c_esp32.c"

        # --- USB Host layer ---
        "${SHARED_SRC}/usb/usbh/usbh.c"
        "${SHARED_SRC}/usb/usbh/intel/intel_wireless_series.c"
        "${SHARED_SRC}/usb/usbh/hid/hid.c"
        "${SHARED_SRC}/usb/usbh/hid/hid_registry.c"
        "${SHARED_SRC}/usb/usbh/hid/hid_utils.c"
        "${SHARED_SRC}/usb/usbh/xinput/xinput.c"
        "${SHARED_SRC}/usb/usbh/xbone_auth/xbone_auth.c"

        # --- USB HID device drivers ---
        "${SHARED_SRC}/usb/usbh/hid/devices/generic/hid_parser.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/generic/hid_gamepad.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/generic/hid_keyboard.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/generic/hid_mouse.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/generic/sinput_host.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/8bitdo/8bitdo_bta.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/8bitdo/8bitdo_m30.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/8bitdo/8bitdo_neo.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/8bitdo/8bitdo_pce.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/8bitdo/8bitdo_sn30.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/dragonrise/dragonrise.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/google/google_stadia.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/hori/hori_horipad.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/hori/hori_pokken.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/logitech/logitech_wingman.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/microsoft/ms_sidewinder_commander.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/microsoft/ms_sidewinder_dualstrike.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/misc/triple_adapter_v1.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/misc/triple_adapter_v2.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/nintendo/gamecube_adapter.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/nintendo/switch2_pro.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/nintendo/switch_pro.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/raphnet/raphnet_pce.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/sega/sega_astrocity.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/sony/sony_ds3.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/sony/sony_ds4.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/sony/ds5_auth.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/sony/p5general_host.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/sony/sony_ds5.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/sony/sony_psc.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/valve/steam_controller_2.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/valve/steam_controller.c"
        "${SHARED_SRC}/usb/usbh/hid/devices/vendors/elo/elo_vagabond.c"

        # --- XInput host driver (tusb_xinput library) ---
        "${SHARED_SRC}/lib/tusb_xinput/xinput_host.c"

        # --- TinyUSB host stack ---
        "${TINYUSB_PATH}/src/host/usbh.c"
        "${TINYUSB_PATH}/src/host/hub.c"
        "${TINYUSB_PATH}/src/class/hid/hid_host.c"
        "${TINYUSB_PATH}/src/class/vendor/vendor_host.c"
        "${TINYUSB_PATH}/src/portable/analog/max3421/hcd_max3421.c"
    )
    set(APP_INCLUDE_DIR "${SHARED_SRC}/apps/usb2usb")
    set(APP_NEEDS_BTSTACK ON)
    set(APP_BTSTACK_MODE "central")
    set(APP_NEEDS_USB_HOST ON)

else()
    # Default: bt2usb
    set(APP_SRCS
        "btstack_hal_esp32.c"
        "${SHARED_SRC}/apps/bt2usb/app.c"
    )
    set(APP_INCLUDE_DIR "${SHARED_SRC}/apps/bt2usb")
    set(APP_NEEDS_BTSTACK ON)
    set(APP_BTSTACK_MODE "central")
endif()

set(SRCS
    # --- ESP32-specific ---
    ${ESP32_COMMON_SRCS}
    ${APP_SRCS}

    # --- Core services ---
    "${SHARED_SRC}/core/app_registry.c"
    "${SHARED_SRC}/core/router/router.c"
    "${SHARED_SRC}/core/services/leds/leds.c"
    "${SHARED_SRC}/core/services/leds/player_leds_gpio.c"
    "${SHARED_SRC}/core/services/storage/storage.c"
    "${SHARED_SRC}/core/services/codes/codes.c"
    "${SHARED_SRC}/core/services/keymap/keymap.c"
    "${SHARED_SRC}/core/services/hotkeys/hotkeys.c"
    "${SHARED_SRC}/core/services/players/manager.c"
    "${SHARED_SRC}/core/services/players/feedback.c"
    "${SHARED_SRC}/core/services/profiles/profile.c"
    "${SHARED_SRC}/core/services/profiles/profile_indicator.c"
    "${SHARED_SRC}/core/services/profiles/runtime_profile.c"

    # --- USB Device ---
    "${SHARED_SRC}/usb/usbd/usbd.c"
    "${SHARED_SRC}/usb/usbd/modes/hid_mode.c"
    "${SHARED_SRC}/usb/usbd/modes/sinput_mode.c"
    "${SHARED_SRC}/usb/usbd/modes/xinput_mode.c"
    "${SHARED_SRC}/usb/usbd/modes/switch_mode.c"
    "${SHARED_SRC}/usb/usbd/modes/ps3_mode.c"
    "${SHARED_SRC}/usb/usbd/modes/psclassic_mode.c"
    "${SHARED_SRC}/usb/usbd/modes/pcemini_mode.c"
    "${SHARED_SRC}/usb/usbd/modes/ps4_mode.c"
    "${SHARED_SRC}/usb/usbd/modes/dualsense_mode.c"
    "${SHARED_SRC}/usb/usbd/modes/p5general_mode.c"
    "${SHARED_SRC}/usb/usbd/modes/xid_mode.c"
    "${SHARED_SRC}/usb/usbd/modes/xbone_mode.c"
    "${SHARED_SRC}/usb/usbd/modes/xac_mode.c"
    "${SHARED_SRC}/usb/usbd/modes/kbmouse_mode.c"
    "${SHARED_SRC}/usb/usbd/modes/gc_adapter_mode.c"
    "${SHARED_SRC}/usb/usbd/cdc/cdc.c"
    "${SHARED_SRC}/usb/usbd/cdc/cdc_protocol.c"
    "${SHARED_SRC}/usb/usbd/cdc/cdc_commands.c"
    "${SHARED_SRC}/usb/usbd/drivers/tud_xid.c"
    "${SHARED_SRC}/usb/usbd/drivers/tud_xinput.c"
    "${SHARED_SRC}/usb/usbd/drivers/tud_xbone.c"
    "${SHARED_SRC}/usb/usbd/drivers/xgip_protocol.c"
    "${SHARED_SRC}/usb/usbd/kbmouse/kbmouse.c"
    "${SHARED_SRC}/lib/libxsm3/xsm3.c"
    "${SHARED_SRC}/lib/libxsm3/usbdsec.c"
    "${SHARED_SRC}/lib/libxsm3/excrypt_des.c"
    "${SHARED_SRC}/lib/libxsm3/excrypt_parve.c"
    "${SHARED_SRC}/lib/libxsm3/excrypt_sha.c"

    # --- TinyUSB device stack ---
    "${TINYUSB_PATH}/src/tusb.c"
    "${TINYUSB_PATH}/src/common/tusb_fifo.c"
    "${TINYUSB_PATH}/src/device/usbd.c"
    "${TINYUSB_PATH}/src/device/usbd_control.c"
    "${TINYUSB_PATH}/src/class/cdc/cdc_device.c"
    "${TINYUSB_PATH}/src/class/hid/hid_device.c"
    "${TINYUSB_PATH}/src/portable/synopsys/dwc2/dcd_dwc2.c"
    "${TINYUSB_PATH}/src/portable/synopsys/dwc2/dwc2_common.c"
)

# Procedural face on the LilyGo AMOLED (shared engine + ESP backend/task)
if(CONFIG_BOARD STREQUAL "lilygo_tdisplay_s3_amoled")
    list(APPEND SRCS
        "${SHARED_SRC}/core/services/display/face_anim.c"
        "eyes_esp32.c"
        "pmu_esp32.c"
    )
endif()

# Add BTstack sources when needed
if(APP_NEEDS_BTSTACK)
    # --- BTstack core (shared by all BTstack modes) ---
    list(APPEND SRCS
        "${BTSTACK_PATH}/src/btstack_linked_list.c"
        "${BTSTACK_PATH}/src/btstack_memory.c"
        "${BTSTACK_PATH}/src/btstack_memory_pool.c"
        "${BTSTACK_PATH}/src/btstack_run_loop.c"
        "${BTSTACK_PATH}/src/btstack_run_loop_base.c"
        "${BTSTACK_PATH}/src/btstack_util.c"
        "${BTSTACK_PATH}/src/btstack_tlv.c"
        "${BTSTACK_PATH}/src/btstack_tlv_none.c"
        "${BTSTACK_PATH}/src/btstack_crypto.c"
        "${BTSTACK_PATH}/src/btstack_ring_buffer.c"
        "${BTSTACK_PATH}/src/hci.c"
        "${BTSTACK_PATH}/src/hci_cmd.c"
        "${BTSTACK_PATH}/src/hci_dump.c"
        "${BTSTACK_PATH}/src/hci_event.c"
        "${BTSTACK_PATH}/src/hci_event_builder.c"
        "${BTSTACK_PATH}/src/l2cap.c"
        "${BTSTACK_PATH}/src/l2cap_signaling.c"
        "${BTSTACK_PATH}/src/ad_parser.c"

        # --- BTstack BLE common ---
        "${BTSTACK_PATH}/src/ble/sm.c"
        "${BTSTACK_PATH}/src/ble/att_dispatch.c"
        "${BTSTACK_PATH}/src/ble/att_db.c"
        "${BTSTACK_PATH}/src/ble/att_db_util.c"
        "${BTSTACK_PATH}/src/ble/att_server.c"
        # le_device_db_tlv ONLY — compiling le_device_db_memory.c alongside it
        # silently wins the static-archive link and bonds live in RAM, so the
        # device forgets every pairing at reboot (macOS keeps its side and the
        # reconnect fails encryption). NVS-backed TLV persists bonds + SM IR/ER.
        "${BTSTACK_PATH}/src/ble/le_device_db_tlv.c"

        # --- BTstack 3rd-party ---
        "${BTSTACK_PATH}/3rd-party/micro-ecc/uECC.c"
        "${BTSTACK_PATH}/3rd-party/rijndael/rijndael.c"

        # --- BTstack platform: FreeRTOS run loop + ESP32 port ---
        "${BTSTACK_PATH}/platform/freertos/btstack_run_loop_freertos.c"
        "${BTSTACK_PATH}/platform/embedded/hci_dump_embedded_stdout.c"
        "${BTSTACK_ESP32_PORT}/btstack_port_esp32.c"
        "${BTSTACK_ESP32_PORT}/btstack_tlv_esp32.c"
    )

    # --- Central mode (bt2usb): GATT client + HID host + device drivers ---
    if(APP_BTSTACK_MODE STREQUAL "central")
        list(APPEND SRCS
            # HID parser (shared with USB path)
            "${SHARED_SRC}/usb/usbh/hid/devices/generic/hid_parser.c"

            # BT HID device drivers
            "${SHARED_SRC}/bt/bthid/bthid.c"
            "${SHARED_SRC}/bt/bthid/bthid_registry.c"
            "${SHARED_SRC}/bt/bthid/devices/generic/bthid_gamepad.c"
            "${SHARED_SRC}/bt/bthid/devices/vendors/sony/ds3_bt.c"
            "${SHARED_SRC}/bt/bthid/devices/vendors/sony/ds4_bt.c"
            "${SHARED_SRC}/bt/bthid/devices/vendors/sony/ds5_bt.c"
            "${SHARED_SRC}/bt/bthid/devices/vendors/nintendo/switch_pro_bt.c"
            "${SHARED_SRC}/bt/bthid/devices/vendors/nintendo/switch2_ble.c"
            "${SHARED_SRC}/bt/bthid/devices/vendors/nintendo/wii_u_pro_bt.c"
            "${SHARED_SRC}/bt/bthid/devices/vendors/nintendo/wiimote_bt.c"
            "${SHARED_SRC}/bt/bthid/devices/vendors/microsoft/xbox_bt.c"
            "${SHARED_SRC}/bt/bthid/devices/vendors/microsoft/xbox_ble.c"
            "${SHARED_SRC}/bt/bthid/devices/vendors/google/stadia_bt.c"
            "${SHARED_SRC}/bt/bthid/devices/generic/sinput_ble.c"
            "${SHARED_SRC}/bt/bthid/devices/vendors/augmental/mouthpad_ble.c"
            "${SHARED_SRC}/bt/bthid/devices/vendors/valve/steam_controller_2_ble.c"
            "${SHARED_SRC}/bt/bthid/devices/vendors/valve/steam_controller_ble.c"
            "${SHARED_SRC}/bt/bthid/devices/vendors/ipega/ipega_bt.c"

            # BTstack host + transport
            "${SHARED_SRC}/bt/btstack/btstack_host.c"
            "${SHARED_SRC}/bt/btstack/bt_device_db.c"
            "${SHARED_SRC}/bt/transport/bt_transport.c"
            "${SHARED_SRC}/bt/transport/bt_transport_esp32.c"

            # BTstack GATT client
            "${BTSTACK_PATH}/src/ble/gatt_client.c"
            "${BTSTACK_PATH}/src/ble/gatt_service_client.c"
            "${BTSTACK_PATH}/src/ble/gatt-service/hids_client.c"
            "${BTSTACK_PATH}/src/ble/gatt-service/device_information_service_client.c"
            "${BTSTACK_PATH}/src/ble/gatt-service/battery_service_client.c"

            # BTstack Classic (compiles but dead code on ESP32-S3 BLE-only)
            "${BTSTACK_PATH}/src/classic/sdp_client.c"
            "${BTSTACK_PATH}/src/classic/sdp_server.c"
            "${BTSTACK_PATH}/src/classic/sdp_util.c"
            "${BTSTACK_PATH}/src/classic/device_id_server.c"
            "${BTSTACK_PATH}/src/classic/hid_host.c"
            "${BTSTACK_PATH}/src/classic/btstack_link_key_db_memory.c"
            "${BTSTACK_PATH}/src/classic/btstack_link_key_db_tlv.c"
        )
    endif()

    # --- Peripheral mode (controller_btusb): GATT server + HID device ---
    if(APP_BTSTACK_MODE STREQUAL "peripheral")
        list(APPEND SRCS
            # BTstack GATT server (att_server.c is in the BLE common block)
            "${BTSTACK_PATH}/src/ble/gatt-service/hids_device.c"
            "${BTSTACK_PATH}/src/ble/gatt-service/battery_service_server.c"
            "${BTSTACK_PATH}/src/ble/gatt-service/device_information_service_server.c"
            "${BTSTACK_PATH}/src/ble/gatt-service/nordic_spp_service_server.c"
            # BTstack HID parser (needed by hids_device.c)
            "${BTSTACK_PATH}/src/btstack_hid_parser.c"
            # BTstack Classic link key DB (needed by btstack_port_esp32.c)
            "${BTSTACK_PATH}/src/classic/btstack_link_key_db_tlv.c"
            # Stub btstack_host_nus_send: cdc_commands.c's FACE.* NUS relay
            # references it, but the real impl (btstack_host.c) is central-only.
            "btstack_host_nus_stub.c"
        )
    endif()
endif()

set(INCLUDE_DIRS
    "."
    "${SHARED_SRC}"
    "${APP_INCLUDE_DIR}"
    "${SHARED_SRC}/core/services/display"
    "${SHARED_SRC}/usb/usbd"
    "${SHARED_SRC}/pad"
    "${SHARED_SRC}/lib/libxsm3"
    # TinyUSB
    "${TINYUSB_PATH}/src"
)

# Add USB host include dirs when needed (usb2usb with MAX3421E)
if(APP_NEEDS_USB_HOST)
    list(APPEND INCLUDE_DIRS
        "${SHARED_SRC}/usb/usbh"
        "${SHARED_SRC}/lib/tusb_xinput"
    )
endif()

# Add BTstack include dirs when needed
if(APP_NEEDS_BTSTACK)
    list(APPEND INCLUDE_DIRS
        "${SHARED_SRC}/bt"
        "${SHARED_SRC}/bt/btstack"
        "${SHARED_SRC}/bt/bthid"
        "${BTSTACK_PATH}/src"
        "${BTSTACK_PATH}/src/ble"
        "${BTSTACK_PATH}/platform/embedded"
        "${BTSTACK_PATH}/platform/freertos"
        "${BTSTACK_PATH}/3rd-party/micro-ecc"
        "${BTSTACK_PATH}/3rd-party/rijndael"
        "${BTSTACK_ESP32_PORT}/include"
    )
    if(APP_BTSTACK_MODE STREQUAL "peripheral")
        list(APPEND INCLUDE_DIRS
            "${SHARED_SRC}/bt/ble_output"
            "${CMAKE_CURRENT_BINARY_DIR}"
        )
    endif()
endif()

# ============================================================================
# REQUIRES — ESP-IDF component dependencies
# ============================================================================

# Component requirements
set(ESP_REQUIRES
    nvs_flash
    espcoredump
    esp_driver_gpio
    esp_adc
    esp_driver_i2c
    esp_driver_rmt
    esp_driver_spi
    esp_lcd
    esp_driver_uart
    esp_timer
    esp_hw_support
    esp_system
)

if(APP_NEEDS_BTSTACK)
    list(APPEND ESP_REQUIRES bt)
endif()

# ============================================================================
# Register as ESP-IDF component
# ============================================================================

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS ${INCLUDE_DIRS}
    REQUIRES ${ESP_REQUIRES}
)

# ============================================================================
# GATT database generation (peripheral mode)
# ============================================================================

if(APP_BTSTACK_MODE STREQUAL "peripheral")
    set(COMPILE_GATT "${BTSTACK_PATH}/tool/compile_gatt.py")

    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ble_gamepad.h"
        COMMAND python3 ${COMPILE_GATT}
            "${SHARED_SRC}/bt/ble_output/ble_gamepad.gatt"
            "${CMAKE_CURRENT_BINARY_DIR}/ble_gamepad.h"
        DEPENDS "${SHARED_SRC}/bt/ble_output/ble_gamepad.gatt"
        COMMENT "Generating ble_gamepad.h from GATT definition"
    )

    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ble_xbox.h"
        COMMAND python3 ${COMPILE_GATT}
            "${SHARED_SRC}/bt/ble_output/ble_xbox.gatt"
            "${CMAKE_CURRENT_BINARY_DIR}/ble_xbox.h"
        DEPENDS "${SHARED_SRC}/bt/ble_output/ble_xbox.gatt"
        COMMENT "Generating ble_xbox.h from GATT definition"
    )

    add_custom_target(gatt_headers DEPENDS
        "${CMAKE_CURRENT_BINARY_DIR}/ble_gamepad.h"
        "${CMAKE_CURRENT_BINARY_DIR}/ble_xbox.h"
    )
    add_dependencies(${COMPONENT_LIB} gatt_headers)
endif()

# ============================================================================
# Compile definitions
# ============================================================================

# Common definitions for all apps
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    CONFIG_USB=1
    PLATFORM_ESP32=1
    BUTTON_USER_GPIO=0
    CFG_TUSB_CONFIG_FILE="tusb_config_esp32.h"
)

# Disable USB host by default (overridden by usb2usb)
if(NOT APP_NEEDS_USB_HOST)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC DISABLE_USB_HOST=1)
endif()

# BTHID host registry — derived from the source list rather than set by hand, so
# it can never drift out of sync with whether bthid_registry.c is actually
# compiled. bt_transport.c gates its bthid_registry_init() call on this; it used
# to rely on a weak stub, which --gc-sections quietly resolved to an empty
# function, leaving ESP32-S3 bt2usb builds with zero BT HID drivers registered.
if("${SRCS}" MATCHES "bthid_registry\\.c")
    target_compile_definitions(${COMPONENT_LIB} PUBLIC CONFIG_BT_HOST=1)
endif()

# Board-specific definitions
if(NOT DEFINED CONFIG_BOARD)
    set(CONFIG_BOARD "devkit")
endif()
if(CONFIG_BOARD STREQUAL "feather_esp32s3")
    target_compile_definitions(${COMPONENT_LIB} PUBLIC BOARD_FEATHER_ESP32S3=1)
endif()
if(CONFIG_BOARD STREQUAL "lilygo_tdisplay_s3_amoled")
    target_compile_definitions(${COMPONENT_LIB} PUBLIC
        BOARD_LILYGO_TDISPLAY_S3_AMOLED=1
        EYES_SCALE=6
        CONFIG_BLE_USB_COEXIST=1)
endif()

# App-specific definitions
if(CONFIG_APP STREQUAL "controller_btusb")
    target_compile_definitions(${COMPONENT_LIB} PUBLIC
        CONFIG_CONTROLLER_BTUSB=1
        CONFIG_PAD_INPUT=1
        SENSOR_PAD=1
        BTSTACK_USE_ESP32=1
        ENABLE_BTSTACK=1
        SENSOR_JOYWING=1
        USBD_DEFAULT_MODE=1
        OLED_I2C_DISPLAY=1
        DISABLE_DISPLAY_SPI=1
    )
elseif(CONFIG_APP STREQUAL "usb2usb")
    target_compile_definitions(${COMPONENT_LIB} PUBLIC
        CONFIG_USB2USB=1
        CONFIG_MAX3421=1
        OLED_I2C_DISPLAY=1
        DISABLE_DISPLAY_SPI=1
    )
elseif(CONFIG_APP STREQUAL "btusb2usb")
    target_compile_definitions(${COMPONENT_LIB} PUBLIC
        CONFIG_BTUSB2USB=1
        CONFIG_MAX3421=1
        BTSTACK_USE_ESP32=1
        ENABLE_BTSTACK=1
        OLED_I2C_DISPLAY=1
        DISABLE_DISPLAY_SPI=1
    )
elseif(APP_NEEDS_BTSTACK)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC
        CONFIG_BT2USB=1
        REQUIRE_BT_INPUT=1
        BTSTACK_USE_ESP32=1
        ENABLE_BTSTACK=1
    )
endif()

# ============================================================================
# Version and build info
# ============================================================================

# Read version
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../VERSION")
    file(READ "${CMAKE_CURRENT_LIST_DIR}/../../VERSION" JOYPAD_VERSION)
    string(STRIP "${JOYPAD_VERSION}" JOYPAD_VERSION)
else()
    set(JOYPAD_VERSION "0.0.0")
endif()

# Derive USB bcdDevice (0xMMmp) from JOYPAD_VERSION so the SInput "dongle version"
# reflects firmware version (matches src/CMakeLists.txt).
string(REPLACE "." ";" _JP_VER_PARTS "${JOYPAD_VERSION}")
list(LENGTH _JP_VER_PARTS _JP_VER_LEN)
if(_JP_VER_LEN GREATER_EQUAL 3)
    list(GET _JP_VER_PARTS 0 _JP_VER_MAJOR)
    list(GET _JP_VER_PARTS 1 _JP_VER_MINOR)
    list(GET _JP_VER_PARTS 2 _JP_VER_PATCH)
    math(EXPR JOYPAD_BCD_DEVICE "(${_JP_VER_MAJOR} << 8) | ((${_JP_VER_MINOR} & 15) << 4) | (${_JP_VER_PATCH} & 15)")
else()
    set(JOYPAD_BCD_DEVICE 256)
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

string(TIMESTAMP BUILD_TIME "%Y-%m-%d %H:%M:%S")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    JOYPAD_VERSION="${JOYPAD_VERSION}"
    SINPUT_BCD_DEVICE=${JOYPAD_BCD_DEVICE}
    GIT_COMMIT="${GIT_COMMIT}"
    BUILD_TIME="${BUILD_TIME}"
    BOARD_NAME="esp32s3"
)

# Optimization and disable -Werror for cross-compiled shared code
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-error)

# Force-include btstack_config.h so all BTstack headers see ENABLE_BLE etc.
# BTstack .c files include it, but .h files check #ifdef without including it.
if(APP_NEEDS_BTSTACK)
    target_compile_options(${COMPONENT_LIB} PRIVATE -include btstack_config.h)
endif()
