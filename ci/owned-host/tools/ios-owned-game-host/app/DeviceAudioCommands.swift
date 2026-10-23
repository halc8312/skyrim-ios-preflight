import Foundation

// Only SetPlaybackIntent is main-safe. All Apple session/engine work and status
// reads use one worker, with bounded pending state rather than a timer backlog.
struct DeviceAudioBackend {
    var setIntent: (Bool) -> UInt64
    var applyIntent: (UInt64) -> Int32
    var controls: (Float, UInt32, UInt32) -> Int32
    var status: () -> (Int32, OHNativeAudioStatusV1?)

    static var native: Self {
        Self(setIntent: { OHNativeAudioSetPlaybackIntentV1($0 ? 1 : 0) },
             applyIntent: { OHNativeAudioApplyPlaybackIntentV1($0) },
             controls: { OHNativeAudioSetControlsV1($0, $1, $2) },
             status: {
                 var value = OHNativeAudioStatusV1()
                 let code = OHNativeAudioReadStatusV1(&value, UInt32(MemoryLayout<OHNativeAudioStatusV1>.size))
                 return (code, code == 0 ? value : nil)
             })
    }
}

struct DeviceAudioUpdate {
    let intent: UInt64
    let controlsRevision: UInt64
    let intentCode: Int32?
    let controlsCode: Int32?
    let statusCode: Int32
    let status: OHNativeAudioStatusV1?
}

final class DeviceAudioCommands: @unchecked Sendable {
    private struct Controls: Equatable {
        let gain: Float
        let muted: UInt32
        let profile: UInt32
    }
    private struct Work {
        let intent: UInt64
        let wanted: Bool
        let applyIntent: Bool
        let controls: Controls?
        let controlsRevision: UInt64
    }
    private let backend: DeviceAudioBackend
    private let output: (DeviceAudioUpdate) -> Void
    private let queue: DispatchQueue
    private let lock = NSLock()
    private var draining = false
    private var intent: UInt64 = 0
    private var wanted = false
    private var pendingIntent = false
    private var latestControls: Controls?
    private var controlsRevision: UInt64 = 0
    private var pendingControls = false
    private var pendingStatus = false

    init(backend: DeviceAudioBackend = .native,
         queue: DispatchQueue = DispatchQueue(label: "skyrim.audio.commands", qos: .userInitiated),
         output: @escaping (DeviceAudioUpdate) -> Void) {
        self.backend = backend; self.queue = queue; self.output = output
    }

    // Native invalidation happens immediately on the caller (Main). A queued or
    // in-flight start cannot reopen render admission after a newer stop token.
    @discardableResult
    func setPlaybackIntent(_ playback: Bool) -> UInt64 {
        precondition(Thread.isMainThread)
        let token = backend.setIntent(playback)
        lock.lock()
        intent = token; wanted = playback; pendingIntent = true; pendingStatus = true
        scheduleLocked()
        lock.unlock()
        return token
    }

    @discardableResult
    func setControls(_ settings: DeviceAudioSettings) -> UInt64 {
        precondition(Thread.isMainThread)
        let normalized = settings.normalized()
        let value = Controls(gain: Float(normalized.gain), muted: normalized.muted ? 1 : 0,
                             profile: normalized.profile.rawValue)
        lock.lock()
        if latestControls != value {
            latestControls = value; controlsRevision &+= 1; pendingControls = true; pendingStatus = true
        }
        let revision = controlsRevision
        scheduleLocked()
        lock.unlock()
        return revision
    }

    func requestStatus() {
        precondition(Thread.isMainThread)
        lock.lock(); pendingStatus = true; scheduleLocked(); lock.unlock()
    }

    private func scheduleLocked() {
        guard !draining, pendingIntent || pendingControls || pendingStatus else { return }
        draining = true
        queue.async { [self] in drain() }
    }

    private func nextWork() -> Work? {
        lock.lock(); defer { lock.unlock() }
        guard pendingIntent || pendingControls || pendingStatus else { draining = false; return nil }
        let work = Work(intent: intent, wanted: wanted, applyIntent: pendingIntent,
                        controls: pendingControls ? latestControls : nil, controlsRevision: controlsRevision)
        pendingIntent = false; pendingControls = false; pendingStatus = false
        return work
    }

    private func drain() {
        precondition(!Thread.isMainThread)
        while let work = nextWork() {
            var intentCode: Int32?
            var controlsCode: Int32?
            // A stop takes precedence. For a start, install the selected profile
            // first so the engine need not be rebuilt immediately afterwards.
            if work.applyIntent && !work.wanted { intentCode = backend.applyIntent(work.intent) }
            if let c = work.controls { controlsCode = backend.controls(c.gain, c.muted, c.profile) }
            if work.applyIntent && work.wanted { intentCode = backend.applyIntent(work.intent) }
            let (statusCode, status) = backend.status()
            let update = DeviceAudioUpdate(intent: work.intent, controlsRevision: work.controlsRevision,
                intentCode: intentCode, controlsCode: controlsCode, statusCode: statusCode, status: status)
            DispatchQueue.main.async { [output] in output(update) }
        }
    }
}
