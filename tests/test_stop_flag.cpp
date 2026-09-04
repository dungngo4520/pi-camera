#include "test_runner.h"
#include "stop_flag.h"

using namespace picamera;

namespace {

// A fresh StopFlag should not report stop requested.
TEST(stop_flag_default_not_stopped) {
    StopFlag sf;
    CHECK(!sf.stopRequested());
}

// requestStop() from a non-signal context should set the flag.
TEST(stop_flag_request_stop) {
    StopFlag sf;
    sf.requestStop();
    CHECK(sf.stopRequested());
}

// install() should succeed and restore() should clean up (no crash).
TEST(stop_flag_install_restore) {
    StopFlag sf;
    CHECK(sf.install());
    CHECK(!sf.stopRequested());
    sf.restore();
    CHECK(!sf.stopRequested());
}

// A second install() on the same instance should fail (already installed).
TEST(stop_flag_double_install_rejected) {
    StopFlag sf;
    CHECK(sf.install());
    CHECK(!sf.install());
    sf.restore();
}

// After restore(), a new StopFlag should be able to install().
TEST(stop_flag_reinstall_after_restore) {
    StopFlag sf1;
    CHECK(sf1.install());
    sf1.restore();

    StopFlag sf2;
    CHECK(sf2.install());
    sf2.restore();
}

// requestStop() after install() should work.
TEST(stop_flag_request_after_install) {
    StopFlag sf;
    CHECK(sf.install());
    sf.requestStop();
    CHECK(sf.stopRequested());
    sf.restore();
}

// Destructor should call restore() — no crash if install() was called.
TEST(stop_flag_destructor_restores) {
    {
        StopFlag sf;
        CHECK(sf.install());
    }
    // If restore() wasn't called by destructor, a new install would fail.
    StopFlag sf2;
    CHECK(sf2.install());
    sf2.restore();
}

} // namespace
