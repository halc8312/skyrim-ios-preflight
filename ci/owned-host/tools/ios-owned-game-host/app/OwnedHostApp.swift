import Foundation
import QuartzCore
import SwiftUI
import UIKit

enum OwnedRunMode: Sendable {
    case play
    case diagnostic

    static let diagnosticSeconds: UInt32 = 600

    var maxSeconds: UInt32? {
        switch self {
        case .play: return nil
        case .diagnostic: return Self.diagnosticSeconds
        }
    }

    func start(token: UInt64) -> Int32 {
        if let seconds = maxSeconds { return OHStartV1(token, seconds) }
        return OHPlayV1(token)
    }

    func statusText(elapsedMilliseconds: UInt64) -> String {
        let seconds = elapsedMilliseconds / 1000
        if let limit = maxSeconds { return "動作確認中 · \(seconds) / \(limit)秒" }
        return "プレイ中 · \(seconds)秒"
    }
}

@main
@MainActor
struct OwnedHostApp: App {
    @StateObject private var session = OwnedSession.shared
    @StateObject private var controls = MobileControlsPreferences()
    @StateObject private var device = DevicePreferences()
    @Environment(\.scenePhase) private var scenePhase
    @State private var settingsShown = false
    @State private var inputCancellation: UInt64 = 0
    var body: some Scene {
        WindowGroup {
            ZStack {
                Color.black.ignoresSafeArea()
                // Keep this view in the hierarchy throughout preparation and
                // execution. The model retains this exact layer until exit.
                MetalViewport(onLayer: session.retainSurface)
                    .aspectRatio(16 / 9, contentMode: .fit)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                if session.running {
                    MobileControlsView(settings: controls.settings, editing: controls.editingLayout,
                        suspended: settingsShown || scenePhase != .active, cancellation: inputCancellation,
                        onPad: session.updatePad, onLayout: controls.move)
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                }
                VStack(alignment: .leading, spacing: 10) {
                    HStack(alignment: .top) {
                        VStack(alignment: .leading, spacing: 4) {
                            Text("Skyrim Lab").font(.headline)
                            Text(session.status).font(.subheadline)
                        }
                        Spacer()
                        if session.running {
                            Text("\(session.decodedFPSLabel) · GPU \(session.gpuErrors)")
                                .font(.caption.monospacedDigit())
                        }
                    }
                    Spacer()
                    if !session.attempted {
                        VStack(alignment: .leading, spacing: 8) {
                            Text("ゲームをプレイ")
                                .font(.title2.bold())
                            Text("PCで準備したGameフォルダを、このアプリの書類へコピーしてください。")
                            Text("通常プレイに時間制限はありません。動作の安定性と互換性は検証中です。")
                                .font(.footnote)
                            HStack {
                                Button("ゲームを開始", action: session.beginPlay)
                                    .buttonStyle(.borderedProminent)
                                    .disabled(!session.canBegin)
                                Button("10分間の診断", action: session.beginDiagnostic)
                                    .buttonStyle(.bordered)
                                    .disabled(!session.canBegin)
                                Button("ファイルを再確認", action: session.checkFiles)
                                    .buttonStyle(.bordered)
                                Button { settingsShown = true } label: { Label("設定", systemImage: "slider.horizontal.3") }
                                    .buttonStyle(.bordered)
                            }
                        }
                        .padding(16)
                        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 16))
                    } else if session.failed {
                        Text("アプリを終了し、開き直してから再実行してください。")
                            .font(.footnote)
                            .padding(12)
                            .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 12))
                    }
                }
                .padding(20)
                .foregroundStyle(.white)
                .allowsHitTesting(!session.running)
                if session.running {
                    VStack {
                        Spacer()
                        HStack(spacing: 14) {
                            if controls.editingLayout {
                                Text("ボタンをドラッグ").font(.caption)
                                Button("リセット") { neutralizeInput(); controls.resetLayout() }
                                Button("完了") {
                                    neutralizeInput(); controls.editingLayout = false; controls.save()
                                }
                            } else {
                                Button { neutralizeInput(); settingsShown = true } label: {
                                    Label("設定", systemImage: "slider.horizontal.3")
                                }
                            }
                        }
                        .font(.subheadline.weight(.semibold))
                        .dynamicTypeSize(...DynamicTypeSize.xxxLarge)
                        .lineLimit(1).minimumScaleFactor(0.75)
                        .padding(.horizontal, 14).frame(height: MobileControlsLayout.toolbarHeight)
                        .background(.regularMaterial, in: Capsule())
                        .padding(.bottom, MobileControlsLayout.toolbarBottomPadding)
                    }
                }
            }
            .preferredColorScheme(.dark)
            .onAppear {
                UIApplication.shared.isIdleTimerDisabled = true
                session.updateDeviceSettings(device.settings)
                session.sceneChanged(scenePhase)
            }
            .onChange(of: device.settings) { session.updateDeviceSettings($0) }
            .onChange(of: session.running) { running in
                if !running { neutralizeInput() }
            }
            .onChange(of: scenePhase) { phase in
                if phase != .active { neutralizeInput(); controls.save() }
                session.sceneChanged(phase)
            }
            .onDisappear { neutralizeInput(); controls.save(); device.save(); session.suspendAudio() }
            .sheet(isPresented: $settingsShown, onDismiss: { neutralizeInput() }) {
                DeviceSettingsHub(preferences: device, controls: controls, session: session) {
                    neutralizeInput(); controls.editingLayout = true
                }
            }
        }
    }
    private func neutralizeInput() {
        inputCancellation &+= 1
        session.releaseButtons()
    }
}

@MainActor
final class OwnedSession: ObservableObject {
    static let shared = OwnedSession()
    static let diagnosticSeconds = OwnedRunMode.diagnosticSeconds
    @Published private(set) var status = "ファイルを確認しています"
    @Published private(set) var attempted = false
    @Published private(set) var running = false
    @Published private(set) var failed = false
    @Published private(set) var canBegin = false
    @Published private(set) var frames: UInt64 = 0
    @Published private(set) var gpuErrors: UInt64 = 0
    @Published private(set) var decodedFPSLabel = "— FPS"
    @Published private(set) var presentedFPSLabel = "— FPS"
    @Published private(set) var sourceSizeLabel = "未測定"
    @Published private(set) var outputSizeLabel = "未測定"
    @Published private(set) var graphicsApplyLabel = "起動待ち"
    @Published private(set) var powerLabel = "—"
    @Published private(set) var audioStateLabel = "未起動"
    @Published private(set) var audioRouteLabel = "—"
    @Published private(set) var audioHardwareLabel = "—"
    @Published private(set) var audioFramesLabel = "未受信"
    @Published private(set) var audioSignalLabel = "未検出"
    @Published private(set) var settingsError: String?
    private var deviceSettings = DeviceSettings()
    private var rendererReady = false
    private var appActive = false
    private var pendingInitialAudioStart = false
    private var audioIntent: UInt64 = 0
    private var audioControlsRevision: UInt64 = 0
    private var audioPlaybackWanted = false
    private var graphicsSettingsError: String?
    private var audioControlsError: String?
    private var audioLifecycleError: String?
    private lazy var audioCommands = DeviceAudioCommands { [weak self] update in
        Task { @MainActor in self?.receiveAudio(update) }
    }
    private var fpsSample: (time: Double, decoded: UInt64, presented: UInt64)?
    private var surface: CAMetalLayer?
    private var token: UInt64 = 0
    private var runMode: OwnedRunMode = .play
    private var pad = MobilePadSnapshot.neutral
    private var localFailure: String?
    private var timer: Timer?
    private let game: URL

    private init() {
        let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        game = documents.appendingPathComponent("Game", isDirectory: true)
        do {
            try FileManager.default.createDirectory(at: game, withIntermediateDirectories: true)
            var privateGame = game
            var values = URLResourceValues()
            values.isExcludedFromBackup = true
            try privateGame.setResourceValues(values)
            // Save content has a separate policy from small control preferences.
            // Reject links/files before setting backup metadata; Rust revalidates
            // the complete sandbox path before any content is read or written.
            var saves = documents.appendingPathComponent("OwnedSaves", isDirectory: true)
            let manager = FileManager.default
            if manager.fileExists(atPath: saves.path) {
                let attributes = try manager.attributesOfItem(atPath: saves.path)
                guard attributes[.type] as? FileAttributeType == .typeDirectory else {
                    throw CocoaError(.fileWriteInvalidFileName)
                }
            } else {
                try manager.createDirectory(at: saves, withIntermediateDirectories: false)
            }
            let attributes = try manager.attributesOfItem(atPath: saves.path)
            guard attributes[.type] as? FileAttributeType == .typeDirectory else {
                throw CocoaError(.fileWriteInvalidFileName)
            }
            var saveValues = URLResourceValues()
            saveValues.isExcludedFromBackup = true
            try saves.setResourceValues(saveValues)
        } catch {
            localFailure = "端末内のGame・OwnedSavesフォルダを安全に準備できませんでした"
        }
        checkFiles()
        timer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.poll() }
        }
    }

    func retainSurface(_ layer: CAMetalLayer) {
        if let retained = surface {
            // A process with an attached renderer cannot migrate to a new layer.
            if retained !== layer { return }
        } else {
            surface = layer
        }
        DispatchQueue.main.async { [weak self] in self?.checkFiles() }
    }

    func checkFiles() {
        guard !attempted else { return }
        if let failure = localFailure { status = failure; canBegin = false; return }
        guard Bundle.main.url(forResource: "OwnedGuestBinding", withExtension: "json") != nil else {
            status = "PCで用意する検証ファイルが未設定です"; canBegin = false; return
        }
        guard FileManager.default.fileExists(atPath: game.appendingPathComponent("default.xex").path) else {
            status = "Game/default.xex が見つかりません"; canBegin = false; return
        }
        status = surface == nil ? "描画画面を準備しています" : "ゲームを開始できます"
        canBegin = surface != nil
    }

    func beginPlay() { begin(.play) }

    func beginDiagnostic() { begin(.diagnostic) }

    private func begin(_ mode: OwnedRunMode) {
        guard canBegin, !attempted, let layer = surface else { return }
        attempted = true; canBegin = false; status = "ファイルとモジュールを照合しています"
        runMode = mode
        let gameBytes = Array(game.path.utf8)
        let layerAddress = UInt(bitPattern: Unmanaged.passUnretained(layer).toOpaque())
        let selectedSettings = deviceSettings.normalized()
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            do {
                let caches = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
                let runs = caches.appendingPathComponent("OwnedRuns", isDirectory: true)
                try FileManager.default.createDirectory(at: runs, withIntermediateDirectories: true)
                // Native creates this fresh leaf; do not precreate or reuse it.
                let content = runs.appendingPathComponent(UUID().uuidString, isDirectory: true)
                let contentBytes = Array(content.path.utf8)
                var generatedToken: UInt64 = 0
                let prepared = gameBytes.withUnsafeBufferPointer { gameBuffer in
                    contentBytes.withUnsafeBufferPointer { contentBuffer in
                        OHPrepareV1(gameBuffer.baseAddress, gameBuffer.count,
                                    contentBuffer.baseAddress, contentBuffer.count, 1, &generatedToken)
                    }
                }
                guard prepared == 0, generatedToken != 0 else {
                    // Token zero is explicitly supported for status reads.
                    // Expose only the validated numeric preparation stage.
                    var detail = OHStatusV1()
                    let readable = OHReadStatusV1(0, &detail, MemoryLayout<OHStatusV1>.size)
                    let message: String
                    if readable == 0, detail.version == 0x0001_0000,
                       detail.bytes == 96, detail.state == 6, (101...110).contains(detail.failure) {
                        message = "準備の段階 \(detail.failure - 100) で失敗しました（コード \(detail.failure)・\(prepared)）"
                    } else {
                        message = "準備に失敗しました（\(prepared)）"
                    }
                    Task { @MainActor in self?.stop(message) }; return
                }
                let preparedToken = generatedToken
                Task { @MainActor in self?.token = preparedToken; self?.status = "Metal描画を準備しています" }
                var boot = selectedSettings.graphics.boot
                let configured = OHConfigureBootGraphicsV1(preparedToken, &boot, MemoryLayout<OHBootGraphicsV1>.size)
                guard configured == 0 else {
                    Task { @MainActor in self?.stop("起動時の画質設定を適用できませんでした（\(configured)）") }; return
                }
                let attached = OHAttachMetalLayerV1(preparedToken,
                    UnsafeMutableRawPointer(bitPattern: layerAddress), 1280, 720)
                guard attached == 0 else {
                    Task { @MainActor in self?.stop("描画の初期化に失敗しました（\(attached)）") }; return
                }
                var presentation = selectedSettings.graphics.presentation
                let applied = OHSetPresentationSettingsV1(preparedToken, &presentation, MemoryLayout<OHPresentationSettingsV1>.size)
                guard applied == 0 else {
                    Task { @MainActor in self?.stop("出力設定を適用できませんでした（\(applied)）") }; return
                }
                let started = mode.start(token: preparedToken)
                guard started == 0 else {
                    Task { @MainActor in self?.stop("実行を開始できませんでした（\(started)）") }; return
                }
                Task { @MainActor in
                    guard let self, self.localFailure == nil, !self.failed else { return }
                    self.rendererReady = true; self.status = mode.statusText(elapsedMilliseconds: 0); self.running = true
                    // Apply edits made while preparation was running. Boot layout
                    // stays frozen to the captured tuple for this process.
                    self.updateDeviceSettings(self.deviceSettings)
                    self.pendingInitialAudioStart = true
                    self.startInitialAudioIfActive()
                }
            } catch {
                Task { @MainActor in self?.stop("実行用の保存先を準備できませんでした") }
            }
        }
    }

    private func stop(_ message: String) {
        guard localFailure == nil else { return }
        localFailure = message; status = message; failed = true; running = false
        pendingInitialAudioStart = false
        audioPlaybackWanted = false
        audioIntent = audioCommands.setPlaybackIntent(false)
        releaseButtons()
        if token != 0 { _ = OHAbortFailedSessionV1(token) }
    }

    private func poll() {
        updateDeviceStatus()
        guard attempted, token != 0, localFailure == nil else { return }
        var value = OHStatusV1()
        let code = OHReadStatusV1(token, &value, MemoryLayout<OHStatusV1>.size)
        guard code == 0, value.version == 0x0001_0000, value.bytes == 96 else {
            stop("状態を確認できませんでした（\(code)）"); return
        }
        switch value.state {
        case 1: status = "ファイルとモジュールを照合しています"
        case 2, 3: status = "Metal描画と起動を準備しています"
        case 4: running = true; status = runMode.statusText(elapsedMilliseconds: value.elapsed_ms)
        case 5:
            // A kernel exit notification does not establish
            // that its other guest workers or renderer have stopped. Keep
            // input available while other workers may remain active when code is 0.
            if value.exit_code == 0 {
                running = true
                status = runMode.statusText(elapsedMilliseconds: value.elapsed_ms)
            } else {
                stop("ゲームの終了コード \(value.exit_code) を検出しました")
            }
        case 6: stop("実行エラーを検出しました（コード \(value.failure)）")
        default: break
        }
        if token != 0 {
            var render = OHRendererStatusV1()
            if OHReadRendererStatusV1(token, &render, MemoryLayout<OHRendererStatusV1>.size) == 0 {
                frames = render.decoded_frames; gpuErrors = render.gpu_errors
                if render.failed != 0 { stop("描画エラーを検出しました。アプリを終了し、開き直してください") }
            }
        }
    }

    func updateDeviceSettings(_ value: DeviceSettings) {
        deviceSettings = value.normalized()
        guard rendererReady, token != 0 else { return }
        var live = deviceSettings.graphics.presentation
        let code = OHSetPresentationSettingsV1(token, &live, MemoryLayout<OHPresentationSettingsV1>.size)
        graphicsSettingsError = code != 0 ? "出力設定を反映できませんでした（\(code)）" : nil
        audioControlsRevision = audioCommands.setControls(deviceSettings.audio)
        settingsError = graphicsSettingsError ?? audioControlsError ?? audioLifecycleError
    }
    func resumeAudio() {
        guard running, appActive else { return }
        audioPlaybackWanted = true
        audioLifecycleError = nil
        settingsError = graphicsSettingsError ?? audioControlsError
        audioStateLabel = "準備中"
        audioIntent = audioCommands.setPlaybackIntent(true)
    }
    func sceneChanged(_ phase: ScenePhase) {
        appActive = phase == .active
        if phase == .background { suspendAudio() }
        if appActive { startInitialAudioIfActive() }
    }
    private func startInitialAudioIfActive() {
        guard appActive, running, pendingInitialAudioStart else { return }
        pendingInitialAudioStart = false
        resumeAudio()
    }
    func suspendAudio() {
        appActive = false
        if rendererReady {
            audioPlaybackWanted = false
            audioIntent = audioCommands.setPlaybackIntent(false)
        }
    }
    private func updateDeviceStatus() {
        let process = ProcessInfo.processInfo
        let thermal: String
        switch process.thermalState {
        case .nominal: thermal = "温度は正常"; case .fair: thermal = "温かい"
        case .serious: thermal = "高温"; case .critical: thermal = "非常に高温"
        @unknown default: thermal = "温度不明"
        }
        powerLabel = thermal + (process.isLowPowerModeEnabled ? " · 低電力モード" : "")
        if rendererReady, token != 0 {
            var status = OHGraphicsStatusV1()
            if OHReadGraphicsStatusV1(token, &status, MemoryLayout<OHGraphicsStatusV1>.size) == 0,
               status.version == 0x0001_0000, status.struct_bytes == 120 {
                let now = CACurrentMediaTime()
                if let previous = fpsSample, now > previous.time,
                   status.decoded_frames >= previous.decoded, status.presented_frames >= previous.presented {
                    let elapsed = now - previous.time
                    decodedFPSLabel = String(format: "%.1f FPS", Double(status.decoded_frames - previous.decoded) / elapsed)
                    presentedFPSLabel = String(format: "%.1f FPS", Double(status.presented_frames - previous.presented) / elapsed)
                }
                fpsSample = (now, status.decoded_frames, status.presented_frames)
                sourceSizeLabel = status.source_width == 0 ? "未測定" : "\(status.source_width) × \(status.source_height)"
                outputSizeLabel = status.output_width == 0 ? "未測定" : "\(status.output_width) × \(status.output_height)"
                graphicsApplyLabel = status.requested_generation == status.applied_generation ? "描画側で受信済み" : "次のフレームを待機中"
            }
        }
        if rendererReady { audioCommands.requestStatus() }
    }

    private func receiveAudio(_ update: DeviceAudioUpdate) {
        // The worker may finish after backgrounding, failure, a new resume, or
        // a settings edit. It must never publish a stale success/error as current.
        guard update.intent == audioIntent, update.controlsRevision == audioControlsRevision else { return }
        if let code = update.controlsCode {
            audioControlsError = code == 0 ? nil : "音声設定を反映できませんでした（\(code)）"
        }
        if let code = update.intentCode, code != 0, code != -4 {
            audioLifecycleError = audioPlaybackWanted ? "音声の再開に失敗しました（\(code)）" :
                "音声の停止処理を確認できませんでした（\(code)）"
        }
        settingsError = graphicsSettingsError ?? audioControlsError ?? audioLifecycleError
        if update.statusCode == 0, let audio = update.status,
           audio.version == 1, audio.struct_size == 256 {
            if audio.resume_required != 0 { audioStateLabel = "再開操作を待機中" }
            else {
                switch audio.state {
                case 0: audioStateLabel = "未起動"; case 1: audioStateLabel = "停止中"
                case 2: audioStateLabel = "準備中"; case 3: audioStateLabel = "出力中"
                case 4: audioStateLabel = "割り込み中"; case 5: audioStateLabel = "接続先の変更で停止"
                case 6: audioStateLabel = "音声サービスの再起動を検出"
                default: audioStateLabel = "出力エラー（\(audio.last_error)）"
                }
            }
            switch audio.route_kind {
            case 1: audioRouteLabel = "iPhone・iPadのスピーカー"; case 2: audioRouteLabel = "有線イヤホン・外部出力"
            case 3: audioRouteLabel = "Bluetooth"; case 4: audioRouteLabel = "AirPlay"
            case 5: audioRouteLabel = "その他"; default: audioRouteLabel = "—"
            }
            audioHardwareLabel = audio.actual_sample_rate_hz == 0 ? "未測定" : "\(audio.actual_sample_rate_hz) Hz · \(audio.output_channels) ch · \(audio.io_buffer_duration_us / 1000) ms"
            audioFramesLabel = audio.transport.submitted_frames == 0 ? "未受信（復号は未確認）" : "\(audio.transport.submitted_frames) サンプルフレーム受信"
            audioSignalLabel = audio.transport.non_silent_frames == 0 ? "非ゼロの信号は未検出" : "\(audio.transport.non_silent_frames) サンプルフレームに信号あり"
        } else if update.statusCode != 0 {
            audioStateLabel = "音声の状態を確認できませんでした（\(update.statusCode)）"
        }
    }

    func updatePad(_ snapshot: MobilePadSnapshot) {
        guard running, token != 0 else { return }
        pad = snapshot
        let code = OHUpdatePadV1(token, pad.buttons, pad.leftTrigger, pad.rightTrigger, pad.lx, pad.ly, pad.rx, pad.ry)
        if code != 0 { stop("操作入力を更新できませんでした（\(code)）") }
    }
    func releaseButtons() {
        pad = .neutral
        if token != 0 { _ = OHUpdatePadV1(token, 0, 0, 0, 0, 0, 0, 0) }
    }
}
