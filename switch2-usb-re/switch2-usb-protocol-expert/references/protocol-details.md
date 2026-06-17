# Raw bulk command/response transcript (interface 1, EP 0x02 OUT / 0x82 IN)

## Methodology (read this before trusting any byte below)

This table was derived by walking **each of the 11 capture logs
independently** (sessions are never mixed together) and pairing every real
`EP OUT 0x02` request (`byte[1]==0x91`) with the IN packet(s) immediately
following it, up to the next OUT. This is the **second** derivation of this
data. The first derivation (an earlier pass that just grouped IN packets by
their own `byte[3]`, without correlating to the OUT request that triggered
them) produced two confirmed errors:

1. Three phantom subcommands (`0x10`, `0x11`, `0x18`) that were never
   actually sent as a real OUT request — verified by grepping all 11 logs
   for an OUT frame with that exact `byte[3]`: zero hits for all three. They
   were misattributed continuation fragments (raw, headerless bytes that
   happen to look like a header when read out of context).
2. The calibration-pattern response (`ad d9 9a ...` repeating triplets) was
   attributed to subcommand `0x02`. It is actually `0x04`'s response,
   confirmed by checking the OUT frame immediately preceding it in 6
   independent sessions — always `02 91 00 04 ...`, never `02 91 00 02 ...`.

**If you're extending this document**: always show the OUT frame and the IN
frame(s) together, and get them from adjacent lines in a single log file's
chronological order. Never assemble a "subcommand X's response" by
searching for a byte pattern across IN packets in isolation.

## Verified per-subcommand request/response pairs

All sequence numbers below (byte 0) match between request and response —
this is the actual correlation evidence, not just adjacency.

```
=== subcmd 0x01 ===
OUT: 07 91 00 01 00 00 00 00
IN:  07 01 00 01 00 f8 00 00 00

OUT: 15 91 00 01 00 0e 00 00 00 02 e1 39 65 05 48 c8 e0 39 65 05 48 c8
IN:  15 01 00 01 00 f8 00 00 01 04 01 3d 17 69 ab a9 3c

OUT: 11 91 00 01 00 00 00 00
IN:  11 01 00 01 00 f8 00 00 03 00 00 00

=== subcmd 0x02 ===
OUT: 15 91 00 02 00 11 00 00 00 68 7b 10 6e 4c ad da 73 03 19 6b ef 27 e6 95 6d
IN:  15 01 00 02 00 f8 00 00 01 fc 9f 34 d4 81 75 40 18 43 72 4e 41 70 8d 0f 0a
  (this exact 16-byte response payload differs every session -- nonce/challenge-like)

OUT: 0a 91 00 02 00 04 00 00 00 00 00 00          <- byte0 (0x0a) here is the
IN:  0a 01 00 02 00 f8 00 00                          SEQUENCE NUMBER, not a
                                                        different subcommand;
                                                        byte3 is still 0x02.
  (this exact request/response pair repeats throughout an idle session --
   the steady-state keepalive. Seen 50 times in one session alone.)

=== subcmd 0x03 ===
OUT: 15 91 00 03 00 01 00 00 00
IN:  15 01 00 03 00 f8 00 00 01

OUT: 11 91 00 03 00 00 00 00
IN:  11 01 00 03 00 f8 00 00 01 c0 03 00 00 e7 d0 1c 3b 79 22 a0 3a 0a e8 9c 42 58 a0 0b 42 0a e8 9c 41 58 a0 0b 41

=== subcmd 0x04 (8-byte argument shape -- see the address-read table below
    for all 6 verified pairs; one example here) ===
OUT: 02 91 00 04 00 08 00 00 40 7e 00 00 80 30 01 00
IN:  02 01 00 04 00 f8 00 00 40 00 00 00 80 30 01 00 01 ad d9 9a 55 56 65 a0 00 0a a0 00 0a e2 20 0e e2 20 0e 9a ad d9 9a ad d9 0a a5 50 0a a5 50 2f f6 62 2f f6 62 0a ff ff 82 f7 81 56 36 61 38 86
IN (continuation, no header): 5f ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff

=== subcmd 0x04 (17-byte argument shape -- response is IDENTICAL across all
    6 sessions despite a different-looking argument each time; verified, not
    a pairing bug) ===
OUT: 15 91 00 04 00 11 00 00 00 0d 2b 3d c0 f0 f6 ec ba 3c 6f 73 ec b7 92 39 db
IN:  15 01 00 04 00 f8 00 00 01 5c f6 ee 79 2c df 05 e1 ba 2b 63 25 c4 1a 5f 10

OUT: 15 91 00 04 00 11 00 00 00 a4 0c 02 c6 a3 52 68 2b f6 3c 95 25 20 97 8b b7
IN:  15 01 00 04 00 f8 00 00 01 5c f6 ee 79 2c df 05 e1 ba 2b 63 25 c4 1a 5f 10
  (different argument, byte-identical response to the example above)

OUT: 15 91 00 04 00 11 00 00 00 6c 94 32 a4 9d 3d de d0 fa 6a e4 70 9d ab 72 42
IN:  15 01 00 04 00 f8 00 00 01 5c f6 ee 79 2c df 05 e1 ba 2b 63 25 c4 1a 5f 10
  (again: different argument, byte-identical response)

=== subcmd 0x07 ===
OUT: 0b 91 00 07 00 04 00 00 00 00 00 00
IN:  0b 01 00 07 00 f8 00 00

OUT: 09 91 00 07 00 04 00 00 00 00 00 00
IN:  09 01 00 07 00 f8 00 00

OUT: 09 91 00 07 00 04 00 00 01 00 00 00
IN:  09 01 00 07 00 f8 00 00

=== subcmd 0x08 ===
OUT: 0a 91 00 08 00 14 00 00 01 ff ff ff ff ff ff ff ff 35 00 46 00 00 00 00 00 00 00 00
IN:  0a 01 00 08 00 f8 00 00

=== subcmd 0x0a ===
OUT: 03 91 00 0a 00 04 00 00 09 00 00 00
IN:  03 01 00 0a 00 f8 00 00

=== subcmd 0x0c ===
OUT: 03 91 00 0c 00 04 00 00 01 00 00 00
IN:  03 01 00 0c 00 f8 00 00

OUT: 01 91 00 0c 00 00 00 00
IN:  01 01 00 0c 00 f8 00 00 61 12 50 10
  (reproduced byte-identically across 4 independent sessions)

=== subcmd 0x0d ===
OUT: 03 91 00 0d 00 08 00 00 01 00 e1 39 65 05 48 c8
IN:  03 01 00 0d 00 f8 00 00 01 00 00 00
```

## `0x04` address-style read: all 6 verified argument/response pairs

The first 4 argument bytes (little-endian) take the values `0x00007e10`,
`0x00007e18`, `0x00007e20`, and `0x00007e40` (the last one three times with
different trailing bytes) — consistent with reading different memory/flash
regions near address `0x7e00`.

```
arg=40 7e 00 00 80 30 01 00
IN: 40 00 00 00 80 30 01 00 01 ad d9 9a 55 56 65 a0 00 0a a0 00 0a e2 20 0e e2 20 0e 9a ad d9 9a ad d9 0a a5 50 0a a5 50 2f f6 62 2f f6 62 0a ff ff 82 f7 81 56 36 61 38 86 5f ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff

arg=40 7e 00 00 c0 30 01 00
IN: 40 00 00 00 c0 30 01 00 01 ad d9 9a 55 56 65 a0 00 0a a0 00 0a e2 20 0e e2 20 0e 9a ad d9 9a ad d9 0a a5 50 0a a5 50 2f f6 62 2f f6 62 0a ff ff 33 78 82 c4 55 5f 0e 46 62 ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff

arg=40 7e 00 00 40 c0 1f 00
IN: 40 00 00 00 40 c0 1f 00 ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff

arg=10 7e 00 00 40 30 01 00
IN: 10 00 00 00 40 30 01 00 e8 c1 ca 41 bf fe d9 3a 8a 67 14 bb 5c 14 32 bb

arg=18 7e 00 00 00 31 01 00
IN: 18 00 00 00 00 31 01 00 00 00 00 00 00 00 00 00 00 00 00 00 4d 41 4e 3d c6 3a e4 3d 31 61 1d 41

arg=20 7e 00 00 60 30 01 00
IN: 20 00 00 00 60 30 01 00 ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff
```

Note the response's first 8 bytes match the request argument **except byte
1, which is always zeroed** (e.g. argument `40 7e 00 00 80 30 01 00` →
response prefix `40 00 00 00 80 30 01 00` — verified byte-for-byte, only
index 1 changes, `0x7e` → `0x00`, in all 6 examples). This is precise enough
to be useful as a parsing rule even though the *meaning* of that byte 1
(stripped in the echo) isn't understood — possibly a category/bank byte
that's intentionally not echoed back.

# EP 0x01 OUT (interrupt, interface 0) — purpose unclear, needs more work

Two distinct kinds of non-empty payloads were observed here, and they look
inconsistent with each other — flagging this as an open question rather than
asserting a theory:

**Sample A** (len=128, does NOT start with 0x09):
```
02608101101e008101101e000000000000608101101e008101101e0000000000000000...
```
This has a repeating `60 81 01 10 1e 00` quad pattern which is structurally
consistent with HD-Rumble-style waveform encoding on the original Joy-Con/Pro
Controller (repeated per-motor frequency/amplitude quads). This is the more
plausible "real rumble command" sample.

**Sample B** (len=192/256, multiples of 64 = N x max-packet-size, starts with
0x09 and an incrementing second byte):
```
0990200000009407853f28833800001e0c40000c0020dcf902408d1001daf7734737660050...
```
This closely resembles the **input report format** seen on EP 0x81 IN (report
ID 0x09, counter byte, same byte patterns like `86/87 xx xx xx 83 3x`). This
is suspicious — it may be a logging artifact in the MITM capture pipeline
(e.g. input data getting mis-attributed to the OUT filter) rather than genuine
host-to-device traffic. **Do not treat Sample B as ground truth for an output
report format without independently verifying it** (e.g. with a hardware USB
analyzer, or by checking whether the capture script's `filter_out`/`filter_in`
plumbing could explain the overlap).

Next step: re-run a focused capture isolating EP 0x01 OUT only, and trigger a
known rumble event (e.g. console connect-rumble) to see which sample type
correlates with it.
