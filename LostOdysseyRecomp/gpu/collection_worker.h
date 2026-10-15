#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <semaphore>
#include <thread>
#include <os/thread_name.h>
#include <utility>

namespace gpu::taa_collection {

// Background analysis is expendable. Stop/destruction must never make the game
// wait for a stalled callback. The detached thread owns its control and captured
// state until its callback exits; callbacks must not refer to application globals.
class CollectionWorker {
public:
    class Control {
    public:
        bool Stopped() const { return stopped_.load(); }
        // Sleeps until Notify or Stop, with no periodic wake-up. A semaphore
        // retains a notification sent before the wait starts; only the consumed
        // token clears signaled_, so a release can never overflow it.
        bool Wait() {
            if (Stopped()) return true;
            wake_.acquire();
            signaled_.store(false);
            return Stopped();
        }
        void Notify() noexcept {
            if (!signaled_.exchange(true)) wake_.release();
        }
        // Sequentially consistent: a Stop whose notify finds a token just
        // taken by Wait is still seen by Wait's final Stopped().
        void Stop() noexcept {
            stopped_.store(true);
            Notify();
        }
    private:
        std::atomic<bool> stopped_{false};
        std::atomic<bool> signaled_{false};
        std::binary_semaphore wake_{0};
    };

    template<class Function> explicit CollectionWorker(Function function)
        : control_(std::make_shared<Control>()) {
        std::thread([control = control_, function = std::move(function)]() mutable {
            os::SetCurrentThreadName("Collect Worker");
            try { function(*control); } catch (...) { /* Optional collection cannot crash the game. */ }
        }).detach();
    }
    ~CollectionWorker() { Stop(); }
    CollectionWorker(const CollectionWorker&) = delete;
    CollectionWorker& operator=(const CollectionWorker&) = delete;
    void Stop() noexcept { control_->Stop(); }
    void Notify() noexcept { control_->Notify(); }
private:
    std::shared_ptr<Control> control_;
};
}
