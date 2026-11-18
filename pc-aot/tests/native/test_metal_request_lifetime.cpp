// Authored portable lifecycle model. No Metal, Objective-C, MoltenVK, or GPU calls.
#include "request_lifetime.h"
#include "owner_retirement.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <stdexcept>
#include <thread>

using metal_compile_experiment::RequestEvent;

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Counted {
    std::atomic<int>& alive;
    explicit Counted(std::atomic<int>& count) : alive(count) { ++alive; }
    virtual ~Counted() { --alive; }
};
struct Payload {
    std::shared_ptr<Counted> library;
    std::shared_ptr<Counted> error;
};
struct Task : Counted {
    using Counted::Counted;
    std::function<void()> retainedCallback;
};
struct Owner : Counted {
    using Counted::Counted;
    bool complete = false;
    bool destroyRequested = false;
    Payload result;
    int adoptedTaskSlot = 0;
};
using Request = metal_compile_experiment::RequestLifetime<Payload, std::shared_ptr<Task>>;

struct Fixture {
    std::atomic<int> owners{0}, tasks{0}, payloads{0}, notifications{0};
    std::mutex ownerMutex;
    std::unique_ptr<Owner> owner = std::make_unique<Owner>(owners);
    std::shared_ptr<Request> request;
    bool expectError = false;
    bool expectWarning = false;
    bool expectTask = true;
    bool throwOnNotify = false;
    bool reenter = false;
    bool dropRequestOnNotify = false;

    Fixture() {
        request = Request::create([this](Payload payload, std::shared_ptr<Task> task) {
            check(bool(payload.error) == (expectError || expectWarning), "error ownership/value changed");
            check(bool(payload.library) == !expectError, "library ownership/value changed");
            check(bool(task) == expectTask, "task ownership/value changed");
            {
                std::lock_guard<std::mutex> lock(ownerMutex);
                check(bool(owner), "notification reached a destroyed owner");
                ++notifications;
                owner->complete = true;
                if (owner->destroyRequested) owner.reset();
            }
            if (reenter) {
                check(request->delivered(), "delivery was not published before notification");
                check(request->callback(makePayload()) == RequestEvent::duplicate, "reentrant callback was forwarded");
            }
            if (dropRequestOnNotify) request.reset();
            if (throwOnNotify) throw std::runtime_error("authored notification failure");
        });
    }

    Payload makePayload() {
        return {expectError ? nullptr : std::make_shared<Counted>(payloads),
                expectError || expectWarning ? std::make_shared<Counted>(payloads) : nullptr};
    }
    std::shared_ptr<Task> makeTask() { return std::make_shared<Task>(tasks); }

    // Model the fixed MVK rule: destroy during a pending compile only marks the
    // owner; final notification may then delete it. No wait duration is changed.
    void requestDestroy() {
        std::lock_guard<std::mutex> lock(ownerMutex);
        if (!owner) return;
        owner->destroyRequested = true;
        if (owner->complete) owner.reset();
    }

    void clean() {
        requestDestroy();
        request.reset();
        check(owners == 0 && tasks == 0 && payloads == 0 && notifications == 1,
              "owner/task/payload leaked or notification count differed");
    }
};

static void normalOrder() {
    Fixture f;
    check(f.request->launchReturned(f.makeTask()) == RequestEvent::pending, "premature launch completion");
    check(f.owners == 1 && f.tasks == 1, "pending launch lost ownership");
    check(f.request->callback(f.makePayload()) == RequestEvent::delivered, "normal callback not delivered");
    check(f.owners == 1, "owner deleted before its explicit destroy");
    f.clean();
}

static void callbackBeforeLaunchReturn() {
    Fixture f;
    f.requestDestroy(); // Waiting caller timed out before the factory returned.
    check(f.request->callback(f.makePayload()) == RequestEvent::pending, "early callback escaped launch barrier");
    check(f.owners == 1 && f.payloads == 1 && f.notifications == 0, "owner retired before task adoption");
    check(f.request->launchReturned(f.makeTask()) == RequestEvent::delivered, "early callback not delivered after launch");
    check(f.owners == 0, "late owner was not retired");
    f.clean();
}

static void timeoutThenLateCallback() {
    Fixture f;
    f.expectError = true;
    f.request->launchReturned(f.makeTask());
    f.requestDestroy();
    check(f.owners == 1 && f.tasks == 1 && f.notifications == 0, "timeout released pending state");
    check(f.request->callback(f.makePayload()) == RequestEvent::delivered, "late compiler error not delivered");
    check(f.owners == 0, "late compiler error did not retire owner");
    f.clean();
}

static void defensiveDuplicates() {
    Fixture f;
    f.request->callback(f.makePayload());
    check(f.request->callback(f.makePayload()) == RequestEvent::duplicate, "early duplicate callback accepted");
    check(f.payloads == 1, "duplicate payload replaced the original");
    f.request->launchReturned(f.makeTask());
    check(f.request->launchReturned(f.makeTask()) == RequestEvent::duplicate, "duplicate launch accepted");
    check(f.request->callback(f.makePayload()) == RequestEvent::duplicate, "late duplicate callback accepted");
    f.clean();
}

static void reentrantNotificationAndWarning() {
    Fixture f;
    f.expectWarning = true; // A non-null result plus diagnostic must remain intact.
    f.reenter = true;
    f.request->launchReturned(f.makeTask());
    f.request->callback(f.makePayload());
    f.clean();
}

static void exceptionDoesNotRetry() {
    Fixture f;
    f.throwOnNotify = true;
    f.request->launchReturned(f.makeTask());
    bool threw = false;
    try { f.request->callback(f.makePayload()); }
    catch (const std::runtime_error&) { threw = true; }
    check(threw && f.request->delivered(), "notification exception was hidden or reset completion");
    check(f.request->callback(f.makePayload()) == RequestEvent::duplicate, "failed notification was retried");
    f.clean();
}

static void lastReferenceAndTaskCycle() {
    Fixture f;
    f.dropRequestOnNotify = true;
    f.requestDestroy();
    std::weak_ptr<Request> weakRequest = f.request;
    auto task = f.makeTask();
    std::weak_ptr<Task> weakTask = task;
    task->retainedCallback = [request = f.request] { (void)request; }; // Deliberate task->state ownership edge.
    f.request->launchReturned(std::move(task));
    check(f.request->callback(f.makePayload()) == RequestEvent::delivered, "last-reference callback not delivered");
    check(weakRequest.expired() && weakTask.expired(), "task/state cycle or notification lifetime leaked");
    f.clean();
}

static void noTaskFailure() {
    Fixture f;
    f.expectError = true;
    f.expectTask = false;
    f.requestDestroy();
    f.request->callback(f.makePayload());
    f.request->launchReturned(nullptr);
    f.clean();
}

static void simultaneousEvents() {
    for (unsigned iteration = 0; iteration < 300; ++iteration) {
        Fixture f;
        std::atomic<bool> start{false};
        std::atomic<int> deliveries{0};
        const auto wait = [&] { while (!start.load(std::memory_order_acquire)) std::this_thread::yield(); };
        std::thread launch([&] {
            wait();
            if (f.request->launchReturned(f.makeTask()) == RequestEvent::delivered) ++deliveries;
        });
        std::thread callback([&] {
            wait();
            if (f.request->callback(f.makePayload()) == RequestEvent::delivered) ++deliveries;
        });
        std::thread timeout([&] { wait(); f.requestDestroy(); });
        start.store(true, std::memory_order_release);
        launch.join(); callback.join(); timeout.join();
        check(deliveries == 1 && f.request->delivered(), "racing events did not deliver exactly once");
        f.clean();
    }
}

using Retirement = metal_compile_experiment::OwnerRetirement<Payload, std::shared_ptr<Task>>;

struct NotifyScope {
    std::atomic<int>& active;
    explicit NotifyScope(std::atomic<int>& value) : active(value) { ++active; }
    ~NotifyScope() { --active; }
};

struct HoldFixture {
    std::atomic<int> owners{0}, tasks{0}, payloads{0}, notifications{0}, retirements{0}, destroys{0}, activeNotify{0};
    std::atomic<bool> launchActive{true};
    std::mutex completionMutex;
    std::condition_variable readyChanged;
    bool ready = false;
    std::unique_ptr<Owner> owner = std::make_unique<Owner>(owners);
    std::shared_ptr<Retirement> state;
    bool expectError = false, expectWarning = false, expectTask = true;
    bool throwOnNotify = false, dropStateOnRetire = false;
    std::function<void()> afterReady;
    std::function<void(const Payload&)> beforeRetire;

    HoldFixture() {
        state = Retirement::create([this](Payload payload) {
            NotifyScope callbackScope(activeNotify);
            bool isLate;
            {
                // Pinned MVKShaderLibraryCompiler::compileComplete retains the
                // result under _completionLock, then endCompile sets done and
                // notifies the existing waiter before returning _isDestroyed.
                std::lock_guard<std::mutex> lock(completionMutex);
                check(bool(owner), "completion touched a retired owner");
                check(bool(payload.error) == (expectError || expectWarning), "retained error changed");
                check(bool(payload.library) == !expectError, "retained library changed");
                owner->result = std::move(payload);
                owner->complete = true;
                ready = true;
                ++notifications;
                readyChanged.notify_all();
                isLate = owner->destroyRequested;
            }
            if (afterReady) afterReady();
            // Pinned callback: if (compileComplete(...)) destroy(). The normal
            // waiting caller independently calls destroy after newMTLLibrary.
            if (isLate) destroy();
            if (throwOnNotify) throw std::runtime_error("authored immediate-notify failure");
        }, [this](std::shared_ptr<Task> task) {
            check(!launchActive && activeNotify == 0, "retired inside a launch/notification scope");
            check(bool(task) == expectTask, "retirement lost the adopted task");
            check(state->retired(), "retirement ran under its own mutex or before publication");
            {
                std::lock_guard<std::mutex> lock(completionMutex);
                check(owner && owner->complete && owner->destroyRequested, "retirement violated markDestroyed order");
                if (beforeRetire) beforeRetire(owner->result);
                owner.reset();
                ++retirements;
            }
            if (dropStateOnRetire) state.reset();
        });
    }

    Payload makePayload() {
        return {expectError ? nullptr : std::make_shared<Counted>(payloads),
                expectError || expectWarning ? std::make_shared<Counted>(payloads) : nullptr};
    }
    std::shared_ptr<Task> makeTask() { return std::make_shared<Task>(tasks); }

    void destroy() {
        bool compileDone;
        {
            // Pinned markDestroyed: set destroyed, return done while locked.
            std::lock_guard<std::mutex> lock(completionMutex);
            if (!owner) return;
            ++destroys;
            owner->destroyRequested = true;
            compileDone = owner->complete;
        }
        // The only changed model step is physical retirement. Notification,
        // done/error state and the preceding markDestroyed decision stay intact.
        if (compileDone) state->requestDestroy();
    }

    void finishLaunch(std::shared_ptr<Task> task) {
        {
            std::lock_guard<std::mutex> lock(completionMutex);
            check(bool(owner), "owner vanished before task adoption");
            owner->adoptedTaskSlot = task ? 1 : 0;
        }
        launchActive = false; // The modeled device monitor/scope has now exited.
        state->finishLaunch(std::move(task));
        // No owner access is allowed after this handoff.
    }

    bool waitReady() {
        std::unique_lock<std::mutex> lock(completionMutex);
        return readyChanged.wait_for(lock, std::chrono::seconds(2), [this] { return ready; });
    }

    void clean() {
        destroy();
        state.reset();
        check(owners == 0 && tasks == 0 && payloads == 0 && notifications == 1 && retirements == 1,
              "owner-hold resources leaked or notification/retirement count differed");
    }
};

static void holdNormalOrder() {
    HoldFixture f;
    f.finishLaunch(f.makeTask());
    check(f.state->callback(f.makePayload()), "normal notification rejected");
    check(f.ready && f.notifications == 1 && f.owners == 1, "normal callback readiness changed");
    f.clean();
}

static void holdEarlyCallbackKeepsReadiness() {
    HoldFixture f;
    check(f.state->callback(f.makePayload()), "early notification rejected");
    check(f.ready && f.notifications == 1 && f.launchActive, "early readiness was incorrectly delayed until launch return");
    f.destroy();
    check(f.owners == 1 && f.payloads == 1 && f.retirements == 0, "waiting caller freed owner during launch");
    f.finishLaunch(f.makeTask());
    check(f.owners == 0, "owner did not retire after launch scope left");
    f.clean();
}

static void holdTimeoutThenLateCallback() {
    HoldFixture f;
    f.expectError = true;
    f.finishLaunch(f.makeTask());
    f.destroy();
    check(!f.ready && f.owners == 1 && f.tasks == 1, "timeout canceled or retired pending work");
    f.state->callback(f.makePayload());
    check(f.ready && f.destroys == 2 && f.owners == 0, "late callback did not follow compileComplete/late destroy ordering");
    f.clean();
}

static void holdActiveNotificationScope() {
    HoldFixture f;
    std::mutex gateMutex;
    std::condition_variable gateChanged;
    bool release = false;
    f.afterReady = [&] {
        std::unique_lock<std::mutex> lock(gateMutex);
        gateChanged.wait(lock, [&] { return release; });
    };
    std::thread callback([&] { f.state->callback(f.makePayload()); });
    check(f.waitReady(), "callback did not wake existing waiter before launch return");
    f.destroy();
    f.finishLaunch(f.makeTask());
    check(f.owners == 1 && f.activeNotify == 1 && f.retirements == 0, "owner retired while callback still used it");
    {
        std::lock_guard<std::mutex> lock(gateMutex);
        release = true;
    }
    gateChanged.notify_all();
    callback.join();
    f.clean();
}

static void holdDuplicatesAndReentrantDestroy() {
    HoldFixture f;
    f.afterReady = [&] {
        check(!f.state->callback(f.makePayload()), "reentrant duplicate callback accepted");
        f.destroy();
        check(f.owners == 1, "reentrant destroy retired an active callback");
    };
    f.state->callback(f.makePayload());
    f.finishLaunch(f.makeTask());
    check(!f.state->finishLaunch(f.makeTask()) && !f.state->callback(f.makePayload()), "duplicate event reused retired owner");
    f.clean();
}

static void holdLastReferenceAndTaskCycle() {
    HoldFixture f;
    f.dropStateOnRetire = true;
    std::weak_ptr<Retirement> weakState = f.state;
    auto task = f.makeTask();
    std::weak_ptr<Task> weakTask = task;
    task->retainedCallback = [state = f.state] { (void)state; };
    f.finishLaunch(std::move(task));
    f.destroy();
    f.state->callback(f.makePayload());
    check(weakState.expired() && weakTask.expired(), "owner-hold task/state cycle survived retirement");
    f.clean();
}

static void holdFailuresAndWarnings() {
    {
        HoldFixture f;
        f.expectError = true;
        f.expectTask = false;
        f.destroy();
        f.state->callback(f.makePayload());
        f.finishLaunch(nullptr);
        f.clean();
    }
    {
        HoldFixture f;
        f.expectWarning = true;
        f.state->callback(f.makePayload());
        check(f.payloads == 2, "non-null library plus warning was not retained");
        f.finishLaunch(f.makeTask());
        f.clean();
    }
}

static void holdNotificationException() {
    HoldFixture f;
    f.throwOnNotify = true;
    f.finishLaunch(f.makeTask());
    bool threw = false;
    try { f.state->callback(f.makePayload()); }
    catch (const std::runtime_error&) { threw = true; }
    check(threw && f.notifications == 1, "notification exception was hidden or repeated");
    check(!f.state->callback(f.makePayload()), "notification retried after throwing");
    f.clean();
}

static void holdSimultaneousEvents() {
    for (unsigned iteration = 0; iteration < 300; ++iteration) {
        HoldFixture f;
        std::atomic<bool> start{false};
        const auto wait = [&] { while (!start.load(std::memory_order_acquire)) std::this_thread::yield(); };
        std::thread launch([&] { wait(); f.finishLaunch(f.makeTask()); });
        std::thread callback([&] { wait(); f.state->callback(f.makePayload()); });
        std::thread timeout([&] { wait(); f.destroy(); });
        start.store(true, std::memory_order_release);
        launch.join(); callback.join(); timeout.join();
        check(f.ready && f.notifications == 1 && f.state->retired(), "owner-hold race changed readiness or retirement");
        f.clean();
    }
}

// Test-local host protocol around the unchanged OwnerRetirement model. A
// provider may notify successfully, then fail while returning its task. That
// fault remains observable without retracting a published result or notifying
// twice. None of this models Objective-C unwinding or Metal task release rules.
enum class LaunchFault { none, missingTask, exception };

struct LaunchTrace {
    std::atomic<int> providerScopes{0};
    LaunchFault fault = LaunchFault::none;
    unsigned faults = 0;
    bool faultAfterNotification = false, failureNotificationAccepted = false;
    bool launchExited = false, launchUnwound = false, exceptionObserved = false;
    bool payloadPreservedAtRetirement = false;
    std::weak_ptr<Counted> firstLibrary, firstError;
};

struct AuthoredLaunchFailure : std::runtime_error {
    AuthoredLaunchFailure() : std::runtime_error("authored provider failed after entering launch") {}
};

struct ProviderScope {
    LaunchTrace& trace;
    explicit ProviderScope(LaunchTrace& value) : trace(value) { ++trace.providerScopes; }
    ~ProviderScope() { --trace.providerScopes; }
};

struct LaunchExitScope {
    HoldFixture& fixture;
    LaunchTrace& trace;
    std::shared_ptr<Task> task;
    LaunchExitScope(HoldFixture& f, LaunchTrace& t) : fixture(f), trace(t) {}
    ~LaunchExitScope() {
        trace.launchExited = true;
        trace.launchUnwound = std::uncaught_exceptions() != 0;
        fixture.finishLaunch(std::move(task));
        // No owner access after the handoff, including during stack unwinding.
    }
};

static void configureLaunchObservation(HoldFixture& f, LaunchTrace& trace, bool throwing) {
    f.expectTask = false;
    f.afterReady = [&] {
        {
            std::lock_guard<std::mutex> lock(f.completionMutex);
            trace.firstLibrary = f.owner->result.library;
            trace.firstError = f.owner->result.error;
        }
        f.destroy(); // Model a waiter that leaves as soon as it receives a result.
    };
    f.beforeRetire = [&, throwing](const Payload& payload) {
        check(trace.launchExited && trace.providerScopes == 0,
              "launch failure retired owner before its provider scope left");
        check(trace.launchUnwound == throwing, "launch exception did not unwind the adoption scope");
        check(payload.library == trace.firstLibrary.lock() && payload.error == trace.firstError.lock(),
              "launch fault replaced the first notified payload");
        trace.payloadPreservedAtRetirement = true;
    };
}

static void recordLaunchFault(HoldFixture& f, LaunchTrace& trace, LaunchFault fault) {
    check(++trace.faults == 1, "launch failure was reported more than once");
    trace.fault = fault;
    trace.faultAfterNotification = f.notifications != 0;
    // A failure before any callback completes the modeled host waiter. After a
    // callback it is a rejected duplicate; the independent fault record remains.
    Payload failure{nullptr, std::make_shared<Counted>(f.payloads)};
    trace.failureNotificationAccepted = f.state->callback(std::move(failure));
}

static void runFailingProvider(HoldFixture& f, LaunchTrace& trace, bool throwing,
                               const std::function<void()>& provider) {
    try {
        LaunchExitScope launch(f, trace);
        try {
            ProviderScope active(trace);
            provider();
            if (throwing) throw AuthoredLaunchFailure();
            launch.task = nullptr;
        } catch (const AuthoredLaunchFailure&) {
            recordLaunchFault(f, trace, LaunchFault::exception);
            throw; // Exercise real C++ unwinding; keep the original exception.
        }
        if (!launch.task) recordLaunchFault(f, trace, LaunchFault::missingTask);
    } catch (const AuthoredLaunchFailure&) {
        trace.exceptionObserved = true;
    }
}

static void verifyLaunchFault(HoldFixture& f, LaunchTrace& trace, bool early, bool throwing) {
    check(trace.faults == 1 && trace.fault == (throwing ? LaunchFault::exception : LaunchFault::missingTask),
          "provider failure disappeared or changed kind");
    check(trace.faultAfterNotification == early && trace.failureNotificationAccepted == !early,
          "launch fault did not preserve the first notification decision");
    check(trace.exceptionObserved == throwing && trace.payloadPreservedAtRetirement,
          "launch unwind or payload observation was lost");
    check(f.notifications == 1 && f.retirements == 1 && f.owners == 0,
          "launch failure changed notification/retirement counts");
    check(trace.firstLibrary.expired() && trace.firstError.expired(), "retired first payload leaked");
    Payload late{std::make_shared<Counted>(f.payloads), nullptr};
    check(!f.state->callback(std::move(late)), "late callback after launch failure touched retired owner");
    f.clean();
}

static void launchNullAndThrowBoundaries() {
    for (bool early : {false, true}) {
        for (bool throwing : {false, true}) {
            HoldFixture f;
            LaunchTrace trace;
            f.expectError = !early;
            configureLaunchObservation(f, trace, throwing);
            runFailingProvider(f, trace, throwing, [&] {
                if (early) {
                    check(f.state->callback(f.makePayload()), "provider's early success was rejected");
                    check(f.ready && f.owners == 1 && f.retirements == 0,
                          "early success was delayed or retired during launch");
                }
            });
            verifyLaunchFault(f, trace, early, throwing);
        }
    }
}

struct NotificationGate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, released = false;
    void arriveAndWait() {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return released; });
    }
    bool waitUntilEntered() {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(2), [&] { return entered; });
    }
    void release() {
        { std::lock_guard<std::mutex> lock(mutex); released = true; }
        changed.notify_all();
    }
};

struct CallbackJoinScope {
    NotificationGate& gate;
    std::thread& thread;
    ~CallbackJoinScope() { gate.release(); if (thread.joinable()) thread.join(); }
};

static void launchFailureWhileCallbackActive() {
    HoldFixture f;
    LaunchTrace trace;
    configureLaunchObservation(f, trace, true);
    NotificationGate gate;
    const auto observe = f.afterReady;
    f.afterReady = [&] { observe(); gate.arriveAndWait(); };
    std::thread callback;
    CallbackJoinScope joined{gate, callback};
    runFailingProvider(f, trace, true, [&] {
        callback = std::thread([&] { f.state->callback(f.makePayload()); });
        check(gate.waitUntilEntered(), "callback did not enter the deterministic failure gate");
    });
    bool held;
    {
        std::lock_guard<std::mutex> lock(f.completionMutex);
        held = f.owner && f.activeNotify == 1 && f.retirements == 0 && trace.launchExited;
    }
    gate.release();
    callback.join();
    check(held, "launch unwind retired an owner while its callback was still active");
    verifyLaunchFault(f, trace, true, true);
}

static void launchUnwindBoundaries() {
    launchNullAndThrowBoundaries();
    launchFailureWhileCallbackActive();
}

// Test-local retirement mailbox. It models no Metal task release/join contract.
struct HandoffControl {
    std::mutex mutex;
    std::condition_variable changed;
    std::thread::id producer;
    bool callbackReturned = false, joinEntered = false, producerExited = false, abort = false;
    std::exception_ptr failure;
    std::atomic<int> wrongThreadJoins{0}, joins{0};
    void stop() { std::lock_guard<std::mutex> lock(mutex); abort = true; changed.notify_all(); }
};
struct JoinTask : Counted {
    std::shared_ptr<HandoffControl> control;
    JoinTask(std::atomic<int>& alive, std::shared_ptr<HandoffControl> value) : Counted(alive), control(std::move(value)) {}
    void joinOutsideProducer() {
        std::unique_lock<std::mutex> lock(control->mutex);
        if (std::this_thread::get_id() == control->producer) {
            ++control->wrongThreadJoins;
            throw std::runtime_error("authored join attempted on callback/producer thread");
        }
        ++control->joins;
        control->joinEntered = true;
        control->changed.notify_all();
        check(control->changed.wait_for(lock, std::chrono::seconds(2), [&] {
            return control->producerExited || control->abort;
        }) && control->producerExited, "waiter could not join after callback returned");
    }
};
using HandoffBridge = metal_compile_experiment::OwnerRetirement<Payload, std::shared_ptr<JoinTask>>;
struct HandoffThreadScope {
    std::shared_ptr<HandoffControl> control;
    std::thread& thread;
    ~HandoffThreadScope() { control->stop(); if (thread.joinable()) thread.join(); }
};

static void retirementHandoff(bool early) {
    std::atomic<int> owners{0}, tasks{0}, payloads{0}, notifications{0}, handoffs{0};
    auto control = std::make_shared<HandoffControl>();
    auto owner = std::make_unique<Owner>(owners);
    auto task = std::make_shared<JoinTask>(tasks, control);
    std::shared_ptr<JoinTask> mailbox;
    bool outsideBridgeMutex = false;
    std::shared_ptr<HandoffBridge> bridge;
    bridge = HandoffBridge::create([&](Payload payload) {
        owner->result = std::move(payload);
        ++notifications;
    }, [&](std::shared_ptr<JoinTask> retiredTask) {
        // This re-enters the bridge on the retirement caller itself. A bridge
        // lock held across Retire would deadlock and fail the process budget.
        outsideBridgeMutex = bridge->retired();
        std::lock_guard<std::mutex> lock(control->mutex);
        mailbox = std::move(retiredTask); // No join or owner destruction here.
        ++handoffs;
        control->changed.notify_all();
    });
    bridge->requestDestroy(); // Logical timeout is not cancellation.
    if (!early) bridge->finishLaunch(std::move(task));
    check(owners == 1 && tasks == 1 && notifications == 0 && handoffs == 0,
          "pending logical timeout lost resources");
    std::thread producer;
    HandoffThreadScope joined{control, producer};
    producer = std::thread([&] {
        try {
            { std::lock_guard<std::mutex> lock(control->mutex); control->producer = std::this_thread::get_id(); }
            bridge->callback(Payload{std::make_shared<Counted>(payloads), nullptr});
            std::unique_lock<std::mutex> lock(control->mutex);
            control->callbackReturned = true;
            control->changed.notify_all();
            // The waiter enters its fake task join while this producer is still
            // alive. It cannot finish until this callback thread exits the gate.
            check(control->changed.wait_for(lock, std::chrono::seconds(2), [&] {
                return control->joinEntered || control->abort;
            }) && control->joinEntered, "cleanup context never entered task join");
            control->producerExited = true;
            control->changed.notify_all();
        } catch (...) {
            std::lock_guard<std::mutex> lock(control->mutex);
            control->failure = std::current_exception();
            control->abort = true;
            control->changed.notify_all();
        }
    });
    if (early) {
        {
            std::unique_lock<std::mutex> lock(control->mutex);
            check(control->changed.wait_for(lock, std::chrono::seconds(2), [&] {
                return control->callbackReturned || control->abort;
            }) && control->callbackReturned, "early notification did not return before launch adoption");
        }
        check(notifications == 1 && handoffs == 0 && owners == 1 && tasks == 1,
              "early callback delayed readiness or retired before task adoption");
        bridge->finishLaunch(std::move(task));
    }
    std::shared_ptr<JoinTask> cleanupTask;
    {
        std::unique_lock<std::mutex> lock(control->mutex);
        check(control->changed.wait_for(lock, std::chrono::seconds(2), [&] {
            return bool(mailbox) || control->abort;
        }) && bool(mailbox), "retirement did not hand off an owned task");
        cleanupTask = std::move(mailbox);
    }
    check(outsideBridgeMutex && owners == 1 && tasks == 1, "handoff released resources before cleanup");
    cleanupTask->joinOutsideProducer(); // Neither bridge nor mailbox mutex held.
    producer.join();
    if (control->failure) std::rethrow_exception(control->failure);
    cleanupTask.reset();
    owner.reset();
    bridge.reset();
    check(owners == 0 && tasks == 0 && payloads == 0 && notifications == 1 && handoffs == 1 &&
          control->joins == 1 && control->wrongThreadJoins == 0 && control->callbackReturned,
          "retirement handoff leaked resources or joined on the producer");
}

static void retirementHandoffBoundaries() {
    retirementHandoff(false);
    retirementHandoff(true);
    std::atomic<int> tasks{0};
    auto control = std::make_shared<HandoffControl>();
    control->producer = std::this_thread::get_id();
    {
        JoinTask task(tasks, control);
        bool rejected = false;
        try { task.joinOutsideProducer(); } catch (const std::runtime_error&) { rejected = true; }
        check(rejected && control->wrongThreadJoins == 1 && control->joins == 0,
              "wrong-placement control did not reject callback-thread join");
    }
    check(tasks == 0, "wrong-placement control leaked its fake task");
}

struct UnsafeOwner { volatile int returnedTask = 0; };
#if defined(__GNUC__)
__attribute__((noinline))
#endif
static void earlyCallbackDestroysOwner(UnsafeOwner* owner) {
    delete owner;
}
#if defined(__GNUC__)
__attribute__((noinline))
#endif
static void assignAfterCallback(UnsafeOwner* owner) {
    owner->returnedTask = 17;
}

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--unsafe-model") == 0) {
        std::fputs("EXPECTED NEGATIVE: authored early-callback owner destruction, not an observed Metal callback\n", stderr);
        auto* owner = new UnsafeOwner;
        earlyCallbackDestroysOwner(owner);
        assignAfterCallback(owner); // Model this->_task assignment after the factory returns.
        return 99; // Reaching here means ASan did not establish the intended negative control.
    }
    const bool launchOnly = argc == 2 && std::strcmp(argv[1], "--launch-unwind-only") == 0;
    const bool handoffOnly = argc == 2 && std::strcmp(argv[1], "--retirement-handoff-only") == 0;
    if (argc != 1 && !launchOnly && !handoffOnly) return 2;
    try {
        if (handoffOnly) {
            retirementHandoffBoundaries();
            std::puts("METAL_RETIREMENT_HANDOFF cases=2 wrong_thread_controls=1 metal_api_verified=0 objc_runtime_verified=0");
            return 0;
        }
        if (launchOnly) {
            launchUnwindBoundaries();
            std::puts("METAL_REQUEST_LAUNCH_UNWIND cases=5 metal_api_verified=0 objc_runtime_verified=0");
            return 0;
        }
        normalOrder();
        callbackBeforeLaunchReturn();
        timeoutThenLateCallback();
        defensiveDuplicates();
        reentrantNotificationAndWarning();
        exceptionDoesNotRetry();
        lastReferenceAndTaskCycle();
        noTaskFailure();
        simultaneousEvents();
        holdNormalOrder();
        holdEarlyCallbackKeepsReadiness();
        holdTimeoutThenLateCallback();
        holdActiveNotificationScope();
        holdDuplicatesAndReentrantDestroy();
        holdLastReferenceAndTaskCycle();
        holdFailuresAndWarnings();
        holdNotificationException();
        holdSimultaneousEvents();
        launchUnwindBoundaries();
        std::puts("METAL_REQUEST_LIFETIME barrier_cases=9 owner_hold_cases=9 launch_unwind_cases=5 concurrent_iterations_each=300 metal_api_verified=0");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "lifecycle model failed: %s\n", error.what());
        return 1;
    }
}
