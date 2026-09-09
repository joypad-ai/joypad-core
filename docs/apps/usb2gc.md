# usb2gc

USB/BT controllers to GameCube/Wii console.

## Overview

Connects USB and Bluetooth controllers to a GameCube or Wii console via the joybus protocol. Supports 5 button mapping profiles, keyboard mode for PSO, copilot mode (merge multiple controllers), and rumble feedback. Requires 130MHz overclock for joybus timing.

## Input

- [USB HID](../input/usb-hid.md), [XInput](../input/xinput.md), [Bluetooth](../input/bluetooth.md) controllers
- USB keyboard (keyboard mode for Phantasy Star Online)
- USB mouse (mapped to analog stick)

## Output

[GameCube Output](../output/gamecube.md) -- joybus PIO protocol on a single port.

## Core Configuration

| Setting | Value |
|---------|-------|
| Routing mode | MERGE (all inputs blended to one output) |
| Player slots | 4 (fixed assignment) |
| Max USB devices | 4 |
| Clock speed | 130MHz (required for joybus) |
| Profile system | 5 profiles, saved to flash |

## Profiles

Press **SELECT + D-Pad Up** (previous profile) or **SELECT + D-Pad Down** (next) together and hold for about **0.7 s** — pressing them one after the other does nothing, and a quick tap passes through to the game. The selection stops at the ends rather than wrapping. Controller rumbles and LED flashes to confirm. Profile persists across power cycles.

| Profile | Description |
|---------|-------------|
| **Default** | Standard mapping for most games |
| **SNES** | L/R as full analog press, Select mapped to Z |
| **SSBM** | Super Smash Bros. Melee competitive (light shield, multi-jump, 85% stick sensitivity) |
| **MKWii** | Mario Kart Wii (LB=wheelie, RB=drift, RT=item throw) |
| **Fighting** | 2D fighters (right stick disabled, LB=C-Up macro) |

See the [Profiles (Detailed)](#profiles-detailed) section below for full button mapping tables per profile.

## Key Features

- **Keyboard mode** -- Toggle with Scroll Lock, F14, or Ctrl+Alt+K. Works with PSO and other GC keyboard games.
- **Copilot mode** -- Multiple controllers merge into one output. All inputs OR'd together.
- **Rumble** -- Forwarded to compatible controllers (Xbox, DualShock 3/4/5, Switch Pro, 8BitDo).
- **Adaptive triggers** -- DualSense L2/R2 analog mapped to GC L/R with configurable threshold.

## Supported Boards

| Board | Build Command |
|-------|---------------|
| KB2040 | `make usb2gc_kb2040` |
| RP2040-Zero | `make usb2gc_rp2040zero` |

## Build and Flash

```bash
make usb2gc_kb2040
make flash-usb2gc_kb2040
```

## Profiles (Detailed)

### Default Profile

Standard mapping for most games:

| USB Input | GameCube Output |
|-----------|-----------------|
| B1 (Cross/A) | B |
| B2 (Circle/B) | A |
| B3 (Square/X) | Y |
| B4 (Triangle/Y) | X |
| L1 (LB/L) | (disabled) |
| R1 (RB/R) | Z |
| L2 (LT/ZL) | L (analog) |
| R2 (RT/ZR) | R (analog) |
| S1 (Select) | (profile switch) |
| S2 (Start) | Start |
| D-Pad | D-Pad |
| Left Stick | Control Stick |
| Right Stick | C-Stick |

### SNES Profile

For SNES-style controllers (L/R as full press):

| USB Input | GameCube Output |
|-----------|-----------------|
| L1 (LB) | L (digital + full analog) |
| R1 (RB) | R (digital + full analog) |
| S1 (Select) | Z |
| Other | Same as Default |

### SSBM Profile

Super Smash Bros. Melee competitive mapping:

| USB Input | GameCube Output | Notes |
|-----------|-----------------|-------|
| B1 (Cross/A) | B | Attack |
| B2 (Circle/B) | A | Jump |
| B3 (Square/X) | Y | Jump |
| B4 (Triangle/Y) | X | Jump |
| L1 (LB) | Z | Grab |
| R1 (RB) | X | Short hop aerial |
| L2 (LT @ 88%) | L (17% analog) | Light shield |
| R2 (RT @ 55%) | L+R | Quit combo |
| Left Stick | 85% sensitivity | Precision movement |

Key features: light shield at 17% on L trigger, quick quit combo (L+R+Start) on RT, multiple jump buttons (A, X, Y, RB), 85% stick sensitivity for precise movement.

### MKWii Profile

Mario Kart Wii optimized:

| USB Input | GameCube Output | Notes |
|-----------|-----------------|-------|
| L1 (LB) | D-Up | Wheelies/tricks |
| R1 (RB) | R (full analog) | Drift |
| R2 (RT) | Z (instant) | Item throw |
| Other | Same as Default | |

### Fighting Profile

2D fighting game layout:

| USB Input | GameCube Output | Notes |
|-----------|-----------------|-------|
| L1 (LB) | C-Up | Macro input |
| R1 (RB) | Z | |
| Right Stick | Disabled | Prevents accidental input |
| Other | Same as Default | |

## Troubleshooting

**Controller not detected by console:**
- Check GameCube cable connections (data, 3.3V, ground).
- Verify the data pin assignment matches your board.

**Rumble not working:**
- Only compatible USB/BT controllers support rumble feedback.
- Check USB power supply -- rumble requires more current. Use a powered USB hub for multiple controllers.

**Profile not saving:**
- Wait 5 seconds after changing the profile for the flash write to complete.
- Reflash firmware if flash memory appears corrupted.

**Keyboard mode not activating:**
- Press Scroll Lock or F14 on the connected USB keyboard.
- No Scroll Lock and no F13-F24 row (most 65% and TKL boards)? Press **Ctrl+Alt+K** instead.
- Check the LED indicator for keyboard mode status.

**Stick drift or incorrect calibration:**
- GameCube expects analog center at 128. USB controllers are auto-calibrated.
- Check for physical stick drift on the input controller.
