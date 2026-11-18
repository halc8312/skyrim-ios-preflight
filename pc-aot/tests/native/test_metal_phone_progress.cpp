#include "runtime_host.h"
#include <cstdio>
#include <stdexcept>

using namespace SkyrimPhoneProgress;
static void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

int main() {
    try {
        uint64_t completed = 0, persisted = 0, largestSuffix = 0;
        uint64_t considered = 0, emitted = 0, skipped = 0;
        auto boundary = [&](Boundary kind) {
            ++considered;
            if (fullSnapshot(kind, completed)) { ++emitted; persisted = completed; }
            else ++skipped;
        };
        for (unsigned i = 0; i < 3; ++i) boundary(Boundary::Control);
        for (unsigned c = 0; c < 12; ++c) {
            boundary(Boundary::Control);
            for (unsigned s = 0; s < 16; ++s) {
                boundary(Boundary::Operation);
                boundary(Boundary::Operation);
                ++completed;
                if (completed - persisted > largestSuffix) largestSuffix = completed - persisted;
                check(completed - persisted <= SnapshotStride, "forced-kill suffix exceeded sample bound");
                boundary(Boundary::SampleComplete);
            }
        }
        for (unsigned i = 0; i < 10; ++i) boundary(Boundary::Control);
        check(considered == 601 && emitted == 73 && skipped == 528, "boundary count mismatch");
        check(completed == 192 && persisted == 192 && largestSuffix == 4, "terminal/recovery sample mismatch");
        check(!fullSnapshot(Boundary::SampleComplete, 0), "empty batch emitted a sample checkpoint");
        // The bound is in completed samples, not elapsed time. Issued Metal
        // work may still take arbitrarily long; this is not a timeout guarantee.
        for (uint64_t count = 1; count < 1000; ++count)
            check(fullSnapshot(Boundary::SampleComplete, count) == (count % 4 == 0), "sample cadence changed");
        std::puts("PHONE_PROGRESS_POLICY considered=601 snapshots=73 skipped=528 suffix=4 gpu_execution_verified=0");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
