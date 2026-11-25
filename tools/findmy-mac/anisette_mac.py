#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Native macOS anisette: the one-time headers Apple's servers want, from the Mac itself.

Since 12 October 2026 Apple refuses logins that use the Android anisette of
anisette-v3-server (error -80045), and only hands the Find My report token
(searchPartyToken) to a macOS or iOS identity. A real Mac has that identity:
the private AOSKit framework makes the headers (dchristl/macless-haystack#248).

    python3 anisette_mac.py          # probe: prints what AOSKit returns (lengths only)

Needs pyobjc:  python3 -m pip install --user pyobjc-core pyobjc-framework-Cocoa
"""
import base64
import json
import locale
import os
import sys
import uuid
from datetime import datetime, timezone
from pathlib import Path

STATE = Path(os.environ.get("KODEFIND_HOME", Path.home() / ".kodefind")) / "findmy-mac"


def _aos():
    from Foundation import NSBundle, NSClassFromString  # pyobjc

    b = NSBundle.bundleWithPath_("/System/Library/PrivateFrameworks/AOSKit.framework")
    if b is None or not b.load():
        raise RuntimeError("AOSKit.framework could not be loaded")
    u = NSClassFromString("AOSUtilities")
    if u is None:
        raise RuntimeError("AOSUtilities is missing from AOSKit")
    return u


def _ids():
    """A stable user id for X-Apple-I-MD-LU, kept between runs like a real client."""
    STATE.mkdir(mode=0o700, parents=True, exist_ok=True)
    p = STATE / "ids.json"
    if p.exists():
        return json.loads(p.read_text())
    d = {"user_id": str(uuid.uuid4()).upper()}
    p.write_text(json.dumps(d))
    os.chmod(p, 0o600)
    return d


def headers(dsid: str = "-2") -> dict:
    """The full anisette header set, with the Mac's own OTP and machine id."""
    u = _aos()
    h = u.retrieveOTPHeadersForDSID_(dsid)
    if h is None or "X-Apple-MD" not in h or "X-Apple-MD-M" not in h:
        raise RuntimeError(f"AOSKit gave no OTP headers: {h!r}")
    udid = None
    serial = "0"
    try:
        udid = str(u.machineUDID())
    except Exception:  # not on every macOS
        pass
    try:
        serial = str(u.machineSerialNumber()) or "0"
    except Exception:
        pass
    ids = _ids()
    loc = locale.getdefaultlocale()[0] or "en_US"
    return {
        "X-Apple-I-MD": str(h["X-Apple-MD"]),
        "X-Apple-I-MD-M": str(h["X-Apple-MD-M"]),
        "X-Apple-I-Client-Time": datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
        "X-Apple-I-TimeZone": str(datetime.now(timezone.utc).astimezone().tzinfo),
        "loc": loc,
        "X-Apple-Locale": loc,
        "X-Apple-I-MD-RINFO": "17106176",
        "X-Apple-I-MD-LU": base64.b64encode(ids["user_id"].encode()).decode(),
        "X-Mme-Device-Id": (udid or ids["user_id"]).upper(),
        "X-Apple-I-SRL-NO": serial,
    }


if __name__ == "__main__":
    try:
        h = headers()
    except ImportError:
        sys.exit("pyobjc is missing: python3 -m pip install --user pyobjc-core pyobjc-framework-Cocoa")
    except Exception as e:  # report, do not hide
        sys.exit(f"AOSKit does not work here: {e}")
    for k, v in h.items():
        shown = v if k in ("X-Apple-I-MD-RINFO", "X-Apple-I-TimeZone", "X-Apple-Locale", "loc") else f"<{len(v)} chars>"
        print(f"{k}: {shown}")
    print("OK: this Mac can make anisette headers")
