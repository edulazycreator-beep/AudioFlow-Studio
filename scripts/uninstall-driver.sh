#!/bin/bash
# Removes AudioFlowDriver.driver and restarts coreaudiod.
set -euo pipefail

DEST="/Library/Audio/Plug-Ins/HAL/AudioFlowDriver.driver"
if [ -d "$DEST" ]; then
  sudo rm -rf "$DEST"
  sudo killall coreaudiod 2>/dev/null || sudo launchctl kickstart -k system/com.apple.audio.coreaudiod
  echo "Removed $DEST"
else
  echo "AudioFlowDriver is not installed."
fi
