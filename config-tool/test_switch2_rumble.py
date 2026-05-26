#!/usr/bin/env python3

import argparse
import time

import hid


HORIPAD_VENDOR_ID = 0x0F0D
HORIPAD_PRODUCT_ID = 0x00C1
RUMBLE_REPORT_ID = 2
RUMBLE_PAYLOAD_LEN = 63


def find_device():
    matches = [
        device
        for device in hid.enumerate(HORIPAD_VENDOR_ID, HORIPAD_PRODUCT_ID)
        if device.get("usage_page") != 0xFF00
    ]
    if not matches:
        raise RuntimeError("No HID Remapper Horipad device found. Select the Horipad/Switch output descriptor and reconnect it.")
    return matches[0]


def open_device():
    device_info = find_device()
    if hasattr(hid, "Device"):
        return hid.Device(path=device_info["path"])

    device = hid.device()
    device.open_path(device_info["path"])
    return device


def rumble_payload(strength):
    strength = max(0, min(255, strength))
    payload = bytearray(RUMBLE_PAYLOAD_LEN)

    # The firmware decodes the Switch-like report from bytes 3-6 and 19-22
    # after report ID 2 has been prepended. Maximize both low/high amplitudes
    # proportionally enough for a simple smoke test.
    amp_hi = int(strength * 0xFC / 255)
    amp_nibble = int(strength * 0x0F / 255)
    amp_byte = strength
    for offset in (2, 18):
        payload[offset] = amp_hi & 0xFC
        payload[offset + 1] = amp_nibble & 0x0F
        payload[offset + 2] = 0xC0 if strength else 0x00
        payload[offset + 3] = amp_byte

    return payload


def send_rumble(device, strength):
    report = bytes([RUMBLE_REPORT_ID]) + rumble_payload(strength)
    written = device.write(report)
    if written <= 0:
        raise RuntimeError("HID write failed")


def main():
    parser = argparse.ArgumentParser(description="Send a test Switch-style rumble output report to HID Remapper.")
    parser.add_argument("--strength", type=int, default=192, help="rumble strength 0-255")
    parser.add_argument("--duration", type=float, default=0.5, help="seconds to rumble before sending stop")
    parser.add_argument("--repeat", type=int, default=1, help="number of rumble pulses")
    args = parser.parse_args()

    device = open_device()
    try:
        for _ in range(args.repeat):
            send_rumble(device, args.strength)
            time.sleep(args.duration)
            send_rumble(device, 0)
            time.sleep(0.2)
    finally:
        close = getattr(device, "close", None)
        if close:
            close()


if __name__ == "__main__":
    main()
