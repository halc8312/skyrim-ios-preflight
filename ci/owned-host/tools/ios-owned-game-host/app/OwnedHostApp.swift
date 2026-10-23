import Foundation
import QuartzCore
import SwiftUI
import UIKit

@main
@MainActor
struct OwnedHostApp: App {
    @StateObject private var session = OwnedSession.shared
    @StateObject private var controls = MobileControlsPreferences()
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
                            Text("\(session.frames) frames · GPU \(session.gpuErrors)")
                                .font(.caption.monospacedDigit())
                        }
                    }
                    Spacer()
                    if !session.attempted {
                        VStack(alignment: .leading, spacing: 8) {
                            Text("起動診断")
                                .font(.title2.bold())
                            Text("PCで準備したGameフォルダを、このアプリの書類へコピーしてください。")
                            Text("最大10分でアプリが終了します。本編の動作と互換性は検証中です。")
                                .font(.footnote)
                            HStack {
                                Button("10分間の動作確認を開始", action: session.beginDiagnostic)
                                    .buttonStyle(.borderedProminent)
                                    .disabled(!session.canBegin)
                                Button("ファイルを再確認", action: session.checkFiles)
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
                                    Label("操作設定", systemImage: "slider.horizontal.3")
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
            .onAppear { UIApplication.shared.isIdleTimerDisabled = true }
            .onChange(of: session.running) { running in
                if !running { neutralizeInput() }
            }
            .onChange(of: scenePhase) { phase in
                if phase != .active { neutralizeInput(); controls.save() }
            }
            .onDisappear { neutralizeInput(); controls.save() }
            .sheet(isPresented: $settingsShown, onDismiss: { neutralizeInput() }) {
                MobileControlsSettingsView(preferences: controls) {
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
    static let diagnosticSeconds: UInt32 = 600
    @Published private(set) var status = "ファイルを確認しています"
    @Published private(set) var attempted = false
    @Published private(set) var running = false
    @Published private(set) var failed = false
    @Published private(set) var canBegin = false
    @Published private(set) var frames: UInt64 = 0
    @Published private(set) var gpuErrors: UInt64 = 0
    private var surface: CAMetalLayer?
    private var token: UInt64 = 0
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
        status = surface == nil ? "描画画面を準備しています" : "起動診断を開始できます"
        canBegin = surface != nil
    }

    func beginDiagnostic() {
        guard canBegin, !attempted, let layer = surface else { return }
        attempted = true; canBegin = false; status = "ファイルとモジュールを照合しています"
        let gameBytes = Array(game.path.utf8)
        let layerAddress = UInt(bitPattern: Unmanaged.passUnretained(layer).toOpaque())
        let maxSeconds = Self.diagnosticSeconds
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
                    Task { @MainActor in self?.stop("準備に失敗しました（\(prepared)）") }; return
                }
                let preparedToken = generatedToken
                Task { @MainActor in self?.token = preparedToken; self?.status = "Metal描画を準備しています" }
                let attached = OHAttachMetalLayerV1(preparedToken,
                    UnsafeMutableRawPointer(bitPattern: layerAddress), 1280, 720)
                guard attached == 0 else {
                    Task { @MainActor in self?.stop("描画の初期化に失敗しました（\(attached)）") }; return
                }
                let started = OHStartV1(preparedToken, maxSeconds)
                guard started == 0 else {
                    Task { @MainActor in self?.stop("起動診断を開始できませんでした（\(started)）") }; return
                }
                Task { @MainActor in self?.status = "起動診断中"; self?.running = true }
            } catch {
                Task { @MainActor in self?.stop("診断用の保存先を準備できませんでした") }
            }
        }
    }

    private func stop(_ message: String) {
        localFailure = message; status = message; failed = true; running = false
        releaseButtons()
    }

    private func poll() {
        guard attempted, localFailure == nil else { return }
        var value = OHStatusV1()
        let code = OHReadStatusV1(token, &value, MemoryLayout<OHStatusV1>.size)
        guard code == 0, value.version == 0x0001_0000, value.bytes == 96 else {
            stop("状態を確認できませんでした（\(code)）"); return
        }
        switch value.state {
        case 1: status = "ファイルとモジュールを照合しています"
        case 2, 3: status = "Metal描画と起動を準備しています"
        case 4: running = true; status = "動作確認中 · \(value.elapsed_ms / 1000) / \(Self.diagnosticSeconds)秒"
        case 5:
            // A kernel exit notification does not establish
            // that its other guest workers or renderer have stopped. Keep
            // input available during this bounded diagnostic when code is 0.
            if value.exit_code == 0 {
                running = true
                status = "動作確認中 · \(value.elapsed_ms / 1000) / \(Self.diagnosticSeconds)秒"
            } else {
                stop("ゲームの終了コード \(value.exit_code) を検出しました")
            }
        case 6: stop("診断を停止しました（コード \(value.failure)）")
        default: break
        }
        if token != 0 {
            var render = OHRendererStatusV1()
            if OHReadRendererStatusV1(token, &render, MemoryLayout<OHRendererStatusV1>.size) == 0 {
                frames = render.decoded_frames; gpuErrors = render.gpu_errors
                if render.failed != 0 { stop("描画エラーを検出しました。時間上限でアプリが終了します") }
            }
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
