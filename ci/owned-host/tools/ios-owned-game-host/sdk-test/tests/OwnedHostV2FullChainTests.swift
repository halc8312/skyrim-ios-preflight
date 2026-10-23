#if PUBLIC_V2_SDK_FIXTURE
import CryptoKit
import Darwin
import Foundation
import Metal
import QuartzCore
import UIKit
import XCTest
@testable import OwnedGameHost

@MainActor
private enum V2SurfaceOwner {
    // Never release a borrowed renderer layer after the test method returns.
    static var window: UIWindow?
    static var layer: CAMetalLayer?
}

private func v2OnWorker<T>(_ operation: @escaping () -> T) async -> T {
    await withCheckedContinuation { continuation in
        DispatchQueue.global(qos: .userInitiated).async {
            continuation.resume(returning: operation())
        }
    }
}

private enum V2LaunchMode {
    case diagnostic, normalPlay

    // The returned flag records the branch that actually invoked the C API.
    // A successful normal result may never be synthesized from OHStartV1.
    func start(_ token: UInt64) -> (code: Int32, normalPlayAPIInvoked: Bool) {
        switch self {
        case .diagnostic: return (OHStartV1(token, 30), false)
        case .normalPlay: return (OHPlayV1(token), true)
        }
    }
}

// Fixed phase boundaries only; they never substitute for the verified proof.
private func v2Stage(_ stage: UInt32) {
    print("OWNED_HOST_PUBLIC_V2_STAGE \(stage)")
    fflush(stdout)
}

// The public SDK fixture records the actual Objective-C layer setters invoked
// by WgpuNative. It observes worker startup without changing production classes.
private final class V2MetalLayerThreadAudit {
    private let lock = NSLock()
    private var setters: UInt32 = 0
    private var offMain: UInt32 = 0
    private var configurations: UInt32 = 0
    func reset() {
        lock.lock(); defer { lock.unlock() }
        setters = 0; offMain = 0; configurations = 0
    }
    func record(_ bit: UInt32) {
        let main = Thread.isMainThread
        lock.lock(); defer { lock.unlock() }
        setters |= bit
        if !main { offMain += 1 }
        if bit == 64 { configurations += 1 }
    }
    func snapshot() -> (setters: UInt32, offMain: UInt32, configurations: UInt32) {
        lock.lock(); defer { lock.unlock() }
        return (setters, offMain, configurations)
    }
}

private final class V2ThreadCheckedMetalLayer: CAMetalLayer {
    static let audit = V2MetalLayerThreadAudit()
    override var device: (any MTLDevice)? {
        get { super.device }
        set { Self.audit.record(1); super.device = newValue }
    }
    override var pixelFormat: MTLPixelFormat {
        get { super.pixelFormat }
        set { Self.audit.record(2); super.pixelFormat = newValue }
    }
    override var framebufferOnly: Bool {
        get { super.framebufferOnly }
        set { Self.audit.record(4); super.framebufferOnly = newValue }
    }
    override var isOpaque: Bool {
        get { super.isOpaque }
        set { Self.audit.record(8); super.isOpaque = newValue }
    }
    override var maximumDrawableCount: Int {
        get { super.maximumDrawableCount }
        set { Self.audit.record(16); super.maximumDrawableCount = newValue }
    }
    override var drawableSize: CGSize {
        get { super.drawableSize }
        set { Self.audit.record(32); super.drawableSize = newValue }
    }
    override var allowsNextDrawableTimeout: Bool {
        get { super.allowsNextDrawableTimeout }
        set { Self.audit.record(64); super.allowsNextDrawableTimeout = newValue }
    }
    override var wantsExtendedDynamicRangeContent: Bool {
        get { super.wantsExtendedDynamicRangeContent }
        set { Self.audit.record(128); super.wantsExtendedDynamicRangeContent = newValue }
    }
}

@MainActor
private final class V2ThreadCheckedMetalView: UIView {
    override class var layerClass: AnyClass { V2ThreadCheckedMetalLayer.self }
}

final class OwnedHostV2FullChainTests: XCTestCase {
    @MainActor
    func testPublicOwnedHostV2FullChain() async throws {
        try await runPublicFixture(mode: .diagnostic)
    }

    @MainActor
    func testPublicOwnedHostV2NormalPlay() async throws {
        try await runPublicFixture(mode: .normalPlay)
    }

    @MainActor
    private func runPublicFixture(mode: V2LaunchMode) async throws {
        let normalPlay = mode == .normalPlay
        executionTimeAllowance = 40
        // An FFI/SDK hang must terminate this isolated process, not leave a
        // partially initialized global Kernel available to a second attempt.
        let watchdog = DispatchSource.makeTimerSource(queue: .global(qos: .userInitiated))
        watchdog.schedule(deadline: .now() + 35)
        watchdog.setEventHandler { fatalError("Public owned-host probe exceeded 35 seconds") }
        watchdog.resume()
        defer { watchdog.cancel() }

        XCTAssertEqual(Bundle.main.bundleIdentifier, "com.halc8312.ownedgamehost.v2fixture")
        XCTAssertEqual(MemoryLayout<OHStatusV1>.size, 96)
        XCTAssertEqual(MemoryLayout<OHRendererStatusV1>.size, 56)
        guard Bundle.main.bundleIdentifier == "com.halc8312.ownedgamehost.v2fixture",
              MemoryLayout<OHStatusV1>.size == 96,
              MemoryLayout<OHRendererStatusV1>.size == 56 else { return }

        let bindingURL = try XCTUnwrap(Bundle.main.url(forResource: "OwnedGuestBinding", withExtension: "json"))
        let bindingBytes = try Data(contentsOf: bindingURL)
        let binding = try XCTUnwrap(JSONSerialization.jsonObject(with: bindingBytes) as? [String: Any])
        XCTAssertEqual(binding["abi_version"] as? UInt32, 0x20000)
        guard binding["abi_version"] as? UInt32 == 0x20000 else { return }
        let unsignedSHA = try XCTUnwrap(binding["unsigned_framework_sha256"] as? String)
        let xexSHA = try XCTUnwrap(binding["xex_sha256"] as? String)
        XCTAssertEqual(unsignedSHA.count, 64)
        XCTAssertEqual(xexSHA.count, 64)
        let source = try XCTUnwrap(Bundle.main.url(forResource: "default", withExtension: "xex", subdirectory: "PublicFixture"))
        let xex = try Data(contentsOf: source)
        XCTAssertEqual(SHA256.hash(data: xex).map { String(format: "%02x", $0) }.joined(), xexSHA)
        let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        // Each selector runs in a new process. Distinct fixed leaves preserve
        // the first mode's authored input/evidence when the bundle is reused.
        let game = documents.appendingPathComponent(normalPlay ? "GameNormalPlay" : "Game", isDirectory: true)
        let content = documents.appendingPathComponent(normalPlay ? "PublicOwnedContentNormalPlay" : "PublicOwnedContent", isDirectory: true)
        let input = game.appendingPathComponent("default.xex")
        XCTAssertFalse(FileManager.default.fileExists(atPath: input.path))
        XCTAssertFalse(FileManager.default.fileExists(atPath: content.path))
        guard !FileManager.default.fileExists(atPath: input.path),
              !FileManager.default.fileExists(atPath: content.path) else { return }
        try FileManager.default.createDirectory(at: game, withIntermediateDirectories: true)
        try xex.write(to: input, options: .withoutOverwriting)
        var saves = documents.appendingPathComponent("OwnedSaves", isDirectory: true)
        if !FileManager.default.fileExists(atPath: saves.path) {
            try FileManager.default.createDirectory(at: saves, withIntermediateDirectories: false)
        }
        let saveAttributes = try FileManager.default.attributesOfItem(atPath: saves.path)
        XCTAssertEqual(saveAttributes[.type] as? FileAttributeType, .typeDirectory)
        guard saveAttributes[.type] as? FileAttributeType == .typeDirectory else { return }
        var saveValues = URLResourceValues()
        saveValues.isExcludedFromBackup = true
        try saves.setResourceValues(saveValues)

        let gameBytes = Array(game.path.utf8)
        let contentBytes = Array(content.path.utf8)
        v2Stage(10) // Preparation entered.
        let prepared = await v2OnWorker { () -> (Int32, UInt64) in
            var token: UInt64 = 0
            let status = gameBytes.withUnsafeBufferPointer { gamePointer in
                contentBytes.withUnsafeBufferPointer { contentPointer in
                    OHPrepareV1(gamePointer.baseAddress, gamePointer.count,
                                contentPointer.baseAddress, contentPointer.count, 0, &token)
                }
            }
            return (status, token)
        }
        v2Stage(11) // Preparation returned.
        XCTAssertEqual(prepared.0, 0)
        XCTAssertNotEqual(prepared.1, 0)
        guard prepared.0 == 0, prepared.1 != 0 else { return }
        let token = prepared.1
        var status = OHStatusV1()
        XCTAssertEqual(OHReadStatusV1(token, &status, MemoryLayout<OHStatusV1>.size), 0)
        XCTAssertEqual(status.state, 2)
        // Starting before a renderer is attached must preserve PREPARED.
        let premature = await v2OnWorker { mode.start(token).code }
        XCTAssertNotEqual(premature, 0)
        XCTAssertEqual(OHReadStatusV1(token, &status, MemoryLayout<OHStatusV1>.size), 0)
        XCTAssertEqual(status.state, 2)
        guard premature != 0, status.state == 2 else { return }

        let scene = try XCTUnwrap(UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.first)
        let viewport = V2ThreadCheckedMetalView(frame: CGRect(x: 0, y: 0, width: 320, height: 180))
        viewport.isOpaque = true
        viewport.backgroundColor = .black
        viewport.contentScaleFactor = 1
        let controller = UIViewController()
        controller.view = viewport
        let window = UIWindow(windowScene: scene)
        window.frame = viewport.frame
        window.rootViewController = controller
        window.makeKeyAndVisible()
        let layer = try XCTUnwrap(viewport.layer as? V2ThreadCheckedMetalLayer)
        V2SurfaceOwner.window = window
        V2SurfaceOwner.layer = layer
        let address = UInt(bitPattern: Unmanaged.passUnretained(layer).toOpaque())
        // Refusal must happen before the one-shot state changes. A subsequent
        // worker attachment must still succeed with the main queue available.
        let mainRefused = OHAttachMetalLayerV1(token, UnsafeMutableRawPointer(bitPattern: address), 1280, 720)
        XCTAssertEqual(mainRefused, -9)
        guard mainRefused == -9 else { return }
        V2ThreadCheckedMetalLayer.audit.reset()
        v2Stage(20) // Renderer attachment entered.
        let attached = await v2OnWorker {
            OHAttachMetalLayerV1(token, UnsafeMutableRawPointer(bitPattern: address), 1280, 720)
        }
        v2Stage(21) // Renderer attachment returned.
        XCTAssertEqual(attached, 0)
        guard attached == 0 else { return }
        let layerAudit = V2ThreadCheckedMetalLayer.audit.snapshot()
        let requiredSettersObserved = layerAudit.setters & 127 == 127 && layerAudit.configurations >= 1
        XCTAssertTrue(requiredSettersObserved)
        XCTAssertEqual(layerAudit.offMain, 0)
        guard requiredSettersObserved, layerAudit.offMain == 0 else { return }
        var renderer = OHRendererStatusV1()
        XCTAssertEqual(OHReadRendererStatusV1(token, &renderer, MemoryLayout<OHRendererStatusV1>.size), 0)
        XCTAssertEqual(renderer.attachment, 2)
        XCTAssertEqual(renderer.initialized, 1)
        XCTAssertEqual(renderer.failed, 0)
        XCTAssertEqual(renderer.gpu_errors, 0)
        guard renderer.attachment == 2, renderer.initialized == 1,
              renderer.failed == 0, renderer.gpu_errors == 0 else { return }
        v2Stage(30) // Guest entry requested.
        let entry = await v2OnWorker { mode.start(token) }
        let started = entry.code
        v2Stage(31) // Guest entry request returned.
        XCTAssertEqual(started, 0)
        XCTAssertEqual(entry.normalPlayAPIInvoked, normalPlay)
        guard started == 0, entry.normalPlayAPIInvoked == normalPlay else { return }
        v2Stage(40) // Exit wait entered.
        let deadline = ProcessInfo.processInfo.systemUptime + 3
        repeat {
            XCTAssertEqual(OHReadStatusV1(token, &status, MemoryLayout<OHStatusV1>.size), 0)
            if status.state == 5 || status.state == 6 { break }
            try await Task.sleep(nanoseconds: 20_000_000)
        } while ProcessInfo.processInfo.systemUptime < deadline
        v2Stage(41) // Exit wait returned.
        XCTAssertEqual(status.state, 5)
        XCTAssertEqual(status.policy, 0)
        XCTAssertEqual(status.failure, 0)
        XCTAssertEqual(status.missing_module, 0)
        XCTAssertEqual(status.missing_ordinal, 0)
        XCTAssertEqual(status.exit_code, 0)
        XCTAssertEqual(status.function_imports, 2)
        XCTAssertEqual(status.registered_functions, 2)
        XCTAssertEqual(status.variable_imports, 0)
        XCTAssertEqual(status.supported_variables, 0)
        XCTAssertEqual(status.function_count, 3)
        XCTAssertEqual(status.ready_to_play, 0)
        XCTAssertEqual(status.signature_authenticated, 0)
        XCTAssertEqual(status.iphone_game_verified, 0)
        XCTAssertEqual(OHReadRendererStatusV1(token, &renderer, MemoryLayout<OHRendererStatusV1>.size), 0)
        XCTAssertEqual(renderer.initialized, 1)
        XCTAssertEqual(renderer.failed, 0)
        XCTAssertEqual(renderer.gpu_errors, 0)
        guard status.state == 5, status.policy == 0, status.failure == 0,
              status.missing_module == 0, status.missing_ordinal == 0, status.exit_code == 0,
              status.function_imports == 2, status.registered_functions == 2,
              status.variable_imports == 0, status.supported_variables == 0,
              status.function_count == 3, status.ready_to_play == 0,
              status.signature_authenticated == 0, status.iphone_game_verified == 0,
              renderer.initialized == 1, renderer.failed == 0, renderer.gpu_errors == 0 else { return }
        // Both entry APIs remain one-shot after this fixture has exited.
        let repeated = await v2OnWorker { mode.start(token).code }
        XCTAssertNotEqual(repeated, 0)
        guard repeated != 0 else { return }
        v2Stage(50) // Fixed readback entered.
        XCTAssertEqual(MemoryLayout<OHPublicV2ProofV1>.size, 64)
        // Fixed readback refuses bad sizes/tokens before changing any output.
        var refused = OHPublicV2ProofV1()
        refused.version = 0xabcdef01
        XCTAssertEqual(OHReadPublicV2ProofV1(token, &refused, 63), -1)
        XCTAssertEqual(refused.version, 0xabcdef01)
        XCTAssertEqual(OHReadPublicV2ProofV1(0, &refused, 64), -2)
        XCTAssertEqual(refused.version, 0xabcdef01)
        let checked = await v2OnWorker { () -> (Int32, OHPublicV2ProofV1) in
            var proof = OHPublicV2ProofV1()
            let code = OHReadPublicV2ProofV1(token, &proof, 64)
            return (code, proof)
        }
        v2Stage(51) // Fixed readback returned.
        XCTAssertEqual(checked.0, 0)
        let p = checked.1
        let observed: [String: UInt32] = [
            "version": p.version, "bytes": p.bytes, "abi_version": p.abi_version,
            "layout_words": p.layout_words, "host_bytes": p.host_bytes,
            "requirements_bytes": p.requirements_bytes, "provider_refusals": p.provider_refusals,
            "provider_preserved": p.provider_preserved, "frequency": p.frequency,
            "cleanup_count": p.cleanup_count, "jump_result": p.jump_result,
            "normal_result": p.normal_result, "mmio_result": p.mmio_result,
            "mmio_lock_word": p.mmio_lock_word, "imports_once": p.imports_once,
            "republish_ok": p.republish_ok,
        ]
        let expected: [String: UInt32] = [
            "version": 0x10000, "bytes": 64, "abi_version": 0x20000,
            "layout_words": 51, "host_bytes": 416, "requirements_bytes": 216,
            "provider_refusals": 10, "provider_preserved": 1, "frequency": 50_000_000,
            "cleanup_count": 1, "jump_result": 0x1111, "normal_result": 0x2222,
            "mmio_result": 1, "mmio_lock_word": 0x80000001, "imports_once": 2, "republish_ok": 1,
        ]
        XCTAssertEqual(observed, expected)
        guard checked.0 == 0, observed == expected else { return }
        var report: [String: Any] = [
            "kind": normalPlay ? "public_owned_host_v2_normal_play_xctest" : "public_owned_host_v2_fullchain_xctest", "passed": true,
            "state": status.state, "policy": status.policy, "exit_code": status.exit_code,
            "function_imports": status.function_imports, "registered_functions": status.registered_functions,
            "function_count": status.function_count, "renderer_initialized": renderer.initialized,
            "gpu_errors": renderer.gpu_errors, "decoded_frames": renderer.decoded_frames,
            "unsigned_fixture_sha256": unsignedSHA, "public_xex_sha256": xexSHA,
            "trusted_binding_sha256": SHA256.hash(data: bindingBytes).map { String(format: "%02x", $0) }.joined(),
            "separate_fresh_process": true, "public_fixture_only": true,
            "game_executed": false, "physical_iphone_verified": false, "ready_to_play": false,
            "signature_authority_authenticated": false, "private_guest_framework_verified": false,
            "game_frame_rendered": false, "mapped_inode_attested": false,
            "main_thread_attach_rejected": mainRefused == -9,
            "metal_layer_required_setters_on_main": requiredSettersObserved,
            "metal_layer_off_main_calls": layerAudit.offMain,
            "v2_proof": observed, "audio_output_verified": false, "xma_pcm_verified": false,
        ]
        if normalPlay { report["normal_play_api_invoked"] = entry.normalPlayAPIInvoked }
        let data = try JSONSerialization.data(withJSONObject: report, options: [.sortedKeys])
        let prefix = normalPlay ? "OWNED_HOST_PUBLIC_V2_NORMAL_PLAY_RESULT " : "OWNED_HOST_PUBLIC_V2_RESULT "
        print(prefix + String(decoding: data, as: UTF8.self))
    }
}

#endif
