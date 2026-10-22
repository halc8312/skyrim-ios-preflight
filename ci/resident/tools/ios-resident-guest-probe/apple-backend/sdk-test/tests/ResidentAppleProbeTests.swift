import Foundation
import XCTest
@testable import ResidentAppleSDKProbe

private final class OutcomeBox {
    private let lock = NSLock()
    private var outcome: (Int32, RAAppleResultV1)?

    func publish(_ value: (Int32, RAAppleResultV1)) {
        lock.lock()
        defer { lock.unlock() }
        precondition(outcome == nil, "A one-shot probe cannot publish twice")
        outcome = value
    }

    func read() -> (Int32, RAAppleResultV1)? {
        lock.lock()
        defer { lock.unlock() }
        return outcome
    }
}

final class ResidentAppleProbeTests: XCTestCase {
    func testResidentRustKernelWithPublicFixture() throws {
        // XCTest's allowance is an additional outer gate. The explicit waiter
        // also bounds a synchronous dlopen/SDK hang before Rust's 3-second wait.
        executionTimeAllowance = 20
        XCTAssertEqual(Bundle.main.bundleIdentifier, "com.halc8312.residentapplepublicprobe")
        XCTAssertEqual(MemoryLayout<RAAppleResultV1>.size, 64)
        XCTAssertEqual(MemoryLayout<RAAppleResultV1>.alignment, 8)
        guard Bundle.main.bundleIdentifier == "com.halc8312.residentapplepublicprobe",
              MemoryLayout<RAAppleResultV1>.size == 64,
              MemoryLayout<RAAppleResultV1>.alignment == 8 else { return }

        // CI obtains this independent full SHA AFTER framework signing and
        // BEFORE app resource packaging. Never calculate it from the runtime
        // candidate or accept a hash self-reported by that candidate.
        let resource = try XCTUnwrap(Bundle.main.url(
            forResource: "ResidentAppleFixture.signed-sha256", withExtension: "txt"))
        let raw = try Data(contentsOf: resource)
        guard raw.count == 65, raw.last == 10,
              raw.prefix(64).allSatisfy({ (48...57).contains($0) || (97...102).contains($0) }) else {
            XCTFail("Independent post-signing SHA resource has an invalid shape")
            return
        }
        let hash = Array(raw.prefix(64))
        let done = expectation(description: "one-shot public resident Rust call completed")
        let box = OutcomeBox()
        DispatchQueue.global(qos: .userInitiated).async {
            var result = RAAppleResultV1()
            let status = hash.withUnsafeBufferPointer { input in
                RAAppleRunPublicProbeV1(input.baseAddress, input.count, &result,
                                       MemoryLayout<RAAppleResultV1>.size)
            }
            box.publish((status, result))
            done.fulfill()
        }
        // A timed-out Rust call must not continue after an ordinary XCTest
        // failure or be retried against an already-initialized global Kernel.
        guard XCTWaiter.wait(for: [done], timeout: 15) == .completed else {
            XCTFail("Public resident call exceeded its 15-second deadline")
            fatalError("Terminating isolated public probe after deadline")
        }
        let (status, result) = try XCTUnwrap(box.read())
        XCTAssertEqual(status, 0)
        XCTAssertEqual(result.version, 0x0001_0000)
        XCTAssertEqual(result.bytes, 64)
        XCTAssertEqual(result.passed, 1)
        XCTAssertEqual(result.failure, 0)
        // RA_CHECK_ALL: all fourteen outcome checks, not permission flags.
        XCTAssertEqual(result.checked_flags, 0x3fff)
        XCTAssertEqual(result.checked_flags & (1 << 6), 1 << 6,
                       "Native jump/unwind cookie requirements and live bind must pass")
        XCTAssertEqual(result.checked_flags & (1 << 7), 1 << 7,
                       "Native jump must return in its same live C++ caller")
        XCTAssertEqual(result.frequency, 50_000_000)
        XCTAssertEqual(result.cleanup_count, 1)
        XCTAssertEqual(result.dispatched_imports, 2)
        XCTAssertEqual(result.exit_code, 0)
        XCTAssertEqual(result.function_count, 3)
        // Never publish a success record after a failed assertion above.
        guard status == 0, result.version == 0x0001_0000, result.bytes == 64,
              result.passed == 1, result.failure == 0, result.checked_flags == 0x3fff,
              result.frequency == 50_000_000, result.cleanup_count == 1,
              result.dispatched_imports == 2, result.exit_code == 0,
              result.function_count == 3 else { return }
        let report: [String: Any] = [
            "kind": "public_apple_resident_xctest", "passed": true,
            "checked_flags": result.checked_flags, "frequency": result.frequency,
            "cleanup_count": result.cleanup_count, "dispatched_imports": result.dispatched_imports,
            "exit_code": result.exit_code, "function_count": result.function_count,
            "independent_post_sign_sha256": String(decoding: hash, as: UTF8.self),
            "inner_call_deadline_seconds": 15, "xctest_allowance_seconds": 20,
            "fresh_dedicated_app_process": true, "physical_iphone_verified": false,
            "game_executed": false, "ready_to_play": false, "compatibility_verified": false,
            "mapped_inode_attested": false, "signature_authority_authenticated": false,
            "private_guest_framework_verified": false, "loader_integration_allowed": false,
        ]
        let data = try JSONSerialization.data(withJSONObject: report, options: [.sortedKeys])
        print("RESIDENT_APPLE_PUBLIC_RESULT " + String(decoding: data, as: UTF8.self))
    }
}
