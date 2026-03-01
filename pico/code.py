"""
Quad PN532 NFC reader for Raspberry Pi Pico (CircuitPython) — UID-only output.

Four PN532 boards share the SPI bus and are selected by separate CS pins.
Polls each reader and prints the tag UID as hex when a NEW tag appears:

    UID:1:04A3B2C1D2E3F4   (7-byte NTAG2xx UID from reader 1)

Presence-based dedup: only sends when UID changes on a reader.
Game-side dedup prevents duplicate spawns; no REMOVE events needed.

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
from adafruit_pn532.spi import PN532_SPI

# --- Configuration ---
SPI_SCK = board.GP2
SPI_MOSI = board.GP3
SPI_MISO = board.GP4
CS_PINS = [board.GP5, board.GP6, board.GP7, board.GP8]

NUM_READERS = 4
POLL_TIMEOUT = 0.05  # 50ms per reader — ~200ms full cycle
MAX_SPI_ERRORS = 5

# --- Init ---
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
    except Exception:
        print("Didn't find PN53x board on reader %d" % (i + 1))

print("Waiting for ISO14443A cards ...")

# --- Presence-based state ---
last_uid = [None] * NUM_READERS
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

        if uid is not None:
            uid_hex = "".join("{:02X}".format(b) for b in uid)
            if uid_hex != last_uid[i]:
                print("UID:%d:%s" % (i + 1, uid_hex))
                last_uid[i] = uid_hex
        else:
            # Tag removed — clear state so re-placing it triggers a new send
            last_uid[i] = None
