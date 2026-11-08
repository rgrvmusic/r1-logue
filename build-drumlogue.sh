#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gerard Braad (Regroove | RGRV)
# Build the M1 drumlogue unit in the logue SDK container.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

LOGUE_SDK_PATH="${LOGUE_SDK_PATH:-$HOME/Projects/logue/logue-sdk}"
PLATFORM_PATH="$LOGUE_SDK_PATH/platform"
CONTAINER_IMAGE="${CONTAINER_IMAGE:-ghcr.io/gbraad-logue/logue-sdk/dev-env:latest}"
if command -v docker >/dev/null 2>&1; then
    CONTAINER_RUNTIME="${CONTAINER_RUNTIME:-docker}"
else
    CONTAINER_RUNTIME="${CONTAINER_RUNTIME:-podman}"
fi
UNIT=m1_drumlogue
PVSC="$SCRIPT_DIR/third_party/pvsc"

if [ ! -d "$PLATFORM_PATH/drumlogue" ]; then
    echo "Error: drumlogue platform not found at $PLATFORM_PATH/drumlogue" >&2
    echo "       Set LOGUE_SDK_PATH to your logue-sdk checkout." >&2
    exit 1
fi
if [ ! -f "$PVSC/instruments/PicoFaceM1/src/m1_engine/m1_engine.cpp" ]; then
    echo "Error: the PicoFaceM1 engine is not checked out." >&2
    echo "       Run: git submodule update --init --depth 1 third_party/pvsc" >&2
    exit 1
fi

echo "========================================"
echo "Building $UNIT for drumlogue"
echo "========================================"
echo "Platform:  $PLATFORM_PATH"
echo "Engine:    $PVSC/instruments/PicoFaceM1"
echo "Container: $CONTAINER_IMAGE"
echo ""

mkdir -p bin

# The container user creates build/ and .dep/ inside the unit directory.
chmod 777 "$UNIT" 2>/dev/null || true

"$CONTAINER_RUNTIME" run --rm \
    -v "$PLATFORM_PATH:/workspace:rw" \
    -v "$SCRIPT_DIR/$UNIT:/workspace/drumlogue/$UNIT:rw" \
    -v "$SCRIPT_DIR/common:/m1common:ro" \
    -v "$PVSC:/pvsc:ro" \
    -h logue-sdk \
    "$CONTAINER_IMAGE" \
    /app/cmd_entry bash -c "
        source /app/drumlogue/environment && \
        cd /workspace/drumlogue/$UNIT && \
        make clean COMMON_PATH=/m1common PVSC=/pvsc && \
        make install COMMON_PATH=/m1common PVSC=/pvsc
    "

find "$UNIT" -maxdepth 1 -name '*.drmlgunit' -exec cp {} bin/ \;

echo ""
echo "Built: bin/R1.drmlgunit"
echo "Install it in /Units/Synths on the drumlogue, then select 'R1'."
