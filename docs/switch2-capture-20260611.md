# Switch 2 Pro BLE Capture 2026-06-11

Capture file analyzed:

```text
/Users/bk/Documents/capture/switch2-20260611-085157.pcap
```

## Summary

- The sniffer captured 23,896 BLE frames.
- The capture contains Switch 2 Pro-looking advertising from
  `3c:a9:ab:69:17:3d`.
- The capture does not contain usable ATT, GATT, SMP, or decrypted controller
  input traffic.
- Three `CONNECT_IND` packets are present, but all are CRC-bad/malformed and
  none target the Switch 2 Pro advertiser.
- This capture is useful for BLE discovery/scanning, not for deriving the
  controller's GATT setup or pairing command exchange.

## Observed Switch 2 Pro Advertising

The likely Switch 2 Pro advertiser repeatedly sent connectable `ADV_IND` packets
with public address:

```text
3c:a9:ab:69:17:3d
```

Representative manufacturer data from Wireshark:

```text
company_id = 0x0553
data       = 01 00 03 7e 05 69 20 00 01 00 00 00 00 00 00 00 0f 00 00 00 00 00 00 00
```

Zephyr's `BT_DATA_MANUFACTURER_DATA` buffer includes the little-endian company
ID bytes at the front, so firmware sees:

```text
53 05 01 00 03 7e 05 69 20 ...
```

Implementation-relevant offsets in that Zephyr buffer:

| Offset | Bytes | Meaning |
| --- | --- | --- |
| `0..1` | `53 05` | Bluetooth company ID `0x0553` |
| `5..6` | `7e 05` | Nintendo USB VID `0x057e` |
| `7..8` | `69 20` | Switch 2 Pro PID `0x2069` |

## What This Means For Firmware

- The scanner should filter manufacturer data on company `0x0553`, not
  `0x057e`.
- The product identity check should look for `0x057e:0x2069` inside the
  manufacturer payload.
- `firmware-bluetooth/src/main.cc` now logs the first matched manufacturer bytes
  into the Switch 2 flight log so the next board run can prove whether the board
  actually saw and attempted to connect to the controller.

## What This Capture Does Not Prove

- It does not prove the current board firmware can complete BLE connection.
- It does not include the controller's proprietary GATT services or handles.
- It does not include the `0x15` key/pairing exchange.
- It does not give new USB console command evidence.

For GATT/pairing analysis, capture again with the sniffer following
`3c:a9:ab:69:17:3d` before the connection attempt starts, or use Wireshark's
nRF Sniffer device selector to lock onto that advertiser.

## Wireshark UI Pairing Capture

Capture file analyzed:

```text
/Users/bk/Documents/capture/capture-ui.pcapng
```

This capture contains a clean `CONNECT_IND` from the console to the Switch 2 Pro
Controller:

| Field | Value |
| --- | --- |
| Frame | `12224` |
| Time | `95.317202000` |
| Initiator | `c8:48:05:65:39:e1` |
| Advertiser | `3c:a9:ab:69:17:3d` |
| CONNECT_IND access address | `0xac83c178` |
| CRC init | `0x87bd1b` |
| Window size | `2`, i.e. 2.5 ms |
| Window offset | `9`, i.e. 11.25 ms |
| Connection interval | `12`, i.e. 15 ms |
| Slave latency | `0` |
| Supervision timeout | `200`, i.e. 2000 ms |
| Channel map | `1f ff 00 00 00` |
| Hop | `1` |
| Sleep clock accuracy | 31 ppm to 50 ppm |

This proves a Nintendo device attempted to connect to the controller. It still
does not include decoded ATT/GATT/SMP traffic after the `CONNECT_IND`; no
packets with data-channel access address `0xac83c178` were present in the saved
capture.

Implementation notes from this capture:

- The real console uses a 15 ms initial interval for this connection attempt.
  The current firmware central connection parameter is `BT_LE_CONN_PARAM(6, 6,
  44, 400)`, i.e. 7.5 ms, so testing a Switch 2-specific 15 ms parameter is a
  reasonable next BLE input experiment if the board sees advertisements but does
  not complete connection.
- The initiator address `c8:48:05:65:39:e1` should be treated as observed
  console-side evidence from this capture, not as a stable console identity
  until more captures confirm it.
- This capture still does not provide the `0x15` pairing/key exchange, GATT
  handle traffic, or notification payloads.

## Second Wireshark UI Capture With Controller Use

The user later overwrote/saved another Wireshark UI capture at the same path:

```text
/Users/bk/Documents/capture/capture-ui.pcapng
```

That capture also contains a clean console `CONNECT_IND` to the same controller:

| Field | Value |
| --- | --- |
| Frame | `5054` |
| Time | `8.659080000` |
| Initiator | `c8:48:05:65:39:e1` |
| Advertiser | `3c:a9:ab:69:17:3d` |
| CONNECT_IND access address | `0xba0e2907` |
| CRC init | `0x08fa3f` |
| Window size | `2`, i.e. 2.5 ms |
| Window offset | `4`, i.e. 5 ms |
| Connection interval | `12`, i.e. 15 ms |
| Slave latency | `0` |
| Supervision timeout | `200`, i.e. 2000 ms |
| Channel map | `1f ff 00 00 00` |
| Hop | `1` |
| Sleep clock accuracy | 51 ppm to 75 ppm |

Even though the controller was reportedly used during this capture, the saved
file still does not contain packets on data-channel access address
`0xba0e2907`, nor decoded ATT/GATT/SMP. Treat it as a second confirmation of
console connection parameters, not as an input or pairing payload capture.

## Followed Wireshark UI Capture With ATT Traffic

The user later saved another Wireshark UI capture at the same path:

```text
/Users/bk/Documents/capture/capture-ui.pcapng
```

This capture finally followed the Switch 2 Pro controller connection. It
contains:

- `4,117` advertising-channel packets on access address `0x8e89bed6`;
- `994` connection data-channel packets on access address `0x41b5f0dc`;
- `30` decoded ATT/L2CAP packets.

The followed data channel proves the nRF Sniffer/Wireshark path can capture the
console/controller BLE command setup when the sniffer is explicitly set to
follow the controller before the console connects.

Important connection-level observations:

| Field | Value |
| --- | --- |
| Data access address | `0x41b5f0dc` |
| Data packets captured | `994` |
| Decoded ATT packets | `30` |
| ATT/L2CAP fixed channel | `0x0004` |
| First decoded ATT frame | `4144` |
| Capture path | Wireshark UI nRF Sniffer, not standalone CLI follow |

Observed ATT setup and command handles:

| Frame | Opcode | Handle | Value / meaning |
| --- | --- | --- | --- |
| `4151` | `0x09` write request | `0x0002` | `04 00 05 00 01 01 00` |
| `4152` | `0x12` write command | `0x0005` | `01 00` |
| `4159` | `0x09` write request | `0x0006` | `36 80 74 f3 a5 3d 8e 13` |
| `4160` | `0x12` write command | `0x001b` | `01 00`; enables notifications/indications for response handle `0x001a` |
| `4164` | `0x12` write command | `0x001f` | `01 00` |
| `4168` | `0x12` write command | `0x0023` | `01 00` |
| `4172` | `0x52` write command | `0x0016` | console command `07 91 01 01 00 00 00 00` after zero padding |
| `4175` | `0x1b` notification | `0x001a` | controller response `07 01 01 01 10 78 00 00 00` |
| `4176` | `0x52` write command | `0x0016` | console command `02 91 01 04 00 08 00 00 40 7e 00 00 30 01 00` |
| `4179` | `0x1b` notification | `0x001a` | controller response includes serial-like text `HEW70006169780`, `057e:2069`, calibration-looking bytes, and `ff` padding |
| `4180` | `0x52` write command | `0x0016` | console command `10 91 01 01 00 00 00 00` |
| `4187` | `0x1b` notification | `0x001a` | response `10 01 01 01 10 78 ...` |
| `4188` | `0x52` write command | `0x0016` | console command `16 91 01 01 00 00 00 00` |
| `4191` | `0x1b` notification | `0x001a` | response to `0x16` |
| `4192` | `0x52` write command | `0x0016` | console command `15 91 01 01 00 0e 00 00 00 02 e1 39 65 05 48 c8 e0 39 65 05 48 c8` |
| `4195` | `0x1b` notification | `0x001a` | `0x15` response fragment |
| `4196` | `0x52` write command | `0x0016` | console command `15 91 01 04 00 11 00 00 00 e2 9f 44 b9 75 5f f5 b1 0e 63 5c 5d 71 f7 ce e8` |
| `4199` | `0x1b` notification | `0x001a` | `0x15` response fragment |
| `4200` | `0x52` write command | `0x0016` | console command `15 91 01 02 00 11 00 00 00 ad a1 7c b3 ec aa b7 ea 66 e4 2c 21 15 1f 3a bf` |
| `4205` | `0x1b` notification | `0x001a` | `0x15` response fragment |
| `4206` | `0x52` write command | `0x0016` | console command `15 91 01 03 00 01 00 00 00` |
| `4213` | `0x1b` notification | `0x001a` | short `0x15` response |

The `0x15` exchange is now confirmed on the real console/controller BLE path.
It is no longer only a public-source inference. However, this capture still
does not prove that a fresh adapter can generate valid `0x15` values; it only
shows one successful exchange between the user's console and physical
controller.

After the command setup, the capture contains many data packets with 74-byte
payload lengths at 15 ms cadence. Wireshark does not decode them as ATT in the
basic field extraction; treat them as likely encrypted or proprietary
controller data until decoded further.

The link-layer encryption boundary is visible:

| Frame | Direction/meaning |
| --- | --- |
| `4214` | clear `LL_ENC_REQ`; SKDm `0x643030e22b539cc9`, IVm `0x96c443da` |
| `4217` | clear `LL_ENC_RSP`; SKDs `0xd5c2371c963a50e8`, IVs `0xcc7b18f3` |
| `4219` | clear `LL_START_ENC_REQ` |
| `4220` and later | marked encrypted by the nRF Sniffer metadata; Wireshark reports MIC errors because it does not have the LTK/session key |

Therefore the post-`4219` packets cannot be decoded from this passive capture
alone. Decrypting them requires the BLE LTK or a capture made with enough key
material supplied to the sniffer. The capture contains SKD/IV values, but not
the LTK needed to derive the session key.

## HID Remapper Bond-Key Export

The branch now includes a debug config command, `GET_SWITCH2_BOND_KEYS`, and a
Mac-friendly helper:

```text
cd /Users/bk/Documents/Codex/2026-06-09/hid-remapper-expert-users-bk-codex/work/switch2-codex/config-tool
python3 pull_switch2_bond_keys.py
```

This reads Zephyr's raw `bt/keys` settings records from the Seeed board over
the HID Remapper config interface and prints each record as hex. Use it after
pairing the Switch 2 Pro Controller to the board.

Important limitation: this exports the Seeed board's BLE bond material for
controller-to-board traffic. It does not magically recover the Switch 2
console's stored LTK for an existing console-to-controller bond. To decrypt a
console/controller sniffer capture, Wireshark needs the LTK for that exact
console/controller bond.

Implementation implications:

- The BLE console command path matches the public handle model from Leon's
  notes: console writes to handle `0x0016`; controller responses arrive on
  handle `0x001a`; CCC at `0x001b` is enabled with `01 00`.
- The console command stream includes raw command bytes that resemble the USB
  command framing documented elsewhere, but with `0x91 0x01` in the second and
  third bytes for this BLE exchange.
- The `0x15` command uses the observed console address bytes in little-endian
  form in frame `4192`: `e1 39 65 05 48 c8`.
- HID Remapper's BLE input-side implementation should continue to prioritize
  connecting as a central to a real controller; console impersonation/wake still
  requires solving or replaying `0x15` material, which is out of scope for a
  blind generic implementation.

## Live Pairing and LTK Extraction — 2026-06-11

The new Switch 2 Pro firmware (descriptor 7, `remapper.uf2` built 2026-06-11)
successfully paired with a physical Switch 2 Pro Controller. The Seeed Xiao
nRF52840 board ran as BLE central; the controller bonded and the Zephyr
`bt/keys` settings snapshot was captured in RAM by `pairing_complete()`.

`pull_switch2_bond_keys.py` was run immediately after pairing:

```
python3 config-tool/pull_switch2_bond_keys.py
```

### Result

```
Snapshot: magic=0x324b4253 version=1 total_len=100 records=1 truncated=0

key: bt/keys/987a14e5945f0  (84 bytes)

LTK (LE SC) at value+14: ee65ad9f98c6b004c072c3dd6ba4a7ef
```

| Field | Value |
| --- | --- |
| Settings key suffix | `987a14e5945f0` |
| LTK (LE SC) | `ee65ad9f98c6b004c072c3dd6ba4a7ef` |
| rand | `0000000000000000` (LE SC = all zeros) |
| ediv | `0000` (LE SC = zero) |
| Record count | 1 |
| Truncated | no |

The 84-byte value layout observed:

```
value+00: 10 10 22 00   key header (flags/enc_size/keys bitmask)
value+04: 00 * 8        rand[8] = 0x00 (LE SC)
value+0c: 00 00         ediv[2] = 0x0000 (LE SC)
value+0e: ee 65 ad 9f 98 c6 b0 04 c0 72 c3 dd 6b a4 a7 ef   LTK val[16]
value+1e: 76 37 5f 94 e5 14 7a 98 43 da ea eb cc ed 37 76   slave LTK or IRK
value+2e: 00 * padding
value+51: 29 00 00 00   trailing counter/flags
```

### Snapshot struct format (`switch2_bond_keys_snapshot`)

Paginated at 28 bytes/page over the HID Remapper config interface:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | `magic` = `0x324b4253` ("SBK2") |
| 4 | 2 | `version` = 1 |
| 6 | 2 | `total_len` (bytes used in `data[]`) |
| 8 | 1 | `record_count` |
| 9 | 1 | `truncated` |
| 10 | 2 | `reserved` |
| 12 | up to 1024 | `data[]` — records |

Each record inside `data[]`:

```
name_len[1]  value_len[2 LE]  name[name_len]  value[value_len]
```

The `name` is the settings key suffix after `bt/keys/` (e.g. `987a14e5945f0`).

### Script path

```
config-tool/pull_switch2_bond_keys.py
```

The script handles the two-path composite device enumeration on macOS (both
paths are the same physical HID config interface; the script tries each until
one returns a valid `SBK2` magic response).

### Why the device enumerates twice on macOS

The HID Remapper Bluetooth board is always a USB composite device. In Switch 2
Pro mode (`our_descriptor_number == 7`) it has three interfaces:

1. HID — Switch 2 Pro gamepad output (report IDs 5, 9, …)
2. HID — config interface (usage page `0xFF00`, usage `0x0020`, report ID 100)
3. Vendor class — EP `0x02` out / `0x82` in for Switch 2 console communication

macOS/hidapi creates one device node per top-level HID collection. If the
config interface descriptor declares two top-level collections with the same
usage, both appear in `hid.enumerate()` with identical usage page/usage. Both
paths point at the same physical interface and respond identically.
