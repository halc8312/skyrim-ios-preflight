import AVFAudio
import Foundation
import XCTest
@testable import OwnedGameHost

// Public host transport/session tests. No guest module, content, microphone or
// audible-output assertion. Run this class serially in the SDK fixture process.
final class OwnedHostAudioTests: XCTestCase {
    override func setUpWithError() throws {
        executionTimeAllowance = 30
        XCTAssertEqual(OHNativeAudioStopV1(), 0)
        endInterruption(resume: false)
        XCTAssertEqual(OHNativeAudioSetControlsV1(1, 0, 0), 0)
    }

    override func tearDownWithError() throws {
        XCTAssertEqual(OHNativeAudioStopV1(), 0)
        endInterruption(resume: false)
    }

    private func status() -> OHNativeAudioStatusV1 {
        var result = OHNativeAudioStatusV1()
        XCTAssertEqual(OHNativeAudioReadStatusV1(&result, UInt32(MemoryLayout<OHNativeAudioStatusV1>.size)), 0)
        XCTAssertEqual(result.version, 1)
        XCTAssertEqual(result.struct_size, 256)
        XCTAssertEqual(result.transport.version, 1)
        XCTAssertEqual(result.transport.struct_size, 160)
        return result
    }

    private func beginInterruption() {
        NotificationCenter.default.post(name: AVAudioSession.interruptionNotification,
            object: AVAudioSession.sharedInstance(),
            userInfo: [AVAudioSessionInterruptionTypeKey: AVAudioSession.InterruptionType.began.rawValue])
    }

    private func endInterruption(resume: Bool) {
        NotificationCenter.default.post(name: AVAudioSession.interruptionNotification,
            object: AVAudioSession.sharedInstance(), userInfo: [
                AVAudioSessionInterruptionTypeKey: AVAudioSession.InterruptionType.ended.rawValue,
                AVAudioSessionInterruptionOptionKey: resume ? AVAudioSession.InterruptionOptions.shouldResume.rawValue : 0,
            ])
        _ = status() // Serial controller barrier after the posted notification.
    }

    func testAudioStatusLayoutAndValidatedControls() throws {
        XCTAssertEqual(MemoryLayout<OHHostAudioTransportV1>.size, 160)
        XCTAssertEqual(MemoryLayout<OHNativeAudioStatusV1>.size, 256)
        XCTAssertEqual(MemoryLayout<OHHostAudioTransportV1>.alignment, 8)
        XCTAssertEqual(MemoryLayout<OHNativeAudioStatusV1>.alignment, 8)
        let profiles: [(UInt32, UInt32)] = [(0, 2048), (1, 512), (2, 4096)]
        for (profile, target) in profiles {
            XCTAssertEqual(OHNativeAudioSetControlsV1(0.375, 1, profile), 0)
            let observed = status()
            XCTAssertEqual(observed.profile, profile)
            XCTAssertEqual(observed.transport.profile, profile)
            XCTAssertEqual(observed.transport.target_frames, target)
            XCTAssertEqual(observed.transport.gain_milli, 375)
            XCTAssertEqual(observed.transport.muted, 1)
        }
        let before = status()
        let rejected: [(Float, UInt32, UInt32)] = [(.nan, 0, 0), (-0.1, 0, 0), (1.1, 0, 0), (0.5, 2, 0), (0.5, 0, 3)]
        for (gain, mute, profile) in rejected {
            XCTAssertNotEqual(OHNativeAudioSetControlsV1(gain, mute, profile), 0)
        }
        let after = status()
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
        var untouched = OHNativeAudioStatusV1(); untouched.version = 55
        XCTAssertEqual(OHNativeAudioReadStatusV1(&untouched, 255), -1)
        XCTAssertEqual(untouched.version, 55)
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

    func testAudioStartStopUsesPlaybackAndReportsActualHardware() {
        XCTAssertEqual(OHNativeAudioStartV1(), 0)
        let started = status()
        XCTAssertEqual(started.state, 3); XCTAssertEqual(started.engine_running, 1)
        XCTAssertEqual(started.user_wants_playback, 1); XCTAssertEqual(started.transport.running, 1)
        XCTAssertTrue((8_000...384_000).contains(started.actual_sample_rate_hz))
        XCTAssertGreaterThan(started.io_buffer_duration_us, 0)
        XCTAssertGreaterThan(started.output_channels, 0)
        XCTAssertTrue((1...5).contains(started.route_kind))
        XCTAssertEqual(AVAudioSession.sharedInstance().category, .playback)
        XCTAssertNil(Bundle.main.object(forInfoDictionaryKey: "NSMicrophoneUsageDescription"))
        XCTAssertEqual(OHNativeAudioStartV1(), 0)
        XCTAssertEqual(status().engine_restarts, started.engine_restarts)
        XCTAssertEqual(OHNativeAudioStopV1(), 0)
        let stopped = status()
        XCTAssertEqual(stopped.state, 1); XCTAssertEqual(stopped.engine_running, 0)
        XCTAssertEqual(stopped.user_wants_playback, 0); XCTAssertEqual(stopped.transport.running, 0)
        XCTAssertEqual(stopped.render_callbacks_active, 0)
        XCTAssertEqual(stopped.actual_sample_rate_hz, 0)
        XCTAssertGreaterThan(stopped.transport.transport_epoch, started.transport.transport_epoch)
    }

    func testAudioInterruptionRequiresResumePermissionAndUserIntent() {
        XCTAssertEqual(OHNativeAudioStartV1(), 0)
        beginInterruption()
        let paused = status()
        XCTAssertEqual(paused.state, 4); XCTAssertEqual(paused.interrupted, 1)
        XCTAssertEqual(paused.engine_running, 0); XCTAssertEqual(paused.transport.running, 0)
        endInterruption(resume: false)
        XCTAssertEqual(status().resume_required, 1)
        XCTAssertEqual(status().engine_running, 0)
        XCTAssertEqual(OHNativeAudioStartV1(), 0)
        beginInterruption(); _ = status(); endInterruption(resume: true)
        XCTAssertEqual(status().engine_running, 1)
        beginInterruption(); _ = status()
        XCTAssertEqual(OHNativeAudioStopV1(), 0)
        endInterruption(resume: true)
        XCTAssertEqual(status().engine_running, 0)
        XCTAssertEqual(status().user_wants_playback, 0)
    }

    func testAudioOutputDisconnectRequiresExplicitStart() {
        XCTAssertEqual(OHNativeAudioStartV1(), 0)
        NotificationCenter.default.post(name: AVAudioSession.routeChangeNotification,
            object: AVAudioSession.sharedInstance(), userInfo: [
                AVAudioSessionRouteChangeReasonKey: AVAudioSession.RouteChangeReason.oldDeviceUnavailable.rawValue,
            ])
        let paused = status()
        XCTAssertEqual(paused.state, 5); XCTAssertEqual(paused.resume_required, 1)
        XCTAssertEqual(paused.engine_running, 0); XCTAssertEqual(paused.transport.running, 0)
        NotificationCenter.default.post(name: AVAudioSession.routeChangeNotification,
            object: AVAudioSession.sharedInstance(), userInfo: [
                AVAudioSessionRouteChangeReasonKey: AVAudioSession.RouteChangeReason.newDeviceAvailable.rawValue,
            ])
        XCTAssertEqual(status().engine_running, 0)
        XCTAssertEqual(OHNativeAudioStartV1(), 0)
        XCTAssertEqual(status().engine_running, 1); XCTAssertEqual(status().resume_required, 0)
    }

    func testAudioMediaServicesResetRequiresExplicitStart() {
        XCTAssertEqual(OHNativeAudioStartV1(), 0)
        let before = status()
        NotificationCenter.default.post(name: AVAudioSession.mediaServicesWereLostNotification,
            object: AVAudioSession.sharedInstance())
        XCTAssertEqual(status().state, 6)
        XCTAssertNotEqual(OHNativeAudioStartV1(), 0)
        NotificationCenter.default.post(name: AVAudioSession.mediaServicesWereResetNotification,
            object: AVAudioSession.sharedInstance())
        let reset = status()
        XCTAssertEqual(reset.state, 6); XCTAssertEqual(reset.resume_required, 1)
        XCTAssertEqual(reset.engine_running, 0); XCTAssertEqual(reset.transport.running, 0)
        XCTAssertEqual(reset.media_resets, before.media_resets + 1)
        XCTAssertEqual(OHNativeAudioStartV1(), 0)
        XCTAssertEqual(status().engine_running, 1)
    }

    func testAudioPullIsPlanarSilenceWithoutClients() {
        let before = status()
        XCTAssertEqual(before.transport.active_clients, 0)
        var left = [Float](repeating: 99, count: 256)
        var right = [Float](repeating: 99, count: 256)
        left.withUnsafeMutableBufferPointer { l in
            right.withUnsafeMutableBufferPointer { r in
                XCTAssertEqual(OHHostAudioPullStereoV1(l.baseAddress, r.baseAddress, 256), 0)
            }
        }
        XCTAssertTrue(left.allSatisfy { $0 == 0 }); XCTAssertTrue(right.allSatisfy { $0 == 0 })
        XCTAssertEqual(status().transport.rendered_frames, before.transport.rendered_frames + 256)
        XCTAssertEqual(status().transport.non_silent_frames, before.transport.non_silent_frames)
    }
}
