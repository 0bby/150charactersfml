/**************************************************************************/
/*!
    Quad PN532 NFC reader for Arduino — UID-only output.

    Four PN532 boards share the hardware SPI bus (SCK, MOSI, MISO) and are
    selected by separate chip-select (SS) pins.  The firmware polls each
    reader in turn with a short timeout and prints the tag UID as hex,
    prefixed with the reader number:

        UID:1:04A3B2C1D2E3F4   (7-byte NTAG2xx UID from reader 1)
        UID:2:AABBCCDD          (4-byte Mifare Classic UID from reader 2)

    The server maps UID -> {type, rarity, abilities} via nfc_tags.json.

    Wiring (Arduino hardware SPI):
        SCK   -> pin 13 (shared)
        MOSI  -> pin 11 (shared)
        MISO  -> pin 12 (shared)
        SS1   -> pin 10 (reader 1 chip select)
        SS2   -> pin 9  (reader 2 chip select)
        SS3   -> pin 8  (reader 3 chip select)
        SS4   -> pin 7  (reader 4 chip select)

    All PN532 boards need SCK/MOSI/MISO wired in parallel; only SS differs.
    Set each PN532 to SPI mode (DIP switches / solder jumpers).
*/
/**************************************************************************/
#include <SPI.h>
#include <Adafruit_PN532.h>

// Chip-select pins — one per reader
#define PN532_SS_1  (10)
#define PN532_SS_2  (9)
#define PN532_SS_3  (8)
#define PN532_SS_4  (7)

// Number of readers
#define NUM_READERS 4

// Hardware SPI with manual chip-select
Adafruit_PN532 readers[NUM_READERS] = {
    Adafruit_PN532(PN532_SS_1),
    Adafruit_PN532(PN532_SS_2),
    Adafruit_PN532(PN532_SS_3),
    Adafruit_PN532(PN532_SS_4),
};

// Debounce: ignore the same UID on the same reader for this many ms
#define DEBOUNCE_MS 2000
uint8_t  lastUID[NUM_READERS][7];
uint8_t  lastUIDLen[NUM_READERS];
uint32_t lastReadTime[NUM_READERS];

/**************************************************************************/
/*!  Return true if uid matches the last-seen UID on this reader.         */
/**************************************************************************/
static bool isSameTag(int r, uint8_t *uid, uint8_t uidLen)
{
    if (uidLen != lastUIDLen[r]) return false;
    return memcmp(uid, lastUID[r], uidLen) == 0;
}

/**************************************************************************/
/*!  Print a byte array as uppercase hex.                                 */
/**************************************************************************/
static void printHex(uint8_t *data, uint8_t len)
{
    for (uint8_t i = 0; i < len; i++) {
        if (data[i] < 0x10) Serial.print('0');
        Serial.print(data[i], HEX);
    }
}

/**************************************************************************/
/*!  Arduino setup — initialise all four readers.                         */
/**************************************************************************/
void setup(void)
{
    Serial.begin(115200);
    while (!Serial) delay(10);

    Serial.println("Hello! Quad-reader NFC init");

    for (int r = 0; r < NUM_READERS; r++) {
        readers[r].begin();

        // Fail fast: only try once per poll (non-blocking-ish)
        readers[r].setPassiveActivationRetries(0x01);

        uint32_t versiondata = readers[r].getFirmwareVersion();
        if (!versiondata) {
            Serial.print("Didn't find PN53x board on reader ");
            Serial.println(r + 1);
            // Don't halt — the other readers may still work
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

        // Configure to read ISO14443A tags
        readers[r].SAMConfig();

        lastUIDLen[r] = 0;
        lastReadTime[r] = 0;
    }

    Serial.println("Waiting for ISO14443A cards ...");
}

/**************************************************************************/
/*!  Main loop — poll each reader in turn, output UID as hex.             */
/**************************************************************************/
void loop(void)
{
    for (int r = 0; r < NUM_READERS; r++) {
        uint8_t uid[7] = {0};
        uint8_t uidLength = 0;

        // Short timeout so we cycle through all 4 readers quickly
        bool found = readers[r].readPassiveTargetID(
            PN532_MIFARE_ISO14443A, uid, &uidLength, 150);

        if (!found) continue;

        // Debounce: skip if same tag was just read on this reader
        uint32_t now = millis();
        if (isSameTag(r, uid, uidLength) &&
            (now - lastReadTime[r]) < DEBOUNCE_MS) {
            continue;
        }

        // Output: UID:<reader>:<hex_uid>
        Serial.print("UID:");
        Serial.print(r + 1);  // 1-indexed
        Serial.print(":");
        printHex(uid, uidLength);
        Serial.println();

        // Debounce tracking
        memcpy(lastUID[r], uid, uidLength);
        lastUIDLen[r] = uidLength;
        lastReadTime[r] = now;
    }
}
