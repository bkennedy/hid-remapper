#!/usr/bin/env python3

import argparse
import errno
import glob
import os
import select
import sys
import termios
import time
import tty


DEFAULT_PORTS = (
    "/dev/cu.usbmodem*",
    "/dev/tty.usbmodem*",
)


def find_port(wait):
    while True:
        ports = []
        for pattern in DEFAULT_PORTS:
            ports.extend(glob.glob(pattern))
        ports = sorted(set(ports))
        if ports:
            return ports[0]
        if not wait:
            raise RuntimeError("No USB modem serial port found.")
        print("Waiting for USB modem serial port...")
        time.sleep(1)


def wait_for_port(path, wait, strict):
    while not os.path.exists(path):
        if not strict:
            ports = []
            for pattern in DEFAULT_PORTS:
                ports.extend(glob.glob(pattern))
            ports = sorted(set(ports))
            if ports:
                print(f"{path} not found; using {ports[0]} instead.")
                return ports[0]
        if not wait:
            raise RuntimeError(f"Serial port not found: {path}")
        print(f"Waiting for {path}...")
        time.sleep(1)
    return path


def open_serial(path, baud):
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_NOCTTY)
    attrs = termios.tcgetattr(fd)
    tty.setraw(fd)
    attrs = termios.tcgetattr(fd)
    attrs[4] = baud
    attrs[5] = baud
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def default_baud():
    return getattr(termios, "B921600", termios.B115200)


def close_serial(fd):
    if fd is not None:
        try:
            os.close(fd)
        except OSError:
            pass


def main():
    parser = argparse.ArgumentParser(description="Capture HID Remapper Bluetooth serial logs from USB CDC.")
    parser.add_argument("--port", default=None, help="serial device path, default: first /dev/cu.usbmodem*")
    parser.add_argument("--out", default="rumble.log", help="log output file")
    parser.add_argument("--seconds", type=float, default=0, help="stop after this many seconds; 0 means until Ctrl-C")
    parser.add_argument("--no-wait", action="store_true", help="fail immediately if no serial port is present")
    parser.add_argument("--append", action="store_true", help="append to output file instead of replacing it")
    parser.add_argument("--strict-port", action="store_true", help="only use --port exactly; do not fall back when macOS renumbers it")
    args = parser.parse_args()

    port = wait_for_port(args.port, not args.no_wait, args.strict_port) if args.port else find_port(not args.no_wait)
    print(f"Capturing {port} -> {args.out}")
    fd = None
    end_at = time.time() + args.seconds if args.seconds else None
    output_mode = "ab" if args.append else "wb"

    try:
        with open(args.out, output_mode) as out:
            while end_at is None or time.time() < end_at:
                if fd is None:
                    try:
                        port = wait_for_port(args.port, not args.no_wait, args.strict_port) if args.port else find_port(not args.no_wait)
                        fd = open_serial(port, default_baud())
                        print(f"Opened {port}")
                    except OSError as exc:
                        if exc.errno in (errno.ENXIO, errno.ENODEV, errno.EBUSY):
                            time.sleep(0.5)
                            continue
                        raise

                try:
                    readable, _, _ = select.select([fd], [], [], 0.2)
                except OSError as exc:
                    if exc.errno in (errno.ENXIO, errno.ENODEV):
                        close_serial(fd)
                        fd = None
                        time.sleep(0.5)
                        continue
                    raise
                if not readable:
                    continue
                try:
                    data = os.read(fd, 4096)
                except OSError as exc:
                    if exc.errno in (errno.ENXIO, errno.ENODEV):
                        print(f"Serial device not ready ({exc}); retrying...")
                        close_serial(fd)
                        fd = None
                        time.sleep(0.5)
                        continue
                    raise
                if not data:
                    continue
                out.write(data)
                out.flush()
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
    finally:
        close_serial(fd)


if __name__ == "__main__":
    main()
