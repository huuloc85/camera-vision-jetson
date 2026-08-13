#!/usr/bin/env python3
"""Jetson-side flow tester for ESP32 PLC/opto bridge.

This script verifies the real production sequence without running the camera UI:
PLC trigger -> ESP32 sends TRIGGER -> Jetson sends OK_ON/NG_ON -> ESP32 drives opto.
"""

import argparse
import os
import select
import sys
import termios
import time


BAUDS = {9600: termios.B9600, 115200: termios.B115200}


def open_uart(path, baud):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0
    attrs[4] = BAUDS[baud]
    attrs[5] = BAUDS[baud]
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 1
    termios.tcflush(fd, termios.TCIOFLUSH)
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def send(fd, line):
    os.write(fd, (line + "\n").encode("ascii"))
    print(f">> {line}")


def read_available(fd, buffer):
    lines = []
    try:
        chunk = os.read(fd, 512)
    except BlockingIOError:
        return buffer, lines
    if not chunk:
        return buffer, lines
    buffer += chunk
    while b"\n" in buffer or b"\r" in buffer:
        indexes = [i for i in (buffer.find(b"\n"), buffer.find(b"\r")) if i >= 0]
        split_at = min(indexes)
        raw = buffer[:split_at]
        buffer = buffer[split_at + 1:]
        line = raw.decode("ascii", errors="replace").strip()
        if line:
            lines.append(line)
    return buffer, lines


def choose_result(mode, count):
    if mode == "ok":
        return "OK_ON"
    if mode == "ng":
        return "NG_ON"
    return "OK_ON" if count % 2 else "NG_ON"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("device", nargs="?", default="/dev/ttyTHS1")
    parser.add_argument("--baud", type=int, default=115200, choices=sorted(BAUDS))
    parser.add_argument("--result", choices=("ok", "ng", "alternate"), default="alternate")
    parser.add_argument("--delay-ms", type=int, default=200)
    parser.add_argument("--clear-ms", type=int, default=0,
                        help="clear OK/NG this many ms after sending the result; 0 keeps production level-hold")
    args = parser.parse_args()

    try:
        fd = open_uart(args.device, args.baud)
    except OSError as exc:
        print(f"Failed to open {args.device}: {exc}", file=sys.stderr)
        print("Try: sudo chmod 666 <device> or run with sudo", file=sys.stderr)
        return 1

    print(f"Opened {args.device} @ {args.baud}")
    print("Press PLC trigger. Ctrl+C to stop.")
    print("Flow: TRIGGER -> BUSY should turn on -> Jetson sends result -> OK/NG on, BUSY off")

    buffer = b""
    trigger_count = 0
    last_status = 0.0

    send(fd, "PING")

    try:
        while True:
            readable, _, _ = select.select([fd], [], [], 0.1)
            if fd in readable:
                buffer, lines = read_available(fd, buffer)
                for line in lines:
                    now = time.monotonic()
                    print(f"<< {line}")
                    if line == "TRIGGER" or line.startswith("TRIGGER "):
                        trigger_count += 1
                        result = choose_result(args.result, trigger_count)
                        time.sleep(args.delay_ms / 1000.0)
                        send(fd, result)
                        print(f"cycle {trigger_count}: sent {result} after {args.delay_ms}ms")
                        if args.clear_ms > 0:
                            time.sleep(args.clear_ms / 1000.0)
                            send(fd, "CLEAR")
                            print(f"cycle {trigger_count}: cleared result after {args.clear_ms}ms")

            now = time.monotonic()
            if now - last_status >= 5.0:
                send(fd, "STATUS")
                last_status = now
    except KeyboardInterrupt:
        print("\nStopping, clearing outputs...")
        send(fd, "CLEAR")
    finally:
        os.close(fd)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
