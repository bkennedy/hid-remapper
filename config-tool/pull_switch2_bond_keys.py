#!/usr/bin/env python3
"""Pull BLE bond key data from the HID Remapper Bluetooth board.

Usage:
    python3 pull_switch2_bond_keys.py

The board must have already paired with the Switch 2 Pro Controller.

Struct layout (switch2_bond_keys_snapshot, paginated at 28 bytes/page):
  offset  0  uint32_t magic       (0x324b4253 = "SBK2")
  offset  4  uint16_t version
  offset  6  uint16_t total_len   bytes of data[] used
  offset  8  uint8_t  record_count
  offset  9  uint8_t  truncated
  offset 10  uint8_t  reserved[2]
  offset 12  uint8_t  data[]      records

Each record: name_len[1] + value_len[2 LE] + name[name_len] + value[value_len]

For LE Secure Connections the LTK is stored as rand[8]=0 + ediv[2]=0 + val[16].
"""

import hid
import struct
import binascii
import sys
import time

REPORT_ID_CONFIG = 100
CONFIG_VERSION = 18
CONFIG_SIZE = 32
GET_SWITCH2_BOND_KEYS = 28
PAGE_SIZE = 28
STRUCT_HEADER_SIZE = 12
BOND_KEYS_MAGIC = 0x324B4253


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
        payload = resp[1 : CONFIG_SIZE + 1]
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
    # Try each interface — pick the first that responds
    for info in devices:
        dev = hid.device()
        dev.open_path(info["path"])
        try:
            send_command(dev, GET_SWITCH2_BOND_KEYS, 0)
            page0 = read_response(dev)
            magic = struct.unpack_from("<L", page0, 0)[0]
            if magic == BOND_KEYS_MAGIC:
                return dev, page0
        except Exception:
            pass
        dev.close()
    raise Exception("No device responded with valid bond key data.")


def pull_bond_keys():
    dev, page0 = open_device()

    magic = struct.unpack_from("<L", page0, 0)[0]
    version = struct.unpack_from("<H", page0, 4)[0]
    total_len = struct.unpack_from("<H", page0, 6)[0]
    record_count = page0[8]
    truncated = page0[9]

    print(f"Snapshot: magic={magic:#010x} version={version} total_len={total_len} "
          f"records={record_count} truncated={truncated}")

    if total_len == 0:
        print("No bond key data (total_len=0). Pair the controller first.")
        dev.close()
        return

    # Collect enough pages to cover the full data
    needed_bytes = STRUCT_HEADER_SIZE + total_len
    needed_pages = (needed_bytes + PAGE_SIZE - 1) // PAGE_SIZE
    raw = bytearray(page0)

    for page in range(1, needed_pages):
        send_command(dev, GET_SWITCH2_BOND_KEYS, page)
        raw.extend(read_response(dev))

    dev.close()

    data = raw[STRUCT_HEADER_SIZE : STRUCT_HEADER_SIZE + total_len]

    print(f"\nRaw data ({len(data)} bytes):")
    for i in range(0, len(data), 16):
        print(f"  {i:04x}: {data[i:i+16].hex(' ')}")

    # Parse records
    print("\nRecords:")
    pos = 0
    while pos + 3 <= len(data):
        name_len = data[pos]
        value_len = struct.unpack_from("<H", data, pos + 1)[0]
        pos += 3
        if pos + name_len + value_len > len(data):
            print("  (truncated record)")
            break
        name = data[pos : pos + name_len].decode("ascii", errors="replace")
        value = bytes(data[pos + name_len : pos + name_len + value_len])
        pos += name_len + value_len

        print(f"\n  key: bt/keys/{name}  ({value_len} bytes)")
        for i in range(0, len(value), 16):
            print(f"    {i:04x}: {value[i:i+16].hex(' ')}")

        # LE SC LTK: rand[8]=0 + ediv[2]=0 + val[16]
        best_ltk = None
        for off in range(len(value) - 25):
            if all(b == 0 for b in value[off : off + 10]):
                ltk = value[off + 10 : off + 26]
                # Skip if first byte is 0 (likely an off-by-one false match)
                # Skip if all-zero or all-0xFF padding
                if ltk[0] != 0 and any(b != 0 for b in ltk) and not all(b == 0 or b == 0xFF for b in ltk):
                    if best_ltk is None:
                        best_ltk = (off, ltk)
        if best_ltk:
            off, ltk = best_ltk
            print(f"\n  LTK (LE SC) at value+{off + 10}: {ltk.hex()}")
        else:
            print("  No LTK candidate found.")


if __name__ == "__main__":
    pull_bond_keys()
