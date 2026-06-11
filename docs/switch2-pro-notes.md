# Switch 2 Pro Controller Protocol Notes

This branch is for Switch 2 Pro Controller passthrough on HID Remapper. The
target product idea is:

- pair a Nintendo Switch 2 Pro Controller to a Seeed Xiao nRF52840 board as
  input;
- make the board control a Nintendo Switch 2 console as if it were a Switch 2
  Pro Controller;
- pass console rumble back to the physical controller when possible;
- investigate console wake/autoconnect.

These notes are a research notebook, not proof that all paths are implemented.
Keep source confidence and protocol direction clear: "real controller to PC",
"adapter to Switch 2 console", and "BLE controller to Switch 2 console" are
different problems.

## Search Coverage

Last broad search pass: 2026-06-09.

Useful public sources found:

- [mlstr0m/switch2bridge-macos](https://github.com/mlstr0m/switch2bridge-macos)
  - Confidence: high for BLE controller input characteristic and input report
    byte layout.
  - Limitation: app is a macOS BLE-to-keyboard bridge. It does not implement a
    virtual HID device, rumble, LED control, console impersonation, or console
    wake.
- [Leon's Notes: Reverse-Engineering the Switch 2 Pro Controller's Bluetooth
  Protocol](https://leonsnotes.ca/2026/04/04/reverse-engineering-the-switch-2-pro-controllers-bluetooth-protocol/)
  - Confidence: high for BLE console-impersonation constraints and pairing
    blockers.
  - Limitation: implementation is not public, and the first-pairing `0x15`
    exchange is explicitly unsolved.
- [Switchbrew Switch 2 Pro Controller](https://switchbrew.org/wiki/Switch_2%3A_Pro_Controller)
  and [switch2brew Pro Controller](https://switch2brew.org/wiki/Pro_Controller)
  - Confidence: high for public wiki-level USB command framing and hardware
    metadata.
  - Limitation: incomplete command documentation.
- [HandHeldLegend ProCon2Tool](https://handheldlegend.github.io/procon2tool/)
  - Confidence: high for WebUSB/WebHID real-controller enablement commands and
    haptic test report shape because the page contains working JavaScript.
  - Limitation: tool enables a real controller for a host computer; it is not a
    Switch 2 console adapter implementation.
- [ikz87/NSW2-controller-enabler](https://github.com/ikz87/NSW2-controller-enabler)
  - Confidence: medium. It is derivative of HandHeldLegend and useful for USB
    command sequence and Linux input mapping clues.
  - Limitation: current code appears more GameCube-controller-focused than the
    README implies, so apply Pro Controller 2 facts cautiously.
- [Linux input thread](https://www.spinics.net/lists/linux-input/msg101879.html)
  and [follow-up](https://www.spinics.net/lists/linux-input/msg102062.html)
  - Confidence: medium-high for host-observed Linux behavior and driver
    maintainers' assessment.
  - Limitation: not a full protocol spec.
- [MissionControl issue 959](https://github.com/ndeadly/MissionControl/issues/959)
  - Confidence: medium for public maintainer status and BLE/HID-profile
    constraints.
  - Limitation: detailed findings are not all public in the issue.
- [BlueRetro issue 1249](https://github.com/darthcloud/BlueRetro/issues/1249)
  - Confidence: low from the issue page alone. Treat it as a tracker that other
    researchers cite, not as a spec.
- [Alia5/SISR](https://github.com/Alia5/SISR) and
  [Alia5/VIIPER](https://github.com/Alia5/VIIPER)
  - Confidence: high for public USB device emulation behavior to PC/SDL-style
    hosts.
  - Limitation: USB emulation to a PC host is not the same as confirmed Switch 2
    console acceptance, though it is the richest public code source found.
- [Nintendo Support controller diagram](https://en-americas-support.nintendo.com/app/answers/detail/a_id/68527/~/nintendo-switch%26nbsp%3B2-pro-controller-diagram)
  - Confidence: high for official button names and physical controls only.
- [Joypad controller page](https://joypad.ai/controllers/nintendo-switch-2-pro-controller)
  - Confidence: medium for high-level metadata such as VID/PID and feature
    labels.
  - Limitation: no packet-level protocol detail.

Searches did not find a complete public Switch 2 Pro BLE pairing implementation
that generates the `0x15` key exchange from scratch. The only public claim found
for BLE console impersonation uses donor controller MAC/LTK material.

## Identity And Hardware Metadata

- Nintendo vendor ID: `0x057e`.
- Switch 2 Pro Controller product ID: `0x2069`.
- Existing Switch 1 Pro Controller product ID: `0x2009`.
- Current HID Remapper descriptor index `6` is labelled Switch Pro Controller,
  but uses `057e:2009`, so treat it as Switch 1 Pro unless proven otherwise.
- Public hardware notes identify the Switch 2 Pro Controller board as
  `BEE-FKC-MAIN-01`, with a MediaTek `MT3689BCA` Bluetooth SoC and an NXP
  `PN71602` NFC controller.
- Official controls include A/B/X/Y, D-pad, L/R, ZL/ZR, `+`, `-`, HOME,
  Capture, C, GL, GR, sticks, player LEDs, audio jack, SYNC, USB-C, NFC, and
  recharge LED.

## USB: Real Controller Enablement

This section is about talking to a real Switch 2 Pro Controller over USB from a
host computer.

### Interface And Endpoint Shape

- Switchbrew says the protocol is extremely similar to the Switch 1 Pro
  Controller, but over USB it uses the bulk OUT endpoint and sends data over
  interface `1`.
- HandHeldLegend's tool selects Nintendo devices including PID `0x2069`, claims
  USB interface `1`, and finds a bulk OUT endpoint before sending setup
  commands.
- The Linux input thread reports that simply adding `057e:2069` to the existing
  Linux `hid_nintendo` driver was not enough; the controller needs extra
  initialization before it behaves as a normal input device.
- Host observations before enablement: the controller may appear in `lsusb`, may
  expose audio/headphone functionality, and may not expose usable controller
  input until the enablement commands are sent.

### Command Header

Switchbrew documents command packets as little-endian and padded to 8 bytes:

| Offset | Meaning |
| --- | --- |
| `0x00` | command ID |
| `0x01` | always `0x91` |
| `0x02` | argument/subcommand area |
| `0x04` | argument/subcommand area |
| `0x06` | argument/subcommand area |

Common command IDs from Switchbrew and public tools:

| Command | Public meaning |
| --- | --- |
| `0x01` | unknown / setup |
| `0x02` | SPI read |
| `0x03` | USB reports / haptics / init |
| `0x07` | unknown setup |
| `0x08` | unknown setup in VIIPER SDL sequence |
| `0x09` | player LED |
| `0x0a` | unknown setup |
| `0x0c` | IMU / feature command |
| `0x10` | unknown setup in HandHeldLegend |
| `0x11` | unknown setup |
| `0x15` | controller MAC / LTK / BLE-related key material |
| `0x16` | unknown setup in HandHeldLegend |

### Public Init Commands

HandHeldLegend and Switchbrew identify this as an initialize/start input or
haptics-related command:

```text
03 91 00 0d 00 08 00 00 01 00 ff ff ff ff ff ff
```

HandHeldLegend's public JavaScript includes these setup commands or variants:

```text
07 91 00 01 00 00 00 00
16 91 00 01 00 00 00 00
15 91 00 01 00 0e 00 00 00 02 ff ff ff ff ff ff ff ff ff ff ff ff ff ff
15 91 00 02 00 11 00 00 00 ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff
15 91 00 03 00 01 00 00 00
09 91 00 07 00 08 00 00 00 00 00 00 00 00 00 00
0c 91 00 02 00 04 00 00 27 00 00 00
11 91 00 03 00 00 00 00
0a 91 00 08 00 14 00 00 01 ff ff ff ff ff ff ff ff 35 00 46 00 00 00 00 00 00 00 00
0c 91 00 04 00 04 00 00 27 00 00 00
03 91 00 0a 00 04 00 00 09 00 00 00
10 91 00 01 00 00 00 00
01 91 00 0c 00 00 00 00
03 91 00 01 00 00 00
0a 91 00 02 00 04 00 00 03 00 00 00
09 91 00 07 00 08 00 00 01 00 00 00 00 00 00 00
```

The `0x15` commands appear to pass console MAC/LTK-like material. Treat dummy
`ff` bytes as tool placeholders, not proof that arbitrary values work on a
Switch 2 console.

VIIPER's SDL-style test sequence is another useful public sequence:

```text
07 91 00 01 00 00 00 00
0c 91 00 02 00 04 00 00 27 00 00 00
11 91 00 01 00 00 00 00
0a 91 00 08 00 14 00 00 01 ff ff ff ff ff ff ff ff 35 00 46 00 00 00 00 00 00 00 00
0c 91 00 04 00 04 00 00 27 00 00 00
01 91 00 0c 00 00 00 00
01 91 00 01 00 00 00 00
08 91 00 02 00 04 00 00 01 00 00 00
03 91 00 0a 00 04 00 00 05 00 00 00
03 91 00 0d 00 08 00 00 01 00 ff ff ff ff ff ff
```

### SPI Reads

HandHeldLegend's dumper reads 2 MiB total using 64-byte chunks over interface 1.
The SPI read command shape is:

```text
02 91 00 04 00 08 00 00 40 7e 00 00 <addr-le32>
```

Publicly referenced addresses include:

- `0x00013000` serial / identity block in VIIPER.
- `0x00013040`.
- `0x00013060`.
- `0x00013080` stick calibration in VIIPER.
- `0x000130c0` stick calibration in VIIPER.
- `0x00013100`.
- `0x001fc040`.
- `0x001fc080` in VIIPER.

### USB Haptics To A Real Controller

HandHeldLegend's haptic test uses HID output report ID `0x02`, not the bulk OUT
command path. It builds a 64-byte report:

- byte `0`: report ID `0x02`;
- byte `1`: `0x50 | (counter & 0x0f)`;
- byte `17`: same value as byte `1`;
- bytes `2..6`: left or first 5-byte haptic data;
- bytes `18..22`: right or repeated 5-byte haptic data;
- send interval: about 4 ms;
- zeroed haptic data stops rumble.

This is strong evidence for the real controller's PC-side rumble report shape.
It is not enough by itself to implement console-to-adapter rumble until the
adapter can receive the console's output report or bulk command.

## USB: Emulating A Switch 2 Pro Controller

VIIPER is the strongest public code source found for presenting as a Switch 2
Pro Controller USB device to a host. Treat it as host/SDL validated unless a
separate capture proves Switch 2 console validation.

### Device Identity And Endpoints

VIIPER defaults:

- VID/PID: `057e:2069`.
- Product string: `Switch 2 Pro Controller`.
- Manufacturer string: `Nintendo`.
- Serial string: `00`.
- `bcdDevice`: `0x0200`.
- Device class/subclass/protocol: `0xef`, `0x02`, `0x01`.
- Two interfaces:
  - interface `0`: HID class, interrupt IN endpoint `0x81`, interrupt OUT
    endpoint `0x01`, 64-byte packets, interval 4;
  - interface `1`: vendor class, bulk OUT endpoint `0x02`, bulk IN endpoint
    `0x82`.
- Microsoft OS 1.0 descriptor binds interface `1` to WinUSB on Windows.

VIIPER report IDs and sizes:

| Item | Value |
| --- | --- |
| Common input report ID | `0x05` |
| Pro input report ID | `0x09` |
| HID output report ID | `0x02` |
| Input report size | 64 bytes |
| Output report size | 64 bytes |
| Internal input state size | 24 bytes |
| Internal output state size | 34 bytes |
| Rumble payload | 16 bytes left + 16 bytes right |

### Report Descriptor

VIIPER's public test locks the HID report descriptor to these bytes:

```text
05 01 09 05 a1 01 85 05 05 ff 09 01 15 00 26 ff 00
95 3f 75 08 81 02 85 09 09 01 95 02 81 02 05 09 19
01 29 15 25 01 95 15 75 01 81 02 95 01 75 03 81 03
05 01 09 01 a1 00 09 30 09 31 09 33 09 35 26 ff 0f
95 04 75 0c 81 02 c0 05 ff 09 02 26 ff 00 95 34 75
08 91 02 85 02 09 01 95 3f 91 02 c0
```

### Input State

VIIPER's normalized input state:

- buttons: 32-bit bitfield;
- sticks: `LX`, `LY`, `RX`, `RY` as 12-bit values in `uint16`, center
  `0x0800`, range `0x0000..0x0fff`;
- motion: accel and gyro axes as signed 16-bit values.

VIIPER button bitfield:

| Bit | Button |
| --- | --- |
| `0` | B |
| `1` | A |
| `2` | Y |
| `3` | X |
| `4` | R |
| `5` | ZR |
| `6` | Plus |
| `7` | Right stick click |
| `8` | D-pad down |
| `9` | D-pad right |
| `10` | D-pad left |
| `11` | D-pad up |
| `12` | L |
| `13` | ZL |
| `14` | Minus |
| `15` | Left stick click |
| `16` | Home |
| `17` | Capture |
| `18` | GR |
| `19` | GL |
| `20` | C |
| `21` | Headset |

### Pro Report `0x09`

VIIPER's Pro report layout:

- byte `0`: report ID `0x09`;
- byte `1`: counter;
- byte `2`: power info;
- bytes `3..5`: button bytes;
- bytes `6..8`: left stick, 12-bit packed;
- bytes `9..11`: right stick, 12-bit packed;
- byte `12`: `0x38` when rumble is enabled, otherwise `0x30`.

VIIPER's Pro report button bytes mostly match `switch2bridge-macos`, but there
is a conflict around Capture/C:

- byte 3: B, A, Y, X, R, ZR, Plus, Right-stick-click;
- byte 4: D-pad down, right, left, up, L, ZL, Minus, Left-stick-click;
- byte 5: Home and rear/system buttons.

The conflict:

- `switch2bridge-macos` maps byte 5 bit `4` as Capture and does not identify C.
- VIIPER maps Capture to bit `1`, GR to bit `2`, GL to bit `3`, and C to bit
  `4`.

Resolve this with real controller samples before hardcoding Capture/C semantics
in HID Remapper.

### Common Report `0x05`

VIIPER's common report is richer and includes motion/battery fields:

- byte `0`: report ID `0x05`;
- bytes `1..4`: 32-bit counter;
- bytes `5..8`: button bytes;
- stick fields begin at bytes `11` and `14`;
- battery voltage appears at offset `0x20`;
- charging state appears at offset `0x22`;
- byte `0x2a`: constant `1`;
- when IMU is enabled, timestamp appears at `0x2b` and motion samples at
  `0x31..0x3d`.

### Bulk Command Responses

VIIPER's command responses use a response header:

```text
<cmd> 01 <seq> <sub> 10 78 00 00
```

VIIPER parses outbound bulk commands as:

- byte `0`: command;
- byte `2`: sequence;
- byte `3`: subcommand.

Implemented public behavior in VIIPER:

- command `0x02`, sub `0x01`: flash read response with 64-byte block payload;
- command `0x03`, sub `0x03`: enable/disable USB reports from byte `8`;
- command `0x03`, sub `0x0a`: select active report ID from byte `8`, accepting
  `0x05` or `0x09`;
- command `0x03`, sub `0x0d`: enable USB reports;
- command `0x09`, sub `0x07`: player LED mask from byte `8`;
- command `0x0c`: feature query/enable/disable flow.

VIIPER feature flags:

| Flag | Meaning |
| --- | --- |
| `0x01` | buttons |
| `0x02` | sticks |
| `0x04` | IMU |
| `0x10` | mouse |
| `0x20` | rumble |

### USB Rumble And LED Output

VIIPER's output state:

- 16 bytes left rumble;
- 16 bytes right rumble;
- flags byte;
- player LED mask byte.

HID OUT report ID `0x02` carries rumble. VIIPER copies two 16-byte rumble
payloads and emits a rumble update. It also accepts HID SetReport with report
ID `0x02`.

Player LED is handled over bulk command `0x09`, sub `0x07`; byte `8` is the LED
mask. A public VIIPER test verifies a mask such as `0x06`.

## Bluetooth: Reading A Real Controller As Input

`switch2bridge-macos` is the best public source for using the physical Switch 2
Pro Controller as a BLE input device.

### Discovery And Characteristics

- Bluetooth company ID: `0x0553`.
- Nintendo USB vendor ID in manufacturer data: `0x057e`, little-endian bytes
  `7e 05`.
- Product ID in manufacturer data: `0x2069`, little-endian bytes `69 20`.
- A 2026-06-11 local sniffer capture of the user's controller saw public
  advertiser address `3c:a9:ab:69:17:3d` using connectable `ADV_IND` and
  manufacturer payload:

```text
company 0x0553, data 01 00 03 7e 05 69 20 00 01 00 00 00 00 00 00 00 0f 00 00 00 00 00 00 00
```

  Interpreted with Zephyr's `BT_DATA_MANUFACTURER_DATA` buffer, the bytes begin
  `53 05 01 00 03 7e 05 69 20 ...`, so firmware should match company `0x0553`
  and then match VID/PID at offsets `5`/`7`.
- Input characteristic UUID:
  `7492866c-ec3e-4619-8258-32755ffcc0f9`.
- Output characteristic UUID:
  `7492866c-ec3e-4619-8258-32755ffcc0f8`.

The public bridge reads input notifications. Its author reports that LED/rumble
does not work yet and likely needs an unreversed handshake.

### BLE Input Packet Layout

Observed button bytes:

- `data[2]`
  - bit `0`: B
  - bit `1`: A
  - bit `2`: Y
  - bit `3`: X
  - bit `4`: R
  - bit `5`: ZR
  - bit `6`: Plus
  - bit `7`: right stick click
- `data[3]`
  - bit `0`: D-pad down
  - bit `1`: D-pad right
  - bit `2`: D-pad left
  - bit `3`: D-pad up
  - bit `4`: L
  - bit `5`: ZL
  - bit `6`: Minus
  - bit `7`: left stick click
- `data[4]`
  - bit `0`: Home
  - bit `2`: GR
  - bit `3`: GL
  - bit `4`: Capture according to `switch2bridge-macos`; see Capture/C conflict
    above.

Observed 12-bit stick packing:

```text
lx = data[5] | ((data[6] & 0x0f) << 8)
ly = ((data[6] & 0xf0) >> 4) | (data[7] << 4)
rx = data[8] | ((data[9] & 0x0f) << 8)
ry = ((data[9] & 0xf0) >> 4) | (data[10] << 4)
center = 2048
```

The bridge maps analog sticks to keyboard directions. It is not proof of exact
deadzone, calibration, motion, battery, or output behavior.

## Bluetooth: Impersonating A Controller To The Switch 2 Console

Leon is the best public writeup for the adapter-to-console BLE direction.

Important findings:

- Switch 2 Pro BLE is not normal HOGP. It uses two proprietary GATT services
  with 14 characteristics and specific handle numbers.
- Console commands are written to handle `0x0016`.
- Controller responses are notifications on handle `0x001a`.
- Activation is a deterministic 19-step sequence once pairing material exists.
- The accepted input report type is `0x20`, not the `0x0d` assumption from some
  earlier Switch controller work.
- An activation response is 16 bytes, not 9 bytes.
- Advertising data includes the console MAC in Nintendo manufacturer-specific
  data. This is involved in home-screen reconnect and wake/autoconnect.
- The console requests a BLE connection interval of 4 units, i.e. 5 ms. This is
  below the usual BLE minimum of 6 units, and public Zephyr/NimBLE stacks reject
  it unless their minimum-interval checks are patched.
- The unsolved blocker is command/subcommand `0x15`, which establishes or
  provides the LTK during first pairing. Publicly demonstrated BLE console
  impersonation sidesteps this by extracting a donor controller's MAC and LTK
  after it has already paired with the console.

Implication for HID Remapper:

- BLE console wake/autoconnect is probably not a clean first milestone unless
  the project accepts donor controller pairing material.
- Without solving `0x15`, a fresh board cannot pair to an arbitrary Switch 2
  console as a brand-new Pro Controller 2 over BLE.
- The Seeed Xiao nRF52840 target may need BLE stack patches to accept the
  console's 5 ms interval.

MissionControl's public issue also supports this direction: its maintainer
reported that Switch 2 controllers required BLE code changes because existing
code assumed standard HID profile behavior. The issue later mentions PoC USB and
BLE input support in test builds, but detailed implementation is not public in
the issue.

## Current HID Remapper Gaps

Repository facts to keep in mind:

- `firmware/src/our_descriptor.cc` has descriptor index `6` labelled Switch Pro
  Controller with VID/PID `057e:2009`, which is Switch 1 Pro.
- `firmware-bluetooth/src/main.cc` contains experimental Switch Pro-looking code
  and `0x91` bytes, but branch history around it names Switch 1 experiments.
  Do not assume it is Switch 2-ready.
- The Bluetooth firmware is BLE-only. It cannot use Bluetooth Classic.
- The existing BLE input path is HID/HOGP-oriented; Switch 2 Pro input needs a
  custom GATT client path for the proprietary characteristic.
- The existing USB device output path is HID interrupt-oriented; Switch 2 USB
  console output likely needs a two-interface device with interface 1 bulk
  endpoints and command responses.
- Descriptor indices are persisted in user config. Do not casually replace
  descriptor index `6` if it would break existing Switch 1 users. Prefer a new
  Switch 2 output descriptor/mode unless compatibility is deliberately handled.

## First Firmware Pass On This Branch

The first implementation pass adds a separate Switch 2 Pro output descriptor at
index `7` with VID/PID `057e:2069`, report IDs `0x05`, `0x09`, and `0x02`, and
VIIPER-derived command plumbing in `firmware-bluetooth/src/main.cc`.

What this pass attempts:

- scan for Nintendo manufacturer data containing Switch 2 Pro PID `0x2069`;
- connect to the controller as a custom BLE device instead of HOGP;
- discover input characteristic `7492866c-ec3e-4619-8258-32755ffcc0f9`;
- discover output characteristic `7492866c-ec3e-4619-8258-32755ffcc0f8`;
- subscribe to input notifications and pack the public 12-bit report layout into
  host-facing report `0x09`;
- respond to a subset of VIIPER-style USB setup commands over the existing HID
  OUT path: flash reads, report selection, feature commands, LED command, and
  HID rumble report `0x02`;
- forward raw output writes to the controller output characteristic as an
  experimental rumble/LED path.

Important limitation:

- The Seeed nRF52840 Zephyr firmware is still configured as two HID interfaces.
  It does **not** yet expose the Switch 2 Pro vendor bulk interface 1 with bulk
  OUT endpoint `0x02` and bulk IN endpoint `0x82`. The first pass reuses the
  existing HID OUT transport to exercise command plumbing. A later USB class or
  descriptor-layer change is needed before this can accurately match the public
  VIIPER/real-controller USB interface shape.

## Suggested Implementation Milestones

1. **Documentation and capture tooling**
   - Keep this file updated with new captures.
   - Add scripts or debug logging only if they are intentionally part of the
     capture workflow.
2. **BLE input from physical Switch 2 Pro Controller**
   - Scan for Nintendo manufacturer data with PID `0x2069`.
   - Connect without requiring HIDS UUID.
   - Discover or directly bind the proprietary input characteristic.
   - Subscribe to `7492866c-ec3e-4619-8258-32755ffcc0f9`.
   - Parse button/stick reports into HID Remapper usages.
   - Leave rumble-to-controller disabled until the output handshake is known.
3. **USB output mode for Switch 2 console**
   - Add a new Switch 2 Pro output descriptor/mode using VID/PID `057e:2069`.
   - Model the two-interface endpoint layout from public VIIPER evidence.
   - Implement interface 1 bulk command handling for init, SPI reads, feature
     commands, report selection, and LED updates.
   - Emit report `0x09` first unless captures show the console wants `0x05`.
   - Receive HID OUT report `0x02` for rumble and translate it into HID
     Remapper's output-rumble path.
4. **Rumble passthrough**
   - Capture or reverse the real controller BLE output handshake.
   - Translate console rumble payloads to the real controller only after the BLE
     output characteristic accepts writes.
5. **Wake/autoconnect**
   - Decide whether donor MAC/LTK extraction is acceptable.
   - If yes, implement storage and BLE advertising carefully.
   - If no, treat wake as blocked until `0x15` first-pairing key exchange is
     solved publicly or locally.

## Open Questions

- Full BLE GATT service UUIDs and exact handle order exposed by a real Switch 2
  Pro Controller.
- Raw BLE input report samples for every button, especially Capture vs C, GL,
  GR, sticks, battery, and motion bytes.
- Exact BLE output initialization sequence for LED and rumble on the physical
  controller.
- Whether a Switch 2 console accepts the VIIPER USB descriptor/command behavior
  unchanged.
- Whether the console prefers report `0x09`, report `0x05`, or switches between
  them during setup.
- Exact USB console haptic output bytes and how they correspond to
  HandHeldLegend/VIIPER rumble fields.
- Whether wired USB connection to the dock can wake the console, or whether
  wake requires BLE paired-controller advertising.
- Whether Switch 2's 5 ms BLE interval can be supported cleanly in the
  nRF52840/Zephyr stack used by HID Remapper.

## Source Index

- https://github.com/mlstr0m/switch2bridge-macos
- https://leonsnotes.ca/2026/04/04/reverse-engineering-the-switch-2-pro-controllers-bluetooth-protocol/
- https://switchbrew.org/wiki/Switch_2%3A_Pro_Controller
- https://switch2brew.org/wiki/Pro_Controller
- https://handheldlegend.github.io/procon2tool/
- https://github.com/ikz87/NSW2-controller-enabler
- https://www.spinics.net/lists/linux-input/msg101879.html
- https://www.spinics.net/lists/linux-input/msg102062.html
- https://github.com/ndeadly/MissionControl/issues/959
- https://github.com/darthcloud/BlueRetro/issues/1249
- https://github.com/Alia5/SISR
- https://github.com/Alia5/VIIPER
- https://en-americas-support.nintendo.com/app/answers/detail/a_id/68527/~/nintendo-switch%26nbsp%3B2-pro-controller-diagram
- https://joypad.ai/controllers/nintendo-switch-2-pro-controller
