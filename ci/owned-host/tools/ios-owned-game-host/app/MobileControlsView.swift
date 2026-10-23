import QuartzCore
import SwiftUI
import UIKit

/// One UIKit surface owns all fingers; separate SwiftUI gestures would compete
/// for movement, camera and button touches. The Metal view stays underneath it.
@MainActor
struct MobileControlsView: UIViewRepresentable {
    let settings: MobileControlsSettings
    let editing: Bool
    let suspended: Bool
    let cancellation: UInt64
    let onPad: (MobilePadSnapshot) -> Void
    let onLayout: (MobilePadControl, PadPoint) -> Void

    func makeUIView(context: Context) -> MobileControlsUIView { MobileControlsUIView(frame: .zero) }
    func updateUIView(_ view: MobileControlsUIView, context: Context) {
        view.configure(settings: settings, editing: editing, suspended: suspended,
                       cancellation: cancellation, onPad: onPad, onLayout: onLayout)
    }
    static func dismantleUIView(_ view: MobileControlsUIView, coordinator: ()) { view.shutdown() }
}

@MainActor
final class MobileControlsUIView: UIView {
    private struct LayoutDrag {
        let control: MobilePadControl
        let offset: PadPoint
        let original: PadPoint
    }
    private var input = MobilePadInput()
    private var settings = MobileControlsSettings()
    private var editing = false
    private var suspended = true
    private var applicationActive = false
    private var cancellation: UInt64?
    private var identities: [ObjectIdentifier: Int] = [:]
    private var nextIdentity = 0
    private var drags: [Int: LayoutDrag] = [:]
    private var displayLink: CADisplayLink?
    private var published = MobilePadSnapshot.neutral
    private var onPad: (MobilePadSnapshot) -> Void = { _ in }
    private var onLayout: (MobilePadControl, PadPoint) -> Void = { _, _ in }

    override init(frame: CGRect) {
        super.init(frame: frame)
        isMultipleTouchEnabled = true
        isOpaque = false
        backgroundColor = .clear
        accessibilityLabel = "ゲーム操作"
        accessibilityHint = "左側をドラッグして移動、右側でカメラ操作。操作設定から感度とボタン配置を変更できます。"
        applicationActive = UIApplication.shared.applicationState == .active
        NotificationCenter.default.addObserver(self, selector: #selector(applicationInactive),
            name: UIApplication.willResignActiveNotification, object: nil)
        NotificationCenter.default.addObserver(self, selector: #selector(applicationInactive),
            name: UIApplication.didEnterBackgroundNotification, object: nil)
        NotificationCenter.default.addObserver(self, selector: #selector(applicationBecameActive),
            name: UIApplication.didBecomeActiveNotification, object: nil)
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }
    deinit { NotificationCenter.default.removeObserver(self) }

    @objc private func applicationInactive() {
        applicationActive = false; cancelAll(); updateDisplayLink(); setNeedsDisplay()
    }
    @objc private func applicationBecameActive() {
        // A finger held across interruption never resumes its old input.
        cancelAll(); applicationActive = true; updateDisplayLink(); setNeedsDisplay()
    }

    func configure(settings value: MobileControlsSettings, editing: Bool, suspended: Bool,
                   cancellation: UInt64, onPad: @escaping (MobilePadSnapshot) -> Void,
                   onLayout: @escaping (MobilePadControl, PadPoint) -> Void) {
        self.onPad = onPad; self.onLayout = onLayout
        if self.editing != editing || self.suspended != suspended || self.cancellation != cancellation {
            cancelAll()
        }
        self.settings = value.normalized()
        self.editing = editing; self.suspended = suspended; self.cancellation = cancellation
        input.configure(viewport: viewport, settings: self.settings)
        publish()
        updateDisplayLink()
        setNeedsDisplay()
    }
    override func layoutSubviews() {
        super.layoutSubviews()
        if input.viewport != viewport { cancelAll() }
        input.configure(viewport: viewport, settings: settings)
        publish(); setNeedsDisplay()
    }
    override func safeAreaInsetsDidChange() {
        super.safeAreaInsetsDidChange()
        cancelAll(); setNeedsDisplay()
    }
    override func didMoveToWindow() {
        super.didMoveToWindow()
        if window == nil { cancelAll() }
        updateDisplayLink()
    }
    func shutdown() { cancelAll(); displayLink?.invalidate(); displayLink = nil }

    private var viewport: PadRect {
        MobilePadMath.viewport(in: PadRect(x: Double(bounds.minX), y: Double(bounds.minY),
                                          width: Double(bounds.width), height: Double(bounds.height)))
    }
    private var safe: PadRect {
        let rect = bounds.inset(by: UIEdgeInsets(top: safeAreaInsets.top + 6, left: safeAreaInsets.left + 6,
                                                bottom: safeAreaInsets.bottom + 6, right: safeAreaInsets.right + 6))
        return MobileControlsLayout.controlArea(in:
            PadRect(x: Double(rect.minX), y: Double(rect.minY), width: Double(rect.width), height: Double(rect.height)))
    }
    private func size(_ control: MobilePadControl) -> PadPoint {
        MobileControlsLayout.buttonSize(control, viewport: viewport, scale: settings.buttonScale)
    }
    private func frame(_ control: MobilePadControl) -> CGRect {
        let size = size(control)
        let p = MobilePadMath.center(position: settings.position(control), viewport: viewport, safe: safe, size: size)
        return CGRect(x: p.x - size.x / 2, y: p.y - size.y / 2, width: size.x, height: size.y)
    }
    private func hit(_ point: CGPoint, retaining held: MobilePadControl? = nil) -> MobilePadControl? {
        return MobileControlsLayout.hit(at: PadPoint(x: Double(point.x), y: Double(point.y)),
            frameFor: { control in
                let rect = frame(control)
                return PadRect(x: Double(rect.minX), y: Double(rect.minY),
                               width: Double(rect.width), height: Double(rect.height))
            }, retaining: held)
    }
    private func point(_ touch: UITouch) -> PadPoint {
        let p = touch.location(in: self)
        return PadPoint(x: Double(p.x), y: Double(p.y))
    }
    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard !suspended, applicationActive else { return }
        for touch in touches {
            let key = ObjectIdentifier(touch)
            guard identities[key] == nil else { continue }
            nextIdentity &+= 1
            let id = nextIdentity, p = point(touch), button = hit(touch.location(in: self))
            if editing {
                guard let control = button, !drags.values.contains(where: { $0.control == control }) else { continue }
                let rect = frame(control)
                drags[id] = LayoutDrag(control: control,
                    offset: PadPoint(x: p.x - Double(rect.midX), y: p.y - Double(rect.midY)),
                    original: settings.position(control))
                identities[key] = id
            } else if input.begin(id, at: p, time: touch.timestamp, button: button) { identities[key] = id }
        }
        publish(); setNeedsDisplay()
    }
    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard !suspended, applicationActive else { return }
        for touch in touches {
            guard let id = identities[ObjectIdentifier(touch)] else { continue }
            if let drag = drags[id] {
                let p = MobilePadMath.draggedPosition(at: point(touch), grabOffset: drag.offset,
                                                     viewport: viewport, safe: safe, size: size(drag.control))
                settings.positions[drag.control.rawValue] = p
                onLayout(drag.control, p)
            } else {
                let button = hit(touch.location(in: self), retaining: input.button(for: id))
                input.move(id, to: point(touch), time: touch.timestamp,
                           button: button)
            }
        }
        publish(); setNeedsDisplay()
    }
    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) { end(touches, cancelled: false) }
    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) { end(touches, cancelled: true) }
    private func end(_ touches: Set<UITouch>, cancelled: Bool) {
        for touch in touches {
            guard let id = identities.removeValue(forKey: ObjectIdentifier(touch)) else { continue }
            if let drag = drags.removeValue(forKey: id), cancelled {
                settings.positions[drag.control.rawValue] = drag.original
                onLayout(drag.control, drag.original)
            }
            input.end(id)
        }
        publish(); setNeedsDisplay()
    }
    private func cancelAll() {
        // Layout cancellation commits the last displayed position; ordinary
        // finger cancellation above restores just that finger's original position.
        identities.removeAll(); drags.removeAll(); input.cancelAll()
        publish()
    }
    private func publish() {
        let value = suspended || editing || !applicationActive ? MobilePadSnapshot.neutral : input.snapshot
        guard value != published else { return }
        published = value; onPad(value)
    }
    private func updateDisplayLink() {
        if window != nil && !suspended && !editing && applicationActive {
            if displayLink == nil {
                let link = CADisplayLink(target: self, selector: #selector(tick(_:)))
                link.add(to: .main, forMode: .common)
                displayLink = link
            }
        } else { displayLink?.invalidate(); displayLink = nil }
    }
    @objc private func tick(_ link: CADisplayLink) {
        let previous = input.snapshot
        input.tick(time: link.timestamp)
        publish()
        if input.snapshot != previous { setNeedsDisplay() }
    }

    override func draw(_ rect: CGRect) {
        guard let context = UIGraphicsGetCurrentContext(), viewport.valid else { return }
        let highContrast = UIAccessibility.isDarkerSystemColorsEnabled
        let opaque = UIAccessibility.isReduceTransparencyEnabled || highContrast
        for control in MobilePadControl.allCases {
            let frame = frame(control)
            let active = input.held(control) || drags.values.contains { $0.control == control }
            let path = UIBezierPath(roundedRect: frame, cornerRadius: control == .start || control == .back ? 12 : frame.height / 2)
            UIColor(white: active ? 0.95 : 0.08,
                    alpha: opaque ? 1 : (active ? max(0.85, settings.buttonOpacity) : settings.buttonOpacity)).setFill()
            path.fill()
            UIColor.white.withAlphaComponent(editing || highContrast ? 1 : 0.65).setStroke()
            path.lineWidth = editing ? 2 : 1; path.stroke()
            let attributes: [NSAttributedString.Key: Any] = [
                .font: UIFontMetrics(forTextStyle: .caption1).scaledFont(
                    for: UIFont.systemFont(ofSize: control == .start || control == .back ? 13 : 16, weight: .semibold),
                    maximumPointSize: 20),
                .foregroundColor: active ? UIColor.black : UIColor.white
            ]
            let text = control.label as NSString, size = text.size(withAttributes: attributes)
            text.draw(at: CGPoint(x: frame.midX - size.width / 2, y: frame.midY - size.height / 2), withAttributes: attributes)
        }
        guard !editing else { return }
        func stick(_ anchor: PadPoint?, vector: PadPoint) {
            guard let anchor else { return }
            let radius = max(1, viewport.height * 0.12)
            let center = CGPoint(x: anchor.x, y: anchor.y)
            context.setStrokeColor(UIColor.white.withAlphaComponent(0.7).cgColor)
            context.setLineWidth(1.5)
            context.strokeEllipse(in: CGRect(x: center.x - radius, y: center.y - radius, width: radius * 2, height: radius * 2))
            context.setFillColor(UIColor.white.withAlphaComponent(0.7).cgColor)
            let knob = CGPoint(x: anchor.x + vector.x * radius, y: anchor.y - vector.y * radius)
            context.fillEllipse(in: CGRect(x: knob.x - 14, y: knob.y - 14, width: 28, height: 28))
        }
        stick(input.movementAnchor, vector: input.leftVector)
        if settings.cameraMode == .stick { stick(input.cameraAnchor, vector: input.cameraVisualVector) }
    }
}
