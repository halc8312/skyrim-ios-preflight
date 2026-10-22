import MetalKit
import SwiftUI

struct TextureView: UIViewRepresentable {
    @ObservedObject var model: ProbeModel

    final class Coordinator {
        var renderer: TextureRenderer?
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeUIView(context: Context) -> MTKView {
        let view = MTKView(frame: .zero, device: MTLCreateSystemDefaultDevice())
        view.isPaused = true
        view.enableSetNeedsDisplay = true
        view.colorPixelFormat = .bgra8Unorm
        view.clearColor = MTLClearColor(red: 0.05, green: 0.05, blue: 0.07, alpha: 1)
        do {
            let renderer = try TextureRenderer(view: view, pixels: model.pixels) { [weak model] success, message in
                DispatchQueue.main.async {
                    model?.metalPassed = success
                    model?.metalStatus = message
                }
            }
            context.coordinator.renderer = renderer
            view.delegate = renderer
            view.setNeedsDisplay()
        } catch {
            DispatchQueue.main.async {
                model.metalStatus = "Metal：\(error.localizedDescription)"
            }
        }
        return view
    }

    func updateUIView(_ view: MTKView, context: Context) {}
}

private enum RenderError: LocalizedError {
    case message(String)
    var errorDescription: String? {
        switch self { case .message(let value): return value }
    }
}

final class TextureRenderer: NSObject, MTKViewDelegate {
    private let queue: MTLCommandQueue
    private let pipeline: MTLRenderPipelineState
    private let texture: MTLTexture
    private let report: (Bool, String) -> Void

    init(view: MTKView, pixels: [UInt8], report: @escaping (Bool, String) -> Void) throws {
        guard pixels.count == 64, let device = view.device,
              let queue = device.makeCommandQueue(),
              let library = device.makeDefaultLibrary(),
              let vertex = library.makeFunction(name: "probe_vertex"),
              let fragment = library.makeFunction(name: "probe_fragment") else {
            throw RenderError.message("GPUまたはシェーダーを準備できません")
        }
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = vertex
        descriptor.fragmentFunction = fragment
        descriptor.colorAttachments[0].pixelFormat = view.colorPixelFormat
        self.pipeline = try device.makeRenderPipelineState(descriptor: descriptor)
        let textureDescriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .rgba8Unorm, width: 4, height: 4, mipmapped: false)
        textureDescriptor.usage = .shaderRead
        textureDescriptor.storageMode = .shared
        guard let texture = device.makeTexture(descriptor: textureDescriptor) else {
            throw RenderError.message("テクスチャを確保できません")
        }
        pixels.withUnsafeBytes { bytes in
            texture.replace(region: MTLRegionMake2D(0, 0, 4, 4), mipmapLevel: 0,
                            withBytes: bytes.baseAddress!, bytesPerRow: 16)
        }
        self.texture = texture
        self.queue = queue
        self.report = report
        super.init()
    }

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {
        view.setNeedsDisplay()
    }

    func draw(in view: MTKView) {
        guard let drawable = view.currentDrawable,
              let pass = view.currentRenderPassDescriptor,
              let command = queue.makeCommandBuffer(),
              let encoder = command.makeRenderCommandEncoder(descriptor: pass) else {
            // A view without a drawable may be between layouts; the resize redraw retries.
            return
        }
        encoder.setRenderPipelineState(pipeline)
        encoder.setFragmentTexture(texture, index: 0)
        encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        encoder.endEncoding()
        command.present(drawable)
        let completion = report
        command.addCompletedHandler { buffer in
            if buffer.status == .completed {
                completion(true, "Metal：GPUの描画処理が完了")
            } else {
                completion(false, "Metal：描画エラー \(buffer.error?.localizedDescription ?? "不明")")
            }
        }
        command.commit()
    }
}
