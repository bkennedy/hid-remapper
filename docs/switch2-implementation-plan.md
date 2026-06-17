# Switch 2 Pro Implementation Plan

This plan tracks the practical path for Switch 2 Pro Controller support on the
`switch2-codex` branch. Keep it updated as captures, logs, and hardware tests
change what we know.

## Current Target

- Input: physical Switch 2 Pro Controller over BLE to Seeed Xiao nRF52840.
- Output: Seeed board appears to the Switch 2 console over USB as a Switch 2 Pro
  Controller.
- First success criterion: controller button presses from BLE input produce
  visible input on the Switch 2 console over USB.
- Later criteria: rumble passthrough, stable reconnect, and console wake.

## Evidence We Have

- USB identity should be Nintendo `057e:2069`, product
  `Switch 2 Pro Controller`.
- Public USB emulation evidence points to one HID interface plus a vendor bulk
  interface for command traffic.
- The local 2026-06-11 BLE sniffer capture shows the physical controller
  advertising as:

```text
address    3c:a9:ab:69:17:3d
company    0x0553
payload    01 00 03 7e 05 69 20 00 01 00 00 00 00 00 00 00 0f 00 00 00 00 00 00 00
```

- In Zephyr manufacturer-data buffers that appears as:

```text
53 05 01 00 03 7e 05 69 20 ...
```

- Therefore BLE scan matching should use Bluetooth company ID `0x0553`, then
  match Nintendo USB VID/PID `057e:2069` inside the payload.
- A Wireshark UI capture saved as
  `/Users/bk/Documents/capture/capture-ui.pcapng` contains a clean console
  `CONNECT_IND` to the controller:

```text
initiator  c8:48:05:65:39:e1
advertiser 3c:a9:ab:69:17:3d
access     0xac83c178
interval   12 units = 15 ms
latency    0
timeout    200 units = 2000 ms
```

- The capture did not include data-channel packets after the `CONNECT_IND`, so
  it still does not reveal ATT/GATT/SMP, but it does give real console
  connection parameters.

## Evidence We Do Not Have Yet

- A clean followed BLE pairing/connection capture with ATT/GATT/SMP.
- Proof that the board completes BLE connection to the physical controller after
  the advertising matcher sees it.
- Proof that the console accepts the current USB descriptor and starts the bulk
  command handshake.
- Any real console USB command capture from the Switch 2 console to a genuine
  controller.
- A solved fresh BLE console-pairing `0x15` key exchange. Wake/autoconnect
  remains blocked unless donor pairing material is allowed.

## Phase 1: Make BLE Discovery Observable

- Match Switch 2 Pro advertisements using company `0x0553` and embedded
  `057e:2069`.
- Persist the matched manufacturer bytes in the Switch 2 flight log.
- Persist whether the board attempted `bt_conn_le_create`.
- Persist connection success/failure reason and security result.
- Keep scan behavior independent from USB descriptor/config mode.
- If the board sees the advertisement but fails to connect, test a Switch
  2-specific BLE connection parameter matching the captured console attempt:
  interval min/max `12`, latency `0`, timeout `200`.

Expected hardware test:

1. Flash the single Switch 2 console firmware.
2. Put the controller in pairing mode.
3. Pull the Switch 2 flight log.
4. Confirm `scan` entries include bytes beginning `53 05 01 00 03 7e 05 69 20`.

## Phase 2: Confirm BLE Controller Input

- Once connected, subscribe to the known/public input characteristic or fixed
  value handle.
- Log subscription result for input, ACK, and output characteristics.
- Log first input notification length and first 8 bytes.
- Confirm public 12-bit stick packing and button bytes against the Monitor tab
  or flight log.

Expected hardware test:

1. Pair/connect the controller to the board.
2. Press A/B/dpad/sticks.
3. Pull flight log.
4. Confirm `ble_input` events change when buttons are pressed.

## Phase 3: Restore Console USB Compatibility

- Keep a single UF2 build.
- Keep USB identity `057e:2069`.
- Re-check descriptor shape against VIIPER and public sources:
  - HID report IDs `0x05`, `0x09`, `0x02`.
  - HID interrupt input/output endpoints on interface 0.
  - Vendor bulk OUT/IN on interface 1.
- Avoid extra USB interfaces in the console build.
- Treat HID Remapper config access in the console image as experimental; if it
  changes console enumeration behavior, remove it and rely on flight-log pull
  from a separate non-console maintenance mode only after user approval.

Expected hardware test:

1. Flash single console firmware.
2. Connect board to Switch 2 console USB.
3. Pull flight log after unplug/replug.
4. Look for `usb_status`, `host_cmd`, `int_out`, `queue_response`, and
   `send_response`.

## Phase 4: Bridge BLE Input To USB Output

- When BLE input packets arrive, pack them into Switch 2 USB report ID `0x09`.
- Send periodic neutral input heartbeat only after console input is enabled.
- Preserve report counter/timer behavior.
- Add flight-log counters for sent input reports and failed input writes.

Expected hardware test:

1. Controller paired to board.
2. Board plugged into console.
3. Press buttons.
4. Console responds or flight log identifies whether failure is BLE input,
   console command handshake, or USB input write.

## Phase 5: Rumble Passthrough

- Parse console output report ID `0x02`.
- Log first rumble report bytes from console.
- Write rumble payload to the controller BLE output characteristic only after
  BLE output characteristic behavior is confirmed by capture or hardware logs.

## Phase 6: Wake And Autoconnect

- Do not implement wake until normal controller input over USB works.
- Wake likely requires BLE console impersonation, console MAC in advertising,
  donor MAC/LTK material, or the unsolved `0x15` first-pairing exchange.
- Keep this separate from USB passthrough work.

## Next Immediate Work

1. Build and flash the single console firmware with the corrected BLE advertiser
   matcher.
2. Pair the controller to the board.
3. Pull the Switch 2 flight log.
4. If scan is confirmed but connection fails, try the captured console-style
   connection parameter `BT_LE_CONN_PARAM(12, 12, 0, 200)` for Switch 2 BLE
   input.
5. If scan/connect is confirmed, debug GATT subscription and input packets.
6. If scan is not confirmed, capture pairing again with the sniffer following
   `3c:a9:ab:69:17:3d` before pairing starts.
