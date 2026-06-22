#!/usr/bin/env python3
"""
Switch 2 Pro USB auth relay.

Setup:
  1. Connect real Switch 2 Pro to Mac via USB cable.
  2. Connect DK J3 to Switch 2 console, J2 to Mac (serial).
  3. Flash DK with relay firmware (iter30+).
  4. Run this script BEFORE navigating to Change Grip/Order on console.
  5. Navigate to Change Grip/Order on the console.

Flow:
  DK prints AUTH01:<hex>  → we forward 0x15:0x01 to real SW2 Pro
  DK prints AUTH02:<hex>  → we forward 0x15:0x02 to real SW2 Pro, read response,
                            write R02:<hex> back to DK serial
  DK reads R02: → completes crypto → console sees successful auth → starts polling

The relay bridges the nRF52840 DK (as USB fake controller to Switch 2 console)
with the real Switch 2 Pro controller (as USB oracle for the challenge-response crypto).
"""

import serial
import sys
import time

# Try to import usb.core; tell user if missing
try:
    import usb.core
    import usb.util
except ImportError:
    print("ERROR: pyusb not installed. Run: pip3 install pyusb")
    sys.exit(1)

DK_PORT = '/dev/tty.usbmodem0010502748531'
DK_BAUD = 115200
SW2_VID = 0x057e
SW2_PID = 0x2069
BULK_OUT_EP = 0x02
BULK_IN_EP = 0x82
TIMEOUT_MS = 3000

def find_sw2_pro():
    dev = usb.core.find(idVendor=SW2_VID, idProduct=SW2_PID)
    if dev is None:
        print("ERROR: Real Switch 2 Pro not found via USB (VID=0x057e PID=0x2069)")
        print("  Make sure the real controller is connected to Mac via USB (not J-Link cable)")
        return None
    print(f"Found Switch 2 Pro: bus={dev.bus} addr={dev.address}")
    return dev

def claim_vendor_interface(dev):
    """Claim interface 1 (vendor BULK) on the real SW2 Pro."""
    try:
        dev.set_configuration()
    except usb.core.USBError:
        pass
    intf = None
    for cfg in dev:
        for i in cfg:
            if i.bInterfaceClass == 0xff:  # vendor
                intf = i
                break
    if intf is None:
        # Try interface 1 directly
        try:
            cfg = dev.get_active_configuration()
            intf = cfg[(1, 0)]
        except Exception as e:
            print(f"ERROR: Can't find vendor interface: {e}")
            return False
    try:
        if dev.is_kernel_driver_active(intf.bInterfaceNumber):
            dev.detach_kernel_driver(intf.bInterfaceNumber)
        usb.util.claim_interface(dev, intf.bInterfaceNumber)
        print(f"Claimed interface {intf.bInterfaceNumber}")
        return True
    except Exception as e:
        print(f"WARNING: Could not claim vendor interface: {e}")
        return False

def send_bulk_out(dev, data: bytes):
    """Send bytes to real SW2 Pro BULK OUT EP 0x02."""
    dev.write(BULK_OUT_EP, data, timeout=TIMEOUT_MS)

def recv_bulk_in(dev, length=25) -> bytes:
    """Read from real SW2 Pro BULK IN EP 0x82."""
    return bytes(dev.read(BULK_IN_EP, length, timeout=TIMEOUT_MS))

def forward_auth01(dev, auth01_hex: str):
    """Forward the 0x15:0x01 packet to the real SW2 Pro."""
    pkt = bytes.fromhex(auth01_hex)
    print(f"  → AUTH01 to SW2 Pro ({len(pkt)} bytes): {pkt.hex()}")
    try:
        send_bulk_out(dev, pkt)
        resp = recv_bulk_in(dev, 25)
        print(f"  ← AUTH01 resp from SW2 Pro: {resp.hex()}")
    except Exception as e:
        print(f"  WARNING: AUTH01 relay error: {e}")

def forward_auth04(dev):
    """Send a 0x15:0x04 placeholder to SW2 Pro to seed its crypto state.
    The challenge bytes don't matter much since we're just priming state;
    the response from real controller will be the constant cert bytes."""
    pkt = bytes.fromhex("1591000400110000000000000000000000000000000000000000000000")
    print(f"  → AUTH04 seed to SW2 Pro ({len(pkt)} bytes)")
    try:
        send_bulk_out(dev, pkt)
        resp = recv_bulk_in(dev, 25)
        print(f"  ← AUTH04 resp from SW2 Pro: {resp.hex()}")
    except Exception as e:
        print(f"  WARNING: AUTH04 relay error: {e}")

def relay_auth02(dev, auth02_hex: str) -> str | None:
    """Send 0x15:0x02 challenge to real SW2 Pro, return 32-char hex response body."""
    challenge = bytes.fromhex(auth02_hex)  # 16 bytes
    # Build 0x91 packet: header(8) + 0x00 leading byte + 16-byte challenge
    pkt = bytes([0x15, 0x91, 0x00, 0x02, 0x00, 0x11, 0x00, 0x00, 0x00]) + challenge
    print(f"  → AUTH02 to SW2 Pro ({len(pkt)} bytes): {pkt.hex()}")
    try:
        send_bulk_out(dev, pkt)
        resp = recv_bulk_in(dev, 25)
        print(f"  ← AUTH02 resp from SW2 Pro ({len(resp)} bytes): {resp.hex()}")
        if len(resp) >= 25 and resp[8] == 0x01:
            return resp[9:25].hex()
        else:
            print(f"  WARNING: unexpected response")
            return None
    except Exception as e:
        print(f"  ERROR: AUTH02 relay failed: {e}")
        return None

def main():
    print("=== Switch 2 Pro Auth Relay ===")
    print(f"DK serial: {DK_PORT}")

    dev = find_sw2_pro()
    if dev is None:
        sys.exit(1)
    claim_vendor_interface(dev)

    try:
        dk = serial.Serial(DK_PORT, DK_BAUD, timeout=0.1)
    except Exception as e:
        print(f"ERROR: Cannot open DK serial port {DK_PORT}: {e}")
        sys.exit(1)

    print("Waiting for DK to start auth sequence... (navigate to Change Grip/Order)")
    auth04_sent = False

    while True:
        line_bytes = dk.readline()
        if not line_bytes:
            continue
        line = line_bytes.decode('utf-8', errors='replace').strip()
        if not line:
            continue

        # Pass through to console for visibility
        print(f"DK: {line}")

        if line.startswith("AUTH01:"):
            auth01_hex = line[7:].strip()
            forward_auth01(dev, auth01_hex)
            # Also send AUTH04 immediately after AUTH01 so real controller is primed
            if not auth04_sent:
                forward_auth04(dev)
                auth04_sent = True

        elif line.startswith("AUTH02:"):
            auth02_hex = line[7:].strip()
            resp_hex = relay_auth02(dev, auth02_hex)
            if resp_hex:
                response_line = f"R02:{resp_hex}\n"
                print(f"  → writing to DK: R02:{resp_hex}")
                dk.write(response_line.encode('utf-8'))
                dk.flush()
            else:
                print("  WARNING: no valid AUTH02 response, relay failed")
            auth04_sent = False  # reset for next auth cycle

        elif "18 91 00 01" in line:
            print("  [0x18:0x01 seen — auth done, polling should start in ~6s]")

        elif "0a 91 00 02" in line:
            print("  *** POLLING STARTED! 0x0a:0x02 received ***")

if __name__ == '__main__':
    main()
