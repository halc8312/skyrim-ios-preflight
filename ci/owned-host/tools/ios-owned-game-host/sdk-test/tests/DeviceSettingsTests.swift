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
            XCTAssertLessThanOrEqual(boot.mirror_mib, 128)
            XCTAssertEqual(boot.reserved, 0)
            XCTAssertTrue(DeviceGraphicsSettings.frameLimits.contains(live.frame_limit))
            XCTAssertTrue(DeviceGraphicsSettings.outputScales.contains(live.output_scale_percent))
            XCTAssertEqual(live.reserved0, 0); XCTAssertEqual(live.reserved1, 0)
        }
        let original = DevicePreset.original.graphics
        XCTAssertEqual(original.videoMode, .p720); XCTAssertEqual(original.internalScale, 1)
        XCTAssertEqual(original.frameLimit, 0); XCTAssertEqual(original.filter, .original)
        XCTAssertEqual(original.outputScale, 100); XCTAssertEqual(original.sharpness, 0)
        XCTAssertFalse(original.mobileBudget); XCTAssertEqual(original.mirrorMiB, 0)
        XCTAssertEqual(original.boot.mobile_texture_budget, 0); XCTAssertEqual(original.boot.mirror_mib, 0)
        XCTAssertEqual(DeviceGraphicsSettings.mirrorBudgets, [0, 64, 128])
        for preset in [DevicePreset.quality, .ultra, .ultimate] {
            XCTAssertFalse(preset.graphics.mobileBudget, "\(preset.rawValue) must not defer textures through the mobile transfer limit")
            XCTAssertEqual(preset.graphics.boot.mobile_texture_budget, 0)
            XCTAssertEqual(preset.graphics.boot.mirror_mib, 128)
        }
        for preset in [DevicePreset.minimum, .performance, .balanced] {
            XCTAssertTrue(preset.graphics.mobileBudget, "\(preset.rawValue) must retain its mobile budget")
            XCTAssertEqual(preset.graphics.boot.mobile_texture_budget, 1)
        }
        XCTAssertEqual(DevicePreset.minimum.graphics.frameLimit, 0, "Maximum performance must not impose a lower FPS ceiling")
        XCTAssertEqual(DevicePreset.performance.graphics.frameLimit, 0)
        XCTAssertEqual(DevicePreset.minimum.graphics.outputScale, 25)
        XCTAssertEqual(DevicePreset.ultimate.graphics.internalScale, 3)
    }
    func testInvalidPersistedValuesCannotRequestUnboundedAllocationOrRate() throws {
        var settings = DeviceSettings()
        settings.graphics = .init(videoMode: .p1080, internalScale: .max, mirrorMiB: .max,
            mobileBudget: true, frameLimit: .max, outputScale: .max, filter: .nearest, sharpness: .max)
        let decoded = try JSONDecoder().decode(DeviceSettings.self, from: JSONEncoder().encode(settings)).normalized()
        XCTAssertEqual(decoded.graphics.internalScale, 2); XCTAssertEqual(decoded.graphics.mirrorMiB, 0)
        XCTAssertEqual(decoded.graphics.frameLimit, 0); XCTAssertEqual(decoded.graphics.outputScale, 100)
        XCTAssertEqual(decoded.graphics.sharpness, 100)
        for legacyBudget in [UInt32(192), UInt32(256)] {
            for mobileBudget in [false, true] {
                var saved = DeviceSettings()
                saved.graphics.mirrorMiB = legacyBudget; saved.graphics.mobileBudget = mobileBudget
                let migrated = try JSONDecoder().decode(DeviceSettings.self, from: JSONEncoder().encode(saved)).normalized()
                XCTAssertEqual(migrated.graphics.mirrorMiB, 128, "Saved budgets must retain their former effective limit")
                XCTAssertEqual(migrated.graphics.boot.mirror_mib, 128)
                XCTAssertEqual(migrated.graphics.mobileBudget, mobileBudget)
            }
        }
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
    func testAudioCommandWorkerCoalescesAndHonorsLatestIntent() async {
        let fixture = AudioCommandFixture()
        let queue = DispatchQueue(label: "public-audio-command-proof")
        let firstStop = expectation(description: "coalesced stop delivered on Main")
        let lastStop = expectation(description: "stop after in-flight start delivered on Main")
        let mainResponsive = expectation(description: "Main remains responsive")
        let started = expectation(description: "worker start entered")
        fixture.didEnterBlockedStart = { started.fulfill() }
        var updates: [UInt64] = []
        var revisions: [UInt64] = []
        let commands = DeviceAudioCommands(backend: fixture.backend, queue: queue) { value in
            XCTAssertTrue(Thread.isMainThread)
            updates.append(value.intent)
            revisions.append(value.controlsRevision)
            if value.intent == 2 { firstStop.fulfill() }
            if value.intent == 4 { lastStop.fulfill() }
        }
        queue.suspend()
        for index in 0..<1000 {
            var controls = DeviceAudioSettings()
            controls.gain = Double(index) / 999
            _ = commands.setControls(controls)
            commands.requestStatus()
        }
        XCTAssertEqual(commands.setPlaybackIntent(true), 1)
        XCTAssertEqual(commands.setPlaybackIntent(false), 2)
        DispatchQueue.main.async { mainResponsive.fulfill() }
        queue.resume()
        await fulfillment(of: [firstStop, mainResponsive], timeout: 3)
        let first = fixture.snapshot()
        XCTAssertEqual(first.applied, [2], "A cancelled queued start must not be submitted")
        XCTAssertEqual(first.controls, [1], "One latest control tuple replaces 1000 queued edits")
        XCTAssertEqual(first.statusReads, 1, "Status polling must remain coalesced")
        XCTAssertFalse(first.blockingCallOnMain)

        XCTAssertEqual(commands.setPlaybackIntent(true), 3)
        await fulfillment(of: [started], timeout: 3)
        var changedControls = DeviceAudioSettings()
        changedControls.gain = 0.5
        XCTAssertEqual(commands.setControls(changedControls), 1001)
        XCTAssertEqual(commands.setPlaybackIntent(false), 4)
        for _ in 0..<1000 { commands.requestStatus() }
        // The intent setter has already invalidated token 3, without waiting for
        // the simulated engine call. Native permit admission is proven in C++.
        XCTAssertEqual(fixture.snapshot().currentIntent, 4)
        fixture.releaseStart.signal()
        await fulfillment(of: [lastStop], timeout: 3)
        let last = fixture.snapshot()
        XCTAssertEqual(last.applied, [2, 3, 4])
        XCTAssertEqual(last.controls, [1, 0.5])
        XCTAssertEqual(last.statusReads, 3)
        XCTAssertFalse(last.blockingCallOnMain)
        XCTAssertEqual(updates, [2, 3, 4])
        XCTAssertEqual(revisions, [1000, 1000, 1001], "An in-flight result must retain its old revision for the Main consumer to refuse")
        XCTAssertFalse(last.blockedStartTimedOut)
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

private final class AudioCommandFixture: @unchecked Sendable {
    struct Snapshot {
        var applied: [UInt64] = []
        var controls: [Float] = []
        var statusReads = 0
        var currentIntent: UInt64 = 0
        var blockingCallOnMain = false
        var blockedStartTimedOut = false
    }
    private let lock = NSLock()
    private var state = Snapshot()
    let releaseStart = DispatchSemaphore(value: 0)
    var didEnterBlockedStart: (() -> Void)?
    func snapshot() -> Snapshot { lock.lock(); defer { lock.unlock() }; return state }
    var backend: DeviceAudioBackend {
        DeviceAudioBackend(setIntent: { [self] _ in
            XCTAssertTrue(Thread.isMainThread)
            lock.lock(); defer { lock.unlock() }
            state.currentIntent &+= 1
            return state.currentIntent
        }, applyIntent: { [self] token in
            lock.lock(); state.blockingCallOnMain = state.blockingCallOnMain || Thread.isMainThread
            state.applied.append(token); lock.unlock()
            if token == 3 {
                didEnterBlockedStart?()
                if releaseStart.wait(timeout: .now() + 3) == .timedOut {
                    lock.lock(); state.blockedStartTimedOut = true; lock.unlock()
                }
            }
            return snapshot().currentIntent == token ? 0 : -4
        }, controls: { [self] gain, _, _ in
            lock.lock(); defer { lock.unlock() }
            state.blockingCallOnMain = state.blockingCallOnMain || Thread.isMainThread
            state.controls.append(gain)
            return 0
        }, status: { [self] in
            lock.lock(); defer { lock.unlock() }
            state.blockingCallOnMain = state.blockingCallOnMain || Thread.isMainThread
            state.statusReads += 1
            var value = OHNativeAudioStatusV1()
            value.version = 1; value.struct_size = 256
            return (0, value)
        })
    }
}
