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
    private func runPublicConstantBackendParity() async throws {
        executionTimeAllowance = 30
        XCTAssertEqual(OHRunPublicConstantComparisonV1(nil, 56), -1)
        var sentinel = OHConstantComparisonV1(); sentinel.version = 77
        let original = withUnsafeBytes(of: sentinel) { Data($0) }
        XCTAssertEqual(OHRunPublicConstantComparisonV1(&sentinel, 55), -1)
        XCTAssertEqual(withUnsafeBytes(of: sentinel) { Data($0) }, original)
        let start = DispatchTime.now().uptimeNanoseconds
        let deadline = GraphicsProbeDeadline()
        DispatchQueue.global(qos: .userInitiated).asyncAfter(deadline: .now() + 25) {
            deadline.terminateIfUnfinished()
        }
        let (status, value): (Int32, OHConstantComparisonV1) = await withCheckedContinuation { continuation in
            DispatchQueue.global(qos: .userInitiated).async {
                var value = OHConstantComparisonV1()
                let status = OHRunPublicConstantComparisonV1(&value, MemoryLayout<OHConstantComparisonV1>.size)
                continuation.resume(returning: (status, value))
            }
        }
        if DispatchTime.now().uptimeNanoseconds - start > 25_000_000_000 { _exit(124) }
        deadline.finish()
        XCTAssertEqual(status, 0)
        XCTAssertEqual(value.version, 0x10000); XCTAssertEqual(value.bytes, 56)
        XCTAssertEqual(value.backend_a, 1); XCTAssertEqual(value.backend_b, 2)
        XCTAssertEqual(value.cases, 6); XCTAssertEqual(value.samples, 72)
        XCTAssertGreaterThan(value.compared_bytes, 0)
        // No speed threshold. Simulator results cannot select a phone backend.
        XCTAssertEqual(value.flags, 7); XCTAssertEqual(value.reserved, 0)
        print("PUBLIC_CONSTANT_BACKEND_RESULT \(value.cases) \(value.samples) \(value.compared_bytes) \(value.flags)")
    }

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
        XCTAssertEqual(result.0, 0)
        XCTAssertEqual(result.1.version, 0x0001_0000)
        XCTAssertEqual(result.1.bytes, 32)
        XCTAssertEqual(result.1.backend, 5, "actual Metal backend is required")
        XCTAssertEqual(result.1.reserved, 0)
        XCTAssertEqual(result.1.checks, 255)
        XCTAssertEqual(result.1.rendered_cases, 6)
        // Keep every original presenter assertion above. This additional probe
        // runs actual authored Xenos shaders through the production translator
        // and offscreen Metal pipelines, with individual sample readback.
        XCTAssertEqual(OHFixtureAlphaCoverageProbeV1(nil, 32), -1)
        var sentinel = OHAlphaCoverageProbeV1()
        sentinel.version = 77
        let untouched = withUnsafeBytes(of: sentinel) { Data($0) }
        XCTAssertEqual(OHFixtureAlphaCoverageProbeV1(&sentinel, 31), -1)
        XCTAssertEqual(withUnsafeBytes(of: sentinel) { Data($0) }, untouched, "refusal leaves output untouched")
        let coverage: (Int32, OHAlphaCoverageProbeV1) = await withCheckedContinuation { continuation in
            DispatchQueue.global(qos: .userInitiated).async {
                var value = OHAlphaCoverageProbeV1()
                let status = OHFixtureAlphaCoverageProbeV1(&value, MemoryLayout<OHAlphaCoverageProbeV1>.size)
                continuation.resume(returning: (status, value))
            }
        }
        if DispatchTime.now().uptimeNanoseconds - started > 15_000_000_000 { _exit(124) }
        deadline.finish()
        XCTAssertEqual(coverage.0, 0)
        XCTAssertEqual(coverage.1.version, 0x0001_0000)
        XCTAssertEqual(coverage.1.bytes, 32)
        XCTAssertEqual(coverage.1.backend, 5, "actual Metal backend is required")
        XCTAssertEqual(coverage.1.reserved, 0)
        XCTAssertEqual(coverage.1.checks, 1023)
        XCTAssertEqual(coverage.1.rendered_cases, 17)
        try await runPublicConstantBackendParity()
        let report: [String: Any] = ["schema": 1, "backend": result.1.backend, "checks": result.1.checks,
            "rendered_cases": result.1.rendered_cases, "offscreen": true, "displayed": false,
            "game": false, "iphone": false]
        let bytes = try JSONSerialization.data(withJSONObject: report, options: [.sortedKeys])
        print("PUBLIC_GRAPHICS_PRESENTATION \(String(decoding: bytes, as: UTF8.self))")
    }
}
