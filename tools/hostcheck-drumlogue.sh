#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gerard Braad (Regroove | RGRV)
# Host check for the M1 drumlogue unit: no container, no device, no cross
# compiler.
#
#   M1_ROM_DIR=<rom-dir> tools/hostcheck-drumlogue.sh
#
# The unit's own unit.cc, header.c, the unmodified PicoFaceM1 engine and the
# -D's in config.mk are built into test/drum/harness.cc, which drives the unit
# through the callbacks the device calls with the real ROM set.
#
# Environment:
#   M1_ROM_DIR      the M1 ROM files (required). tools/prepare-roms.sh finds
#                   the chips in the tree and stages them under m1-* names.
#   LOGUE_SDK_PATH  the logue SDK checkout. Default: ~/Projects/logue/logue-sdk

set -e

cd "$(dirname "${BASH_SOURCE[0]}")/.."

LOGUE_SDK_PATH="${LOGUE_SDK_PATH:-$HOME/Projects/logue/logue-sdk}"
UNIT=m1_drumlogue
PVSC="$PWD/third_party/pvsc"
PVSC_M1="$PVSC/instruments/PicoFaceM1"
SDK_DRUMLOGUE="$LOGUE_SDK_PATH/platform/drumlogue/common"
SDK_COMMON="$LOGUE_SDK_PATH/platform/common"

if [ -z "$M1_ROM_DIR" ]; then
    echo "hostcheck: set M1_ROM_DIR to the directory holding the M1 ROMs" >&2
    exit 2
fi
if [ ! -f "$SDK_DRUMLOGUE/unit.h" ]; then
    echo "hostcheck: the drumlogue SDK headers are not at $SDK_DRUMLOGUE" >&2
    echo "           Set LOGUE_SDK_PATH to your logue-sdk checkout." >&2
    exit 1
fi
if [ ! -f "$PVSC_M1/src/m1_engine/m1_engine.cpp" ]; then
    echo "hostcheck: the PicoFaceM1 engine is not checked out" >&2
    echo "           Run: git submodule update --init --depth 1 third_party/pvsc" >&2
    exit 1
fi

# The ROMs are staged into a scratch directory with the names the unit reads;
# its m1.log lands there too, not in the ROM set.
ROM_DIR=$(mktemp -d)
trap 'rm -rf "$ROM_DIR"' EXIT
./tools/prepare-roms.sh "$M1_ROM_DIR" "$ROM_DIR"

# The unit's config.mk holds the -D's, so there is never a second copy here.
# The unit is pointed at the staged ROM directory. The two pvsc_*.cc files are
# the wrappers the SDK build uses for the engine's .cpp sources.
udefs() {
    sed -n 's/^UDEFS[[:space:]]*\(+*\)=[[:space:]]*//p' "$UNIT/config.mk" | tr '\n' ' ' |
        sed 's/-DRFX_USERFS_PROGRAMS_PATH=[^ ]*//g'
}

DEFS=($(udefs) -DRFX_USERFS_PROGRAMS_PATH="$ROM_DIR")
SRC=(common/pvsc_m1_engine.cc common/pvsc_m1_fx.cc)
INCS=(-I"$UNIT" -Icommon -I"$PVSC_M1/include" -I"$PVSC_M1/src"
      -I"$SDK_DRUMLOGUE" -I"$SDK_COMMON")

mkdir -p build
gcc -O1 -c "$UNIT/header.c" -o build/m1-drum-header.o "${DEFS[@]}" \
    -I"$UNIT" -I"$SDK_DRUMLOGUE" -I"$SDK_COMMON"
g++ -std=gnu++14 -O2 -Wall -Wextra -o build/hostcheck \
    test/drum/harness.cc "$UNIT/unit.cc" build/m1-drum-header.o "${SRC[@]}" \
    "${INCS[@]}" "${DEFS[@]}" -lm

build/hostcheck
