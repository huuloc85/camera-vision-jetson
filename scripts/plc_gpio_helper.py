#!/usr/bin/env python3
import argparse
import os
import sys


def set_jetson_model_name_if_needed():
    if os.environ.get("JETSON_MODEL_NAME"):
        return
    override = os.environ.get("JETSON_INSPECT_JETSON_MODEL_NAME") or os.environ.get(
        "OPENCV_TAIL_JETSON_MODEL_NAME"
    )
    if override:
        os.environ["JETSON_MODEL_NAME"] = override
        return
    try:
        with open("/proc/device-tree/model", "rb") as src:
            model = src.read().decode("utf-8", errors="ignore").replace("\x00", " ").lower()
    except Exception:
        return
    if "orin nano" in model:
        os.environ["JETSON_MODEL_NAME"] = "JETSON_ORIN_NANO"


def load_gpio():
    set_jetson_model_name_if_needed()
    try:
        import Jetson.GPIO as GPIO
        return GPIO, "Jetson.GPIO"
    except Exception as jetson_error:
        try:
            import RPi.GPIO as GPIO
            return GPIO, "RPi.GPIO"
        except Exception as rpi_error:
            raise RuntimeError(
                f"khong import duoc Jetson.GPIO ({jetson_error}) hoac RPi.GPIO ({rpi_error})"
            )


def write_line(text):
    print(text, flush=True)


def set_result(GPIO, ok_pin, ng_pin, result):
    GPIO.output(ok_pin, GPIO.HIGH if result == "OK" else GPIO.LOW)
    GPIO.output(ng_pin, GPIO.HIGH if result == "NG" else GPIO.LOW)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--trigger-pin", type=int, required=True)
    parser.add_argument("--ok-pin", type=int, required=True)
    parser.add_argument("--ng-pin", type=int, required=True)
    parser.add_argument("--busy-pin", type=int, required=True)
    args = parser.parse_args()

    try:
        GPIO, gpio_name = load_gpio()
        GPIO.setmode(GPIO.BOARD)
        try:
            GPIO.setwarnings(False)
        except Exception:
            pass

        input_kwargs = {}
        if hasattr(GPIO, "PUD_DOWN"):
            input_kwargs["pull_up_down"] = GPIO.PUD_DOWN
        GPIO.setup(args.trigger_pin, GPIO.IN, **input_kwargs)
        GPIO.setup(args.ok_pin, GPIO.OUT, initial=GPIO.LOW)
        GPIO.setup(args.ng_pin, GPIO.OUT, initial=GPIO.LOW)
        GPIO.setup(args.busy_pin, GPIO.OUT, initial=GPIO.LOW)
        write_line(f"READY {gpio_name} BOARD")
    except Exception as exc:
        write_line(f"ERROR {exc}")
        return 1

    try:
        for raw in sys.stdin:
            parts = raw.strip().split()
            if not parts:
                continue
            op = parts[0].upper()
            try:
                if op == "READ":
                    write_line("1" if GPIO.input(args.trigger_pin) else "0")
                elif op == "BUSY" and len(parts) == 2:
                    GPIO.output(args.busy_pin, GPIO.HIGH if parts[1] != "0" else GPIO.LOW)
                    write_line("OK")
                elif op == "RESULT" and len(parts) == 2:
                    set_result(GPIO, args.ok_pin, args.ng_pin, parts[1].upper())
                    write_line("OK")
                elif op == "ALL_LOW":
                    GPIO.output(args.busy_pin, GPIO.LOW)
                    set_result(GPIO, args.ok_pin, args.ng_pin, "WAIT")
                    write_line("OK")
                elif op == "QUIT":
                    GPIO.output(args.busy_pin, GPIO.LOW)
                    set_result(GPIO, args.ok_pin, args.ng_pin, "WAIT")
                    write_line("OK")
                    break
                else:
                    write_line("ERROR lenh PLC GPIO khong hop le")
            except Exception as exc:
                write_line(f"ERROR {exc}")
    finally:
        try:
            GPIO.output(args.busy_pin, GPIO.LOW)
            set_result(GPIO, args.ok_pin, args.ng_pin, "WAIT")
            GPIO.cleanup([args.trigger_pin, args.ok_pin, args.ng_pin, args.busy_pin])
        except Exception:
            pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
