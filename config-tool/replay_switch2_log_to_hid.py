#!/usr/bin/env python3

import argparse
import json
import re
import time

import hid


CONFIG_USAGE_PAGE = 0xFF00
REPORT_LEN = 64
OUT_PAYLOAD_LEN = REPORT_LEN - 1

CAPTURE_RE = re.compile(
    r"SWITCH2 CAPTURE (?P<index>\d+) source=(?P<source>\w+) iface=(?P<iface>\d+) "
    r"report_id=(?P<report_id>\d+) len=(?P<len>\d+) .* t=(?P<t>\d+)"
)
CAPTURE_COMPACT_RE = re.compile(
    r"SWITCH2 CAPTURE (?P<index>\d+) source=(?P<source>\w+) iface=(?P<iface>\d+) "
    r"report_id=(?P<report_id>\d+) len=(?P<len>\d+) .* t=(?P<t>\d+) "
    r"data=(?P<data>(?:[0-9a-fA-F]{2}\s*)+)"
)
CAPTURE_DATA_RE = re.compile(
    r"SWITCH2 CAPTURE_DATA (?P<index>\d+) offset=(?P<offset>\d+) data=(?P<data>(?:[0-9a-fA-F]{2}\s*)+)"
)
REPLAY_RE = re.compile(
    r"SWITCH2 REPLAY seq=(?P<seq>\d+) source=(?P<source>\w+) iface=(?P<iface>\d+) "
    r"report_id=(?P<report_id>\d+) len=(?P<len>\d+)"
)
REPLAY_DATA_RE = re.compile(
    r"SWITCH2 REPLAY_DATA seq=(?P<seq>\d+) offset=(?P<offset>\d+) data=(?P<data>(?:[0-9a-fA-F]{2}\s*)+)"
)


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
        candidates = list(hid.enumerate())

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
        raise RuntimeError("No matching HID gamepad interface found. Use --list, then pass --vid/--pid, --path, or --product-contains.")
    return matches[0]


def list_devices():
    for device in sorted(hid.enumerate(), key=lambda d: (d.get("vendor_id") or 0, d.get("product_id") or 0, d.get("usage_page") or 0)):
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
    print(f"Opening {device_info.get('product_string') or 'HID device'} path={device_info['path']!r}")
    if hasattr(hid, "Device"):
        return hid.Device(path=device_info["path"])

    device = hid.device()
    device.open_path(device_info["path"])
    return device


def parse_capture_log(path, include_replay=False):
    records = []
    latest_by_index = {}
    latest_by_seq = {}

    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if include_replay:
                match = REPLAY_RE.search(line)
                if match:
                    record = {
                        "source": match.group("source"),
                        "iface": int(match.group("iface")),
                        "report_id": int(match.group("report_id")),
                        "len": int(match.group("len")),
                        "t": 0,
                        "data": bytearray(),
                    }
                    latest_by_seq[int(match.group("seq"))] = record
                    records.append(record)
                    continue

                match = REPLAY_DATA_RE.search(line)
                if match:
                    record = latest_by_seq.get(int(match.group("seq")))
                    if record is None:
                        continue
                    offset = int(match.group("offset"))
                    chunk = bytes(int(part, 16) for part in match.group("data").split())
                    if len(record["data"]) < offset:
                        record["data"].extend(bytes(offset - len(record["data"])))
                    if len(record["data"]) == offset:
                        record["data"].extend(chunk)
                    else:
                        record["data"][offset:offset + len(chunk)] = chunk
                    continue

            match = CAPTURE_RE.search(line)
            if match:
                record = {
                    "source": match.group("source"),
                    "iface": int(match.group("iface")),
                    "report_id": int(match.group("report_id")),
                    "len": int(match.group("len")),
                    "t": int(match.group("t")),
                    "data": bytearray(),
                }
                latest_by_index[int(match.group("index"))] = record
                records.append(record)
                compact_match = CAPTURE_COMPACT_RE.search(line)
                if compact_match:
                    record["data"].extend(bytes(int(part, 16) for part in compact_match.group("data").split()))
                continue

            match = CAPTURE_DATA_RE.search(line)
            if not match:
                continue
            record = latest_by_index.get(int(match.group("index")))
            if record is None:
                continue
            offset = int(match.group("offset"))
            chunk = bytes(int(part, 16) for part in match.group("data").split())
            if len(record["data"]) < offset:
                record["data"].extend(bytes(offset - len(record["data"])))
            if len(record["data"]) == offset:
                record["data"].extend(chunk)
            else:
                record["data"][offset:offset + len(chunk)] = chunk

    for record in records:
        record["data"] = bytes(record["data"][:record["len"]])

    return records


def read_once(device, timeout_ms):
    if hasattr(device, "read"):
        try:
            return bytes(device.read(REPORT_LEN, timeout_ms=timeout_ms))
        except TypeError:
            return bytes(device.read(REPORT_LEN, timeout_ms))
    if hasattr(device, "read_timeout"):
        return bytes(device.read_timeout(REPORT_LEN, timeout_ms))
    return bytes(device.read(REPORT_LEN))


def drain(device, seconds):
    end_at = time.time() + seconds
    responses = []
    while time.time() < end_at:
        data = read_once(device, 50)
        if data:
            responses.append(data)
            print(f"< report=0x{data[0]:02x} len={len(data)} data={data[1:17].hex(' ')}")
    return responses


def write_output(device, report_id, payload):
    payload = bytes(payload)
    if len(payload) > OUT_PAYLOAD_LEN:
        raise ValueError(f"payload too large for report 0x{report_id:02x}: {len(payload)}")
    report = bytes([report_id]) + payload + bytes(OUT_PAYLOAD_LEN - len(payload))
    written = device.write(report)
    if written <= 0:
        raise RuntimeError(f"HID write failed for report 0x{report_id:02x}")
    print(f"> report=0x{report_id:02x} len={len(payload)} data={payload[:16].hex(' ')}")


def replay(device, records, sources, read_seconds, delay_seconds):
    transcript = []
    wanted_sources = set(sources)
    commands = [
        record
        for record in records
        if record["iface"] == 0 and record["source"] in wanted_sources and record["len"] > 0
    ]

    print(f"Replaying {len(commands)} commands from sources: {', '.join(sources)}")
    drain(device, 0.25)

    for index, command in enumerate(commands):
        write_output(device, command["report_id"], command["data"])
        responses = drain(device, read_seconds)
        transcript.append({
            "index": index,
            "source": command["source"],
            "report_id": command["report_id"],
            "len": command["len"],
            "data": command["data"].hex(" "),
            "responses": [
                {
                    "report_id": response[0] if response else 0,
                    "len": len(response),
                    "data": response.hex(" "),
                }
                for response in responses
            ],
        })
        if delay_seconds:
            time.sleep(delay_seconds)

    return transcript


def main():
    parser = argparse.ArgumentParser(description="Replay Switch 2 output reports captured from HID Remapper logs into another HID device.")
    parser.add_argument("--list", action="store_true", help="list HID devices and exit")
    parser.add_argument("--log", default="switch2.log", help="serial log containing SWITCH2 CAPTURE / SWITCH2 CAPTURE_DATA lines")
    parser.add_argument("--out", default="switch2-mayflash-replay.json", help="JSON replay transcript output")
    parser.add_argument("--sources", default="control_queued,interrupt_queued", help="comma-separated capture sources to replay")
    parser.add_argument("--read-seconds", type=float, default=0.2, help="seconds to read responses after each write")
    parser.add_argument("--delay-seconds", type=float, default=0.02, help="delay after each command")
    parser.add_argument("--include-replay", action="store_true", help="also replay SWITCH2 REPLAY lines emitted by firmware self-tests")
    parser.add_argument("--vid", type=parse_int, help="target vendor ID")
    parser.add_argument("--pid", type=parse_int, help="target product ID")
    parser.add_argument("--path", help="exact HID path from --list")
    parser.add_argument("--product-contains", default="mayflash", help="target product string filter; 'mayflash' also matches MAGIC-NS2")
    args = parser.parse_args()

    if args.list:
        list_devices()
        return
    if (args.vid is None) != (args.pid is None):
        raise RuntimeError("--vid and --pid must be provided together")

    records = parse_capture_log(args.log, args.include_replay)
    if not records:
        raise RuntimeError(f"No full capture records found in {args.log}. Flash the logging build and capture a fresh serial log first.")

    device = open_device(args)
    try:
        transcript = replay(
            device,
            records,
            [source.strip() for source in args.sources.split(",") if source.strip()],
            args.read_seconds,
            args.delay_seconds,
        )
    finally:
        close = getattr(device, "close", None)
        if close:
            close()

    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(transcript, f, indent=2)
    print(f"Wrote {args.out}")


if __name__ == "__main__":
    main()
