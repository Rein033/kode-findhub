#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline checks of tools/kodefind against GoogleFindMyTools' own crypto.

    GFMT_DIR=/path/to/GoogleFindMyTools python3 tests/tools/test_kodefind.py

Nothing here talks to Google. It imports GoogleFindMyTools' modules, so run
it only on a checkout you trust.
"""
import os
import secrets
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "kodefind"))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "reference"))
os.environ["KODEFIND_HOME"] = tempfile.mkdtemp()

import kodefind as kf   # noqa: E402
import fhn_ref          # noqa: E402

kf.STORE = kf.Path(os.environ["KODEFIND_HOME"])
kf.gfmt_import(os.environ["GFMT_DIR"])
kf.patch_gfmt()

import NovaApi.ExecuteAction.LocateTracker.decrypt_locations as dl                 # noqa: E402
import SpotApi.UploadPrecomputedPublicKeyIds.upload_precomputed_public_key_ids as up  # noqa: E402
from FMDNCrypto.foreign_tracker_cryptor import encrypt                                 # noqa: E402

eik = secrets.token_bytes(32)
pair = int(time.time()) - 3 * 86400
kf.save("test", eik, pair)

# 1. What is uploaded for slot s is the EID the Dot sends at beacon time s..s+1023.
lst = kf.eid_list(eik, pair, pair + 86400, 4 * 3600)
assert lst, "empty list"
for info in lst:
    off = info.timestamp.seconds - pair
    assert off % 1024 == 0
    assert info.publicKeyId.truncatedEid == fhn_ref.eid(eik, off + 777)[:10]
print(f"eid_list ok ({len(lst)} slots)")

# 2. A report encrypted to yesterday's EID decrypts with time_offset 0, as GFMT passes it.
t = int(time.time()) - pair - 86400
enc, sx = encrypt(b"\x08\x96\x01 kode dot", secrets.token_bytes(32), fhn_ref.eid(eik, t))
assert dl.decrypt(eik, enc, sx, 0) == b"\x08\x96\x01 kode dot"
print("decrypt slot search ok")

# 3. A tracker that is not ours goes through the original code unchanged.
other = secrets.token_bytes(32)
l2 = up.get_next_eids(other, pair, pair, 3 * 1024)
assert len({x.publicKeyId.truncatedEid for x in l2}) == 1, "static list expected"
print("static trackers untouched ok")
print("all ok")
