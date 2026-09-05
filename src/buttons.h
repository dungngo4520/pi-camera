#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <unordered_map>

// Forward declarations of the libgpiod C structs so the header doesn't need
// to pull in <gpiod.h> (libgpiod is a required dependency). The full
// definitions are only needed in buttons.cpp, which includes <gpiod.h>
// directly.
struct gpiod_line_request;
struct gpiod_edge_event_buffer;

namespace picamera {

enum class ButtonId {
  None = 0,
  Shutter,  // Joystick press (BCM13) — capture full-res image
  Key1,     // BCM21
  Key2,     // BCM20
  Key3,     // BCM16
  JoyUp,    // BCM6
  JoyDown,  // BCM19
  JoyLeft,  // BCM5
  JoyRight, // BCM26
};

struct ButtonEvent {
  ButtonId id = ButtonId::None;
  bool pressed = false; // true = pressed (falling edge), false = released
  // For shutter release events: duration of the press in milliseconds.
  // Used to emulate half-press (long hold = metering lock) vs full-press
  // (quick tap = capture). 0 for press events.
  int pressDurationMs = 0;
};

// GPIO button input via libgpiod v2 edge events.
// Reads the Waveshare 1.44" LCD HAT's 3 keys + joystick (8 GPIO pins).
class ButtonInput {
public:
  bool init();
  void shutdown();
  ~ButtonInput() noexcept { shutdown(); }

  // Poll for a button event with timeout (0 = non-blocking).
  // Returns a ButtonEvent with id=None if no event within timeout.
  ButtonEvent poll(int timeoutMs);

  // Convenience: wait specifically for a press event (ignores releases).
  // Returns the button id or None on timeout.
  ButtonId waitForPress(int timeoutMs);

private:
  gpiod_line_request *gpioReq_ = nullptr;
  gpiod_edge_event_buffer *eventBuf_ = nullptr;
  std::unordered_map<unsigned int, std::chrono::steady_clock::time_point>
      pressTimes_;
  // Queue of pending events not yet returned by poll(). This prevents
  // losing events when multiple edges arrive in a single read.
  std::deque<ButtonEvent> pendingEvents_;
};

} // namespace picamera
