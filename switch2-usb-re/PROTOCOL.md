# Nintendo Switch 2 Pro Controller — Everything We Know About the USB Protocol

> **Status as of 2026-06-16.** Captured via a GreatFET One running Facedancer
> as a USB MITM between a real Switch 2 console and a real Switch 2 Pro
> Controller. 11 independent sessions captured, each producing a chronological
> raw hex log. All byte-exact evidence below is from those logs unless noted.
> Confidence: **HIGH** = repeated, exceptionless; **MEDIUM** = plausible,
> partially evidenced; **LOW** = speculative.

---

## Table of Contents

1. [Device Identity](#1-device-identity)
2. [USB Descriptor Structure](#2-usb-descriptor-structure)
3. [Interface Layout](#3-interface-layout)
4. [HID Input Report (Interface 0, EP 0x81 IN)](#4-hid-input-report-interface-0-ep-0x81-in)
5. [HID Output Report (Interface 0, EP 0x01 OUT)](#5-hid-output-report-interface-0-ep-0x01-out)
6. [Vendor Bulk Protocol (Interface 1, EP 0x02 OUT / 0x82 IN)](#6-vendor-bulk-protocol-interface-1-ep-0x02-out--0x82-in)
7. [Wake-from-Sleep Mechanism](#7-wake-from-sleep-mechanism)
8. [Authentication and Encryption](#8-authentication-and-encryption)
9. [What's Still Unknown](#9-whats-still-unknown)
10. [Capture Setup and Files](#10-capture-setup-and-files)

---

## 1. Device Identity

| Field | Value |
|---|---|
| USB VID | `0x057E` (Nintendo) |
| USB PID | `0x2069` |
| bcdDevice | `0x0201` |
| bDeviceClass | `0xEF` (Miscellaneous) |
| bDeviceSubClass | `0x02` |
| bDeviceProtocol | `0x01` (IAD signaling — device uses Interface Association Descriptors) |
| bMaxPacketSize0 | 64 |
| iManufacturer | "Nintendo" |
| iProduct | "Switch 2 Pro Controller" |
| iSerialNumber | "00" (placeholder — real serial number retrieved via a vendor control request; see §2) |
| bNumConfigurations | 1 |

**Raw device descriptor (18 bytes):**
```
12 01 00 02 ef 02 01 40 7e 05 69 20 01 02 01 02 03 01
```

---

## 2. USB Descriptor Structure

The configuration descriptor is 268 bytes total. The complete byte-exact parse follows.
If anything in prose below conflicts with this table, the table wins.

### 2.1 Configuration descriptor header

```
09 02 0c 01 05 01 04 c0 fa
```
- wTotalLength = 268
- bNumInterfaces = 5
- bConfigurationValue = 1
- iConfiguration = 4 ("Config_0")
- bmAttributes = 0xC0 (self-powered, no remote wakeup)
- bMaxPower = 250 → 500 mA

### 2.2 Complete descriptor table (every descriptor in order)

| Offset | Len | Descriptor type | Key fields |
|---|---|---|---|
| 0 | 9 | CONFIGURATION | (see above) |
| 9 | 8 | **IAD #1** | bFirstInterface=**0**, bInterfaceCount=**1**, bFunctionClass=0x03 (HID) |
| 17 | 9 | INTERFACE 0, alt 0 | class=0x03 HID, 2 endpoints, iInterface=5 ("If_Hid") |
| 26 | 9 | HID class descriptor (0x21) | bcdHID=0x0111, bCountryCode=0, bNumDescriptors=1, bDescriptorType=0x22 (Report), wDescriptorLength=97 |
| 35 | 7 | ENDPOINT 0x81 | Interrupt IN, maxPacket=64, interval=4ms |
| 42 | 7 | ENDPOINT 0x01 | Interrupt OUT, maxPacket=64, interval=4ms |
| 49 | 8 | **IAD #2** | bFirstInterface=**1**, bInterfaceCount=**1**, bFunctionClass=0xFF (Vendor) |
| 57 | 9 | INTERFACE 1, alt 0 | class=0xFF Vendor, 2 endpoints, iInterface=6 ("Switch 2 Pro Controller") |
| 66 | 7 | ENDPOINT 0x02 | Bulk OUT, maxPacket=64 |
| 73 | 7 | ENDPOINT 0x82 | Bulk IN, maxPacket=64 |
| 80 | 8 | **IAD #3** | bFirstInterface=**2**, bInterfaceCount=**3**, bFunctionClass=0x01 (Audio) — covers interfaces 2, 3, and 4 as one audio function |
| 88 | 9 | INTERFACE 2, alt 0 | class=0x01 Audio, subclass=0x01 AudioControl, 0 endpoints |
| 97–167 | varies | 7× CS_INTERFACE (0x24) | Audio class-specific descriptors — decoded byte-exact in `references/usb-descriptors.md`, not fully field-decoded here |
| 168 | 9 | INTERFACE 3, alt 0 | class=0x01 Audio, subclass=0x02 AudioStreaming, 0 endpoints |
| 177 | 9 | INTERFACE 3, alt 1 | class=0x01 Audio, subclass=0x02 AudioStreaming, 1 endpoint |
| 186 | 7 | CS_INTERFACE (0x24) | |
| 193 | 11 | CS_INTERFACE (0x24) | sample rate 48000 Hz (0x0000BB80) |
| 204 | 7 | ENDPOINT 0x03 | Isochronous OUT, maxPacket=192, interval=1ms |
| 211 | 7 | CS_ENDPOINT (0x25) | |
| 218 | 9 | INTERFACE 4, alt 0 | class=0x01 Audio, subclass=0x02 AudioStreaming, 0 endpoints |
| 227 | 9 | INTERFACE 4, alt 1 | class=0x01 Audio, subclass=0x02 AudioStreaming, 1 endpoint |
| 236 | 7 | CS_INTERFACE (0x24) | |
| 243 | 11 | CS_INTERFACE (0x24) | sample rate 48000 Hz |
| 254 | 7 | ENDPOINT 0x83 | Isochronous IN, maxPacket=192, interval=1ms |
| 261 | 7 | CS_ENDPOINT (0x25) | |

**Total: 268 bytes.** ✓

### 2.3 IAD grouping (HIGH confidence)

Three IADs, all placed immediately before the interface(s) they cover:

| IAD | bFirstInterface | bInterfaceCount | Covers |
|---|---|---|---|
| #1 | 0 | 1 | Interface 0 (HID) alone |
| #2 | 1 | 1 | Interface 1 (Vendor) alone |
| #3 | 2 | **3** | Interfaces 2, 3, **and** 4 together — one audio function (AudioControl + 2×AudioStreaming) |

An earlier analysis incorrectly described IAD #3 as two separate IADs covering
"2+3" and "4" separately. The raw hex at offset 80 is unambiguous: `080b020301010000`
— `bInterfaceCount=3`. The three-IAD reading matches the class-structure too (one
AudioControl unit plus two AudioStreaming alts, one per direction).

### 2.4 String descriptors

| Index | String |
|---|---|
| 1 | "Nintendo" |
| 2 | "Switch 2 Pro Controller" |
| 3 | "00" (placeholder serial) |
| 4 | "Config_0" |
| 5 | "If_Hid" |
| 6 | "Switch 2 Pro Controller" |

### 2.5 Per-unit serial number (MEDIUM confidence)

During enumeration a vendor/class control transfer (`bmRequestType=0xA1`, direction IN)
returned a 64-byte block whose bytes 2–15 decode as ASCII `HEW70006169780` — plausibly
the real per-unit serial number. The full `SETUP` fields (bRequest/wValue/wIndex) were
not captured for this transfer. The "00" in string descriptor 3 is a placeholder.

---

## 3. Interface Layout

| Interface | Class | Purpose | Endpoints used |
|---|---|---|---|
| 0 | HID (0x03) | Main controller input (buttons/sticks/IMU) and output (rumble) | EP 0x81 IN (interrupt, 64B, 4ms poll), EP 0x01 OUT (interrupt, 64B, 4ms) |
| 1 | Vendor (0xFF) | Command/response control channel — handshake, calibration reads, keepalive | EP 0x02 OUT (bulk, 64B), EP 0x82 IN (bulk, 64B) |
| 2 | Audio/AudioControl | Microphone passthrough — not relevant to button/stick input | — (control-only interface) |
| 3 | Audio/AudioStreaming | Mic audio stream OUT (to console) — alt 1 activates iso endpoint | EP 0x03 Isochronous OUT (maxPacket=192, alt 1 only) |
| 4 | Audio/AudioStreaming | Mic audio stream IN (from console?) | EP 0x83 Isochronous IN (maxPacket=192, alt 1 only) |

**Console behavior on connect:**
1. Enumerates (requests device + config descriptors).
2. Immediately starts vendor bulk handshake on interface 1 (see §6).
3. Issues `SET_INTERFACE` requests for the audio interface alternate settings
   (sets alts on interfaces 3 and 4). This is expected behavior and must be
   accepted by any emulator — it's harmless to ignore for input-only use.
4. Begins polling EP 0x81 IN at the 4ms rate for controller input.

**IAD parser gotcha:** the first IAD appears in the config descriptor *before*
any `INTERFACE` descriptor has been parsed (it's at offset 9, before interface 0
at offset 17). Parsers that assume `last_interface` is non-None when they
encounter a class-specific descriptor will crash. Facedancer has this bug; it's
patched in `proxy.py`. Write your own parser defensively.

---

## 4. HID Input Report (Interface 0, EP 0x81 IN)

**64 bytes, report ID `0x09`, sent continuously at 4ms intervals.**

Structure derived from per-byte statistical analysis (variance + transition
count) across a ~30,000-packet capture covering all buttons. Byte positions and
byte count confirmed HIGH; bit assignments within bytes 3/4/5 still need
single-button capture.

| Offset | Bytes | Field | Confidence | Notes |
|---|---|---|---|---|
| 0 | 1 | Report ID | HIGH | Always `0x09` |
| 1 | 1 | Sequence counter | HIGH | Wraps 0x00–0xFF, increments every packet |
| 2 | 1 | Reserved | HIGH | Always `0x00` in all captures |
| 3 | 1 | Button byte A | HIGH | See bit table below — ABXY, R, ZR, 2 new bits |
| 4 | 1 | Button byte B | HIGH | See bit table below — −/+, L3/R3, Home, Capture, 2 new bits |
| 5 | 1 | Button byte C | HIGH | See bit table below — D-pad, L, ZL |
| 6–11 | 6 | Analog stick data | MEDIUM | Continuously varying; likely L/R stick X/Y axes; exact packing (8-bit per axis vs 12-bit packed) not confirmed |
| 12–15 | 4 | Unknown | MEDIUM | Low-variance; may be additional button/pad data or fixed |
| 16+ | 48 | IMU / gyro / accel | LOW | High-entropy, rapidly changing; consistent with 6-axis IMU subsampled multiple times per report (matches the original Switch Pro Controller's 3× IMU per-report scheme). Not decoded. |

### Button bit table (bytes 3/4/5) — HIGH confidence

Confirmed by a direct interactive single-button-at-a-time capture session —
every button was pressed individually and identified from the single-bit change
it produced in the live log. **The Switch 2 Pro Controller uses a completely
different bit layout from the original Switch Pro Controller.** Do not apply
the original mapping — it is wrong.

The Switch 2 organizes buttons by controller side: byte 3 = right side,
byte 4 = left side, byte 5 = special/new.

**Byte 3 (offset 3) — right-side buttons:**

| Bit | Mask | Button |
|---|---|---|
| 0 | `0x01` | B |
| 1 | `0x02` | A |
| 2 | `0x04` | Y |
| 3 | `0x08` | X |
| 4 | `0x10` | R |
| 5 | `0x20` | ZR |
| 6 | `0x40` | + (Plus) |
| 7 | `0x80` | R3 (right stick click) |

**Byte 4 (offset 4) — left-side buttons:**

| Bit | Mask | Button |
|---|---|---|
| 0 | `0x01` | D-pad Down |
| 1 | `0x02` | D-pad Right |
| 2 | `0x04` | D-pad Left |
| 3 | `0x08` | D-pad Up |
| 4 | `0x10` | L |
| 5 | `0x20` | ZL |
| 6 | `0x40` | − (Minus) |
| 7 | `0x80` | L3 (left stick click) |

**Byte 5 (offset 5) — special/new buttons:**

| Bit | Mask | Button |
|---|---|---|
| 0 | `0x01` | Home |
| 1 | `0x02` | Capture |
| 2 | `0x04` | GR (right back grip — new Switch 2 button) |
| 3 | `0x08` | GL (left back grip — new Switch 2 button) |
| 4 | `0x10` | Chat (new Switch 2 button) |
| 5–7 | — | Unknown (not observed) |

**Relationship to original Switch Pro Controller format:** The original Pro
Controller uses 49-byte HID reports with report ID `0x30`. The Switch 2 Pro
Controller uses 64 bytes with report ID `0x09`. The byte count and report ID
both differ, and the button bit layout is completely reorganized — do not
reuse the original parser. Start fresh from the field table above.

**What's still needed to complete this section:**
- Slow full-range stick sweeps (one axis at a time) to determine 8-bit vs
  12-bit encoding in bytes 6–11.
- Decode bytes 12–15 (low-variance, may be additional buttons or fixed).

---

## 5. HID Output Report (Interface 0, EP 0x01 OUT)

**Purpose: rumble/haptic. Status: ambiguous capture, needs focused re-capture.**

Two distinct kinds of non-empty payloads observed on this endpoint, and they
are inconsistent with each other:

**Sample A** (len=128, does NOT start with 0x09):
```
02 60 81 01 10 1e 00 81 01 10 1e 00 00 00 00 00 00 60 81 01 10 1e 00 81 01 10 1e 00 00 00 00 00 ...
```
The repeating `60 81 01 10 1e 00` quad pattern is structurally consistent with
HD-Rumble-style waveform encoding used on the original Joy-Con/Pro Controller
(repeated per-motor frequency/amplitude quads). This is the more credible "real
rumble command" sample.

**Sample B** (len=192 or 256, starts with `0x09` and an incrementing counter byte):
```
09 90 20 00 00 00 94 07 85 3f 28 83 38 00 00 1e 0c 40 00 0c 00 20 dc f9 02 40 8d 10 01 da f7 73 ...
```
This closely resembles the **input report format** seen on EP 0x81 IN (same
report ID `0x09`, same counter byte structure, same `86/87 xx xx xx 83 3x`
patterns). This is suspicious — it may be a MITM pipeline artifact where input
data is mis-attributed to the `filter_out` handler, rather than real
host-to-device traffic.

**Do not treat Sample B as authoritative** without independently verifying it
(e.g. with a hardware USB analyzer, or by auditing the proxy pipeline).

**Next step:** run a focused capture around a known rumble event (console
connect-rumble, or a game's explicit rumble trigger) and verify which sample
type correlates with it.

---

## 6. Vendor Bulk Protocol (Interface 1, EP 0x02 OUT / 0x82 IN)

This is a clean, fully request/response, sequence-numbered protocol. **HIGH
confidence** on the framing and the subcommand catalogue (second derivation,
verified per-session independently).

### 6.1 Request format (EP 0x02 OUT)

```
byte  0:  sequence number — increments per request from 0x00, wraps at 0xFF
byte  1:  0x91 — fixed "request" opcode
byte  2:  0x00 — always zero
byte  3:  subcommand ID
byte  4:  0x00 — always zero
byte  5:  expected-response-size class — correlated with subcommand, not with
           the actual request payload length (e.g. 0x00 = empty/small response,
           0x04 = 4-byte response, 0x08 = 8-byte arg, 0x11 = 17-byte arg, etc.)
bytes 6+: subcommand argument payload (zero-padded to fill the bulk packet)
```

### 6.2 Response format (EP 0x82 IN)

```
byte  0:  sequence number — echoes the request's byte 0
byte  1:  0x01 — fixed "response/ack" opcode
byte  2:  0x00 — always zero
byte  3:  subcommand ID — echoed from request
byte  4:  0x00 — always zero
byte  5:  0xf8 — constant across every observed response; purpose unclear;
           possibly a protocol-version marker or fixed status flag
bytes 6+: response payload, length varies by subcommand (may be zero)
```

**Continuation packets:** responses longer than one 64-byte bulk transfer
continue in a second IN packet with no header — just raw continuation bytes
starting where the first packet left off. This has been observed for
subcommands `0x01`, `0x02`, and `0x04`. **Do not parse the second IN packet
as if it has a header** — an earlier analysis was corrupted exactly this way,
producing three phantom subcommands (`0x10`, `0x11`, `0x18`) from
mis-attributed continuation fragments. Those three never appeared as genuine
OUT requests in any of the 11 logs and have been removed from the table.

### 6.3 Subcommand catalogue (9 verified subcommands)

Derived by walking each of the 11 capture logs independently, pairing every
`EP OUT 0x02` request (`byte[1]==0x91`) with the IN packet(s) up to the next
OUT. The complete, verified set of subcommand IDs actually observed as
genuine requests: **`0x01, 0x02, 0x03, 0x04, 0x07, 0x08, 0x0a, 0x0c, 0x0d`**.
No other ID was ever observed as a genuine request.

---

#### Subcommand `0x0d` — Session handshake/init (MEDIUM confidence)

Always the **first** command sent on connect. The argument (bytes 6+) contains
a 6-byte value that looks like a timestamp or session token (e.g. `e1 39 65 05
48 c8`, seen consistently as bytes 10–15 in the frame with argument header
`01 00`). The response is always `01 00 00 00` (4 bytes).

```
OUT: 03 91 00 0d 00 08 00 00 01 00 e1 39 65 05 48 c8
IN:  03 01 00 0d 00 f8 00 00 01 00 00 00
```

---

#### Subcommand `0x04` — Address-style memory/calibration read (HIGH confidence)

**8-byte argument shape:** The first 4 argument bytes form a little-endian address
(values seen: `0x00007e10`, `0x00007e18`, `0x00007e20`, `0x00007e40`), consistent
with a "read N bytes from flash address X" operation. The response is byte-identical
across all 6 independent sessions for the same argument — this is the strongest
possible evidence for a deterministic, address-indexed flash read.

**Response echo rule:** the first 8 bytes of the response payload repeat the request
argument, **except byte index 1 is always zeroed** (e.g. argument `40 7e 00 00 80 30
01 00` → response prefix `40 00 00 00 80 30 01 00`). This is exceptionless across all
6 pairs. Byte 1 (`0x7e`) is likely a bank/category selector that is intentionally not
echoed back.

All 6 verified argument/response pairs (response bytes after the 8-byte echo prefix):

| Arg bytes 6–13 | Address (LE) | Response data (after 8-byte echo prefix) | Interpretation |
|---|---|---|---|
| `40 7e 00 00 80 30 01 00` | 0x00007e40 | `01 ad d9 9a 55 56 65 a0 00 0a a0 00 0a e2 20 0e ...` (+ 16-byte continuation) | Repeating triplets characteristic of packed 12-bit calibration values — likely **analog stick calibration** |
| `40 7e 00 00 c0 30 01 00` | 0x00007e40 | `01 ad d9 9a 55 56 65 a0 00 0a a0 00 0a e2 20 0e ...` (different trailing bytes) | Second stick calibration page |
| `40 7e 00 00 40 c0 1f 00` | 0x00007e40 | `01 ff ff ff...` (all 0xFF) | Unprogrammed/erased flash region |
| `10 7e 00 00 40 30 01 00` | 0x00007e10 | `01 e8 c1 ca 41 bf fe d9 3a 8a 67 14 bb 5c 14 32 bb` (16 bytes, opaque) | Different data region |
| `18 7e 00 00 00 31 01 00` | 0x00007e18 | `01 00 00 00 00 ... 4d 41 4e 3d c6 3a ...` | Contains ASCII `4d 41 4e` = "MAN" — possibly a manufacturing/model string fragment |
| `20 7e 00 00 60 30 01 00` | 0x00007e20 | `01 ff ff ff...` (all 0xFF) | Unprogrammed flash region |

**17-byte argument shape — surprising verified finding:** When called with a
17-byte argument (byte 5 = `0x11`), the response is `01 5c f6 ee 79 2c df 05
e1 ba 2b 63 25 c4 1a 5f 10` — byte-identical across all 6 sessions, even though
the argument itself is different every session. Verified line-by-line in the raw
logs (not a pairing artifact). Either the argument bytes are ignored in producing
this particular response, or this is a fixed-placeholder reply path.

```
OUT: 15 91 00 04 00 11 00 00 00 0d 2b 3d c0 f0 f6 ec ba 3c 6f 73 ec b7 92 39 db
IN:  15 01 00 04 00 f8 00 00 01 5c f6 ee 79 2c df 05 e1 ba 2b 63 25 c4 1a 5f 10
```

---

#### Subcommand `0x02` — Nonce exchange + idle keepalive (HIGH for keepalive, LOW for first-call purpose)

Two structurally different call shapes:

**First call per session (17-byte argument):** the argument looks random/session-variable
and the response is a different 16-byte payload every session. Consistent with a
nonce or challenge/response exchange.

```
OUT: 15 91 00 02 00 11 00 00 00 68 7b 10 6e 4c ad da 73 03 19 6b ef 27 e6 95 6d
IN:  15 01 00 02 00 f8 00 00 01 fc 9f 34 d4 81 75 40 18 43 72 4e 41 70 8d 0f 0a
```
(this exact response differs across sessions — not a fixed value)

**Steady-state keepalive (4-byte zero argument):** by far the most frequent bulk
exchange during an idle connection. Seen 50+ times in one session. Both request
and response have empty/minimal payload.

```
OUT: 0a 91 00 02 00 04 00 00 00 00 00 00
IN:  0a 01 00 02 00 f8 00 00
```
Note: byte 0 (`0x0a`) here is the **sequence number**, not a subcommand ID.
The subcommand byte is always byte 3 (`0x02`). An earlier version of this document
incorrectly identified `0x0a` as the keepalive subcommand — it is not.

---

#### Subcommand `0x01` — Flag/state query (LOW confidence — three distinct call shapes)

```
OUT: 07 91 00 01 00 00 00 00
IN:  07 01 00 01 00 f8 00 00 00        ← 1-byte payload: 0x00

OUT: 15 91 00 01 00 0e 00 00 00 02 e1 39 65 05 48 c8 e0 39 65 05 48 c8
IN:  15 01 00 01 00 f8 00 00 01 04 01 3d 17 69 ab a9 3c  ← 8-byte payload

OUT: 11 91 00 01 00 00 00 00
IN:  11 01 00 01 00 f8 00 00 03 00 00 00  ← 3-byte payload: 0x03 0x00 0x00
```

Three distinct call shapes suggests this may be a multipurpose query that takes
different sub-modes via byte 5 (the "expected-response-size class" field). Not
yet understood.

---

#### Subcommand `0x03` — Flag/state query (LOW confidence — two call shapes)

```
OUT: 15 91 00 03 00 01 00 00 00
IN:  15 01 00 03 00 f8 00 00 01  ← 1-byte payload: 0x01

OUT: 11 91 00 03 00 00 00 00
IN:  11 01 00 03 00 f8 00 00 01 c0 03 00 00 e7 d0 1c 3b 79 22 a0 3a 0a e8 9c 42 58 a0 0b 42 0a e8 9c 41 58 a0 0b 41  ← 24-byte payload
```

---

#### Subcommand `0x07` — Keepalive/ready ping (MEDIUM confidence)

Simple ping with empty response. Called periodically. Argument varies between
all-zeros and `01 00 00 00`.

```
OUT: 0b 91 00 07 00 04 00 00 00 00 00 00
IN:  0b 01 00 07 00 f8 00 00
```

---

#### Subcommand `0x08` — Write/set command (LOW confidence)

Seen once per session. The argument contains a bitmask that is mostly `0xff` bytes,
plus some smaller values (`35 00 46 00`). Consistent with writing a filter, mask, or
configuration table. Response is empty.

```
OUT: 0a 91 00 08 00 14 00 00 01 ff ff ff ff ff ff ff ff 35 00 46 00 00 00 00 00 00 00 00
IN:  0a 01 00 08 00 f8 00 00
```

---

#### Subcommand `0x0a` — Rare set/config command (LOW confidence)

Seen 5 times in one session, always this exact exchange. This is **not** the idle
keepalive — that is subcommand `0x02`. The two were confused in an earlier draft
of this document because the sequence number `0x0a` made a request to subcommand
`0x02` look like it was for subcommand `0x0a` at a glance.

```
OUT: 03 91 00 0a 00 04 00 00 09 00 00 00
IN:  03 01 00 0a 00 f8 00 00
```

---

#### Subcommand `0x0c` — Identifier/version read (MEDIUM confidence)

Two distinct call shapes. The second shape returns `61 12 50 10` — reproduced
byte-identically across 4 independent sessions and is very likely a fixed firmware
version or hardware identifier.

```
OUT: 03 91 00 0c 00 04 00 00 01 00 00 00
IN:  03 01 00 0c 00 f8 00 00                  ← empty response

OUT: 01 91 00 0c 00 00 00 00
IN:  01 01 00 0c 00 f8 00 00 61 12 50 10      ← 4-byte fixed value
```

Note: an earlier analysis pass temporarily removed this second shape as
"unreproducible," which was incorrect. Re-verified in the raw logs — it appears
in 4 independent sessions with byte-identical result. It's real.

---

#### Subcommand `0x0d` — Session init

(Documented above as the first subcommand seen on connect.)

---

### 6.4 Typical connection sequence

Based on the 11 captured sessions, the console sends subcommands in roughly this
order on every new connection:

1. `0x0d` — session init (first, always, with timestamp-like argument)
2. `0x04` — calibration read (8-byte arg, address 0x00007e40 and adjacent)
3. `0x04` — calibration read (17-byte arg)
4. `0x02` — nonce exchange (17-byte arg, first-call shape)
5. `0x01`, `0x03` — state queries (various shapes)
6. `0x08` — write/set configuration
7. `0x0c` — identifier read
8. `0x07` — ready ping
9. `0x02` — idle keepalive (repeats indefinitely, ~every second)

The exact ordering and which call shapes appear varies slightly across sessions.
Only `0x02` (keepalive) continues throughout the session; everything else is
one-shot at init.

---

## 7. Wake-from-Sleep Mechanism

**Finding: USB wake does not work. The wake mechanism is BLE, not USB. (HIGH confidence)**

We ran three experiments trying to trigger console wake via USB:

1. `emulate_static.py` — pure GreatFET emulation, no real controller, no subcommand
   responder. Console never issued a bus reset or any USB activity while asleep.
2. `wake_test.py` — GreatFET with a canned vendor bulk handshake responder, cycling
   through 8 candidate Home-button-held HID report patterns every 4 seconds.
   Zero USB bus activity from the console while asleep.
3. **Real controller through the fully-connected proxy** — let the console fall asleep
   naturally, then pressed Home on the real controller. Zero new lines appeared in the
   capture log.

**Explanation:** the Switch 2's Bluetooth radio stays in a low-power scanning state
during sleep, listening specifically for the BLE wake-up advertising packet broadcast
by official Switch 2 peripherals. Holding Home causes the controller to broadcast this
packet **over the air, regardless of whether it is also plugged in via USB**. The USB
interface receives no wake signal. This is why all three experiments failed: we were
instrumenting the wrong transport.

**Implication for USB-only projects:** a device sitting between a wired controller
and the console via USB cannot replicate Home-button wake. Reproducing wake requires
a BLE radio in the loop (e.g. an nRF52840 that can sniff and replay the wake advertising
packet), which is a separate project entirely.

---

## 8. Authentication and Encryption

**Status: open question — no encryption key identified, nonce pattern suggests per-session auth.**

### What we observe

The first call to subcommand `0x02` per session carries a 17-byte argument that
appears random/different every session (e.g. `68 7b 10 6e 4c ad da 73 03 19 6b
ef 27 e6 95 6d`). The response is also different every session (e.g. `fc 9f 34
d4 81 75 40 18 43 72 4e 41 70 8d 0f 0a`). This is consistent with a
**challenge/response or nonce exchange**, not a static key — the console and
controller are likely computing a session-derived value.

### What we don't know

- Whether the console rejects non-matching responses (i.e. whether auth is
  enforced at all for ongoing input). It might just be a handshake for pairing
  bookkeeping rather than an encryption gate on button data.
- The exact algorithm. The 17-byte challenge and 16-byte response don't obviously
  match a well-known HMAC-SHA256 / AES-CMAC / elliptic curve scheme by length alone.

### The BLE bonding LTK is the real pairing key

The "controller paired to this specific console" relationship is almost certainly
stored as a **BLE Long Term Key (LTK)** from the initial wireless pairing/bonding
process. This is:
- **Not visible in our USB captures** (it was exchanged over BLE, not USB).
- Stored in the console's BLE bond database and the controller's NVM.
- The mechanism by which the console "knows" this specific controller — it is the
  cryptographic root of the pairing.

**To capture the LTK:** requires a BLE sniffer during the initial pairing ceremony
(e.g. a Wireshark session using a Nordic nRF52840 USB dongle with the `BTLE`
dissector, or a Ubertooth) with LE legacy or secure pairing keys intercepted.
This is a separate investigation from the USB MITM work.

### Testing whether USB auth is enforced

A faster approach (without BLE sniffing): build an emulator that responds to
the `0x02` first-call with a random/fixed 16-byte response, and observe whether
the console continues sending input polls normally. If HID input reports
continue being read, auth is not enforced on the USB side. If the console drops
the connection or stops polling, auth is enforced and the response must match a
computed value.

---

## 9. What's Still Unknown

In rough priority order:

1. **Stick encoding** in bytes 6–11: whether it's 2× 8-bit (simple) or 2× 12-bit
   packed (like the original Pro Controller). Requires slow full-range axis sweeps.

2. **Rumble command format** on EP 0x01 OUT: the Sample A pattern is plausible
   but unverified. Needs a capture timed with a known rumble event.

3. **IMU layout** in input report bytes ~16+: which bytes are accelerometer vs.
   gyro, byte order, scale. Needs still-vs.-shaken comparison.

4. **Subcommand `0x01` and `0x03` purpose**: they have multiple call shapes and the
   responses vary. Systematic argument variation would help.

5. **Whether USB auth is enforced**: one targeted test (emulate with wrong `0x02`
   response, see if the console stops polling) resolves this without BLE sniffing.

6. **The subcommand `0x04` 17-byte response**: why does a variable argument produce
   an identical response? Test whether changing byte 5 (`0x11`) or bytes 7+ produces
   a different response.

7. **Byte 5 bits 4–7**: always observed as zero; unknown if unused or reserved.

8. **BLE wake packet format**: requires a BLE sniffer during a real Home-button wake.

9. **Vendor control transfer for real serial number**: determine the exact bRequest/
   wValue/wIndex for the control transfer that returns `HEW70006169780`.

---

## 10. Capture Setup and Files

### Hardware

- **GreatFET One** (Great Scott Gadgets) — acts as a USB full-speed device.
- **Two USB connections**: USB A from GreatFET to Mac (control), USB A from GreatFET
  to Switch 2 console dock (the emulated device side).
- **Real Switch 2 Pro Controller** plugged directly into the Mac's USB port for the
  proxy to forward to.

### Software

| File | Purpose |
|---|---|
| `proxy.py` | Main USB MITM. Monkeypatches Facedancer for IAD/ENOSPC/pre-config-NAK bugs. Writes chronological hex log to `captures/<timestamp>.log`. Streams live pcap to `/tmp/switch2_usb.pipe` (see below). |
| `captures_to_pcap.py` | Converts a finished hex log to a Wireshark-openable pcap file (linktype 220, USB_LINUX_MMAPPED). Usage: `python3 captures_to_pcap.py captures/<file>.log` |
| `wake_test.py` | GreatFET emulator that cycles through 8 button-bit patterns trying to trigger console wake. Conclusive negative result — see §7. |
| `emulate_static.py` | Static GreatFET emulation with hardcoded descriptors. Diagnostic only. |
| `captures/` | Directory of raw hex log files, one per session. |
| `switch2-usb-protocol-expert/` | AI skill version of this knowledge (concise, agent-targeted). |

### Live Wireshark capture

`proxy.py` creates a named pipe at `/tmp/switch2_usb.pipe` on startup and streams
every transfer as a pcap record to it in real time. To use:

```bash
# In one terminal — start Wireshark before or shortly after starting proxy.py
wireshark -k -i /tmp/switch2_usb.pipe

# In another terminal
sudo ~/greatfet-venv/bin/python3 proxy.py
```

Wireshark will open with live traffic using the USB dissector (linktype 220).
Useful filters:
- `usb.transfer_type == 3` — bulk transfers only (vendor protocol on interface 1)
- `usb.transfer_type == 1` — interrupt transfers only (HID input/output)
- `usb.endpoint_address == 0x82` — vendor bulk IN (controller responses)
- `usb.endpoint_address == 0x81` — HID input reports

### Post-session pcap conversion

```bash
python3 captures_to_pcap.py captures/20260616-113304.log
# Produces captures/20260616-113304.pcap
wireshark captures/20260616-113304.pcap
```

### Hex log format

Each line:
```
[HH:MM:SS.ffffff] EP IN  0x81 len=64 09012800...
[HH:MM:SS.ffffff] EP OUT 0x02 len=12 0391000a...
[HH:MM:SS.ffffff] CTRL IN  req=<repr> len=N <hex>
```
`EP` = endpoint transfer (direction + endpoint address). `CTRL` = control transfer.
Sequence numbers in the vendor bulk frames let you pair any OUT with its IN response.

### Running the proxy

```bash
sudo ~/greatfet-venv/bin/python3 switch2-usb-re/proxy.py
```

Requires root (macOS needs root to claim the proxied USB device from the OS).
The script restarts automatically after crashes. Press `Ctrl-C` to stop cleanly.

### Parsing subcommand captures correctly

When analyzing a log to add or verify a subcommand:
1. Find every line matching `EP OUT 0x02`.
2. Read its hex; confirm byte 1 = `0x91` (request opcode).
3. Note byte 3 (subcommand ID) and byte 0 (sequence number).
4. Find the next `EP IN 0x82` line(s) — up to the next `EP OUT 0x02` — and
   confirm byte 0 = same sequence number (this is the correlation evidence).
5. If there are two IN lines back-to-back with the same sequence number,
   the second is a continuation packet with no header.
6. **Never** characterize a subcommand by looking at IN packets in isolation.
   The three phantom subcommands (`0x10`, `0x11`, `0x18`) that appeared in an
   earlier analysis were produced exactly this way and are confirmed artifacts.
