#pragma once

#include <atomic>
#include <csignal>

namespace picamera {

// RAII helper for graceful shutdown on SIGINT/SIGTERM.
//
// install() registers a handler that sets an internal atomic flag; the
// running loop polls stopRequested() and exits cleanly. The previous signal
// handlers are saved and restored by restore() (also called by the
// destructor), so nesting/stacking is safe.
//
// Only one StopFlag may be installed at a time (the signal handler routes
// through a single instance pointer). The app runs either a timelapse or a
// preview loop, never both, so this is sufficient.
class StopFlag {
public:
    using SigHandler = void (*)(int);

    StopFlag() = default;
    ~StopFlag() { restore(); }
    StopFlag(const StopFlag &) = delete;
    StopFlag &operator=(const StopFlag &) = delete;

    // Register the SIGINT/SIGTERM handlers and reset the flag.
    void install() {
        stop_.store(false);
        instance_ = this;
        oldInt_ = std::signal(SIGINT, &StopFlag::handler);
        oldTerm_ = std::signal(SIGTERM, &StopFlag::handler);
    }

    // Restore the signal handlers that were in effect before install().
    void restore() {
        std::signal(SIGINT, oldInt_);
        std::signal(SIGTERM, oldTerm_);
        instance_ = nullptr;
    }

    bool stopRequested() const { return stop_.load(); }
    void requestStop() { stop_.store(true); }

private:
    static void handler(int) {
        if (instance_) instance_->stop_.store(true);
    }

    std::atomic<bool> stop_{false};
    SigHandler oldInt_ = SIG_DFL;
    SigHandler oldTerm_ = SIG_DFL;
    static inline StopFlag *instance_ = nullptr;
};

} // namespace picamera
