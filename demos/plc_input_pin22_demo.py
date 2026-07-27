#!/usr/bin/env python3
"""Demo PLC opto input on Jetson BOARD pin 22.

The production app uses BOARD numbering too. This demo can watch active-HIGH
or active-LOW edges, debounces them, and prints each accepted trigger.
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


def read_mock_state(start_time, interval_sec):
    phase = int((time.monotonic() - start_time) / interval_sec)
    return 1 if phase % 2 else 0


def main():
    parser = argparse.ArgumentParser(
        description="Watch PLC opto input on Jetson BOARD pin 22."
    )
    parser.add_argument("--pin", type=int, default=22, help="Jetson BOARD pin number")
    parser.add_argument(
        "--debounce-ms", type=float, default=40.0, help="Edge debounce time"
    )
    parser.add_argument(
        "--poll-ms", type=float, default=1.0, help="GPIO polling interval"
    )
    parser.add_argument(
        "--duration",
        type=float,
        default=0.0,
        help="Stop after this many seconds; 0 runs until Ctrl+C",
    )
    parser.add_argument(
        "--show-state", action="store_true", help="Print every raw LOW/HIGH change"
    )
    parser.add_argument(
        "--active-high",
        action="store_true",
        help="Trigger on LOW-to-HIGH instead of the default HIGH-to-LOW",
    )
    parser.add_argument(
        "--mock", action="store_true", help="Run without Jetson.GPIO for development"
    )
    parser.add_argument(
        "--mock-interval",
        type=float,
        default=0.25,
        help="Seconds between mock LOW/HIGH transitions",
    )
    args = parser.parse_args()

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    active_low = not args.active_high

    GPIO = load_gpio(args.mock)
    if GPIO:
        GPIO.setwarnings(False)
        GPIO.setmode(GPIO.BOARD)
        try:
            pull = GPIO.PUD_UP if active_low else GPIO.PUD_DOWN
            GPIO.setup(args.pin, GPIO.IN, pull_up_down=pull)
        except TypeError:
            GPIO.setup(args.pin, GPIO.IN)

    active_label = "LOW" if active_low else "HIGH"
    print(
        f"PLC input demo: BOARD pin={args.pin}, active={active_label}, "
        f"debounce={args.debounce_ms:.1f}ms"
    )
    print("Waiting for PNP opto signal. Press Ctrl+C to stop.")

    start_time = time.monotonic()
    last_state = 0
    if GPIO:
        last_state = int(GPIO.input(args.pin))
    trigger_count = 0
    last_trigger = 0.0
    debounce_sec = args.debounce_ms / 1000.0
    poll_sec = args.poll_ms / 1000.0

    try:
        while running:
            now = time.monotonic()
            if args.duration > 0 and now - start_time >= args.duration:
                break

            state = (
                read_mock_state(start_time, args.mock_interval)
                if args.mock
                else int(GPIO.input(args.pin))
            )

            if args.show_state and state != last_state:
                level = "HIGH" if state else "LOW"
                print(f"{time.strftime('%H:%M:%S')} state={level}")

            triggered = (
                state == 0 and last_state == 1
                if active_low
                else state == 1 and last_state == 0
            )
            if triggered and now - last_trigger >= debounce_sec:
                trigger_count += 1
                last_trigger = now
                print(
                    f"{time.strftime('%H:%M:%S')} TRIGGER #{trigger_count} "
                    f"on BOARD {args.pin}"
                )

            last_state = state
            time.sleep(poll_sec)
    finally:
        if GPIO:
            GPIO.cleanup(args.pin)

    print(f"Stopped. accepted_triggers={trigger_count}")


if __name__ == "__main__":
    main()
