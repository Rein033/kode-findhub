#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Make an OpenHaystack key pair for a Kode Dot or Kode-watch (Apple Find My).

    python3 genkey.py --name "Kode Dot"            # writes ~/.kodefind/haystack/<name>.keys + .json
    python3 genkey.py --name "Kode Dot" --port /dev/cu.usbmodem101 [--watch]

The Dot or watch sends the 28-byte public key ("advertisement key") as
Apple's offline-finding advert. iPhones nearby report their location,
encrypted to that key, to Apple. Only the private key decrypts it. Fetch the
reports with macless-haystack (https://github.com/dchristl/macless-haystack:
an Apple ID with 2FA and its anisette Docker container, no Mac needed) or
with OpenHaystack on a Mac. Both read the .keys file written here.

The key format is the one of OpenHaystack/macless-haystack generate_keys.py:
private key = P-224 scalar (28 bytes), advertisement key = x of d*G
(28 bytes), hashed key = SHA-256(advertisement key), each base64.
"""
import argparse
import base64
import hashlib
import json
import os
import sys
import time
from pathlib import Path

from cryptography.hazmat.primitives.asymmetric import ec

STORE = Path(os.environ.get("KODEFIND_HOME", Path.home() / ".kodefind")) / "haystack"


def make_key():
    while True:
        priv = ec.generate_private_key(ec.SECP224R1())
        d = priv.private_numbers().private_value.to_bytes(28, "big")
        x = priv.public_key().public_numbers().x.to_bytes(28, "big")
        # OpenHaystack's own generator retries keys whose hash starts with '/' in base64:
        # the hashed key is used in a file name there.
        h = base64.b64encode(hashlib.sha256(x).digest()).decode()
        if "/" not in h[:7]:
            return d, x, h


def send(port, line, watch):
    import serial

    with serial.Serial() as s:
        s.port, s.baudrate, s.timeout, s.dtr, s.rts = port, 115200, 0.2, False, False
        s.open()
        if watch:
            time.sleep(6)   # the watch resets when its port opens
        s.reset_input_buffer()
        s.write((line + "\n").encode())
        end = time.time() + 8
        while time.time() < end:
            raw = s.readline().decode(errors="replace").strip()
            if raw.startswith((">ok", ">err", "findmy ")):
                return raw
    return "(no answer)"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--name", default="Kode Dot")
    ap.add_argument("--port", help="write the key to the device over USB")
    ap.add_argument("--watch", action="store_true", help="the device is a Kode-watch")
    a = ap.parse_args()

    STORE.mkdir(mode=0o700, parents=True, exist_ok=True)
    safe = "".join(c if c.isalnum() or c in "-_" else "_" for c in a.name)
    keys = STORE / f"{safe}.keys"
    if keys.exists():
        # Never overwrite a key: reuse it (for a second try, or a reset device).
        x = base64.b64decode(keys.read_text().split("Advertisement key: ")[1].split()[0])
        print(f"[genkey] using the existing key in {keys}")
        line = (f"findmy key {x.hex()}" if a.watch else f'findmy_set key="{x.hex()}"')
        if a.port:
            print(send(a.port, line, a.watch))
        else:
            print("  " + line)
        return
    d, x, h = make_key()
    keys.write_text(f"Private key: {base64.b64encode(d).decode()}\n"
                    f"Advertisement key: {base64.b64encode(x).decode()}\n"
                    f"Hashed adv key: {h}\n")
    os.chmod(keys, 0o600)
    j = STORE / f"{safe}.json"
    j.write_text(json.dumps([{
        "id": int.from_bytes(hashlib.sha256(x).digest()[:4], "big"),
        "name": a.name, "privateKey": base64.b64encode(d).decode(),
        "advertisementKey": base64.b64encode(x).decode(), "hashedKey": h,
        "isActive": True, "isDeployed": True, "colorComponents": [0, 0.54, 0.76, 1], "icon": "",
    }], indent=2))
    os.chmod(j, 0o600)
    print(f"[genkey] keys in {keys} (back them up)")
    line = (f"findmy key {x.hex()}" if a.watch else f'findmy_set key="{x.hex()}"')
    if a.port:
        print(send(a.port, line, a.watch))
    else:
        print("Send this to the device over USB (the public key only; the private key stays here):")
        print("  " + line)


if __name__ == "__main__":
    main()
