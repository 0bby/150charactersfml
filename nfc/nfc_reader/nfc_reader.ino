/**************************************************************************/
/*!
    Quad PN532 NFC reader for Arduino — UID-only output.

    Four PN532 boards share the hardware SPI bus (SCK, MOSI, MISO) and are
    selected by separate chip-select (SS) pins.

    Presence-based dedup: only sends UID when a NEW tag appears on a reader.
    Game-side dedup prevents duplicate spawns; no REMOVE events needed.

    Wiring (Arduino hardware SPI):
        SCK   -> pin 13 (shared)
        MOSI  -> pin 11 (shared)
        MISO  -> pin 12 (shared)
        SS1   -> pin 10 (reader 1 chip select)
        SS2   -> pin 9  (reader 2 chip select)
        SS3   -> pin 8  (reader 3 chip select)
        SS4   -> pin 7  (reader 4 chip select)

    IMPORTANT: All CS pins are driven HIGH at startup before any reader is
    initialised, to prevent bus contention from floating pins.
*/
/**************************************************************************/
#include <SPI.h>
#include <Adafruit_PN532.h>

// Chip-select pins — one per reader
const int CS_PINS[4] = {10, 9, 8, 7};
#define NUM_READERS 4
#define POLL_TIMEOUT 50  // ms per reader — ~200ms full cycle

Adafruit_PN532 readers[NUM_READERS] = {
    Adafruit_PN532(CS_PINS[0]),
    Adafruit_PN532(CS_PINS[1]),
    Adafruit_PN532(CS_PINS[2]),
    Adafruit_PN532(CS_PINS[3]),
};

bool    readerOK[NUM_READERS];
uint8_t lastUID[NUM_READERS][7];
uint8_t lastUIDLen[NUM_READERS];

static void printHex(uint8_t *data, uint8_t len)
{
    for (uint8_t i = 0; i < len; i++) {
        if (data[i] < 0x10) Serial.print('0');
        Serial.print(data[i], HEX);
    }
}

void setup(void)
{
    Serial.begin(115200);
    while (!Serial) delay(10);
    delay(2000);  // Let SPI bus and readers stabilise after reset

    Serial.println("Hello! Quad-reader NFC init");

    // Drive ALL CS pins HIGH before initialising ANY reader
    for (int i = 0; i < NUM_READERS; i++) {
        pinMode(CS_PINS[i], OUTPUT);
        digitalWrite(CS_PINS[i], HIGH);
        readerOK[i] = false;
        lastUIDLen[i] = 0;
    }

    // Retry init up to 3 times — bridge opening serial resets Arduino via DTR,
    // and readers may not be ready on the first attempt.
    for (int attempt = 0; attempt < 3; attempt++) {
        int failCount = 0;
        for (int r = 0; r < NUM_READERS; r++) {
            if (readerOK[r]) continue;  // already init'd

            readers[r].begin();
            readers[r].setPassiveActivationRetries(0x01);

            uint32_t versiondata = readers[r].getFirmwareVersion();
            if (!versiondata) {
                failCount++;
                for (int j = 0; j < NUM_READERS; j++)
                    digitalWrite(CS_PINS[j], HIGH);
                continue;
            }

            Serial.print("Reader ");
            Serial.print(r + 1);
            Serial.print(": PN5");
            Serial.print((versiondata >> 24) & 0xFF, HEX);
            Serial.print(" fw ");
            Serial.print((versiondata >> 16) & 0xFF, DEC);
            Serial.print('.');
            Serial.println((versiondata >> 8) & 0xFF, DEC);

            readers[r].SAMConfig();
            readerOK[r] = true;
        }
        if (failCount == 0) break;  // all readers OK
        Serial.print("Retrying init (attempt ");
        Serial.print(attempt + 2);
        Serial.println("/3)...");
        delay(1000);
    }

    for (int r = 0; r < NUM_READERS; r++) {
        if (!readerOK[r]) {
            Serial.print("Didn't find PN53x board on reader ");
            Serial.println(r + 1);
        }
    }

    Serial.println("Waiting for ISO14443A cards ...");
}

void loop(void)
{
    for (int r = 0; r < NUM_READERS; r++) {
        if (!readerOK[r]) continue;

        uint8_t uid[7] = {0};
        uint8_t uidLength = 0;

        bool found = readers[r].readPassiveTargetID(
            PN532_MIFARE_ISO14443A, uid, &uidLength, POLL_TIMEOUT);

        if (found) {
            // Only send if this is a different tag than last seen on this reader
            bool same = (uidLength == lastUIDLen[r]) &&
                        (memcmp(uid, lastUID[r], uidLength) == 0);
            if (!same) {
                Serial.print("UID:");
                Serial.print(r + 1);
                Serial.print(":");
                printHex(uid, uidLength);
                Serial.println();

                memcpy(lastUID[r], uid, uidLength);
                lastUIDLen[r] = uidLength;
            }
        } else {
            // Tag removed — clear state so re-placing triggers a new send
            lastUIDLen[r] = 0;
        }
    }
}
