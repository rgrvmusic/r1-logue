#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gerard Braad (Regroove | RGRV)
#
# Stage a Korg M1 ROM set under the names the unit reads:
#
#   tools/prepare-roms.sh <rom-dir> <dest-dir> [<all-data.syx>]
#
# <rom-dir> may be a directory tree; the files are found by chip name:
# IC30 (or IC20/IC26) for the coefficient ROM, IC36-IC43 for the PCM and the
# Rev 19 IC23/IC32 pair for the OS. Nothing is derived; the files are copied
# as they are.

set -e

if [ $# -lt 2 ]; then
    echo "usage: $0 <rom-dir> <dest-dir> [<all-data.syx>]" >&2
    exit 2
fi

SRC="$1"
DEST="$2"
PRELOAD="${3:-}"
mkdir -p "$DEST"

find_first() {
    find "$SRC" -type f -iname "$1" -print -quit 2>/dev/null
}

copy_as() {
    local src="$1" name="$2"
    if [ -z "$src" ] || [ ! -f "$src" ]; then
        echo "prepare-roms: missing $name in $SRC" >&2
        exit 1
    fi
    cp -f "$src" "$DEST/$name"
    printf '  %-16s %s\n' "$name" "$src"
}

COEF="$(find_first 'ic30.bin')"
[ -n "$COEF" ] || COEF="$(find_first 'ic20.bin')"
[ -n "$COEF" ] || COEF="$(find_first 'ic26.bin')"
copy_as "$COEF" m1-ic30.bin

for n in 36 37 38 39 40 41 42 43; do
    copy_as "$(find_first "ic$n.bin")" "m1-ic$n.bin"
done

OS_EVEN="$(find_first '*rev19*ic32*')"
OS_ODD="$(find_first '*rev19*ic23*')"
[ -n "$OS_EVEN" ] || OS_EVEN="$(find_first '*ic32*')"
[ -n "$OS_ODD" ] || OS_ODD="$(find_first '*ic23*')"
copy_as "$OS_EVEN" m1-ic32.bin
copy_as "$OS_ODD" m1-ic23.bin

if [ -z "$PRELOAD" ]; then
    PRELOAD="$(find_first '*.syx')"
fi
if [ -n "$PRELOAD" ] && [ -f "$PRELOAD" ]; then
    # Korg M1 ALL DATA: F0 42 3n 19 50 ...
    header="$(od -An -tx1 -N5 "$PRELOAD" 2>/dev/null | tr -d ' \n')"
    case "$header" in
    f042*1950) copy_as "$PRELOAD" m1-preload.syx ;;
    *) echo "prepare-roms: $PRELOAD is not an M1 ALL DATA dump; skipped" >&2 ;;
    esac
fi

echo "ROM set staged in $DEST"
