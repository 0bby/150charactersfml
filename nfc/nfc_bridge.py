import serial
import sys

ser = None
for i in range(10):
    port = f'/dev/ttyACM{i}'
    try:
        ser = serial.Serial(port, 115200, timeout=1)
        print(f"Opened {port}", file=sys.stderr)
        break
    except serial.SerialException:
        continue

if ser is None:
    print("No /dev/ttyACM* port found", file=sys.stderr)
    sys.exit(1)

print("Waiting for NFC cards...", file=sys.stderr)
while True:
    line = ser.readline().decode('utf-8').strip()
    if line:
        print(line)
