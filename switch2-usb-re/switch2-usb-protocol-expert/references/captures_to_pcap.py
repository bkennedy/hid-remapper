#!/usr/bin/env python3
"""
Convert one of our flat hex capture logs (captures/*.log, produced by
proxy.py's RawHexLogFilter) into a classic pcap file using the Linux
usbmon-mmapped link-layer type (220), which Wireshark's built-in USB
dissector understands natively.

This lets you open the existing capture in Wireshark for filtering
(usb.endpoint_address, usb.transfer_type), "follow stream", and byte-level
inspection -- it does NOT add any new data beyond what's already in the log.

Known limitations (documented so nobody mistakes this for more than it is):
  - Our log only stores the parsed/repr'd control request, not the raw
    8-byte SETUP packet, so CTRL entries are emitted with a zeroed setup
    field. Wireshark will show them as control transfers but won't decode
    bRequest/wValue/wIndex correctly for those specific entries.
  - Every entry is emitted as a single "Complete" (C) event with no paired
    "Submit" (S) event, since our log doesn't record submit-vs-complete
    timing separately. Wireshark will still display these fine, just
    without a computed transfer duration.
  - The log has no date, only time-of-day with microsecond resolution; we
    fabricate an arbitrary date so relative ordering/deltas within one
    capture are correct. If a capture session crosses midnight, ordering
    will break (rare given typical session lengths).

Usage:
  python3 captures_to_pcap.py captures/20260616-113304.log
  python3 captures_to_pcap.py captures/20260616-113304.log -o out.pcap
"""

import argparse
import datetime
import re
import struct
import sys

PCAP_MAGIC = 0xA1B2C3D4
LINKTYPE_USB_LINUX_MMAPPED = 220

XFER_ISO = 0
XFER_INTERRUPT = 1
XFER_CONTROL = 2
XFER_BULK = 3

LINE_RE = re.compile(
    r"^\[(?P<ts>\d{2}:\d{2}:\d{2}\.\d{6})\] "
    r"(?:"
    r"CTRL (?P<ctrl_dir>IN|OUT)\s+req=.*?\s+len=(?P<ctrl_len>\d+)\s*(?P<ctrl_hex>[0-9a-f]*)"
    r"|"
    r"EP\s+(?P<ep_dir>IN|OUT)\s+(?P<ep_addr>0x[0-9a-f]{2})\s+len=(?P<ep_len>\d+)\s*(?P<ep_hex>[0-9a-f]*)"
    r")$"
)

# Endpoint number -> transfer type, based on this controller's known
# descriptors (see ../switch2-usb-protocol-expert/references/usb-descriptors.md).
EP_XFER_TYPE = {
    0x01: XFER_INTERRUPT, 0x81: XFER_INTERRUPT,
    0x02: XFER_BULK, 0x82: XFER_BULK,
    0x03: XFER_ISO, 0x83: XFER_ISO,
}


def parse_timestamp(ts: str, base_date: datetime.date) -> float:
    t = datetime.datetime.strptime(ts, "%H:%M:%S.%f").time()
    dt = datetime.datetime.combine(base_date, t)
    return dt.timestamp()


def build_usbmon_packet(urb_id: int, ts: float, xfer_type: int, epnum: int,
                         devnum: int, busnum: int, data: bytes,
                         setup: bytes = b"\x00" * 8) -> bytes:
    ts_sec = int(ts)
    ts_usec = int(round((ts - ts_sec) * 1_000_000))

    # Common header, offsets 0-39 (40 bytes).
    header = struct.pack(
        "<Q c B B B H B B q I I I I",
        urb_id,             # id (8 bytes)
        b"C",                # type: 'C' = Complete
        xfer_type,           # xfer_type
        epnum,                # epnum (includes direction bit)
        devnum,               # devnum
        busnum,               # busnum
        0xFF,                 # flag_setup: 0xFF = no setup data captured (see module docstring)
        0,                    # flag_data: 0 = data is present
        ts_sec,               # ts_sec
        ts_usec,              # ts_usec
        0,                    # status
        len(data),            # length (submitted/actual)
        len(data),            # len_cap (captured)
    )
    assert len(header) == 40, len(header)

    # Union at offset 40-47 (8 bytes): setup packet for control transfers,
    # or iso_rec (error_count, numdesc) for isochronous. We don't have raw
    # setup bytes captured, so this is zeroed except where the caller
    # explicitly passes one in.
    union = setup.ljust(8, b"\x00")[:8]

    # Trailer, offsets 48-63 (16 bytes): interval, start_frame, xfer_flags, ndesc.
    trailer = struct.pack("<I I I I", 0, 0, 0, 0)

    full_header = header + union + trailer
    assert len(full_header) == 64, len(full_header)
    return full_header + data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logfile")
    parser.add_argument("-o", "--output")
    parser.add_argument("--devnum", type=int, default=2)
    parser.add_argument("--busnum", type=int, default=1)
    args = parser.parse_args()

    out_path = args.output or (args.logfile.rsplit(".", 1)[0] + ".pcap")
    base_date = datetime.date.today()

    packets = []
    skipped = 0
    with open(args.logfile) as f:
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue
            m = LINE_RE.match(line)
            if not m:
                skipped += 1
                continue

            ts = parse_timestamp(m.group("ts"), base_date)

            if m.group("ctrl_len") is not None:
                xfer_type = XFER_CONTROL
                epnum = 0x80 if m.group("ctrl_dir") == "IN" else 0x00
                data = bytes.fromhex(m.group("ctrl_hex") or "")
            else:
                ep_addr = int(m.group("ep_addr"), 16)
                xfer_type = EP_XFER_TYPE.get(ep_addr & 0x7F, XFER_BULK)
                epnum = ep_addr
                data = bytes.fromhex(m.group("ep_hex") or "")

            packets.append((ts, xfer_type, epnum, data))

    if not packets:
        print(f"No parseable lines found in {args.logfile} (skipped {skipped})", file=sys.stderr)
        sys.exit(1)

    with open(out_path, "wb") as out:
        # Classic pcap global header.
        out.write(struct.pack(
            "<IHHiIII",
            PCAP_MAGIC, 2, 4, 0, 0, 65535, LINKTYPE_USB_LINUX_MMAPPED,
        ))

        for i, (ts, xfer_type, epnum, data) in enumerate(packets):
            pkt = build_usbmon_packet(
                urb_id=i + 1, ts=ts, xfer_type=xfer_type, epnum=epnum,
                devnum=args.devnum, busnum=args.busnum, data=data,
            )
            ts_sec = int(ts)
            ts_usec = int(round((ts - ts_sec) * 1_000_000))
            out.write(struct.pack("<IIII", ts_sec, ts_usec, len(pkt), len(pkt)))
            out.write(pkt)

    print(f"Wrote {len(packets)} packets to {out_path} (skipped {skipped} unparseable lines)")
    print("Open it in Wireshark; filter examples: usb.transfer_type==0x3 (bulk), "
          "usb.endpoint_address==0x81")


if __name__ == "__main__":
    main()
