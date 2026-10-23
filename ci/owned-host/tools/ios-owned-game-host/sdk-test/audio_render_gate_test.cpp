// Authored portable concurrency check for the exact native render admission.
#include "../native/OwnedHostAudioRenderGate.h"
#include <cassert>
#include <cstdio>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#ifdef NDEBUG
#error "This authored gate fixture requires assertions enabled"
#endif

int main() {
    OHAudioRenderGate gate;
    const uint64_t old_token = gate.next_generation(); gate.open();
    std::promise<void> admitted_promise;
    auto admitted = admitted_promise.get_future();
    std::promise<void> release_promise;
    auto release = release_promise.get_future();
    auto callback = std::async(std::launch::async, [&] {
        OHAudioRenderScope scope(gate);
        assert(gate.accepts(old_token));
        admitted_promise.set_value();
        release.wait();
    });
    assert(admitted.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    gate.close();
    assert(gate.active_count() == 1); // Owner must not replace the admitted graph.
    release_promise.set_value();
    assert(callback.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    callback.get(); assert(gate.active_count() == 0);
    const uint64_t new_token = gate.next_generation(); gate.open();
    {
        OHAudioRenderScope delayed_old(gate);
        assert(!gate.accepts(old_token)); // A delayed old graph may only silence.
        assert(gate.accepts(new_token));
        {
            OHAudioRenderScope unexpected_overlap(gate);
            assert(!gate.accepts(new_token));
        }
        assert(gate.accepts(new_token));
    }
    gate.close(); assert(gate.active_count() == 0);

    // Overlap the owner with callbacks without timing sleeps. Exactly one
    // callback thread models the source node's single render consumer.
    std::atomic<uint64_t> published{0};
    std::atomic<uint64_t> retired{0};
    std::atomic<bool> finish{false};
    std::atomic<uint64_t> accepted{0};
    std::thread render([&] {
        while (!finish.load(std::memory_order_seq_cst)) {
            const uint64_t token = published.load(std::memory_order_seq_cst);
            OHAudioRenderScope scope(gate);
            if (gate.accepts(token)) {
                assert(retired.load(std::memory_order_seq_cst) < token);
                accepted.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
                assert(retired.load(std::memory_order_seq_cst) < token);
            }
        }
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    for (unsigned cycle = 0; cycle != 10000; ++cycle) {
        const uint64_t token = gate.next_generation();
        published.store(token, std::memory_order_seq_cst); gate.open();
        if (cycle == 0) {
            while (accepted.load(std::memory_order_relaxed) == 0) {
                assert(std::chrono::steady_clock::now() < deadline);
                std::this_thread::yield();
            }
        }
        std::this_thread::yield(); gate.close();
        while (gate.active_count() != 0) {
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
        retired.store(token, std::memory_order_seq_cst);
    }
    finish.store(true, std::memory_order_seq_cst); render.join();
    assert(gate.active_count() == 0);
    assert(accepted.load(std::memory_order_relaxed) != 0);
    std::puts("OWNED_HOST_AUDIO_GATE_RESULT {\"schema\":1,\"case_count\":4,\"stress_cycles\":10000,\"passed\":true,\"apple_engine_verified\":false,\"public_only\":true}");
}
