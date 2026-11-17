// Authored API contracts only. These functions provide no Apple OS evidence.
#pragma once
#include <atomic>
#include <cerrno>
#include <mutex>
#include <pthread.h>
namespace host_stub {
inline std::atomic<unsigned> tries{0}, blocks{0}, unlocks{0};
struct Qos {
    unsigned value = 17;
    int relative = -3, get_result = 0, set_result = 0;
    unsigned gets = 0, sets = 0, selfs = 0;
};
inline thread_local Qos qos;
}
struct os_unfair_lock { std::mutex mutex; };
#define OS_UNFAIR_LOCK_INIT {}
inline bool os_unfair_lock_trylock(os_unfair_lock* lock) { ++host_stub::tries; return lock->mutex.try_lock(); }
inline void os_unfair_lock_lock(os_unfair_lock* lock) { ++host_stub::blocks; lock->mutex.lock(); }
inline void os_unfair_lock_unlock(os_unfair_lock* lock) { ++host_stub::unlocks; lock->mutex.unlock(); }
using qos_class_t = unsigned;
constexpr qos_class_t QOS_CLASS_UNSPECIFIED = 0, QOS_CLASS_USER_INITIATED = 25;
inline int pthread_get_qos_class_np(pthread_t, qos_class_t* value, int* relative) {
    ++host_stub::qos.gets; errno = ERANGE;
    if (host_stub::qos.get_result) return host_stub::qos.get_result;
    *value = host_stub::qos.value; *relative = host_stub::qos.relative; return 0;
}
inline int pthread_set_qos_class_self_np(qos_class_t value, int relative) {
    ++host_stub::qos.sets; errno = EIO;
    if (host_stub::qos.set_result) return host_stub::qos.set_result;
    host_stub::qos.value = value; host_stub::qos.relative = relative; return 0;
}
inline qos_class_t qos_class_self() { ++host_stub::qos.selfs; errno = EDOM; return host_stub::qos.value; }
