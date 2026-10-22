import SwiftUI

@main
struct ProbeApp: App {
    @StateObject private var model = ProbeModel()

    var body: some Scene {
        WindowGroup {
            ScrollView {
                VStack(alignment: .leading, spacing: 20) {
                    Text("Skyrim iOS 開発テスト")
                        .font(.largeTitle.bold())
                    Text("ゲーム本体は、まだ起動できません。移植の土台となる処理と、iPhone上の描画を確認するアプリです。")
                        .foregroundStyle(.secondary)

                    if model.cpuPassed {
                        TextureView(model: model)
                            .frame(height: 240)
                            .clipShape(RoundedRectangle(cornerRadius: 16))
                            .accessibilityLabel("自作BC1テクスチャ。赤、青、赤紫、青紫の4本の縦縞")
                    }

                    GroupBox("検証結果") {
                        VStack(alignment: .leading, spacing: 12) {
                            Label(model.cpuStatus, systemImage: model.cpuPassed ? "checkmark.circle.fill" : "xmark.circle.fill")
                            Label(model.metalStatus, systemImage: model.metalPassed ? "checkmark.circle.fill" : "display")
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(.vertical, 4)
                    }
                    Text("この画面ではJITは不要です。自作のテストデータのみを使い、ゲームファイルやアカウントにはアクセスしません。")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }
                .padding(24)
            }
        }
    }
}

final class ProbeModel: ObservableObject {
    @Published var metalStatus = "Metal：描画を待っています"
    @Published var metalPassed = false
    let cpuPassed: Bool
    let cpuStatus: String
    let pixels: [UInt8]

    init() {
        guard probe_abi_version() == 1, probe_width() == 4,
              probe_height() == 4, probe_rgba_byte_len() == 64 else {
            pixels = []
            cpuPassed = false
            cpuStatus = "CPU：Rustブリッジの仕様が一致しません"
            return
        }
        var decoded = [UInt8](repeating: 0, count: 64)
        let result = decoded.withUnsafeMutableBytes { bytes in
            probe_decode_rgba(bytes.baseAddress?.assumingMemoryBound(to: UInt8.self), bytes.count)
        }
        let row: [UInt8] = [255, 0, 0, 255, 0, 0, 255, 255,
                            170, 0, 85, 255, 85, 0, 170, 255]
        let expected = Array(repeating: row, count: 4).flatMap { $0 }
        let cppPassed = guest_probe(5) == ((UInt64(5) << 7) ^ 0x1234)
            && synthetic_import() == 17 && synthetic_dispatch() == 23
        let passed = result == 64 && decoded == expected && cppPassed
        cpuPassed = passed
        pixels = passed ? decoded : []
        cpuStatus = passed
            ? "CPU：RustのBC1展開・全16画素・C++呼出しが一致"
            : "CPU：テストに失敗しました（Rust結果 \(result)）"
    }
}
