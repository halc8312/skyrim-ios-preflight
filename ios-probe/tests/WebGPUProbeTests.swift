import Metal
import QuartzCore
import UIKit
import XCTest
@testable import SkyrimProbe

private final class WebGPULayerView: UIView {
    override class var layerClass: AnyClass { CAMetalLayer.self }
    var metalLayer: CAMetalLayer { layer as! CAMetalLayer }
}

final class WebGPUProbeTests: XCTestCase {
    // Async polling has a 5-second budget in C++. Synchronous driver operations
    // and device destruction cannot be interrupted there. The CI's xcodebuild
    // test timeout remains the final process-level bound if a driver hangs.
    @MainActor
    func testWebGPUMetalSurfacePresentAndEveryReadbackPixel() async throws {
        executionTimeAllowance = 30
        var report = ProbeWebGPUReport()
        let capacity = MemoryLayout<ProbeWebGPUReport>.size
        XCTAssertEqual(capacity, 136)
        XCTAssertEqual(MemoryLayout<ProbeWebGPUReport>.alignment, 8)
        report.passed = 0xA5_A5A5
        XCTAssertEqual(probe_webgpu_metal(nil, 64, 64, nil, capacity), -1)
        XCTAssertEqual(probe_webgpu_metal(nil, 64, 64, &report, capacity - 1), -2)
        XCTAssertEqual(report.passed, 0xA5_A5A5)

        // Unsupported Metal or a missing visible host scene is a failed gate,
        // never a skip. This fixture does not run guest/Xenos/game instructions.
        let device = try XCTUnwrap(MTLCreateSystemDefaultDevice(), "Metal device is unavailable")
        let scene = try XCTUnwrap(
            UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
                .first { $0.activationState == .foregroundActive },
            "No active host UIWindowScene for CAMetalLayer")
        let priorKeyWindow = scene.windows.first { $0.isKeyWindow }
        let window = UIWindow(windowScene: scene)
        window.frame = CGRect(x: 0, y: 0, width: 64, height: 64)
        let controller = UIViewController()
        let view = WebGPULayerView(frame: window.bounds)
        controller.view = view
        window.rootViewController = controller
        let layer = view.metalLayer
        layer.device = device
        layer.pixelFormat = .bgra8Unorm
        layer.framebufferOnly = true
        view.contentScaleFactor = 1
        layer.contentsScale = 1
        layer.drawableSize = CGSize(width: 64, height: 64)
        window.makeKeyAndVisible()
        defer {
            window.isHidden = true
            window.rootViewController = nil
            priorKeyWindow?.makeKey()
        }
        view.layoutIfNeeded()
        CATransaction.flush()
        // Let UIKit attach and display the layer before entering synchronous
        // WebGPU processing on MainActor. These strong owners survive the call.
        await Task.yield()
        try await Task.sleep(nanoseconds: 100_000_000)
        XCTAssertTrue(view.window === window)
        XCTAssertFalse(window.isHidden)
        XCTAssertEqual(layer.drawableSize, CGSize(width: 64, height: 64))
        let status = withExtendedLifetime((window, controller, view, layer)) {
            probe_webgpu_metal(Unmanaged.passUnretained(layer).toOpaque(), 64, 64,
                               &report, capacity)
        }
        let details = "status=\(status), passed=\(report.passed), failed=\(report.failed), "
            + "adapter=\(report.adapter_status), device=\(report.device_status), "
            + "surface=\(report.surface_status), map=\(report.map_status), "
            + "errors=\(report.gpu_error_count), lost=\(report.device_lost_count), "
            + "elapsed_ms=\(report.elapsed_ms)"
        let attachment = XCTAttachment(string: details)
        attachment.name = "Public WebGPU Metal probe"
        attachment.lifetime = .keepAlways
        add(attachment)
        XCTAssertEqual(status, 0, details)
        XCTAssertEqual(report.abi_version, 1, details)
        XCTAssertEqual(report.struct_size, UInt32(capacity), details)
        XCTAssertEqual(report.native_version, 0x1D00_0101, details)
        XCTAssertEqual(report.passed, 0x7FF, details)
        XCTAssertEqual(report.failed, 0, details)
        XCTAssertEqual(report.gpu_error_count, 0, details)
        XCTAssertEqual(report.device_lost_count, 0, details)
        XCTAssertLessThanOrEqual(report.elapsed_ms, 5_000, details)
        XCTAssertEqual(report.pixel_count, 16, details)
        let pixels = withUnsafeBytes(of: &report.rgba) { Array($0) }
        XCTAssertEqual(pixels.count, 64)
        for pixel in 0..<16 {
            XCTAssertEqual(Array(pixels[(pixel * 4)..<(pixel * 4 + 4)]),
                           [UInt8(255), 0, 0, 255], "RGBA pixel \(pixel); \(details)")
        }
    }
}
