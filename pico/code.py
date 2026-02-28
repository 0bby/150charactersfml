import board
import busio
import digitalio
import time
from adafruit_pn532.spi import PN532_SPI

SPI_SCK = board.GP2
SPI_MOSI = board.GP3
SPI_MISO = board.GP4

time.sleep(2)

# All 4 CS pins must be driven HIGH at all times to prevent bus contention.
cs_pins = []
for pin in [board.GP5, board.GP6, board.GP7, board.GP8]:
    cs = digitalio.DigitalInOut(pin)
    cs.direction = digitalio.Direction.OUTPUT
    cs.value = True
    cs_pins.append(cs)

spi = busio.SPI(SPI_SCK, MOSI=SPI_MOSI, MISO=SPI_MISO)

# Init readers
ACTIVE = [0, 1, 2, 3]  # 0=GP5, 1=GP6, 2=GP7, 3=GP8
readers = [None, None, None, None]
init_errors = [None, None, None, None]
for i in ACTIVE:
    try:
        pn532 = PN532_SPI(spi, cs_pins[i])
        fw = pn532.firmware_version
        pn532.SAM_configuration()
        readers[i] = pn532
    except Exception as e:
        init_errors[i] = str(e)

# Print status AFTER a delay so bridge can see it
time.sleep(5)
print("=== READER STATUS ===")
for i in ACTIVE:
    if readers[i] is not None:
        print("Reader %d (GP%d): OK" % (i, 5 + i))
    else:
        print("Reader %d (GP%d): FAILED - %s" % (i, 5 + i, init_errors[i]))
print("=== %d/%d readers active ===" % (sum(1 for r in readers if r), len(ACTIVE)))
print("Waiting for an ISO14443A Card ...")


def read_ndef_text(pn532):
    """Read NDEF text from NTAG215. Returns string or None."""
    data = bytearray()
    for page in range(4, 11):
        try:
            page_data = pn532.ntag2xx_read_block(page)
            if page_data is None:
                return None
            data.extend(page_data[:4])
        except Exception:
            return None

    off = 0
    while off < len(data):
        tlv_type = data[off]
        off += 1
        if tlv_type == 0x00:
            continue
        if tlv_type == 0xFE:
            break
        if data[off] == 0xFF:
            off += 1
            tlv_len = (data[off] << 8) | data[off + 1]
            off += 2
        else:
            tlv_len = data[off]
            off += 1
        if tlv_type != 0x03:
            off += tlv_len
            continue
        flags = data[off]
        type_len = data[off + 1]
        sr = flags & 0x10
        il = flags & 0x08
        tnf = flags & 0x07
        pos = off + 2
        if sr:
            payload_len = data[pos]
            pos += 1
        else:
            payload_len = (data[pos] << 24) | (data[pos+1] << 16) | (data[pos+2] << 8) | data[pos+3]
            pos += 4
        id_len = 0
        if il:
            id_len = data[pos]
            pos += 1
        rec_type = data[pos]
        pos += type_len + id_len
        if tnf == 0x01 and type_len == 1 and rec_type == ord('T'):
            lang_len = data[pos] & 0x3F
            pos += 1 + lang_len
            payload_len -= 1 + lang_len
        elif tnf == 0x01 and type_len == 1 and rec_type == ord('U'):
            pos += 1
            payload_len -= 1
        text = ""
        for j in range(payload_len):
            if pos + j < len(data):
                text += chr(data[pos + j])
        return text

    return None


# --- Fast round-robin polling with NDEF retry ---
POLL_TIMEOUT = 0.05
MAX_SPI_ERRORS = 5
MAX_NDEF_RETRIES = 10

last_uids = [None, None, None, None]
pending_ndef = [False, False, False, False]
retry_count = [0, 0, 0, 0]
err_count = [0, 0, 0, 0]

while True:
    for i in ACTIVE:
        if readers[i] is None:
            continue

        try:
            uid = readers[i].read_passive_target(timeout=POLL_TIMEOUT)
            err_count[i] = 0
        except Exception as e:
            err_count[i] += 1
            if err_count[i] >= MAX_SPI_ERRORS:
                print("Reader %d disabled after %d SPI errors: %s" % (i, MAX_SPI_ERRORS, e))
                readers[i] = None
            continue

        if uid is not None:
            uid_hex = "".join(["{:02X}".format(b) for b in uid])

            if uid_hex != last_uids[i]:
                # New tag appeared
                last_uids[i] = uid_hex
                text = read_ndef_text(readers[i])
                if text is not None:
                    print("PAYLOAD:" + text)
                    pending_ndef[i] = False
                    retry_count[i] = 0
                else:
                    # NDEF failed — don't send anything yet, retry next cycle
                    pending_ndef[i] = True
                    retry_count[i] = 1

            elif pending_ndef[i]:
                # Same tag still present, NDEF pending — retry
                text = read_ndef_text(readers[i])
                if text is not None:
                    print("PAYLOAD:" + text)
                    pending_ndef[i] = False
                    retry_count[i] = 0
                else:
                    retry_count[i] += 1
                    if retry_count[i] >= MAX_NDEF_RETRIES:
                        # Give up, send raw UID as last resort
                        print("PAYLOAD:" + uid_hex)
                        pending_ndef[i] = False
                        retry_count[i] = 0
        else:
            if last_uids[i] is not None:
                print("REMOVED:%d" % i)
            last_uids[i] = None
            pending_ndef[i] = False
            retry_count[i] = 0
