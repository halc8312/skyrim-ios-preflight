// Authored QoS API contract on non-Apple hosts; actual APIs on an Apple host.
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <thread>
#include <vector>
#if HOST_APPLE_STUB
#include "host_wait_apple_stubs.h"
#define __APPLE__ 1
#endif
#include "host_cs_qos.h"
#if HOST_APPLE_STUB
#undef __APPLE__
#endif
namespace dxvk::env {
thread_local bool named = false;
void setThreadName(const char* name) { assert(std::strcmp(name, "dxvk-cs") == 0); named = true; }
}
namespace dxvk {
void actual_startup() {
// ACTUAL_CS_STARTUP
}
}

static void verify_enabled() {
    errno = EAGAIN;
    const auto record = skyrim::host_wait::csQosStartup();
    assert(errno == EAGAIN && record.enabled == 1 && record.supported == 1);
    assert(record.before_result == 0 && record.set_result == 0 && record.after_result == 0);
    assert(record.set_attempted == 1 && record.requested == unsigned(QOS_CLASS_USER_INITIATED));
    assert(record.requested_after == unsigned(QOS_CLASS_USER_INITIATED) && record.current_after == unsigned(QOS_CLASS_USER_INITIATED));
    assert(record.relative_after == 0);
}
int main(int argc, char** argv) {
    assert(argc == 2);
    const bool enabled = std::strcmp(argv[1], "enabled") == 0;
    if (enabled) {
        assert(setenv("SKYRIM_DXVK_CS_QOS", "1", 1) == 0);
#if HOST_APPLE_STUB
        host_stub::qos = {};
#endif
        verify_enabled();
#if HOST_APPLE_STUB
        assert(host_stub::qos.gets == 2 && host_stub::qos.sets == 1 && host_stub::qos.selfs == 2);
        // Failed getters and a failed setter remain explicit numeric failures;
        // they neither alter inheritance nor leak an API-modified errno.
        host_stub::qos = {}; host_stub::qos.get_result = EIO; host_stub::qos.set_result = EPERM;
        errno = EBUSY;
        const auto failed = skyrim::host_wait::observeCsQos(true);
        assert(errno == EBUSY && failed.before_result == EIO && failed.after_result == EIO && failed.set_result == EPERM);
        assert(failed.requested_before == 0 && failed.requested_after == 0 && host_stub::qos.value == 17);
        assert(host_stub::qos.gets == 2 && host_stub::qos.sets == 1 && host_stub::qos.selfs == 2);
        host_stub::qos = {};
#endif
        std::vector<std::thread> workers;
        for (unsigned n = 0; n < 8; ++n) workers.emplace_back([] {
            errno = EAGAIN; dxvk::actual_startup(); assert(dxvk::env::named && errno == EAGAIN);
#if HOST_APPLE_STUB
            assert(host_stub::qos.gets == 2 && host_stub::qos.sets == 1 && host_stub::qos.selfs == 2);
#endif
        });
        for (auto& worker : workers) worker.join();
        for (unsigned n = 0; n < 64; ++n) verify_enabled(); // Logging alone is capped.
    } else {
        const char* values[] = {nullptr, "", "0", "true", "01", "1 ", "2"};
        for (const char* value : values) {
            assert((value ? setenv("SKYRIM_DXVK_CS_QOS", value, 1) : unsetenv("SKYRIM_DXVK_CS_QOS")) == 0);
            errno = EAGAIN; dxvk::actual_startup();
            assert(dxvk::env::named && errno == EAGAIN);
        }
#if HOST_APPLE_STUB
        assert(host_stub::qos.gets == 0 && host_stub::qos.sets == 0 && host_stub::qos.selfs == 0);
        assert(host_stub::qos.value == 17 && host_stub::qos.relative == -3);
#endif
    }
#if defined(__APPLE__) && !HOST_APPLE_STUB
    constexpr unsigned apple = 1;
#else
    constexpr unsigned apple = 0;
#endif
    std::printf("{\"schema\":1,\"enabled\":%u,\"authored_api_stub\":%u,\"apple_api_verified\":%u,"
        "\"iphone_verified\":0,\"effective_qos_verified\":0,\"performance_verified\":0,\"errno_preserved\":1,\"semantics\":1}\n",
        unsigned(enabled), unsigned(HOST_APPLE_STUB), apple && unsigned(enabled));
}
