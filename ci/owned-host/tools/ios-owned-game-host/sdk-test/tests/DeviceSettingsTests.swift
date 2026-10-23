import Foundation
import SwiftUI
import UIKit
import XCTest
@testable import OwnedGameHost

final class DeviceSettingsTests: XCTestCase {
    func testPresetsProduceBoundedNativeTuplesAndKeepOriginalDefault() {
        XCTAssertEqual(DevicePreset.original.graphics, DeviceGraphicsSettings())
        for preset in DevicePreset.allCases {
            let g = preset.graphics, boot = g.boot, live = g.presentation
            XCTAssertEqual(g, g.normalized())
            XCTAssertEqual(boot.version, 0x0001_0000); XCTAssertEqual(boot.struct_bytes, 32)
            XCTAssertLessThanOrEqual(boot.video_width * boot.internal_scale, 3840)
            XCTAssertLessThanOrEqual(boot.video_height * boot.internal_scale, 2160)
            XCTAssertEqual(boot.reserved, 0)
            XCTAssertTrue(DeviceGraphicsSettings.frameLimits.contains(live.frame_limit))
            XCTAssertTrue(DeviceGraphicsSettings.outputScales.contains(live.output_scale_percent))
            XCTAssertEqual(live.reserved0, 0); XCTAssertEqual(live.reserved1, 0)
        }
        let original = DevicePreset.original.graphics
        XCTAssertEqual(original.videoMode, .p720); XCTAssertEqual(original.internalScale, 1)
        XCTAssertEqual(original.frameLimit, 0); XCTAssertEqual(original.filter, .original)
        XCTAssertFalse(original.mobileBudget); XCTAssertEqual(original.mirrorMiB, 0)
    }
    func testInvalidPersistedValuesCannotRequestUnboundedAllocationOrRate() throws {
        var settings = DeviceSettings()
        settings.graphics = .init(videoMode: .p1080, internalScale: .max, mirrorMiB: .max,
            mobileBudget: true, frameLimit: .max, outputScale: .max, filter: .nearest, sharpness: .max)
        let decoded = try JSONDecoder().decode(DeviceSettings.self, from: JSONEncoder().encode(settings)).normalized()
        XCTAssertEqual(decoded.graphics.internalScale, 2); XCTAssertEqual(decoded.graphics.mirrorMiB, 0)
        XCTAssertEqual(decoded.graphics.frameLimit, 0); XCTAssertEqual(decoded.graphics.outputScale, 100)
        XCTAssertEqual(decoded.graphics.sharpness, 100)
        settings.audio.gain = .infinity; XCTAssertEqual(settings.normalized().audio.gain, 1)
        settings.audio.gain = -2; XCTAssertEqual(settings.normalized().audio.gain, 0)
        settings.version = 99; XCTAssertEqual(settings.normalized(), DeviceSettings())
    }
    func testNativeStructLayoutsMatchCContracts() {
        XCTAssertEqual(MemoryLayout<OHBootGraphicsV1>.size, 32)
        XCTAssertEqual(MemoryLayout<OHPresentationSettingsV1>.size, 32)
        XCTAssertEqual(MemoryLayout<OHGraphicsStatusV1>.size, 120)
        XCTAssertEqual(MemoryLayout<OHGraphicsStatusV1>.alignment, 8)
        XCTAssertEqual(MemoryLayout<OHHostAudioTransportV1>.size, 160)
        XCTAssertEqual(MemoryLayout<OHNativeAudioStatusV1>.size, 256)
    }
    @MainActor
    func testLocalPreferencesRoundTripAndRefuseUnknownOrOversizedData() throws {
        let name = "public-device-settings-\(UUID().uuidString)"
        let defaults = try XCTUnwrap(UserDefaults(suiteName: name))
        defer { defaults.removePersistentDomain(forName: name) }
        let first = DevicePreferences(defaults: defaults)
        first.apply(.performance)
        first.audio { $0.gain = 0.35; $0.muted = true; $0.profile = .robust }
        XCTAssertEqual(DevicePreferences(defaults: defaults).settings, first.settings)
        first.graphics { $0.frameLimit = 24 }
        XCTAssertEqual(first.settings.graphics.presetLabel, "カスタム")
        defaults.set(Data(repeating: 0, count: 4097), forKey: DevicePreferences.key)
        XCTAssertEqual(DevicePreferences(defaults: defaults).settings, DeviceSettings())
        defaults.set(Data("{\"version\":99}".utf8), forKey: DevicePreferences.key)
        XCTAssertEqual(DevicePreferences(defaults: defaults).settings, DeviceSettings())
    }
    @MainActor
    func testPublicSettingsScreensRenderWithoutStartingGuest() throws {
        let name = "public-device-settings-screen-\(UUID().uuidString)"
        let defaults = try XCTUnwrap(UserDefaults(suiteName: name))
        defer { defaults.removePersistentDomain(forName: name) }
        let preferences = DevicePreferences(defaults: defaults)
        let graphics = UIHostingController(rootView: DeviceGraphicsSettingsView(preferences: preferences, session: .shared))
        let audio = UIHostingController(rootView: DeviceAudioSettingsView(preferences: preferences, session: .shared))
        let scene = try XCTUnwrap(UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
            .first { $0.activationState == .foregroundActive })
        let window = UIWindow(windowScene: scene)
        window.frame = CGRect(x: 0, y: 0, width: 956, height: 440)
        defer { window.isHidden = true; window.rootViewController = nil }
        for (name, controller) in [("PublicGraphicsSettings", graphics as UIViewController), ("PublicAudioSettings", audio as UIViewController)] {
            window.rootViewController = controller; window.isHidden = false
            controller.loadViewIfNeeded(); controller.view.frame = window.bounds
            controller.view.backgroundColor = .black; controller.view.layoutIfNeeded()
            window.layoutIfNeeded()
            let format = UIGraphicsImageRendererFormat(); format.scale = 1; format.opaque = true
            format.preferredRange = .standard
            var drawn = false
            let image = UIGraphicsImageRenderer(size: controller.view.bounds.size, format: format).image { context in
                UIColor.black.setFill(); context.fill(controller.view.bounds)
                drawn = controller.view.drawHierarchy(in: controller.view.bounds, afterScreenUpdates: true)
            }
            XCTAssertTrue(drawn)
            let attachment = XCTAttachment(data: try XCTUnwrap(image.pngData()), uniformTypeIdentifier: "public.png")
            attachment.name = name; attachment.lifetime = .keepAlways
            add(attachment); XCTAssertEqual(image.size.width, 956)
        }
        XCTAssertFalse(OwnedSession.shared.attempted)
    }
}
