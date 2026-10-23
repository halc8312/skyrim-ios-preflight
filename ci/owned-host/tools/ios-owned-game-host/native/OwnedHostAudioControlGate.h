// Non-render control primitives shared with the authored portable fixture.
#ifndef OH_AUDIO_CONTROL_GATE_H
#define OH_AUDIO_CONTROL_GATE_H
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>

class OHAudioPlaybackIntent {
public:
    struct Snapshot { uint64_t token; bool wanted; };
private:
    mutable std::mutex mutex_;
    uint64_t token_ = 0;
    bool wanted_ = false;
public:
    // close/open actions must be short, nonblocking host operations. In
    // particular, they must never call Apple APIs or wait for render work.
    template<class Close> uint64_t set(uint32_t wanted, Close close) {
        if (wanted > 1) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        if (token_ == std::numeric_limits<uint64_t>::max()) {
            wanted_ = false; close(); return 0; // No generation wrap/reuse.
        }
        ++token_; wanted_ = wanted != 0;
        if (!wanted_) close();
        return token_;
    }
    Snapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return {token_, wanted_};
    }
    bool current(uint64_t token) const {
        const auto value = snapshot();
        return token != 0 && value.token == token;
    }
    bool permits(uint64_t token) const {
        const auto value = snapshot();
        return token != 0 && value.token == token && value.wanted;
    }
    template<class Open> bool commit(uint64_t token, Open open) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (token == 0 || token_ != token || !wanted_) return false;
        open(); return true;
    }
    // A resume-required event retires all actions published before its
    // serialized handling. Keep desired intent for status, but require a new
    // publication before any queued Apply or automatic restart can succeed.
    template<class Close> void invalidate(Close close) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (token_ != std::numeric_limits<uint64_t>::max()) ++token_;
        else wanted_ = false;
        close();
    }
    // A timed-out older request must not cancel a newer foreground intent.
    template<class Close> bool cancel(uint64_t token, Close close) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (token == 0 || token_ != token) return false;
        if (token_ != std::numeric_limits<uint64_t>::max()) ++token_;
        wanted_ = false; close(); return true;
    }
};

class OHAudioRequestAdmission {
    std::atomic<bool> pending_{false};
public:
    bool begin() {
        bool expected = false;
        return pending_.compare_exchange_strong(expected, true);
    }
    void finish() { pending_.store(false); }
};

template<class Result> class OHAudioCompletion {
    std::mutex mutex_;
    std::condition_variable changed_;
    bool abandoned_ = false;
    bool finished_ = false;
    Result result_{};
public:
    // A request abandoned while queued is skipped; an already executing Apple
    // call cannot be killed. Its eventual result remains heap-owned.
    bool begin() {
        std::lock_guard<std::mutex> lock(mutex_);
        return !abandoned_;
    }
    void finish(Result result) {
        std::lock_guard<std::mutex> lock(mutex_);
        result_ = result; finished_ = true; changed_.notify_all();
    }
    template<class Rep, class Period>
    bool wait_for(std::chrono::duration<Rep, Period> timeout, Result &out) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!changed_.wait_for(lock, timeout, [&] { return finished_; })) {
            abandoned_ = true; return false;
        }
        out = result_; return true;
    }
};
#endif
