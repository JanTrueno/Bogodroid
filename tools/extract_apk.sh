#!/usr/bin/env bash
#
# Build a Bogodroid gamefiles/ tree out of an installed Android app.
#
# Modern apps ship as an App Bundle: a base.apk plus per-ABI, per-density and
# per-language splits. The base.apk holds assets/ but NOT lib/ -- the native
# libraries live in split_config.arm64_v8a.apk. Extractors that grab only the
# base are why lib/arm64-v8a/ comes out empty.
#
# This takes any mix of .apk / .apks / .xapk / .zip and pulls assets/ and
# lib/arm64-v8a/ out of whichever pieces actually contain them.
#
# Usage:
#   tools/extract_apk.sh <output-dir> <file> [file...]
#
# Getting the pieces off a device (adb, or a root shell on the device):
#   pm path com.Level5.LT1R          # lists base.apk and every split
#   adb pull <each path>             # if going through adb
#
# Example:
#   tools/extract_apk.sh gamefiles/layton ~/apks/*.apk

set -euo pipefail

ABI="${ABI:-arm64-v8a}"

if [ "$#" -lt 2 ]; then
    echo "usage: $0 <output-dir> <apk|apks|xapk> [more...]" >&2
    exit 2
fi

out=$1
shift

command -v unzip >/dev/null || { echo "error: unzip is not installed" >&2; exit 1; }

mkdir -p "$out"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# .apks / .xapk are zips of apks -- unpack a level so the inner apks are seen
inputs=()
for f in "$@"; do
    [ -f "$f" ] || { echo "skipping $f (not a file)" >&2; continue; }
    case "$f" in
        *.apks|*.xapk|*.zip)
            echo "unpacking container $(basename "$f")"
            d="$tmp/$(basename "$f").d"
            mkdir -p "$d"
            unzip -q -o "$f" -d "$d"
            while IFS= read -r inner; do inputs+=("$inner"); done \
                < <(find "$d" -name '*.apk' | sort)
            ;;
        *) inputs+=("$f") ;;
    esac
done

[ "${#inputs[@]}" -gt 0 ] || { echo "error: no apks found" >&2; exit 1; }

found_assets=0
found_lib=0

for apk in "${inputs[@]}"; do
    name=$(basename "$apk")
    got=""

    # List once into a file and grep the file. Deliberately not
    # "unzip -l ... | grep -q": grep -q exits at the first match, which
    # SIGPIPEs unzip part-way through a long listing, and under `set -o
    # pipefail` that makes the whole pipeline report failure. Small apks
    # finish writing before grep quits and look fine, while a big asset
    # split silently reports "nothing" -- which is exactly the bug this
    # script exists to avoid.
    unzip -Z1 "$apk" > "$tmp/listing" 2>/dev/null || true

    if grep -q '^assets/' "$tmp/listing"; then
        unzip -q -o "$apk" 'assets/*' -d "$out"
        found_assets=1
        got="assets"
    fi

    if grep -q "^lib/$ABI/" "$tmp/listing"; then
        unzip -q -o "$apk" "lib/$ABI/*" -d "$out"
        found_lib=1
        got="${got:+$got + }lib/$ABI"
    fi

    echo "  ${name}: ${got:-nothing needed}"
done

echo
if [ "$found_assets" -eq 0 ]; then
    echo "WARNING: no assets/ found -- is the base.apk included?" >&2
fi
if [ "$found_lib" -eq 0 ]; then
    echo "WARNING: no lib/$ABI/ found." >&2
    echo "  The native libraries are in the ABI split, not the base apk." >&2
    echo "  Include split_config.${ABI//-/_}.apk (run 'pm path <package>' to list them)." >&2
    echo "  Set ABI=... to target a different architecture." >&2
fi

echo "result in $out:"
du -sh "$out" 2>/dev/null || true
find "$out" -maxdepth 2 -mindepth 1 -type d | sort | sed 's/^/  /'
