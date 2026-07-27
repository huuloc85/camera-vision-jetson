#!/usr/bin/env python3
"""Trigger-to-result GPIO tool for Jetson BOARD pins.

Reads trigger input on BOARD 22 and responds with OK or NG on the
existing output pins so you can isolate whether the opto / PLC side
is the problem.
"""

import argparse
import queue
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


def set_outputs(GPIO, ok_pin, ng_pin, busy_pin, off_level):
    GPIO.output(ok_pin, off_level)
    GPIO.output(ng_pin, off_level)
    GPIO.output(busy_pin, off_level)


def pulse(GPIO, pin, on_level, off_level, pulse_sec):
    GPIO.output(pin, on_level)
    time.sleep(pulse_sec)
    GPIO.output(pin, off_level)

def level_name(state):
    return "HIGH" if state else "LOW"


def read_state(GPIO, pin, mock, mock_state_fn):
    if mock:
        return mock_state_fn()
    return int(GPIO.input(pin))


def main():
    parser = argparse.ArgumentParser(
        description="Read GPIO 22 trigger and respond with OK / NG outputs."
    )
    parser.add_argument("--trigger-pin", type=int, default=22)
    parser.add_argument("--ok-pin", type=int, default=15)
    parser.add_argument("--ng-pin", type=int, default=13)
    parser.add_argument("--busy-pin", type=int, default=16)
    parser.add_argument(
        "--result",
        choices=("ok", "ng", "toggle"),
        default="toggle",
        help="Which result to send on each trigger.",
    )
    parser.add_argument(
        "--toggle-start",
        choices=("ok", "ng"),
        default="ok",
        help="Starting result when --result=toggle.",
    )
    parser.add_argument(
        "--trigger-active-high",
        action="store_true",
        help="Treat trigger as active-HIGH instead of the default active-LOW.",
    )
    parser.add_argument(
        "--output-active-low",
        action="store_true",
        help="Drive outputs LOW to turn ON instead of the default HIGH-to-ON.",
    )
    parser.add_argument("--pulse-sec", type=float, default=0.2)
    parser.add_argument("--busy-hold-sec", type=float, default=0.05)
    parser.add_argument(
        "--hold-result-until-release",
        action="store_true",
        help="Keep OK/NG ON until trigger returns idle, useful for PLC handshake tests.",
    )
    parser.add_argument(
        "--release-timeout-sec",
        type=float,
        default=5.0,
        help="Max seconds to wait for trigger release when holding result.",
    )
    parser.add_argument("--debounce-ms", type=float, default=50.0)
    parser.add_argument("--poll-ms", type=float, default=1.0)
    parser.add_argument(
        "--show-state", action="store_true", help="Print raw LOW/HIGH changes."
    )
    parser.add_argument(
        "--mock", action="store_true", help="Run without Jetson.GPIO for development."
    )
    args = parser.parse_args()

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    GPIO = load_gpio(args.mock)

    trigger_active_low = not args.trigger_active_high
    output_active_low = args.output_active_low
    active_state = 0 if trigger_active_low else 1
    idle_state = 1 if trigger_active_low else 0
    on_level = 0 if output_active_low else 1
    off_level = 1 if output_active_low else 0
    debounce_sec = args.debounce_ms / 1000.0
    poll_sec = args.poll_ms / 1000.0

    trigger_events = queue.Queue(maxsize=32)

    if GPIO:
        GPIO.setwarnings(False)
        GPIO.setmode(GPIO.BOARD)
        for pin in (args.ok_pin, args.ng_pin, args.busy_pin):
            GPIO.setup(pin, GPIO.OUT, initial=off_level)
        try:
            pull = GPIO.PUD_UP if trigger_active_low else GPIO.PUD_DOWN
            GPIO.setup(args.trigger_pin, GPIO.IN, pull_up_down=pull)
        except TypeError:
            GPIO.setup(args.trigger_pin, GPIO.IN)

        edge = GPIO.FALLING if trigger_active_low else GPIO.RISING

        def on_trigger(_channel):
            try:
                trigger_events.put_nowait(time.monotonic())
            except queue.Full:
                pass

        GPIO.add_event_detect(
            args.trigger_pin,
            edge,
            callback=on_trigger,
            bouncetime=int(args.debounce_ms),
        )

        initial_state = int(GPIO.input(args.trigger_pin))
        if initial_state == active_state:
            try:
                trigger_events.put_nowait(time.monotonic())
            except queue.Full:
                pass

    print(
        "Trigger tool: TRIGGER=BOARD {trig} active={trig_active}, "
        "OK=BOARD {ok}, NG=BOARD {ng}, BUSY=BOARD {busy}, output_active={out_active}, "
        "result={result}".format(
            trig=args.trigger_pin,
            trig_active="LOW" if trigger_active_low else "HIGH",
            ok=args.ok_pin,
            ng=args.ng_pin,
            busy=args.busy_pin,
            out_active="LOW" if output_active_low else "HIGH",
            result=args.result,
        )
    )
    print("Waiting for trigger input. Ctrl+C to stop.")

    next_toggle = args.toggle_start
    armed = True
    last_state = read_state(GPIO, args.trigger_pin, args.mock, lambda: idle_state)
    last_change = 0.0
    trigger_count = 0

    def mock_state_fn():
        phase = int((time.monotonic() * 2.0))
        return active_state if phase % 2 else idle_state

    def log_output(name, is_on):
        print(f"{time.strftime('%H:%M:%S')} {name}={'ON' if is_on else 'OFF'}")

    def drive_output(pin, name, level, is_on):
        GPIO.output(pin, level)
        log_output(name, is_on)

    def wait_for_release(previous_state, held_result_name=None):
        waiting_since = time.monotonic()
        release_warned = False
        if held_result_name:
            print(
                f"{time.strftime('%H:%M:%S')} WAIT_TRIGGER_LOW holding "
                f"{held_result_name}=ON until trigger goes LOW"
            )
        else:
            print(f"{time.strftime('%H:%M:%S')} WAIT_TRIGGER_LOW")
        while running:
            state = int(GPIO.input(args.trigger_pin))
            if state == idle_state:
                if args.show_state and state != previous_state:
                    print(f"{time.strftime('%H:%M:%S')} state={level_name(state)}")
                print(f"{time.strftime('%H:%M:%S')} TRIGGER_RELEASED state={level_name(state)}")
                return state, True

            waited_sec = time.monotonic() - waiting_since
            if not release_warned and waited_sec >= 0.5:
                print(
                    f"{time.strftime('%H:%M:%S')} WAIT_RELEASE trigger still {level_name(state)}"
                )
                release_warned = True
            if args.release_timeout_sec > 0 and waited_sec >= args.release_timeout_sec:
                print(
                    f"{time.strftime('%H:%M:%S')} RELEASE_TIMEOUT trigger still "
                    f"{level_name(state)}"
                )
                return state, False
            time.sleep(poll_sec)

        return previous_state, False

    try:
        while running:
            if GPIO and not args.mock:
                try:
                    trigger_events.get(timeout=poll_sec)
                except queue.Empty:
                    continue

                state = active_state
                if args.show_state and state != last_state:
                    print(f"{time.strftime('%H:%M:%S')} state={level_name(state)}")

                now = time.monotonic()
                if now - last_change < debounce_sec:
                    continue
                if not armed:
                    continue

                trigger_count += 1
                last_change = now
                armed = False

                result = args.result
                if result == "toggle":
                    result = next_toggle
                    next_toggle = "ng" if next_toggle == "ok" else "ok"

                print(
                    f"{time.strftime('%H:%M:%S')} TRIGGER #{trigger_count} -> {result.upper()}"
                )

                set_outputs(GPIO, args.ok_pin, args.ng_pin, args.busy_pin, off_level)
                drive_output(args.busy_pin, "BUSY", on_level, True)
                time.sleep(args.busy_hold_sec)

                result_pin = args.ok_pin if result == "ok" else args.ng_pin
                result_name = result.upper()
                if result == "ok":
                    result_pin = args.ok_pin
                else:
                    result_pin = args.ng_pin

                if args.hold_result_until_release:
                    drive_output(result_pin, result_name, on_level, True)
                    drive_output(args.busy_pin, "BUSY", off_level, False)
                else:
                    drive_output(result_pin, result_name, on_level, True)
                    drive_output(args.busy_pin, "BUSY", off_level, False)
                    time.sleep(args.pulse_sec)
                    drive_output(result_pin, result_name, off_level, False)

                last_state = state

                while True:
                    try:
                        trigger_events.get_nowait()
                    except queue.Empty:
                        break

                held_result = result_name if args.hold_result_until_release else None
                last_state, released = wait_for_release(last_state, held_result)
                if args.hold_result_until_release:
                    drive_output(result_pin, result_name, off_level, False)

                armed = True
                continue

            state = read_state(GPIO, args.trigger_pin, args.mock, mock_state_fn)

            if args.show_state and state != last_state:
                print(f"{time.strftime('%H:%M:%S')} state={level_name(state)}")

            now = time.monotonic()
            edge_detected = (
                armed
                and state == active_state
                and last_state == idle_state
                and now - last_change >= debounce_sec
            )
            if edge_detected:
                trigger_count += 1
                last_change = now
                armed = False

                result = args.result
                if result == "toggle":
                    result = next_toggle
                    next_toggle = "ng" if next_toggle == "ok" else "ok"

                print(
                    f"{time.strftime('%H:%M:%S')} TRIGGER #{trigger_count} -> {result.upper()}"
                )

                print(f"{time.strftime('%H:%M:%S')} BUSY=ON")
                time.sleep(args.busy_hold_sec)
                print(f"{time.strftime('%H:%M:%S')} {result.upper()}=ON")
                print(f"{time.strftime('%H:%M:%S')} BUSY=OFF")
                if args.hold_result_until_release:
                    print(
                        f"{time.strftime('%H:%M:%S')} WAIT_TRIGGER_LOW holding "
                        f"{result.upper()}=ON until trigger goes LOW"
                    )
                else:
                    time.sleep(args.pulse_sec)
                    print(f"{time.strftime('%H:%M:%S')} {result.upper()}=OFF")

            if not armed and state == idle_state:
                if args.hold_result_until_release:
                    print(f"{time.strftime('%H:%M:%S')} TRIGGER_RELEASED state={level_name(state)}")
                    print(f"{time.strftime('%H:%M:%S')} {result.upper()}=OFF")
                armed = True

            last_state = state
            time.sleep(poll_sec)
    finally:
        if GPIO:
            try:
                GPIO.remove_event_detect(args.trigger_pin)
            except Exception:
                pass
            set_outputs(GPIO, args.ok_pin, args.ng_pin, args.busy_pin, off_level)
            GPIO.cleanup((args.ok_pin, args.ng_pin, args.busy_pin, args.trigger_pin))


if __name__ == "__main__":
    main()
