import XCTest
@testable import SkyrimProbe

final class OwnedAbiProbeTests: XCTestCase {
    func testGenericAdapterWithNativeSDKProvidersAndRustUnwind() {
        var result = OwnedAbiReport()
        XCTAssertEqual(probe_owned_abi_selftest(&result, MemoryLayout<OwnedAbiReport>.size), 0)
        XCTAssertEqual(result.abi_version, 1)
        XCTAssertEqual(result.struct_size, UInt32(MemoryLayout<OwnedAbiReport>.size))
        XCTAssertEqual(result.failed, 0)
        XCTAssertEqual(result.passed, 0xFFFF)
        XCTAssertEqual(result.rejected_bindings, 7)
        XCTAssertEqual(result.fatal_callbacks, 0)
        XCTAssertEqual(result.unwind_cleanups, 1)
    }

    func testNullAndShortOutputDoNotBindOrWrite() {
        XCTAssertEqual(probe_owned_abi_selftest(nil, MemoryLayout<OwnedAbiReport>.size), -1)
        var result = OwnedAbiReport()
        result.passed = 0xA5A5_A5A5_A5A5_A5A5
        XCTAssertEqual(probe_owned_abi_selftest(&result, MemoryLayout<OwnedAbiReport>.size - 1), -2)
        XCTAssertEqual(result.passed, 0xA5A5_A5A5_A5A5_A5A5)
    }
}
