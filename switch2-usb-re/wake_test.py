#!/usr/bin/env python3
"""
Experiment: can the GreatFET alone (no real controller) wake a sleeping
Switch 2 console by completing the vendor bulk handshake with canned
responses and continuously reporting Home+Capture as held?

Home/Capture bit positions are a MEDIUM-confidence guess based on the
original Joy-Con/Pro Controller's well-documented button-byte layout
(byte offset+1 = "shared" byte: ... R-stick, L-stick, Home, Capture, ...),
which lines up with our own finding that input report byte 4 is a full
8-bit button field. Not yet verified against an isolated single-button
capture for THIS controller.

Run with sudo:
  sudo ~/greatfet-venv/bin/python3 wake_test.py
"""

from facedancer import main
from facedancer.device import USBDevice
from facedancer.configuration import USBConfiguration
from facedancer.types import USBDirection

# Same IAD-parsing fix as proxy.py/emulate_static.py.
import facedancer.configuration as _fd_configuration
from facedancer.descriptor import USBDescribable as _USBDescribable, USBDescriptor as _USBDescriptor
from facedancer.interface import USBInterface as _USBInterface
from facedancer.endpoint import USBEndpoint as _USBEndpoint
from facedancer.descriptor import StringRef as _StringRef


@classmethod
def _patched_from_binary_descriptor(cls, data, strings={}):
    import struct
    length = data[0]
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
                pass
            elif len(last_interface.endpoints) == 0:
                last_interface.add_descriptor(descriptor)
            else:
                last_endpoint.add_descriptor(descriptor)

        data = data[length:]

    return configuration


_fd_configuration.USBConfiguration.from_binary_descriptor = _patched_from_binary_descriptor

DEVICE_DESCRIPTOR = bytes.fromhex("12010002ef0201407e056920010201020301")
CONFIG_DESCRIPTOR = bytes.fromhex(
    "09020c01050104c0fa080b0001030000000904000002030000050921110100012261"
    "000705810340000407050103400004080b0101ff0000000904010002ff0000060705"
    "020240000007058202400000080b0203010100000904020000010100000a24010001"
    "47000203040c24020101010002030000000a2406020101030000000924030302030"
    "002000c2402040102000100000000092406050401030000092403060101000500090"
    "403000001020000090403010101020000072401010001000b2402010202100180bb0"
    "00705030dc0000107250100000000090404000001020000090404010101020000072"
    "401060001000b2402010202100180bb000705830dc0000107250100000000"
)

STRINGS = {1: "Nintendo", 2: "Switch 2 Pro Controller", 3: "000000000000"}

# Canned subcommand response payloads (bytes following the
# seq/0x01/0x00/subcmd/0x00/0xf8 header), extracted from a real capture.
# Best-effort placeholders, not guaranteed correct -- see references/protocol-details.md.
CANNED_RESPONSES = {
    0x01: bytes.fromhex("0000a5f400000000000093560000885600000000000000000000"),
    0x02: bytes.fromhex("000001dc4c631e8b7b53f0224b2a96dc5e5c89"),
    0x03: bytes.fromhex("000001c0030000e7d01c3b7922a03a0ae89c4258a00b420ae89c4158a00b41"),
    0x04: bytes.fromhex("0000400000008030010001add99a555665a0000aa0000ae2200ee2200e9aadd99aadd90aa5500aa5502ff6622ff6620affff82f7815636613886"),
    0x07: b"",
    0x08: b"",
    0x0a: b"",
    0x0c: bytes.fromhex("61125010"),
    0x0d: bytes.fromhex("01000000"),
}

# Cycle through candidate "buttons held" patterns, one at a time, since we
# don't know the real bit mapping for this controller yet. Each entry is
# (label, byte3, byte4, byte5).
PATTERNS = [
    ("none (baseline)",       0x00, 0x00, 0x00),
    ("home only (b4 bit4)",   0x00, 0x10, 0x00),
    ("capture only (b4 bit5)", 0x00, 0x20, 0x00),
    ("home+capture (b4 0x30)", 0x00, 0x30, 0x00),
    ("all of byte4",          0x00, 0xff, 0x00),
    ("all of byte3",          0xff, 0x00, 0x00),
    ("all of byte5",          0x00, 0x00, 0xff),
    ("all buttons (3,4,5)",   0xff, 0xff, 0xff),
]

PATTERN_HOLD_SECONDS = 4
_pattern_state = {"index": 0, "last_switch": 0.0}


def current_pattern():
    import time as _time
    now = _time.monotonic()
    if now - _pattern_state["last_switch"] >= PATTERN_HOLD_SECONDS:
        _pattern_state["index"] = (_pattern_state["index"] + 1) % len(PATTERNS)
        _pattern_state["last_switch"] = now
        label, b3, b4, b5 = PATTERNS[_pattern_state["index"]]
        print(f"[wake_test] now sending pattern: {label}  (byte3={b3:#04x} byte4={b4:#04x} byte5={b5:#04x})")
    return PATTERNS[_pattern_state["index"]]


def make_input_report(counter: int) -> bytes:
    _, b3, b4, b5 = current_pattern()
    report = bytearray(64)
    report[0] = 0x09
    report[1] = counter & 0xFF
    report[3] = b3
    report[4] = b4
    report[5] = b5
    return bytes(report)


if __name__ == "__main__":
    device = USBDevice.from_binary_descriptor(DEVICE_DESCRIPTOR, strings=STRINGS)
    device.configurations.clear()
    configuration = USBConfiguration.from_binary_descriptor(CONFIG_DESCRIPTOR)
    device.add_configuration(configuration)

    # Look these up directly from the static interface structure (keyed by
    # (interface_number, alt_setting)) rather than via
    # configuration.get_endpoint()/active_interfaces, which is only
    # populated once a real SET_CONFIGURATION has actually happened.
    iface0 = configuration.interfaces[(0, 0)]
    iface1 = configuration.interfaces[(1, 0)]
    ep81 = iface0.get_endpoint(1, USBDirection.IN)
    ep02 = iface1.get_endpoint(2, USBDirection.OUT)
    ep82 = iface1.get_endpoint(2, USBDirection.IN)
    assert ep81 and ep02 and ep82, "endpoint lookup failed"

    counter = [0]

    def on_ep81_requested():
        counter[0] += 1
        ep81.send(make_input_report(counter[0]))

    def on_ep02_received(data: bytes):
        if len(data) < 4:
            return
        seq = data[0]
        subcmd = data[3]
        payload = CANNED_RESPONSES.get(subcmd, b"")
        response = bytes([seq, 0x01, 0x00, subcmd, 0x00, 0xf8]) + payload
        ep82.send(response)
        print(f"[wake_test] subcmd 0x{subcmd:02x} -> sent {len(response)} bytes")

    ep81.handle_data_requested = on_ep81_requested
    ep02.handle_data_received = on_ep02_received

    print("Emulating Switch 2 Pro Controller with canned handshake responses,")
    print(f"cycling through {len(PATTERNS)} button patterns every {PATTERN_HOLD_SECONDS}s.")
    print("Watch the console for ANY sign of life and note which pattern was active.")
    main(device)
