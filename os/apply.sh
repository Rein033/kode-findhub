#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Put the Find Hub beacon into a KodeOS checkout (git clone --recursive).
#
#   ./os/apply.sh /path/to/KodeOS
#
# Copies the beacon's own files into the SDK submodule, then applies
# os/kode-sdk.patch (in sdk/) and os/kodeos.patch (in KodeOS). Made against
# kodeOS 0.1.5 / kode-sdk v0.1.5. Run it once; `git -C KodeOS/sdk status`
# shows what it changed.
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
os="${1:?usage: apply.sh /path/to/KodeOS}"
sdk="$os/sdk"
radio="$sdk/components/kdrv/radio"

[ -f "$os/main/main.c" ] && [ -d "$radio/src" ] || {
    echo "apply.sh: $os is not a KodeOS checkout with its sdk/ submodule" >&2; exit 1; }
if grep -q kdrv_ble_fhn "$radio/src/kdrv_ble.c"; then
    echo "apply.sh: the beacon is already in $os" >&2; exit 1
fi

git -C "$sdk" apply --check "$here/os/kode-sdk.patch"
git -C "$os"  apply --check "$here/os/kodeos.patch"

# The maths, from components/kfind (one source for the tests and the OS).
mkdir -p "$radio/src/uecc"
cp "$here/components/kfind/src/kfind.c" "$here/components/kfind/src/kfind_crypto.c" "$radio/src/"
cp "$here/components/kfind/include/kfind.h" "$radio/include/"
cp "$here/components/kfind/third_party/micro-ecc/"* "$radio/src/uecc/"
# The scheduler and the verbs.
cp "$here/os/sdk/kdrv_ble_fhn.h" "$radio/include/"
cp "$here/os/sdk/kdrv_ble_fhn.inc" "$radio/src/"
cp "$here/os/sdk/findhub_verbs.c" "$sdk/components/kaos/radio/src/"

git -C "$sdk" apply "$here/os/kode-sdk.patch"
git -C "$os"  apply "$here/os/kodeos.patch"
echo "apply.sh: done. Build with: cd $os && idf.py build"
