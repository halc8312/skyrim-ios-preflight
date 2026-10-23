import Darwin
import Foundation
import XCTest

private final class GraphicsProbeDeadline: @unchecked Sendable {
    private let lock = NSLock()
    private var completed = false
    func finish() { lock.lock(); completed = true; lock.unlock() }
    func terminateIfUnfinished() {
        lock.lock()
        let expired = !completed
        lock.unlock()
        if expired { _exit(124) }
    }
}

final class GraphicsPresentationTests: XCTestCase {
    func testPublicOffscreenMetalPresentation() async throws {
        executionTimeAllowance = 20
        let started = DispatchTime.now().uptimeNanoseconds
        let deadline = GraphicsProbeDeadline()
        DispatchQueue.global(qos: .userInitiated).asyncAfter(deadline: .now() + 15) {
            deadline.terminateIfUnfinished()
        }
        let result: (Int32, OHGraphicsPresentationProbeV1) = await withCheckedContinuation { continuation in
            DispatchQueue.global(qos: .userInitiated).async {
                var value = OHGraphicsPresentationProbeV1()
                let status = OHRunPublicGraphicsProbeV1(&value, MemoryLayout<OHGraphicsPresentationProbeV1>.size)
                continuation.resume(returning: (status, value))
            }
        }
        // A delayed timer callback must not turn an over-budget native call
        // into a pass. The outer SDK runner remains a further process ceiling.
        if DispatchTime.now().uptimeNanoseconds - started > 15_000_000_000 { _exit(124) }
        deadline.finish()
        XCTAssertEqual(result.0, 0)
        XCTAssertEqual(result.1.version, 0x0001_0000)
        XCTAssertEqual(result.1.bytes, 32)
        XCTAssertEqual(result.1.backend, 5, "actual Metal backend is required")
        XCTAssertEqual(result.1.reserved, 0)
        XCTAssertEqual(result.1.checks, 255)
        XCTAssertEqual(result.1.rendered_cases, 6)
        let report: [String: Any] = ["schema": 1, "backend": result.1.backend, "checks": result.1.checks,
            "rendered_cases": result.1.rendered_cases, "offscreen": true, "displayed": false,
            "game": false, "iphone": false]
        let bytes = try JSONSerialization.data(withJSONObject: report, options: [.sortedKeys])
        print("PUBLIC_GRAPHICS_PRESENTATION \(String(decoding: bytes, as: UTF8.self))")
    }
}
