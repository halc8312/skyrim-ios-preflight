import Foundation
import XCTest
@testable import OwnedGameHost

final class NativeTraceSDKProbeTests: XCTestCase {
    func testDarwinUnwindAcrossPublicCppFrames() async throws {
        executionTimeAllowance = 20
        let watchdog = DispatchSource.makeTimerSource(queue: .global(qos: .userInitiated))
        watchdog.schedule(deadline: .now() + 15)
        watchdog.setEventHandler { fatalError("Public unwind probe exceeded 15 seconds") }
        watchdog.resume()
        defer { watchdog.cancel() }
        XCTAssertEqual(Bundle.main.bundleIdentifier, "com.halc8312.ownedgamehost")
        XCTAssertEqual(Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String, "0.1.0")
        XCTAssertEqual(Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String, "18")
        XCTAssertEqual(Bundle.main.object(forInfoDictionaryKey: "LSSupportsGameMode") as? Bool, true)
        XCTAssertEqual(MemoryLayout<OHNativeTraceSDKResult>.size, 32)
        let observed: (Int32, OHNativeTraceSDKResult) = await withCheckedContinuation { continuation in
            DispatchQueue.global(qos: .userInitiated).async {
                var result = OHNativeTraceSDKResult()
                let status = OHTestNativeTraceSDKV1(&result, MemoryLayout<OHNativeTraceSDKResult>.size)
                continuation.resume(returning: (status, result))
            }
        }
        let (status, result) = observed
        XCTAssertEqual(status, 0)
        XCTAssertEqual(result.abi_version, 1)
        XCTAssertEqual(result.struct_size, 32)
        XCTAssertTrue((3...32).contains(result.captured_count))
        XCTAssertEqual(result.expected_parent_count, 2)
        XCTAssertEqual(result.matched_parent_count, 2)
        XCTAssertEqual(result.rx_parent_count, 2)
        XCTAssertEqual(result.flags, 15)
        XCTAssertEqual(result.reserved, 0)
        guard status == 0, result.abi_version == 1, result.struct_size == 32,
              (3...32).contains(result.captured_count), result.expected_parent_count == 2,
              result.matched_parent_count == 2, result.rx_parent_count == 2,
              result.flags == 15, result.reserved == 0 else { return }
        // No raw PC, image path or symbol is returned from C++ or printed here.
        let report: [String: Any] = [
            "abi_version": result.abi_version, "struct_size": result.struct_size,
            "captured_count": result.captured_count,
            "expected_parent_count": result.expected_parent_count,
            "matched_parent_count": result.matched_parent_count,
            "rx_parent_count": result.rx_parent_count, "flags": result.flags,
            "reserved": result.reserved, "passed": true, "public_cpp_only": true,
            "private_guest_fatal_path_verified": false,
        ]
        let data = try JSONSerialization.data(withJSONObject: report, options: [.sortedKeys])
        print("OWNED_HOST_NATIVE_TRACE_RESULT " + String(decoding: data, as: UTF8.self))
    }
}
