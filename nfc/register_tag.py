#!/usr/bin/env python3
"""
NFC Tag Registration Tool

Scans an NFC tag via Arduino serial, then registers it on the game server
with a unit type and rarity.

Usage:
    python3 register_tag.py

Environment:
    NFC_SERVER  - server hostname (default: autochess.kenzhiyilin.com)
    NFC_PORT    - server port (default: 7777)

Requires: pyserial (pip install pyserial)
"""

import os
import sys
import struct
import socket
import glob
import time

try:
    import serial
except ImportError:
    print("Error: pyserial not installed. Run: pip install pyserial")
    sys.exit(1)

# Protocol constants (must match net_protocol.h)
NET_MAGIC = 0x4A4D
MSG_NFC_REGISTER = 0x12
MSG_NFC_DATA = 0x91
NFC_STATUS_OK = 0

UNIT_TYPES = {
    "mushroom": 0, "goblin": 1, "cat": 2, "devil": 3, "fish": 4, "lizard": 5,
}
RARITIES = {"common": 0, "rare": 1, "legendary": 2}

SERVER = os.environ.get("NFC_SERVER", "autochess.kenzhiyilin.com")
PORT = int(os.environ.get("NFC_PORT", "7777"))


def find_serial_port():
    """Auto-detect Arduino serial port."""
    patterns = ["/dev/ttyACM*", "/dev/cu.usbmodem*"]
    for pat in patterns:
        ports = sorted(glob.glob(pat))
        if ports:
            return ports[0]
    return None


def net_send_msg(sock, msg_type, payload):
    """Send a protocol message: [magic:2][type:1][size:2] + payload."""
    header = struct.pack(">HBH", NET_MAGIC, msg_type, len(payload))
    sock.sendall(header + payload)


def net_recv_msg(sock):
    """Receive a protocol message. Returns (type, payload)."""
    header = b""
    while len(header) < 5:
        chunk = sock.recv(5 - len(header))
        if not chunk:
            raise ConnectionError("Connection closed")
        header += chunk
    magic, msg_type, size = struct.unpack(">HBH", header)
    if magic != NET_MAGIC:
        raise ValueError(f"Bad magic: 0x{magic:04X}")
    payload = b""
    while len(payload) < size:
        chunk = sock.recv(size - len(payload))
        if not chunk:
            raise ConnectionError("Connection closed")
        payload += chunk
    return msg_type, payload


def register_tag(uid_bytes, type_index, rarity):
    """Send MSG_NFC_REGISTER to server and return status."""
    # Payload: [uidLen:1][uid:N][typeIndex:1][rarity:1]
    payload = bytes([len(uid_bytes)]) + uid_bytes + bytes([type_index, rarity])

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(5)
    try:
        sock.connect((SERVER, PORT))
        net_send_msg(sock, MSG_NFC_REGISTER, payload)
        msg_type, resp = net_recv_msg(sock)
        if msg_type != MSG_NFC_DATA:
            print(f"  Unexpected response type: 0x{msg_type:02X}")
            return False
        # Parse: [uidLen:1][uid:N][status:1][typeIndex:1][rarity:1][abilities...]
        uid_len = resp[0]
        status = resp[1 + uid_len]
        return status == NFC_STATUS_OK
    except Exception as e:
        print(f"  Server error: {e}")
        return False
    finally:
        sock.close()


def main():
    # Get unit type
    print("Unit types:", ", ".join(f"{name}({idx})" for name, idx in UNIT_TYPES.items()))
    type_input = input("Type (name or number): ").strip().lower()
    if type_input.isdigit():
        type_index = int(type_input)
    elif type_input in UNIT_TYPES:
        type_index = UNIT_TYPES[type_input]
    else:
        print(f"Unknown type: {type_input}")
        sys.exit(1)

    # Get rarity
    print("Rarities:", ", ".join(f"{name}({idx})" for name, idx in RARITIES.items()))
    rarity_input = input("Rarity (name or number) [common]: ").strip().lower() or "common"
    if rarity_input.isdigit():
        rarity = int(rarity_input)
    elif rarity_input in RARITIES:
        rarity = RARITIES[rarity_input]
    else:
        print(f"Unknown rarity: {rarity_input}")
        sys.exit(1)

    type_name = [k for k, v in UNIT_TYPES.items() if v == type_index][0]
    rarity_name = [k for k, v in RARITIES.items() if v == rarity][0]
    print(f"\nWill register as: {type_name} (type={type_index}), {rarity_name} (rarity={rarity})")
    print(f"Server: {SERVER}:{PORT}")

    # Open serial
    port = find_serial_port()
    if not port:
        print("No Arduino serial port found!")
        sys.exit(1)

    print(f"Opening {port}...")
    ser = serial.Serial(port, 115200, timeout=1)
    time.sleep(2)  # Wait for Arduino reset

    print("\nScan a tag on any reader...")
    try:
        while True:
            line = ser.readline().decode("utf-8", errors="replace").strip()
            if not line:
                continue
            if line.startswith("UID:"):
                # Parse "UID:<reader>:<hex_uid>"
                parts = line.split(":", 2)
                if len(parts) < 3:
                    continue
                reader = parts[1]
                hex_uid = parts[2].upper()
                uid_bytes = bytes.fromhex(hex_uid)
                print(f"\n  Reader {reader}: UID {hex_uid} ({len(uid_bytes)} bytes)")
                print(f"  Registering as {type_name} ({rarity_name})...")

                if register_tag(uid_bytes, type_index, rarity):
                    print("  SUCCESS! Tag registered.")
                else:
                    print("  FAILED to register tag.")

                print("\nScan another tag or Ctrl+C to exit...")
            else:
                # Debug: print other serial output
                print(f"  [serial] {line}")
    except KeyboardInterrupt:
        print("\nDone.")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
