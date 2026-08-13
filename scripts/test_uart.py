#!/usr/bin/env python3
"""Simple Jetson UART tester for the ESP32-S3 test firmware.

No pyserial dependency: uses Linux termios directly.
"""

import argparse
import os
import select
import sys
import termios
import time


BAUDS = {
    9600: termios.B9600,
    115200: termios.B115200,
}


def open_uart(path, baud):
    if baud not in BAUDS:
        raise ValueError(f"unsupported baud rate: {baud}")

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


def write_line(fd, line):
    os.write(fd, (line + "\n").encode("ascii"))
    print(f">> {line}")


def read_lines(fd, duration_s, prefix="<< "):
    deadline = time.monotonic() + duration_s
    buf = b""
    lines = []

    while time.monotonic() < deadline:
        timeout = max(0.0, min(0.1, deadline - time.monotonic()))
        readable, _, _ = select.select([fd], [], [], timeout)
        if not readable:
            continue

        try:
            chunk = os.read(fd, 256)
        except BlockingIOError:
            continue
        if not chunk:
            continue

        buf += chunk
        while b"\n" in buf or b"\r" in buf:
            split_at = min(
                [i for i in (buf.find(b"\n"), buf.find(b"\r")) if i >= 0]
            )
            raw = buf[:split_at]
            buf = buf[split_at + 1:]
            line = raw.decode("ascii", errors="replace").strip()
            if line:
                print(prefix + line)
                lines.append(line)

    return lines


def run_auto(fd):
    print("Waiting for TEST_READY/PONG data...")
    read_lines(fd, 2.0)

    write_line(fd, "PING")
    pong = read_lines(fd, 1.0)
    if not any("PONG" in line for line in pong):
        print("WARN: no PONG received. Check RX/TX wiring and device path.")

    for cmd in ("TEST_OK", "TEST_NG", "TEST_BUSY", "TEST_ALL"):
        write_line(fd, cmd)
        read_lines(fd, 3.0 if cmd == "TEST_ALL" else 1.5)


def monitor(fd):
    print("Monitor mode. Press Ctrl+C to stop. Try PLC trigger or type commands.")
    print("Commands: PING, TEST_OK, TEST_NG, TEST_BUSY, TEST_ALL")
    stdin_fd = sys.stdin.fileno()
    buf = b""

    while True:
        readable, _, _ = select.select([fd, stdin_fd], [], [], 0.1)
        if stdin_fd in readable:
            line = sys.stdin.readline()
            if not line:
                return
            write_line(fd, line.strip())

        if fd in readable:
            try:
                chunk = os.read(fd, 256)
            except BlockingIOError:
                continue
            buf += chunk
            while b"\n" in buf or b"\r" in buf:
                split_at = min(
                    [i for i in (buf.find(b"\n"), buf.find(b"\r")) if i >= 0]
                )
                raw = buf[:split_at]
                buf = buf[split_at + 1:]
                line = raw.decode("ascii", errors="replace").strip()
                if line:
                    print("<< " + line)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("device", nargs="?", default="/dev/ttyTHS1")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--monitor", action="store_true")
    args = parser.parse_args()

    try:
        fd = open_uart(args.device, args.baud)
    except OSError as exc:
        print(f"Failed to open {args.device}: {exc}", file=sys.stderr)
        print("Try: sudo chmod 666 <device> or run with sudo", file=sys.stderr)
        return 1

    print(f"Opened {args.device} @ {args.baud}")
    try:
        if args.monitor:
            monitor(fd)
        else:
            run_auto(fd)
            print("Done. Use --monitor to watch trigger events continuously.")
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        os.close(fd)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
