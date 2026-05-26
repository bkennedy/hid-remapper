#!/usr/bin/env python3

import argparse
import json
import time

import hid


SWITCH_PRO_VENDOR_ID = 0x057E
SWITCH_PRO_PRODUCT_ID = 0x2009
REMAPPER_VENDOR_ID = 0xCAFE
REMAPPER_PRODUCT_ID = 0xBAF2
CONFIG_USAGE_PAGE = 0xFF00
REPORT_LEN = 64
OUT_PAYLOAD_LEN = REPORT_LEN - 1


def parse_int(value):
    return int(value, 0)


def find_device(vid=None, pid=None, path=None, product_contains=None):
    if path is not None:
        matches = [device for device in hid.enumerate() if device.get("path") == path.encode() or device.get("path") == path]
        if not matches:
            raise RuntimeError(f"No HID device found at path {path!r}")
        return matches[0]

    if vid is not None and pid is not None:
        candidates = list(hid.enumerate(vid, pid))
    else:
        candidates = list(hid.enumerate(SWITCH_PRO_VENDOR_ID, SWITCH_PRO_PRODUCT_ID))
        candidates.extend(hid.enumerate(REMAPPER_VENDOR_ID, REMAPPER_PRODUCT_ID))

    if product_contains:
        product_contains = product_contains.lower()
        product_aliases = {
            "mayflash": ("mayflash", "magic-ns", "magic-ns2", "magic ns", "magic ns2"),
        }
        product_needles = product_aliases.get(product_contains, (product_contains,))
        candidates = [
            device
            for device in candidates
            if any(needle in (device.get("product_string") or "").lower() for needle in product_needles)
        ]

    matches = [device for device in candidates if device.get("usage_page") != CONFIG_USAGE_PAGE]
    if not matches:
        raise RuntimeError("No matching gamepad HID interface found. Use --list, then pass --vid/--pid, --path, or --product-contains.")
    return matches[0]


def list_devices():
    devices = sorted(
        hid.enumerate(),
        key=lambda device: (
            device.get("vendor_id") or 0,
            device.get("product_id") or 0,
            device.get("usage_page") or 0,
            device.get("usage") or 0,
        ),
    )
    for device in devices:
        print(
            "vid={:04x} pid={:04x} usage_page={} usage={} iface={} product={!r} manufacturer={!r} path={!r}".format(
                device.get("vendor_id") or 0,
                device.get("product_id") or 0,
                hex(device.get("usage_page")) if device.get("usage_page") is not None else None,
                hex(device.get("usage")) if device.get("usage") is not None else None,
                device.get("interface_number"),
                device.get("product_string"),
                device.get("manufacturer_string"),
                device.get("path"),
            )
        )


def open_device(args):
    device_info = find_device(args.vid, args.pid, args.path, args.product_contains)
    print(f"Opening {device_info.get('product_string') or 'Switch Pro device'} path={device_info['path']!r}")
    if hasattr(hid, "Device"):
        return hid.Device(path=device_info["path"])

    device = hid.device()
    device.open_path(device_info["path"])
    return device


def write_output(device, report_id, payload):
    payload = bytes(payload)
    if len(payload) > OUT_PAYLOAD_LEN:
        raise ValueError("payload too large")
    report = bytes([report_id]) + payload + bytes(OUT_PAYLOAD_LEN - len(payload))
    written = device.write(report)
    if written <= 0:
        raise RuntimeError(f"HID write failed for report 0x{report_id:02x}")
    print(f"> report=0x{report_id:02x} data={payload[:16].hex(' ')}")


def read_once(device, timeout_ms):
    if hasattr(device, "read"):
        try:
            return bytes(device.read(REPORT_LEN, timeout_ms=timeout_ms))
        except TypeError:
            return bytes(device.read(REPORT_LEN, timeout_ms))
    if hasattr(device, "read_timeout"):
        return bytes(device.read_timeout(REPORT_LEN, timeout_ms))
    return bytes(device.read(REPORT_LEN))


def drain(device, seconds, capture=None, label="read"):
    end_at = time.time() + seconds
    reports = []
    while time.time() < end_at:
        data = read_once(device, 100)
        if not data:
            continue
        reports.append(data)
        if capture is not None:
            capture.append({
                "label": label,
                "report_id": data[0],
                "len": len(data),
                "data": data.hex(" "),
            })
        print(f"< report=0x{data[0]:02x} len={len(data)} data={data[1:17].hex(' ')}")
    return reports


def usb_init(device, capture=None):
    for command in (0x01, 0x02, 0x03, 0x04):
        write_output(device, 0x80, bytes([command]))
        if capture is not None:
            capture.append({"label": "write_usb_init", "report_id": 0x80, "data": f"{command:02x}"})
        drain(device, 0.25, capture, f"usb_init_{command:02x}")


def subcommand(device, counter, subcmd, args=(), capture=None):
    payload = bytearray(OUT_PAYLOAD_LEN)
    payload[0] = counter & 0xff
    payload[1:9] = bytes([0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40])
    payload[9] = subcmd
    payload[10:10 + len(args)] = bytes(args)
    write_output(device, 0x01, payload)
    if capture is not None:
        capture.append({
            "label": "write_subcommand",
            "report_id": 0x01,
            "subcommand": subcmd,
            "args": bytes(args).hex(" "),
            "data": bytes(payload).hex(" "),
        })
    drain(device, 0.35, capture, f"subcommand_{subcmd:02x}")


def main():
    parser = argparse.ArgumentParser(description="Run a Mac-side Switch Pro handshake against HID Remapper.")
    parser.add_argument("--list", action="store_true", help="list visible HID devices and exit")
    parser.add_argument("--vid", type=parse_int, help="vendor ID to open, for example 0x057e")
    parser.add_argument("--pid", type=parse_int, help="product ID to open, for example 0x2009")
    parser.add_argument("--path", help="exact HID path from --list")
    parser.add_argument("--product-contains", help="only open a device whose product string contains this text")
    parser.add_argument("--capture-json", help="write all writes and reads to this JSON file")
    parser.add_argument("--read-seconds", type=float, default=2.0, help="seconds to read final 0x30 input reports")
    args = parser.parse_args()

    if args.list:
        list_devices()
        return

    if (args.vid is None) != (args.pid is None):
        raise RuntimeError("--vid and --pid must be provided together")

    capture = []
    device = open_device(args)
    try:
        print("Draining startup reports...")
        drain(device, 0.5, capture, "startup")

        print("USB init...")
        usb_init(device, capture)

        print("Subcommands...")
        counter = 0
        for subcmd, subargs in (
            (0x33, ()),
            (0x40, (0x00,)),
            (0x41, (0x03, 0x00, 0x01, 0x01)),
            (0x10, (0x3d, 0x60, 0x00, 0x00, 18)),
            (0x10, (0x86, 0x60, 0x00, 0x00, 18)),
            (0x48, (0x01,)),
            (0x03, (0x30,)),
        ):
            subcommand(device, counter, subcmd, subargs, capture)
            counter += 1

        print(f"Reading input for {args.read_seconds}s...")
        reports = drain(device, args.read_seconds, capture, "final_input")
        counts = {}
        for report in reports:
            counts[report[0]] = counts.get(report[0], 0) + 1
        print("Counts:", " ".join(f"0x{report_id:02x}={count}" for report_id, count in sorted(counts.items())))
        if args.capture_json:
            with open(args.capture_json, "w", encoding="utf-8") as f:
                json.dump(capture, f, indent=2)
            print(f"Wrote {args.capture_json}")
    finally:
        close = getattr(device, "close", None)
        if close:
            close()


if __name__ == "__main__":
    main()
