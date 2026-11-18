#pragma once

// Isolated lifecycle model, not wired into Metal or MoltenVK. Existing wait and
// timeout semantics stay outside this class. In particular, timeout is not
// cancellation: the launch/callback holders must keep this state alive.
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace metal_compile_experiment {

enum class RequestEvent { pending, delivered, duplicate };

template<typename Payload, typename Task>
class RequestLifetime : public std::enable_shared_from_this<RequestLifetime<Payload, Task>> {
    static_assert(std::is_nothrow_move_constructible_v<Payload>);
    static_assert(std::is_nothrow_move_constructible_v<Task>);
    static_assert(std::is_nothrow_move_assignable_v<Payload>);
    static_assert(std::is_nothrow_move_assignable_v<Task>);

public:
    using Notify = std::function<void(Payload, Task)>;

    static std::shared_ptr<RequestLifetime> create(Notify notify) {
        if (!notify) throw std::invalid_argument("completion notification is required");
        return std::shared_ptr<RequestLifetime>(new RequestLifetime(std::move(notify)));
    }

    RequestEvent callback(Payload payload) {
        // Notification may synchronously destroy the owner holding the last
        // external reference. Keep the request alive until this method returns.
        auto keepAlive = this->shared_from_this();
        std::optional<Delivery> delivery;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (callbackSeen_) return RequestEvent::duplicate;
            payload_.emplace(std::move(payload));
            callbackSeen_ = true;
            delivery = readyLocked();
        }
        return deliver(std::move(delivery));
    }

    RequestEvent launchReturned(Task task) {
        auto keepAlive = this->shared_from_this();
        std::optional<Delivery> delivery;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (launchSeen_) return RequestEvent::duplicate;
            task_.emplace(std::move(task));
            launchSeen_ = true;
            delivery = readyLocked();
        }
        return deliver(std::move(delivery));
    }

    bool delivered() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return delivered_;
    }

private:
    struct Delivery { Payload payload; Task task; Notify notify; };

    explicit RequestLifetime(Notify notify) : notify_(std::move(notify)) {}

    std::optional<Delivery> readyLocked() {
        if (!launchSeen_ || !callbackSeen_ || delivered_) return std::nullopt;
        delivered_ = true;
        Delivery delivery{std::move(*payload_), std::move(*task_), std::move(notify_)};
        payload_.reset();
        task_.reset();
        return delivery;
    }

    static RequestEvent deliver(std::optional<Delivery> delivery) {
        if (!delivery) return RequestEvent::pending;
        // No request mutex is held during notification or owned task release.
        // Task ownership leaves the state, breaking task->callback->state->task
        // cycles. A real API adapter must choose its own safe task-retirement
        // point; this portable model does not establish Metal's release rules.
        delivery->notify(std::move(delivery->payload), std::move(delivery->task));
        return RequestEvent::delivered;
    }

    mutable std::mutex mutex_;
    bool launchSeen_ = false;
    bool callbackSeen_ = false;
    bool delivered_ = false;
    std::optional<Payload> payload_;
    std::optional<Task> task_;
    Notify notify_;
};

} // namespace metal_compile_experiment
