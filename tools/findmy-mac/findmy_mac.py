#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fetch Apple Find My reports for your OpenHaystack keys, from your own Mac.

Apple only gives the report token to a macOS/iOS identity since October 2026,
so the login and every fetch use the Mac's native anisette (anisette_mac.py).
The reports stay encrypted: only the private keys in ~/.kodefind/haystack can
read them, and those never leave the Mac. They are pushed to the findmy page,
whose browser code decrypts them with the key you imported there.

    python3 findmy_mac.py --mh ~/macless-haystack login     # once: Apple ID, password, SMS code
    python3 findmy_mac.py fetch                              # fetch + push (what launchd runs)
    python3 findmy_mac.py install                            # every 15 min via launchd
    python3 findmy_mac.py uninstall

The login reuses macless-haystack's endpoint/register code (AGPL-3.0,
github.com/dchristl/macless-haystack); --mh points at a clone of it.
"""
import argparse
import base64
import hashlib
import json
import os
import plistlib
import subprocess
import sys
import time
from pathlib import Path

import anisette_mac

HOME = Path(os.environ.get("KODEFIND_HOME", Path.home() / ".kodefind"))
STATE = HOME / "findmy-mac"
KEYS = HOME / "haystack"
AUTH = STATE / "auth.json"
REPORTS = STATE / "reports.json"
KEEP_S = 7 * 24 * 3600
APPLE_EPOCH = 978307200
DEFAULT_PUSH = "ssh -o BatchMode=yes root@pve2 \"pct exec 110 -- sh -c 'cat > /opt/findmy-cache/reports.json'\""
LABEL = "nl.uselesssite.findmy"


def hashed_ids():
    """The hashed advertisement keys Apple indexes reports by, from genkey.py's .keys files."""
    ids = []
    for p in sorted(KEYS.glob("*.keys")):
        for line in p.read_text().splitlines():
            if line.startswith("Advertisement key: "):
                adv = base64.b64decode(line.split(": ", 1)[1].strip())
                ids.append(base64.b64encode(hashlib.sha256(adv).digest()).decode())
    return ids


def login(args):
    if not args.mh:
        sys.exit("--mh /path/to/macless-haystack is needed for the login")
    root = os.path.abspath(os.path.expanduser(args.mh))
    if not os.path.isfile(os.path.join(root, "endpoint", "register", "pypush_gsa_icloud.py")):
        sys.exit(f"{root} is not a macless-haystack clone")
    sys.path.insert(0, root)
    from endpoint.register import pypush_gsa_icloud as gsa  # noqa: E402

    gsa.generate_anisette_headers = anisette_mac.headers   # the Mac's identity, not Android's
    m = gsa.icloud_login_mobileme()
    tokens = m.get("delegates", {}).get("com.apple.mobileme", {}).get("service-data", {}).get("tokens", {})
    if "searchPartyToken" not in tokens:
        sys.exit(f"Logged in, but Apple gave no searchPartyToken (tokens: {sorted(tokens)}).")
    STATE.mkdir(mode=0o700, parents=True, exist_ok=True)
    AUTH.write_text(json.dumps({"dsid": m["dsid"], "searchPartyToken": tokens["searchPartyToken"]}))
    os.chmod(AUTH, 0o600)
    print(f"[findmy-mac] logged in; token kept in {AUTH}")


def fetch(args):
    import requests

    if not AUTH.exists():
        sys.exit("run 'login' first")
    a = json.loads(AUTH.read_text())
    ids = hashed_ids()
    if not ids:
        sys.exit(f"no keys in {KEYS} (tools/haystack/genkey.py makes them)")
    r = requests.post("https://gateway.icloud.com/acsnservice/fetch",
                      auth=(a["dsid"], a["searchPartyToken"]),
                      headers=anisette_mac.headers(),
                      json={"search": [{"startDate": 1, "ids": ids}]}, timeout=60)
    if r.status_code == 401:
        sys.exit("Apple says 401: the token expired; run 'login' again")
    r.raise_for_status()
    new = r.json().get("results", [])

    # Keep a week of reports, so the page has history even if Apple drops old ones.
    old = json.loads(REPORTS.read_text()).get("results", []) if REPORTS.exists() else []
    seen, merged = set(), []
    cut = time.time() - KEEP_S
    for e in new + old:
        try:
            ts = int.from_bytes(base64.b64decode(e["payload"])[0:4], "big") + APPLE_EPOCH
        except (KeyError, ValueError):
            continue
        k = (e.get("id"), e.get("payload"))
        if ts < cut or k in seen:
            continue
        seen.add(k)
        merged.append(e)
    doc = {"results": merged, "statusCode": "200", "fetched": int(time.time())}
    STATE.mkdir(mode=0o700, parents=True, exist_ok=True)
    REPORTS.write_text(json.dumps(doc))
    print(f"[findmy-mac] {len(new)} new reports from Apple, {len(merged)} kept, {len(ids)} keys")
    if args.push != "none":
        p = subprocess.run(args.push, shell=True, input=json.dumps(doc).encode(), capture_output=True, timeout=60)
        if p.returncode:
            sys.exit(f"[findmy-mac] push failed: {p.stderr.decode().strip()}")
        print("[findmy-mac] pushed to the findmy page")


def install(args):
    agents = Path.home() / "Library" / "LaunchAgents"
    agents.mkdir(parents=True, exist_ok=True)
    plist = agents / f"{LABEL}.plist"
    STATE.mkdir(mode=0o700, parents=True, exist_ok=True)
    d = {
        "Label": LABEL,
        "ProgramArguments": [sys.executable, str(Path(__file__).resolve()), "fetch"],
        "StartInterval": 900,
        "RunAtLoad": True,
        "StandardOutPath": str(STATE / "launchd.log"),
        "StandardErrorPath": str(STATE / "launchd.log"),
        "EnvironmentVariables": {"PATH": "/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin:/usr/local/bin"},
    }
    plist.write_bytes(plistlib.dumps(d))
    subprocess.run(["launchctl", "unload", str(plist)], capture_output=True)
    subprocess.run(["launchctl", "load", str(plist)], check=True)
    print(f"[findmy-mac] every 15 min via {plist}; log in {STATE / 'launchd.log'}")


def uninstall(args):
    plist = Path.home() / "Library" / "LaunchAgents" / f"{LABEL}.plist"
    subprocess.run(["launchctl", "unload", str(plist)], capture_output=True)
    plist.unlink(missing_ok=True)
    print("[findmy-mac] removed")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    l = sub.add_parser("login")
    l.add_argument("--mh", default=str(Path.home() / "macless-haystack"))
    f = sub.add_parser("fetch")
    f.add_argument("--push", default=DEFAULT_PUSH, help="shell command that takes the JSON on stdin, or 'none'")
    sub.add_parser("install")
    sub.add_parser("uninstall")
    a = ap.parse_args()
    {"login": login, "fetch": fetch, "install": install, "uninstall": uninstall}[a.cmd](a)


if __name__ == "__main__":
    main()
