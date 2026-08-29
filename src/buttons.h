#pragma once

#include <cstdint>

namespace picamera {

enum class ButtonId {
    None = 0,
    Shutter,    // Joystick press (BCM13) — capture full-res image
    Key1,       // BCM21
    Key2,       // BCM20
    Key3,       // BCM16
    JoyUp,      // BCM6
    JoyDown,    // BCM19
    JoyLeft,    // BCM5
    JoyRight,   // BCM26
};

struct ButtonEvent {
    ButtonId id = ButtonId::None;
    bool pressed = false; // true = pressed (falling edge), false = released
};

// GPIO button input via libgpiod v2 edge events.
// Reads the Waveshare 1.44" LCD HAT's 3 keys + joystick (8 GPIO pins).
class ButtonInput {
public:
    bool init();
    void shutdown();

    // Poll for a button event with timeout (0 = non-blocking).
    // Returns a ButtonEvent with id=None if no event within timeout.
    ButtonEvent poll(int timeoutMs);

    // Convenience: wait specifically for a press event (ignores releases).
    // Returns the button id or None on timeout.
    ButtonId waitForPress(int timeoutMs);

private:
    void *gpioReq_ = nullptr;   // gpiod_line_request*
    void *eventBuf_ = nullptr;  // gpiod_edge_event_buffer*
};

} // namespace picamera
