#!/usr/bin/env python3
"""Pull USB-side pairing bytes captured from the Switch 2 console.

Usage:
    python3 pull_switch2_usb_pairing.py

Connect the board to the Switch 2 console (USB) while it is also paired to the
real controller (BLE). The board captures every 0x15 command the console sends
over the USB vendor interface. Run this after the console has completed its
pairing exchange with the board.

Struct layout (switch2_usb_pairing_snapshot, paginated at 28 bytes/page):
  offset  0  uint32_t magic      (0x50554b53 = "SKUP")
  offset  4  uint16_t version
  offset  6  uint16_t total_len  bytes of data[] used
  offset  8  uint8_t  cmd_count  number of 0x15 commands captured
  offset  9  uint8_t  reserved[3]
  offset 12  uint8_t  data[]     raw USB command bytes, concatenated
"""

import hid
import struct
import binascii
import time

REPORT_ID_CONFIG = 100
CONFIG_VERSION = 18
CONFIG_SIZE = 32
GET_SWITCH2_USB_PAIRING = 29
PAGE_SIZE = 28
STRUCT_HEADER_SIZE = 12
USB_PAIRING_MAGIC = 0x50554B53


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
    for info in devices:
        dev = hid.device()
        dev.open_path(info["path"])
        try:
            send_command(dev, GET_SWITCH2_USB_PAIRING, 0)
            page0 = read_response(dev)
            magic = struct.unpack_from("<L", page0, 0)[0]
            if magic == USB_PAIRING_MAGIC:
                return dev, page0
        except Exception:
            pass
        dev.close()
    raise Exception("No device responded with valid USB pairing data.")


def pull_usb_pairing():
    dev, page0 = open_device()

    magic = struct.unpack_from("<L", page0, 0)[0]
    version = struct.unpack_from("<H", page0, 4)[0]
    total_len = struct.unpack_from("<H", page0, 6)[0]
    cmd_count = page0[8]

    print(f"Snapshot: magic={magic:#010x} version={version} "
          f"total_len={total_len} cmd_count={cmd_count}")

    if total_len == 0:
        print("No USB pairing data captured.")
        print("Connect the board to a Switch 2 console and let it complete pairing.")
        dev.close()
        return

    needed_bytes = STRUCT_HEADER_SIZE + total_len
    needed_pages = (needed_bytes + PAGE_SIZE - 1) // PAGE_SIZE
    raw = bytearray(page0)

    for page in range(1, needed_pages):
        send_command(dev, GET_SWITCH2_USB_PAIRING, page)
        raw.extend(read_response(dev))

    dev.close()

    data = raw[STRUCT_HEADER_SIZE : STRUCT_HEADER_SIZE + total_len]

    print(f"\nRaw USB 0x15 command bytes ({len(data)} bytes):")
    for i in range(0, len(data), 16):
        print(f"  {i:04x}: {data[i:i+16].hex(' ')}")

    # Parse individual commands: each is a fixed-format USB vendor command.
    # Format: [cmd=0x15][0x91][seq][sub][len_lo][len_hi][00][00][00][payload...]
    print("\nParsed 0x15 commands:")
    pos = 0
    while pos < len(data):
        if pos + 9 > len(data):
            break
        cmd = data[pos]
        sub = data[pos + 3]
        payload_len = struct.unpack_from("<H", data, pos + 4)[0]
        total_cmd_len = 9 + payload_len
        if pos + total_cmd_len > len(data):
            print(f"  (truncated at offset {pos})")
            break
        payload = data[pos + 9 : pos + total_cmd_len]
        print(f"  cmd=0x{cmd:02x} sub=0x{sub:02x} payload ({payload_len} bytes): {payload.hex()}")
        pos += total_cmd_len

    # The 0x15 sub=0x01 command contains the console's BLE addresses.
    # sub=0x04 and sub=0x02 contain key exchange material.
    print("\nConsole BLE address candidates (from sub=0x01 payload):")
    pos = 0
    found = False
    while pos < len(data):
        if pos + 9 > len(data):
            break
        sub = data[pos + 3]
        payload_len = struct.unpack_from("<H", data, pos + 4)[0]
        total_cmd_len = 9 + payload_len
        if sub == 0x01 and payload_len >= 13:
            payload = data[pos + 9 : pos + 9 + payload_len]
            # Bytes 1..6 = primary address, 7..12 = secondary address
            addr1 = bytes(reversed(payload[1:7]))
            addr2 = bytes(reversed(payload[7:13]))
            print(f"  Primary:   {addr1.hex(':')}")
            print(f"  Secondary: {addr2.hex(':')}")
            found = True
        pos += total_cmd_len
    if not found:
        print("  No sub=0x01 command found yet.")


if __name__ == "__main__":
    pull_usb_pairing()
