import Jetson.GPIO as GPIO
import time

GPIO.setmode(GPIO.BOARD)

OK_PIN = 15
NG_PIN = 13
BUSY_PIN = 16

GPIO.setup(OK_PIN, GPIO.OUT, initial=GPIO.LOW)
GPIO.setup(NG_PIN, GPIO.OUT, initial=GPIO.LOW)
GPIO.setup(BUSY_PIN, GPIO.OUT, initial=GPIO.LOW)


def trigger(pin, name):
    print(name, "ON")

    GPIO.output(pin, GPIO.HIGH)

    time.sleep(1)

    GPIO.output(pin, GPIO.LOW)

    print(name, "OFF")


print("Jetson OK / NG / BUSY test")
print("--------------------------")
print("1 = OK")
print("2 = NG")
print("3 = BUSY")
print("Ctrl+C de thoat")

try:
    while True:
        key = input("Trigger: ")

        if key == "1":
            trigger(OK_PIN, "OK")

        elif key == "2":
            trigger(NG_PIN, "NG")

        elif key == "3":
            trigger(BUSY_PIN, "BUSY")

except KeyboardInterrupt:
    print("\nStop")

finally:
    GPIO.output(OK_PIN, GPIO.LOW)
    GPIO.output(NG_PIN, GPIO.LOW)
    GPIO.output(BUSY_PIN, GPIO.LOW)
    GPIO.cleanup()