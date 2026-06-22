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
import threading
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
    devs = list(usb.core.find(idVendor=SW2_VID, idProduct=SW2_PID, find_all=True))
    if not devs:
        print("ERROR: No Switch 2 Pro found via USB (VID=0x057e PID=0x2069)")
        print("  Make sure: real SW2 Pro plugged into Mac, J3 plugged into console (not Mac)")
        return None
    for d in devs:
        try: sn = d.serial_number
        except: sn = '?'
        print(f"  Found bus={d.bus} addr={d.address} sn={sn!r}")
    if len(devs) == 1:
        # Only one device: if J3 is in the console the DK is invisible to Mac,
        # so this must be the real controller. (On macOS IOHIDManager prevents
        # reading the real serial number, so sn reads as '00' — don't filter on it.)
        d = devs[0]
        print(f"Using single device: bus={d.bus} addr={d.address}")
        return d
    # Multiple devices: DK is still on Mac USB (J3 not in console yet).
    # Try to pick the non-DK by serial; real SW2 Pro serial is 'HEW70006169780'.
    for d in devs:
        try: sn = d.serial_number
        except: sn = None
        if sn and sn not in ('00', ''):
            print(f"Found real Switch 2 Pro: bus={d.bus} addr={d.address} sn={sn!r}")
            return d
    print("ERROR: Multiple 057E:2069 devices but can't identify real SW2 Pro.")
    print("  Plug J3 into the Switch 2 console so the DK is not visible to Mac.")
    return None

def claim_vendor_interface(dev):
    """Claim interface 1 (vendor BULK) on the real SW2 Pro.
    Don't call set_configuration() — it triggers re-enumeration and provokes IOHIDManager."""
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

# Lock protecting all vendor-bulk USB access to the real SW2 Pro.
# Keepalive thread and auth functions both hold this around each transaction.
_usb_lock = threading.Lock()
# Global device ref — updated on reconnect so all callers see the fresh handle.
_dev_ref = [None]

def _reconnect() -> bool:
    """Re-find and re-claim the real SW2 Pro after an [Errno 19] / stale handle.
    Called with _usb_lock already held.  Retries up to 5s for re-enumeration.
    Returns True on success."""
    print("  [reconnect] searching for real SW2 Pro (5 attempts)...")
    for attempt in range(5):
        dev = find_sw2_pro()
        if dev is not None:
            claim_vendor_interface(dev)
            _dev_ref[0] = dev
            print(f"  [reconnect] success on attempt {attempt+1}")
            return True
        if attempt < 4:
            print(f"  [reconnect] not found yet, waiting 1s...")
            time.sleep(1.0)
    print("  [reconnect] FAILED — real SW2 Pro not found after 5 attempts")
    return False

def _bulk_transact(out_data: bytes, in_len: int, timeout_ms=TIMEOUT_MS) -> bytes:
    """Single locked write+read pair with auto-reconnect on ENODEV."""
    with _usb_lock:
        for attempt in range(2):
            dev = _dev_ref[0]
            try:
                dev.write(BULK_OUT_EP, out_data, timeout=timeout_ms)
                try:
                    return bytes(dev.read(BULK_IN_EP, in_len, timeout=timeout_ms))
                except usb.core.USBTimeoutError:
                    return b''
            except usb.core.USBError as e:
                if e.errno == 19 and attempt == 0:  # ENODEV — handle went stale
                    print(f"  [Errno 19] stale handle, reconnecting...")
                    if not _reconnect():
                        raise
                    continue  # retry with fresh handle
                raise

def _start_keepalive():
    """Background thread: send 0x02 idle keepalive every 2s to prevent sleep."""
    seq = [0]
    def _run():
        while True:
            time.sleep(2.0)
            pkt = bytes([seq[0] & 0xff, 0x91, 0x00, 0x02, 0x00, 0x04,
                         0x00, 0x00, 0x00, 0x00, 0x00, 0x00])
            seq[0] = (seq[0] + 1) & 0xff
            try:
                _bulk_transact(pkt, 25, timeout_ms=500)
            except Exception:
                pass  # don't crash the thread on transient errors
    t = threading.Thread(target=_run, daemon=True)
    t.start()
    return t

def forward_auth01(auth01_hex: str):
    """Forward the 0x15:0x01 packet to the real SW2 Pro."""
    pkt = bytes.fromhex(auth01_hex)
    print(f"  → AUTH01 to SW2 Pro ({len(pkt)} bytes): {pkt.hex()}")
    try:
        resp = _bulk_transact(pkt, 25)
        print(f"  ← AUTH01 resp from SW2 Pro: {resp.hex()}")
    except Exception as e:
        print(f"  WARNING: AUTH01 relay error: {e}")

def forward_auth04():
    """Send a 0x15:0x04 placeholder to SW2 Pro to seed its crypto state."""
    pkt = bytes.fromhex("15910004001100000000000000000000000000000000000000")
    print(f"  → AUTH04 seed to SW2 Pro ({len(pkt)} bytes)")
    try:
        resp = _bulk_transact(pkt, 25)
        print(f"  ← AUTH04 resp from SW2 Pro: {resp.hex()}")
    except Exception as e:
        print(f"  WARNING: AUTH04 relay error: {e}")

def relay_auth02(auth02_hex: str) -> str | None:
    """Send 0x15:0x02 challenge to real SW2 Pro, return 32-char hex response body."""
    challenge = bytes.fromhex(auth02_hex)  # 16 bytes
    # Build 0x91 packet: header(8) + 0x00 leading byte + 16-byte challenge
    pkt = bytes([0x15, 0x91, 0x00, 0x02, 0x00, 0x11, 0x00, 0x00, 0x00]) + challenge
    print(f"  → AUTH02 to SW2 Pro ({len(pkt)} bytes): {pkt.hex()}")
    try:
        resp = _bulk_transact(pkt, 25)
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
    _dev_ref[0] = dev  # set global so _bulk_transact and reconnect can find it
    _start_keepalive()
    print("Keepalive started (0x02 ping every 2s to prevent real controller sleep)")

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
            forward_auth01(auth01_hex)
            # Also send AUTH04 immediately after AUTH01 so real controller is primed
            if not auth04_sent:
                forward_auth04()
                auth04_sent = True

        elif line.startswith("AUTH02:"):
            auth02_hex = line[7:].strip()
            resp_hex = relay_auth02(auth02_hex)
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
