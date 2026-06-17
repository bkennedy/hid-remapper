---
name: switch2-usb-protocol-expert
description: Reverse-engineered knowledge of the Nintendo Switch 2 Pro Controller's wired USB protocol (descriptors, HID input report layout, vendor bulk command/response protocol). Use whenever working on Switch 2 Pro Controller USB support, decoding its input reports, its vendor-specific bulk command protocol, or extending hid-remapper / similar projects to support it.
---

# Switch 2 Pro Controller USB Protocol Expert

This skill documents what has been reverse-engineered so far about the Nintendo
Switch 2 Pro Controller's **wired USB** protocol, captured via a GreatFET One
running Facedancer as a USB man-in-the-middle (MITM) between a real Switch 2
console and a real Switch 2 Pro Controller. See `references/capture-method.md`
for how the capture was done and how to extend it.

Confidence levels are marked per finding: **HIGH** (strong, repeated evidence),
**MEDIUM** (plausible, partially evidenced), **LOW** (speculative, needs
verification).

## Identity (HIGH confidence)

- USB VID:PID = `0x057E:0x2069` (Nintendo)
- bcdDevice = `0x0201`
- bDeviceClass/SubClass/Protocol = `0xEF/0x02/0x01` (Multi-interface Function,
  uses Interface Association Descriptors — relevant if you're parsing the
  config descriptor yourself, see "IAD gotcha" below)
- Strings: iManufacturer = "Nintendo", iProduct = "Switch 2 Pro Controller"
- Full raw device + config descriptor hex: `references/usb-descriptors.md`

## Interface layout (HIGH confidence)

| Interface | Class | Purpose | Endpoints |
|---|---|---|---|
| 0 | HID (0x03) | Main controller input report + output (rumble) | EP 0x81 IN (interrupt, 64B, 4ms), EP 0x01 OUT (interrupt, 64B, 4ms) |
| 1 | Vendor-specific (0xFF) | Command/response control channel (info, calibration, handshake) | EP 0x82 IN (bulk, 64B), EP 0x02 OUT (bulk, 64B) |
| 2 | Audio (mic passthrough) | Not relevant to input | — |
| 3, 4 | Audio (mic passthrough, alt-setting 1 has isochronous EPs) | Console probes these on connect; not relevant to input | EP 0x03/0x83 isochronous |

The console issues `SET_INTERFACE` to the audio alt-settings shortly after
configuring — if you're emulating this device, you must accept that or it will
spam endpoint errors (harmless to ignore for input-only purposes).

### IAD gotcha

The config descriptor has exactly **three** Interface Association Descriptors
(0x0B), each placed immediately before the interface(s) it covers — verified
byte-for-byte in `references/usb-descriptors.md`'s descriptor table:

| IAD | bFirstInterface | bInterfaceCount | Covers |
|---|---|---|---|
| #1 | 0 | 1 | Interface 0 (HID) alone |
| #2 | 1 | 1 | Interface 1 (vendor) alone |
| #3 | 2 | **3** | Interfaces 2, 3, **and** 4 together — one audio function (AudioControl + 2x AudioStreaming) |

(Earlier draft of this doc incorrectly described IAD #3 as two separate
IADs covering "2+3" and "4" — it's one IAD with bInterfaceCount=3. The table
in `references/usb-descriptors.md` is byte-exact; trust that over prose.)

Some descriptor parsers (e.g. Facedancer's, see `references/capture-method.md`)
choke on an IAD appearing before any interface has been parsed yet. If you
write your own parser, don't assume `last_interface` is non-None when
handling a config-level descriptor that isn't an Interface or Endpoint
descriptor.

## Interface 0: HID input report (EP 0x81 IN, 64 bytes) — MEDIUM/HIGH confidence

Sent continuously at a 4ms interval (matches the descriptor's `bInterval=4`).
Structure derived from per-byte statistical analysis (variance/transition
counts) across a ~30,000-packet capture covering all buttons:

| Offset | Field | Confidence | Notes |
|---|---|---|---|
| 0 | Report ID | HIGH | Constant `0x09` |
| 1 | Sequence counter | HIGH | Wraps 0-255, increments every packet |
| 2 | (unused/reserved) | HIGH | Always `0x00` in capture |
| 3 | Button byte A | HIGH | See bit table below |
| 4 | Button byte B | HIGH | See bit table below |
| 5 | Button byte C | HIGH | See bit table below |
| 6-11 | Analog stick data | MEDIUM | Continuously varying; likely L/R stick X/Y, exact packing (8-bit vs 12-bit packed) not yet confirmed |
| 12+ | IMU / gyro / accel + padding | LOW | Rapidly varying high-entropy bytes consistent with 6-axis IMU sampled multiple times per report (matches the original Pro Controller's 3x IMU subsample scheme), not yet decoded bit-for-bit |

### Button bit mapping (byte 3/4/5) — HIGH confidence

Derived from a direct interactive single-button-at-a-time capture session —
every button below was pressed individually and confirmed by observing the exact
single-bit change in the live log. **The Switch 2 Pro Controller uses a
completely different bit layout from the original Switch Pro Controller.** Do
not apply the original Pro Controller mapping — it is wrong for the Switch 2.

The Switch 2 organizes by controller side: byte 3 = right side, byte 4 = left
side, byte 5 = special/new buttons.

**Byte 3 — right-side buttons:**

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

**Byte 4 — left-side buttons:**

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

**Byte 5 — special/new buttons:**

| Bit | Mask | Button |
|---|---|---|
| 0 | `0x01` | Home |
| 1 | `0x02` | Capture |
| 2 | `0x04` | GR (right back grip — new Switch 2 button) |
| 3 | `0x08` | GL (left back grip — new Switch 2 button) |
| 4 | `0x10` | Chat (new Switch 2 button) |
| 5–7 | — | Unknown (not observed) |

## Interface 0: HID output report (EP 0x01 OUT) — LOW confidence, open question

Console sends data here periodically, but two structurally different kinds of
payloads were observed, and they're inconsistent with each other — this needs
more work before trusting it. One sample type has a repeating quad pattern
consistent with HD-Rumble-style waveform encoding from the original Joy-Con/
Pro Controller; the other looks suspiciously like a duplicate of the EP 0x81
input report format, which may indicate a capture-pipeline artifact rather
than genuine output data. See `references/protocol-details.md` for both
samples and the recommended follow-up capture to disambiguate them.

## Interface 1: Vendor bulk command/response protocol — HIGH confidence

This is a clean, fully request/response, sequence-numbered protocol. Full
captured transcript with interpretation: `references/protocol-details.md`.

**Request format** (EP 0x02 OUT):
```
byte 0: sequence number (increments per request, wraps)
byte 1: 0x91 (fixed "request" opcode)
byte 2: 0x00
byte 3: subcommand ID
byte 4: 0x00
byte 5: expected-response-size class (tied to subcommand, not request payload length)
byte 6+: subcommand-specific argument payload (often zero-padded)
```

**Response format** (EP 0x82 IN):
```
byte 0: sequence number (echoes the request)
byte 1: 0x01 (fixed "response/ack" opcode)
byte 2: 0x00
byte 3: subcommand ID (echoed)
byte 4: 0x00
byte 5: 0xf8 (constant across every observed response — purpose unclear, possibly a fixed protocol marker)
byte 6+: response payload, length varies by subcommand
```

Responses longer than one bulk transfer are split into a second IN packet
with no header — just raw continuation bytes starting at the byte immediately
following the first packet's payload (observed for subcommands 0x01, 0x02,
0x04 with large payloads). **Precision pitfall, confirmed by re-deriving this
table**: if you scan IN packets independently of their triggering OUT
request, a continuation fragment can look like a fake "header" (e.g. a
fragment starting `0xff...` can masquerade as opcode byte `0x01`/subcommand
byte `0xff` etc.). Always pair each IN packet sequence to the OUT request
that immediately preceded it; never parse an IN packet as a header in
isolation. An earlier draft of this table had three phantom subcommands
(`0x10`, `0x11`, `0x18`) that turned out to be exactly this kind of
misattributed continuation fragment — verified against all 11 capture logs,
**these three were never actually sent as a real OUT request with that
subcommand ID**, so they've been removed below.

### Subcommand catalogue — verified set, byte-exact examples

Every subcommand below was confirmed by walking each of the 11 capture logs
**independently** (never mixing sessions), pairing each real `EP OUT 0x02`
request (`byte[1]==0x91`) with the IN packet(s) immediately following it up
to the next OUT, and tallying exact `(request, response)` combos by
frequency. This is the second derivation of this table — the first one
(scanning IN packets in isolation, grouped only by their own `byte[3]`)
produced two confirmed errors, both fixed here:
1. Three phantom subcommands (`0x10`, `0x11`, `0x18`) that were never
   actually sent as a real OUT request — they were misattributed
   continuation fragments (see the precision pitfall note above).
2. **The calibration-pattern response was attributed to the wrong
   subcommand.** It is `0x04`'s response, not `0x02`'s.

This is the complete, verified set of subcommand IDs actually requested:
`0x01, 0x02, 0x03, 0x04, 0x07, 0x08, 0x0a, 0x0c, 0x0d`. No other ID was ever
observed as a genuine request across any of the 11 logs.

**`0x04` — deterministic address-style memory/calibration read (HIGH
confidence).** When called with an 8-byte argument, the response is
**byte-identical every time for the same argument, across all 6 independent
sessions where it was captured** — this is the strongest possible evidence
from this kind of capture (not "looks similar," but exact byte-for-byte
reproduction on every reconnect). The first 4 argument bytes vary in a way
consistent with a little-endian address (`0x00007e10`, `0x00007e18`,
`0x00007e20`, `0x00007e40` — i.e. `7e10`/`7e18`/`7e20`/`7e40`), strongly
suggesting "read N bytes starting at flash/memory address X." All 6 verified
argument/response pairs:

| Argument (bytes 6+) | Response payload (bytes 9+, after status byte) |
|---|---|
| `40 7e 00 00 80 30 01 00` | `01 ad d9 9a 55 56 65 a0 00 0a a0 00 0a e2 20 0e e2 20 0e 9a ad d9 9a ad d9 0a a5 50 0a a5 50 2f f6 62 2f f6 62 0a ff ff 82 f7 81 56 36 61 38 86 5f ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff` (repeating triplets characteristic of packed 12-bit calibration values — likely **analog stick calibration**) |
| `40 7e 00 00 c0 30 01 00` | `01 ad d9 9a 55 56 65 a0 00 0a a0 00 0a e2 20 0e e2 20 0e 9a ad d9 9a ad d9 0a a5 50 0a a5 50 2f f6 62 2f f6 62 0a ff ff 33 78 82 c4 55 5f 0e 46 62 ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff` (same calibration-pattern prefix, different trailing bytes — likely a second stick/page) |
| `40 7e 00 00 40 c0 1f 00` | `01 ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff` (all-`0xff` — likely an unprogrammed/erased flash region) |
| `10 7e 00 00 40 30 01 00` | `01 e8 c1 ca 41 bf fe d9 3a 8a 67 14 bb 5c 14 32 bb` (16-byte opaque payload, not the calibration pattern — different data region) |
| `18 7e 00 00 00 31 01 00` | `01 00 00 00 00 00 00 00 00 00 00 00 00 4d 41 4e 3d c6 3a e4 3d 31 61 1d 41` (contains ASCII-adjacent bytes `4d 41 4e` = "MAN" — possibly a manufacturing/model string fragment) |
| `20 7e 00 00 60 30 01 00` | `01 ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff` (all-`0xff`) |

Full request/response frames (with header) for all 6 are in
`references/protocol-details.md`.

**`0x04` (and `0x02`) with a 17-byte argument — session-variable, one
genuinely surprising verified finding.** When called with a 17-byte argument
that looks random/different every session, `0x02`'s response is also a
different 16-byte payload every session (consistent with a nonce or
session-derived value, not calibration). But **`0x04`'s response to this
same call shape is the exact same 16 bytes every single time, across all 6
sessions, despite the argument being different each time**:
`15 01 00 04 00 f8 00 00 01 5c f6 ee 79 2c df 05 e1 ba 2b 63 25 c4 1a 5f 10`.
This was re-verified line-by-line against the raw logs (not a pairing
artifact) — see `references/protocol-details.md` for the full evidence. Take
this at face value rather than assuming it must be a mistake: either the
argument bytes aren't actually used in computing this particular response,
or there's a fixed/placeholder reply path for this call shape.

| Subcommand | Example OUT (request) | Example IN (response, incl. continuation if any) | Purpose | Confidence |
|---|---|---|---|---|
| `0x01` | `07 91 00 01 00 00 00 00` | `07 01 00 01 00 f8 00 00 00` | Simple flag/state query (1-byte payload `00`); also seen with a 14-byte arg returning a long response reusing the `0x0d` session value, and with an empty arg returning `03 00 00 00` — three distinct call shapes, purpose differs per shape | LOW |
| `0x02` | `15 91 00 02 00 11 00 00 00 68 7b 10 6e 4c ad da 73 03 19 6b ef 27 e6 95 6d` | `15 01 00 02 00 f8 00 00 01 fc 9f 34 d4 81 75 40 18 43 72 4e 41 70 8d 0f 0a` | First call per session returns a different 16-byte payload every time (nonce/challenge-like); later calls with a 4-byte zero arg (e.g. `0a 91 00 02 00 04 00 00 00 00 00 00`) are the **steady-state idle keepalive** — by far the most frequent bulk exchange during an idle connection (50 occurrences in one session alone), almost always an empty response | LOW (first-call purpose), HIGH (idle-keepalive behavior) |
| `0x03` | `15 91 00 03 00 01 00 00 00` | `15 01 00 03 00 f8 00 00 01` | Simple flag/state query (1-byte payload `01`); also seen with an empty arg returning a long ~31-byte opaque payload | LOW |
| `0x04` | see table above | see table above | **Address-style memory/calibration read** (8-byte-arg case) | HIGH for the read mechanism existing and being deterministic; LOW for what each address actually represents |
| `0x07` | `0b 91 00 07 00 04 00 00 00 00 00 00` | `0b 01 00 07 00 f8 00 00` (empty payload) | Keepalive/ready ping | MEDIUM |
| `0x08` | `0a 91 00 08 00 14 00 00 01 ff ff ff ff ff ff ff ff 35 00 46 00 00 00 00 00 00 00 00` | `0a 01 00 08 00 f8 00 00` (empty payload) | Write/set-style command (mask or filter config, mostly `0xff`) | LOW |
| `0x0a` | `03 91 00 0a 00 04 00 00 09 00 00 00` | `03 01 00 0a 00 f8 00 00` (empty payload) | Seen 5 times in one session, always this exact request/response. **Not the idle keepalive** — that's subcommand `0x02` (see above); don't confuse the two just because both have empty responses | LOW |
| `0x0c` | two distinct verified shapes: `03 91 00 0c 00 04 00 00 01 00 00 00` **or** `01 91 00 0c 00 00 00 00` | `03 01 00 0c 00 f8 00 00` (empty) **or** `01 01 00 0c 00 f8 00 00 61 12 50 10` respectively | The second shape (`61 12 50 10` response) is reproducible across 4 independent sessions with byte-identical request and response — likely a firmware/build version or fixed identifier read. (Correction: an earlier pass through this table briefly and incorrectly concluded this response was unreproducible; re-verified, it is real and consistent.) | MEDIUM (second shape), LOW (first shape's purpose) |
| `0x0d` | `03 91 00 0d 00 08 00 00 01 00 e1 39 65 05 48 c8` | `03 01 00 0d 00 f8 00 00 01 00 00 00` | First command sent on connect (byte 6+ argument is a 6-byte session/timestamp-like value, e.g. `e1 39 65 05 48 c8`) — handshake/session init | MEDIUM |

All examples above are real, verified OUT→IN pairs (same sequence number in
both directions) pulled directly from per-session log analysis — not
hand-assembled. **If you've already built code assuming subcommand `0x0a` is
the idle keepalive** (matching an earlier, incorrect version of this skill),
change it to `0x02` — the keepalive's actual subcommand byte is `0x02`.
`0x0a` is a real but separate, rarer subcommand with its own argument/response
shown above.

## Wake-from-sleep — it's BLE, not a USB signal at all (HIGH confidence)

The Switch 2 console (this is well documented for original controllers, and
the Pro Controller 2/Joy-Con 2 are confirmed to do the same — see
[GameFAQs Q&A](https://gamefaqs.gamespot.com/switch-2/507478-nintendo-switch-2/answers/653885-can-i-wake-the-nintendo-switch-2-from-sleep-mode-wirelessly-using-the-pro)
and a [GameFAQs board post](https://gamefaqs.gamespot.com/boards/189706-nintendo-switch/77313820))
can be woken from sleep by holding **Home** on an official Switch 2
controller. We tried to reproduce/trigger this via the GreatFET USB MITM and
it does **not** work at the USB-data level, across three separate
experiments:

1. `emulate_static.py` (pure GreatFET emulation, no real controller, no
   subcommand responder) — console never even issued a bus reset toward the
   GreatFET while asleep.
2. `wake_test.py` (GreatFET emulation with a canned vendor-bulk handshake
   responder and continuous Home/Capture-held input reports, later extended
   to cycle through 8 candidate button-bit patterns) — same result, zero bus
   activity from the console while asleep.
3. **The real controller, through a fully-connected real proxy session**
   (i.e. the actual working setup that normally captures everything) — let
   the console fall asleep on its own without touching the script, then
   pressed Home on the real controller. **Zero new lines appeared in the
   capture log at all.**

**Why**: the wake mechanism is not USB at all, even when the controller is
wired. The Switch 2's Bluetooth radio stays in a low-power listening state
during sleep, scanning specifically for the pairing/wake protocol used by
official Switch 2 peripherals (Joy-Con 2, Pro Controller 2). Holding **Home**
makes the controller broadcast a wireless BLE wake-up packet over the air —
this happens **regardless of whether the controller is also plugged in via
USB**, because the wake path is BLE end-to-end, not USB remote-wakeup
signaling. This fully explains our negative results: we were instrumenting
the wrong transport entirely. (This explanation was provided by the user,
without an independent published source — flagging as the working
explanation since it's fully consistent with every wake experiment we ran
and is more specific/complete than our prior "USB electrical resume signal"
guess, which it supersedes.)

**Implication for hid-remapper or similar wired-passthrough projects**: a
USB-only device sitting between a wired Pro Controller 2 and the console can
never replicate Home-button wake, because the real signal travels over a
radio our USB MITM has no visibility into or control over. Reproducing wake
would require a BLE radio in the loop (e.g. sniffing/transmitting the actual
BLE advertising/wake packet), which is a fundamentally different project
(see `references/capture-method.md`'s "what's needed" section — this
supersedes the USB-remote-wakeup-hardware suggestion that used to be here).

## What's still unknown / next steps

1. **Stick encoding** (offsets 6-11): whether it's 2x 8-bit axes per stick or
   12-bit packed like the original controller. Needs a capture with deliberate
   slow full-range stick sweeps, one stick at a time.
2. **Rumble/haptic encoding** on EP 0x01 OUT: byte-for-byte meaning of the
   waveform quads.
3. **IMU layout** in the input report tail (offset ~16 onward): needs a
   capture with the controller held still vs. rotated/shaken, correlated
   against known accelerometer/gyro scale conventions from the original
   controller.
4. Most subcommands above marked LOW confidence need correlation: vary one
   argument at a time and diff responses.
5. **Byte 5 bits 4–7**: observed as always-zero; unknown if they're unused
   or map to undiscovered buttons/states.

When extending this skill, append new findings to
`references/protocol-details.md` with raw hex evidence, and update the tables
above with revised confidence levels — don't just overwrite LOW with a guess,
upgrade it only when you have repeated/varied evidence.
