import SwiftUI

@MainActor
struct DeviceSettingsHub: View {
    @ObservedObject var preferences: DevicePreferences
    @ObservedObject var controls: MobileControlsPreferences
    @ObservedObject var session: OwnedSession
    let editLayout: () -> Void
    var body: some View {
        TabView {
            DeviceGraphicsSettingsView(preferences: preferences, session: session)
                .tabItem { Label("画質", systemImage: "display") }
            DeviceAudioSettingsView(preferences: preferences, session: session)
                .tabItem { Label("音声", systemImage: "speaker.wave.2") }
            MobileControlsSettingsView(preferences: controls, editLayout: editLayout)
                .tabItem { Label("操作", systemImage: "gamecontroller") }
        }
    }
}

@MainActor
struct DeviceGraphicsSettingsView: View {
    @ObservedObject var preferences: DevicePreferences
    @ObservedObject var session: OwnedSession
    @Environment(\.dismiss) private var dismiss
    private var graphics: DeviceGraphicsSettings { preferences.settings.graphics }
    var body: some View {
        NavigationStack {
            Form {
                Section {
                    Picker("プリセット", selection: Binding(get: { graphics.presetLabel }, set: { label in
                        if let preset = DevicePreset.allCases.first(where: { $0.label == label }) { preferences.apply(preset) }
                    })) {
                        if graphics.presetLabel == "カスタム" { Text("カスタム").tag("カスタム") }
                        ForEach(DevicePreset.allCases, id: \.self) { Text($0.label).tag($0.label) }
                    }
                    Text("設定の幅を試す実験版です。最高画質では負荷とメモリ使用量が大きく増えます。動作が不安定なら起動前に従来の設定へ戻せます。")
                        .font(.footnote).foregroundStyle(.secondary)
                }
                Section("描画・次回の起動から") {
                    Picker("要求解像度（試験）", selection: Binding(get: { graphics.videoMode }, set: { value in preferences.graphics { $0.videoMode = value } })) {
                        ForEach(DeviceVideoMode.allCases, id: \.self) { Text($0.label).tag($0) }
                    }
                    Picker("スーパーサンプリング", selection: Binding(get: { graphics.internalScale }, set: { value in preferences.graphics { $0.internalScale = value } })) {
                        Text("1×（標準）").tag(UInt32(1)); Text("2×（4倍の画素数）").tag(UInt32(2))
                        if graphics.videoMode != .p1080 { Text("3×（9倍の画素数）").tag(UInt32(3)) }
                    }
                    Text("タイトル画面では低解像度を要求しても1280×720の出力が続くことを確認しています。この項目による内部描画の軽量化は未確認です。実際の入力サイズは下の状態で確認できます。")
                        .font(.footnote).foregroundStyle(.secondary)
                    if session.attempted { Label("この項目の変更はアプリを終了して開き直すと反映します。", systemImage: "arrow.clockwise").font(.footnote) }
                }
                Section("画面への出力・すぐに反映") {
                    Picker("表示FPSの上限", selection: Binding(get: { graphics.frameLimit }, set: { value in preferences.graphics { $0.frameLimit = value } })) {
                        ForEach(DeviceGraphicsSettings.frameLimits, id: \.self) { value in Text(value == 0 ? "制限なし" : "\(value) FPS").tag(value) }
                    }
                    Picker("出力サイズ", selection: Binding(get: { graphics.outputScale }, set: { value in preferences.graphics { $0.outputScale = value } })) {
                        ForEach(DeviceGraphicsSettings.outputScales, id: \.self) { Text("\($0)%").tag($0) }
                    }
                    Picker("拡大・縮小", selection: Binding(get: { graphics.filter }, set: { value in preferences.graphics { $0.filter = value } })) {
                        ForEach(DeviceFilter.allCases, id: \.self) { Text($0.label).tag($0) }
                    }
                    LabeledContent("シャープネス", value: "\(graphics.sharpness)%")
                    Slider(value: Binding(get: { Double(graphics.sharpness) }, set: { value in preferences.graphics { $0.sharpness = UInt32(value.rounded()) } }), in: 0...100, step: 5)
                        .accessibilityLabel("シャープネス")
                    Text("出力サイズは基準1280×720に対する割合です。出力サイズと表示FPSは最後の画面転送に作用します。ゲーム内部の描画負荷や速度を直接変えません。高FPS指定でゲームの30 FPS制限が解除されるわけではありません。")
                        .font(.footnote).foregroundStyle(.secondary)
                }
                Section("メモリ・次回の起動から") {
                    Toggle("モバイル向けテクスチャキャッシュ", isOn: Binding(get: { graphics.mobileBudget }, set: { value in preferences.graphics { $0.mobileBudget = value } }))
                    Picker("共有メモリキャッシュ", selection: Binding(get: { graphics.mirrorMiB }, set: { value in preferences.graphics { $0.mirrorMiB = value } })) {
                        ForEach(DeviceGraphicsSettings.mirrorBudgets, id: \.self) { Text($0 == 0 ? "従来の設定" : "\($0) MiB").tag($0) }
                    }
                    Text("キャッシュ量を抑えても画素の精度は下げません。小さすぎると再転送が増え、かえって遅くなる場合があります。アプリ全体のメモリ上限ではありません。")
                        .font(.footnote).foregroundStyle(.secondary)
                }
                Section("現在の状態") {
                    LabeledContent("新しいフレーム", value: session.decodedFPSLabel)
                    LabeledContent("画面転送", value: session.presentedFPSLabel)
                    LabeledContent("ゲームからの入力", value: session.sourceSizeLabel)
                    LabeledContent("実際の出力", value: session.outputSizeLabel)
                    LabeledContent("設定の反映", value: session.graphicsApplyLabel)
                    LabeledContent("端末", value: session.powerLabel)
                    if let error = session.settingsError { Text(error).foregroundStyle(.orange) }
                    Text("FPSは直近の処理回数です。ディスプレイに実際に表示された回数やゲームのシミュレーション速度は別です。")
                        .font(.footnote).foregroundStyle(.secondary)
                }
            }
            .navigationTitle("画質とパフォーマンス")
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button("完了") { dismiss() } } }
        }
    }
}

@MainActor
struct DeviceAudioSettingsView: View {
    @ObservedObject var preferences: DevicePreferences
    @ObservedObject var session: OwnedSession
    @Environment(\.dismiss) private var dismiss
    var body: some View {
        NavigationStack {
            Form {
                Section("音量") {
                    Toggle("ミュート", isOn: Binding(get: { preferences.settings.audio.muted }, set: { value in preferences.audio { $0.muted = value } }))
                    LabeledContent("ゲーム全体", value: "\(Int(preferences.settings.audio.gain * 100))%")
                    Slider(value: Binding(get: { preferences.settings.audio.gain }, set: { value in preferences.audio { $0.gain = value } }), in: 0...1, step: 0.01)
                        .accessibilityLabel("ゲーム全体の音量")
                }
                Section("出力") {
                    Picker("バッファ", selection: Binding(get: { preferences.settings.audio.profile }, set: { value in preferences.audio { $0.profile = value } })) {
                        ForEach(DeviceAudioProfile.allCases, id: \.self) { Text($0.label).tag($0) }
                    }
                    Text("低遅延は音切れが増える場合があります。Bluetoothの遅延は接続先にも左右されます。端末の出力形式に合わせて変換します。")
                        .font(.footnote).foregroundStyle(.secondary)
                    Button("音声の再生を再開", action: session.resumeAudio).disabled(!session.running)
                    Text("イヤホンを外した後や音声サービスが再起動した後は、このボタンで再開してください。")
                        .font(.footnote).foregroundStyle(.secondary)
                }
                Section("現在の状態") {
                    LabeledContent("音声出力", value: session.audioStateLabel)
                    LabeledContent("出力先", value: session.audioRouteLabel)
                    LabeledContent("端末の形式", value: session.audioHardwareLabel)
                    LabeledContent("ゲームからの音声", value: session.audioFramesLabel)
                    LabeledContent("出力データの信号", value: session.audioSignalLabel)
                    if let error = session.settingsError { Text(error).foregroundStyle(.orange) }
                    Text("出力エンジンの起動だけではゲーム音声の復号成功は確認できません。無音の場合もゲーム側の音声処理を調査できるよう、受信量を表示します。")
                        .font(.footnote).foregroundStyle(.secondary)
                }
            }
            .navigationTitle("音声")
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button("完了") { dismiss() } } }
        }
    }
}
