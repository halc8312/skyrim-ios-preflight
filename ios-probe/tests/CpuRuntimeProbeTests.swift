import XCTest
@testable import SkyrimProbe

final class CpuRuntimeProbeTests: XCTestCase {
    func testAll209ContextOffsetsMatchAcrossRustAndCpp() {
        let count = 209
        let sentinel = UInt32(0xA5A5_A5A5)
        var rust = [UInt32](repeating: sentinel, count: count + 2)
        var cpp = rust
        let rustStatus = rust.withUnsafeMutableBufferPointer {
            probe_runtime_context_layout($0.baseAddress!.advanced(by: 1), count)
        }
        let cppStatus = cpp.withUnsafeMutableBufferPointer {
            probe_cpp_context_layout($0.baseAddress!.advanced(by: 1), count)
        }
        XCTAssertEqual(rustStatus, 209)
        XCTAssertEqual(cppStatus, 209)
        XCTAssertEqual(rust.first, sentinel)
        XCTAssertEqual(rust.last, sentinel)
        XCTAssertEqual(cpp.first, sentinel)
        XCTAssertEqual(cpp.last, sentinel)
        for index in 0..<count {
            XCTAssertEqual(rust[index + 1], cpp[index + 1], "PPC context layout word \(index)")
        }
        XCTAssertEqual(rust[1], 1)
        XCTAssertEqual(rust[2], 2688)
        XCTAssertEqual(rust[3], 64)
    }

    func testRealGuestMemoryBackend() {
        var result = ProbeRuntimeResult()
        let status = probe_runtime_memory_selftest(&result, MemoryLayout<ProbeRuntimeResult>.size)
        XCTAssertEqual(status, 0)
        XCTAssertEqual(result.abi_version, 1)
        XCTAssertEqual(result.struct_size, UInt32(MemoryLayout<ProbeRuntimeResult>.size))
        XCTAssertEqual(result.failed, 0)
        XCTAssertEqual(result.passed, 0x3FF)
    }

    func testCppGuestModuleCallsRustAndSharesRealMemory() throws {
        let descriptor = try XCTUnwrap(probe_cpp_guest_descriptor())
        var result = ProbeRuntimeResult()
        let status = probe_runtime_cpu_roundtrip(descriptor, &result, MemoryLayout<ProbeRuntimeResult>.size)
        XCTAssertEqual(status, 0)
        XCTAssertEqual(result.abi_version, 1)
        XCTAssertEqual(result.failed, 0)
        XCTAssertEqual(result.passed, 0x3F_0000)
        XCTAssertEqual(result.callback_result, 0x1020_3040)
    }

    func testMalformedDescriptorRejectedBeforeInvocation() throws {
        let descriptor = try XCTUnwrap(probe_cpp_guest_descriptor())
        var invalid = descriptor.pointee
        invalid.abi_version = 0
        var result = ProbeRuntimeResult()
        result.passed = 0xA5A5_A5A5_A5A5_A5A5
        let status = probe_runtime_cpu_roundtrip(&invalid, &result, MemoryLayout<ProbeRuntimeResult>.size)
        XCTAssertEqual(status, -5)
        XCTAssertEqual(result.passed, 0xA5A5_A5A5_A5A5_A5A5)
        XCTAssertEqual(probe_runtime_cpu_roundtrip(nil, &result, MemoryLayout<ProbeRuntimeResult>.size), -1)
    }

    func testRealHardwareFPCRControlRoundtrip() {
        // Checks actual ARM64 FPCR write/readback on the execution host and
        // restores its original value before returning to XCTest.
        XCTAssertEqual(probe_cpp_fpcr_roundtrip(), 0x7F)
    }

    func testSDKSetjmpPairWithTwoLiveCallerSnapshots() {
        var result = ProbeJumpReport()
        let status = probe_cpp_jump_selftest(&result, MemoryLayout<ProbeJumpReport>.size)
        XCTAssertEqual(status, 0)
        XCTAssertEqual(result.abi_version, 1)
        XCTAssertEqual(result.struct_size, UInt32(MemoryLayout<ProbeJumpReport>.size))
        XCTAssertEqual(result.failed, 0)
        XCTAssertEqual(result.passed, 0xFF)
        XCTAssertEqual(result.capture_calls, 2)
        XCTAssertEqual(result.zero_returns, 2)
        XCTAssertEqual(result.resumed_returns, 2)
        XCTAssertEqual(result.first_return, 7)
        XCTAssertEqual(result.second_return, 11)
        XCTAssertEqual(result.buffer_bytes, 192)
        XCTAssertEqual(result.buffer_alignment, 4)
        XCTAssertEqual(result.state_alignment, 64)
    }

    func testJumpOutputNullAndShortAreRejectedWithoutWrites() {
        XCTAssertEqual(probe_cpp_jump_selftest(nil, MemoryLayout<ProbeJumpReport>.size), -1)
        var result = ProbeJumpReport()
        result.passed = 0xA5A5_A5A5_A5A5_A5A5
        XCTAssertEqual(probe_cpp_jump_selftest(&result, MemoryLayout<ProbeJumpReport>.size - 1), -2)
        XCTAssertEqual(result.passed, 0xA5A5_A5A5_A5A5_A5A5)
    }

    func testNarrowPublicRuntimeComponents() {
        // Narrow kernel-table/VFS/XEX-parser/D3D-header/XMA-value checks only.
        XCTAssertEqual(probe_runtime_component_checks(), 31)
    }

    func testRustUnwindRunsCppRAIICleanupBeforeRustCatch() {
        var counter = UInt32(0)
        // The foreign unwind is wholly inside Rust -> C++ -> Rust. This Swift
        // caller observes a normal return after Rust catches its own payload.
        XCTAssertEqual(probe_cpp_unwind_selftest(&counter), 0)
        XCTAssertEqual(counter, 1)
    }
}
