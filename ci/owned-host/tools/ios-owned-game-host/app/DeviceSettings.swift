import Foundation
import Darwin
import SwiftUI
import os

struct DeviceMemoryUsage: Equatable {
    let usedMiB: Int
    let budgetMiB: Int

    static func read() -> Self? {
        #if targetEnvironment(simulator)
        return nil
        #else
        var info = task_vm_info_data_t()
        var count = mach_msg_type_number_t(MemoryLayout<task_vm_info_data_t>.size / MemoryLayout<integer_t>.size)
        let result = withUnsafeMutablePointer(to: &info) { pointer in
            pointer.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
                task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count)
            }
        }
        guard result == KERN_SUCCESS else { return nil }
        let used = info.phys_footprint
        let (budget, overflow) = used.addingReportingOverflow(UInt64(os_proc_available_memory()))
        guard !overflow, budget / 1_048_576 <= UInt64(Int.max) else { return nil }
        return Self(usedMiB: Int(used / 1_048_576), budgetMiB: Int(budget / 1_048_576))
        #endif
    }
}

struct DeviceRuntimeStatus {
    let lowPowerMode: Bool
    let thermalLabel: String

    static func read() -> Self {
        let process = ProcessInfo.processInfo
        let thermal: String
        switch process.thermalState {
        case .nominal: thermal = "通常"
        case .fair: thermal = "やや高温"
        case .serious: thermal = "高温"
        case .critical: thermal = "温度保護中"
        @unknown default: thermal = "不明"
        }
        return Self(lowPowerMode: process.isLowPowerModeEnabled, thermalLabel: thermal)
    }
}

enum DeviceVideoMode: String, Codable, CaseIterable {
    case p360, p540, p720, p1080
    var size: (UInt32, UInt32) {
        switch self { case .p360: return (640, 360); case .p540: return (960, 540)
        case .p720: return (1280, 720); case .p1080: return (1920, 1080) }
    }
    var label: String { "\(size.0) × \(size.1)" }
}
enum DeviceFilter: UInt32, Codable, CaseIterable {
    case original, bilinear, nearest, bicubic
    var label: String {
        switch self { case .original: return "自動（従来）"; case .bilinear: return "軽量・なめらか"
        case .nearest: return "輪郭をそのまま"; case .bicubic: return "高品質・なめらか" }
    }
}
enum DeviceAudioProfile: UInt32, Codable, CaseIterable {
    case balanced, lowLatency, robust
    var label: String {
        switch self { case .balanced: return "バランス"; case .lowLatency: return "低遅延"
        case .robust: return "途切れにくさ優先" }
    }
}
enum DevicePreset: String, CaseIterable {
    case original, minimum, performance, balanced, quality, ultra, ultimate
    var label: String {
        switch self { case .original: return "従来の設定"; case .minimum: return "最大限に軽量"
        case .performance: return "パフォーマンス"; case .balanced: return "バランス"
        case .quality: return "画質優先"; case .ultra: return "ウルトラ（試験）"
        case .ultimate: return "最高画質（試験）" }
    }
    var graphics: DeviceGraphicsSettings {
        switch self {
        case .original: return DeviceGraphicsSettings()
        case .minimum: return .init(videoMode: .p360, internalScale: 1, mirrorMiB: 128,
            mobileBudget: true, frameLimit: 0, outputScale: 25, filter: .bilinear, sharpness: 0)
        case .performance: return .init(videoMode: .p540, internalScale: 1, mirrorMiB: 128,
            mobileBudget: true, frameLimit: 0, outputScale: 75, filter: .bilinear, sharpness: 0)
        case .balanced: return .init(videoMode: .p720, internalScale: 1, mirrorMiB: 128,
            mobileBudget: true, frameLimit: 30, outputScale: 100, filter: .original, sharpness: 0)
        case .quality: return .init(videoMode: .p720, internalScale: 1, mirrorMiB: 128,
            mobileBudget: false, frameLimit: 30, outputScale: 100, filter: .bicubic, sharpness: 0)
        case .ultra: return .init(videoMode: .p720, internalScale: 2, mirrorMiB: 128,
            mobileBudget: false, frameLimit: 30, outputScale: 100, filter: .bicubic, sharpness: 0)
        case .ultimate: return .init(videoMode: .p720, internalScale: 3, mirrorMiB: 128,
            mobileBudget: false, frameLimit: 0, outputScale: 100, filter: .bicubic, sharpness: 0)
        }
    }
}

struct DeviceGraphicsSettings: Codable, Equatable {
    static let frameLimits: [UInt32] = [0, 15, 20, 24, 30, 40, 60, 90, 120]
    static let outputScales: [UInt32] = [25, 50, 75, 100, 125, 150, 200]
    static let mirrorBudgets: [UInt32] = [0, 64, 128]
    var videoMode: DeviceVideoMode = .p720
    var internalScale: UInt32 = 1
    var mirrorMiB: UInt32 = 0
    var mobileBudget = false
    var frameLimit: UInt32 = 0
    var outputScale: UInt32 = 100
    var filter: DeviceFilter = .original
    var sharpness: UInt32 = 0
    func normalized() -> Self {
        var v = self
        v.internalScale = min(max(v.internalScale, 1), 3)
        if v.videoMode == .p1080 { v.internalScale = min(v.internalScale, 2) }
        if v.mirrorMiB == 192 || v.mirrorMiB == 256 { v.mirrorMiB = 128 }
        if !Self.mirrorBudgets.contains(v.mirrorMiB) { v.mirrorMiB = 0 }
        if !Self.frameLimits.contains(v.frameLimit) { v.frameLimit = 0 }
        if !Self.outputScales.contains(v.outputScale) { v.outputScale = 100 }
        v.sharpness = min(v.sharpness, 100)
        return v
    }
    var boot: OHBootGraphicsV1 {
        let v = normalized()
        var out = OHBootGraphicsV1()
        out.version = 0x0001_0000; out.struct_bytes = 32
        out.video_width = v.videoMode.size.0; out.video_height = v.videoMode.size.1
        out.internal_scale = v.internalScale; out.mirror_mib = v.mirrorMiB
        out.mobile_texture_budget = v.mobileBudget ? 1 : 0
        return out
    }
    var presentation: OHPresentationSettingsV1 {
        let v = normalized()
        var out = OHPresentationSettingsV1()
        out.version = 0x0001_0000; out.struct_bytes = 32
        out.frame_limit = v.frameLimit; out.output_scale_percent = v.outputScale
        out.filter = v.filter.rawValue; out.sharpness = v.sharpness
        return out
    }
    var presetLabel: String {
        DevicePreset.allCases.first(where: { $0.graphics == normalized() })?.label ?? "カスタム"
    }
}
struct DeviceAudioSettings: Codable, Equatable {
    var gain: Double = 1
    var muted = false
    var profile: DeviceAudioProfile = .balanced
    func normalized() -> Self {
        var v = self
        v.gain = v.gain.isFinite ? min(max(v.gain, 0), 1) : 1
        return v
    }
}
struct DeviceSettings: Codable, Equatable {
    var version = 1
    var graphics = DeviceGraphicsSettings()
    var audio = DeviceAudioSettings()
    func normalized() -> Self {
        guard version == 1 else { return DeviceSettings() }
        var v = self; v.graphics = graphics.normalized(); v.audio = audio.normalized(); return v
    }
}
@MainActor
final class DevicePreferences: ObservableObject {
    static let key = "device-settings-v1"
    @Published private(set) var settings: DeviceSettings
    private let defaults: UserDefaults
    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        if let data = defaults.data(forKey: Self.key), data.count <= 4096,
           let decoded = try? JSONDecoder().decode(DeviceSettings.self, from: data) {
            settings = decoded.normalized()
        } else { settings = DeviceSettings() }
    }
    func graphics(_ update: (inout DeviceGraphicsSettings) -> Void) {
        var next = settings; update(&next.graphics); settings = next.normalized(); save()
    }
    func audio(_ update: (inout DeviceAudioSettings) -> Void) {
        var next = settings; update(&next.audio); settings = next.normalized(); save()
    }
    func apply(_ preset: DevicePreset) { graphics { $0 = preset.graphics } }
    func save() {
        guard let data = try? JSONEncoder().encode(settings.normalized()), data.count <= 4096 else { return }
        defaults.set(data, forKey: Self.key)
    }
}
