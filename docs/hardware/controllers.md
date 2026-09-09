# Supported Controllers

Complete list of supported input devices for Joypad OS adapters.

## USB Controllers

### Xbox Controllers
- Xbox Original (Duke/S-Controller)
- Xbox 360 (wired and wireless with adapter)
- Xbox One (all revisions)
- Xbox Series X|S

**Features:**
- Full button and analog support
- Rumble feedback
- X-input protocol

### PlayStation Controllers
- PlayStation Classic Controller
- DualShock 3 (PS3)
- DualShock 4 (PS4)
- DualSense (PS5)

**Features:**
- Full button and analog support
- Rumble feedback (DS3/DS4/DS5)
- Touchpad button (DS4/DS5)
- Adaptive trigger threshold (DualSense)

### Nintendo Controllers
- **Switch Pro Controller** - Full support with rumble
- **Switch 2 Pro Controller** - Full support
- **Joy-Con Grip** - Dual Joy-Cons in grip mode
- **Joy-Con Single** - Individual Joy-Con support
- **GameCube Adapter** - Official Nintendo GameCube adapter (4 ports)
- **NSO GameCube Controller** - Nintendo Switch Online GameCube controller

**Features:**
- Full button and analog support
- Rumble feedback (Pro Controller)
- Capture button support
- Home button support

### 8BitDo Controllers

**Wireless Controllers:**
- PCEngine 2.4g Controller
- M30 2.4g Controller (Genesis/Mega Drive)
- M30 Bluetooth Controller
- NeoGeo Controller

**USB Adapters:**
- Wireless USB Adapter (Grey/Red)
- Wireless USB Adapter 2 (Black/Red)

**Features:**
- Full button support
- Analog triggers (M30)
- Turbo functionality
- Mode switching

### Other Supported Controllers
- **Logitech Wingman Action Pad** - Classic PC gamepad
- **Sega Astrocity Mini Controller** - Arcade stick
- **Hori Pokken Tournament Controller** - Fight stick
- **Hori Horipad** - Generic Hori gamepads
- **Google Stadia Controller** - USB mode
- **Generic DirectInput Controllers** - Most D-input gamepads
- **Generic HID Gamepads** - Standard USB HID joysticks

## USB Keyboards

**All standard USB HID keyboards supported:**
- Full key mapping to controller buttons
- GameCube: Dedicated keyboard mode (Scroll Lock / F14 / Ctrl+Alt+K)
- Arrow keys → D-Pad
- WASD → Left stick
- Space/Enter → Action buttons

**Tested Keyboards:**
- Apple Magic Keyboard
- Logitech K120/K380
- Generic USB keyboards
- Mechanical keyboards (Cherry MX, etc.)

## USB Mice

**All standard USB HID mice supported:**
- Optical mice
- Laser mice
- Gaming mice with high DPI
- Trackballs

**Functionality by Console:**
- PCEngine: Mouse emulation (Afterburner II, Darius Plus)
- Nuon: Spinner emulation (Tempest 3000)
- 3DO: Mouse emulation (native 3DO mouse protocol)
- GameCube: Mouse → stick emulation

**Not Supported:**
- Scroll wheel (ignored)
- Extra mouse buttons (buttons 4+)

## USB Hubs

**Multi-player support via USB hubs:**
- Up to 8 simultaneous devices (3DO)
- Up to 5 simultaneous devices (PCEngine)
- Up to 4 simultaneous devices (Loopy, GameCube, Dreamcast)
- Any standard USB 2.0 hub
- Powered hubs recommended for 4+ devices

**Tested Hubs:**
- Anker 4-Port USB 3.0 Hub
- Amazon Basics 7-Port USB Hub
- Generic USB 2.0 hubs

**Limitations:**
- Total current draw must not exceed USB spec
- Some controllers require more power (rumble)
- Use powered hub for 3+ high-power devices

## Bluetooth Controllers (via Dongle or Built-in)

Bluetooth controllers connect via USB Bluetooth dongles on RP2040 boards, or via built-in radio on Pico W, ESP32-S3, and nRF52840.

### Classic BT + BLE (Pico W / Pico 2 W / USB Dongle)

| Controller | Status |
|---|---|
| DualShock 3 (PS3) | Supported (with rumble) |
| DualShock 4 (PS4) | Supported (with rumble, touchpad) |
| DualSense (PS5) | Supported (with rumble) |
| Switch Pro Controller | Supported (with rumble) |
| Switch 2 Pro Controller | Supported |
| Wii U Pro Controller | Supported |
| NSO GameCube | Supported |
| Xbox One / Series (BT mode) | Supported |
| Google Stadia | Supported |
| Generic BT HID | Basic support |

### BLE Only (ESP32-S3 / nRF52840)

| Controller | Status |
|---|---|
| Xbox One / Series (BLE mode) | Supported |
| 8BitDo controllers (BLE mode) | Supported |
| Switch 2 Pro (BLE) | Supported |
| Generic BLE HID gamepads | Supported |

Classic BT controllers (DS3, DS4, DualSense, Switch Pro, Wii U Pro) require the Pico W or a USB BT dongle.

### Bluetooth Dongles

**Important**: Only dongles with firmware in ROM work on embedded. Most BT 5.0+ dongles use Realtek chips that require host-side firmware loading and **will not work**.

**Supported Chipsets:**
- **Broadcom** (e.g. BCM20702A0) — firmware in ROM, recommended
- **CSR/Cambridge Silicon Radio** (e.g. CSR8510 A10) — firmware in ROM, works but beware counterfeits
- **Barrot BR8654A02** - firmware in ROM

**Not Supported:**
- **Realtek** (RTL8761B, RTL8761BU, etc.) — requires firmware loading at every boot, not implemented
- This includes almost all BT 5.0+ dongles on the market

**Tested and Working:**
- Kinivo BTD-400 (Broadcom BCM20702A0, BT 4.0) — recommended
- Panda PBU40 (Broadcom BCM20702A0, BT 4.0) — recommended
- Amazon Basics BT 4.0 (unknown Chinese chip, BT 4.0)
- ASUS USB-BT400 (Broadcom BCM20702, BT 4.0)
- Adafruit Bluetooth 4.0 USB Module #1327 (CSR8510 A10)
- UGREEN Bluetooth 6.0 USB Adapter (Barrot BR8654A02)

**Known Not Working:**
- TP-Link UB400/UB500 (Realtek RTL8761B)
- ASUS USB-BT500 (Realtek RTL8761B)
- UGREEN BT 5.0 adapters (Realtek)
- Avantree DG45 (Realtek)
- Zexmte BT 5.0 (Realtek)

**Buying Tips:**
- Look for BT 4.0 dongles with Broadcom chips
- Kinivo BTD-400 and Panda PBU40 are safe choices (~$12)
- Avoid random "CSR8510" listings on Amazon — many are counterfeit clones with pairing issues
- BT 5.0+ dongles are almost all Realtek — avoid for embedded use

**Note**: Bluetooth adds slight latency compared to wired USB. For competitive play, wired is recommended.
