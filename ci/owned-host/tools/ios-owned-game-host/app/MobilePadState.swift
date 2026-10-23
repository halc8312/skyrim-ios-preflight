import Foundation

struct MobilePadSnapshot: Equatable {
    var buttons: UInt16 = 0
    var leftTrigger: UInt8 = 0
    var rightTrigger: UInt8 = 0
    var lx: Int16 = 0
    var ly: Int16 = 0
    var rx: Int16 = 0
    var ry: Int16 = 0
    static let neutral = MobilePadSnapshot()
}

struct PadPoint: Codable, Equatable {
    var x: Double
    var y: Double
    static let zero = PadPoint(x: 0, y: 0)
}

struct PadRect: Equatable {
    var x: Double
    var y: Double
    var width: Double
    var height: Double
    var valid: Bool { x.isFinite && y.isFinite && width.isFinite && height.isFinite && width > 0 && height > 0 }
    var midX: Double { x + width / 2 }
    var midY: Double { y + height / 2 }
    func contains(_ p: PadPoint) -> Bool {
        valid && p.x.isFinite && p.y.isFinite && p.x >= x && p.x <= x + width && p.y >= y && p.y <= y + height
    }
    func intersection(_ other: PadRect) -> PadRect {
        let left = max(x, other.x), top = max(y, other.y)
        return PadRect(x: left, y: top, width: max(0, min(x + width, other.x + other.width) - left),
                       height: max(0, min(y + height, other.y + other.height) - top))
    }
}

enum MobilePadControl: String, CaseIterable, Codable, Hashable {
    case up, down, left, right, start, back, l3, r3, lb, rb, lt, rt, a, b, x, y
    var label: String {
        switch self {
        case .up: return "↑"
        case .down: return "↓"
        case .left: return "←"
        case .right: return "→"
        case .start: return "Start"
        case .back: return "Back"
        default: return rawValue.uppercased()
        }
    }
    var mask: UInt16 {
        switch self {
        case .up: return 0x0001
        case .down: return 0x0002
        case .left: return 0x0004
        case .right: return 0x0008
        case .start: return 0x0010
        case .back: return 0x0020
        case .l3: return 0x0040
        case .r3: return 0x0080
        case .lb: return 0x0100
        case .rb: return 0x0200
        case .a: return 0x1000
        case .b: return 0x2000
        case .x: return 0x4000
        case .y: return 0x8000
        case .lt, .rt: return 0
        }
    }
    var defaultPosition: PadPoint {
        switch self {
        case .up: return PadPoint(x: 0.11, y: 0.40)
        case .down: return PadPoint(x: 0.11, y: 0.65)
        case .left: return PadPoint(x: 0.045, y: 0.525)
        case .right: return PadPoint(x: 0.175, y: 0.525)
        case .back: return PadPoint(x: 0.44, y: 0.10)
        case .start: return PadPoint(x: 0.56, y: 0.10)
        case .lb: return PadPoint(x: 0.20, y: 0.25)
        case .lt: return PadPoint(x: 0.075, y: 0.25)
        case .rb: return PadPoint(x: 0.80, y: 0.25)
        case .rt: return PadPoint(x: 0.925, y: 0.25)
        case .l3: return PadPoint(x: 0.28, y: 0.84)
        case .r3: return PadPoint(x: 0.70, y: 0.84)
        case .a: return PadPoint(x: 0.865, y: 0.84)
        case .b: return PadPoint(x: 0.94, y: 0.69)
        case .x: return PadPoint(x: 0.79, y: 0.69)
        case .y: return PadPoint(x: 0.865, y: 0.54)
        }
    }
}

enum MobileCameraMode: String, Codable, CaseIterable, Hashable {
    case swipe, stick
    var label: String { self == .swipe ? "スワイプ" : "スティック" }
}

enum MobileControlsLayout {
    static let toolbarHeight = 44.0
    static let toolbarBottomPadding = 8.0
    static let toolbarClearance = 8.0
    static var reservedBottom: Double { toolbarHeight + toolbarBottomPadding + toolbarClearance }
    static func controlArea(in safeBounds: PadRect) -> PadRect {
        guard safeBounds.valid else { return PadRect(x: 0, y: 0, width: 0, height: 0) }
        return PadRect(x: safeBounds.x, y: safeBounds.y, width: safeBounds.width,
                       height: max(0, safeBounds.height - reservedBottom))
    }
    static func buttonSize(_ control: MobilePadControl, viewport: PadRect, scale: Double) -> PadPoint {
        let scale = scale.isFinite ? min(1.35, max(1, scale)) : 1
        let diameter = max(44, min(54, viewport.height * 0.13))
        return PadPoint(x: (control == .start || control == .back ? 64 : diameter) * scale,
                        y: diameter * scale)
    }
    static func hit(at point: PadPoint, frameFor: (MobilePadControl) -> PadRect?,
                    retaining held: MobilePadControl? = nil) -> MobilePadControl? {
        // Small finger drift should not interrupt a held action. Leaving the
        // extra eight points still releases it, and another button can be slid onto.
        if let held, let frame = frameFor(held), frame.valid {
            let tolerance = PadRect(x: frame.x - 8, y: frame.y - 8,
                                    width: frame.width + 16, height: frame.height + 16)
            if tolerance.contains(point) { return held }
        }
        return MobilePadControl.allCases.reversed().first { frameFor($0)?.contains(point) == true }
    }
}

struct MobileControlsSettings: Codable, Equatable {
    var version = 1
    var sensitivity = 1.0
    var invertY = false
    var cameraMode: MobileCameraMode = .stick
    var buttonScale = 1.0
    var buttonOpacity = 0.64
    var positions: [String: PadPoint] = [:]

    private enum CodingKeys: String, CodingKey {
        case version, sensitivity, invertY, cameraMode, buttonScale, buttonOpacity, positions
    }
    init() {}
    init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        version = try values.decode(Int.self, forKey: .version)
        sensitivity = try values.decode(Double.self, forKey: .sensitivity)
        invertY = try values.decode(Bool.self, forKey: .invertY)
        // Existing saved swipe choices and layouts must not become new defaults.
        cameraMode = try values.decode(MobileCameraMode.self, forKey: .cameraMode)
        positions = try values.decode([String: PadPoint].self, forKey: .positions)
        buttonScale = try values.decodeIfPresent(Double.self, forKey: .buttonScale) ?? 1
        buttonOpacity = try values.decodeIfPresent(Double.self, forKey: .buttonOpacity) ?? 0.64
    }

    func normalized() -> MobileControlsSettings {
        guard version == 1 else { return MobileControlsSettings() }
        var result = self
        result.sensitivity = sensitivity.isFinite ? min(3, max(0.25, sensitivity)) : 1
        result.buttonScale = buttonScale.isFinite ? min(1.35, max(1, buttonScale)) : 1
        result.buttonOpacity = buttonOpacity.isFinite ? min(0.85, max(0.35, buttonOpacity)) : 0.64
        result.positions = [:]
        for control in MobilePadControl.allCases {
            guard let p = positions[control.rawValue], p.x.isFinite, p.y.isFinite else { continue }
            result.positions[control.rawValue] = PadPoint(x: min(1, max(0, p.x)), y: min(1, max(0, p.y)))
        }
        return result
    }
    func position(_ control: MobilePadControl) -> PadPoint { positions[control.rawValue] ?? control.defaultPosition }
}

enum MobilePadMath {
    static func axis(_ value: Double) -> Int16 {
        guard value.isFinite else { return 0 }
        return Int16((min(1, max(-1, value)) * 32767).rounded())
    }
    static func unit(_ x: Double, _ y: Double) -> PadPoint {
        guard x.isFinite, y.isFinite else { return .zero }
        let length = hypot(x, y)
        return length > 1 ? PadPoint(x: x / length, y: y / length) : PadPoint(x: x, y: y)
    }
    static func viewport(in bounds: PadRect) -> PadRect {
        guard bounds.valid else { return PadRect(x: 0, y: 0, width: 0, height: 0) }
        let width = min(bounds.width, bounds.height * 16 / 9)
        let height = width * 9 / 16
        return PadRect(x: bounds.midX - width / 2, y: bounds.midY - height / 2, width: width, height: height)
    }
    static func center(position: PadPoint, viewport: PadRect, safe: PadRect, size: PadPoint) -> PadPoint {
        guard viewport.valid, size.x.isFinite, size.y.isFinite, size.x > 0, size.y > 0 else { return .zero }
        guard safe.valid else { return PadPoint(x: viewport.midX, y: viewport.midY) }
        let area = viewport.intersection(safe)
        guard area.valid else { return PadPoint(x: viewport.midX, y: viewport.midY) }
        func clamp(_ value: Double, _ origin: Double, _ extent: Double, _ half: Double) -> Double {
            guard extent >= half * 2 else { return origin + extent / 2 }
            return min(origin + extent - half, max(origin + half, value))
        }
        let px = position.x.isFinite ? position.x : 0.5, py = position.y.isFinite ? position.y : 0.5
        return PadPoint(x: clamp(viewport.x + px * viewport.width, area.x, area.width, size.x / 2),
                        y: clamp(viewport.y + py * viewport.height, area.y, area.height, size.y / 2))
    }
    static func draggedPosition(at point: PadPoint, grabOffset: PadPoint, viewport: PadRect,
                                safe: PadRect, size: PadPoint) -> PadPoint {
        guard viewport.valid else { return PadPoint(x: 0.5, y: 0.5) }
        let raw = PadPoint(x: (point.x - grabOffset.x - viewport.x) / viewport.width,
                           y: (point.y - grabOffset.y - viewport.y) / viewport.height)
        let p = center(position: raw, viewport: viewport, safe: safe, size: size)
        return PadPoint(x: (p.x - viewport.x) / viewport.width, y: (p.y - viewport.y) / viewport.height)
    }
}

/// Pure touch ownership. It stores copied coordinates, never UITouch or native pointers.
struct MobilePadInput {
    private enum Role { case button(MobilePadControl?), move(PadPoint), camera(PadPoint, PadPoint, Double) }
    private var roles: [Int: Role] = [:]
    private var left = PadPoint.zero
    private var look = PadPoint.zero
    private var lastSwipeMotion = -Double.infinity
    private(set) var snapshot = MobilePadSnapshot.neutral
    private(set) var viewport = PadRect(x: 0, y: 0, width: 0, height: 0)
    private(set) var settings = MobileControlsSettings()
    var movementAnchor: PadPoint? { roles.values.compactMap { role -> PadPoint? in
        if case .move(let p) = role { return p }; return nil
    }.first }
    var cameraAnchor: PadPoint? { roles.values.compactMap { role -> PadPoint? in
        if case .camera(let p, _, _) = role { return p }; return nil
    }.first }
    var leftVector: PadPoint { left }
    var rightVector: PadPoint { look }
    var cameraVisualVector: PadPoint {
        // The knob follows the finger; gain, curve and inverted game Y must
        // change the input only, never displace the visual away from the finger.
        guard viewport.valid else { return .zero }
        for role in roles.values {
            if case .camera(let anchor, let point, _) = role {
                let radius = max(1, viewport.height * 0.12)
                return MobilePadMath.unit((point.x - anchor.x) / radius, -(point.y - anchor.y) / radius)
            }
        }
        return .zero
    }
    func held(_ control: MobilePadControl) -> Bool {
        roles.values.contains { if case .button(let held) = $0 { return held == control }; return false }
    }
    func button(for id: Int) -> MobilePadControl? {
        guard let role = roles[id], case .button(let control) = role else { return nil }
        return control
    }

    mutating func configure(viewport: PadRect, settings: MobileControlsSettings) {
        let value = settings.normalized()
        if self.viewport != viewport || self.settings.cameraMode != value.cameraMode
            || self.settings.invertY != value.invertY || self.settings.sensitivity != value.sensitivity
            || self.settings.buttonScale != value.buttonScale { cancelAll() }
        self.viewport = viewport
        self.settings = value
    }
    @discardableResult
    mutating func begin(_ id: Int, at p: PadPoint, time: Double, button: MobilePadControl?) -> Bool {
        guard roles[id] == nil, viewport.contains(p), time.isFinite else { return false }
        if let button { roles[id] = .button(button) }
        else if p.x < viewport.midX {
            guard movementAnchor == nil else { return false }
            roles[id] = .move(p)
        } else {
            guard cameraAnchor == nil else { return false }
            roles[id] = .camera(p, p, time)
        }
        rebuild()
        return true
    }
    mutating func move(_ id: Int, to p: PadPoint, time: Double, button: MobilePadControl?) {
        guard let role = roles[id], p.x.isFinite, p.y.isFinite, time.isFinite, viewport.valid else { return }
        switch role {
        case .button: roles[id] = .button(button)
        case .move(let anchor):
            let radius = max(1, viewport.height * 0.12)
            left = MobilePadMath.unit((p.x - anchor.x) / radius, -(p.y - anchor.y) / radius)
        case .camera(let anchor, let previous, let previousTime):
            let ySign = settings.invertY ? 1.0 : -1.0
            if settings.cameraMode == .stick {
                let radius = max(1, viewport.height * 0.12)
                let v = MobilePadMath.unit((p.x - anchor.x) / radius, (p.y - anchor.y) * ySign / radius)
                // Curve the magnitude, not each axis: preserve the finger's
                // direction and full diagonal travel while keeping fine control.
                let gain = hypot(v.x, v.y) * settings.sensitivity
                look = MobilePadMath.unit(v.x * gain, v.y * gain)
            } else {
                // XInput accepts a turn-rate axis, not an absolute camera angle.
                // Normalize finger velocity in viewport-heights/sec, responding on this event.
                guard time > previousTime else { return }
                // Use the real interval after a delayed event. Capping it at
                // 1/15 s inflated slow deliveries into fast camera turns.
                let dt = max(1.0 / 240, time - previousTime)
                let scale = max(1, viewport.height) * dt * 1.5
                look = MobilePadMath.unit((p.x - previous.x) * settings.sensitivity / scale,
                                          (p.y - previous.y) * ySign * settings.sensitivity / scale)
                lastSwipeMotion = time
            }
            roles[id] = .camera(anchor, p, time)
        }
        rebuild()
    }
    mutating func end(_ id: Int) {
        guard let role = roles.removeValue(forKey: id) else { return }
        switch role {
        case .move: left = .zero
        case .camera: look = .zero; lastSwipeMotion = -Double.infinity
        case .button: break
        }
        rebuild()
    }
    mutating func tick(time: Double) {
        // A stationary swipe finger must not leave a turn axis held. This is
        // display-driven release only; touch-down/move is never debounced.
        if settings.cameraMode == .swipe && time.isFinite && time - lastSwipeMotion > 1.0 / 30 {
            look = .zero
            rebuild()
        }
    }
    mutating func cancelAll() {
        roles.removeAll(); left = .zero; look = .zero
        lastSwipeMotion = -Double.infinity; snapshot = .neutral
    }
    private mutating func rebuild() {
        var value = MobilePadSnapshot()
        for control in MobilePadControl.allCases where held(control) {
            if control == .lt { value.leftTrigger = 255 }
            else if control == .rt { value.rightTrigger = 255 }
            else { value.buttons |= control.mask }
        }
        if value.buttons & 0x0003 == 0x0003 { value.buttons &= ~0x0003 }
        if value.buttons & 0x000C == 0x000C { value.buttons &= ~0x000C }
        value.lx = MobilePadMath.axis(left.x); value.ly = MobilePadMath.axis(left.y)
        value.rx = MobilePadMath.axis(look.x); value.ry = MobilePadMath.axis(look.y)
        snapshot = value
    }
}
