/// Deterministic test signal for loopback verification.
/// Left = ramp k/16384 for k in 0..<8192 (exactly representable in Float32), Right = -Left.
public enum LoopbackSignal {
    public static let period: UInt64 = 8192
    public static let scale: Float = 16384

    @inline(__always)
    public static func left(_ frame: UInt64) -> Float { Float(frame % period) / scale }
}

/// Checks a received stream for bit-exact continuity and channel separation.
/// Pure value type so it can be unit-tested without audio hardware.
public struct LoopbackVerifier: Sendable {
    public private(set) var started = false
    public private(set) var verifiedFrames = 0
    public private(set) var continuityErrors = 0
    public private(set) var channelErrors = 0
    public private(set) var receivedNonZeroFrames = 0
    private var previous: Float = 0

    public init() {}

    @inline(__always)
    public mutating func consume(left: Float, right: Float) {
        if left != 0 || right != 0 { receivedNonZeroFrames += 1 }
        if !started {
            guard left != 0 else { return }
            started = true
            previous = left
            if right != -left { channelErrors += 1 }
            return
        }
        let k = Int((previous * LoopbackSignal.scale).rounded())
        let expected = Float((k + 1) % Int(LoopbackSignal.period)) / LoopbackSignal.scale
        if left != expected { continuityErrors += 1 }
        if right != -left { channelErrors += 1 }
        previous = left
        verifiedFrames += 1
    }

    public var passed: Bool { started && continuityErrors == 0 && channelErrors == 0 && verifiedFrames > 0 }
}
