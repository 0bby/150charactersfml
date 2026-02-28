/**************************************************************************/
/*!
    NFC Tag Writer for Autochess Unit Tags

    Writes NDEF Text records to NTAG2xx tags via PN532 (I2C).

    Usage:
      1. Open Serial Monitor at 115200 baud
      2. Type a unit name: "mushroom", "shroom", "goblin" (case-insensitive)
         Or type a raw unit code like "0", "1", "0FB1XX..."
      3. Present tags one at a time — each gets written with that code
      4. Type a new name/code to switch, or "quit" to stop

    Unit codes:
      0 = Mushroom (Toad)
      1 = Goblin
      2 = Cat
      3 = Devil
      4 = Fish
      5 = Lizard
*/
/**************************************************************************/
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_PN532.h>

#define PN532_IRQ   (2)
#define PN532_RESET (3)

Adafruit_PN532 nfc(PN532_IRQ, PN532_RESET);

// Current unit code to write (set via serial)
char unitCode[64] = "";
bool codeReady = false;

// Serial input buffer
char inputBuf[64];
int inputPos = 0;

void setup(void) {
  Serial.begin(115200);
  while (!Serial) delay(10);

  Serial.println("=== NFC Unit Tag Writer ===");

  nfc.begin();
  Wire.setClock(400000);

  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.println("Didn't find PN53x board");
    while (1);
  }
  Serial.print("Found chip PN5"); Serial.println((versiondata >> 24) & 0xFF, HEX);

  Serial.println();
  Serial.println("Enter unit type: mushroom, goblin, cat, devil, fish, lizard");
  Serial.println("  Or a raw code like: 0, 1, 2, 3, 4, 5");
  Serial.println("  Type 'quit' to exit.");
  Serial.print("> ");
}

// Build an NDEF Text record TLV for the given string.
// Returns total bytes written into buf.
// Format: [03][len][NDEF header][payload][FE]
int buildNdefText(const char *text, uint8_t *buf, int bufSize) {
  int textLen = strlen(text);
  // NDEF Text record: flags(1) + typeLen(1) + payloadLen(1) + type'T'(1) + statusByte(1) + langCode"en"(2) + text
  int ndefRecordLen = 1 + 1 + 1 + 1 + 1 + 2 + textLen; // header portion
  int payloadLen = 1 + 2 + textLen;  // status byte + "en" + text

  int pos = 0;

  // TLV: type = 0x03 (NDEF message)
  buf[pos++] = 0x03;
  // TLV length = NDEF record total
  int ndefLen = 1 + 1 + 1 + 1 + payloadLen; // flags + typeLen + payloadLen + type + payload
  buf[pos++] = (uint8_t)ndefLen;

  // NDEF record header
  buf[pos++] = 0xD1;  // MB=1, ME=1, CF=0, SR=1, IL=0, TNF=001 (well-known)
  buf[pos++] = 0x01;  // type length = 1
  buf[pos++] = (uint8_t)payloadLen;
  buf[pos++] = 'T';   // type = Text

  // Text record payload
  buf[pos++] = 0x02;  // status byte: UTF-8, language code length = 2
  buf[pos++] = 'e';
  buf[pos++] = 'n';
  memcpy(buf + pos, text, textLen);
  pos += textLen;

  // Terminator TLV
  buf[pos++] = 0xFE;

  return pos;
}

bool writeNdefToTag(const uint8_t *ndefData, int ndefLen) {
  // NTAG2xx user memory starts at page 4, 4 bytes per page
  int totalPages = (ndefLen + 3) / 4;  // round up

  // Pad to 4-byte boundary
  uint8_t padded[256];
  memset(padded, 0, sizeof(padded));
  memcpy(padded, ndefData, ndefLen);

  for (int i = 0; i < totalPages; i++) {
    uint8_t page = 4 + i;
    if (page > 44) {  // NTAG213 has pages 4-39, NTAG215 has 4-129
      Serial.println("  Warning: data exceeds NTAG213 capacity, trying anyway...");
    }
    if (!nfc.mifareultralight_WritePage(page, padded + (i * 4))) {
      Serial.print("  FAILED to write page "); Serial.println(page);
      return false;
    }
  }
  return true;
}

void processInput(const char *input) {
  // Trim whitespace
  while (*input == ' ' || *input == '\t') input++;
  char trimmed[64];
  strncpy(trimmed, input, sizeof(trimmed) - 1);
  trimmed[sizeof(trimmed) - 1] = '\0';
  int len = strlen(trimmed);
  while (len > 0 && (trimmed[len-1] == ' ' || trimmed[len-1] == '\t' || trimmed[len-1] == '\r' || trimmed[len-1] == '\n')) {
    trimmed[--len] = '\0';
  }

  if (len == 0) return;

  // Check for quit
  if (strcasecmp(trimmed, "quit") == 0 || strcasecmp(trimmed, "exit") == 0) {
    Serial.println("Bye!");
    while (1);
  }

  // Map friendly names to unit codes
  if (strcasecmp(trimmed, "mushroom") == 0 || strcasecmp(trimmed, "shroom") == 0 || strcasecmp(trimmed, "toad") == 0) {
    strcpy(unitCode, "0");
  } else if (strcasecmp(trimmed, "goblin") == 0) {
    strcpy(unitCode, "1");
  } else if (strcasecmp(trimmed, "cat") == 0) {
    strcpy(unitCode, "2");
  } else if (strcasecmp(trimmed, "devil") == 0 || strcasecmp(trimmed, "demon") == 0) {
    strcpy(unitCode, "3");
  } else if (strcasecmp(trimmed, "fish") == 0) {
    strcpy(unitCode, "4");
  } else if (strcasecmp(trimmed, "lizard") == 0) {
    strcpy(unitCode, "5");
  } else {
    // Assume raw unit code — validate first char is a digit 0-5
    if (trimmed[0] >= '0' && trimmed[0] <= '5') {
      strcpy(unitCode, trimmed);
    } else {
      Serial.print("Unknown unit: '"); Serial.print(trimmed); Serial.println("'");
      Serial.println("Try: mushroom, goblin, cat, devil, fish, lizard, or a raw code (0-5)");
      Serial.print("> ");
      return;
    }
  }

  codeReady = true;
  Serial.print("Writing code '"); Serial.print(unitCode); Serial.println("' to tags.");
  Serial.println("Present a tag to write... (type new name to switch)");
}

void loop(void) {
  // Check for serial input
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (inputPos > 0) {
        inputBuf[inputPos] = '\0';
        processInput(inputBuf);
        inputPos = 0;
      }
    } else if (inputPos < (int)sizeof(inputBuf) - 1) {
      inputBuf[inputPos++] = c;
    }
  }

  // If no code set yet, just wait for input
  if (!codeReady) return;

  // Try to detect a tag
  uint8_t uid[7];
  uint8_t uidLength;

  if (!nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 500)) {
    return;  // No tag found, keep polling
  }

  if (uidLength != 7) {
    Serial.println("  Not an NTAG2xx tag (need 7-byte UID). Skipping.");
    delay(1000);
    return;
  }

  // Print UID
  Serial.print("  Tag UID: ");
  for (int i = 0; i < 7; i++) {
    if (uid[i] < 0x10) Serial.print("0");
    Serial.print(uid[i], HEX);
    if (i < 6) Serial.print(":");
  }
  Serial.println();

  // Build NDEF
  uint8_t ndefBuf[128];
  int ndefLen = buildNdefText(unitCode, ndefBuf, sizeof(ndefBuf));

  // Write it
  Serial.print("  Writing '"); Serial.print(unitCode); Serial.print("'...");
  if (writeNdefToTag(ndefBuf, ndefLen)) {
    Serial.println(" OK!");
  } else {
    Serial.println(" WRITE FAILED");
  }

  // Wait for tag to be removed before writing another
  Serial.println("  Remove tag, then present next one.");
  delay(2000);
}

// Case-insensitive string compare (not available on all Arduino cores)
int strcasecmp(const char *a, const char *b) {
  while (*a && *b) {
    char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
    char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
    if (ca != cb) return ca - cb;
    a++; b++;
  }
  return (unsigned char)*a - (unsigned char)*b;
}
