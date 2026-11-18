#pragma once

// Independent owner-retirement model, NOT a Metal/MoltenVK adapter. Completion
// notification runs immediately on the first callback, even if launch is still
// executing. Only physical retirement waits for the launch and notification
// scopes to leave. The existing caller owns all wait/timeout/error semantics.
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace metal_compile_experiment {

template<typename Payload, typename Task>
class OwnerRetirement : public std::enable_shared_from_this<OwnerRetirement<Payload, Task>> {
    static_assert(std::is_nothrow_move_constructible_v<Task>);
    static_assert(std::is_nothrow_move_assignable_v<Task>);

public:
    using Notify = std::function<void(Payload)>;
    using Retire = std::function<void(Task)>;

    static std::shared_ptr<OwnerRetirement> create(Notify notify, Retire retire) {
        if (!notify || !retire) throw std::invalid_argument("notification and retirement are required");
        return std::shared_ptr<OwnerRetirement>(new OwnerRetirement(std::move(notify), std::move(retire)));
    }

    bool callback(Payload payload) {
        auto keepAlive = this->shared_from_this();
        Notify notify;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (callbackSeen_) return false;
            callbackSeen_ = true;
            notify = std::move(notify_);
        }
        // This is where fixed MVK's compileComplete/endCompile would publish its
        // result and wake the existing waiter. No launch-return barrier is added.
        try {
            notify(std::move(payload));
        } catch (...) {
            finishCallback();
            throw; // Do not invent a result or retry a notification that threw.
        }
        finishCallback();
        return true;
    }

    // Invoke after the entire launch scope exits, including any device monitor
    // and autorelease-pool cleanup, and after adopting the returned task. Neither
    // the launch block nor the callback may dereference the owner after handoff.
    bool finishLaunch(Task task) {
        auto keepAlive = this->shared_from_this();
        std::optional<Retirement> retirement;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (launchFinished_) return false;
            task_.emplace(std::move(task));
            launchFinished_ = true;
            retirement = readyLocked();
        }
        retireOutsideLock(std::move(retirement));
        return true;
    }

    // A caller may request destruction after completion, or a late callback may
    // repeat it after an earlier timeout. This is idempotent, not cancellation.
    void requestDestroy() {
        auto keepAlive = this->shared_from_this();
        std::optional<Retirement> retirement;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            destroyRequested_ = true;
            retirement = readyLocked();
        }
        retireOutsideLock(std::move(retirement));
    }

    bool retired() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return retired_;
    }

private:
    struct Retirement { Task task; Retire retire; };

    OwnerRetirement(Notify notify, Retire retire) : notify_(std::move(notify)), retire_(std::move(retire)) {}

    void finishCallback() {
        std::optional<Retirement> retirement;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callbackFinished_ = true;
            retirement = readyLocked();
        }
        retireOutsideLock(std::move(retirement));
    }

    std::optional<Retirement> readyLocked() {
        if (!destroyRequested_ || !launchFinished_ || !callbackFinished_ || retired_) return std::nullopt;
        retired_ = true;
        Retirement retirement{std::move(*task_), std::move(retire_)};
        task_.reset();
        return retirement;
    }

    static void retireOutsideLock(std::optional<Retirement> retirement) noexcept {
        // Like a destructor, the retirement action must not throw. It receives
        // the owned task and chooses its retirement point. Real Metal release/
        // callback rules still require separate API tests; this model proves no
        // Metal cancellation, timeout, QoS, or GPU behavior.
        if (retirement) retirement->retire(std::move(retirement->task));
    }

    mutable std::mutex mutex_;
    bool callbackSeen_ = false;
    bool callbackFinished_ = false;
    bool launchFinished_ = false;
    bool destroyRequested_ = false;
    bool retired_ = false;
    std::optional<Task> task_;
    Notify notify_;
    Retire retire_;
};

} // namespace metal_compile_experiment
