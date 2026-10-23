import Foundation
import UIKit
import XCTest
@testable import OwnedGameHost

final class MobileControlsTests: XCTestCase {
    private let viewport = PadRect(x: 0, y: 0, width: 1_000, height: 562.5)
    private func configured(_ mode: MobileCameraMode = .stick, invert: Bool = false, gain: Double = 1) -> MobilePadInput {
        var input = MobilePadInput(), settings = MobileControlsSettings()
        settings.cameraMode = mode; settings.invertY = invert; settings.sensitivity = gain
        input.configure(viewport: viewport, settings: settings)
        return input
    }

    func testSimultaneousMovementCameraAndButtonsPreserveFullSnapshot() {
        var input = configured()
        XCTAssertTrue(input.begin(1, at: PadPoint(x: 300, y: 350), time: 0, button: nil))
        input.move(1, to: PadPoint(x: 330, y: 320), time: 0.01, button: nil)
        XCTAssertTrue(input.begin(2, at: PadPoint(x: 650, y: 300), time: 0, button: nil))
        input.move(2, to: PadPoint(x: 680, y: 270), time: 0.01, button: nil)
        let axes = input.snapshot
        XCTAssertGreaterThan(axes.lx, 0); XCTAssertGreaterThan(axes.ly, 0)
        XCTAssertGreaterThan(axes.rx, 0); XCTAssertGreaterThan(axes.ry, 0)
        XCTAssertTrue(input.begin(3, at: PadPoint(x: 850, y: 450), time: 0.02, button: .a))
        XCTAssertTrue(input.begin(4, at: PadPoint(x: 100, y: 120), time: 0.02, button: .lt))
        XCTAssertEqual(input.snapshot.buttons, 0x1000); XCTAssertEqual(input.snapshot.leftTrigger, 255)
        XCTAssertEqual(input.snapshot.lx, axes.lx); XCTAssertEqual(input.snapshot.ly, axes.ly)
        XCTAssertEqual(input.snapshot.rx, axes.rx); XCTAssertEqual(input.snapshot.ry, axes.ry)
        input.end(3)
        XCTAssertEqual(input.snapshot.buttons, 0); XCTAssertEqual(input.snapshot.leftTrigger, 255)
        XCTAssertEqual(input.snapshot.lx, axes.lx); XCTAssertEqual(input.snapshot.rx, axes.rx)
        input.cancelAll(); XCTAssertEqual(input.snapshot, .neutral)
    }

    func testTwoFingersHoldingSameButtonReleaseIndependently() {
        var input = configured()
        for id in 1...2 { XCTAssertTrue(input.begin(id, at: PadPoint(x: 850, y: 450), time: 0, button: .a)) }
        input.end(1); XCTAssertEqual(input.snapshot.buttons, 0x1000)
        input.end(2); XCTAssertEqual(input.snapshot, .neutral)
    }

    func testCancellationAndReconfigurationNeutralizeAllInputs() {
        var input = configured()
        input.begin(1, at: PadPoint(x: 300, y: 350), time: 0, button: nil)
        input.move(1, to: PadPoint(x: 330, y: 350), time: 0.01, button: nil)
        input.begin(2, at: PadPoint(x: 900, y: 100), time: 0, button: .rt)
        let held = input.snapshot
        input.configure(viewport: viewport, settings: input.settings)
        XCTAssertEqual(input.snapshot, held)
        input.cancelAll(); XCTAssertEqual(input.snapshot, .neutral)
        input.begin(3, at: PadPoint(x: 900, y: 100), time: 0, button: .rt)
        var settings = input.settings; settings.cameraMode = .swipe
        input.configure(viewport: viewport, settings: settings)
        XCTAssertEqual(input.snapshot, .neutral)
        input.begin(4, at: PadPoint(x: 900, y: 100), time: 0, button: .rt)
        input.configure(viewport: PadRect(x: 10, y: 0, width: 980, height: 550), settings: settings)
        XCTAssertEqual(input.snapshot, .neutral)
        input.end(4); XCTAssertEqual(input.snapshot, .neutral)
    }

    func testStickAndSwipeDirectionsSensitivityAndNeutralization() {
        var normal = configured(), inverted = configured(invert: true), stronger = configured(gain: 2)
        for index in 0...2 {
            switch index {
            case 0:
                normal.begin(1, at: PadPoint(x: 650, y: 300), time: 0, button: nil)
                normal.move(1, to: PadPoint(x: 670, y: 280), time: 0.016, button: nil)
            case 1:
                inverted.begin(1, at: PadPoint(x: 650, y: 300), time: 0, button: nil)
                inverted.move(1, to: PadPoint(x: 670, y: 280), time: 0.016, button: nil)
            default:
                stronger.begin(1, at: PadPoint(x: 650, y: 300), time: 0, button: nil)
                stronger.move(1, to: PadPoint(x: 670, y: 280), time: 0.016, button: nil)
            }
        }
        XCTAssertGreaterThan(normal.snapshot.rx, 0); XCTAssertGreaterThan(normal.snapshot.ry, 0)
        XCTAssertEqual(inverted.snapshot.ry, -normal.snapshot.ry)
        XCTAssertGreaterThan(stronger.snapshot.rx, normal.snapshot.rx)
        XCTAssertEqual(inverted.cameraVisualVector, normal.cameraVisualVector)
        XCTAssertEqual(stronger.cameraVisualVector, normal.cameraVisualVector)
        var swipe = configured(.swipe)
        swipe.begin(1, at: PadPoint(x: 650, y: 300), time: 0, button: nil)
        swipe.move(1, to: PadPoint(x: 670, y: 280), time: 0.016, button: nil)
        XCTAssertGreaterThan(swipe.snapshot.rx, 0); XCTAssertGreaterThan(swipe.snapshot.ry, 0)
        swipe.tick(time: 0.02); XCTAssertGreaterThan(swipe.snapshot.rx, 0)
        swipe.tick(time: 0.06); XCTAssertEqual(swipe.snapshot, .neutral)
        swipe.end(1); XCTAssertNil(swipe.cameraAnchor)

        // Sparse guest polls can observe a held stick even when display ticks
        // continue between them. A swipe still intentionally releases after 33 ms.
        let held = normal.snapshot
        for poll in [1.0 / 15, 0.2, 0.5] {
            normal.tick(time: poll)
            XCTAssertEqual(normal.snapshot, held)
        }
        normal.end(1); XCTAssertEqual(normal.snapshot, .neutral)

        var diagonal = configured()
        diagonal.begin(1, at: PadPoint(x: 650, y: 300), time: 0, button: nil)
        let radius = viewport.height * 0.12
        diagonal.move(1, to: PadPoint(x: 650 + radius * 0.8, y: 300 - radius * 0.6), time: 0.02, button: nil)
        XCTAssertEqual(diagonal.snapshot.rx, MobilePadMath.axis(0.8))
        XCTAssertEqual(diagonal.snapshot.ry, MobilePadMath.axis(0.6))

        var delayed = configured(.swipe), timely = configured(.swipe)
        for id in 0...1 {
            if id == 0 {
                timely.begin(1, at: PadPoint(x: 650, y: 300), time: 0, button: nil)
                timely.move(1, to: PadPoint(x: 659, y: 300), time: 0.02, button: nil)
            } else {
                delayed.begin(1, at: PadPoint(x: 650, y: 300), time: 0, button: nil)
                delayed.move(1, to: PadPoint(x: 740, y: 300), time: 0.2, button: nil)
            }
        }
        XCTAssertEqual(delayed.snapshot.rx, timely.snapshot.rx, "Equal finger velocity must not accelerate after delayed delivery")
        let beforeStale = delayed.snapshot
        delayed.move(1, to: PadPoint(x: 600, y: 300), time: 0.2, button: nil)
        delayed.move(1, to: PadPoint(x: 600, y: 300), time: 0.1, button: nil)
        XCTAssertEqual(delayed.snapshot, beforeStale)
    }

    func testTouchOwnershipDoesNotStealActiveStick() {
        var input = configured()
        XCTAssertTrue(input.begin(1, at: PadPoint(x: 300, y: 350), time: 0, button: nil))
        XCTAssertFalse(input.begin(2, at: PadPoint(x: 310, y: 350), time: 0, button: nil))
        input.move(1, to: PadPoint(x: 330, y: 350), time: 0.01, button: nil)
        let held = input.snapshot
        input.move(2, to: PadPoint(x: 200, y: 350), time: 0.02, button: nil)
        input.end(2); XCTAssertEqual(input.snapshot, held)
        XCTAssertFalse(input.begin(1, at: PadPoint(x: 900, y: 100), time: 0.02, button: .rt))
        input.end(1); XCTAssertEqual(input.snapshot, .neutral)
        XCTAssertTrue(input.begin(2, at: PadPoint(x: 310, y: 350), time: 0.03, button: nil))
        XCTAssertFalse(input.begin(3, at: PadPoint(x: -1, y: 10), time: 0, button: .a))
    }

    func testButtonSlideAndOppositeDpadCancelSafely() {
        var input = configured()
        input.begin(1, at: PadPoint(x: 850, y: 450), time: 0, button: .a)
        input.move(1, to: PadPoint(x: 900, y: 400), time: 0.01, button: .b)
        XCTAssertEqual(input.snapshot.buttons, 0x2000)
        input.move(1, to: PadPoint(x: 700, y: 400), time: 0.02, button: nil)
        XCTAssertEqual(input.snapshot, .neutral)
        input.begin(2, at: PadPoint(x: 100, y: 200), time: 0, button: .up)
        input.begin(3, at: PadPoint(x: 100, y: 300), time: 0, button: .down)
        XCTAssertEqual(input.snapshot.buttons, 0)
        input.end(3); XCTAssertEqual(input.snapshot.buttons, 1)
        input.begin(4, at: PadPoint(x: 50, y: 250), time: 0, button: .left)
        XCTAssertEqual(input.snapshot.buttons, 5)
        XCTAssertEqual(MobilePadControl.allCases.reduce(UInt16(0)) { $0 | $1.mask } & 0x0C00, 0)

        let frames: [MobilePadControl: PadRect] = [
            .a: PadRect(x: 800, y: 400, width: 50, height: 50),
            .b: PadRect(x: 900, y: 400, width: 50, height: 50)
        ]
        var button = configured()
        button.begin(5, at: PadPoint(x: 825, y: 425), time: 0, button: .a)
        let drift = PadPoint(x: 854, y: 425)
        XCTAssertNil(MobileControlsLayout.hit(at: drift, frameFor: { frames[$0] }))
        let retained = MobileControlsLayout.hit(at: drift, frameFor: { frames[$0] }, retaining: button.button(for: 5))
        button.move(5, to: drift, time: 0.01,
                    button: retained)
        XCTAssertEqual(button.snapshot.buttons, MobilePadControl.a.mask)
        let released = PadPoint(x: 859, y: 425)
        let outside = MobileControlsLayout.hit(at: released, frameFor: { frames[$0] }, retaining: button.button(for: 5))
        button.move(5, to: released, time: 0.02, button: outside)
        XCTAssertEqual(button.snapshot, .neutral)
        button.move(5, to: PadPoint(x: 925, y: 425), time: 0.03,
                    button: MobileControlsLayout.hit(at: PadPoint(x: 925, y: 425), frameFor: { frames[$0] }))
        XCTAssertEqual(button.snapshot.buttons, MobilePadControl.b.mask)
        button.end(5); XCTAssertEqual(button.snapshot, .neutral)
        XCTAssertNil(MobileControlsLayout.hit(at: PadPoint(x: .nan, y: 425), frameFor: { frames[$0] }, retaining: .a))
    }

    func testAxisAndLayoutBoundsRejectNonFiniteInputs() {
        XCTAssertEqual(MobilePadMath.axis(.nan), 0); XCTAssertEqual(MobilePadMath.axis(.infinity), 0)
        XCTAssertEqual(MobilePadMath.axis(99), 32767); XCTAssertEqual(MobilePadMath.axis(-99), -32767)
        XCTAssertEqual(MobilePadMath.unit(.nan, 0), .zero)
        let clamped = MobilePadMath.unit(3, 4)
        XCTAssertEqual(hypot(clamped.x, clamped.y), 1, accuracy: 0.000001)
        let fitted = MobilePadMath.viewport(in: PadRect(x: 0, y: 0, width: 1000, height: 1000))
        XCTAssertEqual(fitted.width, 1000); XCTAssertEqual(fitted.height, 562.5)
        XCTAssertEqual(fitted.y, 218.75)
        XCTAssertFalse(MobilePadMath.viewport(in: PadRect(x: 0, y: 0, width: .nan, height: 10)).valid)
        XCTAssertEqual(MobilePadMath.center(position: .zero,
            viewport: PadRect(x: .nan, y: 0, width: 1, height: 1), safe: viewport, size: PadPoint(x: 44, y: 44)), .zero)
    }

    func testLayoutDragPreservesGrabOffsetAndClampsSafeBounds() {
        let rawSafe = PadRect(x: 40, y: 20, width: 920, height: 522.5), size = PadPoint(x: 44, y: 44)
        let safe = MobileControlsLayout.controlArea(in: rawSafe)
        XCTAssertEqual(safe.height, rawSafe.height - 60)
        let center = MobilePadMath.center(position: PadPoint(x: 0.8, y: 0.7), viewport: viewport, safe: safe, size: size)
        let offset = PadPoint(x: 8, y: -6), finger = PadPoint(x: center.x + 8, y: center.y - 6)
        let unchanged = MobilePadMath.draggedPosition(at: finger, grabOffset: offset, viewport: viewport, safe: safe, size: size)
        XCTAssertEqual(unchanged.x, 0.8, accuracy: 0.000001); XCTAssertEqual(unchanged.y, 0.7, accuracy: 0.000001)
        let outside = MobilePadMath.draggedPosition(at: PadPoint(x: -10_000, y: 10_000), grabOffset: offset,
                                                    viewport: viewport, safe: safe, size: size)
        let clamped = MobilePadMath.center(position: outside, viewport: viewport, safe: safe, size: size)
        XCTAssertEqual(clamped.x, 62); XCTAssertEqual(clamped.y, 460.5)
        // Even a button dragged to the bottom cannot land under Done/Reset/settings.
        XCTAssertLessThanOrEqual(clamped.y + size.y / 2, rawSafe.y + rawSafe.height - MobileControlsLayout.reservedBottom)
        let tiny = MobilePadMath.center(position: .zero, viewport: viewport,
            safe: PadRect(x: 10, y: 10, width: 5, height: 5), size: size)
        XCTAssertEqual(tiny, PadPoint(x: 12.5, y: 12.5))
        for control in MobilePadControl.allCases {
            let original = MobileControlsLayout.buttonSize(control, viewport: viewport, scale: 1)
            let enlarged = MobileControlsLayout.buttonSize(control, viewport: viewport, scale: 1.35)
            XCTAssertGreaterThanOrEqual(original.y, 44)
            XCTAssertEqual(enlarged.y, original.y * 1.35, accuracy: 0.000001)
            let center = MobilePadMath.center(position: PadPoint(x: 1, y: 1), viewport: viewport, safe: safe, size: enlarged)
            XCTAssertLessThanOrEqual(center.x + enlarged.x / 2, safe.x + safe.width)
            XCTAssertLessThanOrEqual(center.y + enlarged.y / 2, safe.y + safe.height)
        }
    }

    func testSettingsRoundTripAndVersionFallback() throws {
        var settings = MobileControlsSettings()
        XCTAssertEqual(settings.cameraMode, .stick)
        settings.sensitivity = 2; settings.invertY = true; settings.cameraMode = .stick
        settings.buttonScale = 1.25; settings.buttonOpacity = 0.4
        settings.positions["a"] = PadPoint(x: 0.8, y: 0.6)
        let decoded = try JSONDecoder().decode(MobileControlsSettings.self, from: JSONEncoder().encode(settings))
        XCTAssertEqual(decoded.normalized(), settings)
        settings.sensitivity = 99; settings.positions["a"] = PadPoint(x: -1, y: 2)
        settings.positions["unknown"] = .zero; settings.positions["b"] = PadPoint(x: .nan, y: 0)
        let normalized = settings.normalized()
        XCTAssertEqual(normalized.sensitivity, 3); XCTAssertEqual(normalized.positions["a"], PadPoint(x: 0, y: 1))
        XCTAssertNil(normalized.positions["unknown"]); XCTAssertNil(normalized.positions["b"])
        let legacy = Data(#"{"version":1,"sensitivity":1.5,"invertY":true,"cameraMode":"swipe","positions":{"a":{"x":0.7,"y":0.6}}}"#.utf8)
        let migrated = try JSONDecoder().decode(MobileControlsSettings.self, from: legacy).normalized()
        XCTAssertEqual(migrated.cameraMode, .swipe)
        XCTAssertEqual(migrated.sensitivity, 1.5); XCTAssertTrue(migrated.invertY)
        XCTAssertEqual(migrated.positions["a"], PadPoint(x: 0.7, y: 0.6))
        XCTAssertEqual(migrated.buttonScale, 1); XCTAssertEqual(migrated.buttonOpacity, 0.64)
        settings.buttonScale = 99; settings.buttonOpacity = -1
        XCTAssertEqual(settings.normalized().buttonScale, 1.35)
        XCTAssertEqual(settings.normalized().buttonOpacity, 0.35)
        settings.buttonScale = .nan; settings.buttonOpacity = .infinity
        XCTAssertEqual(settings.normalized().buttonScale, 1)
        XCTAssertEqual(settings.normalized().buttonOpacity, 0.64)
        settings.version = 2; XCTAssertEqual(settings.normalized(), MobileControlsSettings())
        settings.version = 1; settings.sensitivity = .nan; XCTAssertEqual(settings.normalized().sensitivity, 1)
    }

    @MainActor
    func testRenderedOverlayDefaultAndLayoutEditing() async throws {
        // A cold hosted test can start before UIKit connects its foreground scene.
        // Yield MainActor so scene activation can complete; absence still fails below.
        let clock = ContinuousClock()
        let deadline = clock.now.advanced(by: .seconds(2))
        while clock.now < deadline && UIApplication.shared.connectedScenes.compactMap({ $0 as? UIWindowScene })
            .first(where: { $0.activationState == .foregroundActive }) == nil {
            try await Task.sleep(for: .milliseconds(20))
        }
        let scene = try XCTUnwrap(UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
            .first { $0.activationState == .foregroundActive })
        let window = UIWindow(windowScene: scene)
        let controller = UIViewController()
        window.frame = CGRect(x: 0, y: 0, width: 844, height: 390)
        window.rootViewController = controller
        controller.view.backgroundColor = .black
        let overlay = MobileControlsUIView(frame: CGRect(x: 0, y: 0, width: 844, height: 390))
        controller.view.addSubview(overlay)
        window.isHidden = false
        defer {
            overlay.shutdown()
            overlay.removeFromSuperview()
            window.isHidden = true
            window.rootViewController = nil
        }

        for editing in [false, true] {
            var settings = MobileControlsSettings()
            if editing {
                // Keep the first image at default appearance. The second exercises
                // the largest supported targets and a lower saved opacity in UIKit.
                settings.buttonScale = 1.35
                settings.buttonOpacity = 0.4
            }
            overlay.configure(settings: settings, editing: editing, suspended: false,
                              cancellation: editing ? 1 : 0, onPad: { _ in }, onLayout: { _, _ in })
            window.layoutIfNeeded(); overlay.setNeedsLayout(); overlay.layoutIfNeeded()
            overlay.setNeedsDisplay(); overlay.layer.displayIfNeeded()
            let format = UIGraphicsImageRendererFormat()
            format.scale = 1; format.opaque = true; format.preferredRange = .standard
            var drawn = false
            let image = UIGraphicsImageRenderer(size: overlay.bounds.size, format: format).image { context in
                UIColor.black.setFill(); context.fill(overlay.bounds)
                drawn = overlay.drawHierarchy(in: overlay.bounds, afterScreenUpdates: true)
            }
            XCTAssertTrue(drawn, "UIKit hierarchy rendering must succeed")
            XCTAssertEqual(image.size, CGSize(width: 844, height: 390))
            XCTAssertNotNil(image.cgImage)
            let png = try XCTUnwrap(image.pngData())
            XCTAssertGreaterThan(png.count, 1_024)
            let attachment = XCTAttachment(data: png, uniformTypeIdentifier: "public.png")
            attachment.name = editing ? "MobileControls-layout-edit-844x390" : "MobileControls-default-844x390"
            attachment.lifetime = .keepAlways
            add(attachment)
        }
        // This checks real UIKit rendering only. It does not synthesize fingers,
        // start a guest, or establish phone/game touch behavior.
    }
}
