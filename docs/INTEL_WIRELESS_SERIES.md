# Intel Wireless Series Gamepad

The original Intel USB base station (`8086:C013`) uses a custom TinyUSB
host driver in `src/usb/usbh/intel/`. Eight RF slots are tracked separately
for each receiver. A remembered slot does not create a player; input from
a known gamepad slot enters JoypadOS through `router_submit_input()`.
Receiver keyboard and mouse support is not implemented. The driver owns
both USB interfaces so the boot mouse cannot collide with RF slot numbers.

## Verified USB transport

Read using local PyUSB/libusb on macOS on 2026-09-07. The receiver has one
configuration (value 1), 89 descriptor bytes and two interfaces.

| Interface / alternate | Class | Endpoint | Type | Max packet | Interval |
|---|---|---|---|---|---|
| 0 / 0 | HID boot keyboard | `81` | Interrupt IN | 8 | 10 ms |
| 0 / 1 | 0/0/0 | `81` | Interrupt IN | 27 | 1 ms |
| 0 / 1 | 0/0/0 | `01` | Interrupt OUT | 25 | 1 ms |
| 0 / 1 | 0/0/0 | `02` | Control | 8 | 0 |
| 1 / 0 | HID boot mouse | `82` | Interrupt IN | 4 | 10 ms |

The driver claims interface 0 before generic HID, finds alternate 1's
interrupt descriptors, then asynchronously selects it with SET_INTERFACE.
Only after success does it open the interrupt endpoints and poll 27 bytes.
Endpoint `02` is not an interrupt transport and is never opened.

The protocol reference is
[intel-wings 0.2](https://downloads.sourceforge.net/project/intel-wings/intel-wings/0.2/intel-wings-0.2.tbz2),
whose `intel-wings.c` internally identifies itself as version 0.6.
Its 128-byte transfers, uninitialized activation padding, hardcoded
alternate-array indexing, and reversed device-type-change comparison
are not used.

## Hardware capture

The connected gamepad successfully activated with exactly six output bytes;
neither 25-byte padding nor the Linux driver's 128-byte send was necessary:

```text
RX 7  03 01 0c 02 ff 00 00        remembered gamepad, slot 1
RX 7  03 02 0c 02 ff 00 00        remembered gamepad, slot 2
RX 7  03 03 01 02 ff 00 00        activate gamepad, slot 3
TX 6  03 01 ff 00 01 63
RX 7  06 03 00 00 ff 00 00        zero state, slot 3
RX 7  03 03 04 02 ff 00 00        gamepad ready, slot 3
RX 13 01 17 13 00 03 03 06 63 04 00 00 02 00
RX 13 01 00 00 00 03 03 06 63 04 00 00 00 00
```

Input has major type `packet[0] & 7 == 1`, RF slot `packet[4] & 7`,
and requires at least 13 bytes. The capture includes button presses and
releases in byte 11. All data is parsed using the actual received length.
Information messages require three bytes, or four when reading device type.
Activations are queued per slot and sent serially from a dedicated buffer;
a second activation cannot overwrite an in-flight transfer.

## Mapping and lifecycle

The mapping follows JoypadOS's M30 driver and `LAYOUT_SEGA_6BUTTON`:

| Physical control | Joypad button |
|---|---|
| A, B, X, Y | B1, B2, B3, B4 |
| Z, C | L1, R1 |
| L, R shoulders | L2, R2 |
| Start | S2 |
| Mouse | S1 (Select) |
| Shift | A1 (Guide) |

Bytes 9/10 contain digital X/Y: `81` means left/up and `7f` means
right/down. Other values mean neutral. These become D-pad buttons;
unused sticks remain centered at 128. Digital triggers are synthesized by
the existing profile system.

USB disconnect/deinitialization clears every submitted RF slot in the
router and player manager, then clears receiver buffers and activation state.
Slot replacement or reactivation also clears previous input. Major type 6
releases held inputs for a slot without forgetting its device type.
No arbitrary inactivity timeout is used: the captured controller reports
changes, so silence alone does not establish a disconnect. The exact RF
power-off notification still needs a hardware capture.

## Reproduce validation

```sh
/usr/bin/python3 -m venv /tmp/joypad-intel-wireless-series-venv
/tmp/joypad-intel-wireless-series-venv/bin/python -m pip install pyusb
/tmp/joypad-intel-wireless-series-venv/bin/python tools/intel-wireless-series/probe.py
sudo /tmp/joypad-intel-wireless-series-venv/bin/python tools/intel-wireless-series/probe.py --capture 45
sh tools/intel-wireless-series/test.sh
```

Install libusb separately if absent (Homebrew `libusb` on macOS).
macOS requires administrator access to detach its HID driver for capture;
the probe restores the original alternate setting and reattaches afterward.
During capture, power on the pad and press/release controls.

The C tests compile the production driver against the real TinyUSB headers,
with USB I/O/router test doubles and AddressSanitizer/UndefinedBehaviorSanitizer.
They replay the hardware sequence and test lengths, mappings, independent
slots/receivers, activation serialization/retries, and disconnect cleanup.
Firmware validation also includes an RP2040 `joypad_ngc` (KB2040) build.
The receiver was exercised through libusb on the Mac; the firmware has not
yet been flashed and tested on a JoypadOS adapter.
