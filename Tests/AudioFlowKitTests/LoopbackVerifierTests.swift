import XCTest
@testable import AudioFlowKit

final class LoopbackVerifierTests: XCTestCase {
    private func feed(_ v: inout LoopbackVerifier, frames: Range<UInt64>, swap: Bool = false) {
        for n in frames {
            let l = LoopbackSignal.left(n)
            if swap { v.consume(left: -l, right: l) } else { v.consume(left: l, right: -l) }
        }
    }

    func testSignalIsExactlyRepresentable() {
        for n in UInt64(0)..<LoopbackSignal.period {
            let l = LoopbackSignal.left(n)
            XCTAssertEqual(Int((l * LoopbackSignal.scale).rounded()), Int(n))
        }
    }

    func testPerfectStreamPassesAcrossWraparound() {
        var v = LoopbackVerifier()
        for _ in 0..<300 { v.consume(left: 0, right: 0) }  // leading silence
        feed(&v, frames: 0..<50_000)
        XCTAssertTrue(v.started)
        XCTAssertEqual(v.continuityErrors, 0)
        XCTAssertEqual(v.channelErrors, 0)
        XCTAssertTrue(v.passed)
        XCTAssertGreaterThan(v.verifiedFrames, 49_000)
    }

    func testDroppedFramesAreDetected() {
        var v = LoopbackVerifier()
        feed(&v, frames: 0..<1000)
        feed(&v, frames: 1010..<2000)
        XCTAssertEqual(v.continuityErrors, 1)
        XCTAssertFalse(v.passed)
    }

    func testChannelSwapIsDetected() {
        var v = LoopbackVerifier()
        // Swapped channels put the inverted ramp on the left, which breaks continuity.
        feed(&v, frames: 1..<100, swap: true)
        XCTAssertGreaterThan(v.continuityErrors, 0)
        XCTAssertFalse(v.passed)
    }

    func testCrosstalkIsDetected() {
        var v = LoopbackVerifier()
        for n in UInt64(1)..<100 {
            let l = LoopbackSignal.left(n)
            v.consume(left: l, right: -l + 0.25)  // right channel carries something else
        }
        XCTAssertGreaterThan(v.channelErrors, 0)
        XCTAssertFalse(v.passed)
    }

    func testSilenceNeverStarts() {
        var v = LoopbackVerifier()
        for _ in 0..<1000 { v.consume(left: 0, right: 0) }
        XCTAssertFalse(v.started)
        XCTAssertFalse(v.passed)
    }

    func testDefaultConfigShape() {
        let c = AudioFlowDriver.Config.factoryDefault
        XCTAssertEqual(c.names.count, AudioFlowDriver.deviceCount)
        XCTAssertEqual(c.enabled, Array(repeating: true, count: 6))
        XCTAssertEqual(AudioFlowDriver.deviceUID(3), "AudioFlowDevice_3")
    }
}
