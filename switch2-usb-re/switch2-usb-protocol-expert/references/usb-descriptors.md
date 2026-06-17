# Raw USB descriptors — Switch 2 Pro Controller (0x057E:0x2069)

Captured via direct USB enumeration (pyusb/libusb) with the controller plugged
directly into a host, and cross-checked against the MITM capture.

## Device descriptor (18 bytes)

```
12 01 00 02 ef 02 01 40 7e 05 69 20 01 02 01 02 03 01
```

| Field | Value |
|---|---|
| bLength | 0x12 (18) |
| bDescriptorType | 0x01 (Device) |
| bcdUSB | 0x0200 |
| bDeviceClass | 0xEF (Miscellaneous) |
| bDeviceSubClass | 0x02 |
| bDeviceProtocol | 0x01 (Interface Association Descriptor) |
| bMaxPacketSize0 | 0x40 (64) |
| idVendor | 0x057E (Nintendo) |
| idProduct | 0x2069 |
| bcdDevice | 0x0201 |
| iManufacturer | 1 ("Nintendo") |
| iProduct | 2 ("Switch 2 Pro Controller") |
| iSerialNumber | 3 |
| bNumConfigurations | 1 |

## Configuration descriptor (268 bytes total)

```
09020c01050104c0fa080b0001030000000904000002030000050921110100012261000705810
340000407050103400004080b0101ff0000000904010002ff0000060705020240000007058202
400000080b0203010100000904020000010100000a2401000147000203040c24020101010002
030000000a2406020101030000000924030302030002000c2402040102000100000000092406
050401030000092403060101000500090403000001020000090403010101020000072401010
001000b2402010202100180bb000705030dc0000107250100000000090404000001020000090
404010101020000072401060001000b2402010202100180bb000705830dc000010725010000
0000
```

(whitespace/wrapping added for readability — concatenate to get the real
byte stream)

## Byte-exact descriptor table (HIGH confidence — mechanically parsed, not summarized by hand)

Every descriptor in the 268-byte config descriptor, in order, with its raw
hex and fully decoded fields. This is the authoritative source — if any
other section in this skill describes the layout in prose and it conflicts
with this table, **this table wins**.

| Offset | Len | Raw hex | Descriptor |
|---|---|---|---|
| 0 | 9 | `09020c01050104c0fa` | CONFIGURATION: wTotalLength=268 bNumInterfaces=5 bConfigurationValue=1 iConfiguration=4 bmAttributes=0xc0 bMaxPower=250 (=500mA) |
| 9 | 8 | `080b000103000000` | **IAD**: bFirstInterface=**0** bInterfaceCount=**1** bFunctionClass=0x03 bFunctionSubClass=0x00 bFunctionProtocol=0x00 iFunction=0 |
| 17 | 9 | `090400000203000005` | INTERFACE: number=0 alt=0 numEndpoints=2 class=0x03 subclass=0x00 protocol=0x00 iInterface=5 |
| 26 | 9 | `092111010001226100` | HID descriptor (class-specific, subtype 0x21) |
| 35 | 7 | `07058103400004` | ENDPOINT: address=0x81 (IN EP1) attributes=0x03 (Interrupt) maxPacketSize=64 interval=4 |
| 42 | 7 | `07050103400004` | ENDPOINT: address=0x01 (OUT EP1) attributes=0x03 (Interrupt) maxPacketSize=64 interval=4 |
| 49 | 8 | `080b0101ff000000` | **IAD**: bFirstInterface=**1** bInterfaceCount=**1** bFunctionClass=0xff bFunctionSubClass=0x00 bFunctionProtocol=0x00 iFunction=0 |
| 57 | 9 | `0904010002ff000006` | INTERFACE: number=1 alt=0 numEndpoints=2 class=0xff subclass=0x00 protocol=0x00 iInterface=6 |
| 66 | 7 | `07050202400000` | ENDPOINT: address=0x02 (OUT EP2) attributes=0x02 (Bulk) maxPacketSize=64 interval=0 |
| 73 | 7 | `07058202400000` | ENDPOINT: address=0x82 (IN EP2) attributes=0x02 (Bulk) maxPacketSize=64 interval=0 |
| 80 | 8 | `080b020301010000` | **IAD**: bFirstInterface=**2** bInterfaceCount=**3** bFunctionClass=0x01 bFunctionSubClass=0x01 bFunctionProtocol=0x00 iFunction=0 |
| 88 | 9 | `090402000001010000` | INTERFACE: number=2 alt=0 numEndpoints=0 class=0x01 (Audio) subclass=0x01 (AudioControl) protocol=0x00 |
| 97 | 10 | `0a240100014700020304` | CS_INTERFACE (audio, type 0x24) |
| 107 | 12 | `0c2402010101000203000000` | CS_INTERFACE (audio, type 0x24) |
| 119 | 10 | `0a240602010103000000` | CS_INTERFACE (audio, type 0x24) |
| 129 | 9 | `092403030203000200` | CS_INTERFACE (audio, type 0x24) |
| 138 | 12 | `0c2402040102000100000000` | CS_INTERFACE (audio, type 0x24) |
| 150 | 9 | `092406050401030000` | CS_INTERFACE (audio, type 0x24) |
| 159 | 9 | `092403060101000500` | CS_INTERFACE (audio, type 0x24) |
| 168 | 9 | `090403000001020000` | INTERFACE: number=3 alt=0 numEndpoints=0 class=0x01 subclass=0x02 (AudioStreaming) protocol=0x00 |
| 177 | 9 | `090403010101020000` | INTERFACE: number=3 alt=1 numEndpoints=1 class=0x01 subclass=0x02 protocol=0x00 |
| 186 | 7 | `07240101000100` | CS_INTERFACE (audio, type 0x24) |
| 193 | 11 | `0b2402010202100180bb00` | CS_INTERFACE (audio, type 0x24) |
| 204 | 7 | `0705030dc00001` | ENDPOINT: address=0x03 (OUT EP3) attributes=0x0d (Isochronous) maxPacketSize=192 interval=1 |
| 211 | 7 | `07250100000000` | CS_ENDPOINT (audio, type 0x25) |
| 218 | 9 | `090404000001020000` | INTERFACE: number=4 alt=0 numEndpoints=0 class=0x01 subclass=0x02 protocol=0x00 |
| 227 | 9 | `090404010101020000` | INTERFACE: number=4 alt=1 numEndpoints=1 class=0x01 subclass=0x02 protocol=0x00 |
| 236 | 7 | `07240106000100` | CS_INTERFACE (audio, type 0x24) |
| 243 | 11 | `0b2402010202100180bb00` | CS_INTERFACE (audio, type 0x24) |
| 254 | 7 | `0705830dc00001` | ENDPOINT: address=0x83 (IN EP3) attributes=0x0d (Isochronous) maxPacketSize=192 interval=1 |
| 261 | 7 | `07250100000000` | CS_ENDPOINT (audio, type 0x25) |

**IAD grouping is exactly three IADs** (precision correction — an earlier
draft of this doc said interfaces 2+3 and 4 were two separate IADs, which
was wrong):
- IAD #1: bFirstInterface=0, bInterfaceCount=1 → covers interface 0 (HID) alone
- IAD #2: bFirstInterface=1, bInterfaceCount=1 → covers interface 1 (vendor) alone
- IAD #3: bFirstInterface=2, bInterfaceCount=**3** → covers interfaces 2, 3, **and** 4 together as a single audio function (AudioControl interface 2 + two AudioStreaming interfaces 3/4, one per direction)

The class-specific (0x24/0x25) audio descriptor payloads have not been
individually decoded field-by-field (e.g. exact terminal IDs, channel
config, sample rates) — they're listed above byte-exact but uninterpreted.
Decode them against the USB Audio Class 1.0 spec if you need that detail;
they are not relevant to button/stick/rumble work.

## String descriptors

- iManufacturer (1): "Nintendo"
- iProduct (2): "Switch 2 Pro Controller"
- iSerialNumber (3): literal "00" (placeholder/anonymized — real serial may
  require a different vendor request, see below)
- iConfiguration (4): "Config_0"
- Interface 0 string (5): "If_Hid"
- Interface 1 string (6): "Switch 2 Pro Controller"

## Unexplained vendor control transfer seen during enumeration

During one capture, a non-standard control IN transfer (`bmRequestType=0xA1`,
i.e. vendor/class request to device, direction IN) returned a ~64-byte block:

```
01 00 48 45 57 37 30 30 30 36 31 36 39 37 38 30 00 00 7e 05 69 20 01 06 01 ...
```//  "HEW70006169780" appears as ASCII starting at byte 2

Decoded ASCII: `HEW70006169780` — this looks like a real per-unit serial
number (different format than the placeholder "00" string descriptor above).
The request parameters that triggered this weren't fully logged; worth
revisiting with a dedicated capture that records full `SETUP` packet fields
(bmRequestType/bRequest/wValue/wIndex/wLength) for every control transfer, not
just the data stage.
