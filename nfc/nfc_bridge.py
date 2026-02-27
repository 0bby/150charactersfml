import serial

ser = serial.Serial('/dev/ttyACM0', 115200, timeout=1)

print("Waiting for NFC cards...")
while True:
    line = ser.readline().decode('utf-8').strip()
    if line:
        print(line)
