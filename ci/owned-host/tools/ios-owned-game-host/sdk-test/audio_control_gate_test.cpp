// Authored, deterministic tests of the production non-render control helpers.
#include "../native/OwnedHostAudioControlGate.h"
#include "../native/OwnedHostAudioRenderGate.h"
#include <cassert>
#include <cstdio>
#include <future>
#include <memory>
#ifdef NDEBUG
#error "This authored control fixture requires assertions enabled"
#endif
using namespace std::chrono_literals;

template<class T> static void ready(std::future<T> &future) {
    assert(future.wait_for(5s) == std::future_status::ready);
}

int main() {
    // 1: Invalid input is inert; a stale timeout cannot cancel newer intent.
    {
        OHAudioPlaybackIntent intent; OHAudioRenderGate render;
        unsigned closes = 0;
        auto close = [&] { ++closes; render.close(); };
        assert(intent.set(2, close) == 0); assert(closes == 0);
        const auto first = intent.set(1, close);
        const auto second = intent.set(1, close);
        assert(second > first && first != 0);
        assert(!intent.cancel(first, close)); assert(closes == 0);
        assert(!intent.commit(first, [&] { render.open(); }));
        assert(intent.commit(second, [&] { render.open(); }));
        assert(render.enabled());
        intent.invalidate(close);
        assert(intent.snapshot().wanted); // Desired status survives the event.
        assert(!intent.current(second));
        assert(!intent.commit(second, [&] { render.open(); }));
        assert(!render.enabled());
        const auto afterEvent = intent.set(1, close);
        assert(intent.commit(afterEvent, [&] { render.open(); }));
        const auto stopped = intent.set(0, close);
        assert(stopped > afterEvent && closes == 2 && !render.enabled());
        assert(!intent.commit(stopped, [&] { render.open(); }));
    }
    // 2: A simulated slow startup may finish after cancellation, but never open.
    {
        OHAudioPlaybackIntent intent; OHAudioRenderGate render;
        auto close = [&] { render.close(); };
        const auto token = intent.set(1, close);
        std::promise<void> enteredPromise, releasePromise;
        auto entered = enteredPromise.get_future(), release = releasePromise.get_future();
        auto startup = std::async(std::launch::async, [&] {
            assert(intent.permits(token)); enteredPromise.set_value();
            ready(release); release.get();
            return intent.commit(token, [&] { render.open(); });
        });
        ready(entered); entered.get();
        intent.set(0, close); releasePromise.set_value();
        ready(startup); assert(!startup.get()); assert(!render.enabled());
    }
    // 3: Cancellation racing the final commit always closes after that commit.
    {
        OHAudioPlaybackIntent intent; OHAudioRenderGate render;
        auto close = [&] { render.close(); };
        const auto token = intent.set(1, close);
        std::promise<void> enteredPromise, releasePromise, cancellingPromise;
        auto entered = enteredPromise.get_future(), release = releasePromise.get_future();
        auto cancelling = cancellingPromise.get_future();
        auto startup = std::async(std::launch::async, [&] {
            return intent.commit(token, [&] {
                enteredPromise.set_value(); ready(release); release.get(); render.open();
            });
        });
        ready(entered); entered.get();
        auto stop = std::async(std::launch::async, [&] {
            cancellingPromise.set_value(); return intent.set(0, close);
        });
        ready(cancelling); cancelling.get();
        assert(stop.wait_for(0s) == std::future_status::timeout);
        releasePromise.set_value(); ready(startup); assert(startup.get());
        ready(stop); assert(stop.get() > token); assert(!render.enabled());
    }
    // 4: Timeout before dispatch skips work and leaves caller output untouched.
    {
        OHAudioCompletion<int> completion; int output = 55;
        assert(!completion.wait_for(0s, output)); assert(output == 55);
        assert(!completion.begin());
    }
    // 5: Executing work outlives a timed-out caller safely; no stack is captured.
    {
        auto caller = std::make_shared<OHAudioCompletion<int>>();
        std::weak_ptr<OHAudioCompletion<int>> lifetime = caller;
        auto queued = caller;
        assert(queued->begin());
        int output = 55;
        assert(!caller->wait_for(0s, output)); assert(output == 55);
        caller.reset(); assert(!lifetime.expired());
        queued->finish(99); queued.reset(); assert(lifetime.expired());
        assert(output == 55);
    }
    // 6: Successful completion publishes the whole owned result before return.
    {
        struct Result { int code; uint64_t token; };
        auto completion = std::make_shared<OHAudioCompletion<Result>>();
        auto worker = std::async(std::launch::async, [completion] {
            assert(completion->begin()); completion->finish({0, 0x123456789ABCDEF0ull});
        });
        Result result{-1, 0}; assert(completion->wait_for(5s, result));
        assert(result.code == 0 && result.token == 0x123456789ABCDEF0ull);
        ready(worker); worker.get();
    }
    // 7: A timed-out executing request still owns the sole admission slot.
    {
        OHAudioRequestAdmission admission; OHAudioCompletion<int> completion;
        assert(admission.begin()); assert(completion.begin()); int output = 55;
        assert(!completion.wait_for(0s, output));
        for (int i = 0; i != 1000; ++i) assert(!admission.begin());
        admission.finish(); completion.finish(0);
        assert(admission.begin()); admission.finish();
    }
    std::puts("OWNED_HOST_AUDIO_CONTROL_RESULT {\"schema\":1,\"case_count\":7,\"passed\":true,\"apple_engine_verified\":false,\"public_only\":true}");
}
