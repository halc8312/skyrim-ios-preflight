import AVFAudio
import Foundation
import XCTest
@testable import OwnedGameHost

// Public host transport/session tests. No guest module, content, microphone or
// audible-output assertion. Run this class serially in the SDK fixture process.
// Hosted XCTest awaits this worker so Main remains free during CoreAudio RPCs.
private let audioTestWorker = DispatchQueue(label: "owned-host.audio-sdk-tests")
private func audioWorker<T>(_ operation: @escaping () -> T) async -> T {
    await withCheckedContinuation { continuation in
        audioTestWorker.async { continuation.resume(returning: operation()) }
    }
}

final class OwnedHostAudioTests: XCTestCase {
    override func setUp() async throws {
        executionTimeAllowance = 30
        await succeeded { OHNativeAudioStopV1() }
        await endInterruption(resume: false)
        await succeeded { OHNativeAudioSetControlsV1(1, 0, 0) }
    }

    override func tearDown() async throws {
        await succeeded { OHNativeAudioStopV1() }
        await endInterruption(resume: false)
    }

    private func succeeded(_ operation: @escaping () -> Int32,
                           file: StaticString = #filePath, line: UInt = #line) async {
        let code = await audioWorker(operation)
        XCTAssertEqual(code, 0, file: file, line: line)
    }

    private func status() async -> OHNativeAudioStatusV1 {
        let (code, result) = await audioWorker {
            var value = OHNativeAudioStatusV1()
            let code = OHNativeAudioReadStatusV1(&value, UInt32(MemoryLayout<OHNativeAudioStatusV1>.size))
            return (code, value)
        }
        XCTAssertEqual(code, 0)
        XCTAssertEqual(result.version, 1)
        XCTAssertEqual(result.struct_size, 256)
        XCTAssertEqual(result.transport.version, 1)
        XCTAssertEqual(result.transport.struct_size, 160)
        return result
    }

    private func beginInterruption() async {
        await audioWorker {
            NotificationCenter.default.post(name: AVAudioSession.interruptionNotification,
                object: AVAudioSession.sharedInstance(),
                userInfo: [AVAudioSessionInterruptionTypeKey: AVAudioSession.InterruptionType.began.rawValue])
        }
    }

    private func endInterruption(resume: Bool) async {
        await audioWorker {
            NotificationCenter.default.post(name: AVAudioSession.interruptionNotification,
                object: AVAudioSession.sharedInstance(), userInfo: [
                    AVAudioSessionInterruptionTypeKey: AVAudioSession.InterruptionType.ended.rawValue,
                    AVAudioSessionInterruptionOptionKey: resume ? AVAudioSession.InterruptionOptions.shouldResume.rawValue : 0,
                ])
        }
        _ = await status() // Controller barrier after the posted notification.
    }

    func testAudioStatusLayoutAndValidatedControls() async throws {
        XCTAssertEqual(MemoryLayout<OHHostAudioTransportV1>.size, 160)
        XCTAssertEqual(MemoryLayout<OHNativeAudioStatusV1>.size, 256)
        XCTAssertEqual(MemoryLayout<OHHostAudioTransportV1>.alignment, 8)
        XCTAssertEqual(MemoryLayout<OHNativeAudioStatusV1>.alignment, 8)
        let refused = await MainActor.run { () -> ([Int32], UInt32, UInt64) in
            var untouched = OHNativeAudioStatusV1(); untouched.version = 55
            return ([OHNativeAudioStartV1(), OHNativeAudioStopV1(),
                     OHNativeAudioApplyPlaybackIntentV1(0),
                     OHNativeAudioSetControlsV1(1, 0, 0),
                     OHNativeAudioReadStatusV1(&untouched, 256)],
                    untouched.version, OHNativeAudioSetPlaybackIntentV1(2))
        }
        XCTAssertEqual(refused.0, [-2, -2, -2, -2, -2])
        XCTAssertEqual(refused.1, 55); XCTAssertEqual(refused.2, 0)
        let profiles: [(UInt32, UInt32)] = [(0, 2048), (1, 512), (2, 4096)]
        for (profile, target) in profiles {
            await succeeded { OHNativeAudioSetControlsV1(0.375, 1, profile) }
            let observed = await status()
            XCTAssertEqual(observed.profile, profile)
            XCTAssertEqual(observed.transport.profile, profile)
            XCTAssertEqual(observed.transport.target_frames, target)
            XCTAssertEqual(observed.transport.gain_milli, 375)
            XCTAssertEqual(observed.transport.muted, 1)
        }
        let before = await status()
        let rejected: [(Float, UInt32, UInt32)] = [(.nan, 0, 0), (-0.1, 0, 0), (1.1, 0, 0), (0.5, 2, 0), (0.5, 0, 3)]
        for (gain, mute, profile) in rejected {
            let code = await audioWorker { OHNativeAudioSetControlsV1(gain, mute, profile) }
            XCTAssertNotEqual(code, 0)
        }
        let after = await status()
        XCTAssertEqual(after.transport.controls_revision, before.transport.controls_revision)
        XCTAssertEqual(after.transport.gain_milli, 375)
        XCTAssertEqual(after.transport.muted, 1)
        XCTAssertEqual(after.transport.profile, 2)
        XCTAssertEqual(after.transport.initialized, 1)
        XCTAssertEqual(after.transport.sample_rate_hz, 48_000)
        XCTAssertEqual(after.transport.channels, 2)
        XCTAssertEqual(after.transport.capacity_frames, 8192)
        XCTAssertEqual(after.reserved0, 0); XCTAssertEqual(after.reserved1, 0)
        XCTAssertEqual(after.transport.reserved0, 0)
        XCTAssertEqual(after.transport.reserved1, 0); XCTAssertEqual(after.transport.reserved2, 0)
        let rejectedSize = await audioWorker { () -> (Int32, UInt32) in
            var untouched = OHNativeAudioStatusV1(); untouched.version = 55
            return (OHNativeAudioReadStatusV1(&untouched, 255), untouched.version)
        }
        XCTAssertEqual(rejectedSize.0, -1); XCTAssertEqual(rejectedSize.1, 55)
        let report: [String: Any] = [
            "schema": 1, "audio_version": 1, "transport_bytes": 160, "native_status_bytes": 256,
            "source_rate_hz": 48_000, "source_channels": 2, "client_capacity": 8,
            "queue_capacity_frames": 8192, "profiles": 3, "authored_only": true,
            "game_audio_verified": false, "audible_output_verified": false,
            "physical_iphone_verified": false,
        ]
        let data = try JSONSerialization.data(withJSONObject: report, options: [.sortedKeys])
        print("OWNED_HOST_AUDIO_RESULT " + String(decoding: data, as: UTF8.self))
    }

    func testAudioStartStopUsesPlaybackAndReportsActualHardware() async {
        let intents = await MainActor.run { () -> (UInt64, UInt64) in
            let queuedStart = OHNativeAudioSetPlaybackIntentV1(1)
            return (queuedStart, OHNativeAudioSetPlaybackIntentV1(0))
        }
        XCTAssertGreaterThan(intents.0, 0); XCTAssertGreaterThan(intents.1, intents.0)
        let stale = await audioWorker { OHNativeAudioApplyPlaybackIntentV1(intents.0) }
        XCTAssertEqual(stale, -4)
        await succeeded { OHNativeAudioApplyPlaybackIntentV1(intents.1) }
        let cancelled = await status()
        XCTAssertEqual(cancelled.engine_running, 0); XCTAssertEqual(cancelled.transport.running, 0)

        let mainResponsive = expectation(description: "Main queue services work during awaited audio startup")
        async let starting = audioWorker {
            DispatchQueue.main.async { mainResponsive.fulfill() }
            return OHNativeAudioStartV1()
        }
        await fulfillment(of: [mainResponsive], timeout: 2)
        let startCode = await starting
        XCTAssertEqual(startCode, 0)
        let started = await status()
        XCTAssertEqual(started.state, 3); XCTAssertEqual(started.engine_running, 1)
        XCTAssertEqual(started.user_wants_playback, 1); XCTAssertEqual(started.transport.running, 1)
        XCTAssertTrue((8_000...384_000).contains(started.actual_sample_rate_hz))
        XCTAssertGreaterThan(started.io_buffer_duration_us, 0)
        XCTAssertGreaterThan(started.output_channels, 0)
        XCTAssertTrue((1...5).contains(started.route_kind))
        let category = await audioWorker { AVAudioSession.sharedInstance().category }
        XCTAssertEqual(category, .playback)
        XCTAssertNil(Bundle.main.object(forInfoDictionaryKey: "NSMicrophoneUsageDescription"))
        await succeeded { OHNativeAudioStartV1() }
        let repeated = await status()
        XCTAssertEqual(repeated.engine_restarts, started.engine_restarts)
        await succeeded { OHNativeAudioStopV1() }
        let stopped = await status()
        XCTAssertEqual(stopped.state, 1); XCTAssertEqual(stopped.engine_running, 0)
        XCTAssertEqual(stopped.user_wants_playback, 0); XCTAssertEqual(stopped.transport.running, 0)
        XCTAssertEqual(stopped.render_callbacks_active, 0)
        XCTAssertEqual(stopped.actual_sample_rate_hz, 0)
        XCTAssertGreaterThan(stopped.transport.transport_epoch, started.transport.transport_epoch)
    }

    func testAudioInterruptionRequiresResumePermissionAndUserIntent() async {
        await succeeded { OHNativeAudioStartV1() }
        await beginInterruption()
        let paused = await status()
        XCTAssertEqual(paused.state, 4); XCTAssertEqual(paused.interrupted, 1)
        XCTAssertEqual(paused.engine_running, 0); XCTAssertEqual(paused.transport.running, 0)
        let beforeNoResume = await MainActor.run { OHNativeAudioSetPlaybackIntentV1(1) }
        await endInterruption(resume: false)
        let stale = await audioWorker { OHNativeAudioApplyPlaybackIntentV1(beforeNoResume) }
        XCTAssertEqual(stale, -4)
        let waiting = await status()
        XCTAssertEqual(waiting.resume_required, 1); XCTAssertEqual(waiting.engine_running, 0)
        await succeeded { OHNativeAudioStartV1() }
        await beginInterruption(); _ = await status(); await endInterruption(resume: true)
        let resumed = await status(); XCTAssertEqual(resumed.engine_running, 1)
        await beginInterruption(); _ = await status()
        await succeeded { OHNativeAudioStopV1() }
        await endInterruption(resume: true)
        let stopped = await status()
        XCTAssertEqual(stopped.engine_running, 0); XCTAssertEqual(stopped.user_wants_playback, 0)
    }

    func testAudioOutputDisconnectRequiresExplicitStart() async {
        await succeeded { OHNativeAudioStartV1() }
        let beforeDisconnect = await MainActor.run { OHNativeAudioSetPlaybackIntentV1(1) }
        await audioWorker {
            NotificationCenter.default.post(name: AVAudioSession.routeChangeNotification,
                object: AVAudioSession.sharedInstance(), userInfo: [
                    AVAudioSessionRouteChangeReasonKey: AVAudioSession.RouteChangeReason.oldDeviceUnavailable.rawValue,
                ])
        }
        let paused = await status()
        XCTAssertEqual(paused.state, 5); XCTAssertEqual(paused.resume_required, 1)
        XCTAssertEqual(paused.engine_running, 0); XCTAssertEqual(paused.transport.running, 0)
        let stale = await audioWorker { OHNativeAudioApplyPlaybackIntentV1(beforeDisconnect) }
        XCTAssertEqual(stale, -4)
        await audioWorker {
            NotificationCenter.default.post(name: AVAudioSession.routeChangeNotification,
                object: AVAudioSession.sharedInstance(), userInfo: [
                    AVAudioSessionRouteChangeReasonKey: AVAudioSession.RouteChangeReason.newDeviceAvailable.rawValue,
                ])
        }
        let waiting = await status(); XCTAssertEqual(waiting.engine_running, 0)
        await succeeded { OHNativeAudioStartV1() }
        let started = await status()
        XCTAssertEqual(started.engine_running, 1); XCTAssertEqual(started.resume_required, 0)
    }

    func testAudioMediaServicesResetRequiresExplicitStart() async {
        await succeeded { OHNativeAudioStartV1() }
        let before = await status()
        let beforeLoss = await MainActor.run { OHNativeAudioSetPlaybackIntentV1(1) }
        await audioWorker {
            NotificationCenter.default.post(name: AVAudioSession.mediaServicesWereLostNotification,
                object: AVAudioSession.sharedInstance())
        }
        let lost = await status(); XCTAssertEqual(lost.state, 6)
        let staleLoss = await audioWorker { OHNativeAudioApplyPlaybackIntentV1(beforeLoss) }
        XCTAssertEqual(staleLoss, -4)
        let refused = await audioWorker { OHNativeAudioStartV1() }
        XCTAssertNotEqual(refused, 0)
        let beforeReset = await MainActor.run { OHNativeAudioSetPlaybackIntentV1(1) }
        await audioWorker {
            NotificationCenter.default.post(name: AVAudioSession.mediaServicesWereResetNotification,
                object: AVAudioSession.sharedInstance())
        }
        let reset = await status()
        XCTAssertEqual(reset.state, 6); XCTAssertEqual(reset.resume_required, 1)
        XCTAssertEqual(reset.engine_running, 0); XCTAssertEqual(reset.transport.running, 0)
        XCTAssertEqual(reset.media_resets, before.media_resets + 1)
        let staleReset = await audioWorker { OHNativeAudioApplyPlaybackIntentV1(beforeReset) }
        XCTAssertEqual(staleReset, -4)
        await succeeded { OHNativeAudioStartV1() }
        let started = await status(); XCTAssertEqual(started.engine_running, 1)
    }

    func testAudioPullIsPlanarSilenceWithoutClients() async {
        let before = await status()
        XCTAssertEqual(before.transport.active_clients, 0)
        var left = [Float](repeating: 99, count: 256)
        var right = [Float](repeating: 99, count: 256)
        left.withUnsafeMutableBufferPointer { l in
            right.withUnsafeMutableBufferPointer { r in
                XCTAssertEqual(OHHostAudioPullStereoV1(l.baseAddress, r.baseAddress, 256), 0)
            }
        }
        XCTAssertTrue(left.allSatisfy { $0 == 0 }); XCTAssertTrue(right.allSatisfy { $0 == 0 })
        let after = await status()
        XCTAssertEqual(after.transport.rendered_frames, before.transport.rendered_frames + 256)
        XCTAssertEqual(after.transport.non_silent_frames, before.transport.non_silent_frames)
    }
}
