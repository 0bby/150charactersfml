"""
Quad PN532 NFC reader for Raspberry Pi Pico (CircuitPython) — UID-only output.

Four PN532 boards share the SPI bus and are selected by separate CS pins.
Polls each reader and prints the tag UID as hex:

    UID:1:04A3B2C1D2E3F4   (7-byte NTAG2xx UID from reader 1)
    UID:2:AABBCCDD          (4-byte Mifare Classic UID from reader 2)

The server maps UID -> {type, rarity, abilities} via nfc_tags.json.

Wiring (Pico SPI0):
    SCK   -> GP2  (shared)
    MOSI  -> GP3  (shared)
    MISO  -> GP4  (shared)
    CS1   -> GP5  (reader 1)
    CS2   -> GP6  (reader 2)
    CS3   -> GP7  (reader 3)
    CS4   -> GP8  (reader 4)
"""

import board
import busio
import digitalio
import time
from adafruit_pn532.spi import PN532_SPI

# --- Configuration ---
SPI_SCK = board.GP2
SPI_MOSI = board.GP3
SPI_MISO = board.GP4
CS_PINS = [board.GP5, board.GP6, board.GP7, board.GP8]

NUM_READERS = 4
DEBOUNCE_MS = 2000
POLL_TIMEOUT = 0.05  # 50ms per reader — fast round-robin
MAX_SPI_ERRORS = 5

# --- Init ---
time.sleep(2)

# Drive all CS pins HIGH to prevent bus contention
cs_ios = []
for pin in CS_PINS:
    cs = digitalio.DigitalInOut(pin)
    cs.direction = digitalio.Direction.OUTPUT
    cs.value = True
    cs_ios.append(cs)

spi = busio.SPI(SPI_SCK, MOSI=SPI_MOSI, MISO=SPI_MISO)

readers = [None] * NUM_READERS
for i in range(NUM_READERS):
    try:
        pn532 = PN532_SPI(spi, cs_ios[i])
        _ = pn532.firmware_version
        pn532.SAM_configuration()
        readers[i] = pn532
        print("Reader %d (GP%d): OK" % (i + 1, 5 + i))
    except Exception as e:
        print("Didn't find PN53x board on reader %d" % (i + 1))

print("Waiting for ISO14443A cards ...")

# --- Debounce state ---
last_uid = [None] * NUM_READERS
last_time = [0.0] * NUM_READERS
err_count = [0] * NUM_READERS

# --- Main loop ---
while True:
    for i in range(NUM_READERS):
        if readers[i] is None:
            continue

        try:
            uid = readers[i].read_passive_target(timeout=POLL_TIMEOUT)
            err_count[i] = 0
        except Exception as e:
            err_count[i] += 1
            if err_count[i] >= MAX_SPI_ERRORS:
                print("Reader %d disabled after %d SPI errors: %s" % (i + 1, MAX_SPI_ERRORS, e))
                readers[i] = None
            continue

        if uid is None:
            continue

        uid_hex = "".join("{:02X}".format(b) for b in uid)

        # Debounce: skip if same tag was just read on this reader
        now = time.monotonic() * 1000  # ms
        if uid_hex == last_uid[i] and (now - last_time[i]) < DEBOUNCE_MS:
            continue

        # Output: UID:<reader>:<hex_uid>
        print("UID:%d:%s" % (i + 1, uid_hex))

        last_uid[i] = uid_hex
        last_time[i] = now
