#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""kodefind: register a Kode Dot in Google's Find Hub, provision it, find it.

Built on GoogleFindMyTools (https://github.com/leonboe1/GoogleFindMyTools,
GPL-3.0, Leon Böttger). That project talks to Google's servers; this file
only changes what it uploads and how it decrypts, so the Dot can rotate its
identifier every 1024 s like a real tag instead of sending one forever:

  register   make a new identity key (EIK), register a tracker with your
             Google account, keep the key in ~/.kodefind/<name>.json and
             (with --port) write it to the Dot over USB.
  provision  write a saved key to a Dot again (after a factory reset).
  locate     list your trackers, refresh the rotating identifiers on
             Google's side (needed at least every 4 days) and print the
             last locations, decrypting each report with the right slot.
  static     print the 'findhub_set key=...' line for a tracker that the
             unmodified GoogleFindMyTools registered (its printed
             advertisement key), for the proven static mode.

Usage:
  python3 kodefind.py --gfmt /path/to/GoogleFindMyTools register --name "Kode Dot" --port /dev/ttyACM0
  python3 kodefind.py --gfmt /path/to/GoogleFindMyTools locate
  python3 kodefind.py provision --name "Kode Dot" --port /dev/ttyACM0
  python3 kodefind.py static --key <40 hex> --port /dev/ttyACM0
"""
import argparse
import json
import os
import secrets
import sys
import time
from pathlib import Path

STORE = Path(os.environ.get("KODEFIND_HOME", Path.home() / ".kodefind"))
ROTATION = 1024
WINDOW_S = 4 * 24 * 3600          # what Google keeps, like GoogleFindMyTools
SEARCH_SLOTS = 9 * 24 * 3600 // ROTATION   # how far back a report may be


# ---- local key store ---------------------------------------------------------

def store_path(name: str) -> Path:
    safe = "".join(c if c.isalnum() or c in "-_" else "_" for c in name)
    return STORE / f"{safe}.json"


def save(name: str, eik: bytes, pair_date: int) -> Path:
    STORE.mkdir(mode=0o700, parents=True, exist_ok=True)
    p = store_path(name)
    p.write_text(json.dumps({"name": name, "eik": eik.hex(), "pair_date": pair_date}, indent=2))
    os.chmod(p, 0o600)
    return p


def load_all():
    out = []
    if STORE.is_dir():
        for p in sorted(STORE.glob("*.json")):
            try:
                d = json.loads(p.read_text())
                out.append({"name": d["name"], "eik": bytes.fromhex(d["eik"]),
                            "pair_date": int(d["pair_date"])})
            except (KeyError, ValueError, OSError):
                print(f"[kodefind] skipping unreadable {p}", file=sys.stderr)
    return out


def rotating_for(eik: bytes):
    for d in load_all():
        if d["eik"] == eik:
            return d
    return None


# ---- the Dot's console ----------------------------------------------------------

def dot_send(port: str, line: str, timeout: float = 5.0) -> str:
    """Send one console line over USB and return the answer up to >ok / >err."""
    import serial  # pyserial

    with serial.Serial(port, 115200, timeout=0.2) as s:
        s.reset_input_buffer()
        s.write((line + "\n").encode())
        got = []
        end = time.time() + timeout
        while time.time() < end:
            raw = s.readline().decode(errors="replace").strip()
            if not raw:
                continue
            got.append(raw)
            if raw.startswith(">ok") or raw.startswith(">err"):
                break
        return "\n".join(got)


def provision_line(eik: bytes, pair_date: int, utp: bool) -> str:
    return f'findhub_set eik="{eik.hex()}" pair_date={pair_date} utp={"on" if utp else "off"}'


def provision(port, eik, pair_date, utp):
    line = provision_line(eik, pair_date, utp)
    if not port:
        print("Type this on the Dot's USB console (it is a secret: not over BLE):")
        print("  " + line)
        return
    ans = dot_send(port, line)
    print(ans)
    if ">ok" not in ans:
        sys.exit("[kodefind] the Dot did not accept the key")
    print(dot_send(port, "findhub_info"))


# ---- GoogleFindMyTools, adapted --------------------------------------------------

def gfmt_import(path: str):
    if not path:
        sys.exit("[kodefind] --gfmt /path/to/GoogleFindMyTools is needed for this command")
    path = os.path.abspath(path)
    if not os.path.isfile(os.path.join(path, "main.py")):
        sys.exit(f"[kodefind] {path} is not a GoogleFindMyTools checkout")
    os.chdir(path)              # it keeps Auth/secrets.json relative to itself
    sys.path.insert(0, path)


def eid_list(eik: bytes, pair_date: int, start: int, duration: int):
    """PublicKeyIdInfo entries for the rotating beacon: beacon time = unix - pair_date."""
    from FMDNCrypto.eid_generator import generate_eid
    from ProtoDecoders.DeviceUpdate_pb2 import PublicKeyIdList

    off = max(0, start - pair_date)
    off -= off % ROTATION
    last = max(0, start - pair_date) + duration
    out = []
    while off <= last:
        info = PublicKeyIdList.PublicKeyIdInfo()
        info.timestamp.seconds = pair_date + off
        info.publicKeyId.truncatedEid = generate_eid(eik, off)[:10]
        out.append(info)
        off += ROTATION
    return out


def patch_gfmt():
    """Make refresh and decryption understand rotating Kode Dots; others unchanged."""
    import SpotApi.UploadPrecomputedPublicKeyIds.upload_precomputed_public_key_ids as up
    import NovaApi.ExecuteAction.LocateTracker.decrypt_locations as dl

    orig_next = up.get_next_eids

    def get_next_eids(eik, pair_date, start_date, duration_seconds):
        mine = rotating_for(eik)
        if mine is None:
            return orig_next(eik, pair_date, start_date, duration_seconds)
        return eid_list(eik, mine["pair_date"], int(start_date), int(duration_seconds))

    up.get_next_eids = get_next_eids

    orig_decrypt = dl.decrypt
    last_hit = {}

    def decrypt(identity_key, encrypted, sx, time_offset):
        mine = rotating_for(identity_key)
        if mine is None:
            return orig_decrypt(identity_key, encrypted, sx, time_offset)
        now_t = int(time.time()) - mine["pair_date"]
        now_slot = max(0, now_t - now_t % ROTATION)
        tried = []
        if identity_key in last_hit:
            tried.append(last_hit[identity_key])
        if time_offset:
            tried.append(time_offset - time_offset % ROTATION)
        for i in range(SEARCH_SLOTS + 2):
            s = now_slot - (i - 1) * ROTATION       # one slot ahead, then back
            if s >= 0:
                tried.append(s)
        seen = set()
        for slot in tried:
            if slot in seen:
                continue
            seen.add(slot)
            try:
                plain = orig_decrypt(identity_key, encrypted, sx, slot)
                last_hit[identity_key] = slot
                return plain
            except ValueError:
                continue
        raise ValueError("no rotation slot decrypts this report")

    dl.decrypt = decrypt


def register(args):
    gfmt_import(args.gfmt)
    from FMDNCrypto.key_derivation import FMDNOwnerOperations
    from KeyBackup.cloud_key_decryptor import encrypt_aes_gcm
    from ProtoDecoders.DeviceUpdate_pb2 import DeviceComponentInformation, SpotDeviceType, RegisterBleDeviceRequest
    from SpotApi.CreateBleDevice.config import mcu_fast_pair_model_id
    from SpotApi.CreateBleDevice.util import flip_bits
    from SpotApi.GetEidInfoForE2eeDevices.get_owner_key import get_owner_key
    from SpotApi.spot_request import spot_request

    if store_path(args.name).exists() and not args.force:
        sys.exit(f"[kodefind] {store_path(args.name)} exists; pick another --name or pass --force")

    owner_key = get_owner_key()
    eik = secrets.token_bytes(32)
    pair_date = int(time.time())

    req = RegisterBleDeviceRequest()
    req.fastPairModelId = mcu_fast_pair_model_id
    req.description.userDefinedName = args.name
    req.description.deviceType = SpotDeviceType.DEVICE_TYPE_BEACON
    comp = DeviceComponentInformation()
    comp.imageUrl = args.image
    req.description.deviceComponentsInformation.append(comp)
    req.capabilities.isAdvertising = True
    req.capabilities.trackableComponents = 1
    req.capabilities.capableComponents = 1
    req.e2eePublicKeyRegistration.rotationExponent = 10
    req.e2eePublicKeyRegistration.pairingDate = pair_date
    secrets_ = req.e2eePublicKeyRegistration.encryptedUserSecrets
    # Bits flipped as GoogleFindMyTools does, so Android does not try to use the key.
    secrets_.encryptedIdentityKey = flip_bits(encrypt_aes_gcm(owner_key, eik), True)
    secrets_.encryptedAccountKey = secrets.token_bytes(44)
    secrets_.encryptedSha256AccountKeyPublicAddress = secrets.token_bytes(60)
    secrets_.ownerKeyVersion = 1
    secrets_.creationDate.seconds = pair_date
    for info in eid_list(eik, pair_date, pair_date, WINDOW_S):
        req.e2eePublicKeyRegistration.publicKeyIdList.publicKeyIdInfo.append(info)
    req.manufacturerName = "kode-findhub"
    req.modelName = "Kode Dot"
    keys = FMDNOwnerOperations()
    keys.generate_keys(identity_key=eik)
    req.ringKey = keys.ringing_key
    req.recoveryKey = keys.recovery_key
    req.unwantedTrackingKey = keys.tracking_key

    p = save(args.name, eik, pair_date)   # before the upload: never lose a key Google has
    spot_request("CreateBleDevice", req.SerializeToString())
    print(f"[kodefind] registered '{args.name}'; key kept in {p}")
    provision(args.port, eik, pair_date, not args.no_utp)


def cmd_provision(args):
    d = next((x for x in load_all() if x["name"] == args.name), None)
    if d is None:
        sys.exit(f"[kodefind] no saved tracker '{args.name}' in {STORE}")
    provision(args.port, d["eik"], d["pair_date"], not args.no_utp)


def cmd_static(args):
    key = bytes.fromhex(args.key)
    if len(key) != 20:
        sys.exit("[kodefind] the advertisement key is 40 hex digits")
    line = f'findhub_set key="{key.hex()}" utp={"off" if args.no_utp else "on"}'
    if args.port:
        print(dot_send(args.port, line))
    else:
        print("  " + line)


def locate(args):
    gfmt_import(args.gfmt)
    patch_gfmt()
    from NovaApi.ListDevices.nbe_list_devices import list_devices
    list_devices()   # refreshes the identifiers, then asks which tracker to locate


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gfmt", default=os.environ.get("GFMT_DIR"), help="GoogleFindMyTools checkout")
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("register")
    r.add_argument("--name", default="Kode Dot")
    r.add_argument("--port", help="the Dot's USB serial port, to provision at once")
    r.add_argument("--image", default="https://raw.githubusercontent.com/kodediy/KodeOS/main/.github/img/kode-dot.png")
    r.add_argument("--no-utp", action="store_true", help="frame 0x40 instead of 0x41")
    r.add_argument("--force", action="store_true")
    p = sub.add_parser("provision")
    p.add_argument("--name", default="Kode Dot")
    p.add_argument("--port")
    p.add_argument("--no-utp", action="store_true")
    s = sub.add_parser("static")
    s.add_argument("--key", required=True)
    s.add_argument("--port")
    s.add_argument("--no-utp", action="store_true")
    sub.add_parser("locate")
    a = ap.parse_args()
    {"register": register, "provision": cmd_provision, "static": cmd_static, "locate": locate}[a.cmd](a)


if __name__ == "__main__":
    main()
