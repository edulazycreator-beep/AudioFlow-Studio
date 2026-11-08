import CoreAudio
import Foundation

/// Client for the AudioFlow HAL plug-in. Mirrors Driver/Source/AudioFlowDriverConfig.h.
public enum AudioFlowDriver {
    public static let bundleID = "com.audioflow.driver"
    public static let deviceCount = 6
    public static let configSelector: AudioObjectPropertySelector = 0x6166_6366  // 'afcf'
    public static let supportedSampleRates: [Double] = [44100, 48000, 88200, 96000]
    public static let defaultNames = [
        "AudioFlow Virtual 1", "AudioFlow Virtual 2", "AudioFlow Virtual 3",
        "AudioFlow Virtual 4", "AudioFlow Monitor", "AudioFlow Output",
    ]

    public static func deviceUID(_ index: Int) -> String { "AudioFlowDevice_\(index)" }

    public struct Config: Sendable, Equatable {
        public var names: [String]
        public var enabled: [Bool]

        public init(names: [String], enabled: [Bool]) {
            self.names = names
            self.enabled = enabled
        }

        public static let factoryDefault = Config(names: defaultNames, enabled: Array(repeating: true, count: deviceCount))
    }

    /// The plug-in's AudioObjectID, or throws if the driver is not loaded.
    public static func plugInID() throws -> AudioObjectID {
        var addr = HAL.address(kAudioHardwarePropertyTranslateBundleIDToPlugIn)
        var bundle = bundleID as CFString
        var id = AudioObjectID(kAudioObjectUnknown)
        var size = UInt32(MemoryLayout<AudioObjectID>.size)
        let status = withUnsafeMutablePointer(to: &bundle) { qualifier in
            AudioObjectGetPropertyData(HAL.systemObject, &addr, UInt32(MemoryLayout<CFString>.size), qualifier, &size, &id)
        }
        if status != noErr || id == kAudioObjectUnknown { throw HALError.notFound("AudioFlow driver (\(bundleID))") }
        return id
    }

    public static var isInstalled: Bool { (try? plugInID()) != nil }

    public static func readConfig() throws -> Config {
        let plugIn = try plugInID()
        var addr = HAL.address(configSelector)
        var ref: Unmanaged<CFPropertyList>?
        var size = UInt32(MemoryLayout<Unmanaged<CFPropertyList>?>.size)
        try check(AudioObjectGetPropertyData(plugIn, &addr, 0, nil, &size, &ref), "read AudioFlow config")
        guard let dict = ref?.takeRetainedValue() as? [String: Any],
              let names = dict["names"] as? [String],
              let enabled = dict["enabled"] as? [Bool]
        else { throw HALError.notFound("valid AudioFlow config") }
        return Config(names: names, enabled: enabled)
    }

    public static func writeConfig(_ config: Config) throws {
        let plugIn = try plugInID()
        var addr = HAL.address(configSelector)
        var plist: CFPropertyList = ["names": config.names, "enabled": config.enabled] as NSDictionary
        let size = UInt32(MemoryLayout<CFPropertyList>.size)
        let status = withUnsafePointer(to: &plist) { AudioObjectSetPropertyData(plugIn, &addr, 0, nil, size, $0) }
        try check(status, "write AudioFlow config")
    }

    /// AudioFlow devices currently published by the driver.
    public static func devices() throws -> [AudioDevice] {
        try HAL.devices().filter { $0.uid.hasPrefix("AudioFlowDevice_") }.sorted { $0.uid < $1.uid }
    }
}
