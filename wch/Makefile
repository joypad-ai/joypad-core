# Joypad OS — usb2usb on WCH CH32V307 (native USBFS host + USBHS device)
#
# Mirrors esp/ and nrf/: a top-level build dir that pulls shared sources from
# ../src and wraps the platform toolchain. Here the "SDK" is the in-repo TinyUSB
# submodule (src/lib/tinyusb) plus the WCH peripheral lib (fetched into TinyUSB's
# hw/mcu/wch via get_deps). The build itself is driven by TinyUSB's own GNU make
# system (examples/build_system), the same machinery the CH32 BSP examples use.
#
#   make            # build usb2usb (BOARD=ch32v307v_r1_1v0)
#   make flash      # flash via WCH-LinkE (wlink)
#   make reset      # reset the target
#   make monitor    # serial monitor hint (use COMrade / `! screen ...`)
#   make clean
#
# Device output = USBHS (high speed, PB6/PB7). Host input = USBFS (PA11/PA12).
# Host HCD = the async TinyUSB driver (hcd_ch32_usbfs.c); we do NOT define
# CFG_TUH_WCH_BACKEND (the WCH-backed hybrid is retired — see
# .dev/docs/ch32v307/host-stack-decision.md).

# ---------------------------------------------------------------------------
# Toolchain — WCH gcc 8.2.0. Newer GCC miscompiles the WCH USB timing code, so
# this is pinned. `make init-wch` (top-level) installs it repo-locally under
# wch/toolchain/ (gitignored). Override GCC8_BIN to use a copy elsewhere.
# ---------------------------------------------------------------------------
WCH_DIR       := $(dir $(abspath $(firstword $(MAKEFILE_LIST))))
GCC8_VER      := 8.2.0-3.1
GCC8_BIN      ?= $(WCH_DIR)toolchain/xPacks/riscv-none-embed-gcc/$(GCC8_VER)/bin
export PATH   := $(GCC8_BIN):$(PATH)
# Absolute tool prefix so the build finds gcc8 regardless of make's exec PATH
# handling. Override CROSS_COMPILE entirely to use a system toolchain.
CROSS_COMPILE ?= $(GCC8_BIN)/riscv-none-embed-

# ---------------------------------------------------------------------------
# Board + speed (must be set before the TinyUSB make includes resolve family)
# ---------------------------------------------------------------------------
BOARD ?= ch32v307v_r1_1v0
# Force high: USBHS device must be high speed (USBFS host is always FS). Use
# override so a stray SPEED=full in the environment can't switch the device IP.
override SPEED := high

# Pull in the TinyUSB make build system (sets TOP, EXAMPLE_PATH, family, etc.).
# tinyusb master renamed examples/build_system/make/make.mk -> hw/bsp/family_support.mk
# (the cpu/ and toolchain/ subdirs it includes still live under examples/build_system/make).
#
# NOTE: src/lib/tinyusb is pinned to the joypad-ai fork branch
# `joypad-0.20.0-ch32` = upstream 0.20.0 release + the CH32 USBFS host commits.
# 0.20.0 (not master) because the Jan–Jun 2026 master host rework regresses
# RP2040 hosts that share the chip with Core-1 real-time console protocols
# (see .dev/dc-dongle-bisect/README.md). All platforms build from this one
# submodule. TINYUSB_ROOT stays overridable for testing other checkouts.
TINYUSB_ROOT ?= ../src/lib/tinyusb
include $(TINYUSB_ROOT)/hw/bsp/family_support.mk

# Joypad shared source tree lives at <repo>/src. Independent of TINYUSB_ROOT
# so an out-of-tree tinyusb checkout doesn't relocate the joypad sources.
JOYPAD ?= $(abspath ../src)

# ---------------------------------------------------------------------------
# Include paths
# ---------------------------------------------------------------------------
INC += \
	src \
	$(JOYPAD) \
	$(JOYPAD)/apps/usb2usb \
	$(JOYPAD)/usb/usbd \
	$(JOYPAD)/usb/usbh \
	$(JOYPAD)/lib/tusb_xinput \
	$(JOYPAD)/lib/libxsm3 \
	$(TOP)/hw \

# Build info (normally injected by CMake on the RP2040/ESP builds)
JOYPAD_VERSION := $(shell cat $(JOYPAD)/../VERSION 2>/dev/null || echo 0.0.0)
GIT_COMMIT     := $(shell git -C $(JOYPAD) rev-parse --short HEAD 2>/dev/null || echo unknown)
BUILD_TIME     := $(shell date "+%Y-%m-%d %H:%M:%S")

CFLAGS += \
	-DPLATFORM_CH32 \
	-DCONFIG_USB2USB=1 \
	-DDISABLE_DISPLAY_SPI=1 \
	-DJOYPAD_VERSION='"$(JOYPAD_VERSION)"' \
	-DGIT_COMMIT='"$(GIT_COMMIT)"' \
	-DBUILD_TIME='"$(BUILD_TIME)"' \
	-DBOARD_NAME='"ch32v307"' \
	-DMAX_PLAYERS_PER_OUTPUT=4 \
	-DMAX_BLEND_DEVICES=4 \
	-DCFG_TUH_TASK_QUEUE_SZ=64 \
	-DUSBD_DEFAULT_MODE=USB_OUTPUT_MODE_SINPUT \

# Shared code is portable C compiled for many targets and trips assorted
# warnings under TinyUSB's -Werror. The ESP/nRF builds relax it the same way.
# (tinyusb master dropped the CFLAGS_GCC convention; gcc_common.mk's -Werror is
# already in CFLAGS by here, so a later -Wno-error in CFLAGS overrides it.)
CFLAGS += -Wno-error

# Bump the stack from the 2KB default — host enumeration + HID parsing + the
# Joypad registry is a deep call chain that overflows 2KB (resets the chip).
LDFLAGS += -Wl,--defsym=__STACK_SIZE=4096

# ---------------------------------------------------------------------------
# WCH platform sources (this directory) — main, tusb_config consumer, stubs
# ---------------------------------------------------------------------------
EXAMPLE_SOURCE += $(wildcard src/*.c)
SRC_C += $(addprefix $(EXAMPLE_PATH)/, $(EXAMPLE_SOURCE))

# Joypad platform HAL
SRC_C += $(JOYPAD)/platform/ch32/platform_ch32.c

# ---------------------------------------------------------------------------
# Joypad shared sources (mirrors esp/main/CMakeLists.txt usb2usb manifest,
# minus the ESP/MAX3421/display/BTstack pieces — CH32 uses native USB host).
# TinyUSB core + class + host/device sources come from src/tinyusb.mk already,
# so they are intentionally NOT listed here.
# ---------------------------------------------------------------------------

# --- Core services ---
SRC_C += \
	$(JOYPAD)/core/app_registry.c \
	$(JOYPAD)/core/router/router.c \
	$(JOYPAD)/core/services/leds/leds.c \
	$(JOYPAD)/core/services/storage/storage.c \
	$(JOYPAD)/core/services/codes/codes.c \
	$(JOYPAD)/core/services/hotkeys/hotkeys.c \
	$(JOYPAD)/core/services/players/manager.c \
	$(JOYPAD)/core/services/players/feedback.c \
	$(JOYPAD)/core/services/profiles/profile.c \
	$(JOYPAD)/core/services/profiles/profile_indicator.c \

# --- App ---
SRC_C += $(JOYPAD)/apps/usb2usb/app.c

# --- USB Host layer ---
SRC_C += \
	$(JOYPAD)/usb/usbh/usbh.c \
	$(JOYPAD)/usb/usbh/intel/intel_wireless_series.c \
	$(JOYPAD)/usb/usbh/hid/hid.c \
	$(JOYPAD)/usb/usbh/hid/hid_registry.c \
	$(JOYPAD)/usb/usbh/hid/hid_utils.c \
	$(JOYPAD)/usb/usbh/xinput/xinput.c \
	$(JOYPAD)/usb/usbh/xbone_auth/xbone_auth.c \

# --- USB host HID device drivers ---
SRC_C += \
	$(JOYPAD)/usb/usbh/hid/devices/generic/hid_parser.c \
	$(JOYPAD)/usb/usbh/hid/devices/generic/hid_gamepad.c \
	$(JOYPAD)/usb/usbh/hid/devices/generic/hid_keyboard.c \
	$(JOYPAD)/usb/usbh/hid/devices/generic/hid_mouse.c \
	$(JOYPAD)/usb/usbh/hid/devices/generic/sinput_host.c \
	$(JOYPAD)/usb/usbh/hid/devices/vendors/8bitdo/8bitdo_bta.c \
	$(JOYPAD)/usb/usbh/hid/devices/vendors/8bitdo/8bitdo_m30.c \
	$(JOYPAD)/usb/usbh/hid/devices/vendors/8bitdo/8bitdo_neo.c \
	$(JOYPAD)/usb/usbh/hid/devices/vendors/8bitdo/8bitdo_pce.c \
	$(JOYPAD)/usb/usbh/hid/devices/vendors/8bitdo/8bitdo_sn30.c \
	$(JOYPAD)/usb/usbh/hid/devices/vendors/dragonrise/dragonrise.c \
	$(JOYPAD)/usb/usbh/hid/devices/vendors/google/google_stadia.c \
	$(JOYPAD)/usb/usbh/hid/devices/vendors/hori/hori_horipad.c \
	$(JOYPAD)/usb/usbh/hid/devices/vendors/hori/hori_pokken.c \
	$(JOYPAD)/usb/usbh/hid/devices/vendors/logitech/logitech_wingman.c \
	$(JOYPAD)/usb/usbh/hid/devices/vendors/microsoft/ms_sidewinder_commander.c \
	$(JOYPAD)/usb/usbh/hid/devices/vendors/microsoft/ms_sidewinder_dualstrike.c \
	$(JOYPAD)/usb/usbh/hid/devices/vendors/misc/triple_adapter_v1.c \
	$(JOYPAD)/usb/usbh/hid/devices/vendors/misc/triple_adapter_v2.c \
	$(JOYPAD)/usb/usbh/hid/devices/vendors/nintendo/gamecube_adapter.c \
	$(JOYPAD)/usb/usbh/hid/devices/vendors/nintendo/switch2_pro.c \
	$(JOYPAD)/usb/usbh/hid/devices/vendors/nintendo/switch_pro.c \
	$(JOYPAD)/usb/usbh/hid/devices/vendors/raphnet/raphnet_pce.c \
	$(JOYPAD)/usb/usbh/hid/devices/vendors/sega/sega_astrocity.c \
	$(JOYPAD)/usb/usbh/hid/devices/vendors/sony/sony_ds3.c \
	$(JOYPAD)/usb/usbh/hid/devices/vendors/sony/sony_ds4.c \
	$(JOYPAD)/usb/usbh/hid/devices/vendors/sony/ds5_auth.c \
	$(JOYPAD)/usb/usbh/hid/devices/vendors/sony/p5general_host.c \
	$(JOYPAD)/usb/usbh/hid/devices/vendors/sony/sony_ds5.c \
	$(JOYPAD)/usb/usbh/hid/devices/vendors/sony/sony_psc.c \
	$(JOYPAD)/usb/usbh/hid/devices/vendors/valve/steam_controller_2.c \
	$(JOYPAD)/usb/usbh/hid/devices/vendors/valve/steam_controller.c \
	$(JOYPAD)/usb/usbh/hid/devices/vendors/elo/elo_vagabond.c \

# --- XInput host driver (tusb_xinput library) ---
SRC_C += $(JOYPAD)/lib/tusb_xinput/xinput_host.c

# --- USB Device modes + drivers ---
SRC_C += \
	$(JOYPAD)/usb/usbd/usbd.c \
	$(JOYPAD)/usb/usbd/modes/hid_mode.c \
	$(JOYPAD)/usb/usbd/modes/sinput_mode.c \
	$(JOYPAD)/usb/usbd/modes/xinput_mode.c \
	$(JOYPAD)/usb/usbd/modes/switch_mode.c \
	$(JOYPAD)/usb/usbd/modes/ps3_mode.c \
	$(JOYPAD)/usb/usbd/modes/psclassic_mode.c \
	$(JOYPAD)/usb/usbd/modes/pcemini_mode.c \
	$(JOYPAD)/usb/usbd/modes/ps4_mode.c \
	$(JOYPAD)/usb/usbd/modes/dualsense_mode.c \
	$(JOYPAD)/usb/usbd/modes/p5general_mode.c \
	$(JOYPAD)/usb/usbd/modes/xid_mode.c \
	$(JOYPAD)/usb/usbd/modes/xbone_mode.c \
	$(JOYPAD)/usb/usbd/modes/xac_mode.c \
	$(JOYPAD)/usb/usbd/modes/kbmouse_mode.c \
	$(JOYPAD)/usb/usbd/modes/gc_adapter_mode.c \
	$(JOYPAD)/usb/usbd/cdc/cdc.c \
	$(JOYPAD)/usb/usbd/cdc/cdc_protocol.c \
	$(JOYPAD)/usb/usbd/cdc/cdc_commands.c \
	$(JOYPAD)/usb/usbd/drivers/tud_xid.c \
	$(JOYPAD)/usb/usbd/drivers/tud_xinput.c \
	$(JOYPAD)/usb/usbd/drivers/tud_xbone.c \
	$(JOYPAD)/usb/usbd/drivers/xgip_protocol.c \
	$(JOYPAD)/usb/usbd/kbmouse/kbmouse.c \

# --- Xbox 360 security (XInput/XID auth) ---
SRC_C += \
	$(JOYPAD)/lib/libxsm3/xsm3.c \
	$(JOYPAD)/lib/libxsm3/usbdsec.c \
	$(JOYPAD)/lib/libxsm3/excrypt_des.c \
	$(JOYPAD)/lib/libxsm3/excrypt_parve.c \
	$(JOYPAD)/lib/libxsm3/excrypt_sha.c \

# TinyUSB's cpu/rv32imac-ilp32.mk emits a modern `-march` string (currently
# `-march=rv32imac_zicsr_zifencei`). The WCH-bundled GCC 8.2.0 we build with
# predates the split-out `zicsr`/`zifencei` extensions and rejects those march
# substrings. Strip every `-march=...` flag and force plain `rv32imac` — this is
# a property of OUR toolchain choice, so the workaround lives in the joypad
# build, not in the (now-pristine) TinyUSB fork.
CFLAGS := $(filter-out -march=%,$(CFLAGS)) -march=rv32imac

include $(TINYUSB_ROOT)/hw/bsp/family_rules.mk

# ---------------------------------------------------------------------------
# Convenience targets (wlink = WCH-LinkE flasher, cargo install wlink)
# ---------------------------------------------------------------------------
WLINK ?= wlink

# family.mk wires `flash: flash-openocd-wch`, but we flash with WCH-LinkE via
# wlink (not riscv-openocd-wch). Override that target's recipe so `make flash`
# uses wlink. (GNU make: overriding recipe for flash-openocd-wch is intentional.)
flash-openocd-wch: $(BUILD)/$(PROJECT).elf
	$(WLINK) flash $<

.PHONY: reset monitor
reset:
	$(WLINK) reset

monitor:
	@echo "Serial debug is on USART1/PA9 @115200 (WCH-Link CDC bridge)."
	@echo "Use COMrade, or in this session: ! screen /dev/cu.usbmodem* 115200"
