#pragma once

#include <atomic>
#include <csignal>
#include <iostream>
#include <thread>

namespace picamera {

// RAII helper for graceful shutdown on SIGINT/SIGTERM.
//
// install() registers handlers via sigaction() that set an atomic flag;
// the running loop polls stopRequested() and exits cleanly. The previous
// signal handlers are saved and restored by restore() (also called by the
// destructor), so nesting/stacking is safe.
//
// The signal handler reads a plain static pointer (set before sigaction()
// is called, cleared after the old handler is restored) and writes to a
// lock-free std::atomic<sig_atomic_t>. Per C++20 §32.4.1, lock-free atomic
// operations are async-signal-safe. This also provides proper cross-thread
// visibility on multi-core systems (Pi 4/5), unlike volatile.
//
// Only one StopFlag may be installed at a time (the signal handler routes
// through a single static pointer). The app runs either a timelapse or a
// preview loop, never both, so this is sufficient.
class StopFlag {
public:
  StopFlag() = default;
  ~StopFlag() { restore(); }
  StopFlag(const StopFlag &) = delete;
  StopFlag &operator=(const StopFlag &) = delete;

  // Register the SIGINT/SIGTERM handlers and reset the flag.
  // Resets rawStop_ BEFORE publishing the instance pointer so that
  // no signal can arrive and set the flag only to be overwritten.
  // Returns false (without installing) if another StopFlag is active
  // or if signal installation fails.
  bool install() {
    // Reset the flag BEFORE publishing the instance pointer so that
    // a signal arriving between the publish and the reset cannot set
    // rawStop_=1 only to have it overwritten by rawStop_=0.
    rawStop_.store(0, std::memory_order_relaxed);
    StopFlag *expected = nullptr;
    if (!instance_.compare_exchange_strong(expected, this,
                                           std::memory_order_acq_rel)) {
      // Another StopFlag is already installed — the class documents
      // that only one may be active at a time. This is a logic error.
      return false;
    }
    // Publish the pointer for the signal handler via atomic store.
    // This happens AFTER the CAS and BEFORE sigaction(), so no signal
    // can arrive until the handler is installed. The atomic store
    // ensures visibility across cores with release ordering.
    sigInstance_.store(this, std::memory_order_release);

    struct sigaction sa{};
    sa.sa_handler = &StopFlag::handler;
    sigemptyset(&sa.sa_mask);
    // Block both signals during handler execution to prevent
    // re-entrant handler calls (handlersActive_ still drains correctly).
    sigaddset(&sa.sa_mask, SIGINT);
    sigaddset(&sa.sa_mask, SIGTERM);
    sa.sa_flags = SA_RESTART;

    if (sigaction(SIGINT, &sa, &oldInt_) != 0) {
      std::cerr << "StopFlag: sigaction(SIGINT) failed\n";
      sigInstance_.store(nullptr, std::memory_order_release);
      instance_.store(nullptr, std::memory_order_release);
      return false;
    }
    if (sigaction(SIGTERM, &sa, &oldTerm_) != 0) {
      std::cerr << "StopFlag: sigaction(SIGTERM) failed\n";
      sigaction(SIGINT, &oldInt_, nullptr);
      sigInstance_.store(nullptr, std::memory_order_release);
      instance_.store(nullptr, std::memory_order_release);
      return false;
    }
    return true;
  }

  // Restore the signal handlers that were in effect before install().
  void restore() {
    if (instance_.load(std::memory_order_acquire) != this)
      return;
    // Restore old handlers FIRST — after this, no NEW signal will route
    // to our handler. However, a signal already being handled on another
    // core may still be executing handler() between the sigInstance_ load
    // and the rawStop_ store. Wait for all in-flight handlers to drain
    // before clearing the pointer and returning.
    sigaction(SIGINT, &oldInt_, nullptr);
    sigaction(SIGTERM, &oldTerm_, nullptr);
    // Spin-wait for any in-flight handler to finish. The handler is
    // only 2 atomic operations (load + store), so this wait is at most
    // a few iterations. sched_yield() avoids burning CPU if the handler
    // is preempted mid-execution.
    while (handlersActive_.load(std::memory_order_acquire) > 0)
      std::this_thread::yield();
    sigInstance_.store(nullptr, std::memory_order_release);
    instance_.store(nullptr, std::memory_order_release);
  }

  // Check whether a stop signal was received.
  bool stopRequested() const {
    return rawStop_.load(std::memory_order_acquire) != 0;
  }

  // Programmatically request stop (from any thread, e.g. button handler).
  void requestStop() { rawStop_.store(1, std::memory_order_release); }

private:
  static void handler(int) {
    // Async-signal-safe: atomically loads the instance pointer and
    // writes to a lock-free std::atomic. Per C++20 §32.4.1, lock-free
    // atomic ops are signal-safe. No locks, no malloc.
    // Increment handlersActive_ BEFORE loading sigInstance_ so that
    // restore() can detect an in-flight handler and wait for it.
    handlersActive_.fetch_add(1, std::memory_order_acq_rel);
    StopFlag *sf = sigInstance_.load(std::memory_order_acquire);
    if (sf != nullptr)
      sf->rawStop_.store(1, std::memory_order_release);
    handlersActive_.fetch_sub(1, std::memory_order_acq_rel);
  }

  std::atomic<sig_atomic_t> rawStop_{0};
  struct sigaction oldInt_{};
  struct sigaction oldTerm_{};
  // Atomic for install()/restore() synchronization.
  static inline std::atomic<StopFlag *> instance_{nullptr};
  // Atomic pointer for the signal handler — lock-free, async-signal-safe.
  static inline std::atomic<StopFlag *> sigInstance_{nullptr};
  // In-flight handler counter — restore() spins until this reaches 0
  // to prevent UAF if a signal is being handled on another core during
  // teardown. Lock-free, async-signal-safe.
  static inline std::atomic<int> handlersActive_{0};
  static_assert(std::atomic<StopFlag *>::is_always_lock_free);
  static_assert(std::atomic<sig_atomic_t>::is_always_lock_free);
  static_assert(std::atomic<int>::is_always_lock_free);
};

} // namespace picamera
