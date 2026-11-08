import CoreAudio
import Foundation

public enum HALError: Error, CustomStringConvertible {
    case status(OSStatus, String)
    case notFound(String)
    case timeout(String)

    public var description: String {
        switch self {
        case let .status(code, what): return "\(what) failed (OSStatus \(code) \(fourCC(code)))"
        case let .notFound(what): return "\(what) not found"
        case let .timeout(what): return "timed out: \(what)"
        }
    }
}

func fourCC(_ code: OSStatus) -> String {
    let n = UInt32(bitPattern: code)
    let bytes = [24, 16, 8, 0].map { UInt8((n >> UInt32($0)) & 0xFF) }
    guard bytes.allSatisfy({ $0 >= 32 && $0 < 127 }) else { return "" }
    return "'" + String(decoding: bytes, as: UTF8.self) + "'"
}

@inline(__always)
func check(_ status: OSStatus, _ what: @autoclosure () -> String) throws {
    if status != noErr { throw HALError.status(status, what()) }
}

/// Snapshot of a Core Audio device.
public struct AudioDevice: Sendable, Hashable {
    public let id: AudioObjectID
    public let uid: String
    public let name: String
    public let inputChannels: Int
    public let outputChannels: Int
    public let nominalSampleRate: Double
    public let transportType: UInt32

    public var isVirtual: Bool { transportType == kAudioDeviceTransportTypeVirtual }
}

/// Thin, synchronous wrappers over the Core Audio HAL property API.
/// Not for use on real-time threads.
public enum HAL {
    public static let systemObject = AudioObjectID(kAudioObjectSystemObject)

    public static func address(
        _ selector: AudioObjectPropertySelector,
        _ scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal
    ) -> AudioObjectPropertyAddress {
        AudioObjectPropertyAddress(mSelector: selector, mScope: scope, mElement: kAudioObjectPropertyElementMain)
    }

    static func get<T>(_ object: AudioObjectID, _ address: AudioObjectPropertyAddress, initial: T) throws -> T {
        var addr = address
        var value = initial
        var size = UInt32(MemoryLayout<T>.size)
        let status = withUnsafeMutablePointer(to: &value) { AudioObjectGetPropertyData(object, &addr, 0, nil, &size, $0) }
        try check(status,
                  "get \(fourCC(OSStatus(bitPattern: address.mSelector))) on \(object)")
        return value
    }

    static func getArray<T>(_ object: AudioObjectID, _ address: AudioObjectPropertyAddress) throws -> [T] {
        var addr = address
        var size: UInt32 = 0
        try check(AudioObjectGetPropertyDataSize(object, &addr, 0, nil, &size), "size of property on \(object)")
        let count = Int(size) / MemoryLayout<T>.stride
        if count == 0 { return [] }
        let raw = UnsafeMutableRawPointer.allocate(byteCount: Int(size), alignment: MemoryLayout<T>.alignment)
        defer { raw.deallocate() }
        try check(AudioObjectGetPropertyData(object, &addr, 0, nil, &size, raw), "array property on \(object)")
        let typed = raw.bindMemory(to: T.self, capacity: count)
        return Array(UnsafeBufferPointer(start: typed, count: Int(size) / MemoryLayout<T>.stride))
    }

    static func getString(_ object: AudioObjectID, _ address: AudioObjectPropertyAddress) throws -> String {
        var addr = address
        var ref: Unmanaged<CFString>?
        var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        try check(AudioObjectGetPropertyData(object, &addr, 0, nil, &size, &ref), "string property on \(object)")
        return ref?.takeRetainedValue() as String? ?? ""
    }

    static func channelCount(_ object: AudioObjectID, scope: AudioObjectPropertyScope) -> Int {
        var addr = address(kAudioDevicePropertyStreamConfiguration, scope)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(object, &addr, 0, nil, &size) == noErr, size > 0 else { return 0 }
        let raw = UnsafeMutableRawPointer.allocate(byteCount: Int(size), alignment: MemoryLayout<AudioBufferList>.alignment)
        defer { raw.deallocate() }
        guard AudioObjectGetPropertyData(object, &addr, 0, nil, &size, raw) == noErr else { return 0 }
        let list = UnsafeMutableAudioBufferListPointer(raw.assumingMemoryBound(to: AudioBufferList.self))
        return list.reduce(0) { $0 + Int($1.mNumberChannels) }
    }

    public static func deviceIDs() throws -> [AudioObjectID] {
        try getArray(systemObject, address(kAudioHardwarePropertyDevices))
    }

    public static func device(id: AudioObjectID) throws -> AudioDevice {
        AudioDevice(
            id: id,
            uid: try getString(id, address(kAudioDevicePropertyDeviceUID)),
            name: try getString(id, address(kAudioObjectPropertyName)),
            inputChannels: channelCount(id, scope: kAudioObjectPropertyScopeInput),
            outputChannels: channelCount(id, scope: kAudioObjectPropertyScopeOutput),
            nominalSampleRate: (try? get(id, address(kAudioDevicePropertyNominalSampleRate), initial: Float64(0))) ?? 0,
            transportType: (try? get(id, address(kAudioDevicePropertyTransportType), initial: UInt32(0))) ?? 0
        )
    }

    public static func devices() throws -> [AudioDevice] {
        try deviceIDs().compactMap { try? device(id: $0) }
    }

    public static func device(uid: String) throws -> AudioDevice {
        guard let d = try devices().first(where: { $0.uid == uid }) else { throw HALError.notFound("device \(uid)") }
        return d
    }

    public static func availableSampleRates(_ id: AudioObjectID) throws -> [Double] {
        let ranges: [AudioValueRange] = try getArray(id, address(kAudioDevicePropertyAvailableNominalSampleRates))
        return ranges.map(\.mMinimum)
    }

    /// Sets the nominal sample rate and waits until the device reports it.
    public static func setNominalSampleRate(_ id: AudioObjectID, _ rate: Double, timeout: TimeInterval = 3) throws {
        var addr = address(kAudioDevicePropertyNominalSampleRate)
        var value = Float64(rate)
        try check(AudioObjectSetPropertyData(id, &addr, 0, nil, UInt32(MemoryLayout<Float64>.size), &value),
                  "set sample rate \(rate)")
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if (try? get(id, address(kAudioDevicePropertyNominalSampleRate), initial: Float64(0))) == rate { return }
            Thread.sleep(forTimeInterval: 0.05)
        }
        throw HALError.timeout("sample rate \(rate) on device \(id)")
    }
}
