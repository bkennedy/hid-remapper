#!/usr/bin/env python3
"""
Diagnostic: statically emulate the Switch 2 Pro Controller's exact captured
descriptors on the GreatFET, with NO real controller involved at all.

This isolates whether the GreatFET<->console physical link works: if the
console issues a bus reset and starts requesting descriptors here, the link
is fine and the earlier "nothing happens" was a proxy/real-device issue. If
this also produces zero traffic, the GreatFET<->console cable/port itself is
the problem.

Run with sudo:
  sudo ~/greatfet-venv/bin/python3 emulate_static.py
"""

from facedancer import main
from facedancer.device import USBDevice
from facedancer.configuration import USBConfiguration
from facedancer.descriptor import StringRef

# Same IAD-parsing fix as proxy.py: facedancer's parser crashes on a
# configuration that has an Interface Association Descriptor before any
# interface has been parsed yet.
import facedancer.configuration as _fd_configuration
from facedancer.descriptor import USBDescribable as _USBDescribable, USBDescriptor as _USBDescriptor
from facedancer.interface import USBInterface as _USBInterface
from facedancer.endpoint import USBEndpoint as _USBEndpoint


@classmethod
def _patched_from_binary_descriptor(cls, data, strings={}):
    import struct
    length = data[0]
    descriptor_type, total_length, num_interfaces, index, string_index, \
        attributes, half_max_power = struct.unpack_from('<xBHBBBBB', data[0:length])

    configuration = cls(
        number=index,
        configuration_string=StringRef.lookup(strings, string_index),
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


if __name__ == "__main__":
    device = USBDevice.from_binary_descriptor(DEVICE_DESCRIPTOR, strings=STRINGS)
    device.configurations.clear()
    configuration = USBConfiguration.from_binary_descriptor(CONFIG_DESCRIPTOR)
    device.add_configuration(configuration)

    print("Emulating static Switch 2 Pro Controller descriptors, no real device involved.")
    main(device)
