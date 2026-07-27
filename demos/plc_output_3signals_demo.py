#!/usr/bin/env python3
"""Demo PLC output signals on Jetson BOARD pins.

This test drives the three existing outputs used by the app:
- OK   = BOARD 15
- NG   = BOARD 13
- BUSY = BOARD 16

Outputs default to active-HIGH, matching the production app.
"""

import argparse
import signal
import sys
import time


running = True


def handle_signal(_sig, _frame):
    global running
    running = False


def load_gpio(mock):
    if mock:
        return None
    try:
        import Jetson.GPIO as GPIO  # type: ignore
    except Exception as exc:  # pragma: no cover - hardware dependency
        print(f"ERROR: cannot import Jetson.GPIO: {exc}", file=sys.stderr)
        print("Run on Jetson, install Jetson.GPIO, or use --mock.", file=sys.stderr)
        sys.exit(2)
    return GPIO


def all_off(GPIO, pins, off_level):
    for pin in pins:
        GPIO.output(pin, off_level)


def pulse(GPIO, pin, pulse_sec, label, pins, on_level, off_level, on_label, off_label):
    all_off(GPIO, pins, off_level)
    GPIO.output(pin, on_level)
    print(f"{time.strftime('%H:%M:%S')} {label}={on_label}")
    time.sleep(pulse_sec)
    GPIO.output(pin, off_level)
    print(f"{time.strftime('%H:%M:%S')} {label}={off_label}")


def busy_hold(GPIO, busy_pin, hold_sec, pins, on_level, off_level, on_label, off_label):
    all_off(GPIO, pins, off_level)
    GPIO.output(busy_pin, on_level)
    print(f"{time.strftime('%H:%M:%S')} BUSY={on_label}")
    time.sleep(hold_sec)
    GPIO.output(busy_pin, off_level)
    print(f"{time.strftime('%H:%M:%S')} BUSY={off_label}")


def main():
    parser = argparse.ArgumentParser(
        description="Test OK / NG / BUSY outputs on Jetson GPIO."
    )
    parser.add_argument("--ok-pin", type=int, default=15)
    parser.add_argument("--ng-pin", type=int, default=13)
    parser.add_argument("--busy-pin", type=int, default=16)
    parser.add_argument("--pulse-sec", type=float, default=1.0)
    parser.add_argument("--busy-hold-sec", type=float, default=2.0)
    parser.add_argument("--gap-sec", type=float, default=0.5)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--mock", action="store_true")
    parser.add_argument(
        "--active-low",
        action="store_true",
        help="Drive outputs LOW to turn ON instead of the default HIGH-to-ON",
    )
    parser.add_argument(
        "--loop", action="store_true", help="Repeat the full sequence until Ctrl+C"
    )
    args = parser.parse_args()

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    GPIO = load_gpio(args.mock)
    pins = (args.ok_pin, args.ng_pin, args.busy_pin)
    active_low = args.active_low

    if GPIO:
        on_level = GPIO.LOW if active_low else GPIO.HIGH
        off_level = GPIO.HIGH if active_low else GPIO.LOW
    else:
        on_level = 0 if active_low else 1
        off_level = 1 if active_low else 0

    on_label = "LOW" if active_low else "HIGH"
    off_label = "HIGH" if active_low else "LOW"

    if GPIO:
        GPIO.setwarnings(False)
        GPIO.setmode(GPIO.BOARD)
        for pin in pins:
            GPIO.setup(pin, GPIO.OUT, initial=off_level)

    print(
        "PLC output demo: OK=BOARD {ok}, NG=BOARD {ng}, BUSY=BOARD {busy}, "
        "active={active}".format(
            ok=args.ok_pin,
            ng=args.ng_pin,
            busy=args.busy_pin,
            active="LOW" if active_low else "HIGH",
        )
    )
    print("Testing BUSY hold, then OK pulse, then NG pulse.")

    count = 0
    try:
        while running:
            count += 1
            print(f"--- cycle {count} ---")
            if GPIO:
                busy_hold(
                    GPIO, args.busy_pin, args.busy_hold_sec, pins,
                    on_level, off_level, on_label, off_label,
                )
                time.sleep(args.gap_sec)
                pulse(
                    GPIO, args.ok_pin, args.pulse_sec, "OK", pins,
                    on_level, off_level, on_label, off_label,
                )
                time.sleep(args.gap_sec)
                pulse(
                    GPIO, args.ng_pin, args.pulse_sec, "NG", pins,
                    on_level, off_level, on_label, off_label,
                )
                time.sleep(args.gap_sec)
                all_off(GPIO, pins, off_level)
            else:
                print(f"{time.strftime('%H:%M:%S')} BUSY={on_label}")
                time.sleep(args.busy_hold_sec)
                print(f"{time.strftime('%H:%M:%S')} BUSY={off_label}")
                time.sleep(args.gap_sec)
                print(f"{time.strftime('%H:%M:%S')} OK={on_label}")
                time.sleep(args.pulse_sec)
                print(f"{time.strftime('%H:%M:%S')} OK={off_label}")
                time.sleep(args.gap_sec)
                print(f"{time.strftime('%H:%M:%S')} NG={on_label}")
                time.sleep(args.pulse_sec)
                print(f"{time.strftime('%H:%M:%S')} NG={off_label}")

            if not args.loop and count >= args.repeat:
                break

        print("Done.")
    finally:
        if GPIO:
            all_off(GPIO, pins, off_level)
            GPIO.cleanup(pins)


if __name__ == "__main__":
    main()
