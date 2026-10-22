import Metal
import QuartzCore
import SwiftUI
import UIKit

@MainActor
final class OwnedMetalView: UIView {
    override class var layerClass: AnyClass { CAMetalLayer.self }

    init(onLayer: @escaping (CAMetalLayer) -> Void) {
        super.init(frame: .zero)
        isOpaque = true
        backgroundColor = .black
        contentScaleFactor = 1
        let surface = layer as! CAMetalLayer
        // Configure only before the retained layer reaches the renderer. The
        // renderer exclusively changes device/drawableSize after attachment.
        surface.device = MTLCreateSystemDefaultDevice()
        surface.pixelFormat = .bgra8Unorm
        surface.framebufferOnly = true
        surface.isOpaque = true
        surface.contentsScale = 1
        surface.drawableSize = CGSize(width: 1280, height: 720)
        surface.maximumDrawableCount = 3
        surface.allowsNextDrawableTimeout = true
        onLayer(surface)
    }

    required init?(coder: NSCoder) { fatalError("Programmatic viewport only") }
}

@MainActor
struct MetalViewport: UIViewRepresentable {
    let onLayer: (CAMetalLayer) -> Void
    func makeUIView(context: Context) -> OwnedMetalView {
        OwnedMetalView(onLayer: onLayer)
    }
    func updateUIView(_ view: OwnedMetalView, context: Context) {}
    // The process-global model retains the exact CAMetalLayer even if SwiftUI
    // removes this view, or renderer initialization fails. It is never rebound.
    static func dismantleUIView(_ view: OwnedMetalView, coordinator: ()) {}
}
