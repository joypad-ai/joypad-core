# GameCube / Wii Output Interface

Emulates a GameCube controller connected to a GameCube or Wii console via the single-wire joybus protocol. Supports up to 4 players, analog triggers, rumble feedback, keyboard mode (for Phantasy Star Online), and app-defined button remapping profiles.

## Protocol

- **Wire protocol**: Single-wire bidirectional joybus at ~250kHz bit rate
- **PIO program**: `joybus.pio` (from `lib/joybus-pio`)
- **Clock requirement**: 130MHz overclock (`set_sys_clock_khz(130000, true)`) -- required for joybus timing accuracy
- **Data pin**: GPIO 7 (KB2040 default)
- **Core**: Runs on Core 1 in a tight loop via `__not_in_flash_func`

The console polls the controller, the adapter responds with a `gc_report_t` containing digital buttons, two analog sticks, and two analog trigger values. The poll command includes a rumble motor byte that the adapter reads back.

### GameCube Controller Cable Pinout

| Pin | Signal | Description |
|-----|--------|-------------|
| 1 | VCC | 5V power |
| 2 | Data | Bidirectional data line |
| 3 | GND | Ground |
| 4 | GND | Ground |
| 5 | (unused) | |
| 6 | 3.3V | Optional, some boards |

See [GameCube Joybus Protocol](../protocols/GAMECUBE_JOYBUS.md) for wire-level details.

### Poll Modes

The GameCube/Wii console can request different report formats:

| Mode | Constant | Description |
|------|----------|-------------|
| 0 | `BUTTON_MODE_0` | Standard controller mode |
| 1 | `BUTTON_MODE_1` | Alternate report format |
| 2 | `BUTTON_MODE_2` | Alternate report format |
| 3 | `BUTTON_MODE_3` | Default gamepad mode (most common) |
| 4 | `BUTTON_MODE_4` | Alternate report format |
| KB | `BUTTON_MODE_KB` | Keyboard mode (3 keypresses + checksum) |

The adapter starts in Mode 3 and switches to KB mode when toggled by the user.

## Player Support

- **Max players**: 4
- **Routing**: Typically MERGE mode (all inputs merged to a single controller output)
- **Multitap**: Each adapter instance emulates one controller port

## Button Mapping

The default mapping uses `GC_BUTTON_*` aliases defined in `gamecube_buttons.h`. Profile-based remapping is applied before the final GC report is built.

### Default JP to GC Mapping

| JP_BUTTON_* | GC Button | Notes |
|-------------|-----------|-------|
| `JP_BUTTON_B1` | B | Small red button |
| `JP_BUTTON_B2` | A | Large green button |
| `JP_BUTTON_B3` | Y | Above A |
| `JP_BUTTON_B4` | X | Right of A |
| `JP_BUTTON_L1` | (disabled) | Not mapped in default profile |
| `JP_BUTTON_R1` | Z | Digital, above R trigger |
| `JP_BUTTON_L2` | L | Analog + digital trigger |
| `JP_BUTTON_R2` | R | Analog + digital trigger |
| `JP_BUTTON_S1` | (profile switch) | Hold 2s then D-pad to cycle |
| `JP_BUTTON_S2` | Start | Start button |
| `JP_BUTTON_DU/DD/DL/DR` | D-pad | Direct mapping |

## Analog Mapping

| Input | GC Output | Notes |
|-------|-----------|-------|
| Left stick X/Y | Control Stick | Y-axis inverted (HID 0=up to GC 0=down). Clamped to 1-255 (some games reject 0). |
| Right stick X/Y | C-Stick | Same inversion and clamping. |
| L2 analog | L trigger analog (0-255) | Threshold-configurable per profile for digital L press. |
| R2 analog | R trigger analog (0-255) | Threshold-configurable per profile for digital R press. |

Keyboard input applies a 0.61x scale factor to analog values (78/128 range) to match the smaller travel expected by games.

## Feedback

### Rumble

The GameCube console sends a rumble byte with each poll. When active, the adapter returns 255 to the player manager, which routes it to the connected USB/BT controller's vibration motor.

### Keyboard LED

In keyboard mode, the adapter reports an LED state (Scroll Lock indicator) that can be read by input controllers.

### Adaptive Triggers (DualSense)

Per-profile L2/R2 thresholds control when analog trigger values trigger the digital L/R press. A threshold of 0 disables analog-to-digital conversion.

## Keyboard Mode

Toggle keyboard mode by pressing **Scroll Lock**, **F14**, or **Ctrl+Alt+K** on a connected USB keyboard:

- All standard HID keys are mapped to GameCube keyboard scancodes via a 256-entry lookup table
- Reports contain 3 keypresses + checksum + counter
- Works with Phantasy Star Online and other keyboard-compatible games
- LED indicator shows when keyboard mode is active
- A1 (Home/Guide) sends the gc-swiss IGR combo (Select+D-down+B+R)

## Profiles

Profiles are defined at the app level (e.g., `src/apps/usb2gc/profiles.h`). The output interface exposes profile count, active index, set/get, and name accessors through the `OutputInterface` struct.

**Switching profiles:**

1. Press **SELECT + D-Pad Up** (previous profile) or **SELECT + D-Pad Down** (next) — both buttons
   together, not one after the other
2. Keep them held for about **0.7 s**; a quick tap passes straight through to the game
3. LED flashes and the controller rumbles to confirm
4. The selection stops at the first and last profile — it does not wrap around
5. Profile saves to flash (persists across power cycles)

See the [usb2gc app](../apps/usb2gc.md) for specific profile definitions (Default, SNES, SSBM, MKWii, Fighting).

## Apps Using This Output

| App | Description |
|-----|-------------|
| `usb2gc` | USB/BT controllers to GameCube |
| `bt2gc` | Bluetooth controllers to GameCube (Pico W) |
| `lodgenet2gc` | LodgeNet hotel controllers to GameCube (Pico) |
| `n642gc` | N64 controllers to GameCube (planned) |
