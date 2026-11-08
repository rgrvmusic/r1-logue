#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gerard Braad (Regroove | RGRV)
# Host check for the R1 microKORG2 oscillator: no container, no device, no
# cross compiler.
#
#   M1_ROM_DIR=<rom-dir> tools/hostcheck-microkorg2.sh
#
# test/mk2/harness.cc drives the unit through the host's oscillator context and
# compares the stereo pair with one reference engine given the same notes.
# test/mk2/stub/unit_osc.h stands in for the SDK header, whose helpers are ARM
# only. The unit's config.mk holds the -D's and the sources.
#
# Environment:
#   M1_ROM_DIR      the M1 ROM files (required).
#   LOGUE_SDK_PATH  the logue SDK checkout. Default: ~/Projects/logue/logue-sdk

set -e

cd "$(dirname "${BASH_SOURCE[0]}")/.."

LOGUE_SDK_PATH="${LOGUE_SDK_PATH:-$HOME/Projects/logue/logue-sdk}"
UNIT=m1_microkorg2
PVSC="$PWD/third_party/pvsc"
PVSC_M1="$PVSC/instruments/PicoFaceM1"
SDK_MK2="$LOGUE_SDK_PATH/platform/microkorg2/common"
SDK_COMMON="$LOGUE_SDK_PATH/platform/common"

if [ -z "$M1_ROM_DIR" ]; then
    echo "hostcheck: set M1_ROM_DIR to the directory holding the M1 ROMs" >&2
    exit 2
fi
if [ ! -f "$SDK_MK2/runtime.h" ] || [ ! -f "$SDK_COMMON/unit.h" ]; then
    echo "hostcheck: the microKORG2 SDK headers are not at $SDK_MK2" >&2
    echo "           Set LOGUE_SDK_PATH to your logue-sdk checkout." >&2
    exit 1
fi
if [ ! -f "$PVSC_M1/src/m1_engine/m1_engine.cpp" ]; then
    echo "hostcheck: the PicoFaceM1 engine is not checked out" >&2
    echo "           Run: git submodule update --init --depth 1 third_party/pvsc" >&2
    exit 1
fi

# The ROMs are staged into a scratch directory with the names the unit reads.
ROM_DIR=$(mktemp -d)
trap 'rm -rf "$ROM_DIR"' EXIT
./tools/prepare-roms.sh "$M1_ROM_DIR" "$ROM_DIR"

udefs() {
    sed -n 's/^UDEFS[[:space:]]*\(+*\)=[[:space:]]*//p' "$UNIT/config.mk" | tr '\n' ' ' |
        sed 's/-DRFX_USERFS_PROGRAMS_PATH=[^ ]*//g'
}

DEFS=($(udefs) -DRFX_USERFS_PROGRAMS_PATH="$ROM_DIR")
SRC=(common/pvsc_m1_engine.cc common/pvsc_m1_fx.cc)
# The stub comes first, so it wins over the SDK's own unit_osc.h.
INCS=(-Itest/mk2/stub -I"$UNIT" -Icommon -I"$PVSC_M1/include" -I"$PVSC_M1/src"
      -I"$SDK_MK2" -I"$SDK_COMMON")

mkdir -p build/mk2
gcc -O1 -c "$UNIT/header.c" -o build/mk2/header.o "${DEFS[@]}" "${INCS[@]}"
g++ -std=gnu++14 -O2 -Wall -Wextra -o build/mk2/hostcheck \
    test/mk2/harness.cc "$UNIT/unit.cc" build/mk2/header.o "${SRC[@]}" \
    "${INCS[@]}" "${DEFS[@]}" -lm

build/mk2/hostcheck
