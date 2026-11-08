#!/bin/bash
# Builds AudioFlowDriver.driver as a Universal 2 (arm64 + x86_64) bundle.
# Usage: scripts/build-driver.sh [version] [build-number]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="${1:-0.1.0}"
BUILD="${2:-1}"
OUT="$ROOT/build"
BUNDLE="$OUT/AudioFlowDriver.driver"

rm -rf "$BUNDLE"
mkdir -p "$BUNDLE/Contents/MacOS" "$BUNDLE/Contents/Resources"

clang++ \
  -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Werror=return-type \
  -arch arm64 -arch x86_64 \
  -mmacosx-version-min=13.0 \
  -fvisibility=hidden \
  -bundle \
  -framework CoreAudio -framework CoreFoundation \
  -o "$BUNDLE/Contents/MacOS/AudioFlowDriver" \
  "$ROOT/Driver/Source/AudioFlowDriver.cpp"

# The factory function must stay exported despite -fvisibility=hidden.
if ! nm -gU "$BUNDLE/Contents/MacOS/AudioFlowDriver" | grep -q "_AudioFlowDriver_Create"; then
  echo "error: AudioFlowDriver_Create is not exported" >&2
  exit 1
fi

sed -e "s/__VERSION__/$VERSION/" -e "s/__BUILD__/$BUILD/" \
  "$ROOT/Driver/Info.plist" > "$BUNDLE/Contents/Info.plist"
plutil -lint "$BUNDLE/Contents/Info.plist"

# Ad-hoc signature unless a Developer ID identity is provided.
IDENTITY="${CODESIGN_IDENTITY:--}"
codesign --force --timestamp=none --options runtime --sign "$IDENTITY" "$BUNDLE"
codesign --verify --strict --verbose=2 "$BUNDLE"

lipo -info "$BUNDLE/Contents/MacOS/AudioFlowDriver"
echo "Built $BUNDLE"
