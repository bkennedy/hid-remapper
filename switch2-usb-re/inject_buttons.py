#!/usr/bin/env python3
"""
Interactive button injector for Switch 2 Pro Controller USB proxy.

Uses real keydown/keyup events — button is held exactly as long as the key is.
Requires proxy.py to be running first.

Keyboard → button mapping:
  b / a / y / x          →  B / A / Y / X
  r / R (shift+r)        →  R / ZR
  l / L (shift+l)        →  L / ZL
  = / -                  →  + / -
  h                      →  Home
  c                      →  Capture
  g / G (shift+g)        →  GL / GR
  t                      →  Chat
  Arrow keys             →  D-pad
  . / ,                  →  R3 / L3

Press Escape or Ctrl+C to quit.
"""

import os
import sys
import threading

from pynput import keyboard

INJECT_PIPE = "/tmp/switch2_inject.pipe"

BYTE3 = {"b": 0x01, "a": 0x02, "y": 0x04, "x": 0x08,
         "r": 0x10, "R": 0x20, "=": 0x40, ".": 0x80}
BYTE4 = {"down": 0x01, "right": 0x02, "left": 0x04, "up": 0x08,
         "l": 0x10, "L": 0x20, "-": 0x40, ",": 0x80}
BYTE5 = {"h": 0x01, "c": 0x02, "G": 0x04, "g": 0x08, "t": 0x10}

LABELS = {
    "b": "B", "a": "A", "y": "Y", "x": "X",
    "r": "R", "R": "ZR", "=": "+", ".": "R3",
    "down": "D-Down", "right": "D-Right", "left": "D-Left", "up": "D-Up",
    "l": "L", "L": "ZL", "-": "-", ",": "L3",
    "h": "Home", "c": "Capture", "G": "GR", "g": "GL", "t": "Chat",
}


def key_to_str(key):
    try:
        ch = key.char
        return ch if ch else None
    except AttributeError:
        name = key.name if hasattr(key, "name") else None
        if name in ("up", "down", "left", "right"):
            return name
        return None


def main():
    if not os.path.exists(INJECT_PIPE):
        print(f"Pipe not found: {INJECT_PIPE}")
        print("Make sure proxy.py is running first.")
        sys.exit(1)

    print("Opening inject pipe (waiting for proxy.py)...")
    fh = open(INJECT_PIPE, "wb", buffering=0)
    print("Connected.\n")
    print("Keys: b/a/y/x=BAYX  r/R=R/ZR  l/L=L/ZL  =/- =+/-")
    print("      arrows=D-pad  h=Home  c=Capture  g/G=GL/GR  t=Chat")
    print("      Esc or Ctrl+C to quit.\n")

    lock = threading.Lock()
    b3, b4, b5 = 0, 0, 0
    held = set()
    stop_event = threading.Event()

    def send(b3, b4, b5):
        fh.write(bytes([b3, b4, b5]))
        fh.flush()

    def print_state():
        status = " + ".join(LABELS[k] for k in sorted(held, key=lambda k: LABELS[k])) or "—"
        print(f"\r  held: [{status}]          ", end="", flush=True)

    def on_press(key):
        nonlocal b3, b4, b5
        k = key_to_str(key)
        if k is None:
            return
        if k == "\x1b":  # Escape
            stop_event.set()
            return False
        bit3 = BYTE3.get(k, 0)
        bit4 = BYTE4.get(k, 0)
        bit5 = BYTE5.get(k, 0)
        if not (bit3 or bit4 or bit5):
            return
        with lock:
            if k in held:
                return  # already held, ignore key repeat
            held.add(k)
            b3 |= bit3
            b4 |= bit4
            b5 |= bit5
            snap = (b3, b4, b5)
        send(*snap)
        print_state()

    def on_release(key):
        nonlocal b3, b4, b5
        k = key_to_str(key)
        if k is None:
            return
        bit3 = BYTE3.get(k, 0)
        bit4 = BYTE4.get(k, 0)
        bit5 = BYTE5.get(k, 0)
        if not (bit3 or bit4 or bit5):
            return
        with lock:
            held.discard(k)
            b3 &= ~bit3
            b4 &= ~bit4
            b5 &= ~bit5
            snap = (b3, b4, b5)
        send(*snap)
        print_state()

    listener = keyboard.Listener(on_press=on_press, on_release=on_release)
    listener.start()

    try:
        stop_event.wait()
    except KeyboardInterrupt:
        pass
    finally:
        listener.stop()
        send(0, 0, 0)
        fh.close()
        print("\nReleased all. Bye.")


if __name__ == "__main__":
    main()
