#!/usr/bin/env python3

import argparse
import json
import time

import hid


CONFIG_USAGE_PAGE = 0xFF00
REPORT_LEN = 64
OUT_PAYLOAD_LEN = REPORT_LEN - 1


def parse_int(value):
    return int(value, 0)


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
                hex(device.get("usage")) if device.get("usage_page") is not None else None,
                device.get("interface_number"),
                device.get("product_string"),
                device.get("manufacturer_string"),
                device.get("path"),
            )
        )


def find_device(vid=None, pid=None, path=None, product_contains=None):
    if path is not None:
        matches = [device for device in hid.enumerate() if device.get("path") == path.encode() or device.get("path") == path]
        if not matches:
            raise RuntimeError(f"No HID device found at path {path!r}")
        return matches[0]

    candidates = list(hid.enumerate(vid, pid)) if vid is not None and pid is not None else list(hid.enumerate())
    if product_contains:
        needles = {
            "mayflash": ("mayflash", "magic-ns", "magic-ns2", "magic ns", "magic ns2"),
        }.get(product_contains.lower(), (product_contains.lower(),))
        candidates = [
            device for device in candidates
            if any(needle in (device.get("product_string") or "").lower() for needle in needles)
        ]

    matches = [device for device in candidates if device.get("usage_page") != CONFIG_USAGE_PAGE]
    if not matches:
        raise RuntimeError("No matching HID device found. Use --list, then pass --path, --vid/--pid, or --product-contains.")
    return matches[0]


def open_device(args):
    device_info = find_device(args.vid, args.pid, args.path, args.product_contains)
    print(f"Opening {device_info.get('product_string') or 'HID device'} path={device_info['path']!r}")
    if hasattr(hid, "Device"):
        return hid.Device(path=device_info["path"])

    device = hid.device()
    device.open_path(device_info["path"])
    return device


def read_once(device, timeout_ms):
    if hasattr(device, "read"):
        try:
            return bytes(device.read(REPORT_LEN, timeout_ms=timeout_ms))
        except TypeError:
            return bytes(device.read(REPORT_LEN, timeout_ms))
    if hasattr(device, "read_timeout"):
        return bytes(device.read_timeout(REPORT_LEN, timeout_ms))
    return bytes(device.read(REPORT_LEN))


def write_output(device, report_id, payload):
    payload = bytes(payload)
    report = bytes([report_id]) + payload + bytes(OUT_PAYLOAD_LEN - len(payload))
    written = device.write(report)
    if written <= 0:
        raise RuntimeError(f"HID write failed for report 0x{report_id:02x}")


def drain(device, seconds):
    end_at = time.monotonic() + seconds
    while time.monotonic() < end_at:
        read_once(device, 20)


def subcommand(device, counter, subcmd, args=()):
    payload = bytearray(OUT_PAYLOAD_LEN)
    payload[0] = counter & 0xff
    payload[1:9] = bytes([0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40])
    payload[9] = subcmd
    payload[10:10 + len(args)] = bytes(args)
    write_output(device, 0x01, payload)


def initialize_switch_pro_mode(device):
    drain(device, 0.3)
    for command in (0x01, 0x02, 0x03, 0x04):
        write_output(device, 0x80, bytes([command]))
        drain(device, 0.12)

    counter = 0
    for subcmd, args in (
        (0x02, ()),
        (0x10, (0x00, 0x60, 0x00, 0x00, 0x10)),
        (0x10, (0x50, 0x60, 0x00, 0x00, 0x06)),
        (0x10, (0x3d, 0x60, 0x00, 0x00, 0x12)),
        (0x10, (0x10, 0x80, 0x00, 0x00, 0x16)),
        (0x10, (0x86, 0x60, 0x00, 0x00, 0x12)),
        (0x10, (0x98, 0x60, 0x00, 0x00, 0x12)),
        (0x10, (0x20, 0x60, 0x00, 0x00, 0x18)),
        (0x10, (0x26, 0x80, 0x00, 0x00, 0x1a)),
        (0x10, (0x80, 0x60, 0x00, 0x00, 0x06)),
        (0x08, (0x00,)),
        (0x10, (0x00, 0x50, 0x00, 0x00, 0x01)),
        (0x30, (0x01,)),
        (0x03, (0x30,)),
        (0x48, (0x01,)),
        (0x40, (0x00,)),
        (0x30, (0x01,)),
    ):
        subcommand(device, counter, subcmd, args)
        counter += 1
        drain(device, 0.05)


def summarize(samples, report_id):
    times = [sample["t"] for sample in samples if sample["report_id"] == report_id]
    deltas = [(times[i] - times[i - 1]) * 1000 for i in range(1, len(times))]
    if not deltas:
        return None
    deltas_sorted = sorted(deltas)
    return {
        "count": len(times),
        "min_ms": min(deltas),
        "max_ms": max(deltas),
        "avg_ms": sum(deltas) / len(deltas),
        "median_ms": deltas_sorted[len(deltas_sorted) // 2],
        "p95_ms": deltas_sorted[int(len(deltas_sorted) * 0.95)],
    }


def main():
    parser = argparse.ArgumentParser(description="Capture HID input report timing from a Switch Pro-compatible device.")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--vid", type=parse_int)
    parser.add_argument("--pid", type=parse_int)
    parser.add_argument("--path")
    parser.add_argument("--product-contains", default="mayflash")
    parser.add_argument("--seconds", type=float, default=5.0)
    parser.add_argument("--out", default="switch2-hid-timing.json")
    parser.add_argument("--skip-init", action="store_true")
    args = parser.parse_args()

    if args.list:
        list_devices()
        return

    device = open_device(args)
    samples = []
    started_at = time.monotonic()
    try:
        if not args.skip_init:
            initialize_switch_pro_mode(device)
            drain(device, 0.2)
            started_at = time.monotonic()

        end_at = started_at + args.seconds
        while time.monotonic() < end_at:
            data = read_once(device, 25)
            if not data:
                continue
            samples.append({
                "t": time.monotonic() - started_at,
                "report_id": data[0],
                "data": data.hex(" "),
            })
    finally:
        close = getattr(device, "close", None)
        if close:
            close()

    result = {
        "samples": samples,
        "summary_0x30": summarize(samples, 0x30),
        "summary_0x21": summarize(samples, 0x21),
    }
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(result, f, indent=2)
    print(json.dumps({k: v for k, v in result.items() if k != "samples"}, indent=2))
    print(f"Wrote {args.out} samples={len(samples)}")


if __name__ == "__main__":
    main()
