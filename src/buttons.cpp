#include "buttons.h"

#include <iostream>
#include <cstring>
#include <gpiod.h>

namespace picamera {

namespace {

// Waveshare 1.44" LCD HAT button GPIO mapping (BCM pins)
struct ButtonMap {
    ButtonId id;
    unsigned int pin;
};

constexpr ButtonMap kButtons[] = {
    {ButtonId::Shutter,  13}, // Joystick press
    {ButtonId::Key1,     21},
    {ButtonId::Key2,     20},
    {ButtonId::Key3,     16},
    {ButtonId::JoyUp,     6},
    {ButtonId::JoyDown,  19},
    {ButtonId::JoyLeft,   5},
    {ButtonId::JoyRight, 26},
};
constexpr size_t kNumButtons = sizeof(kButtons) / sizeof(kButtons[0]);

ButtonId pinToButton(unsigned int pin) {
    for (const auto &b : kButtons)
        if (b.pin == pin) return b.id;
    return ButtonId::None;
}

} // namespace

bool ButtonInput::init() {
    struct gpiod_chip *chip = gpiod_chip_open("/dev/gpiochip0");
    if (!chip) {
        std::cerr << "Buttons: cannot open gpiochip0: " << strerror(errno) << "\n";
        return false;
    }

    // Configure all button pins as input with pull-up and falling edge
    struct gpiod_line_settings *settings = gpiod_line_settings_new();
    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
    gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_PULL_UP);
    gpiod_line_settings_set_edge_detection(settings, GPIOD_LINE_EDGE_BOTH);
    gpiod_line_settings_set_debounce_period_us(settings, 50000); // 50ms debounce

    struct gpiod_line_config *lineCfg = gpiod_line_config_new();
    for (size_t i = 0; i < kNumButtons; ++i) {
        unsigned int pin = kButtons[i].pin;
        gpiod_line_config_add_line_settings(lineCfg, &pin, 1, settings);
    }

    struct gpiod_request_config *reqCfg = gpiod_request_config_new();
    gpiod_request_config_set_consumer(reqCfg, "picamera-buttons");

    struct gpiod_line_request *req =
        gpiod_chip_request_lines(chip, reqCfg, lineCfg);

    gpiod_line_config_free(lineCfg);
    gpiod_request_config_free(reqCfg);
    gpiod_line_settings_free(settings);
    gpiod_chip_close(chip); // request holds its own reference

    if (!req) {
        std::cerr << "Buttons: cannot request GPIO lines: " << strerror(errno) << "\n";
        return false;
    }
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
    if (!gpioReq_) return evt;

    auto *req = gpioReq_;
    auto *buf = eventBuf_;

    // Wait for edge events (timeout in nanoseconds)
    int64_t timeoutNs = static_cast<int64_t>(timeoutMs) * 1000000LL;
    int ret = gpiod_line_request_wait_edge_events(req, timeoutNs);
    if (ret <= 0) return evt; // timeout or error

    // Read events
    int nEvents = gpiod_line_request_read_edge_events(req, buf, kNumButtons * 2);
    if (nEvents <= 0) return evt;

    // Return the first press event (falling edge = button pressed)
    for (int i = 0; i < nEvents; ++i) {
        struct gpiod_edge_event *event =
            gpiod_edge_event_buffer_get_event(buf, i);
        if (!event) continue;

        unsigned int pin = gpiod_edge_event_get_line_offset(event);
        int type = gpiod_edge_event_get_event_type(event);

        evt.id = pinToButton(pin);
        evt.pressed = (type == GPIOD_EDGE_EVENT_FALLING_EDGE);
        if (evt.id != ButtonId::None)
            return evt;
    }
    return evt;
}

ButtonId ButtonInput::waitForPress(int timeoutMs) {
    auto deadline = timeoutMs;
    while (deadline > 0) {
        ButtonEvent evt = poll(deadline);
        if (evt.id != ButtonId::None && evt.pressed)
            return evt.id;
        if (evt.id == ButtonId::None)
            break; // timeout
        // Got a release event, keep waiting
        deadline -= 1; // approx — poll returned quickly
    }
    return ButtonId::None;
}

void ButtonInput::shutdown() {
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
