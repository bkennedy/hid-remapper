# How this was captured (and how to capture more)

## Hardware

- A GreatFET One (https://greatscottgadgets.com/greatfet/one/), which has
  **two USB ports**.
- A Mac running the capture script.
- A real Switch 2 Pro Controller.
- A real Switch 2 console.

## Topology

```
Real Pro Controller --USB--> Mac (runs the capture script)
Mac                  --USB--> GreatFET's first/control port
GreatFET's second port --USB--> Switch 2 console
```

The Mac is the "control host" in Facedancer's USBProxy terminology: it has
the real controller plugged in directly (and queries it like a normal USB
host via pyusb/libusb), and it also controls the GreatFET over USB. The
GreatFET's *other* port is what's physically connected to the console, and is
the only thing the console ever talks to — it sees an emulated device that
the Mac-side script is transparently proxying to/from the real controller,
logging everything in both directions along the way.

## Software setup

```bash
python3 -m venv ~/greatfet-venv
source ~/greatfet-venv/bin/activate
pip install greatfet facedancer
greatfet_firmware --autoflash   # flash GreatFET firmware matching the installed host tools
```

## Running the proxy

The actual capture script is `../../proxy.py` (relative to this file:
`switch2-usb-re/proxy.py`). Run it as root (macOS requires this to claim the
real controller from the OS):

```bash
sudo ~/greatfet-venv/bin/python3 proxy.py
```

It logs a human-readable trace to stdout and a flat hex transcript to
`switch2-usb-re/captures/<timestamp>.log`, one line per transfer:

```
[HH:MM:SS.ffffff] CTRL IN  req=<...> len=N <hex>
[HH:MM:SS.ffffff] EP IN  0x81 len=N <hex>
[HH:MM:SS.ffffff] EP OUT 0x02 len=N <hex>
```

The script contains three monkeypatches that were necessary to get this
working at all, with comments explaining why — see `proxy.py` directly. In
short:
1. Facedancer's descriptor parser crashes on this controller's Interface
   Association Descriptor without a fix.
2. The GreatFET's IN-endpoint buffer can transiently overrun under load
   (ENOSPC) — patched to drop the packet and continue rather than crash.
3. Facedancer's NAK handler crashes if a NAK arrives before
   `SET_CONFIGURATION` has completed — patched to ignore until configured.
4. The whole `main(proxy)` call is wrapped in a restart loop that releases
   the GreatFET handle and waits 2s before retrying on any crash, so a long
   capture session survives transient errors instead of needing manual
   restarts (note: restarting too fast without releasing the handle causes
   `OSError: timed out trying to claim access to a libgreat device!`).

## Known gotchas encountered during this session

- **The console may silently prefer Bluetooth** over the wired connection if
  it has previously paired with the controller over BT. If captures show
  almost no traffic after initial enumeration, check whether the console
  switched to BT — you may need to "forget" the controller in the console's
  Bluetooth settings first to force it to use the wired path.
- **macOS HID driver contention**: the controller's interface 0 is HID-class,
  so macOS's `IOHIDFamily` driver will try to attach. Check
  `ioreg -p IOUSB -l -w0 | grep -A30 "Switch 2 Pro Controller"` for
  `UsbExclusiveOwner` — it should show your Python process, not a system HID
  service, confirming your script (not the OS) holds the device.
- **The controller can intermittently vanish from the Mac's USB tree**
  (`ioreg` stops showing it) if left idle/unconfigured for a while. If a
  capture goes quiet with the process still running, check
  `ioreg -p IOUSB -l -w0 | grep -i "USB Product Name"` for both the
  controller and the GreatFET, and physically reseat the controller's cable
  if it's missing.
- **A standalone static-emulation script** (no real controller needed) is
  useful for isolating whether the GreatFET<->console physical link itself
  works, independent of proxy logic — build a `facedancer.device.USBDevice`
  from the captured device descriptor bytes and a `USBConfiguration` from the
  captured config descriptor bytes (reusing the IAD parser patch above), then
  call `facedancer.main(device)`. If the console reaches `SET_CONFIGURATION`
  against this static device, the physical link is confirmed working and any
  remaining connection issues are in the real proxy/forwarding logic, not the
  hardware.

## What's needed to extend this capture

- **Per-button isolation**: capture while pressing exactly one button at a
  time (with pauses), to cleanly diff `EP 0x81 IN` packets immediately before
  and after each press and assign bit positions confidently.
- **Stick sweep**: capture while slowly moving one stick through its full
  range, one axis at a time, to determine encoding (8-bit vs 12-bit packed).
- **IMU correlation**: capture while holding the controller still vs.
  rotating/shaking it, to identify which bytes correspond to gyro vs accel
  and their scale/sign conventions.
- **Rumble disambiguation**: capture `EP 0x01 OUT` in isolation around a known
  rumble trigger (e.g. the console's connect-rumble) to confirm which of the
  two conflicting sample patterns documented in `protocol-details.md` is the
  real rumble encoding.

## Wake-from-sleep experiment script

`wake_test.py` (copied here from `switch2-usb-re/wake_test.py`) builds a pure
GreatFET emulation (no real controller) that answers the vendor bulk
handshake with canned responses and cycles through 8 candidate button-bit
patterns every 4 seconds, printing which one is active. It was used to test
whether USB-data-level signals could wake a sleeping console — they can't,
because wake-from-sleep is a BLE radio mechanism, not USB at all (even when
wired). See "Wake-from-sleep — it's BLE, not a USB signal at all" in
`../SKILL.md` for the full explanation. If you want to pursue replicating
wake yourself, you'd need a BLE radio in the loop (e.g. sniffing or
transmitting the actual wake advertising packet) — this is a different
project than the USB MITM setup here.

## Converting captures to Wireshark/pcap

`captures_to_pcap.py` (copied here from `switch2-usb-re/captures_to_pcap.py`)
converts one of our flat hex `captures/*.log` files into a classic pcap using
the Linux usbmon-mmapped link-layer type (220), which Wireshark's built-in USB
dissector reads natively:

```bash
python3 captures_to_pcap.py captures/20260616-113304.log
# writes captures/20260616-113304.pcap
```

Validated against real Wireshark/tshark (not just hand-checked): it correctly
identifies transfer type, endpoint number/direction, and data length for
every entry. Useful filters once opened: `usb.transfer_type==0x3` (bulk),
`usb.endpoint_number.endpoint==1` combined with direction, "follow stream",
and byte-level inspection of any transfer.

Known limitations (this transforms existing data for better tooling, it does
NOT add new information):
- Our log stores the parsed/repr'd control request, not the raw 8-byte SETUP
  packet, so CTRL entries get a zeroed setup field in the pcap — Wireshark
  shows them as control transfers but won't decode bRequest/wValue/wIndex
  for those specific entries.
- Every entry is a standalone "Complete" event (no paired "Submit"), so
  Wireshark won't show a computed transfer duration.
- The log has no date, only time-of-day; the script fabricates today's date,
  which is correct for ordering/deltas within one session but would break if
  a capture session spanned midnight.
