#!/bin/bash
# Installs AudioFlowDriver.driver into the system HAL plug-in folder and
# restarts coreaudiod. Requires administrator rights.
# NOTE: restarting coreaudiod briefly interrupts all audio on the Mac.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${1:-$ROOT/build/AudioFlowDriver.driver}"
DEST="/Library/Audio/Plug-Ins/HAL/AudioFlowDriver.driver"

if [ ! -d "$SRC" ]; then
  echo "error: $SRC not found. Run scripts/build-driver.sh first." >&2
  exit 1
fi

sudo mkdir -p /Library/Audio/Plug-Ins/HAL
sudo rm -rf "$DEST"
sudo cp -R "$SRC" "$DEST"
sudo chown -R root:wheel "$DEST"
sudo chmod -R 755 "$DEST"
sudo xattr -dr com.apple.quarantine "$DEST" 2>/dev/null || true

echo "Restarting coreaudiod..."
sudo killall coreaudiod 2>/dev/null || sudo launchctl kickstart -k system/com.apple.audio.coreaudiod
sleep 3
echo "Installed $DEST"
