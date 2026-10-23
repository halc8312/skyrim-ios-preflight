import Foundation
import SwiftUI

@MainActor
final class MobileControlsPreferences: ObservableObject {
    private static let key = "mobile-controls-v1"
    @Published private(set) var settings: MobileControlsSettings
    @Published var editingLayout = false
    private let defaults: UserDefaults

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        if let bytes = defaults.data(forKey: Self.key), bytes.count <= 16_384,
           let value = try? JSONDecoder().decode(MobileControlsSettings.self, from: bytes) {
            settings = value.normalized()
        } else { settings = MobileControlsSettings() }
    }
    func setSensitivity(_ value: Double) { settings.sensitivity = value; settings = settings.normalized() }
    func setInvertY(_ value: Bool) { settings.invertY = value }
    func setCameraMode(_ value: MobileCameraMode) { settings.cameraMode = value }
    func move(_ control: MobilePadControl, to point: PadPoint) {
        settings.positions[control.rawValue] = point
        settings = settings.normalized()
    }
    func resetLayout() { settings.positions = [:] }
    func save() {
        guard let data = try? JSONEncoder().encode(settings.normalized()), data.count <= 16_384 else { return }
        defaults.set(data, forKey: Self.key)
    }
}

@MainActor
struct MobileControlsSettingsView: View {
    @ObservedObject var preferences: MobileControlsPreferences
    @Environment(\.dismiss) private var dismiss
    let editLayout: () -> Void

    var body: some View {
        NavigationStack {
            Form {
                Section("カメラ") {
                    Picker("右側の操作", selection: Binding(get: { preferences.settings.cameraMode },
                                                          set: preferences.setCameraMode)) {
                        ForEach(MobileCameraMode.allCases, id: \.self) { mode in Text(mode.label).tag(mode) }
                    }
                    HStack {
                        Text("感度")
                        Spacer()
                        Text(String(format: "%.2f×", preferences.settings.sensitivity)).monospacedDigit()
                    }
                    Slider(value: Binding(get: { preferences.settings.sensitivity }, set: preferences.setSensitivity),
                           in: 0.25...3, step: 0.05, onEditingChanged: { if !$0 { preferences.save() } })
                        .accessibilityLabel("カメラ感度")
                    Toggle("上下を反転", isOn: Binding(get: { preferences.settings.invertY },
                                                       set: preferences.setInvertY))
                    Text("スワイプは指の移動で視点を動かします。スティックは最初に触れた位置を中心に、離すまで方向を入力します。")
                        .font(.footnote).foregroundStyle(.secondary)
                }
                Section("ボタン配置") {
                    Button("ボタンの位置を変更") {
                        preferences.save()
                        editLayout()
                        dismiss()
                    }
                    Text("配置編集中はゲームへ操作を送りません。ゲーム自体や動作確認の時間上限は停止しません。")
                        .font(.footnote).foregroundStyle(.secondary)
                }
                Section {
                    Text("左側の空いた場所をドラッグして移動、右側でカメラ操作。ボタンとスティックは同時に使えます。設定と配置はこの端末に保存します。")
                        .font(.footnote).foregroundStyle(.secondary)
                }
            }
            .navigationTitle("操作設定")
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button("完了") { dismiss() } } }
        }
        .onDisappear { preferences.save() }
    }
}
