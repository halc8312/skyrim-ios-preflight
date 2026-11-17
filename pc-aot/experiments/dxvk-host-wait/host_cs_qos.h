/* Isolated startup-only host experiment; never included by production. */
#pragma once
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#include <sys/qos.h>
#endif

namespace skyrim::host_wait {
struct PreserveErrno {
    int saved = errno;
    ~PreserveErrno() { errno = saved; }
};
struct QosRecord {
    uint64_t thread = 0;
    unsigned enabled = 0, supported = 0, requested = 0, set_attempted = 0;
    unsigned requested_before = 0, current_before = 0, requested_after = 0, current_after = 0;
    int relative_before = 0, relative_after = 0;
    int before_result = ENOTSUP, set_result = ENOTSUP, after_result = ENOTSUP;
};

inline QosRecord observeCsQos(bool enabled) noexcept {
    PreserveErrno keep;
    QosRecord record;
    record.enabled = unsigned(enabled);
    if (!enabled) return record; // Disabled path performs no QoS query or set.
#if defined(__APPLE__)
    record.supported = 1;
    record.requested = unsigned(QOS_CLASS_USER_INITIATED);
    qos_class_t before = QOS_CLASS_UNSPECIFIED, after = QOS_CLASS_UNSPECIFIED;
    record.before_result = pthread_get_qos_class_np(pthread_self(), &before, &record.relative_before);
    record.requested_before = unsigned(before);
    record.current_before = unsigned(qos_class_self());
    record.set_attempted = 1;
    record.set_result = pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
    record.after_result = pthread_get_qos_class_np(pthread_self(), &after, &record.relative_after);
    record.requested_after = unsigned(after);
    record.current_after = unsigned(qos_class_self());
#endif
    return record;
}

inline QosRecord csQosStartup() noexcept {
    PreserveErrno keep;
    const char* value = std::getenv("SKYRIM_DXVK_CS_QOS");
    const bool enabled = value && value[0] == '1' && value[1] == '\0';
    if (!enabled) return {}; // Original inheritance: no APIs, counter or report.
    QosRecord record = observeCsQos(enabled);
    static std::atomic<uint64_t> ordinal{0};
    record.thread = ordinal.fetch_add(1, std::memory_order_relaxed) + 1;
    // Logging is bounded and observational. All threads retain the selected
    // startup policy even after the report limit; no metrics drive scheduling.
    if (record.thread <= 64) {
        std::fprintf(stderr,
            "DXVK_HOST_CS_QOS thread=%llu enabled=%u supported=%u requested=%u "
            "requested_before=%u current_before=%u relative_before=%d before_result=%d "
            "set_attempted=%u set_result=%d requested_after=%u current_after=%u "
            "relative_after=%d after_result=%d effective_observed=0 iphone_verified=0\n",
            static_cast<unsigned long long>(record.thread), record.enabled, record.supported, record.requested,
            record.requested_before, record.current_before, record.relative_before, record.before_result,
            record.set_attempted, record.set_result, record.requested_after, record.current_after,
            record.relative_after, record.after_result);
    }
    // Apple's two getters expose requested/self class, not temporary effective
    // override priority. The numeric "current" fields name qos_class_self only.
    return record;
}
}
