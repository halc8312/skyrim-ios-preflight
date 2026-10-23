// Public, allocation-free callback admission for one serialized engine owner.
#ifndef OH_AUDIO_RENDER_GATE_H
#define OH_AUDIO_RENDER_GATE_H
#include <atomic>
#include <cstdint>

static_assert(std::atomic<uint64_t>::is_always_lock_free, "audio needs lock-free u64");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "audio needs lock-free u32");
static_assert(std::atomic<bool>::is_always_lock_free, "audio needs lock-free bool");

class OHAudioRenderGate {
    std::atomic<uint64_t> generation_{1};
    std::atomic<uint32_t> active_{0};
    std::atomic<bool> enabled_{false};
public:
    // These operations deliberately all use SC, not independent acquire/release
    // pairs. In the one SC order, enter precedes the callback's enabled/token
    // checks; close precedes the owner's zero-count observation. A callback
    // passing old checks is therefore counted until leave. A callback arriving
    // after zero cannot pass an old token, even if a new engine has reopened.
    // No AVAudioEngine.stop() callback-barrier guarantee is assumed.
    void enter() { active_.fetch_add(1, std::memory_order_seq_cst); }
    void leave() { active_.fetch_sub(1, std::memory_order_seq_cst); }
    bool accepts(uint64_t token) const {
        return enabled_.load(std::memory_order_seq_cst) &&
               generation_.load(std::memory_order_seq_cst) == token &&
               active_.load(std::memory_order_seq_cst) == 1;
    }
    // Overlapping callbacks fail closed rather than wait. Once a callback
    // passes count==1, a second cannot pass while the first scope is alive.
    void close() {
        enabled_.store(false, std::memory_order_seq_cst);
        generation_.fetch_add(1, std::memory_order_seq_cst);
    }
    uint64_t next_generation() { return generation_.fetch_add(1, std::memory_order_seq_cst) + 1; }
    void open() { enabled_.store(true, std::memory_order_seq_cst); }
    bool enabled() const { return enabled_.load(std::memory_order_seq_cst); }
    uint32_t active_count() const { return active_.load(std::memory_order_seq_cst); }
};

class OHAudioRenderScope {
    OHAudioRenderGate &gate_;
public:
    explicit OHAudioRenderScope(OHAudioRenderGate &gate) : gate_(gate) { gate_.enter(); }
    ~OHAudioRenderScope() { gate_.leave(); }
    OHAudioRenderScope(const OHAudioRenderScope &) = delete;
    OHAudioRenderScope &operator=(const OHAudioRenderScope &) = delete;
};
#endif
