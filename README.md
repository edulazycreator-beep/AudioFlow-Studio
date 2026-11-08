# AudioFlow Studio

Professional virtual audio routing and mixing for macOS (13 Ventura or newer, Apple Silicon and Intel).

> **Status: Phase 1 — virtual audio driver.** The mixer, routing matrix and SwiftUI app arrive in later phases. See the roadmap below.

## Phase 1 contents

| Path | What it is |
|---|---|
| `Driver/` | C++ Core Audio HAL plug-in (`AudioFlowDriver.driver`). Publishes six virtual loopback devices: AudioFlow Virtual 1–4, AudioFlow Monitor, AudioFlow Output. Audio played into a device's output comes out of its input. |
| `Sources/AudioFlowKit/` | Swift 6 library: HAL device enumeration and the driver configuration client. Shared with the future app. |
| `Sources/afctl/` | Command-line tool to inspect, configure and test the driver. |
| `Tests/` | Unit tests (`swift test`). |
| `scripts/` | Build, install and uninstall scripts. |
| `.github/workflows/` | CI: builds Universal 2 binaries on a macOS runner, installs the driver, and runs device, sample-rate, persistence and bit-exact loopback tests. |

### Phase 1 features

- Six genuine Core Audio devices, visible in System Settings › Sound and selectable in OBS, QuickTime, Zoom, browsers, DAWs.
- Stereo, 32-bit float, 44.1 / 48 / 88.2 / 96 kHz.
- Buffer size is set per client app (handled by the macOS HAL).
- Rename, enable and disable devices; settings persist across restarts.
- Stable device UIDs (`AudioFlowDevice_0`…`_5`) so apps remember their selection after a rename.

Not yet in Phase 1: mono / 4 / 8 / 16-channel configurations (planned as a later driver revision), the app UI, routing and mixing.

## Try it on your Mac

1. Download the latest **AudioFlow-Phase1** artifact from the repository's *Actions* tab and unzip it.
2. Install the driver (asks for your password; audio briefly stops while macOS's audio service restarts):
   ```bash
   unzip AudioFlowDriver.driver.zip
   bash install-driver.sh "$PWD/AudioFlowDriver.driver"
   ```
3. Check it:
   ```bash
   chmod +x afctl
   xattr -d com.apple.quarantine afctl 2>/dev/null
   ./afctl check
   ./afctl loopback-test --device 0
   ```
   The loopback test reads from a virtual input, so macOS will ask your terminal app for microphone access the first time.
4. Remove it any time with `bash uninstall-driver.sh`.

CI builds are ad-hoc signed. They install fine by hand as above. Developer ID signing and notarization come with the DMG in Phase 8.

## afctl commands

```
afctl list [--all]              List AudioFlow devices (or all devices)
afctl check                     Verify the driver and its devices
afctl config                    Show names and enabled state
afctl rename <0-5> <name>       Rename a device
afctl enable|disable <0-5>      Show or hide a device
afctl reset-config              Restore defaults
afctl set-rate <0-5> <rate>     44100 | 48000 | 88200 | 96000
afctl loopback-test [--device N] [--seconds S] [--rate R]
```

## Build locally

Requires Xcode 16 or newer.

```bash
bash scripts/build-driver.sh
swift test
swift build -c release --arch arm64 --arch x86_64
bash scripts/install-driver.sh
```

## Roadmap

1. **Virtual device** ← current
2. Audio engine and input/output routing
3. Mixer and DSP effects
4. Routing matrix
5. SwiftUI interface
6. Application capture and monitoring
7. Testing and optimization
8. Signing, notarization and DMG
