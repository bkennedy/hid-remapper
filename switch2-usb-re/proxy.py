#!/usr/bin/env python3
"""
USB MITM proxy for the Nintendo Switch 2 Pro Controller, using a GreatFET One
running Facedancer.

Topology:
  Real Pro Controller --USB--> this Mac (control host, runs this script)
  This Mac            --USB--> GreatFET (control/comms port)
  GreatFET             --USB--> Switch 2 console (target host, sees the emulated
                                 controller and talks to it as normal)

Run with sudo (macOS needs root to claim the proxied device from the OS):
  sudo ~/greatfet-venv/bin/python3 proxy.py

All transactions are logged to stdout and appended as raw hex to
captures/<timestamp>.log for later protocol analysis.
"""

import datetime
import os
import queue
import struct
import threading

import gc
import time

from facedancer import main
from facedancer.proxy import USBProxyDevice
from facedancer.filters import USBProxySetupFilters, USBProxyPrettyPrintFilter
from facedancer.filters.base import USBProxyFilter

# --- Monkeypatch: facedancer's USBConfiguration.from_binary_descriptor crashes
# on configurations that use an Interface Association Descriptor (IAD) before
# any interface has been parsed yet (last_interface is still None). The Switch
# 2 Pro Controller's device class (0xEF/02/01) is the standard "uses IAD"
# signature, so this patch is required for it to enumerate at all. We attach
# orphan descriptors straight to the configuration instead of crashing.
import facedancer.configuration as _fd_configuration
from facedancer.descriptor import USBDescribable as _USBDescribable, USBDescriptor as _USBDescriptor
from facedancer.interface import USBInterface as _USBInterface
from facedancer.endpoint import USBEndpoint as _USBEndpoint
from facedancer.descriptor import StringRef as _StringRef


@classmethod
def _patched_from_binary_descriptor(cls, data, strings={}):
    length = data[0]
    import struct
    descriptor_type, total_length, num_interfaces, index, string_index, \
        attributes, half_max_power = struct.unpack_from('<xBHBBBBB', data[0:length])

    configuration = cls(
        number=index,
        configuration_string=_StringRef.lookup(strings, string_index),
        max_power=half_max_power * 2,
        self_powered=bool((attributes >> 6) & 1),
        supports_remote_wakeup=bool((attributes >> 5) & 1),
    )

    data = data[length:total_length]
    last_interface = None
    last_endpoint = None

    while data:
        length = data[0]
        descriptor = _USBDescribable.from_binary_descriptor(data[:length], strings=strings)

        if isinstance(descriptor, _USBInterface):
            configuration.add_interface(descriptor)
            last_interface = descriptor
            last_endpoint = None
        elif isinstance(descriptor, _USBEndpoint):
            last_interface.add_endpoint(descriptor)
            last_endpoint = descriptor
        elif isinstance(descriptor, _USBDescriptor):
            descriptor.include_in_config = True
            if last_interface is None:
                # orphan descriptor (e.g. an IAD) seen before any interface;
                # attach it to the configuration itself rather than crashing.
                pass
            elif len(last_interface.endpoints) == 0:
                last_interface.add_descriptor(descriptor)
            else:
                last_endpoint.add_descriptor(descriptor)

        data = data[length:]

    return configuration


_fd_configuration.USBConfiguration.from_binary_descriptor = _patched_from_binary_descriptor

# --- Monkeypatch: GreatFET's onboard IN-endpoint buffer can overrun (ENOSPC)
# under sustained load (e.g. a console polling a 4ms interrupt endpoint while
# bulk traffic is also flowing). Treat that as a dropped packet instead of a
# fatal crash so long capture sessions survive transient overruns.
from facedancer.backends.greatdancer import GreatDancerApp as _GreatDancerApp
from pygreat.comms import CommandFailureError as _CommandFailureError

_orig_send_on_endpoint = _GreatDancerApp.send_on_endpoint


def _patched_send_on_endpoint(self, ep_num, data, blocking=True):
    try:
        return _orig_send_on_endpoint(self, ep_num, data, blocking=blocking)
    except _CommandFailureError as e:
        print(f"[warn] dropped packet on EP{ep_num}/IN due to {e}")


_GreatDancerApp.send_on_endpoint = _patched_send_on_endpoint

# --- Monkeypatch: USBProxyDevice.handle_nak crashes if a NAK arrives on an IN
# endpoint before SET_CONFIGURATION has been processed (self.configuration is
# still None at that point). Just ignore NAKs until we're configured.
from facedancer.proxy import USBProxyDevice as _USBProxyDevice

_orig_handle_nak = _USBProxyDevice.handle_nak


def _patched_handle_nak(self, ep_num):
    if self.configuration is None:
        return
    return _orig_handle_nak(self, ep_num)


_USBProxyDevice.handle_nak = _patched_handle_nak

ID_VENDOR = 0x057E
ID_PRODUCT = 0x2069

CAPTURE_DIR = os.path.join(os.path.dirname(__file__), "captures")
os.makedirs(CAPTURE_DIR, exist_ok=True)
CAPTURE_PATH = os.path.join(
    CAPTURE_DIR, datetime.datetime.now().strftime("%Y%m%d-%H%M%S") + ".log"
)


class RawHexLogFilter(USBProxyFilter):
    """Appends every transfer (control + endpoint) to a flat hex log file."""

    def __init__(self):
        super().__init__()
        self._fh = open(CAPTURE_PATH, "a")

    def _write(self, direction, label, data):
        ts = datetime.datetime.now().strftime("%H:%M:%S.%f")
        hexstr = bytes(data).hex()
        line = f"[{ts}] {direction} {label} len={len(data)} {hexstr}\n"
        self._fh.write(line)
        self._fh.flush()

    def filter_control_in(self, req, data, stalled):
        if data:
            self._write("CTRL", f"IN  req={req}", data)
        return req, data, stalled

    def filter_control_out(self, req, data):
        if data:
            self._write("CTRL", f"OUT req={req}", data)
        return req, data

    def filter_in(self, ep_num, data):
        self._write("EP", f"IN  0x{0x80 | ep_num:02x}", data)
        return ep_num, data

    def filter_out(self, ep_num, data):
        self._write("EP", f"OUT 0x{ep_num:02x}", data)
        return ep_num, data


PCAP_MAGIC = 0xA1B2C3D4
LINKTYPE_USB_LINUX_MMAPPED = 220
XFER_ISO, XFER_INTERRUPT, XFER_CONTROL, XFER_BULK = 0, 1, 2, 3
EP_XFER_TYPE = {
    0x01: XFER_INTERRUPT, 0x81: XFER_INTERRUPT,
    0x02: XFER_BULK, 0x82: XFER_BULK,
    0x03: XFER_ISO, 0x83: XFER_ISO,
}
LIVE_PIPE = "/tmp/switch2_usb.pipe"


class LivePcapFilter(USBProxyFilter):
    """Streams live pcap records to a named pipe for real-time Wireshark capture.

    Open the pipe before (or shortly after) starting proxy.py with:
        wireshark -k -i /tmp/switch2_usb.pipe
    or in Wireshark GUI: Capture > Options > Manage Interfaces > Pipes, add the path.

    Wireshark link-type: USB_LINUX_MMAPPED (220) — use the USB dissector.
    """

    _global_header = struct.pack(
        "<IHHiIII", PCAP_MAGIC, 2, 4, 0, 0, 65535, LINKTYPE_USB_LINUX_MMAPPED
    )

    def __init__(self, pipe_path=LIVE_PIPE, devnum=2, busnum=1, max_queue=4000):
        super().__init__()
        self._devnum = devnum
        self._busnum = busnum
        self._seq = 0
        self._pipe_path = pipe_path
        self._q = queue.Queue(maxsize=max_queue)

        if os.path.exists(pipe_path):
            os.remove(pipe_path)
        os.mkfifo(pipe_path)
        print(f"[pcap] Named pipe created: {pipe_path}")
        print(f"[pcap] Run: wireshark -k -i {pipe_path}")

        t = threading.Thread(target=self._writer_thread, daemon=True)
        t.start()

    def _writer_thread(self):
        while True:
            print("[pcap] Waiting for Wireshark to open the pipe...")
            try:
                with open(self._pipe_path, "wb", buffering=0) as fh:
                    print("[pcap] Wireshark connected — streaming live pcap")
                    fh.write(self._global_header)
                    while True:
                        try:
                            record = self._q.get(timeout=1.0)
                        except queue.Empty:
                            continue
                        if record is None:
                            return
                        fh.write(record)
            except BrokenPipeError:
                print("[pcap] Wireshark disconnected — waiting for reconnect")
            except Exception as e:
                print(f"[pcap] pipe error: {e!r} — retrying")
            # drain stale queued packets before next reader connects
            while not self._q.empty():
                try:
                    self._q.get_nowait()
                except queue.Empty:
                    break

    def _emit(self, xfer_type, epnum, data):
        self._seq += 1
        ts = datetime.datetime.now().timestamp()
        ts_sec = int(ts)
        ts_usec = int(round((ts - ts_sec) * 1_000_000))

        usbmon_header = struct.pack(
            "<Q c B B B H B B q I I I I",
            self._seq,           # urb id
            b"C",                 # type: Complete
            xfer_type,
            epnum,
            self._devnum,
            self._busnum,
            0xFF,                 # flag_setup: no setup data
            0,                    # flag_data: data present
            ts_sec,
            ts_usec,
            0,                    # status
            len(data),
            len(data),
        )
        usbmon_union = b"\x00" * 8   # zeroed setup/iso union
        usbmon_trailer = struct.pack("<IIII", 0, 0, 0, 0)
        usbmon = usbmon_header + usbmon_union + usbmon_trailer  # 64 bytes
        assert len(usbmon) == 64

        pkt = usbmon + bytes(data)
        pcap_rec_header = struct.pack("<IIII", ts_sec, ts_usec, len(pkt), len(pkt))
        try:
            self._q.put_nowait(pcap_rec_header + pkt)
        except queue.Full:
            pass  # drop if Wireshark hasn't connected yet / is too slow

    def filter_control_in(self, req, data, stalled):
        if data:
            self._emit(XFER_CONTROL, 0x80, data)
        return req, data, stalled

    def filter_control_out(self, req, data):
        if data:
            self._emit(XFER_CONTROL, 0x00, data)
        return req, data

    def filter_in(self, ep_num, data):
        ep_addr = 0x80 | ep_num
        self._emit(EP_XFER_TYPE.get(ep_num & 0x7F, XFER_BULK), ep_addr, data)
        return ep_num, data

    def filter_out(self, ep_num, data):
        self._emit(EP_XFER_TYPE.get(ep_num & 0x7F, XFER_BULK), ep_num, data)
        return ep_num, data


if __name__ == "__main__":
    print(f"Logging raw transfers to {CAPTURE_PATH}")

    while True:
        proxy = USBProxyDevice(idVendor=ID_VENDOR, idProduct=ID_PRODUCT)

        # required: forwards control transfers (enumeration etc.) between target
        # host and the real controller
        proxy.add_filter(USBProxySetupFilters(proxy, verbose=0))

        # human-readable console log
        proxy.add_filter(USBProxyPrettyPrintFilter(verbose=5))

        # raw hex log to file, for later parsing / building protocol docs
        proxy.add_filter(RawHexLogFilter())

        # live pcap stream to named pipe — open with: wireshark -k -i /tmp/switch2_usb.pipe
        proxy.add_filter(LivePcapFilter())

        try:
            main(proxy)
        except KeyboardInterrupt:
            break
        except Exception as e:
            print(f"[warn] proxy crashed ({e!r}), cleaning up and restarting...")
            # Drop our reference to the proxy (and its GreatDancerApp/GreatFET
            # handle) and force a GC pass so the underlying USB device handle
            # is actually released before we try to claim it again. Without
            # this, the next greatfet.GreatFET() construction in the retry
            # loop can fail with "timed out trying to claim access to a
            # libgreat device!" because the old handle is still open.
            del proxy
            gc.collect()
            time.sleep(2.0)
            continue
