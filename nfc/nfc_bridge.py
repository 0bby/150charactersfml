import serial
import sys
import os

ser = None

# Try Linux-style /dev/ttyACM* (Arduino)
for i in range(10):
    port = f'/dev/ttyACM{i}'
    try:
        ser = serial.Serial(port, 115200, timeout=1)
        print(f"Opened {port}", file=sys.stderr)
        break
    except serial.SerialException:
        continue

# Try macOS-style /dev/cu.usbmodem* (Pico / CircuitPython)
if ser is None:
    for name in sorted(os.listdir('/dev')):
        if name.startswith('cu.usbmodem'):
            port = f'/dev/{name}'
            try:
                ser = serial.Serial(port, 115200, timeout=1)
                print(f"Opened {port}", file=sys.stderr)
                break
            except serial.SerialException:
                continue

if ser is None:
    print("No serial port found (tried /dev/ttyACM* and /dev/cu.usbmodem*)", file=sys.stderr)
    sys.exit(1)

print("Waiting for NFC cards...", file=sys.stderr)
while True:
    line = ser.readline().decode('utf-8').strip()
    if line:
        print(line)
