# SW2 Pro USB Emulation — Session Summary (2026-06-22)

## Goal

Make the nRF52840 DK appear and **register as a player** on the Nintendo Switch 2
Change Grip/Order screen, spoofing a real Switch 2 Pro Controller via USB composite
(HID + vendor BULK + Audio).

---

## What Works (Confirmed)

- Full USB composite enumeration: VID/PID `057E:2069`, three IADs, HID IF0 + vendor BULK IF1 + Audio IF2-4
- Auth relay: DK serial ↔ real SW2 Pro USB for AUTH01/AUTH04/AUTH02 challenge-response
- Handshake completes: `host=33`, `response=25/0`, `response_q=0/2` (no queue overflow)
- Connect rumble fires: `rumble=2/0` — console accepts the controller at the protocol level
- HID polling: console polls DK at ~115Hz continuously
- **No kernel panic**: fixed by removing touchscreen taps (auto-pressing L+R in firmware instead)
- **No 0x0d flood death spiral**: rate-limiter + `k_msgq_purge` prevents queue overflow

## What Doesn't Work Yet

- `set=0` always — console never sends SET_REPORT (player LED assignment)
- Controller not appearing on Change Grip/Order screen as a visible/selectable controller

---

## Root Cause Found (2026-06-22)

**L+R auto-press was firing too early and only once per boot.**

Session timeline discovered from heartbeat logs vs stats logs:

1. `t=0s` — DK boots
2. `t≈1s` — Console sends 0x0d, `enabled=1`, DK sets `sw2_lr_autopress_ms = now + 1500`
3. `t≈2.5–4s` — **L+R pressed** (too early — grip screen hasn't shown the controller yet; 33-command auth still running)
4. `t≈39s` — Console does USB CONFIGURE reset → `switch_pro_reset_session()` called → BUT `sw2_lr_autopress_ms` was **not reset** (bug)
5. `t≈40s` — New USB mini-session, 0x0d received, `enabled=1`, but `sw2_lr_autopress_ms = 2500` (stale, already past press window) → **no L+R ever again this boot**
6. `t=52s` — Disconnect; user sees "controller not showing" during t=40–52s window where L+R was silent

The stats log only starts at t=40.232s (reset by `switch_pro_reset_session`) which is why we
were fooled into thinking `enabled=1` was first seen at t=41s. The heartbeat log (static timer,
not reset by session) showed `enabled=1` from t=1s.

---

## Fix Applied — iter31 (flashed 2026-06-22, relay running)

**`firmware-bluetooth/src/main.cc`**

### 1. `switch_pro_reset_session()` now resets `sw2_lr_autopress_ms = 0`

Ensures L+R fires fresh in every new USB session, not just once per boot.

```cpp
static void switch_pro_reset_session() {
    switch_pro_input_enabled = false;
    switch_pro_timer = 0;
    switch_pro_last_input_ms = 0;
    switch_pro_last_stats_ms = 0;
    switch_pro_last_button_log_ms = 0;
    sw2_lr_autopress_ms = 0;  // reset per-session so L+R fires fresh after each USB CONFIGURE
    k_msgq_purge(&switch_pro_response_q);
    switch_pro_reset_input();
    switch_pro_reset_axis_diagnostics();
}
```

### 2. Delay increased: `now + 1500` → `now + 4000`

Press fires 4s after `input_enabled`, giving the grip screen time to display the
controller before we press L+R.

```cpp
if (!sw2_lr_autopress_ms) sw2_lr_autopress_ms = now + 4000;
```

### 3. Heartbeat log enhanced

Now shows `b34=XXYY` (HID bytes 3 & 4, shows `1010` during press) and
`autopress=N` (ms since press window opened; 0–1500 = active press window).

### Expected new behavior

- Each USB session: 0x0d arrives → `enabled=1` → 4s delay → L+R pressed for 1.5s (3×150ms on / 350ms off)
- Stats should show `buttons=10 10 00` during press window
- If registration works: `set > 0`

---

## Current Hardware Setup

| Thing | State |
|---|---|
| DK firmware | iter31 flashed, running |
| Relay | `/Users/bk/controller/xac/bk-remapper/hid-remapper/config-tool/sw2_auth_relay.py` → `/tmp/relay.log` |
| Real SW2 Pro | Must be plugged into Mac USB for AUTH02 relay |
| J3 cable | Must be plugged into Switch 2 console |

**Next action needed**: plug J3 into Switch 2, restart console (Change Grip screen appears automatically).

---

## Key Files

| File | Role |
|---|---|
| `firmware-bluetooth/src/main.cc` | All SW2 USB emulation |
| `config-tool/sw2_auth_relay.py` | AUTH relay script |
| `/tmp/relay.log` | Live DK serial output |

## Key Constants

| Item | Value |
|---|---|
| Real controller serial | `HEW70006169780` |
| Real controller MAC | `3c:a9:ab:69:17:3d` |
| DK J-Link SN | `1050274853` |
| DK serial port | `/dev/tty.usbmodem0010502748531` |
| AUTH04 packet (hex) | `15910004001100000000000000000000000000000000000000` (25 bytes) |
| R button | `byte[3] bit4 = 0x10` |
| L button | `byte[4] bit4 = 0x10` |

## Build / Flash / Reset Commands

```bash
# Build
docker run --platform linux/amd64 -v "$PWD":/workdir/project \
  -w /workdir/project/firmware-bluetooth \
  nordicplayground/nrfconnect-sdk:v2.2-branch \
  west build -b nrf52840dk_nrf52840

# Flash
nrfutil device program \
  --firmware firmware-bluetooth/build/zephyr/remapper.hex \
  --serial-number 1050274853

# Reset
JLinkExe -device nRF52840_xxAA -if SWD -speed 4000 \
  -selectemubysn 1050274853 -autoconnect 1 \
  -CommandFile /dev/stdin <<< $'r\ngo\nexit\n'

# Start relay (fresh log)
pkill -f sw2_auth_relay.py
python3 config-tool/sw2_auth_relay.py > /tmp/relay.log 2>&1 &
```

## Success Indicator

`set > 0` in `switch_pro_stats` = console sent SET_REPORT (player LED) = controller
registered as player 1 on the Change Grip screen.
