import Metal
import XCTest
@testable import SkyrimProbe

final class MetalProbeTests: XCTestCase {
    @MainActor
    func testRustDecoderAndCppBridge() {
        let model = ProbeModel()
        XCTAssertTrue(model.cpuPassed, model.cpuStatus)
        XCTAssertEqual(model.pixels.count, 64)
    }

    @MainActor
    func testProductionShaderRendersEveryDecodedPixel() throws {
        let model = ProbeModel()
        XCTAssertTrue(model.cpuPassed, model.cpuStatus)
        guard model.cpuPassed else { return }

        // Missing Metal support is a failure: this test proves an actual GPU
        // rendering/readback path, rather than treating an absent GPU as a pass.
        let device = try XCTUnwrap(MTLCreateSystemDefaultDevice(), "Metal device is unavailable")
        let queue = try XCTUnwrap(device.makeCommandQueue(), "Cannot create Metal command queue")
        let library = try device.makeLibrary(source: ProbeShaderSource.msl, options: nil)
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = try XCTUnwrap(library.makeFunction(name: "probe_vertex"))
        descriptor.fragmentFunction = try XCTUnwrap(library.makeFunction(name: "probe_fragment"))
        descriptor.colorAttachments[0].pixelFormat = .bgra8Unorm
        let pipeline = try device.makeRenderPipelineState(descriptor: descriptor)

        let sourceDescriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .rgba8Unorm, width: 4, height: 4, mipmapped: false)
        sourceDescriptor.usage = .shaderRead
        sourceDescriptor.storageMode = .shared
        let source = try XCTUnwrap(device.makeTexture(descriptor: sourceDescriptor))
        model.pixels.withUnsafeBytes { bytes in
            source.replace(region: MTLRegionMake2D(0, 0, 4, 4), mipmapLevel: 0,
                           withBytes: bytes.baseAddress!, bytesPerRow: 16)
        }

        // Match MTKView's production BGRA output, but render to a CPU-readable
        // texture so completion alone cannot hide an incorrect shader or binding.
        let targetDescriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: 4, height: 4, mipmapped: false)
        targetDescriptor.usage = .renderTarget
        targetDescriptor.storageMode = .shared
        let target = try XCTUnwrap(device.makeTexture(descriptor: targetDescriptor))
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = target
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].storeAction = .store
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.05, green: 0.05, blue: 0.07, alpha: 1)

        let command = try XCTUnwrap(queue.makeCommandBuffer())
        let encoder = try XCTUnwrap(command.makeRenderCommandEncoder(descriptor: pass))
        encoder.setRenderPipelineState(pipeline)
        encoder.setFragmentTexture(source, index: 0)
        encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        encoder.endEncoding()
        command.commit()
        command.waitUntilCompleted() // Deliberate synchronization in the test only.
        guard command.status == .completed else {
            XCTFail("Metal render failed: \(command.error?.localizedDescription ?? "unknown error")")
            return
        }

        var bgra = [UInt8](repeating: 0, count: 64)
        bgra.withUnsafeMutableBytes { bytes in
            target.getBytes(bytes.baseAddress!, bytesPerRow: 16,
                            from: MTLRegionMake2D(0, 0, 4, 4), mipmapLevel: 0)
        }
        for pixel in 0..<16 {
            let offset = pixel * 4
            let actualRGBA = [bgra[offset + 2], bgra[offset + 1], bgra[offset], bgra[offset + 3]]
            let expectedRGBA = Array(model.pixels[offset..<(offset + 4)])
            XCTAssertEqual(actualRGBA, expectedRGBA, "RGBA mismatch at pixel (\(pixel % 4), \(pixel / 4))")
        }
    }
}
