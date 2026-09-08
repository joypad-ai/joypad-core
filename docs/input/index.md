# Input Interfaces

An input interface reads controller data from a specific source and normalizes it into the common `input_event_t` format. Once normalized, input events are submitted to the [router](../core/index.md) for delivery to outputs.

Every input -- whether a USB gamepad, a Bluetooth controller, a SNES pad wired directly to GPIO, or a phone connected over WiFi -- produces the same event structure. This is what makes Joypad OS universal: any input can drive any output.

## All Input Interfaces

| Input | Protocol | Location | Method | Notes |
|-------|----------|----------|--------|-------|
| USB HID | USB 2.0 | `src/usb/usbh/hid/` | TinyUSB host stack | Gamepads, keyboards, mice, hubs. Vendor-specific drivers for Xbox, PlayStation, Nintendo, 8BitDo, HORI, Logitech, Sega, Raphnet, and more. |
| XInput | USB 2.0 | `src/usb/usbh/xinput/` | TinyUSB host stack | Xbox 360, Xbox One, Xbox Series controllers via XInput protocol. |
| [Intel Wireless Series](../INTEL_WIRELESS_SERIES.md) | USB receiver / RF | `src/usb/usbh/intel/` | Custom TinyUSB host driver | Original `8086:C013` base station, gamepad activation and eight independent RF slots. |
| Bluetooth | BT Classic + BLE | `src/bt/` | BTstack | Wireless controllers via USB BT dongle (RP2040), built-in radio (Pico W), or BLE (ESP32-S3, nRF52840). Per-vendor drivers mirror USB HID structure. |
| WiFi (JOCP) | UDP/TCP over WiFi | `src/wifi/jocp/` | LWIP | Joypad Open Controller Protocol. Adapter runs as WiFi AP on Pico W. UDP port 30100 for input, TCP port 30101 for control. |
| SNES | Shift register (GPIO) | `src/native/host/snes/` | GPIO polling | SNES and NES controllers, SNES mouse, Xband keyboard. Also used for NES via compatible shift register protocol. |
| N64 | Joybus (PIO) | `src/native/host/n64/` | PIO state machine | N64 controllers. Supports rumble pak. Y-axis inverted during normalization. |
| GameCube | Joybus (PIO) | `src/native/host/gc/` | PIO state machine | GameCube controllers. Supports rumble. Y-axis inverted during normalization. Polls at 125Hz. |
| NES | PIO | `src/native/host/nes/` | PIO state machine | NES controllers via dedicated PIO program. |
| Neo Geo | GPIO (active-low) | `src/native/host/arcade/` | GPIO polling | Neo Geo arcade sticks. Internal pull-ups, active-low button reads. |
| LodgeNet | PIO | `src/native/host/lodgenet/` | PIO state machine | LodgeNet hotel system controllers. Supports N64, GameCube, and SNES controller types on the LodgeNet bus. |
| 3DO Host | PIO | `src/native/host/3do/` | PIO state machine | Reads 3DO controllers directly. |
| Nuon Host | Polyface (PIO) | (experimental) | PIO state machine | Reads Nuon controllers. Experimental support. |
| PSX/PS2 | SIO (PIO+DMA) | `src/native/host/psx/` | PIO+DMA, 500 kHz w/ active pull-up | PlayStation 1/2 controllers. Auto-detects digital, DualShock, DS2 (pressure), neGcon, flightstick, GunCon, JogCon, and PS Mouse. |
| GPIO | GPIO pins | `src/native/device/gpio/` | GPIO polling | Custom-wired buttons and analog sticks for bespoke controller builds (Fisher Price, Alpakka, etc.). |
| UART | Serial | `src/native/host/uart/` | UART RX | Input bridge from external microcontrollers (e.g., ESP32 Bluetooth bridge). |

## How Input Interfaces Work

Each input interface implements the `InputInterface` struct:

```c
typedef struct {
    const char* name;
    input_source_t source;
    void (*init)(void);
    void (*task)(void);
    bool (*is_connected)(void);
    uint8_t (*get_device_count)(void);
} InputInterface;
```

- `init()` is called once during startup to configure hardware (USB host, PIO programs, GPIO pins, etc.).
- `task()` is called every iteration of the Core 0 main loop. It polls for new data and calls `router_submit_input()` when a controller reports.
- `is_connected()` and `get_device_count()` let the system track active controllers.

## Normalization Rules

All input drivers follow these rules when building `input_event_t`:

1. **Buttons** use `JP_BUTTON_*` constants in W3C Gamepad API order. See the [Glossary](../overview/glossary.md) for the full mapping.
2. **Analog axes** are normalized to 0-255 with 128 as center. 0 = up/left, 255 = down/right (HID convention).
3. **Y-axis**: Drivers for Nintendo-convention controllers (N64, GameCube) must invert the Y axis during normalization.
4. **Device address**: USB devices use TinyUSB's `dev_addr`. Bluetooth uses addresses assigned by BTstack. Native controllers use the 0xD0+ range.

## Supported Controllers

USB and Bluetooth inputs support a wide range of controllers through vendor-specific drivers. Generic HID parsing handles any standard-compliant gamepad automatically. Vendor-specific drivers provide enhanced support (rumble, LEDs, analog triggers) for:

- Microsoft: Xbox 360, Xbox One, Xbox Series, Xbox Adaptive Controller
- Sony: DualShock 3, DualShock 4, DualSense, DualSense Edge
- Nintendo: Switch Pro, Joy-Con, GameCube adapter, NES/SNES Online
- 8BitDo: Pro 2, Ultimate, Lite, Zero, M30, SN30, and more
- HORI: Fighting Commander, RAP, HORIPAD
- Logitech: F310, F710
- Sega: Saturn/Genesis USB pads
- Raphnet: N64/GC to USB adapters
- Many more via generic HID

See the [controllers list](../hardware/controllers.md) for the full compatibility table.

## Next Steps

- [Architecture](../overview/architecture.md) -- Where inputs fit in the system
- [Data Flow](../overview/data-flow.md) -- How input events travel to outputs
- [Output Interfaces](../output/index.md) -- The other side of the pipeline
- [Apps](../apps/index.md) -- How apps select which inputs to use
