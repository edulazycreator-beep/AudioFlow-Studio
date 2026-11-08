// afctl — command-line control and test tool for the AudioFlow driver.

import AudioFlowKit
import CoreAudio
import Foundation

func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data("error: \(message)\n".utf8))
    exit(1)
}

func usage() -> Never {
    print("""
    usage: afctl <command> [options]

      list [--all]                     List AudioFlow devices (or all Core Audio devices)
      check                            Verify all enabled AudioFlow devices are published correctly
      config                           Print driver configuration
      rename <index> <name>            Rename device 0-5
      enable <index> | disable <index> Enable or disable device 0-5
      reset-config                     Restore default names and enable all devices
      set-rate <index> <rate>          Set device sample rate (44100, 48000, 88200, 96000)
      loopback-test [--device N] [--seconds S] [--rate R]
                                       Play a test signal into the device output and verify
                                       it arrives bit-exact on the device input
    """)
    exit(2)
}

func intArg(_ s: String?, _ what: String) -> Int {
    guard let s, let v = Int(s) else { fail("\(what) must be a number") }
    return v
}

func deviceIndex(_ s: String?) -> Int {
    let i = intArg(s, "device index")
    guard (0..<AudioFlowDriver.deviceCount).contains(i) else { fail("device index must be 0-5") }
    return i
}

func option(_ name: String, in args: [String]) -> String? {
    guard let i = args.firstIndex(of: name), i + 1 < args.count else { return nil }
    return args[i + 1]
}

func printDevice(_ d: AudioDevice) {
    let rate = String(format: "%.0f", d.nominalSampleRate)
    print("\(d.id)\t\(d.uid)\t\(d.name)\tin:\(d.inputChannels) out:\(d.outputChannels)\t\(rate) Hz")
}

// MARK: - Commands

func cmdList(_ args: [String]) {
    do {
        let devices = args.contains("--all") ? try HAL.devices() : try AudioFlowDriver.devices()
        devices.forEach(printDevice)
        if devices.isEmpty { print("(no devices)") }
    } catch { fail("\(error)") }
}

func cmdCheck() {
    do {
        _ = try AudioFlowDriver.plugInID()
        let config = try AudioFlowDriver.readConfig()
        let devices = try AudioFlowDriver.devices()
        var problems: [String] = []
        for i in 0..<AudioFlowDriver.deviceCount {
            let uid = AudioFlowDriver.deviceUID(i)
            let found = devices.first { $0.uid == uid }
            if !config.enabled[i] {
                if found != nil { problems.append("\(uid) is disabled but still published") }
                continue
            }
            guard let d = found else { problems.append("\(uid) missing"); continue }
            if d.name != config.names[i] { problems.append("\(uid) name '\(d.name)' != '\(config.names[i])'") }
            if d.inputChannels != 2 || d.outputChannels != 2 {
                problems.append("\(uid) channels in:\(d.inputChannels) out:\(d.outputChannels), expected 2/2")
            }
            if !d.isVirtual { problems.append("\(uid) transport type is not virtual") }
            let rates = try HAL.availableSampleRates(d.id)
            if rates != AudioFlowDriver.supportedSampleRates { problems.append("\(uid) rates \(rates)") }
            printDevice(d)
        }
        if problems.isEmpty {
            print("OK: \(devices.count) AudioFlow device(s) published correctly")
        } else {
            problems.forEach { print("FAIL: \($0)") }
            exit(1)
        }
    } catch { fail("\(error)") }
}

func cmdConfig() {
    do {
        let c = try AudioFlowDriver.readConfig()
        for i in c.names.indices { print("\(i)\t\(c.enabled[i] ? "enabled " : "disabled")\t\(c.names[i])") }
    } catch { fail("\(error)") }
}

func updateConfig(_ change: (inout AudioFlowDriver.Config) -> Void) {
    do {
        var c = try AudioFlowDriver.readConfig()
        change(&c)
        try AudioFlowDriver.writeConfig(c)
        Thread.sleep(forTimeInterval: 0.5)  // let the HAL process the notification
        cmdConfig()
    } catch { fail("\(error)") }
}

func cmdSetRate(_ args: [String]) {
    let i = deviceIndex(args.first)
    let rate = Double(intArg(args.dropFirst().first, "rate"))
    do {
        let d = try HAL.device(uid: AudioFlowDriver.deviceUID(i))
        try HAL.setNominalSampleRate(d.id, rate)
        printDevice(try HAL.device(id: d.id))
    } catch { fail("\(error)") }
}

/// State shared with the IO thread. Written only by the IO thread while running,
/// read by the main thread after the IOProc has stopped.
final class LoopbackContext: @unchecked Sendable {
    var outFrame: UInt64 = 0
    var verifier = LoopbackVerifier()
    var cycles = 0
    var badLayout = false
}

func cmdLoopbackTest(_ args: [String]) {
    let index = option("--device", in: args).map { deviceIndex($0) } ?? 0
    let seconds = Double(option("--seconds", in: args) ?? "5") ?? 5
    let rate = option("--rate", in: args).map { Double(intArg($0, "rate")) } ?? 48000

    let device: AudioDevice
    do {
        device = try HAL.device(uid: AudioFlowDriver.deviceUID(index))
        try HAL.setNominalSampleRate(device.id, rate)
    } catch { fail("\(error)") }

    print("Loopback test: \(device.name) at \(Int(rate)) Hz for \(seconds) s")
    let ctx = LoopbackContext()
    var procID: AudioDeviceIOProcID?

    // @Sendable keeps the block nonisolated: it runs on Core Audio's real-time thread.
    let status = AudioDeviceCreateIOProcIDWithBlock(&procID, device.id, nil) { @Sendable _, inInput, _, outOutput, _ in
        ctx.cycles += 1
        let outList = UnsafeMutableAudioBufferListPointer(outOutput)
        if let out = outList.first, let data = out.mData, out.mNumberChannels == 2 {
            let frames = Int(out.mDataByteSize) / (2 * MemoryLayout<Float>.size)
            let samples = data.assumingMemoryBound(to: Float.self)
            for f in 0..<frames {
                let l = LoopbackSignal.left(ctx.outFrame)
                samples[2 * f] = l
                samples[2 * f + 1] = -l
                ctx.outFrame += 1
            }
        } else {
            ctx.badLayout = true
        }

        let inList = UnsafeMutableAudioBufferListPointer(UnsafeMutablePointer(mutating: inInput))
        if let input = inList.first, let data = input.mData, input.mNumberChannels == 2 {
            let frames = Int(input.mDataByteSize) / (2 * MemoryLayout<Float>.size)
            let samples = data.assumingMemoryBound(to: Float.self)
            for f in 0..<frames { ctx.verifier.consume(left: samples[2 * f], right: samples[2 * f + 1]) }
        } else {
            ctx.badLayout = true
        }
    }
    guard status == noErr, let procID else { fail("AudioDeviceCreateIOProcIDWithBlock failed (\(status))") }

    let startStatus = AudioDeviceStart(device.id, procID)
    guard startStatus == noErr else { fail("AudioDeviceStart failed (\(startStatus))") }
    Thread.sleep(forTimeInterval: seconds)
    AudioDeviceStop(device.id, procID)
    AudioDeviceDestroyIOProcID(device.id, procID)
    Thread.sleep(forTimeInterval: 0.1)

    let v = ctx.verifier
    let expectedFrames = Int(rate * seconds)
    print("  IO cycles:            \(ctx.cycles)")
    print("  frames written:       \(ctx.outFrame)")
    print("  non-zero frames read: \(v.receivedNonZeroFrames)")
    print("  frames verified:      \(v.verifiedFrames)")
    print("  continuity errors:    \(v.continuityErrors)")
    print("  channel errors:       \(v.channelErrors)")

    if ctx.badLayout { fail("unexpected buffer layout (expected one interleaved stereo buffer per direction)") }
    if !v.started {
        fail("""
        no signal received on the input. If frames were written, the process probably lacks \
        microphone permission (macOS delivers silence instead of an error).
        """)
    }
    if v.verifiedFrames < expectedFrames / 2 { fail("too few frames verified (\(v.verifiedFrames) of ~\(expectedFrames))") }
    if !v.passed { fail("loopback is not bit-exact") }
    print("PASS: bit-exact loopback with correct stereo separation")
}

// MARK: - Dispatch

var args = Array(CommandLine.arguments.dropFirst())
guard let command = args.first else { usage() }
args.removeFirst()

switch command {
case "list": cmdList(args)
case "check": cmdCheck()
case "config": cmdConfig()
case "rename":
    let i = deviceIndex(args.first)
    let name = args.dropFirst().joined(separator: " ")
    guard !name.isEmpty else { fail("name required") }
    updateConfig { $0.names[i] = name }
case "enable": let i = deviceIndex(args.first); updateConfig { $0.enabled[i] = true }
case "disable": let i = deviceIndex(args.first); updateConfig { $0.enabled[i] = false }
case "reset-config": updateConfig { $0 = .factoryDefault }
case "set-rate": cmdSetRate(args)
case "loopback-test": cmdLoopbackTest(args)
default: usage()
}
