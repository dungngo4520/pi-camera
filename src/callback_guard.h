#pragma once

// Shared RAII guard for tracking in-flight libcamera request callbacks.
//
// Used by CameraApp and DualStream to ensure the in-flight
// callback counter is decremented and the condition variable is notified
// on every exit path (including early returns and exceptions), so that
// shutdown()/stop() can safely wait for all callbacks to drain before
// destroying Request objects.

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace picamera {

class CallbackGuard {
public:
    CallbackGuard(std::atomic<int> &counter, std::condition_variable &cv,
                  std::mutex &mtx)
        : counter_(counter), cv_(cv), mtx_(mtx) {}
    ~CallbackGuard() {
        // Hold the mutex during decrement + notify to prevent a lost
        // wakeup: the waiter checks callbacksInFlight_ (an atomic) inside
        // wait_for's predicate loop, but between the predicate check
        // returning false and the internal wait() call, the counter could
        // be decremented and notify_all() called without the waiter ever
        // seeing the notification. Holding the mutex ensures the decrement
        // and notify are serialized with the waiter's predicate check.
        std::lock_guard<std::mutex> lk(mtx_);
        counter_.fetch_sub(1, std::memory_order_acq_rel);
        cv_.notify_all();
    }
private:
    std::atomic<int> &counter_;
    std::condition_variable &cv_;
    std::mutex &mtx_;
};

} // namespace picamera
