#include "buttons.h"
#include "safe_path.h" // errnoString

#include <chrono>
#include <cstring>
#include <gpiod.h>
#include <iostream>
#include <unordered_map>

namespace picamera {

namespace {

// Waveshare 1.44" LCD HAT button GPIO mapping (BCM pins)
struct ButtonMap {
  ButtonId id;
  unsigned int pin;
};

constexpr ButtonMap kButtons[] = {
    {ButtonId::Shutter, 13}, // Joystick press
    {ButtonId::Key1, 21},     {ButtonId::Key2, 20},    {ButtonId::Key3, 16},
    {ButtonId::JoyUp, 6},     {ButtonId::JoyDown, 19}, {ButtonId::JoyLeft, 5},
    {ButtonId::JoyRight, 26},
};
constexpr size_t kNumButtons = sizeof(kButtons) / sizeof(kButtons[0]);

ButtonId pinToButton(unsigned int pin) {
  for (const auto &b : kButtons)
    if (b.pin == pin)
      return b.id;
  return ButtonId::None;
}

} // namespace

bool ButtonInput::init() {
  struct gpiod_chip *chip = gpiod_chip_open("/dev/gpiochip0");
  if (!chip) {
    std::cerr << "Buttons: cannot open gpiochip0: " << errnoString(errno)
              << "\n";
    return false;
  }

  // Configure all button pins as input with pull-up and falling edge
  struct gpiod_line_settings *settings = gpiod_line_settings_new();
  if (!settings) {
    std::cerr << "Buttons: gpiod_line_settings_new failed (OOM?)\n";
    gpiod_chip_close(chip);
    return false;
  }
  gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
  gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_PULL_UP);
  gpiod_line_settings_set_edge_detection(settings, GPIOD_LINE_EDGE_BOTH);
  gpiod_line_settings_set_debounce_period_us(settings, 50000); // 50ms debounce

  struct gpiod_line_config *lineCfg = gpiod_line_config_new();
  if (!lineCfg) {
    std::cerr << "Buttons: gpiod_line_config_new failed (OOM?)\n";
    gpiod_line_settings_free(settings);
    gpiod_chip_close(chip);
    return false;
  }
  for (size_t i = 0; i < kNumButtons; ++i) {
    unsigned int pin = kButtons[i].pin;
    gpiod_line_config_add_line_settings(lineCfg, &pin, 1, settings);
  }

  struct gpiod_request_config *reqCfg = gpiod_request_config_new();
  if (!reqCfg) {
    std::cerr << "Buttons: gpiod_request_config_new failed (OOM?)\n";
    gpiod_line_config_free(lineCfg);
    gpiod_line_settings_free(settings);
    gpiod_chip_close(chip);
    return false;
  }
  gpiod_request_config_set_consumer(reqCfg, "picamera-buttons");

  struct gpiod_line_request *req =
      gpiod_chip_request_lines(chip, reqCfg, lineCfg);

  gpiod_line_config_free(lineCfg);
  gpiod_request_config_free(reqCfg);
  gpiod_line_settings_free(settings);

  if (!req) {
    std::cerr << "Buttons: cannot request GPIO lines: " << errnoString(errno)
              << "\n";
    gpiod_chip_close(chip);
    return false;
  }
  // The request holds its own reference to the chip; safe to close
  // the local handle now that we know the request succeeded.
  gpiod_chip_close(chip);
  gpioReq_ = req;

  eventBuf_ = gpiod_edge_event_buffer_new(kNumButtons * 2);
  if (!eventBuf_) {
    std::cerr << "Buttons: cannot create event buffer\n";
    gpiod_line_request_release(req);
    gpioReq_ = nullptr;
    return false;
  }

  std::cout << "Buttons: " << kNumButtons << " buttons initialized\n";
  return true;
}

ButtonEvent ButtonInput::poll(int timeoutMs) {
  ButtonEvent evt;
  if (!gpioReq_)
    return evt;

  // Return a pending event from the queue first (from a previous read
  // that returned multiple events).
  if (!pendingEvents_.empty()) {
    evt = pendingEvents_.front();
    pendingEvents_.pop_front();
    return evt;
  }

  auto *req = gpioReq_;
  auto *buf = eventBuf_;

  // Wait for edge events (timeout in nanoseconds)
  // Clamp negative timeouts to 0 (non-blocking poll).
  // Clamp to INT64_MAX/1e6 to prevent overflow when scaling ms → ns.
  int64_t clampedMs = std::max(0, timeoutMs);
  constexpr int64_t kMaxMsBeforeNsOverflow = INT64_MAX / 1000000LL;
  clampedMs = std::min(clampedMs, kMaxMsBeforeNsOverflow);
  int64_t timeoutNs = clampedMs * 1000000LL;
  int ret = gpiod_line_request_wait_edge_events(req, timeoutNs);
  if (ret <= 0)
    return evt; // timeout or error

  // Read events
  int nEvents = gpiod_line_request_read_edge_events(req, buf, kNumButtons * 2);
  if (nEvents <= 0)
    return evt;

  // Process all events: queue them and return the first one.
  // This prevents losing events when multiple edges arrive in a single
  // read (e.g. fast multi-button chord or quick press+release).
  for (int i = 0; i < nEvents; ++i) {
    struct gpiod_edge_event *event = gpiod_edge_event_buffer_get_event(buf, i);
    if (!event)
      continue;

    unsigned int pin = gpiod_edge_event_get_line_offset(event);
    int type = gpiod_edge_event_get_event_type(event);

    ButtonEvent e;
    e.id = pinToButton(pin);
    if (e.id == ButtonId::None)
      continue;

    e.pressed = (type == GPIOD_EDGE_EVENT_FALLING_EDGE);
    if (e.pressed) {
      // Record press time
      pressTimes_[pin] = std::chrono::steady_clock::now();
      e.pressDurationMs = 0;
    } else {
      // Calculate press duration on release. If no matching press
      // was recorded (e.g., button held at startup or lost event),
      // mark as -1 so handleShutterRelease ignores it.
      auto it = pressTimes_.find(pin);
      if (it != pressTimes_.end()) {
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - it->second);
        e.pressDurationMs = static_cast<int>(duration.count());
        pressTimes_.erase(it);
      } else {
        e.pressDurationMs = -1;
      }
    }
    pendingEvents_.push_back(e);
  }

  if (!pendingEvents_.empty()) {
    evt = pendingEvents_.front();
    pendingEvents_.pop_front();
  }
  return evt;
}

ButtonId ButtonInput::waitForPress(int timeoutMs) {
  auto start = std::chrono::steady_clock::now();
  while (true) {
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    int remaining = timeoutMs - static_cast<int>(elapsed.count());
    if (remaining <= 0)
      break;

    ButtonEvent evt = poll(remaining);
    if (evt.id != ButtonId::None && evt.pressed)
      return evt.id;
    if (evt.id == ButtonId::None)
      break; // poll timed out
             // Got a release event (or non-None without press) — keep waiting
             // with whatever time remains.
  }
  return ButtonId::None;
}

void ButtonInput::shutdown() {
  pressTimes_.clear();
  if (eventBuf_) {
    gpiod_edge_event_buffer_free(eventBuf_);
    eventBuf_ = nullptr;
  }
  if (gpioReq_) {
    gpiod_line_request_release(gpioReq_);
    gpioReq_ = nullptr;
  }
}

} // namespace picamera
