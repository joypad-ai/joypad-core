#!/usr/bin/env python3
"""Enumerate Intel receiver descriptors, optionally capture/activate gamepads.

Requires PyUSB and libusb. On macOS, capture needs sudo to detach Apple HID.
Descriptor enumeration does not claim interfaces or change configuration.
"""
import argparse
import ctypes.util
import sys
import time

import usb.backend.libusb1
import usb.core
import usb.util


def find_library(name):
    return ctypes.util.find_library(name) or (
        "/opt/homebrew/lib/libusb-1.0.dylib" if sys.platform == "darwin" else None
    )


def capture(dev, seconds):
    alt = dev.get_active_configuration()[(0, 1)]
    endpoints = {ep.bEndpointAddress: ep for ep in alt}
    ep_in, ep_out = endpoints[0x81], endpoints[0x01]
    if (ep_in.bmAttributes & 3, ep_in.wMaxPacketSize,
        ep_out.bmAttributes & 3, ep_out.wMaxPacketSize) != (3, 27, 3, 25):
        raise RuntimeError("Unexpected Intel transport descriptors")

    claimed = detached = changed = False
    original_alt = 0
    try:
        if dev.is_kernel_driver_active(0):
            dev.detach_kernel_driver(0)
            detached = True
        usb.util.claim_interface(dev, 0)
        claimed = True
        original_alt = int(dev.ctrl_transfer(0x81, 10, 0, 0, 1)[0])  # GET_INTERFACE
        dev.set_interface_altsetting(interface=0, alternate_setting=1)
        changed = True
        print(f"Listening on interface 0 alt 1 for {seconds:g} seconds", flush=True)
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            try:
                packet = bytes(ep_in.read(ep_in.wMaxPacketSize, timeout=500))
            except usb.core.USBTimeoutError:
                continue
            print("RX", len(packet), packet.hex(" "), flush=True)
            if (len(packet) >= 4 and packet[0] & 7 == 3
                    and packet[2] == 1 and packet[3] == 2):
                reply = bytes([packet[1] & 7, 1, 255, 0, 1, 99])
                sent = ep_out.write(reply, timeout=1000)
                print("TX", sent, reply.hex(" "), flush=True)
                if sent != len(reply):
                    raise RuntimeError("Short activation write")
    finally:
        # Try every cleanup step even when the receiver has been unplugged.
        cleanup = []
        if changed:
            cleanup.append(lambda: dev.set_interface_altsetting(interface=0, alternate_setting=original_alt))
        if claimed:
            cleanup.append(lambda: usb.util.release_interface(dev, 0))
        if detached:
            cleanup.append(lambda: dev.attach_kernel_driver(0))
        for action in cleanup:
            try:
                action()
            except usb.core.USBError as exc:
                print(f"Cleanup: {exc}", file=sys.stderr)
        usb.util.dispose_resources(dev)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", type=float, metavar="SECONDS",
                        help="claim interface 0, activate gamepads and print raw packets")
    args = parser.parse_args()
    if args.capture is not None and not 0 < args.capture <= 3600:
        parser.error("capture duration must be between 0 and 3600 seconds")
    backend = usb.backend.libusb1.get_backend(find_library=find_library)
    dev = usb.core.find(idVendor=0x8086, idProduct=0xc013, backend=backend)
    if dev is None:
        raise SystemExit("Intel 8086:C013 receiver not found")
    print(f"Device {dev.bus}:{dev.address}, speed={dev.speed}, configurations={dev.bNumConfigurations}")
    for cfg in dev:
        print(f"Configuration {cfg.bConfigurationValue}: {cfg.wTotalLength} bytes, {cfg.bNumInterfaces} interfaces")
        for itf in cfg:
            print(f"  Interface {itf.bInterfaceNumber} alt {itf.bAlternateSetting}: "
                  f"class/subclass/protocol={itf.bInterfaceClass}/{itf.bInterfaceSubClass}/{itf.bInterfaceProtocol}")
            for ep in itf:
                print(f"    EP {ep.bEndpointAddress:#04x}: attributes={ep.bmAttributes:#04x}, "
                      f"max_packet={ep.wMaxPacketSize}, interval={ep.bInterval}")
    if args.capture is not None:
        capture(dev, args.capture)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
