/* Isolated contended-path observer. No printing, policy or shared counters. */
#pragma once
#include <cerrno>
#include <cstdint>
#include <time.h>

namespace skyrim::host_wait {
struct LockWaitCounters {
    uint64_t waits = 0, samples = 0, wall_ns = 0, cpu_ns = 0, clock_failures = 0;
};
inline thread_local LockWaitCounters lockWaitCounters;
inline void addSaturated(uint64_t& total, uint64_t value) noexcept {
    total = value > UINT64_MAX - total ? UINT64_MAX : total + value;
}
inline bool readWaitClock(clockid_t which, uint64_t& value) noexcept {
    timespec stamp{};
    if (clock_gettime(which, &stamp) != 0 || stamp.tv_sec < 0 || stamp.tv_nsec < 0
        || stamp.tv_nsec >= 1000000000 || uint64_t(stamp.tv_sec) > UINT64_MAX / 1000000000)
        return false;
    const uint64_t seconds = uint64_t(stamp.tv_sec) * 1000000000;
    if (uint64_t(stamp.tv_nsec) > UINT64_MAX - seconds) return false;
    value = seconds + uint64_t(stamp.tv_nsec);
    return true;
}
class LockWaitSample {
public:
    LockWaitSample() noexcept : saved_errno_(errno) {
        valid_ = readWaitClock(CLOCK_MONOTONIC, wall_) && readWaitClock(CLOCK_THREAD_CPUTIME_ID, cpu_);
    }
    ~LockWaitSample() noexcept {
        uint64_t wall = 0, cpu = 0;
        addSaturated(lockWaitCounters.waits, 1);
        // Nest the CPU interval inside wall time (wall/CPU ... CPU/wall).
        if (valid_ && readWaitClock(CLOCK_THREAD_CPUTIME_ID, cpu) && readWaitClock(CLOCK_MONOTONIC, wall)
            && wall >= wall_ && cpu >= cpu_ && cpu - cpu_ <= wall - wall_) {
            addSaturated(lockWaitCounters.samples, 1);
            addSaturated(lockWaitCounters.wall_ns, wall - wall_);
            addSaturated(lockWaitCounters.cpu_ns, cpu - cpu_);
        } else {
            addSaturated(lockWaitCounters.clock_failures, 1);
        }
        errno = saved_errno_;
    }
    LockWaitSample(const LockWaitSample&) = delete;
    LockWaitSample& operator=(const LockWaitSample&) = delete;
private:
    int saved_errno_;
    uint64_t wall_ = 0, cpu_ = 0;
    bool valid_ = false;
};
inline LockWaitCounters takeLockWaitCounters() noexcept {
    const auto result = lockWaitCounters;
    lockWaitCounters = {};
    return result;
}
}
