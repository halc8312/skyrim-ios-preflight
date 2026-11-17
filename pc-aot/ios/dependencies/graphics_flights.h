/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <cassert>
#include <condition_variable>
#include <mutex>

namespace skyrim {

/* Used only by the startup-selected exact graphics-state route.
 * One coordinator per graphics-pipeline object. Keys use the original state's
 * equality and the exact render-pass pointer. Tickets live on callers' stacks;
 * no borrowed ticket pointer survives the table lock and no allocation failure
 * falls back to an incompatible synchronization scheme.
 *
 * Lock order: table -> instance list, for ready rechecks only. A registered
 * owner must run to completion, never wait in a prefetch admission gate. Build
 * and GPU calls run outside both locks; publication drops the list lock before
 * ticket destruction takes the table lock. The caller joins workers before
 * destroying pipelines, as required by DXVK's existing lifetime contract. */
template<typename State, typename RenderPass>
class GraphicsFlights {
public:
    class Ticket;
    using Lock = std::unique_lock<std::mutex>;

    GraphicsFlights() = default;
    ~GraphicsFlights() { assert(head_ == nullptr); }
    GraphicsFlights(const GraphicsFlights&) = delete;
    GraphicsFlights& operator=(const GraphicsFlights&) = delete;
    Lock lock() { return Lock(mutex_); }

    class Ticket {
    public:
        Ticket(GraphicsFlights& owner, const State& state, const RenderPass* pass)
            : owner_(owner), state_(state), pass_(pass) {}
        ~Ticket() {
            if (!active_) return;
            {
                auto lock = owner_.lock();
                Ticket** at = &owner_.head_;
                while (*at && *at != this) at = &(*at)->next_;
                assert(*at == this);
                *at = next_;
            }
            owner_.changed_.notify_all();
        }
        Ticket(const Ticket&) = delete;
        Ticket& operator=(const Ticket&) = delete;
        const State& state() const { return state_; }
        const RenderPass* pass() const { return pass_; }

        bool busy(const Lock& lock) const {
            assert(lock.owns_lock() && lock.mutex() == &owner_.mutex_);
            // Keep release builds warning-clean without changing synchronization.
            (void)lock;
            for (auto* at = owner_.head_; at; at = at->next_)
                if (at->pass_ == pass_ && at->state_ == state_) return true;
            return false;
        }
        void wait(Lock& lock) {
            assert(!active_);
            // Predicate and removal use the same mutex: notifications cannot be
            // lost, and a waiter never dereferences an expired owner's ticket.
            owner_.changed_.wait(lock, [&] { return !busy(lock); });
        }
        void begin(const Lock& lock) {
            assert(!active_ && !busy(lock));
            (void)lock;
            next_ = owner_.head_;
            owner_.head_ = this;
            active_ = true;
        }
    private:
        GraphicsFlights& owner_;
        const State state_;
        const RenderPass* const pass_;
        Ticket* next_ = nullptr;
        bool active_ = false;
    };
private:
    std::mutex mutex_;
    std::condition_variable changed_;
    Ticket* head_ = nullptr;
};
}
