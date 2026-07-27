import Jetson.GPIO as GPIO

GPIO.setmode(GPIO.BOARD)

GPIO.setup(16, GPIO.OUT)

GPIO.output(16, GPIO.HIGH)

print("Pin 16 HIGH")
input("Press Enter to exit...")

GPIO.cleanup()