#!/usr/bin/env python3
"""Pull and decode the Switch 2 flight log from the board.

Usage:
    python3 pull_switch2_flight_log.py

The flight log is a 64-entry circular buffer stored in RAM. Each entry records
a timestamped event (boot, BLE connect/disconnect, USB commands, etc.).
"""

import hid
import struct
import binascii
import time

REPORT_ID_CONFIG = 100
CONFIG_VERSION = 18
CONFIG_SIZE = 32
GET_SWITCH2_FLIGHT_LOG = 27
PAGE_SIZE = 28

FLIGHT_MAGIC = 0x32574648  # "HFW2"
FLIGHT_VERSION = 1
FLIGHT_EVENTS = 64
EVENT_SIZE = 20  # ms(4) + seq(2) + event(1) + len(1) + a(1) + b(1) + c(1) + d(1) + data(8)
HEADER_SIZE = 12  # magic(4) + version(2) + next_seq(2) + head(1) + wrapped(1) + reserved(2)
STRUCT_SIZE = HEADER_SIZE + FLIGHT_EVENTS * EVENT_SIZE  # 1292 bytes

EVENT_NAMES = {
    1: "boot",
    2: "usb_status",
    3: "set_report",
    4: "get_report",
    5: "int_out",
    6: "host_cmd",
    7: "queue_response",
    8: "send_response",
    9: "send_input",
    10: "input_enable",
    11: "ble_input",
    12: "ble_init",
    13: "rumble",
    14: "config_set",
    15: "bt_connect",
    16: "bt_disconnect",
    17: "bt_security",
    18: "bt_pairing",
    19: "scan",
    20: "bond_keys",
}


def add_crc(buf):
    return buf + struct.pack("<L", binascii.crc32(buf[1:]))


def send_command(dev, cmd, index):
    data = struct.pack("<BBL22x", CONFIG_VERSION, cmd, index)
    dev.send_feature_report(add_crc(bytes([REPORT_ID_CONFIG]) + data))


def read_response(dev):
    for _ in range(15):
        time.sleep(0.05)
        resp = bytes(dev.get_feature_report(REPORT_ID_CONFIG, CONFIG_SIZE + 1))
        if len(resp) < CONFIG_SIZE + 1:
            continue
        payload = resp[1:CONFIG_SIZE + 1]
        stored_crc = struct.unpack_from("<L", payload, 28)[0]
        if binascii.crc32(payload[0:28]) != stored_crc:
            raise Exception("CRC mismatch")
        return payload[0:28]
    raise Exception("No valid response after retries")


def open_device():
    devices = [
        d for d in hid.enumerate()
        if d["usage_page"] == 0xFF00 and d["usage"] == 0x0020
    ]
    if not devices:
        raise Exception("No HID Remapper devices found.")
    dev = hid.device()
    dev.open_path(devices[0]["path"])
    return dev


def pull_flight_log():
    dev = open_device()

    needed_pages = (STRUCT_SIZE + PAGE_SIZE - 1) // PAGE_SIZE
    raw = bytearray()

    for page in range(needed_pages):
        send_command(dev, GET_SWITCH2_FLIGHT_LOG, page)
        chunk = read_response(dev)
        raw.extend(chunk)

    dev.close()
    raw = raw[:STRUCT_SIZE]

    magic, version, next_seq, head, wrapped = struct.unpack_from("<LHHBB", raw, 0)
    print(f"Header: magic={magic:#010x} version={version} next_seq={next_seq} head={head} wrapped={wrapped}")

    if magic != FLIGHT_MAGIC:
        print(f"Bad magic (expected {FLIGHT_MAGIC:#010x}). Is the board in Switch 2 Pro mode?")
        return

    count = FLIGHT_EVENTS if wrapped else head
    if count == 0:
        print("Flight log is empty.")
        return

    print(f"\n{'#':>4}  {'ms':>8}  {'seq':>5}  {'event':<16}  {'a':>3} {'b':>3} {'c':>3} {'d':>3}  data")
    print("-" * 80)

    boot_ms = None
    for i in range(count):
        idx = (head - count + i) % FLIGHT_EVENTS
        offset = HEADER_SIZE + idx * EVENT_SIZE
        ms, seq, event, length, a, b, c, d = struct.unpack_from("<LHBBBBBB", raw, offset)
        data = raw[offset + 12: offset + 12 + 8]
        data_hex = data[:length].hex() if length > 0 else ""

        if boot_ms is None:
            boot_ms = ms
        rel_ms = ms - boot_ms

        name = EVENT_NAMES.get(event, f"event_{event}")
        print(f"{i:>4}  {rel_ms:>8}  {seq:>5}  {name:<16}  {a:>3} {b:>3} {c:>3} {d:>3}  {data_hex}")


if __name__ == "__main__":
    pull_flight_log()
