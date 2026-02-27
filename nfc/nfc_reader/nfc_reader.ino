/**************************************************************************/
/*!
    @file     readMifareClassic.pde
    @author   Adafruit Industries
	@license  BSD (see license.txt)

    This example will wait for any ISO14443A card or tag, and
    depending on the size of the UID will attempt to read from it.

    If the card has a 4-byte UID it is probably a Mifare
    Classic card, and the following steps are taken:

    Reads the 4 byte (32 bit) ID of a MiFare Classic card.
    Since the classic cards have only 32 bit identifiers you can stick
	them in a single variable and use that to compare card ID's as a
	number. This doesn't work for ultralight cards that have longer 7
	byte IDs!

    Note that you need the baud rate to be 115200 because we need to
	print out the data and read from the card at the same time!

This is an example sketch for the Adafruit PN532 NFC/RFID breakout boards
This library works with the Adafruit NFC breakout
  ----> https://www.adafruit.com/products/364

Check out the links above for our tutorials and wiring diagrams
These chips use SPI to communicate, 4 required to interface

Adafruit invests time and resources providing this open source code,
please support Adafruit and open-source hardware by purchasing
products from Adafruit!
*/
/**************************************************************************/
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_PN532.h>

// If using the breakout with SPI, define the pins for SPI communication.
#define PN532_SCK  (2)
#define PN532_MOSI (3)
#define PN532_SS   (4)
#define PN532_MISO (5)

// If using the breakout or shield with I2C, define just the pins connected
// to the IRQ and reset lines.  Use the values below (2, 3) for the shield!
#define PN532_IRQ   (2)
#define PN532_RESET (3)  // Not connected by default on the NFC Shield

// Uncomment just _one_ line below depending on how your breakout or shield
// is connected to the Arduino:

// Use this line for a breakout with a SPI connection:
//Adafruit_PN532 nfc(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS);

// Use this line for a breakout with a hardware SPI connection.  Note that
// the PN532 SCK, MOSI, and MISO pins need to be connected to the Arduino's
// hardware SPI SCK, MOSI, and MISO pins.  On an Arduino Uno these are
// SCK = 13, MOSI = 11, MISO = 12.  The SS line can be any digital IO pin.
//Adafruit_PN532 nfc(PN532_SS);

// Or use this line for a breakout or shield with an I2C connection:
Adafruit_PN532 nfc(PN532_IRQ, PN532_RESET);

void setup(void) {
  Serial.begin(115200);
  while (!Serial) delay(10); // for Leonardo/Micro/Zero

  Serial.println("Hello!");

  nfc.begin();

  uint32_t versiondata = nfc.getFirmwareVersion();
  if (! versiondata) {
    Serial.print("Didn't find PN53x board");
    while (1); // halt
  }
  // Got ok data, print it out!
  Serial.print("Found chip PN5"); Serial.println((versiondata>>24) & 0xFF, HEX);
  Serial.print("Firmware ver. "); Serial.print((versiondata>>16) & 0xFF, DEC);
  Serial.print('.'); Serial.println((versiondata>>8) & 0xFF, DEC);

  Serial.println("Waiting for an ISO14443A Card ...");
}


void loop(void) {
  uint8_t success;
  uint8_t uid[] = { 0, 0, 0, 0, 0, 0, 0 };  // Buffer to store the returned UID
  uint8_t uidLength;                        // Length of the UID (4 or 7 bytes depending on ISO14443A card type)

  // Wait for an ISO14443A type cards (Mifare, etc.).  When one is found
  // 'uid' will be populated with the UID, and uidLength will indicate
  // if the uid is 4 bytes (Mifare Classic) or 7 bytes (Mifare Ultralight)
  success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength);

  if (success) {
    // Display some basic information about the card
    Serial.println("Found an ISO14443A card");
    Serial.print("  UID Length: ");Serial.print(uidLength, DEC);Serial.println(" bytes");
    Serial.print("  UID Value: ");
    nfc.PrintHex(uid, uidLength);

    if (uidLength == 4)
    {
      // We probably have a Mifare Classic card ...
      uint32_t cardid = uid[0];
      cardid <<= 8;
      cardid |= uid[1];
      cardid <<= 8;
      cardid |= uid[2];
      cardid <<= 8;
      cardid |= uid[3];
      Serial.print("Seems to be a Mifare Classic card #");
      Serial.println(cardid);
    }

    if (uidLength == 7)
    {
      // NTAG215 (Ultralight-compatible, 7-byte UID)
      Serial.println("Detected NTAG2xx tag");

      // Read user pages 4-48 into buffer (180 bytes, enough for ~150 char payloads)
      uint8_t data[180];
      uint8_t pageBuf[16]; // ReadPage returns up to 16 bytes
      bool readOk = true;

      for (uint8_t page = 4; page < 49; page++) {
        if (!nfc.mifareultralight_ReadPage(page, pageBuf)) {
          readOk = false;
          break;
        }
        memcpy(&data[(page - 4) * 4], pageBuf, 4);
      }

      if (readOk) {
        // Walk TLV blocks to find NDEF message (type 0x03)
        int off = 0;
        while (off < (int)sizeof(data)) {
          uint8_t tlvType = data[off++];
          if (tlvType == 0x00) continue;   // NULL TLV
          if (tlvType == 0xFE) break;      // Terminator

          // Read TLV length (1 or 3 bytes)
          uint16_t tlvLen;
          if (data[off] == 0xFF) {
            off++;
            tlvLen = ((uint16_t)data[off] << 8) | data[off + 1];
            off += 2;
          } else {
            tlvLen = data[off++];
          }

          if (tlvType != 0x03) {
            off += tlvLen; // skip non-NDEF TLVs
            continue;
          }

          // Parse NDEF record header
          uint8_t flags   = data[off];
          uint8_t typeLen  = data[off + 1];
          bool    sr       = flags & 0x10;
          bool    il       = flags & 0x08;
          uint8_t tnf      = flags & 0x07;
          int     pos      = off + 2;

          uint32_t payloadLen;
          if (sr) {
            payloadLen = data[pos++];
          } else {
            payloadLen = ((uint32_t)data[pos] << 24) | ((uint32_t)data[pos+1] << 16) |
                         ((uint32_t)data[pos+2] << 8) | data[pos+3];
            pos += 4;
          }

          uint8_t idLen = 0;
          if (il) idLen = data[pos++];

          uint8_t recType = data[pos];
          pos += typeLen; // skip type field
          pos += idLen;   // skip ID field

          // Strip NDEF record overhead to get raw text
          if (tnf == 0x01 && typeLen == 1 && recType == 'T') {
            // NFC Forum Text Record: skip status byte + language code
            uint8_t langLen = data[pos] & 0x3F;
            pos += 1 + langLen;
            payloadLen -= 1 + langLen;
          } else if (tnf == 0x01 && typeLen == 1 && recType == 'U') {
            // NFC Forum URI Record: skip URI prefix byte
            pos += 1;
            payloadLen -= 1;
          }

          Serial.print("PAYLOAD:");
          for (uint32_t j = 0; j < payloadLen && (pos + (int)j) < (int)sizeof(data); j++) {
            Serial.print((char)data[pos + j]);
          }
          Serial.println();
          break;
        }
      } else {
        Serial.println("Failed to read tag pages");
      }
    }
    Serial.println("");
  }
}
